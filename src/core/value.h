#pragma once

#include <chrono>
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
class NativeContext;
class NativeReplay;
enum class Role { System, Developer, User, Assistant, Tool };
enum class ToolCallKind { ClientExecuted, ServerExecuted, ApprovalRequest };
enum class InvalidReason { Truncated, NotJson, DuplicateKey, DepthExceeded, Empty, Other };
struct Text { std::string value; };
struct Refusal { std::string text; std::string raw_code; };
struct ToolCall {
  std::string id, name;
  ToolCallKind kind = ToolCallKind::ClientExecuted;
  std::shared_ptr<const json::Document> input;
  std::string wire_type{};
  std::shared_ptr<const json::Document> wire_metadata{};
};
struct InvalidToolCall {
  std::string id, name;
  ToolCallKind kind = ToolCallKind::ClientExecuted;
  std::string raw_fragment;
  InvalidReason reason = InvalidReason::Other;
  std::string wire_type{};
  std::shared_ptr<const json::Document> wire_metadata{};
};
struct Thinking { std::string text; std::optional<std::string> signature{}; };
struct RedactedThinking { std::string data; };
struct ServerToolResult {
  std::string tool_use_id, wire_type;
  std::shared_ptr<const json::Document> content;
};
struct ToolResult { std::string tool_use_id, content; bool is_error = false; };
struct Reasoning {
  std::string id;
  std::vector<std::string> summary;
  std::optional<std::string> encrypted_content{}, status{};
  std::vector<std::string> content{};
};
struct Opaque {
  std::string wire_type;
  std::shared_ptr<const json::Document> wire_metadata;
};
using Part = std::variant<Text, Refusal, ToolCall, InvalidToolCall, Thinking, RedactedThinking, ServerToolResult, ToolResult, Reasoning, Opaque>;
struct Message {
  std::string id;
  Role role = Role::Assistant;
  std::vector<Part> parts;
  std::shared_ptr<const NativeReplay> native{};
  // Responses output is one atomic ordered replay group, not imported JSON authority.
  std::shared_ptr<const json::Document> wire_output{};
};
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
struct StopReason {
  StopKind kind = StopKind::Unknown;
  std::string raw;
  std::optional<std::string> sequence{};
  std::shared_ptr<const json::Document> details{};
};
enum class ErrorKind { InvalidConfig, InvalidRequest, Unsupported, Transport, ProtocolCorrupt, Truncated, RemoteFailure, Cancelled, DeadlineExceeded, ResourceLimit, Misuse, ReplayIneligible, Authentication, Permission, NotFound, RateLimited, QuotaExhausted, LimitUnknown, Overloaded };
enum class RetryClass { Never, Transient, AfterReset, Unknown };
enum class RetrySafety { NotSent, PossiblyAccepted, RejectedBeforeOutput, OutputObserved };
struct AttemptEvidence {
  bool request_may_have_left = false;
  std::int64_t request_body_bytes = 0;
  bool response_head_seen = false;
  std::uint32_t transport_internal_resends = 0;
  std::uint32_t attempts = 0;
};
struct Error {
  ErrorKind kind = ErrorKind::ProtocolCorrupt;
  std::string safe_message;
  RetryClass retry_class = RetryClass::Never;
  RetrySafety retry_safety = RetrySafety::PossiblyAccepted;
  int http_status = 0;
  std::string vendor_code{};
  std::optional<std::chrono::milliseconds> retry_after{};
  AttemptEvidence attempt{};
};
struct Completion { std::vector<Message> messages; StopReason stop; Usage usage; };
struct PartialCompletion { std::vector<Message> messages; Usage usage; std::optional<StopReason> stop; };
struct Failure { Error error; PartialCompletion partial; };
using Outcome = std::variant<Completion, Failure>;
struct LocalId { uint32_t value = 0; friend bool operator==(LocalId, LocalId) = default; };
enum class PartKind { Text, Refusal, ToolCall, Thinking, RedactedThinking, ServerToolResult, Reasoning, Opaque };
struct PartHeader {
  std::string wire_id, name;
  ToolCallKind tool_kind = ToolCallKind::ClientExecuted;
  std::string wire_type{};
  std::shared_ptr<const json::Document> wire_metadata{};
};
enum class DeltaChannel { Content, Signature };
struct DeltaPayload { PartKind kind; std::string_view bytes; DeltaChannel channel = DeltaChannel::Content; };
struct Begin { std::string generation; };
struct MessageBegin {
  LocalId message;
  std::optional<std::string> vendor_id;
  Role role = Role::Assistant;
  std::shared_ptr<const NativeContext> native_context{};
};
struct PartBegin { LocalId message, part; PartKind kind = PartKind::Text; PartHeader header; uint64_t order = 0; };
struct PartDelta { LocalId part; DeltaPayload payload; };
// No snapshot means seal the accumulator-owned bytes. A snapshot reconciles a prefix,
// never appends it. Tool snapshots are raw argument strings, parsed only at seal.
struct PartSeal {
  LocalId part;
  std::optional<std::string_view> snapshot;
  std::shared_ptr<const json::Document> wire_metadata{};
};
struct MessageSeal {
  LocalId message;
  std::shared_ptr<const json::Document> wire_output{};
};
struct UsageUpdate { Usage snapshot; };
struct Stop { StopReason reason; };
struct Commit { std::string evidence; };
struct Fail { Error error; };
using Event = std::variant<Begin, MessageBegin, PartBegin, PartDelta, PartSeal, MessageSeal, UsageUpdate, Stop, Commit, Fail>;
struct SemanticLimits { size_t max_parts = 1024; size_t max_content_bytes = 16 << 20; size_t max_tool_bytes = 1 << 20; size_t max_json_depth = 64; };
} // namespace sp
