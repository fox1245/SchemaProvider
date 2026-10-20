#include "codecs/gemini.h"
#include "codecs/gemini_request.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>
#include <iostream>
#include <set>
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
// A call or result the service sent without an id is named "gemini-local-<token>-<part>". The token is random and
// drawn per response, so a conversation saved by one process and resumed by another cannot hold one id twice
// (native_archive_test resumes across processes). The ids of one response share a token.
void local_ids_carry_a_random_token_per_response() {
  const auto wire = body(R"re([{"functionCall":{"name":"lookup","args":{"x":1}}},{"executableCode":{"language":"PYTHON","code":"print(1)"}},)re"
                         R"re({"codeExecutionResult":{"outcome":"OUTCOME_OK","output":"1"}},{"codeExecutionResult":{"outcome":"OUTCOME_OK","output":"2"}}])re");
  const auto token_of = [](const std::string& id, std::string_view part) {
    constexpr std::string_view prefix = "gemini-local-";
    CHECK(id.starts_with(prefix) && id.size() == prefix.size() + 16 + 1 + part.size() && id.ends_with("-" + std::string(part)));
    const auto token = id.substr(prefix.size(), 16);
    CHECK(std::all_of(token.begin(), token.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    return token;
  };
  std::set<std::string> tokens;
  for (int response = 0; response < 8; ++response) {
    const auto outcome = buffered(wire);
    const auto& p = complete(outcome).messages[0].parts;
    CHECK(p.size() == 4);
    const auto token = token_of(std::get<ToolCall>(p[0]).id, "0");
    CHECK(token_of(std::get<ToolCall>(p[1]).id, "1") == token && token_of(std::get<ServerToolResult>(p[3]).tool_use_id, "3") == token);
    tokens.insert(token);
  }
  CHECK(tokens.size() == 8);
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
    auto portable = r; portable.history_mode = gemini::HistoryMode::PortableForeign;
    const auto unchanged = gemini::encode(desc(), portable, o == &b);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(unchanged));
    CHECK(std::get<gemini::EncodedRequest>(unchanged).body == std::get<gemini::EncodedRequest>(encoded).body);
    auto compromised = portable; std::get<Thinking>(compromised.messages[1].parts[0]).signature = "changed";
    const auto denied = gemini::encode(desc(), compromised, true);
    CHECK(std::holds_alternative<Error>(denied) && std::get<Error>(denied).kind == ErrorKind::ReplayIneligible);
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
    bad = r; bad.include_thoughts = false; reject(bad);
    bad = r; bad.max_output_tokens = 2049;
    const auto larger = gemini::encode(desc(), bad, true);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(larger));
    CHECK(std::get<gemini::EncodedRequest>(larger).max_output_tokens == 2049);
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
// Generate has no cache-write counter: input_uncached = promptTokenCount - cachedContentTokenCount
// (Evidence::Derived) when the cached count is reported, and unknown when it is omitted.
void uncached_input_rule() {
  for (const auto& o : {buffered(body(R"([{"text":"hello"}])", "STOP", counters)), stream({body(R"([{"text":"hello"}])", "STOP", counters)})}) {
    const auto& u = complete(o).usage;
    CHECK(u.input_total->value == 10 && u.cache_read->value == 4 && !u.cache_write);
    CHECK(u.input_uncached->value == 6 && u.input_uncached->evidence == Evidence::Derived);
  }
  // Live gemini-2.5-flash-lite shape without cachedContentTokenCount.
  auto o = buffered(body(R"([{"text":"hello"}])", "STOP", R"({"promptTokenCount":21,"candidatesTokenCount":1,"totalTokenCount":22,"promptTokensDetails":[{"modality":"TEXT","tokenCount":21}],"serviceTier":"standard"})"));
  CHECK(complete(o).usage.input_total->value == 21 && !complete(o).usage.cache_read && !complete(o).usage.input_uncached);
}
StopKind policy_kind(descriptor::StopKind kind) {
  switch (kind) {
    case descriptor::StopKind::EndTurn: return StopKind::EndTurn;
    case descriptor::StopKind::MaxTokens: return StopKind::MaxTokens;
    case descriptor::StopKind::ToolUse: return StopKind::ToolUse;
    case descriptor::StopKind::ContentFilter: return StopKind::ContentFilter;
    case descriptor::StopKind::Refusal: return StopKind::Refusal;
    case descriptor::StopKind::PauseTurn: return StopKind::PauseTurn;
    case descriptor::StopKind::ContextLimit: return StopKind::ContextLimit;
    case descriptor::StopKind::StopSequence: return StopKind::StopSequence;
    case descriptor::StopKind::MalformedCall: return StopKind::MalformedCall;
    case descriptor::StopKind::Compaction: return StopKind::Compaction;
    case descriptor::StopKind::Unknown: return StopKind::Unknown;
  }
  return StopKind::Unknown;
}
// The codec maps finishReason with a compiled table; the google.generate policy stop_reasons table
// restates it (STOP is listed with its call-free meaning) and must not drift from it.
void stop_kinds_match_policy_table() {
  const auto stop_of = [](std::string_view reason, std::string_view parts = R"([{"text":"hello"}])") {
    const auto o = buffered(body(parts, reason));
    if (const auto* c = std::get_if<Completion>(&o)) return c->stop.kind;
    const auto& f = std::get<Failure>(o);
    CHECK(f.error.kind == ErrorKind::RemoteFailure && f.partial.stop);
    return f.partial.stop->kind;
  };
  const std::pair<std::string_view, StopKind> expected[]{
      {"STOP", StopKind::EndTurn}, {"MAX_TOKENS", StopKind::MaxTokens}, {"SAFETY", StopKind::ContentFilter},
      {"IMAGE_SAFETY", StopKind::ContentFilter}, {"MALFORMED_FUNCTION_CALL", StopKind::MalformedCall}, {"OTHER", StopKind::Unknown}};
  for (const auto& [reason, kind] : expected) CHECK(stop_of(reason) == kind);
  CHECK(stop_of("STOP", R"([{"functionCall":{"id":"owned","name":"lookup","args":{"x":1}}}])") == StopKind::ToolUse);
  const std::string_view reasons[]{"STOP", "MAX_TOKENS", "SAFETY", "RECITATION", "BLOCKLIST", "PROHIBITED_CONTENT", "SPII",
      "IMAGE_SAFETY", "IMAGE_PROHIBITED_CONTENT", "IMAGE_RECITATION", "ESCALATION", "PUP_LIMITED_DISABLED",
      "MALFORMED_FUNCTION_CALL", "UNEXPECTED_TOOL_CALL", "TOO_MANY_TOOL_CALLS", "OTHER", "LANGUAGE", "NO_IMAGE", "IMAGE_OTHER"};
  for (const auto reason : reasons) CHECK(policy_kind(desc().stop_kind(reason)) == stop_of(reason));
  for (const auto& [raw, kind] : desc().stop_mappings()) {
    CHECK(std::find(std::begin(reasons), std::end(reasons), raw) != std::end(reasons));
    CHECK(policy_kind(kind) == stop_of(raw));
  }
  // A policy snapshot may restate the compiled mapping but cannot rewrite it.
  std::string policy(config_defaults::descriptor_policy_json);
  const std::string fact = "\"IMAGE_SAFETY\":\"ContentFilter\"";
  const auto at = policy.find(fact); CHECK(at != std::string::npos);
  policy.replace(at, fact.size(), "\"IMAGE_SAFETY\":\"EndTurn\"");
  const auto rewritten = descriptor::load_policy(policy, config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::ConfigError>(rewritten) && std::get<descriptor::ConfigError>(rewritten).pointer == "/stop_reasons");
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
void tool_result_helper_needs_a_json_object() {
  auto first = buffered(body(R"([{"functionCall":{"name":"lookup","args":{"x":1}}}])"));
  const auto& call_message = complete(first).messages[0];
  const auto id = std::get<ToolCall>(call_message.parts[0]).id;
  auto with = [&](Message result) {
    auto r = request();
    r.messages.push_back(call_message);
    r.messages.push_back(std::move(result));
    return gemini::encode(desc(), r, true);
  };
  // Message::tool_result passes its text through: Generate takes only one JSON object.
  auto object = with(Message::tool_result(id, R"({"result":5})"));
  CHECK(std::holds_alternative<gemini::EncodedRequest>(object));
  CHECK(std::get<gemini::EncodedRequest>(object).body.find(R"("response":{"result":5})") != std::string::npos);
  auto failed = with(Message::tool_result(id, R"({"reason":"x"})", true));
  CHECK(std::holds_alternative<gemini::EncodedRequest>(failed));
  CHECK(std::get<gemini::EncodedRequest>(failed).body.find(R"("response":{"error":{"reason":"x"}})") != std::string::npos);
  for (const char* text : {"5", "[5]", "not json", ""}) {
    auto refused = with(Message::tool_result(id, text));
    CHECK(std::holds_alternative<Error>(refused) && std::get<Error>(refused).kind == ErrorKind::InvalidRequest);
  }
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
  // The service's own calls and results are typed parts. A result without an id answers the oldest open call of its
  // kind, and one with no open call gets a local id.
  o = buffered(body(R"([{"toolCall":{"id":"server","toolType":"GOOGLE_SEARCH","args":{}}},{"codeExecutionResult":{"outcome":"OUTCOME_OK","output":"one"}}])"));
  {
    const auto& p = complete(o).messages[0].parts;
    CHECK(p.size() == 2 && complete(o).stop.kind == StopKind::EndTurn);
    const auto& call = std::get<ToolCall>(p[0]);
    CHECK(call.kind == ToolCallKind::ServerExecuted && call.id == "server" && call.name == "google_search" && call.wire_type == "toolCall" && call.input->root().size() == 0);
    const auto& result = std::get<ServerToolResult>(p[1]);
    CHECK(result.wire_type == "codeExecutionResult" && result.tool_use_id.starts_with("gemini-local-") && result.content->root().get("codeExecutionResult").get("output").as_string() == "one");
  }
  o = buffered(body(R"re([{"executableCode":{"language":"PYTHON","code":"print(1)"}},{"executableCode":{"language":"PYTHON","code":"print(2)"}},)re"
                    R"re({"codeExecutionResult":{"outcome":"OUTCOME_OK","output":"1"}},{"codeExecutionResult":{"outcome":"OUTCOME_OK","output":"2"}},)re"
                    R"re({"toolCall":{"id":"m1","toolName":"docs:search","args":{"q":"x"}}},{"toolResponse":{"id":"m1","response":{"result":"y"}}}])re"));
  {
    const auto& p = complete(o).messages[0].parts;
    CHECK(p.size() == 6);
    const auto& first = std::get<ToolCall>(p[0]), &second = std::get<ToolCall>(p[1]);
    CHECK(first.name == "code_execution" && first.wire_type == "executableCode" && first.input->root().get("code").as_string() == "print(1)" && first.id != second.id);
    CHECK(std::get<ServerToolResult>(p[2]).tool_use_id == first.id && std::get<ServerToolResult>(p[3]).tool_use_id == second.id);
    const auto& mcp = std::get<ToolCall>(p[4]);
    CHECK(mcp.kind == ToolCallKind::ServerExecuted && mcp.name == "docs:search" && mcp.input->root().get("q").as_string() == "x");
    CHECK(std::get<ServerToolResult>(p[5]).tool_use_id == "m1" && std::get<ServerToolResult>(p[5]).wire_type == "toolResponse");
  }
  for (auto p : {R"([{"toolCall":{"id":"s","args":{}}}])", R"([{"toolCall":{"id":"s","toolType":"GOOGLE_SEARCH_WEB","args":[]}}])",
                 R"([{"executableCode":{"id":"","code":"x"}}])", R"([{"toolResponse":{"id":7}}])", R"([{"toolCall":{"id":"call_owned","toolType":"URL_CONTEXT"}},{"functionCall":{"id":"call_owned","name":"lookup","args":{}}}])"})
  { o = buffered(body(p)); failure(o, ErrorKind::ProtocolCorrupt); }
}
void raw_observation_ownership() {
  auto wire = body(R"([{"toolCall":{"id":"server","toolType":"GOOGLE_SEARCH","args":{"z":[null,2,1]}}}])");
  wire.pop_back(); wire += R"(,"future":{"z":[null,{"b":2,"a":1}],"a":false}})";
  auto outcome = buffered(wire);
  const auto& done = complete(outcome);
  CHECK(done.raw_events.size() == 1 && done.raw_events[0].payload->root().dump() == wire);
  CHECK(std::get<ToolCall>(done.messages[0].parts[0]).kind == ToolCallKind::ServerExecuted);
  const auto prefix = body(R"([{"text":"owned prefix"}])", "", "null");
  const auto terminal = body("[]", "STOP", "null");
  const auto trailing = R"({"usageMetadata":{"promptTokenCount":4,"candidatesTokenCount":2},"future":[3,2,1]})";
  outcome = stream({prefix, terminal, trailing});
  const auto& history = complete(outcome).raw_events;
  CHECK(history.size() == 3);
  CHECK(history[0].payload->root().dump() == prefix && history[1].payload->root().dump() == terminal &&
      history[2].payload->root().dump() == trailing);
  CHECK(!complete(outcome).usage.output_total && !complete(outcome).usage.reasoning);
  outcome = stream({prefix});
  const auto& partial = failure(outcome, ErrorKind::Truncated).partial;
  CHECK(partial.raw_events.size() == 1 && partial.raw_events[0].payload->root().dump() == prefix);
  CHECK(std::get<Text>(partial.messages[0].parts[0]).value == "owned prefix");
  const auto duplicate = body(R"([{"functionCall":{"id":"dup","name":"lookup","args":{"z":1,"z":2,"a":[3,2]}}}])");
  outcome = buffered(duplicate);
  const auto& invalid = complete(outcome);
  CHECK(invalid.raw_events[0].payload->root().dump() == duplicate);
  CHECK(std::get<InvalidToolCall>(invalid.messages[0].parts[0]).raw_fragment == R"({"z":1,"z":2,"a":[3,2]})");
  CHECK(!invalid.messages[0].native || !invalid.messages[0].native->complete());
  const auto unsupported = body(R"([{"futureTool":{"b":2,"a":1}}])");
  outcome = buffered(unsupported);
  CHECK(failure(outcome, ErrorKind::Unsupported).partial.raw_events[0].payload->root().dump() == unsupported);
  const auto remote = R"({"error":{"message":"private","vendor":{"b":2,"a":1}}})";
  outcome = buffered(remote);
  CHECK(failure(outcome, ErrorKind::RemoteFailure).partial.raw_events[0].payload->root().dump() == remote);
  outcome = stream({prefix, terminal, remote});
  const auto& rejected = failure(outcome, ErrorKind::RemoteFailure).partial;
  CHECK(rejected.raw_events.size() == 3 && rejected.raw_events[2].payload->root().dump() == remote);
  CHECK(rejected.stop && rejected.stop->kind == StopKind::EndTurn);
}
void named_error_raw_ownership() {
  const auto prefix = body(R"([{"text":"before error"}])", "", "null");
  const auto structured = R"({"code":429,"details":{"future":[{"b":2,"a":1}]},"message":"private"})";
  for (const auto& error : {std::string(structured), std::string("{"), std::string("unstructured"), std::string("[]")}) {
    const auto outcome = [&] {
      Accumulator a; gemini::Codec c(desc(), gemini::Mode::Sse, a, context());
      CHECK(c.frame("message", prefix)); CHECK(!c.frame("error", error));
      CHECK(!c.frame("message", body("[]"))); c.finish();
      CHECK(a.outcome()); return *a.outcome();
    }();
    const auto& partial = failure(outcome, ErrorKind::RemoteFailure).partial;
    CHECK(std::get<Text>(partial.messages[0].parts[0]).value == "before error");
    CHECK(partial.raw_events.size() == (error == structured ? 2U : 1U));
    CHECK(partial.raw_events[0].payload->root().dump() == prefix);
    if (error == structured) CHECK(partial.raw_events[1].type == "error" &&
        partial.raw_events[1].payload->root().dump() == structured);
  }
  const auto bounded = [&] {
    Accumulator a({}, {}, 0); gemini::Codec c(desc(), gemini::Mode::Sse, a, context());
    CHECK(!c.frame("error", "{\"vendor\":\"" + std::string(8192, 'x') + "\"}"));
    CHECK(a.outcome()); return *a.outcome();
  }();
  CHECK(failure(bounded, ErrorKind::RemoteFailure).partial.raw_events.empty());
}
void portable_foreign_history() {
  auto r = request();
  r.messages.push_back(Message{"foreign-text", Role::Assistant, {Text{"prior answer"}}});
  auto rejected = gemini::encode(desc(), r, false);
  CHECK(std::holds_alternative<Error>(rejected) && std::get<Error>(rejected).kind == ErrorKind::ReplayIneligible);
  r.history_mode = gemini::HistoryMode::PortableForeign;
  auto encoded = gemini::encode(desc(), r, false);
  CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  auto wire = owned(std::get<gemini::EncodedRequest>(encoded).body);
  CHECK(json::equal(wire->root().get("contents").at(1), owned(R"({"role":"model","parts":[{"text":"prior answer"}]})")->root()));
  r.tools.push_back({"other", "Other value", owned(R"({"type":"object"})")});
  r.messages.push_back(Message{"foreign-calls", Role::Assistant,
      {Text{"checking"}, ToolCall{"foreign-a", "lookup", ToolCallKind::ClientExecuted, owned(R"({"x":1})")},
       Text{"also"}, ToolCall{"foreign-b", "other", ToolCallKind::ClientExecuted, owned(R"({"y":2})")}}});
  r.messages.push_back(Message{"", Role::Tool, {ToolResult{"foreign-b", R"({"second":2})"}, ToolResult{"foreign-a", R"({"first":1})", true}}});
  encoded = gemini::encode(desc(), r, true);
  CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  wire = owned(std::get<gemini::EncodedRequest>(encoded).body);
  const auto calls = wire->root().get("contents").at(2).get("parts");
  CHECK(!calls.at(0).get("thoughtSignature").valid() && !calls.at(2).get("thoughtSignature").valid());
  CHECK(calls.at(1).get("thoughtSignature").as_string() == "skip_thought_signature_validator");
  CHECK(!calls.at(3).get("thoughtSignature").valid());
  CHECK(calls.at(1).get("functionCall").get("id").as_string() == "foreign-a");
  CHECK(calls.at(3).get("functionCall").get("args").get("y").as_uint() == 2);
  const auto results = wire->root().get("contents").at(3).get("parts");
  CHECK(results.at(0).get("functionResponse").get("name").as_string() == "other");
  CHECK(results.at(0).get("functionResponse").get("id").as_string() == "foreign-b");
  CHECK(results.at(1).get("functionResponse").get("response").get("error").get("first").as_uint() == 1);
  for (const auto& message : r.messages) CHECK(!message.native && !message.wire_output);
  auto refuse = [&](const gemini::Request& bad, ErrorKind kind) {
    auto e = gemini::encode(desc(), bad, false);
    CHECK(std::holds_alternative<Error>(e) && std::get<Error>(e).kind == kind);
  };
  auto bad = r; bad.messages[1].parts.push_back(Thinking{"private", "signature"}); refuse(bad, ErrorKind::ReplayIneligible);
  for (const Part& part : std::vector<Part>{RedactedThinking{"private"}, Reasoning{"reasoning-id", {"summary"}},
      Thought{{"summary"}, "signature"}, Opaque{"foreign", owned(R"({"private":true})")}, Refusal{"refusal", ""},
      InvalidToolCall{"bad", "lookup", ToolCallKind::ClientExecuted, "{", InvalidReason::NotJson}}) {
    bad = r; bad.messages[1].parts.push_back(part); refuse(bad, ErrorKind::ReplayIneligible);
  }
  bad = r; bad.messages[1].wire_output = owned(R"([{"text":"prior answer"}])"); refuse(bad, ErrorKind::ReplayIneligible);
  bad = r; std::get<ToolCall>(bad.messages[2].parts[1]).wire_metadata = owned(R"({"thoughtSignature":"captured"})"); refuse(bad, ErrorKind::ReplayIneligible);
  bad = r; std::get<ToolCall>(bad.messages[2].parts[1]).wire_type = "function"; refuse(bad, ErrorKind::ReplayIneligible);
  bad = r; std::get<ToolCall>(bad.messages[2].parts[1]).kind = ToolCallKind::ServerExecuted; refuse(bad, ErrorKind::ReplayIneligible);
  bad = r; std::get<ToolCall>(bad.messages[2].parts[3]).id = "foreign-a"; refuse(bad, ErrorKind::InvalidRequest);
  bad = r; std::get<ToolCall>(bad.messages[2].parts[1]).input = owned("[]"); refuse(bad, ErrorKind::InvalidRequest);
  bad = r; std::get<ToolResult>(bad.messages[3].parts[1]).tool_use_id = "foreign-b"; refuse(bad, ErrorKind::InvalidRequest);
  bad = r; bad.messages[3].parts.pop_back(); refuse(bad, ErrorKind::InvalidRequest);
  bad = r; bad.messages.pop_back(); refuse(bad, ErrorKind::InvalidRequest);
  // A real newly decoded output binds the imported prefix but grants no
  // authority to the caller-created messages themselves.
  Accumulator a; gemini::Codec codec(desc(), gemini::Mode::Buffered, a, std::get<gemini::EncodedRequest>(encoded).context);
  codec.buffered(body(R"([{"text":"new native"}])"), {}); CHECK(a.outcome());
  r.messages.push_back(complete(*a.outcome()).messages[0]); CHECK(r.messages.back().native->complete());
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(desc(), r, false)));
  bad = r; std::get<Text>(bad.messages[1].parts[0]).value = "changed import"; refuse(bad, ErrorKind::ReplayIneligible);
  bad = r; bad.account_scope = "foreign origin"; refuse(bad, ErrorKind::ReplayIneligible);
}
void typed_generation_controls() {
  auto r = request(); r.thinking_budget.reset(); r.temperature = 0.25; r.include_thoughts = false;
  r.safety_settings = {{gemini::SafetyCategory::Harassment, gemini::SafetyThreshold::BlockNone},
      {gemini::SafetyCategory::HateSpeech, gemini::SafetyThreshold::BlockOnlyHigh},
      {gemini::SafetyCategory::SexuallyExplicit, gemini::SafetyThreshold::BlockMediumAndAbove},
      {gemini::SafetyCategory::DangerousContent, gemini::SafetyThreshold::BlockLowAndAbove},
      {gemini::SafetyCategory::CivicIntegrity, gemini::SafetyThreshold::Off}};
  const auto expected = owned(R"([{"category":"HARM_CATEGORY_HARASSMENT","threshold":"BLOCK_NONE"},{"category":"HARM_CATEGORY_HATE_SPEECH","threshold":"BLOCK_ONLY_HIGH"},{"category":"HARM_CATEGORY_SEXUALLY_EXPLICIT","threshold":"BLOCK_MEDIUM_AND_ABOVE"},{"category":"HARM_CATEGORY_DANGEROUS_CONTENT","threshold":"BLOCK_LOW_AND_ABOVE"},{"category":"HARM_CATEGORY_CIVIC_INTEGRITY","threshold":"OFF"}])");
  for (auto [level, name] : {std::pair{gemini::ThinkingLevel::Minimal, "minimal"}, {gemini::ThinkingLevel::Low, "low"},
      {gemini::ThinkingLevel::Medium, "medium"}, {gemini::ThinkingLevel::High, "high"}}) {
    r.thinking_level = level;
    auto e = gemini::encode(desc(), r, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(e));
    const auto wire = owned(std::get<gemini::EncodedRequest>(e).body);
    const auto config = wire->root().get("generationConfig");
    CHECK(config.get("temperature").as_double() == 0.25);
    CHECK(config.get("thinkingConfig").get("thinkingLevel").as_string() == name);
    CHECK(!config.get("thinkingConfig").get("thinkingBudget").valid());
    CHECK(!config.get("thinkingConfig").get("includeThoughts").as_bool());
    CHECK(json::equal(wire->root().get("safetySettings"), expected->root()));
  }
  for (auto [mode, name] : {std::pair{gemini::ToolChoiceMode::Auto, "AUTO"}, {gemini::ToolChoiceMode::Any, "ANY"},
      {gemini::ToolChoiceMode::None, "NONE"}, {gemini::ToolChoiceMode::Validated, "VALIDATED"}}) {
    r.tool_choice = gemini::ToolChoice{mode, {}};
    if (mode == gemini::ToolChoiceMode::Any || mode == gemini::ToolChoiceMode::Validated) r.tool_choice->allowed_function_names = {"lookup"};
    const auto e = gemini::encode(desc(), r, true); CHECK(std::holds_alternative<gemini::EncodedRequest>(e));
    const auto wire = owned(std::get<gemini::EncodedRequest>(e).body);
    const auto choice = wire->root().get("toolConfig").get("functionCallingConfig");
    CHECK(choice.get("mode").as_string() == name);
    if (!r.tool_choice->allowed_function_names.empty()) CHECK(choice.get("allowedFunctionNames").at(0).as_string() == "lookup");
    else CHECK(!choice.get("allowedFunctionNames").valid());
  }
  const auto seeded = gemini::encode(desc(), r, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(seeded));
  Accumulator a; gemini::Codec codec(desc(), gemini::Mode::Buffered, a, std::get<gemini::EncodedRequest>(seeded).context);
  codec.buffered(body(R"([{"text":"answer"}])"), {}); CHECK(a.outcome());
  auto continuation = r; continuation.messages.push_back(complete(*a.outcome()).messages[0]);
  continuation.tool_choice = gemini::ToolChoice{gemini::ToolChoiceMode::None, {}};
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(desc(), continuation, true)));
  continuation.tool_choice.reset(); continuation.required_tool = "lookup";
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(desc(), continuation, false)));
  auto refuse = [&](gemini::Request bad) { const auto e = gemini::encode(desc(), bad, false); CHECK(std::holds_alternative<Error>(e) && std::get<Error>(e).kind == ErrorKind::InvalidRequest); };
  auto bad = r; bad.thinking_budget = 0; refuse(bad);
  bad = r; bad.history_mode = static_cast<gemini::HistoryMode>(99); refuse(bad);
  bad = r; bad.thinking_level = static_cast<gemini::ThinkingLevel>(99); refuse(bad);
  bad = r; bad.temperature = 3; refuse(bad);
  bad = r; bad.safety_settings.push_back(bad.safety_settings[0]); refuse(bad);
  bad = r; bad.safety_settings[0].category = static_cast<gemini::SafetyCategory>(99); refuse(bad);
  bad = r; bad.safety_settings[0].threshold = static_cast<gemini::SafetyThreshold>(99); refuse(bad);
  bad = r; bad.tool_choice->mode = static_cast<gemini::ToolChoiceMode>(99); refuse(bad);
  bad = r; bad.required_tool = "lookup"; refuse(bad);
  bad = r; bad.tool_choice->allowed_function_names = {"lookup", "lookup"}; refuse(bad);
  bad = r; bad.tool_choice->allowed_function_names = {"undeclared"}; refuse(bad);
  bad = r; bad.tool_choice->mode = gemini::ToolChoiceMode::None; refuse(bad);
  bad = r; bad.tool_choice->mode = gemini::ToolChoiceMode::Auto; refuse(bad);
}

