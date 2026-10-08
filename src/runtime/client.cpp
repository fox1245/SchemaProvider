#include "runtime/client.h"
#include "runtime/testing.h"
#include "core/native.h"
#include "codecs/messages.h"
#include "codecs/responses.h"
#include "codecs/gemini.h"
#include "codecs/interactions.h"

#include <openssl/sha.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <iterator>
#include <utility>

namespace sp::runtime::detail {
namespace {
bool prior_attempt_proves_no_usage(const Failure& failed) noexcept {
  const auto& attempt = failed.error.attempt;
  const auto& usage = failed.partial.usage;
  return failed.error.retry_safety == RetrySafety::NotSent &&
      !attempt.request_may_have_left && attempt.request_body_bytes == 0 &&
      !attempt.response_head_seen && attempt.transport_internal_resends == 0 &&
      failed.partial.messages.empty() && !failed.partial.stop && !failed.partial.wire_envelope &&
      failed.partial.raw_events.empty() && usage.stage == UsageStage::Missing &&
      usage.quality == UsageQuality::Consistent && !usage.input_total && !usage.output_total &&
      !usage.total && !usage.provider_reported_total && !usage.input_uncached &&
      !usage.cache_read && !usage.cache_write && !usage.reasoning && usage.extra.empty() && usage.conflicts.empty();
}
thread_local unsigned runtime_depth = 0;
thread_local const void* pool_thread = nullptr;
struct RuntimeScope {
  RuntimeScope() { ++runtime_depth; }
  ~RuntimeScope() { --runtime_depth; }
};

Error error_for(ErrorKind kind) { return Error{kind, std::string(safe_message(kind))}; }
Result misuse_result() {
  static const Result result = std::make_shared<const Outcome>(Failure{error_for(ErrorKind::Misuse), {}});
  return result;
}

// Workers own only Core, not the executor containing their thread handles. Thus a
// shutdown on a worker can detach those existing handles without a reaper thread.
class PoolExecutor final : public Executor {
  struct Core {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Task> ready;
    using Key = std::pair<SteadyTime, Timer>;
    using Timers = std::map<Key, Task>;
    Timers timers;
    std::unordered_map<Timer, std::map<Key, Task>::iterator> timer_index;
    Timer next_timer = 1;
    bool stopping = false;
  };
  std::shared_ptr<Core> core_ = std::make_shared<Core>();
  std::mutex shutdown_mutex_;
  std::vector<std::thread> threads_;

  static void run(const std::shared_ptr<Core>& core) {
    pool_thread = core.get();
    for (;;) {
      Task task;
      {
        std::unique_lock lock(core->mutex);
        for (;;) {
          const auto now = std::chrono::steady_clock::now();
          if (!core->timers.empty() && core->timers.begin()->first.first <= now) {
            auto timer = core->timers.begin();
            core->timer_index.erase(timer->first.second);
            task = std::move(timer->second);
            core->timers.erase(timer);
            break;
          }
          if (!core->ready.empty()) {
            task = std::move(core->ready.front());
            core->ready.pop_front();
            break;
          }
          if (core->stopping) { pool_thread = nullptr; return; }
          if (core->timers.empty()) core->wake.wait(lock);
          else {
            const auto wake_at = core->timers.begin()->first.first;
            core->wake.wait_until(lock, wake_at);
          }
        }
      }
      try { task(); } catch (...) { /* An operation boundary owns error delivery. */ }
    }
  }
 public:
  explicit PoolExecutor(std::size_t count) {
    threads_.reserve(count);
    try {
      for (std::size_t i = 0; i < count; ++i)
        threads_.emplace_back([core = core_] { run(core); });
    } catch (...) { shutdown(); throw; }
  }
  ~PoolExecutor() override { shutdown(); }
  void post(Task task) override {
    {
      std::lock_guard lock(core_->mutex);
      if (core_->stopping) return;
      core_->ready.push_back(std::move(task));
    }
    core_->wake.notify_one();
  }
  Timer schedule(SteadyTime at, Task task) override {
    Timer id;
    Core::Timers::node_type rollback;
    {
      std::lock_guard lock(core_->mutex);
      if (core_->stopping) return 0;
      id = core_->next_timer++;
      auto entry = core_->timers.emplace(Core::Key{at, id}, std::move(task)).first;
      try { core_->timer_index.emplace(id, entry); }
      catch (...) { rollback = core_->timers.extract(entry); throw; }
    }
    core_->wake.notify_all();
    return id;
  }
  void cancel(Timer id) noexcept override {
    // Destroy cancelled captures outside the queue lock: their destructors may
    // release an operation or executor. No stale timer heap retains requests.
    Task released;
    {
      std::lock_guard lock(core_->mutex);
      auto found = core_->timer_index.find(id);
      if (found == core_->timer_index.end()) return;
      released = std::move(found->second->second);
      core_->timers.erase(found->second);
      core_->timer_index.erase(found);
    }
    core_->wake.notify_all();
  }
  SteadyTime now() const noexcept override { return std::chrono::steady_clock::now(); }
  WallTime wall_now() const noexcept override { return std::chrono::system_clock::now(); }
  bool in_thread() const noexcept override { return pool_thread != nullptr || runtime_depth != 0; }
  void shutdown() noexcept override {
    std::vector<std::thread> threads;
    std::map<Core::Key, Task> released;
    {
      std::lock_guard shutdown_lock(shutdown_mutex_);
      {
        std::lock_guard lock(core_->mutex);
        core_->stopping = true;
        core_->timer_index.clear();
        released.swap(core_->timers);
      }
      threads.swap(threads_);
    }
    core_->wake.notify_all();
    for (auto& thread : threads) {
      if (pool_thread == core_.get()) thread.detach();
      else thread.join();
    }
  }
};

class ScopedTimer {
 public:
  ScopedTimer() = default;
  ~ScopedTimer() { reset(); }
  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;
  // The OperationState's executor owner outlives both scoped timers.
  void arm(Executor& owner, SteadyTime at, Executor::Task task) {
    reset();
    owner_ = &owner;
    id_ = owner.schedule(at, std::move(task));
  }
  void reset() noexcept {
    if (id_) owner_->cancel(std::exchange(id_, 0));
    owner_ = nullptr;
  }
 private:
  Executor* owner_ = nullptr;
  Executor::Timer id_ = 0;
};

bool equal_ascii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    if (lower(static_cast<unsigned char>(a[i])) != lower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}
void validate(const descriptor::ValidatedDescriptor& descriptor, const Options& options) {
  if (!options.policy) throw descriptor::ConfigError{"/policy", "admitted runtime snapshot", 1, "runtime options rejected"};
  const auto& ceiling = options.policy->admission();
  auto reject = [](std::string pointer) {
    throw descriptor::ConfigError{std::move(pointer), "value within admitted runtime policy", 1, "runtime options rejected"};
  };
  if (options.workers == 0 || options.workers > ceiling.workers) reject("/workers");
  if (options.transport.io_threads == 0 || options.transport.io_threads > ceiling.io_threads) reject("/io_threads");
  if (options.transport.resolver_threads == 0 || options.transport.resolver_threads > ceiling.resolver_threads) reject("/resolver_threads");
  if (options.transport.max_host_connections < 0 || static_cast<std::uint64_t>(options.transport.max_host_connections) > ceiling.max_host_connections) reject("/max_host_connections");
  if (options.transport.max_head_bytes == 0 || options.transport.max_head_bytes > ceiling.max_head_bytes) reject("/max_head_bytes");
  if (options.transport.dns_ttl.count() < 0 || static_cast<std::uint64_t>(options.transport.dns_ttl.count()) > ceiling.dns_ttl_seconds) reject("/dns_ttl_seconds");
  // The stall bounds are durations no longer than the longest admissible operation deadline, so
  // they share that ceiling; zero disables a bound.
  if (!detail::valid_stall_bound(options.transport.connect_timeout, ceiling.default_timeout_ms)) reject("/connect_timeout_ms");
  if (!detail::valid_stall_bound(options.transport.first_byte_timeout, ceiling.default_timeout_ms)) reject("/first_byte_timeout_ms");
  if (!detail::valid_stall_bound(options.transport.idle_timeout, ceiling.default_timeout_ms)) reject("/idle_timeout_ms");
  if (options.default_timeout.count() <= 0 || static_cast<std::uint64_t>(options.default_timeout.count()) > ceiling.default_timeout_ms) reject("/default_timeout_ms");
  if (options.slow_callback_threshold.count() < 0 || static_cast<std::uint64_t>(options.slow_callback_threshold.count()) > ceiling.slow_callback_threshold_ms) reject("/slow_callback_threshold_ms");
  if (options.retry_tokens > ceiling.retry_tokens) reject("/retry_tokens");
  if (!std::isfinite(options.retry_tokens_per_second) || options.retry_tokens_per_second < 0 || options.retry_tokens_per_second > ceiling.retry_tokens_per_second) reject("/retry_tokens_per_second");
  if (options.limits.max_operations == 0 || options.limits.max_operations > ceiling.max_operations) reject("/max_operations");
  if (options.limits.queued_body_chunks == 0 || options.limits.queued_body_chunks > ceiling.queued_body_chunks) reject("/queued_body_chunks");
  if (options.limits.queued_body_bytes == 0 || options.limits.queued_body_bytes > ceiling.queued_body_bytes) reject("/queued_body_bytes");
  if (options.limits.max_response_bytes == 0 || options.limits.max_response_bytes > ceiling.max_response_bytes) reject("/max_response_bytes");
  if (options.limits.max_error_bytes == 0 || options.limits.max_error_bytes > ceiling.max_error_bytes) reject("/max_error_bytes");
  if (options.limits.semantic.max_parts == 0 || options.limits.semantic.max_parts > ceiling.semantic_max_parts) reject("/semantic_max_parts");
  if (options.limits.semantic.max_content_bytes == 0 || options.limits.semantic.max_content_bytes > ceiling.semantic_max_content_bytes) reject("/semantic_max_content_bytes");
  if (options.limits.semantic.max_tool_bytes == 0 || options.limits.semantic.max_tool_bytes > ceiling.semantic_max_tool_bytes) reject("/semantic_max_tool_bytes");
  if (options.limits.semantic.max_json_depth == 0 || options.limits.semantic.max_json_depth > ceiling.semantic_max_json_depth) reject("/semantic_max_json_depth");
  if (options.limits.sse.max_line_bytes == 0 || options.limits.sse.max_line_bytes > ceiling.sse_max_line_bytes) reject("/sse_max_line_bytes");
  if (options.limits.sse.max_event_bytes == 0 || options.limits.sse.max_event_bytes > ceiling.sse_max_event_bytes) reject("/sse_max_event_bytes");
  if (options.limits.sse.max_total_bytes == 0 || options.limits.sse.max_total_bytes > ceiling.sse_max_total_bytes) reject("/sse_max_total_bytes");
  if (options.workers > ceiling.total_threads || options.transport.io_threads > ceiling.total_threads - options.workers ||
      options.transport.resolver_threads > ceiling.total_threads - options.workers - options.transport.io_threads) reject("/total_threads");
  if (options.api_key.size() > ceiling.api_key_bytes) reject("/api_key");
  for (unsigned char byte : options.api_key) if (byte < 32 || byte == 127) reject("/api_key");
  if (!options.api_key.empty()) {
    for (const auto& header : descriptor.headers())
      if (equal_ascii(header.first, "authorization") || equal_ascii(header.first, "x-api-key")) reject("/api_key");
  }
}

struct CallbackAbort {};
struct StopObserved {};
}  // namespace

struct ClientState : std::enable_shared_from_this<ClientState> {
  const descriptor::ValidatedDescriptor descriptor;
  const Options options;
  std::shared_ptr<Executor> executor;
  std::shared_ptr<AttemptTransport> transport;
  std::mutex mutex;
  std::condition_variable drained;
  std::unordered_map<OperationState*, std::shared_ptr<OperationState>> active;
  std::size_t prepared_operations = 0;
  TokenBucket bucket;
  std::function<double()> random01;
  bool stopping = false, asynchronous_shutdown = false, closing = false, closed = false;
  std::atomic<std::uint64_t> slow_callbacks{0}, callback_exceptions{0};

