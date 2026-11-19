// Pins the values that sealed history depends on: the replay identity of the built-in policy, the native origin
// digest and the archive descriptor binding of every descriptor preset, and one Chat configuration digest.
// A sealed message and a native archive carry these values, so a change that moves one makes what was sealed
// before it unusable. Moving a pin must be a deliberate, documented change; a data edit that only changes
// admission (models, MIME types, limits, levels) must leave every pin where it is.
#include "codecs/accumulator.h"
#include "codecs/chat.h"
#include "codecs/gemini.h"
#include "codecs/interactions.h"
#include "codecs/messages.h"
#include "codecs/responses.h"
#include "core/native.h"
#include "core/native_archive_custody_internal.h"
#include "core/native_testing.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "descriptor/presets.h"

#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

namespace descriptor = sp::descriptor;
namespace presets = sp::descriptor::presets;
namespace archive = sp::archive_internal;

// One line per moved value, naming it with the pinned and the computed hex.
void pin(std::string_view name, std::string_view pinned, const std::string& computed) {
  if (computed == pinned) return;
  ++failures;
  std::fprintf(stderr, "PIN MOVED %.*s: pinned \"%.*s\" computed \"%s\"\n", static_cast<int>(name.size()), name.data(),
               static_cast<int>(pinned.size()), pinned.data(), computed.c_str());
}
// The descriptor a preset loads, or a counted failure that names the preset.
const descriptor::ValidatedDescriptor* loaded(std::string_view name, const descriptor::LoadResult& result) {
  const auto* value = std::get_if<descriptor::ValidatedDescriptor>(&result);
  if (!value) {
    ++failures;
    std::fprintf(stderr, "PRESET NOT LOADED %.*s: %s\n", static_cast<int>(name.size()), name.data(),
                 std::get<descriptor::ConfigError>(result).message.c_str());
  }
  return value;
}
std::string origin_hex(const descriptor::ValidatedDescriptor* value) {
  if (!value) return "<not loaded>";
  const auto digest = sp::native_testing::origin_digest_of(*value);
  return digest ? archive::hex(*digest) : "<no origin digest>";
}
std::string binding_hex(const descriptor::ValidatedDescriptor* value) {
  if (!value) return "<not loaded>";
  try {
    const auto limits = archive::resolve_custody_limits({}, *value).archive;
    return archive::hex(archive::descriptor_binding_digest(*value, limits));
  } catch (const sp::archive_fs::Rejected&) {
    return "<binding rejected>";
  }
}

void builtin_policy_identity() {
  const auto policy = descriptor::builtin_policy();
  CHECK(policy != nullptr);
  if (!policy) return;
  // Moving it makes history of every family sealed under the built-in policy ReplayIneligible.
  pin("builtin_policy.identity", "7497073d527aa8ccf2f1c10ab8ecea999da7d19e645e5b45ed0e713f705e1641", policy->identity_hex());
  // The content digest also covers admission data and moves with every data edit, so it is not pinned.
  CHECK(!policy->content_digest_hex().empty());
  CHECK(policy->content_digest_hex() != policy->identity_hex());
}

