#include "codecs/chat.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <charconv>
#include <cmath>
#include <set>

namespace sp::chat {
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; };
  if (descriptor.family() != "openai.chat") return Error{ErrorKind::InvalidConfig, "descriptor family does not match Chat codec"};
  if (request.model.empty() || request.messages.empty()) return bad("model and messages are required");
  if (request.temperature && (!std::isfinite(*request.temperature) || *request.temperature < 0 || *request.temperature > 2)) return bad("temperature outside range");
  if (request.top_p && (!std::isfinite(*request.top_p) || *request.top_p < 0 || *request.top_p > 1)) return bad("top_p outside range");
  if (request.max_output_tokens && !*request.max_output_tokens) return bad("max_output_tokens must be positive");
  if (request.reasoning_effort && *request.reasoning_effort != "none" &&
      *request.reasoning_effort != "low" && *request.reasoning_effort != "medium" &&
      *request.reasoning_effort != "high" && *request.reasoning_effort != "xhigh" &&
      *request.reasoning_effort != "max")
    return bad("unsupported typed reasoning effort");
  bool has_images = false;
  for (const auto& message : request.messages) {
    if (!message.images.empty() && message.role != Role::User)
      return bad("image inputs require user role");
    for (const auto& image : message.images) {
      if (!valid_image(image)) return bad("invalid inline image payload");
      has_images = true;
    }
  }
  const json::Limits limits{has_images ? 16U << 20 : 1U << 20, 64};
  auto build = [&](json::BoundedWriter& body, bool validate) -> std::optional<Error> {
    std::set<std::string_view> pending, names;
    body.raw("{").quoted(descriptor.request_model_member()).raw(":").quoted(request.model);
    body.raw(",").quoted(descriptor.request_messages_member()).raw(":[");
    bool comma = false;
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
    if (request.temperature) number("temperature", *request.temperature);
    if (request.top_p) number("top_p", *request.top_p);
    if (request.max_output_tokens)
      body.raw(",").quoted(descriptor.max_output_tokens_member()).raw(":").raw(std::to_string(*request.max_output_tokens));
    if (request.reasoning_effort) body.raw(",\"reasoning_effort\":").quoted(*request.reasoning_effort);
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
  return result;
}
} // namespace sp::chat
