// The JavaScript reference host (runtime/js_host.h) against a local HTTP server that tests/js_host_test.mjs starts in
// the same Node process: the module has no thread to block on, so JavaScript starts a scenario, the event loop carries
// the fetch, and the module reports the outcome back through `Module.onReport(name, json)`. No network, no keys.
//
// Usage: node tests/js_host_test.mjs <sp_js_host_tests.js>
#include "codecs/chat.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include "runtime/client.h"
#include "runtime/js_host.h"
#include "sp/build_config.h"

#include <emscripten.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

EM_JS(void, sp_test_report, (const char* name, const char* json), {
  Module.onReport(UTF8ToString(name), UTF8ToString(json));
});
EM_JS_DEPS(sp_test_deps, "$UTF8ToString");

namespace {
using namespace sp;
using namespace sp::runtime;
using namespace std::chrono_literals;

// A JSON object written by hand.
class Object {
 public:
  Object& text(const char* key, std::string_view value) { return raw(key, json::quote(value)); }
  Object& number(const char* key, long long value) { return raw(key, std::to_string(value)); }
  Object& flag(const char* key, bool value) { return raw(key, value ? "true" : "false"); }
  Object& raw(const char* key, const std::string& value) {
    body_ += (body_.empty() ? "" : ",") + json::quote(key) + ":" + value;
    return *this;
  }
  std::string str() const { return "{" + body_ + "}"; }
 private:
  std::string body_;
};
std::string array_of(const std::vector<std::size_t>& values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) out += (i ? "," : "") + std::to_string(values[i]);
  return out + "]";
}

const char* kind_name(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::InvalidConfig: return "InvalidConfig";
    case ErrorKind::InvalidRequest: return "InvalidRequest";
    case ErrorKind::Unsupported: return "Unsupported";
    case ErrorKind::Transport: return "Transport";
    case ErrorKind::ProtocolCorrupt: return "ProtocolCorrupt";
    case ErrorKind::Truncated: return "Truncated";
    case ErrorKind::RemoteFailure: return "RemoteFailure";
    case ErrorKind::Cancelled: return "Cancelled";
    case ErrorKind::DeadlineExceeded: return "DeadlineExceeded";
    case ErrorKind::ResourceLimit: return "ResourceLimit";
    case ErrorKind::Misuse: return "Misuse";
    case ErrorKind::ReplayIneligible: return "ReplayIneligible";
    case ErrorKind::Authentication: return "Authentication";
    case ErrorKind::Permission: return "Permission";
    case ErrorKind::NotFound: return "NotFound";
    case ErrorKind::RateLimited: return "RateLimited";
    case ErrorKind::QuotaExhausted: return "QuotaExhausted";
    case ErrorKind::LimitUnknown: return "LimitUnknown";
    case ErrorKind::Overloaded: return "Overloaded";
  }
  return "?";
}
const char* safety_name(RetrySafety safety) {
  switch (safety) {
    case RetrySafety::NotSent: return "NotSent";
    case RetrySafety::PossiblyAccepted: return "PossiblyAccepted";
    case RetrySafety::RejectedBeforeOutput: return "RejectedBeforeOutput";
    case RetrySafety::OutputObserved: return "OutputObserved";
  }
  return "?";
}

const char* status_name(transport::Status status) {
  switch (status) {
    case transport::Status::Completed: return "Completed";
    case transport::Status::Cancelled: return "Cancelled";
    case transport::Status::DeadlineExceeded: return "DeadlineExceeded";
    case transport::Status::Failed: return "Failed";
  }
  return "?";
}
const char* failure_name(transport::FailureKind failure) {
  using F = transport::FailureKind;
  switch (failure) {
    case F::None: return "None";
    case F::Receive: return "Receive";
    case F::Truncated: return "Truncated";
    case F::Protocol: return "Protocol";
    case F::FirstByteTimeout: return "FirstByteTimeout";
    case F::IdleTimeout: return "IdleTimeout";
    case F::Other: return "Other";
    case F::UploadSource: return "UploadSource";
    default: return "Else";
  }
}
const char* stage_name(transport::Stage stage) {
  switch (stage) {
    case transport::Stage::Queued: return "Queued";
    case transport::Stage::Resolved: return "Resolved";
    case transport::Stage::Connected: return "Connected";
    case transport::Stage::RequestStarted: return "RequestStarted";
    case transport::Stage::ResponseStarted: return "ResponseStarted";
  }
  return "?";
}