void preset_origins_and_bindings() {
  const auto openai_chat_result = presets::try_openai_chat();
  const auto* openai_chat = loaded("openai_chat", openai_chat_result);
  // Moving it makes Chat history sealed through presets::openai_chat() ReplayIneligible.
  pin("openai_chat.origin", "b3a7cd4d5a9f8d237737e7187b761b088ac3c9edbe319c3aed37a512ae6fa35b", origin_hex(openai_chat));
  // Moving it makes native archives written for presets::openai_chat() stop matching the descriptor.
  pin("openai_chat.archive_binding", "b8eb39201aef5af9c728afd5a418bffae32c6bc0c2803bca313c1c1dda8bbfe3", binding_hex(openai_chat));

  const auto openai_responses_result = presets::try_openai_responses();
  const auto* openai_responses = loaded("openai_responses", openai_responses_result);
  // Moving it makes Responses history sealed through presets::openai_responses() ReplayIneligible.
  pin("openai_responses.origin", "d31f748095815272c317616dc98b6225b4e9522b83de0d4900e8e3587aed70c6", origin_hex(openai_responses));
  // Moving it makes native archives written for presets::openai_responses() stop matching the descriptor.
  pin("openai_responses.archive_binding", "210f0a8da3b1ff2cbc9e94a2101b8ef2a0dd3775450e3129de1d60c6096fbfbd", binding_hex(openai_responses));

  const auto anthropic_messages_result = presets::try_anthropic_messages();
  const auto* anthropic_messages = loaded("anthropic_messages", anthropic_messages_result);
  // Moving it makes Messages history sealed through presets::anthropic_messages() ReplayIneligible.
  pin("anthropic_messages.origin", "1369646587e0c49d52f937b0a9dac004b63453b25045685f85c3248f13352b7f", origin_hex(anthropic_messages));
  // Moving it makes native archives written for presets::anthropic_messages() stop matching the descriptor.
  pin("anthropic_messages.archive_binding", "b86d8570dbc9628d997bba32ea2824da52abc6bb39f22e0058a0b91cf1a064a0", binding_hex(anthropic_messages));

  const auto google_generate_routed_result = presets::try_google_generate();
  const auto* google_generate_routed = loaded("google_generate_routed", google_generate_routed_result);
  // Moving it makes Generate history sealed through the model-routed presets::google_generate() ReplayIneligible.
  pin("google_generate_routed.origin", "32c10d13950ef6ce9f030093918ce74d46391c66183b62f794bb47a5ac28920e", origin_hex(google_generate_routed));
  // Moving it makes native archives written for the model-routed presets::google_generate() stop matching it.
  pin("google_generate_routed.archive_binding", "c9fd48ef451c060feef5d1ea14829e02dc6158438b1116b12fda7507e6b98d9d", binding_hex(google_generate_routed));

  const auto google_generate_literal_result = presets::try_google_generate("gemini-2.5-flash");
  const auto* google_generate_literal = loaded("google_generate_literal", google_generate_literal_result);
  // Moving it makes Generate history sealed through a literal-model presets::google_generate(model) ReplayIneligible.
  pin("google_generate_literal.origin", "e3a58583f0c43d433655dac7e107d9ea4360f66e56bc460a968d38d8131fffba", origin_hex(google_generate_literal));
  // Moving it makes native archives written for a literal-model presets::google_generate(model) stop matching it.
  pin("google_generate_literal.archive_binding", "f45e9563b4d959b3e471e6593048849beb307c38298db0a2a50e952c78572b7b", binding_hex(google_generate_literal));

  const auto google_interactions_result = presets::try_google_interactions();
  const auto* google_interactions = loaded("google_interactions", google_interactions_result);
  // Moving it makes Interactions history sealed through presets::google_interactions() ReplayIneligible.
  pin("google_interactions.origin", "c16784633aaf4ca14c98519c1bea9dd98aea4b59becdab41c67e1014cacda63a", origin_hex(google_interactions));
  // Moving it makes native archives written for presets::google_interactions() stop matching the descriptor.
  pin("google_interactions.archive_binding", "921344e650b17b2ce78b226c6e6f4d119d9ae1fe1a1ab2e77517b4cfc35df448", binding_hex(google_interactions));

  const auto openrouter_chat_result = presets::try_openrouter_chat();
  const auto* openrouter_chat = loaded("openrouter_chat", openrouter_chat_result);
  // Moving it makes Chat history sealed through presets::openrouter_chat() ReplayIneligible.
  pin("openrouter_chat.origin", "b08080a4a008a2b0dcb64075383432de713057c48aacf23c90705d365510311d", origin_hex(openrouter_chat));
  // Moving it makes native archives written for presets::openrouter_chat() stop matching the descriptor.
  pin("openrouter_chat.archive_binding", "29c45ed29b11c14b814b44832bb705e550d7bcf88ecacc9d32bbe9eaffa011ea", binding_hex(openrouter_chat));

  const auto openrouter_responses_result = presets::try_openrouter_responses();
  const auto* openrouter_responses = loaded("openrouter_responses", openrouter_responses_result);
  // Moving it makes Responses history sealed through presets::openrouter_responses() ReplayIneligible.
  pin("openrouter_responses.origin", "81b3687c27a1779338c484bddee6343581b555057b75c5de6644c02f687daf08", origin_hex(openrouter_responses));
  // Moving it makes native archives written for presets::openrouter_responses() stop matching the descriptor.
  pin("openrouter_responses.archive_binding", "32ad39490d014ec66ae5ed2aa789150010c6c6fca96ddbfe9fecf73aaa400d52", binding_hex(openrouter_responses));

  const auto openrouter_messages_result = presets::try_openrouter_messages();
  const auto* openrouter_messages = loaded("openrouter_messages", openrouter_messages_result);
  // Moving it makes Messages history sealed through presets::openrouter_messages() ReplayIneligible.
  pin("openrouter_messages.origin", "fbb75c7f80876fe87631c3451c5d9cb5598073c4c59063ef183c73fc9a577cdd", origin_hex(openrouter_messages));
  // Moving it makes native archives written for presets::openrouter_messages() stop matching the descriptor.
  pin("openrouter_messages.archive_binding", "4fff18038bcb2608a23bc5c571623280a29d1bc5d7e92cd761f07d6961dbf8fd", binding_hex(openrouter_messages));

  const auto loopback_chat_result = presets::try_loopback_chat();
  const auto* loopback_chat = loaded("loopback_chat", loopback_chat_result);
  // Moving it makes Chat history sealed through presets::loopback_chat() ReplayIneligible.
  pin("loopback_chat.origin", "3408aed90a07e71cf2146b6f301c4f1b9b046fa0dc6896cb4e8625a19cac0824", origin_hex(loopback_chat));
  // Moving it makes native archives written for presets::loopback_chat() stop matching the descriptor.
  pin("loopback_chat.archive_binding", "0f1103871022d523db97bebcbfac77fd34210dd50bb28e9aae090880bf088cd5", binding_hex(loopback_chat));
}

