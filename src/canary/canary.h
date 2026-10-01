#pragma once

#include "core/value.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sp::canary {
enum class Provider { OpenAI, Anthropic };
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
  friend Profile parse_profile(std::string_view, bool);
};
// All exceptions have fixed, non-sensitive messages. Neither parser echoes input.
Profile parse_profile(std::string_view, bool allow_test_loopback = false);
std::string read_profile_file(const std::string& path);
// An explicit file overrides the process environment. Only NAME=value syntax is
// supported; no shell, expansion, export, quoting, or fallback on malformed input.
std::string credential(Provider, const std::optional<std::string>& env_file = {});
struct Totals { std::uint64_t calls{}, tokens{}, micro_usd{}; };
enum class Reservation { Allowed, Calls, Tokens, Cost };
struct Debit { Reservation result; Totals total; };
std::uint64_t reserved_cost(const Bounds&);
// Separate opens + flock serialize threads and processes; partial records fail closed.
Debit reserve(const Profile&, const std::string& ledger_path);
Totals ledger_totals(const Profile&, const std::string& ledger_path);

enum class State { Passed, Failed, NotRun };
enum class Reason {
  None, MissingCredential, CallBudget, TokenBudget, CostBudget, LedgerFailure,
  RuntimeFailure, MissingText, MissingTool, InvalidTool, MissingSignature,
  PrerequisiteFailed, RetentionMismatch, MutationUnavailable,
  SignatureRejected, NegativeAccepted, NegativeInconclusive, InvalidUsage
};
enum class Replay { NotApplicable, ReplayVerified, ReplayAcceptanceUnobservable };
struct Case {
  std::string name; // fixed runner-owned identifier, never a remote identifier
  State state = State::NotRun;
  Reason reason = Reason::PrerequisiteFailed;
  bool dispatched = false;
  std::uint64_t attempts = 0;
  std::optional<std::uint64_t> input_tokens{}, output_tokens{};
  std::optional<std::uint64_t> estimated_micro_usd{};
  std::optional<std::uint64_t> input_uncached{}, cache_read{}, cache_write{}, reasoning{};
  UsageStage usage_stage = UsageStage::Missing;
  UsageQuality usage_quality = UsageQuality::Consistent;
};
struct Report {
  Provider provider{};
  bool test_only = false;
  Replay replay = Replay::NotApplicable;
  bool positive_retained = false, signature_mutated = false;
  std::uint64_t native_leaves = 0;
  Totals reserved;
  bool reserved_known = false;
  std::vector<Case> cases;
};
// Synchronous caller bridge, production runtime + HTTP backend, no retries.
// Raw response material and native replay objects remain in process memory only.
Report run(const Profile&, const std::string& ledger_path, std::string api_key);
std::string report_json(const Report&);
std::string_view plan_json();
std::string_view help_text();
} // namespace sp::canary
