// Usage: sp_responses_runtime_tests <node> <responses_server.mjs>
#include "runtime/client.h"
#include "codecs/responses_request.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include "support/runtime_peer.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using runtime_test::Peer;
using runtime_test::require;
using sp::runtime::Client;
using sp::runtime::Result;
constexpr std::string_view key = "RESPONSES_SECRET_MARKER_790ec1";
constexpr std::string_view cipher = "RESPONSES_CIPHER_MARKER_274ab2";
std::shared_ptr<const sp::json::Document> document(std::string_view text) {
  return std::make_shared<const sp::json::Document>(runtime_test::parse(text));
}
sp::descriptor::ValidatedDescriptor descriptor(const Peer& peer, bool foreign = false) {
  auto loaded = sp::descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"responses-runtime\",\"family\":\"openai.responses\",\"connection\":{\"base_url\":"
    + sp::json::quote(std::string(foreign ? "http://localhost:" : "http://127.0.0.1:") + std::to_string(peer.port))
    + R"(,"paths":{"buffered":"/v1/responses","streaming":"/v1/responses"}},"bindings":{"model":"model","messages":"input","stream":"stream","max_output_tokens":"max_output_tokens","usage":["usage"]}})");
  require(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(loaded), "Responses descriptor rejected");
  return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}
