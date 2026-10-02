#include "codecs/gemini_request.h"
#include "core/native.h"
#include "json/json.h"
#include <map>
#include <set>

namespace sp::gemini {
namespace {
constexpr size_t request_limit = 16 << 20;
bool name_valid(std::string_view name) {
  if (name.empty() || name.size() > 128) return false;
  for (const auto c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}
}
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](std::string message) -> EncodeResult { return Error{ErrorKind::InvalidRequest, std::move(message)}; };
  auto replay_bad = []() -> EncodeResult { return Error{ErrorKind::ReplayIneligible, "native replay provenance, binding or content mismatch"}; };
  if (descriptor.family() != "google.generate") return Error{ErrorKind::InvalidConfig, "Gemini encoder requires generate descriptor"};
  if (request.model.empty() || request.model.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._") != std::string::npos) return bad("literal model name required");
  const auto base = "/v1beta/models/" + request.model;
  if (descriptor.path(false) != base + ":generateContent" || descriptor.path(true) != base + ":streamGenerateContent?alt=sse") return Error{ErrorKind::InvalidConfig, "Gemini descriptor paths must target the same literal model"};
  if (request.messages.empty() || request.messages.size() > 100000 || !request.max_output_tokens) return bad("messages and positive output cap required");
  if (request.thinking_budget && *request.thinking_budget > request.max_output_tokens) return bad("thinking budget exceeds output cap");
  for (const auto& message : request.messages) {
    if (!message.native && (message.role == Role::Assistant || message.wire_output)) return replay_bad();
    if (!message.native) for (const auto& p : message.parts)
      if (!std::holds_alternative<Text>(p) && !std::holds_alternative<Image>(p) && !std::holds_alternative<ToolResult>(p)) return replay_bad();
  }
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, streaming));
  if (!context->history_valid_) return replay_bad();
  if (!context->valid_) return Error{ErrorKind::ResourceLimit, "native binding could not be captured"};
  EncodedRequest result{"POST", std::string(descriptor.path(streaming)), {}, {}, {}};
  for (const auto& header : descriptor.headers()) {
    std::string key = header.first;
    for (auto& c : key) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (key != "content-type" && key != "accept") result.headers.push_back(header);
  }
  result.headers.emplace_back("Content-Type", "application/json");
  result.headers.emplace_back("Accept", streaming ? "text/event-stream" : "application/json");
  auto build = [&](json::BoundedWriter& w) -> std::optional<Error> {
    auto invalid = [](const char* text) { return Error{ErrorKind::InvalidRequest, text}; };
    std::set<std::string_view> tool_names;
    w.raw("{\"generationConfig\":{\"maxOutputTokens\":").raw(std::to_string(request.max_output_tokens));
    w.raw(",\"thinkingConfig\":{\"includeThoughts\":").raw(request.include_thoughts ? "true" : "false");
    if (request.thinking_budget) w.raw(",\"thinkingBudget\":").raw(std::to_string(*request.thinking_budget));
    w.raw("}}");
    if (!request.system.empty()) w.raw(",\"systemInstruction\":{\"parts\":[{\"text\":").quoted(request.system).raw("}]}");
    if (!request.tools.empty()) {
      w.raw(",\"tools\":[{\"functionDeclarations\":[");
      bool comma = false;
      for (const auto& tool : request.tools) {
        if (!name_valid(tool.name) || !tool_names.insert(tool.name).second || !tool.parameters || !tool.parameters->root().is_object()) return invalid("client functions require unique names and object schemas");
        if (comma) w.raw(",");
        comma = true;
        w.raw("{\"name\":").quoted(tool.name).raw(",\"description\":").quoted(tool.description).raw(",\"parametersJsonSchema\":").value(tool.parameters->root(), 4).raw("}");
      }
      w.raw("]}]");
    }
    if (request.required_tool) {
      if (!tool_names.contains(*request.required_tool)) return invalid("required function must be declared");
      w.raw(",\"toolConfig\":{\"functionCallingConfig\":{\"mode\":\"ANY\",\"allowedFunctionNames\":[").quoted(*request.required_tool).raw("]}}");
    }
    struct Pending { std::string_view name; bool wire_id; };
    std::map<std::string_view, Pending> pending;
    std::set<std::string_view> all_calls;
    w.raw(",\"contents\":[");
    bool comma = false;
    for (const auto& m : request.messages) {
      if (m.parts.empty()) return invalid("empty content message");
      if (comma) w.raw(",");
      comma = true;
      if (m.role == Role::Assistant) {
        if (!pending.empty()) return invalid("all function results must precede the next model message");
        if (!m.native || !m.wire_output || !m.wire_output->root().is_array()) return Error{ErrorKind::ReplayIneligible, "native parts array required"};
        for (const auto& part : m.parts) if (auto call = std::get_if<ToolCall>(&part)) {
          if (call->kind != ToolCallKind::ClientExecuted || !tool_names.contains(call->name) || !all_calls.insert(call->id).second || !call->wire_metadata) return invalid("function ownership mismatch");
          const auto fc = call->wire_metadata->root().get("functionCall");
          pending.emplace(call->id, Pending{call->name, fc.get("id").valid()});
        }
        w.raw("{\"role\":\"model\",\"parts\":").value(m.wire_output->root(), 3).raw("}");
      } else if (m.role == Role::User || m.role == Role::Tool) {
        const bool results_only = !pending.empty();
        if (m.role == Role::Tool && !results_only) return invalid("tool result lacks pending function");
        w.raw("{\"role\":\"user\",\"parts\":[");
        bool pc = false;
        for (const auto& p : m.parts) {
          if (pc) w.raw(",");
          pc = true;
          if (auto text = std::get_if<Text>(&p)) {
            if (results_only || m.role == Role::Tool) return invalid("function results must be immediate and complete");
            w.raw("{\"text\":").quoted(text->value).raw("}");
          } else if (auto image = std::get_if<Image>(&p)) {
            if (results_only || m.role == Role::Tool || !valid_image(*image)) return invalid("invalid inline image or image role");
            w.raw("{\"inlineData\":{\"mimeType\":").quoted(image->mime).raw(",\"data\":\"").raw(*image->data).raw("\"}}");
          } else if (auto tr = std::get_if<ToolResult>(&p)) {
            auto found = pending.find(tr->tool_use_id);
            if (found == pending.end()) return invalid("function result lacks unique client ownership");
            auto parsed = json::parse(tr->content, {request_limit, 64});
            auto doc = std::get_if<json::Document>(&parsed);
            if (!doc || !doc->root().is_object()) return invalid("function result must be JSON object");
            w.raw("{\"functionResponse\":{\"name\":").quoted(found->second.name);
            if (found->second.wire_id) w.raw(",\"id\":").quoted(tr->tool_use_id);
            w.raw(",\"response\":");
            if (tr->is_error) w.raw("{\"error\":").value(doc->root(), 6).raw("}");
            else w.value(doc->root(), 5);
            w.raw("}}"); pending.erase(found);
          } else return Error{ErrorKind::Unsupported, "unsupported imported Gemini part"};
        }
        w.raw("]}");
        if (results_only && !pending.empty()) return invalid("all function results must appear together");
      } else return Error{ErrorKind::Unsupported, "Gemini supports user, tool and captured model messages"};
      if (!w.ok()) return invalid("request exceeds JSON limits or encoding");
    }
    if (!pending.empty()) return invalid("function results required before dispatch");
    w.raw("]}");
    if (!w.ok()) return invalid("request exceeds JSON limits or encoding");
    return {};
  };
  json::BoundedWriter measured({request_limit, 64});
  if (auto error = build(measured)) return *error;
  result.body.reserve(measured.size());
  json::BoundedWriter writer({measured.size(), 64}, &result.body);
  if (auto error = build(writer)) return *error;
  result.context = std::move(context);
  return result;
}
} // namespace sp::gemini
