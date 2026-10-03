#pragma once

#include "core/value.h"
#include "qualification/meter.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sp::canary {
enum class Provider { OpenAI, Anthropic, Gemini, OpenAIResponses, VisionChat, VisionResponses, VisionMessages, VisionGemini, VisionInteractions };
constexpr bool vision_provider(Provider provider) {
  return provider >= Provider::VisionChat && provider <= Provider::VisionInteractions;
}
struct Bounds {
  std::uint64_t input_tokens{}, output_tokens{}, input_rate{}, output_rate{};
  std::uint64_t calls{}, tokens{}, micro_usd{};
};
class Profile {
 public:
  Provider provider() const { return provider_; }
  const std::string& model() const { return model_; }
  const std::string& origin() const { return origin_; }
  const Bounds& bounds() const { return bounds_; }
  std::uint64_t thinking_budget() const { return thinking_budget_; }
  bool loopback() const { return loopback_; }
 private:
  Profile() = default;
  Provider provider_{};
  std::string model_, origin_;
  Bounds bounds_;
  std::uint64_t thinking_budget_{};
  bool loopback_{};
  friend Profile parse_profile(std::string_view, bool, std::optional<std::uint64_t>);
};
// All exceptions have fixed, non-sensitive messages. Neither parser echoes input.
Profile parse_profile(std::string_view, bool allow_test_loopback = false,
                      std::optional<std::uint64_t> output_tokens = {});
std::string read_profile_file(const std::string& path);
// An explicit file overrides the process environment. Only NAME=value syntax is
// supported; no shell, expansion, export, quoting, or fallback on malformed input.
std::string credential(Provider, const std::optional<std::string>& env_file = {});
std::uint64_t reserved_cost(const Bounds&);

enum class State { Passed, Failed, NotRun };
enum class ExecutionMode { Full, BaselinePair, GoogleDiagnostics };
struct RunSelection {
  ExecutionMode mode = ExecutionMode::Full;
  std::uint64_t repetition = 0;
  // Diagnostics only: remaining attempts in this bounded CLI invocation.
  std::uint64_t attempt_limit = 15;
};
enum class Reason {
  None, MissingCredential, CallBudget, TokenBudget, CostBudget, LedgerFailure,
  RuntimeFailure, MissingText, MissingTool, InvalidTool, MissingSignature,
  PrerequisiteFailed, RetentionMismatch, MutationUnavailable,
  SignatureRejected, NegativeAccepted, NegativeInconclusive, InvalidUsage,
  MissingReasoning, CiphertextRejected, OmissionAccepted, OmissionRejected,
  IncorrectVision, UnreadableVision, DiagnosticLimit
};
enum class Replay { NotApplicable, ReplayVerified, ReplayAcceptanceUnobservable };
struct Case {
  std::string name; // fixed runner-owned identifier, never a remote identifier
  State state = State::NotRun;
  Reason reason = Reason::PrerequisiteFailed;
  bool dispatched = false;
  bool selected = true;
  std::uint64_t attempts = 0;
  std::optional<std::uint64_t> input_tokens{}, output_tokens{};
  std::optional<std::uint64_t> estimated_micro_usd{};
  std::optional<std::uint64_t> input_uncached{}, cache_read{}, cache_write{}, reasoning{};
  UsageStage usage_stage = UsageStage::Missing;
  UsageQuality usage_quality = UsageQuality::Consistent;
  std::optional<ErrorKind> failure_kind{};
  int http_status = 0;
  std::uint64_t reasoning_items = 0, summary_items = 0;
  bool encrypted_present = false, native_complete = false;
  bool image_sent = false, reasoning_requested = false, reasoning_disabled = false, default_off = false;
  bool visible_reasoning = false, native_present = false;
  std::optional<bool> vision_correct{};
  std::optional<qualification::SettlementKind> settlement{};
  std::optional<std::uint64_t> reserved_micro_usd{}, charged_micro_usd{};
  std::optional<qualification::Denial> admission_denial{};
  std::uint64_t native_carriers = 0, distinct_native_carriers = 0;
  bool duplicate_control = false, control_changed = false;
};
struct Report {
  Provider provider{};
  ExecutionMode mode = ExecutionMode::Full;
  bool test_only = false;
  Replay replay = Replay::NotApplicable;
  bool positive_retained = false, signature_mutated = false;
  bool ciphertext_mutated = false, reasoning_removed = false;
  std::uint64_t native_leaves = 0;
  std::optional<qualification::Totals> meter_totals{};
  std::uint64_t repetition = 1;
  std::vector<Case> cases;
};
// Synchronous caller bridge, production runtime + HTTP backend, no retries.
// Raw response material and native replay objects remain in process memory only.
Report run(const Profile&, const std::string& project_root, std::string api_key, RunSelection = {});
Report run_vision(const Profile&, const std::string& project_root, std::string api_key, RunSelection = {});
std::string report_json(const Report&);
std::string_view plan_json();
std::string_view help_text();
} // namespace sp::canary
