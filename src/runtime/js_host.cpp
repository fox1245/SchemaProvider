// The JavaScript reference host (runtime/js_host.h). JavaScript owns the event loop: it calls the exported functions
// below from timers, microtasks and fetch continuations, and the functions return. Nothing waits.
#include "runtime/js_host.h"
#include "sp/build_config.h"

#ifndef __EMSCRIPTEN__
#error "runtime/js_host.cpp is the Emscripten host; configure with an Emscripten toolchain and SP_BUILD_JS_HOST=ON"
#endif
#if SP_BUILD_THREADS
#error "the JavaScript host runs on the one JavaScript thread: configure with SP_THREADS=OFF"
#endif

#include <emscripten.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The JavaScript side. State lives on `Module.spJsHost` (one per module instance): the pending timers and the
// fetches in flight. Every call into the module goes through a function exported below, from a later turn of the
// event loop than the one that asked for it.
EM_JS(void, sp_js_drain_later, (unsigned executor, int macrotask), {
  const run = () => _sp_js_executor_drain(executor);
  if (macrotask) setTimeout(run, 0);
  else if (typeof queueMicrotask === 'function') queueMicrotask(run);
  else Promise.resolve().then(run);
});

EM_JS(void, sp_js_timer_set, (unsigned executor, unsigned timer, double delay_ms), {
  const host = (Module.spJsHost ||= { timers: new Map(), calls: new Map() });
  const key = executor + ':' + timer;
  const old = host.timers.get(key);
  if (old !== undefined) clearTimeout(old);
  host.timers.set(key, setTimeout(() => {
    host.timers.delete(key);
    _sp_js_executor_fire(executor, timer);
  }, delay_ms));
});

EM_JS(void, sp_js_timer_clear, (unsigned executor, unsigned timer), {
  const host = Module.spJsHost;
  if (!host) return;
  const key = executor + ':' + timer;
  const old = host.timers.get(key);
  if (old !== undefined) {
    clearTimeout(old);
    host.timers.delete(key);
  }
});

// Starts a fetch. `head` is `method NUL url NUL (name NUL value NUL)*`, UTF-8. The answer comes back through
// sp_js_fetch_head, then sp_js_fetch_body once per chunk (the next chunk is read only after sp_js_fetch_next), and
// last through sp_js_fetch_end with a code (see FetchCall::finish).
EM_JS(void, sp_js_fetch_start, (unsigned call, const char* head, int head_len, const uint8_t* body, int body_len, int has_body,
                                int first_byte_ms, int idle_ms), {
  const host = (Module.spJsHost ||= { timers: new Map(), calls: new Map() });
  const c = { controller: new AbortController(), reader: null, ended: false, aborted: false, why: 0,
              credit: 0, waiter: null, first: 0, idle: 0 };
  host.calls.set(call, c);
  // Exactly one end, and never from inside this call.
  const end = (code) => {
    if (c.ended) return;
    c.ended = true;
    clearTimeout(c.first);
    clearTimeout(c.idle);
    host.calls.delete(call);
    if (c.reader) c.reader.cancel().catch(() => {});
    Promise.resolve().then(() => _sp_js_fetch_end(call, code));
  };
  const fail = (code) => end(c.why || (c.aborted ? 3 : code));
  const parts = new TextDecoder().decode(HEAPU8.subarray(head, head + head_len)).split('\u0000');
  parts.pop();
  let init;
  try {
    const headers = new Headers();
    for (let i = 2; i + 1 < parts.length; i += 2) headers.append(parts[i], parts[i + 1]);
    // Only what every environment accepts: a Worker rejects several of the optional fields.
    init = { method: parts[0], headers, signal: c.controller.signal, redirect: 'manual' };
    if (has_body) init.body = HEAPU8.slice(body, body + body_len);
    new URL(parts[1]);
  } catch (e) {
    end(6);
    return;
  }
  if (first_byte_ms > 0) c.first = setTimeout(() => { c.why = 4; c.controller.abort(); }, first_byte_ms);
  let sent;
  try { sent = globalThis.fetch(parts[1], init); } catch (e) { end(6); return; }
  sent.then(async (response) => {
    clearTimeout(c.first);
    if (c.ended) return;
    if (response.type === 'opaqueredirect' || response.status === 0) { end(7); return; }
    let names = "";
    response.headers.forEach((value, name) => { names += name + '\u0000' + value + '\u0000'; });
    const text = new TextEncoder().encode(names);
    // The room is asked for first: memory may grow, and HEAPU8 then names the new view.
    const room = _sp_js_scratch(text.length);
    HEAPU8.set(text, room);
    _sp_js_fetch_head(call, response.status, text.length);
    if (!response.body) { end(0); return; }
    c.reader = response.body.getReader();
    for (;;) {
      // The idle bound runs while a read is outstanding, not while the runtime holds the stream paused.
      if (idle_ms > 0) c.idle = setTimeout(() => { c.why = 5; c.controller.abort(); }, idle_ms);
      let step;
      try { step = await c.reader.read(); } catch (e) { clearTimeout(c.idle); fail(2); return; }
      clearTimeout(c.idle);
      if (c.ended) return;
      if (step.done) { end(0); return; }
      if (step.value.length === 0) continue;
      const at = _sp_js_scratch(step.value.length);
      HEAPU8.set(step.value, at);
      _sp_js_fetch_body(call, step.value.length);
      if (c.credit > 0) c.credit--;
      else await new Promise((resolve) => { c.waiter = resolve; });
      if (c.aborted) { fail(3); return; }
    }
  }, () => fail(1)).catch(() => fail(2));
});

