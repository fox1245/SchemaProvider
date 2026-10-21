#include "core/failure.h"
#include <algorithm>

namespace sp {
bool usage_reported_nothing(const Usage& usage) noexcept {
  const auto& cost = usage.provider_cost;
  return usage.stage == UsageStage::Missing && usage.quality == UsageQuality::Consistent && !usage.input_total &&
         !usage.output_total && !usage.total && !usage.provider_reported_total && !usage.input_uncached &&
         !usage.cache_read && !usage.cache_write && !usage.reasoning && usage.extra.empty() && usage.conflicts.empty() &&
         !cost.total && !cost.upstream_total && !cost.upstream_input && !cost.upstream_output && !cost.is_byok &&
         cost.byok_status == CostStatus::Missing && cost.source == CostSource::None &&
         cost.quality == UsageQuality::Consistent &&
         std::all_of(cost.status.begin(), cost.status.end(), [](CostStatus status) { return status == CostStatus::Missing; });
}
bool request_declined(const Failure& failure) noexcept {
  const auto& attempt = failure.error.attempt;
  const auto& partial = failure.partial;
  return failure.error.retry_safety == RetrySafety::RejectedBeforeOutput && attempt.request_may_have_left &&
         attempt.response_head_seen && attempt.transport_internal_resends == 0 && !attempt.prior_usage_unknown &&
         partial.messages.empty() && !partial.stop && !partial.wire_envelope &&
         std::all_of(partial.raw_events.begin(), partial.raw_events.end(),
                     [](const RawWire& event) { return event.type == http_error_event; }) &&
         usage_reported_nothing(partial.usage);
}
} // namespace sp
