#pragma once

#include "core/media.h"
#include "sp/build_config.h"
#include "sp/config_defaults.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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
// A source an answer cites. `kind` is the provider's annotation type (url_citation, file_citation,
// container_file_citation, file_path, ...); the fields it does not have stay empty. `start` and `end` are the
// provider's offsets into the text when it gives them. The provider's object is kept whole in `raw`.
// Citations are read from the wire the message keeps: they are not part of its native seal.
struct Citation {
  std::string kind, url, title, file_id, filename;
  std::optional<std::uint64_t> start, end;
  std::shared_ptr<const json::Document> raw;
};
struct Text { std::string value; std::vector<Citation> citations{}; };
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
// A block the SDK does not type, kept whole: a response block the policy keeps (Messages `fallback`, `compaction`),
// or an input block the caller writes and the policy admits (Messages `search_result`). `wire_metadata` is the block.
struct Opaque {
  std::string wire_type;
  std::shared_ptr<const json::Document> wire_metadata;
};
struct ToolResultHostMetadata {
  std::string name, status;
  bool retryable = false, effect_uncertain = false;
};
struct ToolResult {
  std::string tool_use_id, content;
  bool is_error = false;
  std::optional<ToolResultHostMetadata> host{};
  // Ordered typed output; nonempty parts require an empty legacy content string, except that Generate takes a JSON
  // object content beside media-only parts as the response object. Generate partitions these into separately
  // ordered text and media lanes. An Opaque part is an input block the policy admits (Messages `search_result`).
  std::vector<std::variant<Text, Media, Opaque>> content_parts{};
};
struct Reasoning {
  std::string id;
  std::vector<std::string> summary;
  std::optional<std::string> encrypted_content{}, status{};
  std::vector<std::string> content{};
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

  static Message user_text(std::string text, std::string message_id = {}) {
    Message message;
    message.id = std::move(message_id);
    message.role = Role::User;
    message.parts.push_back(Text{std::move(text)});
    return message;
  }
  static Message system_text(std::string text, std::string message_id = {}) {
    Message message;
    message.id = std::move(message_id);
    message.role = Role::System;
    message.parts.push_back(Text{std::move(text)});
    return message;
  }
  static Message developer_text(std::string text, std::string message_id = {}) {
    Message message;
    message.id = std::move(message_id);
    message.role = Role::Developer;
    message.parts.push_back(Text{std::move(text)});
    return message;
  }
  static Message assistant_text(std::string text, std::string message_id = {}) {
    Message message;
    message.id = std::move(message_id);
    message.role = Role::Assistant;
    message.parts.push_back(Text{std::move(text)});
    return message;
  }
  // One tool result in a Role::Tool message. `content` is the legacy text form and is passed
  // through unchanged. Generate requires it to be one JSON object (a plain string such as "5"
  // is refused before dispatch there); the other families accept any text. `is_error` follows
  // each family's wire: Messages and Interactions send the flag, Generate wraps the object as
  // `error`, Responses refuses `true` as Unsupported and Chat has no field for it. Use
  // ToolResult::content_parts directly for typed text/media results.
  static Message tool_result(std::string tool_use_id, std::string content,
                             bool is_error = false, std::string message_id = {}) {
    Message message;
    message.id = std::move(message_id);
    message.role = Role::Tool;
    ToolResult result;
    result.tool_use_id = std::move(tool_use_id);
    result.content = std::move(content);
    result.is_error = is_error;
    message.parts.push_back(std::move(result));
    return message;
  }

