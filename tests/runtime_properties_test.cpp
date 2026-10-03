// Real, model-free runtime properties. Usage: sp_runtime_tests <node> <runtime_server.mjs>
#include "runtime/client.h"
#include "runtime/testing.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include "support/runtime_peer.h"

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using sp::runtime::Client;
using sp::runtime::Operation;
using sp::runtime::Result;
using runtime_test::Peer;
using runtime_test::write_all;
constexpr std::string_view marker = "RUNTIME_SECRET_MARKER_62c490";
void require(bool yes, std::string_view message) {
  if (!yes) throw std::runtime_error(std::string(message));
}
std::string descriptor_source(std::uint64_t port, bool messages, std::string_view suffix = {}) {
  const auto path = messages ? "/v1/messages" : "/v1/chat/completions";
  return "{\"descriptor_version\":1,\"revision\":1,\"id\":\"runtime-loopback\",\"family\":"
    + sp::json::quote(messages ? "anthropic.messages" : "openai.chat")
    + ",\"evidence\":{\"urls\":[\"https://example.com/spec\"],\"verified_at\":\"2026-10-01\"},"
      "\"connection\":{\"base_url\":" + sp::json::quote("http://127.0.0.1:" + std::to_string(port) + std::string(suffix))
    + ",\"paths\":{\"buffered\":" + sp::json::quote(path) + ",\"streaming\":" + sp::json::quote(path)
    + "},\"headers\":{\"anthropic-version\":\"2023-06-01\"}},"
      "\"bindings\":{\"model\":\"model\",\"messages\":\"messages\",\"stream\":\"stream\",\"max_output_tokens\":\"max_tokens\",\"usage\":[\"usage\"]},"
      "\"stop_reasons\":{\"stop\":\"EndTurn\",\"end_turn\":\"EndTurn\",\"tool_use\":\"ToolUse\"}}";
}
sp::descriptor::ValidatedDescriptor descriptor(const Peer& peer, bool messages) {
  auto loaded = sp::descriptor::load(descriptor_source(peer.port, messages));
  require(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(loaded), "runtime descriptor rejected");
  return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}
sp::runtime::Options options() {
  sp::runtime::Options result;
  result.api_key = marker;
  result.default_timeout = 15s;
  result.retry_tokens_per_second = 0;
  return result;
}
sp::runtime::Request request(const std::string& model, bool messages = false) {
  if (!messages) { sp::chat::Request r; r.model = model; r.messages.push_back({sp::Role::User, "synthetic"}); return r; }
  sp::messages::Request r; r.model = model; r.account_scope = "synthetic-account";
  sp::Message m; m.role = sp::Role::User; m.parts.emplace_back(sp::Text{"synthetic"}); r.messages.push_back(std::move(m)); return r;
}
sp::runtime::RunOptions run(bool streaming = true, bool retry = false, bool risk = false) {
  sp::runtime::RunOptions result; result.streaming = streaming;
  result.retry = sp::runtime::RetryPolicy{retry, risk, 3, 0ms, 0ms}; return result;
}
const sp::Failure& failure(const Result& result, std::optional<sp::ErrorKind> kind = {}) {
  require(result && std::holds_alternative<sp::Failure>(*result), "expected failure outcome");
  const auto& fail = std::get<sp::Failure>(*result);
  if (kind) require(fail.error.kind == *kind, "incorrect failure class");
  require(fail.error.safe_message.find(marker) == std::string::npos && fail.error.vendor_code.find(marker) == std::string::npos,
    "secret leaked through safe error metadata");
  return fail;
}
std::string text(const std::vector<sp::Message>& messages) {
  std::string result;
  for (const auto& m : messages) for (const auto& part : m.parts) if (const auto* p = std::get_if<sp::Text>(&part)) result += p->value;
  return result;
}
void success(const Result& result, std::string_view expected = "hello") {
  require(result && std::holds_alternative<sp::Completion>(*result), "expected successful outcome");
  const auto& c = std::get<sp::Completion>(*result);
  require(c.stop.kind == sp::StopKind::EndTurn && text(c.messages) == expected, "incorrect completed content or stop reason");
}
struct Capture {
  std::mutex mutex;
  std::condition_variable cv;
  Result result;
  std::size_t outcomes = 0, deltas = 0, terminals = 0;
  sp::runtime::Callbacks callbacks() {
    return {[this](const sp::Event& event) {
      std::lock_guard lock(mutex);
      if (std::holds_alternative<sp::PartDelta>(event)) ++deltas;
      if (std::holds_alternative<sp::Commit>(event) || std::holds_alternative<sp::Fail>(event)) ++terminals;
      cv.notify_all();
    }, [this](Result value) { std::lock_guard lock(mutex); result = std::move(value); ++outcomes; cv.notify_all(); }};
  }
  Result await() {
    std::unique_lock lock(mutex);
    require(cv.wait_for(lock, 20s, [&] { return !!result; }), "outcome callback timed out");
    require(outcomes == 1 && terminals == 0, "terminal callback delivered more than once or as semantic event");
    return result;
  }
  void delta() {
    std::unique_lock lock(mutex); require(cv.wait_for(lock, 15s, [&] { return deltas != 0; }), "semantic output timed out");
  }
};
Result finish(Operation& op, Capture& capture) {
  auto result = capture.await();
  (void)op.join();
  return result;
}

