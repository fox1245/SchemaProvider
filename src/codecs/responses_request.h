#pragma once
#include "core/value.h"
#include "descriptor/descriptor.h"
#include <utility>

namespace sp::responses {
struct ToolDefinition {
  std::string name, description;
  std::shared_ptr<const json::Document> parameters;
  bool strict = true;
};
struct ReasoningOptions {
  std::string effort = "low";
  std::string summary = "auto";
};
struct Request {
  std::string model, account_scope, instructions;
  std::vector<sp::Message> messages;
  std::vector<ToolDefinition> tools;
  std::optional<uint64_t> max_output_tokens;
  std::optional<ReasoningOptions> reasoning;
  std::optional<std::string> required_tool;
};
struct EncodedRequest {
  std::string method, path;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::shared_ptr<const NativeContext> context;
};
using EncodeResult = std::variant<EncodedRequest, Error>;
EncodeResult encode(const descriptor::ValidatedDescriptor&, const Request&, bool streaming);
} // namespace sp::responses
