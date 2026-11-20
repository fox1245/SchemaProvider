// Realtime session codecs (D8), against the official SDKs. tests/conformance/realtime records one scripted
// scenario through google-genai Live and the OpenAI realtime client; the SDK's codecs must send the same frames for
// the same setup and commands, once canonicalized, and decode the recorded server frames into the expected events
// and per-turn completions.
// Usage: sp_realtime_tests <fixture directory>
#include "codecs/realtime.h"
#include "core/media.h"
#include "core/pricing.h"
#include "descriptor/catalogue.h"
#include "descriptor/policy.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include "sp/config_defaults.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
std::filesystem::path fixtures;
std::vector<std::string> lines(const std::string& name) {
  std::ifstream in(fixtures / name, std::ios::binary);
  CHECK(in.good());
  std::vector<std::string> out;
  for (std::string line; std::getline(in, line);) if (!line.empty()) out.push_back(line);
  return out;
}
std::shared_ptr<const json::Document> document(std::string_view text) {
  auto parsed = json::parse(text);
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
// The built-in policy with its `endpoints` replaced.
std::variant<descriptor::PolicySnapshot, descriptor::ConfigError> policy_with(std::string_view endpoints) {
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  CHECK(std::holds_alternative<json::Document>(parsed));
  std::string text = "{";
  for (auto member : std::get<json::Document>(parsed).root().members()) {
    if (text.size() > 1) text += ',';
    text += json::quote(member.key) + ":" + (member.key == "endpoints" ? std::string(endpoints) : member.value.dump());
  }
  return descriptor::load_policy(text + "}", config_defaults::codec_defaults_json);
}
// The canonical form two frames are compared in: keys sorted; for Google, keys in camelCase (the official SDK sends
// some in snake_case, which the service takes as the same); base64 payloads as their bytes, whatever the alphabet.
std::string camel(std::string_view key) {
  std::string out;
  bool upper = false;
  for (const char c : key) {
    if (c == '_') { upper = true; continue; }
    out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
    upper = false;
  }
  return out;
}
std::string canonical(json::Value value, bool google, std::string_view key = {}) {
  if (value.is_object()) {
    std::map<std::string, std::string> members;
    for (auto member : value.members()) {
      const auto name = google ? camel(member.key) : std::string(member.key);
      members[name] = canonical(member.value, google, name);
    }
    std::string out = "{";
    for (const auto& [name, text] : members) out += (out.size() > 1 ? "," : "") + json::quote(name) + ":" + text;
    return out + "}";
  }
  if (value.is_array()) {
    std::string out = "[";
    for (auto item : value.elements()) out += (out.size() > 1 ? "," : "") + canonical(item, google);
    return out + "]";
  }
  if (value.is_string() && (key == "data" || key == "audio" || key == "delta")) {
    std::string text(value.as_string()), bytes;
    std::replace(text.begin(), text.end(), '-', '+');
    std::replace(text.begin(), text.end(), '_', '/');
    while (text.size() % 4) text += '=';
    if (base64_decode(text, bytes)) return "bytes:" + std::to_string(bytes.size()) + ":" + base64_encode(bytes);
  }
  return value.dump();
}
std::string canonical(std::string_view text, bool google) { return canonical(document(text)->root(), google); }
std::vector<std::string> frames_of(std::variant<std::vector<realtime::Frame>, Error> result) {
  if (const auto* error = std::get_if<Error>(&result)) throw std::runtime_error("encode refused: " + error->safe_message);
  std::vector<std::string> out;
  for (const auto& frame : std::get<std::vector<realtime::Frame>>(result)) out.push_back(frame.data);
  return out;
}
std::string pcm_ramp(std::size_t repeats) {
  std::string out;
  for (std::size_t r = 0; r < repeats; ++r) for (int i = 0; i < 256; ++i) out += static_cast<char>(i);
  return out;
}
realtime::Tool weather(std::string_view parameters) { return {"get_weather", "The weather in a city.", document(parameters)}; }

// The client side: the frames each codec sends equal the official SDK's for the scenario.
void outbound(const descriptor::ValidatedDescriptor& d, bool google, const realtime::Setup& setup, const std::vector<realtime::Command>& commands,
              const std::string& golden) {
  auto codec = realtime::make_codec(d);
  CHECK(codec);
  auto sent = frames_of(codec->open(setup));
  for (const auto& command : commands) for (auto& frame : frames_of(codec->encode(command))) sent.push_back(std::move(frame));
  const auto expected = lines(golden);
  CHECK(sent.size() == expected.size());
  for (std::size_t i = 0; i < sent.size(); ++i)
    if (canonical(sent[i], google) != canonical(expected[i], google))
      throw std::runtime_error(golden + " frame " + std::to_string(i) + " differs:\n  sdk:      " + canonical(sent[i], google) + "\n  official: " + canonical(expected[i], google));
}
// The server side: the recorded frames, decoded in order by `codec`, or by a fresh codec of the descriptor.
std::vector<realtime::Event> inbound(const descriptor::ValidatedDescriptor& d, const std::string& recording,
                                     realtime::Codec* opened = nullptr) {
  auto fresh = opened ? nullptr : realtime::make_codec(d);
  auto* codec = opened ? opened : fresh.get();
  std::vector<realtime::Event> events;
  for (const auto& line : lines(recording)) {
    auto decoded = codec->decode(false, line);
    if (auto* error = std::get_if<Error>(&decoded)) throw std::runtime_error("decode failed: " + error->safe_message + " on " + line.substr(0, 80));
    for (auto& event : std::get<std::vector<realtime::Event>>(decoded)) events.push_back(std::move(event));
  }
  return events;
}
template <class T> std::vector<const T*> all(const std::vector<realtime::Event>& events) {
  std::vector<const T*> out;
  for (const auto& event : events) if (const auto* value = std::get_if<T>(&event)) out.push_back(value);
  return out;
}

void google_live() {
  const auto d = descriptor::presets::google_generate();
  realtime::Setup setup;
  setup.model = "gemini-3.8-live";
  setup.instructions = "Answer briefly.";
  setup.voice = "Puck";
  setup.input_transcription = setup.output_transcription = true;
  setup.resume = "";
  setup.tools = {weather(R"({"type":"OBJECT","properties":{"city":{"type":"STRING"}},"required":["city"]})")};
  outbound(d, true, setup,
           {realtime::Audio{pcm_ramp(25)}, realtime::EndAudio{}, realtime::Text{"And in Seoul?"},
            realtime::ToolResult{"call-1", "get_weather", document(R"({"temperature":21})")}},
           "google-client.jsonl");
  const auto events = inbound(d, "google-server.jsonl");
  CHECK(std::holds_alternative<realtime::Ready>(events.front()));
  const auto transcripts = all<realtime::Transcript>(events);
  CHECK(transcripts.size() == 2 && transcripts[0]->input && transcripts[0]->text == "What is the weather?" && !transcripts[1]->input);
  CHECK(all<realtime::TurnAborted>(events).size() == 1);
  const auto calls = all<realtime::ToolCallEvent>(events);
  CHECK(calls.size() == 1 && calls[0]->id == "call-1" && calls[0]->name == "get_weather" && calls[0]->arguments->root().get("city").as_string() == "Seoul");
  const auto turns = all<realtime::TurnCommitted>(events);
  CHECK(turns.size() == 2);
  // The tool call ends the first turn; the spoken answer is the second.
  CHECK(turns[0]->turn.stop.kind == StopKind::ToolUse);
  const auto& answer = turns[1]->turn;
  CHECK(answer.stop.kind == StopKind::EndTurn && answer.usage.input_total->value == 120 && answer.usage.output_total->value == 40);
  // The tokens of each modality stay with the totals, so a lease can tell audio from text.
  CHECK(answer.usage.extra.at("promptTokensDetails.TEXT").value == 100 && answer.usage.extra.at("promptTokensDetails.AUDIO").value == 20 &&
        answer.usage.extra.at("responseTokensDetails.AUDIO").value == 40);
  const auto* audio = std::get_if<Media>(&answer.messages.at(0).parts.at(0));
  CHECK(audio && audio->kind == MediaKind::Audio && audio->mime == "audio/pcm;rate=24000" && audio->transcript == "It is 21 degrees in Seoul." &&
        base64_decoded_size(*audio->data) == 1024);
  CHECK(all<realtime::AudioDelta>(events).size() == 1 && all<realtime::AudioDelta>(events)[0]->pcm.size() == 1024);
  const auto resume = all<realtime::ResumptionUpdate>(events);
  CHECK(resume.size() == 1 && resume[0]->handle == "resume-1" && resume[0]->resumable);
  const auto drain = all<realtime::DrainNotice>(events);
  CHECK(drain.size() == 1 && drain[0]->time_left == std::chrono::milliseconds(10000));
  // Voice activity, and the empty messages an idle session sends.
  auto codec = realtime::make_codec(d);
  CHECK(std::get<std::vector<realtime::Event>>(codec->decode(true, "{}")).empty());
  const auto activity = std::get<std::vector<realtime::Event>>(codec->decode(true, R"({"voiceActivity":{"type":"ACTIVITY_END","audioOffset":"3.760s"}})"));
  CHECK(activity.size() == 1 && std::holds_alternative<realtime::SpeechStopped>(activity[0]));
  // Refusals before any I/O.
  CHECK(std::get<Error>(codec->encode(realtime::Cancel{})).kind == ErrorKind::Unsupported);
  auto spaced = setup; spaced.model = "models/gemini";
  CHECK(std::holds_alternative<Error>(realtime::connect(d, spaced, "key")));
  auto foreign = setup; foreign.extensions = document(R"({"generationConfig":{}})");
  CHECK(std::get<Error>(realtime::connect(d, foreign, "key")).reason == "request.realtime.options_invalid");
  auto admitted = setup; admitted.extensions = document(R"({"realtimeInputConfig":{"automaticActivityDetection":{"disabled":true}}})");
  const auto connection = std::get<realtime::Connection>(realtime::connect(d, admitted, "key"));
  CHECK(connection.url == "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent" &&
        connection.headers.at(0) == (std::pair<std::string, std::string>{"x-goog-api-key", "key"}));
  CHECK(document(frames_of(realtime::make_codec(d)->open(admitted))[0])->root().get("setup").get("realtimeInputConfig").get("automaticActivityDetection").get("disabled").as_bool());
}

void openai_realtime() {
  const auto d = descriptor::presets::openai_chat();
  realtime::Setup setup;
  setup.model = "gpt-realtime-2.1-mini";
  setup.instructions = "Answer briefly.";
  setup.voice = "alloy";
  setup.input_transcription = true;
  setup.transcription_model = "gpt-live-transcribe";
  setup.tools = {weather(R"({"type":"object","properties":{"city":{"type":"string"}},"required":["city"]})")};
  outbound(d, false, setup,
           {realtime::Audio{pcm_ramp(25)}, realtime::EndAudio{}, realtime::Text{"And in Seoul?"},
            realtime::ToolResult{"call-1", "get_weather", document(R"({"temperature":21})")}, realtime::Respond{}, realtime::Cancel{}},
           "openai-client.jsonl");
  const auto events = inbound(d, "openai-server.jsonl");
  const auto* ready = std::get_if<realtime::Ready>(&events.front());
  CHECK(ready && ready->session_id == "sess-1");
  const auto turns = all<realtime::TurnCommitted>(events);
  CHECK(turns.size() == 1);
  const auto& answer = turns[0]->turn;
  CHECK(answer.stop.kind == StopKind::EndTurn && answer.usage.input_total->value == 120 && answer.usage.output_total->value == 40 && answer.usage.total->value == 160);
  CHECK(answer.usage.extra.at("input_token_details.text_tokens").value == 100 && answer.usage.extra.at("input_token_details.audio_tokens").value == 20 &&
        answer.usage.extra.at("output_token_details.audio_tokens").value == 40 && answer.usage.cache_read && answer.usage.cache_read->value == 0 &&
        !answer.usage.extra.contains("input_token_details.cached_tokens"));
  const auto* audio = std::get_if<Media>(&answer.messages.at(0).parts.at(0));
  CHECK(audio && audio->transcript == "It is 21 degrees." && base64_decoded_size(*audio->data) == 1024 && answer.messages[0].id == "resp-1");
  CHECK(all<realtime::Transcript>(events).size() == 1 && all<realtime::TurnAborted>(events).size() == 1);
  // The model is a query parameter; the key a bearer header.
  const auto connection = std::get<realtime::Connection>(realtime::connect(d, setup, "key"));
  CHECK(connection.url == "wss://api.openai.com/v1/realtime?model=gpt-realtime-2.1-mini" && connection.headers.at(0).second == "Bearer key");
  auto untranscribed = setup; untranscribed.transcription_model.clear();
  CHECK(std::holds_alternative<Error>(realtime::make_codec(d)->open(untranscribed)));
  CHECK(std::holds_alternative<Error>(realtime::connect(descriptor::presets::anthropic_messages(), setup, "key")));
  // An error event and a message that is not JSON.
  auto codec = realtime::make_codec(d);
  const auto error = std::get<std::vector<realtime::Event>>(codec->decode(false, R"({"type":"error","error":{"type":"invalid_request_error","code":"invalid_value","message":"x"}})"));
  CHECK(std::get<realtime::ProviderError>(error.at(0)).error.vendor_code == "invalid_value");
  CHECK(std::get<Error>(codec->decode(false, "not json")).reason == "realtime.message.invalid");
}

// An OpenAI response.done ends the turn by its status: completed is EndTurn, and incomplete is read by the reason its
// status_details state (content_filter, max_output_tokens), MaxTokens when it states none. A status the codec does not
// know, or a reason it does not know, is Unknown with that value as the raw stop, never EndTurn. A failure-class status
// is the provider's error.
void response_statuses() {
  const auto d = descriptor::presets::openai_chat();
  // The events of a response.done with `status` and, when given, `status_details`, after a text delta when `content`.
  const auto done = [&](realtime::Codec& codec, std::string_view status, std::string_view details, bool content) {
    if (content) CHECK(std::get<std::vector<realtime::Event>>(codec.decode(false, R"({"type":"response.output_text.delta","delta":"hi"})")).size() == 1);
    return std::get<std::vector<realtime::Event>>(codec.decode(false, R"({"type":"response.done","response":{"id":"r","status":)" +
        json::quote(status) + (details.empty() ? std::string() : ",\"status_details\":" + std::string(details)) + "}}"));
  };
  const auto events_of = [&](std::string_view status, std::string_view details, bool content) {
    auto codec = realtime::make_codec(d);
    return done(*codec, status, details, content);
  };
  const auto turn_of = [&](std::string_view status, std::size_t errors = 0, std::string_view details = {}) {
    const auto events = events_of(status, details, true);
    const auto turns = all<realtime::TurnCommitted>(events);
    CHECK(turns.size() == 1 && turns[0]->turn.joined_text() == "hi" && all<realtime::ProviderError>(events).size() == errors);
    return turns[0]->turn.stop;
  };
  const auto completed = turn_of("completed"), incomplete = turn_of("incomplete");
  CHECK(completed.kind == StopKind::EndTurn && completed.raw == "completed");
  CHECK(incomplete.kind == StopKind::MaxTokens && incomplete.raw == "incomplete");
  for (const std::string_view status : {"unexpected", "Error"}) {
    const auto unexpected = turn_of(status);
    CHECK(unexpected.kind == StopKind::Unknown && unexpected.raw == status);
  }
  const auto failed = turn_of("failed", 1);
  CHECK(failed.kind == StopKind::Unknown && failed.raw == "failed");
  // An incomplete response by its reason: a blocked one does not read as a truncated one.
  const auto filtered = turn_of("incomplete", 0, R"({"type":"incomplete","reason":"content_filter"})");
  CHECK(filtered.kind == StopKind::ContentFilter && filtered.raw == "content_filter");
  const auto capped = turn_of("incomplete", 0, R"({"type":"incomplete","reason":"max_output_tokens"})");
  CHECK(capped.kind == StopKind::MaxTokens && capped.raw == "max_output_tokens");
  const auto other = turn_of("incomplete", 0, R"({"type":"incomplete","reason":"future_reason"})");
  CHECK(other.kind == StopKind::Unknown && other.raw == "future_reason");
  // Without content as well, a stop the codec cannot name ends a turn, so it is delivered. An empty cancelled or
  // failed response commits none, and neither does an empty incomplete one whose reason the codec names.
  const auto unnamed_events = events_of("unexpected", {}, false);
  const auto unnamed = all<realtime::TurnCommitted>(unnamed_events);
  CHECK(unnamed.size() == 1 && unnamed[0]->turn.stop.kind == StopKind::Unknown && unnamed[0]->turn.stop.raw == "unexpected");
  const auto reason_events = events_of("incomplete", R"({"type":"incomplete","reason":"future_reason"})", false);
  const auto unnamed_reason = all<realtime::TurnCommitted>(reason_events);
  CHECK(unnamed_reason.size() == 1 && unnamed_reason[0]->turn.stop.kind == StopKind::Unknown && unnamed_reason[0]->turn.stop.raw == "future_reason");
  CHECK(all<realtime::TurnCommitted>(events_of("cancelled", {}, false)).empty() && all<realtime::TurnAborted>(events_of("cancelled", {}, false)).size() == 1);
  CHECK(all<realtime::TurnCommitted>(events_of("failed", {}, false)).empty() && all<realtime::ProviderError>(events_of("failed", {}, false)).size() == 1);
  for (const std::string_view reason : {"content_filter", "max_output_tokens"})
    CHECK(all<realtime::TurnCommitted>(events_of("incomplete", R"({"type":"incomplete","reason":")" + std::string(reason) + "\"}", false)).empty());
  CHECK(all<realtime::TurnCommitted>(events_of("incomplete", {}, false)).empty());
  // A failure-class status other than the lifecycle's own is the provider's error, with or without content, and
  // commits no turn; what the response carried does not reach the next turn.
  for (const bool content : {false, true}) {
    auto codec = realtime::make_codec(d);
    const auto erred = done(*codec, "error", {}, content);
    CHECK(all<realtime::TurnCommitted>(erred).empty() && all<realtime::ProviderError>(erred).size() == 1);
    CHECK(std::get<std::vector<realtime::Event>>(codec->decode(false, R"({"type":"response.output_text.delta","delta":"next"})")).size() == 1);
    const auto next_events = done(*codec, "completed", {}, false);
    const auto next = all<realtime::TurnCommitted>(next_events);
    CHECK(next.size() == 1 && next[0]->turn.joined_text() == "next" && next[0]->turn.stop.kind == StopKind::EndTurn);
  }
}
// A turn's usage keeps what the provider reports beside the totals.
void usage_details() {
  {
    auto codec = realtime::make_codec(descriptor::presets::google_generate());
    std::get<std::vector<realtime::Event>>(codec->decode(false, R"({"serverContent":{"modelTurn":{"parts":[{"text":"hi"}]}}})"));
    const auto events = std::get<std::vector<realtime::Event>>(codec->decode(false, R"({"serverContent":{"turnComplete":true},"usageMetadata":{
        "promptTokenCount":50,"cachedContentTokenCount":10,"responseTokenCount":6,"totalTokenCount":56,"thoughtsTokenCount":4,"toolUsePromptTokenCount":3,
        "promptTokensDetails":[{"modality":"AUDIO","tokenCount":30},{"modality":"TEXT","tokenCount":20}],"cacheTokensDetails":[{"modality":"TEXT","tokenCount":10}],
        "responseTokensDetails":[{"modality":"TEXT","tokenCount":6}],"toolUsePromptTokensDetails":[{"modality":"TEXT","tokenCount":3},{"modality":7},{"tokenCount":1}]}})"));
    const auto& usage = all<realtime::TurnCommitted>(events).at(0)->turn.usage;
    CHECK(usage.input_total->value == 50 && usage.output_total->value == 6 && usage.total->value == 56 && usage.cache_read->value == 10);
    CHECK(usage.extra.at("thoughtsTokenCount").value == 4 && usage.extra.at("toolUsePromptTokenCount").value == 3 &&
          usage.extra.at("promptTokensDetails.AUDIO").value == 30 && usage.extra.at("promptTokensDetails.TEXT").value == 20 &&
          usage.extra.at("cacheTokensDetails.TEXT").value == 10 && usage.extra.at("responseTokensDetails.TEXT").value == 6 &&
          usage.extra.at("toolUsePromptTokensDetails.TEXT").value == 3);
    CHECK(usage.extra.size() == 7);
    // A turn without a usage message has none, and one turn's usage does not follow it into the next.
    std::get<std::vector<realtime::Event>>(codec->decode(false, R"({"serverContent":{"modelTurn":{"parts":[{"text":"again"}]}}})"));
    const auto next = std::get<std::vector<realtime::Event>>(codec->decode(false, R"({"serverContent":{"turnComplete":true}})"));
    const auto& bare = all<realtime::TurnCommitted>(next).at(0)->turn.usage;
    CHECK(!bare.input_total && bare.extra.empty());
  }
  {
    auto codec = realtime::make_codec(descriptor::presets::openai_chat());
    const auto events = std::get<std::vector<realtime::Event>>(codec->decode(false, R"({"type":"response.done","response":{"id":"r","status":"completed","usage":{
        "total_tokens":30,"input_tokens":20,"output_tokens":10,"input_token_details":{"text_tokens":5,"audio_tokens":9,"image_tokens":6,"cached_tokens":4,
        "cached_tokens_details":{"text_tokens":1,"audio_tokens":3}},"output_token_details":{"text_tokens":2,"audio_tokens":8,"reasoning_tokens":1}}}})"));
    const auto& usage = all<realtime::TurnCommitted>(events).at(0)->turn.usage;
    CHECK(usage.input_total->value == 20 && usage.output_total->value == 10 && usage.total->value == 30 && usage.cache_read->value == 4 &&
          usage.reasoning->value == 1);
    CHECK(usage.extra.at("input_token_details.text_tokens").value == 5 && usage.extra.at("input_token_details.audio_tokens").value == 9 &&
          usage.extra.at("input_token_details.image_tokens").value == 6 && usage.extra.at("input_token_details.cached_tokens_details.audio_tokens").value == 3 &&
          usage.extra.at("input_token_details.cached_tokens_details.text_tokens").value == 1 && usage.extra.at("output_token_details.text_tokens").value == 2 &&
          usage.extra.at("output_token_details.audio_tokens").value == 8);
    CHECK(usage.extra.size() == 7);
  }
  {
    // A transcription that bills tokens reports them by modality too.
    realtime::Setup setup;
    setup.model = "gpt-live-transcribe";
    setup.output = {};
    auto codec = realtime::make_codec(descriptor::presets::openai_chat());
    frames_of(codec->open(setup));
    const auto events = std::get<std::vector<realtime::Event>>(codec->decode(false, R"({"type":"conversation.item.input_audio_transcription.completed","item_id":"i","transcript":"hi",
        "usage":{"type":"tokens","total_tokens":14,"input_tokens":12,"input_token_details":{"text_tokens":0,"audio_tokens":12},"output_tokens":2}})"));
    const auto& usage = all<realtime::TurnCommitted>(events).at(0)->turn.usage;
    CHECK(usage.input_total->value == 12 && usage.output_total->value == 2 && usage.extra.at("input_token_details.audio_tokens").value == 12);
  }
}

