#pragma once
// Prices from the model catalogue, and an estimate of a call's cost from its usage. The provider's own figure is
// Usage::provider_cost; this is the catalogue's, for a provider that reports none or a host that budgets ahead.
#include "core/value.h"
#include "descriptor/catalogue.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sp::pricing {

// Exact nano-USD per million units: per million tokens, and for `request` per million requests. Parsed from the
// entry's decimal USD per unit. OpenRouter's unit names (prompt, completion, input_cache_read, input_cache_write,
// internal_reasoning) and the plain ones (input, output, cache_read, cache_write, reasoning) name the same units;
// the plain name wins when both are present. A negative listed price (OpenRouter's "-1", a variable price) is no
// price.
struct ModelPrice {
  std::optional<std::uint64_t> input, output, cache_read, cache_write, reasoning, request;
  bool empty() const noexcept { return !input && !output && !cache_read && !cache_write && !reasoning && !request; }
};
ModelPrice price_of(const descriptor::CatalogueEntry& entry);

struct CostEstimate {
  // Set when every counter the call billed has a price. Rounded up once, to whole nano-USD.
  std::optional<UsdAmount> total;
  // The catalogue entry that priced the call.
  std::string entry;
  // The served model had no priced entry, so the requested model's priced it.
  bool requested_entry = false;
  // Rules applied where the entry gives no price: "cache_read at input", "cache_write at input",
  // "reasoning at output".
  std::vector<std::string> assumptions;
  // Why there is no total: "model" (no priced entry), or a counter the call billed with no price or no count
  // ("input", "output").
  std::vector<std::string> unpriced;
};

// Prices `usage` by the catalogue entry of the served model, else of the requested one. Never fetches: refresh the
// catalogue first (transport::refresh_catalogue) when prices may have changed.
// - Input: input_uncached at `input`, cache reads at `cache_read` and cache writes at `cache_write`. Without
//   input_uncached it is input_total less the cache counters.
// - Output: with a `reasoning` price, reasoning tokens at it and the rest of output_total at `output`; otherwise
//   all of output_total at `output`.
// - Messages: the compaction iterations (Usage::extra `compaction.input_tokens`, `compaction.output_tokens` and
//   the cache counters) at the same rates, since the top-level counters leave them out.
// - `request` is charged once.
// Server tool fees, images and other per-unit charges are not priced.
CostEstimate estimate(const descriptor::ModelCatalogue& catalogue, std::string_view family, std::string_view origin,
                      const ServedModel& model, const Usage& usage);

} // namespace sp::pricing
