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
// The service's enumerations (sizes, qualities, environments, ...) are sent as given: the SDK checks their
// syntax, and the service decides which values a model accepts.
struct ApproximateLocation { std::optional<std::string> city, country, region, timezone; };
struct WebSearchTool {
  std::vector<std::string> allowed_domains;  // filters.allowed_domains
  std::optional<ApproximateLocation> user_location;
  std::optional<std::string> search_context_size;
  std::optional<bool> external_web_access;
};
struct ImageGenerationTool {
  std::optional<std::string> size, quality, output_format;
  std::optional<std::string> model, action, background, moderation, input_fidelity;
  std::optional<std::uint32_t> output_compression, partial_images;
  // input_image_mask, for an edit.
  std::optional<std::string> mask_file_id, mask_image_url;
};
struct FileSearchTool { std::vector<std::string> vector_store_ids; std::optional<std::uint32_t> max_num_results; };
struct ToolSearchTool {};
struct SkillReference { std::string skill_id; };
struct ContainerAutoEnvironment { std::vector<SkillReference> skills; };
struct ShellTool { ContainerAutoEnvironment environment; };
// A secret injected for one allow-listed domain. The value is sent but never bound to replay.
struct DomainSecret { std::string domain, name, value; };
struct ContainerNetworkPolicy {
  bool disabled = false;
  std::vector<std::string> allowed_domains;
  std::vector<DomainSecret> domain_secrets;
};
// code_interpreter: an existing container by id, or else an automatic one with these settings.
struct CodeInterpreterTool {
  std::optional<std::string> container_id;
  std::vector<std::string> file_ids;
  std::optional<std::string> memory_limit;  // "1g", "4g", ...
  std::optional<ContainerNetworkPolicy> network_policy;
};
struct McpToolFilter { std::vector<std::string> tool_names; std::optional<bool> read_only; };
// A remote MCP server, or a connector or tunnel, whose tools the service calls. Exactly one of server_url,
// connector_id and tunnel_id. The authorization and header values are credentials: they are sent but never bound.
struct McpTool {
  std::string server_label;
  std::optional<std::string> server_url, connector_id, tunnel_id;
  std::optional<std::string> authorization;
  std::vector<std::pair<std::string, std::string>> headers;
  std::optional<McpToolFilter> allowed_tools;
  // "always" or "never"; or else the filters of tools that always or never need approval.
  std::optional<std::string> require_approval;
  std::optional<McpToolFilter> approval_always, approval_never;
  std::optional<std::string> server_description;
  std::optional<bool> defer_loading;
};
// The model acts on a computer through computer_call items: the caller performs the actions and answers each with
// a ToolResult holding a screenshot. `computer` has no settings; `computer_use_preview` names the display.
struct ComputerTool {};
struct ComputerUsePreviewTool { std::string environment; std::uint32_t display_width = 0, display_height = 0; };
// A server tool OpenRouter runs (openrouter:web_search, openrouter:datetime, openrouter:image_generation, ...),
// sent with its parameters object as given. Declared OpenRouter origins only.
struct OpenRouterServerTool { std::string type; std::shared_ptr<const json::Document> parameters; };
// New alternatives go at the end: the configuration digest binds the index.
using HostedTool = std::variant<WebSearchTool, ImageGenerationTool, FileSearchTool, ToolSearchTool, ShellTool,
                                CodeInterpreterTool, McpTool, ComputerTool, ComputerUsePreviewTool, OpenRouterServerTool>;
// The answer to an mcp_approval_request item of an earlier response.
struct McpApproval { std::string request_id; bool approve = false; std::optional<std::string> reason; };
// OpenRouter image output: Image in modalities, with Text when text is wanted too.
enum class OutputModality { Text, Image };
enum class Verbosity { Low, Medium, High };
enum class Truncation { Disabled, Auto };
enum class Include {
  ReasoningEncryptedContent, WebSearchSources, FileSearchResults,
  MessageOutputTextLogprobs, ComputerCallOutputImageUrl, CodeInterpreterCallOutputs
};
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
  // Bounded provider-held cursor; messages contains only new input. A cursor
  // alone admits text/image input, not client tool-result or NativeReplay authority.
  std::optional<std::string> previous_response_id;
  // To authorize tool results, supply the original prefix plus the authentic
  // terminal response, or one authentic in-process cursor-produced response.
  // The last response id must equal the cursor. This history is never sent.
  std::vector<sp::Message> previous_response_history;
  std::optional<bool> parallel_tool_calls;
  std::optional<Verbosity> verbosity;
  std::optional<Truncation> truncation;
  // Absent keeps reasoning.encrypted_content; an explicit empty vector sends [].
  std::optional<std::vector<Include>> include;
  // Declared OpenRouter origins only.
  std::vector<OutputModality> modalities;
  std::optional<sp::OpenRouterImageConfig> image_config;
  std::vector<sp::OpenRouterPlugin> plugins;
  // Sent as mcp_approval_response input items after the messages. Each answers an mcp_approval_request in the
  // captured history, or of the response that previous_response_id continues.
  std::vector<McpApproval> mcp_approvals;

  static Request simple(std::string model_name, std::string user_prompt,
                        std::optional<uint64_t> max_tokens = std::nullopt,
                        std::string system_instructions = {}) {
    Request request;
    request.model = std::move(model_name);
    request.instructions = std::move(system_instructions);
    request.messages.push_back(sp::Message::user_text(std::move(user_prompt)));
    request.max_output_tokens = max_tokens;
    return request;
  }
  static Request simple(std::string model_name, std::vector<sp::Message> history,
                        std::optional<uint64_t> max_tokens = std::nullopt,
                        std::string system_instructions = {}) {
    Request request;
    request.model = std::move(model_name);
    request.instructions = std::move(system_instructions);
    request.messages = std::move(history);
    request.max_output_tokens = max_tokens;
    return request;
  }
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
