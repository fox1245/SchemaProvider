#include "codecs/chat.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <charconv>
#include <cmath>
#include <set>
#include <limits>

namespace sp::chat {
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; };
  if (descriptor.family() != "openai.chat") return Error{ErrorKind::InvalidConfig, "descriptor family does not match Chat codec"};
  if (request.model.empty() || (request.messages.empty() && request.canonical_messages.empty()))
    return bad("model and messages are required");
  if (!request.messages.empty() && !request.canonical_messages.empty())
    return bad("choose one message representation");
  const auto& resources = descriptor.policy()->resources();
  auto effective = descriptor::effective_defaults(descriptor, request.model);
  if (request.max_output_tokens) effective.max_output_tokens = request.max_output_tokens;
  if (request.temperature) effective.temperature = request.temperature;
  if (request.top_p) effective.top_p = request.top_p;
  if (request.reasoning_effort) effective.reasoning_effort = *request.reasoning_effort;
  if (request.service_tier) effective.service_tier = *request.service_tier;
  if (auto error = descriptor::validate_choices(descriptor, request.model, effective)) return bad(std::move(*error));
  if (request.reasoning || request.include_reasoning || request.usage_include || !request.models.empty()) {
    if (!descriptor::contains(descriptor.family_policy().openrouter_origins, descriptor.base_url()))
      return bad("gateway controls require declared OpenRouter origin");
  }
  if (request.reasoning) {
    const auto& reasoning = *request.reasoning;
    if (!reasoning.effort && !reasoning.max_tokens && !reasoning.exclude && !reasoning.enabled)
      return bad("reasoning options must not be empty");
    if (reasoning.effort && !descriptor::contains(descriptor.family_policy().reasoning_efforts, *reasoning.effort))
      return bad("unsupported reasoning effort");
    if (reasoning.effort && reasoning.max_tokens)
      return bad("reasoning effort and max_tokens are mutually exclusive");
    if (reasoning.max_tokens && (!*reasoning.max_tokens ||
        *reasoning.max_tokens > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        (effective.max_output_tokens && *reasoning.max_tokens > *effective.max_output_tokens)))
      return bad("reasoning max_tokens outside output budget");
    if (reasoning.enabled == false && (reasoning.effort || reasoning.max_tokens))
      return bad("disabled reasoning cannot specify effort or budget");
    if (effective.reasoning_effort)
      return bad("scalar and nested reasoning controls conflict");
    if (reasoning.exclude == true && request.include_reasoning == true)
      return bad("excluded reasoning cannot be included");
  }
  if (request.models.size() > resources.request_tools) return bad("alternative model count limit exceeded");
  std::set<std::string_view> alternative_models;
  for (const auto& model : request.models) {
    if (model.empty() || !alternative_models.insert(model).second)
      return bad("alternative models require unique nonempty names");
    if (auto error = descriptor::validate_choices(descriptor, model, effective)) return bad(std::move(*error));
  }
  if (request.provider)
    if (auto error = request_controls::validate_routing(descriptor, *request.provider))
      return bad(std::move(*error));
  if (request.response_format)
    if (auto error = request_controls::validate_response_format(*request.response_format, resources))
      return bad(std::move(*error));
  if (std::max(request.messages.size(), request.canonical_messages.size()) > resources.request_messages || request.tools.size() > resources.request_tools)
    return bad("request count limit exceeded");
  bool has_images = false;
  for (const auto& message : request.messages) {
    if (message.images.size() > resources.request_parts || message.tool_calls.size() > resources.request_parts)
      return bad("request part count limit exceeded");
    if (!message.images.empty() && message.role != Role::User)
      return bad("image inputs require user role");
    for (const auto& image : message.images) {
      if (!valid_image(image, resources.image_decoded_bytes)) return bad("invalid inline image payload");
      has_images = true;
    }
  }
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, effective, streaming));
  if (!context->history_valid_)
    return Error{ErrorKind::ReplayIneligible, "native replay provenance, binding or content mismatch"};
  if (!context->valid_)
    return Error{ErrorKind::ResourceLimit, "native Chat binding could not be captured"};
  for (const auto& message : request.canonical_messages) {
    if (message.wire_output || (message.native && message.role != Role::Assistant))
      return Error{ErrorKind::ReplayIneligible, "invalid native Chat history representation"};
    if (message.parts.size() > resources.request_parts) return bad("request part count limit exceeded");
    bool has_thinking = false;
    bool has_details = false;
    bool has_frames = false;
    for (const auto& part : message.parts) {
      if (const auto* image = std::get_if<Image>(&part)) {
        if (message.role != Role::User || !valid_image(*image, resources.image_decoded_bytes))
          return bad("invalid image or role");
        has_images = true;
      } else if (const auto* thinking = std::get_if<Thinking>(&part)) {
        if (message.role != Role::Assistant || has_thinking || thinking->signature)
          return Error{ErrorKind::ReplayIneligible, "Chat requires one plain unsigned assistant thinking part"};
        has_thinking = true;
      } else if (const auto* opaque = std::get_if<Opaque>(&part)) {
        if (opaque->wire_type == "reasoning_details.frame") {
          if (!message.native || !opaque->wire_metadata)
            return Error{ErrorKind::ReplayIneligible, "reasoning frame lacks native authority"};
          const auto metadata = opaque->wire_metadata->root();
          if (!metadata.is_object() || metadata.size() != 2 ||
              metadata.get("type").as_string() != "reasoning_details.frame" ||
              !metadata.get("details").is_array())
            return Error{ErrorKind::ReplayIneligible, "invalid retained reasoning frame"};
          has_frames = true;
          continue;
        }
        if (opaque->wire_type != "reasoning_details" || !message.native ||
            message.role != Role::Assistant || has_details || !opaque->wire_metadata ||
            !descriptor::contains(descriptor.family_policy().openrouter_origins, descriptor.base_url()))
          return Error{ErrorKind::ReplayIneligible, "Chat reasoning details require authenticated native replay"};
        const auto metadata = opaque->wire_metadata->root();
        if (!metadata.is_object() || (metadata.size() != 2 && metadata.size() != 3) ||
            metadata.get("type").as_string() != "reasoning_details" ||
            !metadata.get("details").is_array() ||
            (metadata.size() == 3 && !metadata.get("frames").is_array()))
          return Error{ErrorKind::ReplayIneligible, "invalid captured reasoning details snapshot"};
        has_details = true;
      } else if (!std::holds_alternative<Text>(part) && !std::holds_alternative<ToolCall>(part) &&
                 !std::holds_alternative<ToolResult>(part) && !std::holds_alternative<Refusal>(part))
        return Error{ErrorKind::Unsupported, "Chat cannot represent this ordered part"};
    }
    if (has_frames && !has_details)
      return Error{ErrorKind::ReplayIneligible, "reasoning stream lacks complete reconstructed details"};
  }
  const json::Limits limits{has_images ? resources.request_bytes : resources.chat_text_request_bytes, resources.json_depth};
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
        default: return bad("invalid message role");
      }
      if (validate && message.role != Role::Tool && !pending.empty())
        return bad("tool results must precede the next message");
      if (comma) body.raw(",");
      comma = true;
      body.raw("{\"role\":").quoted(role).raw(",\"content\":");
      if (message.role == Role::Tool) {
        if (message.parts.size() != 1 || !std::holds_alternative<ToolResult>(message.parts.front()))
          return bad("tool message requires exactly one typed result");
        const auto& result = std::get<ToolResult>(message.parts.front());
        if (validate && (result.tool_use_id.empty() || pending.erase(result.tool_use_id) != 1))
          return bad("tool result must resolve one pending call");
        body.quoted(result.content).raw(",\"tool_call_id\":").quoted(result.tool_use_id);
      } else {
        body.raw("[");
        bool part_comma = false;
        for (const auto& part : message.parts) {
          if (const auto* text = std::get_if<Text>(&part)) {
            if (part_comma) body.raw(",");
            part_comma = true;
            body.raw("{\"type\":\"text\",\"text\":").quoted(text->value).raw("}");
          } else if (const auto* image = std::get_if<Image>(&part)) {
            if (part_comma) body.raw(",");
            part_comma = true;
            body.raw("{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:")
                .raw(image->mime).raw(";base64,").raw(*image->data)
                .raw("\",\"detail\":").quoted(image_detail_name(image->detail)).raw("}}");
          } else if (std::holds_alternative<ToolResult>(part)) return bad("tool result requires tool role");
        }
        body.raw("]");
        for (const auto& part : message.parts)
          if (const auto* thinking = std::get_if<Thinking>(&part))
            body.raw(",\"reasoning_content\":").quoted(thinking->text);
        for (const auto& part : message.parts)
          if (const auto* opaque = std::get_if<Opaque>(&part); opaque && opaque->wire_type == "reasoning_details")
            body.raw(",\"reasoning_details\":").value(opaque->wire_metadata->root().get("details"), 3);
        bool call_comma = false, calls = false, refusal = false;
        for (const auto& part : message.parts) if (const auto* value = std::get_if<Refusal>(&part)) {
          if (message.role != Role::Assistant || refusal || !value->raw_code.empty())
            return Error{ErrorKind::Unsupported, "refusal cannot be represented by Chat"};
          refusal = true;
          body.raw(",\"refusal\":").quoted(value->text);
        }
        for (const auto& part : message.parts) if (const auto* call = std::get_if<ToolCall>(&part)) {
          if (message.role != Role::Assistant) return bad("tool calls require assistant role");
          if (validate) {
            if (call->kind != ToolCallKind::ClientExecuted ||
                (!call->wire_type.empty() && call->wire_type != "function") || call->wire_metadata)
              return Error{ErrorKind::Unsupported, "only plain client function calls can be replayed"};
            if (call->id.empty() || call->name.empty() || !call->input || !call->input->root().is_object() ||
                !pending.insert(call->id).second)
              return bad("tool calls require unique ids, names and object arguments");
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
      if (!body.ok()) return bad("request exceeds JSON limits or encoding");
    }
    for (const auto& message : request.messages) {
      std::string_view role;
      switch (message.role) {
        case Role::System: role = "system"; break;
        case Role::Developer: role = "developer"; break;
        case Role::User: role = "user"; break;
        case Role::Assistant: role = "assistant"; break;
        case Role::Tool: role = "tool"; break;
        default: return bad("invalid message role");
      }
      if (validate) {
        if (message.role == Role::Tool) {
          if (message.tool_call_id.empty() || !message.tool_calls.empty() || pending.erase(message.tool_call_id) != 1)
            return bad("tool result must resolve one pending call");
        } else {
          if (!pending.empty()) return bad("tool results must precede the next message");
          if (!message.tool_call_id.empty()) return bad("tool_call_id is only valid on tool results");
          if (!message.tool_calls.empty() && message.role != Role::Assistant)
            return bad("tool calls require an assistant message");
        }
      }
      if (comma) body.raw(",");
      comma = true;
      body.raw("{\"role\":").quoted(role).raw(",\"content\":");
      if (message.images.empty()) body.quoted(message.text);
      else {
        body.raw("[");
        bool image_comma = false;
        for (const auto& image : message.images) {
          if (image_comma) body.raw(",");
          image_comma = true;
          // MIME and base64 are validated ASCII; stream the URI without a copy.
          body.raw("{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:")
              .raw(image.mime).raw(";base64,").raw(*image.data)
              .raw("\",\"detail\":").quoted(image_detail_name(image.detail)).raw("}}");
          if (!body.ok()) return bad("request exceeds JSON limits or encoding");
        }
        if (!message.text.empty())
          body.raw(",{\"type\":\"text\",\"text\":").quoted(message.text).raw("}");
        body.raw("]");
      }
      if (!body.ok()) return bad("request exceeds JSON limits or encoding");
      if (message.role == Role::Tool) body.raw(",\"tool_call_id\":").quoted(message.tool_call_id);
      if (!message.tool_calls.empty()) {
        body.raw(",\"tool_calls\":[");
        bool call_comma = false;
        for (const auto& call : message.tool_calls) {
          if (validate) {
            if (call.kind != ToolCallKind::ClientExecuted ||
                (!call.wire_type.empty() && call.wire_type != "function") || call.wire_metadata)
              return Error{ErrorKind::Unsupported, "only plain client function calls can be replayed"};
            if (call.id.empty() || call.name.empty() || !call.input || !call.input->root().is_object() ||
                !pending.insert(call.id).second)
              return bad("tool calls require unique ids, names and object arguments");
          }
          if (call_comma) body.raw(",");
          call_comma = true;
          body.raw("{\"id\":").quoted(call.id).raw(",\"type\":\"function\",\"function\":{\"name\":")
              .quoted(call.name).raw(",\"arguments\":").quoted_json(call.input->root()).raw("}}");
          if (!body.ok()) return bad("request exceeds JSON limits or encoding");
        }
        body.raw("]");
      }
      body.raw("}");
      if (!body.ok()) return bad("request exceeds JSON limits or encoding");
    }
    if (validate && !pending.empty()) return bad("missing tool results");
    body.raw("],").quoted(descriptor.request_stream_member()).raw(streaming ? ":true" : ":false");
    if (streaming) body.raw(",\"stream_options\":{\"include_usage\":true}");
    if (!request.tools.empty()) {
      body.raw(",\"tools\":["); comma = false;
      for (const auto& tool : request.tools) {
        if (validate && (tool.name.empty() || !tool.parameters || !tool.parameters->root().is_object() ||
                         !names.insert(tool.name).second))
          return bad("tools require unique names and object schemas");
        if (comma) body.raw(",");
        comma = true;
        body.raw("{\"type\":\"function\",\"function\":{\"name\":").quoted(tool.name)
            .raw(",\"description\":").quoted(tool.description).raw(",\"parameters\":")
            .value(tool.parameters->root(), 4).raw("}}");
        if (!body.ok()) return bad("request exceeds JSON limits or encoding");
      }
      body.raw("]");
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
    body.raw("}");
    if (!body.ok()) return bad("request exceeds JSON limits or encoding");
    return {};
  };
  json::BoundedWriter measure(limits);
  if (auto error = build(measure, true)) return std::move(*error);
  EncodedRequest result{"POST", std::string(descriptor.path(streaming)), descriptor.headers(), {}};
  result.headers.emplace_back("Content-Type", "application/json");
  result.headers.emplace_back("Accept", streaming ? "text/event-stream" : "application/json");
  result.body.reserve(measure.size());
  json::BoundedWriter output(limits, &result.body);
  if (auto error = build(output, false)) return std::move(*error);
  result.context = std::move(context);
  result.max_output_tokens = effective.max_output_tokens;
  return result;
}
} // namespace sp::chat
