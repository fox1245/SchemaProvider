#pragma once
// Exact money arithmetic and the price unit reader the pricing sources share (pricing.cpp, endpoint_pricing.cpp).
// Not installed: only the library's own sources include it.
#include "core/pricing.h"
#include "descriptor/policy.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sp::pricing::detail {
// An unsigned 128-bit value in two 64-bit halves (MSVC has no 128-bit integer). `overflow` records a carry out of
// 128 bits.
struct Wide {
  std::uint64_t high = 0, low = 0;
  bool overflow = false;

  static Wide product(std::uint64_t a, std::uint64_t b) {
    constexpr std::uint64_t mask = 0xffffffffu;
    const std::uint64_t ll = (a & mask) * (b & mask), lh = (a & mask) * (b >> 32), hl = (a >> 32) * (b & mask),
                        hh = (a >> 32) * (b >> 32);
    const std::uint64_t middle = (ll >> 32) + (lh & mask) + (hl & mask);
    return {hh + (lh >> 32) + (hl >> 32) + (middle >> 32), (middle << 32) | (ll & mask), false};
  }
  Wide& operator+=(const Wide& other) {
    const auto old_low = low, old_high = high;
    low += other.low;
    const std::uint64_t carry = low < old_low ? 1 : 0;
    high += other.high;
    const bool wrapped = high < old_high;
    const auto before_carry = high;
    high += carry;
    overflow = overflow || other.overflow || wrapped || high < before_carry;
    return *this;
  }
  Wide& operator+=(std::uint64_t value) { return *this += Wide{0, value, false}; }
  bool fits() const { return !overflow && high == 0; }
  // The quotient by a divisor below 2^32, by 32-bit long division.
  Wide divided(std::uint32_t divisor) const {
    const std::uint64_t limbs[4] = {high >> 32, high & 0xffffffffu, low >> 32, low & 0xffffffffu};
    std::uint64_t quotient[4] = {}, remainder = 0;
    for (int i = 0; i < 4; ++i) {
      const auto current = (remainder << 32) | limbs[i];
      quotient[i] = current / divisor;
      remainder = current % divisor;
    }
    return {(quotient[0] << 32) | quotient[1], (quotient[2] << 32) | quotient[3], overflow};
  }
};

// A sum of rates times quantities (nano-USD per million units times units) rounded up once to whole nano-USD; nothing
// when the amount does not fit 64 bits.
inline std::optional<std::uint64_t> rounded_nano_usd(Wide sum) {
  sum += 999'999;
  const auto nano = sum.divided(1'000'000);
  if (!nano.fits()) return std::nullopt;
  return nano.low;
}

// An amount of USD that a JSON number states (a `provider.max_price` member) as whole nano-USD, rounded up from the
// number's exact binary value, so that no amount a reader of the number may take is above it. Nothing for a negative
// or non-finite number and for an amount above max_price_limit_nano_usd.
inline std::optional<std::uint64_t> nano_usd_rounded_up(double usd) {
  constexpr std::uint64_t nano = 1'000'000'000;
  if (!std::isfinite(usd) || usd < 0) return std::nullopt;
  if (usd == 0) return 0;
  // usd = mantissa / 2^shift, exactly: frexp and ldexp only move the exponent.
  int exponent = 0;
  const auto mantissa = static_cast<std::uint64_t>(std::ldexp(std::frexp(usd, &exponent), 53));
  const int shift = 53 - exponent;
  if (shift <= 0) return std::nullopt;  // at least 2^52 USD
  // The quotient of mantissa x 1e9 (below 2^83) by 2^shift, and whether it leaves a remainder.
  const auto product = Wide::product(mantissa, nano);
  std::uint64_t quotient = 0;
  bool remainder = false;
  if (shift >= 128) {
    remainder = true;
  } else if (shift >= 64) {
    const int rest = shift - 64;
    quotient = rest ? product.high >> rest : product.high;
    remainder = product.low != 0 || (rest && (product.high & ((std::uint64_t{1} << rest) - 1)) != 0);
  } else {
    if (product.high >> shift) return std::nullopt;
    quotient = (product.low >> shift) | (product.high << (64 - shift));
    remainder = (product.low & ((std::uint64_t{1} << shift) - 1)) != 0;
  }
  if (remainder) ++quotient;
  if (quotient > max_price_limit_nano_usd) return std::nullopt;
  return quotient;
}
// Whether a cap read as `cap` nano-USD (nano_usd_rounded_up) is at most `amount` nano-USD as a host writes it: at most
// the amount, or the very value the JSON number nearest `amount` / 1e9 USD reads as, which is the number a host that
// writes the amount sends (0.02 USD reads as 20000001).
inline bool cap_within(std::uint64_t cap, std::uint64_t amount) {
  if (cap <= amount) return true;
  if (amount > max_price_limit_nano_usd) return false;
  // Both are exact in a double below 2^53, and the quotient is rounded to the nearest double.
  return nano_usd_rounded_up(static_cast<double>(amount) / 1e9) == cap;
}