void chat_audio_configuration() {
  const auto result = presets::try_openai_chat();
  const auto* chat = loaded("openai_chat", result);
  if (!chat) return;
  sp::chat::Request request;
  request.model = "fixture-model";
  request.messages.push_back({sp::Role::User, "speak"});
  request.modalities = {sp::chat::OutputModality::Text, sp::chat::OutputModality::Audio};
  request.audio = sp::chat::AudioOutput{"alloy", "pcm16"};
  // The request is one the encoder admits, and its context binds this descriptor's origin.
  const auto encoded = sp::chat::encode(*chat, request, false);
  const auto* admitted = std::get_if<sp::chat::EncodedRequest>(&encoded);
  CHECK(admitted && admitted->context && admitted->context->matches_descriptor(*chat));
  // No control is set on the request, so the encoder binds the model's effective defaults as they are.
  const auto choices = descriptor::effective_defaults(*chat, request.model);
  const auto digest = sp::native_testing::chat_configuration_digest_of(*chat, request, choices);
  const std::string computed = digest ? archive::hex(*digest) : "<no configuration digest>";
  // Moving it makes Chat history sealed with an audio output configuration ReplayIneligible.
  pin("openai_chat.audio_configuration", "057315db972f9d2e706c5fab62aaee57d5e65cdf81d24950945636dc798eaf04", computed);
}

