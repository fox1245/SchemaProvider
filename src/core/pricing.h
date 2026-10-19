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
  // The dearer rates a call may bill: a 1-hour cache write (`cache_write_1h`) and an image output token
  // (`output_image`).
  std::optional<std::uint64_t> cache_write_1h, output_image;
  // A routing gateway's route ceiling, the dearest of its endpoints' prices (`ceiling_input`, `ceiling_output`,
  // `ceiling_request`). A host pins it as OpenRouter's `provider.max_price`.
  std::optional<std::uint64_t> ceiling_input, ceiling_output, ceiling_request;
  // The fee per use of a server tool, by tool type (`tool:<type>`), in nano-USD per million uses, sorted by type.
  // A listed "0" is a declared zero.
  std::vector<std::pair<std::string, std::uint64_t>> tool_fees;
  bool empty() const noexcept {
    return !input && !output && !cache_read && !cache_write && !reasoning && !request && !cache_write_1h &&
           !output_image && !ceiling_input && !ceiling_output && !ceiling_request && tool_fees.empty();
  }
};
// The entry's prices for a request in `service_tier` with `input_tokens` input tokens (cache reads and writes
// included). A unit is `<name>[/<tier>][+<tokens>]`:
// - `/<tier>` prices a service tier (`input/flex`, `output/priority`). With a tier only its units count, so a tier
//   the entry does not price has no price and a priority tier is never billed at the standard rates.
// - `+<tokens>` prices requests above that many input tokens (`input+200000`); the highest band the request is
//   above wins, unit by unit, over the units without one.
ModelPrice price_of(const descriptor::CatalogueEntry& entry, std::string_view service_tier = {},
                    std::uint64_t input_tokens = 0);
// Each unit at its dearest over every band a request of at most `input_tokens` input tokens may fall in: the
// prices to reserve with.
ModelPrice dearest_price_of(const descriptor::CatalogueEntry& entry, std::string_view service_tier,
                            std::uint64_t input_tokens);

struct CostEstimate {
  // Set when every counter the call billed has a price. Rounded up once, to whole nano-USD.
  std::optional<UsdAmount> total;
  // The catalogue entry that priced the call.
  std::string entry;
  // The served model had no priced entry, so the requested model's priced it.
  bool requested_entry = false;
  // Rules applied where the entry or the usage says less than the bill: "cache_read at input", "cache_write at
  // input", "cache_write_1h at cache_write", and the dearer rates "cache_write at cache_write_1h", "output at
  // output_image" and "route ceiling".
  std::vector<std::string> assumptions;
  // Why there is no total: "model" (no priced entry), "ceiling" (no route ceiling), or a counter the call billed
  // with no price or no count ("input", "output", "cache_write_1h", "output_image").
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
// - Cache writes: Messages' 5-minute writes at `cache_write` and 1-hour writes at `cache_write_1h`. Without that
//   split, all of them at the dearer rate.
// - Image output: Gemini's image output tokens (Generate `candidatesTokensDetails.IMAGE`, Interactions
//   `output_tokens_by_modality.image`) at `output_image`. Without a modality breakdown, all output that is not
//   reasoning at the dearer of `output` and `output_image`.
// - `request` is charged once.
// Server tool fees are not priced. With a `service_tier`, the entry's prices for that tier, and the input band of the
// call's input tokens (`price_of`). With `route_ceiling` (a routing gateway whose served endpoint is unknown),
// every input token at `ceiling_input`, every output token at `ceiling_output`, and the dearer of `request` and
// `ceiling_request`; an entry without a ceiling has no total ("ceiling").
CostEstimate estimate(const descriptor::ModelCatalogue& catalogue, std::string_view family, std::string_view origin,
                      const ServedModel& model, const Usage& usage, std::string_view service_tier = {},
                      bool route_ceiling = false);

// What a call may use: its admitted input limit, its output cap, and at most `uses` of each server tool type.
struct BoundRequest {
  std::uint64_t input_tokens = 0, output_tokens = 0;
  // Price at the route ceiling (a routing gateway), not at the listed price.
  bool route_ceiling = false;
  std::vector<std::pair<std::string, std::uint64_t>> tool_uses;
};
struct Bound {
  // The largest amount a call within the request can bill, rounded up once to whole nano-USD.
  std::optional<UsdAmount> total;
  // Why there is no total: "input" or "output" (no rate), "ceiling" (no route ceiling), "tool:<type>" (no fee),
  // or "overflow".
  std::vector<std::string> unpriced;
};
// A reservation bound, from `dearest_price_of` when the entry prices input bands: input tokens at the dearest input rate (input, cache reads and writes, the 1-hour write),
// output tokens at the dearest output rate (output, reasoning, image output), the request fee, and each tool's fee
// times its uses. With `route_ceiling` the ceiling rates replace the listed ones. For usage within the request,
// `estimate` with the same prices never exceeds it. Messages compaction iterations bill tokens outside the call's
// counters and are not bounded here.
Bound upper_bound(const ModelPrice& price, const BoundRequest& request);

} // namespace sp::pricing