// An HTTP error body can wrap its envelope in a one-element array (Google Interactions).
// The raw event keeps it exactly as received; anything else that is not an object stays rejected.
void http_error_envelope_in_an_array_is_retained() {
  const Error error{ErrorKind::Authentication, "authentication failed"};
  for (const std::string body : {R"({"error":{"code":400,"status":"INVALID_ARGUMENT"}})",
                                 R"([{"error":{"code":400,"status":"INVALID_ARGUMENT"}}])"}) {
    Accumulator a;
    CHECK(a.accept(RawWire{"http.error", owned(body)}, &error));
    CHECK(a.accept(Fail{error}));
    CHECK(a.outcome());
    const auto& partial = failure(*a.outcome(), ErrorKind::Authentication).partial;
    CHECK(partial.raw_events.size() == 1 && partial.raw_events[0].type == "http.error");
    CHECK(partial.raw_events[0].payload->root().dump() == body);
  }
  for (const std::string body : {"[]", "[1]", R"([[{"error":{}}]])", R"([{"a":1},{"b":2}])", "\"text\"", "1"}) {
    Accumulator a;
    CHECK(!a.accept(RawWire{"http.error", owned(body)}, &error));
  }
  Accumulator elsewhere;  // only an HTTP error body is allowed this shape
  CHECK(!elsewhere.accept(RawWire{"message", owned(R"([{"a":1}])")}, &error));
}

