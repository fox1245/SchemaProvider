#pragma once
#include "core/value.h"
#include "descriptor/descriptor.h"
#include <utility>

namespace sp::interactions {
struct ToolDefinition {
  std::string name, description;
  std::shared_ptr<const json::Document> parameters;
};
struct Request {
  std::string model, account_scope, system;
  std::vector<Message> messages;
  std::vector<ToolDefinition> tools;
  std::optional<uint64_t> max_output_tokens;
  std::optional<std::string> thinking_level;
  std::optional<bool> thinking_summaries;
  std::optional<std::string> service_tier;
  std::optional<std::string> required_tool;
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
} // namespace sp::interactions
