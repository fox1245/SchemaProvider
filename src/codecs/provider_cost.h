#pragma once
#include "core/value.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

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
  // The parser keeps no lexeme, so recover the shortest decimal that round-trips to the
  // parsed binary64 (the provider's own decimal whenever it had at most 17 significant
  // digits) and convert that decimal exactly. Only a nonzero digit beyond the ninth
  // fractional place rounds up: 1.5e-06 is 1500 nanoUSD, 1e-10 is 1.
  char text[32];
  const auto written = std::to_chars(text, text + sizeof text, amount, std::chars_format::scientific);
  if (written.ec != std::errc{}) return CostStatus::Malformed;
  uint64_t significand = 0;
  int digits = 0, exponent = 0;
  const char* cursor = text;
  for (; cursor != written.ptr && *cursor != 'e'; ++cursor)
    if (*cursor != '.') { significand = significand * 10 + static_cast<uint64_t>(*cursor - '0'); ++digits; }
  if (cursor == written.ptr || std::from_chars(cursor + (cursor[1] == '+' ? 2 : 1), written.ptr, exponent).ec != std::errc{})
    return CostStatus::Malformed;
  // amount == significand * 10^(exponent - digits + 1), so nanoUSD == significand * 10^scale.
  const int scale = exponent - digits + 1 + 9;
  uint64_t nanos = significand;
  if (scale >= 0) {
    for (int i = 0; i < scale; ++i) nanos *= 10;  // bounded by the 2^53 nanoUSD precheck above
  } else {
    uint64_t divisor = 1;
    for (int i = 0; i < -scale && divisor <= significand; ++i) divisor *= 10;
    nanos = divisor > significand ? 1 : significand / divisor + (significand % divisor ? 1U : 0U);
  }
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
