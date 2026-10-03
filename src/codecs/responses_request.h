#pragma once
#include "core/value.h"
#include "core/request_controls.h"
#include "descriptor/descriptor.h"
#include <utility>

namespace sp::responses {
struct ToolDefinition {
  std::string name, description;
  std::shared_ptr<const json::Document> parameters;
  std::optional<bool> strict;
  std::optional<bool> defer_loading;
};
struct ReasoningOptions {
  std::optional<std::string> effort, summary;
};
struct WebSearchTool {};
struct ImageGenerationTool { std::optional<std::string> size, quality; };
struct FileSearchTool { std::vector<std::string> vector_store_ids; };
struct ToolSearchTool {};
struct SkillReference { std::string skill_id; };
struct ContainerAutoEnvironment { std::vector<SkillReference> skills; };
struct ShellTool { ContainerAutoEnvironment environment; };
using HostedTool = std::variant<WebSearchTool, ImageGenerationTool, FileSearchTool, ToolSearchTool, ShellTool>;
struct Request {
  std::string model, account_scope, instructions;
  std::vector<sp::Message> messages;
  std::vector<ToolDefinition> tools;
  std::optional<uint64_t> max_output_tokens;
  std::optional<ReasoningOptions> reasoning;
  std::optional<std::string> required_tool;
  std::optional<std::string> service_tier;
  std::optional<double> temperature, top_p;
  std::optional<bool> store;
  std::optional<sp::ResponseFormat> response_format;
  std::optional<sp::OpenRouterRouting> provider;
  std::vector<HostedTool> hosted_tools;
  std::optional<std::uint64_t> max_tool_calls;
};
struct EncodedRequest {
  std::string method, path;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::shared_ptr<const NativeContext> context;
  std::optional<std::uint64_t> max_output_tokens{};
  std::optional<std::uint64_t> model_invocation_limit{1};
};
using EncodeResult = std::variant<EncodedRequest, Error>;
EncodeResult encode(const descriptor::ValidatedDescriptor&, const Request&, bool streaming);
} // namespace sp::responses
