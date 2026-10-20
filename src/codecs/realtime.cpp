#include "codecs/realtime.h"
#include "codecs/endpoint.h"
#include "codecs/reason.h"
#include "core/media.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace sp::realtime {
namespace {
// A refusal of the setup or a command, with the code of the check.
Error refusal(ErrorKind kind, std::string message) {
  Error error{kind, std::move(message)};
  error.reason = std::string(reason::request_code(reason::Source::RealtimeRequest, error.safe_message));
  return error;
}
Error bad(std::string message) { return refusal(ErrorKind::InvalidRequest, std::move(message)); }
Error unsupported(std::string message) { return refusal(ErrorKind::Unsupported, std::move(message)); }
bool token(std::string_view value) {
  return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}
std::string text_of(json::Value value) { return value.is_string() ? std::string(value.as_string()) : std::string(); }
std::optional<Count> counter(json::Value value) {
  if (!value.is_uint()) return std::nullopt;
  return Count{value.as_uint(), Evidence::Reported};
}
const descriptor::EndpointFact* fact_of(const descriptor::ValidatedDescriptor& descriptor) {
  return descriptor.policy()->endpoint("realtime", descriptor.base_url());
}
// A Google duration (`10s`, `1.5s`).
std::optional<std::chrono::milliseconds> duration_of(std::string_view text) {
  if (text.size() < 2 || text.back() != 's') return std::nullopt;
  text.remove_suffix(1);
  double seconds = 0;
  try { std::size_t used = 0; seconds = std::stod(std::string(text), &used); if (used != text.size()) return std::nullopt; }
  catch (...) { return std::nullopt; }
  if (!std::isfinite(seconds) || seconds < 0 || seconds > 86400) return std::nullopt;
  return std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000));
}
std::shared_ptr<const json::Document> document(std::string_view text, SemanticLimits limits) {
  auto parsed = json::parse(text, {limits.max_content_bytes, limits.max_json_depth});
  if (!std::holds_alternative<json::Document>(parsed)) return nullptr;
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}

// A failure of the session the server's messages caused.
Error reject(ErrorKind kind, std::string text) {
  Error error{kind, std::move(text)};
  error.reason = std::string(reason::code(reason::Source::Realtime, error.safe_message));
  return error;
}
// One model turn, assembled into the chat outcome a consumer reads.
class Turn {
 public:
  void audio(std::string_view pcm, std::string_view mime) {
    pcm_.append(pcm);
    if (mime_.empty()) mime_ = std::string(mime);
    active_ = true;
  }
  void text(std::string_view value) { text_.append(value); active_ = true; }
  void transcript(std::string_view value) { transcript_.append(value); active_ = true; }
  void set_transcript(std::string value) { transcript_ = std::move(value); active_ = true; }
  void tool_call(std::string id, std::string name, std::string arguments) { calls_.push_back({std::move(id), std::move(name), std::move(arguments)}); active_ = true; }
  void usage(Usage usage) { usage_ = std::move(usage); }
  // A transcription session's turn is the user's utterance.
  void user_text(std::string_view value) { text_.append(value); role_ = Role::User; active_ = true; }
  bool active() const { return active_; }
  // The turn as a Completion, through the accumulator so it keeps the chat invariants; then a fresh turn.
  std::variant<Completion, Error> commit(std::string id, StopReason stop, SemanticLimits limits) {
    Accumulator accumulator(limits);
    std::uint32_t local = 0;
    std::uint64_t order = 0;
    bool ok = accumulator.accept(Begin{id}) && accumulator.accept(MessageBegin{{0}, id.empty() ? std::nullopt : std::optional<std::string>(id), role_, nullptr});
    const auto part = [&](PartKind kind, PartHeader header, std::string_view bytes) {
      const LocalId at{++local};
      ok = ok && accumulator.accept(PartBegin{{0}, at, kind, std::move(header), order++}) &&
           (bytes.empty() || accumulator.accept(PartDelta{at, {kind, bytes}})) && accumulator.accept(PartSeal{at, {}});
    };
    if (!text_.empty()) part(PartKind::Text, {}, text_);
    if (!pcm_.empty()) {
      PartHeader header;
      header.media.kind = MediaKind::Audio;
      header.media.source = MediaSource::Inline;
      header.media.mime = mime_;
      header.media.transcript = transcript_;
      const auto encoded = base64_encode(pcm_);
      part(PartKind::Media, std::move(header), encoded);
    } else if (!transcript_.empty()) {
      part(PartKind::Text, {}, transcript_);
    }
    for (auto& call : calls_) {
      PartHeader header;
      header.wire_id = call.id;
      header.name = call.name;
      part(PartKind::ToolCall, std::move(header), call.arguments.empty() ? std::string_view("{}") : std::string_view(call.arguments));
    }
    ok = ok && accumulator.accept(UsageUpdate{usage_}) && accumulator.accept(Stop{std::move(stop)}) && accumulator.accept(MessageSeal{{0}}) &&
         accumulator.accept(Commit{"realtime.turn"});
    *this = Turn{};
    if (!accumulator.outcome()) return reject(ErrorKind::ProtocolCorrupt, "a turn did not assemble");
    if (auto* failure = std::get_if<Failure>(&*accumulator.outcome())) return failure->error;
    (void)ok;
    return std::get<Completion>(*accumulator.outcome());
  }
 private:
  struct Call { std::string id, name, arguments; };
  std::string pcm_, mime_, text_, transcript_;
  std::vector<Call> calls_;
  Usage usage_;
  Role role_ = Role::Assistant;
  bool active_ = false;
};