  ClientState(descriptor::ValidatedDescriptor d, Options o, std::shared_ptr<Executor> e,
              std::shared_ptr<AttemptTransport> t, std::function<double()> random)
      : descriptor(std::move(d)), options(std::move(o)), executor(std::move(e)), transport(std::move(t)),
        bucket(options.retry_tokens, options.retry_tokens_per_second, executor->now()), random01(std::move(random)) {
    if (!random01) {
      random01 = [engine = std::mt19937_64(std::random_device{}())]() mutable {
        return std::generate_canonical<double, 53>(engine);
      };
    }
  }
  bool in_thread() const noexcept {
    return runtime_depth || pool_thread || sp::transport::on_io_thread() || executor->in_thread();
  }
  void shutdown() noexcept;
  void release(OperationState*);
  void close_resources() noexcept;
};

struct OperationState : std::enable_shared_from_this<OperationState> {
  enum class Stage { Initial, InFlight, Backoff, Terminal };
  struct Head {
    transport::ResponseHead value;
    SteadyTime received;
    WallTime wall_received;
  };
  std::shared_ptr<ClientState> client;
  // Retained after terminal delivery solely for worker-misuse detection.
  std::shared_ptr<Executor> executor;
  RunOptions options;
  Callbacks callbacks;
  SteadyTime deadline;
  transport::HttpRequest request;
  std::shared_ptr<const NativeContext> native_context;
  enum class Family { Chat, Messages, Responses, Gemini, Interactions };
  Family family = Family::Chat;
  bool admitted = false;
  bool prepared = false;
  std::string prepared_model;
  std::optional<std::uint64_t> prepared_max_output_tokens;
  std::optional<std::uint64_t> prepared_model_invocation_limit;

  mutable std::mutex mutex;
  std::condition_variable joined;
  bool ready = false, scheduled = false, finished = false, accepting = false;
  bool cancellation = false, deadline_fired = false, retry_fired = false, control_pending = false;
  bool head_received = false, done_received = false, paused = false;
  std::size_t paused_bytes = 0;
  std::uint64_t generation = 0;
  std::optional<Head> head_input;
  std::deque<std::string> body_input;
  std::optional<transport::Result> done_input;
  std::optional<ErrorKind> input_error;
  OperationStats statistics;
  Result result;
  struct StopCallback {
    std::weak_ptr<OperationState> operation;
    void operator()() const noexcept { if (auto state = operation.lock()) state->cancel(); }
  };
  std::optional<std::stop_callback<StopCallback>> stop_callback;
  // Fixed before activation; consumed by the first actor turn.
  std::optional<Error> initial_error;

