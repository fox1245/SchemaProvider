#include "codecs/gemini_request.h"
#include "core/native.h"
#include "json/json.h"
#include "descriptor/policy.h"
#include <map>
#include <set>
#include <charconv>

namespace sp::gemini {
namespace {
bool name_valid(std::string_view name) {
  if (name.empty() || name.size() > 128) return false;
  for (const auto c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}
std::string_view thinking_level_name(ThinkingLevel level) {
  switch (level) {
    case ThinkingLevel::Minimal: return "minimal";
    case ThinkingLevel::Low: return "low";
    case ThinkingLevel::Medium: return "medium";
    case ThinkingLevel::High: return "high";
  }
  return {};
}
std::string_view safety_category_name(SafetyCategory category) {
  switch (category) {
    case SafetyCategory::Harassment: return "HARM_CATEGORY_HARASSMENT";
    case SafetyCategory::HateSpeech: return "HARM_CATEGORY_HATE_SPEECH";
    case SafetyCategory::SexuallyExplicit: return "HARM_CATEGORY_SEXUALLY_EXPLICIT";
    case SafetyCategory::DangerousContent: return "HARM_CATEGORY_DANGEROUS_CONTENT";
    case SafetyCategory::CivicIntegrity: return "HARM_CATEGORY_CIVIC_INTEGRITY";
  }
  return {};
}
std::string_view safety_threshold_name(SafetyThreshold threshold) {
  switch (threshold) {
    case SafetyThreshold::BlockNone: return "BLOCK_NONE";
    case SafetyThreshold::BlockOnlyHigh: return "BLOCK_ONLY_HIGH";
    case SafetyThreshold::BlockMediumAndAbove: return "BLOCK_MEDIUM_AND_ABOVE";
    case SafetyThreshold::BlockLowAndAbove: return "BLOCK_LOW_AND_ABOVE";
    case SafetyThreshold::Off: return "OFF";
  }
  return {};
}
std::string_view tool_choice_name(ToolChoiceMode mode) {
  switch (mode) {
    case ToolChoiceMode::Auto: return "AUTO";
    case ToolChoiceMode::Any: return "ANY";
    case ToolChoiceMode::None: return "NONE";
    case ToolChoiceMode::Validated: return "VALIDATED";
  }
  return {};
}
}
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](std::string message) -> EncodeResult { return Error{ErrorKind::InvalidRequest, std::move(message)}; };
  auto replay_bad = []() -> EncodeResult { return Error{ErrorKind::ReplayIneligible, "native replay provenance, binding or content mismatch"}; };
  if (descriptor.family() != "google.generate") return Error{ErrorKind::InvalidConfig, "Gemini encoder requires generate descriptor"};
  if (request.model.empty() || request.model.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._") != std::string::npos) return bad("literal model name required");
  const auto base = "/v1beta/models/" + request.model;
  if (descriptor.path(false) != base + ":generateContent" || descriptor.path(true) != base + ":streamGenerateContent?alt=sse") return Error{ErrorKind::InvalidConfig, "Gemini descriptor paths must target the same literal model"};
  const auto& resources = descriptor.policy()->resources();
  auto effective = descriptor::effective_defaults(descriptor, request.model);
  if (request.max_output_tokens) effective.max_output_tokens = request.max_output_tokens;
  if (request.thinking_budget) effective.thinking_budget = request.thinking_budget;
  if (request.include_thoughts) effective.include_thoughts = request.include_thoughts;
  if (request.history_mode != HistoryMode::NativeOnly && request.history_mode != HistoryMode::PortableForeign)
    return bad("invalid history mode");
  if (request.thinking_level) {
    const auto level = thinking_level_name(*request.thinking_level);
    if (level.empty()) return bad("invalid thinking level");
    if (request.thinking_budget) return bad("thinking level and budget are mutually exclusive");
    effective.thinking_budget.reset();
    effective.thinking_level = level;
  }
  if (request.temperature) effective.temperature = request.temperature;
  unsigned safety_categories = 0;
  for (const auto& setting : request.safety_settings) {
    if (safety_category_name(setting.category).empty() || safety_threshold_name(setting.threshold).empty())
      return bad("safety settings require valid unique categories and thresholds");
    const auto category = 1U << static_cast<unsigned>(setting.category);
    if (safety_categories & category) return bad("safety settings require valid unique categories and thresholds");
    safety_categories |= category;
  }
  if (request.tool_choice) {
    if (request.required_tool) return bad("tool choice and required function are mutually exclusive");
    if (tool_choice_name(request.tool_choice->mode).empty()) return bad("invalid tool choice mode");
    if (!request.tool_choice->allowed_function_names.empty() &&
        request.tool_choice->mode != ToolChoiceMode::Any && request.tool_choice->mode != ToolChoiceMode::Validated)
      return bad("allowed functions require ANY or VALIDATED mode");
  }
  if (auto error = descriptor::validate_choices(descriptor, request.model, effective)) return bad(std::move(*error));
  if (request.messages.empty() || request.messages.size() > resources.request_messages || request.tools.size() > resources.request_tools)
    return bad("request count limit exceeded");
  for (const auto& message : request.messages) {
    if (message.parts.size() > resources.request_parts) return bad("request part count limit exceeded");
    if (!message.native) {
      if (message.wire_output) return replay_bad();
      if (message.role == Role::Assistant) {
        if (request.history_mode != HistoryMode::PortableForeign) return replay_bad();
        for (const auto& part : message.parts) {
          if (std::holds_alternative<Text>(part)) continue;
          const auto* call = std::get_if<ToolCall>(&part);
          if (!call || call->kind != ToolCallKind::ClientExecuted || !call->wire_type.empty() || call->wire_metadata)
            return replay_bad();
        }
      } else for (const auto& part : message.parts)
        if (!std::holds_alternative<Text>(part) && !std::holds_alternative<Image>(part) && !std::holds_alternative<ToolResult>(part))
          return replay_bad();
    }
  }
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, effective, streaming));
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
    w.raw("{\"generationConfig\":{");
    bool generation_comma = false;
    if (effective.max_output_tokens) {
      w.quoted(descriptor.max_output_tokens_member()).raw(":").raw(std::to_string(*effective.max_output_tokens));
      generation_comma = true;
    }
    if (effective.temperature) {
      if (generation_comma) w.raw(",");
      char bytes[64]; const auto converted = std::to_chars(bytes, bytes + sizeof bytes, *effective.temperature);
      w.raw("\"temperature\":").raw(std::string_view(bytes, converted.ptr));
      generation_comma = true;
    }
    if (effective.include_thoughts || effective.thinking_budget || effective.thinking_level) {
      if (generation_comma) w.raw(",");
      w.raw("\"thinkingConfig\":{");
      if (effective.include_thoughts) w.raw("\"includeThoughts\":").raw(*effective.include_thoughts ? "true" : "false");
      if (effective.thinking_budget) {
        if (effective.include_thoughts) w.raw(",");
        w.raw("\"thinkingBudget\":").raw(std::to_string(*effective.thinking_budget));
      }
      if (effective.thinking_level) {
        if (effective.include_thoughts || effective.thinking_budget) w.raw(",");
        w.raw("\"thinkingLevel\":").quoted(*effective.thinking_level);
      }
      w.raw("}");
    }
    w.raw("}");
    if (!request.safety_settings.empty()) {
      w.raw(",\"safetySettings\":[");
      bool comma = false;
      for (const auto& setting : request.safety_settings) {
        if (comma) w.raw(",");
        comma = true;
        w.raw("{\"category\":").quoted(safety_category_name(setting.category))
            .raw(",\"threshold\":").quoted(safety_threshold_name(setting.threshold)).raw("}");
      }
      w.raw("]");
    }
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
    if (request.tool_choice) {
      const auto& choice = *request.tool_choice;
      if (tool_names.empty() && choice.mode != ToolChoiceMode::None) return invalid("function choice requires declared functions");
      std::set<std::string_view> allowed;
      w.raw(",\"toolConfig\":{\"functionCallingConfig\":{\"mode\":").quoted(tool_choice_name(choice.mode));
      if (!choice.allowed_function_names.empty()) {
        w.raw(",\"allowedFunctionNames\":[");
        bool comma = false;
        for (const auto& name : choice.allowed_function_names) {
          if (!name_valid(name) || !tool_names.contains(name) || !allowed.insert(name).second)
            return invalid("allowed functions require unique declared names");
          if (comma) w.raw(",");
          comma = true;
          w.quoted(name);
        }
        w.raw("]");
      }
      w.raw("}}");
    }
    struct Pending { std::string_view name; bool wire_id; };
    std::map<std::string_view, Pending> pending;
    std::set<std::string_view> all_calls;
    w.raw(",").quoted(descriptor.request_messages_member()).raw(":[");
    bool comma = false;
    for (const auto& m : request.messages) {
      if (m.parts.empty()) return invalid("empty content message");
      if (comma) w.raw(",");
      comma = true;
      if (m.role == Role::Assistant) {
        if (!pending.empty()) return invalid("all function results must precede the next model message");
        if (m.native) {
          if (!m.wire_output || !m.wire_output->root().is_array()) return Error{ErrorKind::ReplayIneligible, "native parts array required"};
          for (const auto& part : m.parts) if (auto call = std::get_if<ToolCall>(&part)) {
            if (call->kind != ToolCallKind::ClientExecuted || !tool_names.contains(call->name) || !all_calls.insert(call->id).second || !call->wire_metadata) return invalid("function ownership mismatch");
            const auto fc = call->wire_metadata->root().get("functionCall");
            pending.emplace(call->id, Pending{call->name, fc.get("id").valid()});
          }
          w.raw("{\"role\":\"model\",\"parts\":").value(m.wire_output->root(), 3).raw("}");
        } else {
          w.raw("{\"role\":\"model\",\"parts\":[");
          bool pc = false, first_call = true;
          for (const auto& part : m.parts) {
            if (pc) w.raw(",");
            pc = true;
            if (const auto* text = std::get_if<Text>(&part)) w.raw("{\"text\":").quoted(text->value).raw("}");
            else if (const auto* call = std::get_if<ToolCall>(&part)) {
              if (call->id.empty() || !tool_names.contains(call->name) || !all_calls.insert(call->id).second ||
                  !call->input || !call->input->root().is_object()) return invalid("portable function requires unique identity, declared name and object arguments");
              pending.emplace(call->id, Pending{call->name, true});
              w.raw("{\"functionCall\":{\"id\":").quoted(call->id).raw(",\"name\":").quoted(call->name)
                  .raw(",\"args\":").value(call->input->root(), 6).raw("}");
              // Google's explicit imported-history bypass applies only to the
              // first foreign function call, never native or text-only parts.
              if (first_call) w.raw(",\"thoughtSignature\":\"skip_thought_signature_validator\"");
              first_call = false;
              w.raw("}");
            } else return Error{ErrorKind::ReplayIneligible, "portable assistant parts must be text or client functions"};
          }
          w.raw("]}");
        }
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
            if (results_only || m.role == Role::Tool || !valid_image(*image, resources.image_decoded_bytes)) return invalid("invalid inline image or image role");
            w.raw("{\"inlineData\":{\"mimeType\":").quoted(image->mime).raw(",\"data\":\"").raw(*image->data).raw("\"}}");
          } else if (auto tr = std::get_if<ToolResult>(&p)) {
            auto found = pending.find(tr->tool_use_id);
            if (found == pending.end()) return invalid("function result lacks unique client ownership");
            auto parsed = json::parse(tr->content, {resources.request_bytes, resources.json_depth});
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
  json::BoundedWriter measured({resources.request_bytes, resources.json_depth});
  if (auto error = build(measured)) return *error;
  result.body.reserve(measured.size());
  json::BoundedWriter writer({measured.size(), resources.json_depth}, &result.body);
  if (auto error = build(writer)) return *error;
  result.context = std::move(context);
  result.max_output_tokens = effective.max_output_tokens;
  return result;
}
} // namespace sp::gemini
