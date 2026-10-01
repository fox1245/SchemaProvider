#include "codecs/responses_request.h"
#include "core/native.h"
#include "json/json.h"
#include <map>
#include <set>

namespace sp::responses {
namespace {
constexpr size_t request_limit = 16 << 20;
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
bool reasoning_options(const ReasoningOptions& options) {
  const auto& effort = options.effort;
  const auto& summary = options.summary;
  return (effort == "none" || effort == "minimal" || effort == "low" || effort == "medium" ||
      effort == "high" || effort == "xhigh") &&
      (summary == "auto" || summary == "concise" || summary == "detailed");
}
} // namespace
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](std::string message) -> EncodeResult { return Error{ErrorKind::InvalidRequest, std::move(message)}; };
  auto replay_bad = []() -> EncodeResult { return Error{ErrorKind::ReplayIneligible, "native replay provenance, binding or content mismatch"}; };
  if (descriptor.family() != "openai.responses") return Error{ErrorKind::InvalidConfig, "Responses encoder requires Responses descriptor"};
  if (request.model.empty() || request.messages.empty()) return bad("model and messages are required");
  if (request.messages.size() > 100000) return bad("too many request messages");
  if (request.max_output_tokens && !*request.max_output_tokens) return bad("max_output_tokens must be positive");
  if (request.reasoning && !reasoning_options(*request.reasoning)) return bad("unsupported typed reasoning options");
  // Captured groups never degrade to caller-imported assistant history when their
  // seal or original items are removed. Only plain input and tool results are imports.
  for (const auto& message : request.messages) {
    if (message.native) {
      if (message.role != Role::Assistant || !message.wire_output || !message.wire_output->root().is_array()) return replay_bad();
    } else {
      if (message.role == Role::Assistant || message.wire_output) return replay_bad();
      for (const auto& part : message.parts) {
        if (!std::holds_alternative<Text>(part) && !std::holds_alternative<ToolResult>(part)) return replay_bad();
      }
    }
  }
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, streaming));
  if (!context->history_valid_) return replay_bad();
  if (!context->valid_) return Error{ErrorKind::ResourceLimit, "native binding could not be captured"};
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
    body.raw(",\"store\":false,\"include\":[\"reasoning.encrypted_content\"]");
    if (!request.instructions.empty()) body.raw(",\"instructions\":").quoted(request.instructions);
    if (request.max_output_tokens) body.raw(",").quoted(descriptor.max_output_tokens_member()).raw(":").raw(std::to_string(*request.max_output_tokens));
    if (request.reasoning) body.raw(",\"reasoning\":{\"effort\":").quoted(request.reasoning->effort)
        .raw(",\"summary\":").quoted(request.reasoning->summary).raw("}");
    std::set<std::string_view> tool_names;
    if (!request.tools.empty()) {
      body.raw(",\"tools\":[");
      bool comma = false;
      for (const auto& tool : request.tools) {
        if (!function_name(tool.name) || !tool_names.insert(tool.name).second) return invalid("tools require unique valid function names");
        if (!tool.parameters || !tool.parameters->root().is_object()) return invalid("function parameters must be an object schema");
        if (comma) body.raw(",");
        comma = true;
        body.raw("{\"type\":\"function\",\"name\":").quoted(tool.name).raw(",\"description\":").quoted(tool.description)
            .raw(",\"parameters\":").value(tool.parameters->root(), 3).raw(",\"strict\":").raw(tool.strict ? "true}" : "false}");
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
    for (const auto& message : request.messages) {
      if (message.native) {
        if (!pending.empty()) return invalid("client tool results must precede the next captured assistant group");
        std::map<std::string_view, const ToolCall*> calls;
        for (const auto& part : message.parts) {
          if (const auto* call = std::get_if<ToolCall>(&part)) {
            if (call->kind != ToolCallKind::ClientExecuted || call->wire_type != "function_call") return Error{ErrorKind::Unsupported, "only client function calls authorize tool results"};
            if (call->id.empty() || !tool_names.contains(call->name) || !call->input || !call->input->root().is_object() ||
                !call_ids.insert(call->id).second || !calls.emplace(call->id, call).second) return invalid("captured client call has invalid or duplicate ownership");
            pending.insert(call->id);
          } else if (!std::holds_alternative<Text>(part) && !std::holds_alternative<Refusal>(part) &&
              !std::holds_alternative<Reasoning>(part) && !std::holds_alternative<Opaque>(part)) {
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
          separator(); body.value(item, 2);
        }
        if (!calls.empty()) return invalid("captured function call is missing its output item");
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
            separator(); body.raw("{\"type\":\"function_call_output\",\"call_id\":").quoted(tool_result->tool_use_id)
                .raw(",\"output\":").quoted(tool_result->content).raw("}");
          }
        } else {
          if (role.empty() || !pending.empty()) return invalid("plain input role or pending client tool results are invalid");
          separator(); body.raw("{\"role\":").quoted(role).raw(",\"content\":[");
          bool comma = false;
          for (const auto& part : message.parts) {
            const auto* text = std::get_if<Text>(&part);
            if (!text) return Error{ErrorKind::Unsupported, "plain Responses input supports typed text only"};
            if (comma) body.raw(",");
            comma = true;
            body.raw("{\"type\":\"input_text\",\"text\":").quoted(text->value).raw("}");
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
  json::BoundedWriter measured({request_limit, 64});
  if (auto error = build(measured)) return *error;
  result.body.reserve(measured.size());
  json::BoundedWriter body({measured.size(), 64}, &result.body);
  if (auto error = build(body)) return *error;
  result.context = std::move(context);
  return result;
}
} // namespace sp::responses
