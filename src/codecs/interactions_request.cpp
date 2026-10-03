#include "codecs/interactions_request.h"
#include "core/native.h"
#include "core/image.h"
#include "json/json.h"
#include "descriptor/policy.h"
#include <limits>
#include <map>
#include <set>

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
  for (const auto& message : request.messages) {
    if (message.parts.size() > resources.request_parts) return bad("request part count limit exceeded");
    if (message.native) {
      if (message.role != Role::Assistant || !message.wire_output || !message.wire_output->root().is_array()) return replay_bad();
    } else {
      if (message.role == Role::Assistant || message.wire_output) return replay_bad();
      for (const auto& part : message.parts) if (!std::holds_alternative<Text>(part) &&
          !std::holds_alternative<Image>(part) && !std::holds_alternative<ToolResult>(part)) return replay_bad();
    }
  }
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, effective, streaming));
  if (!context->history_valid_) return replay_bad();
  if (!context->valid_) return Error{ErrorKind::ResourceLimit, "native binding could not be captured"};
  EncodedRequest result{"POST", std::string(descriptor.path(streaming)), {}, {}, {}};
  for (const auto& header : descriptor.headers()) {
    auto key = lower(header.first);
    if (key != "accept" && key != "content-type") result.headers.push_back(header);
  }
  result.headers.emplace_back("Content-Type", "application/json");
  result.headers.emplace_back("Accept", streaming ? "text/event-stream" : "application/json");
  auto build = [&](json::BoundedWriter& body) -> std::optional<Error> {
    auto invalid = [](std::string label) { return Error{ErrorKind::InvalidRequest, std::move(label)}; };
    body.raw("{").quoted(descriptor.request_model_member()).raw(":").quoted(request.model)
        .raw(",\"store\":false,").quoted(descriptor.request_stream_member()).raw(":").raw(streaming ? "true" : "false");
    if (effective.service_tier) body.raw(",\"service_tier\":").quoted(*effective.service_tier);
    if (!request.system.empty()) body.raw(",\"system_instruction\":").quoted(request.system);
    std::set<std::string_view> names;
    if (!request.tools.empty()) {
      body.raw(",\"tools\":["); bool comma = false;
      for (const auto& tool : request.tools) {
        if (!function_name(tool.name) || !names.insert(tool.name).second || !tool.parameters ||
            !tool.parameters->root().is_object()) return invalid("unique valid functions and object parameters required");
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
          } else if (!std::holds_alternative<Text>(part) && !std::holds_alternative<Thought>(part))
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
                .raw(",\"name\":").quoted(found->second).raw(",\"result\":").quoted(tool->content)
                .raw(",\"is_error\":").raw(tool->is_error ? "true}" : "false}");
            pending.erase(found);
          }
        } else {
          if (message.role != Role::User || !pending.empty()) return invalid("plain input requires user role and resolved calls");
          separator(); body.raw("{\"type\":\"user_input\",\"content\":["); bool content_comma = false;
          for (const auto& part : message.parts) {
            if (content_comma) body.raw(",");
            content_comma = true;
            if (auto text = std::get_if<Text>(&part)) body.raw("{\"type\":\"text\",\"text\":").quoted(text->value).raw("}");
            else if (auto image = std::get_if<Image>(&part)) {
              if (!valid_image(*image, resources.image_decoded_bytes)) return invalid("invalid typed image");
              body.raw("{\"type\":\"image\",\"mime_type\":").quoted(image->mime).raw(",\"data\":").quoted(*image->data).raw("}");
            } else return Error{ErrorKind::Unsupported, "unsupported input content"};
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