  std::string joined_text() const {
    std::string out;
    for (const auto& part : parts)
      if (const auto* text = std::get_if<Text>(&part)) out += text->value;
    return out;
  }
  std::string joined_thinking() const {
    std::string out;
    for (const auto& part : parts) {
      if (const auto* thinking = std::get_if<Thinking>(&part)) {
        out += thinking->text;
      } else if (const auto* reasoning = std::get_if<Reasoning>(&part)) {
        const auto& items = !reasoning->content.empty() ? reasoning->content : reasoning->summary;
        for (const auto& item : items) out += item;
      } else if (const auto* thought = std::get_if<Thought>(&part)) {
        for (const auto& item : thought->summary) out += item;
      }
    }
    return out;
  }
  // The next three projections borrow from this Message (a view or references into `parts`).
  // They are deleted on a temporary so a borrow cannot outlive its owner; bind the Message,
  // or the owning Outcome, to a name first.
  std::optional<std::string_view> first_refusal() const& noexcept {
    for (const auto& part : parts)
      if (const auto* refusal = std::get_if<Refusal>(&part)) return refusal->text;
    return std::nullopt;
  }
  std::optional<std::string_view> first_refusal() const&& noexcept = delete;
  // Host-executable tool calls only (ToolCallKind::ClientExecuted).
  // ServerExecuted and ApprovalRequest calls are never returned here.
  std::vector<std::reference_wrapper<const ToolCall>> executable_tool_calls() const& {
    std::vector<std::reference_wrapper<const ToolCall>> out;
    for (const auto& part : parts) {
      if (const auto* call = std::get_if<ToolCall>(&part);
          call && call->kind == ToolCallKind::ClientExecuted) {
        out.emplace_back(*call);
      }
    }
    return out;
  }
  std::vector<std::reference_wrapper<const ToolCall>> executable_tool_calls() const&& = delete;
  std::vector<std::reference_wrapper<const InvalidToolCall>> invalid_tool_calls() const& {
    std::vector<std::reference_wrapper<const InvalidToolCall>> out;
    for (const auto& part : parts) {
      if (const auto* call = std::get_if<InvalidToolCall>(&part)) {
        out.emplace_back(*call);
      }
    }
    return out;
  }
  std::vector<std::reference_wrapper<const InvalidToolCall>> invalid_tool_calls() const&& = delete;
  bool has_invalid_tool_calls() const noexcept {
    for (const auto& part : parts)
      if (std::holds_alternative<InvalidToolCall>(part)) return true;
    return false;
  }
};
enum class Evidence { Reported, Derived };
struct Count { uint64_t value = 0; Evidence evidence = Evidence::Reported; };
enum class UsageStage { Missing, Partial, Final };
enum class UsageQuality { Consistent, Inconsistent };
struct UsageConflict { std::string counter, detail; };
enum class CostSource { None, OpenRouterUsd, UnknownCurrency };
enum class CostStatus { Missing, Available, Malformed, PrecisionExceeded, Overflow, UnknownCurrency, Conflict };
// CeilingParsedBinary64 is retained for source compatibility; amounts are no longer produced with it.
// CeilingCatalogueProduct: a catalogue estimate (core/pricing.h), the exact sum of counters times prices rounded up once.
enum class CostRounding { CeilingParsedBinary64, CeilingShortestDecimal, CeilingCatalogueProduct };
struct UsdAmount {
  uint64_t nano_usd = 0;
  Evidence evidence = Evidence::Reported;
  CostRounding rounding = CostRounding::CeilingShortestDecimal;
};
// Provider-reported usage metadata, not an invoice, estimate, or budget authority.
// One nanoUSD is 1e-9 USD. Amounts convert exactly from the shortest decimal that
// round-trips to the parsed binary64 value (the original lexeme is unavailable); only a
// remainder below one nanoUSD rounds upward. Stage is the enclosing Usage.stage.
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

  // Convenience reads of one counter. An absent counter stays nullopt (unknown), never zero.
  // They return the bare number: they drop Count::evidence (Reported or Derived) and say
  // nothing about `stage` (a partial or failed call), `quality` or `conflicts`. Read those
  // fields, or the Count itself, before settling or billing on a value. total_tokens_value()
  // prefers `total` and falls back to `provider_reported_total`, so it can mix the two;
  // cost_nano_usd_value() is the provider-reported USD total only when its status is
  // Available (see provider_cost), not an invoice.
  std::optional<uint64_t> input_total_value() const noexcept {
    return input_total ? std::optional<uint64_t>(input_total->value) : std::nullopt;
  }
  std::optional<uint64_t> output_total_value() const noexcept {
    return output_total ? std::optional<uint64_t>(output_total->value) : std::nullopt;
  }
  std::optional<uint64_t> reasoning_value() const noexcept {
    return reasoning ? std::optional<uint64_t>(reasoning->value) : std::nullopt;
  }
  std::optional<uint64_t> total_tokens_value() const noexcept {
    if (total) return total->value;
    if (provider_reported_total) return provider_reported_total->value;
    return std::nullopt;
  }
  std::optional<uint64_t> cost_nano_usd_value() const noexcept {
    return provider_cost.total ? std::optional<uint64_t>(provider_cost.total->nano_usd) : std::nullopt;
  }
};
// Pending: the provider is still working on a job it runs in the background (an Interactions `background` request,
// with `raw` its status). The message id names the job; retrieve it later to get its result.
// Compaction: the answer is a summary of the conversation in place of a reply (a Messages `compaction` request), or
// the turn paused right after writing one (`pause_after_compaction`). Its message carries the summary block.
enum class StopKind { EndTurn, ToolUse, MaxTokens, StopSequence, ContentFilter, Refusal, PauseTurn, ContextLimit, MalformedCall, Unknown, Pending, Compaction };
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
  // Which check of the SDK failed, as a stable machine-readable code ("responses.event.unsupported",
  // "chat.finish_reason.invalid"): lower-case dotted segments from a fixed table, never built from the response,
  // so it holds no vendor text, payload bytes or model output. Empty when no specific check applies, as for
  // transport, HTTP and vendor-reported errors, which keep using kind, http_status and vendor_code.
  // safe_message does not change and does not include it.
  std::string reason{};
};
// Owned wire observations, in arrival order before their semantic projections.
// Neither field grants native replay authority.
struct RawWire { std::string type; std::shared_ptr<const json::Document> payload; };
// How the model a response reported relates to the requested one. A name never makes a completed,
// billed response fail; only `Unexplained` withholds replay authority from the native state.
enum class ServedRelation : std::uint8_t {
  NotReported,  // the response named no model
  Exact,        // the requested name
  Snapshot,     // the requested name with a version or date suffix ("-20251001", "-2025-08-07", "-001")
  Listed,       // a name the model catalogue relates to the request (aliases, served_as, alias_of)
  Routed,       // another name at a declared routing gateway
  Unexplained,  // any other name: delivered, but its native state is not replayable
};
struct ServedModel {
  std::string requested, served;
  ServedRelation relation = ServedRelation::NotReported;
};
struct Completion {
  std::vector<Message> messages;
  StopReason stop;
  Usage usage;
  std::shared_ptr<const json::Document> wire_envelope{};
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events{};
  // The requested and the reported model, and how they relate.
  ServedModel model{};
  // A value the endpoint's typed outcome needs that is no wire evidence and must reach no journal (a resumable
  // upload's session URL). It is not in `raw_events`, and a restored Completion does not have it.
  std::string secret{};