  // Everything below is confined to the serialized actor.
  Stage stage = Stage::Initial;
  ScopedTimer deadline_timer, retry_timer;
  std::uint32_t attempts = 0;
  std::unique_ptr<Attempt> attempt;
  std::unique_ptr<Accumulator> accumulator;
  std::unique_ptr<chat::Codec> chat_codec;
  std::unique_ptr<messages::Codec> messages_codec;
  std::unique_ptr<responses::Codec> responses_codec;
  std::unique_ptr<gemini::Codec> gemini_codec;
  std::unique_ptr<interactions::Codec> interactions_codec;
  std::unique_ptr<transport::SseFramer> framer;
  std::optional<Head> response;
  ResponseInfo response_info;
  std::string buffered;
  std::shared_ptr<const json::Document> observed_document;
  std::size_t response_bytes = 0;
  bool semantic_output = false;
  std::optional<Error> stopping_error;
  std::optional<transport::Result> terminal_wire;
  std::optional<Outcome> pending_failure;
  bool prior_usage_unknown = false;
  std::vector<RawWire> prior_raw_events;

  OperationState(std::shared_ptr<ClientState> c, RunOptions o, Callbacks cb, SteadyTime end)
      : client(std::move(c)), executor(client ? client->executor : nullptr), options(std::move(o)),
        callbacks(std::move(cb)), deadline(end) {}

