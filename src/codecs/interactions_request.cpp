#include "codecs/interactions_request.h"
#include "codecs/media_input.h"
#include "core/native.h"
#include "json/json.h"
#include "descriptor/policy.h"
#include <limits>
#include <map>
#include <set>
#include <type_traits>
#include <initializer_list>

namespace sp::interactions {
namespace {
bool function_name(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  for (char c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}
std::string lower(std::string_view text) {
  std::string value(text);
  for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return value;
}
bool one_of(std::string_view value, std::initializer_list<std::string_view> values) {
  for (auto item : values) if (item == value) return true;
  return false;
}
bool duration_valid(std::string_view value) {
  if (value.empty() || value.back() != 's') return false;
  value.remove_suffix(1);
  if (!value.empty() && value.front() == '-') value.remove_prefix(1);
  const auto dot = value.find('.');
  const auto whole = value.substr(0, dot);
  if (whole.empty() || whole.find_first_not_of("0123456789") != std::string_view::npos) return false;
  if (dot == std::string_view::npos) return true;
  const auto fraction = value.substr(dot + 1);
  return !fraction.empty() && fraction.size() <= 9 && fraction.find_first_not_of("0123456789") == std::string_view::npos;
}
void write_strings(json::BoundedWriter& body, const std::vector<std::string>& values) {
  body.raw("["); bool comma = false;
  for (const auto& value : values) { if (comma) body.raw(","); comma = true; body.quoted(value); }
  body.raw("]");
}
void write_delivery(json::BoundedWriter& body, std::optional<Delivery> delivery) {
  if (delivery) body.raw(",\"delivery\":").quoted(*delivery == Delivery::Inline ? "inline" : "uri");
}
void write_format(json::BoundedWriter& body, const ResponseFormat& format, size_t depth) {
  std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, AudioResponseFormat>) {
      body.raw("{\"type\":\"audio\"");
      if (value.mime_type) body.raw(",\"mime_type\":").quoted(*value.mime_type);
      if (value.sample_rate) body.raw(",\"sample_rate\":").raw(std::to_string(*value.sample_rate));
      if (value.bit_rate) body.raw(",\"bit_rate\":").raw(std::to_string(*value.bit_rate));
      write_delivery(body, value.delivery);
    } else if constexpr (std::is_same_v<T, ImageResponseFormat>) {
      body.raw("{\"type\":\"image\"");
      if (value.mime_type) body.raw(",\"mime_type\":").quoted(*value.mime_type);
      if (value.aspect_ratio) body.raw(",\"aspect_ratio\":").quoted(*value.aspect_ratio);
      if (value.image_size) body.raw(",\"image_size\":").quoted(*value.image_size);
      write_delivery(body, value.delivery);
    } else if constexpr (std::is_same_v<T, VideoResponseFormat>) {
      body.raw("{\"type\":\"video\"");
      if (value.aspect_ratio) body.raw(",\"aspect_ratio\":").quoted(*value.aspect_ratio);
      if (value.resolution) body.raw(",\"resolution\":").quoted(*value.resolution);
      if (value.duration) body.raw(",\"duration\":").quoted(*value.duration);
      if (value.gcs_uri) body.raw(",\"gcs_uri\":").quoted(*value.gcs_uri);
      write_delivery(body, value.delivery);
    } else {
      body.raw("{\"type\":\"text\"");
      if (value.mime_type) body.raw(",\"mime_type\":").quoted(*value.mime_type);
      if (value.schema) body.raw(",\"schema\":").value(value.schema->root(), depth + 1);
    }
    body.raw("}");
  }, format);
}
void write_speakers(json::BoundedWriter& body, const std::vector<SpeechConfig>& speakers) {
  body.raw("["); bool comma = false;
  for (const auto& speaker : speakers) {
    if (comma) body.raw(",");
    comma = true;
    body.raw("{"); bool field = false;
    if (speaker.voice) { body.raw("\"voice\":").quoted(*speaker.voice); field = true; }
    if (speaker.speaker) { if (field) body.raw(","); body.raw("\"speaker\":").quoted(*speaker.speaker); field = true; }
    if (speaker.language) { if (field) body.raw(","); body.raw("\"language\":").quoted(*speaker.language); }
    body.raw("}");
  }
  body.raw("]");
}
// Interactions content objects: image, audio, video and document, each carrying inline `data` or a
// `uri`. Admission has already established that the family accepts the kind and source form.
void write_media(json::BoundedWriter& body, const Media& media) {
  body.raw("{\"type\":").quoted(media_kind_name(media.kind));
  if (!media.mime.empty()) body.raw(",\"mime_type\":").quoted(lower(mime_essence(media.mime)));
  if (media.source == MediaSource::Inline) body.raw(",\"data\":\"").raw(*media.data).raw("\"");
  else body.raw(",\"uri\":").quoted(media.reference);
  if (!media.name.empty()) body.raw(",\"name\":").quoted(media.name);
  body.raw("}");
}
}
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](std::string label) -> EncodeResult { return Error{ErrorKind::InvalidRequest, std::move(label)}; };
  auto replay_bad = []() -> EncodeResult { return Error{ErrorKind::ReplayIneligible, "native replay provenance, binding or content mismatch"}; };
  if (descriptor.family() != "google.interactions" || descriptor.path(false) != "/v1beta/interactions" ||
      descriptor.path(true) != "/v1beta/interactions") return Error{ErrorKind::InvalidConfig, "Interactions requires the model interaction endpoint"};
  const auto& resources = descriptor.policy()->resources();
  auto effective = descriptor::effective_defaults(descriptor, request.model);
  if (request.max_output_tokens) effective.max_output_tokens = request.max_output_tokens;
  if (request.thinking_level) effective.thinking_level = *request.thinking_level;
  if (request.thinking_summaries) effective.thinking_summaries = request.thinking_summaries;
  if (request.service_tier) effective.service_tier = *request.service_tier;
  if (auto error = descriptor::validate_choices(descriptor, request.model, effective)) return bad(std::move(*error));
  if (request.model.empty() || request.messages.empty() || request.messages.size() > resources.request_messages || request.tools.size() > resources.request_tools)
    return bad("model and bounded messages/tools required");
  if (request.response_format.size() > resources.request_parts) return bad("response format count limit exceeded");
  for (const auto& format : request.response_format) {
    const char* error = nullptr;
    std::visit([&](const auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (!std::is_same_v<T, TextResponseFormat>)
        if (value.delivery && *value.delivery != Delivery::Inline && *value.delivery != Delivery::Uri) error = "invalid delivery mode";
      if constexpr (std::is_same_v<T, AudioResponseFormat>) {
        if (value.mime_type && !one_of(*value.mime_type, {"audio/mp3","audio/ogg_opus","audio/l16","audio/wav","audio/alaw","audio/mulaw"})) error = "unsupported audio output MIME type";
        if (value.bit_rate && value.mime_type && !one_of(*value.mime_type, {"audio/mp3","audio/ogg_opus"})) error = "audio bit rate only applies to MP3 or Opus";
      } else if constexpr (std::is_same_v<T, ImageResponseFormat>) {
        if (value.mime_type && *value.mime_type != "image/jpeg") error = "unsupported image output MIME type";
        if (value.aspect_ratio && !one_of(*value.aspect_ratio, {"1:1","1:4","4:1","1:8","8:1","2:3","3:2","3:4","4:3","4:5","5:4","9:16","16:9","21:9"})) error = "unsupported image aspect ratio";
        if (value.image_size && !one_of(*value.image_size, {"512","1K","2K","4K"})) error = "unsupported image size";
      } else if constexpr (std::is_same_v<T, VideoResponseFormat>) {
        if (value.aspect_ratio && !one_of(*value.aspect_ratio, {"16:9","9:16"})) error = "unsupported video aspect ratio";
        if (value.resolution && !one_of(*value.resolution, {"360p","720p","1080p","4k"})) error = "unsupported video resolution";
        if (value.duration && !duration_valid(*value.duration)) error = "video duration requires protobuf Duration syntax";
      } else {
        if (value.mime_type && !one_of(*value.mime_type, {"application/json","text/plain"})) error = "unsupported text output MIME type";
        if (value.schema && (!value.mime_type || *value.mime_type != "application/json" || !value.schema->root().is_object())) error = "text schema requires application/json and an object schema";
      }
    }, format);
    if (error) return bad(error);
  }
  if (request.speech_config) {
    const auto count = std::visit([](const auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, SpeakerConfig>) return value.speakers.size();
      else return value.size();
    }, *request.speech_config);
    if (count > resources.request_parts) return bad("speech configuration count limit exceeded");
  }
  if (request.transcription_config) {
    const auto& transcription = *request.transcription_config;
    if (transcription.language_codes.size() > resources.request_parts || transcription.custom_vocabulary.size() > 1000 ||
        transcription.custom_vocabulary.size() > resources.request_parts) return bad("transcription list count limit exceeded");
    if (transcription.mode && *transcription.mode != TranscriptionMode::Verbatim && *transcription.mode != TranscriptionMode::Smart)
      return bad("invalid transcription mode");
    if ((transcription.speaker_diarization || transcription.word_timestamps) &&
        (transcription.mode == TranscriptionMode::Smart || !transcription.custom_vocabulary.empty()))
      return bad("timestamps and diarization cannot combine with smart mode or custom vocabulary");
  }
  bool captured_history = false;
  std::optional<Error> tool_error;
  for (const auto& message : request.messages) {
    if (message.parts.size() > resources.request_parts) return bad("request part count limit exceeded");
    captured_history = captured_history || static_cast<bool>(message.native);
    for (const auto& part : message.parts) if (const auto* tool = std::get_if<ToolResult>(&part); tool && !tool_error) {
      if (!tool->content_parts.empty() && !tool->content.empty())
        tool_error = Error{ErrorKind::InvalidRequest, "tool result content and content_parts are mutually exclusive"};
      else if (tool->content_parts.size() > resources.request_parts)
        tool_error = Error{ErrorKind::InvalidRequest, "tool result part count limit exceeded"};
      else for (const auto& nested : tool->content_parts) if (const auto* media = std::get_if<Media>(&nested)) {
        if (static_cast<unsigned>(media->kind) < media_kind_count && static_cast<unsigned>(media->source) <= 2 &&
            media->kind != MediaKind::Image)
          tool_error = Error{ErrorKind::Unsupported, "Interactions function result supports image and text subcontent only"};
        else tool_error = media_input::admit(descriptor, *media);
        if (!tool_error && (media->detail != ImageDetail::Auto ||
            !media->name.empty() || !media->id.empty() || !media->transcript.empty()))
          tool_error = Error{ErrorKind::Unsupported, "Interactions function result image has unsupported controls or metadata"};
        if (tool_error) break;
      }
    }
    if (message.native) {
      if (message.role != Role::Assistant || !message.wire_output || !message.wire_output->root().is_array()) return replay_bad();
    } else {
      if (message.role == Role::Assistant || message.wire_output) return replay_bad();
      for (const auto& part : message.parts) {
        if (const auto* media = std::get_if<Media>(&part)) {
          if (message.role != Role::User) return bad("media inputs require user role");
          if (auto error = media_input::admit(descriptor, *media)) return *error;
          if (!media->name.empty() && media->kind != MediaKind::Video)
            return Error{ErrorKind::Unsupported, "Interactions only supports a media name for video content"};
        } else if (!std::holds_alternative<Text>(part) && !std::holds_alternative<ToolResult>(part)) return replay_bad();
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
    auto key = lower(header.first);
    if (key != "accept" && key != "content-type") result.headers.push_back(header);
  }
  result.headers.emplace_back("Content-Type", "application/json");
  result.headers.emplace_back("Accept", streaming ? "text/event-stream" : "application/json");
  // Declaration ownership is independent of the sizing and emission passes.
  std::set<std::string_view> names;
  for (const auto& tool : request.tools) {
    if (!function_name(tool.name) || !names.insert(tool.name).second || !tool.parameters ||
        !tool.parameters->root().is_object()) return bad("unique valid functions and object parameters required");
  }
  auto build = [&](json::BoundedWriter& body) -> std::optional<Error> {
    auto invalid = [](std::string label) { return Error{ErrorKind::InvalidRequest, std::move(label)}; };
    body.raw("{").quoted(descriptor.request_model_member()).raw(":").quoted(request.model)
        .raw(",\"store\":false,").quoted(descriptor.request_stream_member()).raw(":").raw(streaming ? "true" : "false");
    if (effective.service_tier) body.raw(",\"service_tier\":").quoted(*effective.service_tier);
    if (!request.system.empty()) body.raw(",\"system_instruction\":").quoted(request.system);
    if (!request.response_format.empty()) {
      body.raw(",\"response_format\":");
      if (request.response_format.size() == 1) write_format(body, request.response_format.front(), 1);
      else {
        body.raw("["); bool comma = false;
        for (const auto& format : request.response_format) { if (comma) body.raw(","); comma = true; write_format(body, format, 2); }
        body.raw("]");
      }
    }
    if (!request.tools.empty()) {
      body.raw(",\"tools\":["); bool comma = false;
      for (const auto& tool : request.tools) {
        if (comma) body.raw(",");
        comma = true;
        body.raw("{\"type\":\"function\",\"name\":").quoted(tool.name).raw(",\"description\":").quoted(tool.description)
            .raw(",\"parameters\":").value(tool.parameters->root(), 3).raw("}");
      }
      body.raw("]");
    }
    body.raw(",\"generation_config\":{");
    bool generation_comma = false;
    if (effective.max_output_tokens) {
      body.quoted(descriptor.max_output_tokens_member()).raw(":").raw(std::to_string(*effective.max_output_tokens));
      generation_comma = true;
    }
    if (effective.thinking_summaries) {
      if (generation_comma) body.raw(",");
      body.raw("\"thinking_summaries\":").quoted(*effective.thinking_summaries ? "auto" : "none");
      generation_comma = true;
    }
    if (effective.thinking_level) {
      if (generation_comma) body.raw(",");
      body.raw("\"thinking_level\":").quoted(*effective.thinking_level);
      generation_comma = true;
    }
    if (request.speech_config) {
      if (generation_comma) body.raw(",");
      body.raw("\"speech_config\":");
      std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, SpeakerConfig>) {
          body.raw("{\"speakers\":"); write_speakers(body, value.speakers);
          if (value.conversational) body.raw(",\"mode\":\"conversational\"");
          body.raw("}");
        }
        else write_speakers(body, value);
      }, *request.speech_config);
      generation_comma = true;
    }
    if (request.transcription_config) {
      if (generation_comma) body.raw(",");
      const auto& transcription = *request.transcription_config;
      body.raw("\"transcription_config\":{\"language_codes\":"); write_strings(body, transcription.language_codes);
      if (!transcription.custom_vocabulary.empty()) { body.raw(",\"custom_vocabulary\":"); write_strings(body, transcription.custom_vocabulary); }
      if (transcription.mode || transcription.speaker_diarization || transcription.word_timestamps) {
        body.raw(",\"mode\":{\"type\":").quoted(transcription.mode == TranscriptionMode::Smart ? "smart" : "verbatim");
        if (transcription.speaker_diarization) body.raw(",\"diarization_mode\":\"speaker\"");
        if (transcription.word_timestamps) body.raw(",\"timestamp_granularities\":[\"word\"]");
        body.raw("}");
      }
      body.raw("}"); generation_comma = true;
    }
    if (request.required_tool) {
      if (!names.contains(*request.required_tool)) return invalid("required tool must name a declared function");
      if (generation_comma) body.raw(",");
      body.raw("\"tool_choice\":{\"allowed_tools\":{\"mode\":\"any\",\"tools\":[").quoted(*request.required_tool).raw("]}}");
    }
    body.raw("},").quoted(descriptor.request_messages_member()).raw(":["); bool comma = false;
    auto separator = [&] { if (comma) body.raw(","); comma = true; };
    std::map<std::string_view, std::string_view> pending;
    std::set<std::string_view> ids;
    for (const auto& message : request.messages) {
      if (message.native) {
        if (!pending.empty()) return invalid("client results must precede next assistant generation");
        std::map<std::string_view, const ToolCall*> calls;
        for (const auto& part : message.parts) {
          if (const auto* call = std::get_if<ToolCall>(&part)) {
            if (call->kind != ToolCallKind::ClientExecuted || call->wire_type != "function_call" ||
                call->id.empty() || !names.contains(call->name) || !call->input || !call->input->root().is_object() ||
                !ids.insert(call->id).second) return invalid("captured function lacks unique declared ownership");
            calls.emplace(call->id, call); pending.emplace(call->id, call->name);
          } else if (!std::holds_alternative<Text>(part) && !std::holds_alternative<Thought>(part) && !std::holds_alternative<Media>(part))
            return Error{ErrorKind::Unsupported, "unsupported captured interaction part"};
        }
        for (auto step : message.wire_output->root().elements()) {
          if (!step.is_object() || !step.get("type").is_string()) return invalid("captured step requires type");
          auto type = step.get("type").as_string();
          if (type == "function_call") {
            auto id = step.get("id"), name = step.get("name"), arguments = step.get("arguments");
            if (!id.is_string() || !name.is_string() || !arguments.is_object()) return invalid("captured function identity invalid");
            auto found = calls.find(id.as_string());
            if (found == calls.end() || found->second->name != name.as_string() ||
                !json::equal(arguments, found->second->input->root())) return invalid("captured function ownership mismatch");
            calls.erase(found);
          } else if (type != "thought" && type != "model_output") return Error{ErrorKind::Unsupported, "unsupported captured interaction step"};
          separator(); body.value(step, 2);
        }
        if (!calls.empty()) return invalid("captured function step missing");
      } else {
        if (message.parts.empty()) return invalid("input content required");
        if (std::holds_alternative<ToolResult>(message.parts.front())) {
          if (message.role != Role::Tool && message.role != Role::User) return invalid("tool results require user or tool role");
          for (const auto& part : message.parts) {
            auto tool = std::get_if<ToolResult>(&part);
            if (!tool) return invalid("tool results cannot mix with plain content");
            auto found = pending.find(tool->tool_use_id);
            if (found == pending.end()) return invalid("tool result requires exactly one pending client call");
            separator(); body.raw("{\"type\":\"function_result\",\"call_id\":").quoted(tool->tool_use_id)
                .raw(",\"name\":").quoted(found->second).raw(",\"result\":");
            if (tool->content_parts.empty()) body.quoted(tool->content);
            else {
              body.raw("["); bool nested_comma = false;
              for (const auto& nested : tool->content_parts) {
                if (nested_comma) body.raw(",");
                nested_comma = true;
                if (const auto* text = std::get_if<Text>(&nested))
                  body.raw("{\"type\":\"text\",\"text\":").quoted(text->value).raw("}");
                else write_media(body, std::get<Media>(nested));
                if (!body.ok()) return Error{ErrorKind::ResourceLimit, "request exceeds JSON limits"};
              }
              body.raw("]");
            }
            body.raw(",\"is_error\":").raw(tool->is_error ? "true}" : "false}");
            pending.erase(found);
          }
        } else {
          if (message.role != Role::User || !pending.empty()) return invalid("plain input requires user role and resolved calls");
          separator(); body.raw("{\"type\":\"user_input\",\"content\":["); bool content_comma = false;
          for (const auto& part : message.parts) {
            if (content_comma) body.raw(",");
            content_comma = true;
            if (auto text = std::get_if<Text>(&part)) body.raw("{\"type\":\"text\",\"text\":").quoted(text->value).raw("}");
            else if (auto media = std::get_if<Media>(&part)) write_media(body, *media);
            else return Error{ErrorKind::Unsupported, "unsupported input content"};
          }
          body.raw("]}");
        }
      }
      if (!body.ok()) return Error{ErrorKind::ResourceLimit, "request exceeds JSON limits"};
    }
    if (!pending.empty()) return invalid("all pending functions require host results");
    body.raw("]}");
    if (!body.ok()) return Error{ErrorKind::ResourceLimit, "request exceeds JSON limits"};
    return std::nullopt;
  };
  json::BoundedWriter measured({resources.request_bytes, resources.json_depth}); if (auto error = build(measured)) return *error;
  result.body.reserve(measured.size()); json::BoundedWriter body({measured.size(), resources.json_depth}, &result.body);
  if (auto error = build(body)) return *error;
  result.context = std::move(context);
  result.max_output_tokens = effective.max_output_tokens;
  return result;
}
} // namespace sp::interactions
