#pragma once
#include "core/value.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <bit>
#include <cmath>
#include <limits>

namespace sp::codecs {
// These are monetary/boolean metadata, never integer token-counter extras.
inline bool monetary_member(std::string_view key) {
  return key == "cost" || key == "cost_details" || key == "is_byok";
}
inline CostStatus usd_amount(json::Value value, bool usd, std::optional<UsdAmount>& target) {
  target.reset();
  if (!value.valid() || value.is_null()) return CostStatus::Missing;
  if (!value.is_number()) return CostStatus::Malformed;
  const double amount = value.as_double();
  if (!std::isfinite(amount) || amount < 0) return CostStatus::Malformed;
  if (!usd) return CostStatus::UnknownCurrency;
  if (amount >= 18446744073.709551616) return CostStatus::Overflow;
  // Above 2^53 nanoUSD binary64 cannot consistently resolve one nanoUSD.
  if (amount > 9007199.254740992) return CostStatus::PrecisionExceeded;
  if (amount == 0) { target = UsdAmount{}; return CostStatus::Available; }
  static_assert(sizeof(double) == sizeof(uint64_t) && std::numeric_limits<double>::is_iec559);
  const uint64_t bits = std::bit_cast<uint64_t>(amount);
  const unsigned exponent = static_cast<unsigned>((bits >> 52) & 0x7ffU);
  const uint64_t mantissa = (bits & ((uint64_t{1} << 52) - 1)) | (exponent ? uint64_t{1} << 52 : 0);
  // Exact binary64 * 1e9 = mantissa * 1953125 / 2^shift. Two stack
  // limbs avoid nonportable __int128 and floating multiplication rounding.
  const uint64_t lower = (mantissa & 0xffffffffU) * 1953125U;
  const uint64_t upper = (mantissa >> 32) * 1953125U;
  const uint64_t low = lower + (upper << 32);
  const uint64_t high = (upper >> 32) + (low < lower ? 1U : 0U);
  const unsigned shift = 1066U - (exponent ? exponent : 1U);
  uint64_t integral = 0;
  bool remainder = true;
  if (shift < 64) {
    integral = (high << (64 - shift)) | (low >> shift);
    remainder = (low & ((uint64_t{1} << shift) - 1)) != 0;
  } else if (shift == 64) {
    integral = high; remainder = low != 0;
  } else if (shift < 128) {
    integral = high >> (shift - 64);
    remainder = low != 0 || (high & ((uint64_t{1} << (shift - 64)) - 1)) != 0;
  }
  const uint64_t nanos = integral + (remainder ? 1U : 0U);
  if (nanos > (uint64_t{1} << 53)) return CostStatus::PrecisionExceeded;
  target = UsdAmount{nanos};
  return CostStatus::Available;
}
inline CostStatus usd_alias_amount(json::Value primary, json::Value alias, bool usd, std::optional<UsdAmount>& target) {
  const auto status = usd_amount(primary, usd, target);
  if (!alias.valid() || alias.is_null()) return status;
  if (status == CostStatus::Missing) return usd_amount(alias, usd, target);
  std::optional<UsdAmount> other;
  const auto other_status = usd_amount(alias, usd, other);
  // Compare the parsed USD values, not their rounded nanoUSD projections:
  // different fractions can otherwise collide in the same rounding bucket.
  if (primary.is_number() && alias.is_number() && primary.as_double() == alias.as_double())
    return status;
  target.reset();
  if (status == CostStatus::Malformed || other_status == CostStatus::Malformed)
    return CostStatus::Malformed;
  return CostStatus::Conflict;
}
inline ProviderReportedCost provider_cost(json::Value usage, const descriptor::ValidatedDescriptor& descriptor) {
  ProviderReportedCost result;
  const auto details = usage.get("cost_details"), byok = usage.get("is_byok");
  const auto total = usage.get("cost");
  if (!total.valid() && !details.valid() && !byok.valid()) return result;
  const bool usd = descriptor::contains(descriptor.family_policy().openrouter_origins, descriptor.base_url());
  result.source = usd ? CostSource::OpenRouterUsd : CostSource::UnknownCurrency;
  const bool chat = descriptor.family() == "openai.chat";
  const std::array<json::Value, 4> fields{total, details.get("upstream_inference_cost"),
      details.get(chat ? "upstream_inference_prompt_cost" : "upstream_inference_input_cost"),
      details.get(chat ? "upstream_inference_completions_cost" : "upstream_inference_output_cost")};
  const std::array<json::Value, 4> aliases{json::Value{}, json::Value{},
      details.get(chat ? "upstream_inference_input_cost" : "upstream_inference_prompt_cost"),
      details.get(chat ? "upstream_inference_output_cost" : "upstream_inference_completions_cost")};
  const std::array<std::optional<UsdAmount>*, 4> targets{&result.total, &result.upstream_total, &result.upstream_input, &result.upstream_output};
  for (size_t i = 0; i < fields.size(); ++i) {
    result.status[i] = usd_alias_amount(fields[i], aliases[i], usd, *targets[i]);
    if (result.status[i] != CostStatus::Available && result.status[i] != CostStatus::Missing)
      result.quality = UsageQuality::Inconsistent;
  }
  if (details.valid() && !details.is_null() && !details.is_object()) {
    result.quality = UsageQuality::Inconsistent;
    for (size_t i = 1; i < result.status.size(); ++i) result.status[i] = CostStatus::Malformed;
  }
  if (byok.valid() && !byok.is_null()) {
    if (byok.is_bool()) { result.is_byok = byok.as_bool(); result.byok_status = CostStatus::Available; }
    else { result.byok_status = CostStatus::Malformed; result.quality = UsageQuality::Inconsistent; }
  }
  return result;
}
} // namespace sp::codecs
