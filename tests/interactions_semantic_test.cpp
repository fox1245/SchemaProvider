#include "codecs/interactions.h"
#include "codecs/interactions_request.h"
#include "core/native.h"
#include "json/json.h"
#include "support/vision_fixture.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace sp;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)
using Frame = std::pair<std::string, std::string>;
json::Document parse(std::string_view bytes) { auto p = json::parse(bytes); CHECK(std::holds_alternative<json::Document>(p)); return std::get<json::Document>(std::move(p)); }
std::shared_ptr<const json::Document> own(std::string_view bytes) { return std::make_shared<const json::Document>(parse(bytes)); }
const descriptor::ValidatedDescriptor& desc() {
  static auto d = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"interactions-test","family":"google.interactions","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1beta/interactions","streaming":"/v1beta/interactions"}},"bindings":{"model":"model","messages":"input","stream":"stream","max_output_tokens":"max_output_tokens","usage":["usage"]}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(d)); return std::get<descriptor::ValidatedDescriptor>(d);
}
interactions::Request request() {
  interactions::Request r; r.model = "fixture-model"; r.account_scope = "fixture"; r.system = "Brief";
  r.thinking_level = "low"; r.max_output_tokens = 128;
  r.tools.push_back({"lookup", "Find", own(R"({"type":"object","properties":{"x":{"type":"integer"}}})")});
  r.messages.push_back(Message{"", Role::User, {Text{"before"}, vision_test::scene_a().image(), Text{"after"}, vision_test::scene_b().image()}});
  return r;
}
std::shared_ptr<const NativeContext> context() { auto e = interactions::encode(desc(), request(), true); CHECK(std::holds_alternative<interactions::EncodedRequest>(e)); return std::get<interactions::EncodedRequest>(e).context; }
const std::string thought = R"({"type":"thought","signature":"FINAL_OPAQUE","summary":[{"type":"text","text":"first"},{"type":"text","text":"second"}],"extra":"retain"})";
const std::string call = R"({"type":"function_call","id":"call1","name":"lookup","arguments":{"x":1}})";
const std::string output = R"({"type":"model_output","content":[{"type":"text","text":"answer","annotations":[]}],"extra":17})";
const std::string counters = R"({"total_input_tokens":100,"total_output_tokens":25,"total_thought_tokens":22,"total_tokens":147,"total_tool_use_tokens":50,"total_cached_tokens":4,"future_tokens":6})";
std::string resource(std::string_view steps, std::string_view status = "completed", std::string_view usage = counters) {
  return "{\"id\":\"i1\",\"model\":\"fixture-model\",\"status\":" + json::quote(status) + ",\"steps\":" + std::string(steps) + ",\"usage\":" + std::string(usage) + "}";
}
Frame frame(std::string name, std::string fields) { return {name, "{\"event_type\":" + json::quote(name) + fields + "}"}; }
Frame created() { return frame("interaction.created", R"(,"interaction":{"id":"i1","status":"in_progress","model":"fixture-model"})"); }
Frame start(unsigned i, std::string_view step) { return frame("step.start", ",\"index\":" + std::to_string(i) + ",\"step\":" + std::string(step)); }
Frame delta(unsigned i, std::string_view value) { return frame("step.delta", ",\"index\":" + std::to_string(i) + ",\"delta\":" + std::string(value)); }
Frame stop(unsigned i) { return frame("step.stop", ",\"index\":" + std::to_string(i)); }
Frame end(std::string_view steps, std::string_view status = "completed", std::string_view usage = counters) { return frame("interaction.completed", ",\"interaction\":" + resource(steps, status, usage)); }
std::vector<Frame> frames(bool tool = false) {
  std::vector<Frame> f{created(), start(0, R"({"type":"thought","signature":"","summary":[],"extra":"retain"})"),
    start(1, tool ? R"({"type":"function_call","id":"call1","name":"lookup","arguments":{}})" : R"({"type":"model_output","content":[],"extra":17})"),
    delta(0, R"({"type":"thought_summary","content":{"type":"text","text":"first"}})"),
    delta(1, tool ? R"({"type":"arguments_delta","arguments":"{\"x\": 1}"})" : R"({"type":"text","text":"ans"})"),
    delta(0, R"({"type":"thought_summary","content":{"type":"text","text":"second"}})"),
    delta(0, R"({"type":"thought_signature","signature":"STREAM_OPAQUE"})"), stop(0)};
  if (!tool) f.push_back(delta(1, R"({"type":"text","text":"wer"})"));
  f.push_back(stop(1)); f.push_back(end("[" + thought + "," + (tool ? call : output) + "]", tool ? "requires_action" : "completed"));
  f.push_back({"done", "[DONE]"}); return f;
}
Outcome buffered(std::string_view bytes, interactions::Close close = {}, SemanticLimits limits = {}) {
  Accumulator a(limits); interactions::Codec c(desc(), interactions::Mode::Buffered, a, context(), limits);
  c.buffered(bytes, close); CHECK(a.outcome()); return *a.outcome();
}
Outcome stream(const std::vector<Frame>& f, interactions::Close close = {}, SemanticLimits limits = {}) {
  Accumulator a(limits); interactions::Codec c(desc(), interactions::Mode::Sse, a, context(), limits);
  for (const auto& [event, data] : f) if (!c.frame(event, data)) break;
  c.finish(close); CHECK(a.outcome()); return *a.outcome();
}
const Completion& complete(const Outcome& o) { CHECK(std::holds_alternative<Completion>(o)); return std::get<Completion>(o); }
const Failure& failure(const Outcome& o, ErrorKind kind) { CHECK(std::holds_alternative<Failure>(o)); CHECK(std::get<Failure>(o).error.kind == kind); return std::get<Failure>(o); }
void exact_steps_and_replay() {
  for (auto o : {buffered(resource("[" + thought + "," + call + "]", "requires_action")), stream(frames(true))}) {
    const auto& c = complete(o); CHECK(c.stop.kind == StopKind::ToolUse && c.messages.size() == 1);
    const auto& m = c.messages[0]; CHECK(m.native && m.native->complete());
    CHECK(json::equal(m.wire_output->root(), parse("[" + thought + "," + call + "]").root()));
    const auto& t = std::get<Thought>(m.parts[0]); CHECK(t.summary == std::vector<std::string>({"first", "second"}) && t.signature == "FINAL_OPAQUE");
    const auto& fn = std::get<ToolCall>(m.parts[1]); CHECK(fn.id == "call1" && fn.name == "lookup" && fn.kind == ToolCallKind::ClientExecuted && fn.input->root().get("x").as_uint() == 1);
    CHECK(c.usage.output_total->value == 47 && c.usage.reasoning->value == 22 && c.usage.total->value == 147);
    CHECK(c.usage.provider_reported_total->value == 147 && c.usage.extra.at("total_tool_use_tokens").value == 50 && c.usage.extra.at("future_tokens").value == 6);
    CHECK(c.usage.input_uncached->value == 96 && c.usage.output_total->evidence == Evidence::Derived && c.usage.quality == UsageQuality::Consistent);
    auto next = request(); next.messages.push_back(m); next.messages.push_back(Message{"", Role::Tool, {ToolResult{"call1", "one"}}});
    auto encoded = interactions::encode(desc(), next, false); CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
    auto wire = parse(std::get<interactions::EncodedRequest>(encoded).body); auto input = wire.root().get("input");
    CHECK(json::equal(input.at(1), m.wire_output->root().at(0)) && json::equal(input.at(2), m.wire_output->root().at(1)));
    CHECK(input.at(3).get("call_id").as_string() == "call1" && input.at(3).get("name").as_string() == "lookup" && input.at(3).get("result").as_string() == "one");
    auto reject = [&](interactions::Request bad) { auto e = interactions::encode(desc(), bad, false); CHECK(std::holds_alternative<Error>(e) && std::get<Error>(e).kind == ErrorKind::ReplayIneligible); };
    auto bad = next; std::get<Thought>(bad.messages[1].parts[0]).signature = "edited"; reject(bad);
    bad = next; std::get<Thought>(bad.messages[1].parts[0]).summary[0] = "edited"; reject(bad);
    bad = next; std::get<Image>(bad.messages[0].parts[1]) = vision_test::scene_b().image(); reject(bad);
    bad = next; bad.messages[1].native.reset(); reject(bad);
    bad = next; bad.messages[1].wire_output.reset(); reject(bad);
    bad = next; std::swap(bad.messages[1].parts[0], bad.messages[1].parts[1]); reject(bad);
    bad = next; bad.thinking_level = "high"; reject(bad);
    bad = next; bad.account_scope = "foreign"; reject(bad);
  }
  auto o = stream(frames()); CHECK(std::get<Text>(complete(o).messages[0].parts[1]).value == "answer");
}
void input_boundaries() {
  auto r = request(); auto e = interactions::encode(desc(), r, false); CHECK(std::holds_alternative<interactions::EncodedRequest>(e));
  auto d = parse(std::get<interactions::EncodedRequest>(e).body); auto content = d.root().get("input").at(0).get("content");
  CHECK(content.at(0).get("text").as_string() == "before" && content.at(1).get("data").as_string() == *vision_test::scene_a().base64);
  CHECK(content.at(2).get("text").as_string() == "after" && content.at(3).get("data").as_string() == *vision_test::scene_b().base64);
  r.thinking_level = "none"; CHECK(std::holds_alternative<Error>(interactions::encode(desc(), r, false)));
  r = request(); std::get<Image>(r.messages[0].parts[1]).data = std::make_shared<const std::string>("not base64"); CHECK(std::holds_alternative<Error>(interactions::encode(desc(), r, false)));
  r = request(); r.required_tool = "missing"; CHECK(std::holds_alternative<Error>(interactions::encode(desc(), r, false)));
  r = request(); r.messages.push_back(Message{"", Role::Tool, {ToolResult{"orphan", "one"}}}); CHECK(std::holds_alternative<Error>(interactions::encode(desc(), r, false)));
}
void lifecycle_and_errors() {
  for (auto [status, kind] : std::vector<std::pair<std::string, ErrorKind>>{{"failed", ErrorKind::RemoteFailure}, {"cancelled", ErrorKind::Cancelled}, {"in_progress", ErrorKind::Unsupported}, {"queued", ErrorKind::Unsupported}}) {
    failure(buffered(resource("[" + output + "]", status)), kind);
    failure(stream({created(), start(0, output), stop(0), end("[" + output + "]", status)}), kind);
  }
  auto incomplete = buffered(resource("[" + thought + "," + output + "]", "incomplete"));
  CHECK(complete(incomplete).stop.kind == StopKind::MaxTokens && !complete(incomplete).messages[0].native->complete());
  failure(stream({created(), frame("interaction.status_update", R"(,"interaction_id":"i1","status":"completed")")}), ErrorKind::Truncated);
  failure(stream({created(), frame("interaction.status_update", R"(,"interaction_id":"i1","status":"failed")")}), ErrorKind::RemoteFailure);
  failure(buffered(resource("[{\"type\":\"google_search_call\"}]")), ErrorKind::Unsupported);
  failure(buffered(resource("[{\"type\":\"function_call\",\"id\":\"x\",\"name\":\"server\",\"arguments\":{}}]", "requires_action")), ErrorKind::Unsupported);
  failure(stream({created(), frame("future.event", "")}), ErrorKind::Unsupported);
  failure(stream({created(), delta(0, R"({"type":"text","text":"x"})")}), ErrorKind::ProtocolCorrupt);
  auto f = frames(); f.insert(f.end() - 2, frame("error", R"(,"error":{"message":"secret"})")); failure(stream(f), ErrorKind::RemoteFailure);
  f = {created(), start(0, R"({"type":"thought","signature":""})"), delta(0, R"({"type":"thought_signature","signature":"one"})"), delta(0, R"({"type":"thought_signature","signature":"two"})")}; failure(stream(f), ErrorKind::ProtocolCorrupt);
  for (auto close : {interactions::Close{false, ErrorKind::Truncated}, interactions::Close{false, ErrorKind::Cancelled}, interactions::Close{false, ErrorKind::DeadlineExceeded}}) {
    auto failed = stream(frames(), close); const auto& partial = failure(failed, close.error).partial;
    CHECK(!partial.messages[0].native->complete() && partial.usage.stage != UsageStage::Final);
  }
  f = frames(); for (size_t n = 0; n + 2 < f.size(); ++n) failure(stream(std::vector<Frame>(f.begin(), f.begin() + n)), ErrorKind::Truncated);
  auto limited = SemanticLimits{}; limited.max_parts = 1; failure(stream(frames(), {}, limited), ErrorKind::ResourceLimit);
}
void unknown_usage_and_signature() {
  for (auto usage : {R"({"total_input_tokens":10,"total_output_tokens":5,"total_thought_tokens":null,"total_tokens":15})", R"({"total_input_tokens":10,"total_output_tokens":5,"total_tokens":15})"}) {
    auto o = buffered(resource("[{\"type\":\"thought\"}," + output + "]", "completed", usage)); const auto& c = complete(o);
    CHECK(!c.usage.output_total && !c.usage.reasoning && c.usage.total->evidence == Evidence::Reported);
    const auto& t = std::get<Thought>(c.messages[0].parts[0]); CHECK(t.summary.empty() && !t.signature);
  }
  auto o = buffered(resource("[" + output + "]", "completed", R"({"total_input_tokens":10,"total_output_tokens":5,"total_thought_tokens":2,"total_tokens":15,"total_tool_use_tokens":4})"));
  CHECK(complete(o).usage.total->value == 17 && complete(o).usage.provider_reported_total->value == 15 && complete(o).usage.quality == UsageQuality::Inconsistent);
  failure(buffered(resource("[" + output + "]", "completed", R"({"total_input_tokens":-1})")), ErrorKind::ProtocolCorrupt);
  auto no_usage = buffered(resource("[" + output + "]", "completed", "null")); CHECK(complete(no_usage).usage.stage == UsageStage::Missing);
}
void missing_terminal_usage() {
  for (const bool null_usage : {false, true}) {
    auto change = delta(0, R"({"type":"text","text":"answer"})");
    auto closed = stop(0);
    if (null_usage) change = frame("step.delta", ",\"index\":0,\"delta\":{\"type\":\"text\",\"text\":\"answer\"},\"metadata\":{\"total_usage\":" + counters + "}");
    else closed = frame("step.stop", ",\"index\":0,\"usage\":" + counters);
    auto terminal = frame("interaction.completed", ",\"interaction\":{\"id\":\"i1\",\"status\":\"completed\"" +
        std::string(null_usage ? ",\"usage\":null" : "") + "}");
    const auto outcome = stream({created(), start(0, R"({"type":"model_output","content":[]})"), change, closed, terminal});
    const auto& done = complete(outcome);
    CHECK(std::get<Text>(done.messages[0].parts[0]).value == "answer");
    CHECK(done.messages[0].native && done.messages[0].native->complete());
    CHECK(done.usage.stage == UsageStage::Missing && !done.usage.input_total && !done.usage.output_total &&
        !done.usage.reasoning && !done.usage.provider_reported_total);
  }
}
void stateless_missing_resource_id() {
  const auto value = "{\"model\":\"fixture-model\",\"status\":\"completed\",\"steps\":[" + output + "],\"usage\":" + counters + "}";
  const auto buffered_outcome = buffered(value);
  const auto& done = complete(buffered_outcome);
  CHECK(done.messages[0].id.empty() && done.messages[0].native && done.messages[0].native->complete());
  CHECK(std::get<Text>(done.messages[0].parts[0]).value == "answer");
  const auto streamed = stream({
      frame("interaction.created", R"(,"interaction":{"model":"fixture-model","status":"in_progress"})"),
      start(0, R"({"type":"model_output","content":[],"extra":17})"),
      delta(0, R"({"type":"text","text":"answer"})"), stop(0),
      frame("interaction.completed", ",\"interaction\":" + value)});
  CHECK(complete(streamed).messages[0].id.empty() && complete(streamed).messages[0].native->complete());
  const auto stateless = stream({
      frame("interaction.created", R"(,"interaction":{"id":"","model":"fixture-model","status":"in_progress"})"),
      frame("interaction.status_update", R"(,"interaction_id":"","status":"in_progress")"),
      start(0, R"({"type":"model_output","content":[],"extra":17})"),
      delta(0, R"({"type":"text","text":"answer"})"), stop(0),
      frame("interaction.completed", ",\"interaction\":{\"id\":\"\",\"model\":\"fixture-model\",\"status\":\"completed\",\"steps\":[" + output + "],\"usage\":" + counters + "}")});
  CHECK(complete(stateless).messages[0].id.empty() && complete(stateless).messages[0].native->complete());
}
}
int main() {
  try { stateless_missing_resource_id(); missing_terminal_usage(); exact_steps_and_replay(); input_boundaries(); lifecycle_and_errors(); unknown_usage_and_signature();
    std::cout << "Interactions semantic contracts passed\n"; return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
