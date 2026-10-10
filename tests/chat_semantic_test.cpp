#include "codecs/chat.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include "transport/sse_framer.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
const descriptor::ValidatedDescriptor& descriptor_value() {
  static const auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"synthetic-chat","family":"openai.chat","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(loaded);
}
std::string envelope(std::string_view fields, bool streaming = true) {
  return "{\"id\":\"generation-1\",\"model\":\"fixture-model\",\"created\":7,\"object\":" +
    json::quote(streaming ? "chat.completion.chunk" : "chat.completion") + (fields.size() > 2 ? "," : "") + std::string(fields.substr(1));
}
std::string usage_frame(std::string_view usage) {
  return envelope("{\"choices\":[],\"usage\":" + std::string(usage) + "}");
}
std::string chunk(std::string_view delta, std::string_view finish = "null") {
  return envelope("{\"choices\":[{\"index\":0,\"delta\":" + std::string(delta) + ",\"finish_reason\":" + std::string(finish) + "}]}");
}
std::string body(std::string_view message, std::string_view finish = "\"stop\"", std::string_view usage = "null") {
  return envelope("{\"choices\":[{\"index\":0,\"message\":" + std::string(message) + ",\"finish_reason\":" + std::string(finish) + "}],\"usage\":" + std::string(usage) + "}", false);
}
Outcome buffered(std::string_view source, bool normal = true) {
  Accumulator accumulator;
  chat::Codec codec(descriptor_value(), chat::Mode::Buffered, accumulator);
  codec.buffered(source, {normal, ErrorKind::Truncated});
  CHECK(accumulator.outcome()); return *accumulator.outcome();
}
Outcome frames(const std::vector<std::string>& source, bool normal = true) {
  Accumulator accumulator;
  chat::Codec codec(descriptor_value(), chat::Mode::Sse, accumulator);
  for (const auto& frame : source) if (!codec.frame("message", frame)) break;
  codec.finish({normal, ErrorKind::Truncated});
  CHECK(accumulator.outcome()); return *accumulator.outcome();
}
const Completion& completed(const Outcome& outcome) { CHECK(std::holds_alternative<Completion>(outcome)); return std::get<Completion>(outcome); }
const Failure& failed(const Outcome& outcome, ErrorKind kind) { CHECK(std::holds_alternative<Failure>(outcome)); const auto& f = std::get<Failure>(outcome); CHECK(f.error.kind == kind); return f; }
std::string text_of(const std::vector<Message>& messages) {
  std::string result;
  for (const auto& message : messages) for (const auto& part : message.parts) if (const auto* text = std::get_if<Text>(&part)) result += text->value;
  return result;
}
std::string projection(const Outcome& outcome) {
  const auto* completion = std::get_if<Completion>(&outcome);
  const auto* failure = std::get_if<Failure>(&outcome);
  const auto& messages = completion ? completion->messages : failure->partial.messages;
  const auto& usage = completion ? completion->usage : failure->partial.usage;
  std::string result = completion ? "complete:" + std::to_string(static_cast<int>(completion->stop.kind)) + completion->stop.raw : "failure:" + std::to_string(static_cast<int>(failure->error.kind));
  for (const auto& message : messages) {
    result += "message:" + std::to_string(static_cast<int>(message.role)) + message.id;
    for (const auto& part : message.parts) {
      result += "part:" + std::to_string(part.index());
      if (const auto* t = std::get_if<Text>(&part)) result += t->value;
      else if (const auto* r = std::get_if<Refusal>(&part)) result += r->text + r->raw_code;
      else if (const auto* t = std::get_if<ToolCall>(&part)) result += t->id + t->name + t->input->root().dump();
      else if (const auto* i = std::get_if<InvalidToolCall>(&part)) result += i->id + i->name + i->raw_fragment + std::to_string(static_cast<int>(i->reason));
    }
  }
  for (const auto* count : {&usage.input_total, &usage.output_total, &usage.total, &usage.provider_reported_total, &usage.input_uncached, &usage.cache_read, &usage.cache_write, &usage.reasoning}) result += *count ? ":" + std::to_string((*count)->value) + "/" + std::to_string(static_cast<int>((*count)->evidence)) : ":unknown";
  result += ":stage" + std::to_string(static_cast<int>(usage.stage)) + ":quality" + std::to_string(static_cast<int>(usage.quality));
  return result;
}
const std::string full_usage = R"({"prompt_tokens":10,"completion_tokens":4,"total_tokens":14,"prompt_tokens_details":{"cached_tokens":4,"cache_write_tokens":0},"completion_tokens_details":{"reasoning_tokens":2}})";
void owned_raw_observations() {
  for (bool streaming : {false, true}) {
    std::vector<std::string> sources;
    auto source = streaming ? chunk(R"({"role":"assistant","content":"kept"})")
                            : body(R"({"role":"assistant","content":"kept"})");
    source.pop_back();
    source += R"(,"vendor":{"z":[3,{"b":true,"a":null}],"a":"retained"},"service_tier":"future"})";
    sources.push_back(source);
    if (streaming) sources.push_back(chunk("{}", "\"stop\""));
    std::vector<RawWire> observed;
    bool begun = false;
    const auto outcome = [&] {
      Accumulator accumulator({}, [&](const Event& event) {
        if (const auto* raw = std::get_if<RawWire>(&event)) {
          CHECK(raw->payload->root().dump() == sources.at(observed.size()));
          observed.push_back(*raw);
        } else if (std::holds_alternative<Begin>(event)) {
          CHECK(observed.at(0).payload->root().get("vendor").get("a").as_string() == "retained");
          begun = true;
        }
      });
      chat::Codec codec(descriptor_value(), streaming ? chat::Mode::Sse : chat::Mode::Buffered, accumulator);
      if (streaming) {
        for (const auto& wire : sources) CHECK(codec.frame("message", wire));
        CHECK(codec.frame("message", "[DONE]"));
        codec.finish();
      } else CHECK(codec.buffered(source, {}));
      CHECK(accumulator.outcome());
      return *accumulator.outcome();
    }();
    const auto& completion = completed(outcome);
    CHECK(begun && text_of(completion.messages) == "kept");
    CHECK(completion.raw_events.size() == sources.size());
    for (size_t i = 0; i < sources.size(); ++i) {
      CHECK(completion.raw_events[i].payload == observed[i].payload);
      CHECK(completion.raw_events[i].payload->root().dump() == sources[i]);
      CHECK(completion.raw_events[i].type == (streaming ? "chat.completion.chunk" : "chat.completion"));
    }
    const auto nested = completion.raw_events[0].payload->root().get("vendor").get("z").at(1);
    auto member = nested.members().begin();
    CHECK((*member++).key == "b" && (*member).key == "a");
  }
}
void raw_failure_evidence() {
  const auto initial = chunk(R"({"content":"partial"})");
  const std::string error = R"({"error":{"message":"private vendor error","extra":[2,1]},"vendor":{"opaque":true}})";
  const auto outcome = [&] {
    Accumulator accumulator;
    chat::Codec codec(descriptor_value(), chat::Mode::Sse, accumulator);
    CHECK(codec.frame("message", initial));
    CHECK(!codec.frame("error", error));
    CHECK(!codec.frame("message", chunk(R"({"content":"ignored"})")));
    codec.finish({false, ErrorKind::Cancelled});
    return *accumulator.outcome();
  }();
  const auto& failure = failed(outcome, ErrorKind::RemoteFailure);
  CHECK(text_of(failure.partial.messages) == "partial");
  CHECK(failure.partial.raw_events.size() == 2);
  CHECK(failure.partial.raw_events[0].payload->root().dump() == initial);
  CHECK(failure.partial.raw_events[1].type == "error");
  CHECK(failure.partial.raw_events[1].payload->root().dump() == error);
  CHECK(failure.error.safe_message.find("private") == std::string::npos);
  for (const auto& message : failure.partial.messages) CHECK(!message.native);
  for (const auto& malformed : {"{", "unstructured error", "[]", R"({"x":1,"x":2})"}) {
    Accumulator accumulator;
    chat::Codec codec(descriptor_value(), chat::Mode::Sse, accumulator);
    CHECK(!codec.frame("error", malformed));
    CHECK(failed(*accumulator.outcome(), ErrorKind::RemoteFailure).partial.raw_events.empty());
  }
  const auto truncated = frames({initial});
  const auto& partial = failed(truncated, ErrorKind::Truncated).partial;
  CHECK(partial.raw_events.size() == 1 && partial.raw_events[0].payload->root().dump() == initial);
  CHECK(text_of(partial.messages) == "partial");
  for (const auto& message : partial.messages) CHECK(!message.native);
  const auto buffered_wire = body(R"({"role":"assistant","content":"partial"})");
  const auto buffered_truncated = buffered(buffered_wire, false);
  CHECK(failed(buffered_truncated, ErrorKind::Truncated).partial.raw_events[0].payload->root().dump() == buffered_wire);
  const auto buffered_error = buffered(error);
  CHECK(failed(buffered_error, ErrorKind::RemoteFailure).partial.raw_events[0].payload->root().dump() == error);
}
void chunk_partition_invariant() {
  const std::string wire = "\xef\xbb\xbf: heartbeat\r\n\r\n"
    "data: {\"id\":\"generation-1\",\"model\":\"fixture-model\",\"created\":7,\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\r\n"
    "data: \"delta\":{\"role\":\"assistant\",\"content\":\"h\xc3\xa9\"},\"finish_reason\":null}]}\r\n\r\n"
    "data: " + chunk(R"({"content":"llo"})") + "\r\n\r\n"
    "data: " + chunk("{}", "\"stop\"") + "\r\n\r\n"
    "data: " + usage_frame(full_usage) + "\r\n\r\n"
    "data: [DONE]\r\n\r\n";
  auto run = [&](size_t split, bool bytes_at_a_time) {
    Accumulator accumulator;
    chat::Codec codec(descriptor_value(), chat::Mode::Sse, accumulator);
    transport::SseFramer framer;
    auto sink = [&](const transport::SseFrame& frame) { return codec.frame(frame.event, frame.data); };
    if (bytes_at_a_time) for (size_t i = 0; i < wire.size(); ++i) CHECK(framer.feed(std::string_view(wire).substr(i, 1), sink));
    else { CHECK(framer.feed(std::string_view(wire).substr(0, split), sink)); CHECK(framer.feed(std::string_view(wire).substr(split), sink)); }
    CHECK(!accumulator.outcome());
    framer.finish(); CHECK(framer.error() == transport::SseError::None); codec.finish();
    CHECK(accumulator.outcome()); return *accumulator.outcome();
  };
  const auto expected = buffered(body(R"({"role":"assistant","content":"héllo"})", "\"stop\"", full_usage));
  CHECK(text_of(completed(expected).messages) == "héllo");
  CHECK(completed(expected).usage.input_uncached->value == 6);
  CHECK(completed(expected).usage.cache_write->value == 0);
  for (size_t split = 0; split <= wire.size(); ++split) CHECK(projection(run(split, false)) == projection(expected));
  CHECK(projection(run(0, true)) == projection(expected));
  // Undelimited DONE must not be delivered by the framer or rescued by normal EOF.
  Accumulator accumulator; chat::Codec codec(descriptor_value(), chat::Mode::Sse, accumulator); transport::SseFramer framer;
  CHECK(framer.feed(std::string_view(wire).substr(0, wire.size() - 2), [&](const auto& frame) { return codec.frame(frame.event, frame.data); }));
  framer.finish(); codec.finish(); failed(*accumulator.outcome(), ErrorKind::Truncated);
}
void no_terminal_no_success() {
  const std::vector<std::string> sequence{chunk(R"({"content":"partial"})"), chunk("{}", "\"stop\""), "[DONE]"};
  for (size_t n = 0; n <= sequence.size(); ++n) for (bool normal : {false, true}) {
    std::vector<std::string> prefix(sequence.begin(), sequence.begin() + static_cast<std::ptrdiff_t>(n));
    auto outcome = frames(prefix, normal);
    if (n == sequence.size() && normal) CHECK(text_of(completed(outcome).messages) == "partial");
    else { const auto& failure = failed(outcome, ErrorKind::Truncated); CHECK(text_of(failure.partial.messages) == (n ? "partial" : "")); }
  }
  failed(frames({"[DONE]"}), ErrorKind::ProtocolCorrupt);
  failed(frames({sequence[0], sequence[1], sequence[0]}), ErrorKind::ProtocolCorrupt);
  failed(frames({sequence[0], sequence[1], "[DONE]", "[DONE]"}), ErrorKind::ProtocolCorrupt);
  const auto after_done = frames({sequence[0], sequence[1], "[DONE]", R"({"error":{"message":"private remote text"}})"});
  CHECK(text_of(failed(after_done, ErrorKind::RemoteFailure).partial.messages) == "partial");
  CHECK(std::get<Failure>(after_done).error.safe_message.find("private") == std::string::npos);
  auto missing = buffered(body(R"({"role":"assistant","content":"partial"})", "null"));
  CHECK(text_of(failed(missing, ErrorKind::Truncated).partial.messages) == "partial");
  size_t notifications = 0, events = 0;
  Accumulator accumulator({}, [&](const Event& event) { ++events; if (std::holds_alternative<Commit>(event) || std::holds_alternative<Fail>(event)) ++notifications; });
  chat::Codec codec(descriptor_value(), chat::Mode::Sse, accumulator);
  for (const auto& frame : sequence) CHECK(codec.frame("message", frame));
  CHECK(notifications == 0); codec.finish(); CHECK(notifications == 1);
  const auto before = events;
  codec.finish({false, ErrorKind::Cancelled}); CHECK(!codec.frame("error", "{}"));
  CHECK(!accumulator.accept(Fail{{ErrorKind::Cancelled, "cancel"}}));
  CHECK(events == before && notifications == 1);
}
void transport_projection_parity() {
  const std::string message = R"({"role":"assistant","content":"kept","refusal":"cannot","tool_calls":[{"id":"a","type":"function","function":{"name":"first","arguments":"{\"x\":1}"}},{"id":"b","type":"function","function":{"name":"second","arguments":"{}"}}]})";
  auto b = buffered(body(message, "\"stop\"", full_usage));
  auto s = frames({
    chunk(R"({"role":"assistant","tool_calls":[{"index":1,"id":"b","type":"function","function":{"name":"second","arguments":"{"}}]})"),
    chunk(R"({"content":"ke","tool_calls":[{"index":0,"id":"a","type":"function","function":{"name":"first","arguments":"{\"x\":"}}]})"),
    chunk(R"({"content":"pt","refusal":"can","tool_calls":[{"index":1,"function":{"arguments":"}"}}]})"),
    chunk(R"({"refusal":"not","tool_calls":[{"index":0,"function":{"arguments":"1}"}}]})"),
    chunk("{}", "\"stop\""), usage_frame(full_usage), "[DONE]"});
  const auto& c = completed(b); CHECK(c.stop.kind == StopKind::ToolUse && c.stop.raw == "stop");
  CHECK(c.messages[0].parts.size() == 4);
  CHECK(std::get<Refusal>(c.messages[0].parts[1]).text == "cannot");
  CHECK(std::get<ToolCall>(c.messages[0].parts[2]).id == "a");
  CHECK(std::get<ToolCall>(c.messages[0].parts[2]).input->root().get("x").as_uint() == 1);
  CHECK(std::get<ToolCall>(c.messages[0].parts[3]).id == "b");
  CHECK(projection(b) == projection(s));
}
void known_corrupt_never_ignored() {
  const std::vector<std::string> corrupt{
    R"({})", R"({"choices":null})", R"({"choices":[null]})",
    R"({"choices":[{"delta":{},"finish_reason":null}]})",
    R"({"choices":[{"index":0,"delta":{},"finish_reason":7}]})",
    R"({"choices":[{"index":0,"delta":{}}]})",
    R"({"choices":[{"index":0,"delta":null,"finish_reason":null}]})",
    chunk(R"({"role":null})"), chunk(R"({"role":"user"})"), chunk(R"({"content":9})"), chunk(R"({"refusal":false})"),
    chunk(R"({"tool_calls":{}})"), chunk(R"({"tool_calls":[{"index":0,"id":"a","type":"function","function":{"name":"f","arguments":{}}}]})"),
    chunk(R"({"tool_calls":[{"index":0,"id":"a","type":"function","function":{"arguments":"{}"}}]})"),
    R"({"choices":[],"usage":{"prompt_tokens":-1,"completion_tokens":0,"total_tokens":0}})", R"({"choices":[],"usage":{"prompt_tokens":1.5,"completion_tokens":0,"total_tokens":0}})",
    R"({"choices":[],"usage":{"prompt_tokens":null,"completion_tokens":0,"total_tokens":0}})", R"({"choices":[],"usage":{"prompt_tokens":0,"completion_tokens":0,"total_tokens":0,"completion_tokens_details":{"reasoning_tokens":"4"}}})",
    R"({"choices":[],"usage":{"prompt_tokens":18446744073709551615,"completion_tokens":1,"total_tokens":0}})",
    R"({"choices":[{"index":0,"index":0,"delta":{},"finish_reason":null}]})"
  };
  for (const auto& invalid : corrupt) {
    const auto wire = invalid.find("\"object\"") == std::string::npos ? envelope(invalid) : invalid;
    failed(frames({chunk(R"({"content":"kept"})"), wire}), ErrorKind::ProtocolCorrupt);
  }
  Accumulator accumulator; chat::Codec codec(descriptor_value(), chat::Mode::Sse, accumulator);
  CHECK(codec.frame("message", envelope(R"({"obfuscation":"x","service_tier":"default","system_fingerprint":42,"choices":[{"index":0,"native_finish_reason":"x","delta":{"content":"ok","new_property":true},"finish_reason":"stop"}]})")));
  CHECK(codec.diagnostics().unknown_properties == 5);
  CHECK(codec.frame("message", "[DONE]")); codec.finish(); CHECK(text_of(completed(*accumulator.outcome()).messages) == "ok");
  Accumulator tagged; chat::Codec unknown(descriptor_value(), chat::Mode::Sse, tagged);
  CHECK(!unknown.frame("new.event", chunk("{}", "\"stop\""))); unknown.finish(); failed(*tagged.outcome(), ErrorKind::Unsupported);
  for (auto error : {R"({"error":null})", R"({"error":false,"choices":[]})"}) failed(frames({error}), ErrorKind::RemoteFailure);
  failed(frames({chunk(R"({"audio":17})")}), ErrorKind::ProtocolCorrupt);
}
void invalid_tools_and_compatibility() {
  const std::string arguments = R"({"text":"a\\b","values":[1,2]})";
  auto expected_arguments = json::parse(arguments);
  for (size_t split = 0; split <= arguments.size(); ++split) {
    const auto first = "{\"tool_calls\":[{\"index\":0,\"id\":\"a\",\"type\":\"function\",\"function\":{\"name\":\"f\",\"arguments\":" + json::quote(std::string_view(arguments).substr(0, split)) + "}}]}";
    const auto second = "{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":" + json::quote(std::string_view(arguments).substr(split)) + "}}]}";
    auto outcome = frames({chunk(first), chunk(second), chunk("{}", "\"tool_calls\""), "[DONE]"});
    const auto& call = std::get<ToolCall>(completed(outcome).messages[0].parts[0]);
    CHECK(call.id == "a" && call.name == "f");
    CHECK(json::equal(call.input->root(), std::get<json::Document>(expected_arguments).root()));
  }
  struct Case { std::string arguments, reason; InvalidReason expected; };
  const std::vector<Case> cases{{"", "stop", InvalidReason::Empty}, {"{\"x\":1,\"x\":2}", "stop", InvalidReason::DuplicateKey}, {"{bad}", "stop", InvalidReason::NotJson}, {"[]", "stop", InvalidReason::NotJson}, {"{\"x\":", "length", InvalidReason::Truncated}};
  for (const auto& test : cases) {
    const auto call = "{\"id\":\"call\",\"type\":\"function\",\"function\":{\"name\":\"f\",\"arguments\":" + json::quote(test.arguments) + "}}";
    const auto message = "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[" + call + "]}";
    auto b = buffered(body(message, json::quote(test.reason)));
    auto s = frames({chunk("{\"tool_calls\":[" + call + "]}"), chunk("{}", json::quote(test.reason)), "[DONE]"});
    const auto& invalid = std::get<InvalidToolCall>(completed(b).messages[0].parts[0]);
    CHECK(invalid.reason == test.expected && invalid.raw_fragment == test.arguments && invalid.id == "call");
    CHECK(projection(b) == projection(s));
  }
  const auto initial = chunk(R"({"tool_calls":[{"index":0,"id":"a","type":"function","function":{"name":"f","arguments":"{"}}]})");
  auto single = frames({initial, chunk(R"({"tool_calls":[{"function":{"arguments":"}"}}]})"), chunk("{}", "\"stop\""), "[DONE]"});
  CHECK(std::get<ToolCall>(completed(single).messages[0].parts[0]).id == "a");
  auto changed = frames({initial, chunk(R"({"tool_calls":[{"index":0,"function":{"arguments":"}"}},{"index":0,"id":"b","function":{"name":"g","arguments":"{}"}}]})"), chunk("{}", "\"tool_calls\""), "[DONE]"});
  CHECK(completed(changed).messages[0].parts.size() == 2);
  CHECK(std::get<ToolCall>(completed(changed).messages[0].parts[1]).id == "b");
  failed(frames({initial, chunk(R"({"tool_calls":[{"index":1,"id":"b","function":{"name":"g","arguments":"{}"}}]})"), chunk(R"({"tool_calls":[{"function":{"arguments":"}"}}]})")}), ErrorKind::ProtocolCorrupt);
  failed(frames({initial, chunk(R"({"tool_calls":[{"index":0,"function":{"name":"different","arguments":"}"}}]})")}), ErrorKind::ProtocolCorrupt);
  auto raw = buffered(body(R"({"role":"assistant","content":"x"})", "\"future_reason\""));
  CHECK(completed(raw).stop.kind == StopKind::Unknown && completed(raw).stop.raw == "future_reason");
  failed(buffered(body(R"({"role":"assistant","content":"x"})", "\"error\"")), ErrorKind::RemoteFailure);
}
void usage_knowledge() {
  auto absent = buffered(body(R"({"role":"assistant","content":""})"));
  CHECK(completed(absent).usage.stage == UsageStage::Missing && !completed(absent).usage.total);
  CHECK(completed(absent).messages[0].parts.size() == 1 && std::get<Text>(completed(absent).messages[0].parts[0]).value.empty());
  auto zero = buffered(body(R"({"role":"assistant","content":null})", "\"stop\"", R"({"prompt_tokens":0,"completion_tokens":0,"total_tokens":0})"));
  CHECK(completed(zero).usage.total->value == 0 && completed(zero).usage.stage == UsageStage::Final);
  auto inconsistent = buffered(body(R"({"role":"assistant","content":"x"})", "\"stop\"", R"({"prompt_tokens":2,"completion_tokens":1,"total_tokens":3,"prompt_tokens_details":{"cached_tokens":4},"completion_tokens_details":{"reasoning_tokens":2}})"));
  const auto& u = completed(inconsistent).usage;
  CHECK(u.quality == UsageQuality::Inconsistent && !u.input_uncached && u.conflicts.size() == 2);
  CHECK(u.cache_read->value == 4 && u.reasoning->value == 2 && u.input_total->value == 2);
  auto repeated = frames({chunk(R"({"content":"x"})", "\"stop\""), usage_frame(full_usage), usage_frame(full_usage), "[DONE]"});
  CHECK(completed(repeated).usage.total->value == 14);
  auto replacement = frames({chunk(R"({"content":"x"})", "\"stop\""), usage_frame(full_usage), usage_frame(R"({"prompt_tokens":0,"completion_tokens":0,"total_tokens":0})"), "[DONE]"});
  CHECK(completed(replacement).usage.total->value == 0 && !completed(replacement).usage.cache_read);
  auto interrupted = frames({chunk(R"({"content":"x"})"), usage_frame(full_usage)}, false);
  CHECK(failed(interrupted, ErrorKind::Truncated).partial.usage.stage == UsageStage::Partial);
}
void required_metadata_and_identity() {
  const std::vector<std::string> names{"id", "model", "object", "created"};
  for (bool streaming : {false, true}) {
    const std::vector<std::string> values{"\"generation-1\"", "\"fixture-model\"", streaming ? "\"chat.completion.chunk\"" : "\"chat.completion\"", "7"};
    auto response = [&](size_t field, std::string_view replacement) {
      std::string wire = "{";
      for (size_t i = 0; i < names.size(); ++i) {
        if (i == field && replacement.empty()) continue;
        wire += json::quote(names[i]) + ":" + (i == field ? std::string(replacement) : values[i]) + ",";
      }
      wire += streaming ? R"("choices":[{"index":0,"delta":{"content":"kept"},"finish_reason":"stop"}]})" :
        R"("choices":[{"index":0,"message":{"role":"assistant","content":"kept"},"finish_reason":"stop"}]})";
      return wire;
    };
    for (size_t i = 0; i < names.size(); ++i) {
      for (auto invalid : {"", "null", "false", "{}", "[]"}) {
        auto wire = response(i, invalid);
        failed(streaming ? frames({wire, "[DONE]"}) : buffered(wire), ErrorKind::ProtocolCorrupt);
      }
      const auto wrong_scalar = response(i, i == 3 ? "\"7\"" : "7");
      failed(streaming ? frames({wrong_scalar, "[DONE]"}) : buffered(wrong_scalar), ErrorKind::ProtocolCorrupt);
    }
    for (auto invalid : {"-1", "1.5", "18446744073709551616"}) {
      const auto wire = response(3, invalid);
      failed(streaming ? frames({wire, "[DONE]"}) : buffered(wire), ErrorKind::ProtocolCorrupt);
    }
    if (streaming) {
      for (size_t i : {0U, 1U, 3U}) {
        auto outcome = frames({chunk(R"({"content":"prefix"})"), response(i, i == 3 ? "8" : "\"changed\"")});
        CHECK(text_of(failed(outcome, ErrorKind::ProtocolCorrupt).partial.messages) == "prefix");
      }
    }
  }
  std::string generation;
  size_t begins = 0;
  Accumulator accumulator({}, [&](const Event& event) {
    if (const auto* begin = std::get_if<Begin>(&event)) { generation = begin->generation; ++begins; }
  });
  chat::Codec codec(descriptor_value(), chat::Mode::Sse, accumulator);
  CHECK(codec.frame("message", chunk(R"({"content":"hello"})")));
  CHECK(codec.frame("message", chunk("{}", "\"stop\"")));
  CHECK(codec.frame("message", "[DONE]")); codec.finish();
  CHECK(generation == "generation-1" && begins == 1);
  CHECK(text_of(completed(*accumulator.outcome()).messages) == "hello");
}
void required_usage_and_unknown_cache() {
  const std::vector<std::string> names{"prompt_tokens", "completion_tokens", "total_tokens"};
  for (size_t field = 0; field < names.size(); ++field) for (auto invalid : {"", "null", "true", "\"0\"", "-1", "0.5", "18446744073709551616"}) {
    std::string usage = "{";
    for (size_t i = 0; i < names.size(); ++i) {
      if (i == field && std::string_view(invalid).empty()) continue;
      if (usage.size() > 1) usage += ",";
      usage += json::quote(names[i]) + ":" + (i == field ? invalid : "0");
    }
    usage += "}";
    failed(buffered(body(R"({"role":"assistant","content":"kept"})", "\"stop\"", usage)), ErrorKind::ProtocolCorrupt);
    failed(frames({chunk("{}", "\"stop\""), usage_frame(usage), "[DONE]"}), ErrorKind::ProtocolCorrupt);
  }
  for (auto details : {R"({})", R"({"cached_tokens":4})", R"({"cache_write_tokens":3})", R"({"cached_tokens":0})", R"({"cache_write_tokens":0})"}) {
    const auto usage = std::string(R"({"prompt_tokens":10,"completion_tokens":4,"total_tokens":14,"prompt_tokens_details":)") + details + "}";
    const auto outcome = buffered(body(R"({"role":"assistant","content":null})", "\"stop\"", usage));
    CHECK(!completed(outcome).usage.input_uncached);
    CHECK(completed(outcome).usage.quality == UsageQuality::Consistent);
  }
  for (auto details : {R"({"cached_tokens":11})", R"({"cache_write_tokens":11})", R"({"cached_tokens":6,"cache_write_tokens":5})"}) {
    const auto usage = std::string(R"({"prompt_tokens":10,"completion_tokens":4,"total_tokens":14,"prompt_tokens_details":)") + details + "}";
    const auto outcome = buffered(body(R"({"role":"assistant","content":null})", "\"stop\"", usage));
    CHECK(!completed(outcome).usage.input_uncached && completed(outcome).usage.quality == UsageQuality::Inconsistent);
    CHECK(completed(outcome).usage.conflicts.size() == 1);
  }
  const auto all_zero = buffered(body(R"({"role":"assistant","content":null})", "\"stop\"", R"({"prompt_tokens":0,"completion_tokens":0,"total_tokens":0,"prompt_tokens_details":{"cached_tokens":0,"cache_write_tokens":0}})"));
  CHECK(completed(all_zero).usage.input_uncached->value == 0);
  const auto exact = buffered(body(R"({"role":"assistant","content":null})", "\"stop\"", R"({"prompt_tokens":9007199254740993,"completion_tokens":0,"total_tokens":9007199254740993})"));
  CHECK(completed(exact).usage.total->value == 9007199254740993ULL);
}
void unindexed_tool_order_and_optional_function() {
  const auto first = chunk(R"({"tool_calls":[{"id":"a","type":"function","function":{"name":"first","arguments":"{}"}},{"id":"b","type":"function","function":{"name":"second","arguments":"{"}}]})");
  const auto next = chunk(R"({"tool_calls":[{"id":"c","type":"function","function":{"name":"third","arguments":"{}"}},{"id":"b","function":{"arguments":"}"}}]})");
  const auto outcome = frames({first, next, chunk(R"({"tool_calls":[{"id":"b"}]})"), chunk("{}", "\"tool_calls\""), "[DONE]"});
  const auto& parts = completed(outcome).messages[0].parts;
  CHECK(parts.size() == 3);
  CHECK(std::get<ToolCall>(parts[0]).id == "a");
  CHECK(std::get<ToolCall>(parts[1]).id == "b");
  CHECK(std::get<ToolCall>(parts[2]).id == "c");
  const auto indexed = chunk(R"({"tool_calls":[{"index":1,"id":"b","function":{"name":"second","arguments":"{}"}}]})");
  const auto no_function = frames({indexed, chunk(R"({"tool_calls":[{"index":1}]})"), chunk("{}", "\"tool_calls\""), "[DONE]"});
  CHECK(std::get<ToolCall>(completed(no_function).messages[0].parts[0]).id == "b");
  failed(frames({first, indexed}), ErrorKind::ProtocolCorrupt);
  failed(frames({indexed, first}), ErrorKind::ProtocolCorrupt);
  failed(frames({first, chunk(R"({"tool_calls":[{}]})")}), ErrorKind::ProtocolCorrupt);
  failed(frames({chunk(R"({"tool_calls":[{"id":"new"}]})")}), ErrorKind::ProtocolCorrupt);
  for (auto value : {"null", "false", "\"function\"", "[]"}) {
    failed(frames({indexed, chunk("{\"tool_calls\":[{\"index\":1,\"function\":" + std::string(value) + "}]}")}), ErrorKind::ProtocolCorrupt);
  }
}
void buffered_close_precedence() {
  const auto valid = body(R"({"role":"assistant","content":"retained"})");
  const auto semantic_error = body(R"({"role":"assistant","content":"retained","tool_calls":false})");
  for (auto error : {ErrorKind::Cancelled, ErrorKind::DeadlineExceeded, ErrorKind::Truncated}) {
    for (const auto& wire : {std::string("{"), valid, semantic_error}) {
      size_t terminals = 0;
      Accumulator accumulator({}, [&](const Event& event) {
        if (std::holds_alternative<Fail>(event) || std::holds_alternative<Commit>(event)) ++terminals;
      });
      chat::Codec codec(descriptor_value(), chat::Mode::Buffered, accumulator);
      CHECK(!codec.buffered(wire, {false, error}));
      const auto& failure = failed(*accumulator.outcome(), error);
      CHECK(text_of(failure.partial.messages) == (wire == "{" ? "" : "retained"));
      codec.finish(); CHECK(terminals == 1);
    }
    SemanticLimits limits; limits.max_tool_bytes = 1;
    Accumulator bounded(limits);
    chat::Codec codec(descriptor_value(), chat::Mode::Buffered, bounded, limits);
    CHECK(!codec.buffered(body(R"({"role":"assistant","content":"retained","tool_calls":[{"id":"a","type":"function","function":{"name":"f","arguments":"{}"}}]})"), {false, error}));
    CHECK(text_of(failed(*bounded.outcome(), error).partial.messages) == "retained");
  }
  failed(buffered("{"), ErrorKind::ProtocolCorrupt);
  failed(buffered(semantic_error), ErrorKind::ProtocolCorrupt);
}
void accumulator_transitions_and_seals() {
  auto start = [](Accumulator& a) { CHECK(a.accept(Begin{})); CHECK(a.accept(MessageBegin{{0}, {}, Role::Assistant})); CHECK(a.accept(PartBegin{{0}, {1}, PartKind::Text, {}, 0})); };
  Accumulator prefix; start(prefix); CHECK(prefix.accept(PartDelta{{1}, {PartKind::Text, "hel"}}));
  CHECK(prefix.accept(PartSeal{{1}, "hello"})); CHECK(prefix.accept(PartSeal{{1}, "hello"}));
  CHECK(prefix.accept(Stop{{StopKind::EndTurn, "stop"}})); CHECK(prefix.accept(MessageSeal{{0}})); CHECK(prefix.accept(Commit{"evidence"}));
  CHECK(text_of(completed(*prefix.outcome()).messages) == "hello");
  Accumulator mismatch; start(mismatch); CHECK(mismatch.accept(PartDelta{{1}, {PartKind::Text, "old"}}));
  CHECK(!mismatch.accept(PartSeal{{1}, "new"})); CHECK(text_of(failed(*mismatch.outcome(), ErrorKind::ProtocolCorrupt).partial.messages) == "old");
  Accumulator after_seal; start(after_seal); CHECK(after_seal.accept(PartSeal{{1}, "sealed"}));
  CHECK(!after_seal.accept(PartDelta{{1}, {PartKind::Text, "extra"}})); CHECK(text_of(failed(*after_seal.outcome(), ErrorKind::ProtocolCorrupt).partial.messages) == "sealed");
  Accumulator after_stop; start(after_stop); CHECK(after_stop.accept(Stop{{StopKind::MaxTokens, "length"}}));
  CHECK(!after_stop.accept(PartDelta{{1}, {PartKind::Text, "late"}})); failed(*after_stop.outcome(), ErrorKind::ProtocolCorrupt);
  Accumulator unbegun; CHECK(!unbegun.accept(PartDelta{{1}, {PartKind::Text, "stray"}})); failed(*unbegun.outcome(), ErrorKind::ProtocolCorrupt);
  Accumulator open; start(open); CHECK(!open.accept(MessageSeal{{0}})); failed(*open.outcome(), ErrorKind::ProtocolCorrupt);
  Accumulator no_stop; start(no_stop); CHECK(!no_stop.accept(Commit{"not sufficient"})); failed(*no_stop.outcome(), ErrorKind::ProtocolCorrupt);
  Accumulator duplicate; start(duplicate); CHECK(!duplicate.accept(PartBegin{{0}, {1}, PartKind::Refusal, {}, 0})); failed(*duplicate.outcome(), ErrorKind::ProtocolCorrupt);
  SemanticLimits limits; limits.max_content_bytes = 3;
  Accumulator bounded(limits); start(bounded); CHECK(bounded.accept(PartDelta{{1}, {PartKind::Text, "abc"}}));
  CHECK(!bounded.accept(PartDelta{{1}, {PartKind::Text, "d"}})); CHECK(text_of(failed(*bounded.outcome(), ErrorKind::ResourceLimit).partial.messages) == "abc");
  SemanticLimits tool_limits; tool_limits.max_tool_bytes = 2;
  Accumulator argument_bound(tool_limits); CHECK(argument_bound.accept(Begin{})); CHECK(argument_bound.accept(MessageBegin{{0}, {}, Role::Assistant}));
  CHECK(argument_bound.accept(PartBegin{{0}, {1}, PartKind::ToolCall, {"call", "f"}, 0}));
  CHECK(argument_bound.accept(PartDelta{{1}, {PartKind::ToolCall, "{}"}}));
  CHECK(!argument_bound.accept(PartDelta{{1}, {PartKind::ToolCall, " "}}));
  const auto& retained = std::get<InvalidToolCall>(failed(*argument_bound.outcome(), ErrorKind::ResourceLimit).partial.messages[0].parts[0]);
  CHECK(retained.raw_fragment == "{}" && retained.reason == InvalidReason::Truncated);
  Accumulator knowledge; start(knowledge); Usage first; first.input_total = Count{7}; first.stage = UsageStage::Partial;
  CHECK(knowledge.accept(UsageUpdate{first})); Usage second; second.input_total = Count{0}; second.stage = UsageStage::Partial;
  CHECK(knowledge.accept(UsageUpdate{second})); CHECK(knowledge.accept(UsageUpdate{Usage{}}));
  CHECK(knowledge.accept(Fail{{ErrorKind::Cancelled, "cancel"}})); CHECK(!failed(*knowledge.outcome(), ErrorKind::Cancelled).partial.usage.input_total);
}
void accumulator_interleaved_ordering() {
  constexpr uint32_t message_count = 16, parts_per_message = 128;
  auto message_id = [](uint32_t message) { return LocalId{7 * (message + 1)}; };
  auto part_id = [](uint32_t message, uint32_t order) {
    return LocalId{(parts_per_message - 1 - order) * message_count + message_count - 1 - message};
  };
  auto text = [](uint32_t message, uint32_t order) { return std::to_string(message) + ":" + std::to_string(order); };
  for (bool partial : {false, true}) {
    SemanticLimits limits; limits.max_parts = message_count * parts_per_message;
    Accumulator a(limits); CHECK(a.accept(Begin{}));
    for (uint32_t i = message_count; i-- > 0;)
      CHECK(a.accept(MessageBegin{message_id(i), std::to_string(i), Role::Assistant}));
    for (uint32_t order = parts_per_message; order-- > 0;) {
      for (uint32_t offset = 0; offset < message_count; ++offset) {
        const uint32_t message = (offset + order) % message_count;
        CHECK(a.accept(PartBegin{message_id(message), part_id(message, order), PartKind::Text, {}, order}));
        CHECK(a.accept(PartDelta{part_id(message, order), {PartKind::Text, text(message, order)}}));
      }
    }
    // Sealing and snapshots arrive in a different order from both IDs and begins.
    for (uint32_t message = 0; message < message_count; ++message) {
      for (uint32_t order = 0; order < parts_per_message; ++order) {
        if (partial && order % 2) continue;
        const auto snapshot = text(message, order) + "|sealed";
        CHECK(a.accept(PartSeal{part_id(message, order), snapshot}));
        CHECK(a.accept(PartSeal{part_id(message, order), snapshot}));
      }
    }
    if (partial) CHECK(a.accept(Fail{{ErrorKind::Cancelled, "cancel"}}));
    else {
      CHECK(a.accept(Stop{{StopKind::EndTurn, "stop"}}));
      for (uint32_t message = message_count; message-- > 0;) CHECK(a.accept(MessageSeal{message_id(message)}));
      CHECK(a.accept(Commit{"evidence"}));
    }
    const auto& messages = partial ? failed(*a.outcome(), ErrorKind::Cancelled).partial.messages : completed(*a.outcome()).messages;
    CHECK(messages.size() == message_count);
    for (uint32_t message = 0; message < message_count; ++message) {
      CHECK(messages[message].id == std::to_string(message));
      CHECK(messages[message].parts.size() == parts_per_message);
      for (uint32_t order = 0; order < parts_per_message; ++order)
        CHECK(std::get<Text>(messages[message].parts[order]).value ==
              text(message, order) + ((!partial || order % 2 == 0) ? "|sealed" : ""));
    }
  }
}
void accumulator_order_uniqueness_and_limits() {
  for (bool sealed : {false, true}) {
    Accumulator a; CHECK(a.accept(Begin{}));
    CHECK(a.accept(MessageBegin{{9}, "later", Role::Assistant}));
    CHECK(a.accept(MessageBegin{{2}, "earlier", Role::Assistant}));
    CHECK(a.accept(PartBegin{{9}, {10}, PartKind::Text, {}, std::numeric_limits<uint64_t>::max()}));
    CHECK(a.accept(PartDelta{{10}, {PartKind::Text, "later"}}));
    CHECK(a.accept(PartBegin{{2}, {20}, PartKind::Text, {}, std::numeric_limits<uint64_t>::max()}));
    CHECK(a.accept(PartSeal{{20}, "earlier"}));
    // Other messages may remain open when this message is sealed.
    CHECK(a.accept(MessageSeal{{2}}));
    if (sealed) CHECK(a.accept(PartSeal{{10}, "later"}));
    CHECK(!a.accept(PartBegin{{9}, {30}, PartKind::Refusal, {}, std::numeric_limits<uint64_t>::max()}));
    const auto& messages = failed(*a.outcome(), ErrorKind::ProtocolCorrupt).partial.messages;
    CHECK(messages.size() == 2 && messages[0].id == "earlier" && messages[1].id == "later");
    CHECK(messages[0].parts.size() == 1 && messages[1].parts.size() == 1);
    CHECK(std::get<Text>(messages[0].parts[0]).value == "earlier");
    CHECK(std::get<Text>(messages[1].parts[0]).value == "later");
  }
  SemanticLimits limits; limits.max_parts = 4;
  Accumulator a(limits); CHECK(a.accept(Begin{})); CHECK(a.accept(MessageBegin{{0}, {}, Role::Assistant}));
  for (uint32_t order = 4; order-- > 0;) {
    CHECK(a.accept(PartBegin{{0}, {order}, PartKind::Text, {}, order}));
    CHECK(a.accept(PartSeal{{order}, std::to_string(order)}));
  }
  CHECK(!a.accept(PartBegin{{0}, {4}, PartKind::Text, {}, 4}));
  const auto& parts = failed(*a.outcome(), ErrorKind::ResourceLimit).partial.messages[0].parts;
  CHECK(parts.size() == 4);
  for (uint32_t order = 0; order < 4; ++order) CHECK(std::get<Text>(parts[order]).value == std::to_string(order));
}
void typed_request_encoding() {
  chat::Request request; request.model = "fixture-model"; request.messages.push_back({Role::User, "Say hello."});
  auto encoded = chat::encode(descriptor_value(), request, true); CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
  const auto& wire = std::get<chat::EncodedRequest>(encoded);
  CHECK(wire.method == "POST" && wire.path == "/v1/chat/completions");
  auto actual = json::parse(wire.body); auto expected = json::parse(R"({"model":"fixture-model","messages":[{"role":"user","content":"Say hello."}],"stream":true,"stream_options":{"include_usage":true}})");
  CHECK(json::equal(std::get<json::Document>(actual).root(), std::get<json::Document>(expected).root()));
  auto schema = json::parse(R"({"type":"object","properties":{"x":{"type":"integer"}}})");
  request.tools.push_back({"f", "description", std::make_shared<const json::Document>(std::move(std::get<json::Document>(schema)))});
  request.temperature = 0; request.top_p = 1; request.max_output_tokens = 7;
  auto rich = chat::encode(descriptor_value(), request, false); CHECK(std::holds_alternative<chat::EncodedRequest>(rich));
  auto parsed = json::parse(std::get<chat::EncodedRequest>(rich).body); auto root = std::get<json::Document>(parsed).root();
  CHECK(root.get("temperature").as_double() == 0 && root.get("max_tokens").as_uint() == 7 && !root.get("stream_options").valid());
  CHECK(root.get("tools").at(0).get("function").get("parameters").get("properties").get("x").get("type").as_string() == "integer");
  auto aliased = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"alias","family":"openai.chat","connection":{"base_url":"https://example.test","paths":{"buffered":"/answer","streaming":"/stream"}},"bindings":{"model":"deployment","messages":"history","stream":"streaming","max_output_tokens":"max_completion_tokens","usage":["metrics","tokens"]},"stop_reasons":{"finished":"EndTurn"}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(aliased));
  const auto& alias = std::get<descriptor::ValidatedDescriptor>(aliased);
  auto encoded_alias = chat::encode(alias, request, false);
  CHECK(std::holds_alternative<chat::EncodedRequest>(encoded_alias));
  CHECK(std::get<chat::EncodedRequest>(encoded_alias).path == "/answer");
  auto alias_body = json::parse(std::get<chat::EncodedRequest>(encoded_alias).body);
  auto alias_root = std::get<json::Document>(alias_body).root();
  CHECK(alias_root.get("deployment").as_string() == request.model && !alias_root.get("model").valid());
  CHECK(alias_root.get("history").at(0).get("content").as_string() == "Say hello.");
  CHECK(alias_root.get("max_completion_tokens").as_uint() == 7 && !alias_root.get("max_tokens").valid());
  Accumulator alias_accumulator; chat::Codec alias_codec(alias, chat::Mode::Buffered, alias_accumulator);
  CHECK(alias_codec.buffered(envelope(R"({"choices":[{"index":0,"message":{"role":"assistant","content":"alias"},"finish_reason":"finished"}],"metrics":{"tokens":{"prompt_tokens":3,"completion_tokens":2,"total_tokens":5}}})", false), {}));
  CHECK(completed(*alias_accumulator.outcome()).stop.kind == StopKind::EndTurn);
  CHECK(completed(*alias_accumulator.outcome()).usage.total->value == 5);
  request.temperature = std::numeric_limits<double>::quiet_NaN(); CHECK(std::get<Error>(chat::encode(descriptor_value(), request, true)).kind == ErrorKind::InvalidRequest);
  request.temperature.reset(); request.messages[0].text = std::string(1, static_cast<char>(0xff));
  CHECK(std::get<Error>(chat::encode(descriptor_value(), request, true)).kind == ErrorKind::InvalidRequest);
}

