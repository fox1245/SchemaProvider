#pragma once

#include "canary/canary.h"
#include "canary/io.h"
#include "crypto/crypto.h"
#include "runtime/client.h"
#include <algorithm>
#include <array>
#include <initializer_list>

namespace sp::canary::detail {
inline std::string digest(std::initializer_list<std::string_view> fields) {
  crypto::Sha256 hash;
  for (auto field : fields) {
    const auto size = std::to_string(field.size());
    hash.update(size);
    hash.update(":");
    hash.update(field);
  }
  const auto bytes = hash.finish();
  constexpr char hex[] = "0123456789abcdef";
  std::string result(64, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    result[2 * i] = hex[bytes[i] >> 4]; result[2 * i + 1] = hex[bytes[i] & 15];
  }
  return result;
}
inline qualification::Outcome metered_outcome(const runtime::Result& result) {
  qualification::Outcome outcome;
  // Partial/failure delivery is never authority to release a hold, even when
  // a partial counter happens to carry a final marker.
  const auto* completion = result ? std::get_if<Completion>(result.get()) : nullptr;
  if (!completion) return outcome;
  const auto& usage = completion->usage;
  // Final counters cover only the reported attempt. Prior uncertain attempts
  // and transport-internal resends can carry additional unreported charges.
  outcome.final_consistent = usage.stage == UsageStage::Final &&
      usage.quality == UsageQuality::Consistent && usage.conflicts.empty() &&
      !completion->attempt.prior_usage_unknown && completion->attempt.transport_internal_resends == 0;
  auto count = [](const std::optional<Count>& value) -> std::optional<std::uint64_t> {
    return value ? std::optional(value->value) : std::nullopt;
  };
  auto extra = [&](std::string_view key) -> std::optional<std::uint64_t> {
    const auto found = usage.extra.find(std::string(key));
    return found == usage.extra.end() ? std::nullopt : std::optional(found->second.value);
  };
  auto& priced = outcome.usage;
  priced.total_input = count(usage.input_total);
  priced.uncached_input = count(usage.input_uncached);
  priced.cache_read_input = count(usage.cache_read);
  priced.cache_write_5m_input = extra("cache_creation.ephemeral_5m_input_tokens");
  priced.cache_write_1h_input = extra("cache_creation.ephemeral_1h_input_tokens");
  // An explicitly reported zero aggregate proves both write bands are zero.
  // A nonzero aggregate without TTL split is deliberately not classified.
  if (usage.cache_write && usage.cache_write->value == 0) {
    if (!priced.cache_write_5m_input) priced.cache_write_5m_input = 0;
    if (!priced.cache_write_1h_input) priced.cache_write_1h_input = 0;
  }
  priced.output = count(usage.output_total);
  priced.thinking = count(usage.reasoning); // inclusive subset, never added
  return outcome;
}
inline runtime::Result not_run_result(const Case& item) {
  Failure failure;
  failure.error.kind = item.failure_kind.value_or(ErrorKind::InvalidRequest);
  failure.error.retry_class = RetryClass::Never;
  failure.error.retry_safety = RetrySafety::NotSent;
  failure.error.safe_message = "qualification pre-dispatch admission denied";
  return std::make_shared<const sp::Outcome>(std::move(failure));
}
class Qualification {
 public:
  explicit Qualification(std::string root) : root_(std::move(root)) {}
  bool admit(const Profile& profile, const runtime::PreparedRequest& prepared,
             bool streaming, Case& item, Report& report) {
    if (!prepared.valid() || prepared.error()) {
      item.state = State::NotRun;
      if (item.reason == Reason::None) item.reason = Reason::RuntimeFailure;
      if (prepared.error()) item.failure_kind = prepared.error()->kind;
      return false;
    }
    if (!meter_) {
      auto opened = qualification::Meter::open(root_);
      if (auto* error = std::get_if<qualification::Error>(&opened)) return deny(*error, item, report);
      meter_ = std::get<std::unique_ptr<qualification::Meter>>(std::move(opened));
    }
    const auto* model = meter_->catalog().find(prepared.model(), prepared.family());
    if (!model) return deny({qualification::Denial::UnknownModel, {}}, item, report);
    const auto output = profile.bounds().output_tokens;
    // Model context and max-input are hard bounds, not character estimates.
    // The caller's output cap is never clamped and INCLUDES thinking.
    if (output > model->max_output || output > model->context_window ||
        (!profile.loopback() && profile.bounds().input_tokens < model->max_input))
      return deny({qualification::Denial::Bounds, {}}, item, report);
    const auto input = std::min({profile.bounds().input_tokens, model->max_input,
                               model->context_window - output});
    std::array<unsigned char, 32> nonce{};
    if (!crypto::random_bytes(nonce.data(), nonce.size())) fail();
    const auto identity = digest({std::string_view(reinterpret_cast<const char*>(nonce.data()), nonce.size())});
    const auto fingerprint = digest({prepared.family(), prepared.model(), profile.origin(),
        prepared.encoded_body(), streaming ? "sse" : "buffered", item.name,
        report.mode == ExecutionMode::BaselinePair ? "baseline-pair" :
            report.mode == ExecutionMode::GoogleDiagnostics ? "google-diagnostics" : "full",
        std::to_string(prepared.deadline().time_since_epoch().count()),
        "qualification:no-provider-or-semantic-retries", std::to_string(input), std::to_string(output)});
    auto reserved = meter_->reserve({identity, fingerprint, std::string(prepared.model()),
                                    std::string(prepared.family()), input, output});
    if (auto* error = std::get_if<qualification::Error>(&reserved)) return deny(*error, item, report);
    claim_.emplace(std::get<qualification::Claim>(std::move(reserved)));
    body_digest_ = digest({prepared.encoded_body()});
    item.reserved_micro_usd = claim_->reserved_micro_usd();
    item.settlement = qualification::SettlementKind::UnknownHold;
    refresh(report);
    return true;
  }
  void dispatch(std::string_view body, Case& item) {
    // Each preparation has exactly one granted attempt. Unexpected automatic
    // retries or wire edits cannot reuse its durable authority.
    if (!claim_ || item.attempts || digest({body}) != body_digest_) {
      item.reason = Reason::RetentionMismatch; fail();
    }
    ++item.attempts;
  }
  void settle(const runtime::Result& result, Case& item, Report& report) {
    if (!claim_) return;
    auto settled = meter_->settle(*claim_, metered_outcome(result));
    claim_.reset();
    if (auto* error = std::get_if<qualification::Error>(&settled)) {
      deny(*error, item, report); fail();
    }
    const auto& settlement = std::get<qualification::Settlement>(settled);
    item.settlement = settlement.kind;
    item.charged_micro_usd = settlement.kind == qualification::SettlementKind::UnknownHold
        ? std::nullopt : std::optional(settlement.charged_micro_usd);
    report.meter_totals = settlement.totals;
  }
 private:
  void refresh(Report& report) {
    auto totals = meter_->totals();
    if (auto* value = std::get_if<qualification::Totals>(&totals)) report.meter_totals = *value;
    else report.meter_totals.reset();
  }
  bool deny(qualification::Error error, Case& item, Report& report) {
    item.admission_denial = error.code; item.state = State::NotRun;
    item.reason = error.code == qualification::Denial::Calls ? Reason::CallBudget :
        error.code == qualification::Denial::Money ? Reason::CostBudget : Reason::LedgerFailure;
    if (meter_) refresh(report);
    return false;
  }
  std::string root_, body_digest_;
  std::unique_ptr<qualification::Meter> meter_;
  std::optional<qualification::Claim> claim_;
};
} // namespace sp::canary::detail
