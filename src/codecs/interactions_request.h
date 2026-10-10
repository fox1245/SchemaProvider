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