// Counts what the fetch transport hands over, and where the stream was cut.
class Recording final : public HostTransport {
 public:
  explicit Recording(std::shared_ptr<HostTransport> inner) : inner_(std::move(inner)) {}
  std::unique_ptr<HostAttempt> start(transport::HttpRequest request, transport::Callbacks callbacks) override {
    ++requests;
    auto forward = callbacks.on_body;
    callbacks.on_body = [this, forward = std::move(forward)](std::string_view bytes) {
      chunks.push_back(bytes.size());
      return forward ? forward(bytes) : true;
    };
    return inner_->start(std::move(request), std::move(callbacks));
  }
  void shutdown() noexcept override { ++shutdowns; inner_->shutdown(); }
  std::vector<std::size_t> chunks;
  int requests = 0, shutdowns = 0;
 private:
  std::shared_ptr<HostTransport> inner_;
};

descriptor::ValidatedDescriptor descriptor_for(const std::string& base_url) {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"js-host","family":"openai.chat","connection":{"base_url":)" +
      json::quote(base_url) + R"(,"paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}})");
  if (!std::holds_alternative<descriptor::ValidatedDescriptor>(loaded)) sp_test_report("fatal", "{\"error\":\"descriptor\"}");
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
chat::Request chat_request(const std::string& path) {
  chat::Request request;
  request.model = path;  // the server routes on the model, which keeps one URL for every scenario
  request.messages.push_back({Role::User, "hello"});
  return request;
}

// One Client over the JavaScript host, and the calls made through it. Scenarios live until the process ends.
struct Scenario {
  std::string name;
  std::shared_ptr<Executor> executor = js::make_executor();
  std::shared_ptr<Recording> wire;
  std::optional<Client> client;
  std::vector<Operation> operations;
  std::vector<Result> outcomes;
  std::string streamed;
  std::size_t events = 0;
  std::size_t expected_outcomes = 1;

  Scenario(std::string n, const std::string& base_url) : name(std::move(n)), wire(std::make_shared<Recording>(js::make_fetch_transport(executor))) {
    client.emplace(descriptor_for(base_url), Options::with_api_key("sk-not-a-real-key"), executor, wire);
  }
  RunOptions run(bool streaming, std::chrono::milliseconds budget = 20s) {
    RunOptions options;
    options.streaming = streaming;
    options.retry = RetryPolicy{false, false, 1, 0ms, 0ms};
    options.deadline = executor->now() + budget;
    return options;
  }
  Object describe(const Result& result) const {
    Object out;
    out.text("scenario", name);
    if (is_completion(*result)) {
      out.flag("ok", true).text("text", joined_text(*result));
    } else {
      const auto& error = std::get<Failure>(*result).error;
      out.flag("ok", false).text("kind", kind_name(error.kind)).text("reason", error.reason).number("http_status", error.http_status)
         .text("safety", safety_name(error.retry_safety)).flag("sent", error.attempt.request_may_have_left);
    }
    out.number("events", static_cast<long long>(events)).text("streamed", streamed).number("requests", wire->requests)
       .number("shutdowns", wire->shutdowns).raw("chunks", array_of(wire->chunks));
    return out;
  }
  // Starts a chat call whose outcome is reported when `expected_outcomes` have arrived.
  Operation& call(const std::string& model, bool streaming, std::chrono::milliseconds budget = 20s,
                  std::function<void(const Event&)> on_event = {}, std::optional<std::chrono::milliseconds> idle = {}) {
    Callbacks callbacks;
    callbacks.on_event = [this, extra = std::move(on_event)](const Event& event) {
      ++events;
      if (const auto* delta = std::get_if<PartDelta>(&event))
        if (delta->payload.kind == PartKind::Text) streamed.append(delta->payload.bytes);
      if (extra) extra(event);
    };
    callbacks.on_outcome = [this](Result result) {
      outcomes.push_back(std::move(result));
      if (outcomes.size() == expected_outcomes) report();
    };
    auto options = run(streaming, budget);
    options.idle_timeout = idle;
    operations.push_back(client->start(chat_request(model), options, std::move(callbacks)));
    return operations.back();
  }
  void report() {
    Object out = describe(outcomes.front());
    out.number("outcomes", static_cast<long long>(outcomes.size()));
    sp_test_report(name.c_str(), out.str().c_str());
  }
};