sp::runtime::Options options() {
  sp::runtime::Options o; o.api_key = key; o.default_timeout = 15s; o.retry_tokens_per_second = 0; return o;
}
sp::responses::Request request(const std::string& model) {
  sp::responses::Request r; r.model = model; r.account_scope = "synthetic-responses-account";
  r.instructions = "Answer briefly"; r.max_output_tokens = 128; r.reasoning = sp::responses::ReasoningOptions{"low", "auto"};
  r.tools.push_back({"lookup", "Find a value", document(R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"],"additionalProperties":false})"), true});
  r.messages.push_back(sp::Message{"", sp::Role::User, {sp::Text{"synthetic question"}}}); return r;
}
sp::runtime::RunOptions run(bool streaming) { sp::runtime::RunOptions o; o.streaming = streaming; return o; }
const sp::Completion& completion(const Result& result) {
  require(result && std::holds_alternative<sp::Completion>(*result), "expected completion"); return std::get<sp::Completion>(*result);
}
const sp::Failure& failure(const Result& result, std::optional<sp::ErrorKind> kind = {}) {
  require(result && std::holds_alternative<sp::Failure>(*result), "expected failure"); const auto& f = std::get<sp::Failure>(*result);
  if (kind) require(f.error.kind == *kind, "unexpected failure kind");
  for (const auto marker : {key, cipher}) require(f.error.safe_message.find(marker) == std::string::npos && f.error.vendor_code.find(marker) == std::string::npos, "private marker leaked in error");
  return f;
}
std::string text(const std::vector<sp::Message>& messages) {
  std::string value; for (const auto& m : messages) for (const auto& p : m.parts) if (auto t = std::get_if<sp::Text>(&p)) value += t->value; return value;
}
void ineligible(const Result& result) {
  const auto& f = failure(result, sp::ErrorKind::ReplayIneligible);
  require(!f.error.attempt.request_may_have_left && f.error.attempt.request_body_bytes == 0, "preflight refusal dispatched bytes");
}
void incomplete_native(const std::vector<sp::Message>& messages) {
  for (const auto& m : messages) require(!m.native || !m.native->complete(), "failed generation obtained complete native authority");
}
struct Capture {
  std::mutex mutex;
  std::condition_variable cv;
  Result result;
  size_t outcomes = 0, terminals = 0;
  bool terminal_envelope = false;
  std::string observed_text;
  sp::runtime::Callbacks callbacks() {
    return {[this](const sp::Event& e) { std::lock_guard lock(mutex);
      if (const auto* delta = std::get_if<sp::PartDelta>(&e);
          delta && delta->payload.kind == sp::PartKind::Text)
        observed_text.append(delta->payload.bytes);
      if (const auto* envelope = std::get_if<sp::ResponseEnvelope>(&e)) {
        const auto status = envelope->payload->root().get("status").as_string();
        terminal_envelope = status == "completed" || status == "incomplete";
      }
      if (std::holds_alternative<sp::Commit>(e) || std::holds_alternative<sp::Fail>(e)) ++terminals;
      cv.notify_all(); },
      [this](Result value) { std::lock_guard lock(mutex); result = std::move(value); ++outcomes; cv.notify_all(); }};
  }
  void terminal_observed() {
    std::unique_lock lock(mutex);
    require(cv.wait_for(lock, 15s, [&] { return terminal_envelope; }), "terminal envelope callback timed out");
  }
  void text_observed(std::string_view expected) {
    std::unique_lock lock(mutex);
    require(cv.wait_for(lock, 15s, [&] { return observed_text == expected; }), "text callback timed out");
  }
  void pending() { std::lock_guard lock(mutex); require(!result && outcomes == 0 && terminals == 0, "committed before observed normal HTTP close"); }
  Result await() {
    std::unique_lock lock(mutex); require(cv.wait_for(lock, 20s, [&] { return !!result; }), "outcome timed out");
    require(outcomes == 1 && terminals == 0, "outcome delivered twice or terminal event exposed"); return result;
  }
};
Result finish(sp::runtime::Operation& operation, Capture& capture) {
  auto result = capture.await(); require(operation.join() == result, "join lost owned outcome"); return result;
}
void check_group(const Result& first) {
  const auto& c = completion(first); require(c.stop.kind == sp::StopKind::ToolUse && c.messages.size() == 1, "generation was not atomic tool group");
  const auto& m = c.messages[0]; require(m.native && m.native->complete() && m.wire_output && m.wire_output->root().size() == 3, "complete group lacks native wire snapshot");
  require(m.parts.size() == 3 && text(c.messages) == "need lookup", "typed output group lost content");
  const auto& r = std::get<sp::Reasoning>(m.parts[0]); require(r.summary == std::vector<std::string>{"Use the lookup result"} && r.encrypted_content == cipher, "summary or cipher not owned");
  const auto& t = std::get<sp::ToolCall>(m.parts[1]); require(t.id == "call_owned" && t.input->root().get("x").as_uint() == 1, "function call ownership lost");
  require(m.wire_output->root().at(1).get("id").as_string() == "fc_item" && m.wire_output->root().at(1).get("call_id").as_string() == "call_owned", "item id was replaced with call_id");
  require(m.wire_output->root().at(2).get("phase").as_string() == "final_answer", "message phase discarded");
  require(c.usage.stage == sp::UsageStage::Final && c.usage.total->value == 17 && c.usage.reasoning->value == 3 && c.usage.input_uncached->value == 6, "inclusive/cache/reasoning usage incorrect");
}
sp::responses::Request continuation(const std::string& model, const Result& result) {
  auto r = request(model); r.messages.push_back(completion(result).messages[0]);
  r.messages.push_back(sp::Message{"", sp::Role::Tool, {sp::ToolResult{"call_owned", "one"}}}); return r;
}
void two_turn_and_refusal(Peer& peer) {
  for (bool streaming : {false, true}) {
    const auto model = peer.arm("grouped"); Client client(descriptor(peer), options());
    auto first = client.complete(request(model), run(streaming)); check_group(first); peer.count(model, 1);
    auto next = continuation(model, first);
    auto reject = [&](sp::responses::Request bad) { ineligible(client.complete(std::move(bad), run(!streaming))); peer.count(model, 1); };
    auto bad = next; std::get<sp::Reasoning>(bad.messages[1].parts[0]).summary[0] = "edited"; reject(std::move(bad));
    bad = next; std::get<sp::Reasoning>(bad.messages[1].parts[0]).encrypted_content = "edited cipher"; reject(std::move(bad));
    bad = next; std::get<sp::ToolCall>(bad.messages[1].parts[1]).id = "foreign call"; reject(std::move(bad));
    bad = next; std::swap(bad.messages[1].parts[0], bad.messages[1].parts[2]); reject(std::move(bad));
    bad = next; bad.messages[1].parts.erase(bad.messages[1].parts.begin()); reject(std::move(bad));
    bad = next; bad.messages[1].native.reset(); reject(std::move(bad));
    bad = next; bad.messages[1].wire_output.reset(); reject(std::move(bad));
    bad = next; auto root = bad.messages[1].wire_output->root();
    bad.messages[1].wire_output = document("[" + root.at(2).dump() + "," + root.at(1).dump() + "," + root.at(0).dump() + "]"); reject(std::move(bad));
    bad = next; root = bad.messages[1].wire_output->root();
    bad.messages[1].wire_output = document("[" + root.at(0).dump() + "," + root.at(2).dump() + "]"); reject(std::move(bad));
    bad = next; bad.model += "-foreign"; reject(std::move(bad));
    bad = next; std::get<sp::Text>(bad.messages[0].parts[0]).value = "edited prefix"; reject(std::move(bad));
    bad = next; bad.messages.insert(bad.messages.begin(), sp::Message{"", sp::Role::User, {sp::Text{"extra prefix"}}}); reject(std::move(bad));
    bad = next; bad.account_scope = "foreign-account"; reject(std::move(bad));
    bad = next; bad.instructions = "changed instructions"; reject(std::move(bad));
    bad = next; bad.reasoning->effort = "medium"; reject(std::move(bad));
    bad = next; bad.tools[0].description = "changed definition"; reject(std::move(bad));
    bad = next; bad.messages.pop_back(); require(std::holds_alternative<sp::Failure>(*client.complete(bad, run(streaming))), "unfulfilled tool accepted"); peer.count(model, 1);
    bad = next; std::get<sp::ToolResult>(bad.messages[2].parts[0]).tool_use_id = "foreign";
    require(std::holds_alternative<sp::Failure>(*client.complete(bad, run(streaming))), "foreign result accepted"); peer.count(model, 1);
    bad = next; bad.messages.push_back(bad.messages.back());
    require(std::holds_alternative<sp::Failure>(*client.complete(bad, run(streaming))), "duplicate result accepted"); peer.count(model, 1);
    Client foreign(descriptor(peer, true), options()); ineligible(foreign.complete(next, run(streaming))); peer.count(model, 1);
    auto second = client.complete(next, run(!streaming)); require(text(completion(second).messages) == "answer one", "continuation answer incorrect");
    peer.count(model, 2); require(peer.stats(model).root().get("replayed").as_uint() == 1, "actual HTTP peer did not verify exact replay");
    check_group(first); // First result remains immutable after another generation and opposite transport mode.
  }
}
void negative_replay_is_only_model_free(Peer& peer) {
  const auto model = peer.arm("replay-rejected"); Client client(descriptor(peer), options());
  auto first = client.complete(request(model), run(false)); check_group(first);
  auto result = client.complete(continuation(model, first), run(true)); const auto& f = failure(result);
  require(f.error.http_status == 400 && f.error.attempt.request_may_have_left, "simulated vendor refusal was hidden");
  peer.count(model, 2, 1); require(peer.stats(model).root().get("replayed").as_uint() == 1, "negative replay did not reach exact-input oracle");
}
void failed_terminal_matrix(Peer& peer) {
  Client client(descriptor(peer), options());
  for (bool streaming : {false, true}) {
    const auto model = peer.arm("short-close"); const auto result = client.complete(request(model), run(streaming));
    const auto& f = failure(result); require(f.error.kind == sp::ErrorKind::Truncated || f.error.kind == sp::ErrorKind::Transport, "abnormal framing accepted");
    incomplete_native(f.partial.messages); require(f.partial.usage.stage != sp::UsageStage::Final, "failed usage reported final"); peer.count(model, 1, 1);
  }
  const auto reset = peer.arm("reset"); failure(client.complete(request(reset), run(true))); peer.count(reset, 1, 1);
  const auto quota = peer.arm("quota"); const auto quota_result = client.complete(request(quota), run(true));
  const auto& q = failure(quota_result, sp::ErrorKind::QuotaExhausted);
  require(q.error.http_status == 429 && q.error.retry_class == sp::RetryClass::Never &&
      q.error.attempt.attempts == 1, "Responses quota error became retryable rate limiting");
  peer.count(quota, 1, 1);
  const auto late = peer.arm("completed-error"); const auto result = client.complete(request(late), run(true));
  const auto& error = failure(result, sp::ErrorKind::RemoteFailure); require(text(error.partial.messages) == "hello", "late error discarded owned output");
  require(error.partial.usage.stage == sp::UsageStage::Partial, "late error usage stayed final"); incomplete_native(error.partial.messages); peer.count(late, 1, 1);
  const auto partial = peer.arm("partial-error"); const auto partial_result = client.complete(request(partial), run(true));
  require(text(failure(partial_result, sp::ErrorKind::RemoteFailure).partial.messages) == "partial owned", "partial error lost deltas"); peer.count(partial, 1, 1);
  const auto truncated = peer.arm("partial"); Capture cut; auto op = client.start(request(truncated), run(true), cut.callbacks());
  peer.wait(truncated, "held"); cut.text_observed("partial owned"); peer.release(truncated);
  const auto cut_result = finish(op, cut); require(text(failure(cut_result).partial.messages) == "partial owned", "truncation lost owned deltas");
  incomplete_native(std::get<sp::Failure>(*cut_result).partial.messages); peer.count(truncated, 1, 1);
}
void named_error_outcomes_and_retry(Peer& peer) {
  Result retained;
  {
    Client client(descriptor(peer), options());
    auto retry = run(true);
    retry.retry = sp::runtime::RetryPolicy{true, true, 2, 0ms, 0ms};
    for (const auto scenario : {"named-empty-malformed", "named-completed-malformed", "named-completed-sentinel",
        "named-partial-typeless", "named-completed-typeless", "named-completed-wrong-type", "named-empty-typeless"}) {
      const std::string_view name = scenario;
      const bool malformed = name.ends_with("-malformed") || name.ends_with("-sentinel");
      const bool empty = name.starts_with("named-empty-");
      const std::string expected = empty ? "" : name.starts_with("named-partial-") ? "partial owned" : "hello";
      const auto model = peer.arm(scenario);
      const auto result = client.complete(request(model), retry);
      const auto& f = failure(result, malformed ? sp::ErrorKind::RemoteFailure : sp::ErrorKind::RateLimited);
      require(text(f.partial.messages) == expected, "named error discarded original output");
      require(f.error.retry_safety == (empty ? sp::RetrySafety::PossiblyAccepted : sp::RetrySafety::OutputObserved),
          "raw error document became generated output or partial output lost retry protection");
      const auto attempts = empty && !malformed ? 2U : 1U;
      require(f.error.attempt.attempts == attempts && f.error.http_status == 200 &&
          f.error.attempt.response_head_seen && f.error.attempt.request_may_have_left,
          "named error lost real HTTP attempt evidence");
      require(f.error.vendor_code == (malformed ? "" : "rate_limit_exceeded"), "named error lost vendor classification");
      if (!malformed) {
        require(f.error.retry_class == sp::RetryClass::AfterReset, "named vendor error lost retry policy");
        require(f.partial.raw_events.back().type == "error" &&
            f.partial.raw_events.back().payload->root().get("error").get("code").as_string() == "rate_limit_exceeded",
            "named error did not retain original valid DOM");
        if (name.ends_with("-wrong-type"))
          require(f.partial.raw_events.back().payload->root().get("type").as_string() == "response.completed",
              "named error rewrote contradictory payload type");
        else require(!f.partial.raw_events.back().payload->root().get("type").valid(), "named error manufactured payload type");
        if (empty) require(f.partial.raw_events.size() == 2 && f.partial.messages.empty() && !f.partial.stop &&
            !f.partial.wire_envelope && f.partial.usage.stage == sp::UsageStage::Missing &&
            f.error.attempt.prior_usage_unknown, "no-output retries dropped prior raw error or invented semantic output");
        retained = result;
      }
      incomplete_native(f.partial.messages);
      peer.count(model, attempts, attempts);
    }
  }
  const auto& raw = failure(retained, sp::ErrorKind::RateLimited).partial.raw_events;
  require(raw[0].payload->root().get("vendor").get("b").at(0).as_uint() == 2 &&
      raw[1].payload->root().get("vendor").get("a").as_bool(), "client destruction invalidated retried raw error DOM");
}
void close_cancel_deadline_and_retention(Peer& peer) {
  Result retained, retained_failure;
  {
    Client client(descriptor(peer), options());
    for (const auto scenario : {"close-gate", "completed-reset"}) {
      const auto model = peer.arm(scenario); Capture capture; auto op = client.start(request(model), run(true), capture.callbacks());
      peer.wait(model, "held"); capture.terminal_observed(); capture.pending(); peer.release(model); auto result = finish(op, capture);
      if (std::string_view(scenario) == "close-gate") { retained = result; require(text(completion(result).messages) == "hello", "normal close lost completion"); peer.count(model, 1); }
      else { const auto& f = failure(result); incomplete_native(f.partial.messages); require(f.partial.usage.stage != sp::UsageStage::Final, "reset after terminal finalized usage"); peer.count(model, 1, 1); }
    }
    const auto cancelled = peer.arm("partial"); Capture cancel; auto op = client.start(request(cancelled), run(true), cancel.callbacks());
    peer.wait(cancelled, "held"); cancel.text_observed("partial owned"); op.cancel(); auto result = finish(op, cancel);
    retained_failure = result;
    const auto& f = failure(result, sp::ErrorKind::Cancelled); require(text(f.partial.messages) == "partial owned", "cancel lost partial ownership"); incomplete_native(f.partial.messages); peer.count(cancelled, 1);
    const auto model = peer.arm("hold"); Capture deadline; auto timed = run(false); timed.deadline = runtime_test::Clock::now() + 5s;
    auto timeout = client.start(request(model), timed, deadline.callbacks()); peer.wait(model, "held");
    failure(finish(timeout, deadline), sp::ErrorKind::DeadlineExceeded); peer.count(model, 1);
  }
  require(text(completion(retained).messages) == "hello", "client destruction invalidated retained completion");
  require(std::get<sp::Reasoning>(completion(retained).messages[0].parts[0]).encrypted_content == cipher, "client destruction invalidated retained cipher");
  const auto& complete_raw = completion(retained).raw_events;
  require(!complete_raw.empty() && complete_raw.back().type == "response.completed" &&
      complete_raw.back().payload->root().get("response").get("output").at(0).get("encrypted_content").as_string() ==
          "TERMINAL_ONLY_REPRESENTATION", "native authority replaced original terminal wire evidence");
  require(text(failure(retained_failure, sp::ErrorKind::Cancelled).partial.messages) == "partial owned", "client destruction invalidated retained failure");
  const auto& partial = failure(retained_failure, sp::ErrorKind::Cancelled).partial;
  require(partial.wire_envelope && partial.wire_envelope->root().get("status").as_string() == "in_progress", "client destruction invalidated retained envelope");
  require(!partial.raw_events.empty() && partial.raw_events.back().type == "response.output_text.delta" &&
      partial.raw_events.back().payload->root().get("delta").as_string() == "partial owned", "cancel lost owned raw text observation");
}
void incomplete_and_opaque(Peer& peer) {
  Client client(descriptor(peer), options());
  for (bool streaming : {false, true}) {
    const auto forced = peer.arm("forced"); auto typed = request(forced); typed.required_tool = "lookup";
    check_group(client.complete(typed, run(streaming))); peer.count(forced, 1);
    const auto model = peer.arm("incomplete"); auto first = client.complete(request(model), run(streaming)); const auto& c = completion(first);
    require(c.stop.kind == sp::StopKind::MaxTokens && std::holds_alternative<sp::InvalidToolCall>(c.messages[0].parts[1]), "incomplete tools were executable or stop fabricated"); incomplete_native(c.messages);
    auto next = request(model); next.messages.push_back(c.messages[0]); ineligible(client.complete(next, run(streaming))); peer.count(model, 1);
    const auto opaque = peer.arm("opaque"); auto result = client.complete(request(opaque), run(streaming));
    const auto& output = completion(result); require(output.stop.kind == sp::StopKind::EndTurn && std::holds_alternative<sp::Opaque>(output.messages[0].parts[0]), "server item turned into host tool");
    for (const auto& m : output.messages) for (const auto& p : m.parts)
      require(!std::holds_alternative<sp::ToolCall>(p), "server-side item is host executable");
    peer.count(opaque, 1);
  }
}
} // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 3, "usage: sp_responses_runtime_tests <node> <responses_server.mjs>");
    runtime_test::LogCapture logs;
    {
      Peer peer(argv[1], argv[2]); two_turn_and_refusal(peer); negative_replay_is_only_model_free(peer);
      failed_terminal_matrix(peer); named_error_outcomes_and_retry(peer); close_cancel_deadline_and_retention(peer); incomplete_and_opaque(peer);
    }
    const auto captured = logs.finish(); require(captured.find(key) == std::string::npos && captured.find(cipher) == std::string::npos, "credential/cipher leaked in diagnostics");
    std::cout << "Responses model-free HTTP contracts passed (not vendor replay validation)\n"; return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
