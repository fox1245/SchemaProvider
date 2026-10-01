#include "runtime/testing.h"
#include "json/json.h"
#include "support/runtime_peer.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using namespace sp;
using namespace sp::runtime;
using namespace std::chrono_literals;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #c); } while (false)

class ManualExecutor final : public detail::Executor {
 public:
  void post(Task task) override {
    if (std::exchange(fail_next_post, false)) throw std::bad_alloc();
    enqueue(std::move(task), Kind::Actor);
  }
  void stimulus(Task task) { enqueue(std::move(task), Kind::Stimulus); }
  std::function<void()> before_actor;
  std::size_t timers_fired = 0;
  bool fail_next_post = false;
  Timer schedule(SteadyTime at, Task task) override {
    std::lock_guard lock(mutex_);
    CHECK(!stopped_);
    const auto id = ++last_id_;
    timers_.emplace(id, Entry{at, std::move(task)});
    return id;
  }
  void cancel(Timer id) noexcept override { std::lock_guard lock(mutex_); timers_.erase(id); }
  SteadyTime now() const noexcept override { std::lock_guard lock(mutex_); return now_; }
  WallTime wall_now() const noexcept override { std::lock_guard lock(mutex_); return wall_; }
  bool in_thread() const noexcept override { return active_ != nullptr; }
  void shutdown() noexcept override { std::lock_guard lock(mutex_); stopped_ = true; }
  void advance(std::chrono::steady_clock::duration delta) {
    CHECK(delta >= decltype(delta)::zero());
    std::lock_guard lock(mutex_);
    now_ += delta;
    wall_ += std::chrono::duration_cast<WallTime::duration>(delta);
  }
  std::size_t ready_count() {
    std::lock_guard lock(mutex_);
    promote_timers();
    return ready_.size();
  }
  bool one(std::size_t choice = 0) {
    Ready ready;
    {
      std::lock_guard lock(mutex_);
      promote_timers();
      if (ready_.empty()) return false;
      choice %= ready_.size();
      auto it = ready_.begin() + static_cast<std::ptrdiff_t>(choice);
      ready = std::move(*it);
      ready_.erase(it);
    }
    CHECK(!active_);
    active_ = this;
    try {
      if (ready.kind == Kind::Actor && before_actor) before_actor();
      if (ready.kind == Kind::Timer) ++timers_fired;
      ready.task();
    }
    catch (...) { active_ = nullptr; throw; }
    active_ = nullptr;
    return true;
  }
  void drain() {
    for (std::size_t i = 0; i < 10000; ++i) if (!one()) return;
    throw std::runtime_error("manual executor failed to quiesce");
  }
  template<class Random> void random_drain(Random& random) {
    for (std::size_t i = 0; i < 10000; ++i) if (!one(random())) return;
    throw std::runtime_error("seeded executor failed to quiesce");
  }
  std::size_t timer_count() const { std::lock_guard lock(mutex_); return timers_.size(); }
  void wait_ready() {
    std::unique_lock lock(mutex_);
    ready_changed_.wait_for(lock, 50ms, [&] { return !ready_.empty(); });
  }
 private:
  enum class Kind { Actor, Stimulus, Timer };
  struct Ready { Task task; Kind kind = Kind::Actor; };
  void enqueue(Task task, Kind kind) {
    { std::lock_guard lock(mutex_); CHECK(!stopped_); ready_.push_back({std::move(task), kind}); }
    ready_changed_.notify_all();
  }
  void promote_timers() {
    for (auto it = timers_.begin(); it != timers_.end();) {
      if (it->second.at <= now_) {
        ready_.push_back({std::move(it->second.task), Kind::Timer});
        it = timers_.erase(it);
      } else ++it;
    }
  }
  struct Entry { SteadyTime at; Task task; };
  SteadyTime now_ = std::chrono::steady_clock::now();
  WallTime wall_ = std::chrono::system_clock::now();
  std::deque<Ready> ready_;
  std::map<Timer, Entry> timers_;
  Timer last_id_ = 0;
  mutable std::mutex mutex_;
  std::condition_variable ready_changed_;
  inline static thread_local const ManualExecutor* active_ = nullptr;
  bool stopped_ = false;
};