// The runtime consumed the chunk handed over: read the next one.
EM_JS(void, sp_js_fetch_next, (unsigned call), {
  const c = Module.spJsHost && Module.spJsHost.calls.get(call);
  if (!c) return;
  if (c.waiter) { const w = c.waiter; c.waiter = null; w(); }
  else c.credit++;
});

EM_JS(void, sp_js_fetch_abort, (unsigned call), {
  const c = Module.spJsHost && Module.spJsHost.calls.get(call);
  if (!c || c.ended) return;
  c.aborted = true;
  c.controller.abort();
  if (c.waiter) { const w = c.waiter; c.waiter = null; w(); }
});

namespace sp::runtime::js {
namespace {

using Clock = std::chrono::steady_clock;

class JsExecutor;
class FetchCall;

struct Registry {
  std::unordered_map<unsigned, std::weak_ptr<JsExecutor>> executors;
  std::unordered_map<unsigned, std::shared_ptr<FetchCall>> calls;
  unsigned last_executor = 0, last_call = 0, last_transport = 0;
  // The one buffer bytes cross the boundary in: JavaScript asks for room, fills it and calls back at once.
  std::string scratch;
};
Registry& registry() {
  static Registry value;
  return value;
}

constexpr unsigned kBatch = 128;
constexpr double kLongestTimeoutMs = 2147483647.0;

class JsExecutor final : public Executor {
 public:
  explicit JsExecutor(unsigned id) : id_(id) {}
  ~JsExecutor() override {
    shutdown();
    registry().executors.erase(id_);
  }
  void post(Task task) override {
    if (stopped_) return;
    ready_.push_back(std::move(task));
    if (!scheduled_) {
      scheduled_ = true;
      sp_js_drain_later(id_, 0);
    }
  }
  Timer schedule(SteadyTime at, Task task) override {
    if (stopped_) return 0;
    const auto id = ++last_timer_;
    timers_.emplace(id, Timed{at, std::move(task)});
    sp_js_timer_set(id_, static_cast<unsigned>(id), delay_ms(at));
    return id;
  }
  void cancel(Timer id) noexcept override {
    const auto found = timers_.find(id);
    if (found == timers_.end()) return;
    Task released = std::move(found->second.task);
    timers_.erase(found);
    sp_js_timer_clear(id_, static_cast<unsigned>(id));
  }
  SteadyTime now() const noexcept override { return Clock::now(); }
  WallTime wall_now() const noexcept override { return std::chrono::system_clock::now(); }
  bool in_thread() const noexcept override { return running_ != 0; }
  bool host_pumped() const noexcept override { return true; }
  void shutdown() noexcept override {
    stopped_ = true;
    std::deque<Task> dropped_ready;
    dropped_ready.swap(ready_);
    std::map<Timer, Timed> dropped_timers;
    dropped_timers.swap(timers_);
    for (const auto& entry : dropped_timers) sp_js_timer_clear(id_, static_cast<unsigned>(entry.first));
  }

