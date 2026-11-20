#include "runtime/request_facts.h"
#include "descriptor/policy.h"
#include <algorithm>
#include <variant>

namespace sp::runtime::detail {
namespace {
// The tier the request names, else the policy's default for the model.
std::string service_tier(const descriptor::ValidatedDescriptor& d, const std::string& model,
                         const std::optional<std::string>& requested) {
  if (requested) return *requested;
  const auto defaults = descriptor::effective_defaults(d, model);
  return defaults.service_tier ? std::string(*defaults.service_tier) : std::string();
}
// Whether the service runs the tool: the policy's client tools are executed by the caller.
bool service_runs(const descriptor::ValidatedDescriptor& d, std::string_view type) {
  return !descriptor::tool_facts(d.family_policy(), type).client;
}
RequestFacts common(const descriptor::ValidatedDescriptor& d, const std::string& model,
                    const std::optional<std::string>& tier) {
  RequestFacts facts;
  facts.service_tier = service_tier(d, model, tier);
  return facts;
}
const std::optional<std::string> no_tier;
// Whether a message of the conversation carries an image, in a part or in a tool result's part.
bool carries_image(const std::vector<Message>& messages) {
  const auto image = [](const Media& media) { return media.kind == MediaKind::Image; };
  for (const auto& message : messages)
    for (const auto& part : message.parts) {
      if (const auto* media = std::get_if<Media>(&part)) {
        if (image(*media)) return true;
      } else if (const auto* result = std::get_if<ToolResult>(&part)) {
        for (const auto& content : result->content_parts)
          if (const auto* media = std::get_if<Media>(&content); media && image(*media)) return true;
      }
    }
  return false;
}
} // namespace

RequestFacts request_facts(const descriptor::ValidatedDescriptor& d, const chat::Request& request) {
  auto facts = common(d, request.model, request.service_tier);
  facts.fallback_models = request.models;
  facts.plugins = !request.plugins.empty();
  facts.routing = request.provider;
  facts.image_input = carries_image(request.canonical_messages) ||
                      std::any_of(request.messages.begin(), request.messages.end(), [](const chat::InputMessage& message) {
                        return std::any_of(message.media.begin(), message.media.end(), [](const Media& media) { return media.kind == MediaKind::Image; });
                      });
  return facts;
}
RequestFacts request_facts(const descriptor::ValidatedDescriptor& d, const messages::Request& request) {
  auto facts = common(d, request.model, no_tier);
  facts.routing = request.provider;
  facts.image_input = carries_image(request.messages);
  // A typed tool carries a `type`; a client function does not. Each typed tool has its own cap.
  for (const auto& tool : request.tools)
    if (const auto tool_facts = descriptor::tool_facts(d.family_policy(), tool.type); !tool.type.empty() && !tool_facts.client)
      facts.tools.push_back({{tool.type}, tool.max_uses, "max_uses", tool_facts.admits_max_uses});
  return facts;
}
RequestFacts request_facts(const descriptor::ValidatedDescriptor& d, const responses::Request& request) {
  auto facts = common(d, request.model, request.service_tier);
  facts.plugins = !request.plugins.empty();
  facts.routing = request.provider;
  facts.image_input = request.previous_response_id || carries_image(request.messages) || carries_image(request.previous_response_history);
  // One cap bounds every hosted tool of the request. `max_tool_calls` is a member of every Responses request, so the
  // cap is admitted whatever the policy's `controls` say.
  ToolCap hosted{{}, request.max_tool_calls, "max_tool_calls", true};
  for (const auto& tool : request.hosted_tools) {
    const auto type = responses::tool_type(tool);
    if (service_runs(d, type)) hosted.types.emplace_back(type);
  }
  if (!hosted.types.empty()) facts.tools.push_back(std::move(hosted));
  return facts;
}
RequestFacts request_facts(const descriptor::ValidatedDescriptor& d, const gemini::Request& request) {
  auto facts = common(d, request.model, no_tier);
  facts.image_input = carries_image(request.messages);
  // No cap bounds a built-in tool or an MCP server.
  for (const auto& tool : request.builtin_tools) facts.tools.push_back({{std::string(gemini::tool_type(tool))}, std::nullopt, {}, false});
  for (const auto& server : request.mcp_servers) facts.tools.push_back({{gemini::tool_type(server)}, std::nullopt, {}, false});
  return facts;
}
RequestFacts request_facts(const descriptor::ValidatedDescriptor& d, const interactions::Request& request) {
  auto facts = common(d, request.model, request.service_tier);
  facts.image_input = request.previous_interaction_id || carries_image(request.messages);
  for (const auto& tool : request.builtin_tools) facts.tools.push_back({{std::string(interactions::tool_type(tool))}, std::nullopt, {}, false});
  return facts;
}
CarriedRequest carried_request(const descriptor::ValidatedDescriptor& family, const FamilyRequest& request, const batches::detail::ItemCaps& caps) {
  CarriedRequest out;
  out.custom_id = caps.custom_id;
  out.model = caps.model;
  out.output_tokens = caps.max_output_tokens;
  out.invocations = caps.model_invocation_limit;
  out.facts = std::visit([&](const auto& typed) { return request_facts(family, typed); }, request);
  std::string tier(family.policy()->billing_tier(family.family(), out.facts.service_tier));
  out.facts.service_tier = std::move(tier);
  return out;
}
} // namespace sp::runtime::detail