// A tool-call turn ends with a message of its own that carries its usage: Google Live sent `toolCall`, then
// `generationComplete`, then `turnComplete` with `usageMetadata` on 2026-10-20. The turn commits there, with the usage
// of the turn that made the calls, and the answer that follows has the usage of its own.
void tool_turn_usage() {
  const auto d = descriptor::presets::google_generate();
  const auto feed = [](realtime::Codec& codec, std::string_view message) { return std::get<std::vector<realtime::Event>>(codec.decode(false, message)); };
  const std::string call = R"({"toolCall":{"functionCalls":[{"name":"get_weather","args":{"city":"Seoul"},"id":"call-1"}]}})";
  const std::string audio = R"({"serverContent":{"modelTurn":{"parts":[{"inlineData":{"data":"AAE=","mimeType":"audio/pcm;rate=24000"}}]}}})";
  {
    auto codec = realtime::make_codec(d);
    feed(*codec, R"({"setupComplete":{}})");
    const auto calls = feed(*codec, call);
    CHECK(all<realtime::ToolCallEvent>(calls).size() == 1 && all<realtime::TurnCommitted>(calls).empty());
    CHECK(feed(*codec, R"({"serverContent":{"generationComplete":true}})").empty());
    const auto ended = feed(*codec, R"({"serverContent":{"turnComplete":true},"usageMetadata":{"promptTokenCount":609,"responseTokenCount":12,"totalTokenCount":621,
        "promptTokensDetails":[{"modality":"TEXT","tokenCount":338},{"modality":"AUDIO","tokenCount":222}],"thoughtsTokenCount":56}})");
    const auto turns = all<realtime::TurnCommitted>(ended);
    CHECK(turns.size() == 1 && turns[0]->turn.stop.kind == StopKind::ToolUse && turns[0]->turn.usage.input_total->value == 609 &&
          turns[0]->turn.usage.output_total->value == 12 && turns[0]->turn.usage.extra.at("promptTokensDetails.AUDIO").value == 222);
    CHECK(std::holds_alternative<ToolCall>(turns[0]->turn.messages.at(0).parts.at(0)));
    feed(*codec, audio);
    const auto finished = feed(*codec, R"({"serverContent":{"turnComplete":true},"usageMetadata":{"promptTokenCount":700,"responseTokenCount":30,"totalTokenCount":730}})");
    const auto answer = all<realtime::TurnCommitted>(finished);
    CHECK(answer.size() == 1 && answer[0]->turn.stop.kind == StopKind::EndTurn && answer[0]->turn.usage.input_total->value == 700 && answer[0]->turn.usage.extra.empty());
    CHECK(std::get<std::vector<realtime::Event>>(codec->finish()).empty());
  }
  {
    // No ending message: the content of the next turn closes the held one, ahead of it, without usage.
    auto codec = realtime::make_codec(d);
    feed(*codec, call);
    const auto next = feed(*codec, audio);
    CHECK(next.size() == 2 && std::holds_alternative<realtime::TurnCommitted>(next[0]) && std::holds_alternative<realtime::AudioDelta>(next[1]));
    const auto& held = std::get<realtime::TurnCommitted>(next[0]).turn;
    CHECK(held.stop.kind == StopKind::ToolUse && !held.usage.input_total);
    // Another batch of calls is another turn.
    auto again = realtime::make_codec(d);
    feed(*again, call);
    const auto second = feed(*again, call);
    CHECK(second.size() == 2 && std::holds_alternative<realtime::TurnCommitted>(second[0]) && std::holds_alternative<realtime::ToolCallEvent>(second[1]));
    // The user speaking over the model closes it, ahead of the abort.
    auto spoken = realtime::make_codec(d);
    feed(*spoken, call);
    const auto cut = feed(*spoken, R"({"serverContent":{"interrupted":true}})");
    CHECK(cut.size() == 2 && std::holds_alternative<realtime::TurnCommitted>(cut[0]) && std::holds_alternative<realtime::TurnAborted>(cut[1]));
  }
  {
    // The end of the session settles a turn still waiting, once.
    auto codec = realtime::make_codec(d);
    feed(*codec, call);
    const auto tail = std::get<std::vector<realtime::Event>>(codec->finish());
    CHECK(tail.size() == 1 && std::get<realtime::TurnCommitted>(tail[0]).turn.stop.kind == StopKind::ToolUse);
    CHECK(std::get<std::vector<realtime::Event>>(codec->finish()).empty());
  }
}

