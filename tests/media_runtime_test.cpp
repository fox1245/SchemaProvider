// Usage: sp_media_runtime_tests <node> <tests/support/media_server.mjs>
// Model-free HTTP/SSE: Node owns the bytes and independently checks the actual request wire.
#include "runtime/client.h"
#include "descriptor/policy.h"
#include "support/runtime_peer.h"
#include "sp/config_defaults.h"
#include <chrono>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
using runtime_test::Peer;
using runtime_test::require;
using namespace std::chrono_literals;
using sp::Media;
using sp::MediaKind;
using sp::MediaSource;
using sp::runtime::Result;
enum class Family { Chat, Responses, Messages, Gemini, Interactions };
constexpr Family families[] = {Family::Chat, Family::Responses, Family::Messages, Family::Gemini, Family::Interactions};
constexpr const char* secret = "MEDIA_SYNTHETIC_KEY";
std::string quote(std::string_view value) { return sp::json::quote(value); }
std::string array(const std::vector<std::string>& items) {
  std::string value = "[";
  for (const auto& item : items) { if (value.size() != 1) value += ','; value += item; }
  return value + ']';
}
std::string family_name(Family family) {
  switch (family) {
    case Family::Chat: return "chat";
    case Family::Responses: return "responses";
    case Family::Messages: return "messages";
    case Family::Gemini: return "gemini";
    case Family::Interactions: return "interactions";
  }
  throw std::runtime_error("invalid test family");
}
sp::descriptor::ValidatedDescriptor descriptor(Peer& peer, Family family, const std::string& model, bool gateway = false,
                                              sp::descriptor::PolicySnapshot policy = sp::descriptor::builtin_policy()) {
  const auto origin = "http://127.0.0.1:" + std::to_string(peer.port);
  const auto name = family == Family::Chat ? "openai.chat" : family == Family::Responses ? "openai.responses" : family == Family::Messages ? "anthropic.messages" : family == Family::Gemini ? "google.generate" : "google.interactions";
  const auto path = family == Family::Chat ? "/v1/chat/completions" : family == Family::Responses ? "/v1/responses" : family == Family::Messages ? "/v1/messages" : family == Family::Gemini ? "/v1beta/models/" + model + ":generateContent" : "/v1beta/interactions";
  const auto streaming = family == Family::Gemini ? "/v1beta/models/" + model + ":streamGenerateContent?alt=sse" : path;
  if (gateway) {
    // Only the gateway's origin changes; preserve the complete official MIME/source admission.
    std::string json(sp::config_defaults::descriptor_policy_json);
    const auto from = quote("https://openrouter.ai"), to = quote(origin);
    std::size_t at = 0;
    while ((at = json.find(from, at)) != std::string::npos) { json.replace(at, from.size(), to); at += to.size(); }
    auto loaded = sp::descriptor::load_policy(json, sp::config_defaults::codec_defaults_json);
    require(std::holds_alternative<sp::descriptor::PolicySnapshot>(loaded), "loopback gateway policy rejected");
    policy = std::get<sp::descriptor::PolicySnapshot>(std::move(loaded));
  }
  auto loaded = sp::descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"media-runtime\",\"family\":" + quote(name)
      + ",\"connection\":{\"base_url\":" + quote(origin) + ",\"paths\":{\"buffered\":" + quote(path)
      + ",\"streaming\":" + quote(streaming) + "},\"headers\":{\"anthropic-version\":\"2023-06-01\"}}}", policy);
  require(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(loaded), "media runtime descriptor rejected");
  return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}
