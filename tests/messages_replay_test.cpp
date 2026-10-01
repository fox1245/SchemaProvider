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
static_assert(std::variant_size_v<Event> == 10);

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
  const auto expected = document(R"({"model":"fixture-model","messages":[{"role":"user","content":[{"type":"text","text":"Hello"}]}],"stream":true,"max_tokens":1024})");
  CHECK(json::equal(document(wire.body)->root(), expected->root()));
  CHECK(wire.context->model() == "fixture-model" && wire.context->matches_descriptor(descriptor));
  size_t version_headers = 0;
  for (const auto& [name, value] : wire.headers) if (name == "anthropic-version") { ++version_headers; CHECK(value == "2023-06-01"); }
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
  CHECK(document(encoded(descriptor, request).body)->root().get("max_tokens").as_uint() == 0);
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
  constexpr size_t limit = 16 << 20;
  const auto descriptor = descriptor_value();
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
  for (unsigned i = 0; i < 43; ++i)
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
  request.tools[0].input_schema = document("{\"nested\":" + std::string(60, '[') + "0" + std::string(60, ']') + "}");
  CHECK(document(encoded(descriptor, request).body)->root().get("tools").at(0).get("input_schema").get("nested").is_array());
  request.tools[0].input_schema = document("{\"nested\":" + std::string(61, '[') + "0" + std::string(61, ']') + "}");
  rejected(descriptor, request, ErrorKind::InvalidRequest);
  request.tools[0].input_schema = document(R"({"values":[-9223372036854775808,18446744073709551615,-0.0,1e300,1e-300]})");
  auto body = document(encoded(descriptor, request).body);
  CHECK(json::equal(body->root().get("tools").at(0).get("input_schema"), request.tools[0].input_schema->root()));
}
} // namespace
int main() {
  try {
    typed_wire_request(); native_retention_and_content_integrity(); prefix_and_origin_binding();
    immutable_context_and_failed_capture(); client_loop_ownership(); server_loop_ownership_and_pause(); signature_accumulation_and_limits();
    invalid_calls_are_not_repaired();
    bounded_json_serialization(); request_encoded_byte_boundaries(); request_json_encoding_and_depth();
    std::cout << "Messages replay properties passed\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