  bool mark_scheduled_locked() {
    if (!ready || scheduled || finished) return false;
    scheduled = true;
    return true;
  }
  void post_actor() {
    executor->post([self = shared_from_this()] { self->run(); });
  }
  void cancel() noexcept {
    bool post;
    {
      std::lock_guard lock(mutex);
      if (finished || cancellation) return;
      cancellation = true;
      control_pending = true;
      post = mark_scheduled_locked();
    }
    if (post) post_actor();
  }
  void timer(bool retry) {
    bool post;
    {
      std::lock_guard lock(mutex);
      if (finished) return;
      if (retry) retry_fired = true;
      else { deadline_fired = true; control_pending = true; }
      post = mark_scheduled_locked();
    }
    if (post) post_actor();
  }
  void activate() {
    if (!initial_error && options.stop_token.stop_possible()) {
      stop_callback.emplace(options.stop_token, StopCallback{weak_from_this()});
    }
    bool post;
    {
      std::lock_guard lock(mutex);
      ready = true;
      post = mark_scheduled_locked();
    }
    if (post) post_actor();
  }
  void receive_head(std::uint64_t epoch, const transport::ResponseHead& value) {
    RuntimeScope scope;
    const auto received = executor->now();
    const auto wall_received = executor->wall_now();
    bool post;
    {
      std::lock_guard lock(mutex);
      if (finished || !accepting || epoch != generation) return;
      std::size_t bytes = 0;
      bool oversized = false;
      const auto limit = client->options.transport.max_head_bytes;
      for (const auto& header : value.headers) {
        if (header.name.size() > limit - bytes) { oversized = true; break; }
        bytes += header.name.size();
        if (header.value.size() > limit - bytes) { oversized = true; break; }
        bytes += header.value.size();
        if (limit - bytes < 4) { oversized = true; break; }
        bytes += 4;
      }
      if (oversized) input_error = ErrorKind::ResourceLimit;
      else if (head_received) input_error = ErrorKind::ProtocolCorrupt;
      else {
        head_received = true;
        head_input = Head{value, received, wall_received};
      }
      post = mark_scheduled_locked();
    }
    if (post) post_actor();
  }
  bool receive_body(std::uint64_t epoch, std::string_view value) {
    RuntimeScope scope;
    bool post = false, consumed = true;
    {
      std::lock_guard lock(mutex);
      if (finished || !accepting || epoch != generation || input_error) return true;
      const auto& limits = client->options.limits;
      if (value.size() > limits.queued_body_bytes) {
        input_error = ErrorKind::ResourceLimit;
        post = mark_scheduled_locked();
      } else if (!value.empty()) {
        if (body_input.size() >= limits.queued_body_chunks ||
            value.size() > limits.queued_body_bytes - statistics.queued_bytes) {
          paused = true;
          paused_bytes = value.size();
          ++statistics.pauses;
          consumed = false;
        } else {
          body_input.emplace_back(value);
          statistics.queued_bytes += value.size();
          statistics.queued_chunks = body_input.size();
          statistics.peak_queued_bytes = std::max(statistics.peak_queued_bytes, statistics.queued_bytes);
        }
        post = mark_scheduled_locked();
      }
    }
    if (post) post_actor();
    return consumed;
  }
  void receive_done(std::uint64_t epoch, const transport::Result& value) {
    RuntimeScope scope;
    bool post;
    {
      std::lock_guard lock(mutex);
      if (finished || epoch != generation || done_received) return;
      done_received = true;
      // No unbounded diagnostic text crosses the wire-event boundary.
      transport::Result bounded;
      bounded.status = value.status;
      bounded.failure = value.failure;
      bounded.http_status = value.http_status;
      bounded.curl_code = value.curl_code;
      bounded.attempt = value.attempt;
      bounded.elapsed = value.elapsed;
      done_input = std::move(bounded);
      post = mark_scheduled_locked();
    }
    if (post) post_actor();
  }
  void note_callback(SteadyTime before, bool threw) {
    if (threw) ++client->callback_exceptions;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(executor->now() - before) >=
        client->options.slow_callback_threshold) ++client->slow_callbacks;
  }
  void semantic(const Event& event) {
    if (std::holds_alternative<Commit>(event) || std::holds_alternative<Fail>(event)) return;
    if (const auto* raw = std::get_if<RawWire>(&event)) observed_document = raw->payload;
    if (!std::holds_alternative<Begin>(event) && !std::holds_alternative<RawWire>(event) &&
        !std::holds_alternative<ResponseEnvelope>(event)) semantic_output = true;
    if (stopping_error) return;
    if (callbacks.on_event) {
      const auto before = executor->now();
      try { callbacks.on_event(event); }
      catch (...) { note_callback(before, true); throw CallbackAbort{}; }
      note_callback(before, false);
    }
    {
      std::lock_guard lock(mutex);
      if (cancellation || deadline_fired || executor->now() >= deadline) throw StopObserved{};
    }
  }
  void fresh_codec() {
    chat_codec.reset();
    messages_codec.reset();
    responses_codec.reset();
    gemini_codec.reset();
    interactions_codec.reset();
    accumulator = std::make_unique<Accumulator>(client->options.limits.semantic,
        [this](const Event& event) { semantic(event); }, client->options.limits.max_response_bytes);
    if (family == Family::Chat) chat_codec = std::make_unique<chat::Codec>(client->descriptor,
        options.streaming ? chat::Mode::Sse : chat::Mode::Buffered, *accumulator,
        client->options.limits.semantic, native_context);
    else if (family == Family::Messages) messages_codec = std::make_unique<messages::Codec>(client->descriptor,
        options.streaming ? messages::Mode::Sse : messages::Mode::Buffered, *accumulator,
        native_context, client->options.limits.semantic);
    else if (family == Family::Responses) responses_codec = std::make_unique<responses::Codec>(client->descriptor,
        options.streaming ? responses::Mode::Sse : responses::Mode::Buffered, *accumulator,
        native_context, client->options.limits.semantic);
    else if (family == Family::Gemini) gemini_codec = std::make_unique<gemini::Codec>(client->descriptor,
        options.streaming ? gemini::Mode::Sse : gemini::Mode::Buffered, *accumulator,
        native_context, client->options.limits.semantic);
    else interactions_codec = std::make_unique<interactions::Codec>(client->descriptor,
        options.streaming ? interactions::Mode::Sse : interactions::Mode::Buffered, *accumulator,
        native_context, client->options.limits.semantic);
    framer = options.streaming ? std::make_unique<transport::SseFramer>(client->options.limits.sse) : nullptr;
    response.reset();
    response_info = {};
    buffered.clear();
    observed_document.reset();
    response_bytes = 0;
    semantic_output = false;
    stopping_error.reset();
  }
  void dispatch() {
    const bool retry = stage == Stage::Backoff;
    fresh_codec();
    const auto epoch = generation + 1;
    transport::Callbacks wire;
    auto weak = weak_from_this();
    wire.on_head = [weak, epoch](const auto& head) { if (auto self = weak.lock()) self->receive_head(epoch, head); };
    wire.on_body = [weak, epoch](std::string_view bytes) {
      if (auto self = weak.lock()) return self->receive_body(epoch, bytes);
      return true;
    };
    wire.on_done = [weak, epoch](const auto& done) { if (auto self = weak.lock()) self->receive_done(epoch, done); };
    auto outbound = options.retry->enabled && attempts + 1 < options.retry->max_attempts ? request : std::move(request);
    outbound.deadline = deadline;
    {
      std::unique_lock lock(mutex);
      // Dispatch is linearized here, after allocation/encoding and immediately
      // before calling the transport. No token is spent on a cancelled wait.
      if (cancellation || deadline_fired || executor->now() >= deadline) {
        const auto kind = cancellation ? ErrorKind::Cancelled : ErrorKind::DeadlineExceeded;
        lock.unlock();
        stop(kind);
        return;
      }
      if (retry) {
        std::lock_guard budget_lock(client->mutex);
        if (!client->bucket.consume(executor->now())) return;
        if (pending_failure) {
          auto& observations = std::get<Failure>(*pending_failure).partial.raw_events;
          if (observations.size() > prior_raw_events.max_size() - prior_raw_events.size())
            throw std::length_error("runtime raw observation extent exceeded");
          prior_raw_events.reserve(prior_raw_events.size() + observations.size());
          prior_raw_events.insert(prior_raw_events.end(), std::make_move_iterator(observations.begin()),
              std::make_move_iterator(observations.end()));
        }
        pending_failure.reset();
      }
      stage = Stage::InFlight;
      generation = epoch;
      accepting = true;
      head_received = done_received = paused = false;
      head_input.reset(); done_input.reset(); input_error.reset();
      body_input.clear();
      statistics.queued_bytes = statistics.queued_chunks = 0;
      ++attempts;
      statistics.attempts = attempts;
    }
    try {
      attempt = client->transport->start(std::move(outbound), std::move(wire));
      if (!attempt) throw std::runtime_error("missing attempt");
    } catch (...) {
      transport::Result failed;
      failed.status = transport::Status::Failed;
      failed.failure = transport::FailureKind::Other;
      receive_done(epoch, failed);
    }
  }
  void stop(ErrorKind kind) {
    if (stage == Stage::Terminal) return;
    if (accumulator && accumulator->terminal()) {
      if (attempt) attempt->cancel();
      return;
    }
    if (!stopping_error) stopping_error = error_for(kind);
    if (stage == Stage::Initial || stage == Stage::Backoff) {
      Failure failure{*stopping_error, {}};
      if (pending_failure) {
        auto& previous = std::get<Failure>(*pending_failure);
        failure.error.attempt = previous.error.attempt;
        failure.error.retry_safety = previous.error.retry_safety;
        failure.partial = std::move(previous.partial);
      } else if (attempts == 0) {
        failure.error.retry_safety = RetrySafety::NotSent;
      }
      failure.error.attempt.attempts = attempts;
      deliver(Outcome{std::move(failure)});
      return;
    }
    {
      std::lock_guard lock(mutex);
      accepting = false;
      body_input.clear();
      statistics.queued_bytes = statistics.queued_chunks = 0;
      paused = false;
    }
    if (attempt) attempt->cancel();
  }
  void fail_semantic(ErrorKind kind) {
    if (!accumulator->terminal()) accumulator->accept(Fail{error_for(kind)});
    {
      std::lock_guard lock(mutex);
      accepting = false;
      body_input.clear();
      statistics.queued_bytes = statistics.queued_chunks = 0;
      paused = false;
    }
    if (attempt) attempt->cancel();
  }
  void inspect(std::string_view body, bool retain_body = false) {
    if (body.size() > client->options.limits.max_error_bytes) {
      response_info = {};
      response_info.kind = ErrorKind::ResourceLimit;
      response_info.retry_class = RetryClass::Never;
      return;
    }
    static const std::vector<transport::Header> no_headers;
    auto classify = [&](json::Value document) {
      response_info = inspect_document_response(*client->options.policy, client->descriptor.family(),
          response ? response->value.status : 0, response ? response->value.headers : no_headers, document,
          response ? response->received : executor->now(), response ? response->wall_received : executor->wall_now());
    };
    if (observed_document) { classify(observed_document->root()); return; }
    auto parsed = json::parse(body, {client->options.limits.max_error_bytes,
        static_cast<std::size_t>(client->options.policy->admission().error_json_depth)});
    if (auto* document = std::get_if<json::Document>(&parsed)) {
      if (retain_body) {
        observed_document = std::make_shared<const json::Document>(std::move(*document));
        classify(observed_document->root());
      } else classify(document->root());
    } else {
      auto& rejected = std::get<json::ParseError>(parsed);
      if (retain_body && rejected.code == json::ParseCode::DuplicateKey && rejected.context.root().valid())
        observed_document = std::make_shared<const json::Document>(std::move(rejected.context));
      // Ambiguous duplicate fields remain diagnostic, never policy authority.
      classify({});
    }
  }
  void body(std::string_view bytes) {
    const auto& limits = client->options.limits;
    if (!response) { fail_semantic(ErrorKind::ProtocolCorrupt); return; }
    if (bytes.size() > limits.max_response_bytes - response_bytes) { fail_semantic(ErrorKind::ResourceLimit); return; }
    response_bytes += bytes.size();
    const bool http_error = response->value.status < 200 || response->value.status >= 300;
    if (http_error || !options.streaming) {
      const auto limit = http_error ? limits.max_error_bytes : limits.max_response_bytes;
      if (bytes.size() > limit - buffered.size()) { fail_semantic(ErrorKind::ResourceLimit); return; }
      buffered.append(bytes);
      return;
    }
    framer->feed(bytes, [this](const transport::SseFrame& frame) {
      if (options.stop_token.stop_requested()) {
        fail_semantic(ErrorKind::Cancelled);
        return false;
      }
      observed_document.reset();
      const bool accepted = chat_codec ? chat_codec->frame(frame.event, frame.data) :
          messages_codec ? messages_codec->frame(frame.event, frame.data) :
          responses_codec ? responses_codec->frame(frame.event, frame.data) :
          gemini_codec ? gemini_codec->frame(frame.event, frame.data) :
          interactions_codec->frame(frame.event, frame.data);
      if (accumulator->terminal()) {
        const auto* failed = std::get_if<Failure>(&*accumulator->outcome());
        if (failed && failed->error.kind == ErrorKind::RemoteFailure) inspect(frame.data);
      }
      return accepted;
    });
    if (framer->error() != transport::SseError::None) {
      fail_semantic(framer->error() == transport::SseError::ResourceLimit ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt);
    } else if (accumulator->terminal()) {
      // Semantic failure is already decided; cancellation only harvests wire evidence.
      if (attempt) attempt->cancel();
      std::lock_guard lock(mutex);
      accepting = false;
      body_input.clear();
      statistics.queued_bytes = statistics.queued_chunks = 0;
      paused = false;
    }
  }
  void done(transport::Result wire) {
    {
      std::lock_guard lock(mutex);
      accepting = false;
      const auto extra = wire.attempt.transport_internal_resends;
      attempts = extra > std::numeric_limits<std::uint32_t>::max() - attempts
          ? std::numeric_limits<std::uint32_t>::max() : attempts + extra;
      statistics.attempts = attempts;
    }
    attempt.reset();
    terminal_wire = wire;
    const bool semantic_failure_decided = accumulator->terminal();
    const bool http_error = (wire.http_status > 0 && (wire.http_status < 200 || wire.http_status >= 300)) ||
        (response && (response->value.status < 200 || response->value.status >= 300));
    if (http_error) inspect(buffered, true);
    const auto close_error = stopping_error ? *stopping_error :
        classify_failure(wire, response_info, {}, semantic_output, attempts, executor->now());
    const bool normal = wire.status == transport::Status::Completed && !stopping_error && !http_error;
    if (!accumulator->terminal()) {
      if (http_error) {
        if (observed_document) accumulator->accept(RawWire{"http.error", observed_document}, &close_error);
        if (!accumulator->terminal()) accumulator->accept(Fail{close_error});
      }
      else if (options.streaming) {
        framer->finish();
        if (framer->error() != transport::SseError::None)
          fail_semantic(framer->error() == transport::SseError::ResourceLimit ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt);
        else if (chat_codec) chat_codec->finish({normal, close_error.kind});
        else if (messages_codec) messages_codec->finish({normal, close_error.kind});
        else if (responses_codec) responses_codec->finish({normal, close_error.kind});
        else if (gemini_codec) gemini_codec->finish({normal, close_error.kind});
        else interactions_codec->finish({normal, close_error.kind});
      } else {
        if (chat_codec) chat_codec->buffered(buffered, {normal, close_error.kind});
        else if (messages_codec) messages_codec->buffered(buffered, {normal, close_error.kind});
        else if (responses_codec) responses_codec->buffered(buffered, {normal, close_error.kind});
        else if (gemini_codec) gemini_codec->buffered(buffered, {normal, close_error.kind});
        else interactions_codec->buffered(buffered, {normal, close_error.kind});
        if (accumulator->terminal()) {
          const auto* failure = std::get_if<Failure>(&*accumulator->outcome());
          if (failure && (failure->error.kind == ErrorKind::RemoteFailure ||
              (!normal && !semantic_output && (failure->error.kind == ErrorKind::Transport ||
                                               failure->error.kind == ErrorKind::Truncated))))
            inspect(buffered);
        }
      }
    }
    if (!accumulator->terminal()) accumulator->accept(Fail{error_for(ErrorKind::ProtocolCorrupt)});
    auto outcome = accumulator->take_outcome();
    if (auto* failure = std::get_if<Failure>(&*outcome)) {
      if (!semantic_failure_decided && !normal && failure->error.kind == close_error.kind)
        failure->error = close_error;
      failure->error = classify_failure(wire, response_info, failure->error, semantic_output, attempts, executor->now());
      bool available;
      double random;
      {
        std::lock_guard lock(client->mutex);
        available = client->bucket.available(executor->now());
        random = available && options.retry->enabled ? client->random01() : 0;
      }
      auto at = available ? retry_at(failure->error, *options.retry, attempts, executor->now(), deadline, random) : std::nullopt;
      if (at) {
        prior_usage_unknown = prior_usage_unknown || !prior_attempt_proves_no_usage(*failure);
        pending_failure = std::move(outcome);
        stage = Stage::Backoff;
        chat_codec.reset(); messages_codec.reset(); responses_codec.reset(); gemini_codec.reset(); interactions_codec.reset(); accumulator.reset(); framer.reset();
        terminal_wire.reset();
        buffered.clear(); response.reset();
        retry_timer.arm(*executor, *at, [weak = weak_from_this()] { if (auto self = weak.lock()) self->timer(true); });
        return;
      }
    }
    deliver(std::move(*outcome));
  }
  void deliver(Outcome outcome) {
    if (!prior_raw_events.empty()) {
      auto& observations = std::visit([](auto& value) -> std::vector<RawWire>& {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Completion>) return value.raw_events;
        else return value.partial.raw_events;
      }, outcome);
      if (observations.size() > prior_raw_events.max_size() - prior_raw_events.size())
        throw std::length_error("runtime raw observation extent exceeded");
      prior_raw_events.reserve(prior_raw_events.size() + observations.size());
      prior_raw_events.insert(prior_raw_events.end(), std::make_move_iterator(observations.begin()),
          std::make_move_iterator(observations.end()));
      observations = std::move(prior_raw_events);
    }
    if (auto* completion = std::get_if<Completion>(&outcome)) {
      completion->attempt.attempts = attempts;
      completion->attempt.prior_usage_unknown = prior_usage_unknown;
      if (terminal_wire) {
        const auto& observed = terminal_wire->attempt;
        completion->attempt.request_may_have_left = observed.reached >= transport::Stage::RequestStarted;
        completion->attempt.request_body_bytes = observed.request_body_bytes;
        completion->attempt.response_head_seen = observed.response_head_seen;
        completion->attempt.transport_internal_resends = observed.transport_internal_resends;
      }
    } else std::get<Failure>(outcome).error.attempt.prior_usage_unknown = prior_usage_unknown;
    {
      std::lock_guard lock(mutex);
      accepting = false;
      result = std::make_shared<const Outcome>(std::move(outcome));
      stage = Stage::Terminal;
      body_input.clear(); head_input.reset(); done_input.reset();
      statistics.queued_bytes = statistics.queued_chunks = 0;
    }
    deadline_timer.reset();
    retry_timer.reset();
    if (attempt) { attempt->cancel(); attempt.reset(); }
    // A stop callback may be executing cancel(). Never destroy its registration
    // while holding mutex, which that callback needs to leave.
    stop_callback.reset();
    RuntimeScope scope;
    if (callbacks.on_outcome) {
      const auto before = executor ? executor->now() : SteadyTime{};
      bool threw = false;
      try { callbacks.on_outcome(result); } catch (...) { threw = true; }
      if (client) note_callback(before, threw);
    }
    callbacks = {};
    chat_codec.reset(); messages_codec.reset(); responses_codec.reset(); gemini_codec.reset(); interactions_codec.reset(); accumulator.reset(); framer.reset();
    pending_failure.reset(); response.reset(); terminal_wire.reset();
    std::string{}.swap(buffered);
    request = {}; native_context.reset();
    auto owner = std::move(client);
    if (owner && admitted) owner->release(this);
    {
      std::lock_guard lock(mutex);
      finished = true;
    }
    joined.notify_all();
  }
  void run() noexcept {
    RuntimeScope scope;
    try { step(); }
    catch (const CallbackAbort&) {
      stopping_error = error_for(ErrorKind::Misuse);
      fail_semantic(ErrorKind::Misuse);
      if (!attempt) finish_interrupted();
    } catch (const StopObserved&) {
      bool cancelled;
      { std::lock_guard lock(mutex); cancelled = cancellation; }
      stop(cancelled ? ErrorKind::Cancelled : ErrorKind::DeadlineExceeded);
      // A callback can interrupt decoding the already-final wire event.
      if (stage == Stage::InFlight && !attempt) finish_interrupted();
    } catch (...) {
      if (stage != Stage::Terminal) {
        stopping_error = error_for(ErrorKind::ResourceLimit);
        if (attempt) attempt->cancel();
        deliver(Outcome{Failure{*stopping_error, {}}});
      }
    }
    bool again = false;
    {
      std::lock_guard lock(mutex);
      if (!finished && (head_input || !body_input.empty() || done_input || input_error ||
          control_pending || retry_fired)) again = true;
      else scheduled = false;
    }
    if (again) post_actor();
  }
  void finish_interrupted() {
    if (!accumulator->terminal()) accumulator->accept(Fail{*stopping_error});
    auto outcome = accumulator->take_outcome();
    if (auto* failure = std::get_if<Failure>(&*outcome)) {
      if (terminal_wire)
        failure->error = classify_failure(*terminal_wire, response_info, failure->error,
            semantic_output, attempts, executor->now());
      else {
        failure->error.attempt.attempts = attempts;
        failure->error.retry_safety = semantic_output ? RetrySafety::OutputObserved : RetrySafety::PossiblyAccepted;
      }
    }
    deliver(std::move(*outcome));
  }
  void step() {
    if (stage == Stage::Terminal) return;
    if (initial_error) {
      deliver(Outcome{Failure{std::move(*initial_error), {}}});
      return;
    }
    bool cancelled, expired, retry;
    std::optional<ErrorKind> failure;
    std::optional<Head> head;
    {
      std::lock_guard lock(mutex);
      cancelled = cancellation;
      expired = deadline_fired || executor->now() >= deadline;
      retry = std::exchange(retry_fired, false);
      failure = std::exchange(input_error, {});
      control_pending = false;
      head = std::move(head_input); head_input.reset();
    }
    if (head) { response = std::move(head); inspect({}); }
    if (failure && accumulator) fail_semantic(*failure);
    if (cancelled || expired) {
      stop(cancelled ? ErrorKind::Cancelled : ErrorKind::DeadlineExceeded);
      if (stage == Stage::Terminal) return;
    }
    if (stage == Stage::Initial) {
      deadline_timer.arm(*executor, deadline, [weak = weak_from_this()] { if (auto self = weak.lock()) self->timer(false); });
      dispatch();
      return;
    }
    if (stage == Stage::Backoff) {
      if (retry) {
        retry_timer.reset();
        dispatch();
        if (pending_failure) deliver(std::move(*pending_failure));
      }
      return;
    }
    std::string bytes;
    std::optional<transport::Result> completed;
    bool resume = false;
    {
      std::lock_guard lock(mutex);
      if (!body_input.empty()) {
        bytes = std::move(body_input.front()); body_input.pop_front();
        statistics.queued_bytes -= bytes.size();
        statistics.queued_chunks = body_input.size();
      } else if (done_input) { completed = std::move(done_input); done_input.reset(); }
      if (paused && accepting && body_input.size() < client->options.limits.queued_body_chunks &&
          paused_bytes <= client->options.limits.queued_body_bytes - statistics.queued_bytes) {
        paused = false;
        resume = true;
      }
    }
    if (!bytes.empty() && !stopping_error && !accumulator->terminal()) body(bytes);
    if (resume && attempt && !stopping_error && !accumulator->terminal()) attempt->resume();
    if (completed) done(std::move(*completed));
  }
};

