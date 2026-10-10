#pragma once

#include "core/media.h"
#include "sp/config_defaults.h"
#include <array>
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
struct ToolResultHostMetadata {
  std::string name, status;
  bool retryable = false, effect_uncertain = false;
};
struct ToolResult {
  std::string tool_use_id, content;
  bool is_error = false;
  std::optional<ToolResultHostMetadata> host{};
  // Ordered typed output; nonempty parts require an empty legacy content string.
  // Generate partitions these into separately ordered text and media lanes.
  std::vector<std::variant<Text, Media>> content_parts{};
};
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
struct Thought {
  std::vector<std::string> summary;
  std::optional<std::string> signature{};
};
using Part = std::variant<Text, Refusal, ToolCall, InvalidToolCall, Thinking, RedactedThinking, ServerToolResult, ToolResult, Reasoning, Opaque, Media, Thought>;
struct Message {
  std::string id;
  Role role = Role::Assistant;
  std::vector<Part> parts;
  std::shared_ptr<const NativeReplay> native{};
  // Atomic ordered native replay group, never imported JSON authority.
  std::shared_ptr<const json::Document> wire_output{};
};
enum class Evidence { Reported, Derived };
struct Count { uint64_t value = 0; Evidence evidence = Evidence::Reported; };
enum class UsageStage { Missing, Partial, Final };
enum class UsageQuality { Consistent, Inconsistent };
struct UsageConflict { std::string counter, detail; };
enum class CostSource { None, OpenRouterUsd, UnknownCurrency };
enum class CostStatus { Missing, Available, Malformed, PrecisionExceeded, Overflow, UnknownCurrency, Conflict };
enum class CostRounding { CeilingParsedBinary64 };
struct UsdAmount {
  uint64_t nano_usd = 0;
  Evidence evidence = Evidence::Reported;
  CostRounding rounding = CostRounding::CeilingParsedBinary64;
};
// Provider-reported usage metadata, not an invoice, estimate, or budget authority.
// One nanoUSD is 1e-9 USD. Amounts round upward from the parsed binary64 value,
// not the unavailable original decimal lexeme; stage is the enclosing Usage.stage.
struct ProviderReportedCost {
  std::optional<UsdAmount> total, upstream_total, upstream_input, upstream_output;
  // Status order: total, upstream_total, upstream_input, upstream_output.
  std::array<CostStatus, 4> status{};
  std::optional<bool> is_byok;
  CostStatus byok_status = CostStatus::Missing;
  CostSource source = CostSource::None;
  UsageQuality quality = UsageQuality::Consistent;
};
struct Usage {
  std::optional<Count> input_total, output_total, total, provider_reported_total;
  std::optional<Count> input_uncached, cache_read, cache_write, reasoning;
  std::map<std::string, Count> extra;
  UsageStage stage = UsageStage::Missing;
  UsageQuality quality = UsageQuality::Consistent;
  std::vector<UsageConflict> conflicts;
  ProviderReportedCost provider_cost;
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
  // A real retried wire attempt could have accrued unreported provider usage.
  bool prior_usage_unknown = false;
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
// Owned wire observations, in arrival order before their semantic projections.
// Neither field grants native replay authority.
struct RawWire { std::string type; std::shared_ptr<const json::Document> payload; };
struct Completion {
  std::vector<Message> messages;
  StopReason stop;
  Usage usage;
  std::shared_ptr<const json::Document> wire_envelope{};
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events{};
};
struct PartialCompletion {
  std::vector<Message> messages;
  Usage usage;
  std::optional<StopReason> stop;
  std::shared_ptr<const json::Document> wire_envelope{};
  std::vector<RawWire> raw_events{};
};
struct Failure { Error error; PartialCompletion partial; };
using Outcome = std::variant<Completion, Failure>;
struct LocalId { uint32_t value = 0; friend bool operator==(LocalId, LocalId) = default; };
enum class PartKind { Text, Refusal, ToolCall, Thinking, RedactedThinking, ServerToolResult, Reasoning, Opaque, Thought, Media };
struct PartHeader {
  std::string wire_id, name;
  ToolCallKind tool_kind = ToolCallKind::ClientExecuted;
  std::string wire_type{};
  std::shared_ptr<const json::Document> wire_metadata{};
  // PartKind::Media only: kind, MIME, source form, reference, name, identity and transcript of
  // the generated media. Inline payload bytes (canonical base64) arrive as Media deltas.
  Media media{};
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
struct ResponseEnvelope { std::shared_ptr<const json::Document> payload; };
using Event = std::variant<Begin, MessageBegin, PartBegin, PartDelta, PartSeal, MessageSeal, UsageUpdate, Stop, Commit, Fail, RawWire, ResponseEnvelope>;
struct SemanticLimits {
  size_t max_parts = config_defaults::defaults_semantic_max_parts;
  size_t max_content_bytes = config_defaults::defaults_semantic_max_content_bytes;
  size_t max_tool_bytes = config_defaults::defaults_semantic_max_tool_bytes;
  size_t max_json_depth = config_defaults::defaults_semantic_max_json_depth;
};
} // namespace sp