void owner_exception_paths(const char* script) {
  auto descriptors = [] {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator{});
  };
  const auto before = descriptors();
  bool rejected = false;
  try { Peer invalid("/", script); } // A directory cannot be exec'd.
  catch (const std::runtime_error&) { rejected = true; }
  require(rejected, "failed peer exec was admitted");
  siginfo_t info{};
  const auto child = ::waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT);
  require(child == -1 && errno == ECHILD, "failed peer constructor retained a child");
  struct Unwind {};
  try {
    runtime_test::LogCapture nested;
    std::cout << marker; // Deliberately buffered; must not escape during unwinding.
    throw Unwind{};
  } catch (const Unwind&) {}
  require(descriptors() == before, "exception unwinding leaked owned descriptors");
}

void quota_abnormal_close(Peer& peer) {
  Client client(descriptor(peer, false), options());
  const auto model = peer.arm("quota-short-close");
  auto result = client.complete(request(model), run(false, true, true));
  const auto report = peer.stats(model);
  const auto count = report.root().get("count").as_uint();
  require(count == 1, "quota body retried after abnormal close: " + std::to_string(count) + " attempts");
  const auto& error = failure(result).error;
  require(error.kind == sp::ErrorKind::Truncated || error.kind == sp::ErrorKind::Transport,
          "abnormal framing evidence was discarded");
  require(error.retry_class == sp::RetryClass::Never && error.vendor_code == "insufficient_quota",
          "quota evidence did not prohibit retry");
  require(error.http_status == 200 && error.attempt.attempts == 1, "quota close observation was lost");
  peer.count(model, 1, 1);
}