std::vector<std::unique_ptr<Scenario>>& scenarios() {
  static std::vector<std::unique_ptr<Scenario>> all;
  return all;
}

// The executor on its own: order, timers, cancellation, a task that posts, and the batch that yields to the event loop.
void executor_scenario() {
  struct State {
    std::shared_ptr<Executor> executor = js::make_executor();
    std::vector<std::string> order;
    bool in_thread_inside = false, in_thread_outside = false;
    int batch_ran = 0;
  };
  static auto state = std::make_shared<State>();
  auto& s = *state;
  s.in_thread_outside = s.executor->in_thread();
  s.executor->post([] { state->order.push_back("post1"); state->in_thread_inside = state->executor->in_thread(); });
  s.executor->post([] {
    state->order.push_back("post2");
    state->executor->post([] { state->order.push_back("nested"); });
  });
  const auto now = s.executor->now();
  s.executor->schedule(now + 60ms, [] { state->order.push_back("t60"); });
  s.executor->schedule(now + 20ms, [] { state->order.push_back("t20"); });
  const auto cancelled = s.executor->schedule(now + 30ms, [] { state->order.push_back("cancelled"); });
  s.executor->cancel(cancelled);
  s.executor->cancel(cancelled);  // twice is nothing
  // More tasks than one batch: all run, in order, and a timer set meanwhile still fires.
  for (int i = 0; i < 1000; ++i) s.executor->post([i] { if (state->batch_ran == i) ++state->batch_ran; });
  s.executor->schedule(now + 120ms, [] {
    Object out;
    out.text("scenario", "executor");
    std::string order;
    for (const auto& item : state->order) order += (order.empty() ? "" : ",") + item;
    out.text("order", order).flag("in_thread_inside", state->in_thread_inside).flag("in_thread_outside", state->in_thread_outside)
       .flag("host_pumped", state->executor->host_pumped()).number("batch_ran", state->batch_ran);
    sp_test_report("executor", out.str().c_str());
  });
  // Nothing has run: `post` and `schedule` never run a task inline.
  if (!s.order.empty()) sp_test_report("executor", "{\"scenario\":\"executor\",\"inline\":true}");
}

// The transport on its own, for what a Client does not show: the request as sent, redirects, backpressure, and a
// completion that is never delivered before `start` returns.
struct DirectScenario {
  std::shared_ptr<Executor> executor = js::make_executor();
  std::shared_ptr<HostTransport> transport = js::make_fetch_transport(executor);
  std::string name, body;
  std::vector<std::unique_ptr<HostAttempt>> attempts;
  std::vector<std::string> events;
  std::vector<std::size_t> chunk_sizes;
  int status = 0;
  transport::Result done;
  bool finished = false, returned = false, called_before_return = false;
  int body_calls_while_paused = 0;
  bool paused = false, refuse_first = false, refused_once = false;
  std::size_t bytes = 0;
  std::uint64_t sum = 0;
  std::string content_type, location;