// A price unit: its name (`input`, `tool:web_search`), its service tier (`/flex`) and the input length above which
// it applies (`+200000`, 0 for every request).
struct Unit {
  std::string_view name, tier;
  std::uint64_t above = 0;
};
inline std::optional<Unit> parse_unit(std::string_view unit) {
  Unit out;
  if (const auto plus = unit.rfind('+'); plus != std::string_view::npos) {
    const auto digits = unit.substr(plus + 1);
    if (digits.empty() || digits.size() > descriptor::max_band_digits) return std::nullopt;
    for (const char c : digits) {
      if (c < '0' || c > '9') return std::nullopt;
      out.above = out.above * 10 + static_cast<std::uint64_t>(c - '0');
    }
    unit = unit.substr(0, plus);
  }
  if (const auto slash = unit.rfind('/'); slash != std::string_view::npos) {
    out.tier = unit.substr(slash + 1);
    unit = unit.substr(0, slash);
  }
  if (unit.empty() || (unit.starts_with(descriptor::tool_fee_prefix) && unit.size() == descriptor::tool_fee_prefix.size())) return std::nullopt;
  out.name = unit;
  return out;
}

inline constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
inline bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a != 0 && b > kMax / a) return false;
  out = a * b;
  return true;
}
inline bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (b > kMax - a) return false;
  out = a + b;
  return true;
}
// The smallest of the figures that state a limit; a zero is no limit.
inline std::optional<std::uint64_t> smallest_limit(std::initializer_list<std::optional<std::uint64_t>> values) {
  std::optional<std::uint64_t> result;
  for (const auto& value : values)
    if (value && *value > 0 && (!result || *value < *result)) result = value;
  return result;
}

// Price keys by the plain name of their unit (descriptor::plain_price_unit; `tool:<type>` for a tool fee).
using UnitKeys = std::vector<std::pair<std::string, std::string_view>>;
inline std::string_view key_of(const UnitKeys& keys, std::string_view unit) noexcept {
  const auto found = std::find_if(keys.begin(), keys.end(), [&](const auto& key) { return key.first == unit; });
  return found == keys.end() ? std::string_view{} : found->second;
}
// The entry's prices for a request in a service tier with some input tokens, as price_of (`dearest` false) and
// dearest_price_of (`dearest` true) read them, and what those leave out. A row applies to the request when it is of
// the tier and the request is above its band (for `dearest`, may reach it).
struct Selection {
  // Each unit with a fixed price, of the highest band that applies or the dearest; a unit with a row that applies at a
  // price that is not a fixed amount has none, whatever its other rows.
  ModelPrice price;
  // The same for the image tokens of an input (`input_image`), which ModelPrice does not hold.
  std::optional<std::uint64_t> input_image;
  // The key of the row that gave each price.
  UnitKeys keys;
  // Each unit with a row that applies at a price that is not a fixed amount (a variable price), with the first such row.
  UnitKeys variable;
  // Whether some unit ModelPrice holds has a row that applies at a fixed price: whether the entry prices the request
  // in the tier at all.
  bool priced = false;
  bool is_variable(std::string_view unit) const noexcept { return !key_of(variable, unit).empty(); }
};
// The keys point into `entry`, which must outlive the selection.
Selection select(const descriptor::CatalogueEntry& entry, std::string_view service_tier, std::uint64_t input_tokens, bool dearest);

