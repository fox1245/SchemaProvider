#include "codecs/messages.h"
#include "codecs/messages_request.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
const descriptor::ValidatedDescriptor& descriptor_value() {
  static auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"synthetic-messages","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(loaded);
}
std::shared_ptr<const NativeContext> context() {
  messages::Request request; request.model = "fixture-model"; request.account_scope = "fixture-account";
  request.messages.push_back(Message{"", Role::User, {Text{"Hello"}}});
  auto result = messages::encode(descriptor_value(), request, true);
  CHECK(std::holds_alternative<messages::EncodedRequest>(result));
  return std::get<messages::EncodedRequest>(result).context;
}
using Frame = std::pair<std::string, std::string>;
const std::string initial_usage = R"({"input_tokens":2,"cache_read_input_tokens":0,"cache_creation_input_tokens":0,"output_tokens":0})";
std::string body(std::string_view content = "[]", std::string_view reason = "\"end_turn\"", std::string_view usage = R"({"input_tokens":2,"cache_read_input_tokens":0,"cache_creation_input_tokens":0,"output_tokens":3})", std::string_view stop_fields = "") {
  return "{\"type\":\"message\",\"id\":\"msg-1\",\"role\":\"assistant\",\"model\":\"fixture-model\",\"content\":" + std::string(content) + ",\"stop_reason\":" + std::string(reason) + ",\"usage\":" + std::string(usage) + std::string(stop_fields) + "}";
}
Frame start(std::string_view usage = initial_usage) { return {"message_start", "{\"type\":\"message_start\",\"message\":" + body("[]", "null", usage) + "}"}; }
Frame block(size_t index, std::string_view value) { return {"content_block_start", "{\"type\":\"content_block_start\",\"index\":" + std::to_string(index) + ",\"content_block\":" + std::string(value) + "}"}; }
Frame delta(size_t index, std::string_view value) { return {"content_block_delta", "{\"type\":\"content_block_delta\",\"index\":" + std::to_string(index) + ",\"delta\":" + std::string(value) + "}"}; }
Frame end(size_t index) { return {"content_block_stop", "{\"type\":\"content_block_stop\",\"index\":" + std::to_string(index) + "}"}; }
Frame stop(std::string_view reason = "\"end_turn\"", std::string_view usage = R"({"output_tokens":3})", std::string_view extra = "") { return {"message_delta", "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":" + std::string(reason) + std::string(extra) + "},\"usage\":" + std::string(usage) + "}"}; }
const Frame done{"message_stop", R"({"type":"message_stop"})"};
const Frame ping{"ping", R"({"type":"ping"})"};
Outcome stream(const std::vector<Frame>& frames, messages::Close close = {}, SemanticLimits limits = {}) {
  Accumulator acc(limits); messages::Codec codec(descriptor_value(), messages::Mode::Sse, acc, context(), limits);
  for (const auto& [event, data] : frames) if (!codec.frame(event, data)) break;
  codec.finish(close); CHECK(acc.outcome()); return *acc.outcome();
}
Outcome buffered(std::string_view wire, messages::Close close = {}, SemanticLimits limits = {}) {
  Accumulator acc(limits); messages::Codec codec(descriptor_value(), messages::Mode::Buffered, acc, context(), limits);
  codec.buffered(wire, close); CHECK(acc.outcome()); return *acc.outcome();
}
const Completion& completed(const Outcome& o) { CHECK(std::holds_alternative<Completion>(o)); return std::get<Completion>(o); }
const Failure& failed(const Outcome& o, ErrorKind k) { CHECK(std::holds_alternative<Failure>(o)); const auto& f = std::get<Failure>(o); CHECK(f.error.kind == k); return f; }
std::string text(const std::vector<Message>& messages) {
  std::string result;
  for (const auto& m : messages) for (const auto& p : m.parts) if (auto t = std::get_if<Text>(&p)) result += t->value;
  return result;
}
void owned_raw_observations() {
  for (bool streaming : {false, true}) {
    std::vector<Frame> sources;
    const std::string content = R"([{"type":"text","text":"kept"}])";
    auto source = streaming ? start().second : body(content);
    source.pop_back();
    source += R"(,"vendor":{"z":[3,{"b":true,"a":null}],"a":"retained"},"service_tier":"future"})";
    sources.emplace_back(streaming ? "message_start" : "", source);
    if (streaming) {
      sources.push_back(block(0, R"({"type":"text","text":"kept"})"));
      sources.push_back(end(0)); sources.push_back(stop()); sources.push_back(done);
    }
    std::vector<RawWire> observed;
    bool begun = false;
    const auto outcome = [&] {
      Accumulator accumulator({}, [&](const Event& event) {
        if (const auto* raw = std::get_if<RawWire>(&event)) {
          CHECK(raw->payload->root().dump() == sources.at(observed.size()).second);
          observed.push_back(*raw);
        } else if (std::holds_alternative<Begin>(event)) {
          CHECK(observed.at(0).payload->root().get("vendor").get("a").as_string() == "retained");
          begun = true;
        }
      });
      messages::Codec codec(descriptor_value(), streaming ? messages::Mode::Sse : messages::Mode::Buffered, accumulator, context());
      if (streaming) {
        for (const auto& [event, wire] : sources) CHECK(codec.frame(event, wire));
        codec.finish();
      } else CHECK(codec.buffered(source, {}));
      CHECK(accumulator.outcome());
      return *accumulator.outcome();
    }();
    const auto& completion = completed(outcome);
    CHECK(begun && text(completion.messages) == "kept");
    CHECK(completion.raw_events.size() == sources.size());
    for (size_t i = 0; i < sources.size(); ++i) {
      CHECK(completion.raw_events[i].payload == observed[i].payload);
      CHECK(completion.raw_events[i].payload->root().dump() == sources[i].second);
      CHECK(completion.raw_events[i].type == (streaming ? sources[i].first : "message"));
    }
    const auto nested = completion.raw_events[0].payload->root().get("vendor").get("z").at(1);
    auto member = nested.members().begin();
    CHECK((*member++).key == "b" && (*member).key == "a");
  }
}
void raw_failure_evidence() {
  const auto initial = start();
  const auto content = block(0, R"({"type":"text","text":"partial"})");
  const Frame error{"error", R"({"type":"error","error":{"message":"private vendor error","extra":[2,1]},"vendor":{"opaque":true}})"};
  const auto outcome = [&] {
    Accumulator accumulator;
    messages::Codec codec(descriptor_value(), messages::Mode::Sse, accumulator, context());
    CHECK(codec.frame(initial.first, initial.second));
    CHECK(codec.frame(content.first, content.second));
    CHECK(!codec.frame(error.first, error.second));
    CHECK(!codec.frame(ping.first, ping.second));
    codec.finish({false, ErrorKind::Cancelled});
    return *accumulator.outcome();
  }();
  const auto& failure = failed(outcome, ErrorKind::RemoteFailure);
  CHECK(text(failure.partial.messages) == "partial");
  CHECK(failure.partial.raw_events.size() == 3);
  CHECK(failure.partial.raw_events[0].payload->root().dump() == initial.second);
  CHECK(failure.partial.raw_events[1].payload->root().dump() == content.second);
  CHECK(failure.partial.raw_events[2].type == "error");
  CHECK(failure.partial.raw_events[2].payload->root().dump() == error.second);
  CHECK(failure.error.safe_message.find("private") == std::string::npos);
  for (const auto& message : failure.partial.messages) CHECK(!message.native || !message.native->complete());
  for (const auto& malformed : {"{", "unstructured error", "[]", R"({"x":1,"x":2})"}) {
    const auto rejected = stream({{"error", malformed}});
    CHECK(failed(rejected, ErrorKind::RemoteFailure).partial.raw_events.empty());
  }
  const auto truncated = stream({initial, content});
  const auto& partial = failed(truncated, ErrorKind::Truncated).partial;
  CHECK(partial.raw_events.size() == 2);
  CHECK(partial.raw_events[1].payload->root().dump() == content.second);
  CHECK(text(partial.messages) == "partial");
  for (const auto& message : partial.messages) CHECK(!message.native || !message.native->complete());
  const auto buffered_wire = body(R"([{"type":"text","text":"partial"}])");
  const auto buffered_truncated = buffered(buffered_wire, {false, ErrorKind::Truncated});
  CHECK(failed(buffered_truncated, ErrorKind::Truncated).partial.raw_events[0].payload->root().dump() == buffered_wire);
  const auto buffered_error = buffered(error.second);
  CHECK(failed(buffered_error, ErrorKind::RemoteFailure).partial.raw_events[0].payload->root().dump() == error.second);
}
void typeless_buffered_error() {
  const std::string wire = R"({"error":{"type":"overloaded_error","message":"private vendor error","extra":[2,1]},"vendor":{"b":true,"a":null}})";
  std::shared_ptr<const json::Document> observed;
  size_t commits = 0;
  const auto outcome = [&] {
    Accumulator acc({}, [&](const Event& event) {
      if (const auto* raw = std::get_if<RawWire>(&event)) observed = raw->payload;
      if (std::holds_alternative<Commit>(event)) ++commits;
    });
    messages::Codec codec(descriptor_value(), messages::Mode::Buffered, acc, context());
    CHECK(!codec.buffered(wire, {}));
    CHECK(acc.outcome()); return *acc.outcome();
  }();
  const auto& f = failed(outcome, ErrorKind::RemoteFailure);
  CHECK(commits == 0 && f.partial.messages.empty() && !f.partial.wire_envelope && !f.partial.stop);
  CHECK(f.partial.raw_events.size() == 1 && f.partial.raw_events[0].type == "messages.buffered");
  CHECK(observed && f.partial.raw_events[0].payload == observed && observed->root().dump() == wire);
  CHECK(!observed->root().get("type").valid() && f.error.safe_message.find("private") == std::string::npos);
  CHECK(f.partial.usage.stage == UsageStage::Missing && !f.partial.usage.total);
  const auto cancelled = buffered(wire, {false, ErrorKind::Cancelled});
  CHECK(failed(cancelled, ErrorKind::Cancelled).partial.raw_events[0].payload->root().dump() == wire);
  const std::string large = "{\"error\":{\"message\":" + json::quote(std::string(1 << 20, 'x')) + "}}";
  SemanticLimits limits; limits.max_content_bytes = 2 << 20;
  Accumulator capped(limits, {}, 4096);
  messages::Codec codec(descriptor_value(), messages::Mode::Buffered, capped, context(), limits);
  CHECK(!codec.buffered(large, {}));
  const auto& overflow = failed(*capped.outcome(), ErrorKind::RemoteFailure);
  CHECK(overflow.partial.raw_events.empty() && overflow.partial.messages.empty() && !overflow.partial.stop);
}
void terminal_matrix() {
  const std::vector<Frame> sequence{start(), block(0, R"({"type":"text","text":"partial"})"), end(0), stop(), done};
  for (size_t n = 0; n <= sequence.size(); ++n) for (bool normal : {false, true}) {
    auto outcome = stream(std::vector<Frame>(sequence.begin(), sequence.begin() + static_cast<std::ptrdiff_t>(n)), {normal, ErrorKind::Truncated});
    if (n == sequence.size() && normal) {
      const auto& c = completed(outcome); CHECK(text(c.messages) == "partial"); CHECK(c.messages[0].native && c.messages[0].native->complete());
    } else {
      const auto& f = failed(outcome, ErrorKind::Truncated); CHECK(text(f.partial.messages) == (n > 1 ? "partial" : ""));
      for (const auto& m : f.partial.messages) CHECK(!m.native || !m.native->complete());
    }
  }
  failed(stream({ping, ping}), ErrorKind::Truncated);
  failed(stream({start(), done}), ErrorKind::Truncated);
  failed(buffered(body("[]", "null")), ErrorKind::Truncated);
  failed(stream({start(), stop(), done, done}), ErrorKind::ProtocolCorrupt);
  auto late = stream({start(), block(0, R"({"type":"text","text":"kept"})"), end(0), stop(), done, {"error", "private unparsed secret"}});
  CHECK(text(failed(late, ErrorKind::RemoteFailure).partial.messages) == "kept");
  CHECK(std::get<Failure>(late).error.safe_message.find("secret") == std::string::npos);
  failed(stream({start(), stop(), done, {"ping", R"({"type":"error","error":{"message":"secret"}})"}}), ErrorKind::RemoteFailure);
  size_t terminals = 0;
  Accumulator acc({}, [&](const Event& e) { if (std::holds_alternative<Commit>(e) || std::holds_alternative<Fail>(e)) ++terminals; });
  messages::Codec codec(descriptor_value(), messages::Mode::Sse, acc, context());
  for (const auto& [event, data] : sequence) CHECK(codec.frame(event, data));
  CHECK(terminals == 0 && !acc.outcome()); codec.finish(); CHECK(terminals == 1);
  codec.finish({false, ErrorKind::Cancelled}); CHECK(!codec.frame("error", "{}")); CHECK(terminals == 1);
}
void text_thinking_and_order() {
  const auto b = buffered(body(R"([{"type":"thinking","thinking":"consider","signature":"signed"},{"type":"redacted_thinking","data":"encrypted"},{"type":"text","text":"héllo"}])"));
  const auto s = stream({start(), block(2, R"({"type":"text","text":"hé"})"), block(0, R"({"type":"thinking","thinking":"","signature":""})"),
    delta(0, R"({"type":"thinking_delta","thinking":"consider"})"), block(1, R"({"type":"redacted_thinking","data":"encrypted"})"),
    delta(2, R"({"type":"text_delta","text":"llo"})"), delta(0, R"({"type":"signature_delta","signature":"sig"})"),
    delta(0, R"({"type":"signature_delta","signature":"ned"})"), end(2), end(0), end(1), stop(), done});
  for (const auto* outcome : {&b, &s}) {
    const auto& c = completed(*outcome); CHECK(c.stop.kind == StopKind::EndTurn); CHECK(text(c.messages) == "héllo");
    const auto& parts = c.messages[0].parts; CHECK(parts.size() == 3);
    const auto& thinking = std::get<Thinking>(parts[0]); CHECK(thinking.text == "consider" && thinking.signature == "signed");
    CHECK(std::get<RedactedThinking>(parts[1]).data == "encrypted");
    CHECK(c.usage.input_total->value == 2 && c.usage.output_total->value == 3 && c.usage.total->value == 5);
  }
  const auto unsigned_thinking = buffered(body(R"([{"type":"thinking","thinking":""},{"type":"thinking","thinking":"","signature":""}])"));
  const auto& parts = completed(unsigned_thinking).messages[0].parts;
  CHECK(!std::get<Thinking>(parts[0]).signature); CHECK(std::get<Thinking>(parts[1]).signature == "");
  failed(stream({start(), block(0, R"({"type":"thinking","thinking":""})"), delta(0, R"({"type":"signature_delta","signature":"x"})"), delta(0, R"({"type":"thinking_delta","thinking":"late"})")}), ErrorKind::ProtocolCorrupt);
}
void tool_fragments() {
  const std::string args = R"({"city":"서울","nested":[1,true]})";
  for (size_t split = 0; split <= args.size(); ++split) {
    // Split only at UTF-8 boundaries; JSON encoders reject invalid text fragments.
    if (split < args.size() && (static_cast<unsigned char>(args[split]) & 0xc0) == 0x80) continue;
    auto outcome = stream({start(), block(1, R"({"type":"tool_use","id":"b","name":"no_args","input":{}})"),
      block(0, R"({"type":"tool_use","id":"a","name":"lookup","input":{},"caller":{"type":"direct"}})"),
      delta(0, "{\"type\":\"input_json_delta\",\"partial_json\":" + json::quote(std::string_view(args).substr(0, split)) + "}"),
      delta(1, R"({"type":"input_json_delta","partial_json":""})"),
      delta(0, "{\"type\":\"input_json_delta\",\"partial_json\":" + json::quote(std::string_view(args).substr(split)) + "}"), end(1), end(0), stop("\"tool_use\""), done});
    const auto& parts = completed(outcome).messages[0].parts;
    const auto& a = std::get<ToolCall>(parts[0]); const auto& b = std::get<ToolCall>(parts[1]);
    CHECK(a.id == "a" && a.input->root().get("city").as_string() == "서울" && a.wire_type == "tool_use");
    CHECK(a.wire_metadata && a.wire_metadata->root().get("caller").get("type").as_string() == "direct");
    CHECK(b.id == "b" && b.input->root().is_object() && b.input->root().size() == 0);
  }
  struct Bad { std::string args, stop; InvalidReason reason; };
  for (const auto& bad : std::vector<Bad>{{"{bad}", "end_turn", InvalidReason::NotJson}, {"{\"x\":", "max_tokens", InvalidReason::Truncated}, {"{\"x\":1,\"x\":2}", "tool_use", InvalidReason::DuplicateKey}, {"[]", "tool_use", InvalidReason::NotJson}}) {
    auto outcome = stream({start(), block(0, R"({"type":"tool_use","id":"a","name":"f","input":{}})"), delta(0, "{\"type\":\"input_json_delta\",\"partial_json\":" + json::quote(bad.args) + "}"), end(0), stop(json::quote(bad.stop)), done});
    const auto& invalid = std::get<InvalidToolCall>(completed(outcome).messages[0].parts[0]);
    CHECK(invalid.raw_fragment == bad.args && invalid.reason == bad.reason);
  }
  const auto duplicate_wire = body(R"([{"type":"tool_use","id":"a","name":"f","input":{"x":1,"x":2}}])", "\"tool_use\"");
  const auto duplicate = buffered(duplicate_wire);
  const auto& duplicate_completion = completed(duplicate);
  const auto& invalid = std::get<InvalidToolCall>(duplicate_completion.messages[0].parts[0]);
  CHECK(invalid.reason == InvalidReason::DuplicateKey && invalid.raw_fragment == R"({"x":1,"x":2})");
  CHECK(duplicate_completion.raw_events[0].payload->root().dump() == duplicate_wire);
  auto members = duplicate_completion.raw_events[0].payload->root().get("content").at(0).get("input").members();
  auto member = members.begin();
  CHECK((*member).key == "x" && (*member++).value.as_uint() == 1);
  CHECK((*member).key == "x" && (*member++).value.as_uint() == 2 && member == members.end());
  failed(buffered(body(R"([{"type":"tool_use","id":"a","name":"f","input":{"x":1,"x":2},"caller":{"type":"direct","type":"direct"}}])")), ErrorKind::ProtocolCorrupt);
}
void server_semantics() {
  const std::string call = R"({"type":"server_tool_use","id":"srv_a","name":"web_search","input":{"query":"x"}})";
  const std::string result = R"({"type":"web_search_tool_result","tool_use_id":"srv_a","content":[{"type":"web_search_result","title":"title","url":"https://example.test","encrypted_content":"opaque"}]})";
  auto b = buffered(body("[" + call + "," + result + "]"));
  auto s = stream({start(), block(0, call), end(0), block(1, result), end(1), stop(), done});
  for (const auto* o : {&b, &s}) {
    const auto& c = completed(*o); CHECK(c.stop.kind == StopKind::EndTurn);
    CHECK(std::get<ToolCall>(c.messages[0].parts[0]).kind == ToolCallKind::ServerExecuted);
    const auto& r = std::get<ServerToolResult>(c.messages[0].parts[1]);
    CHECK(r.tool_use_id == "srv_a" && r.wire_type == "web_search_tool_result");
    CHECK(r.content->root().get("content").at(0).get("encrypted_content").as_string() == "opaque");
  }
  auto paused = buffered(body("[" + call + "]", "\"pause_turn\"")); CHECK(completed(paused).stop.kind == StopKind::PauseTurn);
  failed(buffered(body("[" + result + "]")), ErrorKind::ProtocolCorrupt);
  failed(buffered(body("[" + call + "," + result + "," + result + "]")), ErrorKind::ProtocolCorrupt);
  failed(buffered(body(R"([{"type":"tool_use","id":"srv_a","name":"web_search","input":{}},)" + result + "]")), ErrorKind::ProtocolCorrupt);
  failed(buffered(body("[" + call + R"(,{"type":"web_search_tool_result","tool_use_id":"srv_a","content":[{"type":"web_search_result","url":"x","title":"x","encrypted_content":42}]}])")), ErrorKind::ProtocolCorrupt);
  const auto encrypted = buffered(body(R"([{"type":"server_tool_use","id":"srv_c","name":"code_execution","input":{}},{"type":"code_execution_tool_result","tool_use_id":"srv_c","content":{"type":"encrypted_code_execution_result","return_code":0,"stderr":"","encrypted_stdout":"cipher","content":[]}}])"));
  CHECK(std::get<ServerToolResult>(completed(encrypted).messages[0].parts[1]).content->root().get("content").get("encrypted_stdout").as_string() == "cipher");
}
void server_result_shapes() {
  struct ResultCase { std::string name, type, content; };
  const std::vector<ResultCase> cases{
    {"web_search", "web_search_tool_result", R"({"type":"web_search_tool_result_error","error_code":"unavailable"})"},
    {"web_fetch", "web_fetch_tool_result", R"({"type":"web_fetch_result","url":"https://example.test","retrieved_at":null,"content":{"type":"document","title":null,"citations":{"enabled":true},"source":{"type":"text","media_type":"text/plain","data":"source text"}}})"},
    {"code_execution", "code_execution_tool_result", R"({"type":"code_execution_result","return_code":-1,"stderr":"failed","stdout":"","content":[{"type":"code_execution_output","file_id":"file-a"}]})"},
    {"bash_code_execution", "bash_code_execution_tool_result", R"({"type":"bash_code_execution_result","return_code":0,"stderr":"","stdout":"ok","content":[]})"},
    {"text_editor_code_execution", "text_editor_code_execution_tool_result", R"({"type":"text_editor_code_execution_view_result","content":"one\n","file_type":"text","num_lines":1,"start_line":1,"total_lines":1})"},
    {"text_editor_code_execution", "text_editor_code_execution_tool_result", R"({"type":"text_editor_code_execution_create_result","is_file_update":false})"},
    {"text_editor_code_execution", "text_editor_code_execution_tool_result", R"({"type":"text_editor_code_execution_str_replace_result","lines":["one"],"new_lines":1,"new_start":0,"old_lines":1,"old_start":0})"},
    {"tool_search_tool_regex", "tool_search_tool_result", R"({"type":"tool_search_tool_search_result","tool_references":[{"type":"tool_reference","tool_name":"lookup"}]})"}
  };
  for (const auto& item : cases) {
    const std::string call = "{\"type\":\"server_tool_use\",\"id\":\"srv\",\"name\":" + json::quote(item.name) + ",\"input\":{}}";
    const std::string result = "{\"type\":" + json::quote(item.type) + ",\"tool_use_id\":\"srv\",\"content\":" + item.content + "}";
    const auto expected = json::parse(result); CHECK(std::holds_alternative<json::Document>(expected));
    const auto b = buffered(body("[" + call + "," + result + "]"));
    const auto s = stream({start(), block(0, call), end(0), block(1, result), end(1), stop(), done});
    for (const auto* outcome : {&b, &s}) {
      const auto& c = completed(*outcome); CHECK(c.stop.kind == StopKind::EndTurn);
      const auto& stored = std::get<ServerToolResult>(c.messages[0].parts[1]);
      CHECK(json::equal(stored.content->root(), std::get<json::Document>(expected).root()));
    }
  }
}
void stop_classes() {
  const std::vector<std::pair<std::string, StopKind>> cases{{"end_turn", StopKind::EndTurn}, {"tool_use", StopKind::ToolUse}, {"max_tokens", StopKind::MaxTokens}, {"pause_turn", StopKind::PauseTurn}, {"refusal", StopKind::Refusal}, {"model_context_window_exceeded", StopKind::ContextLimit}, {"stop_sequence", StopKind::StopSequence}, {"future_reason", StopKind::Unknown}};
  for (const auto& [raw, kind] : cases) {
    const std::string extra = kind == StopKind::StopSequence ? ",\"stop_sequence\":\"END\"" : kind == StopKind::Refusal ? R"(,"stop_details":{"type":"refusal","category":"cyber","explanation":"declined"})" : "";
    const auto b = buffered(body("[]", json::quote(raw), initial_usage, extra));
    const auto s = stream({start(), stop(json::quote(raw), R"({"output_tokens":0})", extra), done});
    for (const auto* o : {&b, &s}) { const auto& c = completed(*o); CHECK(c.stop.kind == kind && c.stop.raw == raw); if (kind == StopKind::StopSequence) CHECK(c.stop.sequence == "END"); if (kind == StopKind::Refusal) CHECK(c.stop.details->root().get("category").as_string() == "cyber"); }
  }
  failed(buffered(body("[]", "\"stop_sequence\"")), ErrorKind::ProtocolCorrupt);
  failed(buffered(body("[]", "\"error\"")), ErrorKind::RemoteFailure);
}
void corruption_matrix() {
  const std::vector<std::vector<Frame>> invalid{
    {start(), start()}, {block(0, R"({"type":"text","text":"x"})")},
    {start(), end(0)}, {start(), delta(0, R"({"type":"text_delta","text":"x"})")},
    {start(), block(0, R"({"type":"text","text":"x"})"), block(0, R"({"type":"text","text":"y"})")},
    {start(), block(0, R"({"type":"text","text":"x"})"), end(0), end(0)},
    {start(), block(1, R"({"type":"text","text":"x"})"), end(1), stop()},
    {start(), block(0, R"({"type":"text","text":"x"})"), stop()},
    {start(), block(0, R"({"type":"text","text":"x"})"), end(0), delta(0, R"({"type":"text_delta","text":"y"})")},
    {start(), stop(), block(0, R"({"type":"text","text":"late"})")},
    {start(), block(0, R"({"type":"text","text":null})")},
    {start(), block(0, R"({"type":"thinking","thinking":"","signature":null})")},
    {start(), block(0, R"({"type":"text","text":""})"), delta(0, R"({"type":"thinking_delta","thinking":"wrong"})")},
    {start(), block(0, R"({"type":"tool_use","id":"a","name":"f","input":[]})")},
    {start(), block(0, R"({"type":"tool_use","id":"a","name":"f","input":{}})"), block(1, R"({"type":"tool_use","id":"a","name":"g","input":{}})")},
    {start(), {"message_delta", R"({"type":"message_delta","model":"other","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":1}})"}},
    {start(), {"message_delta", R"({"type":"message_delta","id":"other","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":1}})"}},
    {{"ping", R"({"type":"message_start"})"}}, {start(), stop(), stop("\"max_tokens\"")}
  };
  for (const auto& frames : invalid) failed(stream(frames), ErrorKind::ProtocolCorrupt);
  failed(stream({start(), {"future_event", R"({"type":"future_event"})"}}), ErrorKind::Unsupported);
  for (auto type : {"citations_delta", "refusal_delta"}) failed(stream({start(), block(0, R"({"type":"text","text":""})"), delta(0, "{\"type\":" + json::quote(type) + "}")}), ErrorKind::Unsupported);
  failed(buffered(body(R"([{"type":"text","text":"x","citations":[{}]}])")), ErrorKind::Unsupported);
  failed(buffered(body(R"([{"type":"container_upload","file_id":"x"}])")), ErrorKind::Unsupported);
  std::string wrong_model = body(); wrong_model.replace(wrong_model.find("fixture-model"), 13, "other-model");
  failed(buffered(wrong_model), ErrorKind::ProtocolCorrupt);
  Accumulator acc; messages::Codec no_context(descriptor_value(), messages::Mode::Buffered, acc, nullptr);
  CHECK(!no_context.buffered(body(), {})); failed(*acc.outcome(), ErrorKind::InvalidConfig);
}
void usage_knowledge() {
  auto unknown = buffered(body("[]", "\"end_turn\"", R"({"input_tokens":2,"output_tokens":3})"));
  const auto& u = completed(unknown).usage; CHECK(!u.input_total && !u.total && !u.cache_read && !u.cache_write); CHECK(u.input_uncached->value == 2);
  auto zero = buffered(body("[]", "\"end_turn\"", R"({"input_tokens":0,"output_tokens":0,"cache_read_input_tokens":0,"cache_creation_input_tokens":0})"));
  CHECK(completed(zero).usage.total->value == 0);
  auto evolved = stream({start(R"({"input_tokens":2,"output_tokens":1,"cache_read_input_tokens":null,"cache_creation_input_tokens":null})"),
    stop("null", R"({"input_tokens":4,"output_tokens":5,"cache_read_input_tokens":3,"cache_creation_input_tokens":2,"cache_creation":{"ephemeral_5m_input_tokens":2,"ephemeral_1h_input_tokens":0},"server_tool_use":{"web_search_requests":1},"output_tokens_details":{"thinking_tokens":2},"iterations":[{"input_tokens":40,"output_tokens":30}]})"),
    stop("\"end_turn\"", R"({"input_tokens":0,"output_tokens":0,"cache_read_input_tokens":null,"output_tokens_details":{"thinking_tokens":1}})"), done});
  const auto& c = completed(evolved).usage;
  CHECK(c.input_uncached->value == 4 && c.output_total->value == 5 && c.input_total->value == 9 && c.total->value == 14 && c.reasoning->value == 2);
  CHECK(c.quality == UsageQuality::Inconsistent && c.conflicts.size() == 3 && c.stage == UsageStage::Final);
  CHECK(c.extra.at("cache_creation.ephemeral_5m_input_tokens").value == 2 && c.extra.at("server_tool_use.web_search_requests").value == 1 && c.extra.at("iterations.0.output_tokens").value == 30);
  auto repeated = stream({start(), stop("null", R"({"output_tokens":3})"), stop(), done}); CHECK(completed(repeated).usage.quality == UsageQuality::Consistent);
  for (auto bad : {"{}", R"({"input_tokens":null,"output_tokens":0})", R"({"input_tokens":1,"output_tokens":false})", R"({"input_tokens":1,"output_tokens":1.2})", R"({"input_tokens":1,"output_tokens":-1})", R"({"input_tokens":1,"output_tokens":1,"cache_creation":{"ephemeral_5m_input_tokens":"2"}})", R"({"input_tokens":18446744073709551615,"output_tokens":1,"cache_read_input_tokens":0,"cache_creation_input_tokens":0})"}) {
    auto outcome = buffered(body(R"([{"type":"text","text":"kept"}])", "\"end_turn\"", bad));
    const auto& f = failed(outcome, ErrorKind::ProtocolCorrupt); CHECK(text(f.partial.messages) == "kept" && !f.partial.stop);
  }
  auto overflow = stream({start(), stop("\"end_turn\"", R"({"input_tokens":18446744073709551615,"output_tokens":1})")});
  CHECK(failed(overflow, ErrorKind::ProtocolCorrupt).partial.usage.input_uncached->value == 2);
  auto missing_delta = stream({start(), stop("\"end_turn\"", "{}")}); failed(missing_delta, ErrorKind::ProtocolCorrupt);
}
void usage_binding_precedence() {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"bound-messages","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1","paths":{"buffered":"/messages","streaming":"/messages"}},"bindings":{"usage":["metrics","tokens"]}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& selected = std::get<descriptor::ValidatedDescriptor>(loaded);
  messages::Request request; request.model = "fixture-model"; request.account_scope = "fixture-account";
  request.messages.push_back(Message{"", Role::User, {Text{"Hello"}}});
  const std::string decoy = R"({"input_tokens":999,"output_tokens":999})";
  for (bool streaming : {false, true}) {
    auto prepared = messages::encode(selected, request, streaming);
    CHECK(std::holds_alternative<messages::EncodedRequest>(prepared));
    Accumulator acc;
    messages::Codec codec(selected, streaming ? messages::Mode::Sse : messages::Mode::Buffered, acc,
                          std::get<messages::EncodedRequest>(prepared).context);
    const auto initial = body("[]", streaming ? "null" : "\"end_turn\"", decoy,
                              R"(,"metrics":{"tokens":{"input_tokens":3,"cache_read_input_tokens":4,"cache_creation_input_tokens":5,"output_tokens":7}})");
    if (streaming) {
      CHECK(codec.frame("message_start", "{\"type\":\"message_start\",\"message\":" + initial + "}"));
      CHECK(codec.frame("message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":999},"metrics":{"tokens":{"output_tokens":7}}})"));
      CHECK(codec.frame(done.first, done.second)); codec.finish();
    } else CHECK(codec.buffered(initial, {}));
    const auto& usage = completed(*acc.outcome()).usage;
    CHECK(usage.input_total && usage.input_total->value == 12);
    CHECK(usage.output_total && usage.output_total->value == 7);
    CHECK(usage.total && usage.total->value == 19);
  }
  auto prepared = messages::encode(selected, request, false);
  Accumulator acc;
  messages::Codec codec(selected, messages::Mode::Buffered, acc, std::get<messages::EncodedRequest>(prepared).context);
  codec.buffered(body("[]", "\"end_turn\"", initial_usage, R"(,"metrics":{"tokens":null})"), {});
  failed(*acc.outcome(), ErrorKind::ProtocolCorrupt);
}
void usage_detail_forward_compatibility() {
  for (const auto& [group, counter] : {std::pair{"cache_creation", "ephemeral_5m_input_tokens"},
                                      std::pair{"server_tool_use", "web_search_requests"},
                                      std::pair{"output_tokens_details", "thinking_tokens"}}) {
    for (auto annotation : {"\"future\"", "null", "false", "[]", "{}", "1.5", "-1"}) {
      auto values = initial_usage;
      values.pop_back();
      values += ",\"" + std::string(group) + "\":{\"" + counter + "\":0,\"future_counter\":7,\"future_annotation\":" + annotation + "}}";
      for (bool streaming : {false, true}) {
        const auto outcome = streaming ? stream({start(), stop("\"end_turn\"", values), done}) : buffered(body("[]", "\"end_turn\"", values));
        const auto& usage = completed(outcome).usage;
        CHECK(usage.total && usage.total->value == 2);
        CHECK(usage.quality == UsageQuality::Consistent);
        CHECK(usage.extra.at(std::string(group) + ".future_counter").value == 7);
        CHECK(!usage.extra.contains(std::string(group) + ".future_annotation"));
      }
    }
    auto malformed = initial_usage;
    malformed.pop_back();
    malformed += ",\"" + std::string(group) + "\":{\"" + counter + "\":\"wrong\"}}";
    failed(buffered(body("[]", "\"end_turn\"", malformed)), ErrorKind::ProtocolCorrupt);
    failed(stream({start(), stop("\"end_turn\"", malformed), done}), ErrorKind::ProtocolCorrupt);
  }
}
void toolset_metadata_shapes() {
  const std::vector<std::string> invalid{"[]", "17", "false", "\"\"", "\"bad scope\"", "\"é\"", json::quote(std::string(65, 'a'))};
  for (const auto& value : invalid) for (bool streaming : {false, true}) {
    const auto tool = "{\"type\":\"tool_use\",\"id\":\"call\",\"name\":\"f\",\"input\":{},\"toolset_name\":" + value + "}";
    size_t tool_begins = 0;
    Accumulator acc({}, [&](const Event& event) {
      if (const auto* part = std::get_if<PartBegin>(&event); part && part->kind == PartKind::ToolCall) ++tool_begins;
    });
    messages::Codec codec(descriptor_value(), streaming ? messages::Mode::Sse : messages::Mode::Buffered, acc, context());
    if (streaming) {
      const auto initial = start(); CHECK(codec.frame(initial.first, initial.second));
      const auto begin = block(0, tool); CHECK(!codec.frame(begin.first, begin.second));
      codec.finish();
    } else CHECK(!codec.buffered(body("[" + tool + "]", "\"tool_use\""), {}));
    const auto& failure = failed(*acc.outcome(), ErrorKind::ProtocolCorrupt);
    for (const auto& message : failure.partial.messages) CHECK(message.parts.empty());
    CHECK(tool_begins == 0);
  }
  for (const auto& value : {std::string("null"), std::string("\"set_A-1\""), json::quote(std::string(64, 'x'))}) {
    const auto tool = "{\"type\":\"tool_use\",\"id\":\"call\",\"name\":\"f\",\"input\":{},\"toolset_name\":" + value + "}";
    for (bool streaming : {false, true}) {
      const auto outcome = streaming ? stream({start(), block(0, tool), end(0), stop("\"tool_use\""), done}) : buffered(body("[" + tool + "]", "\"tool_use\""));
      const auto& call = std::get<ToolCall>(completed(outcome).messages[0].parts[0]);
      CHECK(call.wire_metadata && call.wire_metadata->root().get("toolset_name").dump() == value);
    }
  }
}
void limits_and_close_priority() {
  SemanticLimits limits; limits.max_parts = 1;
  failed(stream({start(), block(1, R"({"type":"text","text":"x"})")}, {}, limits), ErrorKind::ResourceLimit);
  limits = {}; limits.max_tool_bytes = 2;
  failed(buffered(body(R"([{"type":"tool_use","id":"a","name":"f","input":{"x":1}}])"), {}, limits), ErrorKind::ResourceLimit);
  limits = {}; limits.max_content_bytes = 64;
  failed(buffered(body(), {}, limits), ErrorKind::ResourceLimit);
  limits = {}; limits.max_json_depth = 2;
  failed(buffered(body(R"([{"type":"text","text":"x"}])"), {}, limits), ErrorKind::ResourceLimit);
  for (auto error : {ErrorKind::Cancelled, ErrorKind::DeadlineExceeded, ErrorKind::Transport, ErrorKind::Truncated}) {
    auto late = buffered(body(R"([{"type":"text","text":"kept"}])", "\"end_turn\"", "{}"), {false, error});
    CHECK(text(failed(late, error).partial.messages) == "kept");
    failed(buffered("{broken", {false, error}), error);
    auto s = stream({start(), block(0, R"({"type":"text","text":"kept"})"), end(0), stop(), done}, {false, error});
    CHECK(text(failed(s, error).partial.messages) == "kept");
  }
  // An already observed in-band failure is not rewritten by a subsequent close.
  failed(stream({start(), {"error", "{}"}}, {false, ErrorKind::Cancelled}), ErrorKind::RemoteFailure);
}
void routed_gateway_model() {
  static auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"gateway-messages","family":"anthropic.messages","connection":{"base_url":"https://openrouter.ai","paths":{"buffered":"/api/v1/messages","streaming":"/api/v1/messages"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& gateway = std::get<descriptor::ValidatedDescriptor>(loaded);
  CHECK(descriptor::routed_gateway(gateway) && !descriptor::routed_gateway(descriptor_value()));
  messages::Request request; request.model = "~anthropic/claude-haiku-latest"; request.account_scope = "fixture-account";
  request.messages.push_back(Message{"", Role::User, {Text{"Hello"}}});
  auto encoded = messages::encode(gateway, request, true); CHECK(std::holds_alternative<messages::EncodedRequest>(encoded));
  const auto routed_context = std::get<messages::EncodedRequest>(encoded).context;
  const auto served = [](std::string wire, std::string_view model) {
    const auto at = wire.find("fixture-model"); CHECK(at != std::string::npos); wire.replace(at, 13, model); return wire;
  };
  const auto run = [&](const descriptor::ValidatedDescriptor& d, std::shared_ptr<const NativeContext> ctx, const std::vector<Frame>& frames, bool sse) {
    Accumulator acc; messages::Codec codec(d, sse ? messages::Mode::Sse : messages::Mode::Buffered, acc, std::move(ctx));
    if (sse) { for (const auto& [event, data] : frames) if (!codec.frame(event, data)) break; codec.finish({}); }
    else codec.buffered(frames.front().second, {});
    CHECK(acc.outcome()); return *acc.outcome();
  };
  const std::string concrete = "anthropic/claude-haiku-4.5-20261001";
  const auto routed = run(gateway, routed_context, {{"", served(body(R"([{"type":"text","text":"hi"}])"), concrete)}}, false);
  CHECK(text(completed(routed).messages) == "hi");
  auto replay = request; replay.messages.push_back(completed(routed).messages[0]);
  replay.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  auto replay_encoded = messages::encode(gateway, replay, false);
  CHECK(std::holds_alternative<messages::EncodedRequest>(replay_encoded));
  auto replay_wire = json::parse(std::get<messages::EncodedRequest>(replay_encoded).body);
  CHECK(std::holds_alternative<json::Document>(replay_wire));
  CHECK(std::get<json::Document>(replay_wire).root().get("model").as_string() == request.model);
  const std::vector<Frame> frames{{"message_start", served(start().second, concrete)}, block(0, R"({"type":"text","text":"hi"})"), end(0), stop(), done};
  CHECK(text(completed(run(gateway, routed_context, frames, true)).messages) == "hi");
  // The served identity must remain stable for the whole response, even through a gateway.
  auto changed = frames;
  changed[3] = {"message_delta", R"({"type":"message_delta","model":"other","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":3}})"};
  failed(run(gateway, routed_context, changed, true), ErrorKind::ProtocolCorrupt);
  changed[3] = {"message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn","model":"other"},"usage":{"output_tokens":3}})"};
  failed(run(gateway, routed_context, changed, true), ErrorKind::ProtocolCorrupt);
  // A non-gateway origin keeps exact request/response model equality.
  failed(buffered(served(body(), concrete)), ErrorKind::ProtocolCorrupt);
}
} // namespace
int main() {
  try {
    routed_gateway_model();
    owned_raw_observations(); raw_failure_evidence(); typeless_buffered_error();
    usage_binding_precedence();
    usage_detail_forward_compatibility(); toolset_metadata_shapes();
    terminal_matrix(); text_thinking_and_order(); tool_fragments(); server_semantics(); server_result_shapes();
    stop_classes(); corruption_matrix(); usage_knowledge(); limits_and_close_priority();
    std::cout << "messages semantic tests passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
