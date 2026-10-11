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
    CHECK(cost.total->evidence == Evidence::Reported && cost.total->rounding == CostRounding::CeilingParsedBinary64);
    CHECK(cost.is_byok && !*cost.is_byok && cost.byok_status == CostStatus::Available);
    CHECK(c.wire_envelope->root().get("usage").get("cost_details").is_object());
  }
  for (const auto& [amount, status, nanos] : std::vector<std::tuple<std::string, CostStatus, uint64_t>>{
      {"0", CostStatus::Available, 0}, {"0.125", CostStatus::Available, 125000000},
      {"0.1", CostStatus::Available, 100000001}, {"1e-10", CostStatus::Available, 1},
      {"5e-324", CostStatus::Available, 1}, {"null", CostStatus::Missing, 0},
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
  for (const auto& served : {"gpt-4.1-mini-2025-04-14", "gpt-4.1-nano-2025-02-29", "gpt-4.1-nano-2025-04-31", "gpt-4.1-nano-2025-04-14-extra"}) {
    failed(decode(direct, alias, {{"", replace_model(body("[]"), served)}}, false), ErrorKind::ProtocolCorrupt);
  }
  auto explicit_request = alias; explicit_request.model = snapshot;
  failed(decode(direct, explicit_request, {{"", replace_model(body("[]"), "gpt-4.1-nano-2025-04-15")}}, false), ErrorKind::ProtocolCorrupt);
  auto namespaced = alias; namespaced.model = "openai/gpt-4.1-nano";
  failed(decode(direct, namespaced, {{"", replace_model(body("[]"), "openai/gpt-4.1-nano-2025-04-14")}}, false), ErrorKind::ProtocolCorrupt);
  const auto leap = replace_model(body("[]"), "gpt-4.1-nano-2024-02-29");
  completed(decode(direct, alias, {{"", leap}}, false));
  // Observed OpenRouter routing alias: the gateway answers with the concrete model it served.
  auto routing = request(); routing.model = "~deepseek/deepseek-v4-flash-latest";
  const auto served = replace_model(body("[" + message + "]"), "deepseek/deepseek-v4-flash-0731");
  CHECK(completed(decode(gateway, routing, {{"", served}}, false)).wire_envelope->root().get("model").as_string() == "deepseek/deepseek-v4-flash-0731");
  failed(decode(direct, routing, {{"", served}}, false), ErrorKind::ProtocolCorrupt);
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
      {"\"created_at\":1", "\"created_at\":-1"}, {"\"model\":\"fixture-model\"", "\"model\":\"foreign\""}}) {
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
} // namespace
int main() {
  try { provider_money_and_snapshot_aliases(); named_error_precedence(); named_error_raw_capacity(); completed_reasoning_ciphertext_authority(); reviewed_wire_boundaries(); typed_encode(); controls_and_cursor_boundaries(); previous_response_ownership(); grouped_native_and_owned_outcomes(); normal_close_required(); cancellation_observation_boundary(); reasoning_stream_snapshots(); tools_interleaving_and_ownership(); incomplete_and_server_items(); unknown_hosted_item_ownership(); unknown_wire_observation_boundary(); usage_boundaries(); corruption_and_bounds();
    std::cout << "Responses semantic contracts passed\n"; return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