// The audio formats come from the endpoint's fact.
void audio_formats() {
  const auto session = [](const descriptor::ValidatedDescriptor& d, std::string input, std::string output) {
    realtime::Setup setup;
    setup.model = "model-x";
    setup.input_audio = std::move(input);
    setup.output_audio = std::move(output);
    auto codec = realtime::make_codec(d);
    auto opened = frames_of(codec->open(setup));
    return std::make_pair(std::move(codec), std::move(opened));
  };
  {
    const auto d = descriptor::presets::openai_chat();
    auto [codec, opened] = session(d, {}, {});
    const auto first = document(opened.at(0));
    const auto update = first->root().get("session").get("audio");
    CHECK(update.get("input").get("format").dump() == R"({"type":"audio/pcm","rate":24000})" && update.get("output").get("format").dump() == R"({"type":"audio/pcm","rate":24000})");
    CHECK(codec->input_rate() == 24000 && codec->input_bytes_per_second() == 48000);
    const auto delta = std::get<std::vector<realtime::Event>>(codec->decode(false, R"({"type":"response.output_audio.delta","delta":"AAE="})"));
    CHECK(std::get<realtime::AudioDelta>(delta.at(0)).mime == "audio/pcm;rate=24000");
    // G.711 has no rate on the wire and a byte per sample.
    auto [g711, chosen] = session(d, "audio/pcmu", "audio/pcma");
    const auto second = document(chosen.at(0));
    const auto selected = second->root().get("session").get("audio");
    CHECK(selected.get("input").get("format").dump() == R"({"type":"audio/pcmu"})" && selected.get("output").get("format").dump() == R"({"type":"audio/pcma"})");
    CHECK(g711->input_rate() == 8000 && g711->input_bytes_per_second() == 8000);
    const auto law = std::get<std::vector<realtime::Event>>(g711->decode(false, R"({"type":"response.output_audio.delta","delta":"AAE="})"));
    CHECK(std::get<realtime::AudioDelta>(law.at(0)).mime == "audio/pcma");
    // A transcription session takes the input format.
    realtime::Setup transcription;
    transcription.model = "model-x";
    transcription.output = {};
    transcription.input_audio = "audio/pcmu";
    const auto opened_transcription = frames_of(realtime::make_codec(d)->open(transcription));
    CHECK(document(opened_transcription.at(0))->root().get("session").get("audio").get("input").get("format").dump() == R"({"type":"audio/pcmu"})");
    // A format the fact does not list is refused, in the setup check and by the codec.
    realtime::Setup foreign;
    foreign.model = "model-x";
    foreign.input_audio = "audio/pcm;rate=16000";
    CHECK(std::get<Error>(realtime::connect(d, foreign, "key")).reason == "request.realtime.audio_format_invalid");
    CHECK(std::holds_alternative<Error>(realtime::make_codec(d)->open(foreign)));
    foreign.input_audio.clear();
    foreign.output_audio = "audio/pcm;rate=16000";
    CHECK(std::get<Error>(realtime::connect(d, foreign, "key")).reason == "request.realtime.audio_format_invalid");
  }
  {
    // Google has one input format; the chunk names it.
    const auto d = descriptor::presets::google_generate();
    auto [codec, opened] = session(d, {}, {});
    CHECK(codec->input_rate() == 16000 && codec->input_bytes_per_second() == 32000);
    CHECK(document(frames_of(codec->encode(realtime::Audio{"ab"})).at(0))->root().get("realtimeInput").get("audio").get("mimeType").as_string() == "audio/pcm;rate=16000");
    realtime::Setup foreign;
    foreign.model = "model-x";
    foreign.input_audio = "audio/pcmu";
    CHECK(std::get<Error>(realtime::connect(d, foreign, "key")).reason == "request.realtime.audio_format_invalid");
  }
  {
    // The fact decides: another rate for Google is a change of the fact, not of the code.
    auto policy = policy_with(R"([{"endpoint":"realtime","shape":"google","origins":["https://generativelanguage.googleapis.com"],"paths":{"connect":"/ws/live"},"auth":"x-goog-api-key",
        "audio":{"input":[{"mime":"audio/pcm;rate=8000","rate":8000,"sample_bytes":2},{"mime":"audio/pcm;rate=48000","rate":48000,"sample_bytes":2}],
                 "output":[{"mime":"audio/pcm;rate=24000","rate":24000,"sample_bytes":2}]}}])");
    CHECK(std::holds_alternative<descriptor::PolicySnapshot>(policy));
    descriptor::presets::PresetOptions options;
    options.policy = std::get<descriptor::PolicySnapshot>(policy);
    const auto d = descriptor::presets::google_generate(std::move(options));
    auto [codec, opened] = session(d, {}, {});
    CHECK(codec->input_rate() == 8000 && codec->input_bytes_per_second() == 16000);
    CHECK(document(frames_of(codec->encode(realtime::Audio{"ab"})).at(0))->root().get("realtimeInput").get("audio").get("mimeType").as_string() == "audio/pcm;rate=8000");
    auto [wide, wide_opened] = session(d, "audio/pcm;rate=48000", {});
    CHECK(wide->input_rate() == 48000 && wide->input_bytes_per_second() == 96000);
    CHECK(document(frames_of(wide->encode(realtime::Audio{"ab"})).at(0))->root().get("realtimeInput").get("audio").get("mimeType").as_string() == "audio/pcm;rate=48000");
  }
}