void tool_history_and_escaped_arguments() {
  const auto outcome = buffered(body(R"({"role":"assistant","content":"checking","tool_calls":[{"id":"a","type":"function","function":{"name":"f","arguments":"{\"value\":\"quote \\\" slash \\\\ nul \\u0000 snow 雪\",\"nested\":[true,null,7]}"}},{"id":"b","type":"function","function":{"name":"g","arguments":"{}"}}]})", "\"tool_calls\""));
  chat::Request request; request.model = "fixture-model";
  request.messages.push_back({Role::User, "Use the two tools."});
  chat::InputMessage assistant{Role::Assistant, "checking"};
  for (const auto& part : completed(outcome).messages.front().parts)
    if (const auto* call = std::get_if<ToolCall>(&part)) assistant.tool_calls.push_back(*call);
  CHECK(assistant.tool_calls.size() == 2);
  request.messages.push_back(std::move(assistant));
  request.messages.push_back({Role::Tool, "second result", {}, "b"});
  request.messages.push_back({Role::Tool, "first result", {}, "a"});
  for (bool streaming : {false, true}) {
    auto encoded = chat::encode(descriptor_value(), request, streaming);
    CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
    auto document = json::parse(std::get<chat::EncodedRequest>(encoded).body);
    CHECK(std::holds_alternative<json::Document>(document));
    auto messages = std::get<json::Document>(document).root().get("messages");
    CHECK(messages.at(2).get("tool_call_id").as_string() == "b");
    CHECK(messages.at(3).get("tool_call_id").as_string() == "a");
    auto arguments = json::parse(messages.at(1).get("tool_calls").at(0).get("function").get("arguments").as_string());
    CHECK(std::holds_alternative<json::Document>(arguments));
    CHECK(json::equal(std::get<json::Document>(arguments).root(), request.messages[1].tool_calls[0].input->root()));
    const std::string expected = std::string("quote \" slash \\ nul ") + '\0' + " snow 雪";
    CHECK(std::get<json::Document>(arguments).root().get("value").as_string() == expected);
  }
  auto rejected = [&](chat::Request value) {
    auto result = chat::encode(descriptor_value(), value, false);
    CHECK(std::holds_alternative<Error>(result));
    CHECK(std::get<Error>(result).kind == ErrorKind::InvalidRequest);
  };
  auto changed = request; changed.messages[2].tool_call_id = "missing"; rejected(std::move(changed));
  changed = request; changed.messages[3].tool_call_id = "b"; rejected(std::move(changed));
  changed = request; changed.messages.pop_back(); rejected(std::move(changed));
  changed = request; changed.messages[0].tool_call_id = "a"; rejected(std::move(changed));
  changed = request; changed.messages[1].role = Role::User; rejected(std::move(changed));
  changed = request; changed.messages[1].tool_calls[1].id = "a"; rejected(std::move(changed));
  changed = request; changed.messages[2].role = Role::Assistant; rejected(std::move(changed));
  changed = request; changed.messages[1].tool_calls[0].input.reset(); rejected(std::move(changed));
  changed = request; changed.messages[1].tool_calls[0].id = std::string(1, static_cast<char>(0xff));
  changed.messages[3].tool_call_id = changed.messages[1].tool_calls[0].id; rejected(std::move(changed));
  changed = request; changed.messages[1].tool_calls[0].kind = ToolCallKind::ServerExecuted;
  CHECK(std::get<Error>(chat::encode(descriptor_value(), changed, false)).kind == ErrorKind::Unsupported);
}

