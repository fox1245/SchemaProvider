// The runtime on a host that has no libcurl and may have no threads (a WebAssembly host): operating-system
// entropy, the built-in runtime policy and its size clamp, the refusal of the default worker pool when the
// build has no threads, the public host seam (an executor and an HTTP transport of the test's own, handed to
// `Client(descriptor, options, executor, transport)`), a chat call through it, buffered and streamed, with the body
// cut at every byte so that the cuts fall inside events and inside UTF-8 sequences, the refusal of blocking calls
// on a host-pumped executor, and the typed refusal of the native archive where there is no file custody.
//
// The test is portable C++ with no peer, file or socket. It runs natively as part of the suite and, built by
// Emscripten with SP_THREADS=OFF, under Node: `node sp_host_runtime_tests.js`.
#include "core/native_archive.h"
#include "core/native_archive_fs.h"
#include "crypto/crypto.h"
#include "descriptor/catalogue.h"
#include "descriptor/presets.h"
#include "runtime/client.h"
#include "runtime/host.h"
#include "runtime/testing.h"
#include "sp/build_config.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace sp;
using namespace sp::runtime;
using namespace std::chrono_literals;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #c); } while (false)

// A single-thread executor: work queues up and the host calls drain() from its own loop. Nothing runs inline.
// `pumped` is what it declares about itself (Executor::host_pumped).
class HostExecutor final : public Executor {
 public:
  explicit HostExecutor(bool pumped = true) : pumped_(pumped) {}
  void post(Task task) override { ready_.push_back(std::move(task)); }
  Timer schedule(SteadyTime at, Task task) override {
    const auto id = ++last_timer_;
    timers_.emplace(id, Timed{at, std::move(task)});
    return id;
  }
  void cancel(Timer id) noexcept override { timers_.erase(id); }
  SteadyTime now() const noexcept override { return std::chrono::steady_clock::now(); }
  WallTime wall_now() const noexcept override { return std::chrono::system_clock::now(); }
  bool in_thread() const noexcept override { return running_ != 0; }
  bool host_pumped() const noexcept override { return pumped_; }
  void shutdown() noexcept override { ++shutdowns; }
  // Runs queued work and due timers until none is left; false if it would not settle.
  bool drain() {
    for (int turns = 0; turns < 100000; ++turns) {
      for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->second.at <= now()) { ready_.push_back(std::move(it->second.task)); it = timers_.erase(it); }
        else ++it;
      }
      if (ready_.empty()) return true;
      auto task = std::move(ready_.front());
      ready_.pop_front();
      ++running_;
      ++tasks_run;
      try { task(); } catch (...) { --running_; throw; }
      --running_;
    }
    return false;
  }
  std::size_t pending() const { return ready_.size(); }
  int shutdowns = 0;
  std::size_t tasks_run = 0;
 private:
  struct Timed { SteadyTime at; Task task; };
  std::deque<Task> ready_;
  std::map<Timer, Timed> timers_;
  Timer last_timer_ = 0;
  unsigned running_ = 0;
  bool pumped_;
};