// The facts a realtime endpoint carries are checked when the policy loads.
void policy_facts() {
  const auto refused_at = [](std::string_view endpoints, std::string_view pointer) {
    const auto loaded = policy_with(endpoints);
    CHECK(std::holds_alternative<descriptor::ConfigError>(loaded) && std::get<descriptor::ConfigError>(loaded).pointer == pointer);
  };
  const auto fact = [](std::string_view audio, std::string_view more = {}) {
    return R"([{"endpoint":"realtime","shape":"openai","origins":["https://api.openai.com"],"paths":{"connect":"/v1/realtime"})" + std::string(audio) + std::string(more) + "}]";
  };
  const std::string good = R"(,"audio":{"input":[{"mime":"audio/pcm;rate=24000","rate":24000,"sample_bytes":2}],"output":[{"mime":"audio/pcmu","rate":8000,"sample_bytes":1}]})";
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(policy_with(fact(good))));
  const auto format = [](std::string_view mime, std::string_view rate = "24000", std::string_view width = "2") {
    return R"(,"audio":{"input":[{"mime":")" + std::string(mime) + R"(","rate":)" + std::string(rate) + R"(,"sample_bytes":)" + std::string(width) +
           R"(}],"output":[{"mime":"audio/pcmu","rate":8000,"sample_bytes":1}]})";
  };
  refused_at(fact(""), "/endpoints/audio");
  refused_at(fact(R"(,"audio":{"input":[],"output":[]})"), "/endpoints/audio/input");
  refused_at(fact(R"(,"audio":{"input":[{"mime":"audio/pcmu","rate":8000,"sample_bytes":1}]})"), "/endpoints/audio");
  refused_at(fact(format("audio/pcm;rate=16000")), "/endpoints/audio/input");   // the parameter is not the rate
  refused_at(fact(format("audio/pcm;codec=x")), "/endpoints/audio/input");
  refused_at(fact(format("text/plain")), "/endpoints/audio/input");
  refused_at(fact(format("Audio/PCM")), "/endpoints/audio/input");
  refused_at(fact(format("audio/pcm", "0")), "/endpoints/audio/input");
  refused_at(fact(format("audio/pcm", "24000", "0")), "/endpoints/audio/input");
  refused_at(fact(format("audio/pcm", "24000", "5")), "/endpoints/audio/input");
  refused_at(fact(R"(,"audio":{"input":[{"mime":"audio/pcmu","rate":8000,"sample_bytes":1},{"mime":"audio/pcmu","rate":8000,"sample_bytes":1}],"output":[{"mime":"audio/pcmu","rate":8000,"sample_bytes":1}]})"),
             "/endpoints/audio/input");
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(policy_with(fact(good, R"(,"close_codes":[{"code":3000,"status":401},{"code":1007,"reason":"x","status":401}])"))));
  refused_at(fact(good, R"(,"close_codes":[{"code":999,"status":401}])"), "/endpoints/close_codes");
  refused_at(fact(good, R"(,"close_codes":[{"code":3000,"status":99}])"), "/endpoints/close_codes");
  refused_at(fact(good, R"(,"close_codes":[{"code":3000,"status":401},{"code":3000,"status":403}])"), "/endpoints/close_codes");
  refused_at(fact(good, R"(,"close_codes":[{"code":3000}])"), "/endpoints/close_codes");
  // They belong to a realtime endpoint.
  refused_at(R"([{"endpoint":"images","shape":"openai","origins":["https://api.openai.com"],"paths":{"generate":"/v1/images"})" + good + "}]", "/endpoints");
  // The handshake sets its own headers, so a fact may not name them.
  for (const auto name : {"Upgrade", "Sec-WebSocket-Key", "Sec-WebSocket-Version", "Sec-WebSocket-Extensions", "Sec-WebSocket-Protocol", "sec-websocket-accept"})
    refused_at(fact(good, std::string(R"(,"headers":{")") + name + R"(":"x"})"), "/endpoints/headers");
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(policy_with(fact(good, R"(,"headers":{"OpenAI-Beta":"realtime=v1"})"))));
  // The shipped facts pin the formats.
  const auto* google = descriptor::builtin_policy()->endpoint("realtime", "https://generativelanguage.googleapis.com");
  CHECK(google && google->audio_input.size() == 1 && google->audio_input[0].mime == "audio/pcm;rate=16000" && google->audio_output.at(0).rate == 24000);
  const auto* openai = descriptor::builtin_policy()->endpoint("realtime", "https://api.openai.com");
  CHECK(openai && openai->audio_input.size() == 3 && openai->audio_input.at(0).mime == "audio/pcm;rate=24000" && openai->audio_input.at(1).mime == "audio/pcmu" &&
        openai->audio_input.at(1).sample_bytes == 1);
  CHECK(google->close_codes.size() == 1 && google->close_codes[0].code == 1007 && google->close_codes[0].status == 400 &&
        openai->close_codes.size() == 1 && openai->close_codes[0].code == 3000 && openai->close_codes[0].status == 401);
}
}  // namespace

