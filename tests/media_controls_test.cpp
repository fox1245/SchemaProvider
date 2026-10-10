#include "codecs/chat.h"
#include "codecs/gemini.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions.h"
#include "codecs/interactions_request.h"
#include "codecs/responses.h"
#include "codecs/responses_request.h"
#include "core/native.h"
#include "json/json.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace sp;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)
json::Document parse(std::string_view bytes) {
  auto parsed = json::parse(bytes); CHECK(std::holds_alternative<json::Document>(parsed));
  return std::get<json::Document>(std::move(parsed));
}
std::shared_ptr<const json::Document> own(std::string_view bytes) { return std::make_shared<const json::Document>(parse(bytes)); }
descriptor::ValidatedDescriptor descriptor_for(std::string_view family, std::string_view model = "fixture-model") {
  const bool gemini = family == "google.generate";
  const std::string path = gemini ? "/v1beta/models/" + std::string(model) + ":generateContent" : family == "google.interactions" ? "/v1beta/interactions" : family == "openai.chat" ? "/v1/chat/completions" : "/v1/responses";
  const std::string stream = gemini ? "/v1beta/models/" + std::string(model) + ":streamGenerateContent?alt=sse" : path;
  auto loaded = descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"media-controls\",\"family\":" + json::quote(family) + ",\"connection\":{\"base_url\":\"http://127.0.0.1:18080\",\"paths\":{\"buffered\":" + json::quote(path) + ",\"streaming\":" + json::quote(stream) + "}}}");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
