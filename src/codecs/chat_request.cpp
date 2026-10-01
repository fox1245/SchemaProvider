#include "codecs/chat.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <charconv>
#include <cmath>
#include <set>

namespace sp::chat {
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](std::string message) -> EncodeResult { return Error{ErrorKind::InvalidRequest, std::move(message)}; };
  if (descriptor.family() != "openai.chat") return Error{ErrorKind::InvalidConfig, "descriptor family does not match Chat codec"};
  if (request.model.empty() || request.messages.empty()) return bad("model and messages are required");
  if (request.temperature && (!std::isfinite(*request.temperature) || *request.temperature < 0 || *request.temperature > 2)) return bad("temperature outside range");
  if (request.top_p && (!std::isfinite(*request.top_p) || *request.top_p < 0 || *request.top_p > 1)) return bad("top_p outside range");
  if (request.max_output_tokens && !*request.max_output_tokens) return bad("max_output_tokens must be positive");
  EncodedRequest result{"POST", std::string(descriptor.path(streaming)), descriptor.headers(), "{"};
  result.headers.emplace_back("Content-Type", "application/json");
  result.headers.emplace_back("Accept", streaming ? "text/event-stream" : "application/json");
  auto& body = result.body;
  body += json::quote(descriptor.request_model_member()) + ":" + json::quote(request.model);
  body += "," + json::quote(descriptor.request_messages_member()) + ":[";
  bool comma = false;
  for (const auto& message : request.messages) {
    std::string_view role;
    switch (message.role) {
      case Role::System: role = "system"; break;
      case Role::Developer: role = "developer"; break;
      case Role::User: role = "user"; break;
      case Role::Assistant: role = "assistant"; break;
      case Role::Tool: return Error{ErrorKind::Unsupported, "tool result input is outside the first Chat cell"};
      default: return bad("invalid message role");
    }
    if (comma) body += ',';
    comma = true;
    body += "{\"role\":" + json::quote(role) + ",\"content\":" + json::quote(message.text) + "}";
  }
  body += "]," + json::quote(descriptor.request_stream_member()) + (streaming ? ":true" : ":false");
  if (streaming) body += ",\"stream_options\":{\"include_usage\":true}";
  if (!request.tools.empty()) {
    body += ",\"tools\":["; comma = false;
    std::set<std::string_view> names;
    for (const auto& tool : request.tools) {
      if (tool.name.empty() || !tool.parameters || !tool.parameters->root().is_object() || !names.insert(tool.name).second) return bad("tools require unique names and object schemas");
      if (comma) body += ',';
      comma = true;
      body += "{\"type\":\"function\",\"function\":{\"name\":" + json::quote(tool.name) + ",\"description\":" + json::quote(tool.description) + ",\"parameters\":" + tool.parameters->root().dump() + "}}";
    }
    body += ']';
  }
  auto number = [&](std::string_view key, double value) {
    char bytes[64]; auto converted = std::to_chars(bytes, bytes + sizeof bytes, value);
    body += "," + json::quote(key) + ":"; body.append(bytes, converted.ptr);
  };
  if (request.temperature) number("temperature", *request.temperature);
  if (request.top_p) number("top_p", *request.top_p);
  if (request.max_output_tokens) body += "," + json::quote(descriptor.max_output_tokens_member()) + ":" + std::to_string(*request.max_output_tokens);
  body += '}';
  // Validate UTF-8 supplied through typed strings and enforce a bounded outbound document.
  auto validated = json::parse(body);
  if (std::holds_alternative<json::ParseError>(validated)) return bad("encoded request violates JSON limits or encoding");
  return result;
}
} // namespace sp::chat