void ClientState::close_resources() noexcept {
  {
    std::lock_guard lock(mutex);
    if (closing) return;
    closing = true;
  }
  transport->shutdown();
  executor->shutdown();
  {
    std::lock_guard lock(mutex);
    closed = true;
  }
  drained.notify_all();
}
void ClientState::release(OperationState* operation) {
  bool close;
  {
    std::lock_guard lock(mutex);
    active.erase(operation);
    close = stopping && asynchronous_shutdown && active.empty();
  }
  drained.notify_all();
  if (close) close_resources();
}
void ClientState::shutdown() noexcept {
  const bool asynchronous = in_thread();
  std::vector<std::shared_ptr<OperationState>> operations;
  bool empty;
  {
    std::lock_guard lock(mutex);
    stopping = true;
    asynchronous_shutdown = asynchronous_shutdown || asynchronous;
    operations.reserve(active.size());
    for (const auto& entry : active) operations.push_back(entry.second);
    empty = active.empty();
  }
  for (const auto& operation : operations) operation->cancel();
  if (asynchronous) {
    if (empty) executor->post([self = shared_from_this()] { self->close_resources(); });
    return;
  }
  {
    std::unique_lock lock(mutex);
    drained.wait(lock, [this] { return active.empty(); });
  }
  close_resources();
  std::unique_lock lock(mutex);
  drained.wait(lock, [this] { return closed; });
}