  // Called from JavaScript.
  void drain() {
    for (unsigned turn = 0; turn < kBatch && !ready_.empty(); ++turn) {
      auto task = std::move(ready_.front());
      ready_.pop_front();
      run(task);
    }
    // More than a batch is waiting: continue from a timer, so that input can arrive in between.
    if (ready_.empty()) scheduled_ = false;
    else sp_js_drain_later(id_, 1);
  }
  void fire(Timer id) {
    const auto found = timers_.find(id);
    if (found == timers_.end()) return;
    if (found->second.at > now()) {  // a timer can fire a little early
      sp_js_timer_set(id_, static_cast<unsigned>(id), delay_ms(found->second.at));
      return;
    }
    auto task = std::move(found->second.task);
    timers_.erase(found);
    run(task);
  }

 private:
  struct Timed { SteadyTime at; Task task; };
  double delay_ms(SteadyTime at) const noexcept {
    const auto left = at - now();
    if (left <= SteadyTime::duration::zero()) return 0;
    return std::min(std::ceil(std::chrono::duration<double, std::milli>(left).count()), kLongestTimeoutMs);
  }
  // An operation's boundary owns the delivery of its errors; one that escapes must not unwind into JavaScript.
  void run(Task& task) noexcept {
    ++running_;
    try { task(); } catch (...) {}
    --running_;
  }
  const unsigned id_;
  std::deque<Task> ready_;
  std::map<Timer, Timed> timers_;
  Timer last_timer_ = 0;
  unsigned running_ = 0;
  bool scheduled_ = false, stopped_ = false;
};

// The codes JavaScript ends a fetch with.
namespace end_code {
constexpr int Completed = 0;
constexpr int NoResponse = 1;      // fetch rejected before a response: the network, DNS, TLS or the peer
constexpr int StreamBroke = 2;     // the body failed after the head
constexpr int Aborted = 3;         // cancelled by the runtime
constexpr int FirstByteStall = 4;
constexpr int IdleStall = 5;
constexpr int NotSent = 6;         // the request was malformed for fetch, or fetch is missing
constexpr int OpaqueResponse = 7;  // a redirect or other response the environment hides
constexpr int NoFiles = 8;         // a file body, which the host cannot read
}  // namespace end_code

class FetchCall : public std::enable_shared_from_this<FetchCall> {
 public:
  FetchCall(unsigned id, unsigned owner, std::shared_ptr<Executor> executor, transport::Callbacks callbacks, std::size_t body_bytes)
      : id_(id), owner_(owner), executor_(std::move(executor)), callbacks_(std::move(callbacks)), body_bytes_(body_bytes),
        started_(Clock::now()) {}
  unsigned owner() const noexcept { return owner_; }

  void cancel() noexcept {
    if (finished_ || cancelled_) return;
    cancelled_ = true;
    sp_js_fetch_abort(id_);
  }
  void resume() noexcept {
    try {
      executor_->post([self = shared_from_this()] {
        if (self->finished_ || !self->paused_) return;
        self->paused_ = false;
        self->offer(std::move(self->pending_));
      });
    } catch (...) {}
  }

  // What JavaScript reports, handed to the executor in the order it was produced.
  void post_head(transport::ResponseHead value) {
    executor_->post([self = shared_from_this(), value = std::move(value)] { self->head(value); });
  }
  void post_body(std::string bytes) {
    executor_->post([self = shared_from_this(), bytes = std::move(bytes)]() mutable { self->offer(std::move(bytes)); });
  }
  void post_end(int code) {
    executor_->post([self = shared_from_this(), code] { self->finish(code); });
  }
  void finish(int code) {
    if (finished_) return;
    finish_now(code);
  }

