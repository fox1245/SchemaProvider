#include "runtime/request_facts.h"
#include "descriptor/policy.h"

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
} // namespace

RequestFacts request_facts(const descriptor::ValidatedDescriptor& d, const chat::Request& request) {
  auto facts = common(d, request.model, request.service_tier);
  facts.fallback_models = request.models;
  facts.plugins = !request.plugins.empty();
  facts.routing = request.provider;
  return facts;
}
RequestFacts request_facts(const descriptor::ValidatedDescriptor& d, const messages::Request& request) {
  auto facts = common(d, request.model, no_tier);
  facts.routing = request.provider;
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
  // No cap bounds a built-in tool or an MCP server.
  for (const auto& tool : request.builtin_tools) facts.tools.push_back({{std::string(gemini::tool_type(tool))}, std::nullopt, {}, false});
  for (const auto& server : request.mcp_servers) facts.tools.push_back({{gemini::tool_type(server)}, std::nullopt, {}, false});
  return facts;
}
RequestFacts request_facts(const descriptor::ValidatedDescriptor& d, const interactions::Request& request) {
  auto facts = common(d, request.model, request.service_tier);
  for (const auto& tool : request.builtin_tools) facts.tools.push_back({{std::string(interactions::tool_type(tool))}, std::nullopt, {}, false});
  return facts;
}
} // namespace sp::runtime::detail
