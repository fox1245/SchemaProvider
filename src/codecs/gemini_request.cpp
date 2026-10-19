#include "codecs/gemini_request.h"
#include "core/request_controls.h"
#include "codecs/media_input.h"
#include "core/native.h"
#include "json/json.h"
#include "descriptor/policy.h"
#include <algorithm>
#include <map>
#include <set>
#include <type_traits>
#include <variant>
#include <charconv>
#include <initializer_list>

namespace sp::gemini {
namespace {
bool name_valid(std::string_view name) {
  if (name.empty() || name.size() > 128) return false;
  for (const auto c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}
// The service's enumerations (encodings, environments, delivery modes) are UPPER_SNAKE names, sent as given.
bool upper_token(std::string_view value) {
  return !value.empty() && value.size() <= 64 &&
         std::all_of(value.begin(), value.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
}
// An RFC 3339 timestamp's characters; the service checks the date itself.
bool timestamp_syntax(std::string_view value) {
  return !value.empty() && value.size() <= 64 && value.front() >= '0' && value.front() <= '9' &&
         std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || c == 'T' || c == 'Z' || c == ':' || c == '+' || c == '-' || c == '.'; });
}
// A protobuf Duration in JSON: seconds with an optional fraction, then "s".
bool duration_syntax(std::string_view value) {
  if (value.size() < 2 || value.size() > 32 || value.back() != 's') return false;
  value.remove_suffix(1);
  const auto dot = value.find('.');
  const auto digits = [](std::string_view part) { return !part.empty() && std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }); };
  return dot == std::string_view::npos ? digits(value) : digits(value.substr(0, dot)) && digits(value.substr(dot + 1));
}
bool https_url(std::string_view value) {
  return value.size() > 8 && value.size() <= 2048 && value.starts_with("https://") &&
         std::none_of(value.begin(), value.end(), [](unsigned char c) { return c <= 0x20 || c >= 0x7f; });
}
bool header_name(std::string_view value) {
  return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
  });
}
bool header_value(std::string_view value) {
  return value.size() <= 8192 && value.find_first_of(std::string_view("\r\n\0", 3)) == std::string_view::npos;
}
bool language_tag(std::string_view value) {
  return !value.empty() && value.size() <= 35 &&
         std::all_of(value.begin(), value.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'; });
}
std::string number_text(double value) {
  char bytes[64]; const auto converted = std::to_chars(bytes, bytes + sizeof bytes, value);
  return std::string(bytes, converted.ptr);
}
std::string_view thinking_level_name(ThinkingLevel level) {
  switch (level) {
    case ThinkingLevel::Minimal: return "minimal";
    case ThinkingLevel::Low: return "low";
    case ThinkingLevel::Medium: return "medium";
    case ThinkingLevel::High: return "high";
  }
  return {};
}
std::string_view safety_category_name(SafetyCategory category) {
  switch (category) {
    case SafetyCategory::Harassment: return "HARM_CATEGORY_HARASSMENT";
    case SafetyCategory::HateSpeech: return "HARM_CATEGORY_HATE_SPEECH";
    case SafetyCategory::SexuallyExplicit: return "HARM_CATEGORY_SEXUALLY_EXPLICIT";
    case SafetyCategory::DangerousContent: return "HARM_CATEGORY_DANGEROUS_CONTENT";
    case SafetyCategory::CivicIntegrity: return "HARM_CATEGORY_CIVIC_INTEGRITY";
  }
  return {};
}
std::string_view safety_threshold_name(SafetyThreshold threshold) {
  switch (threshold) {
    case SafetyThreshold::BlockNone: return "BLOCK_NONE";
    case SafetyThreshold::BlockOnlyHigh: return "BLOCK_ONLY_HIGH";
    case SafetyThreshold::BlockMediumAndAbove: return "BLOCK_MEDIUM_AND_ABOVE";
    case SafetyThreshold::BlockLowAndAbove: return "BLOCK_LOW_AND_ABOVE";
    case SafetyThreshold::Off: return "OFF";
  }
  return {};
}
std::string_view tool_choice_name(ToolChoiceMode mode) {
  switch (mode) {
    case ToolChoiceMode::Auto: return "AUTO";
    case ToolChoiceMode::Any: return "ANY";
    case ToolChoiceMode::None: return "NONE";
    case ToolChoiceMode::Validated: return "VALIDATED";
  }
  return {};
}
std::string_view modality_name(ResponseModality modality) {
  switch (modality) {
    case ResponseModality::Text: return "TEXT";
    case ResponseModality::Image: return "IMAGE";
    case ResponseModality::Audio: return "AUDIO";
  }
  return {};
}
bool one_of(std::string_view value, std::initializer_list<std::string_view> values) {
  for (auto item : values) if (item == value) return true;
  return false;
}
bool voice_valid(const VoiceConfig& voice) {
  return !(voice.prebuilt_voice_name && voice.voice);
}
void write_voice(json::BoundedWriter& w, const VoiceConfig& voice) {
  w.raw("{");
  if (voice.prebuilt_voice_name) w.raw("\"prebuiltVoiceConfig\":{\"voiceName\":").quoted(*voice.prebuilt_voice_name).raw("}");
  if (voice.voice) w.raw("\"voice\":").quoted(*voice.voice);
  w.raw("}");
}
void write_strings(json::BoundedWriter& w, const std::vector<std::string>& values) {
  w.raw("["); bool comma = false;
  for (const auto& value : values) { if (comma) w.raw(","); comma = true; w.quoted(value); }
  w.raw("]");
}
// generateContent media parts: inlineData for inline payloads, fileData for Files-API URIs and
// public locations. Admission has already established that the family accepts the kind and source.
void write_media(json::BoundedWriter& w, const Media& media) {
  if (media.source == MediaSource::Inline) {
    w.raw("{\"inlineData\":{\"mimeType\":").quoted(media.mime).raw(",\"data\":\"").raw(*media.data).raw("\"");
    if (!media.name.empty()) w.raw(",\"displayName\":").quoted(media.name);
    w.raw("}}");
    return;
  }
  w.raw("{\"fileData\":{");
  if (!media.mime.empty()) w.raw("\"mimeType\":").quoted(media.mime).raw(",");
  w.raw("\"fileUri\":").quoted(media.reference);
  if (!media.name.empty()) w.raw(",\"displayName\":").quoted(media.name);
  w.raw("}}");
}
}
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](std::string message) -> EncodeResult { return Error{ErrorKind::InvalidRequest, std::move(message)}; };
  auto replay_bad = []() -> EncodeResult { return Error{ErrorKind::ReplayIneligible, "native replay provenance, binding or content mismatch"}; };
  if (descriptor.family() != "google.generate") return Error{ErrorKind::InvalidConfig, "Gemini encoder requires generate descriptor"};
  if (request.model.empty() || request.model.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._") != std::string::npos) return bad("literal model name required");
  const auto base = "/v1beta/models/" + request.model;
  // descriptor::load holds a google.generate descriptor to one model in both paths, or to {model}; a literal route
  // serves only the request for its own model.
  if (!descriptor.model_routed() &&
      (descriptor.path(false) != base + ":generateContent" || descriptor.path(true) != base + ":streamGenerateContent?alt=sse"))
    return Error{ErrorKind::InvalidConfig, "Gemini descriptor paths must target the same literal model"};
  const auto& resources = descriptor.policy()->resources();
  auto effective = descriptor::effective_defaults(descriptor, request.model);
  if (request.max_output_tokens) effective.max_output_tokens = request.max_output_tokens;
  if (request.thinking_budget) effective.thinking_budget = request.thinking_budget;
  if (request.include_thoughts) effective.include_thoughts = request.include_thoughts;
  if (request.history_mode != HistoryMode::NativeOnly && request.history_mode != HistoryMode::PortableForeign)
    return bad("invalid history mode");
  if (request.thinking_level) {
    const auto level = thinking_level_name(*request.thinking_level);
    if (level.empty()) return bad("invalid thinking level");
    if (request.thinking_budget) return bad("thinking level and budget are mutually exclusive");
    effective.thinking_budget.reset();
    effective.thinking_level = level;
  }
  if (request.temperature) effective.temperature = request.temperature;
  unsigned safety_categories = 0;
  for (const auto& setting : request.safety_settings) {
    if (safety_category_name(setting.category).empty() || safety_threshold_name(setting.threshold).empty())
      return bad("safety settings require valid unique categories and thresholds");
    const auto category = 1U << static_cast<unsigned>(setting.category);
    if (safety_categories & category) return bad("safety settings require valid unique categories and thresholds");
    safety_categories |= category;
  }
  if (request.tool_choice) {
    if (request.required_tool) return bad("tool choice and required function are mutually exclusive");
    if (tool_choice_name(request.tool_choice->mode).empty()) return bad("invalid tool choice mode");
    if (!request.tool_choice->allowed_function_names.empty() &&
        request.tool_choice->mode != ToolChoiceMode::Any && request.tool_choice->mode != ToolChoiceMode::Validated)
      return bad("allowed functions require ANY or VALIDATED mode");
  }
  if (auto error = descriptor::validate_choices(descriptor, request.model, effective)) return bad(std::move(*error));
  unsigned modalities = 0;
  for (auto modality : request.response_modalities) {
    if (modality_name(modality).empty()) return bad("invalid response modality");
    const auto bit = 1U << static_cast<unsigned>(modality);
    if (modalities & bit) return bad("response modalities must be unique");
    modalities |= bit;
  }
  if (request.speech_config) {
    const auto& speech = *request.speech_config;
    if (speech.voice_config && !speech.speaker_voice_configs.empty())
      return bad("single and multi-speaker voice configurations are mutually exclusive");
    if (speech.voice_config && !voice_valid(*speech.voice_config)) return bad("voice alternatives are mutually exclusive");
    if (speech.speaker_voice_configs.size() > resources.request_parts) return bad("speaker configuration count limit exceeded");
    for (const auto& speaker : speech.speaker_voice_configs)
      if (speaker.speaker.empty() || !voice_valid(speaker.voice_config)) return bad("speaker name and exclusive voice configuration required");
    if (speech.language_code && !request_controls::language_tag(*speech.language_code))
      return bad("speech language code must be a language tag");
  }
  if (request.image_config) {
    const auto& image = *request.image_config;
    if (image.aspect_ratio && !request_controls::aspect_ratio_syntax(*image.aspect_ratio)) return bad("image aspect ratio must be W:H");
    if (image.image_size && !request_controls::size_token(*image.image_size)) return bad("image size must be a short token");
  }
  if (request.audio_transcription_config) {
    const auto& transcription = *request.audio_transcription_config;
    if (transcription.language_codes.size() > resources.request_parts || transcription.custom_vocabulary.size() > 1000 ||
        transcription.custom_vocabulary.size() > resources.request_parts) return bad("transcription list count limit exceeded");
    if (transcription.mode && *transcription.mode != TranscriptionMode::Verbatim && *transcription.mode != TranscriptionMode::Smart)
      return bad("invalid transcription mode");
    const bool annotations = transcription.word_timestamp.value_or(false) || transcription.diarization.value_or(false);
    if (annotations && (transcription.mode == TranscriptionMode::Smart || !transcription.custom_vocabulary.empty()))
      return bad("timestamps and diarization cannot combine with SMART or custom vocabulary");
  }
  if (request.messages.empty() || request.messages.size() > resources.request_messages || request.tools.size() > resources.request_tools)
    return bad("request count limit exceeded");
  bool has_media = false;
  bool captured_history = false;
  std::optional<Error> tool_error;
  for (const auto& message : request.messages) {
    if (message.parts.size() > resources.request_parts) return bad("request part count limit exceeded");
    captured_history = captured_history || static_cast<bool>(message.native);
    for (const auto& part : message.parts) if (std::holds_alternative<Media>(part)) has_media = true;
    else if (const auto* tool = std::get_if<ToolResult>(&part); tool && !tool_error) {
      // A JSON object beside media-only parts is the response object (computer use answers {"url": ...} and a
      // screenshot); beside text parts it would have no lane.
      if (!tool->content_parts.empty() && !tool->content.empty() &&
          std::any_of(tool->content_parts.begin(), tool->content_parts.end(), [](const auto& nested) { return std::holds_alternative<Text>(nested); }))
        tool_error = Error{ErrorKind::InvalidRequest, "tool result content and content_parts are mutually exclusive"};
      else if (tool->content_parts.size() > resources.request_parts)
        tool_error = Error{ErrorKind::InvalidRequest, "tool result part count limit exceeded"};
      else if (std::any_of(tool->content_parts.begin(), tool->content_parts.end(), [](const auto& nested) { return std::holds_alternative<Opaque>(nested); }))
        tool_error = Error{ErrorKind::Unsupported, "tool result blocks are not admitted for this family"};
      else for (const auto& nested : tool->content_parts) if (const auto* media = std::get_if<Media>(&nested)) {
        has_media = true;
        // Inline only, in any form the family's media policy admits; the service decides what a model reads.
        if (static_cast<unsigned>(media->kind) < media_kind_count && static_cast<unsigned>(media->source) <= 2 &&
            media->source != MediaSource::Inline)
          tool_error = Error{ErrorKind::Unsupported, "function response media requires inline bytes"};
        else tool_error = media_input::admit(descriptor, *media);
        if (!tool_error && (media->detail != ImageDetail::Auto || !media->name.empty() ||
                            !media->id.empty() || !media->transcript.empty()))
          tool_error = Error{ErrorKind::Unsupported, "function response media has unsupported controls or metadata"};
        if (tool_error) break;
      }
    }
    if (!message.native) {
      if (message.wire_output) return replay_bad();
      if (message.role == Role::Assistant) {
        if (request.history_mode != HistoryMode::PortableForeign) return replay_bad();
        for (const auto& part : message.parts) {
          if (std::holds_alternative<Text>(part)) continue;
          const auto* call = std::get_if<ToolCall>(&part);
          if (!call || call->kind != ToolCallKind::ClientExecuted || !call->wire_type.empty() || call->wire_metadata)
            return replay_bad();
        }
      } else for (const auto& part : message.parts) {
        if (const auto* media = std::get_if<Media>(&part)) {
          if (message.role != Role::User) return bad("media inputs require user role");
          if (auto error = media_input::admit(descriptor, *media)) return *error;
        } else if (!std::holds_alternative<Text>(part) && !std::holds_alternative<ToolResult>(part))
          return replay_bad();
      }
    }
  }
  if (tool_error && !captured_history) return *tool_error;
  if (auto refusal = request_controls::validate_extensions(descriptor, request.extensions))
    return Error{refusal->admitted ? ErrorKind::InvalidRequest : ErrorKind::Unsupported, std::move(refusal->message)};
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, effective, streaming));
  if (!context->history_valid_) return replay_bad();
  if (!context->valid_) return Error{ErrorKind::ResourceLimit, "native binding could not be captured"};
  if (tool_error) return *tool_error;
  EncodedRequest result{"POST", descriptor.resolved_path(streaming, request.model), {}, {}, {}};
  for (const auto& header : descriptor.headers()) {
    std::string key = header.first;
    for (auto& c : key) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (key != "content-type" && key != "accept") result.headers.push_back(header);
  }
  result.headers.emplace_back("Content-Type", "application/json");
  result.headers.emplace_back("Accept", streaming ? "text/event-stream" : "application/json");
  auto build = [&](json::BoundedWriter& w) -> std::optional<Error> {
    auto invalid = [](const char* text) { return Error{ErrorKind::InvalidRequest, text}; };
    std::set<std::string_view> tool_names;
    w.raw("{\"generationConfig\":{");
    bool generation_comma = false;
    if (effective.max_output_tokens) {
      w.quoted(descriptor.max_output_tokens_member()).raw(":").raw(std::to_string(*effective.max_output_tokens));
      generation_comma = true;
    }
    if (effective.temperature) {
      if (generation_comma) w.raw(",");
      char bytes[64]; const auto converted = std::to_chars(bytes, bytes + sizeof bytes, *effective.temperature);
      w.raw("\"temperature\":").raw(std::string_view(bytes, converted.ptr));
      generation_comma = true;
    }
    if (effective.include_thoughts || effective.thinking_budget || effective.thinking_level) {
      if (generation_comma) w.raw(",");
      w.raw("\"thinkingConfig\":{");
      if (effective.include_thoughts) w.raw("\"includeThoughts\":").raw(*effective.include_thoughts ? "true" : "false");
      if (effective.thinking_budget) {
        if (effective.include_thoughts) w.raw(",");
        w.raw("\"thinkingBudget\":").raw(std::to_string(*effective.thinking_budget));
      }
      if (effective.thinking_level) {
        if (effective.include_thoughts || effective.thinking_budget) w.raw(",");
        w.raw("\"thinkingLevel\":").quoted(*effective.thinking_level);
      }
      w.raw("}");
      generation_comma = true;
    }
    if (!request.response_modalities.empty()) {
      if (generation_comma) w.raw(",");
      w.raw("\"responseModalities\":["); bool comma = false;
      for (auto modality : request.response_modalities) { if (comma) w.raw(","); comma = true; w.quoted(modality_name(modality)); }
      w.raw("]"); generation_comma = true;
    }
    if (request.speech_config) {
      if (generation_comma) w.raw(",");
      const auto& speech = *request.speech_config;
      w.raw("\"speechConfig\":{"); bool comma = false;
      if (speech.voice_config) { w.raw("\"voiceConfig\":"); write_voice(w, *speech.voice_config); comma = true; }
      if (!speech.speaker_voice_configs.empty()) {
        if (comma) w.raw(",");
        w.raw("\"multiSpeakerVoiceConfig\":{\"speakerVoiceConfigs\":["); bool speaker_comma = false;
        for (const auto& speaker : speech.speaker_voice_configs) {
          if (speaker_comma) w.raw(",");
          speaker_comma = true;
          w.raw("{\"speaker\":").quoted(speaker.speaker).raw(",\"voiceConfig\":"); write_voice(w, speaker.voice_config); w.raw("}");
        }
        w.raw("]}"); comma = true;
      }
      if (speech.language_code) { if (comma) w.raw(","); w.raw("\"languageCode\":").quoted(*speech.language_code); }
      w.raw("}"); generation_comma = true;
    }
    if (request.image_config) {
      if (generation_comma) w.raw(",");
      const auto& image = *request.image_config;
      w.raw("\"imageConfig\":{");
      if (image.aspect_ratio) w.raw("\"aspectRatio\":").quoted(*image.aspect_ratio);
      if (image.image_size) { if (image.aspect_ratio) w.raw(","); w.raw("\"imageSize\":").quoted(*image.image_size); }
      w.raw("}"); generation_comma = true;
    }
    if (request.audio_transcription_config) {
      if (generation_comma) w.raw(",");
      const auto& transcription = *request.audio_transcription_config;
      w.raw("\"audioTranscriptionConfig\":{\"languageCodes\":"); write_strings(w, transcription.language_codes);
      if (!transcription.custom_vocabulary.empty()) { w.raw(",\"customVocabulary\":"); write_strings(w, transcription.custom_vocabulary); }
      if (transcription.word_timestamp) w.raw(",\"wordTimestamp\":").raw(*transcription.word_timestamp ? "true" : "false");
      if (transcription.diarization) w.raw(",\"diarization\":").raw(*transcription.diarization ? "true" : "false");
      if (transcription.mode) w.raw(",\"mode\":").quoted(*transcription.mode == TranscriptionMode::Smart ? "SMART" : "VERBATIM");
      w.raw("}"); generation_comma = true;
    }
    if (request.audio_format && (request.audio_format->mime_type || request.audio_format->sample_rate || request.audio_format->bit_rate || request.audio_format->delivery)) {
      const auto& format = *request.audio_format;
      if ((format.mime_type && !upper_token(*format.mime_type)) || (format.delivery && !upper_token(*format.delivery)))
        return invalid("audio format must be an upper-case token");
      if ((format.sample_rate && (!*format.sample_rate || *format.sample_rate > 384000)) || (format.bit_rate && (!*format.bit_rate || *format.bit_rate > 1000000)))
        return invalid("audio sample or bit rate is out of range");
      if (generation_comma) w.raw(",");
      w.raw("\"responseFormat\":{\"audio\":{");
      bool comma = false;
      auto member = [&](std::string_view key) -> json::BoundedWriter& { if (comma) w.raw(","); comma = true; return w.quoted(key).raw(":"); };
      if (format.mime_type) member("mimeType").quoted(*format.mime_type);
      if (format.sample_rate) member("sampleRate").raw(std::to_string(*format.sample_rate));
      if (format.bit_rate) member("bitRate").raw(std::to_string(*format.bit_rate));
      if (format.delivery) member("delivery").quoted(*format.delivery);
      w.raw("}}"); generation_comma = true;
    }
    w.raw("}");
    if (!request.safety_settings.empty()) {
      w.raw(",\"safetySettings\":[");
      bool comma = false;
      for (const auto& setting : request.safety_settings) {
        if (comma) w.raw(",");
        comma = true;
        w.raw("{\"category\":").quoted(safety_category_name(setting.category))
            .raw(",\"threshold\":").quoted(safety_threshold_name(setting.threshold)).raw("}");
      }
      w.raw("]");
    }
    if (!request.system.empty()) w.raw(",\"systemInstruction\":{\"parts\":[{\"text\":").quoted(request.system).raw("}]}");
    const bool computer_use = std::any_of(request.builtin_tools.begin(), request.builtin_tools.end(),
                                          [](const BuiltinTool& tool) { return std::holds_alternative<ComputerUseTool>(tool); });
    // Every built-in tool but computer use, and every MCP server, runs on the service.
    const bool builtin_service_tools = request.builtin_tools.size() > (computer_use ? 1u : 0u);
    const bool service_runs_tools = !request.mcp_servers.empty() || builtin_service_tools;
    if (!request.tools.empty() || !request.builtin_tools.empty() || !request.mcp_servers.empty()) {
      w.raw(",\"tools\":[");
      bool tool_comma = false;
      auto next_tool = [&]() -> json::BoundedWriter& { if (tool_comma) w.raw(","); tool_comma = true; return w; };
      if (!request.tools.empty()) {
        next_tool().raw("{\"functionDeclarations\":[");
        bool comma = false;
        for (const auto& tool : request.tools) {
          if (!name_valid(tool.name) || !tool_names.insert(tool.name).second || !tool.parameters || !tool.parameters->root().is_object()) return invalid("client functions require unique names and object schemas");
          if (comma) w.raw(",");
          comma = true;
          w.raw("{\"name\":").quoted(tool.name).raw(",\"description\":").quoted(tool.description).raw(",\"parametersJsonSchema\":").value(tool.parameters->root(), 4).raw("}");
        }
        w.raw("]}");
      }
      std::set<std::size_t> builtin_kinds;
      for (const auto& tool : request.builtin_tools) {
        if (!builtin_kinds.insert(tool.index()).second) return invalid("built-in tools must be unique");
        auto strings = [&](const std::vector<std::string>& values) {
          w.raw("[");
          for (std::size_t i = 0; i < values.size(); ++i) { if (i) w.raw(","); w.quoted(values[i]); }
          w.raw("]");
        };
        const char* refused = std::visit([&](const auto& value) -> const char* {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, GoogleSearchTool>) {
            if (value.start_time.has_value() != value.end_time.has_value() || (value.start_time && (!timestamp_syntax(*value.start_time) || !timestamp_syntax(*value.end_time))))
              return "search time range requires both RFC 3339 ends";
            next_tool().raw("{\"googleSearch\":{");
            if (value.start_time) w.raw("\"timeRangeFilter\":{\"startTime\":").quoted(*value.start_time).raw(",\"endTime\":").quoted(*value.end_time).raw("}");
            if (value.web_search || value.image_search) {
              if (value.start_time) w.raw(",");
              w.raw("\"searchTypes\":{");
              if (value.web_search) w.raw("\"webSearch\":{}");
              if (value.image_search) w.raw(value.web_search ? ",\"imageSearch\":{}" : "\"imageSearch\":{}");
              w.raw("}");
            }
            w.raw("}}");
          } else if constexpr (std::is_same_v<T, UrlContextTool>) next_tool().raw("{\"urlContext\":{}}");
          else if constexpr (std::is_same_v<T, CodeExecutionTool>) next_tool().raw("{\"codeExecution\":{}}");
          else if constexpr (std::is_same_v<T, GoogleMapsTool>) {
            next_tool().raw("{\"googleMaps\":{");
            if (value.enable_widget) w.raw("\"enableWidget\":").raw(*value.enable_widget ? "true" : "false");
            w.raw("}}");
          } else if constexpr (std::is_same_v<T, ComputerUseTool>) {
            if (!upper_token(value.environment) || !std::all_of(value.disabled_safety_policies.begin(), value.disabled_safety_policies.end(), upper_token))
              return "computer use environment and policies must be upper-case tokens";
            std::set<std::string_view> excluded;
            for (const auto& name : value.excluded_functions)
              if (!name_valid(name) || !excluded.insert(name).second) return "excluded computer use functions require unique names";
            next_tool().raw("{\"computerUse\":{\"environment\":").quoted(value.environment);
            if (!value.excluded_functions.empty()) { w.raw(",\"excludedPredefinedFunctions\":"); strings(value.excluded_functions); }
            if (!value.disabled_safety_policies.empty()) { w.raw(",\"disabledSafetyPolicies\":"); strings(value.disabled_safety_policies); }
            if (value.prompt_injection_detection) w.raw(",\"enablePromptInjectionDetection\":").raw(*value.prompt_injection_detection ? "true" : "false");
            w.raw("}}");
          } else {
            std::set<std::string_view> stores;
            if (value.store_names.empty()) return "file search requires unique store names";
            for (const auto& name : value.store_names)
              if (!name.starts_with("fileSearchStores/") || name.size() <= 17 || name.size() > 256 || !stores.insert(name).second ||
                  std::any_of(name.begin(), name.end(), [](unsigned char c) { return c <= 0x20 || c >= 0x7f; }))
                return "file search requires unique store names";
            if ((value.metadata_filter && (value.metadata_filter->empty() || value.metadata_filter->size() > 8192)) || (value.top_k && !*value.top_k))
              return "file search filter or count is out of range";
            next_tool().raw("{\"fileSearch\":{\"fileSearchStoreNames\":"); strings(value.store_names);
            if (value.metadata_filter) w.raw(",\"metadataFilter\":").quoted(*value.metadata_filter);
            if (value.top_k) w.raw(",\"topK\":").raw(std::to_string(*value.top_k));
            w.raw("}}");
          }
          return nullptr;
        }, tool);
        if (refused) return invalid(refused);
      }
      if (!request.mcp_servers.empty()) {
        next_tool().raw("{\"mcpServers\":[");
        std::set<std::string_view> servers;
        for (std::size_t i = 0; i < request.mcp_servers.size(); ++i) {
          const auto& server = request.mcp_servers[i];
          if (!name_valid(server.name) || !servers.insert(server.name).second || !https_url(server.url))
            return invalid("MCP servers require unique names and https URLs");
          std::set<std::string> header_names;
          for (const auto& [name, value] : server.headers) {
            std::string lower(name);
            for (auto& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
            if (!header_name(name) || !header_value(value) || !header_names.insert(std::move(lower)).second) return invalid("MCP server headers require unique names and single-line values");
          }
          if ((server.timeout && !duration_syntax(*server.timeout)) || (server.sse_read_timeout && !duration_syntax(*server.sse_read_timeout)))
            return invalid("MCP server timeouts must be durations in seconds");
          if (i) w.raw(",");
          w.raw("{\"name\":").quoted(server.name).raw(",\"streamableHttpTransport\":{\"url\":").quoted(server.url);
          if (!server.headers.empty()) {
            w.raw(",\"headers\":{");
            for (std::size_t h = 0; h < server.headers.size(); ++h) { if (h) w.raw(","); w.quoted(server.headers[h].first).raw(":").quoted(server.headers[h].second); }
            w.raw("}");
          }
          if (server.timeout) w.raw(",\"timeout\":").quoted(*server.timeout);
          if (server.sse_read_timeout) w.raw(",\"sseReadTimeout\":").quoted(*server.sse_read_timeout);
          if (server.terminate_on_close) w.raw(",\"terminateOnClose\":").raw(*server.terminate_on_close ? "true" : "false");
          w.raw("}}");
        }
        w.raw("]}");
      }
      w.raw("]");
    }
    // The service refuses a built-in tool next to a client function unless it also returns the tool's invocations
    // (toolCall and toolResponse parts), so that combination turns them on by default. MCP servers do not need them.
    const bool server_invocations = request.server_tool_invocations.value_or(!request.tools.empty() && builtin_service_tools);
    if (server_invocations && !service_runs_tools) return invalid("server tool invocations require a service-run tool");
    const bool write_invocations = service_runs_tools && (request.server_tool_invocations || server_invocations);
    if (request.retrieval_config) {
      const auto& retrieval = *request.retrieval_config;
      if (retrieval.latitude.has_value() != retrieval.longitude.has_value() ||
          (retrieval.latitude && !(*retrieval.latitude >= -90.0 && *retrieval.latitude <= 90.0 && *retrieval.longitude >= -180.0 && *retrieval.longitude <= 180.0)) ||
          (retrieval.language_code && !language_tag(*retrieval.language_code)))
        return invalid("retrieval location or language is invalid");
    }
    const bool retrieval = request.retrieval_config && (request.retrieval_config->latitude || request.retrieval_config->language_code);
    const bool tool_config = request.required_tool || request.tool_choice || write_invocations || retrieval;
    bool config_comma = false;
    if (tool_config) w.raw(",\"toolConfig\":{");
    if (request.required_tool) {
      if (!tool_names.contains(*request.required_tool) && !(computer_use && name_valid(*request.required_tool))) return invalid("required function must be declared");
      w.raw("\"functionCallingConfig\":{\"mode\":\"ANY\",\"allowedFunctionNames\":[").quoted(*request.required_tool).raw("]}");
      config_comma = true;
    }
    if (request.tool_choice) {
      const auto& choice = *request.tool_choice;
      if (tool_names.empty() && !computer_use && choice.mode != ToolChoiceMode::None) return invalid("function choice requires declared functions");
      std::set<std::string_view> allowed;
      w.raw("\"functionCallingConfig\":{\"mode\":").quoted(tool_choice_name(choice.mode));
      if (!choice.allowed_function_names.empty()) {
        w.raw(",\"allowedFunctionNames\":[");
        bool comma = false;
        for (const auto& name : choice.allowed_function_names) {
          // Computer use's functions are the service's own: their names are not known here.
          if (!name_valid(name) || !(tool_names.contains(name) || computer_use) || !allowed.insert(name).second)
            return invalid("allowed functions require unique declared names");
          if (comma) w.raw(",");
          comma = true;
          w.quoted(name);
        }
        w.raw("]");
      }
      w.raw("}");
      config_comma = true;
    }
    if (write_invocations) {
      if (config_comma) w.raw(",");
      w.raw("\"includeServerSideToolInvocations\":").raw(server_invocations ? "true" : "false");
      config_comma = true;
    }
    if (retrieval) {
      const auto& value = *request.retrieval_config;
      if (config_comma) w.raw(",");
      w.raw("\"retrievalConfig\":{");
      if (value.latitude) w.raw("\"latLng\":{\"latitude\":").raw(number_text(*value.latitude)).raw(",\"longitude\":").raw(number_text(*value.longitude)).raw("}");
      if (value.language_code) w.raw(value.latitude ? ",\"languageCode\":" : "\"languageCode\":").quoted(*value.language_code);
      w.raw("}");
    }
    if (tool_config) w.raw("}");
    struct Pending { std::string_view name; bool wire_id; };
    std::map<std::string_view, Pending> pending;
    std::set<std::string_view> all_calls;
    w.raw(",").quoted(descriptor.request_messages_member()).raw(":[");
    bool comma = false;
    for (const auto& m : request.messages) {
      if (m.parts.empty()) return invalid("empty content message");
      if (comma) w.raw(",");
      comma = true;
      if (m.role == Role::Assistant) {
        if (!pending.empty()) return invalid("all function results must precede the next model message");
        if (m.native) {
          if (!m.wire_output || !m.wire_output->root().is_array()) return Error{ErrorKind::ReplayIneligible, "native parts array required"};
          // The service refuses some of its own parts when they come back (an MCP toolCall has no toolType). The
          // policy lists those shapes, and such a message fails here instead of as the service's 400.
          for (const auto part : m.wire_output->root().elements())
            for (const auto& [member, missing] : descriptor.family_policy().unreplayable_parts)
              if (part.get(member).is_object() && !part.get(member).get(missing).valid())
                return Error{ErrorKind::Unsupported, "captured part is one the service refuses to take back"};
          for (const auto& part : m.parts) if (auto call = std::get_if<ToolCall>(&part)) {
            // The service ran its own calls and answered them in the same message; they replay with the wire.
            if (call->kind == ToolCallKind::ServerExecuted) continue;
            if (call->kind != ToolCallKind::ClientExecuted || !(tool_names.contains(call->name) || computer_use) || !all_calls.insert(call->id).second || !call->wire_metadata) return invalid("function ownership mismatch");
            const auto fc = call->wire_metadata->root().get("functionCall");
            pending.emplace(call->id, Pending{call->name, fc.get("id").valid()});
          }
          w.raw("{\"role\":\"model\",\"parts\":").value(m.wire_output->root(), 3).raw("}");
        } else {
          w.raw("{\"role\":\"model\",\"parts\":[");
          bool pc = false, first_call = true;
          for (const auto& part : m.parts) {
            if (pc) w.raw(",");
            pc = true;
            if (const auto* text = std::get_if<Text>(&part)) w.raw("{\"text\":").quoted(text->value).raw("}");
            else if (const auto* call = std::get_if<ToolCall>(&part)) {
              if (call->id.empty() || !tool_names.contains(call->name) || !all_calls.insert(call->id).second ||
                  !call->input || !call->input->root().is_object()) return invalid("portable function requires unique identity, declared name and object arguments");
              pending.emplace(call->id, Pending{call->name, true});
              w.raw("{\"functionCall\":{\"id\":").quoted(call->id).raw(",\"name\":").quoted(call->name)
                  .raw(",\"args\":").value(call->input->root(), 6).raw("}");
              // Google's explicit imported-history bypass applies only to the
              // first foreign function call, never native or text-only parts.
              if (first_call) w.raw(",\"thoughtSignature\":\"skip_thought_signature_validator\"");
              first_call = false;
              w.raw("}");
            } else return Error{ErrorKind::ReplayIneligible, "portable assistant parts must be text or client functions"};
          }
          w.raw("]}");
        }
      } else if (m.role == Role::User || m.role == Role::Tool) {
        const bool results_only = !pending.empty();
        if (m.role == Role::Tool && !results_only) return invalid("tool result lacks pending function");
        w.raw("{\"role\":\"user\",\"parts\":[");
        bool pc = false;
        for (const auto& p : m.parts) {
          if (pc) w.raw(",");
          pc = true;
          if (auto text = std::get_if<Text>(&p)) {
            if (results_only || m.role == Role::Tool) return invalid("function results must be immediate and complete");
            w.raw("{\"text\":").quoted(text->value).raw("}");
          } else if (auto media = std::get_if<Media>(&p)) {
            if (results_only || m.role == Role::Tool) return invalid("media cannot appear among function results");
            write_media(w, *media);
          } else if (auto tr = std::get_if<ToolResult>(&p)) {
            auto found = pending.find(tr->tool_use_id);
            if (found == pending.end()) return invalid("function result lacks unique client ownership");
            w.raw("{\"functionResponse\":{\"name\":").quoted(found->second.name);
            if (found->second.wire_id) w.raw(",\"id\":").quoted(tr->tool_use_id);
            w.raw(",\"response\":");
            if (tr->content_parts.empty() || !tr->content.empty()) {
              // Legacy text content must be one JSON object, exactly as before typed parts existed.
              auto parsed = json::parse(tr->content, {resources.request_bytes, resources.json_depth});
              auto doc = std::get_if<json::Document>(&parsed);
              if ((!doc || !doc->root().is_object()) && tr->content_parts.empty()) return invalid("function result must be JSON object");
              if (!doc || !doc->root().is_object()) return invalid("tool result content must be a JSON object beside media parts");
              if (tr->is_error) w.raw("{\"error\":").value(doc->root(), 6).raw("}");
              else w.value(doc->root(), 5);
              if (!tr->content_parts.empty()) {
                w.raw(",\"parts\":[");
                bool comma = false;
                for (const auto& nested : tr->content_parts) {
                  const auto& media = std::get<Media>(nested);
                  if (comma) w.raw(",");
                  comma = true;
                  w.raw("{\"inlineData\":{\"mimeType\":").quoted(media.mime).raw(",\"data\":\"").raw(*media.data).raw("\"}}");
                  if (!w.ok()) return Error{ErrorKind::ResourceLimit, "request exceeds JSON limits or encoding"};
                }
                w.raw("]");
              }
            } else {
              // The protocol separates text/JSON response from binary parts. Preserve
              // each lane's order without inventing cross-lane references: it cannot
              // express text/media interleaving or named FunctionResponseBlob values.
              w.raw(tr->is_error ? "{\"error\":[" : "{\"output\":[");
              bool comma = false; bool binary = false;
              for (const auto& nested : tr->content_parts) {
                if (const auto* text = std::get_if<Text>(&nested)) {
                  if (comma) w.raw(",");
                  comma = true;
                  w.quoted(text->value);
                } else binary = true;
                if (!w.ok()) return Error{has_media ? ErrorKind::ResourceLimit : ErrorKind::InvalidRequest,
                                         "request exceeds JSON limits or encoding"};
              }
              w.raw("]}");
              if (binary) {
                w.raw(",\"parts\":["); comma = false;
                for (const auto& nested : tr->content_parts) if (const auto* media = std::get_if<Media>(&nested)) {
                  if (comma) w.raw(",");
                  comma = true;
                  w.raw("{\"inlineData\":{\"mimeType\":").quoted(media->mime)
                      .raw(",\"data\":\"").raw(*media->data).raw("\"}}");
                  if (!w.ok()) return Error{ErrorKind::ResourceLimit, "request exceeds JSON limits or encoding"};
                }
                w.raw("]");
              }
            }
            w.raw("}}"); pending.erase(found);
          } else return Error{ErrorKind::Unsupported, "unsupported imported Gemini part"};
        }
        w.raw("]}");
        if (results_only && !pending.empty()) return invalid("all function results must appear together");
      } else return Error{ErrorKind::Unsupported, "Gemini supports user, tool and captured model messages"};
      if (!w.ok()) return Error{has_media ? ErrorKind::ResourceLimit : ErrorKind::InvalidRequest,
                               "request exceeds JSON limits or encoding"};
    }
    if (!pending.empty()) return invalid("function results required before dispatch");
    w.raw("]");
    request_controls::write_extensions(w, request.extensions);
    w.raw("}");
    if (!w.ok()) return Error{has_media ? ErrorKind::ResourceLimit : ErrorKind::InvalidRequest,
                             "request exceeds JSON limits or encoding"};
    return {};
  };
  json::BoundedWriter measured({resources.request_bytes, resources.json_depth});
  if (auto error = build(measured)) return *error;
  result.body.reserve(measured.size());
  json::BoundedWriter writer({measured.size(), resources.json_depth}, &result.body);
  if (auto error = build(writer)) return *error;
  result.context = std::move(context);
  result.max_output_tokens = effective.max_output_tokens;
  return result;
}
PortableProjection project_portable(const Message& captured) {
  PortableProjection out;
  out.message.id = captured.id;
  out.message.role = captured.role;
  for (const auto& part : captured.parts) {
    if (const auto* text = std::get_if<Text>(&part); text && !text->value.empty()) { out.message.parts.push_back(Text{text->value}); continue; }
    if (const auto* call = std::get_if<ToolCall>(&part); call && call->kind == ToolCallKind::ClientExecuted && call->input && call->input->root().is_object()) {
      ToolCall portable; portable.id = call->id; portable.name = call->name; portable.input = call->input;
      out.message.parts.push_back(std::move(portable));
      continue;
    }
    out.dropped.push_back(std::visit([](const auto& value) -> std::string {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, Text>) return "text";
      else if constexpr (std::is_same_v<T, ToolCall> || std::is_same_v<T, ServerToolResult> || std::is_same_v<T, Opaque> || std::is_same_v<T, InvalidToolCall>) return value.wire_type;
      else if constexpr (std::is_same_v<T, Thinking>) return "thought";
      else if constexpr (std::is_same_v<T, Media>) return "inlineData";
      else return "part";
    }, part));
  }
  return out;
}
} // namespace sp::gemini