void family_modes(Peer& peer) {
  for (bool messages : {false, true}) for (bool streaming : {false, true}) {
    Client client(descriptor(peer, messages), options());
    const auto model = peer.arm("normal"); Capture capture;
    auto op = client.start(request(model, messages), run(streaming), capture.callbacks());
    auto result = finish(op, capture); success(result);
    const auto& usage = std::get<sp::Completion>(*result).usage;
    require(usage.output_total && usage.output_total->value == 3 && usage.stage == sp::UsageStage::Final,
      "final output usage was not retained");
    if (messages) {
      require(usage.input_uncached && usage.input_uncached->value == 2 && !usage.input_total &&
              !usage.cache_read && !usage.cache_write, "missing cache evidence was invented");
    } else require(usage.input_total && usage.input_total->value == 2, "Chat input usage was not retained");
    peer.count(model, 1);
    const auto blocking = peer.arm("normal"); success(client.complete(request(blocking, messages), run(streaming))); peer.count(blocking, 1);
  }
}
void retry_policy(Peer& peer) {
  for (bool messages : {false, true}) for (bool streaming : {false, true}) {
    Client client(descriptor(peer, messages), options());
    for (bool enabled : {false, true}) {
      const auto model = peer.arm("recover");
      auto result = client.complete(request(model, messages), run(streaming, enabled));
      const auto& f = failure(result);
      require(f.error.retry_safety == sp::RetrySafety::PossiblyAccepted && f.error.attempt.attempts == 1,
        "possibly accepted failure retried without billing consent");
      require(f.error.http_status == 503 && f.error.attempt.response_head_seen && f.error.attempt.request_may_have_left,
        "response evidence was lost");
      require(f.partial.raw_events.size() == 1 && f.partial.raw_events[0].payload &&
          f.partial.raw_events[0].payload->root().get("error").get("message").as_string() == marker,
          "non-success HTTP response dropped original owned error JSON");
      peer.count(model, 1, 1);
    }
    const auto recovered = peer.arm("recover");
    const auto recovered_result = client.complete(request(recovered, messages), run(streaming, true, true));
    success(recovered_result); peer.count(recovered, 2, 1);
    const auto& observations = std::get<sp::Completion>(*recovered_result).raw_events;
    require(observations.size() >= 2 && observations[0].payload &&
        observations[0].payload->root().get("error").get("message").as_string() == marker,
        "successful retry discarded the prior actual HTTP error observation");
    const auto quota = peer.arm("quota");
    failure(client.complete(request(quota, messages), run(streaming, true, true)),
      messages ? sp::ErrorKind::LimitUnknown : sp::ErrorKind::QuotaExhausted); peer.count(quota, 1, 1);
    const auto capped = peer.arm("always-error"); auto capped_run = run(streaming, true, true); capped_run.retry->max_attempts = 2;
    const auto capped_result = client.complete(request(capped, messages), capped_run);
    require(failure(capped_result).error.attempt.attempts == 2, "max attempts not applied"); peer.count(capped, 2, 2);
  }
  Client client(descriptor(peer, false), options());
  for (bool risk : {false, true}) {
    const auto model = peer.arm("reset-recover"); auto result = client.complete(request(model), run(true, true, risk));
    if (risk) { success(result); peer.count(model, 2, 1); }
    else { require(failure(result).error.retry_safety == sp::RetrySafety::PossiblyAccepted, "reset safety underestimated"); peer.count(model, 1, 1); }
  }
}
void safe_connect_retry() {
  // Reserve a port without listening: the kernel refuses connections, and no
  // unrelated listener can claim it between attempts.
  runtime_test::Fd guard(socket(AF_INET, SOCK_STREAM, 0));
  const auto fd = guard.get();
  require(fd >= 0, "cannot reserve refused-connect port");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "cannot bind refused-connect port");
  socklen_t size = sizeof(address);
  require(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "cannot inspect refused-connect port");
  for (bool enabled : {false, true}) {
    auto loaded = sp::descriptor::load(descriptor_source(ntohs(address.sin_port), false));
    require(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(loaded), "connect descriptor rejected");
    Client client(std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded)), options());
    auto result = client.complete(request("not-sent"), run(false, enabled));
    const auto& f = failure(result, sp::ErrorKind::Transport);
    require(f.error.retry_safety == sp::RetrySafety::NotSent && !f.error.attempt.request_may_have_left
      && !f.error.attempt.response_head_seen && f.error.attempt.request_body_bytes == 0,
      "connect failure incorrectly marked possibly accepted");
    require(f.error.attempt.attempts == (enabled ? 3U : 1U), "safe retry attempt count incorrect");
  }
}
void partial_ping_and_close(Peer& peer) {
  for (bool messages : {false, true}) {
    Client client(descriptor(peer, messages), options());
    for (bool callback : {false, true}) {
      const auto model = peer.arm("partial-error"); Capture capture;
      auto op = client.start(request(model, messages), run(true, true, true), callback ? capture.callbacks() : sp::runtime::Callbacks{});
      auto result = callback ? finish(op, capture) : op.join();
      const auto& f = failure(result);
      require(f.error.retry_safety == sp::RetrySafety::OutputObserved && text(f.partial.messages) == "partial", "partial output lost or retried");
      require(f.error.attempt.attempts == 1, "output-observed attempt retried"); peer.count(model, 1, 1);
    }
    const auto ping = peer.arm("ping-recover"); success(client.complete(request(ping, messages), run(true, true, true))); peer.count(ping, 2, 1);
    const auto partial = peer.arm("partial"); Capture c;
    auto op = client.start(request(partial, messages), run(), c.callbacks()); peer.wait(partial, "held"); c.delta(); peer.release(partial);
    const auto partial_result = finish(op, c);
    const auto& f = failure(partial_result); require(text(f.partial.messages) == "partial", "reset lost retained partial"); peer.count(partial, 1, 1);
    const auto gate = peer.arm("close-gate"); Capture gated;
    auto g = client.start(request(gate, messages), run(), gated.callbacks()); peer.wait(gate, "held"); gated.delta();
    { std::lock_guard lock(gated.mutex); require(!gated.result, "Commit occurred before normal transport close"); }
    peer.release(gate); success(finish(g, gated)); peer.count(gate, 1);
    for (bool streaming : {false, true}) {
      const auto short_body = peer.arm("short-close");
      failure(client.complete(request(short_body, messages), run(streaming, true, true))); peer.count(short_body, 1, 1);
    }
  }
}
void typeless_messages_runtime_error(Peer& peer) {
  Result retained;
  {
    Client client(descriptor(peer, true), options());
    for (bool retry : {false, true}) {
      const auto model = peer.arm("typeless-buffered-error");
      auto controls = run(false, retry, true); controls.retry->max_attempts = 2;
      const auto result = client.complete(request(model, true), controls);
      const auto& f = failure(result, sp::ErrorKind::Overloaded);
      const auto attempts = retry ? 2U : 1U;
      require(f.error.vendor_code == "overloaded_error" && f.error.retry_class == sp::RetryClass::Transient &&
          f.error.retry_safety == sp::RetrySafety::PossiblyAccepted && f.error.attempt.attempts == attempts &&
          f.error.http_status == 200 && f.error.attempt.response_head_seen && f.error.attempt.request_may_have_left,
          "typeless buffered error lost vendor or retry evidence");
      require(f.partial.messages.empty() && !f.partial.stop && !f.partial.wire_envelope &&
          f.partial.usage.stage == sp::UsageStage::Missing && !f.partial.usage.total,
          "raw error document became semantic output or manufactured usage");
      require(f.partial.raw_events.size() == attempts && f.error.attempt.prior_usage_unknown == retry,
          "retry lost previous actual error observations or attempt uncertainty");
      for (const auto& raw : f.partial.raw_events) {
        require(raw.type == "messages.buffered" && !raw.payload->root().get("type").valid() &&
            raw.payload->root().get("error").get("type").as_string() == "overloaded_error" &&
            raw.payload->root().get("error").get("message").as_string() == marker,
            "typeless buffered raw diagnostic changed provider JSON");
      }
      peer.count(model, attempts, attempts);
      retained = result;
    }
  }
  const auto& raw = failure(retained, sp::ErrorKind::Overloaded).partial.raw_events;
  require(raw[0].payload->root().get("vendor").get("b").as_bool() &&
      raw[1].payload->root().get("vendor").get("a").is_null(), "owned error DOM did not survive client destruction");
}
void minima_budget_deadline(Peer& peer) {
  for (bool messages : {false, true}) {
    auto opts = options(); opts.retry_tokens = 1; Client client(descriptor(peer, messages), opts);
    const auto one = peer.arm("always-error"), two = peer.arm("always-error");
    auto a = request(one, messages), b = request(two, messages);
    if (messages) std::get<sp::messages::Request>(b).account_scope = "another-account-cannot-renew-bucket";
    failure(client.complete(a, run(false, true, true))); failure(client.complete(b, run(false, true, true)));
    peer.count(one, 2, 2); peer.count(two, 1, 1);
  }
  {
    auto opts = options(); opts.retry_tokens = 1; Client racing(descriptor(peer, false), opts);
    const auto first = peer.arm("recover"), second = peer.arm("recover");
    auto a = racing.start(request(first), run(false, true, true));
    auto b = racing.start(request(second), run(false, true, true));
    const auto ar = a.join(), br = b.join();
    const auto successes = unsigned(std::holds_alternative<sp::Completion>(*ar)) + unsigned(std::holds_alternative<sp::Completion>(*br));
    require(successes == 1, "concurrent operations did not share a single retry token");
    auto as = peer.stats(first), bs = peer.stats(second);
    require(as.root().get("count").as_uint() + bs.root().get("count").as_uint() == 3,
      "shared retry budget overspent under simultaneous failures");
    peer.count(first, std::holds_alternative<sp::Completion>(*ar) ? 2 : 1, 1);
    peer.count(second, std::holds_alternative<sp::Completion>(*br) ? 2 : 1, 1);
  }
  Client client(descriptor(peer, false), options());
  const auto rate = peer.arm("rate"); auto r = run(false, true, true); r.retry->max_attempts = 2;
  failure(client.complete(request(rate), r), sp::ErrorKind::RateLimited); peer.count(rate, 2, 2);
  auto times = peer.stats(rate); auto ts = times.root().get("times");
  require(ts.at(1).as_double() - ts.at(0).as_double() >= 1240.0, "retry dispatched before strongest server minimum");
  for (const auto scenario : {"long-rate", "malformed-rate"}) {
    const auto model = peer.arm(scenario); auto deadline = run(false, true, true); deadline.deadline = Clock::now() + 3s;
    const auto result = client.complete(request(model), deadline); failure(result, sp::ErrorKind::RateLimited); peer.count(model, 1, 1);
  }
  const auto expired = peer.arm("normal"); auto past = run(); past.deadline = Clock::now() - 1ms;
  failure(client.complete(request(expired), past), sp::ErrorKind::DeadlineExceeded); peer.count(expired, 0);
  const auto held = peer.arm("hold"); auto timeout = run(); timeout.deadline = Clock::now() + 500ms;
  Capture capture; auto op = client.start(request(held), timeout, capture.callbacks()); peer.wait(held, "held");
  failure(finish(op, capture), sp::ErrorKind::DeadlineExceeded); peer.count(held, 1);
  const auto retried = peer.arm("retry-held"); auto absolute = run(false, true, true);
  absolute.deadline = Clock::now() + 2500ms; Capture retry_capture;
  auto retry_op = client.start(request(retried), absolute, retry_capture.callbacks()); peer.wait(retried, "held");
  failure(finish(retry_op, retry_capture), sp::ErrorKind::DeadlineExceeded);
  require(Clock::now() < *absolute.deadline + 2s, "retry reset the absolute deadline");
  peer.count(retried, 2, 1);
  for (bool stop : {false, true}) {
    const auto model = peer.arm("rate"); std::stop_source source; auto backoff = run(false, true, true); backoff.stop_token = source.get_token();
    Capture c; auto pending = client.start(request(model), backoff, c.callbacks()); peer.wait(model, "closed");
    if (stop) source.request_stop(); else pending.cancel();
    failure(finish(pending, c), sp::ErrorKind::Cancelled); peer.count(model, 1, 1);
  }
}
void ownership(Peer& peer) {
  Client client(descriptor(peer, false), options());
  for (const auto how : {0, 1, 2, 3}) {
    const auto model = peer.arm("hold"); Capture capture; std::stop_source source; auto ro = run(); ro.stop_token = source.get_token();
    auto op = client.start(request(model), ro, capture.callbacks()); peer.wait(model, "held");
    if (how == 0) op.cancel();
    if (how == 1) source.request_stop();
    if (how == 2) op = Operation{};
    if (how == 3) { op.detach(); require(!op.valid(), "detached handle remained valid"); peer.release(model); }
    auto result = capture.await();
    if (how == 3) success(result); else failure(result, sp::ErrorKind::Cancelled);
    if (op.valid()) require(op.join() == result, "cancelled join changed result");
    peer.count(model, 1);
  }
  const auto before = peer.arm("normal"); std::stop_source stopped; stopped.request_stop(); auto ro = run(); ro.stop_token = stopped.get_token();
  failure(client.complete(request(before), ro), sp::ErrorKind::Cancelled); peer.count(before, 0);
  failure(Operation{}.join(), sp::ErrorKind::Misuse);
  const auto moved = peer.arm("hold"); Capture c; auto old = client.start(request(moved), run(), c.callbacks());
  auto live = std::move(old); failure(old.join(), sp::ErrorKind::Misuse); peer.wait(moved, "held"); live.cancel(); failure(finish(live, c), sp::ErrorKind::Cancelled);
  const auto destruction = peer.arm("hold"); Capture destroyed; Operation pending;
  { Client scoped(descriptor(peer, false), options()); pending = scoped.start(request(destruction), run(), destroyed.callbacks()); peer.wait(destruction, "held"); }
  failure(finish(pending, destroyed), sp::ErrorKind::Cancelled); peer.count(destruction, 1);
}
void callback_ownership_and_misuse(Peer& peer) {
  for (bool drop_client : {false, true}) {
    auto client = std::make_unique<Client>(descriptor(peer, false), options());
    const auto model = peer.arm("partial"); Capture capture; std::mutex gate; std::optional<Operation> handle; bool fired = false;
    auto callbacks = capture.callbacks(); auto record = callbacks.on_event;
    callbacks.on_event = [&](const sp::Event& event) {
      record(event);
      if (!std::holds_alternative<sp::PartDelta>(event)) return;
      std::lock_guard lock(gate);
      if (fired) return;
      fired = true;
      if (drop_client) client.reset(); else handle.reset();
    };
    { std::lock_guard lock(gate); handle.emplace(client->start(request(model), run(), std::move(callbacks))); }
    failure(capture.await(), sp::ErrorKind::Cancelled);
    { std::lock_guard lock(gate); require(fired, "drop callback never fired"); if (handle) failure(handle->join(), sp::ErrorKind::Cancelled); }
    peer.count(model, 1);
  }
  Client client(descriptor(peer, false), options());
  const auto model = peer.arm("partial"), nested = peer.arm("normal");
  Capture capture; std::mutex gate; Operation op; Result self, complete; bool checked = false;
  auto cb = capture.callbacks(); auto record = cb.on_event;
  cb.on_event = [&](const sp::Event& event) {
    record(event);
    if (!std::holds_alternative<sp::PartDelta>(event)) return;
    std::lock_guard lock(gate);
    self = op.join(); complete = client.complete(request(nested)); checked = true; op.cancel();
  };
  { std::lock_guard lock(gate); op = client.start(request(model), run(), std::move(cb)); }
  failure(finish(op, capture), sp::ErrorKind::Cancelled);
  require(checked, "misuse callback never ran"); failure(self, sp::ErrorKind::Misuse); failure(complete, sp::ErrorKind::Misuse); peer.count(nested, 0); peer.count(model, 1);
}
void raw_io_misuse(Peer& peer) {
  Client client(descriptor(peer, false), options());
  const auto held = peer.arm("hold"), nested = peer.arm("normal"), raw_model = peer.arm("normal");
  auto operation = client.start(request(held));
  peer.wait(held, "held");
  sp::transport::Transport transport;
  sp::transport::HttpRequest raw;
  raw.url = "http://127.0.0.1:" + std::to_string(peer.port) + "/v1/chat/completions";
  raw.headers = {{"Authorization", "Bearer " + std::string(marker)}, {"Content-Type", "application/json"}};
  raw.body = "{\"model\":" + sp::json::quote(raw_model) + ",\"messages\":[],\"stream\":false}";
  raw.deadline = Clock::now() + 5s;
  Result joined, completed;
  auto transfer = transport.start(std::move(raw), {
    [&](const sp::transport::ResponseHead&) {
      joined = operation.join();
      completed = client.complete(request(nested));
    }, {}, [](const sp::transport::Result&) {}
  });
  require(transfer.join().status == sp::transport::Status::Completed, "unrelated raw transfer failed");
  failure(joined, sp::ErrorKind::Misuse); failure(completed, sp::ErrorKind::Misuse);
  operation.cancel(); failure(operation.join(), sp::ErrorKind::Cancelled);
  peer.count(held, 1); peer.count(nested, 0); peer.count(raw_model, 1);
}