// Google Live (`BidiGenerateContent`): one JSON object per message, either way.
class GoogleCodec final : public Codec {
 public:
  GoogleCodec(const descriptor::EndpointFact& fact, SemanticLimits limits) : fact_(fact), limits_(limits) {}
  std::uint32_t input_rate() const noexcept override { return 16000; }
  std::variant<std::vector<Frame>, Error> open(const Setup& setup) override {
    // A transcription model answers in text only; it refuses AUDIO.
    transcribing_ = setup.output.empty();
    std::string out = "{\"setup\":{\"model\":" + json::quote("models/" + setup.model) + ",\"generationConfig\":{\"responseModalities\":[";
    for (std::size_t i = 0; i < setup.output.size(); ++i) out.append(i ? "," : "").append(setup.output[i] == Modality::Audio ? "\"AUDIO\"" : "\"TEXT\"");
    if (transcribing_) out += "\"TEXT\"";
    out += "]";
    if (!setup.voice.empty()) out += ",\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":" + json::quote(setup.voice) + "}}}";
    out += "}";
    if (!setup.instructions.empty()) out += ",\"systemInstruction\":{\"parts\":[{\"text\":" + json::quote(setup.instructions) + "}],\"role\":\"user\"}";
    if (setup.input_transcription || transcribing_) out += ",\"inputAudioTranscription\":{}";
    if (setup.output_transcription) out += ",\"outputAudioTranscription\":{}";
    if (setup.resume) out += setup.resume->empty() ? ",\"sessionResumption\":{}" : ",\"sessionResumption\":{\"handle\":" + json::quote(*setup.resume) + "}";
    if (!setup.tools.empty()) {
      out += ",\"tools\":[{\"functionDeclarations\":[";
      for (std::size_t i = 0; i < setup.tools.size(); ++i) {
        const auto& tool = setup.tools[i];
        out.append(i ? "," : "").append("{\"name\":").append(json::quote(tool.name)).append(",\"description\":").append(json::quote(tool.description));
        if (tool.parameters) out.append(",\"parameters\":").append(tool.parameters->root().dump());
        out += "}";
      }
      out += "]}]";
    }
    if (setup.extensions) for (auto member : setup.extensions->root().members()) out += "," + json::quote(member.key) + ":" + member.value.dump();
    return std::vector<Frame>{{false, out + "}}"}};
  }
  std::variant<std::vector<Frame>, Error> encode(const Command& command) override {
    if (const auto* audio = std::get_if<Audio>(&command))
      return std::vector<Frame>{{false, "{\"realtimeInput\":{\"audio\":{\"data\":\"" + base64_encode(audio->pcm) + "\",\"mimeType\":\"audio/pcm;rate=16000\"}}}"}};
    if (std::holds_alternative<EndAudio>(command)) return std::vector<Frame>{{false, "{\"realtimeInput\":{\"audioStreamEnd\":true}}"}};
    if (const auto* text = std::get_if<Text>(&command))
      return std::vector<Frame>{{false, "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":" + json::quote(text->text) + "}]}],\"turnComplete\":true}}"}};
    if (const auto* result = std::get_if<ToolResult>(&command)) {
      if (!token(result->call_id) || result->name.empty()) return bad("a tool result names its call and tool");
      return std::vector<Frame>{{false, "{\"toolResponse\":{\"functionResponses\":[{\"id\":" + json::quote(result->call_id) + ",\"name\":" + json::quote(result->name) +
                                            ",\"response\":" + (result->output ? result->output->root().dump() : std::string("{}")) + "}]}}"}};
    }
    // Google answers by itself.
    if (std::holds_alternative<Respond>(command)) return std::vector<Frame>{};
    return unsupported("Google Live has no cancel; speaking over the model interrupts it");
  }
  std::variant<std::vector<Event>, Error> decode(bool, std::string_view data) override {
    auto wire = document(data, limits_);
    if (!wire || !wire->root().is_object()) return reject(ErrorKind::ProtocolCorrupt, "a realtime message is a JSON object");
    const auto root = wire->root();
    std::vector<Event> events;
    // Google also sends empty messages while a session idles.
    bool modelled = root.size() == 0;
    if (root.get("setupComplete").valid()) { events.push_back(Ready{}); modelled = true; }
    if (const auto activity = root.get("voiceActivity").get("type"); activity.is_string()) {
      modelled = true;
      if (activity.as_string() == "ACTIVITY_START") events.push_back(SpeechStarted{});
      else if (activity.as_string() == "ACTIVITY_END") events.push_back(SpeechStopped{});
    }
    // Usage comes with content messages; the turn keeps the latest.
    if (const auto usage = root.get("usageMetadata"); usage.is_object()) {
      Usage value;
      value.input_total = counter(usage.get("promptTokenCount"));
      value.output_total = counter(usage.get("responseTokenCount"));
      value.total = counter(usage.get("totalTokenCount"));
      value.cache_read = counter(usage.get("cachedContentTokenCount"));
      value.stage = UsageStage::Final;
      turn_.usage(std::move(value));
      modelled = true;
    }
    if (const auto content = root.get("serverContent"); content.is_object()) {
      modelled = true;
      if (const auto parts = content.get("modelTurn").get("parts"); parts.is_array()) {
        for (auto part : parts.elements()) {
          if (const auto inline_data = part.get("inlineData"); inline_data.is_object()) {
            std::string pcm;
            if (!base64_decode(text_of(inline_data.get("data")), pcm)) return reject(ErrorKind::ProtocolCorrupt, "realtime audio is base64");
            const auto mime = text_of(inline_data.get("mimeType"));
            turn_.audio(pcm, mime);
            events.push_back(AudioDelta{std::move(pcm), mime});
          } else if (part.get("text").is_string()) {
            turn_.text(part.get("text").as_string());
            events.push_back(TextDelta{std::string(part.get("text").as_string())});
          }
        }
      }
      if (const auto interim = content.get("interimInputTranscription").get("text"); interim.is_string())
        events.push_back(Transcript{true, std::string(interim.as_string()), true});
      if (const auto input = content.get("inputTranscription").get("text"); input.is_string()) {
        events.push_back(Transcript{true, std::string(input.as_string())});
        if (transcribing_) {
          turn_.user_text(input.as_string());
          if (auto error = commit(events, StopReason{StopKind::EndTurn, "transcribed"})) return *error;
        }
      }
      if (const auto output = content.get("outputTranscription").get("text"); output.is_string()) {
        turn_.transcript(output.as_string());
        events.push_back(Transcript{false, std::string(output.as_string())});
      }
      const bool interrupted = content.get("interrupted").is_bool() && content.get("interrupted").as_bool();
      const bool complete = content.get("turnComplete").is_bool() && content.get("turnComplete").as_bool();
      if (interrupted) {
        events.push_back(TurnAborted{});
        if (auto error = commit(events, StopReason{StopKind::Unknown, "interrupted"})) return *error;
      } else if (complete) {
        if (auto error = commit(events, StopReason{StopKind::EndTurn, "turn_complete"})) return *error;
      }
    }
    if (const auto call = root.get("toolCall").get("functionCalls"); call.is_array()) {
      modelled = true;
      for (auto item : call.elements()) {
        const auto args = item.get("args");
        auto arguments = args.valid() ? document(args.dump(), limits_) : document("{}", limits_);
        turn_.tool_call(text_of(item.get("id")), text_of(item.get("name")), args.valid() ? args.dump() : "{}");
        events.push_back(ToolCallEvent{text_of(item.get("id")), text_of(item.get("name")), std::move(arguments)});
      }
      // The model waits for the results: its turn ends with the calls.
      if (auto error = commit(events, StopReason{StopKind::ToolUse, "tool_call"})) return *error;
    }
    if (const auto cancelled = root.get("toolCallCancellation").get("ids"); cancelled.is_array()) {
      modelled = true;
      ToolCallsCancelled event;
      for (auto id : cancelled.elements()) event.ids.push_back(text_of(id));
      events.push_back(std::move(event));
    }
    if (const auto update = root.get("sessionResumptionUpdate"); update.is_object()) {
      modelled = true;
      events.push_back(ResumptionUpdate{text_of(update.get("newHandle")), update.get("resumable").is_bool() && update.get("resumable").as_bool()});
    }
    if (const auto away = root.get("goAway"); away.is_object()) {
      modelled = true;
      events.push_back(DrainNotice{duration_of(text_of(away.get("timeLeft")))});
    }
    if (!modelled) events.push_back(Unmodelled{wire});
    return events;
  }
 private:
  std::optional<Error> commit(std::vector<Event>& events, StopReason stop) {
    if (!turn_.active()) return std::nullopt;
    auto turn = turn_.commit({}, std::move(stop), limits_);
    if (auto* error = std::get_if<Error>(&turn)) return *error;
    events.push_back(TurnCommitted{std::move(std::get<Completion>(turn))});
    return std::nullopt;
  }
  const descriptor::EndpointFact& fact_;
  SemanticLimits limits_;
  Turn turn_;
  bool transcribing_ = false;
};

