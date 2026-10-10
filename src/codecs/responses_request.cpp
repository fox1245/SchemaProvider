#include "codecs/responses_request.h"
#include "codecs/media_input.h"
#include "core/native.h"
#include "json/json.h"
#include "descriptor/policy.h"
#include <map>
#include <set>
#include <type_traits>
#include <charconv>
#include <limits>

namespace sp::responses {
namespace {
std::string lower(std::string_view text) {
  std::string value(text);
  for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return value;
}
std::string_view role_name(Role role) {
  switch (role) {
    case Role::System: return "system";
    case Role::Developer: return "developer";
    case Role::User: return "user";
    default: return {};
  }
}
bool function_name(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  for (const char c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}
std::string_view hosted_type(const HostedTool& tool) {
  return std::visit([](const auto& value) -> std::string_view {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, WebSearchTool>) return "web_search";
    if constexpr (std::is_same_v<T, ImageGenerationTool>) return "image_generation";
    if constexpr (std::is_same_v<T, FileSearchTool>) return "file_search";
    if constexpr (std::is_same_v<T, ToolSearchTool>) return "tool_search";
    if constexpr (std::is_same_v<T, ShellTool>) return "shell";
  }, tool);
}
std::string_view verbosity_name(Verbosity value) {
  switch (value) {
    case Verbosity::Low: return "low";
    case Verbosity::Medium: return "medium";
    case Verbosity::High: return "high";
  }
  return {};
}
std::string_view truncation_name(Truncation value) {
  switch (value) {
    case Truncation::Disabled: return "disabled";
    case Truncation::Auto: return "auto";
  }
  return {};
}
std::string_view include_name(Include value) {
  switch (value) {
    case Include::ReasoningEncryptedContent: return "reasoning.encrypted_content";
    case Include::WebSearchSources: return "web_search_call.action.sources";
    case Include::FileSearchResults: return "file_search_call.results";
    case Include::MessageOutputTextLogprobs: return "message.output_text.logprobs";
    case Include::ComputerCallOutputImageUrl: return "computer_call_output.output.image_url";
    case Include::CodeInterpreterCallOutputs: return "code_interpreter_call.outputs";
  }
  return {};
}
bool cursor_valid(std::string_view id) {
  if (id.empty() || id.size() > 256) return false;
  for (const char c : id) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}
// Responses input content: input_image, input_audio, input_video and input_file. Admission has
// already established that the family and origin accept this kind and source form.
void write_media(json::BoundedWriter& body, const Media& media) {
  switch (media.kind) {
    case MediaKind::Image:
      body.raw("{\"type\":\"input_image\",");
      if (media.source == MediaSource::File) body.raw("\"file_id\":").quoted(media.reference);
      else { body.raw("\"image_url\":"); media_input::uri_or_url(body, media); }
      body.raw(",\"detail\":").quoted(image_detail_name(media.detail)).raw("}");
      break;
    case MediaKind::Audio:
      body.raw("{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"").raw(*media.data)
          .raw("\",\"format\":").quoted(*media_input::audio_format(media.mime)).raw("}}");
      break;
    case MediaKind::Video:
      body.raw("{\"type\":\"input_video\",\"video_url\":");
      media_input::uri_or_url(body, media);
      body.raw("}");
      break;
    case MediaKind::Document:
      body.raw("{\"type\":\"input_file\",");
      if (media.source == MediaSource::File) body.raw("\"file_id\":").quoted(media.reference);
      else if (media.source == MediaSource::Url) body.raw("\"file_url\":").quoted(media.reference);
      else {
        body.raw("\"filename\":").quoted(media_input::document_filename(media)).raw(",\"file_data\":");
        media_input::data_uri(body, media);
      }
      body.raw("}");
      break;
  }
}
std::optional<Error> admit(const descriptor::ValidatedDescriptor& descriptor, const Media& media) {
  if (auto error = media_input::admit(descriptor, media)) return error;
  if (media.kind == MediaKind::Audio && !media_input::audio_format(media.mime))
    return Error{ErrorKind::Unsupported, "Responses input_audio has no format name for this MIME type"};
  return std::nullopt;
}
} // namespace
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](std::string message) -> EncodeResult { return Error{ErrorKind::InvalidRequest, std::move(message)}; };
  auto replay_bad = []() -> EncodeResult { return Error{ErrorKind::ReplayIneligible, "native replay provenance, binding or content mismatch"}; };
  if (descriptor.family() != "openai.responses") return Error{ErrorKind::InvalidConfig, "Responses encoder requires Responses descriptor"};
  if (request.model.empty() || request.messages.empty()) return bad("model and messages are required");
  const auto& resources = descriptor.policy()->resources();
  const auto& defaults = descriptor.policy()->defaults(descriptor.family(), request.model);
  auto effective = descriptor::effective_defaults(descriptor, request.model);
  if (request.max_output_tokens) effective.max_output_tokens = request.max_output_tokens;
  if (request.service_tier) effective.service_tier = *request.service_tier;
  if (request.temperature) effective.temperature = request.temperature;
  if (request.top_p) effective.top_p = request.top_p;
  if (request.reasoning) {
    effective.reasoning_enabled = true;
    if (request.reasoning->effort) effective.reasoning_effort = *request.reasoning->effort;
    if (request.reasoning->summary) effective.reasoning_summary = *request.reasoning->summary;
  }
  if (auto error = descriptor::validate_choices(descriptor, request.model, effective)) return bad(std::move(*error));
  if (request.messages.size() > resources.request_messages || request.tools.size() > resources.request_tools ||
      request.hosted_tools.size() > resources.request_tools - request.tools.size())
    return bad("request count limit exceeded");
  if (request.previous_response_id && !cursor_valid(*request.previous_response_id)) return bad("invalid previous response cursor");
  if (!request.previous_response_id && !request.previous_response_history.empty()) return bad("previous response history requires a cursor");
  if (request.previous_response_history.size() > resources.request_messages) return bad("previous response history count limit exceeded");
  for (const auto& message : request.previous_response_history)
    if (message.parts.size() > resources.request_parts) return bad("previous response part count limit exceeded");
  if (!request.previous_response_history.empty()) {
    const auto& previous = request.previous_response_history.back();
    if (previous.role != Role::Assistant || !previous.native || !previous.wire_output ||
        !previous.wire_output->root().is_array() || previous.id != *request.previous_response_id) return replay_bad();
  }
  if (request.verbosity && verbosity_name(*request.verbosity).empty()) return bad("invalid verbosity");
  if (request.truncation && truncation_name(*request.truncation).empty()) return bad("invalid truncation");
  if (request.include) {
    std::set<Include> selections;
    for (const auto selection : *request.include)
      if (include_name(selection).empty() || !selections.insert(selection).second) return bad("invalid or duplicate include selection");
  }
  if (request.provider)
    if (auto error = request_controls::validate_routing(descriptor, *request.provider)) return bad(std::move(*error));
  if (request.response_format)
    if (auto error = request_controls::validate_response_format(*request.response_format, resources)) return bad(std::move(*error));
  std::set<std::string_view> hosted_types;
  for (const auto& tool : request.hosted_tools) {
    const auto type = hosted_type(tool);
    bool admitted = false;
    for (const auto& fact : descriptor.family_policy().server_tools)
      if (fact.type == type) admitted = true;
    if (!admitted) return Error{ErrorKind::Unsupported, "hosted Responses tool is not admitted by policy"};
    if (!hosted_types.insert(type).second) return bad("hosted tool types must be unique");
    if (const auto* image = std::get_if<ImageGenerationTool>(&tool)) {
      if (image->size && *image->size != "auto" && *image->size != "1024x1024" &&
          *image->size != "1024x1536" && *image->size != "1536x1024")
        return bad("unsupported image generation size");
      if (image->quality && *image->quality != "auto" && *image->quality != "low" &&
          *image->quality != "medium" && *image->quality != "high")
        return bad("unsupported image generation quality");
      if (image->output_format && *image->output_format != "png" && *image->output_format != "jpeg" &&
          *image->output_format != "webp")
        return bad("unsupported image generation output format");
    }
    if (const auto* files = std::get_if<FileSearchTool>(&tool)) {
      if (files->vector_store_ids.empty() || files->vector_store_ids.size() > resources.request_parts)
        return bad("file search requires bounded vector store ids");
      std::set<std::string_view> ids;
      for (const auto& id : files->vector_store_ids)
        if (id.empty() || !ids.insert(id).second) return bad("vector store ids must be nonempty and unique");
    }
    if (const auto* shell = std::get_if<ShellTool>(&tool)) {
      if (shell->environment.skills.size() > resources.request_parts) return bad("shell skill count limit exceeded");
      std::set<std::string_view> ids;
      for (const auto& skill : shell->environment.skills)
        if (skill.skill_id.empty() || !ids.insert(skill.skill_id).second)
          return bad("shell skill references must be nonempty and unique");
    }
  }
  bool deferred = false;
  for (const auto& tool : request.tools) deferred = deferred || tool.defer_loading.value_or(false);
  if (deferred && !hosted_types.contains("tool_search"))
    return bad("deferred functions require tool_search");
  if (hosted_types.contains("tool_search") && !deferred)
    return bad("tool_search requires a deferred function definition");
  // Captured groups never degrade to caller-imported assistant history when their
  // seal or original items are removed. Only plain input and tool results are imports.
  bool captured_history = false;
  std::optional<Error> media_error;
  for (const auto& message : request.messages) {
    if (message.parts.size() > resources.request_parts) return bad("request part count limit exceeded");
    if (request.previous_response_id && (message.native || message.wire_output || message.role == Role::Assistant)) return replay_bad();
    if (message.native) {
      if (message.role != Role::Assistant || !message.wire_output || !message.wire_output->root().is_array()) return replay_bad();
    } else {
      if (message.role == Role::Assistant || message.wire_output) return replay_bad();
      for (const auto& part : message.parts) {
        if (!std::holds_alternative<Text>(part) && !std::holds_alternative<ToolResult>(part) &&
            !std::holds_alternative<Media>(part)) return replay_bad();
      }
    }
    captured_history = captured_history || static_cast<bool>(message.native);
    for (const auto& part : message.parts) {
      if (const auto* media = std::get_if<Media>(&part); media && !media_error && !message.native) {
        if (message.role != Role::User) media_error = Error{ErrorKind::InvalidRequest, "media inputs require user role"};
        else media_error = admit(descriptor, *media);
      } else if (const auto* tool = std::get_if<ToolResult>(&part); tool && !media_error) {
        if (!tool->content_parts.empty() && !tool->content.empty())
          media_error = Error{ErrorKind::InvalidRequest, "tool result content and content_parts are mutually exclusive"};
        else if (tool->content_parts.size() > resources.request_parts)
          media_error = Error{ErrorKind::InvalidRequest, "tool result part count limit exceeded"};
        else for (const auto& nested : tool->content_parts) {
          if (const auto* media = std::get_if<Media>(&nested)) {
            if ((media->kind == MediaKind::Audio || media->kind == MediaKind::Video) &&
                static_cast<unsigned>(media->source) <= 2)
              media_error = Error{ErrorKind::Unsupported, "Responses function output supports images and files only"};
            else media_error = admit(descriptor, *media);
            if (!media_error && (!media->id.empty() || !media->transcript.empty() ||
                (media->kind == MediaKind::Image && !media->name.empty()) ||
                (media->kind == MediaKind::Document && media->detail != ImageDetail::Auto) ||
                (media->kind == MediaKind::Document && media->source != MediaSource::Inline && !media->name.empty())))
              media_error = Error{ErrorKind::Unsupported, "function output media contains unsupported controls or metadata"};
            if (media_error) break;
          }
        }
      }
    }
  }
  // Preserve native-prefix mismatch precedence over payload errors in edited
  // captured history, but reject invalid ordinary input before hashing.
  if (media_error && !captured_history) return *media_error;
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, effective, streaming));
  if (!context->history_valid_) return replay_bad();
  if (!context->valid_) return Error{ErrorKind::ResourceLimit, "native binding could not be captured"};
  if (media_error) return *media_error;
  EncodedRequest result{"POST", std::string(descriptor.path(streaming)), {}, {}, {}};
  for (const auto& header : descriptor.headers()) {
    const auto key = lower(header.first);
    if (key != "content-type" && key != "accept") result.headers.push_back(header);
  }
  result.headers.emplace_back("Content-Type", "application/json");
  result.headers.emplace_back("Accept", streaming ? "text/event-stream" : "application/json");
  auto build = [&](json::BoundedWriter& body) -> std::optional<Error> {
    auto invalid = [](std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; };
    body.raw("{").quoted(descriptor.request_model_member()).raw(":").quoted(request.model);
    body.raw(",").quoted(descriptor.request_stream_member()).raw(streaming ? ":true" : ":false");
    body.raw(",\"store\":").raw(request.store.value_or(false) ? "true" : "false");
    if (request.previous_response_id) body.raw(",\"previous_response_id\":").quoted(*request.previous_response_id);
    if (request.parallel_tool_calls) body.raw(",\"parallel_tool_calls\":").raw(*request.parallel_tool_calls ? "true" : "false");
    if (request.truncation) body.raw(",\"truncation\":").quoted(truncation_name(*request.truncation));
    body.raw(",\"include\":[");
    if (request.include) {
      bool comma = false;
      for (const auto selection : *request.include) {
        if (comma) body.raw(",");
        comma = true;
        body.quoted(include_name(selection));
      }
    } else body.quoted("reasoning.encrypted_content");
    body.raw("]");
    auto number = [&](std::string_view key, double value) {
      char bytes[64];
      const auto converted = std::to_chars(bytes, bytes + sizeof bytes, value);
      body.raw(",").quoted(key).raw(":").raw({bytes, static_cast<size_t>(converted.ptr - bytes)});
    };
    if (effective.temperature) number("temperature", *effective.temperature);
    if (effective.top_p) number("top_p", *effective.top_p);
    if (request.provider) {
      body.raw(",\"provider\":");
      request_controls::write_routing(body, *request.provider);
    }
    if (request.response_format || request.verbosity) {
      body.raw(",\"text\":{");
      if (request.response_format) {
        body.raw("\"format\":");
        request_controls::write_response_format(body, *request.response_format, true);
      }
      if (request.verbosity) {
        if (request.response_format) body.raw(",");
        body.raw("\"verbosity\":").quoted(verbosity_name(*request.verbosity));
      }
      body.raw("}");
    }
    if (!request.instructions.empty()) body.raw(",\"instructions\":").quoted(request.instructions);
    if (effective.max_output_tokens) body.raw(",").quoted(descriptor.max_output_tokens_member()).raw(":").raw(std::to_string(*effective.max_output_tokens));
    if (request.max_tool_calls) body.raw(",\"max_tool_calls\":").raw(std::to_string(*request.max_tool_calls));
    if (effective.reasoning_enabled) body.raw(",\"reasoning\":{\"effort\":").quoted(*effective.reasoning_effort)
        .raw(",\"summary\":").quoted(*effective.reasoning_summary).raw("}");
    if (effective.service_tier) body.raw(",\"service_tier\":").quoted(*effective.service_tier);
    std::set<std::string_view> tool_names;
    if (!request.tools.empty() || !request.hosted_tools.empty()) {
      body.raw(",\"tools\":[");
      bool comma = false;
      for (const auto& tool : request.tools) {
        if (!function_name(tool.name) || !tool_names.insert(tool.name).second) return invalid("tools require unique valid function names");
        if (!tool.parameters || !tool.parameters->root().is_object()) return invalid("function parameters must be an object schema");
        if (comma) body.raw(",");
        comma = true;
        body.raw("{\"type\":\"function\",\"name\":").quoted(tool.name).raw(",\"description\":").quoted(tool.description)
            .raw(",\"parameters\":").value(tool.parameters->root(), 3);
        const auto strict = tool.strict ? tool.strict : defaults.strict_tools;
        if (strict) body.raw(",\"strict\":").raw(*strict ? "true" : "false");
        if (tool.defer_loading) body.raw(",\"defer_loading\":").raw(*tool.defer_loading ? "true" : "false");
        body.raw("}");
      }
      for (const auto& tool : request.hosted_tools) {
        if (comma) body.raw(",");
        comma = true;
        body.raw("{\"type\":").quoted(hosted_type(tool));
        std::visit([&](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, ImageGenerationTool>) {
            if (value.size) body.raw(",\"size\":").quoted(*value.size);
            if (value.quality) body.raw(",\"quality\":").quoted(*value.quality);
            if (value.output_format) body.raw(",\"output_format\":").quoted(*value.output_format);
          } else if constexpr (std::is_same_v<T, FileSearchTool>) {
            body.raw(",\"vector_store_ids\":[");
            bool id_comma = false;
            for (const auto& id : value.vector_store_ids) {
              if (id_comma) body.raw(",");
              id_comma = true; body.quoted(id);
            }
            body.raw("]");
          } else if constexpr (std::is_same_v<T, ShellTool>) {
            body.raw(",\"environment\":{\"type\":\"container_auto\"");
            if (!value.environment.skills.empty()) {
              body.raw(",\"skills\":[");
              bool skill_comma = false;
              for (const auto& skill : value.environment.skills) {
                if (skill_comma) body.raw(",");
                skill_comma = true;
                body.raw("{\"type\":\"skill_reference\",\"skill_id\":").quoted(skill.skill_id).raw("}");
              }
              body.raw("]");
            }
            body.raw("}");
          }
        }, tool);
        body.raw("}");
      }
      body.raw("]");
    }
    if (request.required_tool) {
      if (!tool_names.contains(*request.required_tool)) return invalid("required_tool must name a declared client function");
      body.raw(",\"tool_choice\":{\"type\":\"function\",\"name\":").quoted(*request.required_tool).raw("}");
    }
    body.raw(",").quoted(descriptor.request_messages_member()).raw(":[");
    std::set<std::string_view> item_ids, call_ids, pending;
    bool input_comma = false;
    auto separator = [&] { if (input_comma) body.raw(","); input_comma = true; };
    auto captured = [&](const Message& message, bool emit_items) -> std::optional<Error> {
        if (!pending.empty()) return invalid("client tool results must precede the next captured assistant group");
        std::map<std::string_view, const ToolCall*> calls;
        for (const auto& part : message.parts) {
          if (const auto* call = std::get_if<ToolCall>(&part)) {
            if (call->kind != ToolCallKind::ClientExecuted || call->wire_type != "function_call") return Error{ErrorKind::Unsupported, "only client function calls authorize tool results"};
            if (call->id.empty() || !tool_names.contains(call->name) || !call->input || !call->input->root().is_object() ||
                !call_ids.insert(call->id).second || !calls.emplace(call->id, call).second) return invalid("captured client call has invalid or duplicate ownership");
            pending.insert(call->id);
          } else if (!std::holds_alternative<Text>(part) && !std::holds_alternative<Refusal>(part) &&
              !std::holds_alternative<Reasoning>(part) && !std::holds_alternative<Opaque>(part) &&
              !std::holds_alternative<Media>(part)) {
            return Error{ErrorKind::Unsupported, "foreign or invalid part cannot be replayed as Responses output"};
          }
        }
        // Keep the entire generation in its original order, including opaque
        // server items, reasoning, function item id/call_id and assistant phase.
        for (const auto item : message.wire_output->root().elements()) {
          if (!item.is_object() || !item.get("type").is_string()) return invalid("captured output item must have a type");
          const auto id = item.get("id");
          if (id.valid() && (!id.is_string() || id.as_string().empty() || !item_ids.insert(id.as_string()).second)) return invalid("captured output item ids must be unique and nonempty");
          if (item.get("type").as_string() == "function_call") {
            const auto call_id = item.get("call_id"), name = item.get("name"), arguments = item.get("arguments");
            if (!id.is_string() || !call_id.is_string() || !name.is_string() || !arguments.is_string()) return invalid("captured function item lacks its original identity");
            const auto found = calls.find(call_id.as_string());
            if (found == calls.end() || found->second->name != name.as_string()) return invalid("captured function item lacks unique client ownership");
            calls.erase(found);
          }
          if (emit_items) { separator(); body.value(item, 2); }
        }
        if (!calls.empty()) return invalid("captured function call is missing its output item");
        return std::nullopt;
    };
    if (!request.previous_response_history.empty())
      if (auto error = captured(request.previous_response_history.back(), false)) return error;
    for (const auto& message : request.messages) {
      if (message.native) {
        if (auto error = captured(message, true)) return error;
      } else {
        if (message.parts.empty()) return invalid("plain input messages require content");
        const auto role = role_name(message.role);
        const bool results = std::holds_alternative<ToolResult>(message.parts.front());
        if (results) {
          if (message.role != Role::Tool && message.role != Role::User) return invalid("tool results require tool or user role");
          for (const auto& part : message.parts) {
            const auto* tool_result = std::get_if<ToolResult>(&part);
            if (!tool_result || tool_result->tool_use_id.empty() || pending.erase(tool_result->tool_use_id) != 1) return invalid("tool result lacks exactly one pending client call_id");
            if (tool_result->is_error) return Error{ErrorKind::Unsupported, "Responses tool results have no typed is_error field"};
            separator(); body.raw("{\"type\":\"function_call_output\",\"call_id\":").quoted(tool_result->tool_use_id).raw(",\"output\":");
            if (tool_result->content_parts.empty()) body.quoted(tool_result->content);
            else {
              body.raw("["); bool nested_comma = false;
              for (const auto& nested : tool_result->content_parts) {
                if (nested_comma) body.raw(",");
                nested_comma = true;
                if (const auto* text = std::get_if<Text>(&nested))
                  body.raw("{\"type\":\"input_text\",\"text\":").quoted(text->value).raw("}");
                else write_media(body, std::get<Media>(nested));
                if (!body.ok()) return Error{ErrorKind::ResourceLimit, "request exceeds JSON byte, depth or encoding limits"};
              }
              body.raw("]");
            }
            body.raw("}");
          }
        } else {
          if (role.empty() || !pending.empty()) return invalid("plain input role or pending client tool results are invalid");
          separator(); body.raw("{\"role\":").quoted(role).raw(",\"content\":[");
          bool comma = false;
          for (const auto& part : message.parts) {
            if (comma) body.raw(",");
            comma = true;
            if (const auto* text = std::get_if<Text>(&part)) {
              body.raw("{\"type\":\"input_text\",\"text\":").quoted(text->value).raw("}");
            } else if (const auto* media = std::get_if<Media>(&part)) {
              write_media(body, *media);
            } else return Error{ErrorKind::Unsupported, "plain Responses input supports typed text and media only"};
            if (!body.ok()) return Error{ErrorKind::ResourceLimit, "request exceeds JSON byte, depth or encoding limits"};
          }
          body.raw("]}");
        }
      }
      if (!body.ok()) return Error{ErrorKind::ResourceLimit, "request exceeds JSON byte, depth or encoding limits"};
    }
    if (!pending.empty()) return invalid("all client function calls require results before dispatch");
    body.raw("]}");
    if (!body.ok()) return Error{ErrorKind::ResourceLimit, "request exceeds JSON byte, depth or encoding limits"};
    return std::nullopt;
  };
  json::BoundedWriter measured({resources.request_bytes, resources.json_depth});
  if (auto error = build(measured)) return *error;
  result.body.reserve(measured.size());
  json::BoundedWriter body({measured.size(), resources.json_depth}, &result.body);
  if (auto error = build(body)) return *error;
  result.context = std::move(context);
  result.max_output_tokens = effective.max_output_tokens;
  if (!request.hosted_tools.empty()) {
    if (!request.max_tool_calls || *request.max_tool_calls == std::numeric_limits<std::uint64_t>::max())
      result.model_invocation_limit.reset();
    else result.model_invocation_limit = *request.max_tool_calls + 1;
  }
  return result;
}
} // namespace sp::responses
