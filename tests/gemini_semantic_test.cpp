#include "codecs/gemini.h"
#include "codecs/gemini_request.h"
#include "core/native.h"
#include "json/json.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace sp;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #c); } while (false)
std::shared_ptr<const json::Document> owned(std::string_view bytes) {
  auto p = json::parse(bytes); CHECK(std::holds_alternative<json::Document>(p));
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(p)));
}
const descriptor::ValidatedDescriptor& desc() {
  static auto d = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"gemini-semantic","family":"google.generate","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1beta/models/fixture-model:generateContent","streaming":"/v1beta/models/fixture-model:streamGenerateContent?alt=sse"}},"bindings":{"model":"model","messages":"contents","stream":"stream","max_output_tokens":"maxOutputTokens","usage":["usageMetadata"]}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(d)); return std::get<descriptor::ValidatedDescriptor>(d);
}
gemini::Request request() {
  gemini::Request r; r.model = "fixture-model"; r.account_scope = "fixture-account"; r.system = "Answer briefly";
  r.max_output_tokens = 2048; r.thinking_budget = 1024;
  r.tools.push_back({"lookup", "Find value", owned(R"({"type":"object","properties":{"x":{"type":"integer"}}})")});
  r.messages.push_back(Message{"", Role::User, {Text{"question"}}}); return r;
}
std::shared_ptr<const NativeContext> context(bool sse = true) {
  auto e = gemini::encode(desc(), request(), sse); CHECK(std::holds_alternative<gemini::EncodedRequest>(e)); return std::get<gemini::EncodedRequest>(e).context;
}
const std::string counters = R"({"promptTokenCount":10,"cachedContentTokenCount":4,"candidatesTokenCount":7,"thoughtsTokenCount":3,"totalTokenCount":20,"toolUsePromptTokenCount":2})";
const std::string parts = R"([{"text":"Consider","thought":true,"thoughtSignature":"THOUGHT_SIG"},{"text":"need lookup","thoughtSignature":"TEXT_SIG"},{"functionCall":{"id":"call_owned","name":"lookup","args":{"x":1}},"thoughtSignature":"CALL_SIG"},{"thoughtSignature":"EMPTY_SIG"}])";
std::string body(std::string_view p, std::string_view reason = "STOP", std::string_view usage = counters, std::string_view model = "fixture-model", std::string_view id = "generation") {
  return "{\"modelVersion\":" + json::quote(model) + ",\"responseId\":" + json::quote(id) + ",\"candidates\":[{\"index\":0,\"content\":{\"role\":\"model\",\"parts\":" + std::string(p) + "},\"finishReason\":" + json::quote(reason) + "}],\"usageMetadata\":" + std::string(usage) + "}";
}
Outcome buffered(std::string_view wire, gemini::Close close = {}) {
  Accumulator a; gemini::Codec c(desc(), gemini::Mode::Buffered, a, context(false)); c.buffered(wire, close); CHECK(a.outcome()); return *a.outcome();
}
Outcome stream(std::initializer_list<std::string> chunks, gemini::Close close = {}) {
  Accumulator a; gemini::Codec c(desc(), gemini::Mode::Sse, a, context());
  for (const auto& b : chunks) if (!c.frame({}, b)) break;
  c.finish(close); CHECK(a.outcome()); return *a.outcome();
}
const Completion& complete(const Outcome& o) { CHECK(std::holds_alternative<Completion>(o)); return std::get<Completion>(o); }
const Failure& failure(const Outcome& o, ErrorKind kind) { CHECK(std::holds_alternative<Failure>(o)); const auto& f = std::get<Failure>(o); CHECK(f.error.kind == kind); return f; }
void group(const Completion& c) {
  CHECK(c.stop.kind == StopKind::ToolUse && c.messages.size() == 1);
  const auto& m = c.messages[0]; CHECK(m.native && m.native->complete() && m.wire_output);
  CHECK(json::equal(m.wire_output->root(), owned(parts)->root()));
  CHECK(m.parts.size() == 4);
  CHECK(std::get<Thinking>(m.parts[0]).text == "Consider" && std::get<Thinking>(m.parts[0]).signature == "THOUGHT_SIG");
  CHECK(std::get<Text>(m.parts[1]).value == "need lookup");
  const auto& call = std::get<ToolCall>(m.parts[2]);
  CHECK(call.id == "call_owned" && call.name == "lookup" && call.kind == ToolCallKind::ClientExecuted);
  CHECK(call.input->root().get("x").as_uint() == 1);
  CHECK(call.wire_metadata->root().get("thoughtSignature").as_string() == "CALL_SIG");
  CHECK(std::get<Opaque>(m.parts[3]).wire_metadata->root().get("thoughtSignature").as_string() == "EMPTY_SIG");
  CHECK(c.usage.stage == UsageStage::Final && c.usage.input_total->value == 10 && c.usage.input_uncached->value == 6);
  CHECK(c.usage.output_total->value == 10 && c.usage.reasoning->value == 3 && c.usage.total->value == 20);
  CHECK(c.usage.provider_reported_total->value == 20 && c.usage.extra.at("toolUsePromptTokenCount").value == 2);
}
void native_group_and_replay() {
  auto b = buffered(body(parts)); auto s = stream({body(parts)}); group(complete(b)); group(complete(s));
  auto first_owner = owned(parts); auto first = first_owner->root();
  auto split = stream({body("[" + first.at(0).dump() + "]", "", "null"), body("[" + first.at(1).dump() + "," + first.at(2).dump() + "," + first.at(3).dump() + "]")});
  group(complete(split));
  for (auto* o : {&b, &s, &split}) {
    auto r = request(); r.required_tool = "lookup";
    r.messages.push_back(complete(*o).messages[0]); r.messages.push_back(Message{"", Role::Tool, {ToolResult{"call_owned", R"({"result":1})"}}});
    auto encoded = gemini::encode(desc(), r, o == &b);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
    auto wire = owned(std::get<gemini::EncodedRequest>(encoded).body);
    CHECK(json::equal(wire->root().get("contents").at(1).get("parts"), owned(parts)->root()));
    CHECK(wire->root().get("contents").at(2).get("parts").at(0).get("functionResponse").get("id").as_string() == "call_owned");
    auto reject = [&](gemini::Request bad) { auto e = gemini::encode(desc(), bad, true); CHECK(std::holds_alternative<Error>(e) && std::get<Error>(e).kind == ErrorKind::ReplayIneligible); };
    auto bad = r; std::get<Text>(bad.messages[0].parts[0]).value = "edited prefix"; reject(bad);
    bad = r; std::get<Thinking>(bad.messages[1].parts[0]).signature = "edited"; reject(bad);
    bad = r; std::swap(bad.messages[1].parts[0], bad.messages[1].parts[1]); reject(bad);
    bad = r; bad.messages[1].native.reset(); reject(bad);
    bad = r; bad.messages[1].wire_output = owned("[]"); reject(bad);
    bad = r; bad.messages[1].parts.pop_back(); reject(bad);
    bad = r; bad.account_scope = "foreign"; reject(bad);
    bad = r; bad.system += "edit"; reject(bad);
    bad = r; bad.thinking_budget = 0; reject(bad);
    bad = r; bad.tools[0].description += "edit"; reject(bad);
    bad = r; bad.messages.pop_back(); CHECK(std::holds_alternative<Error>(gemini::encode(desc(), bad, false)));
    bad = r; std::get<ToolResult>(bad.messages[2].parts[0]).tool_use_id = "foreign"; CHECK(std::holds_alternative<Error>(gemini::encode(desc(), bad, false)));
  }
}
void nullable_usage() {
  auto o = buffered(body(R"([{"text":"hello"}])", "STOP", R"({"promptTokenCount":10,"candidatesTokenCount":4,"thoughtsTokenCount":null,"totalTokenCount":17})"));
  const auto& c = complete(o); CHECK(c.usage.input_total->value == 10 && !c.usage.output_total && !c.usage.total && !c.usage.reasoning && !c.usage.input_uncached);
  CHECK(c.usage.provider_reported_total->value == 17);
  o = buffered(body("[]", "STOP", "null")); CHECK(complete(o).usage.stage == UsageStage::Missing);
  o = buffered(body("[]", "STOP", R"({"promptTokenCount":10,"cachedContentTokenCount":11,"candidatesTokenCount":4,"thoughtsTokenCount":3,"totalTokenCount":99})"));
  CHECK(complete(o).usage.quality == UsageQuality::Inconsistent && !complete(o).usage.input_uncached && complete(o).usage.total->value == 17 && complete(o).usage.provider_reported_total->value == 99);
  o = buffered(body("[]", "STOP", R"({"candidatesTokenCount":18446744073709551615,"thoughtsTokenCount":1})")); failure(o, ErrorKind::ProtocolCorrupt);
}
void invalid_model_calls() {
  for (auto args : {"[]", "null", "1", R"({"x":1,"x":2})"}) {
    auto o = buffered(body("[{\"functionCall\":{\"id\":\"owned\",\"name\":\"lookup\",\"args\":" + std::string(args) + "}}]"));
    const auto& p = complete(o).messages[0].parts[0]; CHECK(std::holds_alternative<InvalidToolCall>(p));
    CHECK(std::get<InvalidToolCall>(p).raw_fragment == args);
    CHECK(!complete(o).messages[0].native || !complete(o).messages[0].native->complete());
  }
  const auto call = R"([{"functionCall":{"id":"owned","name":"lookup","args":{"x":1}}}])";
  for (auto stop : {"MAX_TOKENS", "MALFORMED_FUNCTION_CALL", "OTHER"}) {
    auto o = stream({body(call, "", "null"), body("[]", stop)});
    CHECK(std::holds_alternative<InvalidToolCall>(complete(o).messages[0].parts[0]));
    CHECK(!complete(o).messages[0].native || !complete(o).messages[0].native->complete());
  }
  auto no_id = buffered(body(R"([{"functionCall":{"name":"lookup","args":{}}}])"));
  auto r = request(); auto m = complete(no_id).messages[0]; auto id = std::get<ToolCall>(m.parts[0]).id;
  r.messages.push_back(m); r.messages.push_back(Message{"", Role::Tool, {ToolResult{id, "{}"}}});
  auto e = gemini::encode(desc(), r, true); CHECK(std::holds_alternative<gemini::EncodedRequest>(e));
  auto wire = owned(std::get<gemini::EncodedRequest>(e).body);
  CHECK(!wire->root().get("contents").at(1).get("parts").at(0).get("functionCall").get("id").valid());
  CHECK(!wire->root().get("contents").at(2).get("parts").at(0).get("functionResponse").get("id").valid());
}
void successive_unidentified_calls() {
  const auto call = R"([{"functionCall":{"name":"lookup","args":{"x":1}}}])";
  auto first = buffered(body(call));
  const auto& first_message = complete(first).messages[0];
  const auto first_id = std::get<ToolCall>(first_message.parts[0]).id;
  auto next = request();
  next.messages.push_back(first_message);
  next.messages.push_back(Message{"", Role::User, {ToolResult{first_id, R"({"result":1})"}}});
  auto encoded = gemini::encode(desc(), next, true);
  CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  Accumulator a;
  gemini::Codec c(desc(), gemini::Mode::Sse, a, std::get<gemini::EncodedRequest>(encoded).context);
  CHECK(c.frame({}, body(call))); c.finish(); CHECK(a.outcome());
  const auto& second_message = complete(*a.outcome()).messages[0];
  const auto second_id = std::get<ToolCall>(second_message.parts[0]).id;
  CHECK(first_id != second_id);
  next.messages.push_back(second_message);
  next.messages.push_back(Message{"", Role::User, {ToolResult{second_id, R"({"result":2})"}}});
  encoded = gemini::encode(desc(), next, false);
  CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  const auto wire = owned(std::get<gemini::EncodedRequest>(encoded).body);
  const auto contents = wire->root().get("contents");
  CHECK(contents.size() == 5);
  for (auto index : {1U, 3U})
    CHECK(!contents.at(index).get("parts").at(0).get("functionCall").get("id").valid());
  for (auto index : {2U, 4U})
    CHECK(!contents.at(index).get("parts").at(0).get("functionResponse").get("id").valid());
  CHECK(contents.at(2).get("parts").at(0).get("functionResponse").get("response").get("result").as_uint() == 1);
  CHECK(contents.at(4).get("parts").at(0).get("functionResponse").get("response").get("result").as_uint() == 2);
}
void terminal_with_omitted_parts() {
  const auto final = R"({"modelVersion":"fixture-model","responseId":"generation","candidates":[{"index":0,"content":{"role":"model"},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":4,"thoughtsTokenCount":0,"totalTokenCount":14}})";
  const auto prefix = body(R"([{"text":"answer"}])", "", "null");
  const auto outcome = stream({prefix, final});
  CHECK(std::get<Text>(complete(outcome).messages[0].parts[0]).value == "answer");
  CHECK(complete(outcome).usage.output_total->value == 4 && complete(outcome).messages[0].native->complete());
  failure(stream({prefix, final}, {false, ErrorKind::Transport}), ErrorKind::Transport);
}
void errors_and_terminals() {
  const auto prefix = body(R"([{"text":"owned partial"}])", "", "null");
  auto o = stream({prefix}); CHECK(std::get<Text>(failure(o, ErrorKind::Truncated).partial.messages[0].parts[0]).value == "owned partial");
  o = stream({body(parts), R"({"error":{"message":"PRIVATE_SIG"}})"});
  const auto& f = failure(o, ErrorKind::RemoteFailure); CHECK(f.partial.messages.size() == 1 && f.partial.usage.stage != UsageStage::Final && f.error.safe_message.find("PRIVATE_SIG") == std::string::npos);
  o = stream({body(parts)}, {false, ErrorKind::Cancelled}); failure(o, ErrorKind::Cancelled);
  o = buffered(body(parts), {false, ErrorKind::Transport}); failure(o, ErrorKind::Transport);
  o = stream({prefix, body("[]", "STOP", "null", "foreign-model")}); failure(o, ErrorKind::ProtocolCorrupt);
  o = stream({prefix, body("[]", "STOP", "null", "fixture-model", "foreign-generation")}); failure(o, ErrorKind::ProtocolCorrupt);
  o = stream({body(parts), body("[]")}); failure(o, ErrorKind::ProtocolCorrupt);
  o = stream({body(parts), "[DONE]"}); failure(o, ErrorKind::ProtocolCorrupt);
  o = buffered(R"({"promptFeedback":{"blockReason":"SAFETY"}})"); failure(o, ErrorKind::RemoteFailure);
  o = buffered(body("[]", "SAFETY")); failure(o, ErrorKind::RemoteFailure);
  for (auto p : {R"([{"text":"x","functionCall":{"name":"lookup"}}])", R"([{"thoughtSignature":2}])", R"([{"functionCall":{"id":"","name":"lookup"}}])"}) { o = buffered(body(p)); failure(o, ErrorKind::ProtocolCorrupt); }
  o = buffered(body(R"([{"functionCall":{"name":"unknown","args":{}}}])")); failure(o, ErrorKind::Unsupported);
  o = buffered(body(R"([{"futureTool":{}}])")); failure(o, ErrorKind::Unsupported);
  o = buffered(R"({"candidates":[{"index":1,"content":{"role":"model","parts":[]},"finishReason":"STOP"}]})"); failure(o, ErrorKind::ProtocolCorrupt);
  o = buffered(R"({"candidates":[{"index":0,"finishReason":"STOP"},{"index":0,"finishReason":"STOP"}]})"); failure(o, ErrorKind::ProtocolCorrupt);
  o = buffered(R"({"candidates":[{"index":0,"index":0,"finishReason":"STOP"}]})"); failure(o, ErrorKind::ProtocolCorrupt);
  o = buffered(body(R"([{"toolCall":{"id":"server","toolType":"GOOGLE_SEARCH","args":{}}},{"codeExecutionResult":{"outcome":"OUTCOME_OK","output":"one"}}])"));
  for (const auto& p : complete(o).messages[0].parts) CHECK(std::holds_alternative<Opaque>(p));
}
}
int main() {
  try { terminal_with_omitted_parts(); successive_unidentified_calls(); native_group_and_replay(); nullable_usage(); invalid_model_calls(); errors_and_terminals(); std::cout << "Gemini semantic invariants passed\n"; }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
