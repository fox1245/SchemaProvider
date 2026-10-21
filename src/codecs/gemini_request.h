#pragma once
#include "core/value.h"
#include "descriptor/descriptor.h"

namespace sp::gemini {
struct ToolDefinition {
  std::string name, description;
  std::shared_ptr<const json::Document> parameters;
};
enum class HistoryMode { NativeOnly, PortableForeign };
enum class ThinkingLevel { Minimal, Low, Medium, High };
enum class SafetyCategory { Harassment, HateSpeech, SexuallyExplicit, DangerousContent, CivicIntegrity };
enum class SafetyThreshold { BlockNone, BlockOnlyHigh, BlockMediumAndAbove, BlockLowAndAbove, Off };
struct SafetySetting {
  SafetyCategory category = SafetyCategory::Harassment;
  SafetyThreshold threshold = SafetyThreshold::BlockMediumAndAbove;
};
enum class ToolChoiceMode { Auto, Any, None, Validated };
struct ToolChoice {
  ToolChoiceMode mode = ToolChoiceMode::Auto;
  std::vector<std::string> allowed_function_names;
};
enum class ResponseModality { Text, Image, Audio };
struct VoiceConfig {
  // VoiceConfig's two wire alternatives are mutually exclusive.
  std::optional<std::string> prebuilt_voice_name, voice;
};
struct SpeakerVoiceConfig { std::string speaker; VoiceConfig voice_config; };
struct SpeechConfig {
  std::optional<VoiceConfig> voice_config;
  std::vector<SpeakerVoiceConfig> speaker_voice_configs;
  std::optional<std::string> language_code;
};
struct ImageConfig { std::optional<std::string> aspect_ratio, image_size; };
enum class TranscriptionMode { Verbatim, Smart };
struct AudioTranscriptionConfig {
  std::vector<std::string> language_codes, custom_vocabulary;
  std::optional<bool> word_timestamp, diarization;
  std::optional<TranscriptionMode> mode;
};
// generationConfig.responseFormat.audio. The encodings (AUDIO_WAV, AUDIO_L16, AUDIO_MP3, AUDIO_OGG_OPUS,
// AUDIO_MULAW, AUDIO_ALAW) and delivery modes (INLINE, URI) are the service's UPPER_SNAKE names, sent as given.
struct AudioFormat {
  std::optional<std::string> mime_type;
  std::optional<std::uint32_t> sample_rate;
  std::optional<std::uint32_t> bit_rate;  // bits per second, for compressed formats
  std::optional<std::string> delivery;
};
// Tools the service runs itself. Code execution's calls and results always arrive as ServerExecuted ToolCall and
// ServerToolResult parts; the other tools' do with server_tool_invocations. Candidate metadata (groundingMetadata,
// urlContextMetadata) is in Completion::stop.details. Every part replays as received.
struct GoogleSearchTool {
  // timeRangeFilter, as RFC 3339 timestamps. The service needs both ends or neither.
  std::optional<std::string> start_time, end_time;
  // searchTypes. Neither set: web search only.
  bool web_search = false, image_search = false;
};
struct UrlContextTool {};
struct CodeExecutionTool {};
struct GoogleMapsTool { std::optional<bool> enable_widget; };
// The service declares the environment's actions (navigate, click_at, ...) as functions. The model calls them like
// client functions: the caller performs each one and answers with a ToolResult.
struct ComputerUseTool {
  std::string environment = "ENVIRONMENT_BROWSER";
  std::vector<std::string> excluded_functions, disabled_safety_policies;
  std::optional<bool> prompt_injection_detection;
};
struct FileSearchTool {
  std::vector<std::string> store_names;  // fileSearchStores/<id>
  std::optional<std::string> metadata_filter;
  std::optional<std::uint32_t> top_k;
};
// New alternatives go at the end: the configuration digest binds the index.
using BuiltinTool = std::variant<GoogleSearchTool, UrlContextTool, CodeExecutionTool, GoogleMapsTool, ComputerUseTool, FileSearchTool>;
// The name a built-in tool bills under in the catalogue (`tool:<type>`): the tool's name in Interactions' `type` and in
// Generate's `tools` member, which spells it in camel case.
std::string_view tool_type(const BuiltinTool&) noexcept;
// A remote MCP server that the service connects to over streamable HTTP and whose tools it runs.
struct McpServer {
  std::string name, url;
  // Sent to the server, typically a credential. Only the names are bound, so a rotated value keeps history replayable.
  std::vector<std::pair<std::string, std::string>> headers;
  std::optional<std::string> timeout, sse_read_timeout;  // durations such as "30s"
  std::optional<bool> terminate_on_close;
};
// The name an MCP server bills under (`tool:mcp:<name>`): the service lists no fee for the server's own tools.
std::string tool_type(const McpServer&);
// toolConfig.retrievalConfig: the user's location and language for Maps and Search grounding.
struct RetrievalConfig {
  std::optional<double> latitude, longitude;
  std::optional<std::string> language_code;
};
struct Request {
  std::string model, account_scope, system;
  std::vector<Message> messages;
  std::vector<ToolDefinition> tools;
  std::optional<uint64_t> max_output_tokens;
  std::optional<uint64_t> thinking_budget;
  std::optional<bool> include_thoughts;
  std::optional<std::string> required_tool;
  HistoryMode history_mode = HistoryMode::NativeOnly;
  std::optional<ThinkingLevel> thinking_level;
  std::optional<double> temperature;
  std::vector<SafetySetting> safety_settings;
  std::optional<ToolChoice> tool_choice;
  std::vector<ResponseModality> response_modalities;
  std::optional<SpeechConfig> speech_config;
  std::optional<ImageConfig> image_config;
  std::optional<AudioTranscriptionConfig> audio_transcription_config;
  std::optional<AudioFormat> audio_format;
  std::vector<BuiltinTool> builtin_tools;
  std::vector<McpServer> mcp_servers;
  // toolConfig.includeServerSideToolInvocations: the answer also carries the calls and results of the tools the
  // service runs (every built-in tool but computer use, and MCP servers). Unset: on exactly when such a built-in tool
  // and a client function are both declared, which the service requires; MCP servers do not need it.
  std::optional<bool> server_tool_invocations;
  std::optional<RetrievalConfig> retrieval_config;
  // Top-level members the policy admits for this origin (#32), sent as given after the typed ones.
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
  // The cache marks of the messages' parts are dropped here and reported (core/cache_marks.h).
  CacheMarkReport cache_marks{};
};
using EncodeResult = std::variant<EncodedRequest, Error>;
EncodeResult encode(const descriptor::ValidatedDescriptor&, const Request&, bool streaming);

// A captured answer as portable history, for HistoryMode::PortableForeign: its text and client function calls,
// without the native seal, signatures or the service's own parts. The SDK never applies it by itself; it is for a
// caller whose captured answer cannot be sent back as it is. `dropped` names the wire type of each part left out.
struct PortableProjection { Message message; std::vector<std::string> dropped; };
PortableProjection project_portable(const Message& captured);
} // namespace sp::gemini