  transport::Callbacks callbacks() {
    transport::Callbacks cb;
    cb.on_head = [this](const transport::ResponseHead& head) {
      if (!returned) called_before_return = true;
      events.push_back("head");
      status = head.status;
      for (const auto& header : head.headers) {
        if (header.name == "content-type") content_type = header.value;
        if (header.name == "location") location = header.value;
      }
    };
    cb.on_body = [this](std::string_view chunk) {
      if (!returned) called_before_return = true;
      if (paused) ++body_calls_while_paused;
      if (refuse_first && !refused_once) {
        refused_once = true;
        paused = true;
        events.push_back("refused");
        // Resume after a while: the transport must have held the chunk and read nothing more in the meantime.
        executor->schedule(executor->now() + 300ms, [this] {
          events.push_back("resume");
          paused = false;
          attempts.front()->resume();
        });
        return false;
      }
      events.push_back("body");
      chunk_sizes.push_back(chunk.size());
      bytes += chunk.size();
      for (const unsigned char byte : chunk) sum += byte;
      body.append(chunk);  // kept, so the module's memory has to grow as the body arrives
      return true;
    };
    cb.on_done = [this](const transport::Result& result) {
      if (!returned) called_before_return = true;
      events.push_back("done");
      done = result;
      finished = true;
      report();
    };
    return cb;
  }
  void report() {
    Object out;
    std::string joined;
    for (const auto& item : events) joined += (joined.empty() ? "" : ",") + item;
    out.text("scenario", name).number("status", status).text("content_type", content_type).text("location", location)
       .text("body", body.size() > 4096 ? body.substr(0, 4096) : body).number("bytes", static_cast<long long>(bytes))
       .text("events", joined).raw("chunks", array_of(chunk_sizes)).flag("called_before_return", called_before_return)
       .text("done_status", status_name(done.status)).text("done_failure", failure_name(done.failure))
       .number("sum", static_cast<long long>(sum)).number("done_http_status", done.http_status).text("reached", stage_name(done.attempt.reached))
       .flag("head_seen", done.attempt.response_head_seen).number("paused_calls", body_calls_while_paused);
    sp_test_report(name.c_str(), out.str().c_str());
  }
};
std::vector<std::unique_ptr<DirectScenario>>& directs() {
  static std::vector<std::unique_ptr<DirectScenario>> all;
  return all;
}

void direct(const std::string& name, const std::string& base_url, const std::string& scenario) {
  auto owned = std::make_unique<DirectScenario>();
  auto& d = *owned;
  d.name = name;
  directs().push_back(std::move(owned));
  transport::HttpRequest request;
  request.url = base_url + "/" + scenario;
  request.deadline = d.executor->now() + 20s;
  if (scenario == "echo") {
    request.method = "POST";
    request.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer sk-not-a-real-key"}, {"X-Probe", "one"}};
    request.body = "{\"probe\":\"\xEC\x95\x88\xEB\x85\x95\"}";
  } else if (scenario == "backpressure") {
    d.refuse_first = true;
  } else if (scenario == "files") {
    request.method = "POST";
    request.file_body = transport::FileBody{"", "/does/not/exist", 0, 1, "", ""};
  } else if (scenario == "first-byte") {
    request.first_byte_timeout = 150ms;
  }
  d.attempts.push_back(d.transport->start(std::move(request), d.callbacks()));
  d.returned = true;
  if (scenario == "abort") {
    d.executor->schedule(d.executor->now() + 100ms, [&d] { d.attempts.front()->cancel(); d.attempts.front()->cancel(); });
  }
}

