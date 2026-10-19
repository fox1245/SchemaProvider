#include "core/pricing.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace sp::pricing {
namespace {
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
  // value * 10 + digit
  Wide shifted(unsigned digit) const {
    auto result = product(low, 10);
    const auto top = product(high, 10);
    result.overflow = overflow || top.high != 0 || result.high > std::numeric_limits<std::uint64_t>::max() - top.low;
    result.high += top.low;
    result += digit;
    return result;
  }
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

// Decimal USD per unit as nano-USD per million units (USD x 1e15), rounded up past the fifteenth fractional digit.
std::optional<std::uint64_t> per_million(std::string_view text) {
  if (text.empty() || text.front() == '-') return std::nullopt;
  const auto dot = text.find('.');
  const auto whole = text.substr(0, dot);
  const auto fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  constexpr unsigned scale = 15;
  Wide value;
  for (const char c : whole) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value.shifted(static_cast<unsigned>(c - '0'));
    if (!value.fits()) return std::nullopt;
  }
  bool remainder = false;
  for (unsigned i = 0; i < scale; ++i) {
    const char c = i < fraction.size() ? fraction[i] : '0';
    if (c < '0' || c > '9') return std::nullopt;
    value = value.shifted(static_cast<unsigned>(c - '0'));
  }
  for (std::size_t i = scale; i < fraction.size(); ++i) {
    if (fraction[i] < '0' || fraction[i] > '9') return std::nullopt;
    remainder = remainder || fraction[i] != '0';
  }
  if (remainder) value += 1;
  if (!value.fits()) return std::nullopt;
  return value.low;
}
} // namespace

ModelPrice price_of(const descriptor::CatalogueEntry& entry) {
  ModelPrice out;
  auto read = [&](std::optional<std::uint64_t>& target, std::string_view plain, std::string_view listed) {
    std::optional<std::uint64_t> from_listed;
    for (const auto& [unit, text] : entry.pricing) {
      if (unit == plain) { target = per_million(text); return; }
      if (unit == listed) from_listed = per_million(text);
    }
    target = from_listed;
  };
  read(out.input, "input", "prompt");
  read(out.output, "output", "completion");
  read(out.cache_read, "cache_read", "input_cache_read");
  read(out.cache_write, "cache_write", "input_cache_write");
  read(out.reasoning, "reasoning", "internal_reasoning");
  read(out.request, "request", "request");
  return out;
}

CostEstimate estimate(const descriptor::ModelCatalogue& catalogue, std::string_view family, std::string_view origin,
                      const ServedModel& model, const Usage& usage) {
  CostEstimate out;
  const descriptor::CatalogueEntry* entry = nullptr;
  ModelPrice price;
  auto priced = [&](std::string_view name) {
    if (name.empty()) return false;
    const auto match = catalogue.find(family, origin, name);
    if (!match.entry) return false;
    auto found = price_of(*match.entry);
    if (found.empty()) return false;
    entry = match.entry; price = found;
    return true;
  };
  const bool served = model.relation != ServedRelation::NotReported && !model.served.empty() && model.served != model.requested;
  const bool by_served = served && priced(model.served);
  if (!by_served && !priced(model.requested)) {
    out.unpriced.push_back("model");
    return out;
  }
  out.entry = entry->id;
  out.requested_entry = served && !by_served;
  auto count = [](const std::optional<Count>& c) -> std::optional<std::uint64_t> { return c ? std::optional<std::uint64_t>(c->value) : std::nullopt; };
  const auto cache_read = count(usage.cache_read).value_or(0), cache_write = count(usage.cache_write).value_or(0);
  std::optional<std::uint64_t> uncached = count(usage.input_uncached);
  if (!uncached && usage.input_total) {
    const auto total = usage.input_total->value;
    uncached = total >= cache_read + cache_write ? total - cache_read - cache_write : 0;
  }
  Wide sum;  // nano-USD x 1e6
  bool complete = true;
  auto charge = [&](std::uint64_t units, const std::optional<std::uint64_t>& rate, std::string_view name) {
    if (!units) return;
    if (!rate) { out.unpriced.emplace_back(name); complete = false; return; }
    sum += Wide::product(units, *rate);
  };
  if (!uncached) { out.unpriced.emplace_back("input"); complete = false; }
  else charge(*uncached, price.input, "input");
  auto fallback = [&](const std::optional<std::uint64_t>& specific, const std::optional<std::uint64_t>& general, std::string_view rule, std::uint64_t units) {
    if (specific || !units) return specific;
    if (general && std::find(out.assumptions.begin(), out.assumptions.end(), rule) == out.assumptions.end()) out.assumptions.emplace_back(rule);
    return general;
  };
  charge(cache_read, fallback(price.cache_read, price.input, "cache_read at input", cache_read), "cache_read");
  charge(cache_write, fallback(price.cache_write, price.input, "cache_write at input", cache_write), "cache_write");
  if (!usage.output_total) { out.unpriced.emplace_back("output"); complete = false; }
  else {
    const auto output = usage.output_total->value;
    const auto reasoning = std::min(count(usage.reasoning).value_or(0), output);
    if (price.reasoning && reasoning) {
      charge(reasoning, price.reasoning, "reasoning");
      charge(output - reasoning, price.output, "output");
    } else charge(output, price.output, "output");
  }
  // A Messages compaction iteration is billed at the model's rates beside the top-level counters.
  if (family == "anthropic.messages") {
    const auto extra = [&](std::string_view counter) -> std::uint64_t {
      const auto found = usage.extra.find("compaction." + std::string(counter));
      return found == usage.extra.end() ? 0 : found->second.value;
    };
    const auto read = extra("cache_read_input_tokens"), write = extra("cache_creation_input_tokens");
    charge(extra("input_tokens"), price.input, "input");
    charge(read, fallback(price.cache_read, price.input, "cache_read at input", read), "cache_read");
    charge(write, fallback(price.cache_write, price.input, "cache_write at input", write), "cache_write");
    charge(extra("output_tokens"), price.output, "output");
  }
  if (price.request) sum += *price.request;
  if (!complete) return out;
  sum += 999'999;
  const auto nano = sum.divided(1'000'000);
  if (!nano.fits()) { out.unpriced.emplace_back("overflow"); return out; }
  out.total = UsdAmount{nano.low, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
  return out;
}

} // namespace sp::pricing