// The route a native context binds is text that a captured answer carries: a later turn replays the answer only when
// its own route is the same text, and a native archive stores it. Each turn below is encoded, its answer captured
// through the family's codec, and the next turn sends the answer back.
struct Turns {
  std::string first, next;
  bool captured = false, replayed = false;
};
std::vector<sp::Message> opening() { return {sp::Message::user_text("hello")}; }
std::optional<sp::Message> answer_of(const sp::Accumulator& accumulator) {
  if (!accumulator.outcome() || !std::holds_alternative<sp::Completion>(*accumulator.outcome())) return std::nullopt;
  const auto& messages = std::get<sp::Completion>(*accumulator.outcome()).messages;
  if (messages.empty() || !messages.front().native || !messages.front().native->complete()) return std::nullopt;
  return messages.front();
}
// `encode` is the family's encoder and `decode` feeds the family's codec the answer.
template <class Request, class Encoded, class Encode, class Decode>
Turns turns(const descriptor::ValidatedDescriptor& d, std::string_view model, bool streaming, Encode encode, Decode decode) {
  Turns out;
  const auto first = encode(d, Request::simple(std::string(model), opening(), 64), streaming);
  const auto* wire = std::get_if<Encoded>(&first);
  if (!wire || !wire->context) return out;
  out.first = std::string(sp::native_testing::ContextAccess::route(*wire->context));
  sp::Accumulator accumulator;
  decode(d, accumulator, wire->context);
  const auto answer = answer_of(accumulator);
  if (!answer) return out;
  out.captured = true;
  auto history = opening();
  history.push_back(*answer);
  history.push_back(sp::Message::user_text("next"));
  const auto next = encode(d, Request::simple(std::string(model), std::move(history), 64), streaming);
  if (const auto* again = std::get_if<Encoded>(&next); again && again->context) {
    out.next = std::string(sp::native_testing::ContextAccess::route(*again->context));
    out.replayed = true;
  }
  return out;
}
using Context = std::shared_ptr<const sp::NativeContext>;
Turns family_turns(const descriptor::ValidatedDescriptor& d, std::string_view model, bool streaming) {
  const auto family = d.family();
  if (family == "openai.chat")
    return turns<sp::chat::Request, sp::chat::EncodedRequest>(d, model, streaming, [](const auto& value, const auto& request, bool mode) { return sp::chat::encode(value, request, mode); },
        [&](const auto& descriptor, sp::Accumulator& accumulator, const Context& context) {
          sp::chat::Codec codec(descriptor, sp::chat::Mode::Buffered, accumulator, {}, context);
          codec.buffered("{\"id\":\"generation-1\",\"model\":" + sp::json::quote(model) +
                         R"(,"created":7,"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"answer"},"finish_reason":"stop"}],"usage":null})", {});
          codec.finish();
        });
  if (family == "anthropic.messages")
    return turns<sp::messages::Request, sp::messages::EncodedRequest>(d, model, streaming, [](const auto& value, const auto& request, bool mode) { return sp::messages::encode(value, request, mode); },
        [&](const auto& descriptor, sp::Accumulator& accumulator, const Context& context) {
          sp::messages::Codec codec(descriptor, sp::messages::Mode::Buffered, accumulator, context);
          codec.buffered("{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":" + sp::json::quote(model) +
                         R"(,"content":[{"type":"text","text":"answer"}],"stop_reason":"end_turn","stop_sequence":null,"usage":{"input_tokens":2,"output_tokens":1}})", {});
          codec.finish();
        });
  if (family == "openai.responses")
    return turns<sp::responses::Request, sp::responses::EncodedRequest>(d, model, streaming, [](const auto& value, const auto& request, bool mode) { return sp::responses::encode(value, request, mode); },
        [&](const auto& descriptor, sp::Accumulator& accumulator, const Context& context) {
          sp::responses::Codec codec(descriptor, sp::responses::Mode::Buffered, accumulator, context);
          codec.buffered("{\"id\":\"resp_1\",\"object\":\"response\",\"created_at\":1,\"model\":" + sp::json::quote(model) +
                         R"(,"status":"completed","output":[{"id":"msg_1","type":"message","status":"completed","role":"assistant","content":[{"type":"output_text","text":"answer","annotations":[]}]}],"usage":{"input_tokens":2,"output_tokens":1,"total_tokens":3},"incomplete_details":null,"error":null})", {});
          codec.finish();
        });
  if (family == "google.generate")
    return turns<sp::gemini::Request, sp::gemini::EncodedRequest>(d, model, streaming, [](const auto& value, const auto& request, bool mode) { return sp::gemini::encode(value, request, mode); },
        [&](const auto& descriptor, sp::Accumulator& accumulator, const Context& context) {
          sp::gemini::Codec codec(descriptor, sp::gemini::Mode::Buffered, accumulator, context);
          codec.buffered("{\"modelVersion\":" + sp::json::quote(model) +
                         R"(,"responseId":"g1","candidates":[{"index":0,"content":{"role":"model","parts":[{"text":"answer"}]},"finishReason":"STOP"}]})", {});
          codec.finish();
        });
  if (family == "google.interactions")
    return turns<sp::interactions::Request, sp::interactions::EncodedRequest>(d, model, streaming, [](const auto& value, const auto& request, bool mode) { return sp::interactions::encode(value, request, mode); },
        [&](const auto& descriptor, sp::Accumulator& accumulator, const Context& context) {
          sp::interactions::Codec codec(descriptor, sp::interactions::Mode::Buffered, accumulator, context);
          codec.buffered("{\"id\":\"i1\",\"model\":" + sp::json::quote(model) +
                         R"(,"status":"completed","steps":[{"type":"model_output","content":[{"type":"text","text":"answer"}]}]})", {});
          codec.finish();
        });
  return {};
}
// The route a turn binds, in both transport modes, and the same route on the turn that replays its answer.
void route(std::string_view name, const descriptor::LoadResult& result, std::string_view model, std::string_view pinned) {
  const auto* value = loaded(name, result);
  if (!value) return;
  for (const bool streaming : {false, true}) {
    const auto turn = family_turns(*value, model, streaming);
    const std::string mode = std::string(name) + (streaming ? ".streamed" : ".buffered");
    // Moving it makes every answer captured on this descriptor stop replaying, and its archives stop restoring.
    pin(mode + ".route", pinned, turn.first);
    if (!turn.captured) { ++failures; std::fprintf(stderr, "NOT CAPTURED %s\n", mode.c_str()); continue; }
    if (!turn.replayed) { ++failures; std::fprintf(stderr, "NOT REPLAYED %s\n", mode.c_str()); continue; }
    pin(mode + ".replayed_route", pinned, turn.next);
  }
}
descriptor::LoadResult loopback(std::string_view id, std::string_view family, std::string_view buffered, std::string_view streaming) {
  return descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":" + sp::json::quote(id) + ",\"family\":" + sp::json::quote(family) +
                          R"(,"connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":)" + sp::json::quote(buffered) +
                          ",\"streaming\":" + sp::json::quote(streaming) + "}}}");
}
void native_routes() {
  route("openai_chat", presets::try_openai_chat(), "fixture-model", "/v1/chat/completions");
  route("openai_responses", presets::try_openai_responses(), "fixture-model", "/v1/responses");
  route("anthropic_messages", presets::try_anthropic_messages(), "fixture-model", "/v1/messages");
  route("google_generate_routed", presets::try_google_generate(), "fixture-model", "/v1beta/models/fixture-model:generateContent");
  route("google_generate_literal", presets::try_google_generate("gemini-2.5-flash"), "gemini-2.5-flash",
        "/v1beta/models/gemini-2.5-flash:generateContent");
  route("google_interactions", presets::try_google_interactions(), "fixture-model", "/v1beta/interactions");
  route("openrouter_chat", presets::try_openrouter_chat(), "fixture-model", "/api/v1/chat/completions");
  route("openrouter_responses", presets::try_openrouter_responses(), "fixture-model", "/api/v1/responses");
  route("openrouter_messages", presets::try_openrouter_messages(), "fixture-model", "/api/v1/messages");
  route("loopback_chat", presets::try_loopback_chat(), "fixture-model", "/v1/chat/completions");
  // Hand-written descriptors: a literal Generate route, a model-routed one and an Interactions one.
  route("loopback_generate_literal",
        loopback("generate-literal", "google.generate", "/v1beta/models/fixture-model:generateContent",
                 "/v1beta/models/fixture-model:streamGenerateContent?alt=sse"),
        "fixture-model", "/v1beta/models/fixture-model:generateContent");
  route("loopback_generate_routed",
        loopback("generate-routed", "google.generate", "/v1beta/models/{model}:generateContent",
                 "/v1beta/models/{model}:streamGenerateContent?alt=sse"),
        "other-model", "/v1beta/models/other-model:generateContent");
  route("loopback_interactions", loopback("interactions", "google.interactions", "/v1beta/interactions", "/v1beta/interactions"),
        "fixture-model", "/v1beta/interactions");
}
} // namespace

int main() {
  try {
    builtin_policy_identity();
    preset_origins_and_bindings();
    chat_audio_configuration();
    native_routes();
  } catch (const std::exception& error) {
    ++failures;
    std::fprintf(stderr, "EXCEPTION %s\n", error.what());
  }
  if (failures) std::fprintf(stderr, "%d policy identity checks failed\n", failures);
  else std::puts("policy identity pins hold");
  return failures ? 1 : 0;
}