// An in-process transport: each request is answered by a task on the host executor, as a host's fetch would, in the
// chunks it was given. A chunk `on_body` does not consume is kept and offered again after resume(), as the transport
// contract says. `hold` keeps the answer back until release(). A cancelled attempt ends with one Cancelled completion
// from a later task, like any transport's.
class FakeTransport final : public HostTransport {
  struct Call {
    transport::Callbacks callbacks;
    std::vector<std::string> chunks;
    std::size_t next = 0;
    bool cancelled = false, finished = false, scheduled = false, paused = false, head_sent = false;
  };
  struct Handle final : HostAttempt {
    FakeTransport* owner;
    std::shared_ptr<Call> call;
    Handle(FakeTransport* o, std::shared_ptr<Call> c) : owner(o), call(std::move(c)) {}
    void cancel() noexcept override {
      ++owner->cancels;
      call->cancelled = true;
      owner->executor_->post([call = call] { FakeTransport::end(*call, transport::Status::Cancelled); });
    }
    void resume() noexcept override {
      owner->executor_->post([call = call, owner = owner] { if (call->paused) { call->paused = false; owner->pump(*call); } });
    }
  };
  static void end(Call& call, transport::Status status) {
    if (call.finished) return;
    call.finished = true;
    transport::Result result;
    result.status = status;
    result.attempt.reached = transport::Stage::RequestStarted;
    if (status == transport::Status::Completed) {
      result.attempt.reached = transport::Stage::ResponseStarted;
      result.attempt.response_head_seen = true;
      result.http_status = 200;
    }
    call.callbacks.on_done(result);
  }
 public:
  FakeTransport(std::shared_ptr<HostExecutor> executor, std::string content_type, std::vector<std::string> chunks)
      : executor_(std::move(executor)), content_type_(std::move(content_type)), chunks_(std::move(chunks)) {}
  std::unique_ptr<HostAttempt> start(transport::HttpRequest request, transport::Callbacks callbacks) override {
    last_url = request.url;
    ++requests;
    auto call = std::make_shared<Call>();
    call->callbacks = std::move(callbacks);
    call->chunks = chunks_;
    calls_.push_back(call);
    if (!hold) schedule(call);
    return std::make_unique<Handle>(this, call);
  }
  void shutdown() noexcept override { ++shutdowns; }
  void release() {
    hold = false;
    for (const auto& call : calls_) schedule(call);
  }
  void set_chunks(std::vector<std::string> chunks) { chunks_ = std::move(chunks); }
  bool hold = false;
  std::string last_url;
  int requests = 0, shutdowns = 0, cancels = 0;
 private:
  void schedule(const std::shared_ptr<Call>& call) {
    if (call->scheduled) return;
    call->scheduled = true;
    executor_->post([call, this] {
      if (call->cancelled || call->finished) return;
      pump(*call);
    });
  }
  void pump(Call& call) {
    if (call.cancelled || call.finished) return;
    if (!call.head_sent) {
      call.head_sent = true;
      transport::ResponseHead head;
      head.status = 200;
      head.version = transport::ResponseVersion::Http1_1;
      head.framing = transport::BodyFraming::ContentLength;
      head.headers.push_back({"content-type", content_type_});
      call.callbacks.on_head(head);
    }
    while (call.next < call.chunks.size()) {
      if (!call.callbacks.on_body(call.chunks[call.next])) {
        call.paused = true;
        return;
      }
      ++call.next;
    }
    end(call, transport::Status::Completed);
  }
  std::shared_ptr<HostExecutor> executor_;
  std::string content_type_;
  std::vector<std::string> chunks_;
  std::vector<std::shared_ptr<Call>> calls_;
};

descriptor::ValidatedDescriptor descriptor_value() {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"host-runtime","family":"openai.chat","connection":{"base_url":"https://api.example.test","paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
chat::Request request_value() {
  chat::Request request;
  request.model = "fixture-model";
  request.messages.push_back({Role::User, "hello"});
  return request;
}
RunOptions run_options(bool streaming, const HostExecutor& executor) {
  RunOptions run;
  run.streaming = streaming;
  run.retry = RetryPolicy{false, false, 1, 0ms, 0ms};
  run.deadline = executor.now() + 30s;
  return run;
}
const char* const buffered_body =
    R"({"id":"c1","model":"fixture-model","created":7,"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"hello from the host"},"finish_reason":"stop"}],"usage":{"prompt_tokens":2,"completion_tokens":4,"total_tokens":6}})";
const std::vector<std::string> sse_body = {
    "data: {\"id\":\"s\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"fixture-model\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"hello \"},\"finish_reason\":null}]}\n\n",
    "data: {\"id\":\"s\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"fixture-model\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"from the host\"},\"finish_reason\":null}]}\n\n",
    "data: {\"id\":\"s\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"fixture-model\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":4,\"total_tokens\":6}}\n\n",
    "data: [DONE]\n\n"};

void build_configuration() {
  // sp/build_config.h stops a translation unit whose thread setting differs from the library's; this records the mode.
  std::printf("build: threads=%d size_t=%zu bytes\n", SP_BUILD_THREADS, sizeof(std::size_t));
}

void entropy() {
  CHECK(crypto::random_bytes(nullptr, 0));
  // getentropy takes at most 256 bytes a call: sizes around that bound, and a large one, must be filled whole.
  for (const std::size_t size : {std::size_t{1}, std::size_t{255}, std::size_t{256}, std::size_t{257}, std::size_t{512}, std::size_t{513}, std::size_t{4096}}) {
    std::vector<unsigned char> first(size + 2, 0xA5), second(size + 2, 0xA5);
    CHECK(crypto::random_bytes(first.data() + 1, size));
    CHECK(crypto::random_bytes(second.data() + 1, size));
    // The guard bytes are untouched, so it neither under- nor overruns.
    CHECK(first.front() == 0xA5 && first.back() == 0xA5 && second.front() == 0xA5 && second.back() == 0xA5);
    if (size >= 16) {
      CHECK(first != second);
      // The last chunk of a multi-chunk request is filled too, not left as the sentinel fill.
      CHECK(std::any_of(first.end() - 9, first.end() - 1, [](unsigned char b) { return b != 0xA5; }));
    }
  }
  std::puts("entropy ok");
}