 private:
  void head(const transport::ResponseHead& value) {
    if (finished_) return;
    head_seen_ = true;
    status_ = value.status;
    if (callbacks_.on_head) callbacks_.on_head(value);
  }
  void offer(std::string bytes) {
    if (finished_) return;
    const bool consumed = !callbacks_.on_body || callbacks_.on_body(bytes);
    if (consumed) sp_js_fetch_next(id_);
    else {
      // Not consumed: keep the chunk, read nothing more, and offer it again at resume().
      pending_ = std::move(bytes);
      paused_ = true;
    }
  }
  void finish_now(int code) {
    finished_ = true;
    auto self = shared_from_this();
    transport::Result result;
    result.http_status = status_;
    result.elapsed = Clock::now() - started_;
    auto& attempt = result.attempt;
    attempt.response_head_seen = head_seen_;
    attempt.version = transport::ResponseVersion::Unknown;
    // Fetch does not say when the request left. Once it was handed over, it may have.
    attempt.reached = head_seen_ ? transport::Stage::ResponseStarted : transport::Stage::RequestStarted;
    // A response head means the request body was received whole.
    attempt.request_body_bytes = head_seen_ ? static_cast<std::int64_t>(body_bytes_) : 0;
    result.status = transport::Status::Failed;
    switch (code) {
      case end_code::Completed: result.status = transport::Status::Completed; break;
      case end_code::NoResponse: result.failure = transport::FailureKind::Receive; break;
      case end_code::StreamBroke: result.failure = transport::FailureKind::Truncated; break;
      case end_code::Aborted: result.status = transport::Status::Cancelled; break;
      case end_code::FirstByteStall: result.failure = transport::FailureKind::FirstByteTimeout; break;
      case end_code::IdleStall: result.failure = transport::FailureKind::IdleTimeout; break;
      case end_code::OpaqueResponse: result.failure = transport::FailureKind::Protocol; break;
      case end_code::NoFiles:
        result.failure = transport::FailureKind::UploadSource;
        attempt.reached = transport::Stage::Queued;
        break;
      case end_code::NotSent:
      default:
        result.failure = transport::FailureKind::Other;
        attempt.reached = transport::Stage::Queued;
        break;
    }
    auto on_done = std::move(callbacks_.on_done);
    callbacks_ = {};
    pending_.clear();
    registry().calls.erase(id_);
    if (on_done) on_done(result);
  }

  const unsigned id_;
  const unsigned owner_;
  std::shared_ptr<Executor> executor_;
  transport::Callbacks callbacks_;
  const std::size_t body_bytes_;
  const Clock::time_point started_;
  std::string pending_;
  int status_ = 0;
  bool head_seen_ = false, finished_ = false, cancelled_ = false, paused_ = false;
};

class FetchAttempt final : public HostAttempt {
 public:
  explicit FetchAttempt(std::shared_ptr<FetchCall> call) : call_(std::move(call)) {}
  ~FetchAttempt() override { call_->cancel(); }
  void cancel() noexcept override { call_->cancel(); }
  void resume() noexcept override { call_->resume(); }
 private:
  std::shared_ptr<FetchCall> call_;
};

bool clean(std::string_view text) { return text.find('\0') == std::string_view::npos; }

class FetchTransport final : public HostTransport {
 public:
  explicit FetchTransport(std::shared_ptr<Executor> executor) : owner_(++registry().last_transport), executor_(std::move(executor)) {}
  std::unique_ptr<HostAttempt> start(transport::HttpRequest request, transport::Callbacks callbacks) override {
    auto& hub = registry();
    const auto id = ++hub.last_call;
    auto call = std::make_shared<FetchCall>(id, owner_, executor_, std::move(callbacks), request.body.size());
    hub.calls.emplace(id, call);
    // Nothing the request carries may hold the separator the boundary uses, and no file is readable here.
    std::string head = request.method;
    head.push_back('\0');
    head += request.url;
    head.push_back('\0');
    bool valid = clean(request.method) && clean(request.url);
    for (const auto& header : request.headers) {
      valid = valid && clean(header.name) && clean(header.value);
      head += header.name;
      head.push_back('\0');
      head += header.value;
      head.push_back('\0');
    }
    if (request.file_body || !valid) {
      call->post_end(request.file_body ? end_code::NoFiles : end_code::NotSent);
    } else {
      const auto bound = [](const std::optional<std::chrono::milliseconds>& value) {
        return value && value->count() > 0 ? static_cast<int>(std::min<long long>(value->count(), 2147483647LL)) : 0;
      };
      sp_js_fetch_start(id, head.data(), static_cast<int>(head.size()),
                        reinterpret_cast<const std::uint8_t*>(request.body.data()), static_cast<int>(request.body.size()),
                        request.body.empty() ? 0 : 1, bound(request.first_byte_timeout), bound(request.idle_timeout));
    }
    return std::make_unique<FetchAttempt>(std::move(call));
  }
  void shutdown() noexcept override {
    // Called after every attempt ended; a call still here is cancelled rather than left to JavaScript.
    std::vector<std::shared_ptr<FetchCall>> open;
    for (const auto& entry : registry().calls)
      if (entry.second->owner() == owner_) open.push_back(entry.second);
    for (const auto& call : open) call->cancel();
  }
 private:
  const unsigned owner_;
  std::shared_ptr<Executor> executor_;
};

std::shared_ptr<JsExecutor> executor_of(unsigned id) {
  const auto found = registry().executors.find(id);
  return found == registry().executors.end() ? nullptr : found->second.lock();
}
std::shared_ptr<FetchCall> call_of(unsigned id) {
  const auto found = registry().calls.find(id);
  return found == registry().calls.end() ? nullptr : found->second;
}

}  // namespace

std::shared_ptr<Executor> make_executor() {
  auto& hub = registry();
  const auto id = ++hub.last_executor;
  auto executor = std::make_shared<JsExecutor>(id);
  hub.executors.emplace(id, executor);
  return executor;
}

std::shared_ptr<HostTransport> make_fetch_transport(std::shared_ptr<Executor> executor) {
  return std::make_shared<FetchTransport>(std::move(executor));
}

}  // namespace sp::runtime::js

