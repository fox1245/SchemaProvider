#pragma once
#include "core/value.h"
#include "descriptor/descriptor.h"

namespace sp::gemini {
struct ToolDefinition {
  std::string name, description;
  std::shared_ptr<const json::Document> parameters;
};
struct Request {
  std::string model, account_scope, system;
  std::vector<Message> messages;
  std::vector<ToolDefinition> tools;
  uint64_t max_output_tokens = 4096;
  std::optional<uint64_t> thinking_budget;
  bool include_thoughts = true;
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
} // namespace sp::gemini