struct Call {
  explicit Call(transport::HttpRequest request, transport::Callbacks callbacks)
      : request(std::move(request)), callbacks(std::move(callbacks)) {
    result.attempt.reached = transport::Stage::RequestStarted;
    result.attempt.request_body_bytes = static_cast<std::int64_t>(this->request.body.size());
  }
  void head(int status) {
    if (done) return;
    result.http_status = status;
    result.attempt.response_head_seen = true;
    result.attempt.reached = transport::Stage::ResponseStarted;
    transport::ResponseHead value;
    value.status = status;
    value.version = transport::ResponseVersion::Http1_1;
    value.framing = transport::BodyFraming::ContentLength;
    callbacks.on_head(value);
  }
  void flush() {
    if (done) return;
    while (!chunks.empty()) {
      if (!callbacks.on_body(chunks.front())) { ++pauses; return; }
      chunks.pop_front();
    }
    if (ending) {
      done = true;
      ++completions;
      result.status = transport::Status::Completed;
      result.failure = transport::FailureKind::None;
      callbacks.on_done(result);
    }
  }
  void reply(int status, std::string body, std::size_t chunk_bytes = 0) {
    if (done) return;
    head(status);
    if (!chunk_bytes) chunk_bytes = body.size();
    for (std::size_t at = 0; at < body.size(); at += chunk_bytes)
      chunks.push_back(body.substr(at, chunk_bytes));
    ending = true;
    flush();
  }
  void cancel() noexcept {
    if (done) return;
    done = true;
    ++cancellations;
    chunks.clear();
    result.status = transport::Status::Cancelled;
    callbacks.on_done(result);
  }
  void fail(transport::FailureKind kind, transport::Stage stage, std::string detail) {
    if (done) return;
    done = true;
    result.status = transport::Status::Failed;
    result.failure = kind;
    result.detail = std::move(detail);
    result.attempt.reached = stage;
    callbacks.on_done(result);
  }
  transport::HttpRequest request;
  transport::Callbacks callbacks;
  transport::Result result;
  std::deque<std::string> chunks;
  std::size_t pauses = 0, resumes = 0;
  std::size_t completions = 0, cancellations = 0;
  bool ending = false, done = false;
};

class ModelTransport final : public detail::AttemptTransport {
  class Handle final : public detail::Attempt {
   public:
    explicit Handle(std::shared_ptr<Call> call) : call_(std::move(call)) {}
    ~Handle() override { call_->cancel(); }
    void cancel() noexcept override { call_->cancel(); }
    void resume() noexcept override { ++call_->resumes; call_->flush(); }
   private:
    std::shared_ptr<Call> call_;
  };
 public:
  std::unique_ptr<detail::Attempt> start(transport::HttpRequest request, transport::Callbacks callbacks) override {
    if (throw_start) throw std::runtime_error("SYNTHETIC_TRANSPORT_EXCEPTION_SECRET");
    auto call = std::make_shared<Call>(std::move(request), std::move(callbacks));
    calls.push_back(call);
    if (on_start) on_start(call);
    return std::make_unique<Handle>(std::move(call));
  }
  void shutdown() noexcept override { for (auto& call : calls) call->cancel(); }
  std::vector<std::shared_ptr<Call>> calls;
  std::function<void(const std::shared_ptr<Call>&)> on_start;
  bool throw_start = false;
};