// What JavaScript calls. None of them throws, and none runs a callback of the runtime: each queues a task on the
// executor of the call, which runs it from its own turn.
extern "C" {
EMSCRIPTEN_KEEPALIVE char* sp_js_scratch(int size) {
  auto& scratch = sp::runtime::js::registry().scratch;
  scratch.assign(static_cast<std::size_t>(size), '\0');
  return scratch.data();
}

EMSCRIPTEN_KEEPALIVE void sp_js_executor_drain(unsigned executor) {
  if (auto found = sp::runtime::js::executor_of(executor)) found->drain();
}

EMSCRIPTEN_KEEPALIVE void sp_js_executor_fire(unsigned executor, unsigned timer) {
  if (auto found = sp::runtime::js::executor_of(executor)) found->fire(timer);
}

EMSCRIPTEN_KEEPALIVE void sp_js_fetch_head(unsigned call, int status, int size) {
  using namespace sp::runtime::js;
  try {
    auto found = call_of(call);
    if (!found) return;
    // `name NUL value NUL` pairs, names already lower-cased by Headers.
    sp::transport::ResponseHead head;
    head.status = status;
    head.framing = sp::transport::BodyFraming::Stream;
    const std::string_view blob(registry().scratch.data(), static_cast<std::size_t>(size));
    for (std::size_t at = 0; at < blob.size();) {
      const auto name_end = blob.find('\0', at);
      if (name_end == std::string_view::npos) break;
      const auto value_end = blob.find('\0', name_end + 1);
      if (value_end == std::string_view::npos) break;
      head.headers.push_back({std::string(blob.substr(at, name_end - at)), std::string(blob.substr(name_end + 1, value_end - name_end - 1))});
      at = value_end + 1;
    }
    found->post_head(std::move(head));
  } catch (...) {}
}

EMSCRIPTEN_KEEPALIVE void sp_js_fetch_body(unsigned call, int size) {
  using namespace sp::runtime::js;
  try {
    auto found = call_of(call);
    if (!found) return;
    std::string bytes = std::move(registry().scratch);
    registry().scratch.clear();
    bytes.resize(static_cast<std::size_t>(size));
    found->post_body(std::move(bytes));
  } catch (...) {}
}

EMSCRIPTEN_KEEPALIVE void sp_js_fetch_end(unsigned call, int code) {
  using namespace sp::runtime::js;
  try {
    if (auto found = call_of(call)) found->post_end(code);
  } catch (...) {}
}
}
