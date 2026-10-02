#include "codecs/chat.h"
#include "codecs/messages.h"
#include "codecs/messages_request.h"
#include "codecs/responses.h"
#include "codecs/responses_request.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include "support/vision_fixture.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
const descriptor::ValidatedDescriptor& desc(std::string_view family) {
  static auto chat = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"vision-chat","family":"openai.chat","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}})");
  static auto messages = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"vision-messages","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}}})");
  static auto responses = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"vision-responses","family":"openai.responses","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/responses","streaming":"/v1/responses"}},"bindings":{"model":"model","messages":"input","stream":"stream","max_output_tokens":"max_output_tokens","usage":["usage"]}})");
  const auto& loaded = family == "chat" ? chat : family == "messages" ? messages : responses;
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(loaded);
}
json::Document parse(std::string_view bytes) {
  auto parsed = json::parse(bytes, {16U << 20, 64});
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::get<json::Document>(std::move(parsed));
}
template<class Encoded, class Result> Encoded take(Result result) {
  CHECK(std::holds_alternative<Encoded>(result));
  return std::get<Encoded>(std::move(result));
}
template<class Result> void error(Result result, ErrorKind kind) {
  CHECK(std::holds_alternative<Error>(result));
  CHECK(std::get<Error>(result).kind == kind);
}
chat::Request chat_request(const Image& image) {
  chat::Request r; r.model = "fixture-model";
  chat::InputMessage user; user.role = Role::User; user.images.push_back(image);
  r.messages.push_back(std::move(user)); return r;
}
messages::Request messages_request(const Image& image) {
  messages::Request r; r.model = "fixture-model"; r.account_scope = "fixture-account";
  r.messages.push_back(Message{{}, Role::User, {image}}); return r;
}
responses::Request responses_request(const Image& image) {
  responses::Request r; r.model = "fixture-model"; r.account_scope = "fixture-account";
  r.messages.push_back(Message{{}, Role::User, {image}}); return r;
}
void reject_all(const Image& image) {
  error(chat::encode(desc("chat"), chat_request(image), false), ErrorKind::InvalidRequest);
  error(messages::encode(desc("messages"), messages_request(image), false), ErrorKind::InvalidRequest);
  error(responses::encode(desc("responses"), responses_request(image), false), ErrorKind::InvalidRequest);
}
// Structural admission deliberately does not promise a pixel decoder. Generate
// canonical base64 with PNG magic and zero padding to isolate decoded-byte bounds.
Image boundary_image(size_t bytes) {
  constexpr std::array<unsigned char, 8> magic{137,80,78,71,13,10,26,10};
  constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  auto data = std::make_shared<std::string>(); data->reserve(((bytes + 2) / 3) * 4);
  auto at = [&](size_t i) -> unsigned { return i < magic.size() ? magic[i] : 0; };
  for (size_t i = 0; i < bytes; i += 3) {
    const unsigned n = (at(i) << 16) | (i + 1 < bytes ? at(i + 1) << 8 : 0) | (i + 2 < bytes ? at(i + 2) : 0);
    data->push_back(alphabet[(n >> 18) & 63]); data->push_back(alphabet[(n >> 12) & 63]);
    data->push_back(i + 1 < bytes ? alphabet[(n >> 6) & 63] : '=');
    data->push_back(i + 2 < bytes ? alphabet[n & 63] : '=');
  }
  return Image{"image/png", std::move(data)};
}
void admission_and_budgets() {
  const auto fixture = vision_test::scene_a().image();
  auto invalid = fixture; invalid.mime = "image/svg+xml"; reject_all(invalid);
  invalid = fixture; invalid.mime = "image/jpeg"; reject_all(invalid);
  invalid = fixture; invalid.mime = "image/png\""; reject_all(invalid);
  invalid = fixture; invalid.data.reset(); reject_all(invalid);
  invalid = fixture; invalid.detail = static_cast<ImageDetail>(255); reject_all(invalid);
  for (const std::string malformed : {"", "AAAA", "iVBORw0KGgo", "iVBORw0KGgo=\n", "iVBORw0KGgo?", "iV=ORw0KGgo=", "iVBORw0KGgp="}) {
    invalid = fixture; invalid.data = std::make_shared<const std::string>(malformed); reject_all(invalid);
  }
  const auto at_limit = boundary_image(5U << 20);
  CHECK(valid_image(at_limit));
  for (bool streaming : {false, true}) {
    take<chat::EncodedRequest>(chat::encode(desc("chat"), chat_request(at_limit), streaming));
    take<messages::EncodedRequest>(messages::encode(desc("messages"), messages_request(at_limit), streaming));
    take<responses::EncodedRequest>(responses::encode(desc("responses"), responses_request(at_limit), streaming));
  }
  reject_all(boundary_image((5U << 20) + 1));
  // Each image is individually admitted; the complete request still has one
  // aggregate 16 MiB budget across all parts, not a budget per image.
  auto c = chat_request(at_limit); c.messages[0].images.assign(3, at_limit);
  CHECK(std::holds_alternative<Error>(chat::encode(desc("chat"), c, false)));
  auto m = messages_request(at_limit); m.messages[0].parts = {at_limit, at_limit, at_limit};
  CHECK(std::holds_alternative<Error>(messages::encode(desc("messages"), m, false)));
  auto r = responses_request(at_limit); r.messages[0].parts = {at_limit, at_limit, at_limit};
  error(responses::encode(desc("responses"), r, false), ErrorKind::ResourceLimit);
}
void role_and_control_admission() {
  const auto image = vision_test::scene_a().image();
  for (const auto role : {Role::System, Role::Developer, Role::Assistant, Role::Tool, static_cast<Role>(255)}) {
    auto c = chat_request(image); c.messages[0].role = role;
    CHECK(std::holds_alternative<Error>(chat::encode(desc("chat"), c, false)));
    auto m = messages_request(image); m.messages[0].role = role;
    CHECK(std::holds_alternative<Error>(messages::encode(desc("messages"), m, false)));
    auto r = responses_request(image); r.messages[0].role = role;
    CHECK(std::holds_alternative<Error>(responses::encode(desc("responses"), r, false)));
  }
  for (auto detail : {ImageDetail::Low, ImageDetail::High, ImageDetail::Original}) {
    auto controlled = image; controlled.detail = detail;
    error(messages::encode(desc("messages"), messages_request(controlled), false), ErrorKind::Unsupported);
    take<chat::EncodedRequest>(chat::encode(desc("chat"), chat_request(controlled), false));
    take<responses::EncodedRequest>(responses::encode(desc("responses"), responses_request(controlled), false));
  }
  for (const std::string effort : {"", "minimal", "auto", "LOW", "none\""}) {
    auto c = chat_request(image); c.reasoning_effort = effort;
    error(chat::encode(desc("chat"), c, false), ErrorKind::InvalidRequest);
  }
  auto m = messages_request(image); m.messages.push_back(Message{{}, Role::Assistant, {Thinking{"unsealed", "forged"}}});
  error(messages::encode(desc("messages"), m, false), ErrorKind::ReplayIneligible);
  auto r = responses_request(image); r.messages.push_back(Message{{}, Role::Assistant, {Text{"unsealed"}}});
  error(responses::encode(desc("responses"), r, false), ErrorKind::ReplayIneligible);
}
void semantic_order_and_integrity() {
  const auto a = vision_test::scene_a().image(), b = vision_test::scene_b().image();
  for (bool streaming : {false, true}) {
    auto c = chat_request(a); c.messages[0].images.push_back(b); c.messages[0].text = std::string(vision_test::question);
    c.reasoning_effort = "max";
    auto cw = take<chat::EncodedRequest>(chat::encode(desc("chat"), c, streaming)); auto cd = parse(cw.body);
    const auto content = cd.root().get("messages").at(0).get("content");
    CHECK(content.size() == 3);
    CHECK(content.at(0).get("image_url").get("url").as_string() == "data:image/png;base64," + *a.data);
    CHECK(content.at(1).get("image_url").get("url").as_string() == "data:image/png;base64," + *b.data);
    CHECK(content.at(2).get("text").as_string() == vision_test::question);
    auto m = messages_request(a); m.messages[0].parts = {Text{"before"}, a, Text{"between"}, b, Text{"after"}};
    auto mw = take<messages::EncodedRequest>(messages::encode(desc("messages"), m, streaming)); auto md = parse(mw.body);
    const auto blocks = md.root().get("messages").at(0).get("content");
    CHECK(blocks.size() == 5 && blocks.at(0).get("text").as_string() == "before" && blocks.at(2).get("text").as_string() == "between" && blocks.at(4).get("text").as_string() == "after");
    CHECK(blocks.at(1).get("source").get("data").as_string() == *a.data);
    CHECK(blocks.at(3).get("source").get("data").as_string() == *b.data);
    auto r = responses_request(a); r.messages[0].parts = m.messages[0].parts;
    auto rw = take<responses::EncodedRequest>(responses::encode(desc("responses"), r, streaming)); auto rd = parse(rw.body);
    const auto parts = rd.root().get("input").at(0).get("content");
    CHECK(parts.size() == 5 && parts.at(0).get("text").as_string() == "before" && parts.at(2).get("text").as_string() == "between" && parts.at(4).get("text").as_string() == "after");
    CHECK(parts.at(1).get("image_url").as_string() == "data:image/png;base64," + *a.data);
    CHECK(parts.at(3).get("image_url").as_string() == "data:image/png;base64," + *b.data);
  }
  // Caller-local owners vanish, but retained typed input and its encoded output
  // keep exact bytes. The independent peer suite additionally decodes PNG pixels.
  std::weak_ptr<const std::string> weak; responses::Request retained;
  {
    auto local = std::make_shared<const std::string>(*a.data); weak = local;
    Image image{"image/png", local}; retained = responses_request(image);
  }
  CHECK(!weak.expired());
  auto wire = take<responses::EncodedRequest>(responses::encode(desc("responses"), retained, false));
  retained.messages.clear(); CHECK(weak.expired());
  auto doc = parse(wire.body);
  CHECK(doc.root().get("input").at(0).get("content").at(0).get("image_url").as_string() == "data:image/png;base64," + *a.data);
}
Message capture_messages(const messages::EncodedRequest& wire) {
  Accumulator accumulator;
  messages::Codec codec(desc("messages"), messages::Mode::Buffered, accumulator, wire.context);
  codec.buffered(R"({"id":"msg_1","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"text","text":"done"}],"stop_reason":"end_turn","stop_sequence":null,"usage":{"input_tokens":2,"output_tokens":1}})", {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  auto message = std::get<Completion>(*accumulator.outcome()).messages.at(0);
  CHECK(message.native && message.native->complete()); return message;
}
Message capture_responses(const responses::EncodedRequest& wire) {
  Accumulator accumulator;
  responses::Codec codec(desc("responses"), responses::Mode::Buffered, accumulator, wire.context);
  codec.buffered(R"({"id":"resp_1","object":"response","created_at":1,"model":"fixture-model","status":"completed","output":[{"id":"msg_1","type":"message","status":"completed","role":"assistant","content":[{"type":"output_text","text":"done","annotations":[]}]}],"usage":{"input_tokens":2,"output_tokens":1,"total_tokens":3},"incomplete_details":null,"error":null})", {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  auto message = std::get<Completion>(*accumulator.outcome()).messages.at(0);
  CHECK(message.native && message.native->complete()); return message;
}
void prefix_mutation(Message& message, unsigned mutation) {
  auto& image = std::get<Image>(message.parts.at(0));
  switch (mutation) {
    case 0: image.data = vision_test::scene_b().base64; break;
    case 1: image.mime = "image/jpeg"; break;
    case 2: image.detail = ImageDetail::Low; break;
    case 3: std::swap(message.parts[0], message.parts[1]); break;
    case 4: message.parts.erase(message.parts.begin()); break;
    case 5: message.parts.push_back(vision_test::scene_b().image()); break;
  }
}
void exact_native_image_prefix_binding() {
  const auto image = vision_test::scene_a().image();
  auto m = messages_request(image); m.messages[0].parts.push_back(Text{"question"});
  auto mw = take<messages::EncodedRequest>(messages::encode(desc("messages"), m, false));
  m.messages.push_back(capture_messages(mw)); m.messages.push_back(Message{{}, Role::User, {Text{"next"}}});
  take<messages::EncodedRequest>(messages::encode(desc("messages"), m, false));
  auto r = responses_request(image); r.messages[0].parts.push_back(Text{"question"});
  auto rw = take<responses::EncodedRequest>(responses::encode(desc("responses"), r, false));
  r.messages.push_back(capture_responses(rw)); r.messages.push_back(Message{{}, Role::User, {Text{"next"}}});
  take<responses::EncodedRequest>(responses::encode(desc("responses"), r, false));
  for (unsigned mutation = 0; mutation != 6; ++mutation) {
    auto changed_m = m; prefix_mutation(changed_m.messages[0], mutation);
    error(messages::encode(desc("messages"), changed_m, false), ErrorKind::ReplayIneligible);
    auto changed_r = r; prefix_mutation(changed_r.messages[0], mutation);
    error(responses::encode(desc("responses"), changed_r, false), ErrorKind::ReplayIneligible);
  }
  // Replacing immutable ownership with equal bytes must not change lineage.
  auto same_m = m; std::get<Image>(same_m.messages[0].parts[0]).data = std::make_shared<const std::string>(*image.data);
  take<messages::EncodedRequest>(messages::encode(desc("messages"), same_m, false));
  auto same_r = r; std::get<Image>(same_r.messages[0].parts[0]).data = std::make_shared<const std::string>(*image.data);
  take<responses::EncodedRequest>(responses::encode(desc("responses"), same_r, false));
}
} // namespace
int main() {
  try {
    admission_and_budgets(); role_and_control_admission();
    semantic_order_and_integrity(); exact_native_image_prefix_binding();
    std::cout << "vision input admission, ordering, ownership and lineage properties passed\n";
    return 0;
  } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
