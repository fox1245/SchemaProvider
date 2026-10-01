#include "codecs/messages.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
messages::EncodedRequest encoded(const descriptor::ValidatedDescriptor& descriptor, const messages::Request& request) {
  auto result = messages::encode(descriptor, request, false);
  CHECK(std::holds_alternative<messages::EncodedRequest>(result));
  return std::get<messages::EncodedRequest>(std::move(result));
}
Outcome decode(const descriptor::ValidatedDescriptor& descriptor, const messages::EncodedRequest& wire,
               std::string_view content, std::string_view stop) {
  Accumulator accumulator;
  messages::Codec codec(descriptor, messages::Mode::Buffered, accumulator, wire.context);
  const auto body = R"({"id":"msg_lookup","type":"message","role":"assistant","model":"fixture-model","content":)" +
      std::string(content) + ",\"stop_reason\":" + json::quote(stop) +
      R"(,"stop_sequence":null,"usage":{"input_tokens":2,"output_tokens":3,"cache_read_input_tokens":0,"cache_creation_input_tokens":0}})";
  codec.buffered(body, {}); codec.finish();
  CHECK(accumulator.outcome());
  return *accumulator.outcome();
}
std::string call(std::string_view id) {
  return R"({"type":"server_tool_use","id":)" + json::quote(id) + R"(,"name":"web_search","input":{"query":"fixture"}})";
}
std::string result(std::string_view id) {
  return R"({"type":"web_search_tool_result","tool_use_id":)" + json::quote(id) + R"(,"content":[]})";
}
void pending_lookup_boundaries() {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"native-lookup","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  const auto& descriptor = std::get<descriptor::ValidatedDescriptor>(loaded);
  messages::Request request; request.model = "fixture-model";
  request.messages.push_back(Message{{}, Role::User, {Text{"Hello"}}});
  request.tools.push_back({"web_search", {}, {}, "web_search_20250305", {}});
  const auto initial = encoded(descriptor, request);
  CHECK(!initial.context->server_tool_name("pending-1000"));
  constexpr unsigned count = 128;
  auto id = [](unsigned index) { return "pending-" + std::to_string(1000 + 2 * index); };
  std::string calls = "[";
  for (unsigned i = count; i-- > 0;) {
    if (calls.size() > 1) calls += ',';
    calls += call(id(i));
  }
  calls += ']';
  auto paused = decode(descriptor, initial, calls, "pause_turn");
  CHECK(std::holds_alternative<Completion>(paused));
  auto& paused_message = std::get<Completion>(paused).messages.at(0);
  CHECK(paused_message.parts.size() == count && paused_message.native && paused_message.native->complete());
  request.messages.push_back(std::move(paused_message));
  const auto continuation = encoded(descriptor, request);
  for (unsigned i = 0; i < count; ++i)
    CHECK(continuation.context->server_tool_name(id(i)) == std::optional<std::string_view>("web_search"));
  const std::string missing[]{"-before", "pending-1001", "zz-after"};
  for (const auto& absent : missing) {
    CHECK(!continuation.context->server_tool_name(absent));
    const auto uncorrelated = decode(descriptor, continuation, "[" + result(absent) + "]", "end_turn");
    CHECK(std::holds_alternative<Failure>(uncorrelated));
    CHECK(std::get<Failure>(uncorrelated).error.kind == ErrorKind::ProtocolCorrupt);
  }
  for (unsigned i : {0U, count / 2, count - 1}) {
    const auto duplicate = decode(descriptor, continuation, "[" + call(id(i)) + "]", "pause_turn");
    CHECK(std::holds_alternative<Failure>(duplicate));
    CHECK(std::get<Failure>(duplicate).error.kind == ErrorKind::ProtocolCorrupt);
  }
  // New calls miss before, within, and after the sorted pending table. Prior-turn
  // results arrive in a permutation unrelated to call arrival or lexical order.
  std::string resumed = "[";
  for (const auto& absent : missing) resumed += call(absent) + ',';
  for (unsigned i = 0; i < count; ++i) resumed += result(id((i * 37) % count)) + ',';
  for (const auto& absent : missing) resumed += result(absent) + ',';
  resumed += R"({"type":"text","text":"done"}])";
  auto completed = decode(descriptor, continuation, resumed, "end_turn");
  CHECK(std::holds_alternative<Completion>(completed));
  auto& message = std::get<Completion>(completed).messages.at(0);
  CHECK(message.parts.size() == count + 7);
  for (unsigned i = 0; i < count; ++i)
    CHECK(std::get<ServerToolResult>(message.parts.at(i + 3)).tool_use_id == id((i * 37) % count));
  CHECK(std::get<Text>(message.parts.back()).value == "done");
  request.messages.push_back(std::move(message));
  request.messages.push_back(Message{{}, Role::User, {Text{"Next"}}});
  const auto cleared = encoded(descriptor, request);
  for (unsigned i = 0; i < count; ++i) CHECK(!cleared.context->server_tool_name(id(i)));
  for (const auto& absent : missing) CHECK(!cleared.context->server_tool_name(absent));
}
} // namespace
int main() {
  try {
    pending_lookup_boundaries();
    std::cout << "native continuation lookup properties passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