void callback_fences_and_diagnostics(Peer& peer) {
  std::mutex mutex; std::condition_variable cv;
  bool entered = false, release = false, returned = false, joining = false;
  auto opts = options(); opts.slow_callback_threshold = 10ms; Client client(descriptor(peer, false), opts);
  // Opening the deliberately blocking callback is also mandatory on unwinding.
  struct OpenGate {
    std::mutex& mutex; std::condition_variable& cv; bool& release;
    void open() { std::lock_guard lock(mutex); release = true; cv.notify_all(); }
    ~OpenGate() { open(); }
  } gate{mutex, cv, release};
  const auto model = peer.arm("normal");
  auto payload = std::make_shared<int>(1); std::weak_ptr<int> weak = payload;
  sp::runtime::Callbacks cb; cb.on_outcome = [&, payload](Result) {
    std::unique_lock lock(mutex); entered = true; cv.notify_all(); cv.wait(lock, [&] { return release; });
  };
  payload.reset(); auto op = client.start(request(model), run(), std::move(cb));
  { std::unique_lock lock(mutex); require(cv.wait_for(lock, 15s, [&] { return entered; }), "outcome fence callback did not enter"); }
  Result joined;
  std::jthread waiter([&](std::stop_token stop) {
    std::stop_callback unblock(stop, [&] { gate.open(); });
    { std::lock_guard lock(mutex); joining = true; cv.notify_all(); }
    joined = op.join();
    { std::lock_guard lock(mutex); returned = true; cv.notify_all(); }
  });
  bool premature;
  {
    std::unique_lock lock(mutex); cv.wait(lock, [&] { return joining; });
    // Deliberately violate the callback latency contract; this is a diagnostic test,
    // not a scheduling sleep. The callback is held by an explicit condition gate.
    cv.wait_until(lock, Clock::now() + 100ms, [&] { return returned; });
    premature = returned; release = true; cv.notify_all();
  }
  waiter.join(); require(!premature, "join returned before outcome callback exit"); success(joined);
  require(weak.expired(), "callback storage retained after join"); require(client.diagnostics().slow_callbacks >= 1, "slow callback not diagnosed");
  const auto throwing = peer.arm("partial"); Capture c; auto throwing_cb = c.callbacks();
  throwing_cb.on_event = [](const sp::Event& event) { if (std::holds_alternative<sp::PartDelta>(event)) throw std::runtime_error(std::string(marker)); };
  auto failed = client.start(request(throwing), run(), std::move(throwing_cb));
  const auto failed_result = finish(failed, c);
  const auto& f = failure(failed_result, sp::ErrorKind::Misuse); require(text(f.partial.messages) == "partial", "callback exception discarded output");
  const auto terminal = peer.arm("normal"); auto terminal_op = client.start(request(terminal), run(), {{}, [](Result) { throw std::runtime_error(std::string(marker)); }});
  success(terminal_op.join()); require(client.diagnostics().callback_exceptions == 2, "callback exception diagnostics not exact");
}
int thread_count() {
  std::ifstream input("/proc/self/status"); std::string line;
  while (std::getline(input, line)) if (line.starts_with("Threads:")) return std::stoi(line.substr(8));
  throw std::runtime_error("cannot inspect process thread count");
}
void held_stream_gate(Peer& peer) {
  const int baseline = thread_count();
  auto opts = options(); opts.limits.max_operations = 256;
  opts.workers = 2; opts.transport.io_threads = 2; opts.transport.resolver_threads = 2;
  const std::size_t threads = opts.workers + opts.transport.io_threads + opts.transport.resolver_threads;
  const std::size_t k = 4 * threads + 64;
  Client client(descriptor(peer, false), opts);
  int first = 0;
  for (auto count : {k, 2 * k}) {
    const auto held = peer.arm("hold"); std::vector<Operation> operations; operations.reserve(count);
    for (std::size_t i = 0; i < count; ++i) operations.push_back(client.start(request(held)));
    peer.wait(held, "held", count);
    const int measured = thread_count(); require(measured <= baseline + static_cast<int>(threads), "held requests allocated per-request threads");
    if (first) require(measured <= first, "thread count increased from K to 2K held requests"); else first = measured;
    const auto short_model = peer.arm("normal"); auto short_run = run(); short_run.deadline = Clock::now() + 5s;
    success(client.complete(request(short_model), short_run)); peer.count(short_model, 1);
    peer.release(held);
    for (auto& op : operations) success(op.join());
    peer.count(held, count);
  }
}
void resource_and_admission(Peer& peer) {
  {
    auto opts = options(); opts.limits.max_operations = 1; Client client(descriptor(peer, false), opts);
    const auto held = peer.arm("hold"), denied = peer.arm("normal"); auto op = client.start(request(held)); peer.wait(held, "held");
    failure(client.complete(request(denied)), sp::ErrorKind::ResourceLimit); peer.count(denied, 0);
    op.cancel(); failure(op.join(), sp::ErrorKind::Cancelled);
    success(client.complete(request(denied))); peer.count(denied, 1);
  }
  for (int cap = 0; cap < 10; ++cap) {
    auto opts = options(); const bool messages = cap >= 5; bool streaming = cap != 0 && cap != 6 && cap != 7 && cap != 8;
    std::string scenario = "normal"; std::string large(8192, 'x');
    if (cap == 0) opts.limits.max_response_bytes = 128;
    if (cap == 1) opts.limits.sse.max_line_bytes = 128;
    if (cap == 2) opts.limits.sse.max_event_bytes = 128;
    if (cap == 3) { opts.limits.sse.max_total_bytes = 512; scenario = "flood"; }
    if (cap == 4) { opts.transport.max_head_bytes = 512; scenario = "header-cap"; }
    if (cap == 5) { opts.limits.semantic.max_parts = 1; scenario = "parts"; }
    if (cap == 6) { opts.limits.semantic.max_tool_bytes = 128; scenario = "tool"; }
    if (cap == 7) { opts.limits.semantic.max_content_bytes = 128; scenario = "reasoning"; }
    if (cap == 8) { opts.limits.max_error_bytes = 128; scenario = "error-cap"; }
    if (cap == 9) opts.limits.max_response_bytes = 128;
    Client client(descriptor(peer, messages), opts); const auto model = peer.arm(scenario, large);
    failure(client.complete(request(model, messages), run(streaming, true, true)), sp::ErrorKind::ResourceLimit); peer.count(model, 1, cap == 8 ? 1 : 0);
  }
  Client client(descriptor(peer, false), options()); const auto invalid = peer.arm("normal");
  auto empty = request(invalid); std::get<sp::chat::Request>(empty).messages.clear();
  failure(client.complete(empty), sp::ErrorKind::InvalidRequest); peer.count(invalid, 0);
  auto bad_retry = run(); bad_retry.retry->enabled = true; bad_retry.retry->max_attempts = 0;
  failure(client.complete(request(invalid), bad_retry)); peer.count(invalid, 0);
  const auto mismatch = client.complete(request(invalid, true));
  const auto mismatch_kind = failure(mismatch).error.kind;
  require(mismatch_kind == sp::ErrorKind::InvalidConfig || mismatch_kind == sp::ErrorKind::InvalidRequest,
    "family mismatch was not rejected at admission"); peer.count(invalid, 0);
}
void secrets_and_replay(Peer& peer) {
  Client client(descriptor(peer, true), options()); const auto model = peer.arm("reasoning");
  auto original = request(model, true); auto value = client.complete(original, run(false)); success(value);
  const auto& message = std::get<sp::Completion>(*value).messages.at(0);
  require(message.native && std::get<sp::Thinking>(message.parts.at(0)).text == marker, "explicit outcome lost opaque reasoning");
  auto replay = std::get<sp::messages::Request>(original); replay.messages.push_back(message);
  replay.account_scope = "different-account";
  failure(client.complete(replay, run(false)), sp::ErrorKind::ReplayIneligible); peer.count(model, 1);
  replay = std::get<sp::messages::Request>(original); replay.messages.push_back(message);
  std::get<sp::Thinking>(replay.messages.back().parts.front()).text += "tampered";
  failure(client.complete(replay, run(false)), sp::ErrorKind::ReplayIneligible); peer.count(model, 1);
  const auto vendor = peer.arm("vendor-secret"); failure(client.complete(request(vendor, true), run(false))); peer.count(vendor, 1, 1);
  auto rejected = sp::descriptor::load(descriptor_source(peer.port, false, "?secret=" + std::string(marker)));
  require(std::holds_alternative<sp::descriptor::ConfigError>(rejected), "credential query admitted");
  const auto& e = std::get<sp::descriptor::ConfigError>(rejected);
  require(e.message.find(marker) == std::string::npos && e.pointer.find(marker) == std::string::npos && e.expected.find(marker) == std::string::npos,
    "rejected descriptor leaked secret");
  auto userinfo_source = descriptor_source(peer.port, false);
  userinfo_source.replace(userinfo_source.find("http://"), 7, "http://" + std::string(marker) + "@");
  auto userinfo = sp::descriptor::load(userinfo_source);
  require(std::holds_alternative<sp::descriptor::ConfigError>(userinfo), "credential userinfo admitted");
  const auto& ue = std::get<sp::descriptor::ConfigError>(userinfo);
  require(ue.message.find(marker) == std::string::npos && ue.pointer.find(marker) == std::string::npos
    && ue.expected.find(marker) == std::string::npos, "rejected userinfo leaked secret");
  auto opts = options(); opts.api_key += '\n'; bool caught = false;
  try { Client bad(descriptor(peer, false), opts); }
  catch (const sp::descriptor::ConfigError& error) { caught = true; require(error.message.find(marker) == std::string::npos, "credential validation leaked secret"); }
  require(caught, "control-bearing credential accepted");
}