  std::string joined_text() const {
    std::string out;
    for (const auto& message : messages) out += message.joined_text();
    return out;
  }
  std::string joined_thinking() const {
    std::string out;
    for (const auto& message : messages) out += message.joined_thinking();
    return out;
  }
  // Borrowing projections: deleted on a temporary, as on Message.
  std::optional<std::string_view> first_refusal() const& noexcept {
    for (const auto& message : messages)
      if (auto refusal = message.first_refusal()) return refusal;
    return std::nullopt;
  }
  std::optional<std::string_view> first_refusal() const&& noexcept = delete;
  std::vector<std::reference_wrapper<const ToolCall>> executable_tool_calls() const& {
    std::vector<std::reference_wrapper<const ToolCall>> out;
    for (const auto& message : messages) {
      auto calls = message.executable_tool_calls();
      out.insert(out.end(), calls.begin(), calls.end());
    }
    return out;
  }
  std::vector<std::reference_wrapper<const ToolCall>> executable_tool_calls() const&& = delete;
  std::vector<std::reference_wrapper<const InvalidToolCall>> invalid_tool_calls() const& {
    std::vector<std::reference_wrapper<const InvalidToolCall>> out;
    for (const auto& message : messages) {
      auto calls = message.invalid_tool_calls();
      out.insert(out.end(), calls.begin(), calls.end());
    }
    return out;
  }
  std::vector<std::reference_wrapper<const InvalidToolCall>> invalid_tool_calls() const&& = delete;
  bool has_invalid_tool_calls() const noexcept {
    for (const auto& message : messages)
      if (message.has_invalid_tool_calls()) return true;
    return false;
  }
};
struct PartialCompletion {
  std::vector<Message> messages;
  Usage usage;
  std::optional<StopReason> stop;
  std::shared_ptr<const json::Document> wire_envelope{};
  std::vector<RawWire> raw_events{};
  ServedModel model{};