// OpenAI Realtime (`/v1/realtime`): typed JSON events, either way.
class OpenAICodec final : public Codec {
 public:
  OpenAICodec(const descriptor::EndpointFact& fact, SemanticLimits limits) : fact_(fact), limits_(limits) {}
  std::uint32_t input_rate() const noexcept override { return 24000; }
  std::variant<std::vector<Frame>, Error> open(const Setup& setup) override {
    transcribing_ = setup.output.empty();
    if (transcribing_) {
      // The transcription model takes no turn detection: the host ends each utterance with EndAudio.
      std::string session = "{\"type\":\"transcription\",\"audio\":{\"input\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000},"
                            "\"transcription\":{\"model\":" + json::quote(setup.model) + "},\"turn_detection\":null}}";
      if (setup.extensions) for (auto member : setup.extensions->root().members()) session += "," + json::quote(member.key) + ":" + member.value.dump();
      return std::vector<Frame>{{false, "{\"type\":\"session.update\",\"session\":" + session + "}}"}};
    }
    std::string session = "{\"type\":\"realtime\"";
    if (!setup.instructions.empty()) session += ",\"instructions\":" + json::quote(setup.instructions);
    session += ",\"output_modalities\":[";
    for (std::size_t i = 0; i < setup.output.size(); ++i) session.append(i ? "," : "").append(setup.output[i] == Modality::Audio ? "\"audio\"" : "\"text\"");
    session += "],\"audio\":{\"input\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000}";
    if (setup.input_transcription) {
      if (setup.transcription_model.empty()) return bad("OpenAI transcribes the input with a transcription model");
      session += ",\"transcription\":{\"model\":" + json::quote(setup.transcription_model) + "}";
    }
    session += "},\"output\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000}";
    if (!setup.voice.empty()) session += ",\"voice\":" + json::quote(setup.voice);
    session += "}}";
    if (!setup.tools.empty()) {
      session += ",\"tools\":[";
      for (std::size_t i = 0; i < setup.tools.size(); ++i) {
        const auto& tool = setup.tools[i];
        session.append(i ? "," : "").append("{\"type\":\"function\",\"name\":").append(json::quote(tool.name)).append(",\"description\":").append(json::quote(tool.description));
        if (tool.parameters) session.append(",\"parameters\":").append(tool.parameters->root().dump());
        session += "}";
      }
      session += "]";
    }
    if (setup.extensions) for (auto member : setup.extensions->root().members()) session += "," + json::quote(member.key) + ":" + member.value.dump();
    if (setup.resume) return unsupported("OpenAI Realtime sessions do not resume");
    return std::vector<Frame>{{false, "{\"type\":\"session.update\",\"session\":" + session + "}}"}};
  }
  std::variant<std::vector<Frame>, Error> encode(const Command& command) override {
    if (const auto* audio = std::get_if<Audio>(&command))
      return std::vector<Frame>{{false, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"" + base64_encode(audio->pcm) + "\"}"}};
    if (std::holds_alternative<EndAudio>(command)) return std::vector<Frame>{{false, "{\"type\":\"input_audio_buffer.commit\"}"}};
    if (const auto* text = std::get_if<Text>(&command))
      return std::vector<Frame>{{false, "{\"type\":\"conversation.item.create\",\"item\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":" +
                                            json::quote(text->text) + "}]}}"}};
    if (const auto* result = std::get_if<ToolResult>(&command)) {
      if (!token(result->call_id)) return bad("a tool result names its call and tool");
      // OpenAI takes the output as a string.
      return std::vector<Frame>{{false, "{\"type\":\"conversation.item.create\",\"item\":{\"type\":\"function_call_output\",\"call_id\":" + json::quote(result->call_id) +
                                            ",\"output\":" + json::quote(result->output ? result->output->root().dump() : std::string("{}")) + "}}"}};
    }
    if (std::holds_alternative<Respond>(command)) return std::vector<Frame>{{false, "{\"type\":\"response.create\"}"}};
    return std::vector<Frame>{{false, "{\"type\":\"response.cancel\"}"}};
  }
  std::variant<std::vector<Event>, Error> decode(bool, std::string_view data) override {
    auto wire = document(data, limits_);
    if (!wire || !wire->root().is_object() || !wire->root().get("type").is_string()) return reject(ErrorKind::ProtocolCorrupt, "a realtime message is a JSON object");
    const auto root = wire->root();
    const auto type = root.get("type").as_string();
    std::vector<Event> events;
    if (type == "session.created") events.push_back(Ready{text_of(root.get("session").get("id"))});
    else if (type == "error") {
      const auto error = root.get("error");
      Error value = reject(ErrorKind::RemoteFailure, "the provider reported an error in the session");
      value.vendor_code = text_of(error.get("code"));
      events.push_back(ProviderError{std::move(value)});
    } else if (type == "input_audio_buffer.speech_started") events.push_back(SpeechStarted{});
    else if (type == "input_audio_buffer.speech_stopped") events.push_back(SpeechStopped{});
    else if (type == "conversation.item.input_audio_transcription.delta") {
      auto& so_far = partial_[text_of(root.get("item_id"))];
      so_far += text_of(root.get("delta"));
      events.push_back(Transcript{true, so_far, true});
    } else if (type == "conversation.item.input_audio_transcription.completed") {
      partial_.erase(text_of(root.get("item_id")));
      const auto transcript = text_of(root.get("transcript"));
      events.push_back(Transcript{true, transcript});
      if (transcribing_) {
        turn_.user_text(transcript);
        if (const auto usage = root.get("usage"); usage.is_object()) turn_.usage(transcription_usage(usage));
        auto turn = turn_.commit(text_of(root.get("item_id")), StopReason{StopKind::EndTurn, "transcribed"}, limits_);
        if (auto* error = std::get_if<Error>(&turn)) return *error;
        events.push_back(TurnCommitted{std::move(std::get<Completion>(turn))});
      }
    }
    else if (type == "response.created") response_ = text_of(root.get("response").get("id"));
    else if (type == "response.output_audio.delta") {
      std::string pcm;
      if (!base64_decode(text_of(root.get("delta")), pcm)) return reject(ErrorKind::ProtocolCorrupt, "realtime audio is base64");
      turn_.audio(pcm, "audio/pcm;rate=24000");
      events.push_back(AudioDelta{std::move(pcm), "audio/pcm;rate=24000"});
    } else if (type == "response.output_text.delta") {
      turn_.text(text_of(root.get("delta")));
      events.push_back(TextDelta{text_of(root.get("delta"))});
    } else if (type == "response.output_audio_transcript.delta") {
      turn_.transcript(text_of(root.get("delta")));
      events.push_back(Transcript{false, text_of(root.get("delta"))});
    } else if (type == "response.output_audio_transcript.done") {
      turn_.set_transcript(text_of(root.get("transcript")));
    } else if (type == "response.function_call_arguments.done") {
      const auto arguments = text_of(root.get("arguments"));
      turn_.tool_call(text_of(root.get("call_id")), text_of(root.get("name")), arguments);
      events.push_back(ToolCallEvent{text_of(root.get("call_id")), text_of(root.get("name")), document(arguments.empty() ? "{}" : arguments, limits_)});
    } else if (type == "response.done") {
      const auto response = root.get("response");
      const auto status = text_of(response.get("status"));
      if (const auto usage = response.get("usage"); usage.is_object()) {
        Usage value;
        value.input_total = counter(usage.get("input_tokens"));
        value.output_total = counter(usage.get("output_tokens"));
        value.total = counter(usage.get("total_tokens"));
        value.cache_read = counter(usage.get("input_token_details").get("cached_tokens"));
        value.stage = UsageStage::Final;
        turn_.usage(std::move(value));
      }
      if (status == "cancelled") events.push_back(TurnAborted{});
      if (status == "failed") events.push_back(ProviderError{reject(ErrorKind::RemoteFailure, "the provider reported an error in the session")});
      const auto kind = status == "cancelled" ? StopKind::Unknown : status == "incomplete" ? StopKind::MaxTokens : StopKind::EndTurn;
      if (turn_.active() || status == "completed") {
        auto turn = turn_.commit(text_of(response.get("id")), StopReason{kind, status == "cancelled" ? "interrupted" : status}, limits_);
        if (auto* error = std::get_if<Error>(&turn)) return *error;
        events.push_back(TurnCommitted{std::move(std::get<Completion>(turn))});
      }
    } else if (type.starts_with("session.") || type.starts_with("input_audio_buffer.") || type.starts_with("conversation.") ||
               type.starts_with("response.") || type == "rate_limits.updated") {
      // Bookkeeping the session carries for the provider's own clients; the turn events above cover what it says.
    } else {
      events.push_back(Unmodelled{wire});
    }
    return events;
  }
 private:
  // A transcription's usage: seconds of audio (`{"type":"duration","seconds":4}`), or tokens.
  static Usage transcription_usage(json::Value usage) {
    Usage value;
    if (const auto seconds = counter(usage.get("seconds"))) value.extra["seconds"] = *seconds;
    value.input_total = counter(usage.get("input_tokens"));
    value.output_total = counter(usage.get("output_tokens"));
    value.total = counter(usage.get("total_tokens"));
    value.stage = UsageStage::Final;
    return value;
  }
  const descriptor::EndpointFact& fact_;
  SemanticLimits limits_;
  Turn turn_;
  std::string response_;
  bool transcribing_ = false;
  // Each utterance's transcript so far, by item, while OpenAI streams it.
  std::map<std::string, std::string> partial_;
};
}  // namespace

