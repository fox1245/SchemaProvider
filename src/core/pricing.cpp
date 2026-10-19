#include "core/pricing.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace sp::pricing {
namespace {
// Decimal USD per unit as nano-USD per million units (USD x 1e15), rounded up past the fifteenth fractional digit.
std::optional<std::uint64_t> per_million(std::string_view text) {
  if (text.empty() || text.front() == '-') return std::nullopt;
  const auto dot = text.find('.');
  const auto whole = text.substr(0, dot);
  const auto fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  constexpr unsigned scale = 15;
  unsigned __int128 value = 0;
  for (const char c : whole) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + static_cast<unsigned>(c - '0');
    if (value > std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
  }
  bool remainder = false;
  for (unsigned i = 0; i < scale; ++i) {
    const char c = i < fraction.size() ? fraction[i] : '0';
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + static_cast<unsigned>(c - '0');
  }
  for (std::size_t i = scale; i < fraction.size(); ++i) {
    if (fraction[i] < '0' || fraction[i] > '9') return std::nullopt;
    remainder = remainder || fraction[i] != '0';
  }
  if (remainder) ++value;
  if (value > std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
  return static_cast<std::uint64_t>(value);
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
  unsigned __int128 sum = 0;  // nano-USD x 1e6
  bool complete = true;
  auto charge = [&](std::uint64_t units, const std::optional<std::uint64_t>& rate, std::string_view name) {
    if (!units) return;
    if (!rate) { out.unpriced.emplace_back(name); complete = false; return; }
    sum += static_cast<unsigned __int128>(units) * *rate;
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
  const auto nano = (sum + 999'999) / 1'000'000;
  if (nano > std::numeric_limits<std::uint64_t>::max()) { out.unpriced.emplace_back("overflow"); return out; }
  out.total = UsdAmount{static_cast<std::uint64_t>(nano), Evidence::Derived, CostRounding::CeilingCatalogueProduct};
  return out;
}

} // namespace sp::pricing