// Unsupported caller misuse: callback waits for another thread which is joining
// the callback's operation. Observe only in an exec-isolated process with a guard.
int cycle_child(const char* node, const char* script) {
  Peer peer(node, script); Client client(descriptor(peer, false), options());
  std::mutex mutex; std::condition_variable cv; bool callback = false, joining = false, joined = false;
  const auto model = peer.arm("normal");
  auto op = client.start(request(model), run(), {{}, [&](Result) {
    std::unique_lock lock(mutex); callback = true; cv.notify_all(); cv.wait(lock, [&] { return joining; });
    write_all(STDOUT_FILENO, "CYCLE_ARMED\n");
    cv.wait(lock, [&] { return joined; });
  }});
  { std::unique_lock lock(mutex); cv.wait(lock, [&] { return callback; }); joining = true; cv.notify_all(); }
  op.join();
  { std::lock_guard lock(mutex); joined = true; cv.notify_all(); }
  return 7;
}
void guarded_cycle(const char* executable, const char* node, const char* script) {
  runtime_test::Pipe output;
  auto pid = fork();
  if (pid == 0) {
    output.reader.reset();
    if (setpgid(0, 0) < 0 || !output.writer.redirect_to(STDOUT_FILENO)) _exit(126);
    execl(executable, executable, node, script, "--join-cycle-child", static_cast<char*>(nullptr)); _exit(127);
  }
  output.writer.reset();
  require(pid > 0, "cycle process fork failed");
  runtime_test::Process child(pid, SIGKILL, true);
  if (setpgid(pid, pid) < 0) require(errno == EACCES, "cycle process group failed");
  std::string received; const auto deadline = Clock::now() + 15s;
  while (received.find('\n') == std::string::npos && Clock::now() < deadline) {
    pollfd fd{output.reader.get(), POLLIN, 0};
    auto ready = poll(&fd, 1, 1000); if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) continue;
    char buffer[64]; auto n = ::read(output.reader.get(), buffer, sizeof(buffer)); if (n <= 0) break;
    received.append(buffer, static_cast<std::size_t>(n));
  }
  require(received == "CYCLE_ARMED\n", "unsupported cross-thread cycle was not armed");
  pollfd fd{output.reader.get(), POLLIN, 0};
  int ready;
  do { ready = poll(&fd, 1, 100); } while (ready < 0 && errno == EINTR);
  require(ready == 0 && child.running(), "unsupported cross-thread cycle did not remain blocked under guard");
}
struct AlarmGuard {
  AlarmGuard() { alarm(150); }
  ~AlarmGuard() { alarm(0); }
};
} // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string_view(argv[3]) == "--join-cycle-child") return cycle_child(argv[1], argv[2]);
  if (argc != 3) { std::cerr << "usage: sp_runtime_tests <node> <runtime_server.mjs>\n"; return 2; }
  std::signal(SIGPIPE, SIG_IGN);
  // Broad process-level guard also bounds accidental join deadlocks, independently
  // of operation deadlines. There are no per-request test threads.
  AlarmGuard alarm_guard;
  runtime_test::LogCapture logs;
  std::string failed, section = "guarded cycle";
  auto check = [&](std::string_view name, auto&& body) { section = name; body(); };
  try {
    guarded_cycle(argv[0], argv[1], argv[2]);
    check("RAII exception ownership", [&] { owner_exception_paths(argv[2]); });
    Peer peer(argv[1], argv[2]);
    check("quota abnormal close", [&] { quota_abnormal_close(peer); });
    check("family modes", [&] { family_modes(peer); });
    check("retry policy", [&] { retry_policy(peer); });
    check("safe connect retry", [&] { safe_connect_retry(); });
    check("partial, ping and close", [&] { partial_ping_and_close(peer); });
    check("typeless Messages buffered error", [&] { typeless_messages_runtime_error(peer); });
    check("minima, bucket and deadline", [&] { minima_budget_deadline(peer); });
    check("ownership", [&] { ownership(peer); });
    check("callback ownership and misuse", [&] { callback_ownership_and_misuse(peer); });
    check("raw I/O blocking misuse", [&] { raw_io_misuse(peer); });
    check("callback fences and diagnostics", [&] { callback_fences_and_diagnostics(peer); });
    check("held stream gate", [&] { held_stream_gate(peer); });
    check("resource and admission", [&] { resource_and_admission(peer); });
    check("secrets and replay", [&] { secrets_and_replay(peer); });
  } catch (const std::exception& error) { failed = section + ": " + error.what(); }
  const auto captured = logs.finish();
  if (captured.find(marker) != std::string::npos || failed.find(marker) != std::string::npos) failed = "secret marker leaked to diagnostic logs";
  if (!failed.empty()) { std::cerr << "runtime properties failed: " << failed << '\n'; return 1; }
  std::cout << "runtime properties: family/mode, retry, ownership, bounds, concurrency, privacy and guarded misuse verified\n";
  return 0;
}