std::variant<Connection, Error> connect(const descriptor::ValidatedDescriptor& descriptor, const Setup& setup, std::string_view api_key) {
  const auto* fact = fact_of(descriptor);
  if (!fact) return unsupported("realtime sessions are not admitted at this origin");
  const auto path = descriptor::endpoint_path(*fact, "connect");
  if (!path) return unsupported("the realtime endpoint has no path");
  // The model is a path segment at Google (`models/<model>`) and a query parameter at OpenAI.
  if (!token(setup.model)) return bad("a realtime model is a token");
  if (setup.output.empty() && (!setup.voice.empty() || !setup.instructions.empty() || !setup.tools.empty() || setup.output_transcription ||
                               !setup.transcription_model.empty() || setup.resume))
    return bad("a transcription session takes no voice, instructions, tools, output transcription or resumption");
  if (setup.voice.size() > 128 || setup.instructions.size() > descriptor.policy()->resources().request_bytes)
    return bad("realtime setup values are bounded");
  for (const auto& tool : setup.tools)
    if (tool.name.empty() || tool.name.size() > 128 || (tool.parameters && !tool.parameters->root().is_object())) return bad("a realtime tool names itself and an object schema");
  if (setup.extensions) {
    if (!fact->options) return unsupported("the realtime endpoint admits no setup options");
    if (!setup.extensions->root().is_object() || descriptor::check_value(*fact->options, setup.extensions->root()))
      return bad("realtime setup options do not match the policy's schema");
  }
  std::string origin(descriptor.base_url());
  if (origin.starts_with("https://")) origin.replace(0, 5, "wss");
  else if (origin.starts_with("http://")) origin.replace(0, 4, "ws");
  Connection connection{origin + std::string(*path)};
  // An OpenAI transcription session names its model in the session update.
  if (fact->shape == "openai") connection.url += setup.output.empty() ? std::string("?intent=transcription") : "?model=" + setup.model;
  if (!api_key.empty()) {
    if (fact->auth == "x-goog-api-key") connection.headers.emplace_back("x-goog-api-key", std::string(api_key));
    else if (fact->auth == "x-api-key") connection.headers.emplace_back("x-api-key", std::string(api_key));
    else connection.headers.emplace_back("Authorization", "Bearer " + std::string(api_key));
  }
  for (const auto& header : fact->headers) connection.headers.push_back(header);
  return connection;
}

std::unique_ptr<Codec> make_codec(const descriptor::ValidatedDescriptor& descriptor, SemanticLimits limits) {
  const auto* fact = fact_of(descriptor);
  if (!fact) return nullptr;
  if (fact->shape == "google") return std::make_unique<GoogleCodec>(*fact, limits);
  if (fact->shape == "openai") return std::make_unique<OpenAICodec>(*fact, limits);
  return nullptr;
}
}  // namespace sp::realtime