// How usage_sum reads a usage.
struct UsageRules {
  // A side whose counter the usage lacks has no price: the input (input_total or input_uncached) and the output
  // (output_total). Off for a side the call does not count; a counter the usage reports is priced either way.
  bool input = true, output = true;
  // Messages compaction iterations bill at the model's rates beside the top-level counters.
  bool compaction = false;
  // Every token at the route ceiling (estimate's `route_ceiling`).
  bool route_ceiling = false;
  // The image tokens among the input, when the caller counts them: carved out of the uncached input and billed at
  // `input_image`, as audio input is at `input_audio`.
  std::optional<std::uint64_t> input_image;
  // The request may have carried an image (#237): the usage's uncached input that is not audio may then be image
  // tokens, when the usage states no count of them (`input_image` here, an image counter, a modality breakdown of the
  // input or the OpenAI realtime counters), and bills at the dearer of `input` and `input_image`.
  bool image_input = false;
  // At the route ceiling: a ceiling the entry lists is below a token price the same entry lists on its side (a ceiling
  // written before that price was listed, or kept from an earlier refresh), so it bounds nothing.
  bool stale_ceiling = false;
};
// One part of a usage's sum: tokens billed at the rate of `unit` (a plain name: `cache_read`, `ceiling_input`,
// `request` for the fee), from the price key `key`.
struct UsageCharge {
  std::string_view unit, key;
  std::uint64_t quantity = 0, rate = 0;
};
struct UsageSum {
  // The exact sum, in nano-USD x 1e6, not rounded.
  Wide sum;
  // Every counter the usage bills has a price and a count; otherwise `unpriced` names why, and `sum` is partial.
  bool complete = true;
  std::vector<std::string> assumptions, unpriced;
  std::vector<UsageCharge> charges;
};
// The token part and the request fee of `estimate`, with its rules, at the prices of `selection` (price_of): summed
// exactly and not rounded, so a caller adds other parts before rounding once.
UsageSum usage_sum(const Selection& selection, const Usage& usage, const UsageRules& rules);
// The token units a chat request's bound and estimate read (by the plain name descriptor::plain_price_unit gives), and
// the tool fees (`tool:<type>`).
inline constexpr std::string_view chat_units[]{"input", "cache_read", "cache_write", "cache_write_1h", "input_audio",
                                               "output", "reasoning", "output_image", "output_audio", "request", "cache_read_audio",
                                               "input_image"};
// A unit a chat request is priced by that `entry` lists at the standard rates and not under `tier`, in any band: under
// `tier` such a unit would be priced at another unit's rate, or not at all, which is not the tier's price for it.
// Empty when every such unit has a row in the tier, and when `tier` is empty or the entry prices nothing under it (which
// a bound refuses as a tier the entry does not price).
inline std::string_view unit_outside_tier(const descriptor::CatalogueEntry& entry, std::string_view tier) {
  if (tier.empty() || std::none_of(entry.pricing.begin(), entry.pricing.end(), [&](const auto& price) {
        const auto unit = parse_unit(price.first);
        return unit && unit->tier == tier;
      }))
    return {};
  const auto read = [](std::string_view name) {
    return name.starts_with(descriptor::tool_fee_prefix) || std::find(std::begin(chat_units), std::end(chat_units), name) != std::end(chat_units);
  };
  for (const auto& [key, text] : entry.pricing) {
    const auto unit = parse_unit(key);
    if (!unit || !unit->tier.empty()) continue;
    const auto name = descriptor::plain_price_unit(unit->name);
    if (!read(name)) continue;
    const bool in_tier = std::any_of(entry.pricing.begin(), entry.pricing.end(), [&](const auto& other) {
      const auto row = parse_unit(other.first);
      return row && row->tier == tier && descriptor::plain_price_unit(row->name) == name;
    });
    if (!in_tier) return name;
  }
  return {};
}
}  // namespace sp::pricing::detail