// The chat scenarios. `model` is the server's route.
void chat_scenario(const std::string& name, const std::string& base_url) {
  auto owned = std::make_unique<Scenario>(name, base_url);
  auto& s = *owned;
  scenarios().push_back(std::move(owned));
  if (name == "chat_buffered") {
    s.call("buffered", false);
    // Nothing ran inline: the outcome cannot be there before the event loop has turned.
    if (!s.outcomes.empty()) sp_test_report(name.c_str(), "{\"error\":\"inline completion\"}");
  } else if (name == "chat_stream") {
    s.call("stream", true);
  } else if (name == "chat_http_error") {
    s.call("limit", false);
  } else if (name == "chat_unreachable") {
    s.call("buffered", false);
  } else if (name == "chat_cancel") {
    s.call("slow", true, 20s, [&s](const Event& event) {
      if (std::holds_alternative<PartDelta>(event) && !s.operations.empty()) s.operations.front().cancel();
    });
  } else if (name == "chat_deadline") {
    s.call("hang", false, 300ms);
  } else if (name == "chat_idle_stall") {
    s.call("stall", true, 20s, {}, 200ms);
  } else if (name == "chat_blocking") {
    // The blocking calls are refused at once here, and nothing is sent for them.
    s.call("buffered", false);
    const auto joined = s.operations.front().join();
    const auto completed = s.client->complete(chat_request("buffered"), s.run(false));
    Object out;
    out.text("scenario", name).flag("join_refused", !is_completion(*joined) && std::get<Failure>(*joined).error.reason == blocking_host_pumped_reason)
       .flag("complete_refused", !is_completion(*completed) && std::get<Failure>(*completed).error.reason == blocking_host_pumped_reason)
       .number("requests_before_pump", s.wire->requests);
    sp_test_report("chat_blocking_refusal", out.str().c_str());
  } else if (name == "chat_destroy") {
    // The call is in flight when JavaScript destroys the Client (chat_destroy_now), from outside any task.
    s.call("slow", true);
    s.operations.front().detach();
  } else if (name == "chat_concurrent") {
    s.expected_outcomes = 3;
    for (int i = 0; i < 3; ++i) s.call("stream", true);
  }
}
}  // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE void sp_test_run(const char* name, const char* base_url) {
  try {
    const std::string scenario = name;
    if (scenario == "build") {
      Object out;
      out.text("scenario", "build").number("threads", SP_BUILD_THREADS).number("size_t", static_cast<long long>(sizeof(std::size_t)));
      sp_test_report("build", out.str().c_str());
    } else if (scenario == "chat_destroy_now") {
      // A Client destroyed with a call in flight returns at once, and the call ends Cancelled when the loop turns.
      for (auto it = scenarios().rbegin(); it != scenarios().rend(); ++it)
        if ((*it)->name == "chat_destroy") {
          (*it)->client.reset();
          Object out;
          out.flag("returned", true).number("outcomes_before_pump", static_cast<long long>((*it)->outcomes.size()))
             .number("shutdowns_before_pump", (*it)->wire->shutdowns);
          sp_test_report("chat_destroy_returned", out.str().c_str());
          break;
        }
    } else if (scenario == "chat_destroy_closed") {
      // Asked once the call has ended and the loop has turned: the transport was shut down by the last operation.
      Object out;
      for (auto it = scenarios().rbegin(); it != scenarios().rend(); ++it)
        if ((*it)->name == "chat_destroy") { out.number("shutdowns", (*it)->wire->shutdowns); break; }
      sp_test_report("chat_destroy_closed", out.str().c_str());
    } else if (scenario == "executor") {
      executor_scenario();
    } else if (scenario.rfind("direct_", 0) == 0) {
      direct(scenario, base_url, scenario.substr(7));
    } else {
      chat_scenario(scenario, base_url);
    }
  } catch (const std::exception& error) {
    sp_test_report("fatal", Object().text("error", error.what()).str().c_str());
  } catch (...) {
    sp_test_report("fatal", "{\"error\":\"unknown exception\"}");
  }
}
}

int main() { return 0; }
