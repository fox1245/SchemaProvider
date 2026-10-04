#include "codecs/messages_request.h"
#include "core/native.h"
#include "json/json.h"
#include "descriptor/policy.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <limits>

namespace sp::messages {
namespace {
std::string lower(std::string_view text) {
  std::string value(text);
  for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return value;
}
std::string_view result_name(std::string_view type) {
  if (type == "web_search_tool_result") return "web_search";
  if (type == "web_fetch_tool_result") return "web_fetch";
  if (type == "code_execution_tool_result") return "code_execution";
  if (type == "bash_code_execution_tool_result") return "bash_code_execution";
  if (type == "text_editor_code_execution_tool_result") return "text_editor_code_execution";
  if (type == "tool_search_tool_result") return "tool_search";
  return {};
}
bool matching_result(std::string_view name, std::string_view type) {
  const auto expected = result_name(type);
  return !expected.empty() && (expected == name || (expected == "tool_search" && (name == "tool_search_tool_regex" || name == "tool_search_tool_bm25")));
}
bool native_part(const Part& part) {
  return !std::holds_alternative<Text>(part) && !std::holds_alternative<ToolResult>(part) &&
      !std::holds_alternative<Refusal>(part) && !std::holds_alternative<Image>(part);
}
} // namespace
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](std::string message) -> EncodeResult { return Error{ErrorKind::InvalidRequest, std::move(message)}; };
  auto replay_bad = []() -> EncodeResult { return Error{ErrorKind::ReplayIneligible, "native replay provenance, binding or content mismatch"}; };
  if (descriptor.family() != "anthropic.messages") return Error{ErrorKind::InvalidConfig, "Messages encoder requires Messages descriptor"};
  if (request.model.empty() || request.messages.empty()) return bad("model and messages are required");
  const auto& resources = descriptor.policy()->resources();
  const auto& family = descriptor.family_policy();
  auto effective = descriptor::effective_defaults(descriptor, request.model);
  if (request.max_tokens) effective.max_output_tokens = request.max_tokens;
  if (request.thinking_budget) effective.thinking_budget = request.thinking_budget;
  if (request.temperature) effective.temperature = request.temperature;
  if (request.top_p) effective.top_p = request.top_p;
  if (request.temperature && descriptor::temperature_forbidden(descriptor, request.model))
    return bad("temperature is prohibited for this model");
  if (request.temperature && (!std::isfinite(*request.temperature) ||
      *request.temperature < family.temperature_range[0] || *request.temperature > family.temperature_range[1]))
    return bad("temperature outside admitted range");
  std::optional<ThinkingMode> thinking_mode = request.thinking_mode;
  if (thinking_mode) {
    switch (*thinking_mode) {
      case ThinkingMode::Manual:
        if (!effective.thinking_budget || !*effective.thinking_budget) return bad("manual thinking requires a positive budget");
        break;
      case ThinkingMode::Adaptive:
      case ThinkingMode::Disabled:
        if (request.thinking_budget) return bad("adaptive and disabled thinking forbid budget");
        effective.thinking_budget.reset();
        break;
      default: return bad("invalid thinking mode");
    }
  } else if (effective.thinking_budget) thinking_mode = ThinkingMode::Manual;
  const bool thinking_enabled = thinking_mode && *thinking_mode != ThinkingMode::Disabled;
  if (thinking_enabled) {
    effective.temperature.reset();
    if (effective.top_p && *effective.top_p < family.thinking_top_p_minimum)
      return bad("thinking top_p outside admitted range");
  }
  if (auto error = descriptor::validate_choices(descriptor, request.model, effective)) return bad(std::move(*error));
  std::string_view effort, ttl, tool_mode;
  if (request.output_effort) switch (*request.output_effort) {
    case OutputEffort::Low: effort = "low"; break;
    case OutputEffort::Medium: effort = "medium"; break;
    case OutputEffort::High: effort = "high"; break;
    case OutputEffort::Max: effort = "max"; break;
    default: return bad("invalid output effort");
  }
  if (request.cache_control && request.cache_control->ttl) switch (*request.cache_control->ttl) {
    case CacheTtl::FiveMinutes: ttl = "5m"; break;
    case CacheTtl::OneHour: ttl = "1h"; break;
    default: return bad("invalid cache TTL");
  }
  if (request.tool_choice) {
    const auto& choice = *request.tool_choice;
    switch (choice.mode) {
      case ToolChoiceMode::Auto: tool_mode = "auto"; break;
      case ToolChoiceMode::Any: tool_mode = "any"; break;
      case ToolChoiceMode::None: tool_mode = "none"; break;
      case ToolChoiceMode::Tool: tool_mode = "tool"; break;
      default: return bad("invalid tool choice");
    }
    if (choice.mode == ToolChoiceMode::Tool) {
      if (choice.name.empty() || std::none_of(request.tools.begin(), request.tools.end(),
          [&](const auto& tool) { return tool.type.empty() && tool.name == choice.name; }))
        return bad("named tool choice requires a declared client tool");
    } else if (!choice.name.empty()) return bad("only named tool choice accepts a name");
    if ((choice.mode == ToolChoiceMode::Any || choice.mode == ToolChoiceMode::Tool) &&
        (request.tools.empty() || thinking_enabled))
      return bad("forced tool choice requires tools and thinking disabled");
    if (choice.mode == ToolChoiceMode::None && choice.disable_parallel_tool_use)
      return bad("none tool choice cannot control parallel use");
  }
  if (request.provider)
    if (auto error = request_controls::validate_routing(descriptor, *request.provider)) return bad(std::move(*error));
  if (request.messages.size() > resources.request_messages || request.tools.size() > resources.request_tools)
    return bad("request count limit exceeded");
  // Check seals before encoding or interpreting the edited contents. Removing a seal
  // never converts native parts or captured tool calls into imported, trusted input.
  bool captured_history = false;
  std::optional<Error> image_error;
  for (size_t i = 0; i < request.messages.size(); ++i) {
    const auto& message = request.messages[i];
    if (message.parts.size() > resources.request_parts) return bad("request part count limit exceeded");
    if (!message.native) for (const auto& part : message.parts) if (native_part(part)) return replay_bad();
    captured_history = captured_history || static_cast<bool>(message.native);
    for (const auto& part : message.parts) if (const auto* image = std::get_if<Image>(&part); image && !image_error) {
      if (message.role != Role::User) image_error = Error{ErrorKind::InvalidRequest, "image inputs require user role"};
      else if (!valid_image(*image, resources.image_decoded_bytes)) image_error = Error{ErrorKind::InvalidRequest, "invalid inline image payload"};
      else if (image->detail != ImageDetail::Auto) image_error = Error{ErrorKind::Unsupported, "Messages images have no detail control"};
    }
  }
  // Invalid ordinary input never reaches hashing. For captured history, retain
  // lineage mismatch precedence even when the edited prefix is no longer valid.
  if (image_error && !captured_history) return *image_error;
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, effective, streaming));
  if (!context->history_valid_) return replay_bad();
  if (!context->valid_) return Error{ErrorKind::ResourceLimit, "native binding could not be captured"};
  if (image_error) return *image_error;
  EncodedRequest result{"POST", std::string(descriptor.path(streaming)), {}, {}, {}};
  std::string_view version = *family.header_version;
  for (const auto& header : descriptor.headers()) {
    auto key = lower(header.first);
    if (key != "content-type" && key != "accept" && key != "anthropic-version") result.headers.push_back(header);
    else if (key == "anthropic-version") version = header.second;
  }
  result.headers.emplace_back("Content-Type", "application/json");
  result.headers.emplace_back("Accept", streaming ? "text/event-stream" : "application/json");
  result.headers.emplace_back("anthropic-version", version);
  auto build = [&](json::BoundedWriter& body) -> std::optional<Error> {
  auto bad = [](std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; };
  body.raw("{").quoted(descriptor.request_model_member()).raw(":").quoted(request.model);
  body.raw(",").quoted(descriptor.max_output_tokens_member()).raw(":").raw(std::to_string(*effective.max_output_tokens));
  body.raw(",").quoted(descriptor.request_stream_member()).raw(streaming ? ":true" : ":false");
  if (!request.system.empty()) body.raw(",\"system\":").quoted(request.system);
  if (thinking_mode) {
    body.raw(",\"thinking\":{\"type\":");
    switch (*thinking_mode) {
      case ThinkingMode::Manual:
        body.quoted("enabled").raw(",\"budget_tokens\":").raw(std::to_string(*effective.thinking_budget)); break;
      case ThinkingMode::Adaptive: body.quoted("adaptive"); break;
      case ThinkingMode::Disabled: body.quoted("disabled"); break;
    }
    body.raw("}");
  }
  if (request.output_effort) body.raw(",\"output_config\":{\"effort\":").quoted(effort).raw("}");
  if (request.cache_control) {
    body.raw(",\"cache_control\":{\"type\":\"ephemeral\"");
    if (!ttl.empty()) body.raw(",\"ttl\":").quoted(ttl);
    body.raw("}");
  }
  if (request.tool_choice) {
    body.raw(",\"tool_choice\":{\"type\":").quoted(tool_mode);
    if (request.tool_choice->mode == ToolChoiceMode::Tool) body.raw(",\"name\":").quoted(request.tool_choice->name);
    if (request.tool_choice->disable_parallel_tool_use)
      body.raw(",\"disable_parallel_tool_use\":").raw(*request.tool_choice->disable_parallel_tool_use ? "true" : "false");
    body.raw("}");
  }
  if (request.provider) { body.raw(",\"provider\":"); request_controls::write_routing(body, *request.provider); }
  auto number = [&](std::string_view key, double value) {
    char bytes[64]; const auto converted = std::to_chars(bytes, bytes + sizeof bytes, value);
    body.raw(",").quoted(key).raw(":").raw(std::string_view(bytes, converted.ptr));
  };
  if (effective.temperature) number("temperature", *effective.temperature);
  if (effective.top_p) number("top_p", *effective.top_p);
  std::set<std::string_view> tool_names;
  if (!request.tools.empty()) {
    body.raw(",\"tools\":[");
    bool comma = false;
    for (const auto& tool : request.tools) {
      if (tool.name.empty() || !tool_names.insert(tool.name).second) return bad("tools require unique nonempty names");
      if (comma) body.raw(",");
      comma = true;
      if (tool.type.empty()) {
        if (!tool.input_schema || !tool.input_schema->root().is_object() || tool.max_uses) return bad("client tools require object schemas and no server fields");
        body.raw("{\"name\":").quoted(tool.name).raw(",\"description\":").quoted(tool.description).raw(",\"input_schema\":").value(tool.input_schema->root(), 3).raw("}");
      } else {
        const auto fact = std::find_if(family.server_tools.begin(), family.server_tools.end(), [&](const auto& value) { return value.type == tool.type; });
        if (fact == family.server_tools.end()) return Error{ErrorKind::Unsupported, "unsupported server tool type"};
        if (tool.name != fact->name || tool.input_schema || !tool.description.empty()) return bad("server tool fields do not match typed schema");
        if (tool.max_uses && (!*tool.max_uses || !fact->max_uses)) return bad("max_uses is unsupported or out of range");
        body.raw("{\"type\":").quoted(tool.type).raw(",\"name\":").quoted(tool.name);
        if (tool.max_uses) body.raw(",\"max_uses\":").raw(std::to_string(*tool.max_uses));
        body.raw("}");
      }
      if (!body.ok()) return bad("request exceeds JSON limits or encoding");
    }
    body.raw("]");
  }
  body.raw(",").quoted(descriptor.request_messages_member()).raw(":[");
  std::set<std::string_view> all_call_ids, client_pending;
  std::map<std::string_view, std::string_view> server_pending;
  bool message_comma = false;
  for (const auto& message : request.messages) {
    if (message.role != Role::User && message.role != Role::Assistant) return Error{ErrorKind::Unsupported, "Messages history supports only user and assistant roles"};
    const bool assistant = message.role == Role::Assistant;
    if (assistant && !client_pending.empty()) return bad("client tool results must immediately follow their assistant message");
    if (!assistant && client_pending.empty() && !server_pending.empty()) return bad("server continuation requires the captured assistant content");
    const bool responding_to_tools = !client_pending.empty();
    const bool server_waiting = !server_pending.empty();
    bool text_seen = false;
    if (message_comma) body.raw(",");
    message_comma = true;
    body.raw(assistant ? "{\"role\":\"assistant\",\"content\":[" : "{\"role\":\"user\",\"content\":[");
    bool part_comma = false;
    for (const auto& part : message.parts) {
      if (part_comma) body.raw(",");
      part_comma = true;
      if (const auto* text = std::get_if<Text>(&part)) {
        if (!assistant && responding_to_tools && server_waiting) return bad("pending server tools require tool-results-only continuation");
        text_seen = true;
        body.raw("{\"type\":\"text\",\"text\":").quoted(text->value).raw("}");
      } else if (const auto* image = std::get_if<Image>(&part)) {
        if (responding_to_tools && server_waiting) return bad("pending server tools require tool-results-only continuation");
        text_seen = true; // Tool results, when present, must precede ordinary input.
        body.raw("{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":")
            .quoted(image->mime).raw(",\"data\":\"").raw(*image->data).raw("\"}}");
      } else if (const auto* thinking = std::get_if<Thinking>(&part)) {
        if (!assistant) return bad("thinking belongs to assistant messages");
        body.raw("{\"type\":\"thinking\",\"thinking\":").quoted(thinking->text);
        if (thinking->signature) body.raw(",\"signature\":").quoted(*thinking->signature);
        body.raw("}");
      } else if (const auto* redacted = std::get_if<RedactedThinking>(&part)) {
        if (!assistant) return bad("redacted thinking belongs to assistant messages");
        body.raw("{\"type\":\"redacted_thinking\",\"data\":").quoted(redacted->data).raw("}");
      } else if (const auto* call = std::get_if<ToolCall>(&part)) {
        if (!assistant || call->id.empty() || call->name.empty() || !call->input || !call->input->root().is_object()) return bad("invalid tool call history");
        if (call->kind == ToolCallKind::ApprovalRequest) return Error{ErrorKind::Unsupported, "approval request replay is unsupported"};
        const bool server = call->kind == ToolCallKind::ServerExecuted;
        const std::string_view type = server ? "server_tool_use" : "tool_use";
        if (call->wire_type != type || !all_call_ids.insert(call->id).second) return bad("tool call type or ownership mismatch");
        if (server) server_pending.emplace(call->id, call->name);
        else client_pending.insert(call->id);
        body.raw("{\"type\":").quoted(type).raw(",\"id\":").quoted(call->id).raw(",\"name\":").quoted(call->name).raw(",\"input\":").value(call->input->root(), 5);
        if (call->wire_metadata) {
          if (!call->wire_metadata->root().is_object()) return bad("tool metadata must be object");
          for (auto member : call->wire_metadata->root().members()) {
            if (member.key == "input" || member.key == "id" || member.key == "name" || member.key == "type") return bad("tool metadata collides with owned fields");
            body.raw(",").quoted(member.key).raw(":").value(member.value, 5);
          }
        }
        body.raw("}");
      } else if (const auto* tool_result = std::get_if<ToolResult>(&part)) {
        if (assistant || text_seen || client_pending.erase(tool_result->tool_use_id) != 1) return bad("tool result lacks immediate unique client ownership");
        body.raw("{\"type\":\"tool_result\",\"tool_use_id\":").quoted(tool_result->tool_use_id).raw(",\"content\":").quoted(tool_result->content).raw(",\"is_error\":").raw(tool_result->is_error ? "true}" : "false}");
      } else if (const auto* server_result = std::get_if<ServerToolResult>(&part)) {
        const auto found = server_pending.find(server_result->tool_use_id);
        if (!assistant || found == server_pending.end() || !matching_result(found->second, server_result->wire_type) || !server_result->content) return bad("server result lacks unique server ownership");
        const auto raw = server_result->content->root();
        if (!raw.is_object() || !raw.get("type").is_string() || raw.get("type").as_string() != server_result->wire_type || !raw.get("tool_use_id").is_string() || raw.get("tool_use_id").as_string() != server_result->tool_use_id) return bad("server result metadata mismatch");
        server_pending.erase(found);
        body.value(raw, 4);
      } else return Error{ErrorKind::Unsupported, "part cannot be replayed as Messages input"};
      if (!body.ok()) return bad("request exceeds JSON limits or encoding");
    }
    body.raw("]}");
    if (!assistant && !client_pending.empty()) return bad("all client tool calls need immediate results");
    if (assistant && !server_pending.empty() && (!message.native || (message.native->stop_ != StopKind::PauseTurn && message.native->stop_ != StopKind::ToolUse))) return bad("unfinished server call lacks continuation stop");
    if (assistant && message.native && message.native->stop_ == StopKind::PauseTurn && !client_pending.empty()) return bad("pause continuation cannot await client tools");
  }
  if (!client_pending.empty()) return bad("client tool results are required before dispatch");
  body.raw("]}");
  if (!body.ok()) return bad("request exceeds JSON limits or encoding");
  return std::nullopt;
  };
  // Measure the entire aggregate before allocating its encoded representation.
  // Both passes use the same writer and syntax, including all document leaves.
  json::BoundedWriter measured({resources.request_bytes, resources.json_depth});
  if (auto error = build(measured)) return *error;
  result.body.reserve(measured.size());
  json::BoundedWriter body({measured.size(), resources.json_depth}, &result.body);
  if (auto error = build(body)) return *error;
  result.context = std::move(context);
  result.max_output_tokens = effective.max_output_tokens;
  for (const auto& tool : request.tools) if (!tool.type.empty()) {
    if (!tool.max_uses || !result.model_invocation_limit ||
        *tool.max_uses > std::numeric_limits<std::uint64_t>::max() - *result.model_invocation_limit) {
      result.model_invocation_limit.reset();
      break;
    }
    *result.model_invocation_limit += *tool.max_uses;
  }
  return result;
}
} // namespace sp::messages
