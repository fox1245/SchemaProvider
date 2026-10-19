#pragma once
#include "core/value.h"
#include "descriptor/descriptor.h"
#include "core/request_controls.h"
#include <utility>

namespace sp::messages {
// web_search's approximate user location.
struct UserLocation { std::optional<std::string> city, region, country, timezone; };
struct ToolDefinition {
  std::string name, description;
  std::shared_ptr<const json::Document> input_schema;
  std::string type;
  std::optional<uint64_t> max_uses;
  // Server tool options. Each is sent only when set, and the service judges whether the tool's version takes it:
  // allowed_callers (`direct`, which the _20260318 web tools need on models without programmatic tool calling),
  // the domain filters and user_location (web_search, web_fetch), citations and max_content_tokens (web_fetch).
  std::vector<std::string> allowed_callers, allowed_domains, blocked_domains;
  std::optional<UserLocation> user_location;
  std::optional<bool> citations;
  std::optional<uint64_t> max_content_tokens;
  // A typed tool's other members (an advisor's model, a toolset's configs), sent as given after the typed ones. The
  // policy's fact for the tool type declares their schema (#32); nothing it does not name is sent.
  std::shared_ptr<const json::Document> options{};
};
enum class ThinkingMode { Manual, Adaptive, Disabled };
// New values go at the end.
enum class OutputEffort { Low, Medium, High, Max, XHigh };
enum class CacheTtl { FiveMinutes, OneHour };
struct CacheControl { std::optional<CacheTtl> ttl; };
enum class ToolChoiceMode { Auto, Any, None, Tool };
struct ToolChoice {
  ToolChoiceMode mode = ToolChoiceMode::Auto;
  std::string name;
  std::optional<bool> disable_parallel_tool_use;
};
struct Request {
  std::string model, system, account_scope;
  std::vector<sp::Message> messages;
  std::vector<ToolDefinition> tools;
  std::optional<uint64_t> max_tokens;
  std::optional<uint64_t> thinking_budget;
  std::optional<double> temperature, top_p;
  std::optional<ThinkingMode> thinking_mode{};
  std::optional<OutputEffort> output_effort{};
  std::optional<CacheControl> cache_control{};
  std::optional<ToolChoice> tool_choice{};
  std::optional<sp::OpenRouterRouting> provider{};
  // A code-execution container to reuse, by the id an earlier response reported.
  std::optional<std::string> container{};
  // Top-level members the policy admits for this origin (#32), sent as given after the typed ones.
  std::shared_ptr<const json::Document> extensions{};
  // anthropic-beta values to send, each admitted by the policy at this origin. A value a declared tool or a replayed
  // block needs is added without being listed here.
  std::vector<std::string> betas{};

  static Request simple(std::string model_name, std::string user_prompt,
                        std::optional<uint64_t> output_cap = std::nullopt,
                        std::string system_prompt = {}) {
    Request request;
    request.model = std::move(model_name);
    request.system = std::move(system_prompt);
    request.messages.push_back(sp::Message::user_text(std::move(user_prompt)));
    request.max_tokens = output_cap;
    return request;
  }
  static Request simple(std::string model_name, std::vector<sp::Message> history,
                        std::optional<uint64_t> output_cap = std::nullopt,
                        std::string system_prompt = {}) {
    Request request;
    request.model = std::move(model_name);
    request.system = std::move(system_prompt);
    request.messages = std::move(history);
    request.max_tokens = output_cap;
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
} // namespace sp::messages
