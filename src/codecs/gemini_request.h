#pragma once
#include "core/value.h"
#include "descriptor/descriptor.h"

namespace sp::gemini {
struct ToolDefinition {
  std::string name, description;
  std::shared_ptr<const json::Document> parameters;
};
enum class HistoryMode { NativeOnly, PortableForeign };
enum class ThinkingLevel { Minimal, Low, Medium, High };
enum class SafetyCategory { Harassment, HateSpeech, SexuallyExplicit, DangerousContent, CivicIntegrity };
enum class SafetyThreshold { BlockNone, BlockOnlyHigh, BlockMediumAndAbove, BlockLowAndAbove, Off };
struct SafetySetting {
  SafetyCategory category = SafetyCategory::Harassment;
  SafetyThreshold threshold = SafetyThreshold::BlockMediumAndAbove;
};
enum class ToolChoiceMode { Auto, Any, None, Validated };
struct ToolChoice {
  ToolChoiceMode mode = ToolChoiceMode::Auto;
  std::vector<std::string> allowed_function_names;
};
struct Request {
  std::string model, account_scope, system;
  std::vector<Message> messages;
  std::vector<ToolDefinition> tools;
  std::optional<uint64_t> max_output_tokens;
  std::optional<uint64_t> thinking_budget;
  std::optional<bool> include_thoughts;
  std::optional<std::string> required_tool;
  HistoryMode history_mode = HistoryMode::NativeOnly;
  std::optional<ThinkingLevel> thinking_level;
  std::optional<double> temperature;
  std::vector<SafetySetting> safety_settings;
  std::optional<ToolChoice> tool_choice;
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
} // namespace sp::gemini
