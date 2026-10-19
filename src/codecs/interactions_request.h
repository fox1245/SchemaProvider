#pragma once
#include "core/value.h"
#include "descriptor/descriptor.h"
#include <utility>

namespace sp::interactions {
struct ToolDefinition {
  std::string name, description;
  std::shared_ptr<const json::Document> parameters;
};
enum class Delivery { Inline, Uri };
struct AudioResponseFormat {
  std::optional<std::string> mime_type;
  std::optional<std::int32_t> sample_rate, bit_rate;
  std::optional<Delivery> delivery;
};
struct ImageResponseFormat {
  std::optional<std::string> mime_type, aspect_ratio, image_size;
  std::optional<Delivery> delivery;
};
struct VideoResponseFormat {
  std::optional<std::string> aspect_ratio, resolution, duration, gcs_uri;
  std::optional<Delivery> delivery;
};
struct TextResponseFormat {
  std::optional<std::string> mime_type;
  std::shared_ptr<const json::Document> schema;
};
using ResponseFormat = std::variant<AudioResponseFormat, ImageResponseFormat, VideoResponseFormat, TextResponseFormat>;
struct SpeechConfig { std::optional<std::string> voice, speaker, language; };
struct SpeakerConfig {
  std::vector<SpeechConfig> speakers;
  // REST-only conversational cadence, documented by the speech generation guide.
  bool conversational = false;
};
// Both official speech_config wire alternatives are available.
using SpeechConfiguration = std::variant<std::vector<SpeechConfig>, SpeakerConfig>;
enum class TranscriptionMode { Verbatim, Smart };
struct TranscriptionConfig {
  std::vector<std::string> language_codes, custom_vocabulary;
  std::optional<TranscriptionMode> mode;
  // Serialized within the verbatim mode object, never the deprecated top-level fields.
  bool speaker_diarization = false, word_timestamps = false;
};
// Tools the service runs itself. Their calls and results come back as ToolCall parts of kind ServerExecuted and
// ServerToolResult parts, and stay in the native steps for replay.
struct GoogleSearchTool { std::vector<std::string> search_types; };  // web_search, image_search, ...
struct UrlContextTool {};
struct CodeExecutionTool {};
using BuiltinTool = std::variant<GoogleSearchTool, UrlContextTool, CodeExecutionTool>;
// What a request does. Create starts an interaction; the others act on the stored interaction `interaction_id`
// and send no input: Get returns its current state (Pending while it runs), Cancel stops a background one and
// returns what it produced (stop raw "cancelled"), Delete removes it (stop raw "deleted").
enum class Lifecycle { Create, Get, Cancel, Delete };
struct Request {
  std::string model, account_scope, system;
  std::vector<Message> messages;
  std::vector<ToolDefinition> tools;
  std::optional<uint64_t> max_output_tokens;
  std::optional<std::string> thinking_level;
  std::optional<bool> thinking_summaries;
  std::optional<std::string> service_tier;
  std::optional<std::string> required_tool;
  // One format encodes as an object; multiple formats encode as an ordered array.
  std::vector<ResponseFormat> response_format;
  std::optional<SpeechConfiguration> speech_config;
  std::optional<TranscriptionConfig> transcription_config;
  std::vector<BuiltinTool> builtin_tools;
  // The service continues a stored interaction: `messages` holds the new input only, and the result is not
  // replayable from the client (the service holds the history). The earlier turn must have set `store`.
  std::optional<std::string> previous_interaction_id;
  // Whether the service keeps this interaction (unset: false). A later previous_interaction_id needs it.
  std::optional<bool> store;
  // generation_config.video_config.task: text_to_video, image_to_video, reference_to_video, edit or extend.
  std::optional<std::string> video_task;
  // The service runs the interaction in the background and answers at once with a Pending result; it needs `store`.
  std::optional<bool> background;
  // An agent (deep-research-..., for example) runs the interaction instead of `model`; leave `model` empty.
  std::optional<std::string> agent;
  Lifecycle lifecycle = Lifecycle::Create;
  std::string interaction_id;
  // Top-level members the policy admits for this origin (#32), sent as given after the typed ones. Creating
  // requests only.
  std::shared_ptr<const json::Document> extensions;

  static Request simple(std::string model_name, std::string user_prompt,
                        std::optional<uint64_t> max_tokens = std::nullopt,
                        std::string system_prompt = {}) {
    Request request;
    request.model = std::move(model_name);
    request.system = std::move(system_prompt);
    request.messages.push_back(sp::Message::user_text(std::move(user_prompt)));
    request.max_output_tokens = max_tokens;
    return request;
  }
  static Request simple(std::string model_name, std::vector<sp::Message> history,
                        std::optional<uint64_t> max_tokens = std::nullopt,
                        std::string system_prompt = {}) {
    Request request;
    request.model = std::move(model_name);
    request.system = std::move(system_prompt);
    request.messages = std::move(history);
    request.max_output_tokens = max_tokens;
    return request;
  }
};
struct EncodedRequest {
  std::string method, path;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::shared_ptr<const NativeContext> context;
  std::optional<std::uint64_t> max_output_tokens{};
  std::optional<std::uint64_t> model_invocation_limit{1};
};
using EncodeResult = std::variant<EncodedRequest, Error>;
EncodeResult encode(const descriptor::ValidatedDescriptor&, const Request&, bool streaming);
} // namespace sp::interactions