Client ClientAccess::make(descriptor::ValidatedDescriptor descriptor, Options options,
                          std::shared_ptr<Executor> executor, std::shared_ptr<AttemptTransport> transport,
                          std::function<double()> random01, RealTransportFactory real_transport) {
  validate(descriptor, options);
  if (!transport && !real_transport)
    throw descriptor::ConfigError{"/transport", "a transport backend", 1, "runtime options rejected"};
  if (!executor) executor = std::make_shared<PoolExecutor>(options.workers);
  if (!transport) {
    auto wire = options.transport;
    if (wire.resolve) {
      auto resolve = std::move(wire.resolve);
      wire.resolve = [resolve = std::move(resolve)](const std::string& host) {
        RuntimeScope scope;
        return resolve(host);
      };
    }
    transport = real_transport(std::move(wire));
  }
  return Client(std::make_shared<ClientState>(std::move(descriptor), std::move(options),
      std::move(executor), std::move(transport), std::move(random01)));
}
PreparedRequest ClientAccess::prepare_control(Client& client, Request request, RunOptions options,
                                             std::function<bool(std::string&)> transform) {
  auto preparation = client.prepare(std::move(request), std::move(options));
  if (preparation.error() || !transform) return preparation;
  auto& state = *preparation.state_;
  const auto digest = [](std::string_view bytes) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> result{};
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), result.data());
    return result;
  };
  const auto before = digest(state.request.body);
  std::optional<ErrorKind> error;
  try {
    if (!transform(state.request.body)) error = ErrorKind::InvalidRequest;
    const auto& resources = state.client->descriptor.policy()->resources();
    const auto ceiling = state.family == OperationState::Family::Chat
        ? resources.chat_text_request_bytes : resources.request_bytes;
    if (state.request.body.size() > ceiling) error = ErrorKind::ResourceLimit;
  } catch (...) { error = ErrorKind::InvalidRequest; }
  if (before != digest(state.request.body)) {
    if (state.native_context) state.native_context = state.native_context->decoding_only();
    // A private qualification control changed the actual body: original typed
    // cap/invocation proofs cannot authorize a bounded ordinary dispatch.
    state.prepared_max_output_tokens.reset();
    state.prepared_model_invocation_limit.reset();
  }
  if (error) {
    state.initial_error = error_for(*error);
    state.initial_error->retry_safety = RetrySafety::NotSent;
  }
  return preparation;
}
OperationStats ClientAccess::stats(const Operation& operation) {
  if (!operation.state_) return {};
  std::lock_guard lock(operation.state_->mutex);
  return operation.state_->statistics;
}
}  // namespace sp::runtime::detail

