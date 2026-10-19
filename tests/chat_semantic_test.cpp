#include "codecs/chat.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "codecs/messages_request.h"
#include "codecs/responses_request.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include "sp/config_defaults.h"
#include "transport/sse_framer.h"
#include <iostream>
#include <limits>
#include <map>
#include <optional>
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
      // Stream identity is id + model; created may repeat or advance but never go back.
      for (size_t i : {0U, 1U, 3U}) {
        auto outcome = frames({chunk(R"({"content":"prefix"})"), response(i, i == 3 ? "6" : "\"changed\"")});
        CHECK(text_of(failed(outcome, ErrorKind::ProtocolCorrupt).partial.messages) == "prefix");
      }
      CHECK(text_of(completed(frames({chunk(R"({"content":"prefix"})"), response(3, "8"), "[DONE]"})).messages) == "prefixkept");
      auto stamped = [](std::string_view created, std::string_view content, std::string_view finish = "null") {
        return "{\"id\":\"generation-1\",\"model\":\"fixture-model\",\"created\":" + std::string(created) +
          ",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":" + json::quote(content) +
          "},\"finish_reason\":" + std::string(finish) + "}]}";
      };
      CHECK(text_of(completed(frames({stamped("7", "a"), stamped("9", "b"), stamped("9", "c"), stamped("12", "d", "\"stop\""), "[DONE]"})).messages) == "abcd");
      auto back = frames({stamped("7", "a"), stamped("9", "b"), stamped("8", "c", "\"stop\""), "[DONE]"});
      CHECK(text_of(failed(back, ErrorKind::ProtocolCorrupt).partial.messages) == "ab");
      CHECK(failed(back, ErrorKind::ProtocolCorrupt).error.safe_message == "creation counter decreased");
      auto other = frames({stamped("7", "a"), stamped("9", "b"),
          R"({"id":"generation-2","model":"fixture-model","created":9,"object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"c"},"finish_reason":"stop"}]})", "[DONE]"});
      CHECK(text_of(failed(other, ErrorKind::ProtocolCorrupt).partial.messages) == "ab");
      CHECK(failed(other, ErrorKind::ProtocolCorrupt).error.safe_message == "inconsistent response identity");
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
void per_chunk_creation_clock() {
  // Shape of an OpenAI-compatible local server that stamps each chunk with the current time.
  // The last two chunks are a captured stream tail; the final chunk carries finish_reason,
  // usage and an extra timings object before [DONE].
  const auto tail = [](std::string_view created, std::string_view choice) {
    return "data: {\"choices\":[" + std::string(choice) + "],\"created\":" + std::string(created) +
      R"(,"id":"chatcmpl-X","model":"local-llm","system_fingerprint":"b4889-9f2250ba","object":"chat.completion.chunk"})" "\n\n";
  };
  const std::string wire =
    tail("1791711213", R"({"finish_reason":null,"index":0,"delta":{"role":"assistant","content":null}})") +
    tail("1791711213", R"({"finish_reason":null,"index":0,"delta":{"content":"Hello"}})") +
    tail("1791711214", R"({"finish_reason":null,"index":0,"delta":{"content":", local"}})") +
    tail("1791711221", R"({"finish_reason":null,"index":0,"delta":{"content":" model."}})") +
    "data: {\"choices\":[{\"finish_reason\":null,\"index\":0,\"delta\":{\"content\":\"\"}}],\"created\":1791711222,\"id\":\"chatcmpl-X\",\"model\":\"local-llm\",\"system_fingerprint\":\"b4889-9f2250ba\",\"object\":\"chat.completion.chunk\"}\n\n"
    "data: {\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,\"delta\":{}}],\"created\":1791711222,\"id\":\"chatcmpl-X\",\"model\":\"local-llm\",\"system_fingerprint\":\"b4889-9f2250ba\",\"object\":\"chat.completion.chunk\",\"usage\":{\"completion_tokens\":45,\"prompt_tokens\":36,\"total_tokens\":81},\"timings\":{\"prompt_n\":26,\"predicted_n\":45}}\n\n"
    "data: [DONE]\n\n";
  Accumulator accumulator;
  chat::Codec codec(descriptor_value(), chat::Mode::Sse, accumulator);
  transport::SseFramer framer;
  CHECK(framer.feed(wire, [&](const transport::SseFrame& frame) { return codec.frame(frame.event, frame.data); }));
  framer.finish(); CHECK(framer.error() == transport::SseError::None); codec.finish();
  CHECK(accumulator.outcome());
  const auto& completion = completed(*accumulator.outcome());
  CHECK(text_of(completion.messages) == "Hello, local model." && completion.stop.kind == StopKind::EndTurn);
  CHECK(completion.usage.input_total && completion.usage.input_total->value == 36);
  CHECK(completion.usage.output_total && completion.usage.output_total->value == 45);
  CHECK(completion.usage.total && completion.usage.total->value == 81);
}
void omitted_open_finish_reason() {
  // Shape of Google's OpenAI-compatible endpoint: every chunk before the last leaves
  // finish_reason out (instead of sending null), repeats the role and carries cumulative usage.
  const auto google = [](std::string_view delta, std::string_view finish, std::string_view completion_tokens) {
    return "data: {\"choices\":[{\"delta\":" + std::string(delta) + (finish.empty() ? "" : ",\"finish_reason\":" + std::string(finish)) +
      ",\"index\":0}],\"created\":1792130675,\"id\":\"g-1\",\"model\":\"gemini-2.5-flash-lite\",\"object\":\"chat.completion.chunk\","
      "\"usage\":{\"completion_tokens\":" + std::string(completion_tokens) + ",\"prompt_tokens\":57,\"total_tokens\":" +
      std::to_string(57 + std::stoi(std::string(completion_tokens))) + "}}\n\n";
  };
  const std::string wire =
    google(R"({"content":"One,","role":"assistant"})", "", "6") +
    google(R"({"content":" two,","role":"assistant"})", "", "24") +
    google(R"({"content":" three.","role":"assistant"})", "\"stop\"", "36") +
    "data: [DONE]\n\n";
  Accumulator accumulator;
  chat::Codec codec(descriptor_value(), chat::Mode::Sse, accumulator);
  transport::SseFramer framer;
  CHECK(framer.feed(wire, [&](const transport::SseFrame& frame) { return codec.frame(frame.event, frame.data); }));
  framer.finish(); CHECK(framer.error() == transport::SseError::None); codec.finish();
  CHECK(accumulator.outcome());
  const auto& completion = completed(*accumulator.outcome());
  CHECK(text_of(completion.messages) == "One, two, three." && completion.stop.kind == StopKind::EndTurn && completion.stop.raw == "stop");
  CHECK(completion.usage.input_total && completion.usage.input_total->value == 57);
  CHECK(completion.usage.output_total && completion.usage.output_total->value == 36);
  CHECK(completion.usage.total && completion.usage.total->value == 93);

  // An absent member on an open stream is the same as null, also for tool calls (an open
  // chunk {"index":0,"delta":{}} used to be ProtocolCorrupt).
  const auto open = [](std::string_view delta) { return envelope("{\"choices\":[{\"index\":0,\"delta\":" + std::string(delta) + "}]}"); };
  const auto tools = frames({
    open(R"({"role":"assistant","tool_calls":[{"index":0,"id":"call-a","type":"function","function":{"name":"weather","arguments":"{\"city\":\"Seoul\"}"}}]})"),
    chunk(R"({"tool_calls":[{"index":1,"id":"call-b","type":"function","function":{"name":"clock","arguments":"{}"}}]})", "\"tool_calls\""),
    "[DONE]"});
  const auto& called = completed(tools);
  CHECK(called.stop.kind == StopKind::ToolUse && called.messages.size() == 1 && called.messages[0].parts.size() == 2);
  CHECK(projection(frames({open(R"({"role":"assistant","content":"a"})"), chunk(R"({"content":"b"})", "\"stop\""), "[DONE]"})) ==
        projection(frames({chunk(R"({"role":"assistant","content":"a"})"), chunk(R"({"content":"b"})", "\"stop\""), "[DONE]"})));

  // Nothing else relaxes: a stream still needs a string finish_reason before [DONE], a chunk
  // after the stop reason still needs the member, and so does a buffered choice.
  const auto unfinished = frames({open(R"({"role":"assistant","content":"prefix"})"), open(R"({"content":" more"})"), "[DONE]"});
  CHECK(text_of(failed(unfinished, ErrorKind::ProtocolCorrupt).partial.messages) == "prefix more");
  const auto after_stop = frames({open(R"({"role":"assistant","content":"prefix"})"), chunk("{}", "\"stop\""),
                                  envelope(R"({"choices":[{"index":0,"delta":{}}],"usage":{"prompt_tokens":5,"completion_tokens":1,"total_tokens":6}})"),
                                  "[DONE]"});
  CHECK(failed(after_stop, ErrorKind::ProtocolCorrupt).partial.stop && failed(after_stop, ErrorKind::ProtocolCorrupt).partial.stop->kind == StopKind::EndTurn);
  failed(buffered(envelope(R"({"choices":[{"index":0,"message":{"role":"assistant","content":"ok"}}]})", false)), ErrorKind::ProtocolCorrupt);
}
// What a stream's sink is shown for the parts of a message: [begin], a text delta with its bytes, [seal]. An empty
// text delta would show as [delta:], and none may.
struct Traced { std::string trace; Outcome outcome; };
Traced traced(const std::vector<std::string>& wire, bool normal = true) {
  Traced result;
  Accumulator accumulator({}, [&](const Event& event) {
    if (std::holds_alternative<PartBegin>(event)) result.trace += "[begin]";
    else if (const auto* delta = std::get_if<PartDelta>(&event)) result.trace += "[" + std::string(delta->payload.kind == PartKind::Text ? "delta:" : "other:") + std::string(delta->payload.bytes) + "]";
    else if (std::holds_alternative<PartSeal>(event)) result.trace += "[seal]";
  });
  chat::Codec codec(descriptor_value(), chat::Mode::Sse, accumulator);
  for (const auto& frame : wire) if (!codec.frame("message", frame)) break;
  codec.finish({normal, ErrorKind::Truncated});
  CHECK(accumulator.outcome()); result.outcome = *accumulator.outcome();
  return result;
}
void empty_text_delta_is_withheld() {
  // OpenAI opens a stream with {"role":"assistant","content":""}: the Text part opens with that empty string. The part
  // is still opened and sealed, in the same place as before, but the sink is shown only deltas that carry text.
  const auto opening = chunk(R"({"role":"assistant","content":"","refusal":null})");
  const auto streamed = traced({opening, chunk(R"({"content":"Hi"})"), chunk(R"({"content":""})"), chunk(R"({"content":" there"})"), chunk("{}", "\"stop\""), "[DONE]"});
  CHECK(streamed.trace == "[begin][delta:Hi][delta: there][seal]");
  CHECK(text_of(completed(streamed.outcome).messages) == "Hi there" && completed(streamed.outcome).messages[0].parts.size() == 1);
  CHECK(projection(streamed.outcome) == projection(buffered(body(R"({"role":"assistant","content":"Hi there"})"))));

  // A text part that only ever received empty text is still there, and is the empty Text a buffered response returns.
  const auto only_empty = traced({opening, chunk("{}", "\"stop\""), "[DONE]"});
  CHECK(only_empty.trace == "[begin][seal]");
  CHECK(completed(only_empty.outcome).messages.size() == 1 && completed(only_empty.outcome).messages[0].parts.size() == 1);
  CHECK(std::holds_alternative<Text>(completed(only_empty.outcome).messages[0].parts[0]) && text_of(completed(only_empty.outcome).messages).empty());
  CHECK(projection(only_empty.outcome) == projection(buffered(body(R"({"role":"assistant","content":""})"))));
  // A null or absent content opens no part, in either mode.
  CHECK(traced({chunk(R"({"role":"assistant","content":null})"), chunk("{}", "\"stop\""), "[DONE]"}).trace.empty());

  // A call cut after the empty opening keeps the empty part in its partial output, as it did.
  const auto cut = traced({opening}, false);
  CHECK(cut.trace == "[begin]");
  const auto& partial = failed(cut.outcome, ErrorKind::Truncated).partial;
  CHECK(partial.messages.size() == 1 && partial.messages[0].parts.size() == 1 && std::holds_alternative<Text>(partial.messages[0].parts[0]));
  CHECK(text_of(partial.messages).empty());

  // Only text deltas are withheld: an empty reasoning delta is delivered as it was.
  const auto reasoning = traced({chunk(R"({"role":"assistant","reasoning_content":""})"), chunk(R"({"content":"ok"})"), chunk("{}", "\"stop\""), "[DONE]"});
  CHECK(reasoning.trace == "[begin][other:][begin][delta:ok][seal][seal]");
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
  check(frames({chunk(R"({"role":"assistant","content":"x"})", "7")}), ErrorKind::ProtocolCorrupt, "invalid finish reason", "chat.finish_reason.invalid");
  check(buffered(body(R"({"role":"assistant","content":"x"})", "[1]")), ErrorKind::ProtocolCorrupt, "invalid finish reason", "chat.finish_reason.invalid");
  check(frames({"{\"id\":\"g\",\"model\":\"" + secret + "\",\"created\":\"" + secret + "\",\"object\":\"chat.completion.chunk\",\"choices\":[]}"}),
        ErrorKind::ProtocolCorrupt, "creation counter required", "chat.created.required");
  check(frames({"{\"id\":\"g\",\"model\":\"m\",\"created\":1,\"object\":" + json::quote(secret) + ",\"choices\":[]}"}),
        ErrorKind::Unsupported, "unknown response object tag", "chat.object.tag_unknown");
  check(frames({chunk(R"({"role":"assistant","content":"x"})", "\"stop\""), "[DONE]", chunk(R"({"content":"y"})")}),
        ErrorKind::ProtocolCorrupt, "frame after DONE", "chat.done.frame_after");
  check(frames({chunk(R"({"role":"assistant","content":"x"})")}), ErrorKind::Truncated, "missing family terminal evidence", "chat.terminal.missing");
  // A close the transport reports is the transport's: its kind stays, and no codec check is named.
  const auto cut = frames({chunk(R"({"role":"assistant","content":"x"})")}, false);
  CHECK(failed(cut, ErrorKind::Truncated).error.reason.empty() && failed(cut, ErrorKind::Truncated).error.safe_message == "abnormal response close");
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
  // Without a reported cache read the uncached share stays unknown (see uncached_input_rule).
  for (auto details : {R"({})", R"({"cache_write_tokens":3})", R"({"cache_write_tokens":0})"}) {
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

// A parallel tool turn may answer every call in one Tool message, as the Responses, Messages and
// Generate codecs accept. Chat Completions takes one wire message per result, so the encoder
// splits the message instead of refusing it.
void one_tool_message_with_several_results() {
  const auto outcome = buffered(body(R"({"role":"assistant","content":"checking","tool_calls":[{"id":"a","type":"function","function":{"name":"f","arguments":"{\"x\":1}"}},{"id":"b","type":"function","function":{"name":"g","arguments":"{}"}}]})", "\"tool_calls\""));
  auto request = [&](std::vector<Message> results) {
    chat::Request value; value.model = "fixture-model";
    value.canonical_messages.push_back(Message{"", Role::User, {Text{"Use the two tools."}}});
    Message assistant{"", Role::Assistant, {}};
    for (const auto& part : completed(outcome).messages.front().parts)
      if (std::holds_alternative<Text>(part) || std::holds_alternative<ToolCall>(part)) assistant.parts.push_back(part);
    value.canonical_messages.push_back(std::move(assistant));
    for (auto& result : results) value.canonical_messages.push_back(std::move(result));
    return value;
  };
  auto tool = [](std::initializer_list<ToolResult> results) {
    Message message{"", Role::Tool, {}};
    for (const auto& result : results) message.parts.push_back(result);
    return message;
  };
  auto wire = [&](const chat::Request& value, bool streaming) {
    auto encoded = chat::encode(descriptor_value(), value, streaming);
    CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
    return std::get<chat::EncodedRequest>(std::move(encoded)).body;
  };
  for (bool streaming : {false, true}) {
    const auto split = wire(request({tool({ToolResult{"b", "second"}, ToolResult{"a", "first", true}})}), streaming);
    // One Tool message per result on the wire, in the order the caller gave.
    auto document = json::parse(split);
    CHECK(std::holds_alternative<json::Document>(document));
    const auto messages = std::get<json::Document>(document).root().get("messages");
    CHECK(messages.size() == 4);
    CHECK(messages.at(2).get("role").as_string() == "tool" && messages.at(3).get("role").as_string() == "tool");
    CHECK(messages.at(2).get("tool_call_id").as_string() == "b" && messages.at(2).get("content").as_string() == "second");
    CHECK(messages.at(3).get("tool_call_id").as_string() == "a" && messages.at(3).get("content").as_string() == "first");
    // The same conversation as two Tool messages encodes to the same bytes.
    CHECK(wire(request({tool({ToolResult{"b", "second"}}), tool({ToolResult{"a", "first", true}})}), streaming) == split);
    // A single result and a result split across messages keep working.
    CHECK(wire(request({tool({ToolResult{"a", "first"}, ToolResult{"b", "second"}})}), streaming) ==
          wire(request({tool({ToolResult{"a", "first"}}), tool({ToolResult{"b", "second"}})}), streaming));
  }
  auto rejects = [&](chat::Request value, ErrorKind kind, std::string_view fragment) {
    auto result = chat::encode(descriptor_value(), value, false);
    CHECK(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    CHECK(error.kind == kind && error.safe_message.find(fragment) != std::string::npos);
  };
  // Results still have to resolve the calls of the preceding assistant message, each exactly once.
  rejects(request({tool({ToolResult{"a", "first"}, ToolResult{"a", "again"}})}), ErrorKind::InvalidRequest, "pending call");
  rejects(request({tool({ToolResult{"a", "first"}, ToolResult{"missing", "x"}})}), ErrorKind::InvalidRequest, "pending call");
  rejects(request({tool({ToolResult{"a", "first"}})}), ErrorKind::InvalidRequest, "missing tool results");
  // What the Tool message may hold is stated by the refusal: tool results only, at least one.
  auto mixed = tool({ToolResult{"a", "first"}});
  mixed.parts.push_back(Text{"note"});
  rejects(request({std::move(mixed), tool({ToolResult{"b", "second"}})}), ErrorKind::InvalidRequest, "Tool message holds only tool results");
  rejects(request({Message{"", Role::Tool, {}}}), ErrorKind::InvalidRequest, "Tool message holds only tool results");
  ToolResult typed{"a", ""}; typed.content_parts = {Text{"typed"}};
  rejects(request({tool({typed, ToolResult{"b", "second"}})}), ErrorKind::Unsupported, "typed media");
}

