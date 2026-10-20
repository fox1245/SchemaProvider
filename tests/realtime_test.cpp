// Realtime session codecs (D8), against the official SDKs. tests/conformance/realtime records one scripted
// scenario through google-genai Live and the OpenAI realtime client; the SDK's codecs must send the same frames for
// the same setup and commands, once canonicalized, and decode the recorded server frames into the expected events
// and per-turn completions.
// Usage: sp_realtime_tests <fixture directory>
#include "codecs/realtime.h"
#include "core/media.h"
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
  CHECK(google->close_codes.size() == 1 && google->close_codes[0].code == 1007 && google->close_codes[0].status == 401 &&
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

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: sp_realtime_tests <fixture directory>");
    fixtures = argv[1];
    google_live(); openai_realtime(); usage_details(); tool_turn_usage(); audio_formats(); policy_facts(); transcription();
    std::cout << "realtime properties passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "realtime test failed: " << error.what() << '\n'; return 1; }
}