namespace sp::runtime {
Options::Options(configuration::PolicySnapshot snapshot) : policy(std::move(snapshot)) {
  if (!policy) throw descriptor::ConfigError{"/policy", "admitted runtime snapshot", 1, "runtime options rejected"};
  const auto& values = policy->defaults();
  workers = values.workers;
  transport.io_threads = static_cast<unsigned>(values.io_threads);
  transport.resolver_threads = static_cast<unsigned>(values.resolver_threads);
  transport.max_host_connections = static_cast<long>(values.max_host_connections);
  transport.max_head_bytes = values.max_head_bytes;
  transport.dns_ttl = std::chrono::seconds(values.dns_ttl_seconds);
  default_timeout = std::chrono::milliseconds(values.default_timeout_ms);
  slow_callback_threshold = std::chrono::milliseconds(values.slow_callback_threshold_ms);
  retry_tokens = values.retry_tokens;
  retry_tokens_per_second = values.retry_tokens_per_second;
  limits.max_operations = values.max_operations;
  limits.queued_body_chunks = values.queued_body_chunks;
  limits.queued_body_bytes = values.queued_body_bytes;
  limits.max_response_bytes = values.max_response_bytes;
  limits.max_error_bytes = values.max_error_bytes;
  limits.semantic.max_parts = values.semantic_max_parts;
  limits.semantic.max_content_bytes = values.semantic_max_content_bytes;
  limits.semantic.max_tool_bytes = values.semantic_max_tool_bytes;
  limits.semantic.max_json_depth = values.semantic_max_json_depth;
  limits.sse.max_line_bytes = values.sse_max_line_bytes;
  limits.sse.max_event_bytes = values.sse_max_event_bytes;
  limits.sse.max_total_bytes = values.sse_max_total_bytes;
}
AdmissionError::AdmissionError(ErrorKind kind) {
  auto error = detail::error_for(kind);
  error.retry_safety = RetrySafety::NotSent;
  outcome_ = std::make_shared<const Outcome>(Failure{std::move(error), {}});
}
const char* AdmissionError::what() const noexcept { return "runtime admission rejected"; }

Operation::Operation(std::shared_ptr<detail::OperationState> state) : state_(std::move(state)) {}
Operation::~Operation() { cancel(); }
Operation::Operation(Operation&& other) noexcept : state_(std::move(other.state_)) {}
Operation& Operation::operator=(Operation&& other) noexcept {
  if (this != &other) { cancel(); state_ = std::move(other.state_); }
  return *this;
}
void Operation::cancel() noexcept { if (state_) state_->cancel(); }
void Operation::detach() noexcept { state_.reset(); }
bool Operation::valid() const noexcept { return bool(state_); }
Result Operation::join() const {
  auto state = state_;
  if (!state || detail::runtime_depth || detail::pool_thread || transport::on_io_thread() ||
      (state->executor && state->executor->in_thread()))
    return detail::misuse_result();
  std::unique_lock lock(state->mutex);
  state->joined.wait(lock, [&] { return state->finished; });
  return state->result;
}

InterfaceContract interface_contract() noexcept {
  const auto core = ::sp::core_interface_contract();
  // Literal implementation revision is independent of the consumer's headers.
  if (core.revision != 4 || codec_interface_revision() != 4 || descriptor::interface_revision() != 4) return {0, 0};
  return {4, core.capabilities | capability::TypedRuntime | capability::PreparedAdmission |
      capability::CompleteAttemptEvidence};
}
const char* InterfaceContractError::what() const noexcept {
  return "SchemaProvider loaded interface contract is unsupported";
}
void require_interface_contract(std::uint32_t expected_revision, std::uint64_t required_capabilities) {
  const auto actual = interface_contract();
  if (actual.revision != expected_revision ||
      (actual.capabilities & required_capabilities) != required_capabilities)
    throw InterfaceContractError();
}
Client::Client(std::shared_ptr<detail::ClientState> state) : state_(std::move(state)) {}
Client::~Client() { if (state_) state_->shutdown(); }
Client::Client(Client&& other) noexcept : state_(std::move(other.state_)) {}
Client& Client::operator=(Client&& other) noexcept {
  if (this != &other) {
    if (state_) state_->shutdown();
    state_ = std::move(other.state_);
  }
  return *this;
}
PreparedRequest::PreparedRequest(std::shared_ptr<detail::OperationState> state)
    : state_(std::move(state)) {}
PreparedRequest::~PreparedRequest() { release(); }
PreparedRequest::PreparedRequest(PreparedRequest&& other) noexcept
    : state_(std::move(other.state_)) {}
PreparedRequest& PreparedRequest::operator=(PreparedRequest&& other) noexcept {
  if (this != &other) {
    release();
    state_ = std::move(other.state_);
  }
  return *this;
}
void PreparedRequest::release() noexcept {
  if (!state_) return;
  auto client = state_->client;
  if (client) {
    std::lock_guard lock(client->mutex);
    if (state_->prepared) {
      --client->prepared_operations;
      state_->prepared = false;
    }
  }
  state_.reset();
}
bool PreparedRequest::valid() const noexcept { return state_ && state_->prepared; }
const Error* PreparedRequest::error() const noexcept {
  return state_ && state_->initial_error ? &*state_->initial_error : nullptr;
}
std::string_view PreparedRequest::family() const noexcept {
  return state_ && state_->client ? state_->client->descriptor.family() : std::string_view{};
}
std::string_view PreparedRequest::model() const noexcept {
  return state_ ? state_->prepared_model : std::string_view{};
}
std::string_view PreparedRequest::encoded_body() const noexcept {
  return state_ ? state_->request.body : std::string_view{};
}
SteadyTime PreparedRequest::deadline() const noexcept {
  return state_ ? state_->deadline : SteadyTime{};
}
const descriptor::ValidatedDescriptor* PreparedRequest::descriptor() const noexcept {
  return state_ && state_->client ? &state_->client->descriptor : nullptr;
}
const RetryPolicy* PreparedRequest::retry_policy() const noexcept {
  return state_ && state_->options.retry ? &*state_->options.retry : nullptr;
}
std::optional<std::uint64_t> PreparedRequest::max_output_tokens() const noexcept {
  return state_ ? state_->prepared_max_output_tokens : std::nullopt;
}
const Limits* PreparedRequest::limits() const noexcept {
  return state_ && state_->client ? &state_->client->options.limits : nullptr;
}
const NativeContext* PreparedRequest::native_context() const noexcept {
  return state_ ? state_->native_context.get() : nullptr;
}
std::optional<std::uint64_t> PreparedRequest::model_invocation_limit() const noexcept {
  return state_ ? state_->prepared_model_invocation_limit : std::nullopt;
}
Operation Client::start(Request request, RunOptions options, Callbacks callbacks) {
  return start(prepare(std::move(request), std::move(options)), std::move(callbacks));
}
Operation Client::start(PreparedRequest preparation, Callbacks callbacks) {
  require_interface_contract(EXPECTED_INTERFACE_REVISION, capability::RequiredProvider);
  auto client = state_;
  auto& operation = preparation.state_;
  if (!client || !operation || operation->client != client || !operation->prepared)
    throw AdmissionError(ErrorKind::Misuse);
  {
    std::lock_guard lock(client->mutex);
    if (client->stopping) throw AdmissionError(ErrorKind::Cancelled);
    client->active.emplace(operation.get(), operation);
    --client->prepared_operations;
    operation->prepared = false;
    operation->admitted = true;
  }
  struct Publication {
    detail::ClientState& owner;
    detail::OperationState& operation;
    bool published = false;
    ~Publication() {
      if (published) return;
      {
        std::lock_guard lock(operation.mutex);
        operation.ready = false;
      }
      operation.stop_callback.reset();
      owner.release(&operation);
    }
  } publication{*client, *operation};
  operation->callbacks = std::move(callbacks);
  try { operation->activate(); }
  catch (...) { throw AdmissionError(ErrorKind::ResourceLimit); }
  publication.published = true;
  return Operation(std::move(operation));
}
PreparedRequest Client::prepare(Request request, RunOptions options) {
  require_interface_contract(EXPECTED_INTERFACE_REVISION, capability::RequiredProvider);
  auto client = state_;
  if (!client) throw AdmissionError(ErrorKind::Misuse);
  if (!options.retry) options.retry = default_retry_policy(*client->options.policy);
  const auto now = client->executor->now();
  const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyTime::max() - now);
  const auto deadline = options.deadline.value_or(client->options.default_timeout >= room
      ? SteadyTime::max() - SteadyTime::duration{1} : now + client->options.default_timeout);
  PreparedRequest preparation(std::make_shared<detail::OperationState>(client, std::move(options), Callbacks{}, deadline));
  auto& operation = preparation.state_;
  std::optional<ErrorKind> admission;
  {
    std::lock_guard lock(client->mutex);
    if (client->stopping) admission = ErrorKind::Cancelled;
    else if (client->active.size() >= client->options.limits.max_operations ||
             client->prepared_operations >= client->options.limits.max_operations - client->active.size())
      admission = ErrorKind::ResourceLimit;
    else {
      ++client->prepared_operations;
      operation->prepared = true;
    }
  }
  if (admission) throw AdmissionError(*admission);
  std::optional<Error> error;
  const auto& retry_ceiling = client->options.policy->admission();
  const auto& stall_ceiling = retry_ceiling.default_timeout_ms;
  if (!detail::valid_retry_policy(*operation->options.retry) || deadline == SteadyTime::max() ||
      (operation->options.connect_timeout && !detail::valid_stall_bound(*operation->options.connect_timeout, stall_ceiling)) ||
      (operation->options.first_byte_timeout && !detail::valid_stall_bound(*operation->options.first_byte_timeout, stall_ceiling)) ||
      (operation->options.idle_timeout && !detail::valid_stall_bound(*operation->options.idle_timeout, stall_ceiling)) ||
      operation->options.retry->max_attempts > retry_ceiling.retry_max_attempts ||
      static_cast<std::uint64_t>(operation->options.retry->base_delay.count()) > retry_ceiling.retry_base_delay_ms ||
      static_cast<std::uint64_t>(operation->options.retry->max_delay.count()) > retry_ceiling.retry_max_delay_ms)
    error = detail::error_for(ErrorKind::InvalidRequest);
  else if (operation->options.stop_token.stop_requested()) error = detail::error_for(ErrorKind::Cancelled);
  else if (client->executor->now() >= deadline) error = detail::error_for(ErrorKind::DeadlineExceeded);
  try {
    if (!error) std::visit([&](const auto& typed) {
      using T = std::decay_t<decltype(typed)>;
      operation->prepared_model = typed.model;
      operation->family = std::is_same_v<T, chat::Request> ? detail::OperationState::Family::Chat :
          std::is_same_v<T, messages::Request> ? detail::OperationState::Family::Messages :
          std::is_same_v<T, responses::Request> ? detail::OperationState::Family::Responses :
          std::is_same_v<T, gemini::Request> ? detail::OperationState::Family::Gemini :
          detail::OperationState::Family::Interactions;
      auto encoded = [&] {
        if constexpr (std::is_same_v<T, chat::Request>) return chat::encode(client->descriptor, typed, operation->options.streaming);
        else if constexpr (std::is_same_v<T, messages::Request>) return messages::encode(client->descriptor, typed, operation->options.streaming);
        else if constexpr (std::is_same_v<T, responses::Request>) return responses::encode(client->descriptor, typed, operation->options.streaming);
        else if constexpr (std::is_same_v<T, gemini::Request>) return gemini::encode(client->descriptor, typed, operation->options.streaming);
        else return interactions::encode(client->descriptor, typed, operation->options.streaming);
      }();
      if (auto* failure = std::get_if<Error>(&encoded)) { error = std::move(*failure); return; }
      auto& value = std::get<0>(encoded);
      auto& wire = operation->request;
      wire.method = std::move(value.method);
      wire.url = std::string(client->descriptor.base_url()) + value.path;
      wire.body = std::move(value.body);
      operation->prepared_max_output_tokens = value.max_output_tokens;
      operation->prepared_model_invocation_limit = value.model_invocation_limit;
      wire.headers.reserve(value.headers.size() + (client->options.api_key.empty() ? 0 : 1));
      for (auto& header : value.headers) wire.headers.push_back({std::move(header.first), std::move(header.second)});
      if (!client->options.api_key.empty()) {
        if (operation->family == detail::OperationState::Family::Messages)
          wire.headers.push_back({"x-api-key", client->options.api_key});
        else if (operation->family == detail::OperationState::Family::Gemini ||
                 operation->family == detail::OperationState::Family::Interactions)
          wire.headers.push_back({"x-goog-api-key", client->options.api_key});
        else wire.headers.push_back({"Authorization", "Bearer " + client->options.api_key});
      }
      wire.deadline = deadline;
      // Resolved here, once, so every attempt (and any backend) sees the same effective bounds:
      // the per-run override, else the client-wide default. Zero means disabled.
      wire.connect_timeout = operation->options.connect_timeout.value_or(client->options.transport.connect_timeout);
      wire.first_byte_timeout = operation->options.first_byte_timeout.value_or(client->options.transport.first_byte_timeout);
      wire.idle_timeout = operation->options.idle_timeout.value_or(client->options.transport.idle_timeout);
      wire.http_version = client->options.http_version;
      wire.ca_file = client->options.ca_file;
      operation->native_context = std::move(value.context);
    }, request);
  } catch (...) { error = detail::error_for(ErrorKind::ResourceLimit); }
  if (error) {
    error->safe_message = detail::safe_message(error->kind);
    error->retry_safety = RetrySafety::NotSent;
    operation->initial_error = std::move(error);
  }
  return preparation;
}
Result Client::complete(Request request, RunOptions options) {
  if (!state_ || state_->in_thread()) return detail::misuse_result();
  try {
    auto operation = start(std::move(request), std::move(options));
    return operation.join();
  } catch (const AdmissionError& error) { return error.outcome(); }
}
Result Client::complete(PreparedRequest preparation) {
  if (!state_ || state_->in_thread()) return detail::misuse_result();
  try {
    auto operation = start(std::move(preparation));
    return operation.join();
  } catch (const AdmissionError& error) { return error.outcome(); }
}
Diagnostics Client::diagnostics() const noexcept {
  if (!state_) return {};
  return {state_->slow_callbacks.load(), state_->callback_exceptions.load()};
}
}  // namespace sp::runtime
