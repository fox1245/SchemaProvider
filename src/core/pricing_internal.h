#pragma once
// Exact money arithmetic and the price unit reader the pricing sources share (pricing.cpp, endpoint_pricing.cpp).
// Not installed: only the library's own sources include it.
#include "descriptor/policy.h"
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>

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
}  // namespace sp::pricing::detail
