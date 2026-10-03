#include "codecs/responses.h"
#include "codecs/responses_request.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
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
    CHECK(std::holds_alternative<Error>(mismatch) && std::get<Error>(mismatch).kind == ErrorKind::ReplayIneligible);
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
  try { named_error_precedence(); named_error_raw_capacity(); completed_reasoning_ciphertext_authority(); reviewed_wire_boundaries(); typed_encode(); grouped_native_and_owned_outcomes(); normal_close_required(); cancellation_observation_boundary(); reasoning_stream_snapshots(); tools_interleaving_and_ownership(); incomplete_and_server_items(); unknown_hosted_item_ownership(); unknown_wire_observation_boundary(); usage_boundaries(); corruption_and_bounds();
    std::cout << "Responses semantic contracts passed\n"; return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
