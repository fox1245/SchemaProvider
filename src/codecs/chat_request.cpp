#include "codecs/chat.h"
#include "codecs/reason.h"
#include "descriptor/policy_refusals.h"
#include "codecs/request_checks.h"
#include "core/cache_marks.h"
#include "core/native.h"
#include "codecs/media_input.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <limits>

namespace sp::chat {
namespace {
// Chat Completions content parts: image_url, input_audio, video_url and file. Admission has
// already established that the family and origin accept this kind and source form.
void write_media(json::BoundedWriter& body, const Media& media) {
  switch (media.kind) {
    case MediaKind::Image:
      body.raw("{\"type\":\"image_url\",\"image_url\":{\"url\":");
      media_input::uri_or_url(body, media);
      body.raw(",\"detail\":").quoted(image_detail_name(media.detail)).raw("}}");
      break;
    case MediaKind::Audio:
      body.raw("{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"").raw(*media.data)
          .raw("\",\"format\":").quoted(*media_input::audio_format(media.mime)).raw("}}");
      break;
    case MediaKind::Video:
      body.raw("{\"type\":\"video_url\",\"video_url\":{\"url\":");
      media_input::uri_or_url(body, media);
      body.raw("}}");
      break;
    case MediaKind::Document:
      body.raw("{\"type\":\"file\",\"file\":{");
      if (media.source == MediaSource::File) body.raw("\"file_id\":").quoted(media.reference);
      else {
        body.raw("\"filename\":").quoted(media_input::document_filename(media)).raw(",\"file_data\":");
        media_input::uri_or_url(body, media);
      }
      body.raw("}}");
      break;
  }
}
std::optional<Error> admit(const descriptor::ValidatedDescriptor& descriptor, const Media& media) {
  if (auto error = media_input::admit(descriptor, media)) return error;
  if (media.kind == MediaKind::Audio && !media_input::audio_format(media.mime))
    return reason::error(ErrorKind::Unsupported, reason::chat_request::chat_input_audio_has_no_format_name_for_this_mime);
  return std::nullopt;
}
std::string_view modality_name(OutputModality modality) noexcept {
  switch (modality) {
    case OutputModality::Text: return "text";
    case OutputModality::Audio: return "audio";
    case OutputModality::Image: return "image";
  }
  return {};
}
// The service names the formats (wav, mp3, pcm16, ...); the SDK checks the shape.
bool audio_format_name(std::string_view format) noexcept {
  return !format.empty() && format.size() <= 16 && std::all_of(format.begin(), format.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}
bool config_token(std::string_view text, std::size_t maximum = 64) noexcept {
  if (text.empty() || text.size() > maximum) return false;
  for (const unsigned char c : text)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '.' ||
          c == '_' || c == '-' || c == '/')) return false;
  return true;
}
std::string_view tool_choice_name(ToolChoiceMode mode) noexcept {
  switch (mode) {
    case ToolChoiceMode::Auto: return "auto";
    case ToolChoiceMode::Required: return "required";
    case ToolChoiceMode::None: return "none";
    default: return {};
  }
}
// Everything a tool choice names is a function the request declares.
std::optional<reason::Reason> validate_tool_choice(const Request& request) {
  const auto& choice = *request.tool_choice;
  std::set<std::string_view> functions;
  for (const auto& tool : request.tools) functions.insert(tool.name);
  switch (choice.mode) {
    case ToolChoiceMode::Auto: case ToolChoiceMode::Required: case ToolChoiceMode::None:
      if (!choice.name.empty()) return reason::chat_request::only_a_function_tool_choice_takes_a_name;
      break;
    case ToolChoiceMode::Function:
      if (!functions.contains(choice.name)) return reason::chat_request::function_tool_choice_must_be_declared;
      break;
    default: return reason::chat_request::invalid_tool_choice_mode;
  }
  if (!choice.allowed) return std::nullopt;
  if (choice.mode != ToolChoiceMode::Auto && choice.mode != ToolChoiceMode::Required)
    return reason::chat_request::allowed_functions_require_auto_or_required;
  if (choice.allowed->empty()) return reason::chat_request::allowed_functions_require_at_least_one;
  std::set<std::string_view> seen;
  for (const auto& name : *choice.allowed) {
    if (!functions.contains(name)) return reason::chat_request::allowed_functions_must_be_declared;
    if (!seen.insert(name).second) return reason::chat_request::allowed_functions_must_be_unique;
  }
  return std::nullopt;
}
void write_tool_choice(json::BoundedWriter& body, const ToolChoice& choice) {
  const auto function = [&](std::string_view name) {
    body.raw("{\"type\":\"function\",\"function\":{\"name\":").quoted(name).raw("}}");
  };
  if (choice.allowed) {
    body.raw("{\"type\":\"allowed_tools\",\"allowed_tools\":{\"mode\":").quoted(tool_choice_name(choice.mode)).raw(",\"tools\":[");
    for (std::size_t i = 0; i < choice.allowed->size(); ++i) {
      if (i) body.raw(",");
      function((*choice.allowed)[i]);
    }
    body.raw("]}}");
  } else if (choice.mode == ToolChoiceMode::Function) function(choice.name);
  else body.quoted(tool_choice_name(choice.mode));
}
// Output media controls: modalities, the OpenAI audio parameters and the OpenRouter image_config.
std::optional<reason::Reason> validate_output(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  unsigned mask = 0;
  for (const auto modality : request.modalities) {
    if (static_cast<unsigned>(modality) > static_cast<unsigned>(OutputModality::Image) || (mask & (1U << static_cast<unsigned>(modality))))
      return reason::chat_request::output_modalities_require_unique_valid_values;
    mask |= 1U << static_cast<unsigned>(modality);
  }
  const bool audio = mask & (1U << static_cast<unsigned>(OutputModality::Audio));
  const bool image = mask & (1U << static_cast<unsigned>(OutputModality::Image));
  if (audio != request.audio.has_value()) return reason::chat_request::audio_output_needs_both_the_audio_modality_and;
  if (request.audio) {
    const auto& output = *request.audio;
    const bool preset = !output.voice.empty(), custom = !output.voice_id.empty();
    if (preset && custom) return reason::chat_request::audio_output_takes_a_voice_or_a_voice_id_not_both;
    if (!preset && !custom) return reason::chat_request::audio_output_needs_a_voice_or_a_voice_id;
    if (!config_token(preset ? output.voice : output.voice_id, 128) || !audio_format_name(output.format))
      return reason::chat_request::audio_output_needs_a_voice_and_a_supported_format;
  }
  if (request.image_config && !image) return reason::chat_request::image_config_needs_the_image_modality;
  if (image && !descriptor::contains(descriptor.family_policy().openrouter_origins, descriptor.base_url()))
    return reason::chat_request::image_output_requires_a_declared_openrouter;
  if (request.image_config) return request_controls::validate_image_config(*request.image_config);
  return std::nullopt;
}
} // namespace
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](const reason::Reason& message) { return reason::error(ErrorKind::InvalidRequest, message); };
  if (descriptor.family() != "openai.chat") return reason::error(ErrorKind::InvalidConfig, reason::chat_request::descriptor_family_does_not_match_chat_codec);
  if (request.model.empty() || (request.messages.empty() && request.canonical_messages.empty()))
    return bad(reason::chat_request::model_and_messages_are_required);
  if (!request.messages.empty() && !request.canonical_messages.empty())
    return bad(reason::chat_request::choose_one_message_representation);
  if (auto refusal = descriptor::route_refusal_reason(descriptor, request.model)) return bad(std::move(*refusal));
  if (auto refusal = descriptor::literal_route_refusal_reason(descriptor, request.model)) return reason::error(ErrorKind::InvalidConfig, *refusal);
  const auto& resources = descriptor.policy()->resources();
  auto effective = descriptor::effective_defaults(descriptor, request.model);
  if (request.max_output_tokens) effective.max_output_tokens = request.max_output_tokens;
  if (request.temperature) effective.temperature = request.temperature;
  if (request.top_p) effective.top_p = request.top_p;
  if (request.reasoning_effort) effective.reasoning_effort = *request.reasoning_effort;
  if (request.service_tier) effective.service_tier = *request.service_tier;
  if (auto error = descriptor::choices_refusal(descriptor, request.model, effective)) return bad(std::move(*error));
  if (request.reasoning || request.include_reasoning || request.usage_include || !request.models.empty() || !request.plugins.empty()) {
    if (!descriptor::contains(descriptor.family_policy().openrouter_origins, descriptor.base_url()))
      return bad(reason::chat_request::gateway_controls_require_declared_openrouter);
  }
  if (auto refusal = request_controls::validate_plugins(descriptor.family_policy(), request.plugins, resources.request_tools))
    return reason::error(refusal->admitted ? ErrorKind::InvalidRequest : ErrorKind::Unsupported, refusal->cause);
  if (request.reasoning) {
    const auto& reasoning = *request.reasoning;
    if (!reasoning.effort && !reasoning.max_tokens && !reasoning.exclude && !reasoning.enabled)
      return bad(reason::chat_request::reasoning_options_must_not_be_empty);
    if (reasoning.effort && !descriptor::contains(descriptor.family_policy().reasoning_efforts, *reasoning.effort))
      return bad(reason::chat_request::unsupported_reasoning_effort);
    if (reasoning.effort && reasoning.max_tokens)
      return bad(reason::chat_request::reasoning_effort_and_max_tokens_are_mutually);
    if (reasoning.max_tokens && (!*reasoning.max_tokens ||
        *reasoning.max_tokens > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        (effective.max_output_tokens && *reasoning.max_tokens > *effective.max_output_tokens)))
      return bad(reason::chat_request::reasoning_max_tokens_outside_output_budget);
    if (reasoning.enabled == false && (reasoning.effort || reasoning.max_tokens))
      return bad(reason::chat_request::disabled_reasoning_cannot_specify_effort_or);
    if (effective.reasoning_effort)
      return bad(reason::chat_request::scalar_and_nested_reasoning_controls_conflict);
    if (reasoning.exclude == true && request.include_reasoning == true)
      return bad(reason::chat_request::excluded_reasoning_cannot_be_included);
  }
  if (request.models.size() > resources.request_tools) return bad(reason::chat_request::alternative_model_count_limit_exceeded);
  std::set<std::string_view> alternative_models;
  for (const auto& model : request.models) {
    if (model.empty() || !alternative_models.insert(model).second)
      return bad(reason::chat_request::alternative_models_require_unique_nonempty_names);
    if (auto error = descriptor::choices_refusal(descriptor, model, effective)) return bad(std::move(*error));
  }
  if (request.provider)
    if (auto error = request_controls::validate_routing(descriptor, *request.provider))
      return bad(std::move(*error));
  if (request.response_format)
    if (auto error = request_controls::validate_response_format(*request.response_format, resources))
      return bad(std::move(*error));
  if (auto error = validate_output(descriptor, request)) return bad(std::move(*error));
  if (std::max(request.messages.size(), request.canonical_messages.size()) > resources.request_messages || request.tools.size() > resources.request_tools)
    return bad(reason::chat_request::request_count_limit_exceeded);
  if (request.tool_choice)
    if (auto error = validate_tool_choice(request)) return bad(std::move(*error));
  bool has_media = false;
  const bool router = descriptor::contains(descriptor.family_policy().openrouter_origins, descriptor.base_url());
  for (const auto& message : request.messages) {
    if (message.media.size() > resources.request_parts || message.tool_calls.size() > resources.request_parts)
      return bad(reason::chat_request::request_part_count_limit_exceeded);
    if (!message.media.empty() && message.role != Role::User && message.role != Role::Assistant)
      return bad(reason::chat_request::media_inputs_require_user_or_supported_assistant);
    for (const auto& media : message.media) {
      if (message.role == Role::Assistant && (!router || media.kind != MediaKind::Image))
        return reason::error(ErrorKind::Unsupported, reason::chat_request::assistant_media_inputs_require_openrouter_images);
      if (auto error = admit(descriptor, media)) return std::move(*error);
      has_media = true;
    }
  }
  bool captured_history = false;
  std::optional<Error> media_error;
  for (const auto& message : request.canonical_messages) {
    captured_history = captured_history || static_cast<bool>(message.native);
    for (const auto& part : message.parts) if (const auto* media = std::get_if<Media>(&part)) {
      if (message.role == Role::User) {
        if (!media_error) media_error = admit(descriptor, *media);
        has_media = true;
      } else if (message.role == Role::Assistant && media->kind == MediaKind::Image) {
        if (!media_error) media_error = router ? admit(descriptor, *media) :
            std::optional<Error>{reason::error(ErrorKind::Unsupported, reason::chat_request::assistant_images_require_declared_openrouter)};
        has_media = true;
      }
    }
  }
  // Reject ordinary malformed input before native capture; edited native prefixes
  // still retain replay-mismatch precedence.
  if (media_error && !captured_history) return *media_error;
  if (auto refusal = request_controls::validate_extensions(descriptor, request.extensions))
    return reason::error(refusal->admitted ? ErrorKind::InvalidRequest : ErrorKind::Unsupported, refusal->cause);
  // Cache marks (core/cache_marks.h) ride the text parts of the ordered conversation, where the policy says the origin
  // and the model's route take them; elsewhere they are dropped and reported, or refused, as the policy says.
  CacheMarkReport mark_report;
  bool write_marks = false;
  {
    const auto marks = cache_marks::tally(request.canonical_messages);
    auto admitted = cache_marks::admit(descriptor, request.model, marks.all);
    if (auto* refusal = std::get_if<Error>(&admitted)) return std::move(*refusal);
    const auto& admission = std::get<cache_marks::Admission>(admitted);
    mark_report.requested = marks.all;
    if (admission.disposition == cache_marks::Disposition::Emit) {
      if (marks.all != marks.text) return reason::error(ErrorKind::Unsupported, reason::chat_request::cache_marks_text_only);
      std::vector<std::optional<CacheTtl>> lifetimes;
      for (const auto& message : request.canonical_messages)
        for (const auto& part : message.parts)
          if (const auto* text = std::get_if<Text>(&part)) {
            if (auto error = cache_marks::marked_text(*text)) return std::move(*error);
            if (text->cache) lifetimes.push_back(text->cache->ttl);
          }
      if (auto error = cache_marks::check(admission.capabilities, lifetimes)) return std::move(*error);
      write_marks = true;
      mark_report.emitted = marks.all;
    }
  }
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, effective, streaming));
  if (!context->history_valid_)
    return reason::error(ErrorKind::ReplayIneligible, reason::chat_request::native_replay_provenance_binding_or_content);
  if (!context->valid_)
    return reason::error(ErrorKind::ResourceLimit, reason::chat_request::native_chat_binding_could_not_be_captured);
  if (media_error) return *media_error;
  for (const auto& message : request.canonical_messages) {
    if (message.wire_output || (message.native && message.role != Role::Assistant))
      return reason::error(ErrorKind::ReplayIneligible, reason::chat_request::invalid_native_chat_history_representation);
    if (message.parts.size() > resources.request_parts) return bad(reason::chat_request::request_part_count_limit_exceeded);
    bool has_thinking = false;
    bool has_details = false;
    bool has_frames = false;
    bool audio_reply = false;
    for (const auto& part : message.parts) {
      if (const auto* media = std::get_if<Media>(&part)) {
        // A generated audio reply is replayed by its provider-held id, never by its bytes.
        if (message.role == Role::Assistant && media->kind == MediaKind::Audio && !media->id.empty()) {
          if (audio_reply) return bad(reason::chat_request::one_audio_reply_per_assistant_message);
          audio_reply = true;
        } else if (message.role == Role::Assistant && media->kind == MediaKind::Image && router) {
          has_media = true;
        } else if (message.role != Role::User) {
          return reason::error(ErrorKind::Unsupported, reason::chat_request::chat_assistant_media_requires_an_audio_reply_id);
        } else {
          has_media = true;
        }
      } else if (const auto* thinking = std::get_if<Thinking>(&part)) {
        if (message.role != Role::Assistant || has_thinking || thinking->signature)
          return reason::error(ErrorKind::ReplayIneligible, reason::chat_request::chat_requires_one_plain_unsigned_assistant);
        has_thinking = true;
      } else if (const auto* opaque = std::get_if<Opaque>(&part)) {
        // History captured while a stream kept each reasoning frame as a part carries these parts and a `frames` member; it still replays.
        if (opaque->wire_type == "reasoning_details.frame") {
          if (!message.native || !opaque->wire_metadata)
            return reason::error(ErrorKind::ReplayIneligible, reason::chat_request::reasoning_frame_lacks_native_authority);
          const auto metadata = opaque->wire_metadata->root();
          if (!metadata.is_object() || metadata.size() != 2 ||
              metadata.get("type").as_string() != "reasoning_details.frame" ||
              !metadata.get("details").is_array())
            return reason::error(ErrorKind::ReplayIneligible, reason::chat_request::invalid_retained_reasoning_frame);
          has_frames = true;
          continue;
        }
        if (opaque->wire_type != "reasoning_details" || !message.native ||
            message.role != Role::Assistant || has_details || !opaque->wire_metadata ||
            !descriptor::contains(descriptor.family_policy().openrouter_origins, descriptor.base_url()))
          return reason::error(ErrorKind::ReplayIneligible, reason::chat_request::chat_reasoning_details_require_authenticated);
        const auto metadata = opaque->wire_metadata->root();
        if (!metadata.is_object() || (metadata.size() != 2 && metadata.size() != 3) ||
            metadata.get("type").as_string() != "reasoning_details" ||
            !metadata.get("details").is_array() ||
            (metadata.size() == 3 && !metadata.get("frames").is_array()))
          return reason::error(ErrorKind::ReplayIneligible, reason::chat_request::invalid_captured_reasoning_details_snapshot);
        has_details = true;
      } else if (!std::holds_alternative<Text>(part) && !std::holds_alternative<ToolCall>(part) &&
                 !std::holds_alternative<ToolResult>(part) && !std::holds_alternative<Refusal>(part))
        return reason::error(ErrorKind::Unsupported, reason::chat_request::chat_cannot_represent_this_ordered_part);
    }
    if (has_frames && !has_details)
      return reason::error(ErrorKind::ReplayIneligible, reason::chat_request::reasoning_stream_lacks_complete_reconstructed);
  }
  const json::Limits limits{has_media ? resources.request_bytes : resources.chat_text_request_bytes, resources.json_depth};
  auto limit_error = [&] { return reason::error(has_media ? ErrorKind::ResourceLimit : ErrorKind::InvalidRequest, reason::chat_request::request_exceeds_json_limits_or_encoding); };
  auto build = [&](json::BoundedWriter& body, bool validate) -> std::optional<Error> {
    std::set<std::string_view> pending, names;
    body.raw("{").quoted(descriptor.request_model_member()).raw(":").quoted(request.model);
    body.raw(",").quoted(descriptor.request_messages_member()).raw(":[");
    bool comma = false;
    for (const auto& message : request.canonical_messages) {
      std::string_view role;
      switch (message.role) {
        case Role::System: role = "system"; break;
        case Role::Developer: role = "developer"; break;
        case Role::User: role = "user"; break;
        case Role::Assistant: role = "assistant"; break;
        case Role::Tool: role = "tool"; break;
        default: return bad(reason::chat_request::invalid_message_role);
      }
      if (validate && message.role != Role::Tool && !pending.empty())
        return bad(reason::chat_request::tool_results_must_precede_the_next_message);
      if (message.role == Role::Tool) {
        // The other families take every result of a tool turn in one Tool message. This API
        // wants one wire message per result, so a message of several results is split here,
        // in the caller's order. Nothing else may share the message.
        if (message.parts.empty() || !std::all_of(message.parts.begin(), message.parts.end(),
            [](const Part& part) { return std::holds_alternative<ToolResult>(part); }))
          return bad(reason::chat_request::a_tool_message_holds_only_tool_results_one_or);
        for (const auto& part : message.parts) {
          const auto& result = std::get<ToolResult>(part);
          if (!result.content_parts.empty())
            return reason::error(ErrorKind::Unsupported, reason::chat_request::chat_tool_results_do_not_support_typed_media);
          if (validate && (result.tool_use_id.empty() || pending.erase(result.tool_use_id) != 1))
            return bad(reason::chat_request::tool_result_must_resolve_one_pending_call);
          if (comma) body.raw(",");
          comma = true;
          body.raw("{\"role\":\"tool\",\"content\":").quoted(result.content)
              .raw(",\"tool_call_id\":").quoted(result.tool_use_id).raw("}");
          if (!body.ok()) return limit_error();
        }
        continue;
      }
      if (comma) body.raw(",");
      comma = true;
      body.raw("{\"role\":").quoted(role).raw(",\"content\":");
      {  // Every non-Tool role: ordered content, reasoning, refusal and tool calls.
        // A generated audio reply is replayed by id; with no other content the field is null.
        const Media* audio_reply = nullptr;
        bool content = false;
        for (const auto& part : message.parts) {
          if (const auto* media = std::get_if<Media>(&part); media && message.role == Role::Assistant && media->kind == MediaKind::Audio) audio_reply = media;
          else if (std::holds_alternative<Text>(part) || std::holds_alternative<Media>(part)) content = true;
        }
        if (audio_reply && !content) body.raw("null");
        else {
          body.raw("[");
          bool part_comma = false;
          for (const auto& part : message.parts) {
            if (const auto* text = std::get_if<Text>(&part)) {
              if (part_comma) body.raw(",");
              part_comma = true;
              body.raw("{\"type\":\"text\",\"text\":").quoted(text->value);
              if (write_marks && text->cache) cache_marks::write(body, *text->cache);
              body.raw("}");
            } else if (const auto* media = std::get_if<Media>(&part)) {
              if (message.role == Role::Assistant && media->kind == MediaKind::Audio) continue;
              if (part_comma) body.raw(",");
              part_comma = true;
              write_media(body, *media);
            } else if (std::holds_alternative<ToolResult>(part)) return bad(reason::chat_request::tool_result_requires_tool_role);
          }
          body.raw("]");
        }
        if (audio_reply) body.raw(",\"audio\":{\"id\":").quoted(audio_reply->id).raw("}");
        for (const auto& part : message.parts)
          if (const auto* thinking = std::get_if<Thinking>(&part))
            body.raw(",\"reasoning_content\":").quoted(thinking->text);
        for (const auto& part : message.parts)
          if (const auto* opaque = std::get_if<Opaque>(&part); opaque && opaque->wire_type == "reasoning_details")
            body.raw(",\"reasoning_details\":").value(opaque->wire_metadata->root().get("details"), 3);
        bool call_comma = false, calls = false, refusal = false;
        for (const auto& part : message.parts) if (const auto* value = std::get_if<Refusal>(&part)) {
          if (message.role != Role::Assistant || refusal || !value->raw_code.empty())
            return reason::error(ErrorKind::Unsupported, reason::chat_request::refusal_cannot_be_represented_by_chat);
          refusal = true;
          body.raw(",\"refusal\":").quoted(value->text);
        }
        for (const auto& part : message.parts) if (const auto* call = std::get_if<ToolCall>(&part)) {
          if (message.role != Role::Assistant) return bad(reason::chat_request::tool_calls_require_assistant_role);
          if (validate) {
            if (call->kind != ToolCallKind::ClientExecuted ||
                (!call->wire_type.empty() && call->wire_type != "function") || call->wire_metadata)
              return reason::error(ErrorKind::Unsupported, reason::chat_request::only_plain_client_function_calls_can_be_replayed);
            if (call->id.empty() || call->name.empty() || !call->input || !call->input->root().is_object() ||
                !pending.insert(call->id).second)
              return bad(reason::chat_request::tool_calls_require_unique_ids_names_and_object);
          }
          if (!calls) { body.raw(",\"tool_calls\":["); calls = true; }
          if (call_comma) body.raw(",");
          call_comma = true;
          body.raw("{\"id\":").quoted(call->id).raw(",\"type\":\"function\",\"function\":{\"name\":")
              .quoted(call->name).raw(",\"arguments\":").quoted_json(call->input->root()).raw("}}");
        }
        if (calls) body.raw("]");
      }
      body.raw("}");
      if (!body.ok()) return limit_error();
    }
    for (const auto& message : request.messages) {
      std::string_view role;
      switch (message.role) {
        case Role::System: role = "system"; break;
        case Role::Developer: role = "developer"; break;
        case Role::User: role = "user"; break;
        case Role::Assistant: role = "assistant"; break;
        case Role::Tool: role = "tool"; break;
        default: return bad(reason::chat_request::invalid_message_role);
      }
      if (validate) {
        if (message.role == Role::Tool) {
          if (message.tool_call_id.empty() || !message.tool_calls.empty() || pending.erase(message.tool_call_id) != 1)
            return bad(reason::chat_request::tool_result_must_resolve_one_pending_call);
        } else {
          if (!pending.empty()) return bad(reason::chat_request::tool_results_must_precede_the_next_message);
          if (!message.tool_call_id.empty()) return bad(reason::chat_request::tool_call_id_is_only_valid_on_tool_results);
          if (!message.tool_calls.empty() && message.role != Role::Assistant)
            return bad(reason::chat_request::tool_calls_require_an_assistant_message);
        }
      }
      if (comma) body.raw(",");
      comma = true;
      body.raw("{\"role\":").quoted(role).raw(",\"content\":");
      if (message.media.empty()) body.quoted(message.text);
      else {
        body.raw("[");
        bool media_comma = false;
        for (const auto& media : message.media) {
          if (media_comma) body.raw(",");
          media_comma = true;
          // MIME and base64 are validated ASCII; stream them without a copy.
          write_media(body, media);
          if (!body.ok()) return limit_error();
        }
        if (!message.text.empty())
          body.raw(",{\"type\":\"text\",\"text\":").quoted(message.text).raw("}");
        body.raw("]");
      }
      if (!body.ok()) return limit_error();
      if (message.role == Role::Tool) body.raw(",\"tool_call_id\":").quoted(message.tool_call_id);
      if (!message.tool_calls.empty()) {
        body.raw(",\"tool_calls\":[");
        bool call_comma = false;
        for (const auto& call : message.tool_calls) {
          if (validate) {
            if (call.kind != ToolCallKind::ClientExecuted ||
                (!call.wire_type.empty() && call.wire_type != "function") || call.wire_metadata)
              return reason::error(ErrorKind::Unsupported, reason::chat_request::only_plain_client_function_calls_can_be_replayed);
            if (call.id.empty() || call.name.empty() || !call.input || !call.input->root().is_object() ||
                !pending.insert(call.id).second)
              return bad(reason::chat_request::tool_calls_require_unique_ids_names_and_object);
          }
          if (call_comma) body.raw(",");
          call_comma = true;
          body.raw("{\"id\":").quoted(call.id).raw(",\"type\":\"function\",\"function\":{\"name\":")
              .quoted(call.name).raw(",\"arguments\":").quoted_json(call.input->root()).raw("}}");
          if (!body.ok()) return limit_error();
        }
        body.raw("]");
      }
      body.raw("}");
      if (!body.ok()) return limit_error();
    }
    if (validate && !pending.empty()) return bad(reason::chat_request::missing_tool_results);
    body.raw("],").quoted(descriptor.request_stream_member()).raw(streaming ? ":true" : ":false");
    if (streaming) body.raw(",\"stream_options\":{\"include_usage\":true}");
    if (!request.tools.empty()) {
      body.raw(",\"tools\":["); comma = false;
      for (const auto& tool : request.tools) {
        if (validate && (tool.name.empty() || !tool.parameters || !tool.parameters->root().is_object() ||
                         !names.insert(tool.name).second))
          return bad(reason::chat_request::tools_require_unique_names_and_object_schemas);
        if (comma) body.raw(",");
        comma = true;
        body.raw("{\"type\":\"function\",\"function\":{\"name\":").quoted(tool.name)
            .raw(",\"description\":").quoted(tool.description).raw(",\"parameters\":")
            .value(tool.parameters->root(), 4).raw("}}");
        if (!body.ok()) return limit_error();
      }
      body.raw("]");
    }
    if (request.tool_choice) {
      body.raw(",\"tool_choice\":");
      write_tool_choice(body, *request.tool_choice);
    }
    auto number = [&](std::string_view key, double value) {
      char bytes[64]; const auto converted = std::to_chars(bytes, bytes + sizeof bytes, value);
      body.raw(",").quoted(key).raw(":").raw({bytes, static_cast<std::size_t>(converted.ptr - bytes)});
    };
    if (effective.temperature) number("temperature", *effective.temperature);
    if (effective.top_p) number("top_p", *effective.top_p);
    if (effective.max_output_tokens)
      body.raw(",").quoted(descriptor.max_output_tokens_member()).raw(":").raw(std::to_string(*effective.max_output_tokens));
    if (effective.reasoning_effort) body.raw(",\"reasoning_effort\":").quoted(*effective.reasoning_effort);
    if (effective.service_tier) body.raw(",\"service_tier\":").quoted(*effective.service_tier);
    if (request.reasoning) {
      const auto& reasoning = *request.reasoning;
      body.raw(",\"reasoning\":{");
      bool field = false;
      auto key = [&](std::string_view name) { if (field) body.raw(","); field = true; body.quoted(name).raw(":"); };
      if (reasoning.effort) { key("effort"); body.quoted(*reasoning.effort); }
      if (reasoning.max_tokens) { key("max_tokens"); body.raw(std::to_string(*reasoning.max_tokens)); }
      if (reasoning.exclude) { key("exclude"); body.raw(*reasoning.exclude ? "true" : "false"); }
      if (reasoning.enabled) { key("enabled"); body.raw(*reasoning.enabled ? "true" : "false"); }
      body.raw("}");
    }
    if (request.include_reasoning) body.raw(",\"include_reasoning\":").raw(*request.include_reasoning ? "true" : "false");
    if (request.usage_include) body.raw(",\"usage\":{\"include\":").raw(*request.usage_include ? "true}" : "false}");
    if (!request.plugins.empty()) { body.raw(",\"plugins\":"); request_controls::write_plugins(body, request.plugins); }
    if (!request.models.empty()) {
      body.raw(",\"models\":[");
      bool model_comma = false;
      for (const auto& model : request.models) { if (model_comma) body.raw(","); model_comma = true; body.quoted(model); }
      body.raw("]");
    }
    if (request.provider) {
      body.raw(",\"provider\":");
      request_controls::write_routing(body, *request.provider);
    }
    if (request.response_format) {
      body.raw(",\"response_format\":");
      request_controls::write_response_format(body, *request.response_format, false);
    }
    if (!request.modalities.empty()) {
      body.raw(",\"modalities\":[");
      bool modality_comma = false;
      for (const auto modality : request.modalities) {
        if (modality_comma) body.raw(",");
        modality_comma = true;
        body.quoted(modality_name(modality));
      }
      body.raw("]");
    }
    if (request.audio) {
      body.raw(",\"audio\":{\"voice\":");
      if (!request.audio->voice_id.empty()) body.raw("{\"id\":").quoted(request.audio->voice_id).raw("}");
      else body.quoted(request.audio->voice);
      body.raw(",\"format\":").quoted(request.audio->format).raw("}");
    }
    if (request.image_config) {
      body.raw(",\"image_config\":");
      request_controls::write_image_config(body, *request.image_config);
    }
    request_controls::write_extensions(body, request.extensions);
    body.raw("}");
    if (!body.ok()) return limit_error();
    return {};
  };
  json::BoundedWriter measure(limits);
  if (auto error = build(measure, true)) return std::move(*error);
  EncodedRequest result{"POST", descriptor.resolved_path(streaming, request.model), descriptor.headers(), {}};
  result.headers.emplace_back("Content-Type", "application/json");
  result.headers.emplace_back("Accept", streaming ? "text/event-stream" : "application/json");
  result.body.reserve(measure.size());
  json::BoundedWriter output(limits, &result.body);
  if (auto error = build(output, false)) return std::move(*error);
  result.context = std::move(context);
  result.max_output_tokens = effective.max_output_tokens;
  result.cache_marks = mark_report;
  return result;
}
} // namespace sp::chat