// Transcription sessions (no output modalities). The server frames were recorded live on 2026-10-20, ids shortened.
void transcription() {
  realtime::Setup setup;
  setup.output = {};
  {
    const auto d = descriptor::presets::google_generate();
    setup.model = "gemini-3.5-transcribe-live";
    auto codec = realtime::make_codec(d);
    const auto opened = frames_of(codec->open(setup));
    CHECK(opened.size() == 1 && canonical(opened[0], true) ==
          canonical(R"({"setup":{"model":"models/gemini-3.5-transcribe-live","generationConfig":{"responseModalities":["TEXT"]},"inputAudioTranscription":{}}})", true));
    const auto events = inbound(d, "google-transcription-server.jsonl", codec.get());
    const auto transcripts = all<realtime::Transcript>(events);
    CHECK(transcripts.size() == 3 && transcripts[0]->partial && transcripts[0]->text == "What is the" && transcripts[1]->partial &&
          !transcripts[2]->partial && transcripts[2]->input && transcripts[2]->text == "What is the capital of France?");
    const auto turns = all<realtime::TurnCommitted>(events);
    CHECK(turns.size() == 1 && turns[0]->turn.messages.size() == 1 && turns[0]->turn.messages[0].role == Role::User &&
          std::get<Text>(turns[0]->turn.messages[0].parts.at(0)).value == "What is the capital of France?" && turns[0]->turn.stop.raw == "transcribed");
    CHECK(std::holds_alternative<realtime::Connection>(realtime::connect(d, setup, "key")));
  }
  {
    const auto d = descriptor::presets::openai_chat();
    setup.model = "gpt-live-transcribe";
    const auto connection = std::get<realtime::Connection>(realtime::connect(d, setup, "key"));
    CHECK(connection.url == "wss://api.openai.com/v1/realtime?intent=transcription");
    auto codec = realtime::make_codec(d);
    const auto opened = frames_of(codec->open(setup));
    CHECK(opened.size() == 1 && canonical(opened[0], false) ==
          canonical(R"({"type":"session.update","session":{"type":"transcription","audio":{"input":{"format":{"type":"audio/pcm","rate":24000},"transcription":{"model":"gpt-live-transcribe"},"turn_detection":null}}}})", false));
    // The host ends the utterance.
    CHECK(frames_of(codec->encode(realtime::EndAudio{})) == std::vector<std::string>{R"({"type":"input_audio_buffer.commit"})"});
    const auto events = inbound(d, "openai-transcription-server.jsonl", codec.get());
    const auto transcripts = all<realtime::Transcript>(events);
    CHECK(transcripts.size() == 4 && transcripts[0]->partial && transcripts[0]->text == " What" && transcripts[1]->text == " What is the" &&
          transcripts[2]->text == " What is the capital of France?" && !transcripts[3]->partial && transcripts[3]->text == "What is the capital of France?");
    const auto turns = all<realtime::TurnCommitted>(events);
    CHECK(turns.size() == 1 && turns[0]->turn.messages.at(0).role == Role::User && turns[0]->turn.usage.extra.at("seconds").value == 4 &&
          turns[0]->turn.usage.stage == UsageStage::Final);
  }
  // A transcription session answers nothing, so it takes none of what shapes an answer.
  for (const auto& change : std::vector<std::function<void(realtime::Setup&)>>{
           [](realtime::Setup& s) { s.voice = "alloy"; }, [](realtime::Setup& s) { s.instructions = "Be brief."; },
           [](realtime::Setup& s) { s.output_transcription = true; }, [](realtime::Setup& s) { s.tools.push_back(weather("{}")); },
           [](realtime::Setup& s) { s.transcription_model = "x"; }, [](realtime::Setup& s) { s.output_audio = "audio/pcm;rate=24000"; }}) {
    auto refused = setup;
    change(refused);
    const auto result = realtime::connect(descriptor::presets::openai_chat(), refused, "key");
    CHECK(std::holds_alternative<Error>(result) && std::get<Error>(result).reason == "request.realtime.transcription_setup_invalid");
  }
}

// A turn's usage prices at the audio rates: the estimate reads the audio counts under the names the codecs write
// (OpenAI `input_token_details.audio_tokens` and `output_token_details.audio_tokens`, Google `promptTokensDetails.AUDIO`
// and `responseTokensDetails.AUDIO`). The recordings' turns carry 100 text and 20 audio tokens in and 40 audio tokens
// out. At the rates below that is 100 x 4e-6 + 20 x 32e-6 + 40 x 64e-6 USD = 3'600'000 nano-USD; at the text rates it
// would be 1'120'000.
void turn_usage_is_priced() {
  auto loaded = descriptor::load_catalogue(R"({"version":1,"models":[
    {"id":"audio-model","pricing":{"input":"0.000004","output":"0.000016","input_audio":"0.000032","output_audio":"0.000064"}},
    {"id":"text-only-model","pricing":{"input":"0.000004","output":"0.000016"}},
    {"id":"thinking-model","pricing":{"input":"0.000004","output":"0.000016","reasoning":"0.00004"}},
    {"id":"image-model","pricing":{"input":"0.000004","output":"0.000016","input_image":"0.00001"}}]})");
  CHECK(std::holds_alternative<descriptor::CatalogueSnapshot>(loaded));
  const auto catalogue = std::get<descriptor::CatalogueSnapshot>(loaded);
  const ServedModel audio{"audio-model", "audio-model", ServedRelation::Exact}, text_only{"text-only-model", "text-only-model", ServedRelation::Exact};
  const auto openai_events = inbound(descriptor::presets::openai_chat(), "openai-server.jsonl");
  const auto openai = all<realtime::TurnCommitted>(openai_events);
  CHECK(openai.size() == 1);
  auto estimate = pricing::estimate(*catalogue, "openai.chat", "", audio, openai[0]->turn.usage);
  CHECK(estimate.total && estimate.total->nano_usd == 3'600'000u && estimate.unpriced.empty());
  // Audio without an audio rate has no total, as in a Chat usage.
  estimate = pricing::estimate(*catalogue, "openai.chat", "", text_only, openai[0]->turn.usage);
  CHECK(!estimate.total && estimate.unpriced == std::vector<std::string>({"input_audio", "output_audio"}));
  const auto google_events = inbound(descriptor::presets::google_generate(), "google-server.jsonl");
  const auto google = all<realtime::TurnCommitted>(google_events);
  CHECK(google.size() == 2);
  estimate = pricing::estimate(*catalogue, "google.generate", "", audio, google[1]->turn.usage);
  CHECK(estimate.total && estimate.total->nano_usd == 3'600'000u && estimate.unpriced.empty());
  estimate = pricing::estimate(*catalogue, "google.generate", "", text_only, google[1]->turn.usage);
  CHECK(!estimate.total && estimate.unpriced == std::vector<std::string>({"input_audio", "output_audio"}));
  // Live's thoughts are counted beside the response tokens, not within them: 100 input, 40 response and 500 thought
  // tokens bill 400'000 + 640'000 nano-USD and the thoughts at the reasoning rate 4e-5, else at the output rate 1.6e-5.
  auto live = realtime::make_codec(descriptor::presets::google_generate());
  std::get<std::vector<realtime::Event>>(live->decode(false, R"({"serverContent":{"modelTurn":{"parts":[{"text":"hi"}]}}})"));
  const auto thinking_events = std::get<std::vector<realtime::Event>>(live->decode(false,
      R"({"serverContent":{"turnComplete":true},"usageMetadata":{"promptTokenCount":100,"responseTokenCount":40,"thoughtsTokenCount":500,"totalTokenCount":140}})"));
  const auto thinking = all<realtime::TurnCommitted>(thinking_events);
  CHECK(thinking.size() == 1 && thinking[0]->turn.usage.extra.at("thoughtsTokenCount").value == 500 && !thinking[0]->turn.usage.reasoning);
  const ServedModel thinks{"thinking-model", "thinking-model", ServedRelation::Exact};
  estimate = pricing::estimate(*catalogue, "google.generate", "", thinks, thinking[0]->turn.usage);
  CHECK(estimate.total && estimate.total->nano_usd == 400'000u + 640'000u + 20'000'000u && estimate.unpriced.empty());
  estimate = pricing::estimate(*catalogue, "google.generate", "", text_only, thinking[0]->turn.usage);
  CHECK(estimate.total && estimate.total->nano_usd == 400'000u + 640'000u + 8'000'000u);
  // OpenAI realtime counts image input tokens under `input_token_details.image_tokens`: 100 of 1000 input tokens at the
  // image input rate 1e-5, or no total where the entry has none.
  auto image_turn = openai[0]->turn.usage;
  image_turn.input_total = Count{1000, Evidence::Reported};
  image_turn.extra.clear();
  image_turn.extra["input_token_details.image_tokens"] = Count{100, Evidence::Reported};
  image_turn.output_total = Count{10, Evidence::Reported};
  estimate = pricing::estimate(*catalogue, "openai.chat", "", ServedModel{"image-model", "image-model", ServedRelation::Exact}, image_turn);
  CHECK(estimate.total && estimate.total->nano_usd == 900u * 4000u + 100u * 10'000u + 10u * 16'000u);
  estimate = pricing::estimate(*catalogue, "openai.chat", "", text_only, image_turn);
  CHECK(!estimate.total && estimate.unpriced == std::vector<std::string>({"input_image"}));
  // Google Live names image tokens only in its prompt breakdown, which a Generate usage shares; the codec copies them to
  // their own counter, so they bill at `input_image` too: 100 of 1000 input tokens, and 10 output tokens.
  auto images = realtime::make_codec(descriptor::presets::google_generate());
  std::get<std::vector<realtime::Event>>(images->decode(false, R"({"serverContent":{"modelTurn":{"parts":[{"text":"hi"}]}}})"));
  const auto image_events = std::get<std::vector<realtime::Event>>(images->decode(false,
      R"({"serverContent":{"turnComplete":true},"usageMetadata":{"promptTokenCount":1000,"responseTokenCount":10,"totalTokenCount":1010,
          "promptTokensDetails":[{"modality":"TEXT","tokenCount":900},{"modality":"IMAGE","tokenCount":100}]}})"));
  const auto& live_images = all<realtime::TurnCommitted>(image_events).at(0)->turn.usage;
  CHECK(live_images.extra.at(std::string(pricing::realtime_image_tokens)).value == 100 && live_images.extra.at("promptTokensDetails.IMAGE").value == 100);
  estimate = pricing::estimate(*catalogue, "google.generate", "", ServedModel{"image-model", "image-model", ServedRelation::Exact}, live_images);
  CHECK(estimate.total && estimate.total->nano_usd == 900u * 4000u + 100u * 10'000u + 10u * 16'000u);
  estimate = pricing::estimate(*catalogue, "google.generate", "", text_only, live_images);
  CHECK(!estimate.total && estimate.unpriced == std::vector<std::string>({"input_image"}));
  // A Generate usage with the same breakdown still bills its image tokens at the input rate.
  auto generated = live_images;
  generated.extra.erase(std::string(pricing::realtime_image_tokens));
  estimate = pricing::estimate(*catalogue, "google.generate", "", text_only, generated);
  CHECK(estimate.total && estimate.total->nano_usd == 1000u * 4000u + 10u * 16'000u);
}