descriptor::ValidatedDescriptor descriptor_value(std::uint64_t port = 18080) {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"scheduler-chat","family":"openai.chat","connection":{"base_url":"http://127.0.0.1:)" + std::to_string(port) + R"(","paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
chat::Request request(std::string model = "fixture-model") {
  chat::Request value;
  value.model = std::move(model);
  value.messages.push_back({Role::User, "synthetic request"});
  return value;
}
std::string body(std::string_view text = "ok") {
  return "{\"id\":\"scheduler\",\"model\":\"fixture-model\",\"created\":7,\"object\":\"chat.completion\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":" +
      json::quote(text) + "},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":1,\"total_tokens\":3}}";
}
std::string text_of(const Result& result) {
  CHECK(result && std::holds_alternative<Completion>(*result));
  std::string text;
  for (const auto& message : std::get<Completion>(*result).messages)
    for (const auto& part : message.parts)
      if (const auto* value = std::get_if<Text>(&part)) text += value->value;
  return text;
}
const Failure& failure(const Result& result, ErrorKind kind) {
  CHECK(result && std::holds_alternative<Failure>(*result));
  const auto& value = std::get<Failure>(*result);
  CHECK(value.error.kind == kind);
  return value;
}
RunOptions buffered(const std::shared_ptr<ManualExecutor>& executor) {
  RunOptions run;
  run.streaming = false;
  run.deadline = executor->now() + 1s;
  return run;
}

void terminal_interleavings() {
  // Replay every ready-task choice, including actor turns and promoted timers,
  // rather than draining after each stimulus. The model records the first
  // control observed by an actor; cancellation wins a same-turn deadline tie.
  std::vector<std::vector<std::size_t>> pending(1);
  std::size_t schedules = 0, completions = 0, cancellations = 0, timers = 0;
  std::array<std::size_t, 3> decisions{};
  while (!pending.empty()) {
    auto prefix = std::move(pending.back()); pending.pop_back();
    auto executor = std::make_shared<ManualExecutor>();
    auto wire = std::make_shared<ModelTransport>();
    auto client = detail::ClientAccess::make(descriptor_value(), {}, executor, wire);
    std::stop_source source;
    auto run = buffered(executor);
    run.stop_token = source.get_token();
    Result observed;
    std::size_t outcomes = 0, late_events = 0;
    bool cancel_requested = false, deadline_reached = false;
    std::optional<ErrorKind> expected;
    auto operation = client.start(request(), run, {
      [&](const Event&) { if (observed) ++late_events; },
      [&](Result result) { observed = std::move(result); ++outcomes; }
    });
    executor->drain();
    CHECK(wire->calls.size() == 1);
    executor->before_actor = [&] {
      if (!observed && !expected) {
        if (cancel_requested) expected = ErrorKind::Cancelled;
        else if (deadline_reached) expected = ErrorKind::DeadlineExceeded;
      }
    };
    executor->stimulus([&] { cancel_requested = true; operation.cancel(); });
    executor->stimulus([&] { cancel_requested = true; source.request_stop(); });
    executor->stimulus([&] { deadline_reached = true; executor->advance(1s); });
    executor->stimulus([&] { wire->calls.front()->reply(200, body()); });
    std::vector<std::size_t> trace;
    while (const auto count = executor->ready_count()) {
      CHECK(trace.size() < 32);
      std::size_t choice = 0;
      if (trace.size() < prefix.size()) choice = prefix[trace.size()];
      else for (std::size_t alternative = count; alternative-- > 1;) {
        auto branch = trace; branch.push_back(alternative);
        pending.push_back(std::move(branch));
      }
      CHECK(choice < count);
      trace.push_back(choice);
      CHECK(executor->one(choice));
    }
    CHECK(trace.size() >= prefix.size() && outcomes == 1 && late_events == 0);
    auto joined = operation.join();
    if (expected) {
      failure(observed, *expected); failure(joined, *expected);
      ++decisions[*expected == ErrorKind::Cancelled ? 1 : 2];
    } else {
      CHECK(text_of(observed) == "ok" && text_of(joined) == "ok");
      ++decisions[0];
    }
    const auto& call = *wire->calls.front();
    CHECK(call.completions + call.cancellations == 1);
    completions += call.completions; cancellations += call.cancellations;
    timers += executor->timers_fired;
    executor->advance(1h); executor->drain();
    CHECK(outcomes == 1 && late_events == 0 && executor->timer_count() == 0);
    executor->before_actor = {};
    CHECK(++schedules < 50000);
  }
  CHECK(decisions[0] && decisions[1] && decisions[2] && timers);
  std::cout << "ready-task schedules=" << schedules << " commit/cancel/deadline="
            << decisions[0] << '/' << decisions[1] << '/' << decisions[2]
            << " actual-wire-completions=" << completions
            << " actual-wire-cancellations=" << cancellations << " timer-callbacks=" << timers << '\n';
}

void paused_redelivery() {
  auto executor = std::make_shared<ManualExecutor>();
  auto wire = std::make_shared<ModelTransport>();
  Options options;
  options.limits.queued_body_chunks = 2;
  options.limits.queued_body_bytes = 128;
  auto client = detail::ClientAccess::make(descriptor_value(), options, executor, wire);
  Result observed;
  std::size_t outcomes = 0;
  auto operation = client.start(request(), buffered(executor), {{}, [&](Result result) {
    observed = std::move(result); ++outcomes;
  }});
  executor->drain();
  const std::string expected(8192, 'q');
  wire->calls.front()->reply(200, body(expected), 64);
  const auto held = detail::ClientAccess::stats(operation);
  CHECK(held.queued_bytes <= 128 && held.queued_chunks <= 2);
  CHECK(wire->calls.front()->pauses > 0 && !observed);
  executor->drain();
  CHECK(text_of(operation.join()) == expected);
  CHECK(outcomes == 1 && wire->calls.front()->resumes > 0);
  const auto drained = detail::ClientAccess::stats(operation);
  CHECK(drained.peak_queued_bytes <= 128 && drained.queued_bytes == 0);
  std::cout << "bounded redelivery bytes=" << expected.size() << " pauses=" << wire->calls.front()->pauses << '\n';
}

void seeded_shared_budget() {
  std::size_t dispatches = 0, terminal_outcomes = 0;
  for (std::uint64_t seed = 1; seed <= 64; ++seed) {
    auto executor = std::make_shared<ManualExecutor>();
    auto wire = std::make_shared<ModelTransport>();
    Options options;
    options.retry_tokens = 3;
    options.retry_tokens_per_second = 0;
    auto client = detail::ClientAccess::make(descriptor_value(), options, executor, wire, [] { return 0.0; });
    std::map<std::string, std::size_t> seen;
    wire->on_start = [&](const auto& call) {
      auto parsed = json::parse(call->request.body);
      CHECK(std::holds_alternative<json::Document>(parsed));
      const auto key = std::string(std::get<json::Document>(parsed).root().get("model").as_string());
      if (++seen[key] == 1) call->reply(503, R"({"error":{"type":"server_error","message":"synthetic unavailable"}})");
      else call->reply(200, body(key));
    };
    std::array<Result, 8> results;
    std::array<std::size_t, 8> outcomes{};
    std::vector<Operation> operations;
    auto run = buffered(executor);
    run.retry.enabled = true;
    run.retry.allow_duplicate_billing_risk = true;
    run.retry.max_attempts = 2;
    for (std::size_t i = 0; i < results.size(); ++i) {
      operations.push_back(client.start(request("slot-" + std::to_string(i)), run, {
        {}, [&, i](Result result) { results[i] = std::move(result); ++outcomes[i]; }
      }));
    }
    std::mt19937_64 random(seed);
    executor->random_drain(random);
    CHECK(wire->calls.size() == 11);
    std::size_t recovered = 0;
    for (std::size_t i = 0; i < results.size(); ++i) {
      CHECK(outcomes[i] == 1);
      (void)operations[i].join();
      if (std::holds_alternative<Completion>(*results[i])) {
        CHECK(text_of(results[i]) == "slot-" + std::to_string(i));
        ++recovered;
      } else {
        const auto& error = std::get<Failure>(*results[i]).error;
        CHECK(error.http_status == 503 && error.attempt.attempts == 1);
      }
    }
    CHECK(recovered == 3 && executor->timer_count() == 0);
    dispatches += wire->calls.size();
    terminal_outcomes += results.size();
  }
  std::cout << "seeded schedules=64 concurrent=8 dispatches=" << dispatches << " outcomes=" << terminal_outcomes << '\n';
}

void callback_and_start_boundaries() {
  {
    auto executor = std::make_shared<ManualExecutor>();
    auto wire = std::make_shared<ModelTransport>();
    auto client = detail::ClientAccess::make(descriptor_value(), {}, executor, wire);
    wire->on_start = [](const auto& call) { call->reply(200, body()); };
    auto owner = std::make_shared<int>(7);
    std::weak_ptr<int> weak = owner;
    Result observed;
    std::size_t outcomes = 0;
    auto operation = client.start(request(), buffered(executor), {
      {}, [owner, &observed, &outcomes](Result result) { observed = std::move(result); ++outcomes; }
    });
    owner.reset();
    executor->drain();
    CHECK(text_of(operation.join()) == "ok" && observed && outcomes == 1);
    CHECK(weak.expired());
  }
  {
    auto executor = std::make_shared<ManualExecutor>();
    auto wire = std::make_shared<ModelTransport>();
    auto client = detail::ClientAccess::make(descriptor_value(), {}, executor, wire);
    wire->throw_start = true;
    Result observed;
    auto operation = client.start(request(), buffered(executor), {{}, [&](Result result) { observed = std::move(result); }});
    executor->drain();
    CHECK(observed && std::holds_alternative<Failure>(*observed));
    const auto& error = std::get<Failure>(*observed).error;
    CHECK(error.safe_message.find("SYNTHETIC_TRANSPORT_EXCEPTION_SECRET") == std::string::npos);
    CHECK(error.vendor_code.empty() && error.retry_safety == RetrySafety::NotSent);
    CHECK(failure(operation.join(), ErrorKind::Transport).error.retry_safety == RetrySafety::NotSent);
  }
  {
    auto executor = std::make_shared<ManualExecutor>();
    auto wire = std::make_shared<ModelTransport>();
    auto client = detail::ClientAccess::make(descriptor_value(), {}, executor, wire);
    Result observed;
    auto operation = client.start(request(), buffered(executor), {{}, [&](Result result) { observed = std::move(result); }});
    executor->drain();
    wire->calls.front()->fail(transport::FailureKind::Receive, transport::Stage::RequestStarted,
                             "SYNTHETIC_TRANSPORT_DETAIL_SECRET");
    executor->drain();
    const auto& error = failure(observed, ErrorKind::Transport).error;
    CHECK(error.safe_message.find("SYNTHETIC_TRANSPORT_DETAIL_SECRET") == std::string::npos);
    CHECK(error.retry_safety == RetrySafety::PossiblyAccepted && error.vendor_code.empty());
    CHECK(failure(operation.join(), ErrorKind::Transport).error.retry_safety == RetrySafety::PossiblyAccepted);
  }
  std::cout << "synchronous-start, callback-release and exception privacy boundaries passed\n";
}

void admission_and_preflight() {
  auto executor = std::make_shared<ManualExecutor>();
  auto wire = std::make_shared<ModelTransport>();
  Options options; options.limits.max_operations = 1;
  auto client = detail::ClientAccess::make(descriptor_value(), options, executor, wire);
  for (int boundary = 0; boundary != 4; ++boundary) {
    auto run = buffered(executor);
    auto input = request();
    std::stop_source stop;
    ErrorKind expected = ErrorKind::InvalidRequest;
    if (boundary == 0) run.retry.max_attempts = 0;
    if (boundary == 1) { run.stop_token = stop.get_token(); stop.request_stop(); expected = ErrorKind::Cancelled; }
    if (boundary == 2) { run.deadline = executor->now(); expected = ErrorKind::DeadlineExceeded; }
    if (boundary == 3) input.model.clear();
    Result observed;
    bool on_executor = false;
    std::size_t callbacks = 0;
    auto operation = client.start(std::move(input), run, {{}, [&](Result value) {
      on_executor = executor->in_thread(); observed = std::move(value); ++callbacks;
    }});
    CHECK(!observed && callbacks == 0 && wire->calls.empty());
    bool rejected_callback = false, rejected = false;
    try {
      auto denied = client.start(request(), buffered(executor), {{}, [&](Result) { rejected_callback = true; }});
    } catch (const AdmissionError& error) {
      rejected = true;
      const auto& metadata = failure(error.outcome(), ErrorKind::ResourceLimit).error;
      CHECK(metadata.retry_safety == RetrySafety::NotSent && metadata.attempt.attempts == 0);
    }
    CHECK(rejected && !rejected_callback);
    executor->drain();
    CHECK(on_executor && callbacks == 1 && !rejected_callback && wire->calls.empty());
    failure(observed, expected); failure(operation.join(), expected);
  }
  executor->fail_next_post = true;
  bool rejected = false, callback = false;
  try {
    auto denied = client.start(request(), buffered(executor), {{}, [&](Result) { callback = true; }});
  } catch (const AdmissionError& error) {
    rejected = true; failure(error.outcome(), ErrorKind::ResourceLimit);
  }
  CHECK(rejected && !callback && wire->calls.empty());
  // Each preceding join must free the sole slot before this dispatch is admitted.
  wire->on_start = [](const auto& call) { call->reply(200, body()); };
  auto operation = client.start(request(), buffered(executor));
  executor->drain();
  CHECK(text_of(operation.join()) == "ok" && wire->calls.size() == 1);
  std::cout << "executor-only preflight, bounded admission and join slot fence passed\n";
}

void real_paused_redelivery(const char* node, const char* script) {
  runtime_test::Peer peer(node, script);
  const auto model = peer.arm("burst-held");
  auto executor = std::make_shared<ManualExecutor>();
  Options options;
  options.api_key = "RUNTIME_SECRET_MARKER_62c490";
  options.limits.queued_body_chunks = 2;
  options.limits.queued_body_bytes = 32 << 10;
  auto client = detail::ClientAccess::make(descriptor_value(peer.port), options, executor);
  Result observed;
  std::size_t outcomes = 0;
  auto run = buffered(executor);
  run.deadline = executor->now() + 30s;
  auto operation = client.start(request(model), run, {{}, [&](Result result) {
    observed = std::move(result); ++outcomes;
  }});
  executor->drain();
  peer.wait(model, "held");
  executor->drain();
  peer.release(model);
  // Hold the executor, not a user callback. The real libcurl producer must pause.
  const auto pause_deadline = std::chrono::steady_clock::now() + 5s;
  while (detail::ClientAccess::stats(operation).pauses == 0 &&
         std::chrono::steady_clock::now() < pause_deadline)
    std::this_thread::sleep_for(1ms);
  const auto paused = detail::ClientAccess::stats(operation);
  CHECK(paused.pauses > 0 && paused.queued_bytes > 0 && !observed);
  CHECK(paused.queued_bytes <= options.limits.queued_body_bytes && paused.queued_chunks <= 2);
  const auto finish_deadline = std::chrono::steady_clock::now() + 30s;
  while (!observed && std::chrono::steady_clock::now() < finish_deadline) {
    if (!executor->one()) executor->wait_ready();
  }
  CHECK(observed && std::holds_alternative<Completion>(*observed));
  (void)operation.join();
  CHECK(outcomes == 1);
  const auto& completion = std::get<Completion>(*observed);
  CHECK(completion.messages.size() == 1 && completion.messages.front().parts.size() == 1);
  const auto& actual = std::get<Text>(completion.messages.front().parts.front()).value;
  std::string expected;
  expected.reserve(8 << 20);
  for (std::size_t i = 0; i < 2048; ++i) {
    const auto number = std::to_string(i);
    expected.append(8 - number.size(), '0').append(number).push_back(':');
    expected.append(4096 - 9, 'x');
  }
  CHECK(actual == expected);
  const auto drained = detail::ClientAccess::stats(operation);
  CHECK(drained.queued_bytes == 0 && drained.peak_queued_bytes <= options.limits.queued_body_bytes);
  peer.count(model, 1);
  std::cout << "real loopback redelivery exact-bytes=" << actual.size()
            << " peak-queued=" << drained.peak_queued_bytes << " pauses=" << drained.pauses << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3) throw std::runtime_error("usage: runtime_scheduler_tests NODE PEER_SCRIPT");
    terminal_interleavings();
    paused_redelivery();
    seeded_shared_budget();
    callback_and_start_boundaries();
    admission_and_preflight();
    real_paused_redelivery(argv[1], argv[2]);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "runtime scheduler failure: " << error.what() << '\n';
    return 1;
  }
}