  std::string joined_text() const {
    std::string out;
    for (const auto& message : messages) out += message.joined_text();
    return out;
  }
  std::string joined_thinking() const {
    std::string out;
    for (const auto& message : messages) out += message.joined_thinking();
    return out;
  }
  // Borrowing projections: deleted on a temporary, as on Message.
  std::optional<std::string_view> first_refusal() const& noexcept {
    for (const auto& message : messages)
      if (auto refusal = message.first_refusal()) return refusal;
    return std::nullopt;
  }
  std::optional<std::string_view> first_refusal() const&& noexcept = delete;
  std::vector<std::reference_wrapper<const ToolCall>> executable_tool_calls() const& {
    std::vector<std::reference_wrapper<const ToolCall>> out;
    for (const auto& message : messages) {
      auto calls = message.executable_tool_calls();
      out.insert(out.end(), calls.begin(), calls.end());
    }
    return out;
  }
  std::vector<std::reference_wrapper<const ToolCall>> executable_tool_calls() const&& = delete;
  std::vector<std::reference_wrapper<const InvalidToolCall>> invalid_tool_calls() const& {
    std::vector<std::reference_wrapper<const InvalidToolCall>> out;
    for (const auto& message : messages) {
      auto calls = message.invalid_tool_calls();
      out.insert(out.end(), calls.begin(), calls.end());
    }
    return out;
  }
  std::vector<std::reference_wrapper<const InvalidToolCall>> invalid_tool_calls() const&& = delete;
  bool has_invalid_tool_calls() const noexcept {
    for (const auto& message : messages)
      if (message.has_invalid_tool_calls()) return true;
    return false;
  }
};
struct Failure { Error error; PartialCompletion partial; };
using Outcome = std::variant<Completion, Failure>;

inline bool is_completion(const Outcome& outcome) noexcept {
  return std::holds_alternative<Completion>(outcome);
}
inline bool is_failure(const Outcome& outcome) noexcept {
  return std::holds_alternative<Failure>(outcome);
}
inline const Completion* as_completion(const Outcome& outcome) noexcept {
  return std::get_if<Completion>(&outcome);
}
inline const Failure* as_failure(const Outcome& outcome) noexcept {
  return std::get_if<Failure>(&outcome);
}
inline const Error* as_error(const Outcome& outcome) noexcept {
  if (const auto* failure = std::get_if<Failure>(&outcome)) return &failure->error;
  return nullptr;
}
inline std::string joined_text(const Outcome& outcome) {
  if (const auto* completion = std::get_if<Completion>(&outcome)) return completion->joined_text();
  return {};
}
inline bool is_completion(const std::shared_ptr<const Outcome>& result) noexcept {
  return result && is_completion(*result);
}
inline bool is_failure(const std::shared_ptr<const Outcome>& result) noexcept {
  return result && is_failure(*result);
}
inline const Completion* as_completion(const std::shared_ptr<const Outcome>& result) noexcept {
  return result ? as_completion(*result) : nullptr;
}
inline const Failure* as_failure(const std::shared_ptr<const Outcome>& result) noexcept {
  return result ? as_failure(*result) : nullptr;
}
inline const Error* as_error(const std::shared_ptr<const Outcome>& result) noexcept {
  return result ? as_error(*result) : nullptr;
}
inline std::string joined_text(const std::shared_ptr<const Outcome>& result) {
  return result ? joined_text(*result) : std::string{};
}
// as_completion/as_failure/as_error return a pointer into the outcome. On a temporary owner
// (for example directly on the result of Client::complete) it would dangle at the end of the
// expression, so those calls are deleted: keep the Result in a variable first.
const Completion* as_completion(const Outcome&&) = delete;
const Failure* as_failure(const Outcome&&) = delete;
const Error* as_error(const Outcome&&) = delete;
const Completion* as_completion(std::shared_ptr<const Outcome>&&) = delete;
const Failure* as_failure(std::shared_ptr<const Outcome>&&) = delete;
const Error* as_error(std::shared_ptr<const Outcome>&&) = delete;
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
  // Text only.
  std::vector<Citation> citations{};
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