void policy_clamp() {
  const auto policy = configuration::builtin_runtime_policy();
  CHECK(policy);
  const auto& admission = policy->admission();
  constexpr auto size_max = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
  for (const auto ceiling : {admission.max_operations, admission.queued_body_bytes, admission.max_response_bytes,
                             admission.semantic_max_content_bytes, admission.sse_max_total_bytes, admission.default_timeout_ms})
    CHECK(ceiling <= size_max);
  if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
    // The shipped ceilings (INT64_MAX) do not fit: they are admitted as the largest size and the host can see which.
    const auto& clamps = policy->clamps();
    CHECK(!clamps.empty());
    for (const auto& clamp : clamps) {
      CHECK(clamp.pointer.rfind("/admission/", 0) == 0);
      CHECK(clamp.admitted == size_max && clamp.configured > size_max);
    }
    CHECK(admission.max_response_bytes == size_max);
    std::printf("policy ok (%zu ceilings clamped to %llu)\n", clamps.size(), static_cast<unsigned long long>(size_max));
  } else {
    CHECK(policy->clamps().empty());
    std::puts("policy ok (no ceiling clamped)");
  }
}

void catalogue_error_is_returned() {
  // The loader catches its own ConfigError and returns it. Without -fexceptions on WebAssembly that catch is inert
  // and the error escapes as an exception.
  auto loaded = descriptor::load_catalogue(R"({"models": 7})");
  CHECK(std::holds_alternative<descriptor::ConfigError>(loaded));
  std::puts("exceptions ok");
}

void default_pool_refuses_without_threads() {
  auto executor = std::make_shared<HostExecutor>();
  auto wire = std::make_shared<FakeTransport>(executor, "application/json", std::vector<std::string>{buffered_body});
  // The public constructor builds nothing without a transport, and without an executor in a build with no pool.
  bool refused = false;
  try { Client client(descriptor_value(), Options::with_api_key("sk-test"), executor, nullptr); }
  catch (const descriptor::ConfigError& error) { refused = error.pointer == "/transport"; }
  CHECK(refused);
#if !SP_BUILD_THREADS
  refused = false;
  try { Client client(descriptor_value(), Options::with_api_key("sk-test"), nullptr, wire); }
  catch (const descriptor::ConfigError& error) { refused = error.pointer == "/executor"; }
  CHECK(refused);
  refused = false;
  try { (void)detail::make_pool_executor(2); }
  catch (const descriptor::ConfigError& error) { refused = error.pointer == "/executor"; }
  CHECK(refused);
  std::puts("default pool refused without threads");
#else
  std::puts("default pool: threads are on, not applicable");
#endif
}

// libcurl keeps idle connections only up to the size of its cache, and a burst that drains closes the surplus. The
// client sizes the cache for the operations it may run at once, so that the next burst finds its connections.
std::size_t g_built_cache_size = 0;
std::shared_ptr<detail::AttemptTransport> capture_cache_size(transport::TransportOptions options) {
  g_built_cache_size = options.connection_cache_size;
  return std::make_shared<FakeTransport>(std::make_shared<HostExecutor>(), "application/json", std::vector<std::string>{});
}
void connection_cache_follows_operations() {
  const auto build = [](Options options) {
    g_built_cache_size = 0;
    detail::ClientAccess::make(descriptor_value(), std::move(options), std::make_shared<HostExecutor>(), nullptr, {},
                               &capture_cache_size);
    return g_built_cache_size;
  };
  auto options = Options::with_api_key("sk-test");
  CHECK(build(options) == options.limits.max_operations);        // derived: the operations it may run at once
  options.limits.max_operations = 100;
  CHECK(build(options) == 100);
  options.transport.connection_cache_size = 8;                    // explicit: honoured, even below the concurrency
  CHECK(build(options) == 8);
  // A size above the admitted ceiling on operations is refused, as the limit it derives from is.
  const std::uint64_t ceiling = options.policy->admission().max_operations;
  if (ceiling < std::numeric_limits<std::size_t>::max()) {
    options.transport.connection_cache_size = static_cast<std::size_t>(ceiling) + 1;
    std::string refused_at;
    try { build(options); }
    catch (const descriptor::ConfigError& error) { refused_at = error.pointer; }
    CHECK(refused_at == "/connection_cache_size");
  }
  std::puts("connection cache follows the operation limit");
}