struct Fixture {
  std::string name, mime, base64, sha;
  std::size_t size;
  unsigned salt;
  MediaKind kind;
  Media media() const { return Media::inline_data(kind, mime, std::make_shared<const std::string>(base64)); }
  std::string plan(std::string_view extra = {}) const {
    return "{\"t\":\"media\",\"fixture\":" + quote(name) + ",\"size\":" + std::to_string(size)
        + ",\"salt\":" + std::to_string(salt) + ",\"mime\":" + quote(mime) + std::string(extra) + '}';
  }
};
Fixture fixture(Peer& peer, std::string name, std::size_t size, unsigned salt, std::string_view mime = {}) {
  auto report = peer.command("{\"fixture\":" + quote(name) + ",\"size\":" + std::to_string(size) + ",\"salt\":" + std::to_string(salt)
      + (mime.empty() ? "" : ",\"mime\":" + quote(mime)) + '}');
  auto root = report.root();
  const auto kind = root.get("kind").as_string();
  require(root.get("size").as_uint() == size && root.get("base64").is_string(), "fixture control did not return exact bounded bytes");
  return {std::move(name), std::string(root.get("mime").as_string()), std::string(root.get("base64").as_string()),
      std::string(root.get("sha256").as_string()), size, salt,
      kind == "image" ? MediaKind::Image : kind == "audio" ? MediaKind::Audio : kind == "video" ? MediaKind::Video : MediaKind::Document};
}
struct Input {
  std::vector<sp::Part> parts;
  std::vector<std::string> expected;
  std::size_t inline_count = 0;
  void text(std::string value) {
    expected.push_back("{\"t\":\"text\",\"text\":" + quote(value) + '}'); parts.push_back(sp::Text{std::move(value)});
  }
  void bytes(const Fixture& f, std::string name = {}) {
    auto media = f.media(); media.name = std::move(name);
    expected.push_back("{\"t\":\"media\",\"kind\":" + quote(sp::media_kind_name(f.kind)) + ",\"mime\":" + quote(f.mime)
        + ",\"source\":\"inline\",\"sha256\":" + quote(f.sha) + (media.name.empty() ? "" : ",\"name\":" + quote(media.name)) + '}');
    parts.push_back(std::move(media)); ++inline_count;
  }
  void reference(Media media) {
    expected.push_back("{\"t\":\"media\",\"kind\":" + quote(sp::media_kind_name(media.kind)) + ",\"mime\":" + quote(media.mime)
        + ",\"source\":" + quote(sp::media_source_name(media.source)) + ",\"ref\":" + quote(media.reference) + '}');
    parts.push_back(std::move(media));
  }
};
sp::runtime::Request request(Family family, const std::string& model, const Input& input, bool mixed = true) {
  sp::Message user{"", sp::Role::User, input.parts};
  if (family == Family::Chat) {
    sp::chat::Request value; value.model = model; value.canonical_messages.push_back(std::move(user)); return value;
  }
  if (family == Family::Responses) {
    sp::responses::Request value; value.model = model; value.account_scope = "media-test"; value.messages.push_back(std::move(user));
    value.hosted_tools.push_back(sp::responses::ImageGenerationTool{}); return value;
  }
  if (family == Family::Messages) {
    sp::messages::Request value; value.model = model; value.account_scope = "media-test"; value.max_tokens = 64; value.messages.push_back(std::move(user)); return value;
  }
  if (family == Family::Gemini) {
    sp::gemini::Request value; value.model = model; value.account_scope = "media-test"; value.messages.push_back(std::move(user));
    value.response_modalities = {sp::gemini::ResponseModality::Text, sp::gemini::ResponseModality::Image};
    if (mixed) value.response_modalities.push_back(sp::gemini::ResponseModality::Audio);
    return value;
  }
  sp::interactions::Request value; value.model = model; value.account_scope = "media-test"; value.messages.push_back(std::move(user));
  value.response_format.push_back(sp::interactions::TextResponseFormat{});
  if (mixed) value.response_format.push_back(sp::interactions::ImageResponseFormat{});
  value.response_format.push_back(sp::interactions::AudioResponseFormat{});
  if (mixed) value.response_format.push_back(sp::interactions::VideoResponseFormat{});
  return value;
}
std::string arm(Peer& peer, Family family, const Input& input, std::vector<std::string> plan,
                std::string_view options = "{}", std::string_view fault = "{}", std::string_view subset = "{}") {
  static std::size_t sequence = 0;
  const auto model = "media-" + std::to_string(++sequence);
  auto report = peer.command("{\"arm\":" + quote(model) + ",\"family\":" + quote(family_name(family))
      + ",\"expect\":" + array(input.expected) + ",\"plan\":" + array(plan) + ",\"options\":" + std::string(options)
      + ",\"fault\":" + std::string(fault) + ",\"subset\":" + std::string(subset) + '}');
  require(report.root().get("armed").as_bool(), "peer did not arm media case"); return model;
}
std::string text_plan(std::string_view text) { return "{\"t\":\"text\",\"text\":" + quote(text) + '}'; }
sp::runtime::Options options() {
  sp::runtime::Options value; value.api_key = secret; value.default_timeout = 15s;
  value.retry_tokens = 0; value.retry_tokens_per_second = 0; return value;
}
sp::runtime::RunOptions run(bool streaming) { sp::runtime::RunOptions value; value.streaming = streaming; return value; }
void checked_request(Peer& peer, const std::string& model, std::size_t inline_count, std::size_t faults = 0) {
  auto report = peer.stats(model); auto root = report.root();
  if (root.get("invalid").as_uint() != 0) throw std::runtime_error("Node wire oracle rejected " + model + ": " + root.dump());
  require(root.get("count").as_uint() == 1 && root.get("unexpected").as_uint() == 0, "media request not singular or wrong route");
  require(root.get("verified").as_uint() == inline_count, "Node did not independently verify all received payload bytes");
  require(root.get("faults").as_uint() == faults, "transport fault not delivered");
}
void exact(const Result& result, const std::vector<sp::Part>& expected) {
  require(result && std::holds_alternative<sp::Completion>(*result), "expected generated media completion");
  const auto& completion = std::get<sp::Completion>(*result);
  require(completion.stop.kind == sp::StopKind::EndTurn && completion.usage.stage == sp::UsageStage::Final, "media result not normally finalized");
  std::vector<const sp::Part*> actual;
  for (const auto& message : completion.messages) {
    require(message.role == sp::Role::Assistant, "generated media role changed");
    for (const auto& part : message.parts) actual.push_back(&part);
  }
  require(actual.size() == expected.size(), "generated output contains missing or unexpected parts");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const auto& want = expected[i]; const auto& got = *actual[i];
    if (const auto* text = std::get_if<sp::Text>(&want)) {
      const auto* value = std::get_if<sp::Text>(&got); require(value && value->value == text->value, "consumer-visible text or ordering differs"); continue;
    }
    const auto& media = std::get<Media>(want); const auto* value = std::get_if<Media>(&got);
    require(value && value->kind == media.kind && value->source == media.source && value->mime == media.mime,
        "consumer-visible media kind, source, MIME or ordering differs");
    require(value->reference == media.reference && value->name == media.name && value->id == media.id && value->transcript == media.transcript,
        "generated media reference, name, identity or transcript differs");
    require(static_cast<bool>(value->data) == static_cast<bool>(media.data), "inline ownership or reference payload differs");
    if (media.data) require(*value->data == *media.data, "generated media bytes differ, duplicated, or preview exposed");
  }
}
Input family_input(Peer& peer, Family family) {
  Input input; input.text("before \xc3\xa9");
  input.bytes(fixture(peer, family == Family::Gemini || family == Family::Interactions ? "heic" : "png", 257, 3));
  input.text("between image and document");
  input.bytes(fixture(peer, "pdf", 263, 5), family == Family::Gemini || family == Family::Interactions ? "" : "evidence.pdf");
  if (family == Family::Chat || family == Family::Gemini || family == Family::Interactions)
    input.bytes(fixture(peer, "wav", 268, 7));
  if (family == Family::Gemini || family == Family::Interactions) {
    input.bytes(fixture(peer, "mp4", 269, 9));
    input.reference(Media::file(MediaKind::Audio, "https://generativelanguage.googleapis.com/v1beta/files/source-audio", "audio/wav"));
    input.reference(Media::url(MediaKind::Video, "https://example.invalid/source-video.mp4", "video/mp4"));
  } else {
    input.reference(Media::url(MediaKind::Image, "https://example.invalid/source-image.png", "image/png"));
    input.reference(Media::file(MediaKind::Document, "file-source-pdf", "application/pdf"));
    if (family != Family::Chat) input.reference(Media::file(MediaKind::Image, "file-source-image", "image/png"));
  }
  input.text("after all media"); return input;
}
void five_family_roundtrips(Peer& peer) {
  for (const auto family : families) for (const bool streaming : {false, true}) {
    const auto input = family_input(peer, family);
    std::vector<std::string> plan{text_plan("generated \xe2\x98\x83 before")};
    std::vector<sp::Part> expected{sp::Text{"generated \xe2\x98\x83 before"}};
    if (family == Family::Chat) {
      const auto output = fixture(peer, streaming ? "l16" : "wav", 522, 11);
      plan.push_back(output.plan(",\"id\":\"audio-result\",\"transcript\":\"spoken transcript \xc3\xa9\""));
      auto media = output.media(); media.id = "audio-result"; media.transcript = "spoken transcript \xc3\xa9"; expected.push_back(std::move(media));
    } else if (family == Family::Responses) {
      const auto first = fixture(peer, "png", 523, 13), second = fixture(peer, "webp", 527, 17);
      plan.push_back(first.plan(",\"id\":\"ig-first\"")); auto media = first.media(); media.id = "ig-first"; expected.push_back(media);
      plan.push_back(text_plan("between generated images")); expected.push_back(sp::Text{"between generated images"});
      plan.push_back(second.plan(",\"id\":\"ig-second\"")); media = second.media(); media.id = "ig-second"; expected.push_back(std::move(media));
    } else if (family == Family::Gemini || family == Family::Interactions) {
      const auto image = fixture(peer, "png", 529, 19), audio = fixture(peer, "l16", 532, 23);
      plan.push_back(image.plan(family == Family::Gemini ? ",\"name\":\"generated.png\"" : ""));
      auto generated_image = image.media(); if (family == Family::Gemini) generated_image.name = "generated.png";
      expected.push_back(std::move(generated_image));
      plan.push_back(text_plan("between generated modalities")); expected.push_back(sp::Text{"between generated modalities"});
      plan.push_back(audio.plan()); expected.push_back(audio.media());
      if (family == Family::Interactions) {
        const auto video = fixture(peer, "mp4", 541, 29); plan.push_back(video.plan()); expected.push_back(video.media());
      }
      const auto file = fixture(peer, family == Family::Interactions ? "mp4" : "png", 547, 31);
      const std::string uri = "https://generativelanguage.googleapis.com/v1beta/files/generated-"
          + std::string(family == Family::Interactions ? "video" : "image");
      plan.push_back(file.plan(",\"delivery\":\"uri\",\"uri\":" + quote(uri)));
      expected.push_back(Media::file(file.kind, uri, file.mime));
      plan.push_back(text_plan("generated after")); expected.push_back(sp::Text{"generated after"});
    }
    // Mixed output is a schema/parser oracle, not a claim that one public model emits all kinds.
    const std::string controls = family == Family::Chat
        ? "{\"modalities\":[\"text\",\"audio\"],\"audio\":{\"voice\":\"alloy\",\"format\":" + quote(streaming ? "pcm16" : "wav") + "}}"
        : family == Family::Responses ? "{\"tools\":[{\"type\":\"image_generation\"}]}"
        : family == Family::Gemini ? "{\"generationConfig\":{\"responseModalities\":[\"TEXT\",\"IMAGE\",\"AUDIO\"]}}"
        : family == Family::Interactions ? "{\"response_format\":[{\"type\":\"text\"},{\"type\":\"image\"},{\"type\":\"audio\"},{\"type\":\"video\"}]}" : "{}";
    const auto model = arm(peer, family, input, plan, family == Family::Gemini ? "{\"pack\":true}" : family == Family::Interactions ? "{\"final_steps\":true}" : "{}", "{}", controls);
    auto typed = request(family, model, input);
    if (family == Family::Chat) {
      auto& chat = std::get<sp::chat::Request>(typed); chat.modalities = {sp::chat::OutputModality::Text, sp::chat::OutputModality::Audio};
      chat.audio = sp::chat::AudioOutput{"alloy", streaming ? "pcm16" : "wav"};
    }
    Result result;
    { sp::runtime::Client client(descriptor(peer, family, model), options()); result = client.complete(std::move(typed), run(streaming)); }
    checked_request(peer, model, input.inline_count); exact(result, expected); // Own bytes beyond Client destruction.
  }
}
void gateway_images_and_video(Peer& peer) {
  for (const bool streaming : {false, true}) {
    auto input = family_input(peer, Family::Chat);
    input.bytes(fixture(peer, "mp4", 277, 37)); input.text("after gateway video");
    const auto image = fixture(peer, "png", 557, 41);
    const std::string uri = "https://example.invalid/generated-image.png";
    auto remote = image.plan(",\"delivery\":\"uri\",\"uri\":" + quote(uri));
    const auto model = arm(peer, Family::Chat, input, {text_plan("gateway answer"), image.plan(), remote}, "{\"repeat_images\":true}", "{}", "{\"modalities\":[\"text\",\"image\"]}");
    auto typed = request(Family::Chat, model, input);
    std::get<sp::chat::Request>(typed).modalities = {sp::chat::OutputModality::Text, sp::chat::OutputModality::Image};
    sp::runtime::Client client(descriptor(peer, Family::Chat, model, true), options());
    const auto result = client.complete(std::move(typed), run(streaming)); checked_request(peer, model, input.inline_count);
    exact(result, {sp::Text{"gateway answer"}, image.media(), Media::url(MediaKind::Image, uri)});
  }
}
void transcription_annotations(Peer& peer) {
  Input input; input.text("transcribe with words and speakers");
  input.bytes(fixture(peer, "wav", 273, 47));
  const std::string annotations = R"([{"type":"word_info","text":"Héllo","start_index":0,"end_index":6,"start_offset":"0.100s","end_offset":"0.450s","speaker":"spk_1"},{"type":"word_info","text":"world","start_index":7,"end_index":12,"start_offset":"0.500s","end_offset":"0.900s","speaker":"spk_2"},{"type":"url_citation","start_index":0,"end_index":12,"url":"https://example.invalid/transcript-source","title":"source"}])";
  auto expected = sp::json::parse(annotations);
  require(std::holds_alternative<sp::json::Document>(expected), "transcription expectation malformed");
  const auto& expected_annotations = std::get<sp::json::Document>(expected);
  const std::string controls = R"({"response_format":{"type":"text"},"generation_config":{"transcription_config":{"language_codes":["en-US"],"mode":{"type":"verbatim","diarization_mode":"speaker","timestamp_granularities":["word"]}}}})";
  for (const bool final_steps : {false, true}) for (const bool contradiction : {false, true}) {
    if (contradiction && !final_steps) continue;
    const auto model = arm(peer, Family::Interactions, input, {text_plan("Héllo world")},
        std::string("{\"transcription\":true,\"final_steps\":") + (final_steps ? "true" : "false")
            + ",\"annotation_contradiction\":" + (contradiction ? "true" : "false") + "}", "{}", controls);
    sp::interactions::Request typed; typed.model = model; typed.account_scope = "media-test";
    typed.messages.push_back({"", sp::Role::User, input.parts});
    typed.response_format.push_back(sp::interactions::TextResponseFormat{});
    typed.transcription_config = sp::interactions::TranscriptionConfig{{"en-US"}, {}, sp::interactions::TranscriptionMode::Verbatim, true, true};
    Result result;
    { sp::runtime::Client client(descriptor(peer, Family::Interactions, model), options()); result = client.complete(std::move(typed), run(true)); }
    checked_request(peer, model, input.inline_count);
    if (contradiction) {
      require(result && std::holds_alternative<sp::Failure>(*result) &&
          std::get<sp::Failure>(*result).error.kind == sp::ErrorKind::ProtocolCorrupt, "contradictory final speaker metadata committed");
      continue;
    }
    exact(result, {sp::Text{"Héllo world"}});
    const auto& completion = std::get<sp::Completion>(*result);
    const auto& message = completion.messages[0];
    require(message.native && message.wire_output, "transcription lacks native ordered output");
    require(sp::json::equal(message.wire_output->root().at(0).get("content").at(0).get("annotations"),
                           expected_annotations.root()), "transcription metadata lost or changed");
    std::size_t observed = 0;
    for (const auto& event : completion.raw_events) {
      const auto delta = event.payload->root().get("delta");
      if (delta.get("type").as_string() != "text_annotation_delta") continue;
      require(observed < 3 && sp::json::equal(delta.get("annotations").at(0), expected_annotations.root().at(observed)),
          "raw transcription observations lost or changed");
      ++observed;
    }
    require(observed == 3, "raw transcription annotations missing");
  }
}
void no_partial_inline(const Result& result, sp::ErrorKind expected, bool prefix_received) {
  require(result && std::holds_alternative<sp::Failure>(*result), "corrupt or truncated generated media committed");
  const auto& failed = std::get<sp::Failure>(*result);
  require(failed.error.kind == expected, "media fault has wrong consumer-visible error kind");
  require(failed.error.attempt.request_may_have_left && failed.error.attempt.response_head_seen, "media failure lacks HTTP attempt evidence");
  std::string text;
  for (const auto& message : failed.partial.messages) for (const auto& part : message.parts) {
    if (const auto* media = std::get_if<Media>(&part)) require(!media->data, "failure exposed a partial inline generated payload");
    if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
  }
  if (prefix_received) require(text == "safe prefix", "failure lost or altered the independently received safe text prefix");
  require(failed.error.safe_message.find(secret) == std::string::npos, "credential exposed by media failure");
}
void malformed_and_truncated(Peer& peer) {
  for (const auto family : {Family::Chat, Family::Responses, Family::Gemini, Family::Interactions}) {
    Input input; input.text("reject incomplete generated bytes");
    const auto output = fixture(peer, family == Family::Chat || family == Family::Interactions ? "l16" : "png", 565, 43);
    const std::string identity = family == Family::Chat ? ",\"id\":\"audio-fault\",\"transcript\":\"not a complete reply\"" : family == Family::Responses ? ",\"id\":\"image-fault\"" : "";
    for (const bool streaming : {false, true}) {
      const auto model = arm(peer, family, input, {text_plan("safe prefix"), output.plan(identity + ",\"fault\":{\"payload\":\"bad-char\"}")});
      auto typed = request(family, model, input, false);
      if (family == Family::Chat) { auto& chat = std::get<sp::chat::Request>(typed); chat.modalities = {sp::chat::OutputModality::Text, sp::chat::OutputModality::Audio}; chat.audio = sp::chat::AudioOutput{"alloy", "pcm16"}; }
      sp::runtime::Client client(descriptor(peer, family, model), options());
      no_partial_inline(client.complete(std::move(typed), run(streaming)), sp::ErrorKind::ProtocolCorrupt, streaming); checked_request(peer, model, 0);
    }
    const auto model = arm(peer, family, input, {text_plan("safe prefix"), output.plan(identity)}, "{}", "{\"transport\":\"clean\",\"at\":\"mid-payload\"}");
    auto typed = request(family, model, input, false);
    if (family == Family::Chat) { auto& chat = std::get<sp::chat::Request>(typed); chat.modalities = {sp::chat::OutputModality::Text, sp::chat::OutputModality::Audio}; chat.audio = sp::chat::AudioOutput{"alloy", "pcm16"}; }
    sp::runtime::Client client(descriptor(peer, family, model), options());
    no_partial_inline(client.complete(std::move(typed), run(true)), sp::ErrorKind::Truncated, true); checked_request(peer, model, 0, 1);
  }
}
void native_media_replay_budget(Peer& peer) {
  // Match the policy rewrite used by the request-side bounded-policy tests.
  const auto replace = [](sp::json::Value object, std::string_view key, std::string_view replacement) {
    std::string value = "{";
    for (const auto member : object.members()) {
      if (value.size() > 1) value += ',';
      value += quote(member.key) + ':' + (member.key == key ? std::string(replacement) : member.value.dump());
    }
    return value + '}';
  };
  auto defaults = sp::json::parse(sp::config_defaults::codec_defaults_json);
  require(std::holds_alternative<sp::json::Document>(defaults), "codec defaults could not be parsed");
  const auto root = std::get<sp::json::Document>(defaults).root();
  auto loaded = sp::descriptor::load_policy(sp::config_defaults::descriptor_policy_json,
      replace(root, "resources", replace(root.get("resources"), "request_bytes", "1024")));
  require(std::holds_alternative<sp::descriptor::PolicySnapshot>(loaded), "bounded replay policy rejected");
  const auto policy = std::get<sp::descriptor::PolicySnapshot>(std::move(loaded));
  for (const bool streaming : {false, true}) {
    Input input; input.text("produce audio");
    const auto audio = fixture(peer, "wav", 1536, 47);
    const auto model = arm(peer, Family::Gemini, input, {audio.plan()});
    auto initial = std::get<sp::gemini::Request>(request(Family::Gemini, model, input, false));
    sp::runtime::Client client(descriptor(peer, Family::Gemini, model, false, policy), options());
    const auto result = client.complete(initial, run(streaming));
    exact(result, {audio.media()});
    auto next = initial;
    for (const auto& message : std::get<sp::Completion>(*result).messages) next.messages.push_back(message);
    next.messages.push_back({"", sp::Role::User, {sp::Text{"continue"}}});
    const auto exhausted = client.complete(next, run(streaming));
    require(exhausted && std::holds_alternative<sp::Failure>(*exhausted), "oversized media replay unexpectedly dispatched");
    require(std::get<sp::Failure>(*exhausted).error.kind == sp::ErrorKind::ResourceLimit,
        "native-media-only replay budget must report ResourceLimit");
    require(!std::get<sp::Failure>(*exhausted).error.attempt.request_may_have_left,
        "oversized media replay reached transport");
    std::get<sp::Text>(next.messages[0].parts[0]).value = "edited prefix";
    const auto edited = client.complete(next, run(streaming));
    require(edited && std::holds_alternative<sp::Failure>(*edited) &&
        std::get<sp::Failure>(*edited).error.kind == sp::ErrorKind::ReplayIneligible,
        "media budget exhaustion hid an edited native prefix");
    checked_request(peer, model, 0);
  }
}
} // namespace
int main(int argc, char** argv) {
#ifdef _WIN32
  runtime_test::Arguments arguments(argc, argv); argc = arguments.argc(); argv = arguments.argv();
#endif
  try {
    require(argc == 3, "usage: sp_media_runtime_tests <node> <media_server.mjs>");
    Peer peer(argv[1], argv[2]); five_family_roundtrips(peer); gateway_images_and_video(peer); transcription_annotations(peer); malformed_and_truncated(peer);
    native_media_replay_budget(peer);
    std::cout << "Five-family typed media HTTP/SSE byte, layout, generated output and fault contracts passed (model-free)\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
