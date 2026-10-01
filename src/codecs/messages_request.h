#pragma once
#include "core/value.h"
#include "descriptor/descriptor.h"
#include <utility>

namespace sp::messages {
struct ToolDefinition {
  std::string name, description;
  std::shared_ptr<const json::Document> input_schema;
  std::string type;
  std::optional<uint64_t> max_uses;
};
struct Request {
  std::string model, system, account_scope;
  std::vector<sp::Message> messages;
  std::vector<ToolDefinition> tools;
  uint64_t max_tokens = 1024;
  std::optional<uint64_t> thinking_budget;
  std::optional<double> temperature, top_p;
};
struct EncodedRequest {
  std::string method, path;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::shared_ptr<const NativeContext> context;
};
using EncodeResult = std::variant<EncodedRequest, Error>;
EncodeResult encode(const descriptor::ValidatedDescriptor&, const Request&, bool streaming);
} // namespace sp::messages
