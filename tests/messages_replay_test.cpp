#include "codecs/messages.h"
#include "codecs/messages_request.h"
#include "core/native.h"
#include "json/json.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
static_assert(!std::is_default_constructible_v<NativeContext>);
static_assert(!std::is_aggregate_v<NativeContext>);
static_assert(!std::is_default_constructible_v<NativeReplay>);
static_assert(!std::is_aggregate_v<NativeReplay>);

descriptor::ValidatedDescriptor descriptor_value(std::string_view base = "http://127.0.0.1:18080", std::string_view route = "/v1/messages", std::string_view header = "synthetic") {
  const std::string source = R"({"descriptor_version":1,"revision":1,"id":"synthetic-messages","family":"anthropic.messages","connection":{"base_url":)" + json::quote(base) + R"(,"paths":{"buffered":)" + json::quote(route) + R"(,"streaming":)" + json::quote(route) + R"(},"headers":{"anthropic-version":"2023-06-01","X-Fixture":)" + json::quote(header) + "}}}";
  auto parsed = descriptor::load(source);
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(parsed));
  return std::get<descriptor::ValidatedDescriptor>(std::move(parsed));
}
std::shared_ptr<const json::Document> document(std::string_view bytes) {
  auto parsed = json::parse(bytes); CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::get<json::Document>(std::move(parsed)));
}
Message user(std::string text) { return Message{{}, Role::User, {Text{std::move(text)}}}; }
messages::Request base_request() {
  messages::Request request; request.model = "fixture-model"; request.account_scope = "fixture-account";
  request.messages.push_back(user("Hello")); return request;
}
messages::EncodedRequest encoded(const descriptor::ValidatedDescriptor& descriptor, const messages::Request& request, bool stream = false) {
  auto result = messages::encode(descriptor, request, stream);
  CHECK(std::holds_alternative<messages::EncodedRequest>(result));
  return std::get<messages::EncodedRequest>(std::move(result));
}
void rejected(const descriptor::ValidatedDescriptor& descriptor, const messages::Request& request, ErrorKind kind = ErrorKind::ReplayIneligible, bool stream = false) {
  const auto result = messages::encode(descriptor, request, stream);
  CHECK(std::holds_alternative<Error>(result));
  const auto& error = std::get<Error>(result); CHECK(error.kind == kind);
  for (const auto marker : {"SECRET_SIGNATURE", "SECRET_REDACTED", "SECRET_SERVER"}) CHECK(error.safe_message.find(marker) == std::string::npos);
}
std::string response(std::string_view content, std::string_view stop = "end_turn", std::string_view id = "msg_fixture") {
  return "{\"id\":" + json::quote(id) + ",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"fixture-model\",\"content\":" + std::string(content) + ",\"stop_reason\":" + json::quote(stop) + ",\"stop_sequence\":null,\"usage\":{\"input_tokens\":2,\"output_tokens\":3,\"cache_read_input_tokens\":0,\"cache_creation_input_tokens\":0}}";
}
Message capture(const descriptor::ValidatedDescriptor& descriptor, const messages::Request& request, std::string_view content, std::string_view stop = "end_turn", bool normal = true, std::string_view id = "msg_fixture") {
  const auto wire = encoded(descriptor, request);
  Accumulator accumulator;
  messages::Codec codec(descriptor, messages::Mode::Buffered, accumulator, wire.context);
  codec.buffered(response(content, stop, id), {normal, ErrorKind::Truncated});
  codec.finish(); CHECK(accumulator.outcome());
  if (normal) {
    CHECK(std::holds_alternative<Completion>(*accumulator.outcome()));
    auto message = std::get<Completion>(*accumulator.outcome()).messages.at(0);
    CHECK(message.native && message.native->complete()); return message;
  }
  CHECK(std::holds_alternative<Failure>(*accumulator.outcome()));
  auto message = std::get<Failure>(*accumulator.outcome()).partial.messages.at(0);
  CHECK(message.native && !message.native->complete()); return message;
}
const std::string thinking_content = R"([{"type":"thinking","thinking":"","signature":""},{"type":"thinking","thinking":"unsigned"},{"type":"thinking","thinking":"reason","signature":"SECRET_SIGNATURE"},{"type":"redacted_thinking","data":"SECRET_REDACTED"},{"type":"text","text":"Answer"}])";
const std::string client_content = R"([{"type":"thinking","thinking":"consider","signature":"SECRET_SIGNATURE"},{"type":"tool_use","id":"call_a","name":"f","input":{"x":1},"caller":{"type":"direct"}},{"type":"tool_use","id":"call_b","name":"g","input":{}}])";
const std::string server_call = R"({"type":"server_tool_use","id":"srvtoolu_a","name":"web_search","input":{"query":"fixture"}})";
const std::string server_result = R"({"type":"web_search_tool_result","tool_use_id":"srvtoolu_a","content":[{"type":"web_search_result","url":"https://example.test","title":"fixture","encrypted_content":"SECRET_SERVER"}]})";
void add_client_tools(messages::Request& request) {
  request.tools.push_back({"f", "first", document(R"({"type":"object","properties":{"x":{"type":"integer"}}})"), {}, {}});
  request.tools.push_back({"g", "second", document(R"({"type":"object"})"), {}, {}});
}
void add_server_tool(messages::Request& request) { request.tools.push_back({"web_search", {}, {}, "web_search_20250305", 2}); }
Message client_results() { return Message{{}, Role::User, {ToolResult{"call_a", "first result", false}, ToolResult{"call_b", "second error", true}}}; }
void typed_wire_request() {
  const auto descriptor = descriptor_value(); auto request = base_request();
  auto wire = encoded(descriptor, request, true);
  CHECK(wire.method == "POST" && wire.path == "/v1/messages");
  CHECK(wire.context->model() == "fixture-model" && wire.context->matches_descriptor(descriptor));
  size_t version_headers = 0;
  for (const auto& [name, value] : wire.headers) if (name == "anthropic-version") ++version_headers;
  CHECK(version_headers == 1);
  CHECK(wire.body.find("fixture-account") == std::string::npos);
  request.system = "be precise"; request.max_tokens = 2048; request.thinking_budget = 1024;
  request.top_p = 0.95; add_client_tools(request); add_server_tool(request);
  auto rich = document(encoded(descriptor, request).body);
  CHECK(rich->root().get("system").as_string() == "be precise");
  CHECK(rich->root().get("thinking").get("type").as_string() == "enabled");
  CHECK(rich->root().get("thinking").get("budget_tokens").as_uint() == 1024);
  CHECK(rich->root().get("tools").at(0).get("input_schema").get("properties").get("x").get("type").as_string() == "integer");
  CHECK(rich->root().get("tools").at(2).get("max_uses").as_uint() == 2);
  request.thinking_budget.reset(); request.max_tokens = 0;
  rejected(descriptor, request, ErrorKind::InvalidRequest);
  request.temperature = std::numeric_limits<double>::infinity(); rejected(descriptor, request, ErrorKind::InvalidRequest);
  request.temperature.reset(); request.thinking_budget = 1023; request.max_tokens = 2048; rejected(descriptor, request, ErrorKind::InvalidRequest);
  request.thinking_budget = 2048; rejected(descriptor, request, ErrorKind::InvalidRequest);
  request.thinking_budget = 1024; request.top_p = 0.9; rejected(descriptor, request, ErrorKind::InvalidRequest);
  request.thinking_budget.reset(); request.top_p.reset(); request.messages[0].role = Role::System; rejected(descriptor, request, ErrorKind::Unsupported);
  request.messages[0].role = Role::User; request.tools.back().type = "arbitrary_raw_type"; rejected(descriptor, request, ErrorKind::Unsupported);
}
void native_retention_and_content_integrity() {
  const auto descriptor = descriptor_value(); auto request = base_request();
  request.messages.push_back(capture(descriptor, request, thinking_content)); request.messages.push_back(user("Continue"));
  auto body = document(encoded(descriptor, request, true).body);
  CHECK(json::equal(body->root().get("messages").at(1).get("content"), document(thinking_content)->root()));
  const auto& parts = request.messages[1].parts;
  CHECK(std::get<Thinking>(parts[0]).signature == std::optional<std::string>(""));
  CHECK(!std::get<Thinking>(parts[1]).signature);
  CHECK(std::get<RedactedThinking>(parts[3]).data == "SECRET_REDACTED");
  const std::vector<std::function<void(messages::Request&)>> mutations{
    [](auto& r) { std::get<Thinking>(r.messages[1].parts[0]).signature.reset(); },
    [](auto& r) { std::get<Thinking>(r.messages[1].parts[1]).signature = ""; },
    [](auto& r) { std::get<Thinking>(r.messages[1].parts[2]).text += "changed"; },
    [](auto& r) { *std::get<Thinking>(r.messages[1].parts[2]).signature += "changed"; },
    [](auto& r) { std::get<RedactedThinking>(r.messages[1].parts[3]).data += "changed"; },
    [](auto& r) { std::get<Text>(r.messages[1].parts[4]).value += "changed"; },
    [](auto& r) { r.messages[1].parts.erase(r.messages[1].parts.begin()); },
    [](auto& r) { std::swap(r.messages[1].parts[0], r.messages[1].parts[1]); },
    [](auto& r) { r.messages[1].native.reset(); },
    [](auto& r) { r.messages[1].id = "other"; },
    [](auto& r) { r.messages[1].role = Role::User; },
    [](auto& r) { r.messages[1].parts.clear(); }
  };
  for (const auto& mutate : mutations) { auto changed = request; mutate(changed); rejected(descriptor, changed); }
  auto imported = base_request(); imported.messages.push_back(Message{{}, Role::Assistant, {Thinking{"", "SECRET_SIGNATURE"}}});
  rejected(descriptor, imported);
  imported.messages.back().parts = {RedactedThinking{"SECRET_REDACTED"}}; rejected(descriptor, imported);
  imported.messages.back().parts = {ServerToolResult{"srvtoolu_a", "web_search_tool_result", document(server_result)}}; rejected(descriptor, imported);
  // A genuine seal cannot authorize a different mutable message, even if ids match.
  imported.messages.back().native = request.messages[1].native; rejected(descriptor, imported);
  auto empty = base_request(); empty.messages.push_back(capture(descriptor, empty, "[]")); empty.messages.push_back(user("next"));
  CHECK(document(encoded(descriptor, empty).body)->root().get("messages").at(1).get("content").size() == 0);
}
void prefix_and_origin_binding() {
  const auto descriptor = descriptor_value(); auto request = base_request(); add_client_tools(request);
  request.system = "original"; request.max_tokens = 2048; request.thinking_budget = 1024;
  request.messages.push_back(capture(descriptor, request, thinking_content)); request.messages.push_back(user("second"));
  const auto original_native = request.messages[1].native;
  for (const uint64_t cap : {2048ULL, 4096ULL, 8192ULL, 16384ULL}) {
    request.max_tokens = cap;
    const auto growth_wire = encoded(descriptor, request);
    CHECK(growth_wire.max_output_tokens == cap);
    const auto growth_body = document(growth_wire.body);
    CHECK(growth_body->root().get("max_tokens").as_uint() == cap);
    CHECK(json::equal(growth_body->root().get("messages").at(1).get("content"), document(thinking_content)->root()));
    CHECK(request.messages[1].native == original_native);
  }
  request.messages.push_back(capture(descriptor, request, R"([{"type":"text","text":"second answer"}])", "end_turn", true, "msg_second"));
  request.messages.push_back(user("third"));
  const auto body = document(encoded(descriptor, request).body);
  CHECK(body->root().get("messages").at(3).get("content").at(0).get("text").as_string() == "second answer");
  const std::vector<std::function<void(messages::Request&)>> mutations{
    [](auto& r) { r.model = "other-model"; }, [](auto& r) { r.account_scope = "other-account"; },
    [](auto& r) { r.system += "changed"; }, [](auto& r) { r.thinking_budget = 1100; },
    [](auto& r) { r.tools[0].description += "changed"; },
    [](auto& r) { r.tools[0].input_schema = document(R"({"type":"object","properties":{"y":{"type":"string"}}})"); },
    [](auto& r) { std::swap(r.tools[0], r.tools[1]); },
    [](auto& r) { std::get<Text>(r.messages[0].parts[0]).value = "changed"; },
    [](auto& r) { std::get<Text>(r.messages[2].parts[0]).value = "changed"; },
    [](auto& r) { r.messages.erase(r.messages.begin() + 2); },
    [](auto& r) { r.messages.insert(r.messages.begin(), user("injected")); }
  };
  for (const auto& mutate : mutations) { auto changed = request; mutate(changed); rejected(descriptor, changed); }
  rejected(descriptor_value("http://127.0.0.1:18081"), request);
  rejected(descriptor_value("http://127.0.0.1:18080", "/other/messages"), request);
  rejected(descriptor_value("http://127.0.0.1:18080", "/v1/messages", "changed-policy"), request);
  // Canonical JSON objects ignore key order, while opaque leaves remain exact.
  auto canonical = request;
  canonical.tools[0].input_schema = document(R"({"properties":{"x":{"type":"integer"}},"type":"object"})");
  CHECK(document(encoded(descriptor, canonical).body)->root().get("messages").at(4).get("content").at(0).get("text").as_string() == "third");
  auto collision = base_request(); collision.system = "ab"; collision.account_scope = "c";
  collision.messages.push_back(capture(descriptor, collision, thinking_content)); collision.messages.push_back(user("next"));
  collision.system = "a"; collision.account_scope = "bc"; rejected(descriptor, collision);
}
void immutable_context_and_failed_capture() {
  const auto descriptor = descriptor_value(); auto initial = base_request();
  const auto wire = encoded(descriptor, initial);
  std::get<Text>(initial.messages[0].parts[0]).value = "edited after encode";
  Accumulator accumulator; messages::Codec codec(descriptor, messages::Mode::Buffered, accumulator, wire.context);
  CHECK(codec.buffered(response(thinking_content), {})); codec.finish();
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  initial.messages.push_back(std::get<Completion>(*accumulator.outcome()).messages[0]); initial.messages.push_back(user("next"));
  rejected(descriptor, initial);
  for (const auto content : {std::string_view(thinking_content), std::string_view(client_content)}) {
    auto partial = base_request(); add_client_tools(partial);
    partial.messages.push_back(capture(descriptor, partial, content, content == client_content ? "tool_use" : "end_turn", false));
    if (content == client_content) partial.messages.push_back(client_results()); else partial.messages.push_back(user("continue"));
    rejected(descriptor, partial);
    partial.messages[1].native.reset(); rejected(descriptor, partial);
  }
  // An SSE body that never reaches message_stop cannot mint a replayable capsule.
  auto partial = base_request(); const auto prepared = encoded(descriptor, partial, true);
  Accumulator stream; messages::Codec streaming(descriptor, messages::Mode::Sse, stream, prepared.context);
  CHECK(streaming.frame("message_start", R"({"type":"message_start","message":{"id":"msg_sse","type":"message","role":"assistant","model":"fixture-model","content":[],"stop_reason":null,"stop_sequence":null,"usage":{"input_tokens":1,"output_tokens":0}}})"));
  CHECK(streaming.frame("content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":"","signature":""}})"));
  CHECK(streaming.frame("content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"partial"}})"));
  streaming.finish(); CHECK(std::holds_alternative<Failure>(*stream.outcome()));
  auto message = std::get<Failure>(*stream.outcome()).partial.messages[0]; CHECK(message.native && !message.native->complete());
  partial.messages.push_back(message); partial.messages.push_back(user("continue")); rejected(descriptor, partial);
}
void client_loop_ownership() {
  const auto descriptor = descriptor_value(); auto request = base_request(); add_client_tools(request);
  request.messages.push_back(capture(descriptor, request, client_content, "tool_use")); request.messages.push_back(client_results());
  auto body = document(encoded(descriptor, request).body);
  CHECK(json::equal(body->root().get("messages").at(1).get("content"), document(client_content)->root()));
  auto results = body->root().get("messages").at(2).get("content");
  CHECK(results.at(0).get("tool_use_id").as_string() == "call_a" && !results.at(0).get("is_error").as_bool());
  CHECK(results.at(1).get("tool_use_id").as_string() == "call_b" && results.at(1).get("is_error").as_bool());
  for (const auto& mutate : std::vector<std::function<void(messages::Request&)>>{
    [](auto& r) { std::get<ToolCall>(r.messages[1].parts[1]).id = "foreign"; },
    [](auto& r) { std::get<ToolCall>(r.messages[1].parts[1]).input = document(R"({"x":2})"); },
    [](auto& r) { std::get<ToolCall>(r.messages[1].parts[1]).wire_metadata = document(R"({"caller":{"type":"code_execution_20250825","tool_id":"foreign"}})"); },
    [](auto& r) { r.messages[1].parts.erase(r.messages[1].parts.begin()); },
    [](auto& r) { r.messages[1].native.reset(); }
  }) { auto changed = request; mutate(changed); rejected(descriptor, changed); }
  for (const auto& mutate : std::vector<std::function<void(messages::Request&)>>{
    [](auto& r) { r.messages.pop_back(); },
    [](auto& r) { r.messages[2].parts.pop_back(); },
    [](auto& r) { r.messages[2].parts.push_back(ToolResult{"call_a", "duplicate", false}); },
    [](auto& r) { std::get<ToolResult>(r.messages[2].parts[0]).tool_use_id = "unknown"; },
    [](auto& r) { r.messages[2].parts.insert(r.messages[2].parts.begin(), Text{"before result"}); },
    [](auto& r) { r.messages.insert(r.messages.begin() + 2, user("intervening")); },
    [](auto& r) { r.messages[2].role = Role::Assistant; }
  }) { auto changed = request; mutate(changed); rejected(descriptor, changed, ErrorKind::InvalidRequest); }
  auto raw = base_request(); raw.messages.push_back(Message{{}, Role::Assistant, {ToolCall{"call_a", "f", ToolCallKind::ClientExecuted, document("{}"), "tool_use", {}}}});
  raw.messages.push_back(Message{{}, Role::User, {ToolResult{"call_a", "unprovenanced", false}}}); rejected(descriptor, raw);
  auto orphan = base_request(); orphan.messages.push_back(Message{{}, Role::User, {ToolResult{"call_a", "orphan", false}}}); rejected(descriptor, orphan, ErrorKind::InvalidRequest);
  // A second captured response binds the tool-result contents from its own prefix.
  request.messages.push_back(capture(descriptor, request, R"([{"type":"text","text":"resolved"}])", "end_turn", true, "msg_resolved")); request.messages.push_back(user("next"));
  CHECK(document(encoded(descriptor, request).body)->root().get("messages").at(3).get("content").at(0).get("text").as_string() == "resolved");
  std::get<ToolResult>(request.messages[2].parts[0]).content += "changed"; rejected(descriptor, request);
}
void server_loop_ownership_and_pause() {
  const auto descriptor = descriptor_value(); auto request = base_request(); add_server_tool(request);
  const auto whole = "[" + server_call + "," + server_result + "]";
  request.messages.push_back(capture(descriptor, request, whole)); request.messages.push_back(user("next"));
  CHECK(json::equal(document(encoded(descriptor, request).body)->root().get("messages").at(1).get("content"), document(whole)->root()));
  auto mutation = request; std::get<ServerToolResult>(mutation.messages[1].parts[1]).content = document(R"({"type":"web_search_tool_result","tool_use_id":"srvtoolu_a","content":[]})"); rejected(descriptor, mutation);
  mutation = request; mutation.messages[1].native.reset(); rejected(descriptor, mutation);
  mutation = request; mutation.messages[2].parts = {ToolResult{"srvtoolu_a", "must not execute server tool", false}}; rejected(descriptor, mutation, ErrorKind::InvalidRequest);
  auto pause = base_request(); add_server_tool(pause);
  pause.messages.push_back(capture(descriptor, pause, "[" + server_call + "]", "pause_turn"));
  const auto paused = encoded(descriptor, pause);
  CHECK(paused.context->server_tool_name("srvtoolu_a") == std::optional<std::string_view>("web_search"));
  CHECK(json::equal(document(paused.body)->root().get("messages").at(1).get("content"), document("[" + server_call + "]")->root()));
  mutation = pause; mutation.messages.push_back(user("break server loop")); rejected(descriptor, mutation, ErrorKind::InvalidRequest);
  pause.messages.push_back(capture(descriptor, pause, "[" + server_result + ",{\"type\":\"text\",\"text\":\"done\"}]", "end_turn", true, "msg_server_done")); pause.messages.push_back(user("next"));
  const auto resumed = encoded(descriptor, pause); CHECK(!resumed.context->server_tool_name("srvtoolu_a"));
  CHECK(document(resumed.body)->root().get("messages").at(2).get("content").at(0).get("content").at(0).get("encrypted_content").as_string() == "SECRET_SERVER");
  auto mixed = base_request(); add_server_tool(mixed); add_client_tools(mixed);
  const auto mixed_content = "[" + server_call + ",{\"type\":\"tool_use\",\"id\":\"call_a\",\"name\":\"f\",\"input\":{}}]";
  mixed.messages.push_back(capture(descriptor, mixed, mixed_content, "tool_use")); mixed.messages.push_back(Message{{}, Role::User, {ToolResult{"call_a", "done", false}}});
  CHECK(encoded(descriptor, mixed).context->server_tool_name("srvtoolu_a") == std::optional<std::string_view>("web_search"));
  mixed.messages.back().parts.push_back(Text{"must not end pending server turn"}); rejected(descriptor, mixed, ErrorKind::InvalidRequest);
}
void signature_accumulation_and_limits() {
  const auto descriptor = descriptor_value(); const auto request = base_request(); const auto prepared = encoded(descriptor, request);
  Accumulator accumulator;
  CHECK(accumulator.accept(Begin{"signature"})); CHECK(accumulator.accept(MessageBegin{{0}, "msg_signature", Role::Assistant, prepared.context}));
  CHECK(accumulator.accept(PartBegin{{0}, {0}, PartKind::Thinking, {}, 0}));
  CHECK(accumulator.accept(PartDelta{{0}, {PartKind::Thinking, "thought"}}));
  CHECK(accumulator.accept(PartDelta{{0}, {PartKind::Thinking, "SECRET_", DeltaChannel::Signature}}));
  CHECK(accumulator.accept(PartDelta{{0}, {PartKind::Thinking, "SIGNATURE", DeltaChannel::Signature}}));
  CHECK(accumulator.accept(PartSeal{{0}, {}})); CHECK(accumulator.accept(MessageSeal{{0}}));
  CHECK(accumulator.accept(Stop{{StopKind::EndTurn, "end_turn"}})); CHECK(accumulator.accept(Commit{"synthetic"}));
  const auto& message = std::get<Completion>(*accumulator.outcome()).messages[0];
  CHECK(std::get<Thinking>(message.parts[0]).text == "thought"); CHECK(std::get<Thinking>(message.parts[0]).signature == "SECRET_SIGNATURE");
  auto replay = request; replay.messages.push_back(message); replay.messages.push_back(user("next"));
  CHECK(document(encoded(descriptor, replay).body)->root().get("messages").at(1).get("content").at(0).get("signature").as_string() == "SECRET_SIGNATURE");
  Accumulator wrong; CHECK(wrong.accept(Begin{"wrong"})); CHECK(wrong.accept(MessageBegin{{0}, {}, Role::Assistant})); CHECK(wrong.accept(PartBegin{{0}, {0}, PartKind::Text, {}, 0}));
  CHECK(!wrong.accept(PartDelta{{0}, {PartKind::Text, "SECRET_SIGNATURE", DeltaChannel::Signature}})); CHECK(std::get<Failure>(*wrong.outcome()).error.kind == ErrorKind::ProtocolCorrupt);
  SemanticLimits limits; limits.max_content_bytes = 5;
  Accumulator bounded(limits); CHECK(bounded.accept(Begin{"bounded"})); CHECK(bounded.accept(MessageBegin{{0}, {}, Role::Assistant})); CHECK(bounded.accept(PartBegin{{0}, {0}, PartKind::Thinking, {}, 0}));
  CHECK(bounded.accept(PartDelta{{0}, {PartKind::Thinking, "abc"}})); CHECK(bounded.accept(PartDelta{{0}, {PartKind::Thinking, "de", DeltaChannel::Signature}}));
  CHECK(!bounded.accept(PartDelta{{0}, {PartKind::Thinking, "f", DeltaChannel::Signature}})); CHECK(std::get<Failure>(*bounded.outcome()).error.kind == ErrorKind::ResourceLimit);
}
void invalid_calls_are_not_repaired() {
  const auto descriptor = descriptor_value(); auto request = base_request(); add_client_tools(request);
  const auto prepared = encoded(descriptor, request, true);
  Accumulator accumulator; messages::Codec codec(descriptor, messages::Mode::Sse, accumulator, prepared.context);
  CHECK(codec.frame("message_start", R"({"type":"message_start","message":{"id":"msg_invalid","type":"message","role":"assistant","model":"fixture-model","content":[],"stop_reason":null,"stop_sequence":null,"usage":{"input_tokens":1,"output_tokens":0}}})"));
  CHECK(codec.frame("content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"call_a","name":"f","input":{}}})"));
  CHECK(codec.frame("content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"x\":"}})"));
  CHECK(codec.frame("content_block_stop", R"({"type":"content_block_stop","index":0})"));
  CHECK(codec.frame("message_delta", R"({"type":"message_delta","delta":{"stop_reason":"max_tokens","stop_sequence":null},"usage":{"output_tokens":2}})"));
  CHECK(codec.frame("message_stop", R"({"type":"message_stop"})")); codec.finish();
  CHECK(std::holds_alternative<Completion>(*accumulator.outcome()));
  const auto& completion = std::get<Completion>(*accumulator.outcome());
  const auto& invalid = std::get<InvalidToolCall>(completion.messages[0].parts[0]);
  CHECK(invalid.reason == InvalidReason::Truncated && invalid.raw_fragment == "{\"x\":");
  request.messages.push_back(completion.messages[0]); request.messages.push_back(Message{{}, Role::User, {ToolResult{"call_a", "invalid arguments", true}}});
  rejected(descriptor, request, ErrorKind::Unsupported);
  request.messages[1].native.reset(); rejected(descriptor, request);
}
void bounded_json_serialization() {
  // Compare to the existing port, including control escapes and a multibyte
  // character crossing the bounded serializer's internal chunk boundary.
  std::string text(4095, 'x');
  text += "\xf0\x9f\x98\x80";
  for (unsigned c = 0; c < 32; ++c) text += static_cast<char>(c);
  text += "\"\\/\x7f";
  const auto expected = json::quote(text);
  json::BoundedWriter measured({expected.size(), 64});
  measured.quoted(text);
  CHECK(measured.ok() && measured.size() == expected.size());
  std::string output; output.reserve(measured.size());
  json::BoundedWriter writer({measured.size(), 64}, &output);
  writer.quoted(text);
  CHECK(writer.ok() && output == expected);
  output.clear();
  json::BoundedWriter short_writer({expected.size() - 1, 64}, &output);
  short_writer.quoted(text).raw("must not append after failure");
  CHECK(!short_writer.ok() && output.empty());
  for (const auto& invalid : {std::string("\xc0\x80"), std::string("\xed\xa0\x80"),
                             std::string("\xf4\x90\x80\x80"), std::string(4095, 'x') + "\xe2\x82"}) {
    output.clear();
    json::BoundedWriter invalid_writer({8192, 64}, &output);
    invalid_writer.quoted(invalid);
    CHECK(!invalid_writer.ok());
  }
  const auto values = document(R"({"a\u0000":[-9223372036854775808,18446744073709551615,-0.0,1.2345678901234567,1e300,1e-300,true,false,null,"\u0000"]})");
  const auto serialized = values->root().dump();
  output.clear(); output.reserve(serialized.size());
  json::BoundedWriter documents({serialized.size(), 2}, &output);
  documents.value(values->root());
  CHECK(documents.ok() && output == serialized);
  json::BoundedWriter too_deep({serialized.size(), 1});
  too_deep.value(values->root());
  CHECK(!too_deep.ok());
  json::BoundedWriter aggregate({serialized.size() * 2 - 1, 64});
  aggregate.value(values->root()).value(values->root());
  CHECK(!aggregate.ok());
}
void request_encoded_byte_boundaries() {
  const auto descriptor = descriptor_value();
  const auto limit = descriptor.policy()->resources().request_bytes;
  // Exact boundary is derived from an actual empty request, not a duplicated
  // spelling of the encoder. Each NUL consumes six wire bytes, not one.
  auto request = base_request();
  auto& text = std::get<Text>(request.messages[0].parts[0]).value;
  text.clear();
  const auto overhead = encoded(descriptor, request).body.size();
  text.assign((limit - overhead) / 6, '\0');
  text.append((limit - overhead) % 6, 'x');
  {
    const auto wire = encoded(descriptor, request);
    CHECK(wire.body.size() == limit);
    CHECK(wire.body.find("\\u0000\\u0000") != std::string::npos);
  }
  text += 'x';
  rejected(descriptor, request, ErrorKind::InvalidRequest);
  // Many individually legal strings must share the same aggregate allowance.
  request = base_request();
  std::get<Text>(request.messages[0].parts[0]).value.clear();
  request.messages.push_back(user(""));
  const auto aggregate_overhead = encoded(descriptor, request).body.size();
  const auto payload_bytes = limit - aggregate_overhead;
  auto& first = std::get<Text>(request.messages[0].parts[0]).value;
  auto& second = std::get<Text>(request.messages[1].parts[0]).value;
  first.assign(payload_bytes / 12, '\0');
  second.assign(payload_bytes / 6 - first.size(), '\0');
  second.append(payload_bytes % 6, 'x');
  CHECK(encoded(descriptor, request).body.size() == limit);
  second += 'x';
  rejected(descriptor, request, ErrorKind::InvalidRequest);
  // All caller string surfaces, not only message text, need escaped-size bounds.
  for (int field = 0; field < 4; ++field) {
    request = base_request();
    add_client_tools(request);
    auto& target = field == 0 ? request.model : field == 1 ? request.system :
        field == 2 ? request.tools[0].name : request.tools[0].description;
    target.assign(limit / 6 + 1, '\0');
    rejected(descriptor, request, ErrorKind::InvalidRequest);
  }
  // Shared small DOMs expand cumulatively; no huge static/global fixture needed.
  request = base_request();
  const auto schema = document("{\"value\":" + json::quote(std::string(65536, '\0')) + "}");
  const auto schema_bytes = schema->root().dump().size();
  for (size_t i = 0; i <= limit / schema_bytes; ++i)
    request.tools.push_back({"tool_" + std::to_string(i), {}, schema, {}, {}});
  rejected(descriptor, request, ErrorKind::InvalidRequest);
  // Replay admission remains ahead of wire-size admission, even for edited data.
  request = base_request();
  request.messages.push_back(capture(descriptor, request, thinking_content));
  request.system.assign(limit / 6 + 1, '\0');
  rejected(descriptor, request);
}
void request_json_encoding_and_depth() {
  const auto descriptor = descriptor_value();
  auto request = base_request();
  const std::string invalid("\xed\xa0\x80");
  for (int field = 0; field < 5; ++field) {
    request = base_request(); add_client_tools(request);
    auto& target = field == 0 ? request.model : field == 1 ? request.system :
        field == 2 ? request.tools[0].name : field == 3 ? request.tools[0].description :
        std::get<Text>(request.messages[0].parts[0]).value;
    target = invalid;
    rejected(descriptor, request, ErrorKind::InvalidRequest);
  }
  request = base_request(); add_client_tools(request);
  const auto array_depth = descriptor.policy()->resources().json_depth - 4;
  request.tools[0].input_schema = document("{\"nested\":" + std::string(array_depth, '[') + "0" + std::string(array_depth, ']') + "}");
  CHECK(document(encoded(descriptor, request).body)->root().get("tools").at(0).get("input_schema").get("nested").is_array());
  request.tools[0].input_schema = document("{\"nested\":" + std::string(array_depth + 1, '[') + "0" + std::string(array_depth + 1, ']') + "}");
  rejected(descriptor, request, ErrorKind::InvalidRequest);
  request.tools[0].input_schema = document(R"({"values":[-9223372036854775808,18446744073709551615,-0.0,1e300,1e-300]})");
  auto body = document(encoded(descriptor, request).body);
  CHECK(json::equal(body->root().get("tools").at(0).get("input_schema"), request.tools[0].input_schema->root()));
}
std::string replace_member(json::Value object, std::string_view key, std::string_view raw) {
  std::string result = "{";
  bool present = false;
  for (auto member : object.members()) {
    if (result.size() > 1) result += ',';
    result += json::quote(member.key) + ':';
    if (member.key == key) { result += raw; present = true; }
    else result += member.value.dump();
  }
  if (!present) {
    if (result.size() > 1) result += ',';
    result += json::quote(key) + ':' + std::string(raw);
  }
  return result + '}';
}
descriptor::PolicySnapshot messages_policy(std::string_view cap, std::string_view header_version = "\"2023-06-01\"") {
  auto source = document(config_defaults::descriptor_policy_json);
  std::string families = "[";
  for (auto family : source->root().get("families").elements()) {
    if (families.size() > 1) families += ',';
    if (family.get("family").as_string() == "anthropic.messages") {
      auto changed_defaults = replace_member(family.get("defaults"), "max_output_tokens", cap);
      auto changed_family = document(replace_member(family, "defaults", changed_defaults));
      changed_family = document(replace_member(changed_family->root(), "header_versions", "[\"2023-06-01\",\"2026-01-01\"]"));
      families += replace_member(changed_family->root(), "header_version", header_version);
    } else families += family.dump();
  }
  families += ']';
  auto loaded = descriptor::load_policy(replace_member(source->root(), "families", families), config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(loaded));
  return std::get<descriptor::PolicySnapshot>(std::move(loaded));
}
descriptor::ValidatedDescriptor policy_descriptor(descriptor::PolicySnapshot policy) {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"policy-replay","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}}})", std::move(policy));
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
void effective_policy_lineage() {
  auto no_cap = policy_descriptor(messages_policy("null"));
  auto request = base_request();
  rejected(no_cap, request, ErrorKind::InvalidRequest);
  request.max_tokens = 20000;
  CHECK(document(encoded(no_cap, request).body)->root().get("max_tokens").as_uint() == 20000);

  auto original = policy_descriptor(messages_policy("257"));
  request.max_tokens.reset();
  auto sealed = capture(original, request, thinking_content);
  request.messages.push_back(sealed); request.messages.push_back(user("next"));
  request.max_tokens = 257; // Explicit and admitted default encode identically.
  encoded(original, request);
  request.max_tokens = 258;
  CHECK(document(encoded(original, request).body)->root().get("max_tokens").as_uint() == 258);
  CHECK(request.messages[1].native == sealed.native);
  CHECK(json::equal(document(encoded(original, request).body)->root().get("messages").at(1).get("content"), document(thinking_content)->root()));
  request.max_tokens = 257;
  auto reloaded = policy_descriptor(messages_policy("258"));
  rejected(reloaded, request); // A changed semantic policy is not old authority.
  encoded(original, request); // Existing snapshot and seal remain usable.
  auto version_changed = policy_descriptor(messages_policy("257", "\"2026-01-01\""));
  rejected(version_changed, request);
  auto resources = document(config_defaults::codec_defaults_json);
  auto resource_values = replace_member(resources->root().get("resources"), "request_bytes", "64");
  auto policy = descriptor::load_policy(config_defaults::descriptor_policy_json,
      replace_member(resources->root(), "resources", resource_values));
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(policy));
  auto bounded = policy_descriptor(std::get<descriptor::PolicySnapshot>(std::move(policy)));
  rejected(bounded, base_request(), ErrorKind::InvalidRequest);

  auto family_source = document(config_defaults::descriptor_policy_json);
  auto closed = descriptor::load_policy(replace_member(family_source->root(), "hooks", "{}"),
      config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::ConfigError>(closed));
}
void thinking_cache_tool_controls() {
  const auto descriptor = descriptor_value();
  auto request = base_request(); request.max_tokens = 2048; request.thinking_budget = 1024;
  request.thinking_mode = messages::ThinkingMode::Manual; request.temperature = 0.7; request.top_p = 0.95;
  auto wire = document(encoded(descriptor, request).body);
  CHECK(!wire->root().get("temperature").valid());
  CHECK(wire->root().get("top_p").as_double() == 0.95);
  CHECK(wire->root().get("thinking").get("budget_tokens").as_uint() == 1024);
  request.thinking_mode = messages::ThinkingMode::Adaptive;
  rejected(descriptor, request, ErrorKind::InvalidRequest);
  request.thinking_budget.reset(); request.output_effort = messages::OutputEffort::Max;
  request.cache_control = messages::CacheControl{messages::CacheTtl::OneHour};
  add_client_tools(request);
  request.tool_choice = messages::ToolChoice{messages::ToolChoiceMode::Auto, {}, false};
  wire = document(encoded(descriptor, request).body);
  CHECK(wire->root().get("thinking").get("type").as_string() == "adaptive");
  CHECK(!wire->root().get("thinking").get("budget_tokens").valid());
  CHECK(!wire->root().get("temperature").valid());
  CHECK(wire->root().get("output_config").get("effort").as_string() == "max");
  CHECK(wire->root().get("cache_control").get("type").as_string() == "ephemeral");
  CHECK(wire->root().get("cache_control").get("ttl").as_string() == "1h");
  CHECK(!wire->root().get("tool_choice").get("disable_parallel_tool_use").as_bool());
  auto changed = request; changed.top_p = 0.9; rejected(descriptor, changed, ErrorKind::InvalidRequest);
  changed = request; changed.model = "Anthropic/CLAUDE-OPUS-4-7"; rejected(descriptor, changed, ErrorKind::InvalidRequest);
  changed = request; changed.temperature = std::numeric_limits<double>::quiet_NaN(); rejected(descriptor, changed, ErrorKind::InvalidRequest);
  changed = request; changed.thinking_mode = static_cast<messages::ThinkingMode>(99); rejected(descriptor, changed, ErrorKind::InvalidRequest);
  changed = request; changed.output_effort = static_cast<messages::OutputEffort>(99); rejected(descriptor, changed, ErrorKind::InvalidRequest);
  changed = request; changed.cache_control->ttl = static_cast<messages::CacheTtl>(99); rejected(descriptor, changed, ErrorKind::InvalidRequest);
  request.thinking_mode = messages::ThinkingMode::Disabled;
  request.tool_choice = messages::ToolChoice{messages::ToolChoiceMode::Tool, "f", true};
  wire = document(encoded(descriptor, request).body);
  CHECK(wire->root().get("thinking").get("type").as_string() == "disabled");
  CHECK(wire->root().get("temperature").as_double() == 0.7);
  CHECK(wire->root().get("tool_choice").get("name").as_string() == "f");
  CHECK(wire->root().get("tool_choice").get("disable_parallel_tool_use").as_bool());
  changed = request; changed.thinking_budget = 1024; rejected(descriptor, changed, ErrorKind::InvalidRequest);
  changed = request; changed.tool_choice->name = "unknown"; rejected(descriptor, changed, ErrorKind::InvalidRequest);
  changed = request; changed.tool_choice->mode = static_cast<messages::ToolChoiceMode>(99); rejected(descriptor, changed, ErrorKind::InvalidRequest);
  changed = request; changed.tool_choice->mode = messages::ToolChoiceMode::Auto; rejected(descriptor, changed, ErrorKind::InvalidRequest);
  request.tool_choice = messages::ToolChoice{messages::ToolChoiceMode::Any, {}, {}};
  CHECK(document(encoded(descriptor, request).body)->root().get("tool_choice").get("type").as_string() == "any");
  request.tool_choice = messages::ToolChoice{messages::ToolChoiceMode::None, {}, {}};
  CHECK(document(encoded(descriptor, request).body)->root().get("tool_choice").get("type").as_string() == "none");
  for (const auto& [value, name] : std::vector<std::pair<messages::OutputEffort, std::string_view>>{
      {messages::OutputEffort::Low, "low"}, {messages::OutputEffort::Medium, "medium"},
      {messages::OutputEffort::High, "high"}, {messages::OutputEffort::Max, "max"}}) {
    auto effort_request = request; effort_request.output_effort = value;
    CHECK(document(encoded(descriptor, effort_request).body)->root().get("output_config").get("effort").as_string() == name);
  }
  auto cache_request = request; cache_request.cache_control->ttl = messages::CacheTtl::FiveMinutes;
  CHECK(document(encoded(descriptor, cache_request).body)->root().get("cache_control").get("ttl").as_string() == "5m");
  cache_request.cache_control->ttl.reset();
  CHECK(!document(encoded(descriptor, cache_request).body)->root().get("cache_control").get("ttl").valid());
  changed = request; changed.tool_choice->disable_parallel_tool_use = false; rejected(descriptor, changed, ErrorKind::InvalidRequest);
  changed = request; changed.thinking_mode = messages::ThinkingMode::Manual; rejected(descriptor, changed, ErrorKind::InvalidRequest);
  request.messages.push_back(capture(descriptor, request, R"([{"type":"text","text":"answer"}])"));
  request.messages.push_back(user("continue"));
  CHECK(std::holds_alternative<messages::EncodedRequest>(messages::encode(descriptor, request, false)));
  for (int field = 0; field != 3; ++field) {
    changed = request;
    if (field == 0) changed.thinking_mode = messages::ThinkingMode::Adaptive;
    if (field == 1) changed.output_effort = messages::OutputEffort::Low;
    if (field == 2) changed.cache_control->ttl = messages::CacheTtl::FiveMinutes;
    rejected(descriptor, changed);
  }
  changed = request; changed.tool_choice = messages::ToolChoice{messages::ToolChoiceMode::Auto, {}, true};
  CHECK(std::holds_alternative<messages::EncodedRequest>(messages::encode(descriptor, changed, false)));
  auto routing = base_request(); routing.provider = OpenRouterRouting{};
  routing.provider->order = {"anthropic"};
  rejected(descriptor, routing, ErrorKind::InvalidRequest);
  const auto gateway = descriptor_value("https://openrouter.ai", "/api/v1/messages");
  CHECK(document(encoded(gateway, routing).body)->root().get("provider").get("order").at(0).as_string() == "anthropic");
  routing.messages.push_back(capture(gateway, routing, R"([{"type":"text","text":"answer"}])"));
  routing.messages.push_back(user("continue"));
  CHECK(std::holds_alternative<messages::EncodedRequest>(messages::encode(gateway, routing, false)));
  routing.provider->order = {"other"};
  rejected(gateway, routing);
}
} // namespace
int main() {
  try {
    typed_wire_request(); native_retention_and_content_integrity(); prefix_and_origin_binding();
    immutable_context_and_failed_capture(); client_loop_ownership(); server_loop_ownership_and_pause(); signature_accumulation_and_limits();
    invalid_calls_are_not_repaired();
    bounded_json_serialization(); request_encoded_byte_boundaries(); request_json_encoding_and_depth();
    effective_policy_lineage();
    thinking_cache_tool_controls();
    std::cout << "Messages replay properties passed\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