// The reasons of a bound, as "<kind> [<subject>]; ...".
std::string reasons_of(const pricing::EndpointBound& bound) {
  std::string out;
  for (const auto& reason : bound.unbounded) out += std::string(pricing::to_string(reason.by)) + " [" + reason.subject + "]; ";
  return out;
}
// A rate in nano-USD per million tokens, from its decimal USD per token.
std::uint64_t rate(std::string_view usd) { return *pricing::per_million(usd); }
// Rates times tokens, rounded up once to whole nano-USD.
std::uint64_t nano(std::initializer_list<std::pair<std::uint64_t, std::string_view>> lines) {
  std::uint64_t sum = 0;
  for (const auto& [tokens, usd] : lines) sum += tokens * rate(usd);
  return (sum + 999'999) / 1'000'000;
}

// The bound of one model response of a session (#124): the host states the response's input by modality, the setup
// states the output cap, and the catalogue prices each modality at its own rate. For every turn within those, the
// estimate of its usage is at most the bound.
void response_bounds() {
  auto loaded = descriptor::load_catalogue(R"({"version":1,"models":[
    {"id":"audio-model","output_limit":4096,"pricing":{"input":"0.000004","output":"0.000016","input_audio":"0.000032","output_audio":"0.000064"}},
    {"id":"text-only-model","output_limit":4096,"pricing":{"input":"0.000004","output":"0.000016"}},
    {"id":"thinking-model","output_limit":4096,"pricing":{"input":"0.000004","output":"0.000016","reasoning":"0.00004"}},
    {"id":"reasoning-audio-model","output_limit":4096,"pricing":{"input":"0.000004","output":"0.000016","input_audio":"0.000032","output_audio":"0.000064","reasoning":"0.0001"}},
    {"id":"dense-model","output_limit":4096,"pricing":{"input":"0.000004","input+1000":"0.000008","cache_read":"0.000001","output":"0.000016","input_audio":"0.000032",
                                                       "input_image":"0.00001","output_audio":"0.000064","reasoning":"0.00004","request":"0.0001"}},
    {"id":"dear-cache-model","output_limit":4096,"pricing":{"input":"0.000004","cache_read":"0.000006","output":"0.000016","input_audio":"0.000032",
                                                            "input_image":"0.000002","output_audio":"0.000064"}},
    {"id":"variable-audio-model","output_limit":4096,"pricing":{"input":"0.000004","output":"0.000016","input_audio":"-1","output_audio":"0.000064"}},
    {"id":"seconds-audio-model","output_limit":4096,"pricing":{"input":"0.000004","output":"0.000016","input_audio":"0.000032","output_audio":"0.000064","item:second":"0.0001"}},
    {"id":"ceiling-model","output_limit":4096,"pricing":{"input":"0.000004","output":"0.000016","input_audio":"0.000032","output_audio":"0.000064","ceiling_input":"0.0001","ceiling_output":"0.0001"}},
    {"id":"gpt-live-transcribe","pricing":{"input":"0","output":"0","item:second":"0.0001"}},
    {"id":"gpt-realtime-2.1-mini","output_limit":4096,"pricing":{"input":"0.000004","output":"0.000016","input_audio":"0.000032","output_audio":"0.000064"}},
    {"id":"tokens-transcribe","input_limit":1000,"output_limit":100,"pricing":{"input":"0.000004","output":"0.000016"}}]})");
  CHECK(std::holds_alternative<descriptor::CatalogueSnapshot>(loaded));
  const auto& catalogue = *std::get<descriptor::CatalogueSnapshot>(loaded);
  const auto openai = descriptor::presets::openai_chat();
  const auto google = descriptor::presets::google_generate();
  const auto served = [](std::string model) { return ServedModel{model, model, ServedRelation::Exact}; };
  const auto capped = [](std::string model, std::string_view cap, std::vector<realtime::Modality> output = {realtime::Modality::Audio}) {
    realtime::Setup setup;
    setup.model = std::move(model);
    setup.output = std::move(output);
    // Only the host starts a response.
    setup.turn_detection = false;
    if (!cap.empty()) setup.extensions = document(R"({"max_output_tokens":)" + std::string(cap) + "}");
    return setup;
  };
  const auto stated = [](std::uint64_t text, std::uint64_t audio, std::uint64_t image = 0) {
    realtime::ResponseInput input;
    input.input_tokens = pricing::RealtimeInputTokens{text, audio, image};
    return input;
  };

  // The policy names the option that caps a response's output: OpenAI's, and none at Google. It names a property of
  // the fact's options that admits an integer, on a realtime fact.
  CHECK(descriptor::builtin_policy()->endpoint("realtime", "https://api.openai.com")->output_cap == "max_output_tokens");
  CHECK(descriptor::builtin_policy()->endpoint("realtime", "https://generativelanguage.googleapis.com")->output_cap.empty());
  {
    const auto fact = [](std::string_view more) {
      return R"([{"endpoint":"realtime","shape":"openai","origins":["https://api.openai.com"],"paths":{"connect":"/v1/realtime"},)"
             R"("audio":{"input":[{"mime":"audio/pcmu","rate":8000,"sample_bytes":1}],"output":[{"mime":"audio/pcmu","rate":8000,"sample_bytes":1}]},)"
             R"("options":{"type":"object","properties":{"cap":{"type":"integer","minimum":1},"word":{"type":"string","max_length":8}}})" + std::string(more) + "}]";
    };
    const auto refused_at = [](const std::string& endpoints, std::string_view pointer) {
      const auto result = policy_with(endpoints);
      CHECK(std::holds_alternative<descriptor::ConfigError>(result) && std::get<descriptor::ConfigError>(result).pointer == pointer);
    };
    const auto accepted = policy_with(fact(R"(,"output_cap":"cap")"));
    CHECK(std::holds_alternative<descriptor::PolicySnapshot>(accepted) &&
          std::get<descriptor::PolicySnapshot>(accepted)->endpoint("realtime", "https://api.openai.com")->output_cap == "cap");
    refused_at(fact(R"(,"output_cap":"word")"), "/endpoints/output_cap");
    refused_at(fact(R"(,"output_cap":"other")"), "/endpoints/output_cap");
    refused_at(fact(R"(,"output_cap":1)"), "/endpoints/output_cap");
    refused_at(R"([{"endpoint":"images","shape":"openai","origins":["https://api.openai.com"],"paths":{"generate":"/v1/images"},"output_cap":"cap"}])",
               "/endpoints/output_cap");
  }

  // The recorded OpenAI turn: 100 text and 20 audio tokens in, 40 audio tokens out. Stated as its input, with a cap of
  // 40, the bound is that turn's estimate exactly: 100 x 4e-6 + 20 x 32e-6 + 40 x 64e-6 USD. The entry's output limit
  // (4096) is not the cap.
  const auto openai_turn = all<realtime::TurnCommitted>(inbound(openai, "openai-server.jsonl")).at(0)->turn.usage;
  auto bound = realtime::response_bound(openai, capped("audio-model", "40"), stated(100, 20), catalogue);
  CHECK(bound.total && bound.total->nano_usd == 3'600'000u && bound.unbounded.empty() && bound.entry == "audio-model" && bound.attempts == 1);
  CHECK(bound.lines.size() == 3 && bound.lines[0].unit == "input" && bound.lines[1].unit == "input_audio" && bound.lines[1].quantity == 20 &&
        bound.lines[1].rate == rate("0.000032") && bound.lines[2].unit == "output" && bound.lines[2].row == "output_audio" && bound.lines[2].quantity == 40);
  auto estimate = pricing::estimate(catalogue, "openai.chat", "https://api.openai.com", served("audio-model"), openai_turn);
  CHECK(estimate.total && estimate.total->nano_usd <= bound.total->nano_usd);
  // A text-only session bills no audio output: the cap at the output rate.
  bound = realtime::response_bound(openai, capped("audio-model", "40", {realtime::Modality::Text}), stated(100, 20), catalogue);
  CHECK(bound.total && bound.total->nano_usd == nano({{100, "0.000004"}, {20, "0.000032"}, {40, "0.000016"}}));
  // The recorded Google turn, through the pure function with a cap the policy does not admit there: the same.
  const auto google_turn = all<realtime::TurnCommitted>(inbound(google, "google-server.jsonl")).at(1)->turn.usage;
  pricing::RealtimeResponseInput live;
  live.family = "google.generate";
  live.origin = "https://generativelanguage.googleapis.com";
  live.model = "audio-model";
  live.audio_output = true;
  live.output_tokens = 40;
  live.thinking_beside_output = true;
  live.thinking_tokens = 0;
  live.input_tokens = pricing::RealtimeInputTokens{100, 20, 0};
  bound = pricing::realtime_response_bound(live, catalogue);
  estimate = pricing::estimate(catalogue, "google.generate", live.origin, served("audio-model"), google_turn);
  CHECK(bound.total && bound.total->nano_usd == 3'600'000u && estimate.total && estimate.total->nano_usd <= bound.total->nano_usd);

  // Reasoning the output total counts bills at the reasoning rate, the dearest output rate here: 40 reasoning tokens
  // of an OpenAI turn at 1e-4 USD.
  auto reasoned = openai_turn;
  reasoned.extra.clear();
  reasoned.reasoning = Count{40, Evidence::Reported};
  bound = realtime::response_bound(openai, capped("reasoning-audio-model", "40"), stated(100, 20), catalogue);
  estimate = pricing::estimate(catalogue, "openai.chat", "https://api.openai.com", served("reasoning-audio-model"), reasoned);
  CHECK(bound.total && bound.total->nano_usd == nano({{100, "0.000004"}, {20, "0.000032"}, {40, "0.0001"}}));
  CHECK(estimate.total && estimate.total->nano_usd == nano({{120, "0.000004"}, {40, "0.0001"}}) && estimate.total->nano_usd <= bound.total->nano_usd);
  // Google Live's thoughts bill beside the output: 100 in, 40 out and 500 thoughts, against a thinking cap of 500.
  live.model = "thinking-model";
  live.audio_output = false;
  live.input_tokens = pricing::RealtimeInputTokens{100, 0, 0};
  live.thinking_tokens = 500;
  auto live_turn = std::get<std::vector<realtime::Event>>(realtime::make_codec(google)->decode(false,
      R"({"serverContent":{"modelTurn":{"parts":[{"text":"hi"}]},"turnComplete":true},"usageMetadata":{"promptTokenCount":100,"responseTokenCount":40,"thoughtsTokenCount":500,"totalTokenCount":140}})"));
  const auto thinking_turn = all<realtime::TurnCommitted>(live_turn).at(0)->turn.usage;
  bound = pricing::realtime_response_bound(live, catalogue);
  estimate = pricing::estimate(catalogue, "google.generate", live.origin, served("thinking-model"), thinking_turn);
  CHECK(bound.total && bound.total->nano_usd == nano({{100, "0.000004"}, {40, "0.00004"}, {500, "0.00004"}}));
  CHECK(estimate.total && estimate.total->nano_usd == 21'040'000u && estimate.total->nano_usd <= bound.total->nano_usd);
  live.thinking_tokens.reset();
  CHECK(reasons_of(pricing::realtime_response_bound(live, catalogue)) == "OutputCap [reasoning]; ");

  // No cap: none set, OpenAI's "inf", and Google, where the policy admits none and thinking has no cap either.
  CHECK(reasons_of(realtime::response_bound(openai, capped("audio-model", ""), stated(100, 20), catalogue)) == "OutputCap []; ");
  CHECK(reasons_of(realtime::response_bound(openai, capped("audio-model", R"("inf")"), stated(100, 20), catalogue)) == "OutputCap []; ");
  realtime::Setup google_setup;
  google_setup.model = "audio-model";
  bound = realtime::response_bound(google, google_setup, stated(100, 20), catalogue);
  CHECK(!bound.total && reasons_of(bound) == "OutputCap []; OutputCap [reasoning]; Unmodeled [realtimeInputConfig.automaticActivityDetection.disabled]; " &&
        bound.lines.empty() && bound.model.empty());
  google_setup.extensions = document(R"({"realtimeInputConfig":{"automaticActivityDetection":{"disabled":true}}})");
  CHECK(reasons_of(realtime::response_bound(google, google_setup, stated(100, 20), catalogue)) == "OutputCap []; OutputCap [reasoning]; ");
  // With tools, Live may bill a tool-use prompt beside the input, which no price covers.
  google_setup.tools.push_back(weather("{}"));
  CHECK(reasons_of(realtime::response_bound(google, google_setup, stated(100, 20), catalogue)) ==
        "OutputCap []; OutputCap [reasoning]; Unmodeled [toolUsePromptTokenCount]; ");
  // Google's own switch is the option; the typed member is refused there.
  auto typed_off = google_setup;
  typed_off.turn_detection = false;
  CHECK(std::get<Error>(realtime::connect(google, typed_off, "key")).reason == "request.realtime.turn_detection_unsupported");
  CHECK(std::get<Error>(realtime::make_codec(google)->open(typed_off)).reason == "request.realtime.turn_detection_unsupported");
  // OpenAI with its turn detection on: one input may start several responses.
  auto detected = capped("audio-model", "40");
  detected.turn_detection = true;
  bound = realtime::response_bound(openai, detected, stated(100, 20), catalogue);
  CHECK(!bound.total && reasons_of(bound) == "Unmodeled [turn_detection]; " && bound.lines.empty() && bound.model.empty() && bound.attempts == 0);
  // Off, the session update says so.
  {
    auto off = realtime::make_codec(openai);
    const auto frames = frames_of(off->open(capped("audio-model", "40")));
    CHECK(document(frames.at(0))->root().get("session").get("audio").get("input").get("turn_detection").is_null());
    auto on = realtime::make_codec(openai);
    CHECK(!document(frames_of(on->open(detected)).at(0))->root().get("session").get("audio").get("input").get("turn_detection").valid());
  }
  // A cap of zero is no cap.
  {
    auto zero = live;
    zero.output_tokens = 0;
    zero.thinking_tokens = 0;
    CHECK(reasons_of(pricing::realtime_response_bound(zero, catalogue)) == "OutputCap []; ");
  }
  // A modality with tokens and no rate of its own is not priced at the text rates.
  CHECK(reasons_of(realtime::response_bound(openai, capped("text-only-model", "40", {realtime::Modality::Text}), stated(100, 20), catalogue)) == "ModelPrice [input_audio]; ");
  CHECK(reasons_of(realtime::response_bound(openai, capped("text-only-model", "40"), stated(100, 0), catalogue)) == "ModelPrice [output_audio]; ");
  CHECK(reasons_of(realtime::response_bound(openai, capped("audio-model", "40"), stated(100, 0, 10), catalogue)) == "ModelPrice [input_image]; ");
  CHECK(reasons_of(realtime::response_bound(openai, capped("variable-audio-model", "40"), stated(100, 20), catalogue)) == "ModelPrice [input_audio]; ");
  CHECK(realtime::response_bound(openai, capped("variable-audio-model", "40"), stated(100, 0), catalogue).total);
  // Audio stated in seconds: no fact converts seconds to tokens.
  auto seconds = stated(100, 0);
  seconds.audio_seconds = 3;
  CHECK(reasons_of(realtime::response_bound(openai, capped("audio-model", "40"), seconds, catalogue)) == "Quantity [input_audio]; ");
  seconds.audio_seconds = 0;
  CHECK(realtime::response_bound(openai, capped("audio-model", "40"), seconds, catalogue).total);
  // No stated input, no entry, an item unit no token counts, a route ceiling, extensions the fact does not admit.
  CHECK(reasons_of(realtime::response_bound(openai, capped("audio-model", "40"), {}, catalogue)) == "InputBound []; ");
  CHECK(reasons_of(realtime::response_bound(openai, capped("unknown-model", "40"), stated(1, 0), catalogue)) == "ModelPrice [unknown-model]; ");
  CHECK(reasons_of(realtime::response_bound(openai, capped("seconds-audio-model", "40"), stated(1, 0), catalogue)) == "Unmodeled [item:second]; ");
  CHECK(reasons_of(realtime::response_bound(openai, capped("ceiling-model", "40"), stated(1, 0), catalogue)) == "RouteCeiling [ceiling-model]; ");
  auto foreign = capped("audio-model", "40");
  foreign.extensions = document(R"({"max_output_tokens":40,"temperature":1})");
  CHECK(reasons_of(realtime::response_bound(openai, foreign, stated(1, 0), catalogue)) == "Unmodeled [extensions]; ");
  // A transcript of the user's audio bills a transcription no turn reports.
  auto transcribed = capped("audio-model", "40");
  transcribed.input_transcription = true;
  transcribed.transcription_model = "gpt-live-transcribe";
  bound = realtime::response_bound(openai, transcribed, stated(100, 20), catalogue);
  CHECK(!bound.total && bound.lines.empty() && reasons_of(bound) == "Unmodeled [input_transcription]; " && bound.model.empty() && bound.attempts == 0);

  // A committed turn names the setup's model and the one the provider reports, so the estimate finds its entry.
  {
    auto opened = realtime::make_codec(openai);
    frames_of(opened->open(capped("gpt-realtime-2.1-mini", "40")));
    const auto events = inbound(openai, "openai-server.jsonl", opened.get());
    const auto& turn = all<realtime::TurnCommitted>(events).at(0)->turn;
    CHECK(turn.model.requested == "gpt-realtime-2.1-mini" && turn.model.served == "gpt-realtime-2.1-mini" && turn.model.relation == ServedRelation::Exact);
    const auto priced = pricing::estimate(catalogue, "openai.chat", "https://api.openai.com", turn.model, turn.usage);
    CHECK(priced.total && priced.total->nano_usd == 3'600'000u && priced.entry == "gpt-realtime-2.1-mini");
    auto live_codec = realtime::make_codec(google);
    realtime::Setup live_setup;
    live_setup.model = "gpt-realtime-2.1-mini";
    frames_of(live_codec->open(live_setup));
    const auto live_events = inbound(google, "google-server.jsonl", live_codec.get());
    const auto& live_model = all<realtime::TurnCommitted>(live_events).at(1)->turn.model;
    CHECK(live_model.requested == "gpt-realtime-2.1-mini" && live_model.served.empty() && live_model.relation == ServedRelation::NotReported);
  }

  // Transcription sessions bill by the second where the entry prices it: the recorded OpenAI utterance of 4 seconds.
  realtime::Setup transcribe;
  transcribe.output = {};
  transcribe.model = "gpt-live-transcribe";
  realtime::ResponseInput utterance;
  utterance.audio_seconds = 4;
  bound = realtime::response_bound(openai, transcribe, utterance, catalogue);
  CHECK(bound.total && bound.total->nano_usd == nano({{4, "0.0001"}}));
  auto transcriber = realtime::make_codec(openai);
  frames_of(transcriber->open(transcribe));
  const auto heard = all<realtime::TurnCommitted>(inbound(openai, "openai-transcription-server.jsonl", transcriber.get())).at(0)->turn.usage;
  pricing::EndpointCounted counted;
  counted.usage = &heard;
  counted.items = {{"item:second", heard.extra.at("seconds").value}};
  counted.model = served("gpt-live-transcribe");
  const auto facts = realtime::transcription_facts(openai, transcribe);
  CHECK(facts.endpoint == "realtime" && facts.operation == "connect" && facts.billing == descriptor::EndpointBilling::Model);
  const auto heard_cost = pricing::endpoint_estimate(facts, counted, catalogue);
  CHECK(heard_cost.total && heard_cost.total->nano_usd == bound.total->nano_usd && pricing::beyond_bound(bound, heard_cost).empty());
  // The chat estimate prices the turn's seconds the same way: at `item:second`, the tokens at their declared zeros.
  auto heard_estimate = pricing::estimate(catalogue, "openai.chat", "https://api.openai.com", served("gpt-live-transcribe"), heard);
  CHECK(heard_estimate.total && heard_estimate.total->nano_usd == bound.total->nano_usd && heard_estimate.unpriced.empty());
  // No `item:second` row, token rates that are not declared zeros, or a second at a variable price: no total, never zero.
  heard_estimate = pricing::estimate(catalogue, "openai.chat", "https://api.openai.com", served("tokens-transcribe"), heard);
  CHECK(!heard_estimate.total && heard_estimate.unpriced == std::vector<std::string>({"input", "output", "item:second"}));
  {
    auto more = descriptor::load_catalogue(R"({"version":1,"models":[{"id":"seconds-only","pricing":{"item:second":"0.0001"}},
        {"id":"variable-second","pricing":{"input":"0","output":"0","item:second":"-1"}},{"id":"qualified-second","pricing":{"input":"0","output":"0","item:second[operation=transcribe]":"0.0001"}}]})");
    CHECK(std::holds_alternative<descriptor::CatalogueSnapshot>(more));
    const auto& other = *std::get<descriptor::CatalogueSnapshot>(more);
    // An entry with no token price at all prices no chat usage, as before.
    CHECK(pricing::estimate(other, "openai.chat", "", served("seconds-only"), heard).unpriced == std::vector<std::string>({"model"}));
    CHECK(pricing::estimate(other, "openai.chat", "", served("variable-second"), heard).unpriced == std::vector<std::string>({"item:second"}));
    CHECK(pricing::estimate(other, "openai.chat", "", served("qualified-second"), heard).unpriced == std::vector<std::string>({"item:second"}));
    // Zero seconds bill nothing.
    auto silent = heard;
    silent.extra.at("seconds").value = 0;
    const auto none = pricing::estimate(other, "openai.chat", "", served("variable-second"), silent);
    CHECK(none.total && none.total->nano_usd == 0);
  }
  // Without the length, and for an entry that prices no second, no bound.
  CHECK(reasons_of(realtime::response_bound(openai, transcribe, {}, catalogue)) == "Quantity [item:second]; ");
  transcribe.model = "tokens-transcribe";
  bound = realtime::response_bound(openai, transcribe, utterance, catalogue);
  CHECK(!bound.total && bound.unbounded.size() == 1 && bound.unbounded[0].by == pricing::UnboundedBy::ModelPrice);

  // Generated turns within their stated input and caps: the estimate is at most the bound.
  std::uint64_t seed = 124;
  const auto draw = [&](std::uint64_t below) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return below ? (seed >> 33) % below : 0;
  };
  std::size_t checked = 0;
  for (int i = 0; i < 400; ++i) {
    const std::string model = i % 3 == 0 ? "dense-model" : i % 3 == 1 ? "dear-cache-model" : "reasoning-audio-model";
    const bool images = model != "reasoning-audio-model", google_shaped = i % 2 == 1, speaks = i % 4 != 3, tools = i % 5 == 0;
    const std::uint64_t text = draw(3000), audio = draw(800), image = images ? draw(300) : 0, cap = 1 + draw(600);
    // The turn: at most the stated input of each modality, cache reads among its text (some of them cached audio, which
    // the estimate prices out of the text share), at most the cap; no audio out when the setup answers in text.
    const std::uint64_t t = draw(text + 1), a = draw(audio + 1), im = draw(image + 1), cached = draw(t + 1), cached_audio = draw((std::min)(a, cached) + 1);
    const std::uint64_t out = draw(cap + 1), reasoning = google_shaped ? 0 : draw(out + 1), out_audio = speaks ? draw(out - reasoning + 1) : 0;
    const std::uint64_t thoughts = google_shaped ? draw(200) : 0;
    Usage usage;
    usage.input_total = Count{t + a + im, Evidence::Reported};
    usage.cache_read = Count{cached, Evidence::Reported};
    usage.output_total = Count{out, Evidence::Reported};
    usage.stage = UsageStage::Final;
    if (google_shaped) {
      usage.extra["promptTokensDetails.TEXT"] = Count{t, Evidence::Reported};
      usage.extra["promptTokensDetails.AUDIO"] = Count{a, Evidence::Reported};
      usage.extra["cacheTokensDetails.AUDIO"] = Count{cached_audio, Evidence::Reported};
      usage.extra["cacheTokensDetails.TEXT"] = Count{cached - cached_audio, Evidence::Reported};
      if (im) {
        usage.extra["promptTokensDetails.IMAGE"] = Count{im, Evidence::Reported};
        usage.extra[std::string(pricing::realtime_image_tokens)] = Count{im, Evidence::Reported};
      }
      usage.extra["responseTokensDetails.AUDIO"] = Count{out_audio, Evidence::Reported};
      usage.extra["responseTokensDetails.TEXT"] = Count{out - out_audio, Evidence::Reported};
      if (thoughts) usage.extra["thoughtsTokenCount"] = Count{thoughts, Evidence::Reported};
    } else {
      usage.reasoning = Count{reasoning, Evidence::Reported};
      usage.extra["input_token_details.text_tokens"] = Count{t, Evidence::Reported};
      usage.extra["input_token_details.audio_tokens"] = Count{a, Evidence::Reported};
      usage.extra["input_token_details.image_tokens"] = Count{im, Evidence::Reported};
      usage.extra["input_token_details.cached_tokens_details.audio_tokens"] = Count{cached_audio, Evidence::Reported};
      usage.extra["input_token_details.cached_tokens_details.text_tokens"] = Count{cached - cached_audio, Evidence::Reported};
      usage.extra["output_token_details.audio_tokens"] = Count{out_audio, Evidence::Reported};
    }
    pricing::RealtimeResponseInput response;
    response.family = google_shaped ? "google.generate" : "openai.chat";
    response.origin = google_shaped ? "https://generativelanguage.googleapis.com" : "https://api.openai.com";
    response.model = model;
    response.audio_output = speaks;
    response.output_tokens = cap;
    response.thinking_beside_output = google_shaped;
    response.thinking_tokens = 200;
    response.input_tokens = pricing::RealtimeInputTokens{text, audio, image};
    // With tools Live may count a tool-use prompt beside the input: no bound, and no estimate either.
    if (google_shaped && tools) {
      response.tool_prompt_beside_input = true;
      usage.extra["toolUsePromptTokenCount"] = Count{1 + draw(100), Evidence::Reported};
      CHECK(reasons_of(pricing::realtime_response_bound(response, catalogue)) == "Unmodeled [toolUsePromptTokenCount]; ");
      CHECK(!pricing::estimate(catalogue, response.family, response.origin, served(model), usage).total);
      ++checked;
      continue;
    }
    const auto turn_bound = pricing::realtime_response_bound(response, catalogue);
    const auto turn_cost = pricing::estimate(catalogue, response.family, response.origin, served(model), usage);
    if (!turn_cost.total) throw std::runtime_error("generated turn " + std::to_string(i) + " has no estimate: " + (turn_cost.unpriced.empty() ? "" : turn_cost.unpriced[0]));
    if (!turn_bound.total || turn_cost.total->nano_usd > turn_bound.total->nano_usd)
      throw std::runtime_error("generated turn " + std::to_string(i) + " of " + model + ": estimate " + std::to_string(turn_cost.total->nano_usd) + " above bound " +
                               (turn_bound.total ? std::to_string(turn_bound.total->nano_usd) : reasons_of(turn_bound)));
    // Through the session's setup, an OpenAI-shaped turn gets the same bound.
    if (!google_shaped) {
      // A tool call ends an OpenAI response within its cap, so tools change nothing.
      auto setup = capped(model, std::to_string(cap), speaks ? std::vector<realtime::Modality>{realtime::Modality::Audio} : std::vector<realtime::Modality>{realtime::Modality::Text});
      if (tools) setup.tools.push_back(weather(R"({"type":"object"})"));
      const auto through = realtime::response_bound(openai, setup, stated(text, audio, image), catalogue);
      CHECK(through.total && through.total->nano_usd == turn_bound.total->nano_usd);
    }
    ++checked;
  }
  CHECK(checked == 400);
}

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: sp_realtime_tests <fixture directory>");
    fixtures = argv[1];
    google_live(); openai_realtime(); response_statuses(); usage_details(); tool_turn_usage(); turn_usage_is_priced(); audio_formats(); policy_facts(); transcription();
    response_bounds();
    std::cout << "realtime properties passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "realtime test failed: " << error.what() << '\n'; return 1; }
}