template<class Request> Request request() {
  Request value; value.model = "fixture-model"; value.messages.push_back(Message{"", Role::User, {Text{"question"}}}); return value;
}
template<class Result> void invalid(const Result& result) { CHECK(std::holds_alternative<Error>(result)); CHECK(std::get<Error>(result).kind == ErrorKind::InvalidRequest); }
template<class Result> void replay_rejected(const Result& result) { CHECK(std::holds_alternative<Error>(result)); CHECK(std::get<Error>(result).kind == ErrorKind::ReplayIneligible); }
Message gemini_reply(const descriptor::ValidatedDescriptor& descriptor, const gemini::EncodedRequest& encoded) {
  Accumulator accumulator; gemini::Codec codec(descriptor, gemini::Mode::Buffered, accumulator, encoded.context);
  codec.buffered(R"({"modelVersion":"fixture-model","responseId":"g1","candidates":[{"index":0,"content":{"role":"model","parts":[{"text":"answer"}]},"finishReason":"STOP"}]})", {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  return std::get<Completion>(*accumulator.outcome()).messages.front();
}
Message interaction_reply(const descriptor::ValidatedDescriptor& descriptor, const interactions::EncodedRequest& encoded, bool image_summary = false) {
  Accumulator accumulator; interactions::Codec codec(descriptor, interactions::Mode::Buffered, accumulator, encoded.context);
  const std::string thought = image_summary ? R"({"type":"thought","signature":"sig","summary":[{"type":"image","mime_type":"image/jpeg","data":"AQID"},{"type":"text","text":"brief"}]},)" : "";
  codec.buffered("{\"id\":\"i1\",\"model\":\"fixture-model\",\"status\":\"completed\",\"steps\":[" + thought + R"({"type":"model_output","content":[{"type":"text","text":"answer"}]}]})", {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  return std::get<Completion>(*accumulator.outcome()).messages.front();
}
void gemini_nonthinking_media_request() {
  const auto descriptor = descriptor_for("google.generate", "gemini-2.5-flash-preview-tts");
  auto value = request<gemini::Request>();
  value.model = "gemini-2.5-flash-preview-tts";
  value.response_modalities = {gemini::ResponseModality::Audio};
  value.speech_config = gemini::SpeechConfig{gemini::VoiceConfig{std::string("Kore"), {}}, {}, {}};
  for (const bool streaming : {false, true}) {
    auto encoded = gemini::encode(descriptor, value, streaming);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
    auto wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
    // Non-thinking media models must not receive an unsolicited thinking directive.
    CHECK(!wire.root().get("generationConfig").get("thinkingConfig").valid());
    auto explicit_control = value; explicit_control.include_thoughts = true;
    encoded = gemini::encode(descriptor, explicit_control, streaming);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
    wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
    CHECK(wire.root().get("generationConfig").get("thinkingConfig").get("includeThoughts").as_bool());
  }
}
void gemini_boundaries_and_binding() {
  const auto descriptor = descriptor_for("google.generate");
  auto value = request<gemini::Request>();
  value.response_modalities = {gemini::ResponseModality::Audio};
  value.speech_config = gemini::SpeechConfig{gemini::VoiceConfig{std::string("Kore"), {}}, {}, std::string("en-US")};
  auto encoded = gemini::encode(descriptor, value, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  auto wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
  CHECK(wire.root().get("generationConfig").get("responseModalities").at(0).as_string() == "AUDIO");
  CHECK(wire.root().get("generationConfig").get("speechConfig").get("voiceConfig").get("prebuiltVoiceConfig").get("voiceName").as_string() == "Kore");
  auto next = value; next.messages.push_back(gemini_reply(descriptor, std::get<gemini::EncodedRequest>(encoded))); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(descriptor, next, false)));
  auto changed = next; changed.speech_config->language_code = "fr-FR"; replay_rejected(gemini::encode(descriptor, changed, false));
  changed = next; changed.speech_config->voice_config->prebuilt_voice_name = "Puck"; replay_rejected(gemini::encode(descriptor, changed, false));
  changed = next; changed.response_modalities = {gemini::ResponseModality::Text, gemini::ResponseModality::Audio}; replay_rejected(gemini::encode(descriptor, changed, false));
  auto bad = value; bad.speech_config->voice_config->voice = "voice_custom"; invalid(gemini::encode(descriptor, bad, false));
  bad = value; bad.speech_config->speaker_voice_configs.push_back({"Joe", {std::string("Puck"), {}}}); invalid(gemini::encode(descriptor, bad, false));
  bad = value; bad.response_modalities.push_back(gemini::ResponseModality::Audio); invalid(gemini::encode(descriptor, bad, false));
  value = request<gemini::Request>(); value.response_modalities = {gemini::ResponseModality::Image}; value.image_config = gemini::ImageConfig{std::string("16:9"), std::string("2K")};
  encoded = gemini::encode(descriptor, value, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  next = value; next.messages.push_back(gemini_reply(descriptor, std::get<gemini::EncodedRequest>(encoded))); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  changed = next; changed.image_config->image_size = "4K"; replay_rejected(gemini::encode(descriptor, changed, false));
  value = request<gemini::Request>(); value.audio_transcription_config = gemini::AudioTranscriptionConfig{{"en-US"}, {}, true, true, gemini::TranscriptionMode::Verbatim};
  encoded = gemini::encode(descriptor, value, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  wire = parse(std::get<gemini::EncodedRequest>(encoded).body); CHECK(wire.root().get("generationConfig").get("audioTranscriptionConfig").get("wordTimestamp").as_bool());
  next = value; next.messages.push_back(gemini_reply(descriptor, std::get<gemini::EncodedRequest>(encoded))); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  changed = next; changed.audio_transcription_config->diarization = false; replay_rejected(gemini::encode(descriptor, changed, false));
  bad = value; bad.audio_transcription_config->mode = gemini::TranscriptionMode::Smart; invalid(gemini::encode(descriptor, bad, false));
  bad = value; bad.audio_transcription_config->custom_vocabulary = {"NeoGraph"}; invalid(gemini::encode(descriptor, bad, false));
  bad = value; bad.audio_transcription_config->word_timestamp = false; bad.audio_transcription_config->diarization = false; bad.audio_transcription_config->mode = gemini::TranscriptionMode::Smart;
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(descriptor, bad, false)));
}
void interaction_nonthinking_transcription_request() {
  const auto descriptor = descriptor_for("google.interactions");
  auto value = request<interactions::Request>();
  value.model = "gemini-3.5-transcribe";
  value.transcription_config = interactions::TranscriptionConfig{{"en-US"}, {}, interactions::TranscriptionMode::Verbatim, true, true};
  for (const bool streaming : {false, true}) {
    auto encoded = interactions::encode(descriptor, value, streaming);
    CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
    auto wire = parse(std::get<interactions::EncodedRequest>(encoded).body);
    CHECK(!wire.root().get("generation_config").get("thinking_summaries").valid());
    auto explicit_control = value; explicit_control.thinking_summaries = true;
    encoded = interactions::encode(descriptor, explicit_control, streaming);
    CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
    wire = parse(std::get<interactions::EncodedRequest>(encoded).body);
    CHECK(wire.root().get("generation_config").get("thinking_summaries").as_string() == "auto");
  }
}
void interaction_boundaries_and_binding() {
  const auto descriptor = descriptor_for("google.interactions");
  auto value = request<interactions::Request>();
  value.response_format = {interactions::AudioResponseFormat{std::string("audio/mp3"), 24000, 64000, interactions::Delivery::Inline}};
  value.speech_config = interactions::SpeakerConfig{{{std::string("Kore"), std::string("Joe"), std::string("en-US")}}, true};
  auto encoded = interactions::encode(descriptor, value, false); CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
  auto wire = parse(std::get<interactions::EncodedRequest>(encoded).body); CHECK(wire.root().get("response_format").is_object());
  CHECK(wire.root().get("generation_config").get("speech_config").get("mode").as_string() == "conversational");
  auto next = value; next.messages.push_back(interaction_reply(descriptor, std::get<interactions::EncodedRequest>(encoded))); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  CHECK(std::holds_alternative<interactions::EncodedRequest>(interactions::encode(descriptor, next, false)));
  auto changed = next; std::get<interactions::AudioResponseFormat>(changed.response_format[0]).bit_rate = 128000; replay_rejected(interactions::encode(descriptor, changed, false));
  changed = next; std::get<interactions::SpeakerConfig>(*changed.speech_config).conversational = false; replay_rejected(interactions::encode(descriptor, changed, false));
  auto bad = value; std::get<interactions::AudioResponseFormat>(bad.response_format[0]).mime_type = "audio/wav"; invalid(interactions::encode(descriptor, bad, false));
  value = request<interactions::Request>();
  value.response_format = {interactions::TextResponseFormat{}, interactions::ImageResponseFormat{{}, std::string("1:8"), std::string("4K"), {}}};
  encoded = interactions::encode(descriptor, value, false); CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
  wire = parse(std::get<interactions::EncodedRequest>(encoded).body); CHECK(wire.root().get("response_format").is_array());
  bad = value; std::get<interactions::TextResponseFormat>(bad.response_format[0]).schema = own(R"({"type":"object"})"); invalid(interactions::encode(descriptor, bad, false));
  std::get<interactions::TextResponseFormat>(bad.response_format[0]).mime_type = "application/json";
  CHECK(std::holds_alternative<interactions::EncodedRequest>(interactions::encode(descriptor, bad, false)));
  value = request<interactions::Request>(); value.response_format = {interactions::VideoResponseFormat{std::string("16:9"), std::string("4k"), std::string("8.5s"), {}, interactions::Delivery::Uri}};
  CHECK(std::holds_alternative<interactions::EncodedRequest>(interactions::encode(descriptor, value, false)));
  bad = value; std::get<interactions::VideoResponseFormat>(bad.response_format[0]).duration = "8.5"; invalid(interactions::encode(descriptor, bad, false));
  value = request<interactions::Request>(); value.transcription_config = interactions::TranscriptionConfig{{"en-US"}, {}, interactions::TranscriptionMode::Verbatim, true, true};
  encoded = interactions::encode(descriptor, value, false); CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
  wire = parse(std::get<interactions::EncodedRequest>(encoded).body); const auto transcription = wire.root().get("generation_config").get("transcription_config");
  CHECK(!transcription.get("diarization_mode").valid() && transcription.get("mode").get("diarization_mode").as_string() == "speaker");
  next = value; next.messages.push_back(interaction_reply(descriptor, std::get<interactions::EncodedRequest>(encoded), true)); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  CHECK(std::holds_alternative<interactions::EncodedRequest>(interactions::encode(descriptor, next, false)));
  changed = next; changed.transcription_config->word_timestamps = false; replay_rejected(interactions::encode(descriptor, changed, false));
  changed = next; changed.messages[1].wire_output = own(R"([{"type":"thought","signature":"sig","summary":[{"type":"image","mime_type":"image/jpeg","data":"BAUG"},{"type":"text","text":"brief"}]},{"type":"model_output","content":[{"type":"text","text":"answer"}]}])");
  replay_rejected(interactions::encode(descriptor, changed, false));
  bad = value; bad.transcription_config->mode = interactions::TranscriptionMode::Smart; invalid(interactions::encode(descriptor, bad, false));
  bad = value; bad.transcription_config->custom_vocabulary = {"NeoGraph"}; invalid(interactions::encode(descriptor, bad, false));
}
void responses_format_binding() {
  const auto descriptor = descriptor_for("openai.responses"); auto value = request<responses::Request>();
  value.hosted_tools.push_back(responses::ImageGenerationTool{{}, {}, std::string("webp")});
  auto encoded = responses::encode(descriptor, value, false); CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
  auto wire = parse(std::get<responses::EncodedRequest>(encoded).body); CHECK(wire.root().get("tools").at(0).get("output_format").as_string() == "webp");
  Accumulator accumulator; responses::Codec codec(descriptor, responses::Mode::Buffered, accumulator, std::get<responses::EncodedRequest>(encoded).context);
  codec.buffered(R"({"id":"r1","object":"response","created_at":1,"model":"fixture-model","status":"completed","output":[{"type":"message","id":"m1","role":"assistant","status":"completed","content":[{"type":"output_text","text":"answer","annotations":[]}]}],"error":null,"incomplete_details":null})", {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  auto next = value; next.messages.push_back(std::get<Completion>(*accumulator.outcome()).messages.front()); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  CHECK(std::holds_alternative<responses::EncodedRequest>(responses::encode(descriptor, next, false)));
  std::get<responses::ImageGenerationTool>(next.hosted_tools[0]).output_format = "jpeg"; replay_rejected(responses::encode(descriptor, next, false));
  std::get<responses::ImageGenerationTool>(value.hosted_tools[0]).output_format = "gif"; invalid(responses::encode(descriptor, value, false));
}
void decode_only_audio_scope() {
  const auto descriptor = descriptor_for("openai.chat"); chat::Request value; value.model = "fixture-model";
  value.messages.push_back({Role::User, "Speak"}); value.modalities = {chat::OutputModality::Audio}; value.audio = chat::AudioOutput{"alloy", "wav"};
  auto encoded = chat::encode(descriptor, value, false); CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
  const auto context = std::get<chat::EncodedRequest>(encoded).context->decoding_only(); CHECK(context->audio_format() == "wav" && !context->replay_eligible());
  Accumulator accumulator; chat::Codec codec(descriptor, chat::Mode::Buffered, accumulator, {}, context);
  codec.buffered(R"({"id":"c1","object":"chat.completion","created":1,"model":"fixture-model","choices":[{"index":0,"message":{"role":"assistant","content":null,"audio":{"id":"a1","data":"AQID","expires_at":123,"transcript":"Hello"}},"finish_reason":"stop"}]})", {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  const auto& completion = std::get<Completion>(*accumulator.outcome()); CHECK(std::get<Media>(completion.messages.front().parts.front()).mime == "audio/wav");
}
}
int main() {
  try { gemini_nonthinking_media_request(); interaction_nonthinking_transcription_request(); gemini_boundaries_and_binding(); interaction_boundaries_and_binding(); responses_format_binding(); decode_only_audio_scope(); }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