// Gemini omits thoughtsTokenCount when thinking is off. output_total is then the candidates
// count, but only when the request set the thinking budget to zero.
void output_total_without_thoughts() {
  auto thinking_off = [](std::optional<uint64_t> budget) {
    auto r = request(); r.thinking_budget = budget;
    auto e = gemini::encode(desc(), r, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(e));
    return std::get<gemini::EncodedRequest>(e).context;
  };
  const auto usage = R"({"promptTokenCount":8,"candidatesTokenCount":3,"totalTokenCount":11})";
  auto run = [&](std::shared_ptr<const NativeContext> context, std::string_view counters) {
    Accumulator a; gemini::Codec c(desc(), gemini::Mode::Buffered, a, std::move(context));
    c.buffered(body(R"([{"text":"ok"}])", "STOP", counters), {}); CHECK(a.outcome());
    return complete(*a.outcome()).usage;
  };
  auto off = run(thinking_off(0), usage);
  CHECK(off.output_total && off.output_total->value == 3 && off.output_total->evidence == Evidence::Derived);
  CHECK(off.total && off.total->value == 11 && off.reasoning && off.reasoning->value == 0 && off.reasoning->evidence == Evidence::Derived);
  // without a reported total only the budget proves the output; reasoning stays unknown
  auto unproven = run(thinking_off(0), R"({"promptTokenCount":8,"candidatesTokenCount":3})");
  CHECK(unproven.output_total && unproven.output_total->value == 3 && !unproven.reasoning && unproven.total->value == 11);
  // thinking enabled (explicit budget, or none at all), no thoughts count and no total proving
  // zero thoughts: still unknown
  for (auto budget : {std::optional<uint64_t>(1024), std::optional<uint64_t>()}) {
    auto on = run(thinking_off(budget), R"({"promptTokenCount":8,"candidatesTokenCount":3,"totalTokenCount":12})");
    CHECK(!on.output_total && !on.total && !on.reasoning && on.provider_reported_total && on.provider_reported_total->value == 12);
    on = run(thinking_off(budget), R"({"promptTokenCount":8,"candidatesTokenCount":3})");
    CHECK(!on.output_total && !on.total && !on.reasoning);
  }
  // a reported thoughts count always wins, even with the budget at zero
  auto reported = run(thinking_off(0), R"({"promptTokenCount":8,"candidatesTokenCount":3,"thoughtsTokenCount":2})");
  CHECK(reported.output_total && reported.output_total->value == 5 && reported.reasoning->value == 2);
  // the flag survives dropping issuing authority, which is what the runtime keeps while decoding
  CHECK(run(thinking_off(0)->decoding_only(), usage).output_total);
}
// Captured gemini-2.5-flash-lite usage with thinking left on: thoughtsTokenCount is omitted, but
// totalTokenCount == prompt + candidates proves the thoughts were zero.
void total_proves_zero_thoughts() {
  const auto captured = R"({"promptTokenCount":21,"candidatesTokenCount":1,"totalTokenCount":22,"promptTokensDetails":[{"modality":"TEXT","tokenCount":21}],"serviceTier":"standard"})";
  for (const auto& o : {buffered(body(R"([{"text":"Blue"}])", "STOP", captured)), stream({body(R"([{"text":"Blue"}])", "STOP", captured)})}) {
    const auto& u = complete(o).usage;
    CHECK(u.input_total->value == 21 && u.provider_reported_total->value == 22 && u.quality == UsageQuality::Consistent);
    CHECK(u.reasoning && u.reasoning->value == 0 && u.reasoning->evidence == Evidence::Derived);
    CHECK(u.output_total && u.output_total->value == 1 && u.output_total->evidence == Evidence::Derived);
    CHECK(u.total && u.total->value == 22 && u.total->evidence == Evidence::Derived);
    CHECK(u.extra.at("candidatesTokenCount").value == 1 && u.extra.at("promptTokensDetails.TEXT").value == 21);
  }
  const auto usage_of = [&](std::string_view counters) { return complete(buffered(body(R"([{"text":"Blue"}])", "STOP", counters))).usage; };
  // a zero tool-use prompt count does not disturb the proof
  auto u = usage_of(R"({"promptTokenCount":21,"candidatesTokenCount":1,"toolUsePromptTokenCount":0,"totalTokenCount":22})");
  CHECK(u.reasoning->value == 0 && u.output_total->value == 1 && u.total->value == 22);
  // a larger total leaves room for unreported thoughts; a nonzero tool-use prompt is ambiguous
  for (auto counters : {R"({"promptTokenCount":21,"candidatesTokenCount":1,"totalTokenCount":30})",
                        R"({"promptTokenCount":21,"candidatesTokenCount":1,"toolUsePromptTokenCount":2,"totalTokenCount":24})",
                        R"({"promptTokenCount":21,"candidatesTokenCount":1,"toolUsePromptTokenCount":2,"totalTokenCount":22})",
                        R"({"promptTokenCount":21,"candidatesTokenCount":1,"totalTokenCount":null})",
                        R"({"candidatesTokenCount":1,"totalTokenCount":1})"}) {
    u = usage_of(counters);
    CHECK(!u.reasoning && !u.output_total && !u.total && u.quality == UsageQuality::Consistent);
  }
  // a reported thoughts count always wins
  u = usage_of(R"({"promptTokenCount":21,"candidatesTokenCount":1,"thoughtsTokenCount":5,"totalTokenCount":27})");
  CHECK(u.reasoning->value == 5 && u.reasoning->evidence == Evidence::Reported && u.output_total->value == 6 && u.total->value == 27);
}
// When the output budget is spent while thinking, usageMetadata has promptTokenCount,
// thoughtsTokenCount and totalTokenCount but no candidatesTokenCount. The total still bounds the
// output, so the output and the total are derived from it instead of being left unknown.
void total_without_candidates_bounds_the_output() {
  const auto spent = R"({"promptTokenCount":13,"totalTokenCount":25,"thoughtsTokenCount":12,"promptTokensDetails":[{"modality":"TEXT","tokenCount":13}]})";
  // Recorded shape: the candidate ends MAX_TOKENS with a content object that has a role and no parts.
  auto wire = [](std::string_view usage) {
    return std::string(R"({"modelVersion":"fixture-model","responseId":"generation","candidates":[{"content":{"role":"model"},"finishReason":"MAX_TOKENS","index":0}],"usageMetadata":)") + std::string(usage) + "}";
  };
  for (const auto& o : {buffered(wire(spent)), stream({wire(spent)}), buffered(body("[]", "MAX_TOKENS", spent))}) {
    const auto& c = complete(o);
    CHECK(c.stop.kind == StopKind::MaxTokens && c.usage.stage == UsageStage::Final && c.usage.quality == UsageQuality::Consistent);
    const auto& u = c.usage;
    CHECK(u.input_total->value == 13 && u.reasoning->value == 12 && u.reasoning->evidence == Evidence::Reported);
    CHECK(u.provider_reported_total->value == 25 && u.provider_reported_total->evidence == Evidence::Reported);
    CHECK(u.output_total && u.output_total->value == 12 && u.output_total->evidence == Evidence::Derived);
    CHECK(u.total && u.total->value == 25 && u.total->evidence == Evidence::Derived);
    CHECK(!u.extra.contains("candidatesTokenCount"));
  }
  const auto usage_of = [&](std::string_view counters) { return complete(buffered(wire(counters))).usage; };
  // Without thoughts either: everything beyond the prompt is output, and an empty output derives zero.
  auto u = usage_of(R"({"promptTokenCount":13,"totalTokenCount":20})");
  CHECK(u.output_total && u.output_total->value == 7 && u.output_total->evidence == Evidence::Derived && u.total->value == 20 && !u.reasoning);
  u = usage_of(R"({"promptTokenCount":13,"totalTokenCount":13})");
  CHECK(u.output_total && u.output_total->value == 0 && u.total->value == 13);
  // A reported candidates count keeps the earlier rules: a larger total is no proof of the output.
  u = usage_of(R"({"promptTokenCount":13,"candidatesTokenCount":3,"totalTokenCount":25})");
  CHECK(!u.output_total && !u.total);
  // A nonzero tool-use prompt count is part of the total but not of the output: ambiguous, so unknown.
  u = usage_of(R"({"promptTokenCount":13,"toolUsePromptTokenCount":2,"thoughtsTokenCount":12,"totalTokenCount":27})");
  CHECK(!u.output_total && !u.total && u.quality == UsageQuality::Consistent);
  u = usage_of(R"({"promptTokenCount":13,"toolUsePromptTokenCount":0,"thoughtsTokenCount":12,"totalTokenCount":25})");
  CHECK(u.output_total && u.output_total->value == 12);
  // Missing prompt or total leaves the output unknown.
  for (auto counters : {R"({"thoughtsTokenCount":12,"totalTokenCount":25})", R"({"promptTokenCount":13,"thoughtsTokenCount":12})",
                        R"({"promptTokenCount":13,"thoughtsTokenCount":12,"totalTokenCount":null})"}) {
    u = usage_of(counters);
    CHECK(!u.output_total && !u.total && u.quality == UsageQuality::Consistent);
  }
  // Counters that cannot be true are a conflict, not a derivation.
  for (auto counters : {R"({"promptTokenCount":13,"thoughtsTokenCount":12,"totalTokenCount":10})",
                        R"({"promptTokenCount":13,"thoughtsTokenCount":12,"totalTokenCount":20})"}) {
    u = usage_of(counters);
    CHECK(!u.output_total && !u.total && u.quality == UsageQuality::Inconsistent && !u.conflicts.empty());
  }
}
// descriptor::load holds a google.generate descriptor to the route shape the encoder accepts, so a descriptor that
// loads encodes for the model its paths name (buffered and streamed) and only for that model, and nothing the encoder
// accepts is refused at load.
void loaded_routes_encode_for_their_model() {
  for (const std::string model : {"gemini-2.5-flash", "fixture-model", "Model_1.0-a", "x"}) {
    auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"route","family":"google.generate","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1beta/models/)" + model +
        R"(:generateContent","streaming":"/v1beta/models/)" + model + R"(:streamGenerateContent?alt=sse"}}})");
    CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
    const auto& d = std::get<descriptor::ValidatedDescriptor>(loaded);
    auto r = request(); r.model = model;
    for (const bool sse : {false, true}) CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(d, r, sse)));
    r.model = model + "-other";
    for (const bool sse : {false, true}) {
      const auto refused = gemini::encode(d, r, sse);
      CHECK(std::holds_alternative<Error>(refused) && std::get<Error>(refused).kind == ErrorKind::InvalidConfig);
    }
  }
}
// One descriptor whose paths name {model} serves every model of the origin. Each request goes to its own model's
// route, and a seal stays bound to its model: the same descriptor refuses to replay it to another one.
void one_descriptor_serves_every_model() {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"routed","family":"google.generate","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1beta/models/{model}:generateContent","streaming":"/v1beta/models/{model}:streamGenerateContent?alt=sse"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& d = std::get<descriptor::ValidatedDescriptor>(loaded);
  for (const std::string model : {"gemini-a", "gemini-b"})
    for (const bool sse : {false, true}) {
      auto r = request(); r.model = model;
      const auto encoded = gemini::encode(d, r, sse);
      CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
      CHECK(std::get<gemini::EncodedRequest>(encoded).path == "/v1beta/models/" + model + (sse ? ":streamGenerateContent?alt=sse" : ":generateContent"));
    }
  auto first = request(); first.model = "gemini-a";
  const auto encoded = std::get<gemini::EncodedRequest>(gemini::encode(d, first, false));
  Accumulator a; gemini::Codec c(d, gemini::Mode::Buffered, a, encoded.context);
  c.buffered(body(R"([{"text":"answer","thoughtSignature":"SIG"}])", "STOP", counters, "gemini-a"), {});
  CHECK(a.outcome());
  const auto& sealed = complete(*a.outcome()).messages.at(0);
  CHECK(sealed.native && sealed.native->complete());
  auto next = first;
  next.messages.push_back(sealed);
  next.messages.push_back(Message{"", Role::User, {Text{"next"}}});
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(d, next, false)));
  next.model = "gemini-b";
  const auto other = gemini::encode(d, next, false);
  CHECK(std::holds_alternative<Error>(other) && std::get<Error>(other).kind == ErrorKind::ReplayIneligible);
}
// Google answers aliases and auto-routed models under other names (lyria-3-pro-preview as lyria-3.5,
// gemini-3.5-flash as gemini-3.6-flash). The name is recorded; an unexplained one is delivered
// without a native seal. The embedded catalogue relates the observed pairs.
void served_model_is_recorded() {
  const auto exact = buffered(body(parts, "STOP"));
  CHECK(complete(exact).model.relation == ServedRelation::Exact && complete(exact).messages.at(0).native);
  const auto version = buffered(body(parts, "STOP", counters, "fixture-model-001"));
  CHECK(complete(version).model.relation == ServedRelation::Snapshot && complete(version).messages.at(0).native);
  for (const bool sse : {false, true}) {
    const auto wire = body(parts, "STOP", counters, "other-model");
    const auto outcome = sse ? stream({wire}) : buffered(wire);
    const auto& c = complete(outcome);
    CHECK(c.model.requested == "fixture-model" && c.model.served == "other-model" && c.model.relation == ServedRelation::Unexplained);
    CHECK(c.messages.size() == 1 && c.messages[0].parts.size() == 4 && !c.messages[0].native);
  }
  // A name first reported in a later chunk still withholds the seal of the message begun before it.
  const auto late = stream({R"({"responseId":"generation","candidates":[{"index":0,"content":{"role":"model","parts":[{"text":"a"}]}}]})",
                            body(R"([{"text":"b"}])", "STOP", counters, "other-model")});
  CHECK(complete(late).model.relation == ServedRelation::Unexplained && !complete(late).messages.at(0).native);
  // Observed pairs live in the embedded catalogue: requests of those models.
  struct Pair { std::string_view requested, served; };
  for (const auto& [requested, served] : {Pair{"lyria-3-pro-preview", "lyria-3.5"}, Pair{"gemini-3.5-flash", "gemini-3.6-flash"}}) {
    auto loaded = descriptor::load(std::string(R"({"descriptor_version":1,"revision":1,"id":"gemini-served","family":"google.generate","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1beta/models/)") +
                                   std::string(requested) + R"(:generateContent","streaming":"/v1beta/models/)" + std::string(requested) +
                                   R"(:streamGenerateContent?alt=sse"}}})");
    CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
    const auto& d = std::get<descriptor::ValidatedDescriptor>(loaded);
    auto r = request(); r.model = std::string(requested);
    auto encoded = gemini::encode(d, r, false);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
    Accumulator a; gemini::Codec c(d, gemini::Mode::Buffered, a, std::get<gemini::EncodedRequest>(encoded).context);
    c.buffered(body(R"([{"text":"x"}])", "STOP", counters, served), {});
    CHECK(a.outcome() && complete(*a.outcome()).model.relation == ServedRelation::Listed);
  }
}
// A failure names the check that failed with a code of the fixed table, whatever the response says; the codec's own text
// stays as it was and nothing the vendor sent reaches either.
void failure_reasons() {
  // A blocked answer keeps Google's reason as the vendor code; only its UPPER_SNAKE shape passes.
  for (const std::string_view finish : {"SAFETY", "MISSING_THOUGHT_SIGNATURE"}) {
    const auto blocked = buffered(body(R"([{"text":"x"}])", finish));
    const auto& error = failure(blocked, ErrorKind::RemoteFailure).error;
    CHECK(error.reason == "gemini.remote.generation_blocked" && error.vendor_code == finish);
    CHECK(failure(blocked, ErrorKind::RemoteFailure).partial.stop && failure(blocked, ErrorKind::RemoteFailure).partial.stop->raw == finish);
  }
  const auto prompt = buffered(R"({"promptFeedback":{"blockReason":"PROHIBITED_CONTENT"},"modelVersion":"fixture-model"})");
  CHECK(failure(prompt, ErrorKind::RemoteFailure).error.vendor_code == "PROHIBITED_CONTENT");
  const auto odd = buffered(R"({"promptFeedback":{"blockReason":"Blocked: other"},"modelVersion":"fixture-model"})");
  CHECK(failure(odd, ErrorKind::RemoteFailure).error.vendor_code.empty());
  const std::string secret = "VENDOR_SECRET_MARKER_5d2";
  const auto check = [&](const Outcome& outcome, ErrorKind kind, std::string_view message, std::string_view code) {
    const auto& error = failure(outcome, kind).error;
    CHECK(error.safe_message == message && error.reason == code);
    CHECK(error.reason.find(secret) == std::string::npos && error.safe_message.find(secret) == std::string::npos);
  };
  check(stream({body(R"([{"text":"x"}])"), body(R"([{"text":"y"}])", "STOP", counters, secret)}), ErrorKind::ProtocolCorrupt,
        "Gemini model identity changed", "gemini.model.identity_changed");
  check(stream({body(R"([{"text":1}])")}), ErrorKind::ProtocolCorrupt, "Gemini text must be string", "gemini.part.text_invalid");
  check(stream({body(R"([{"text":"x","unknown_part_key":")" + secret + R"("}])")}), ErrorKind::Unsupported, "unknown Gemini part semantic", "gemini.part.semantic_unknown");
  check(stream({body(R"([{"text":"x"}])", "", "null")}), ErrorKind::Truncated, "missing Gemini finish reason", "gemini.terminal.missing");
  const auto cut = stream({body(R"([{"text":"x"}])", "", "null")}, {false, ErrorKind::Transport});
  CHECK(failure(cut, ErrorKind::Transport).error.reason.empty() && failure(cut, ErrorKind::Transport).error.safe_message == "abnormal Gemini response close");
}
// What a stream's sink is shown for the parts of a message: [begin], a text delta with its bytes, [seal]. An empty
// text delta would show as [delta:], and none may.
struct Traced { std::string trace; Outcome outcome; };
Traced traced(std::initializer_list<std::string> chunks, gemini::Close close = {}) {
  Traced result;
  Accumulator a({}, [&](const Event& event) {
    if (std::holds_alternative<PartBegin>(event)) result.trace += "[begin]";
    else if (const auto* d = std::get_if<PartDelta>(&event)) result.trace += "[" + std::string(d->payload.kind == PartKind::Text ? "delta:" : "other:") + std::string(d->payload.bytes) + "]";
    else if (std::holds_alternative<PartSeal>(event)) result.trace += "[seal]";
  });
  gemini::Codec c(desc(), gemini::Mode::Sse, a, context());
  for (const auto& b : chunks) if (!c.frame({}, b)) break;
  c.finish(close); CHECK(a.outcome()); result.outcome = *a.outcome();
  return result;
}
void empty_text_delta_is_withheld() {
  // Gemini ends most answers with a part whose text is "" next to the finish reason. Without a signature that part says
  // nothing: it is not a typed part. A streamed one joins the answer it ends in the retained wire; one with nothing to
  // join stays a wire part of its own, and native replay sends it.
  const auto streamed = traced({body(R"([{"text":"Hel"}])", "", "null"), body(R"([{"text":"lo"}])", "", "null"), body(R"([{"text":""}])")});
  CHECK(streamed.trace == "[begin][delta:Hel][delta:lo][seal]");
  const auto& completion = complete(streamed.outcome);
  CHECK(completion.joined_text() == "Hello" && completion.messages.size() == 1 && completion.messages[0].parts.size() == 1);
  CHECK(completion.messages[0].wire_output && json::equal(completion.messages[0].wire_output->root(), owned(R"([{"text":"Hello"}])")->root()));
  // A buffered body has the parts it has: each wire part is a part of its own.
  const auto whole = complete(buffered(body(R"([{"text":"Hel"},{"text":"lo"},{"text":""}])")));
  CHECK(whole.joined_text() == "Hello" && whole.messages[0].parts.size() == 2 && whole.stop.kind == completion.stop.kind);
  for (size_t i = 0; i < 2; ++i) CHECK(std::holds_alternative<Text>(whole.messages[0].parts[i]));
  CHECK(whole.messages[0].wire_output && whole.messages[0].wire_output->root().size() == 3);
  const auto leading = traced({body(R"([{"text":""}])", "", "null"), body(R"([{"text":"ok"}])")});
  CHECK(leading.trace == "[begin][delta:ok][seal]" && complete(leading.outcome).messages[0].wire_output->root().size() == 2);

  // An empty text part that carries a signature is the same part with the same signature in both modes.
  const auto signed_empty = R"([{"text":"","thoughtSignature":"EMPTY_TEXT_SIG"}])";
  const auto signed_stream = traced({body(R"([{"text":"a"}])", "", "null"), body(signed_empty)});
  CHECK(signed_stream.trace == "[begin][delta:a][seal][begin][seal]");
  const auto& signed_parts = complete(signed_stream.outcome).messages[0].parts;
  CHECK(signed_parts.size() == 2 && std::get<Text>(signed_parts[1]).value.empty());
  CHECK(complete(signed_stream.outcome).messages[0].wire_output && complete(signed_stream.outcome).messages[0].wire_output->root().at(1).get("thoughtSignature").as_string() == "EMPTY_TEXT_SIG");

  // A call cut after an empty part has no typed part in its partial output either.
  const auto cut = traced({body(R"([{"text":""}])", "", "null")}, {false, ErrorKind::Truncated});
  CHECK(cut.trace.empty());
  const auto& partial = failure(cut.outcome, ErrorKind::Truncated).partial;
  CHECK(partial.messages.empty() || partial.messages[0].parts.empty());

  // Only text deltas are withheld: an empty thought part is delivered as it was.
  const auto thought = traced({body(R"([{"text":"","thought":true}])", "", "null"), body(R"([{"text":"ok"}])")});
  CHECK(thought.trace == "[begin][other:][seal][begin][delta:ok][seal]");
}
Outcome stream_all(const std::vector<std::string>& chunks, SemanticLimits limits = {}, std::shared_ptr<const NativeContext> ctx = {}) {
  Accumulator a(limits); gemini::Codec c(desc(), gemini::Mode::Sse, a, ctx ? ctx : context(), limits);
  for (const auto& b : chunks) if (!c.frame({}, b)) break;
  c.finish(); CHECK(a.outcome()); return *a.outcome();
}
std::string repeat(std::string_view piece, size_t count) { std::string result; for (size_t i = 0; i < count; ++i) result += piece; return result; }
// Parts that differ in nothing but their text are the chunks of one part. Thousands of them are one typed part and one
// retained wire part, the ones a buffered reply of the same content carries.
void streamed_chunks_continue_their_part() {
  constexpr size_t n = 1000;
  std::vector<std::string> chunks;
  for (size_t i = 0; i < n; ++i) chunks.push_back(body(R"([{"text":"t","thought":true}])", "", "null"));
  for (size_t i = 0; i < 2 * n; ++i) chunks.push_back(body(R"([{"text":"x"}])", "", "null"));
  chunks.push_back(body(R"([{"text":""}])"));
  const auto live = stream_all(chunks);
  const auto whole = buffered(body("[{\"text\":" + json::quote(repeat("t", n)) + ",\"thought\":true},{\"text\":" + json::quote(repeat("x", 2 * n)) + "}]"));
  for (const auto* outcome : {&live, &whole}) {
    const auto& m = complete(*outcome).messages.at(0);
    CHECK(m.parts.size() == 2 && std::get<Thinking>(m.parts[0]).text == repeat("t", n) && !std::get<Thinking>(m.parts[0]).signature);
    CHECK(std::get<Text>(m.parts[1]).value == repeat("x", 2 * n));
  }
  CHECK(json::equal(complete(live).messages[0].wire_output->root(), complete(whole).messages[0].wire_output->root()));
  CHECK(complete(live).usage.output_total->value == complete(whole).usage.output_total->value);
  // Replay sends the merged parts, as it does after the buffered reply.
  std::string replayed[2];
  size_t at = 0;
  for (const auto* outcome : {&live, &whole}) {
    auto r = request();
    r.messages.push_back(complete(*outcome).messages[0]);
    r.messages.push_back(Message{"", Role::User, {Text{"again"}}});
    const auto encoded = gemini::encode(desc(), r, false);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
    replayed[at++] = owned(std::get<gemini::EncodedRequest>(encoded).body)->root().get("contents").at(1).get("parts").dump();
  }
  CHECK(json::equal(owned(replayed[0])->root(), owned(replayed[1])->root()));

  // A signature, a call, or any other difference in the part ends the run: those chunks are parts of their own.
  chunks.clear();
  for (size_t i = 0; i < n; ++i) chunks.push_back(body(R"([{"text":"p","thought":true}])", "", "null"));
  chunks.push_back(body(R"([{"text":"q","thought":true,"thoughtSignature":"THOUGHT_SIG"}])", "", "null"));
  for (size_t i = 0; i < n; ++i) chunks.push_back(body(R"([{"text":"r","thought":true}])", "", "null"));
  for (size_t i = 0; i < n; ++i) chunks.push_back(body(R"([{"text":"a"}])", "", "null"));
  chunks.push_back(body(R"([{"text":"b","thoughtSignature":"TEXT_SIG"}])", "", "null"));
  for (size_t i = 0; i < n; ++i) chunks.push_back(body(R"([{"text":"c"}])", "", "null"));
  chunks.push_back(body(R"([{"functionCall":{"id":"call_1","name":"lookup","args":{"x":1}}}])", "", "null"));
  for (size_t i = 0; i < n; ++i) chunks.push_back(body(R"([{"text":"d","partMetadata":{"k":1}}])", "", "null"));
  for (size_t i = 0; i < n; ++i) chunks.push_back(body(R"([{"text":"e","partMetadata":{"k":2}}])", "", "null"));
  for (size_t i = 0; i < n; ++i) chunks.push_back(body(R"([{"text":"f","thought":false}])", "", "null"));
  for (size_t i = 0; i < n; ++i) chunks.push_back(body(R"([{"text":"g"}])", "", "null"));
  chunks.push_back(body(R"([{"text":""}])"));
  const auto mixed = complete(stream_all(chunks)).messages.at(0);
  CHECK(mixed.parts.size() == 11);
  CHECK(std::get<Thinking>(mixed.parts[0]).text == repeat("p", n) && !std::get<Thinking>(mixed.parts[0]).signature);
  CHECK(std::get<Thinking>(mixed.parts[1]).text == "q" && std::get<Thinking>(mixed.parts[1]).signature == "THOUGHT_SIG");
  CHECK(std::get<Thinking>(mixed.parts[2]).text == repeat("r", n));
  CHECK(std::get<Text>(mixed.parts[3]).value == repeat("a", n) && std::get<Text>(mixed.parts[4]).value == "b");
  CHECK(std::get<Text>(mixed.parts[5]).value == repeat("c", n) && std::get<ToolCall>(mixed.parts[6]).id == "call_1");
  CHECK(std::get<Text>(mixed.parts[7]).value == repeat("d", n) && std::get<Text>(mixed.parts[8]).value == repeat("e", n));
  CHECK(std::get<Text>(mixed.parts[9]).value == repeat("f", n) && std::get<Text>(mixed.parts[10]).value == repeat("g", n));
  const auto wire = mixed.wire_output->root();
  CHECK(wire.size() == 11 && wire.at(1).get("thoughtSignature").as_string() == "THOUGHT_SIG" && wire.at(4).get("thoughtSignature").as_string() == "TEXT_SIG");
  CHECK(wire.at(0).get("text").as_string() == repeat("p", n) && wire.at(0).get("thought").as_bool() && !wire.at(0).get("thoughtSignature").valid());
  CHECK(wire.at(7).get("partMetadata").get("k").as_uint() == 1 && wire.at(8).get("partMetadata").get("k").as_uint() == 2);
  CHECK(wire.at(9).get("thought").valid() && !wire.at(10).get("thought").valid());
  // The mixed message replays under the signatures it was given.
  auto r = request();
  r.messages.push_back(mixed);
  r.messages.push_back(Message{"", Role::Tool, {ToolResult{"call_1", R"({"result":1})"}}});
  const auto encoded = gemini::encode(desc(), r, false);
  CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  CHECK(json::equal(owned(std::get<gemini::EncodedRequest>(encoded).body)->root().get("contents").at(1).get("parts"), wire));

  // A transcript arrives the same way.
  chunks.clear();
  for (size_t i = 0; i < 2 * n; ++i) chunks.push_back(body(R"([{"audioTranscription":{"text":"hi "}}])", "", "null"));
  chunks.push_back(body(R"([{"text":""}])"));
  const auto transcript = complete(stream_all(chunks)).messages.at(0);
  CHECK(transcript.parts.size() == 1 && std::get<Text>(transcript.parts[0]).value == repeat("hi ", 2 * n));
  const auto transcript_whole = complete(buffered(body("[{\"audioTranscription\":{\"text\":" + json::quote(repeat("hi ", 2 * n)) + R"(}},{"text":""}])"))).messages.at(0);
  CHECK(transcript_whole.parts.size() == 1 && json::equal(transcript.wire_output->root(), transcript_whole.wire_output->root()));

  // The limit bounds the parts of the answer, not its chunks: two parts fit a limit of two however many chunks make them,
  // and a third does not.
  SemanticLimits tight;
  tight.max_parts = 2;
  chunks.clear();
  for (size_t i = 0; i < n; ++i) chunks.push_back(body(R"([{"text":"t","thought":true}])", "", "null"));
  for (size_t i = 0; i < n; ++i) chunks.push_back(body(R"([{"text":"x"}])", "", "null"));
  chunks.push_back(body(R"([{"text":""}])"));
  CHECK(complete(stream_all(chunks, tight)).messages.at(0).parts.size() == 2);
  chunks.insert(chunks.end() - 1, body(R"([{"text":"s","thoughtSignature":"SIG"}])", "", "null"));
  const auto refused = failure(stream_all(chunks, tight), ErrorKind::ResourceLimit);
  CHECK(refused.error.reason == "gemini.part.limit");
}
}
int main() {
  try { http_error_envelope_in_an_array_is_retained(); output_total_without_thoughts(); total_proves_zero_thoughts(); total_without_candidates_bounds_the_output(); portable_foreign_history(); typed_generation_controls(); raw_observation_ownership(); named_error_raw_ownership(); terminal_with_omitted_parts(); tool_result_helper_needs_a_json_object(); successive_unidentified_calls(); local_ids_carry_a_random_token_per_response(); native_group_and_replay(); nullable_usage(); uncached_input_rule(); stop_kinds_match_policy_table(); invalid_model_calls(); errors_and_terminals(); loaded_routes_encode_for_their_model(); empty_text_delta_is_withheld(); streamed_chunks_continue_their_part(); one_descriptor_serves_every_model(); served_model_is_recorded(); failure_reasons(); std::cout << "Gemini semantic invariants passed\n"; }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
