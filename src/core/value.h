#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sp::json { class Document; }
namespace sp {
enum class Role { System, Developer, User, Assistant, Tool };
enum class ToolCallKind { ClientExecuted, ServerExecuted, ApprovalRequest };
enum class InvalidReason { Truncated, NotJson, DuplicateKey, DepthExceeded, Empty, Other };
struct Text { std::string value; };
struct Refusal { std::string text; std::string raw_code; };
struct ToolCall {
  std::string id, name;
  ToolCallKind kind = ToolCallKind::ClientExecuted;
  std::shared_ptr<const json::Document> input;
};
struct InvalidToolCall {
  std::string id, name;
  ToolCallKind kind = ToolCallKind::ClientExecuted;
  std::string raw_fragment;
  InvalidReason reason = InvalidReason::Other;
};
using Part = std::variant<Text, Refusal, ToolCall, InvalidToolCall>;
struct Message { std::string id; Role role = Role::Assistant; std::vector<Part> parts; };
enum class Evidence { Reported, Derived };
struct Count { uint64_t value = 0; Evidence evidence = Evidence::Reported; };
enum class UsageStage { Missing, Partial, Final };
enum class UsageQuality { Consistent, Inconsistent };
struct UsageConflict { std::string counter, detail; };
struct Usage {
  std::optional<Count> input_total, output_total, total, provider_reported_total;
  std::optional<Count> input_uncached, cache_read, cache_write, reasoning;
  std::map<std::string, Count> extra;
  UsageStage stage = UsageStage::Missing;
  UsageQuality quality = UsageQuality::Consistent;
  std::vector<UsageConflict> conflicts;
};
enum class StopKind { EndTurn, ToolUse, MaxTokens, StopSequence, ContentFilter, Refusal, PauseTurn, ContextLimit, MalformedCall, Unknown };
struct StopReason { StopKind kind = StopKind::Unknown; std::string raw; };
enum class ErrorKind { InvalidConfig, InvalidRequest, Unsupported, Transport, ProtocolCorrupt, Truncated, RemoteFailure, Cancelled, DeadlineExceeded, ResourceLimit, Misuse };
struct Error { ErrorKind kind = ErrorKind::ProtocolCorrupt; std::string safe_message; };
struct Completion { std::vector<Message> messages; StopReason stop; Usage usage; };
struct PartialCompletion { std::vector<Message> messages; Usage usage; std::optional<StopReason> stop; };
struct Failure { Error error; PartialCompletion partial; };
using Outcome = std::variant<Completion, Failure>;
struct LocalId { uint32_t value = 0; friend bool operator==(LocalId, LocalId) = default; };
enum class PartKind { Text, Refusal, ToolCall };
struct PartHeader { std::string wire_id, name; ToolCallKind tool_kind = ToolCallKind::ClientExecuted; };
struct DeltaPayload { PartKind kind; std::string_view bytes; };
struct Begin { std::string generation; };
struct MessageBegin { LocalId message; std::optional<std::string> vendor_id; Role role = Role::Assistant; };
struct PartBegin { LocalId message, part; PartKind kind = PartKind::Text; PartHeader header; uint64_t order = 0; };
struct PartDelta { LocalId part; DeltaPayload payload; };
// No snapshot means seal the accumulator-owned bytes. A snapshot reconciles a prefix,
// never appends it. Tool snapshots are raw argument strings, parsed only at seal.
struct PartSeal { LocalId part; std::optional<std::string_view> snapshot; };
struct MessageSeal { LocalId message; };
struct UsageUpdate { Usage snapshot; };
struct Stop { StopReason reason; };
struct Commit { std::string evidence; };
struct Fail { Error error; };
using Event = std::variant<Begin, MessageBegin, PartBegin, PartDelta, PartSeal, MessageSeal, UsageUpdate, Stop, Commit, Fail>;
struct SemanticLimits { size_t max_parts = 1024; size_t max_content_bytes = 16 << 20; size_t max_tool_bytes = 1 << 20; size_t max_json_depth = 64; };
} // namespace sp