// The seam is public, and the names the private header used for it are the same types.
void seam_is_public() {
  static_assert(std::is_same_v<detail::Executor, runtime::Executor>);
  static_assert(std::is_same_v<detail::Attempt, runtime::HostAttempt>);
  static_assert(std::is_same_v<detail::AttemptTransport, runtime::HostTransport>);
  // An executor that says nothing about itself is not host-pumped, and a transport without a byte stream has none.
  struct Plain final : Executor {
    void post(Task) override {}
    Timer schedule(SteadyTime, Task) override { return 0; }
    void cancel(Timer) noexcept override {}
    SteadyTime now() const noexcept override { return {}; }
    WallTime wall_now() const noexcept override { return {}; }
    bool in_thread() const noexcept override { return false; }
    void shutdown() noexcept override {}
  };
  CHECK(!Plain().host_pumped() && HostExecutor().host_pumped() && !HostExecutor(false).host_pumped());
  auto executor = std::make_shared<HostExecutor>();
  FakeTransport wire(executor, "application/json", {});
  CHECK(wire.open_stream(transport::WsRequest{}, transport::WsCallbacks{}) == nullptr);
  require_interface_contract(EXPECTED_INTERFACE_REVISION, capability::HostSeam);
  std::puts("seam is public");
}

// A chat call through the public constructor; `legacy` builds the Client through the private test access instead.
void chat_call(bool streaming, bool legacy = false) {
  auto executor = std::make_shared<HostExecutor>();
  auto wire = streaming
      ? std::make_shared<FakeTransport>(executor, "text/event-stream", sse_body)
      : std::make_shared<FakeTransport>(executor, "application/json", std::vector<std::string>{buffered_body});
  std::optional<Client> client;
  if (legacy) client.emplace(detail::ClientAccess::make(descriptor_value(), Options::with_api_key("sk-not-a-real-key"), executor, wire));
  else client.emplace(descriptor_value(), Options::with_api_key("sk-not-a-real-key"), executor, wire);
  const auto host_thread = std::this_thread::get_id();
  Result outcome;
  int events = 0;
  bool all_on_host_thread = true;
  Callbacks callbacks;
  callbacks.on_event = [&](const Event&) { ++events; all_on_host_thread = all_on_host_thread && std::this_thread::get_id() == host_thread; };
  callbacks.on_outcome = [&](Result result) { outcome = std::move(result); all_on_host_thread = all_on_host_thread && std::this_thread::get_id() == host_thread; };
  auto operation = client->start(request_value(), run_options(streaming, *executor), callbacks);
  CHECK(!outcome);  // nothing ran inline: the host has not pumped yet
  CHECK(executor->drain());
  CHECK(outcome);
  CHECK(is_completion(*outcome));
  CHECK(joined_text(*outcome) == "hello from the host");
  CHECK(wire->requests == 1 && wire->last_url == "https://api.example.test/v1/chat/completions");
  CHECK(all_on_host_thread);
  if (streaming) CHECK(events > 0);
  operation.detach();
  // With nothing in flight the destructor closes the transport and the executor at once.
  client.reset();
  CHECK(wire->shutdowns == 1 && executor->shutdowns == 1);
  std::printf("chat %s ok%s\n", streaming ? "streamed" : "buffered", legacy ? " (private test access)" : "");
}

// SSE with non-ASCII text in either line-ending style: three-byte (Hangul) and four-byte (emoji) UTF-8 sequences.
std::string sse_utf8(const char* newline) {
  const std::string nl = newline;
  const auto event = [&](const std::string& delta, const std::string& finish) {
    return std::string("data: {\"id\":\"s\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"fixture-model\",\"choices\":[{\"index\":0,\"delta\":") +
           delta + ",\"finish_reason\":" + finish + "}]" +
           (finish == "null" ? "" : ",\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":4,\"total_tokens\":6}") + "}" + nl + nl;
  };
  return event("{\"role\":\"assistant\",\"content\":\"\xEC\x95\x88\xEB\x85\x95 \"}", "null") +
         event("{\"content\":\"\xF0\x9F\x98\x80 ok\"}", "null") + event("{}", "\"stop\"") + "data: [DONE]" + nl + nl;
}

