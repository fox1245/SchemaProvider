#pragma once
// Realtime sessions (D8): the provider-neutral setup, client commands and server events of a full-duplex session,
// and the codec of each provider that turns them into WebSocket messages and back. Google Live
// (`BidiGenerateContent`) and OpenAI Realtime (`/v1/realtime`). The session that runs a codec over the transport
// is `realtime::Session` (`<transport/realtime_session.h>`); this layer does no I/O.
#include "codecs/accumulator.h"
#include "core/value.h"
#include "descriptor/descriptor.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sp::realtime {
enum class Modality { Audio, Text };
struct Tool {
  std::string name, description;
  std::shared_ptr<const json::Document> parameters;
};
struct Setup {
  std::string model;
  // What the model answers with. Empty: a transcription session. The model answers nothing, and the user's audio
  // comes back as transcripts, a turn per utterance. The model is a transcription model (Google
  // `gemini-3.5-transcribe-live`, OpenAI `gpt-live-transcribe`); a session takes no voice, instructions, tools or
  // output transcription. At OpenAI the host ends each utterance with `EndAudio`.
  std::vector<Modality> output{Modality::Audio};
  std::string instructions{};
  // A voice the provider names (`Puck`, `alloy`).
  std::string voice{};
  // Transcripts of what the user says and of what the model says. OpenAI transcribes the input with a model of its
  // own (`transcription_model`); Google with the session's model.
  bool input_transcription = false, output_transcription = false;
  std::string transcription_model{};
  std::vector<Tool> tools{};
  // Google: a resumable session, resumed from this handle when it is not empty.
  std::optional<std::string> resume{};
  // Setup members the endpoint's policy fact admits, sent as given.
  std::shared_ptr<const json::Document> extensions{};
};

// Client commands.
// PCM at the provider's input rate (Google 16 kHz, OpenAI 24 kHz), mono, 16-bit little-endian.
struct Audio { std::string pcm; };
// The end of the user's audio: Google `audioStreamEnd`, OpenAI `input_audio_buffer.commit`.
struct EndAudio {};
// A user text turn. Google takes it as a complete turn; OpenAI as a conversation item, answered on `Respond`.
struct Text { std::string text; };
// The result of a tool call the model made.
struct ToolResult {
  std::string call_id, name;
  std::shared_ptr<const json::Document> output;
};
// OpenAI: answer now (`response.create`). Google answers by itself; this sends nothing there.
struct Respond {};
// OpenAI: stop the answer being made (`response.cancel`).
struct Cancel {};
using Command = std::variant<Audio, EndAudio, Text, ToolResult, Respond, Cancel>;

// A WebSocket message.
struct Frame {
  bool binary = false;
  std::string data;
};

// Server events.
struct Ready { std::string session_id; };
// The model's audio, decoded: PCM at the provider's output rate.
struct AudioDelta { std::string pcm, mime; };
struct TextDelta { std::string text; };
struct Transcript {
  // Of what the user said (input) or of what the model said.
  bool input = false;
  std::string text;
  // An input transcript so far, which later ones replace until a final one (partial false) ends the utterance.
  bool partial = false;
};
struct ToolCallEvent {
  std::string id, name;
  std::shared_ptr<const json::Document> arguments;
};
struct ToolCallsCancelled { std::vector<std::string> ids; };
// The provider heard the user start or stop speaking.
struct SpeechStarted {};
struct SpeechStopped {};
// The model's turn was cut short (the user spoke over it, or a cancel); its turn still commits, marked interrupted.
struct TurnAborted {};
// A finished or interrupted turn, as the chat outcome a consumer already reads: the model's text, its audio as one
// inline Media part, its transcript, its tool calls, the turn's usage, and a stop (`interrupted` when cut short).
// In a transcription session a turn is one utterance: a user message with its final transcript, and its usage
// (OpenAI reports seconds of audio, `Usage::extra["seconds"]`; Google reports none).
struct TurnCommitted { Completion turn; };
// Google: the handle that resumes this session, for the host to journal.
struct ResumptionUpdate {
  std::string handle;
  bool resumable = false;
};
// The provider will close the session; reconnect before `time_left` passes.
struct DrainNotice { std::optional<std::chrono::milliseconds> time_left{}; };
// An error the provider reported in the session; the session may go on.
struct ProviderError { Error error; };
// A server message the codec does not model, kept whole.
struct Unmodelled { std::shared_ptr<const json::Document> raw; };
using Event = std::variant<Ready, AudioDelta, TextDelta, Transcript, ToolCallEvent, ToolCallsCancelled, SpeechStarted, SpeechStopped,
                           TurnAborted, TurnCommitted, ResumptionUpdate, DrainNotice, ProviderError, Unmodelled>;
// Control that goes ahead of queued audio: a tool result, a cancel.
inline bool control(const Command& command) { return std::holds_alternative<ToolResult>(command) || std::holds_alternative<Cancel>(command); }
// A turn marker: a command the audio queued before it must reach the provider first (the end of the audio, a text
// turn, a request to answer).
inline bool turn_marker(const Command& command) { return !std::holds_alternative<Audio>(command) && !control(command); }

// Where the session connects: the request target and the handshake headers, credentials included.
struct Connection {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
};
// The setup checked against the origin's `realtime` fact, with no I/O.
std::variant<Connection, Error> connect(const descriptor::ValidatedDescriptor&, const Setup&, std::string_view api_key);

class Codec {
 public:
  virtual ~Codec() = default;
  // What the client sends once the socket is open.
  virtual std::variant<std::vector<Frame>, Error> open(const Setup&) = 0;
  virtual std::variant<std::vector<Frame>, Error> encode(const Command&) = 0;
  // The events of one server message. An error ends the session.
  virtual std::variant<std::vector<Event>, Error> decode(bool binary, std::string_view data) = 0;
  // The sample rate of the PCM the provider takes, which paces outbound audio.
  virtual std::uint32_t input_rate() const noexcept = 0;
};
// The codec of the origin's realtime shape, or null when the origin has no realtime fact.
std::unique_ptr<Codec> make_codec(const descriptor::ValidatedDescriptor&, SemanticLimits = {});
}  // namespace sp::realtime