// Current OpenAI chat models (gpt-5.x and gpt-6.x, the reasoning models) answer HTTP 400
// "Use 'max_completion_tokens'" to max_tokens. Direct OpenAI therefore defaults to the new member;
// OpenRouter and other compatible endpoints keep max_tokens, which they all understand. An explicit
// binding always wins, whatever the origin.
void output_cap_member_by_origin() {
  auto cap_member = [](std::string_view origin, std::string_view bindings = {}, descriptor::PolicySnapshot policy = {}) {
    const std::string source = R"({"descriptor_version":1,"revision":1,"id":"cap","family":"openai.chat","connection":{"base_url":")" +
        std::string(origin) + R"(","paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}})" +
        (bindings.empty() ? std::string() : ",\"bindings\":" + std::string(bindings)) + "}";
    auto loaded = policy ? descriptor::load(source, std::move(policy)) : descriptor::load(source);
    CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
    const auto& value = std::get<descriptor::ValidatedDescriptor>(loaded);
    chat::Request request; request.model = "gpt-6-luna"; request.max_output_tokens = 7;
    request.messages.push_back({Role::User, "Say hello."});
    auto encoded = chat::encode(value, request, false);
    CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
    auto body = json::parse(std::get<chat::EncodedRequest>(encoded).body);
    const auto root = std::get<json::Document>(body).root();
    CHECK(root.get("max_completion_tokens").valid() != root.get("max_tokens").valid());
    const std::string member(value.max_output_tokens_member());
    CHECK(root.get(member).valid() && root.get(member).as_uint() == 7);
    return member;
  };
  CHECK(cap_member("https://api.openai.com") == "max_completion_tokens");
  CHECK(cap_member("https://openrouter.ai") == "max_tokens");
  CHECK(cap_member("https://api.groq.com") == "max_tokens");
  CHECK(cap_member("http://127.0.0.1:18080") == "max_tokens");
  // The other output slots keep their defaults, and an explicit binding wins either way.
  CHECK(cap_member("https://api.openai.com", R"({"max_output_tokens":"max_tokens"})") == "max_tokens");
  CHECK(cap_member("https://openrouter.ai", R"({"max_output_tokens":"max_completion_tokens"})") == "max_completion_tokens");
  CHECK(cap_member("https://api.openai.com", R"({"model":"deployment"})") == "max_completion_tokens");

  // The origin list is policy data: a snapshot that predates it keeps max_tokens everywhere, and a
  // list is admitted only for Chat, as canonical origins, apart from the OpenRouter ones.
  auto document = json::parse(config_defaults::descriptor_policy_json);
  CHECK(std::holds_alternative<json::Document>(document));
  const auto root = std::get<json::Document>(document).root();
  auto snapshot = [&](std::string_view family, std::string_view origins) {
    std::string families = "[";
    for (auto entry : root.get("families").elements()) {
      if (families.size() > 1) families += ',';
      std::string member = "{";
      for (auto field : entry.members()) {
        if (field.key == "max_completion_tokens_origins") continue;
        if (member.size() > 1) member += ',';
        member += json::quote(field.key) + ':' + field.value.dump();
      }
      if (!origins.empty() && entry.get("family").as_string() == family) member += ",\"max_completion_tokens_origins\":" + std::string(origins);
      families += member + '}';
    }
    families += ']';
    return descriptor::load_policy("{\"version\":1,\"revision\":" + std::to_string(root.get("revision").as_uint()) + ",\"families\":" + families + '}',
                                   config_defaults::codec_defaults_json);
  };
  auto admitted = snapshot("openai.chat", {});
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(admitted));
  if (auto* old = std::get_if<descriptor::PolicySnapshot>(&admitted)) CHECK(cap_member("https://api.openai.com", {}, *old) == "max_tokens");
  auto custom = snapshot("openai.chat", R"(["https://gateway.example.test"])");
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(custom));
  if (auto* policy = std::get_if<descriptor::PolicySnapshot>(&custom)) {
    CHECK(cap_member("https://gateway.example.test", {}, *policy) == "max_completion_tokens");
    CHECK(cap_member("https://api.openai.com", {}, *policy) == "max_tokens");
  }
  for (std::string_view bad : {R"(["https://api.openai.com/"])", R"(["api.openai.com"])", R"(["https://openrouter.ai"])", R"("https://api.openai.com")",
                               R"(["https://api.openai.com","https://api.openai.com"])", R"([""])"})
    CHECK(std::holds_alternative<descriptor::ConfigError>(snapshot("openai.chat", bad)));
  CHECK(std::holds_alternative<descriptor::ConfigError>(snapshot("openai.responses", R"(["https://api.openai.com"])")));
  CHECK(std::holds_alternative<descriptor::ConfigError>(snapshot("anthropic.messages", R"(["https://api.anthropic.com"])")));
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
// OpenRouter plugins are sent as given at a declared origin, and URL citations of the answer become typed
// citations of its text, buffered and streamed.
void gateway_plugins_and_citations() {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"gateway","family":"openai.chat","connection":{"base_url":"https://openrouter.ai","paths":{"buffered":"/api/v1/chat/completions","streaming":"/api/v1/chat/completions"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& gateway = std::get<descriptor::ValidatedDescriptor>(loaded);
  auto options = [](std::string_view text) {
    auto parsed = json::parse(text); CHECK(std::holds_alternative<json::Document>(parsed));
    return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  };
  chat::Request request; request.model = "fixture-model"; request.messages.push_back({Role::User, "hello"});
  request.plugins = {{"web", options(R"({"max_results":2,"engine":"exa"})")}, {"response-healing", {}}};
  auto result = chat::encode(gateway, request, false);
  CHECK(std::holds_alternative<chat::EncodedRequest>(result));
  auto parsed = json::parse(std::get<chat::EncodedRequest>(result).body); CHECK(std::holds_alternative<json::Document>(parsed));
  const auto plugins = std::get<json::Document>(parsed).root().get("plugins");
  CHECK(plugins.size() == 2 && plugins.at(0).get("id").as_string() == "web" && plugins.at(0).get("max_results").as_uint() == 2u &&
        plugins.at(0).get("engine").as_string() == "exa" && plugins.at(1).size() == 1 && plugins.at(1).get("id").as_string() == "response-healing");
  CHECK(std::get<Error>(chat::encode(descriptor_value(), request, false)).kind == ErrorKind::InvalidRequest);
  auto reject = [&](const chat::Request& changed) { CHECK(std::get<Error>(chat::encode(gateway, changed, false)).kind == ErrorKind::InvalidRequest); };
  auto changed = request; changed.plugins.push_back(changed.plugins.front()); reject(changed);
  changed = request; changed.plugins[0].options = options(R"({"id":"other"})"); reject(changed);
  changed = request; changed.plugins[0].options = options("[1]"); reject(changed);
  changed = request; changed.plugins[0].id = "not a token"; reject(changed);

  const std::string cite = R"({"type":"url_citation","url_citation":{"url":"https://example.com","title":"Example","content":"Excerpt","start_index":0,"end_index":7}})";
  auto check = [&](const Outcome& outcome) {
    const auto& text = std::get<Text>(completed(outcome).messages.at(0).parts.at(0));
    CHECK(text.value == "Example Domain" && text.citations.size() == 1);
    const auto& c = text.citations[0];
    CHECK(c.kind == "url_citation" && c.url == "https://example.com" && c.title == "Example" && c.start == 0u && c.end == 7u &&
          c.raw && c.raw->root().get("url_citation").get("content").as_string() == "Excerpt");
  };
  Accumulator accumulator;
  chat::Codec codec(gateway, chat::Mode::Buffered, accumulator, {}, std::get<chat::EncodedRequest>(result).context);
  codec.buffered(body(R"({"role":"assistant","content":"Example Domain","annotations":[)" + cite + "]}"), {});
  CHECK(accumulator.outcome()); check(*accumulator.outcome());
  // A stream that repeats the annotation still has one citation.
  auto streamed = chat::encode(gateway, request, true); CHECK(std::holds_alternative<chat::EncodedRequest>(streamed));
  Accumulator stream_accumulator;
  chat::Codec stream_codec(gateway, chat::Mode::Sse, stream_accumulator, {}, std::get<chat::EncodedRequest>(streamed).context);
  for (const auto& frame : {chunk(R"({"role":"assistant","content":"Example "})"), chunk(R"({"content":"Domain","annotations":[)" + cite + "]}"),
                            chunk(R"({"annotations":[)" + cite + "]}", "\"stop\""), std::string("[DONE]")})
    CHECK(stream_codec.frame("message", frame));
  stream_codec.finish({true, ErrorKind::Truncated});
  CHECK(stream_accumulator.outcome()); check(*stream_accumulator.outcome());
  // The plugins are configuration of the conversation.
  request.messages.clear(); request.canonical_messages = {Message{{}, Role::User, {Text{"hello"}}}, completed(*accumulator.outcome()).messages.at(0), Message{{}, Role::User, {Text{"next"}}}};
  CHECK(std::holds_alternative<chat::EncodedRequest>(chat::encode(gateway, request, false)));
  changed = request; changed.plugins.pop_back();
  CHECK(std::get<Error>(chat::encode(gateway, changed, false)).kind == ErrorKind::ReplayIneligible);
}
// OpenRouter provider.max_price (https://openrouter.ai/docs/guides/routing/provider-selection#max-price).
void gateway_max_price() {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"gateway","family":"openai.chat","connection":{"base_url":"https://openrouter.ai","paths":{"buffered":"/api/v1/chat/completions","streaming":"/api/v1/chat/completions"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& gateway = std::get<descriptor::ValidatedDescriptor>(loaded);
  const auto wire = [&](const chat::Request& request) {
    auto result = chat::encode(gateway, request, false);
    CHECK(std::holds_alternative<chat::EncodedRequest>(result));
    return std::get<chat::EncodedRequest>(result).body;
  };
  const auto rejected = [&](const chat::Request& request, ErrorKind kind, const descriptor::ValidatedDescriptor& d) {
    auto result = chat::encode(d, request, false);
    CHECK(std::holds_alternative<Error>(result) && std::get<Error>(result).kind == kind);
  };
  const auto capture = [&](const chat::Request& request) {
    auto result = chat::encode(gateway, request, false);
    CHECK(std::holds_alternative<chat::EncodedRequest>(result));
    Accumulator accumulator;
    chat::Codec codec(gateway, chat::Mode::Buffered, accumulator, {}, std::get<chat::EncodedRequest>(result).context);
    codec.buffered(body(R"({"role":"assistant","content":"answer"})"), {});
    CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
    return std::get<Completion>(*accumulator.outcome()).messages.at(0);
  };
  chat::Request request; request.model = "fixture-model"; request.messages.push_back({Role::User, "hello"});
  OpenRouterRouting routing; routing.zdr = true;
  OpenRouterMaxPrice price;
  price.prompt_nano_usd = 75'000'000;          // $0.075 per million prompt tokens
  price.completion_nano_usd = 15'000'000'000;  // $15 per million completion tokens
  price.image_nano_usd = 100'000;              // $0.0001 per image
  price.audio_nano_usd = 1;                    // one nanoUSD per audio unit
  price.request_nano_usd = 0;                  // free endpoints only
  routing.max_price = price; request.provider = routing;
  // Exact decimal JSON numbers: no exponent (0.0001, not 1e-04), no trailing zeros, no binary rounding.
  const auto text = wire(request);
  CHECK(text.find(R"("provider":{"zdr":true,"max_price":{"prompt":0.075,"completion":15,"image":0.0001,"audio":0.000000001,"request":0}})") != std::string::npos);
  auto parsed = json::parse(text); CHECK(std::holds_alternative<json::Document>(parsed));
  const auto sent = std::get<json::Document>(parsed).root().get("provider").get("max_price");
  CHECK(sent.size() == 5 && sent.get("prompt").as_double() == 0.075 && sent.get("completion").as_double() == 15.0);
  CHECK(sent.get("image").as_double() == 0.0001 && sent.get("audio").as_double() == 1e-9 && sent.get("request").as_double() == 0.0);
  // Only amounts that are set are sent, after the other routing members.
  routing.data_collection = "deny"; routing.max_price = OpenRouterMaxPrice{}; routing.max_price->completion_nano_usd = 1'500'000'000;
  request.provider = routing;
  CHECK(wire(request).find(R"("provider":{"zdr":true,"data_collection":"deny","max_price":{"completion":1.5}})") != std::string::npos);
  routing.max_price->completion_nano_usd = std::uint64_t{1} << 53; request.provider = routing;
  CHECK(wire(request).find(R"("max_price":{"completion":9007199.254740992})") != std::string::npos);
  routing.max_price.reset(); request.provider = routing;
  CHECK(wire(request).find("max_price") == std::string::npos);
  // An empty ceiling, an amount above 2^53 nanoUSD and an undeclared origin reject before I/O.
  auto changed = request; changed.provider->max_price = OpenRouterMaxPrice{};
  rejected(changed, ErrorKind::InvalidRequest, gateway);
  for (const auto amount : {(std::uint64_t{1} << 53) + 1, std::numeric_limits<std::uint64_t>::max()}) {
    changed.provider->max_price->image_nano_usd = amount;
    rejected(changed, ErrorKind::InvalidRequest, gateway);
  }
  changed.provider->max_price->image_nano_usd = 1;
  CHECK(wire(changed).find(R"("max_price":{"image":0.000000001})") != std::string::npos);
  rejected(changed, ErrorKind::InvalidRequest, descriptor_value());
  // The ceiling joins the routing binding of native history.
  request.provider->max_price = price;
  const auto answer = capture(request);
  request.messages.clear();
  request.canonical_messages = {Message{{}, Role::User, {Text{"hello"}}}, answer, Message{{}, Role::User, {Text{"next"}}}};
  CHECK(wire(request).find(R"("max_price":{"prompt":0.075,)") != std::string::npos);
  for (int field = 0; field != 3; ++field) {
    changed = request;
    if (field == 0) changed.provider->max_price->prompt_nano_usd = 75'000'001;
    if (field == 1) changed.provider->max_price->request_nano_usd.reset();
    if (field == 2) changed.provider->max_price.reset();
    rejected(changed, ErrorKind::ReplayIneligible, gateway);
  }
  // History sealed without a ceiling keeps its binding and cannot gain one.
  changed = request; changed.provider->max_price.reset(); changed.canonical_messages.resize(1);
  const auto unpriced = capture(changed);
  changed.canonical_messages = {Message{{}, Role::User, {Text{"hello"}}}, unpriced, Message{{}, Role::User, {Text{"next"}}}};
  CHECK(wire(changed).find("max_price") == std::string::npos);
  changed.provider->max_price = price;
  rejected(changed, ErrorKind::ReplayIneligible, gateway);
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
// Captured OpenRouter Chat usage (openai/gpt-5-nano): unlisted detail counters must survive in
// extra and decimal costs must not gain a nanoUSD (1.5e-06 was 1501).
void unlisted_usage_leaves_and_decimal_cost() {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"captured-openrouter-chat","family":"openai.chat","connection":{"base_url":"https://openrouter.ai","paths":{"buffered":"/api/v1/chat/completions","streaming":"/api/v1/chat/completions"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& gateway = std::get<descriptor::ValidatedDescriptor>(loaded);
  const auto decode = [&](std::string_view counters, bool sse) {
    Accumulator acc; chat::Codec codec(gateway, sse ? chat::Mode::Sse : chat::Mode::Buffered, acc);
    if (sse) {
      for (const auto& wire : {chunk(R"({"role":"assistant","content":"blue"})", "\"stop\""), usage_frame(counters), std::string("[DONE]")})
        if (!codec.frame("message", wire)) break;
      codec.finish();
    } else codec.buffered(body(R"({"role":"assistant","content":"blue"})", "\"stop\"", counters), {});
    CHECK(acc.outcome()); return *acc.outcome();
  };
  const std::string captured = R"({"prompt_tokens":30,"completion_tokens":144,"total_tokens":174,"cost":0.0000591,"is_byok":false,"prompt_tokens_details":{"cached_tokens":0,"cache_write_tokens":0,"audio_tokens":0,"video_tokens":0},"cost_details":{"upstream_inference_cost":0.0000591,"upstream_inference_prompt_cost":0.0000015,"upstream_inference_completions_cost":0.0000576},"completion_tokens_details":{"reasoning_tokens":128,"image_tokens":0,"audio_tokens":0}})";
  for (bool sse : {false, true}) {
    const auto outcome = decode(captured, sse);
    const auto& usage = completed(outcome).usage;
    CHECK(text_of(completed(outcome).messages) == "blue" && usage.quality == UsageQuality::Consistent);
    CHECK(usage.input_total->value == 30 && usage.output_total->value == 144 && usage.reasoning->value == 128 && usage.total->value == 174);
    // Typed and already-named counters are not repeated; monetary members never become leaves.
    const std::map<std::string, uint64_t> expected{{"input_audio", 0}, {"audio_tokens", 0},
        {"prompt_tokens_details.video_tokens", 0}, {"completion_tokens_details.image_tokens", 0}};
    CHECK(usage.extra.size() == expected.size());
    for (const auto& [key, value] : expected) CHECK(usage.extra.contains(key) && usage.extra.at(key).value == value && usage.extra.at(key).evidence == Evidence::Reported);
    const auto& cost = usage.provider_cost;
    CHECK(cost.source == CostSource::OpenRouterUsd && cost.quality == UsageQuality::Consistent);
    CHECK(cost.total->nano_usd == 59100 && cost.upstream_total->nano_usd == 59100);
    CHECK(cost.upstream_input->nano_usd == 1500 && cost.upstream_output->nano_usd == 57600);
    CHECK(cost.upstream_input->nano_usd + cost.upstream_output->nano_usd == cost.total->nano_usd);
    CHECK(cost.total->rounding == CostRounding::CeilingShortestDecimal);
  }
  // Nonzero unlisted counters at every depth; noninteger or negative leaves stay diagnostics only.
  const auto nonzero = decode(R"({"prompt_tokens":100,"completion_tokens":20,"total_tokens":120,"prompt_cache_hit_tokens":10,"queue_time":0.25,"future":-1,"server":{"web_search_requests":2,"label":"x"},"prompt_tokens_details":{"cached_tokens":10,"cache_write_tokens":5,"audio_tokens":0,"video_tokens":40},"completion_tokens_details":{"reasoning_tokens":4,"image_tokens":7,"audio_tokens":0,"accepted_prediction_tokens":1}})", true);
  const auto& usage = completed(nonzero).usage;
  CHECK(usage.extra.at("prompt_tokens_details.video_tokens").value == 40 && usage.extra.at("completion_tokens_details.image_tokens").value == 7);
  CHECK(usage.extra.at("prompt_cache_hit_tokens").value == 10 && usage.extra.at("server.web_search_requests").value == 2);
  CHECK(usage.extra.at("accepted_prediction_tokens").value == 1 && !usage.extra.contains("completion_tokens_details.accepted_prediction_tokens"));
  CHECK(!usage.extra.contains("queue_time") && !usage.extra.contains("future") && !usage.extra.contains("prompt_tokens_details.cached_tokens"));
  CHECK(usage.extra.size() == 7 && usage.cache_read->value == 10 && usage.cache_write->value == 5 && usage.input_uncached->value == 85);
}
// input_uncached = prompt_tokens - cached_tokens - cache_write_tokens (Evidence::Derived) once the
// cache read is reported. OpenAI-shaped origins have no cache-write counter, so its absence is zero;
// a declared OpenRouter origin always reports cache_write_tokens, so its absence there stays unknown.
void uncached_input_rule() {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"uncached-openrouter-chat","family":"openai.chat","connection":{"base_url":"https://openrouter.ai","paths":{"buffered":"/api/v1/chat/completions","streaming":"/api/v1/chat/completions"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& gateway = std::get<descriptor::ValidatedDescriptor>(loaded);
  const auto decode = [&](const descriptor::ValidatedDescriptor& d, std::string_view counters, bool sse) {
    Accumulator acc; chat::Codec codec(d, sse ? chat::Mode::Sse : chat::Mode::Buffered, acc);
    if (sse) {
      for (const auto& wire : {chunk(R"({"role":"assistant","content":"ok"})", "\"stop\""), usage_frame(counters), std::string("[DONE]")})
        if (!codec.frame("message", wire)) break;
      codec.finish();
    } else codec.buffered(body(R"({"role":"assistant","content":"ok"})", "\"stop\"", counters), {});
    CHECK(acc.outcome()); return completed(*acc.outcome()).usage;
  };
  const auto counters = [](std::string_view details) {
    return std::string(R"({"prompt_tokens":10,"completion_tokens":4,"total_tokens":14,"prompt_tokens_details":)") + std::string(details) + "}";
  };
  struct Case { const descriptor::ValidatedDescriptor* origin; const char* details; std::optional<uint64_t> uncached; };
  const Case cases[]{
      {&descriptor_value(), R"({"cached_tokens":4})", 6},
      {&descriptor_value(), R"({"cached_tokens":0})", 10},
      {&descriptor_value(), R"({"cached_tokens":4,"cache_write_tokens":3})", 3},
      {&descriptor_value(), R"({"cache_write_tokens":3})", std::nullopt},
      {&gateway, R"({"cached_tokens":4,"cache_write_tokens":3})", 3},
      {&gateway, R"({"cached_tokens":0,"cache_write_tokens":0})", 10},
      {&gateway, R"({"cached_tokens":4})", std::nullopt},
      {&gateway, R"({"cache_write_tokens":3})", std::nullopt}};
  for (const auto& c : cases) for (bool sse : {false, true}) {
    const auto usage = decode(*c.origin, counters(c.details), sse);
    CHECK(usage.quality == UsageQuality::Consistent && usage.input_total->value == 10);
    CHECK(usage.input_uncached.has_value() == c.uncached.has_value());
    if (c.uncached) CHECK(usage.input_uncached->value == *c.uncached && usage.input_uncached->evidence == Evidence::Derived);
  }
  // Live direct OpenAI shape (gpt-5-nano): no cache_write_tokens member at all.
  const auto live = decode(descriptor_value(), R"({"prompt_tokens":30,"completion_tokens":74,"total_tokens":104,"prompt_tokens_details":{"cached_tokens":0,"audio_tokens":0},"completion_tokens_details":{"reasoning_tokens":64,"audio_tokens":0,"accepted_prediction_tokens":0,"rejected_prediction_tokens":0}})", false);
  CHECK(live.input_uncached->value == 30 && live.input_uncached->evidence == Evidence::Derived && !live.cache_write);
}
// Projections that borrow from their owner, and pointer accessors into an Outcome, must refuse
// a temporary owner at compile time; the named-owner forms must stay callable.
template <class T> concept BorrowsRefusal = requires(T&& value) { std::forward<T>(value).first_refusal(); };
template <class T> concept BorrowsExecutable = requires(T&& value) { std::forward<T>(value).executable_tool_calls(); };
template <class T> concept BorrowsInvalid = requires(T&& value) { std::forward<T>(value).invalid_tool_calls(); };
template <class T> concept PointsIntoOutcome = requires(T&& value) {
  as_completion(std::forward<T>(value)); as_failure(std::forward<T>(value)); as_error(std::forward<T>(value));
};
static_assert(BorrowsRefusal<const Message&> && BorrowsExecutable<const Message&> && BorrowsInvalid<const Message&>);
static_assert(!BorrowsRefusal<Message> && !BorrowsExecutable<Message> && !BorrowsInvalid<Message>);
static_assert(BorrowsRefusal<const Completion&> && BorrowsExecutable<const Completion&> && BorrowsInvalid<const Completion&>);
static_assert(!BorrowsRefusal<Completion> && !BorrowsExecutable<Completion> && !BorrowsInvalid<Completion>);
static_assert(BorrowsRefusal<const PartialCompletion&> && BorrowsExecutable<const PartialCompletion&> && BorrowsInvalid<const PartialCompletion&>);
static_assert(!BorrowsRefusal<PartialCompletion> && !BorrowsExecutable<PartialCompletion> && !BorrowsInvalid<PartialCompletion>);
static_assert(PointsIntoOutcome<const Outcome&> && PointsIntoOutcome<const std::shared_ptr<const Outcome>&>);
static_assert(!PointsIntoOutcome<Outcome> && !PointsIntoOutcome<std::shared_ptr<const Outcome>>);
void value_builders_and_projections() {
  const auto m_user = Message::user_text("hello", "u1");
  CHECK(m_user.role == Role::User && m_user.id == "u1" && m_user.joined_text() == "hello");
  const auto m_sys = Message::system_text("system prompt");
  CHECK(m_sys.role == Role::System && m_sys.joined_text() == "system prompt");
  const auto m_dev = Message::developer_text("dev prompt");
  CHECK(m_dev.role == Role::Developer && m_dev.joined_text() == "dev prompt");
  const auto m_asst = Message::assistant_text("answer");
  CHECK(m_asst.role == Role::Assistant && m_asst.joined_text() == "answer");
  const auto m_tool = Message::tool_result("call_9", R"({"ok":1})", true, "t1");
  CHECK(m_tool.role == Role::Tool && m_tool.id == "t1" && m_tool.parts.size() == 1);
  const auto& tr = std::get<ToolResult>(m_tool.parts[0]);
  CHECK(tr.tool_use_id == "call_9" && tr.content == R"({"ok":1})" && tr.is_error);

  Message rich{{}, Role::Assistant, {
      Thinking{"step1 ", {}},
      Text{"partA "},
      Thinking{"step2", {}},
      Refusal{"blocked_1", {}},
      Refusal{"blocked_2", {}},
      ToolCall{"c_client", "exec_me", ToolCallKind::ClientExecuted, {}, {}, {}},
      ToolCall{"c_server", "web_search", ToolCallKind::ServerExecuted, {}, {}, {}},
      ToolCall{"c_approval", "mcp_approve", ToolCallKind::ApprovalRequest, {}, {}, {}},
      InvalidToolCall{"c_bad", "broken", ToolCallKind::ClientExecuted, "{bad", InvalidReason::NotJson, {}, {}},
      Text{"partB"}
  }, {}, {}};
  CHECK(rich.joined_text() == "partA partB");
  CHECK(rich.joined_thinking() == "step1 step2");
  CHECK(rich.first_refusal() && *rich.first_refusal() == "blocked_1");
  const auto exec_calls = rich.executable_tool_calls();
  CHECK(exec_calls.size() == 1 && exec_calls[0].get().id == "c_client" && exec_calls[0].get().kind == ToolCallKind::ClientExecuted);
  const auto bad_calls = rich.invalid_tool_calls();
  CHECK(bad_calls.size() == 1 && bad_calls[0].get().id == "c_bad" && rich.has_invalid_tool_calls());

  Usage u{};
  CHECK(!u.input_total_value() && !u.output_total_value() && !u.reasoning_value() && !u.total_tokens_value() && !u.cost_nano_usd_value());
  u.input_total = Count{12, Evidence::Reported};
  u.output_total = Count{34, Evidence::Reported};
  u.reasoning = Count{10, Evidence::Reported};
  u.total = Count{46, Evidence::Reported};
  u.provider_cost.total = UsdAmount{59100};
  CHECK(u.input_total_value() == 12 && u.output_total_value() == 34 && u.reasoning_value() == 10 && u.total_tokens_value() == 46 && u.cost_nano_usd_value() == 59100);

  Completion comp;
  comp.messages = {m_asst, rich};
  comp.usage = u;
  CHECK(comp.joined_text() == "answerpartA partB");
  CHECK(comp.joined_thinking() == "step1 step2");
  CHECK(comp.first_refusal() && *comp.first_refusal() == "blocked_1");
  CHECK(comp.executable_tool_calls().size() == 1 && comp.invalid_tool_calls().size() == 1 && comp.has_invalid_tool_calls());

  Outcome out_comp = comp;
  CHECK(is_completion(out_comp) && !is_failure(out_comp) && as_completion(out_comp) != nullptr && as_failure(out_comp) == nullptr && as_error(out_comp) == nullptr);
  CHECK(joined_text(out_comp) == "answerpartA partB");
  auto ptr_comp = std::make_shared<const Outcome>(out_comp);
  CHECK(is_completion(ptr_comp) && !is_failure(ptr_comp) && as_completion(ptr_comp) != nullptr && joined_text(ptr_comp) == "answerpartA partB");

  Failure fail{Error{ErrorKind::RemoteFailure, "boom"}, PartialCompletion{{m_asst}, u, {}}};
  CHECK(fail.partial.joined_text() == "answer");
  Outcome out_fail = fail;
  CHECK(!is_completion(out_fail) && is_failure(out_fail) && as_failure(out_fail) != nullptr && as_error(out_fail)->safe_message == "boom" && joined_text(out_fail).empty());
  std::shared_ptr<const Outcome> null_res;
  CHECK(!is_completion(null_res) && !is_failure(null_res) && as_completion(null_res) == nullptr && as_failure(null_res) == nullptr && as_error(null_res) == nullptr && joined_text(null_res).empty());
}
void simple_requests_all_families() {
  using namespace descriptor;
  const auto contains = [](const std::string& body, std::string_view needle) { return body.find(needle) != std::string::npos; };
  const auto history = [] { return std::vector<Message>{Message::user_text("hi")}; };
  // The system text and the output cap reach each family's own member. Outside Chat the prompt
  // and history forms write the same bytes.
  const auto chat_text = chat::encode(presets::openai_chat(), chat::Request::simple("gpt-4o-mini", "hi", 128, "be brief"), false);
  const auto chat_hist = chat::encode(presets::openai_chat(), chat::Request::simple("gpt-4o-mini", history(), 128, "be brief"), false);
  CHECK(std::holds_alternative<chat::EncodedRequest>(chat_text) && std::holds_alternative<chat::EncodedRequest>(chat_hist));
  const auto& chat_body = std::get<chat::EncodedRequest>(chat_text).body;
  // Chat's history form is the canonical path, which writes content as typed parts, not strings.
  const auto& chat_hist_body = std::get<chat::EncodedRequest>(chat_hist).body;
  CHECK(contains(chat_hist_body, R"("role":"system","content":[{"type":"text","text":"be brief"}])") &&
        contains(chat_hist_body, R"("role":"user","content":[{"type":"text","text":"hi"}])") &&
        contains(chat_hist_body, R"("max_completion_tokens":128)"));
  CHECK(contains(chat_body, R"("role":"system","content":"be brief")") && contains(chat_body, R"("role":"user","content":"hi")") && contains(chat_body, R"("max_completion_tokens":128)"));

  const auto msg_text = messages::encode(presets::anthropic_messages(), messages::Request::simple("claude-sonnet-4-5", "hi", 256, "be brief"), false);
  const auto msg_hist = messages::encode(presets::anthropic_messages(), messages::Request::simple("claude-sonnet-4-5", history(), 256, "be brief"), false);
  CHECK(std::holds_alternative<messages::EncodedRequest>(msg_text) && std::holds_alternative<messages::EncodedRequest>(msg_hist));
  const auto& msg_body = std::get<messages::EncodedRequest>(msg_text).body;
  CHECK(msg_body == std::get<messages::EncodedRequest>(msg_hist).body);
  CHECK(contains(msg_body, R"("system")") && contains(msg_body, "be brief") && contains(msg_body, R"("max_tokens":256)"));

  const auto resp_text = responses::encode(presets::openai_responses(), responses::Request::simple("gpt-4.1-mini", "hi", 256, "be brief"), false);
  const auto resp_hist = responses::encode(presets::openai_responses(), responses::Request::simple("gpt-4.1-mini", history(), 256, "be brief"), false);
  CHECK(std::holds_alternative<responses::EncodedRequest>(resp_text) && std::holds_alternative<responses::EncodedRequest>(resp_hist));
  const auto& resp_body = std::get<responses::EncodedRequest>(resp_text).body;
  CHECK(resp_body == std::get<responses::EncodedRequest>(resp_hist).body);
  CHECK(contains(resp_body, R"("instructions":"be brief")") && contains(resp_body, R"("max_output_tokens":256)"));

  const auto gem_text = gemini::encode(presets::google_generate("gemini-2.5-flash"), gemini::Request::simple("gemini-2.5-flash", "hi", 256, "be brief"), false);
  const auto gem_hist = gemini::encode(presets::google_generate("gemini-2.5-flash"), gemini::Request::simple("gemini-2.5-flash", history(), 256, "be brief"), false);
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gem_text) && std::holds_alternative<gemini::EncodedRequest>(gem_hist));
  const auto& gem_body = std::get<gemini::EncodedRequest>(gem_text).body;
  CHECK(gem_body == std::get<gemini::EncodedRequest>(gem_hist).body);
  CHECK(contains(gem_body, "be brief") && contains(gem_body, R"("maxOutputTokens":256)"));

  const auto int_text = interactions::encode(presets::google_interactions(), interactions::Request::simple("gemini-2.5-flash", "hi", 256, "be brief"), false);
  const auto int_hist = interactions::encode(presets::google_interactions(), interactions::Request::simple("gemini-2.5-flash", history(), 256, "be brief"), false);
  CHECK(std::holds_alternative<interactions::EncodedRequest>(int_text) && std::holds_alternative<interactions::EncodedRequest>(int_hist));
  const auto& int_body = std::get<interactions::EncodedRequest>(int_text).body;
  CHECK(int_body == std::get<interactions::EncodedRequest>(int_hist).body);
  CHECK(contains(int_body, R"("system_instruction":"be brief")") && contains(int_body, R"("max_output_tokens":256)"));

  // Without a system text or cap, neither member is written.
  const auto bare = chat::encode(presets::openai_chat(), chat::Request::simple("gpt-4o-mini", "hi"), false);
  CHECK(std::holds_alternative<chat::EncodedRequest>(bare));
  CHECK(!contains(std::get<chat::EncodedRequest>(bare).body, "system") && !contains(std::get<chat::EncodedRequest>(bare).body, "max_"));

  // The history form keeps sealed native history continuable and refuses an edited prefix.
  chat::Request turn1 = chat::Request::simple("fixture-model", std::vector<Message>{Message::user_text("hello")});
  const auto enc1 = chat::encode(descriptor_value(), turn1, false);
  CHECK(std::holds_alternative<chat::EncodedRequest>(enc1));
  Accumulator acc;
  chat::Codec codec(descriptor_value(), chat::Mode::Buffered, acc, {}, std::get<chat::EncodedRequest>(enc1).context);
  codec.buffered(body(R"({"role":"assistant","content":"answer"})"), {});
  CHECK(acc.outcome() && is_completion(*acc.outcome()));
  const auto sealed_msg = as_completion(*acc.outcome())->messages.at(0);
  chat::Request turn2 = chat::Request::simple("fixture-model", std::vector<Message>{Message::user_text("hello"), sealed_msg, Message::user_text("next")});
  CHECK(std::holds_alternative<chat::EncodedRequest>(chat::encode(descriptor_value(), turn2, false)));
  chat::Request edited = chat::Request::simple("fixture-model", std::vector<Message>{Message::user_text("HELLO"), sealed_msg, Message::user_text("next")});
  CHECK(std::get<Error>(chat::encode(descriptor_value(), edited, false)).kind == ErrorKind::ReplayIneligible);
}
} // namespace
int main() {
  try {
    idempotent_terminal_money_tail(); unlisted_usage_leaves_and_decimal_cost(); uncached_input_rule();
    owned_raw_observations(); raw_failure_evidence();
    chunk_partition_invariant(); no_terminal_no_success(); transport_projection_parity(); known_corrupt_never_ignored();
    invalid_tools_and_compatibility(); usage_knowledge(); accumulator_transitions_and_seals(); typed_request_encoding();
    required_metadata_and_identity(); per_chunk_creation_clock(); omitted_open_finish_reason(); empty_text_delta_is_withheld(); failure_reasons(); required_usage_and_unknown_cache(); unindexed_tool_order_and_optional_function(); buffered_close_precedence();
    accumulator_interleaved_ordering(); accumulator_order_uniqueness_and_limits();
    tool_history_and_escaped_arguments(); one_tool_message_with_several_results(); output_cap_member_by_origin(); quoted_json_and_request_bounds();
    gateway_request_controls(); gateway_max_price(); gateway_plugins_and_citations();
    value_builders_and_projections(); simple_requests_all_families();
    std::cout << "chat semantic properties passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
