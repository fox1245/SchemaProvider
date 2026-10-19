// Realtime session codecs (D8), against the official SDKs. tests/conformance/realtime records one scripted
// scenario through google-genai Live and the OpenAI realtime client; the SDK's codecs must send the same frames for
// the same setup and commands, once canonicalized, and decode the recorded server frames into the expected events
// and per-turn completions.
// Usage: sp_realtime_tests <fixture directory>
#include "codecs/realtime.h"
#include "core/media.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include <filesystem>
#include <fstream>
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
// The server side: the recorded frames, decoded in order.
std::vector<realtime::Event> inbound(const descriptor::ValidatedDescriptor& d, const std::string& recording) {
  auto codec = realtime::make_codec(d);
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
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: sp_realtime_tests <fixture directory>");
    fixtures = argv[1];
    google_live(); openai_realtime();
    std::cout << "realtime properties passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "realtime test failed: " << error.what() << '\n'; return 1; }
}
