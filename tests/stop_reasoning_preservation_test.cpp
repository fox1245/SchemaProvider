#include "codecs/chat.h"
#include "codecs/messages.h"
#include "codecs/messages_request.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include "transport/sse_framer.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
json::Document parse(std::string_view wire) {
  auto value = json::parse(wire);
  CHECK(std::holds_alternative<json::Document>(value));
  return std::get<json::Document>(std::move(value));
}
std::shared_ptr<const json::Document> owned(std::string_view wire) {
  return std::make_shared<const json::Document>(parse(wire));
}
descriptor::ValidatedDescriptor desc(std::string_view family, std::string_view origin = "https://openrouter.ai") {
  const auto path = family == "openai.chat" ? "/v1/chat/completions" : "/v1/messages";
  auto loaded = descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"stop-reasoning-proof\",\"family\":" +
      json::quote(family) + ",\"connection\":{\"base_url\":" + json::quote(origin) +
      ",\"paths\":{\"buffered\":" + json::quote(path) + ",\"streaming\":" + json::quote(path) + "}}}");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::string chunk(std::string_view delta, std::string_view reason = "null") {
  return "{\"id\":\"generation-1\",\"model\":\"fixture-model\",\"created\":7,\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":" +
      std::string(delta) + ",\"finish_reason\":" + std::string(reason) + "}]}";
}
std::string body(std::string_view message, std::string_view reason) {
  return "{\"id\":\"generation-1\",\"model\":\"fixture-model\",\"created\":7,\"object\":\"chat.completion\",\"choices\":[{\"index\":0,\"message\":" +
      std::string(message) + ",\"finish_reason\":" + std::string(reason) + "}]}";
}
std::string call(std::string_view arguments, bool streaming) {
  return "{" + std::string(streaming ? "\"index\":0," : "") +
      "\"id\":\"call-1\",\"type\":\"function\",\"function\":{\"name\":\"lookup\",\"arguments\":" + json::quote(arguments) + "}}";
}
struct Observed {
  Outcome outcome;
  std::vector<StopReason> stops;
  std::string text;
};
const Completion& completed(const Observed& observed) {
  CHECK(std::holds_alternative<Completion>(observed.outcome));
  return std::get<Completion>(observed.outcome);
}
Observed decode_chat(const descriptor::ValidatedDescriptor& descriptor, bool streaming,
                     const std::vector<std::string>& source,
                     std::shared_ptr<const NativeContext> context = {}) {
  std::vector<StopReason> stops;
  std::string text;
  Accumulator accumulator({}, [&](const Event& event) {
    if (const auto* stop = std::get_if<Stop>(&event)) {
      // This is the public runtime callback boundary, before final ownership.
      CHECK(!accumulator.outcome());
      stops.push_back(stop->reason);
    }
    if (const auto* delta = std::get_if<PartDelta>(&event); delta && delta->payload.kind == PartKind::Text)
      text.append(delta->payload.bytes);
  });
  chat::Codec codec(descriptor, streaming ? chat::Mode::Sse : chat::Mode::Buffered, accumulator, {}, std::move(context));
  if (streaming) {
    transport::SseFramer framer;
    const auto sink = [&](const transport::SseFrame& frame) { return codec.frame(frame.event, frame.data); };
    for (const auto& frame : source) {
      const std::string wire = "data: " + frame + "\r\n\r\n";
      // Fragment the actual SSE bytes, not just pre-decoded JSON events.
      for (size_t offset = 0; offset < wire.size(); offset += 7)
        CHECK(framer.feed(std::string_view(wire).substr(offset, 7), sink));
    }
    framer.finish();
    CHECK(framer.error() == transport::SseError::None);
    codec.finish();
  } else {
    CHECK(source.size() == 1);
    CHECK(codec.buffered(source.front(), {}));
  }
  CHECK(accumulator.outcome());
  return {*accumulator.outcome(), std::move(stops), std::move(text)};
}
void assert_stop(const Observed& observed, StopKind expected, std::string_view raw) {
  CHECK(observed.stops.size() == 1);
  CHECK(observed.stops.front().kind == expected && observed.stops.front().raw == raw);
  const auto& final = completed(observed);
  CHECK(final.stop.kind == expected && final.stop.raw == raw);
  CHECK(final.raw_events.back().payload->root().get("choices").at(0).get("finish_reason").as_string() == raw);
}
void client_call_stops() {
  const auto descriptor = desc("openai.chat");
  struct Case { std::string arguments, raw; StopKind stop; bool valid; InvalidReason invalid; };
  const std::vector<Case> cases{
      {R"({"x":1})", "stop", StopKind::ToolUse, true, InvalidReason::Other},
      {"{bad}", "stop", StopKind::ToolUse, false, InvalidReason::NotJson},
      {"", "stop", StopKind::ToolUse, false, InvalidReason::Empty},
      {R"({"x":1,"x":2})", "stop", StopKind::ToolUse, false, InvalidReason::DuplicateKey},
      {R"({"x":)", "length", StopKind::MaxTokens, false, InvalidReason::Truncated},
      {R"({"x":1})", "length", StopKind::MaxTokens, true, InvalidReason::Other},
      {"{bad}", "content_filter", StopKind::ContentFilter, false, InvalidReason::NotJson},
      {R"({"x":1})", "content_filter", StopKind::ContentFilter, true, InvalidReason::Other},
      {"{bad}", "future_terminal", StopKind::Unknown, false, InvalidReason::NotJson},
      {R"({"x":1})", "future_terminal", StopKind::Unknown, true, InvalidReason::Other}};
  for (const auto& test : cases) for (const bool streaming : {false, true}) {
    const auto reason = json::quote(test.raw);
    const auto first = "{\"role\":\"assistant\",\"content\":\"checking\",\"tool_calls\":[" + call(test.arguments, streaming) + "]}";
    const auto observed = decode_chat(descriptor, streaming,
        streaming ? std::vector<std::string>{chunk(first), chunk("{}", reason), "[DONE]"}
                  : std::vector<std::string>{body(first, reason)});
    assert_stop(observed, test.stop, test.raw);
    const auto& parts = completed(observed).messages.at(0).parts;
    CHECK(std::get<Text>(parts.at(0)).value == "checking");
    if (test.valid) {
      const auto& tool = std::get<ToolCall>(parts.at(1));
      CHECK(tool.kind == ToolCallKind::ClientExecuted && tool.id == "call-1" && tool.name == "lookup");
      CHECK(tool.input->root().get("x").as_uint() == 1);
    } else {
      const auto& tool = std::get<InvalidToolCall>(parts.at(1));
      CHECK(tool.kind == ToolCallKind::ClientExecuted && tool.id == "call-1" && tool.name == "lookup");
      CHECK(tool.raw_fragment == test.arguments && tool.reason == test.invalid);
    }
  }
  for (const bool streaming : {false, true}) {
    const auto observed = decode_chat(descriptor, streaming,
        streaming ? std::vector<std::string>{chunk(R"({"role":"assistant","content":"answer"})"), chunk("{}", "\"stop\""), "[DONE]"}
                  : std::vector<std::string>{body(R"({"role":"assistant","content":"answer"})", "\"stop\"")});
    assert_stop(observed, StopKind::EndTurn, "stop");
    CHECK(std::get<Text>(completed(observed).messages.at(0).parts.at(0)).value == "answer");
  }
}
void hosted_call_stops() {
  const auto descriptor = desc("anthropic.messages", "https://api.anthropic.com");
  messages::Request request;
  request.model = "fixture-model";
  request.account_scope = "fixture-account";
  request.messages.push_back({"", Role::User, {Text{"Hello"}}});
  const std::string server = R"({"type":"server_tool_use","id":"srv-1","name":"web_search","input":{"query":"x"}})";
  const std::string result = R"({"type":"web_search_tool_result","tool_use_id":"srv-1","content":[{"type":"web_search_result","title":"title","url":"https://example.test","encrypted_content":"opaque"}]})";
  const auto message = [](std::string_view content, std::string_view reason) {
    return "{\"type\":\"message\",\"id\":\"msg-1\",\"role\":\"assistant\",\"model\":\"fixture-model\",\"content\":" + std::string(content) +
        ",\"stop_reason\":" + std::string(reason) + ",\"usage\":{\"input_tokens\":2,\"output_tokens\":3}}";
  };
  for (const bool streaming : {false, true}) {
    const auto encoded = messages::encode(descriptor, request, streaming);
    CHECK(std::holds_alternative<messages::EncodedRequest>(encoded));
    std::vector<StopReason> stops;
    Accumulator accumulator({}, [&](const Event& event) {
      if (const auto* stop = std::get_if<Stop>(&event)) {
        CHECK(!accumulator.outcome());
        stops.push_back(stop->reason);
      }
    });
    messages::Codec codec(descriptor, streaming ? messages::Mode::Sse : messages::Mode::Buffered,
        accumulator, std::get<messages::EncodedRequest>(encoded).context);
    if (streaming) {
      CHECK(codec.frame("message_start", "{\"type\":\"message_start\",\"message\":" + message("[]", "null") + "}"));
      for (const auto& [index, block] : std::vector<std::pair<int, std::string>>{{0, server}, {1, result}}) {
        CHECK(codec.frame("content_block_start", "{\"type\":\"content_block_start\",\"index\":" + std::to_string(index) + ",\"content_block\":" + block + "}"));
        CHECK(codec.frame("content_block_stop", "{\"type\":\"content_block_stop\",\"index\":" + std::to_string(index) + "}"));
      }
      CHECK(codec.frame("message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":3}})"));
      CHECK(codec.frame("message_stop", R"({"type":"message_stop"})"));
      codec.finish();
    } else CHECK(codec.buffered(message("[" + server + "," + result + "]", "\"end_turn\""), {}));
    CHECK(accumulator.outcome());
    const auto& final = std::get<Completion>(*accumulator.outcome());
    CHECK(stops.size() == 1 && stops.front().kind == StopKind::EndTurn && stops.front().raw == "end_turn");
    CHECK(final.stop.kind == StopKind::EndTurn && final.stop.raw == "end_turn");
    const auto& parts = final.messages.at(0).parts;
    const auto& tool = std::get<ToolCall>(parts.at(0));
    CHECK(tool.kind == ToolCallKind::ServerExecuted && tool.id == "srv-1");
    const auto& output = std::get<ServerToolResult>(parts.at(1));
    CHECK(output.tool_use_id == tool.id && output.content->root().get("content").at(0).get("encrypted_content").as_string() == "opaque");
  }
}
void reasoning_fragments_and_native_replay() {
  const auto descriptor = desc("openai.chat");
  chat::Request request;
  request.model = "fixture-model";
  request.max_output_tokens = 128;
  request.canonical_messages.push_back({"", Role::User, {Text{"Hello"}}});
  request.tools.push_back({"lookup", "Find a value", owned(R"({"type":"object","properties":{"x":{"type":"integer"}}})")});
  const auto encoded = chat::encode(descriptor, request, true);
  CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
  const std::vector<std::string> details{
      R"([{"type":"reasoning.summary","summary":"A","format":"fmt-v1","index":0}])",
      R"([{"type":"reasoning.text","text":"B","format":"fmt-v1","index":1}])",
      R"([{"type":"reasoning.summary","summary":"C","format":"fmt-v1","index":0}])",
      R"([{"type":"reasoning.text","text":"Check ","id":"text-2","signature":"SIG","format":"fmt-v1","index":2}])",
      R"([{"type":"reasoning.text","text":"Distinct","id":"text-3","format":"fmt-v1","index":3}])",
      R"([{"type":"reasoning.text","text":"route","id":"text-2","signature":"SIG","format":"fmt-v1","index":2}])",
      R"([{"type":"reasoning.encrypted","data":"AB","id":"encrypted-1","format":"fmt-v1","index":4}])",
      R"([{"type":"reasoning.encrypted","data":"CD","id":"encrypted-1","format":"fmt-v1","index":4}])"};
  std::vector<std::string> frames;
  for (size_t index = 0; index < details.size(); ++index) {
    std::string delta = "{\"reasoning_details\":" + details[index];
    if (index == 0 || index == 2) delta += ",\"reasoning\":" + json::quote(index == 0 ? "Hello " : "world");
    frames.push_back(chunk(delta + "}"));
  }
  frames.push_back(chunk("{\"content\":\"answer\",\"tool_calls\":[" + call(R"({"x":1})", true) + "]}"));
  frames.push_back(chunk("{}", "\"stop\""));
  frames.push_back("[DONE]");
  const auto observed = decode_chat(descriptor, true, frames, std::get<chat::EncodedRequest>(encoded).context);
  assert_stop(observed, StopKind::ToolUse, "stop");
  CHECK(observed.text == "answer");
  const auto& native = completed(observed).messages.at(0);
  CHECK(native.native && native.native->complete());
  CHECK(std::get<Thinking>(native.parts.front()).text == "Hello world");
  const auto expected = parse(R"([{"type":"reasoning.summary","summary":"AC","format":"fmt-v1","index":0},{"type":"reasoning.text","text":"B","format":"fmt-v1","index":1},{"type":"reasoning.text","text":"Check route","id":"text-2","signature":"SIG","format":"fmt-v1","index":2},{"type":"reasoning.text","text":"Distinct","id":"text-3","format":"fmt-v1","index":3},{"type":"reasoning.encrypted","data":"AB","id":"encrypted-1","format":"fmt-v1","index":4},{"type":"reasoning.encrypted","data":"CD","id":"encrypted-1","format":"fmt-v1","index":4}])");
  size_t frame_index = 0, snapshot_index = 0;
  for (size_t index = 0; index < native.parts.size(); ++index) {
    if (const auto* opaque = std::get_if<Opaque>(&native.parts[index])) {
      if (opaque->wire_type == "reasoning_details.frame") {
        CHECK(frame_index < details.size());
        CHECK(json::equal(opaque->wire_metadata->root().get("details"), parse(details[frame_index]).root()));
        ++frame_index;
      } else {
        CHECK(opaque->wire_type == "reasoning_details");
        snapshot_index = index;
        CHECK(json::equal(opaque->wire_metadata->root().get("details"), expected.root()));
        const auto retained = opaque->wire_metadata->root().get("frames");
        CHECK(retained.size() == details.size());
        for (size_t frame = 0; frame < details.size(); ++frame)
          CHECK(json::equal(retained.at(frame), parse(details[frame]).root()));
      }
    }
  }
  CHECK(frame_index == details.size() && snapshot_index != 0);
  auto follow = request;
  follow.canonical_messages.push_back(native);
  follow.canonical_messages.push_back({"", Role::Tool, {ToolResult{"call-1", "found"}}});
  follow.canonical_messages.push_back({"", Role::User, {Text{"Continue"}}});
  const auto replay = chat::encode(descriptor, follow, false);
  CHECK(std::holds_alternative<chat::EncodedRequest>(replay));
  const auto wire = parse(std::get<chat::EncodedRequest>(replay).body);
  const auto assistant = wire.root().get("messages").at(1);
  CHECK(json::equal(assistant.get("reasoning_details"), expected.root()));
  CHECK(assistant.get("reasoning_content").as_string() == "Hello world");
  CHECK(assistant.get("content").at(0).get("text").as_string() == "answer");
  CHECK(assistant.get("tool_calls").at(0).get("id").as_string() == "call-1");
  const auto rejected = [&](const chat::Request& changed, const descriptor::ValidatedDescriptor& origin) {
    const auto result = chat::encode(origin, changed, false);
    CHECK(std::holds_alternative<Error>(result));
    CHECK(std::get<Error>(result).kind == ErrorKind::ReplayIneligible);
  };
  auto changed = follow;
  changed.canonical_messages[1].native.reset();
  rejected(changed, descriptor);
  changed = follow;
  std::get<Opaque>(changed.canonical_messages[1].parts[snapshot_index]).wire_metadata =
      owned(R"({"type":"reasoning_details","details":[{"type":"reasoning.encrypted","data":"ABCD","index":3}],"frames":[]})");
  rejected(changed, descriptor);
  changed = follow;
  std::get<Opaque>(changed.canonical_messages[1].parts[1]).wire_metadata =
      owned(R"({"type":"reasoning_details.frame","details":[{"type":"reasoning.summary","summary":"edited","index":0}]})");
  rejected(changed, descriptor);
  changed = follow;
  std::swap(changed.canonical_messages[1].parts[1], changed.canonical_messages[1].parts[2]);
  rejected(changed, descriptor);
  rejected(follow, desc("openai.chat", "https://api.openai.com"));
  frames.resize(details.size());
  const auto partial = decode_chat(descriptor, true, frames, std::get<chat::EncodedRequest>(encoded).context);
  CHECK(std::holds_alternative<Failure>(partial.outcome));
  const auto& failure = std::get<Failure>(partial.outcome);
  CHECK(failure.error.kind == ErrorKind::Truncated);
  changed = request;
  changed.canonical_messages.push_back(failure.partial.messages.at(0));
  rejected(changed, descriptor);
}
} // namespace
int main() {
  try {
    client_call_stops();
    hosted_call_stops();
    reasoning_fragments_and_native_replay();
    std::cout << "Stop and streamed reasoning preservation contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