// The body cut at every byte offset into two pieces, then into pieces of one, three and five bytes: the framing and
// the UTF-8 decoding must not depend on where the host's stream was cut. One byte at a time also fills the
// runtime's queue, so the transport is paused and resumed on the way.
void body_cut_anywhere(bool streaming, const std::string& body, const std::string& expected, const char* name) {
  auto executor = std::make_shared<HostExecutor>();
  auto wire = std::make_shared<FakeTransport>(executor, streaming ? "text/event-stream" : "application/json", std::vector<std::string>{});
  Client client(descriptor_value(), Options::with_api_key("sk-not-a-real-key"), executor, wire);
  std::size_t runs = 0;
  const auto run = [&](std::vector<std::string> chunks) {
    wire->set_chunks(std::move(chunks));
    Result outcome;
    Callbacks callbacks;
    callbacks.on_outcome = [&](Result result) { outcome = std::move(result); };
    auto operation = client.start(request_value(), run_options(streaming, *executor), callbacks);
    CHECK(executor->drain());
    CHECK(outcome && is_completion(*outcome) && joined_text(*outcome) == expected);
    operation.detach();
    ++runs;
  };
  for (std::size_t cut = 1; cut < body.size(); ++cut) run({body.substr(0, cut), body.substr(cut)});
  for (const std::size_t size : {std::size_t{1}, std::size_t{3}, std::size_t{5}}) {
    std::vector<std::string> pieces;
    for (std::size_t at = 0; at < body.size(); at += size) pieces.push_back(body.substr(at, size));
    run(std::move(pieces));
  }
  std::printf("%s cut anywhere ok (%zu runs)\n", name, runs);
}

void cut_bodies() {
  const std::string expected = "\xEC\x95\x88\xEB\x85\x95 \xF0\x9F\x98\x80 ok";
  body_cut_anywhere(true, sse_utf8("\n"), expected, "streamed (LF)");
  body_cut_anywhere(true, sse_utf8("\r\n"), expected, "streamed (CRLF)");
  // The same text in a buffered answer, the JSON cut inside the multi-byte sequences too.
  const std::string buffered = std::string(R"({"id":"c1","model":"fixture-model","created":7,"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":")") +
      expected + R"("},"finish_reason":"stop"}],"usage":{"prompt_tokens":2,"completion_tokens":4,"total_tokens":6}})";
  body_cut_anywhere(false, buffered, expected, "buffered");
}

const Failure& failure_of(const Result& result) {
  CHECK(result && std::holds_alternative<Failure>(*result));
  return std::get<Failure>(*result);
}
void expect_refusal(const Error& error, std::string_view reason) {
  CHECK(error.kind == ErrorKind::Misuse && error.reason == reason && error.retry_safety == RetrySafety::NotSent);
}

// A blocking call on a host-pumped executor is refused at once, with a typed error: nothing runs, nothing is sent,
// and the operation it concerned goes on. Called from a task of an executor that is not host-pumped, it is refused
// too, with the other reason.
void blocking_calls_are_refused() {
  {
    auto executor = std::make_shared<HostExecutor>();
    auto wire = std::make_shared<FakeTransport>(executor, "application/json", std::vector<std::string>{buffered_body});
    wire->hold = true;
    Client client(descriptor_value(), Options::with_api_key("sk-not-a-real-key"), executor, wire);
    Result outcome;
    Callbacks callbacks;
    callbacks.on_outcome = [&](Result result) { outcome = std::move(result); };
    const auto run = run_options(false, *executor);
    auto operation = client.start(request_value(), run, callbacks);
    const auto turns = executor->tasks_run;
    expect_refusal(failure_of(operation.join()).error, blocking_host_pumped_reason);
    expect_refusal(failure_of(client.complete(request_value(), run)).error, blocking_host_pumped_reason);
    expect_refusal(failure_of(client.complete(client.prepare(request_value(), run))).error, blocking_host_pumped_reason);
    // A typed endpoint call and a prepared one go through the same check.
    images::Request image;
    image.model = "fixture-image-model";
    image.prompt = "a lighthouse";
    const auto drawn = client.complete(image, run);
    CHECK(std::holds_alternative<Failure>(drawn));
    expect_refusal(std::get<Failure>(drawn).error, blocking_host_pumped_reason);
    CHECK(std::holds_alternative<Failure>(client.complete(client.prepare(image, run))));
    // Nothing waited, nothing ran, nothing was sent.
    CHECK(executor->tasks_run == turns && wire->requests == 0 && !outcome);
    // The operation was not harmed: the host pumps and it completes.
    wire->release();
    CHECK(executor->drain());
    CHECK(outcome && is_completion(*outcome) && joined_text(*outcome) == "hello from the host");
    operation.detach();
  }
  {
    auto executor = std::make_shared<HostExecutor>(false);
    auto wire = std::make_shared<FakeTransport>(executor, "text/event-stream", sse_body);
    Client client(descriptor_value(), Options::with_api_key("sk-not-a-real-key"), executor, wire);
    Operation operation;
    Result joined, completed, outcome;
    Callbacks callbacks;
    callbacks.on_event = [&](const Event&) {
      if (completed) return;
      joined = operation.join();
      completed = client.complete(request_value(), run_options(true, *executor));
    };
    callbacks.on_outcome = [&](Result result) { outcome = std::move(result); };
    operation = client.start(request_value(), run_options(true, *executor), callbacks);
    CHECK(executor->drain());
    expect_refusal(failure_of(joined).error, blocking_in_thread_reason);
    expect_refusal(failure_of(completed).error, blocking_in_thread_reason);
    CHECK(wire->requests == 1 && outcome && is_completion(*outcome));
    operation.detach();
  }
  std::puts("blocking calls refused");
}

