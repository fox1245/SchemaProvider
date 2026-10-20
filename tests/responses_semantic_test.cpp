#include "codecs/responses.h"
#include "codecs/responses_request.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <tuple>
#include <vector>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
using Frame = std::pair<std::string, std::string>;
json::Document parse(std::string_view text) {
  auto result = json::parse(text); CHECK(std::holds_alternative<json::Document>(result));
  return std::get<json::Document>(std::move(result));
}
std::shared_ptr<const json::Document> owned(std::string_view text) { return std::make_shared<const json::Document>(parse(text)); }
const descriptor::ValidatedDescriptor& desc() {
  static auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"responses-semantic","family":"openai.responses","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/responses","streaming":"/v1/responses"}},"bindings":{"model":"model","messages":"input","stream":"stream","max_output_tokens":"max_output_tokens","usage":["usage"]}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(loaded);
}
responses::Request request() {
  responses::Request r; r.model = "fixture-model"; r.account_scope = "fixture-account";
  r.instructions = "Answer briefly"; r.max_output_tokens = 128; r.reasoning = responses::ReasoningOptions{"low", "auto"};
  r.tools.push_back({"lookup", "Find a value", owned(R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"],"additionalProperties":false})"), true});
  r.messages.push_back(Message{"", Role::User, {Text{"Hello"}}}); return r;
}
std::shared_ptr<const NativeContext> context() {
  auto encoded = responses::encode(desc(), request(), true); CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
  return std::get<responses::EncodedRequest>(encoded).context;
}
const std::string usage = R"({"input_tokens":10,"output_tokens":7,"total_tokens":17,"input_tokens_details":{"cached_tokens":4},"output_tokens_details":{"reasoning_tokens":3}})";
const std::string reasoning = R"({"id":"rs_1","type":"reasoning","status":"completed","summary":[{"type":"summary_text","text":"Consider carefully"}],"encrypted_content":"SEALED_NATIVE_MARKER_a810"})";
const std::string message = R"({"id":"msg_1","type":"message","status":"completed","role":"assistant","phase":"final_answer","content":[{"type":"output_text","text":"hello","annotations":[]}]})";
const std::string call = R"({"id":"fc_1","type":"function_call","status":"completed","call_id":"call_1","name":"lookup","arguments":"{\"x\":1}"})";
std::string body(std::string_view output, std::string_view status = "completed", std::string_view counters = usage, std::string_view detail = "null") {
  return "{\"id\":\"resp_1\",\"object\":\"response\",\"created_at\":1,\"model\":\"fixture-model\",\"status\":" + json::quote(status)
    + ",\"output\":" + std::string(output) + ",\"usage\":" + std::string(counters) + ",\"incomplete_details\":" + std::string(detail) + ",\"error\":null}";
}
Frame event(std::string type, std::string fields = {}) { return {type, "{\"type\":" + json::quote(type) + std::move(fields) + "}"}; }
Frame created() { return event("response.created", ",\"response\":" + body("[]", "in_progress", "null")); }
Frame added(size_t i, std::string_view item) { return event("response.output_item.added", ",\"output_index\":" + std::to_string(i) + ",\"item\":" + std::string(item)); }
Frame item_done(size_t i, std::string_view item) { return event("response.output_item.done", ",\"output_index\":" + std::to_string(i) + ",\"item\":" + std::string(item)); }
Frame terminal(std::string_view output, std::string_view status = "completed", std::string_view counters = usage, std::string_view detail = "null") {
  return event(status == "incomplete" ? "response.incomplete" : "response.completed", ",\"response\":" + body(output, status, counters, detail));
}
Frame text_delta(std::string_view id, size_t index, std::string_view delta) {
  return event("response.output_text.delta", ",\"item_id\":" + json::quote(id) + ",\"output_index\":" + std::to_string(index) + ",\"content_index\":0,\"delta\":" + json::quote(delta));
}
Frame args_delta(std::string_view id, size_t index, std::string_view delta) {
  return event("response.function_call_arguments.delta", ",\"item_id\":" + json::quote(id) + ",\"output_index\":" + std::to_string(index) + ",\"delta\":" + json::quote(delta));
}
std::vector<Frame> text_frames() {
  return {created(), added(0, R"({"id":"msg_1","type":"message","status":"in_progress","role":"assistant","content":[]})"),
    event("response.content_part.added", R"(,"item_id":"msg_1","output_index":0,"content_index":0,"part":{"type":"output_text","text":"","annotations":[]})"),
    text_delta("msg_1", 0, "hel"), text_delta("msg_1", 0, "lo"),
    event("response.output_text.done", R"(,"item_id":"msg_1","output_index":0,"content_index":0,"text":"hello")"),
    event("response.content_part.done", R"(,"item_id":"msg_1","output_index":0,"content_index":0,"part":{"type":"output_text","text":"hello","annotations":[]})"),
    item_done(0, message), terminal("[" + message + "]")};
}
Outcome buffered(std::string_view wire, responses::Close close = {}, SemanticLimits limits = {}) {
  Accumulator acc(limits); responses::Codec codec(desc(), responses::Mode::Buffered, acc, context(), limits);
  codec.buffered(wire, close); CHECK(acc.outcome()); return *acc.outcome();
}
Outcome stream(const std::vector<Frame>& frames, responses::Close close = {}, SemanticLimits limits = {}) {
  Accumulator acc(limits); responses::Codec codec(desc(), responses::Mode::Sse, acc, context(), limits);
  for (const auto& [name, data] : frames) if (!codec.frame(name, data)) break;
  codec.finish(close); CHECK(acc.outcome()); return *acc.outcome();
}
const Completion& completed(const Outcome& outcome) { CHECK(std::holds_alternative<Completion>(outcome)); return std::get<Completion>(outcome); }
const Failure& failed(const Outcome& outcome, ErrorKind kind) { CHECK(std::holds_alternative<Failure>(outcome)); const auto& f = std::get<Failure>(outcome); CHECK(f.error.kind == kind); return f; }
std::string text(const std::vector<Message>& messages) {
  std::string result; for (const auto& m : messages) for (const auto& p : m.parts) if (auto t = std::get_if<Text>(&p)) result += t->value; return result;
}
void typed_encode() {
  auto r = request(); r.required_tool = "missing";
  CHECK(std::holds_alternative<Error>(responses::encode(desc(), r, false)));
  r = request(); r.tools.push_back(r.tools.front()); CHECK(std::holds_alternative<Error>(responses::encode(desc(), r, false)));
  r = request(); r.messages.push_back(Message{"", Role::Tool, {ToolResult{"orphan", "result"}}}); CHECK(std::holds_alternative<Error>(responses::encode(desc(), r, false)));
}
void controls_and_cursor_boundaries() {
  const auto encode_body = [&](const responses::Request& r) {
    auto encoded = responses::encode(desc(), r, false);
    CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
    return parse(std::get<responses::EncodedRequest>(encoded).body);
  };
  const auto rejected = [&](const responses::Request& r, ErrorKind kind = ErrorKind::InvalidRequest) {
    auto encoded = responses::encode(desc(), r, false);
    CHECK(std::holds_alternative<Error>(encoded) && std::get<Error>(encoded).kind == kind);
  };
  auto r = request();
  r.parallel_tool_calls = false;
  r.response_format = ResponseFormat{};
  for (const auto& [value, name] : std::vector<std::pair<responses::Verbosity, std::string>>{
      {responses::Verbosity::Low, "low"}, {responses::Verbosity::Medium, "medium"}, {responses::Verbosity::High, "high"}}) {
    r.verbosity = value;
    auto wire = encode_body(r);
    CHECK(!wire.root().get("parallel_tool_calls").as_bool());
    CHECK(wire.root().get("text").get("verbosity").as_string() == name);
    CHECK(wire.root().get("text").get("format").get("type").as_string() == "json_object");
  }
  r.parallel_tool_calls = true;
  r.response_format.reset();
  r.truncation = responses::Truncation::Disabled;
  auto wire = encode_body(r);
  CHECK(wire.root().get("parallel_tool_calls").as_bool());
  CHECK(wire.root().get("truncation").as_string() == "disabled");
  r.truncation = responses::Truncation::Auto;
  r.include = std::vector<responses::Include>{responses::Include::WebSearchSources, responses::Include::FileSearchResults,
    responses::Include::MessageOutputTextLogprobs, responses::Include::ComputerCallOutputImageUrl,
    responses::Include::CodeInterpreterCallOutputs, responses::Include::ReasoningEncryptedContent};
  wire = encode_body(r);
  CHECK(wire.root().get("truncation").as_string() == "auto");
  CHECK(json::equal(wire.root().get("include"), parse(R"(["web_search_call.action.sources","file_search_call.results","message.output_text.logprobs","computer_call_output.output.image_url","code_interpreter_call.outputs","reasoning.encrypted_content"])").root()));
  r.include = std::vector<responses::Include>{};
  wire = encode_body(r); CHECK(wire.root().get("include").size() == 0);
  r.include.reset();
  wire = encode_body(r); CHECK(json::equal(wire.root().get("include"), parse(R"(["reasoning.encrypted_content"])").root()));
  r.verbosity = static_cast<responses::Verbosity>(-1); rejected(r);
  r = request(); r.truncation = static_cast<responses::Truncation>(99); rejected(r);
  r = request(); r.include = std::vector<responses::Include>{static_cast<responses::Include>(99)}; rejected(r);
  r.include = std::vector<responses::Include>{responses::Include::FileSearchResults, responses::Include::FileSearchResults}; rejected(r);
  r = request();
  for (const auto& cursor : {std::string{}, std::string("resp bad"), std::string("resp\nbad"), std::string(257, 'a'), std::string("resp/\xc3\xa9")}) {
    r.previous_response_id = cursor; rejected(r);
  }
  r.previous_response_id = std::string(256, 'a');
  wire = encode_body(r);
  CHECK(wire.root().get("previous_response_id").as_string() == *r.previous_response_id);
  CHECK(wire.root().get("input").size() == 1);
  auto encoded = responses::encode(desc(), r, false);
  CHECK(!std::get<responses::EncodedRequest>(encoded).context->replay_eligible());
  r.messages = {Message{"", Role::Tool, {ToolResult{"call_1", "one"}}}};
  rejected(r); // A caller-provided cursor alone does not authorize client results.
  r = request(); r.previous_response_history = r.messages; rejected(r);
}
// OpenRouter provider.max_price (https://openrouter.ai/docs/guides/routing/provider-selection#max-price)
// on the Responses route; the request schema shares the Chat provider object.
void openrouter_max_price() {
  static const auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"gateway-responses","family":"openai.responses","connection":{"base_url":"https://openrouter.ai","paths":{"buffered":"/api/v1/responses","streaming":"/api/v1/responses"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& gateway = std::get<descriptor::ValidatedDescriptor>(loaded);
  const auto outcome = [&](const descriptor::ValidatedDescriptor& d, const responses::Request& r) {
    auto encoded = responses::encode(d, r, false);
    return std::holds_alternative<Error>(encoded) ? std::optional<ErrorKind>(std::get<Error>(encoded).kind) : std::nullopt;
  };
  auto r = request(); r.provider = OpenRouterRouting{}; r.provider->order = {"openai"};
  OpenRouterMaxPrice price; price.prompt_nano_usd = 75'000'000; price.completion_nano_usd = 15'000'000'000; price.image_nano_usd = 100'000;
  r.provider->max_price = price;
  auto encoded = responses::encode(gateway, r, false);
  CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
  const auto& wire = std::get<responses::EncodedRequest>(encoded).body;
  CHECK(wire.find(R"("provider":{"order":["openai"],"max_price":{"prompt":0.075,"completion":15,"image":0.0001}})") != std::string::npos);
  auto changed = r; changed.provider->max_price = OpenRouterMaxPrice{};
  CHECK(outcome(gateway, changed) == ErrorKind::InvalidRequest);
  changed.provider->max_price->request_nano_usd = (std::uint64_t{1} << 53) + 1;
  CHECK(outcome(gateway, changed) == ErrorKind::InvalidRequest);
  CHECK(outcome(desc(), r) == ErrorKind::InvalidRequest);
  // Captured native output stays bound to the ceiling it was requested under.
  Accumulator acc;
  responses::Codec codec(gateway, responses::Mode::Buffered, acc, std::get<responses::EncodedRequest>(encoded).context);
  codec.buffered(body("[" + reasoning + "," + message + "]"), {});
  CHECK(acc.outcome());
  auto follow = r; follow.messages.push_back(completed(*acc.outcome()).messages.at(0));
  follow.messages.push_back(Message{"", Role::User, {Text{"next"}}});
  CHECK(!outcome(gateway, follow));
  changed = follow; changed.provider->max_price->completion_nano_usd = 14'999'999'999;
  CHECK(outcome(gateway, changed) == ErrorKind::ReplayIneligible);
  changed = follow; changed.provider->max_price.reset();
  CHECK(outcome(gateway, changed) == ErrorKind::ReplayIneligible);
}
void strict_tool_schemas() {
  // The embedded policy defaults Responses function tools to strict.
  CHECK(desc().policy()->defaults("openai.responses", desc().base_url(), "fixture-model").strict_tools == std::optional<bool>(true));
  const std::string plain = R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"]})";
  const std::string closed = R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"],"additionalProperties":false})";
  const auto with = [](std::string_view schema, std::optional<bool> strict) {
    auto r = request(); r.tools = {{"lookup", "Find a value", owned(schema), strict, {}}}; return r;
  };
  const auto sent = [](const responses::Request& r) {
    auto encoded = responses::encode(desc(), r, false);
    CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
    auto wire = parse(std::get<responses::EncodedRequest>(encoded).body);
    std::vector<bool> flags;
    for (auto tool : wire.root().get("tools").elements()) { CHECK(tool.get("strict").is_bool()); flags.push_back(tool.get("strict").as_bool()); }
    return flags;
  };
  const auto refused = [](const responses::Request& r, std::string_view pointer, std::string_view reason) {
    auto encoded = responses::encode(desc(), r, false);
    CHECK(std::holds_alternative<Error>(encoded));
    const auto& error = std::get<Error>(encoded);
    CHECK(error.kind == ErrorKind::InvalidRequest && error.http_status == 0 && error.vendor_code.empty());
    CHECK(error.safe_message == "strict function \"lookup\" parameters are not strict-compatible at " +
          json::quote(pointer) + ": " + std::string(reason));
  };
  const std::string open_object = "object schema needs \"additionalProperties\": false";
  const std::string unlisted = "property is not listed in \"required\"";
  // Policy default: kept for a compatible schema, sent as false otherwise.
  CHECK(sent(with(plain, {})) == std::vector<bool>{false});
  CHECK(sent(with(closed, {})) == std::vector<bool>{true});
  // Explicit false is honored; explicit true on an incompatible schema fails before dispatch.
  CHECK(sent(with(plain, false)) == std::vector<bool>{false});
  CHECK(sent(with(closed, false)) == std::vector<bool>{false});
  CHECK(sent(with(closed, true)) == std::vector<bool>{true});
  refused(with(plain, true), "", open_object);
  // Each tool is judged on its own schema.
  auto mixed = with(closed, {}); mixed.tools.push_back({"other", "", owned(plain), {}, {}});
  CHECK((sent(mixed) == std::vector<bool>{true, false}));
  // Nested objects reached through properties, items, prefixItems, anyOf and $defs.
  const std::vector<std::tuple<std::string, std::string, std::string>> nested{
    {R"({"type":"object","properties":{"x":{"type":"integer"}},"additionalProperties":false})", "/properties/x", unlisted},
    {R"({"properties":{},"additionalProperties":false})", "", "root must be an object schema"},
    {R"({"type":"object","properties":{"point":{"type":"object","properties":{"x":{"type":"number"}},"required":["x"]}},"required":["point"],"additionalProperties":false})",
     "/properties/point", open_object},
    {R"({"type":"object","properties":{"rows":{"type":"array","items":{"type":"object","properties":{"a":{"type":"string"},"b":{"type":"string"}},"required":["a"],"additionalProperties":false}}},"required":["rows"],"additionalProperties":false})",
     "/properties/rows/items/properties/b", unlisted},
    {R"({"type":"object","properties":{"pair":{"type":"array","prefixItems":[{"type":"string"},{"type":"object"}]}},"required":["pair"],"additionalProperties":false})",
     "/properties/pair/prefixItems/1", open_object},
    {R"({"type":"object","properties":{"v":{"anyOf":[{"type":"string"},{"type":["object","null"],"properties":{}}]}},"required":["v"],"additionalProperties":false})",
     "/properties/v/anyOf/1", open_object},
    {R"({"type":"object","properties":{"n":{"$ref":"#/$defs/node"}},"required":["n"],"additionalProperties":false,"$defs":{"node":{"type":"object","properties":{"next":{"type":"object","properties":{"id":{"type":"string"}},"additionalProperties":false}},"required":["next"],"additionalProperties":false}}})",
     "/$defs/node/properties/next/properties/id", unlisted},
    {R"({"type":"object","properties":{"a/b~c":{"type":"object","additionalProperties":true}},"required":["a/b~c"],"additionalProperties":false})",
     "/properties/a~1b~0c", open_object},
  };
  for (const auto& [schema, pointer, reason] : nested) {
    CHECK(sent(with(schema, {})) == std::vector<bool>{false});
    CHECK(sent(with(schema, false)) == std::vector<bool>{false});
    refused(with(schema, true), pointer, reason);
  }
  // The same shapes made strict-compatible keep the default; other keywords are not judged locally.
  const std::string compatible = R"({"type":"object","properties":{"rows":{"type":"array","items":{"type":"object","properties":{"a":{"type":"string","pattern":"^a"},"b":{"anyOf":[{"type":"null"},{"type":"object","properties":{},"additionalProperties":false}]}},"required":["a","b"],"additionalProperties":false}},"pair":{"type":"array","prefixItems":[{"type":"string","format":"email"},{"$ref":"#/$defs/leaf"}]}},"required":["rows","pair"],"additionalProperties":false,"$defs":{"leaf":{"type":"object","properties":{"id":{"type":"string"}},"required":["id"],"additionalProperties":false}}})";
  CHECK(sent(with(compatible, {})) == std::vector<bool>{true});
  CHECK(sent(with(compatible, true)) == std::vector<bool>{true});
  // Native continuation: the configuration digest binds the requested strictness, so a turn
  // captured with a downgraded default continues and still sends false.
  const auto first = with(plain, {});
  auto encoded = responses::encode(desc(), first, false);
  CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
  Accumulator acc{SemanticLimits{}};
  responses::Codec codec(desc(), responses::Mode::Buffered, acc, std::get<responses::EncodedRequest>(encoded).context, SemanticLimits{});
  codec.buffered(body("[" + call + "]"), {});
  CHECK(acc.outcome());
  const auto& turn = completed(*acc.outcome()).messages.front();
  CHECK(turn.native && turn.native->complete());
  auto next = first; next.messages.push_back(turn); next.messages.push_back(Message{"", Role::Tool, {ToolResult{"call_1", "one"}}});
  CHECK(sent(next) == std::vector<bool>{false});
  auto changed = next; changed.tools[0].strict = true;
  refused(changed, "", open_object);
  changed = next; changed.tools[0].strict = false;
  auto mismatch = responses::encode(desc(), changed, false);
  CHECK(std::holds_alternative<Error>(mismatch) && std::get<Error>(mismatch).kind == ErrorKind::ReplayIneligible);
}
void grouped_native_and_owned_outcomes() {
  const auto output = "[" + reasoning + "," + call + "," + message + "]";
  auto b = buffered(body(output)); auto s = stream({created(), added(0, reasoning), item_done(0, reasoning), added(1, call), item_done(1, call),
    added(2, R"({"id":"msg_1","type":"message","status":"in_progress","role":"assistant","content":[]})"),
    event("response.content_part.added", R"(,"item_id":"msg_1","output_index":2,"content_index":0,"part":{"type":"output_text","text":"","annotations":[]})"),
    text_delta("msg_1", 2, "hello"),
    event("response.output_text.done", R"(,"item_id":"msg_1","output_index":2,"content_index":0,"text":"hello")"),
    event("response.content_part.done", R"(,"item_id":"msg_1","output_index":2,"content_index":0,"part":{"type":"output_text","text":"hello","annotations":[]})"),
    item_done(2, message), terminal(output)});
  for (const auto* o : {&b, &s}) {
    const auto& c = completed(*o); CHECK(c.messages.size() == 1 && c.stop.kind == StopKind::ToolUse);
    const auto& m = c.messages.front(); CHECK(m.native && m.native->complete() && m.wire_output);
    CHECK(json::equal(m.wire_output->root(), parse(output).root())); CHECK(m.parts.size() == 3);
    const auto& r = std::get<Reasoning>(m.parts[0]); CHECK(r.id == "rs_1" && r.summary == std::vector<std::string>{"Consider carefully"});
    CHECK(r.encrypted_content == "SEALED_NATIVE_MARKER_a810" && r.status == "completed");
    const auto& t = std::get<ToolCall>(m.parts[1]); CHECK(t.id == "call_1" && t.name == "lookup" && t.kind == ToolCallKind::ClientExecuted);
    CHECK(t.input && t.input->root().get("x").as_uint() == 1 && t.wire_metadata->root().get("id").as_string() == "fc_1");
    CHECK(m.wire_output->root().at(2).get("phase").as_string() == "final_answer" && text(c.messages) == "hello");
    auto r2 = request(); r2.messages.push_back(m); r2.messages.push_back(Message{"", Role::Tool, {ToolResult{"call_1", "one"}}});
    auto encoded = responses::encode(desc(), r2, false); CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
    auto d = parse(std::get<responses::EncodedRequest>(encoded).body); auto input = d.root().get("input");
    for (size_t i = 0; i < 3; ++i) CHECK(json::equal(input.at(i + 1), m.wire_output->root().at(i)));
    CHECK(input.at(4).get("type").as_string() == "function_call_output" && input.at(4).get("call_id").as_string() == "call_1");
    auto changed = r2; changed.service_tier = "default";
    auto mismatch = responses::encode(desc(), changed, false);
    CHECK(std::holds_alternative<Error>(mismatch) && std::get<Error>(mismatch).kind == ErrorKind::ReplayIneligible);
    changed = r2; changed.max_output_tokens = 129;
    mismatch = responses::encode(desc(), changed, false);
    CHECK(std::holds_alternative<responses::EncodedRequest>(mismatch));
    CHECK(std::get<responses::EncodedRequest>(mismatch).max_output_tokens == 129);
    changed = r2; changed.reasoning->summary = "detailed";
    mismatch = responses::encode(desc(), changed, false);
    CHECK(std::holds_alternative<Error>(mismatch) && std::get<Error>(mismatch).kind == ErrorKind::ReplayIneligible);
    changed = r2; changed.tools[0].strict = false;
    mismatch = responses::encode(desc(), changed, false);
    CHECK(std::holds_alternative<Error>(mismatch) && std::get<Error>(mismatch).kind == ErrorKind::ReplayIneligible);
  }
  // Codec and accumulator are already destroyed; strings and parsed arguments remain owned.
  CHECK(std::get<Reasoning>(completed(s).messages[0].parts[0]).encrypted_content == "SEALED_NATIVE_MARKER_a810");
}
void previous_response_ownership() {
  const auto first = buffered(body("[" + reasoning + "," + call + "," + message + "]"));
  auto r = request();
  r.previous_response_id = completed(first).messages[0].id;
  r.previous_response_history = r.messages;
  r.previous_response_history.push_back(completed(first).messages[0]);
  r.messages = {Message{"", Role::Tool, {ToolResult{"call_1", "one"}}}};
  auto encoded = responses::encode(desc(), r, false);
  CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
  const auto& value = std::get<responses::EncodedRequest>(encoded);
  auto wire = parse(value.body);
  CHECK(!value.context->replay_eligible());
  CHECK(json::equal(wire.root().get("input"), parse(R"([{"type":"function_call_output","call_id":"call_1","output":"one"}])").root()));
  const auto reject = [&](const responses::Request& bad, ErrorKind kind) {
    const auto result = responses::encode(desc(), bad, false);
    CHECK(std::holds_alternative<Error>(result) && std::get<Error>(result).kind == kind);
  };
  auto bad = r; bad.previous_response_id = "other"; reject(bad, ErrorKind::ReplayIneligible);
  bad = r; std::get<Text>(bad.previous_response_history[0].parts[0]).value = "changed"; reject(bad, ErrorKind::ReplayIneligible);
  bad = r; std::get<ToolCall>(bad.previous_response_history.back().parts[1]).id = "foreign"; reject(bad, ErrorKind::ReplayIneligible);
  bad = r; bad.previous_response_history.back().native.reset(); reject(bad, ErrorKind::ReplayIneligible);
  bad = r; bad.previous_response_history.erase(bad.previous_response_history.begin()); reject(bad, ErrorKind::ReplayIneligible);
  bad = r; bad.messages.push_back(bad.previous_response_history.back()); reject(bad, ErrorKind::ReplayIneligible);
  bad = r; bad.messages.push_back(bad.messages[0]); reject(bad, ErrorKind::InvalidRequest);
  bad = r; std::get<ToolResult>(bad.messages[0].parts[0]).tool_use_id = "orphan"; reject(bad, ErrorKind::InvalidRequest);
  bad = r; bad.messages = {Message{"", Role::User, {Text{"skip result"}}}}; reject(bad, ErrorKind::InvalidRequest);
  for (int selection = 0; selection < 4; ++selection) {
    bad = r;
    if (selection == 0) bad.parallel_tool_calls = false;
    else if (selection == 1) bad.verbosity = responses::Verbosity::High;
    else if (selection == 2) bad.truncation = responses::Truncation::Auto;
    else bad.include = std::vector<responses::Include>{};
    reject(bad, ErrorKind::ReplayIneligible);
  }
}
void normal_close_required() {
  const auto frames = text_frames();
  for (size_t n = 0; n <= frames.size(); ++n) for (bool normal : {false, true}) {
    auto o = stream(std::vector<Frame>(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(n)), {normal, ErrorKind::Truncated});
    if (n == frames.size() && normal) CHECK(text(completed(o).messages) == "hello");
    else { const auto& f = failed(o, ErrorKind::Truncated); for (const auto& m : f.partial.messages) CHECK(!m.native || !m.native->complete()); }
  }
  size_t commits = 0;
  Accumulator acc({}, [&](const Event& e) { if (std::holds_alternative<Commit>(e)) ++commits; });
  responses::Codec codec(desc(), responses::Mode::Sse, acc, context());
  for (const auto& [name, data] : frames) CHECK(codec.frame(name, data));
  CHECK(!acc.outcome() && commits == 0); codec.finish(); CHECK(acc.outcome() && commits == 1);
  codec.finish({false, ErrorKind::Cancelled}); CHECK(commits == 1 && !codec.frame("error", "{}"));
  auto late = frames; late.push_back(event("error", R"(,"code":"server_error","message":"SECRET_ERROR_MARKER_b007")"));
  const auto o = stream(late); const auto& f = failed(o, ErrorKind::RemoteFailure);
  CHECK(text(f.partial.messages) == "hello" && f.partial.usage.stage == UsageStage::Partial);
  CHECK(f.error.safe_message.find("SECRET_ERROR_MARKER") == std::string::npos);
  failed(buffered("not-json", {false, ErrorKind::Cancelled}), ErrorKind::Cancelled);
  failed(buffered(body("[]", "failed")), ErrorKind::RemoteFailure);
  failed(buffered(body("[]", "cancelled")), ErrorKind::Cancelled);
  failed(stream({created(), event("response.failed", ",\"response\":" + body("[]", "failed"))}), ErrorKind::RemoteFailure);
}
void named_error_precedence() {
  const auto frames = text_frames();
  for (const auto count : {size_t{0}, size_t{4}, frames.size()}) {
    const std::string expected = count == 0 ? "" : count == 4 ? "hel" : "hello";
    for (const auto& wire : {std::string("{"), std::string("[]"), std::string("[DONE]"),
        std::string(R"({"x":1,"x":2})"), std::string("{}"),
        std::string(R"({"type":7,"error":{"message":"private"}})"),
        std::string(R"({"type":"response.completed","vendor":{"b":[2,1],"a":true}})")}) {
      size_t commits = 0, failures = 0;
      std::shared_ptr<const json::Document> observed;
      const auto outcome = [&] {
        Accumulator acc({}, [&](const Event& e) {
          if (std::holds_alternative<Commit>(e)) ++commits;
          if (std::holds_alternative<Fail>(e)) ++failures;
          if (const auto* raw = std::get_if<RawWire>(&e); raw && raw->type == "error") observed = raw->payload;
        });
        responses::Codec codec(desc(), responses::Mode::Sse, acc, context());
        for (size_t i = 0; i < count; ++i) CHECK(codec.frame(frames[i].first, frames[i].second));
        CHECK(!codec.frame("error", wire));
        CHECK(!codec.frame("message", "[DONE]"));
        codec.finish({false, ErrorKind::Cancelled});
        CHECK(acc.outcome());
        return *acc.outcome();
      }();
      const auto& f = failed(outcome, ErrorKind::RemoteFailure);
      CHECK(commits == 0 && failures == 1 && text(f.partial.messages) == expected);
      CHECK(f.error.safe_message.find("private") == std::string::npos);
      CHECK(f.partial.raw_events.size() == count + (observed ? 1 : 0));
      for (size_t i = 0; i < count; ++i) CHECK(f.partial.raw_events[i].payload->root().dump() == frames[i].second);
      if (wire == "{}" || wire.find("vendor") != std::string::npos || wire.find("private") != std::string::npos) {
        CHECK(observed && f.partial.raw_events.back().payload == observed);
        CHECK(f.partial.raw_events.back().type == "error" && observed->root().dump() == wire);
      } else CHECK(!observed);
      for (const auto& m : f.partial.messages) CHECK(!m.native || !m.native->complete());
      if (count == frames.size()) {
        CHECK(f.partial.usage.stage == UsageStage::Partial);
        CHECK(f.partial.wire_envelope->root().get("status").as_string() == "completed");
      }
    }
    for (bool byte_limit : {false, true}) {
      SemanticLimits limits;
      std::string wire;
      if (byte_limit) {
        size_t prefix_bytes = 0;
        for (size_t i = 0; i < count; ++i) prefix_bytes += frames[i].second.size();
        limits.max_content_bytes = prefix_bytes + 1;
        wire = "{}";
      } else {
        limits.max_json_depth = 8;
        wire = R"({"error":[[[[[[[[[[0]]]]]]]]]]})";
      }
      std::vector<Frame> source(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(count));
      source.emplace_back("error", wire);
      const auto outcome = stream(source, {}, limits);
      const auto& f = failed(outcome, ErrorKind::RemoteFailure);
      CHECK(text(f.partial.messages) == expected && f.partial.raw_events.size() == count);
      for (size_t i = 0; i < count; ++i) CHECK(f.partial.raw_events[i].payload->root().dump() == frames[i].second);
    }
  }
}
void named_error_raw_capacity() {
  const auto frames = text_frames();
  const std::string wire = "{\"vendor\":" + json::quote(std::string(1 << 20, 'x')) + "}";
  SemanticLimits limits; limits.max_content_bytes = 2 << 20;
  for (size_t count : {size_t{0}, size_t{4}}) {
    size_t commits = 0, failures = 0;
    Accumulator acc(limits, [&](const Event& e) {
      if (std::holds_alternative<Commit>(e)) ++commits;
      if (std::holds_alternative<Fail>(e)) ++failures;
    }, 4096);
    responses::Codec codec(desc(), responses::Mode::Sse, acc, context(), limits);
    for (size_t i = 0; i < count; ++i) CHECK(codec.frame(frames[i].first, frames[i].second));
    CHECK(!codec.frame("error", wire));
    CHECK(!codec.frame("message", "[DONE]"));
    codec.finish();
    const auto& f = failed(*acc.outcome(), ErrorKind::RemoteFailure);
    CHECK(commits == 0 && failures == 1 && text(f.partial.messages) == (count ? "hel" : ""));
    CHECK(f.partial.raw_events.size() == count);
    for (size_t i = 0; i < count; ++i) CHECK(f.partial.raw_events[i].payload->root().dump() == frames[i].second);
  }
}
void cancellation_observation_boundary() {
  const auto frames = text_frames();
  for (const auto& [count, expected] : std::vector<std::pair<size_t, std::string>>{{3, ""}, {4, "hel"}}) {
    const auto outcome = stream(std::vector<Frame>(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(count)),
        {false, ErrorKind::Cancelled});
    const auto& f = failed(outcome, ErrorKind::Cancelled);
    CHECK(text(f.partial.messages) == expected);
    CHECK(f.partial.wire_envelope && f.partial.wire_envelope->root().get("status").as_string() == "in_progress");
    CHECK(!f.partial.raw_events.empty());
    const auto& last = f.partial.raw_events.back();
    CHECK(last.type == (expected.empty() ? "response.content_part.added" : "response.output_text.delta"));
    if (expected.empty()) {
      CHECK(last.payload->root().get("part").get("type").as_string() == "output_text");
      CHECK(last.payload->root().get("part").get("text").as_string().empty());
    } else CHECK(last.payload->root().get("delta").as_string() == expected);
    for (const auto& m : f.partial.messages) CHECK(!m.native || !m.native->complete());
  }
}
void reasoning_stream_snapshots() {
  const auto r_start = R"({"id":"rs_1","type":"reasoning","summary":[]})";
  auto frames = std::vector<Frame>{created(), added(0, r_start),
    event("response.reasoning_summary_part.added", R"(,"item_id":"rs_1","output_index":0,"summary_index":0,"part":{"type":"summary_text","text":""})"),
    event("response.reasoning_summary_text.delta", R"(,"item_id":"rs_1","output_index":0,"summary_index":0,"delta":"Consider ")"),
    event("response.reasoning_summary_text.delta", R"(,"item_id":"rs_1","output_index":0,"summary_index":0,"delta":"carefully")"),
    event("response.reasoning_summary_text.done", R"(,"item_id":"rs_1","output_index":0,"summary_index":0,"text":"Consider carefully")"),
    event("response.reasoning_summary_part.done", R"(,"item_id":"rs_1","output_index":0,"summary_index":0,"part":{"type":"summary_text","text":"Consider carefully"})"),
    item_done(0, reasoning), terminal("[" + reasoning + "]")};
  auto o = stream(frames); const auto& r = std::get<Reasoning>(completed(o).messages[0].parts[0]);
  CHECK(r.summary.size() == 1 && r.summary[0] == "Consider carefully" && r.encrypted_content == "SEALED_NATIVE_MARKER_a810");
  auto mismatch = frames; mismatch.back() = terminal(R"([{"id":"rs_1","type":"reasoning","status":"completed","summary":[{"type":"summary_text","text":"different"}],"encrypted_content":"SEALED_NATIVE_MARKER_a810"}])");
  failed(stream(mismatch), ErrorKind::ProtocolCorrupt);
  // A provisional encrypted field is not native-final authority.
  const auto provisional = stream({created(), added(0, reasoning)}, {false, ErrorKind::Truncated});
  for (const auto& m : failed(provisional, ErrorKind::Truncated).partial.messages) CHECK(!m.native || !m.native->complete());
  for (const auto& m : std::get<Failure>(provisional).partial.messages)
    for (const auto& p : m.parts) if (auto partial = std::get_if<Reasoning>(&p)) CHECK(!partial->encrypted_content);
  const std::string detailed = R"({"id":"rs_detail","type":"reasoning","status":"completed","summary":[{"type":"summary_text","text":"first"},{"type":"summary_text","text":"second"}],"content":[{"type":"reasoning_text","text":"separate content"}],"encrypted_content":"distinct sealed content"})";
  auto detailed_buffered = buffered(body("[" + detailed + "]"));
  auto detailed_stream = stream({created(), added(0, detailed), item_done(0, detailed), terminal("[" + detailed + "]")});
  for (const auto* outcome : {&detailed_buffered, &detailed_stream}) {
    const auto& value = std::get<Reasoning>(completed(*outcome).messages[0].parts[0]);
    CHECK(value.summary.size() == 2 && value.summary[0] == "first" && value.summary[1] == "second");
    CHECK(value.content.size() == 1 && value.content[0] == "separate content" && value.encrypted_content == "distinct sealed content");
  }
}
void completed_reasoning_ciphertext_authority() {
  const auto terminal_reasoning = R"({"id":"rs_1","type":"reasoning","status":"completed","summary":[{"type":"summary_text","text":"Consider carefully"}],"encrypted_content":"TERMINAL_ONLY_REPRESENTATION"})";
  auto outcome = stream({created(), added(0, R"({"id":"rs_1","type":"reasoning","summary":[]})"),
      item_done(0, reasoning), terminal("[" + std::string(terminal_reasoning) + "]")});
  const auto& group = completed(outcome).messages[0];
  CHECK(group.native && group.native->complete());
  CHECK(std::get<Reasoning>(group.parts[0]).encrypted_content == "SEALED_NATIVE_MARKER_a810");
  CHECK(group.wire_output->root().at(0).get("encrypted_content").as_string() == "SEALED_NATIVE_MARKER_a810");
  auto next = request(); next.messages.push_back(group);
  const auto encoded = responses::encode(desc(), next, true);
  CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
  CHECK(parse(std::get<responses::EncodedRequest>(encoded).body).root().get("input").at(1)
      .get("encrypted_content").as_string() == "SEALED_NATIVE_MARKER_a810");
  failed(stream({created(), added(0, reasoning), item_done(0, reasoning),
      terminal(R"([{"id":"rs_1","type":"reasoning","status":"completed","summary":[{"type":"summary_text","text":"different"}],"encrypted_content":"TERMINAL_ONLY_REPRESENTATION"}])")}),
      ErrorKind::ProtocolCorrupt);
  failed(stream({created(), added(0, reasoning), item_done(0, reasoning),
      terminal(R"([{"id":"rs_1","type":"reasoning","status":"completed","summary":[{"type":"summary_text","text":"Consider carefully"}],"encrypted_content":7}])")}),
      ErrorKind::ProtocolCorrupt);
}
void tools_interleaving_and_ownership() {
  const std::string call2 = R"({"id":"fc_2","type":"function_call","status":"completed","call_id":"call_2","name":"lookup","arguments":"{\"x\":2}"})";
  const std::string output = "[" + call + "," + call2 + "]";
  auto o = stream({created(), added(1, R"({"id":"fc_2","type":"function_call","status":"in_progress","call_id":"call_2","name":"lookup","arguments":""})"),
    added(0, R"({"id":"fc_1","type":"function_call","status":"in_progress","call_id":"call_1","name":"lookup","arguments":""})"),
    args_delta("fc_1", 0, "{\"x\":"), args_delta("fc_2", 1, "{\"x\":2}"), args_delta("fc_1", 0, "1}"),
    event("response.function_call_arguments.done", R"(,"item_id":"fc_2","output_index":1,"arguments":"{\"x\":2}")"), item_done(1, call2),
    event("response.function_call_arguments.done", R"(,"item_id":"fc_1","output_index":0,"arguments":"{\"x\":1}")"), item_done(0, call), terminal(output)});
  const auto& parts = completed(o).messages[0].parts; CHECK(parts.size() == 2);
  CHECK(std::get<ToolCall>(parts[0]).id == "call_1" && std::get<ToolCall>(parts[1]).input->root().get("x").as_uint() == 2);
  failed(stream({created(), added(0, R"({"id":"fc_1","type":"function_call","call_id":"call_1","name":"lookup","arguments":""})"), args_delta("fc_other", 0, "{}")}), ErrorKind::ProtocolCorrupt);
  for (const auto& args : {std::string("{bad}"), std::string("[]"), std::string("{\"x\":1,\"x\":2}")}) {
    const auto item = "{\"id\":\"fc_bad\",\"type\":\"function_call\",\"status\":\"completed\",\"call_id\":\"call_bad\",\"name\":\"lookup\",\"arguments\":" + json::quote(args) + "}";
    auto invalid = buffered(body("[" + item + "]"));
    const auto& p = std::get<InvalidToolCall>(completed(invalid).messages[0].parts[0]); CHECK(p.raw_fragment == args && p.id == "call_bad");
    CHECK(p.reason == (args.starts_with("{\"x\"") ? InvalidReason::DuplicateKey : InvalidReason::NotJson));
  }
  failed(buffered(body(R"([{"id":"fc_bad","type":"function_call","call_id":"call_bad","name":"lookup","arguments":{}}])")), ErrorKind::ProtocolCorrupt);
}
void incomplete_and_server_items() {
  const std::string unfinished = R"({"id":"fc_1","type":"function_call","status":"incomplete","call_id":"call_1","name":"lookup","arguments":"{\"x\":"})";
  auto o = buffered(body("[" + reasoning + "," + unfinished + "]", "incomplete", usage, R"({"reason":"max_output_tokens"})"));
  const auto& c = completed(o); CHECK(c.stop.kind == StopKind::MaxTokens);
  CHECK(std::holds_alternative<InvalidToolCall>(c.messages[0].parts[1])); CHECK(!c.messages[0].native || !c.messages[0].native->complete());
  auto r = request(); r.messages.push_back(c.messages[0]); CHECK(std::holds_alternative<Error>(responses::encode(desc(), r, false)));
  auto filtered = buffered(body("[]", "incomplete", "null", R"({"reason":"content_filter"})")); CHECK(completed(filtered).stop.kind == StopKind::ContentFilter);
  const std::string server = R"({"id":"ws_1","type":"web_search_call","status":"completed","action":{"type":"search","query":"x"}})";
  auto opaque = buffered(body("[" + server + "," + message + "]")); CHECK(completed(opaque).stop.kind == StopKind::EndTurn);
  const auto& parts = completed(opaque).messages[0].parts; CHECK(std::holds_alternative<Opaque>(parts[0]));
  CHECK(json::equal(std::get<Opaque>(parts[0]).wire_metadata->root(), parse(server).root()));
  for (const auto& p : parts) CHECK(!std::holds_alternative<ToolCall>(p));
}
void unknown_hosted_item_ownership() {
  const std::string hosted = R"({"id":"hosted_1","type":"future_hosted_call","status":"completed","call_id":"provider_call","name":"lookup","arguments":{"x":2},"result":{"text":"provider result"}})";
  const auto output = "[" + hosted + "," + message + "]";
  for (const auto& outcome : {buffered(body(output)),
      stream({created(), added(0, hosted), item_done(0, hosted), added(1, message), item_done(1, message), terminal(output)})}) {
    const auto& c = completed(outcome);
    CHECK(c.stop.kind == StopKind::EndTurn && text(c.messages) == "hello");
    const auto& group = c.messages[0];
    CHECK(group.native && group.native->complete() && json::equal(group.wire_output->root(), parse(output).root()));
    const auto& item = std::get<Opaque>(group.parts[0]);
    CHECK(item.wire_type == "future_hosted_call" && json::equal(item.wire_metadata->root(), parse(hosted).root()));
    for (const auto& p : group.parts) CHECK(!std::holds_alternative<ToolCall>(p));
    auto next = request(); next.messages.push_back(group);
    const auto encoded = responses::encode(desc(), next, false);
    CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
    const auto input = parse(std::get<responses::EncodedRequest>(encoded).body);
    CHECK(json::equal(input.root().get("input").at(1), item.wire_metadata->root()));
    CHECK(json::equal(input.root().get("input").at(2), parse(message).root()));
    next.account_scope = "foreign-account";
    const auto foreign = responses::encode(desc(), next, false);
    CHECK(std::holds_alternative<Error>(foreign) && std::get<Error>(foreign).kind == ErrorKind::ReplayIneligible);
  }
}
responses::EncodedRequest encoded_ok(const descriptor::ValidatedDescriptor& d, const responses::Request& r) {
  auto encoded = responses::encode(d, r, false); CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
  return std::get<responses::EncodedRequest>(std::move(encoded));
}
template<class Result> ErrorKind refused(const Result& result) { CHECK(std::holds_alternative<Error>(result)); return std::get<Error>(result).kind; }
void hosted_tool_options() {
  auto r = request();
  responses::WebSearchTool search; search.allowed_domains = {"example.com"}; search.user_location = responses::ApproximateLocation{std::string("Seoul"), std::string("KR"), {}, std::string("Asia/Seoul")};
  search.search_context_size = "low"; search.external_web_access = false;
  responses::ImageGenerationTool image; image.size = "2048x1152"; image.quality = "xhigh"; image.model = "gpt-image-1-mini"; image.action = "edit";
  image.partial_images = 2; image.output_compression = 50; image.mask_file_id = "file-mask";
  responses::CodeInterpreterTool code; code.file_ids = {"file-1"}; code.memory_limit = "4g";
  code.network_policy = responses::ContainerNetworkPolicy{false, {"api.example.com"}, {{"api.example.com", "X-Token", "one"}}};
  responses::McpTool mcp; mcp.server_label = "docs"; mcp.server_url = "https://mcp.example.test/mcp"; mcp.authorization = "token-one";
  mcp.headers = {{"X-Key", "one"}}; mcp.allowed_tools = responses::McpToolFilter{{"search"}, {}};
  mcp.approval_never = responses::McpToolFilter{{}, true}; mcp.server_description = "Docs"; mcp.defer_loading = true;
  r.hosted_tools = {search, image, responses::FileSearchTool{{"vs_1"}, 5}, code, mcp, responses::ComputerTool{}};
  const auto sent = encoded_ok(desc(), r);
  const auto wire = parse(sent.body);
  const auto tools = wire.root().get("tools");
  CHECK(tools.size() == 7);
  const auto web = tools.at(1);
  CHECK(web.get("type").as_string() == "web_search" && web.get("filters").get("allowed_domains").at(0).as_string() == "example.com" &&
        web.get("user_location").get("type").as_string() == "approximate" && web.get("user_location").get("city").as_string() == "Seoul" &&
        !web.get("user_location").get("region").valid() && web.get("search_context_size").as_string() == "low" && !web.get("external_web_access").as_bool());
  const auto generate = tools.at(2);
  CHECK(generate.get("size").as_string() == "2048x1152" && generate.get("quality").as_string() == "xhigh" && generate.get("model").as_string() == "gpt-image-1-mini" &&
        generate.get("action").as_string() == "edit" && generate.get("partial_images").as_uint() == 2u && generate.get("output_compression").as_uint() == 50u &&
        generate.get("input_image_mask").get("file_id").as_string() == "file-mask");
  CHECK(tools.at(3).get("max_num_results").as_uint() == 5u);
  const auto container = tools.at(4).get("container");
  CHECK(tools.at(4).get("type").as_string() == "code_interpreter" && container.get("type").as_string() == "auto" && container.get("file_ids").at(0).as_string() == "file-1" &&
        container.get("memory_limit").as_string() == "4g" && container.get("network_policy").get("type").as_string() == "allowlist" &&
        container.get("network_policy").get("domain_secrets").at(0).get("value").as_string() == "one");
  const auto server = tools.at(5);
  CHECK(server.get("type").as_string() == "mcp" && server.get("server_label").as_string() == "docs" && server.get("server_url").as_string() == "https://mcp.example.test/mcp" &&
        server.get("authorization").as_string() == "token-one" && server.get("headers").get("X-Key").as_string() == "one" &&
        server.get("allowed_tools").get("tool_names").at(0).as_string() == "search" && server.get("require_approval").get("never").get("read_only").as_bool() &&
        server.get("server_description").as_string() == "Docs" && server.get("defer_loading").as_bool());
  CHECK(tools.at(6).get("type").as_string() == "computer" && tools.at(6).size() == 1);
  auto by_id = r; by_id.hosted_tools = {responses::CodeInterpreterTool{std::string("cntr_1"), {}, {}, {}}};
  CHECK(parse(encoded_ok(desc(), by_id).body).root().get("tools").at(1).get("container").as_string() == "cntr_1");
  auto preview = r; preview.hosted_tools = {responses::ComputerUsePreviewTool{"browser", 1024, 768}};
  const auto preview_wire = parse(encoded_ok(desc(), preview).body);
  const auto screen = preview_wire.root().get("tools").at(1);
  CHECK(screen.get("type").as_string() == "computer_use_preview" && screen.get("display_width").as_uint() == 1024u && screen.get("environment").as_string() == "browser");

  // Credentials rotate without losing the history; the tools' other settings are configuration.
  Accumulator acc; responses::Codec codec(desc(), responses::Mode::Buffered, acc, sent.context);
  codec.buffered(body("[" + message + "]"), {}); CHECK(acc.outcome());
  auto next = r; next.messages.push_back(completed(*acc.outcome()).messages[0]); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  encoded_ok(desc(), next);
  auto changed = next; std::get<responses::McpTool>(changed.hosted_tools[4]).authorization = "token-two";
  std::get<responses::McpTool>(changed.hosted_tools[4]).headers[0].second = "two";
  std::get<responses::CodeInterpreterTool>(changed.hosted_tools[3]).network_policy->domain_secrets[0].value = "two";
  encoded_ok(desc(), changed);
  // The conversation moves to the container a response reported, or changes the automatic one's settings (#101).
  changed = next; changed.hosted_tools[3] = responses::CodeInterpreterTool{std::string("cntr_1"), {}, {}, {}};
  CHECK(parse(encoded_ok(desc(), changed).body).root().get("tools").at(4).get("container").as_string() == "cntr_1");
  changed = next; std::get<responses::CodeInterpreterTool>(changed.hosted_tools[3]).memory_limit = "1g";
  std::get<responses::CodeInterpreterTool>(changed.hosted_tools[3]).network_policy.reset();
  encoded_ok(desc(), changed);
  changed = next; changed.hosted_tools.erase(changed.hosted_tools.begin() + 3);
  CHECK(refused(responses::encode(desc(), changed, false)) == ErrorKind::ReplayIneligible);
  changed = next; std::get<responses::McpTool>(changed.hosted_tools[4]).server_label = "other";
  CHECK(refused(responses::encode(desc(), changed, false)) == ErrorKind::ReplayIneligible);
  changed = next; std::get<responses::ImageGenerationTool>(changed.hosted_tools[1]).partial_images = 3;
  CHECK(refused(responses::encode(desc(), changed, false)) == ErrorKind::ReplayIneligible);
  changed = next; std::get<responses::WebSearchTool>(changed.hosted_tools[0]).search_context_size = "high";
  CHECK(refused(responses::encode(desc(), changed, false)) == ErrorKind::ReplayIneligible);

  auto bad = [&](auto edit) { auto copy = r; edit(copy); CHECK(refused(responses::encode(desc(), copy, false)) == ErrorKind::InvalidRequest); };
  bad([](auto& q) { std::get<responses::ImageGenerationTool>(q.hosted_tools[1]).size = "big"; });
  bad([](auto& q) { std::get<responses::ImageGenerationTool>(q.hosted_tools[1]).quality = "Extra High"; });
  bad([](auto& q) { std::get<responses::ImageGenerationTool>(q.hosted_tools[1]).output_compression = 101; });
  bad([](auto& q) { std::get<responses::FileSearchTool>(q.hosted_tools[2]).max_num_results = 0u; });
  bad([](auto& q) { std::get<responses::CodeInterpreterTool>(q.hosted_tools[3]).container_id = "cntr_1"; });
  bad([](auto& q) { std::get<responses::CodeInterpreterTool>(q.hosted_tools[3]).network_policy->domain_secrets[0].domain = "other.example.com"; });
  bad([](auto& q) { std::get<responses::CodeInterpreterTool>(q.hosted_tools[3]).network_policy->disabled = true; });
  bad([](auto& q) { std::get<responses::McpTool>(q.hosted_tools[4]).connector_id = "connector_gmail"; });
  bad([](auto& q) { std::get<responses::McpTool>(q.hosted_tools[4]).server_url = "http://mcp.example.test/mcp"; });
  bad([](auto& q) { std::get<responses::McpTool>(q.hosted_tools[4]).headers.push_back({"x-key", "again"}); });
  bad([](auto& q) { std::get<responses::McpTool>(q.hosted_tools[4]).authorization = "line\nbreak"; });
  bad([](auto& q) { std::get<responses::McpTool>(q.hosted_tools[4]).require_approval = "never"; });
  bad([](auto& q) { std::get<responses::McpTool>(q.hosted_tools[4]).allowed_tools = responses::McpToolFilter{}; });
  bad([](auto& q) { q.hosted_tools.push_back(q.hosted_tools[4]); });
  bad([](auto& q) { q.hosted_tools.push_back(responses::ComputerTool{}); });
  bad([](auto& q) { q.hosted_tools = {responses::ComputerUsePreviewTool{"browser", 0, 768}}; });
  // Two servers with their own labels are two tools.
  auto two = r; auto second = mcp; second.server_label = "more"; two.hosted_tools.push_back(second); encoded_ok(desc(), two);
  // OpenRouter's own tools and image output exist only at a declared OpenRouter origin.
  auto routed = request(); routed.hosted_tools = {responses::OpenRouterServerTool{"openrouter:datetime", {}}};
  CHECK(refused(responses::encode(desc(), routed, false)) == ErrorKind::Unsupported);
  routed = request(); routed.modalities = {responses::OutputModality::Image};
  CHECK(refused(responses::encode(desc(), routed, false)) == ErrorKind::Unsupported);
}
void openrouter_server_tools_and_images() {
  static const auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"gateway-responses","family":"openai.responses","connection":{"base_url":"https://openrouter.ai","paths":{"buffered":"/api/v1/responses","streaming":"/api/v1/responses"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& gateway = std::get<descriptor::ValidatedDescriptor>(loaded);
  auto r = request();
  r.hosted_tools = {responses::OpenRouterServerTool{"openrouter:datetime", {}}, responses::OpenRouterServerTool{"openrouter:image_generation", owned(R"({"model":"vendor/image-model","quality":"high"})")}};
  r.modalities = {responses::OutputModality::Image, responses::OutputModality::Text};
  OpenRouterImageConfig config; config.aspect_ratio = "1:1"; config.image_size = "1K"; r.image_config = config;
  const auto sent = encoded_ok(gateway, r);
  const auto wire = parse(sent.body);
  CHECK(wire.root().get("tools").at(1).get("type").as_string() == "openrouter:datetime" && !wire.root().get("tools").at(1).get("parameters").valid());
  CHECK(wire.root().get("tools").at(2).get("parameters").get("model").as_string() == "vendor/image-model");
  CHECK(wire.root().get("modalities").at(0).as_string() == "image" && wire.root().get("modalities").at(1).as_string() == "text" &&
        wire.root().get("image_config").get("aspect_ratio").as_string() == "1:1" && wire.root().get("image_config").get("image_size").as_string() == "1K");
  auto bad = r; bad.hosted_tools = {responses::OpenRouterServerTool{"datetime", {}}}; CHECK(refused(responses::encode(gateway, bad, false)) == ErrorKind::InvalidRequest);
  bad = r; bad.hosted_tools = {responses::OpenRouterServerTool{"openrouter:datetime", owned("[1]")}}; CHECK(refused(responses::encode(gateway, bad, false)) == ErrorKind::InvalidRequest);
  bad = r; bad.modalities = {responses::OutputModality::Text}; CHECK(refused(responses::encode(gateway, bad, false)) == ErrorKind::InvalidRequest);
  bad = r; bad.modalities.push_back(responses::OutputModality::Text); CHECK(refused(responses::encode(gateway, bad, false)) == ErrorKind::InvalidRequest);
  bad = r; bad.image_config->aspect_ratio = "one to one"; CHECK(refused(responses::encode(gateway, bad, false)) == ErrorKind::InvalidRequest);
  // The router's tool step is an opaque item, and a generated image without a format keeps an empty MIME.
  const std::string step = R"({"type":"openrouter:datetime","id":"st_1","status":"completed","datetime":"2026-10-19T03:04:14Z","timezone":"UTC"})";
  const std::string picture = R"({"id":"ig_1","type":"image_generation_call","status":"completed","result":"iVBORw0KGgo="})";
  // OpenRouter sends a base64 data URL; its MIME is the image's when the item names no format.
  const std::string url_picture = R"({"id":"ig_2","type":"image_generation_call","status":"completed","result":"data:image/png;base64,iVBORw0KGgo="})";
  Accumulator acc; responses::Codec codec(gateway, responses::Mode::Buffered, acc, sent.context);
  codec.buffered(body("[" + step + "," + picture + "," + url_picture + "," + message + "]"), {}); CHECK(acc.outcome());
  const auto& parts = completed(*acc.outcome()).messages[0].parts;
  CHECK(std::get<Opaque>(parts[0]).wire_type == "openrouter:datetime" && std::get<Media>(parts[1]).kind == MediaKind::Image && std::get<Media>(parts[1]).mime.empty());
  CHECK(std::get<Media>(parts[2]).mime == "image/png" && *std::get<Media>(parts[2]).data == "iVBORw0KGgo=");
  CHECK(json::equal(completed(*acc.outcome()).messages[0].wire_output->root().at(2), parse(url_picture).root()));
  const std::string broken = R"({"id":"ig_3","type":"image_generation_call","status":"completed","result":"data:image/png,iVBORw0KGgo="})";
  failed(buffered(body("[" + broken + "]")), ErrorKind::ProtocolCorrupt);
  auto next = r; next.messages.push_back(completed(*acc.outcome()).messages[0]); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  encoded_ok(gateway, next);
  auto changed = next; changed.image_config->image_size = "2K"; CHECK(refused(responses::encode(gateway, changed, false)) == ErrorKind::ReplayIneligible);
}
void plugins_and_citations() {
  static const auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"gateway-responses","family":"openai.responses","connection":{"base_url":"https://openrouter.ai","paths":{"buffered":"/api/v1/responses","streaming":"/api/v1/responses"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& gateway = std::get<descriptor::ValidatedDescriptor>(loaded);
  auto r = request(); r.plugins = {{"web", owned(R"({"max_results":2})")}};
  const auto sent = encoded_ok(gateway, r);
  const auto wire = parse(sent.body);
  CHECK(wire.root().get("plugins").at(0).get("id").as_string() == "web" && wire.root().get("plugins").at(0).get("max_results").as_uint() == 2u);
  CHECK(refused(responses::encode(desc(), r, false)) == ErrorKind::Unsupported);
  auto twice = r; twice.plugins.push_back(twice.plugins[0]); CHECK(refused(responses::encode(gateway, twice, false)) == ErrorKind::InvalidRequest);
  // Every annotation of an output_text is a citation of that text; the kinds keep their own fields.
  const std::string cited = R"({"id":"msg_c","type":"message","status":"completed","role":"assistant","content":[{"type":"output_text","text":"see the source","annotations":[)"
      R"({"type":"url_citation","url":"https://example.com","title":"Example","start_index":4,"end_index":10,"content":"excerpt"},)"
      R"({"type":"file_citation","file_id":"file-1","filename":"notes.txt","index":3}]}]})";
  for (const bool streaming : {false, true}) {
    Accumulator acc; responses::Codec codec(gateway, streaming ? responses::Mode::Sse : responses::Mode::Buffered, acc, sent.context);
    if (streaming) {
      const std::string opening = R"({"id":"msg_c","type":"message","status":"in_progress","role":"assistant","content":[]})";
      const std::string part = R"({"type":"output_text","text":"","annotations":[]})";
      const auto cited_doc = parse(cited);
      const auto annotation = cited_doc.root().get("content").at(0).get("annotations");
      std::vector<Frame> frames{created(), added(0, opening), event("response.content_part.added", R"(,"item_id":"msg_c","output_index":0,"content_index":0,"part":)" + part),
          text_delta("msg_c", 0, "see the source")};
      for (std::size_t n = 0; n < 2; ++n)
        frames.push_back(event("response.output_text.annotation.added", R"(,"item_id":"msg_c","output_index":0,"content_index":0,"annotation_index":)" + std::to_string(n) +
                               ",\"annotation\":" + annotation.at(n).dump()));
      const auto done_part = parse(cited).root().get("content").at(0).dump();
      frames.push_back(event("response.output_text.done", R"(,"item_id":"msg_c","output_index":0,"content_index":0,"text":"see the source")"));
      frames.push_back(event("response.content_part.done", R"(,"item_id":"msg_c","output_index":0,"content_index":0,"part":)" + done_part));
      frames.push_back(item_done(0, cited)); frames.push_back(terminal("[" + cited + "]"));
      for (const auto& [name, data] : frames) CHECK(codec.frame(name, data));
      codec.finish({});
    } else codec.buffered(body("[" + cited + "]"), {});
    CHECK(acc.outcome());
    const auto& text = std::get<Text>(completed(*acc.outcome()).messages[0].parts[0]);
    CHECK(text.value == "see the source" && text.citations.size() == 2);
    CHECK(text.citations[0].kind == "url_citation" && text.citations[0].url == "https://example.com" && text.citations[0].title == "Example" &&
          text.citations[0].start == 4u && text.citations[0].end == 10u && text.citations[0].raw->root().get("content").as_string() == "excerpt");
    CHECK(text.citations[1].kind == "file_citation" && text.citations[1].file_id == "file-1" && text.citations[1].filename == "notes.txt" &&
          text.citations[1].start == 3u && !text.citations[1].end && text.citations[1].url.empty());
    // Citations are read from the wire the message keeps, so the next turn replays it as received.
    auto next = r; next.messages.push_back(completed(*acc.outcome()).messages[0]); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
    const auto replayed = parse(encoded_ok(gateway, next).body);
    CHECK(json::equal(replayed.root().get("input").at(1), parse(cited).root()));
    auto changed = next; changed.plugins.clear(); CHECK(refused(responses::encode(gateway, changed, false)) == ErrorKind::ReplayIneligible);
  }
}
void service_item_progress_events() {
  // Code interpreter and MCP calls stream their code and arguments under the item's own event names.
  const std::string code_added = R"({"id":"ci_1","type":"code_interpreter_call","status":"in_progress","code":"","container_id":"cntr_1","outputs":null})";
  const std::string code_done = R"({"id":"ci_1","type":"code_interpreter_call","status":"completed","code":"17*23","container_id":"cntr_1","outputs":null})";
  const std::string mcp_added = R"({"id":"mcp_1","type":"mcp_call","status":"in_progress","server_label":"docs","name":"search","arguments":"","output":null,"error":null,"approval_request_id":null})";
  const std::string mcp_done = R"({"id":"mcp_1","type":"mcp_call","status":"completed","server_label":"docs","name":"search","arguments":"{\"q\":\"x\"}","output":"y","error":null,"approval_request_id":null})";
  const auto on = [](std::string type, std::string id, size_t index, std::string fields) {
    return event(std::move(type), ",\"item_id\":" + json::quote(id) + ",\"output_index\":" + std::to_string(index) + std::move(fields));
  };
  const auto output = "[" + code_done + "," + mcp_done + "," + message + "]";
  std::vector<Frame> frames{created(), added(0, code_added), on("response.code_interpreter_call.in_progress", "ci_1", 0, ""),
      on("response.code_interpreter_call_code.delta", "ci_1", 0, R"(,"delta":"17*")"), on("response.code_interpreter_call_code.done", "ci_1", 0, R"(,"code":"17*23")"),
      on("response.code_interpreter_call.interpreting", "ci_1", 0, ""), on("response.code_interpreter_call.completed", "ci_1", 0, ""), item_done(0, code_done),
      added(1, mcp_added), on("response.mcp_call.in_progress", "mcp_1", 1, ""), on("response.mcp_call_arguments.delta", "mcp_1", 1, R"(,"delta":"{\"q\"")"),
      on("response.mcp_call_arguments.done", "mcp_1", 1, R"(,"arguments":"{\"q\":\"x\"}")"), on("response.mcp_call.completed", "mcp_1", 1, ""), item_done(1, mcp_done),
      added(2, message), item_done(2, message), terminal(output)};
  const auto outcome = stream(frames);
  const auto& c = completed(outcome);
  CHECK(text(c.messages) == "hello" && std::get<Opaque>(c.messages[0].parts[0]).wire_type == "code_interpreter_call" && std::get<Opaque>(c.messages[0].parts[1]).wire_type == "mcp_call");
  CHECK(json::equal(c.messages[0].wire_output->root(), parse(output).root()));
  // A progress name must belong to its item, and a message item has no progress events.
  auto wrong = frames; wrong[3] = on("response.mcp_call_arguments.delta", "ci_1", 0, R"(,"delta":"x")");
  failed(stream(wrong), ErrorKind::Unsupported);
  const auto frames_text = text_frames();
  std::vector<Frame> on_message(frames_text.begin(), frames_text.begin() + 3);
  on_message.push_back(on("response.output_text.future", "msg_1", 0, ""));
  failed(stream(on_message), ErrorKind::Unsupported);
}
void computer_calls_and_mcp_approvals() {
  auto r = request(); r.hosted_tools = {responses::ComputerTool{}};
  const std::string action = R"({"id":"cu_1","type":"computer_call","status":"completed","call_id":"call_cu","actions":[{"type":"screenshot"}],"pending_safety_checks":[{"id":"sc_1","code":"malicious_instructions","message":"check"}]})";
  const auto output = "[" + action + "]";
  const auto sent = encoded_ok(desc(), r);
  for (const bool streaming : {false, true}) {
    Accumulator acc; responses::Codec codec(desc(), streaming ? responses::Mode::Sse : responses::Mode::Buffered, acc, sent.context);
    if (streaming) {
      const std::string opening = R"({"id":"cu_1","type":"computer_call","status":"in_progress","call_id":"call_cu","actions":[],"pending_safety_checks":[]})";
      for (const auto& [name, data] : std::vector<Frame>{created(), added(0, opening), item_done(0, action), terminal(output)}) CHECK(codec.frame(name, data));
      codec.finish({});
    } else codec.buffered(body(output), {});
    CHECK(acc.outcome());
    const auto& c = completed(*acc.outcome());
    const auto& call = std::get<ToolCall>(c.messages[0].parts[0]);
    CHECK(c.stop.kind == StopKind::ToolUse && call.kind == ToolCallKind::ClientExecuted && call.name == "computer" && call.id == "call_cu" &&
          call.wire_type == "computer_call" && call.input->root().get("actions").at(0).get("type").as_string() == "screenshot");
    auto png = std::make_shared<const std::string>("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");
    ToolResult shot; shot.tool_use_id = "call_cu"; shot.content = R"({"acknowledged_safety_checks":[{"id":"sc_1"}]})"; shot.content_parts = {Media::image("image/png", png)};
    auto next = r; next.messages.push_back(c.messages[0]); next.messages.push_back(Message{"", Role::Tool, {shot}});
    const auto wire = parse(encoded_ok(desc(), next).body);
    const auto answer = wire.root().get("input").at(2);
    CHECK(answer.get("type").as_string() == "computer_call_output" && answer.get("call_id").as_string() == "call_cu" &&
          answer.get("output").get("type").as_string() == "computer_screenshot" && answer.get("output").get("image_url").as_string().starts_with("data:image/png;base64,iVBOR") &&
          answer.get("acknowledged_safety_checks").at(0).get("id").as_string() == "sc_1");
    auto plain = shot; plain.content.clear(); next.messages.back() = Message{"", Role::Tool, {plain}};
    CHECK(!parse(encoded_ok(desc(), next).body).root().get("input").at(2).get("acknowledged_safety_checks").valid());
    // The screenshot's data URL carries no whitespace from a MIME type that has some around its parameters.
    for (const char* mime : {"image/png; q=1", "image/png;\tq=1"}) {
      auto spaced = plain; spaced.content_parts = {Media::image(mime, png)}; next.messages.back() = Message{"", Role::Tool, {spaced}};
      CHECK(parse(encoded_ok(desc(), next).body).root().get("input").at(2).get("output").get("image_url").as_string().starts_with("data:image/png;q=1;base64,iVBOR"));
    }
    auto worded = shot; worded.content_parts.push_back(Text{"note"}); next.messages.back() = Message{"", Role::Tool, {worded}};
    CHECK(refused(responses::encode(desc(), next, false)) == ErrorKind::InvalidRequest);
    auto extra = shot; extra.content = R"({"note":"x"})"; next.messages.back() = Message{"", Role::Tool, {extra}};
    CHECK(refused(responses::encode(desc(), next, false)) == ErrorKind::InvalidRequest);
    auto two = shot; two.content_parts.push_back(Media::image("image/png", png)); next.messages.back() = Message{"", Role::Tool, {two}};
    CHECK(refused(responses::encode(desc(), next, false)) == ErrorKind::InvalidRequest);
    // A function result may not carry content beside its parts.
  }
  // An approval answers a request the history holds, or one the continued response holds.
  auto m = request(); responses::McpTool mcp; mcp.server_label = "docs"; mcp.server_url = "https://mcp.example.test/mcp"; mcp.require_approval = "always";
  m.hosted_tools = {mcp};
  const std::string ask = R"({"id":"mcpr_1","type":"mcp_approval_request","server_label":"docs","name":"search","arguments":"{}"})";
  const auto asked = encoded_ok(desc(), m);
  Accumulator acc; responses::Codec codec(desc(), responses::Mode::Buffered, acc, asked.context);
  codec.buffered(body("[" + ask + "]"), {}); CHECK(acc.outcome());
  const auto& reply = completed(*acc.outcome());
  const auto& request_part = std::get<ToolCall>(reply.messages[0].parts[0]);
  CHECK(reply.stop.kind == StopKind::ToolUse && request_part.kind == ToolCallKind::ApprovalRequest && request_part.id == "mcpr_1" &&
        request_part.name == "search" && request_part.wire_type == "mcp_approval_request" && reply.executable_tool_calls().empty());
  auto approve = m; approve.messages.push_back(reply.messages[0]); approve.mcp_approvals = {{"mcpr_1", true, {}}};
  const auto approved_wire = parse(encoded_ok(desc(), approve).body);
  const auto approved = approved_wire.root().get("input");
  CHECK(approved.size() == 3 && approved.at(2).get("type").as_string() == "mcp_approval_response" && approved.at(2).get("approval_request_id").as_string() == "mcpr_1" &&
        approved.at(2).get("approve").as_bool() && !approved.at(2).get("reason").valid());
  // The service takes a reason only with a refusal.
  auto refusal = approve; refusal.mcp_approvals = {{"mcpr_1", false, std::string("not now")}};
  CHECK(parse(encoded_ok(desc(), refusal).body).root().get("input").at(2).get("reason").as_string() == "not now");
  auto reasoned = approve; reasoned.mcp_approvals = {{"mcpr_1", true, std::string("fine")}}; CHECK(refused(responses::encode(desc(), reasoned, false)) == ErrorKind::InvalidRequest);
  auto unknown = approve; unknown.mcp_approvals = {{"mcpr_2", false, {}}}; CHECK(refused(responses::encode(desc(), unknown, false)) == ErrorKind::InvalidRequest);
  auto twice = approve; twice.mcp_approvals.push_back(twice.mcp_approvals[0]); CHECK(refused(responses::encode(desc(), twice, false)) == ErrorKind::InvalidRequest);
  auto cursor = m; cursor.previous_response_id = "resp_1"; cursor.mcp_approvals = {{"mcpr_9", false, {}}};
  CHECK(parse(encoded_ok(desc(), cursor).body).root().get("input").at(1).get("approve").as_bool() == false);
  // Without a computer tool a computer call is not the caller's to answer.
  auto undeclared = request(); undeclared.messages.push_back(reply.messages[0]);
  Accumulator bare; responses::Codec bare_codec(desc(), responses::Mode::Buffered, bare, encoded_ok(desc(), r).context);
  bare_codec.buffered(body(output), {}); CHECK(bare.outcome());
  undeclared = request(); undeclared.messages.push_back(completed(*bare.outcome()).messages[0]);
  CHECK(refused(responses::encode(desc(), undeclared, false)) != ErrorKind::Unsupported);
}
// Include selections may be named beyond the typed ones.
void named_includes() {
  auto named = request(); named.include_names = {"message.output_text.logprobs"};
  const auto included = parse(encoded_ok(desc(), named).body);
  CHECK(included.root().get("include").size() == 2 && included.root().get("include").at(0).as_string() == "reasoning.encrypted_content" &&
        included.root().get("include").at(1).as_string() == "message.output_text.logprobs");
  named.include = std::vector<responses::Include>{};
  const auto only_named = parse(encoded_ok(desc(), named).body);
  CHECK(only_named.root().get("include").size() == 1 && only_named.root().get("include").at(0).as_string() == "message.output_text.logprobs");
  named.include = std::vector<responses::Include>{responses::Include::MessageOutputTextLogprobs};
  CHECK(refused(responses::encode(desc(), named, false)) == ErrorKind::InvalidRequest);
  named.include.reset(); named.include_names = {"Not A Name"}; CHECK(refused(responses::encode(desc(), named, false)) == ErrorKind::InvalidRequest);
}
void unknown_wire_observation_boundary() {
  const auto unknown = event("response.future_semantic.delta",
      R"(,"delta":"not assistant text","status":"completed","metadata":{"nested":[{"value":"owned unknown payload"}]})");
  const auto frames = text_frames();
  for (const auto& [count, expected] : std::vector<std::pair<size_t, std::string>>{{1, ""}, {4, "hel"}, {8, "hello"}}) {
    std::vector<Frame> wire(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(count));
    wire.push_back(unknown);
    wire.insert(wire.end(), frames.begin() + static_cast<std::ptrdiff_t>(count), frames.end());
    const auto outcome = stream(wire);
    const auto& f = failed(outcome, ErrorKind::Unsupported);
    CHECK(text(f.partial.messages) == expected && !f.partial.stop);
    CHECK(f.partial.wire_envelope && f.partial.wire_envelope->root().get("status").as_string() == "in_progress");
    for (const auto& m : f.partial.messages) CHECK(!m.native || !m.native->complete());
    CHECK(!f.partial.raw_events.empty() && f.partial.raw_events.front().type == "response.created");
    const auto& retained = f.partial.raw_events.back();
    CHECK(retained.type == unknown.first &&
        retained.payload->root().get("metadata").get("nested").at(0).get("value").as_string() == "owned unknown payload");
    if (count != 1) {
      const auto& prior = f.partial.raw_events.at(f.partial.raw_events.size() - 2);
      CHECK(prior.type == (count == 4 ? "response.output_text.delta" : "response.output_item.done"));
    }
  }
  failed(stream({created(), unknown}), ErrorKind::Unsupported);
  failed(stream({created(), {"response.output_text.delta", unknown.second}}), ErrorKind::ProtocolCorrupt);
}
void usage_boundaries() {
  auto normal = buffered(body("[" + reasoning + "]")); const auto& u = completed(normal).usage;
  CHECK(u.stage == UsageStage::Final && u.quality == UsageQuality::Consistent);
  CHECK(u.input_total->value == 10 && u.output_total->value == 7 && u.total->value == 17);
  CHECK(u.cache_read->value == 4 && u.input_uncached->value == 6 && u.input_uncached->evidence == Evidence::Derived && u.reasoning->value == 3);
  auto nullable = buffered(body("[]", "completed", R"({"input_tokens":null,"output_tokens":7,"total_tokens":null,"input_tokens_details":{"cached_tokens":null},"output_tokens_details":{"reasoning_tokens":null,"audio_tokens":2}})"));
  const auto& n = completed(nullable).usage; CHECK(!n.input_total && !n.total && !n.cache_read && !n.input_uncached && !n.reasoning && n.output_total->value == 7);
  bool retained = false; for (const auto& [key, count] : n.extra) if (key.find("audio_tokens") != std::string::npos && count.value == 2) retained = true; CHECK(retained);
  auto conflict = buffered(body("[]", "completed", R"({"input_tokens":2,"output_tokens":3,"total_tokens":99,"input_tokens_details":{"cached_tokens":4},"output_tokens_details":{"reasoning_tokens":5}})"));
  const auto& x = completed(conflict).usage; CHECK(x.quality == UsageQuality::Inconsistent && !x.conflicts.empty()); CHECK(x.provider_reported_total->value == 99 && !x.input_uncached);
  failed(buffered(body("[]", "completed", R"({"input_tokens":-1})")), ErrorKind::ProtocolCorrupt);
}
// input_uncached = input_tokens - cached_tokens - cache_write_tokens (Evidence::Derived) once the
// cache read is reported. Direct OpenAI reports cache_write_tokens and OpenRouter omits it, so no
// Responses origin always carries it: a reported write is subtracted and an absent one is zero.
void uncached_input_rule() {
  const auto usage_of = [](std::string_view counters, bool sse) {
    auto frames = text_frames(); frames.back() = terminal("[" + message + "]", "completed", counters);
    const auto outcome = sse ? stream(frames) : buffered(body("[" + message + "]", "completed", counters));
    return completed(outcome).usage;
  };
  const auto counters = [](std::string_view details) {
    return std::string(R"({"input_tokens":100,"output_tokens":7,"total_tokens":107,"input_tokens_details":)") + std::string(details) + "}";
  };
  for (bool sse : {false, true}) {
    auto u = usage_of(counters(R"({"cached_tokens":30,"cache_write_tokens":20})"), sse);
    CHECK(u.quality == UsageQuality::Consistent && u.cache_read->value == 30 && u.cache_write->value == 20);
    CHECK(u.input_uncached->value == 50 && u.input_uncached->evidence == Evidence::Derived);
    u = usage_of(counters(R"({"cached_tokens":30})"), sse);
    CHECK(u.quality == UsageQuality::Consistent && !u.cache_write && u.input_uncached->value == 70);
    u = usage_of(counters(R"({"cache_write_tokens":20})"), sse);
    CHECK(u.quality == UsageQuality::Consistent && !u.cache_read && !u.input_uncached);
    u = usage_of(counters(R"({"cached_tokens":60,"cache_write_tokens":50})"), sse);
    CHECK(u.quality == UsageQuality::Inconsistent && !u.input_uncached);
    CHECK(u.conflicts.size() == 1 && u.conflicts[0].counter == "cache_write");
    // Live direct OpenAI shape (gpt-5-nano, cached prefix).
    u = usage_of(R"({"input_tokens":5547,"input_tokens_details":{"cache_write_tokens":0,"cached_tokens":5376},"output_tokens":97,"output_tokens_details":{"reasoning_tokens":64},"total_tokens":5644})", sse);
    CHECK(u.quality == UsageQuality::Consistent && u.input_uncached->value == 171 && u.cache_write->value == 0);
  }
}
void provider_money_and_snapshot_aliases() {
  const auto descriptor_for = [](std::string_view origin) {
    auto loaded = descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"cost-alias\",\"family\":\"openai.responses\",\"connection\":{\"base_url\":" +
        json::quote(origin) + ",\"paths\":{\"buffered\":\"/v1/responses\",\"streaming\":\"/v1/responses\"}}}");
    CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
    return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
  };
  const auto gateway = descriptor_for("https://openrouter.ai");
  const auto direct = descriptor_for("https://api.openai.com");
  const auto replace_model = [](std::string wire, std::string_view model) {
    const auto pos = wire.find("\"model\":\"fixture-model\""); CHECK(pos != std::string::npos);
    wire.replace(pos, std::string("\"model\":\"fixture-model\"").size(), "\"model\":" + json::quote(model));
    return wire;
  };
  const auto decode = [&](const descriptor::ValidatedDescriptor& d, const responses::Request& r,
                          const std::vector<Frame>& wires, bool sse) {
    auto encoded = responses::encode(d, r, sse); CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
    Accumulator acc;
    responses::Codec codec(d, sse ? responses::Mode::Sse : responses::Mode::Buffered, acc,
        std::get<responses::EncodedRequest>(encoded).context);
    if (sse) { for (const auto& [name, wire] : wires) if (!codec.frame(name, wire)) break; codec.finish(); }
    else codec.buffered(wires.front().second, {});
    CHECK(acc.outcome()); return *acc.outcome();
  };
  const std::string money = R"({"input_tokens":16,"output_tokens":6,"total_tokens":22,"cost":4e-6,"is_byok":false,"cost_details":{"upstream_inference_cost":0.000004,"upstream_inference_input_cost":1.6e-6,"upstream_inference_output_cost":2.4e-6}})";
  for (bool sse : {false, true}) {
    const auto wire = body("[" + message + "]", "completed", money);
    const auto outcome = decode(gateway, request(), sse ? std::vector<Frame>{created(), added(0, message), item_done(0, message),
        terminal("[" + message + "]", "completed", money)} : std::vector<Frame>{{"", wire}}, sse);
    const auto& c = completed(outcome); const auto& cost = c.usage.provider_cost;
    CHECK(text(c.messages) == "hello" && c.usage.total->value == 22 && c.usage.extra.empty());
    CHECK(c.usage.stage == UsageStage::Final && cost.source == CostSource::OpenRouterUsd && cost.quality == UsageQuality::Consistent);
    CHECK(cost.total->nano_usd == 4000 && cost.upstream_total->nano_usd == 4000);
    CHECK(cost.upstream_input->nano_usd == 1600 && cost.upstream_output->nano_usd == 2400);
    CHECK(cost.total->evidence == Evidence::Reported && cost.total->rounding == CostRounding::CeilingShortestDecimal);
    CHECK(cost.is_byok && !*cost.is_byok && cost.byok_status == CostStatus::Available);
    CHECK(c.wire_envelope->root().get("usage").get("cost_details").is_object());
  }
  for (const auto& [amount, status, nanos] : std::vector<std::tuple<std::string, CostStatus, uint64_t>>{
      {"0", CostStatus::Available, 0}, {"0.125", CostStatus::Available, 125000000},
      {"0.1", CostStatus::Available, 100000000}, {"1e-10", CostStatus::Available, 1},
      {"5e-324", CostStatus::Available, 1}, {"null", CostStatus::Missing, 0},
      // Captured OpenRouter decimals convert exactly; binary64 ceiling used to add one nanoUSD.
      {"1.5e-06", CostStatus::Available, 1500}, {"0.0000015", CostStatus::Available, 1500},
      {"0.00029", CostStatus::Available, 290000}, {"0.0002724", CostStatus::Available, 272400},
      {"0.0000176", CostStatus::Available, 17600}, {"0.0000371", CostStatus::Available, 37100},
      {"0.00004928", CostStatus::Available, 49280}, {"0.0006649", CostStatus::Available, 664900},
      {"123.456789012", CostStatus::Available, 123456789012}, {"9007199.25474099", CostStatus::Available, 9007199254740990},
      {"9007199.254740992", CostStatus::PrecisionExceeded, 0},  // shortest form is ...993, above 2^53
      // Only a nonzero digit beyond the ninth fractional place rounds up.
      {"1.0000000001", CostStatus::Available, 1000000001}, {"1.0000000009", CostStatus::Available, 1000000001},
      {"0.0000000015", CostStatus::Available, 2}, {"1.0000000001e-9", CostStatus::Available, 2},
      {"0.000012345678901", CostStatus::Available, 12346}, {"1.000000000000001e-12", CostStatus::Available, 1},
      {"1e-9", CostStatus::Available, 1}, {"2.5e-9", CostStatus::Available, 3},
      {"-1", CostStatus::Malformed, 0}, {"\"0.5\"", CostStatus::Malformed, 0},
      {"true", CostStatus::Malformed, 0}, {"{}", CostStatus::Malformed, 0},
      {"\"NaN\"", CostStatus::Malformed, 0}, {"\"Infinity\"", CostStatus::Malformed, 0},
      {"9007199", CostStatus::Available, 9007199000000000},
      {"20000000000", CostStatus::Overflow, 0},
      {"9007199.5", CostStatus::PrecisionExceeded, 0}, {"1e100", CostStatus::Overflow, 0}}) {
    const auto outcome = decode(gateway, request(), {{"", body("[" + message + "]", "completed", "{\"cost\":" + amount + "}")}}, false);
    const auto& c = completed(outcome); const auto& cost = c.usage.provider_cost;
    CHECK(text(c.messages) == "hello" && cost.status[0] == status);
    if (status == CostStatus::Available) CHECK(cost.total && cost.total->nano_usd == nanos);
    else CHECK(!cost.total);
    CHECK(cost.quality == (status == CostStatus::Available || status == CostStatus::Missing ? UsageQuality::Consistent : UsageQuality::Inconsistent));
    CHECK(c.usage.extra.empty());
  }
  const auto captured = decode(gateway, request(), {{"", body("[]", "completed",
      R"({"cost":0.00029,"is_byok":false,"cost_details":{"upstream_inference_cost":0.00029,"upstream_inference_input_cost":0.0002724,"upstream_inference_output_cost":0.0000176}})")}}, false);
  const auto& sum = completed(captured).usage.provider_cost;
  CHECK(sum.total->nano_usd == 290000 && sum.upstream_input->nano_usd + sum.upstream_output->nano_usd == sum.upstream_total->nano_usd);
  const auto partial_fields = decode(gateway, request(), {{"", body("[]", "completed",
      R"({"cost_details":{"upstream_inference_input_cost":0,"upstream_inference_output_cost":-1,"future_price":0.25},"is_byok":true})")}}, false);
  const auto& fields = completed(partial_fields).usage.provider_cost;
  CHECK(!fields.total && !fields.upstream_total && fields.upstream_input->nano_usd == 0 && !fields.upstream_output);
  CHECK(fields.status[0] == CostStatus::Missing && fields.status[1] == CostStatus::Missing);
  CHECK(fields.status[2] == CostStatus::Available && fields.status[3] == CostStatus::Malformed && fields.quality == UsageQuality::Inconsistent);
  CHECK(fields.is_byok && *fields.is_byok && completed(partial_fields).usage.extra.empty());
  for (const auto& [prompt_alias, expected] : std::vector<std::pair<std::string, CostStatus>>{
      {"1e-10", CostStatus::Available}, {"2e-10", CostStatus::Conflict}, {"false", CostStatus::Malformed}}) {
    const std::string counters = R"({"cost":4e-6,"cost_details":{"upstream_inference_input_cost":1e-10,"upstream_inference_prompt_cost":)"
        + prompt_alias + R"(,"upstream_inference_output_cost":2.4e-6,"upstream_inference_completions_cost":2.4e-6}})";
    for (bool sse : {false, true}) {
      const auto outcome = decode(gateway, request(), sse ? std::vector<Frame>{created(), terminal("[]", "completed", counters)}
          : std::vector<Frame>{{"", body("[]", "completed", counters)}}, sse);
      const auto& cost = completed(outcome).usage.provider_cost;
      CHECK(cost.total->nano_usd == 4000 && cost.upstream_output->nano_usd == 2400 && cost.status[2] == expected);
      CHECK(completed(outcome).usage.extra.empty());
      if (expected == CostStatus::Available) CHECK(cost.upstream_input->nano_usd == 1 && cost.quality == UsageQuality::Consistent);
      else CHECK(!cost.upstream_input && cost.quality == UsageQuality::Inconsistent);
      CHECK(completed(outcome).wire_envelope->root().get("usage").get("cost_details").get("upstream_inference_prompt_cost").valid());
    }
  }
  const auto chat_names = decode(gateway, request(), {{"", body("[]", "completed",
      R"({"cost_details":{"upstream_inference_prompt_cost":1.6e-6,"upstream_inference_completions_cost":2.4e-6}})")}}, false);
  CHECK(completed(chat_names).usage.provider_cost.upstream_input->nano_usd == 1600);
  CHECK(completed(chat_names).usage.provider_cost.upstream_output->nano_usd == 2400);
  const auto unknown = decode(desc(), request(), {{"", body("[]", "completed", money)}}, false);
  CHECK(completed(unknown).usage.provider_cost.source == CostSource::UnknownCurrency);
  CHECK(completed(unknown).usage.provider_cost.status[0] == CostStatus::UnknownCurrency);
  CHECK(!completed(unknown).usage.provider_cost.total && completed(unknown).usage.provider_cost.quality == UsageQuality::Inconsistent);
  const auto malformed = decode(gateway, request(), {{"", body("[]", "completed", R"({"cost_details":[],"is_byok":"false"})")}}, false);
  CHECK(completed(malformed).usage.provider_cost.status[1] == CostStatus::Malformed);
  CHECK(completed(malformed).usage.provider_cost.byok_status == CostStatus::Malformed);
  CHECK(!completed(malformed).usage.provider_cost.is_byok);
  for (const auto& final_usage : {std::string("null"), usage}) {
    const auto cleared = decode(gateway, request(), {event("response.created", ",\"response\":" + body("[]", "in_progress", money)),
        terminal("[]", "completed", final_usage)}, true);
    CHECK(!completed(cleared).usage.provider_cost.total && completed(cleared).usage.provider_cost.source == CostSource::None);
  }
  const auto partial = decode(gateway, request(), {event("response.created", ",\"response\":" + body("[]", "in_progress", money))}, true);
  CHECK(failed(partial, ErrorKind::Truncated).partial.usage.stage == UsageStage::Partial);
  CHECK(failed(partial, ErrorKind::Truncated).partial.usage.provider_cost.total->nano_usd == 4000);
  failed(decode(gateway, request(), {{"", body("[]", "completed", R"({"cost":0.25,"input_tokens":1.5})")}}, false), ErrorKind::ProtocolCorrupt);
  failed(decode(gateway, request(), {{"", body("[]", "completed", R"({"cost":0.25,"unknown_tokens":1.5})")}}, false), ErrorKind::ProtocolCorrupt);

  auto alias = request(); alias.model = "gpt-4.1-nano";
  const std::string snapshot = "gpt-4.1-nano-2025-04-14";
  const auto output = "[" + reasoning + "," + call + "," + message + "]";
  for (bool sse : {false, true}) {
    auto wires = sse ? std::vector<Frame>{created(), added(0, reasoning), item_done(0, reasoning),
        added(1, call), item_done(1, call), added(2, message), item_done(2, message), terminal(output)}
        : std::vector<Frame>{{"", body(output)}};
    for (auto& [name, wire] : wires) if (wire.find("\"model\":\"fixture-model\"") != std::string::npos) wire = replace_model(wire, snapshot);
    const auto first = decode(direct, alias, wires, sse); const auto& c = completed(first);
    CHECK(c.messages[0].native && c.messages[0].native->complete());
    CHECK(c.wire_envelope->root().get("model").as_string() == snapshot);
    auto replay = alias; replay.messages.push_back(c.messages[0]);
    replay.messages.push_back(Message{"", Role::Tool, {ToolResult{"call_1", "one"}}});
    auto encoded = responses::encode(direct, replay, false); CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
    CHECK(parse(std::get<responses::EncodedRequest>(encoded).body).root().get("model").as_string() == alias.model);
    const std::string next_reasoning = R"({"id":"rs_2","type":"reasoning","status":"completed","summary":[{"type":"summary_text","text":"Continue carefully"}],"encrypted_content":"SEALED_NATIVE_MARKER_second"})";
    const std::string next_message = R"({"id":"msg_2","type":"message","status":"completed","role":"assistant","content":[{"type":"output_text","text":"continued","annotations":[]}]})";
    auto next_reply_wire = replace_model(body("[" + next_reasoning + "," + next_message + "]"), snapshot);
    next_reply_wire.replace(next_reply_wire.find("resp_1"), std::string("resp_1").size(), "resp_2");
    const auto replay_reply = decode(direct, replay, {{"", next_reply_wire}}, false);
    CHECK(completed(replay_reply).messages[0].id == "resp_2");
    CHECK(completed(replay_reply).messages[0].wire_output->root().at(0).get("id").as_string() == "rs_2");
    CHECK(completed(replay_reply).messages[0].wire_output->root().at(1).get("id").as_string() == "msg_2");
    CHECK(text(completed(replay_reply).messages) == "continued");
    auto replay_next = replay; replay_next.messages.push_back(completed(replay_reply).messages[0]);
    replay_next.messages.push_back(Message{"", Role::User, {Text{"continue"}}});
    CHECK(std::holds_alternative<responses::EncodedRequest>(responses::encode(direct, replay_next, false)));
    auto changed = replay; changed.model = snapshot;
    CHECK(std::get<Error>(responses::encode(direct, changed, false)).kind == ErrorKind::ReplayIneligible);
    auto cursor = alias; cursor.previous_response_id = c.messages[0].id;
    cursor.previous_response_history = cursor.messages; cursor.previous_response_history.push_back(c.messages[0]);
    cursor.messages = {Message{"", Role::Tool, {ToolResult{"call_1", "one"}}}};
    encoded = responses::encode(direct, cursor, false); CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
    const auto cursor_wire = parse(std::get<responses::EncodedRequest>(encoded).body);
    CHECK(cursor_wire.root().get("model").as_string() == alias.model && cursor_wire.root().get("previous_response_id").as_string() == "resp_1");
    const auto cursor_reply = decode(direct, cursor, {{"", next_reply_wire}}, false);
    const auto& cursor_message = completed(cursor_reply).messages[0];
    CHECK(cursor_message.native && completed(cursor_reply).wire_envelope->root().get("model").as_string() == snapshot);
    auto cursor_next = alias; cursor_next.previous_response_id = cursor_message.id;
    cursor_next.previous_response_history = {cursor_message};
    cursor_next.messages = {Message{"", Role::User, {Text{"continue"}}}};
    CHECK(std::holds_alternative<responses::EncodedRequest>(responses::encode(direct, cursor_next, false)));
    auto cursor_mismatch = cursor_next; cursor_mismatch.model = snapshot;
    CHECK(std::get<Error>(responses::encode(direct, cursor_mismatch, false)).kind == ErrorKind::ReplayIneligible);
    changed = cursor; changed.model = snapshot;
    CHECK(std::get<Error>(responses::encode(direct, changed, false)).kind == ErrorKind::ReplayIneligible);
    changed = cursor; changed.previous_response_id = "foreign";
    CHECK(std::get<Error>(responses::encode(direct, changed, false)).kind == ErrorKind::ReplayIneligible);
    auto explicit_request = alias; explicit_request.model = snapshot;
    completed(decode(direct, explicit_request, wires, sse));
    // A declared routing gateway may serve another concrete model; replay stays bound to the request.
    const auto routed = decode(gateway, alias, wires, sse);
    CHECK(completed(routed).wire_envelope->root().get("model").as_string() == snapshot);
    auto routed_replay = alias; routed_replay.messages.push_back(completed(routed).messages[0]);
    routed_replay.messages.push_back(Message{"", Role::Tool, {ToolResult{"call_1", "one"}}});
    auto routed_encoded = responses::encode(gateway, routed_replay, false);
    CHECK(std::holds_alternative<responses::EncodedRequest>(routed_encoded));
    CHECK(parse(std::get<responses::EncodedRequest>(routed_encoded).body).root().get("model").as_string() == alias.model);
    if (sse) {
      wires.back().second = replace_model(terminal(output).second, "gpt-4.1-nano-2025-04-15");
      failed(decode(direct, alias, wires, true), ErrorKind::ProtocolCorrupt);
      failed(decode(gateway, alias, wires, true), ErrorKind::ProtocolCorrupt);
      wires.back().second = replace_model(terminal(output).second, alias.model);
      failed(decode(direct, alias, wires, true), ErrorKind::ProtocolCorrupt);
    }
  }
  // No name fails a completed response: a suffix of digits and dashes is a snapshot by its shape,
  // and any other name is unexplained.
  for (const auto& served : {"gpt-4.1-nano-2025-02-29", "gpt-4.1-nano-2025-04-31"})
    CHECK(completed(decode(direct, alias, {{"", replace_model(body("[]"), served)}}, false)).model.relation == ServedRelation::Snapshot);
  for (const auto& served : {"gpt-4.1-mini-2025-04-14", "gpt-4.1-nano-2025-04-14-extra"})
    CHECK(completed(decode(direct, alias, {{"", replace_model(body("[]"), served)}}, false)).model.relation == ServedRelation::Unexplained);
  auto explicit_request = alias; explicit_request.model = snapshot;
  CHECK(completed(decode(direct, explicit_request, {{"", replace_model(body("[]"), "gpt-4.1-nano-2025-04-15")}}, false)).model.relation ==
        ServedRelation::Unexplained);
  auto namespaced = alias; namespaced.model = "openai/gpt-4.1-nano";
  CHECK(completed(decode(direct, namespaced, {{"", replace_model(body("[]"), "openai/gpt-4.1-nano-2025-04-14")}}, false)).model.relation ==
        ServedRelation::Snapshot);
  const auto leap = replace_model(body("[]"), "gpt-4.1-nano-2024-02-29");
  completed(decode(direct, alias, {{"", leap}}, false));
  // Observed OpenRouter routing alias: the gateway answers with the concrete model it served.
  auto routing = request(); routing.model = "~deepseek/deepseek-v4-flash-latest";
  const auto served = replace_model(body("[" + message + "]"), "deepseek/deepseek-v4-flash-0731");
  const auto at_gateway = decode(gateway, routing, {{"", served}}, false);
  CHECK(completed(at_gateway).wire_envelope->root().get("model").as_string() == "deepseek/deepseek-v4-flash-0731");
  CHECK(completed(at_gateway).model.relation == ServedRelation::Routed && completed(at_gateway).model.served == "deepseek/deepseek-v4-flash-0731");
  CHECK(completed(decode(direct, routing, {{"", served}}, false)).model.relation == ServedRelation::Unexplained);
}
void corruption_and_bounds() {
  auto frames = text_frames(); frames[3] = text_delta("foreign", 0, "hel"); failed(stream(frames), ErrorKind::ProtocolCorrupt);
  frames = text_frames(); frames.back() = terminal("[]"); failed(stream(frames), ErrorKind::ProtocolCorrupt);
  frames = text_frames(); frames.push_back(text_delta("msg_1", 0, "late")); failed(stream(frames), ErrorKind::ProtocolCorrupt);
  failed(stream({{"response.created", terminal("[]").second}}), ErrorKind::ProtocolCorrupt);
  auto sequenced = std::vector<Frame>{event("response.created", ",\"sequence_number\":2,\"response\":" + body("[]", "in_progress", "null")),
    event("response.in_progress", ",\"sequence_number\":1,\"response\":" + body("[]", "in_progress", "null"))};
  failed(stream(sequenced), ErrorKind::ProtocolCorrupt);
  for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
      {"\"id\":\"resp_1\"", "\"id\":null"}, {"\"object\":\"response\"", "\"object\":\"message\""},
      {"\"created_at\":1", "\"created_at\":-1"}, {"\"model\":\"fixture-model\"", "\"model\":\"\""}}) {
    auto invalid = body("[]"); const auto position = invalid.find(from); CHECK(position != std::string::npos);
    invalid.replace(position, from.size(), to); failed(buffered(invalid), ErrorKind::ProtocolCorrupt);
  }
  failed(buffered(body("[]", "incomplete", usage)), ErrorKind::ProtocolCorrupt);
  failed(buffered(body("[]", "completed", usage, R"({"reason":"max_output_tokens"})")), ErrorKind::ProtocolCorrupt);
  SemanticLimits small; small.max_content_bytes = 256;
  const auto large = "{\"id\":\"rs_large\",\"type\":\"reasoning\",\"summary\":[],\"encrypted_content\":" + json::quote(std::string(257, 'x')) + "}";
  failed(buffered(body("[" + large + "]"), {}, small), ErrorKind::ResourceLimit);
  small = {}; small.max_parts = 1; failed(buffered(body("[" + reasoning + "," + message + "]"), {}, small), ErrorKind::ResourceLimit);
  small = {}; small.max_tool_bytes = 4; failed(buffered(body("[" + call + "]"), {}, small), ErrorKind::ResourceLimit);
}
void reviewed_wire_boundaries() {
  size_t failures = 0;
  auto check = [&](std::string_view name, auto scenario) {
    try { scenario(); }
    catch (const std::exception&) { ++failures; std::cerr << "Responses boundary failed: " << name << '\n'; }
  };
  check("incomplete-valid-json-tool", [&] {
    const std::string item = R"({"id":"fc_1","type":"function_call","status":"incomplete","call_id":"call_1","name":"lookup","arguments":"{\"x\":1}"})";
    for (const auto& outcome : {
        buffered(body("[" + item + "]", "incomplete", usage, R"({"reason":"max_output_tokens"})")),
        stream({created(), added(0, item), item_done(0, item),
            terminal("[" + item + "]", "incomplete", usage, R"({"reason":"max_output_tokens"})")})}) {
      const auto& c = completed(outcome);
      CHECK(c.stop.kind == StopKind::MaxTokens);
      CHECK(std::holds_alternative<InvalidToolCall>(c.messages[0].parts[0]));
      const auto& call = std::get<InvalidToolCall>(c.messages[0].parts[0]);
      CHECK(call.reason == InvalidReason::Truncated && call.raw_fragment == "{\"x\":1}");
      CHECK(!c.messages[0].native || !c.messages[0].native->complete());
    }
  });
  check("terminal-unknown-usage", [&] {
    const auto outcome = stream({event("response.created", ",\"response\":" + body("[]", "in_progress", usage)),
        terminal("[]", "completed", "null")});
    const auto& value = completed(outcome).usage;
    CHECK(value.stage == UsageStage::Missing && !value.input_total && !value.output_total &&
        !value.provider_reported_total && !value.reasoning && value.extra.empty());
  });
  check("wrong-annotation-owner", [&] {
    failed(stream({created(), added(0, reasoning), event("response.output_text.annotation.added",
        R"(,"item_id":"rs_1","output_index":0,"content_index":0,"annotation_index":0,"annotation":{"type":"file_path","file_id":"f","index":0})")}),
        ErrorKind::ProtocolCorrupt);
  });
  check("corrupt-annotation-and-logprob", [&] {
    for (const auto& field : {std::string("\"annotations\":[7]"), std::string("\"annotations\":[],\"logprobs\":[7]")}) {
      const auto item = R"({"id":"msg_1","type":"message","status":"completed","role":"assistant","content":[{"type":"output_text","text":"hello",)"
          + field + "}]}";
      failed(buffered(body("[" + item + "]")), ErrorKind::ProtocolCorrupt);
    }
  });
  CHECK(failures == 0);
}
// A failure names the check that failed with a code of the fixed table, whatever the response says; the codec's own text
// stays as it was and nothing the vendor sent reaches either.
void failure_reasons() {
  const std::string secret = "VENDOR_SECRET_MARKER_5d2";
  const auto check = [&](const Outcome& outcome, ErrorKind kind, std::string_view message, std::string_view code) {
    const auto& error = failed(outcome, kind).error;
    CHECK(error.safe_message == message && error.reason == code);
    CHECK(error.reason.find(secret) == std::string::npos && error.safe_message.find(secret) == std::string::npos);
  };
  const auto served = [&](std::string wire) { wire.replace(wire.find("fixture-model"), 13, secret); return wire; };
  // Another served name is recorded, not refused (served_model_is_recorded).
  CHECK(completed(buffered(served(body("[" + message + "]")))).model.relation == ServedRelation::Unexplained);
  auto frames = text_frames();
  const std::vector<Frame> opening(frames.begin(), frames.begin() + 3);
  check(stream({event("response." + secret)}), ErrorKind::Unsupported, "unsupported semantic Responses event", "responses.event.unsupported");
  check(stream({opening[1]}), ErrorKind::ProtocolCorrupt, "event before response.created", "responses.stream.event_before_created");
  check(stream(opening), ErrorKind::Truncated, "missing Responses terminal evidence", "responses.terminal.missing");
  const auto cut = stream(opening, {false, ErrorKind::Cancelled});
  CHECK(failed(cut, ErrorKind::Cancelled).error.reason.empty() && failed(cut, ErrorKind::Cancelled).error.safe_message == "abnormal response close");
}
// A text part opens with the "" of content_part.added, and output_text.done repeats the whole text, which leaves an
// empty remainder after a stream that delivered every delta. Neither empty delta reaches the sink; the part does.
void empty_text_delta_is_withheld() {
  std::string trace;
  Accumulator acc({}, [&](const Event& event) {
    if (std::holds_alternative<PartBegin>(event)) trace += "[begin]";
    else if (const auto* d = std::get_if<PartDelta>(&event)) trace += "[delta:" + std::string(d->payload.bytes) + "]";
    else if (std::holds_alternative<PartSeal>(event)) trace += "[seal]";
  });
  responses::Codec codec(desc(), responses::Mode::Sse, acc, context());
  for (const auto& [name, data] : text_frames()) CHECK(codec.frame(name, data));
  codec.finish(); CHECK(acc.outcome());
  CHECK(trace == "[begin][delta:hel][delta:lo][seal]");
  CHECK(text(completed(*acc.outcome()).messages) == "hello" && completed(*acc.outcome()).messages[0].parts.size() == 1);
}
} // namespace
int main() {
  try { empty_text_delta_is_withheld(); failure_reasons(); provider_money_and_snapshot_aliases(); named_error_precedence(); named_error_raw_capacity(); completed_reasoning_ciphertext_authority(); reviewed_wire_boundaries(); typed_encode(); controls_and_cursor_boundaries(); openrouter_max_price(); strict_tool_schemas(); previous_response_ownership(); grouped_native_and_owned_outcomes(); normal_close_required(); cancellation_observation_boundary(); reasoning_stream_snapshots(); tools_interleaving_and_ownership(); incomplete_and_server_items(); unknown_hosted_item_ownership(); unknown_wire_observation_boundary(); usage_boundaries(); uncached_input_rule(); corruption_and_bounds();
    named_includes(); hosted_tool_options(); openrouter_server_tools_and_images(); service_item_progress_events(); computer_calls_and_mcp_approvals(); plugins_and_citations();
    std::cout << "Responses semantic contracts passed\n"; return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
