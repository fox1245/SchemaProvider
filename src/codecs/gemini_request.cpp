#include "codecs/gemini_request.h"
#include "codecs/media_input.h"
#include "core/native.h"
#include "json/json.h"
#include "descriptor/policy.h"
#include <map>
#include <set>
#include <charconv>
#include <initializer_list>

namespace sp::gemini {
namespace {
bool name_valid(std::string_view name) {
  if (name.empty() || name.size() > 128) return false;
  for (const auto c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
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
  if (descriptor.path(false) != base + ":generateContent" || descriptor.path(true) != base + ":streamGenerateContent?alt=sse") return Error{ErrorKind::InvalidConfig, "Gemini descriptor paths must target the same literal model"};
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
    if (speech.language_code && !one_of(*speech.language_code, {"de-DE","en-AU","en-GB","en-IN","en-US","es-US","fr-FR","hi-IN","pt-BR","ar-XA","es-ES","fr-CA","id-ID","it-IT","ja-JP","tr-TR","vi-VN","bn-IN","gu-IN","kn-IN","ml-IN","mr-IN","ta-IN","te-IN","nl-NL","ko-KR","cmn-CN","pl-PL","ru-RU","th-TH"}))
      return bad("unsupported speech language code");
  }
  if (request.image_config) {
    const auto& image = *request.image_config;
    if (image.aspect_ratio && !one_of(*image.aspect_ratio, {"1:1","1:4","4:1","1:8","8:1","2:3","3:2","3:4","4:3","4:5","5:4","9:16","16:9","21:9"}))
      return bad("unsupported image aspect ratio");
    if (image.image_size && !one_of(*image.image_size, {"512","1K","2K","4K"})) return bad("unsupported image size");
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
      if (!tool->content_parts.empty() && !tool->content.empty())
        tool_error = Error{ErrorKind::InvalidRequest, "tool result content and content_parts are mutually exclusive"};
      else if (tool->content_parts.size() > resources.request_parts)
        tool_error = Error{ErrorKind::InvalidRequest, "tool result part count limit exceeded"};
      else for (const auto& nested : tool->content_parts) if (const auto* media = std::get_if<Media>(&nested)) {
        has_media = true;
        if (static_cast<unsigned>(media->kind) < media_kind_count && static_cast<unsigned>(media->source) <= 2 &&
            (media->source != MediaSource::Inline ||
             (media->kind == MediaKind::Document && !mime_equal(media->mime, "application/pdf"))))
          tool_error = Error{ErrorKind::Unsupported, "function response media requires inline nontext bytes"};
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
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, effective, streaming));
  if (!context->history_valid_) return replay_bad();
  if (!context->valid_) return Error{ErrorKind::ResourceLimit, "native binding could not be captured"};
  if (tool_error) return *tool_error;
  EncodedRequest result{"POST", std::string(descriptor.path(streaming)), {}, {}, {}};
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
      w.raw("}");
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
    if (!request.tools.empty()) {
      w.raw(",\"tools\":[{\"functionDeclarations\":[");
      bool comma = false;
      for (const auto& tool : request.tools) {
        if (!name_valid(tool.name) || !tool_names.insert(tool.name).second || !tool.parameters || !tool.parameters->root().is_object()) return invalid("client functions require unique names and object schemas");
        if (comma) w.raw(",");
        comma = true;
        w.raw("{\"name\":").quoted(tool.name).raw(",\"description\":").quoted(tool.description).raw(",\"parametersJsonSchema\":").value(tool.parameters->root(), 4).raw("}");
      }
      w.raw("]}]");
    }
    if (request.required_tool) {
      if (!tool_names.contains(*request.required_tool)) return invalid("required function must be declared");
      w.raw(",\"toolConfig\":{\"functionCallingConfig\":{\"mode\":\"ANY\",\"allowedFunctionNames\":[").quoted(*request.required_tool).raw("]}}");
    }
    if (request.tool_choice) {
      const auto& choice = *request.tool_choice;
      if (tool_names.empty() && choice.mode != ToolChoiceMode::None) return invalid("function choice requires declared functions");
      std::set<std::string_view> allowed;
      w.raw(",\"toolConfig\":{\"functionCallingConfig\":{\"mode\":").quoted(tool_choice_name(choice.mode));
      if (!choice.allowed_function_names.empty()) {
        w.raw(",\"allowedFunctionNames\":[");
        bool comma = false;
        for (const auto& name : choice.allowed_function_names) {
          if (!name_valid(name) || !tool_names.contains(name) || !allowed.insert(name).second)
            return invalid("allowed functions require unique declared names");
          if (comma) w.raw(",");
          comma = true;
          w.quoted(name);
        }
        w.raw("]");
      }
      w.raw("}}");
    }
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
          for (const auto& part : m.parts) if (auto call = std::get_if<ToolCall>(&part)) {
            if (call->kind != ToolCallKind::ClientExecuted || !tool_names.contains(call->name) || !all_calls.insert(call->id).second || !call->wire_metadata) return invalid("function ownership mismatch");
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
            if (tr->content_parts.empty()) {
              // Legacy text content must be one JSON object, exactly as before typed parts existed.
              auto parsed = json::parse(tr->content, {resources.request_bytes, resources.json_depth});
              auto doc = std::get_if<json::Document>(&parsed);
              if (!doc || !doc->root().is_object()) return invalid("function result must be JSON object");
              if (tr->is_error) w.raw("{\"error\":").value(doc->root(), 6).raw("}");
              else w.value(doc->root(), 5);
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
    w.raw("]}");
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
} // namespace sp::gemini