// A destructor that waited for the operations it cancelled would wait forever on a host-pumped executor. It returns
// at once; the operations end when the host pumps next, and the last one closes the transport and the executor.
void client_destruction_does_not_wait() {
  for (const bool dispatched : {false, true}) {
    auto executor = std::make_shared<HostExecutor>();
    auto wire = std::make_shared<FakeTransport>(executor, "application/json", std::vector<std::string>{buffered_body});
    wire->hold = true;
    std::optional<Client> client;
    client.emplace(descriptor_value(), Options::with_api_key("sk-not-a-real-key"), executor, wire);
    Result outcome;
    Callbacks callbacks;
    callbacks.on_outcome = [&](Result result) { outcome = std::move(result); };
    auto operation = client->start(request_value(), run_options(false, *executor), callbacks);
    if (dispatched) CHECK(executor->drain() && wire->requests == 1);
    operation.detach();
    client.reset();
    CHECK(!outcome && executor->shutdowns == 0 && wire->shutdowns == 0);
    CHECK(executor->drain());
    CHECK(outcome && failure_of(outcome).error.kind == ErrorKind::Cancelled);
    // The runtime may cancel an attempt more than once, which the contract allows; a call never sent is never cancelled.
    CHECK(wire->requests == (dispatched ? 1 : 0) && (dispatched ? wire->cancels >= 1 : wire->cancels == 0));
    CHECK(executor->shutdowns == 1 && wire->shutdowns == 1);
  }
  std::puts("client destroyed without waiting");
}

void archive_custody() {
  if (archive_fs::available()) {
    std::puts("archive: file custody present, covered by the native archive tests");
    return;
  }
  // No file custody: provision and open refuse with a typed error and say nothing about the path or key file.
  for (const bool create : {true, false}) {
    auto activation = create
        ? NativeArchive::provision("/archive/dir", "/archive/key", "owner", descriptor_value())
        : NativeArchive::open("/archive/dir", "/archive/key", "owner", descriptor_value());
    const auto* error = std::get_if<Error>(&activation);
    CHECK(error);
    CHECK(error->kind == ErrorKind::Unsupported && error->retry_safety == RetrySafety::NotSent);
    CHECK(error->safe_message.find("unavailable") != std::string::npos);
    CHECK(error->safe_message.find("/archive") == std::string::npos);
  }
  std::puts("archive refused (unavailable)");
}
}  // namespace

int main() {
  try {
    build_configuration();
    entropy();
    policy_clamp();
    catalogue_error_is_returned();
    default_pool_refuses_without_threads();
    connection_cache_follows_operations();
    seam_is_public();
    chat_call(false);
    chat_call(true);
    chat_call(true, true);
    cut_bodies();
    blocking_calls_are_refused();
    client_destruction_does_not_wait();
    archive_custody();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "host runtime test failed: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("host runtime test failed: an exception of unknown type escaped\n", stderr);
    return 1;
  }
  std::puts("host runtime: ok");
  return 0;
}
