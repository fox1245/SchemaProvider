// The runtime on a host that has no libcurl and may have no threads (a WebAssembly host): operating-system
// entropy, the built-in runtime policy and its size clamp, the refusal of the default worker pool when the
// build has no threads, a chat call carried by a host-pumped executor and an in-process transport, buffered and
// streamed, and the typed refusal of the native archive where there is no file custody.
//
// The test is portable C++ with no peer, file or socket. It runs natively as part of the suite and, built by
// Emscripten with SP_THREADS=OFF, under Node: `node sp_host_runtime_tests.js`.
#include "core/native_archive.h"
#include "core/native_archive_fs.h"
#include "crypto/crypto.h"
#include "descriptor/catalogue.h"
#include "descriptor/presets.h"
#include "runtime/testing.h"
#include "sp/build_config.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace sp;
using namespace sp::runtime;
using namespace std::chrono_literals;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #c); } while (false)

// A single-thread executor: work queues up and the host calls drain() from its own loop. Nothing runs inline.
class HostExecutor final : public detail::Executor {
 public:
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
  void shutdown() noexcept override {}
  // Runs queued work and due timers until none is left; false if it would not settle.
  bool drain() {
    for (int turns = 0; turns < 10000; ++turns) {
      for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->second.at <= now()) { ready_.push_back(std::move(it->second.task)); it = timers_.erase(it); }
        else ++it;
      }
      if (ready_.empty()) return true;
      auto task = std::move(ready_.front());
      ready_.pop_front();
      ++running_;
      try { task(); } catch (...) { --running_; throw; }
      --running_;
    }
    return false;
  }
 private:
  struct Timed { SteadyTime at; Task task; };
  std::deque<Task> ready_;
  std::map<Timer, Timed> timers_;
  Timer last_timer_ = 0;
  unsigned running_ = 0;
};

// An in-process transport: each request is answered by a task on the host executor, as a host's fetch would.
class FakeTransport final : public detail::AttemptTransport {
  struct Call {
    transport::Callbacks callbacks;
    bool cancelled = false;
  };
  struct Handle final : detail::Attempt {
    std::shared_ptr<Call> call;
    explicit Handle(std::shared_ptr<Call> c) : call(std::move(c)) {}
    void cancel() noexcept override { call->cancelled = true; }
    void resume() noexcept override {}
  };
 public:
  FakeTransport(std::shared_ptr<HostExecutor> executor, std::string content_type, std::vector<std::string> chunks)
      : executor_(std::move(executor)), content_type_(std::move(content_type)), chunks_(std::move(chunks)) {}
  std::unique_ptr<detail::Attempt> start(transport::HttpRequest request, transport::Callbacks callbacks) override {
    last_url = request.url;
    ++requests;
    auto call = std::make_shared<Call>();
    call->callbacks = std::move(callbacks);
    executor_->post([call, this] {
      if (call->cancelled) return;
      transport::ResponseHead head;
      head.status = 200;
      head.version = transport::ResponseVersion::Http1_1;
      head.framing = transport::BodyFraming::ContentLength;
      head.headers.push_back({"content-type", content_type_});
      transport::Result result;
      result.attempt.reached = transport::Stage::ResponseStarted;
      result.attempt.response_head_seen = true;
      result.http_status = 200;
      call->callbacks.on_head(head);
      for (const auto& chunk : chunks_) call->callbacks.on_body(chunk);
      result.status = transport::Status::Completed;
      result.failure = transport::FailureKind::None;
      call->callbacks.on_done(result);
    });
    return std::make_unique<Handle>(call);
  }
  void shutdown() noexcept override {}
  std::string last_url;
  int requests = 0;
 private:
  std::shared_ptr<HostExecutor> executor_;
  std::string content_type_;
  std::vector<std::string> chunks_;
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
#if !SP_BUILD_THREADS
  auto executor = std::make_shared<HostExecutor>();
  auto wire = std::make_shared<FakeTransport>(executor, "application/json", std::vector<std::string>{buffered_body});
  bool refused = false;
  try {
    auto client = detail::ClientAccess::make(descriptor_value(), Options::with_api_key("sk-test"), nullptr, wire);
  } catch (const descriptor::ConfigError& error) {
    refused = error.pointer == "/executor";
  }
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

void chat_call(bool streaming) {
  auto executor = std::make_shared<HostExecutor>();
  auto wire = streaming
      ? std::make_shared<FakeTransport>(executor, "text/event-stream", sse_body)
      : std::make_shared<FakeTransport>(executor, "application/json", std::vector<std::string>{buffered_body});
  auto client = detail::ClientAccess::make(descriptor_value(), Options::with_api_key("sk-not-a-real-key"), executor, wire);
  const auto host_thread = std::this_thread::get_id();
  Result outcome;
  int events = 0;
  bool all_on_host_thread = true;
  Callbacks callbacks;
  callbacks.on_event = [&](const Event&) { ++events; all_on_host_thread = all_on_host_thread && std::this_thread::get_id() == host_thread; };
  callbacks.on_outcome = [&](Result result) { outcome = std::move(result); all_on_host_thread = all_on_host_thread && std::this_thread::get_id() == host_thread; };
  auto operation = client.start(request_value(), run_options(streaming, *executor), callbacks);
  CHECK(!outcome);  // nothing ran inline: the host has not pumped yet
  CHECK(executor->drain());
  CHECK(outcome);
  CHECK(is_completion(*outcome));
  CHECK(joined_text(*outcome) == "hello from the host");
  CHECK(wire->requests == 1 && wire->last_url == "https://api.example.test/v1/chat/completions");
  CHECK(all_on_host_thread);
  if (streaming) CHECK(events > 0);
  operation.detach();
  std::printf("chat %s ok\n", streaming ? "streamed" : "buffered");
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
    chat_call(false);
    chat_call(true);
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