void quoted_json_and_request_bounds() {
  const auto parsed = json::parse(R"({"quote\"key":"\u0000\b\f\n\r\t\"\\雪","array":[1,-2,1.25,true,null,{}]})");
  CHECK(std::holds_alternative<json::Document>(parsed));
  const auto value = std::get<json::Document>(parsed).root();
  const auto reference = json::quote(value.dump());
  for (const auto limit : {reference.size() - 1, reference.size(), reference.size() + 1}) {
    json::BoundedWriter measure({limit, 64}); measure.quoted_json(value);
    std::string encoded; encoded.reserve(limit);
    json::BoundedWriter output({limit, 64}, &encoded); output.quoted_json(value);
    CHECK(measure.ok() == (limit >= reference.size()) && output.ok() == measure.ok());
    if (output.ok()) CHECK(encoded == reference && measure.size() == encoded.size());
  }
  json::BoundedWriter shallow({1024, 1}); shallow.quoted_json(value); CHECK(!shallow.ok());
  chat::Request request; request.model = "fixture-model"; request.messages.push_back({Role::User, {}});
  const auto empty = chat::encode(descriptor_value(), request, false);
  CHECK(std::holds_alternative<chat::EncodedRequest>(empty));
  constexpr std::size_t limit = 1U << 20;
  request.messages[0].text.assign(limit - std::get<chat::EncodedRequest>(empty).body.size(), 'x');
  const auto exact = chat::encode(descriptor_value(), request, false);
  CHECK(std::holds_alternative<chat::EncodedRequest>(exact) && std::get<chat::EncodedRequest>(exact).body.size() == limit);
  request.messages[0].text.push_back('x');
  CHECK(std::get<Error>(chat::encode(descriptor_value(), request, false)).kind == ErrorKind::InvalidRequest);
  request.messages[0].text.assign(limit / 6, '\0');
  CHECK(std::get<Error>(chat::encode(descriptor_value(), request, false)).kind == ErrorKind::InvalidRequest);
}
void gateway_request_controls() {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"gateway","family":"openai.chat","connection":{"base_url":"https://openrouter.ai","paths":{"buffered":"/api/v1/chat/completions","streaming":"/api/v1/chat/completions"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& gateway = std::get<descriptor::ValidatedDescriptor>(loaded);
  chat::Request request; request.model = "fixture-model"; request.messages.push_back({Role::User, "hello"});
  request.reasoning = chat::ReasoningOptions{"low", {}, false, true};
  request.include_reasoning = true; request.usage_include = false;
  request.models = {"openai/gpt-4o-mini", "anthropic/claude-sonnet-4"};
  auto result = chat::encode(gateway, request, true);
  CHECK(std::holds_alternative<chat::EncodedRequest>(result));
  auto parsed = json::parse(std::get<chat::EncodedRequest>(result).body);
  CHECK(std::holds_alternative<json::Document>(parsed));
  const auto root = std::get<json::Document>(parsed).root();
  CHECK(root.get("reasoning").get("effort").as_string() == "low");
  CHECK(!root.get("reasoning").get("exclude").as_bool() && root.get("reasoning").get("enabled").as_bool());
  CHECK(root.get("include_reasoning").as_bool() && !root.get("usage").get("include").as_bool());
  CHECK(root.get("models").at(1).as_string() == "anthropic/claude-sonnet-4");
  CHECK(std::get<Error>(chat::encode(descriptor_value(), request, false)).kind == ErrorKind::InvalidRequest);
  auto reject = [&](const chat::Request& changed) {
    CHECK(std::get<Error>(chat::encode(gateway, changed, false)).kind == ErrorKind::InvalidRequest);
  };
  auto changed = request; changed.reasoning->effort = "arbitrary"; reject(changed);
  changed = request; changed.reasoning->max_tokens = 128; reject(changed);
  changed = request; changed.reasoning->enabled = false; reject(changed);
  changed = request; changed.reasoning->exclude = true; reject(changed);
  changed = request; changed.reasoning_effort = "low"; reject(changed);
  changed = request; changed.models.push_back(changed.models.front()); reject(changed);
  changed = request; changed.temperature = 0.5; changed.models = {"OpenAI/GPT-5-nano"}; reject(changed);
  request.reasoning->effort.reset(); request.reasoning->max_tokens = 256; request.max_output_tokens = 512;
  result = chat::encode(gateway, request, false);
  CHECK(std::holds_alternative<chat::EncodedRequest>(result));
  parsed = json::parse(std::get<chat::EncodedRequest>(result).body);
  CHECK(std::get<json::Document>(parsed).root().get("reasoning").get("max_tokens").as_uint() == 256);
  changed = request; changed.reasoning->max_tokens = 0; reject(changed);
  changed = request; changed.reasoning->max_tokens = 513; reject(changed);
  changed = request; changed.reasoning->max_tokens = std::numeric_limits<uint64_t>::max(); reject(changed);
  Accumulator accumulator;
  chat::Codec codec(gateway, chat::Mode::Buffered, accumulator, {}, std::get<chat::EncodedRequest>(result).context);
  codec.buffered(body(R"({"role":"assistant","content":"answer"})"), {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  request.messages.clear(); request.canonical_messages = {Message{{}, Role::User, {Text{"hello"}}}};
  request.canonical_messages.push_back(std::get<Completion>(*accumulator.outcome()).messages.at(0));
  request.canonical_messages.push_back(Message{{}, Role::User, {Text{"next"}}});
  CHECK(std::holds_alternative<chat::EncodedRequest>(chat::encode(gateway, request, false)));
  for (int field = 0; field != 7; ++field) {
    changed = request;
    if (field == 0) changed.reasoning->max_tokens = 255;
    if (field == 1) changed.include_reasoning = false;
    if (field == 2) changed.usage_include = true;
    if (field == 3) changed.models.pop_back();
    if (field == 4) changed.reasoning->exclude.reset();
    if (field == 5) changed.reasoning->enabled.reset();
    if (field == 6) { changed.reasoning->max_tokens.reset(); changed.reasoning->effort = "medium"; }
    CHECK(std::get<Error>(chat::encode(gateway, changed, false)).kind == ErrorKind::ReplayIneligible);
  }
}
void idempotent_terminal_money_tail() {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"tail-money","family":"openai.chat","connection":{"base_url":"https://openrouter.ai","paths":{"buffered":"/api/v1/chat/completions","streaming":"/api/v1/chat/completions"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& gateway = std::get<descriptor::ValidatedDescriptor>(loaded);
  const std::string money = R"({"prompt_tokens":16,"completion_tokens":6,"total_tokens":22,"cost":0.000004,"is_byok":false,"cost_details":{"upstream_inference_cost":4e-6,"upstream_inference_prompt_cost":1.6e-6,"upstream_inference_completions_cost":2.4e-6}})";
  const auto tail = [&](std::string_view delta, std::string_view finish = "\"stop\"", std::string_view counters = "") {
    auto wire = chunk(delta, finish); wire.pop_back();
    return wire + ",\"usage\":" + std::string(counters.empty() ? std::string_view(money) : counters) + "}";
  };
  const auto decode = [&](const std::vector<std::string>& wires) {
    Accumulator acc; chat::Codec codec(gateway, chat::Mode::Sse, acc);
    for (const auto& wire : wires) if (!codec.frame("message", wire)) break;
    codec.finish(); CHECK(acc.outcome()); return *acc.outcome();
  };
  for (const auto& empty : {"{}", R"({"content":""})", R"({"content":null})", R"({"content":"","role":"assistant"})", R"({"role":"assistant"})"}) {
    size_t stops = 0, commits = 0, deltas = 0;
    Accumulator acc({}, [&](const Event& event) {
      if (std::holds_alternative<Stop>(event)) ++stops;
      if (std::holds_alternative<Commit>(event)) ++commits;
      if (std::holds_alternative<PartDelta>(event)) ++deltas;
    });
    chat::Codec codec(gateway, chat::Mode::Sse, acc);
    CHECK(codec.frame("message", chunk(R"({"content":"hello"})")));
    CHECK(codec.frame("message", chunk("{}", "\"stop\"")));
    CHECK(codec.frame("message", tail(empty)));
    CHECK(codec.frame("message", tail(empty)));
    CHECK(codec.frame("message", "[DONE]")); codec.finish();
    const auto& c = completed(*acc.outcome());
    CHECK(stops == 1 && commits == 1 && deltas == 1 && text_of(c.messages) == "hello");
    CHECK(c.messages.size() == 1 && c.messages[0].parts.size() == 1 && c.usage.total->value == 22);
    CHECK(c.usage.provider_cost.total->nano_usd == 4000 && c.usage.provider_cost.upstream_input->nano_usd == 1600);
    CHECK(c.usage.provider_cost.upstream_output->nano_usd == 2400 && c.usage.provider_cost.is_byok && !*c.usage.provider_cost.is_byok);
    CHECK(c.usage.provider_cost.upstream_total->nano_usd == 4000);
    for (const auto status : c.usage.provider_cost.status) CHECK(status == CostStatus::Available);
    CHECK(c.usage.provider_cost.source == CostSource::OpenRouterUsd && c.usage.stage == UsageStage::Final && c.usage.extra.empty());
    CHECK(c.raw_events.size() == 4 && c.raw_events.back().payload->root().get("usage").get("cost").is_number());
  }
  const auto no_content = decode({chunk("{}", "\"stop\""), tail(R"({"content":""})"), "[DONE]"});
  CHECK(completed(no_content).messages[0].parts.empty());
  const auto standard = decode({chunk(R"({"content":"hello"})", "\"stop\""), usage_frame(money), "[DONE]"});
  CHECK(completed(standard).usage.provider_cost.total->nano_usd == 4000);
  const auto buffered_outcome = [&] {
    Accumulator acc; chat::Codec codec(gateway, chat::Mode::Buffered, acc);
    codec.buffered(body(R"({"role":"assistant","content":"hello"})", "\"stop\"", money), {});
    CHECK(acc.outcome()); return *acc.outcome();
  }();
  CHECK(completed(buffered_outcome).usage.provider_cost.total->nano_usd == 4000);
  CHECK(completed(buffered_outcome).usage.provider_cost.upstream_input->nano_usd == 1600);
  CHECK(completed(buffered_outcome).usage.provider_cost.upstream_output->nano_usd == 2400);
  for (const auto status : completed(buffered_outcome).usage.provider_cost.status) CHECK(status == CostStatus::Available);
  CHECK(text_of(completed(buffered_outcome).messages) == "hello");
  for (const auto& delta : {R"({"content":"late"})", R"({"tool_calls":[{"index":0,"function":{"arguments":"{}"}}]})",
      R"({"role":"user"})", R"({"role":null})", R"({"role":7})", R"({"reasoning":"late"})", R"({"content":7})", R"({"vendor":"late"})"}) {
    failed(decode({chunk(R"({"content":"hello"})", "\"stop\""), tail(delta), "[DONE]"}), ErrorKind::ProtocolCorrupt);
  }
  for (const auto& [input_alias, expected] : std::vector<std::pair<std::string, CostStatus>>{
      {"1e-10", CostStatus::Available}, {"2e-10", CostStatus::Conflict}, {"\"invalid\"", CostStatus::Malformed}}) {
    const std::string counters = R"({"prompt_tokens":1,"completion_tokens":1,"total_tokens":2,"cost":4e-6,"cost_details":{"upstream_inference_prompt_cost":1e-10,"upstream_inference_input_cost":)"
        + input_alias + R"(,"upstream_inference_completions_cost":2.4e-6,"upstream_inference_output_cost":2.4e-6}})";
    const auto result = decode({chunk(R"({"content":"hello"})", "\"stop\""), tail(R"({"content":"","role":"assistant"})", "\"stop\"", counters), "[DONE]"});
    const auto& cost = completed(result).usage.provider_cost;
    CHECK(cost.total->nano_usd == 4000 && cost.upstream_output->nano_usd == 2400 && cost.status[2] == expected);
    CHECK(completed(result).usage.extra.empty() && text_of(completed(result).messages) == "hello");
    if (expected == CostStatus::Available) CHECK(cost.upstream_input->nano_usd == 1 && cost.quality == UsageQuality::Consistent);
    else CHECK(!cost.upstream_input && cost.quality == UsageQuality::Inconsistent);
    CHECK(completed(result).raw_events.back().payload->root().get("usage").get("cost_details").get("upstream_inference_input_cost").valid());
  }
  const auto alternative_names = decode({chunk("{}", "\"stop\""), tail(R"({"content":"","role":"assistant"})", "\"stop\"",
      R"({"prompt_tokens":1,"completion_tokens":1,"total_tokens":2,"cost_details":{"upstream_inference_prompt_cost":null,"upstream_inference_input_cost":1.6e-6,"upstream_inference_output_cost":2.4e-6}})"), "[DONE]"});
  CHECK(completed(alternative_names).usage.provider_cost.upstream_input->nano_usd == 1600);
  CHECK(completed(alternative_names).usage.provider_cost.upstream_output->nano_usd == 2400);
  const auto output_conflict = decode({chunk("{}", "\"stop\""), tail(R"({"content":"","role":"assistant"})", "\"stop\"",
      R"({"prompt_tokens":1,"completion_tokens":1,"total_tokens":2,"cost_details":{"upstream_inference_completions_cost":2.4e-6,"upstream_inference_output_cost":2.5e-6}})"), "[DONE]"});
  CHECK(completed(output_conflict).usage.provider_cost.status[3] == CostStatus::Conflict);
  CHECK(!completed(output_conflict).usage.provider_cost.upstream_output && completed(output_conflict).usage.provider_cost.quality == UsageQuality::Inconsistent);
  failed(decode({chunk("{}", "\"stop\""), tail("{}", "\"length\""), "[DONE]"}), ErrorKind::ProtocolCorrupt);
  failed(decode({chunk("{}", "\"stop\""), tail("{}", "null"), "[DONE]"}), ErrorKind::ProtocolCorrupt);
  failed(decode({chunk("{}", "\"stop\""), chunk("{}", "\"stop\""), "[DONE]"}), ErrorKind::ProtocolCorrupt);
  failed(decode({chunk("{}", "\"stop\""), tail("{}", "\"error\""), "[DONE]"}), ErrorKind::RemoteFailure);
  failed(decode({chunk("{}", "\"stop\""), tail("{}"), R"({"error":{"message":"late"}})"}), ErrorKind::RemoteFailure);
  auto changed_identity = tail("{}");
  changed_identity.replace(changed_identity.find("generation-1"), std::string("generation-1").size(), "generation-2");
  failed(decode({chunk("{}", "\"stop\""), changed_identity, "[DONE]"}), ErrorKind::ProtocolCorrupt);
  const auto truncated = decode({chunk("{}", "\"stop\""), tail("{}")});
  const auto& partial = failed(truncated, ErrorKind::Truncated).partial;
  CHECK(partial.usage.stage == UsageStage::Partial && partial.usage.provider_cost.total->nano_usd == 4000);
  const auto cleared = decode({chunk("{}", "\"stop\""), tail("{}"), usage_frame(full_usage), "[DONE]"});
  CHECK(!completed(cleared).usage.provider_cost.total && completed(cleared).usage.provider_cost.source == CostSource::None);
  for (const auto& [amount, expected] : std::vector<std::pair<std::string, CostStatus>>{
      {"0", CostStatus::Available}, {"null", CostStatus::Missing}, {"-0.1", CostStatus::Malformed},
      {"\"4e-6\"", CostStatus::Malformed}, {"9007199.5", CostStatus::PrecisionExceeded}, {"1e100", CostStatus::Overflow}}) {
    const auto counters = "{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2,\"cost\":" + amount + "}";
    const auto result = decode({chunk(R"({"content":"hello"})", "\"stop\""), tail("{}", "\"stop\"", counters), "[DONE]"});
    const auto& c = completed(result);
    CHECK(text_of(c.messages) == "hello" && c.usage.provider_cost.status[0] == expected);
    CHECK(c.usage.extra.empty());
    if (expected == CostStatus::Available) CHECK(c.usage.provider_cost.total->nano_usd == 0);
    else CHECK(!c.usage.provider_cost.total);
  }
  failed(decode({chunk("{}", "\"stop\""), tail("{}", "\"stop\"", R"({"prompt_tokens":1.5,"completion_tokens":1,"total_tokens":2,"cost":4e-6})"), "[DONE]"}), ErrorKind::ProtocolCorrupt);
  const auto unknown = frames({chunk("{}", "\"stop\""), usage_frame(money), "[DONE]"});
  CHECK(completed(unknown).usage.provider_cost.source == CostSource::UnknownCurrency);
  CHECK(completed(unknown).usage.provider_cost.status[0] == CostStatus::UnknownCurrency && !completed(unknown).usage.provider_cost.total);
  const auto missing = frames({chunk("{}", "\"stop\""), usage_frame(full_usage), "[DONE]"});
  CHECK(!completed(missing).usage.provider_cost.total && completed(missing).usage.provider_cost.source == CostSource::None);
}
} // namespace
int main() {
  try {
    idempotent_terminal_money_tail();
    owned_raw_observations(); raw_failure_evidence();
    chunk_partition_invariant(); no_terminal_no_success(); transport_projection_parity(); known_corrupt_never_ignored();
    invalid_tools_and_compatibility(); usage_knowledge(); accumulator_transitions_and_seals(); typed_request_encoding();
    required_metadata_and_identity(); required_usage_and_unknown_cache(); unindexed_tool_order_and_optional_function(); buffered_close_precedence();
    accumulator_interleaved_ordering(); accumulator_order_uniqueness_and_limits();
    tool_history_and_escaped_arguments(); quoted_json_and_request_bounds();
    gateway_request_controls();
    std::cout << "chat semantic properties passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
