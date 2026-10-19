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
  // A server caller is a server tool type the policy admits, so a new code execution version admits its calls.
  CHECK(completed(buffered(body(R"([{"type":"tool_use","id":"a","name":"lookup","input":{},"caller":{"type":"code_execution_20260521","tool_id":"srv_1"}}])", "\"tool_use\""))).stop.kind == StopKind::ToolUse);
  failed(buffered(body(R"([{"type":"tool_use","id":"a","name":"lookup","input":{},"caller":{"type":"future_runner_20270101","tool_id":"srv_1"}}])", "\"tool_use\"")), ErrorKind::Unsupported);
  failed(buffered(body(R"([{"type":"tool_use","id":"a","name":"lookup","input":{},"caller":{"type":"code_execution_20260521"}}])", "\"tool_use\"")), ErrorKind::ProtocolCorrupt);
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
    {"tool_search_tool_regex", "tool_search_tool_result", R"({"type":"tool_search_tool_search_result","tool_references":[{"type":"tool_reference","tool_name":"lookup"}]})"},
    // A tool the service adds, and a result kind the SDK does not know, decode by the block's name.
    {"advisor", "advisor_tool_result", R"({"type":"advisor_result","text":"consider the edge case"})"},
    {"web_fetch", "web_fetch_tool_result", R"({"type":"web_fetch_result","url":"https://example.test","retrieved_at":null,"content":{"type":"document","title":null,"source":{"type":"base64","media_type":"image/png","data":"AA=="}}})"},
    {"code_execution", "code_execution_tool_result", R"({"type":"future_execution_result","note":"new"})"}
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
  // A result names its own tool: one for another tool is still uncorrelated.
  failed(buffered(body(R"([{"type":"server_tool_use","id":"srv","name":"advisor","input":{}},{"type":"web_search_tool_result","tool_use_id":"srv","content":[]}])")), ErrorKind::ProtocolCorrupt);
  failed(buffered(body(R"([{"type":"server_tool_use","id":"srv","name":"advisory","input":{}},{"type":"advisor_tool_result","tool_use_id":"srv","content":{"type":"advisor_result"}}])")), ErrorKind::ProtocolCorrupt);
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
  failed(stream({start(), block(0, R"({"type":"text","text":""})"), delta(0, R"({"type":"refusal_delta"})")}), ErrorKind::Unsupported);
  // Citations become citations of the text: a malformed one is corrupt, a valid one never fails the answer.
  failed(stream({start(), block(0, R"({"type":"text","text":""})"), delta(0, R"({"type":"citations_delta"})")}), ErrorKind::ProtocolCorrupt);
  failed(buffered(body(R"([{"type":"text","text":"x","citations":[{}]}])")), ErrorKind::ProtocolCorrupt);
  const std::string citation = R"({"type":"web_search_result_location","url":"https://example.test","title":"t","encrypted_index":"EI","cited_text":"x"})";
  const std::string location = R"({"type":"char_location","cited_text":"x","document_index":0,"document_title":"Notes","start_char_index":2,"end_char_index":5})";
  const auto cited = buffered(body("[{\"type\":\"text\",\"text\":\"x\",\"citations\":[" + citation + "," + location + "]}]"));
  CHECK(text(completed(cited).messages) == "x" && !completed(cited).raw_events.empty());
  {
    const auto& part = std::get<Text>(completed(cited).messages[0].parts[0]);
    CHECK(part.citations.size() == 2 && part.citations[0].kind == "web_search_result_location" && part.citations[0].url == "https://example.test" &&
          part.citations[0].title == "t" && !part.citations[0].start && part.citations[0].raw->root().get("encrypted_index").as_string() == "EI");
    CHECK(part.citations[1].kind == "char_location" && part.citations[1].title == "Notes" && part.citations[1].start == 2u && part.citations[1].end == 5u);
  }
  const auto cited_stream = stream({start(), block(0, R"({"type":"text","text":"","citations":[]})"), delta(0, R"({"type":"text_delta","text":"x"})"),
                                    delta(0, "{\"type\":\"citations_delta\",\"citation\":" + citation + "}"), end(0), stop(), done});
  CHECK(text(completed(cited_stream).messages) == "x");
  CHECK(std::get<Text>(completed(cited_stream).messages[0].parts[0]).citations.size() == 1 &&
        std::get<Text>(completed(cited_stream).messages[0].parts[0]).citations[0].url == "https://example.test");
  // A citation of a search result the caller wrote (#53) names its source and the cited block range.
  const std::string result_location = R"({"type":"search_result_location","cited_text":"Rotate them monthly.","source":"kb://guide","title":"Guide","search_result_index":0,"start_block_index":1,"end_block_index":2})";
  const auto from_search = buffered(body("[{\"type\":\"text\",\"text\":\"Monthly.\",\"citations\":[" + result_location + "]}]"));
  const auto& from_result = std::get<Text>(completed(from_search).messages[0].parts[0]);
  CHECK(from_result.citations.size() == 1 && from_result.citations[0].kind == "search_result_location" && from_result.citations[0].url == "kb://guide" &&
        from_result.citations[0].title == "Guide" && from_result.citations[0].start == 1u && from_result.citations[0].end == 2u &&
        from_result.citations[0].raw->root().get("search_result_index").as_uint() == 0u);
  failed(buffered(body(R"([{"type":"future_block","file_id":"x"}])")), ErrorKind::Unsupported);
  std::string wrong_model = body(); wrong_model.replace(wrong_model.find("fixture-model"), 13, "other-model");
  CHECK(completed(buffered(wrong_model)).model.relation == ServedRelation::Unexplained);
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
// Messages reports the exclusive uncached input itself; input_total is derived from it, so the
// cross-family identity input_uncached = input_total - cache_read - cache_write holds by construction.
void uncached_input_rule() {
  auto full = buffered(body("[]", "\"end_turn\"", R"({"input_tokens":10,"cache_read_input_tokens":20,"cache_creation_input_tokens":30,"output_tokens":3})"));
  const auto& u = completed(full).usage;
  CHECK(u.input_uncached->value == 10 && u.input_uncached->evidence == Evidence::Reported);
  CHECK(u.input_total->value == 60 && u.input_total->evidence == Evidence::Derived);
  CHECK(u.input_total->value - u.cache_read->value - u.cache_write->value == u.input_uncached->value);
  auto no_write = buffered(body("[]", "\"end_turn\"", R"({"input_tokens":10,"cache_read_input_tokens":20,"output_tokens":3})"));
  const auto& n = completed(no_write).usage;
  CHECK(n.input_uncached->value == 10 && n.input_uncached->evidence == Evidence::Reported && !n.input_total && !n.cache_write);
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
  CHECK(completed(routed).model.relation == ServedRelation::Routed && completed(routed).model.served == concrete);
  // Another origin delivers the same answer, with the name unexplained.
  CHECK(completed(buffered(served(body(), concrete))).model.relation == ServedRelation::Unexplained);
}
// Observed direct Anthropic behavior: the documented alias claude-haiku-4-5 is answered with
// "model":"claude-haiku-4-5-20251001". Any origin reads a dated suffix as a snapshot.
void direct_dated_snapshot_alias() {
  static auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"direct-messages","family":"anthropic.messages","connection":{"base_url":"https://api.anthropic.com","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& direct = std::get<descriptor::ValidatedDescriptor>(loaded);
  const auto request_for = [](std::string model) {
    messages::Request request; request.model = std::move(model); request.account_scope = "fixture-account";
    request.messages.push_back(Message{"", Role::User, {Text{"Hello"}}}); return request;
  };
  const auto served = [](std::string wire, std::string_view model) {
    for (auto at = wire.find("fixture-model"); at != std::string::npos; at = wire.find("fixture-model")) wire.replace(at, 13, model);
    return wire;
  };
  const auto run = [&](const descriptor::ValidatedDescriptor& d, const messages::Request& request, const std::vector<Frame>& frames, bool sse) {
    auto encoded = messages::encode(d, request, sse); CHECK(std::holds_alternative<messages::EncodedRequest>(encoded));
    Accumulator acc; messages::Codec codec(d, sse ? messages::Mode::Sse : messages::Mode::Buffered, acc, std::get<messages::EncodedRequest>(encoded).context);
    if (sse) { for (const auto& [event, data] : frames) if (!codec.frame(event, data)) break; codec.finish({}); }
    else codec.buffered(frames.front().second, {});
    CHECK(acc.outcome()); return *acc.outcome();
  };
  const auto wires = [&](std::string_view model, bool sse) {
    return sse ? std::vector<Frame>{{"message_start", served(start().second, model)}, block(0, R"({"type":"text","text":"hi"})"), end(0), stop(), done}
               : std::vector<Frame>{{"", served(body(R"([{"type":"text","text":"hi"}])"), model)}};
  };
  const auto alias = request_for("claude-haiku-4-5");
  const std::string snapshot = "claude-haiku-4-5-20251001";
  for (bool sse : {false, true}) {
    const auto first = run(direct, alias, wires(snapshot, sse), sse);
    const auto& c = completed(first);
    CHECK(text(c.messages) == "hi" && c.messages[0].native && c.messages[0].native->complete());
    CHECK(c.model.requested == alias.model && c.model.served == snapshot && c.model.relation == ServedRelation::Snapshot);
    // Replay stays bound to the requested alias, not the served snapshot.
    auto replay = alias; replay.messages.push_back(c.messages[0]); replay.messages.push_back(Message{"", Role::User, {Text{"again"}}});
    auto encoded = messages::encode(direct, replay, false); CHECK(std::holds_alternative<messages::EncodedRequest>(encoded));
    auto wire = json::parse(std::get<messages::EncodedRequest>(encoded).body); CHECK(std::holds_alternative<json::Document>(wire));
    CHECK(std::get<json::Document>(wire).root().get("model").as_string() == alias.model);
    auto changed = replay; changed.model = snapshot;
    CHECK(std::holds_alternative<Error>(messages::encode(direct, changed, false)) && std::get<Error>(messages::encode(direct, changed, false)).kind == ErrorKind::ReplayIneligible);
    // An explicit snapshot request is exact; another date is unexplained, still delivered.
    CHECK(completed(run(direct, request_for(snapshot), wires(snapshot, sse), sse)).model.relation == ServedRelation::Exact);
    CHECK(completed(run(direct, request_for(snapshot), wires("claude-haiku-4-5-20251002", sse), sse)).model.relation == ServedRelation::Unexplained);
    CHECK(completed(run(descriptor_value(), alias, wires(snapshot, sse), sse)).model.relation == ServedRelation::Snapshot);
  }
  // The served identity must remain stable for the whole response.
  for (const auto& later : {std::string("claude-haiku-4-5-20251002"), alias.model}) {
    auto frames = wires(snapshot, true);
    frames[3] = {"message_delta", R"({"type":"message_delta","model":")" + later + R"(","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":3}})"};
    failed(run(direct, alias, frames, true), ErrorKind::ProtocolCorrupt);
  }
  // A suffix of digits and dashes is a snapshot by its shape; anything else is unexplained. No name fails.
  for (const auto& suffixed : {"claude-haiku-4-5-20250229", "claude-haiku-4-5-00001001", "claude-haiku-4-5-2025-10-01",
                               "claude-haiku-4-5-2025100", "claude-haiku-4-5-202510011", "claude-haiku-4-5-20240229"})
    CHECK(completed(run(direct, alias, wires(suffixed, false), false)).model.relation == ServedRelation::Snapshot);
  for (const auto& other : {"claude-haiku-4-5-20251001-extra", "claude-haiku-4-6-20251001", "claude-haiku-4-520251001", "claude-haiku-4-5-2025100a"})
    CHECK(completed(run(direct, alias, wires(other, false), false)).model.relation == ServedRelation::Unexplained);
  CHECK(completed(run(direct, request_for("anthropic/claude-haiku-4-5"), wires("anthropic/claude-haiku-4-5-20251001", false), false)).model.relation ==
        ServedRelation::Snapshot);
  CHECK(completed(run(direct, request_for("Claude-Haiku-4-5"), wires("claude-haiku-4-5-20251001", false), false)).model.relation ==
        ServedRelation::Unexplained);
}
// Captured OpenRouter Messages wire (anthropic/claude-haiku-4.5, ids scrubbed). OpenRouter ends the
// stream with an extra `[DONE]` frame after message_stop and reports USD cost inside usage.
void openrouter_sentinel_and_cost() {
  static auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"captured-openrouter-messages","family":"anthropic.messages","connection":{"base_url":"https://openrouter.ai","paths":{"buffered":"/api/v1/messages","streaming":"/api/v1/messages"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& gateway = std::get<descriptor::ValidatedDescriptor>(loaded);
  const auto context_for = [](const descriptor::ValidatedDescriptor& d) {
    messages::Request request; request.model = "anthropic/claude-haiku-4.5"; request.account_scope = "fixture-account";
    request.messages.push_back(Message{"", Role::User, {Text{"Reply with one word: sky color?"}}});
    auto encoded = messages::encode(d, request, true); CHECK(std::holds_alternative<messages::EncodedRequest>(encoded));
    return std::get<messages::EncodedRequest>(encoded).context;
  };
  size_t unknown_properties = 0;
  const auto run = [&](const descriptor::ValidatedDescriptor& d, const std::vector<Frame>& frames, bool sse) {
    Accumulator acc; messages::Codec codec(d, sse ? messages::Mode::Sse : messages::Mode::Buffered, acc, context_for(d));
    if (sse) { for (const auto& [event, data] : frames) if (!codec.frame(event, data)) break; codec.finish({}); }
    else codec.buffered(frames.front().second, {});
    unknown_properties = codec.diagnostics().unknown_properties;
    CHECK(acc.outcome()); return *acc.outcome();
  };
  const std::string start_usage = R"({"input_tokens":3,"output_tokens":1,"output_tokens_details":null,"cache_creation_input_tokens":0,"cache_read_input_tokens":6219,"cache_creation":{"ephemeral_5m_input_tokens":0,"ephemeral_1h_input_tokens":0},"inference_geo":null,"server_tool_use":null,"service_tier":"standard","speed":"standard"})";
  const std::string money = R"(,"cost":0.0006649,"is_byok":false,"cost_details":{"upstream_inference_cost":0.0006649,"upstream_inference_prompt_cost":0.0006249,"upstream_inference_completions_cost":0.00004})";
  const std::string final_counters = R"({"input_tokens":3,"output_tokens":8,"output_tokens_details":{"thinking_tokens":0},"cache_creation_input_tokens":0,"cache_read_input_tokens":6219,"cache_creation":{"ephemeral_5m_input_tokens":0,"ephemeral_1h_input_tokens":0},"server_tool_use":null,"service_tier":"standard","speed":"standard")";
  const std::vector<Frame> captured{
      {"message_start", R"({"type":"message_start","message":{"id":"msg_fixture","type":"message","role":"assistant","container":null,"content":[],"model":"anthropic/claude-haiku-4.5","stop_reason":null,"stop_details":null,"stop_sequence":null,"usage":)" + start_usage + R"(,"provider":"Amazon Bedrock"}})"},
      {"content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":"","citations":[]}})"},
      {"content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"t"}})"},
      {"content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"undra-ember"}})"},
      {"content_block_stop", R"({"type":"content_block_stop","index":0})"},
      {"message_delta", R"({"type":"message_delta","delta":{"container":null,"stop_details":null,"stop_reason":"end_turn","stop_sequence":null},"usage":)" + final_counters + money + R"(},"context_management":null})"},
      {"message_stop", R"({"type":"message_stop"})"}};
  const auto with = [&](std::vector<Frame> frames, std::initializer_list<Frame> tail) { frames.insert(frames.end(), tail); return frames; };
  const auto check_stream = [&](const Outcome& outcome) {
    const auto& c = completed(outcome);
    CHECK(text(c.messages) == "tundra-ember" && c.stop.kind == StopKind::EndTurn);
    CHECK(c.usage.stage == UsageStage::Final && c.usage.output_total->value == 8 && c.usage.cache_read->value == 6219 && c.usage.reasoning->value == 0);
    const auto& cost = c.usage.provider_cost;
    CHECK(cost.source == CostSource::OpenRouterUsd && cost.quality == UsageQuality::Consistent && cost.is_byok && !*cost.is_byok);
    CHECK(cost.total->nano_usd == 664900 && cost.upstream_total->nano_usd == 664900);
    CHECK(cost.upstream_input->nano_usd == 624900 && cost.upstream_output->nano_usd == 40000);
    for (const auto status : cost.status) CHECK(status == CostStatus::Available);
    for (const auto& [key, value] : c.usage.extra) CHECK(key.starts_with("cache_creation."));
  };
  // The observed `event: data` sentinel, the default SSE event name, and no sentinel all complete.
  check_stream(run(gateway, with(captured, {{"data", "[DONE]"}}), true));
  check_stream(run(gateway, with(captured, {{"message", "[DONE]"}}), true));
  check_stream(run(gateway, captured, true));
  // Anything else after termination is still corrupt, as is a sentinel anywhere else.
  for (const auto& tail : std::vector<std::vector<Frame>>{
           {{"data", "[DONE]"}, {"data", "[DONE]"}}, {{"data", "not json"}}, {{"data", "[DONE] "}}, {{"error_like", "[DONE]"}},
           {{"data", "[DONE]"}, {"message_stop", R"({"type":"message_stop"})"}}, {{"message_stop", R"({"type":"message_stop"})"}}}) {
    auto frames = captured; frames.insert(frames.end(), tail.begin(), tail.end());
    failed(run(gateway, frames, true), ErrorKind::ProtocolCorrupt);
  }
  auto early = captured; early.insert(early.end() - 1, Frame{"data", "[DONE]"});
  failed(run(gateway, early, true), ErrorKind::ProtocolCorrupt);
  // A direct Anthropic origin never sends the sentinel, so it stays corrupt there.
  auto direct_frames = with(captured, {{"data", "[DONE]"}});
  for (auto& [event, data] : direct_frames) for (auto at = data.find("anthropic/claude-haiku-4.5"); at != std::string::npos; at = data.find("anthropic/claude-haiku-4.5"))
    data.replace(at, 26, "fixture-model");
  CHECK(text(completed(stream({direct_frames.begin(), direct_frames.end() - 1})).messages) == "tundra-ember");
  failed(stream(direct_frames), ErrorKind::ProtocolCorrupt);
  // Buffered: the captured envelope keeps its cost; a trailing sentinel is never part of a body.
  const std::string buffered_usage = R"({"input_tokens":29,"output_tokens":4,"output_tokens_details":{"thinking_tokens":0},"cache_creation_input_tokens":0,"cache_read_input_tokens":0,"cache_creation":{"ephemeral_5m_input_tokens":0,"ephemeral_1h_input_tokens":0},"inference_geo":null,"server_tool_use":null,"service_tier":"standard","speed":"standard")";
  const auto envelope = [&](std::string_view usage) {
    return R"({"id":"msg_fixture","type":"message","role":"assistant","container":null,"content":[{"type":"text","text":"Blue"}],"model":"anthropic/claude-haiku-4.5","stop_reason":"end_turn","stop_details":null,"stop_sequence":null,"usage":)" + std::string(usage) + R"(,"provider":"Amazon Bedrock"})";
  };
  const auto buffered_money = R"(,"cost":4.9e-05,"is_byok":false,"cost_details":{"upstream_inference_cost":4.9e-05,"upstream_inference_prompt_cost":2.9e-05,"upstream_inference_completions_cost":2e-05}})";
  const auto bought = run(gateway, {{"", envelope(buffered_usage + buffered_money)}}, false);
  const auto with_money_unknowns = unknown_properties;
  const auto& c = completed(bought);
  CHECK(text(c.messages) == "Blue" && c.usage.total->value == 33 && c.usage.extra.size() == 2);
  CHECK(c.usage.provider_cost.source == CostSource::OpenRouterUsd && c.usage.provider_cost.total->nano_usd == 49000);
  CHECK(c.usage.provider_cost.upstream_input->nano_usd == 29000 && c.usage.provider_cost.upstream_output->nano_usd == 20000);
  CHECK(c.usage.provider_cost.upstream_total->nano_usd == 49000);
  // Monetary members are typed cost, not unknown usage properties.
  CHECK(completed(run(gateway, {{"", envelope(buffered_usage + "}")}}, false)).usage.provider_cost.source == CostSource::None);
  CHECK(unknown_properties == with_money_unknowns);
  failed(run(gateway, {{"", envelope(buffered_usage + buffered_money) + "\n[DONE]"}}, false), ErrorKind::ProtocolCorrupt);
  // An origin without declared USD semantics keeps an explicit unknown currency.
  auto direct_body = envelope(buffered_usage + buffered_money);
  direct_body.replace(direct_body.find("anthropic/claude-haiku-4.5"), 26, "fixture-model");
  const auto unknown_currency = completed(buffered(direct_body)).usage.provider_cost;
  CHECK(unknown_currency.source == CostSource::UnknownCurrency && !unknown_currency.total && unknown_currency.status[0] == CostStatus::UnknownCurrency);
}

// A call that is cut off (cancelled, deadline, reset) has the usage the provider had reported so far,
// labelled partial, and nothing more. message_start carries a placeholder output count (the first token
// or none), not the count so far: a stream cut before message_delta has input evidence and no output
// evidence, which is not the same as zero output. The cumulative count of message_delta is evidence.
void cut_stream_usage_has_no_placeholder_output() {
  const std::string first_token = R"({"input_tokens":2,"cache_read_input_tokens":0,"cache_creation_input_tokens":0,"output_tokens":1})";
  const std::vector<Frame> prefix_frames{block(0, R"({"type":"text","text":""})"), delta(0, R"({"type":"text_delta","text":"partial"})")};
  for (const auto& usage : {initial_usage, first_token}) {
    for (const auto kind : {ErrorKind::Cancelled, ErrorKind::DeadlineExceeded, ErrorKind::Truncated, ErrorKind::Transport}) {
      std::vector<Frame> frames{start(usage)};
      frames.insert(frames.end(), prefix_frames.begin(), prefix_frames.end());
      const auto outcome = stream(frames, {false, kind});
      const auto& f = failed(outcome, kind);
      CHECK(text(f.partial.messages) == "partial");
      const auto& u = f.partial.usage;
      CHECK(u.stage == UsageStage::Partial && u.quality == UsageQuality::Consistent && u.conflicts.empty());
      CHECK(u.input_uncached && u.input_uncached->value == 2 && u.input_uncached->evidence == Evidence::Reported);
      CHECK(!u.output_total && !u.total && !u.provider_reported_total);
    }
  }
  // Before message_start there is nothing at all: no stage, no counter.
  for (const auto kind : {ErrorKind::Cancelled, ErrorKind::Transport}) {
    const auto outcome = stream({}, {false, kind});
    const auto& u = failed(outcome, kind).partial.usage;
    CHECK(u.stage == UsageStage::Missing && !u.input_total && !u.input_uncached && !u.output_total && !u.total && u.extra.empty());
  }
  // The cumulative count in message_delta is evidence, kept when the stream is cut after it.
  std::vector<Frame> frames{start(first_token)};
  frames.insert(frames.end(), prefix_frames.begin(), prefix_frames.end());
  frames.push_back(end(0)); frames.push_back(stop("\"end_turn\"", R"({"output_tokens":5})"));
  const auto cut_outcome = stream(frames, {false, ErrorKind::Cancelled});
  const auto& cut = failed(cut_outcome, ErrorKind::Cancelled).partial.usage;
  CHECK(cut.stage == UsageStage::Partial && cut.output_total && cut.output_total->value == 5 && cut.output_total->evidence == Evidence::Reported);
  frames.push_back(done);
  const auto whole_outcome = stream(frames);
  const auto& whole = completed(whole_outcome).usage;
  CHECK(whole.stage == UsageStage::Final && whole.output_total->value == 5 && whole.input_total->value == 2 && whole.total->value == 7);
  CHECK(whole.quality == UsageQuality::Consistent);
  // A final count below the one in message_start is still a conflict.
  const std::string large = R"({"input_tokens":2,"cache_read_input_tokens":0,"cache_creation_input_tokens":0,"output_tokens":5})";
  std::vector<Frame> regress{start(large), block(0, R"({"type":"text","text":""})"), end(0), stop("\"end_turn\"", R"({"output_tokens":3})"), done};
  CHECK(completed(stream(regress)).usage.quality == UsageQuality::Inconsistent);
}
// What a stream's sink is shown for the parts of a message: [begin], a text delta with its bytes, [seal]. An empty
// text delta would show as [delta:], and none may.
struct Traced { std::string trace; Outcome outcome; };
Traced traced(const std::vector<Frame>& frames, messages::Close close = {}) {
  Traced result;
  Accumulator acc({}, [&](const Event& event) {
    if (std::holds_alternative<PartBegin>(event)) result.trace += "[begin]";
    else if (const auto* d = std::get_if<PartDelta>(&event)) result.trace += "[" + std::string(d->payload.kind == PartKind::Text ? "delta:" : "other:") + std::string(d->payload.bytes) + "]";
    else if (std::holds_alternative<PartSeal>(event)) result.trace += "[seal]";
  });
  messages::Codec codec(descriptor_value(), messages::Mode::Sse, acc, context());
  for (const auto& [event, data] : frames) if (!codec.frame(event, data)) break;
  codec.finish(close); CHECK(acc.outcome()); result.outcome = *acc.outcome();
  return result;
}
void empty_text_delta_is_withheld() {
  // content_block_start carries the block's text, which is "" on every real stream: the Text part opens with an empty
  // delta. The part is still opened and sealed, in the same place as before, but the sink is shown only text.
  const auto opening = block(0, R"({"type":"text","text":""})");
  const auto streamed = traced({start(), opening, delta(0, R"({"type":"text_delta","text":"Hi"})"), delta(0, R"({"type":"text_delta","text":""})"),
                                delta(0, R"({"type":"text_delta","text":" there"})"), end(0), stop(), done});
  CHECK(streamed.trace == "[begin][delta:Hi][delta: there][seal]");
  const auto whole = buffered(body(R"([{"type":"text","text":"Hi there"}])"));
  CHECK(text(completed(streamed.outcome).messages) == "Hi there" && completed(streamed.outcome).messages[0].parts.size() == 1);
  CHECK(completed(streamed.outcome).messages[0].parts.size() == completed(whole).messages[0].parts.size() && completed(streamed.outcome).stop.kind == completed(whole).stop.kind);

  // A block that only ever received empty text is still there, and is the empty Text a buffered response returns.
  const auto only_empty = traced({start(), opening, end(0), stop(), done});
  CHECK(only_empty.trace == "[begin][seal]");
  CHECK(completed(only_empty.outcome).messages.size() == 1 && completed(only_empty.outcome).messages[0].parts.size() == 1);
  CHECK(std::holds_alternative<Text>(completed(only_empty.outcome).messages[0].parts[0]) && text(completed(only_empty.outcome).messages).empty());
  const auto empty_whole = buffered(body(R"([{"type":"text","text":""}])"));
  CHECK(completed(empty_whole).messages[0].parts.size() == 1 && std::holds_alternative<Text>(completed(empty_whole).messages[0].parts[0]));
  // Two text blocks keep their order and their separate parts, the first of them empty.
  const auto two = traced({start(), opening, end(0), block(1, R"({"type":"text","text":""})"), delta(1, R"({"type":"text_delta","text":"x"})"), end(1), stop(), done});
  CHECK(two.trace == "[begin][seal][begin][delta:x][seal]" && completed(two.outcome).messages[0].parts.size() == 2);

  // A call cut after the empty opening keeps the empty part in its partial output, as it did.
  const auto cut = traced({start(), opening}, {false, ErrorKind::Truncated});
  CHECK(cut.trace == "[begin]");
  const auto& partial = failed(cut.outcome, ErrorKind::Truncated).partial;
  CHECK(partial.messages.size() == 1 && partial.messages[0].parts.size() == 1 && std::holds_alternative<Text>(partial.messages[0].parts[0]));

  // Only text deltas are withheld: a thinking block's empty opening delta is delivered as it was.
  const auto thinking = traced({start(), block(0, R"({"type":"thinking","thinking":""})"), delta(0, R"({"type":"thinking_delta","thinking":"hm"})"), end(0), stop(), done});
  CHECK(thinking.trace == "[begin][other:][other:hm][seal]");
}
// The reported model is recorded and classified; no name fails a completed response. Only an
// unexplained name withholds replay authority from the native state.
void served_model_is_recorded() {
  const std::string content = R"([{"type":"thinking","thinking":"t","signature":"SIG"},{"type":"text","text":"answer"}])";
  auto with_model = [](std::string wire, std::string_view model) { wire.replace(wire.find("fixture-model"), 13, model); return wire; };
  auto replays = [](const Message& sealed) {
    messages::Request request; request.model = "fixture-model"; request.account_scope = "fixture-account";
    request.messages = {Message{"", Role::User, {Text{"Hello"}}}, sealed, Message{"", Role::User, {Text{"next"}}}};
    const auto encoded = messages::encode(descriptor_value(), request, true);
    if (const auto* error = std::get_if<Error>(&encoded)) { CHECK(error->kind == ErrorKind::ReplayIneligible); return false; }
    return true;
  };
  const auto exact = buffered(body(content));
  CHECK(completed(exact).model.requested == "fixture-model" && completed(exact).model.served == "fixture-model");
  CHECK(completed(exact).model.relation == ServedRelation::Exact && completed(exact).messages.at(0).native &&
        replays(completed(exact).messages.at(0)));
  const auto dated = buffered(with_model(body(content), "fixture-model-20261001"));
  CHECK(completed(dated).model.relation == ServedRelation::Snapshot && replays(completed(dated).messages.at(0)));
  for (const bool streaming : {false, true}) {
    auto foreign = start();
    foreign.second = with_model(foreign.second, "other-model");
    const auto outcome = streaming
        ? stream({foreign, block(0, R"({"type":"thinking","thinking":""})"), delta(0, R"({"type":"thinking_delta","thinking":"t"})"),
                  delta(0, R"({"type":"signature_delta","signature":"SIG"})"), end(0), block(1, R"({"type":"text","text":""})"),
                  delta(1, R"({"type":"text_delta","text":"answer"})"), end(1), stop(), done})
        : buffered(with_model(body(content), "other-model"));
    const auto& completion = completed(outcome);
    CHECK(completion.model.served == "other-model" && completion.model.relation == ServedRelation::Unexplained);
    CHECK(completion.joined_text() == "answer");
    // Delivered without a native seal, so a continuation that replays its thinking is refused.
    CHECK(!completion.messages.at(0).native && !replays(completion.messages.at(0)));
  }
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
  const auto text_block = block(0, R"({"type":"text","text":""})");
  check(stream({start(), text_block, delta(0, R"({"text":"x"})")}), ErrorKind::ProtocolCorrupt, "delta type required", "messages.delta.type_required");
  check(stream({start(), text_block, delta(0, "{\"type\":" + json::quote(secret) + "}")}), ErrorKind::Unsupported, "unknown content delta", "messages.delta.unknown");
  check(stream({start(), text_block, delta(0, R"({"type":"thinking_delta","thinking":"x"})")}), ErrorKind::ProtocolCorrupt, "delta kind differs from block", "messages.delta.kind_mismatch");
  check(stream({start(), text_block}), ErrorKind::Truncated, "missing Messages terminal evidence", "messages.terminal.missing");
  const auto cut = stream({start(), text_block}, {false, ErrorKind::Transport});
  CHECK(failed(cut, ErrorKind::Transport).error.reason.empty() && failed(cut, ErrorKind::Transport).error.safe_message == "abnormal response close");
}
} // namespace
// A code-execution container (dynamic filtering in the _20260318 web tools) and cache diagnostics
// describe the response; refusing them threw away a billed answer (#46). They stay in the raw events.
void response_metadata_is_kept() {
  const std::string container = R"("container":{"id":"container_fixture","expires_at":"2026-10-18T00:00:00Z"})";
  const auto b = buffered(body(R"([{"type":"text","text":"Blue"}])", "\"end_turn\"", initial_usage,
                               "," + container + R"(,"diagnostics":{"cache":{"status":"miss"}})"));
  const auto& c = completed(b);
  CHECK(text(c.messages) == "Blue");
  bool kept = false;
  for (const auto& raw : c.raw_events)
    if (raw.payload && raw.payload->root().get("container").get("id").is_string()) kept = true;
  CHECK(kept);
  const auto s = stream({start(), block(0, R"({"type":"text","text":""})"), delta(0, R"({"type":"text_delta","text":"Blue"})"), end(0),
                         stop("\"end_turn\"", R"({"output_tokens":3})", "," + container), done});
  CHECK(text(completed(s).messages) == "Blue");
  // A server-side context edit still changes what was answered.
  failed(buffered(body("[]", "\"end_turn\"", initial_usage, R"(,"context_management":{"applied_edits":[{"type":"clear_tool_uses_20250919"}]})")), ErrorKind::Unsupported);
  failed(buffered(body("[]", "\"end_turn\"", initial_usage, R"(,"container":"container_fixture")")), ErrorKind::ProtocolCorrupt);
}
// An advisor call and its result, with the advisor's iteration in usage (#53): the result block is kept whole, the
// advisor's model and the response's speed are labels, not counters.
// The MCP connector's blocks, and blocks the policy keeps whole (#53): a listing and a fallback are kept as captured,
// an MCP call is a server call that names its server, and its result pairs with it by id.
void mcp_and_kept_blocks() {
  const std::string listing = R"({"type":"mcp_tool_listing","mcp_server_name":"ctx","tools":[{"name":"resolve","input_schema":{"type":"object"}}]})";
  const std::string call = R"({"type":"mcp_tool_use","id":"mcptoolu_1","name":"resolve","server_name":"ctx","input":{"q":"react"}})";
  const std::string result = R"({"type":"mcp_tool_result","tool_use_id":"mcptoolu_1","is_error":false,"content":[{"type":"text","text":"/facebook/react"}]})";
  const std::string fallback = R"({"type":"fallback","from":{"model":"fixture-model"},"to":{"model":"fixture-other"},"trigger":{"type":"refusal","category":null}})";
  auto check = [&](const Outcome& outcome) {
    const auto& parts = completed(outcome).messages[0].parts;
    CHECK(parts.size() == 5);
    CHECK(std::get<Opaque>(parts[0]).wire_type == "mcp_tool_listing" && std::get<Opaque>(parts[0]).wire_metadata->root().get("tools").size() == 1);
    const auto& mcp = std::get<ToolCall>(parts[1]);
    CHECK(mcp.kind == ToolCallKind::ServerExecuted && mcp.name == "resolve" && mcp.wire_type == "mcp_tool_use" &&
          mcp.wire_metadata->root().get("server_name").as_string() == "ctx" && mcp.input->root().get("q").as_string() == "react");
    CHECK(std::get<ServerToolResult>(parts[2]).wire_type == "mcp_tool_result" && std::get<ServerToolResult>(parts[2]).tool_use_id == "mcptoolu_1");
    CHECK(std::get<Opaque>(parts[3]).wire_type == "fallback");
  };
  check(buffered(body("[" + listing + "," + call + "," + result + "," + fallback + R"(,{"type":"text","text":"ok"}])")));
  std::string open_call = call; open_call.replace(open_call.find(R"({"q":"react"})"), 13, "{}");
  check(stream({start(), block(0, listing), end(0), block(1, open_call), delta(1, R"({"type":"input_json_delta","partial_json":"{\"q\":\"react\"}"})"), end(1),
                block(2, result), end(2), block(3, fallback), end(3), block(4, R"({"type":"text","text":""})"), delta(4, R"({"type":"text_delta","text":"ok"})"), end(4),
                stop(), done}));
  // An MCP call names its server; a kept block takes no delta; an MCP result answers only an MCP call.
  std::string nameless = call; nameless.replace(nameless.find(R"("server_name":"ctx",)"), 20, "");
  failed(buffered(body("[" + nameless + "]", "\"tool_use\"")), ErrorKind::ProtocolCorrupt);
  failed(stream({start(), block(0, fallback), delta(0, R"({"type":"text_delta","text":"x"})")}), ErrorKind::ProtocolCorrupt);
  const std::string search = R"({"type":"server_tool_use","id":"srvtoolu_1","name":"web_search","input":{}})";
  failed(buffered(body("[" + search + R"(,{"type":"mcp_tool_result","tool_use_id":"srvtoolu_1","content":"x"}])")), ErrorKind::ProtocolCorrupt);
  failed(buffered(body("[" + call + R"(,{"type":"mcp_tool_result","tool_use_id":"mcptoolu_1","content":17}])")), ErrorKind::ProtocolCorrupt);
}
void advisor_result_and_iterations() {
  const std::string content = R"([{"type":"server_tool_use","id":"srvtoolu_adv","name":"advisor","input":{}},)"
                              R"({"type":"advisor_tool_result","tool_use_id":"srvtoolu_adv","content":{"type":"advisor_result","text":"Use a queue."}},)"
                              R"({"type":"text","text":"Done."}])";
  const std::string usage = R"({"input_tokens":2,"cache_read_input_tokens":0,"cache_creation_input_tokens":0,"output_tokens":3,"speed":"standard",)"
                            R"("iterations":[{"type":"message","input_tokens":2,"output_tokens":1},{"type":"advisor_message","model":"fixture-advisor","input_tokens":9,"output_tokens":4}]})";
  Accumulator acc; messages::Codec codec(descriptor_value(), messages::Mode::Buffered, acc, context());
  CHECK(codec.buffered(body(content, "\"end_turn\"", usage), {}));
  const auto& done = completed(*acc.outcome());
  const auto& parts = done.messages[0].parts;
  CHECK(std::get<ToolCall>(parts[0]).kind == ToolCallKind::ServerExecuted && std::get<ToolCall>(parts[0]).name == "advisor");
  const auto& result = std::get<ServerToolResult>(parts[1]);
  CHECK(result.wire_type == "advisor_tool_result" && result.content->root().get("content").get("text").as_string() == "Use a queue.");
  CHECK(done.usage.extra.at("iterations.1.input_tokens").value == 9 && !done.usage.extra.contains("iterations.1.model"));
  CHECK(codec.diagnostics().unknown_properties == 1);  // advisor_result is a shape the SDK keeps without checking
  failed(buffered(body("[]", "\"end_turn\"", R"({"input_tokens":2,"output_tokens":3,"speed":1})")), ErrorKind::ProtocolCorrupt);
}
// Compaction (#53): the summary block is kept whole, its stop is Compaction, and its iterations are counted apart
// from the top-level counters. A threshold block streams as a start with no content and one delta.
void compaction_blocks_and_usage() {
  const std::string signed_block = R"({"type":"compaction","content":"Summary: a recipe app.","signature":"c2lnbmVk"})";
  const std::string summary_usage = R"({"input_tokens":0,"output_tokens":0,"iterations":[{"input_tokens":170,"output_tokens":171,"cache_read_input_tokens":0,"cache_creation_input_tokens":0,"type":"compaction"}]})";
  const auto check_summary = [&](const Outcome& outcome) {
    const auto& c = completed(outcome);
    CHECK(c.stop.kind == StopKind::Compaction && c.stop.raw == "compaction");
    CHECK(c.messages.size() == 1 && c.messages[0].parts.size() == 1 && c.messages[0].native && c.messages[0].native->complete());
    const auto* kept = std::get_if<Opaque>(&c.messages[0].parts[0]);
    CHECK(kept && kept->wire_type == "compaction" && kept->wire_metadata->root().get("content").as_string() == "Summary: a recipe app." &&
          kept->wire_metadata->root().get("signature").as_string() == "c2lnbmVk");
    CHECK(c.usage.extra.at("compaction.input_tokens").value == 170 && c.usage.extra.at("compaction.output_tokens").value == 171);
    CHECK(c.usage.input_uncached && c.usage.input_uncached->value == 0 && c.usage.quality == UsageQuality::Consistent);
  };
  check_summary(buffered(body("[" + signed_block + "]", "\"compaction\"", summary_usage)));
  // On demand, the block arrives whole in content_block_start.
  check_summary(stream({start(R"({"input_tokens":0,"output_tokens":0})"), block(0, signed_block), ping, end(0), stop("\"compaction\"", summary_usage), done}));

  // At a threshold the block comes first and the answer follows. message_start counts the input before the summary;
  // the top-level counters after it count what remained, and replace it.
  const std::string threshold_usage = R"({"input_tokens":251,"cache_read_input_tokens":0,"cache_creation_input_tokens":0,"output_tokens":40,"iterations":[{"input_tokens":112197,"output_tokens":319,"type":"compaction"},{"input_tokens":251,"output_tokens":40,"type":"message"}]})";
  const auto threshold = stream({start(R"({"input_tokens":112197,"cache_read_input_tokens":0,"cache_creation_input_tokens":0,"output_tokens":6})"),
      block(0, R"({"type":"compaction","content":null})"), delta(0, R"({"type":"compaction_delta","content":"Summary: "})"),
      delta(0, R"({"type":"compaction_delta","content":"the code word."})"), end(0),
      block(1, R"({"type":"text","text":""})"), delta(1, R"({"type":"text_delta","text":"HELIOTROPE"})"), end(1),
      {"message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":)" + threshold_usage + R"(,"context_management":{"applied_edits":[]}})"}, done});
  const auto& answered = completed(threshold);
  CHECK(answered.stop.kind == StopKind::EndTurn && text(answered.messages) == "HELIOTROPE");
  const auto* summary = std::get_if<Opaque>(&answered.messages[0].parts[0]);
  CHECK(summary && summary->wire_metadata->root().get("content").as_string() == "Summary: the code word." &&
        summary->wire_metadata->root().get("type").as_string() == "compaction" && !summary->wire_metadata->root().get("signature").valid());
  CHECK(answered.usage.input_uncached->value == 251 && answered.usage.output_total->value == 40 && answered.usage.quality == UsageQuality::Consistent);
  CHECK(answered.usage.extra.at("compaction.input_tokens").value == 112197 && answered.usage.extra.at("compaction.output_tokens").value == 319);
  const auto whole = buffered(body(R"([{"type":"compaction","content":"Summary: the code word."},{"type":"text","text":"HELIOTROPE"}])", "\"end_turn\"",
                                   threshold_usage, R"(,"context_management":{"applied_edits":[]})"));
  CHECK(completed(whole).usage.input_uncached->value == 251 && completed(whole).usage.extra.at("compaction.input_tokens").value == 112197);
  // Paused after the summary: the stop is Compaction and the message holds the block alone.
  CHECK(completed(buffered(body(R"([{"type":"compaction","content":"Summary."}])", "\"compaction\"", summary_usage, R"(,"context_management":{"applied_edits":[]})"))).stop.kind == StopKind::Compaction);
  // A context edit the server applied still changes what was answered, streamed as well; a block of another type
  // takes no compaction delta.
  failed(stream({start(), block(0, R"({"type":"text","text":"x"})"), end(0),
                 {"message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":3},"context_management":{"applied_edits":[{"type":"clear_tool_uses_20250919"}]}})"}, done}),
         ErrorKind::Unsupported);
  failed(buffered(body("[]", "\"end_turn\"", initial_usage, R"(,"context_management":{"applied_edits":{}})")), ErrorKind::ProtocolCorrupt);
  failed(stream({start(), block(0, R"({"type":"fallback","from":{"model":"a"}})"), delta(0, R"({"type":"compaction_delta","content":"x"})")}), ErrorKind::Unsupported);
}
int main() {
  try {
    routed_gateway_model(); openrouter_sentinel_and_cost(); direct_dated_snapshot_alias(); served_model_is_recorded();
    owned_raw_observations(); raw_failure_evidence(); typeless_buffered_error();
    usage_binding_precedence();
    usage_detail_forward_compatibility(); toolset_metadata_shapes();
    terminal_matrix(); text_thinking_and_order(); tool_fragments(); server_semantics(); server_result_shapes();
    stop_classes(); corruption_matrix(); usage_knowledge(); cut_stream_usage_has_no_placeholder_output(); uncached_input_rule(); limits_and_close_priority(); empty_text_delta_is_withheld(); failure_reasons(); response_metadata_is_kept();
    advisor_result_and_iterations(); mcp_and_kept_blocks(); compaction_blocks_and_usage();
    std::cout << "messages semantic tests passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
