#include "codecs/interactions_request.h"
#include "codecs/reason.h"
#include "descriptor/policy_refusals.h"
#include "codecs/request_checks.h"
#include "core/request_controls.h"
#include "codecs/media_input.h"
#include "core/cache_marks.h"
#include "core/native.h"
#include "json/json.h"
#include "descriptor/policy.h"
#include <limits>
#include <map>
#include <algorithm>
#include <set>
#include <type_traits>
#include <initializer_list>

namespace sp::interactions {
std::string_view tool_type(const BuiltinTool& tool) noexcept {
  return std::visit([](const auto& value) -> std::string_view {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, GoogleSearchTool>) return "google_search";
    else if constexpr (std::is_same_v<T, UrlContextTool>) return "url_context";
    else if constexpr (std::is_same_v<T, CodeExecutionTool>) return "code_execution";
    else static_assert(sizeof(T) == 0, "a built-in tool alternative has no type here");
  }, tool);
}
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
std::string_view tool_choice_name(ToolChoiceMode mode) {
  switch (mode) {
    case ToolChoiceMode::Auto: return "auto";
    case ToolChoiceMode::Any: return "any";
    case ToolChoiceMode::None: return "none";
    case ToolChoiceMode::Validated: return "validated";
  }
  return {};
}
}
// The path of an operation on a stored interaction: the route's path with `suffix` after it, then the route's query.
std::string operation_path(std::string_view route, std::string_view suffix) {
  const auto query = route.find('?');
  std::string path(route.substr(0, query));
  path += suffix;
  if (query != std::string_view::npos) path += route.substr(query);
  return path;
}
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](const reason::Reason& label) -> EncodeResult { return reason::error(ErrorKind::InvalidRequest, label); };
  auto replay_bad = []() -> EncodeResult { return reason::error(ErrorKind::ReplayIneligible, reason::interactions_request::native_replay_provenance_binding_or); };
  // The paths are the route fact of the descriptor's origin, which the policy states (descriptor::route_admitted).
  if (descriptor.family() != "google.interactions" || !descriptor::route_admitted(descriptor))
    return reason::error(ErrorKind::InvalidConfig, reason::interactions_request::interactions_requires_the_model);
  if (auto refusal = descriptor::route_refusal_reason(descriptor, request.model)) return bad(std::move(*refusal));
  if (auto refusal = descriptor::literal_route_refusal_reason(descriptor, request.model)) return reason::error(ErrorKind::InvalidConfig, *refusal);
  const auto& resources = descriptor.policy()->resources();
  auto effective = descriptor::effective_defaults(descriptor, request.model);
  if (request.max_output_tokens) effective.max_output_tokens = request.max_output_tokens;
  if (request.thinking_level) effective.thinking_level = *request.thinking_level;
  if (request.thinking_summaries) effective.thinking_summaries = request.thinking_summaries;
  if (request.service_tier) effective.service_tier = *request.service_tier;
  if (auto error = descriptor::choices_refusal(descriptor, request.model, effective)) return bad(std::move(*error));
  const auto name = [](std::string_view value, std::string_view more) {
    return !value.empty() && value.size() <= 256 && std::all_of(value.begin(), value.end(), [&](char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
             more.find(c) != std::string_view::npos;
    });
  };
  if (request.model.empty() == !request.agent.has_value()) return bad(reason::interactions_request::model_or_agent_required);
  if (request.agent && !name(*request.agent, {})) return bad(reason::interactions_request::invalid_agent);
  if (request.lifecycle != Lifecycle::Create) {
    // An operation on a stored interaction: no input, no stream, the id in the path.
    // The id is one path segment: never `.` or `..`, and no `..` inside it.
    if (!name(request.interaction_id, "=") || request.interaction_id == "." || request.interaction_id.find("..") != std::string::npos)
      return bad(reason::interactions_request::invalid_interaction_id);
    if (!request.messages.empty() || streaming) return bad(reason::interactions_request::lifecycle_request_invalid);
    if (request.extensions) return bad(reason::interactions_request::an_interaction_lifecycle_request_carries_no);
    auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, effective, false));
    if (!context->valid_) return reason::error(ErrorKind::ResourceLimit, reason::interactions_request::native_binding_could_not_be_captured);
    const auto cancel = request.lifecycle == Lifecycle::Cancel;
    EncodedRequest result{request.lifecycle == Lifecycle::Get ? "GET" : cancel ? "POST" : "DELETE",
                          operation_path(descriptor.resolved_path(false, request.model), "/" + request.interaction_id + (cancel ? "/cancel" : "")), {}, {}, {}};
    for (const auto& header : descriptor.headers()) {
      auto key = lower(header.first);
      if (key != "accept" && key != "content-type") result.headers.push_back(header);
    }
    if (cancel) { result.headers.emplace_back("Content-Type", "application/json"); result.body = "{}"; }
    result.headers.emplace_back("Accept", "application/json");
    result.context = std::move(context);
    return result;
  }
  if (request.background.value_or(false) && !request.store.value_or(false)) return bad(reason::interactions_request::background_needs_store);
  CacheMarkReport mark_report;
  if (auto error = cache_marks::drop(descriptor, request.model, request.messages, mark_report)) return std::move(*error);
  if (request.messages.empty() || request.messages.size() > resources.request_messages ||
      request.tools.size() + request.builtin_tools.size() > resources.request_tools)
    return bad(reason::interactions_request::model_and_bounded_messages_tools_required);
  // Values the service extends (tasks, search types) are tokens, not a fixed list; the service judges them.
  const auto token = [](std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
           std::all_of(value.begin(), value.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
  };
  if (request.video_task && !token(*request.video_task)) return bad(reason::interactions_request::video_task_must_be_a_lower_case_token);
  for (const auto& tool : request.builtin_tools)
    if (const auto* search = std::get_if<GoogleSearchTool>(&tool)) {
      if (search->search_types.size() > 8) return bad(reason::interactions_request::google_search_search_types_must_be_bounded);
      std::set<std::string_view> seen;
      for (const auto& type : search->search_types)
        if (!token(type) || !seen.insert(type).second) return bad(reason::interactions_request::google_search_search_types_must_be_bounded);
    }
  if (request.previous_interaction_id) {
    const auto& id = *request.previous_interaction_id;
    if (id.empty() || id.size() > 256 || std::any_of(id.begin(), id.end(), [](unsigned char c) { return c <= 0x20 || c >= 0x7f; }))
      return bad(reason::interactions_request::invalid_previous_interaction_id);
    for (const auto& message : request.messages)
      if (message.native || message.role == Role::Assistant)
        return bad(reason::interactions_request::previous_interaction_input_not_new);
  }
  if (request.response_format.size() > resources.request_parts) return bad(reason::interactions_request::response_format_count_limit_exceeded);
  for (const auto& format : request.response_format) {
    const reason::Reason* error = nullptr;
    std::visit([&](const auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (!std::is_same_v<T, TextResponseFormat>)
        if (value.delivery && *value.delivery != Delivery::Inline && *value.delivery != Delivery::Uri) error = &reason::interactions_request::invalid_delivery_mode;
      if constexpr (std::is_same_v<T, AudioResponseFormat>) {
        // The service names the formats; the SDK checks their shape and the media kind.
        if (value.mime_type && (!mime_syntax(*value.mime_type) || mime_kind(*value.mime_type) != MediaKind::Audio)) error = &reason::interactions_request::audio_output_mime_type_must_be_an_audio;
      } else if constexpr (std::is_same_v<T, ImageResponseFormat>) {
        if (value.mime_type && (!mime_syntax(*value.mime_type) || mime_kind(*value.mime_type) != MediaKind::Image)) error = &reason::interactions_request::image_output_mime_type_must_be_an_image;
        if (value.aspect_ratio && !request_controls::aspect_ratio_syntax(*value.aspect_ratio)) error = &reason::interactions_request::image_aspect_ratio_must_be_w_h;
        if (value.image_size && !request_controls::size_token(*value.image_size)) error = &reason::interactions_request::image_size_must_be_a_short_token;
      } else if constexpr (std::is_same_v<T, VideoResponseFormat>) {
        if (value.aspect_ratio && !request_controls::aspect_ratio_syntax(*value.aspect_ratio)) error = &reason::interactions_request::video_aspect_ratio_must_be_w_h;
        if (value.resolution && !request_controls::size_token(*value.resolution)) error = &reason::interactions_request::video_resolution_must_be_a_short_token;
        if (value.duration && !duration_valid(*value.duration)) error = &reason::interactions_request::video_duration_requires_protobuf_duration;
      } else {
        if (value.mime_type && !mime_syntax(*value.mime_type)) error = &reason::interactions_request::text_output_mime_type_must_be_a_mime_type;
        if (value.schema && (!value.mime_type || *value.mime_type != "application/json" || !value.schema->root().is_object())) error = &reason::interactions_request::text_schema_requires_application_json_and;
      }
    }, format);
    if (error) return bad(*error);
  }
  if (request.speech_config) {
    const auto count = std::visit([](const auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, SpeakerConfig>) return value.speakers.size();
      else return value.size();
    }, *request.speech_config);
    if (count > resources.request_parts) return bad(reason::interactions_request::speech_configuration_count_limit_exceeded);
  }
  if (request.transcription_config) {
    const auto& transcription = *request.transcription_config;
    if (transcription.language_codes.size() > resources.request_parts || descriptor::over_limit(descriptor, "transcription_vocabulary", transcription.custom_vocabulary.size()) ||
        transcription.custom_vocabulary.size() > resources.request_parts) return bad(reason::interactions_request::transcription_list_count_limit_exceeded);
    if (transcription.mode && *transcription.mode != TranscriptionMode::Verbatim && *transcription.mode != TranscriptionMode::Smart)
      return bad(reason::interactions_request::invalid_transcription_mode);
    if ((transcription.speaker_diarization || transcription.word_timestamps) &&
        (transcription.mode == TranscriptionMode::Smart || !transcription.custom_vocabulary.empty()))
      return bad(reason::interactions_request::timestamps_and_diarization_cannot_combine);
  }
  bool captured_history = false;
  std::optional<Error> tool_error;
  for (const auto& message : request.messages) {
    if (message.parts.size() > resources.request_parts) return bad(reason::interactions_request::request_part_count_limit_exceeded);
    captured_history = captured_history || static_cast<bool>(message.native);
    for (const auto& part : message.parts) if (const auto* tool = std::get_if<ToolResult>(&part); tool && !tool_error) {
      if (!tool->content_parts.empty() && !tool->content.empty())
        tool_error = reason::error(ErrorKind::InvalidRequest, reason::interactions_request::tool_result_content_and_content_parts_are);
      else if (tool->content_parts.size() > resources.request_parts)
        tool_error = reason::error(ErrorKind::InvalidRequest, reason::interactions_request::tool_result_part_count_limit_exceeded);
      else if (std::any_of(tool->content_parts.begin(), tool->content_parts.end(), [](const auto& nested) { return std::holds_alternative<Opaque>(nested); }))
        tool_error = reason::error(ErrorKind::Unsupported, reason::interactions_request::tool_result_blocks_not_admitted);
      else for (const auto& nested : tool->content_parts) if (const auto* media = std::get_if<Media>(&nested)) {
        if (static_cast<unsigned>(media->kind) < media_kind_count && static_cast<unsigned>(media->source) <= 2 &&
            media->kind != MediaKind::Image)
          tool_error = reason::error(ErrorKind::Unsupported, reason::interactions_request::interactions_function_result_supports);
        else tool_error = media_input::admit(descriptor, *media);
        if (!tool_error && (media->detail != ImageDetail::Auto ||
            !media->name.empty() || !media->id.empty() || !media->transcript.empty()))
          tool_error = reason::error(ErrorKind::Unsupported, reason::interactions_request::interactions_function_result_image_has);
        if (tool_error) break;
      }
    }
    if (message.native) {
      if (message.role != Role::Assistant || !message.wire_output || !message.wire_output->root().is_array()) return replay_bad();
    } else {
      if (message.role == Role::Assistant || message.wire_output) return replay_bad();
      for (const auto& part : message.parts) {
        if (const auto* media = std::get_if<Media>(&part)) {
          if (message.role != Role::User) return bad(reason::interactions_request::media_inputs_require_user_role);
          if (auto error = media_input::admit(descriptor, *media)) return *error;
          if (!media->name.empty() && media->kind != MediaKind::Video)
            return reason::error(ErrorKind::Unsupported, reason::interactions_request::interactions_only_supports_a_media_name);
        } else if (!std::holds_alternative<Text>(part) && !std::holds_alternative<ToolResult>(part)) return replay_bad();
      }
    }
  }
  if (tool_error && !captured_history) return *tool_error;
  if (auto refusal = request_controls::validate_extensions(descriptor, request.extensions))
    return reason::error(refusal->admitted ? ErrorKind::InvalidRequest : ErrorKind::Unsupported, refusal->cause);
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, effective, streaming));
  if (!context->history_valid_) return replay_bad();
  if (!context->valid_) return reason::error(ErrorKind::ResourceLimit, reason::interactions_request::native_binding_could_not_be_captured);
  if (tool_error) return *tool_error;
  EncodedRequest result{"POST", descriptor.resolved_path(streaming, request.model), {}, {}, {}};
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
        !tool.parameters->root().is_object()) return bad(reason::interactions_request::unique_valid_functions_and_object);
  }
  if (request.tool_choice) {
    // The names a choice allows are functions the request declares.
    const auto& choice = *request.tool_choice;
    if (request.required_tool) return bad(reason::interactions_request::tool_choice_and_required_tool_exclusive);
    if (tool_choice_name(choice.mode).empty()) return bad(reason::interactions_request::invalid_tool_choice_mode);
    if (choice.allowed) {
      if (choice.allowed->empty()) return bad(reason::interactions_request::allowed_tools_require_at_least_one);
      std::set<std::string_view> seen;
      for (const auto& allowed : *choice.allowed) {
        if (!names.contains(allowed)) return bad(reason::interactions_request::allowed_tools_must_be_declared);
        if (!seen.insert(allowed).second) return bad(reason::interactions_request::allowed_tools_must_be_unique);
      }
    }
  }
  // Over the size limit, a request with media is a resource limit; one of text alone is invalid.
  const bool has_media = media_input::carries_media(request.messages);
  const auto limit_error = [&] {
    return reason::error(has_media ? ErrorKind::ResourceLimit : ErrorKind::InvalidRequest, reason::interactions_request::request_exceeds_json_limits);
  };
  auto build = [&](json::BoundedWriter& body) -> std::optional<Error> {
    auto invalid = [](const reason::Reason& label) { return reason::error(ErrorKind::InvalidRequest, label); };
    if (request.agent) body.raw("{\"agent\":").quoted(*request.agent);
    else body.raw("{").quoted(descriptor.request_model_member()).raw(":").quoted(request.model);
    body.raw(",\"store\":").raw(request.store.value_or(false) ? "true" : "false").raw(",")
        .quoted(descriptor.request_stream_member()).raw(":").raw(streaming ? "true" : "false");
    if (request.previous_interaction_id) body.raw(",\"previous_interaction_id\":").quoted(*request.previous_interaction_id);
    if (request.background) body.raw(",\"background\":").raw(*request.background ? "true" : "false");
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
    if (!request.tools.empty() || !request.builtin_tools.empty()) {
      body.raw(",\"tools\":["); bool comma = false;
      for (const auto& tool : request.tools) {
        if (comma) body.raw(",");
        comma = true;
        body.raw("{\"type\":\"function\",\"name\":").quoted(tool.name).raw(",\"description\":").quoted(tool.description)
            .raw(",\"parameters\":").value(tool.parameters->root(), 3).raw("}");
      }
      for (const auto& tool : request.builtin_tools) {
        if (comma) body.raw(",");
        comma = true;
        std::visit([&](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, GoogleSearchTool>) {
            body.raw("{\"type\":\"google_search\"");
            if (!value.search_types.empty()) { body.raw(",\"search_types\":"); write_strings(body, value.search_types); }
            body.raw("}");
          } else if constexpr (std::is_same_v<T, UrlContextTool>) body.raw("{\"type\":\"url_context\"}");
          else body.raw("{\"type\":\"code_execution\"}");
        }, tool);
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
    if (request.video_task) {
      if (generation_comma) body.raw(",");
      body.raw("\"video_config\":{\"task\":").quoted(*request.video_task).raw("}");
      generation_comma = true;
    }
    if (request.required_tool) {
      if (!names.contains(*request.required_tool)) return invalid(reason::interactions_request::required_tool_must_name_a_declared);
      if (generation_comma) body.raw(",");
      body.raw("\"tool_choice\":{\"allowed_tools\":{\"mode\":\"any\",\"tools\":[").quoted(*request.required_tool).raw("]}}");
    } else if (request.tool_choice) {
      const auto& choice = *request.tool_choice;
      if (generation_comma) body.raw(",");
      body.raw("\"tool_choice\":");
      if (!choice.allowed) body.quoted(tool_choice_name(choice.mode));
      else {
        body.raw("{\"allowed_tools\":{\"mode\":").quoted(tool_choice_name(choice.mode)).raw(",\"tools\":");
        write_strings(body, *choice.allowed);
        body.raw("}}");
      }
    }
    body.raw("},").quoted(descriptor.request_messages_member()).raw(":["); bool comma = false;
    auto separator = [&] { if (comma) body.raw(","); comma = true; };
    std::map<std::string_view, std::string_view> pending;
    std::set<std::string_view> ids;
    for (const auto& message : request.messages) {
      if (message.native) {
        if (!pending.empty()) return invalid(reason::interactions_request::client_results_must_precede_next);
        std::map<std::string_view, const ToolCall*> calls;
        for (const auto& part : message.parts) {
          if (const auto* call = std::get_if<ToolCall>(&part); call && call->kind == ToolCallKind::ServerExecuted) continue;
          if (std::holds_alternative<ServerToolResult>(part)) continue;
          if (const auto* call = std::get_if<ToolCall>(&part)) {
            if (call->kind != ToolCallKind::ClientExecuted || call->wire_type != "function_call" ||
                call->id.empty() || !names.contains(call->name) || !call->input || !call->input->root().is_object() ||
                !ids.insert(call->id).second) return invalid(reason::interactions_request::captured_function_lacks_unique_declared);
            calls.emplace(call->id, call); pending.emplace(call->id, call->name);
          } else if (!std::holds_alternative<Text>(part) && !std::holds_alternative<Thought>(part) && !std::holds_alternative<Media>(part))
            return reason::error(ErrorKind::Unsupported, reason::interactions_request::unsupported_captured_interaction_part);
        }
        for (auto step : message.wire_output->root().elements()) {
          if (!step.is_object() || !step.get("type").is_string()) return invalid(reason::interactions_request::captured_step_requires_type);
          auto type = step.get("type").as_string();
          if (type == "function_call") {
            auto id = step.get("id"), name = step.get("name"), arguments = step.get("arguments");
            if (!id.is_string() || !name.is_string() || !arguments.is_object()) return invalid(reason::interactions_request::captured_function_identity_invalid);
            auto found = calls.find(id.as_string());
            if (found == calls.end() || found->second->name != name.as_string() ||
                !json::equal(arguments, found->second->input->root())) return invalid(reason::interactions_request::captured_function_ownership_mismatch);
            calls.erase(found);
          } else if (type != "thought" && type != "model_output" &&
                     !(type != "function_result" && (type.ends_with("_call") || type.ends_with("_result"))))
            return reason::error(ErrorKind::Unsupported, reason::interactions_request::unsupported_captured_interaction_step);
          separator(); body.value(step, 2);
        }
        if (!calls.empty()) return invalid(reason::interactions_request::captured_function_step_missing);
      } else {
        if (message.parts.empty()) return invalid(reason::interactions_request::input_content_required);
        if (std::holds_alternative<ToolResult>(message.parts.front())) {
          if (message.role != Role::Tool && message.role != Role::User) return invalid(reason::interactions_request::tool_results_require_user_or_tool_role);
          for (const auto& part : message.parts) {
            auto tool = std::get_if<ToolResult>(&part);
            if (!tool) return invalid(reason::interactions_request::tool_results_cannot_mix_with_plain);
            auto found = pending.find(tool->tool_use_id);
            if (found == pending.end()) return invalid(reason::interactions_request::tool_result_requires_exactly_one_pending);
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
                if (!body.ok()) return limit_error();
              }
              body.raw("]");
            }
            body.raw(",\"is_error\":").raw(tool->is_error ? "true}" : "false}");
            pending.erase(found);
          }
        } else {
          if (message.role != Role::User || !pending.empty()) return invalid(reason::interactions_request::plain_input_requires_user_role_and);
          separator(); body.raw("{\"type\":\"user_input\",\"content\":["); bool content_comma = false;
          for (const auto& part : message.parts) {
            if (content_comma) body.raw(",");
            content_comma = true;
            if (auto text = std::get_if<Text>(&part)) body.raw("{\"type\":\"text\",\"text\":").quoted(text->value).raw("}");
            else if (auto media = std::get_if<Media>(&part)) write_media(body, *media);
            else return reason::error(ErrorKind::Unsupported, reason::interactions_request::unsupported_input_content);
          }
          body.raw("]}");
        }
      }
      if (!body.ok()) return limit_error();
    }
    if (!pending.empty()) return invalid(reason::interactions_request::all_pending_functions_require_host);
    body.raw("]");
    request_controls::write_extensions(body, request.extensions);
    body.raw("}");
    if (!body.ok()) return limit_error();
    return std::nullopt;
  };
  json::BoundedWriter measured({resources.request_bytes, resources.json_depth}); if (auto error = build(measured)) return *error;
  result.body.reserve(measured.size()); json::BoundedWriter body({measured.size(), resources.json_depth}, &result.body);
  if (auto error = build(body)) return *error;
  result.context = std::move(context);
  result.max_output_tokens = effective.max_output_tokens;
  result.cache_marks = mark_report;
  return result;
}
} // namespace sp::interactions
