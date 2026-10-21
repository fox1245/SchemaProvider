#pragma once
// Prices from the model catalogue, and an estimate of a call's cost from its usage. The provider's own figure is
// Usage::provider_cost; this is the catalogue's, for a provider that reports none or a host that budgets ahead.
#include "core/request_facts.h"
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
// internal_reasoning, audio, audio_output) and the plain ones (input, output, cache_read, cache_write, reasoning,
// input_audio, output_audio) name the same units; the plain name wins when both are present. A negative listed
// price (OpenRouter's "-1", a variable price) is no price. Other listed units (OpenRouter's `image`, `web_search`)
// are not read: nothing in a usage counts them.
struct ModelPrice {
  std::optional<std::uint64_t> input, output, cache_read, cache_write, reasoning, request;
  // The dearer rates a call may bill: a 1-hour cache write (`cache_write_1h`), an image output token
  // (`output_image`), and audio input and output tokens (`input_audio`, `output_audio`).
  std::optional<std::uint64_t> cache_write_1h, output_image, input_audio, output_audio;
  // A routing gateway's route ceiling, the dearest of its endpoints' prices (`ceiling_input`, `ceiling_output`,
  // `ceiling_request`). A host pins it as OpenRouter's `provider.max_price`.
  std::optional<std::uint64_t> ceiling_input, ceiling_output, ceiling_request;
  // The fee per use of a server tool, by tool type (`tool:<type>`), in nano-USD per million uses, sorted by type.
  // A listed "0" is a declared zero.
  std::vector<std::pair<std::string, std::uint64_t>> tool_fees;
  bool empty() const noexcept {
    return !input && !output && !cache_read && !cache_write && !reasoning && !request && !cache_write_1h &&
           !output_image && !input_audio && !output_audio && !ceiling_input && !ceiling_output && !ceiling_request &&
           tool_fees.empty();
  }
};
// The entry's prices for a request in `service_tier` with `input_tokens` input tokens (cache reads and writes
// included). A unit is `<name>[/<tier>][+<tokens>]`:
// - `/<tier>` prices a service tier (`input/flex`, `output/priority`). With a tier only its units count, so a tier
//   the entry does not price has no price and a priority tier is never billed at the standard rates.
// - `+<tokens>` prices requests above that many input tokens (`input+200000`); the highest band the request is
//   above wins, unit by unit, over the units without one.
// `input_tokens` defaults to 0, which is above no band: the base rates, not the long-context ones. Reserve with
// `dearest_price_of`, or with the `upper_bound` that takes the entry.
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
  // input", "cache_write_1h at cache_write", the dearer rates "cache_write at cache_write_1h", "output at
  // output_image" and "route ceiling", and "<unit> at zero" where the entry prices a unit at zero while the unit it
  // refines ("input" for cache and audio input, "output" for reasoning, image and audio output) has a price.
  std::vector<std::string> assumptions;
  // Why there is no total: "model" (no priced entry), "tier" (an entry priced only outside the asked service
  // tier), "ceiling" (no route ceiling), a counter the call billed with no price or no count ("input", "output",
  // "input_audio", "output_audio", "output_image"), or a usage counter the estimate does not price
  // ("toolUsePromptTokenCount").
  std::vector<std::string> unpriced;
  // The usage was still arriving (UsageStage::Partial) or its counters contradict each other
  // (UsageQuality::Inconsistent): the provider may bill more than `total`.
  bool lower_bound = false;
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
// - Audio: the audio part of the input (Chat `input_audio`, Generate `promptTokensDetails.AUDIO`, Interactions
//   `input_tokens_by_modality.audio`) at `input_audio`, and of the output at `output_audio`. Audio tokens without
//   that rate, or more audio input than uncached input, leave no total ("input_audio", "output_audio").
// - A zero rate for a unit whose base unit has a price is applied and named in `assumptions` ("<unit> at zero").
// - `request` is charged once.
// Server tool fees are not priced. Gemini's `toolUsePromptTokenCount` bills beyond the prompt count and is not
// priced either: a usage that reports it has no total. With a `service_tier`, the entry's prices for that tier, and
// the input band of the call's input tokens (`price_of`); the tier is matched exactly, so a response's `default` or
// `auto` selects no rows unless the entry lists them, and the reason is "tier". The usage does not say which tier
// billed the call: pass the one the request asked for. With `route_ceiling` (a routing gateway whose served
// endpoint is unknown), every input token at `ceiling_input`, every output token at `ceiling_output`, and the
// dearer of `request` and `ceiling_request`; an entry without a ceiling has no total ("ceiling").
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
// A reservation bound, from `dearest_price_of` when the entry prices input bands: input tokens at the dearest input
// rate (input, cache reads and writes, the 1-hour write, audio input), output tokens at the dearest output rate
// (output, reasoning, image output, audio output), the request fee, and each tool's fee times its uses. With
// `route_ceiling` the ceiling rates replace the listed ones. For usage within the request, `estimate` with the same
// prices never exceeds it. Messages compaction iterations and Gemini's tool-use prompt tokens bill outside the call's
// counters and are not bounded here.
Bound upper_bound(const ModelPrice& price, const BoundRequest& request);
// The bound with the entry's dearest prices over every input band the request may reach.
Bound upper_bound(const descriptor::CatalogueEntry& entry, std::string_view service_tier, const BoundRequest& request);


// What a prepared request may bill at most, with the reason when no bound exists.
enum class UnboundedBy : std::uint8_t {
  OutputCap,      // the request states no output cap
  InputBound,     // nothing bounds a model's input: no measured count and no catalogue limit
  Invocations,    // the request does not state how many model invocations it may make
  ModelPrice,     // the catalogue has no price for a model, or none for a unit the bound needs
  TierPrice,      // the entry prices no unit under the request's service tier
  ToolFee,        // the entry lists no fee for a server tool
  ToolCap,        // a server tool's uses are not capped
  RouteCeiling,   // a routing gateway's route ceiling is missing or cannot be pinned
  Unmodeled,      // the request uses something the catalogue cannot price (plugins, an endpoint call)
  Overflow,       // the bound does not fit 64 bits
};
std::string_view to_string(UnboundedBy) noexcept;
struct UnboundedReason {
  UnboundedBy by = UnboundedBy::Unmodeled;
  // What lacks the bound: a model id, a tool type, a unit, a tier.
  std::string subject;
  // Why, in a short phrase for a person; not a key.
  std::string detail;
};
// The request, as the bound reads it. `family`, `origin` and `model` select catalogue entries as
// ModelCatalogue::find does.
struct WorstCaseInput {
  std::string_view family, origin, model;
  const RequestFacts* facts = nullptr;
  // The tier the catalogue prices the call under: empty for the standard rates (DescriptorPolicy::billing_tier). A request
  // that names no tier and a policy that sets no default leave it empty, so the call is priced at the standard rates;
  // an account that bills such a request at a dearer tier is not covered. Name the tier to bound it.
  std::string_view service_tier;
  // The origin is a routing gateway: its served endpoint is unknown, so the call is priced at the route ceiling.
  bool routed_gateway = false;
  // The request's output cap, in tokens.
  std::optional<std::uint64_t> output_tokens;
  // The input tokens of the request as the host counted them. It must not understate the prompt of `model`; it is
  // not checked, and it is not used for a fallback model, whose tokenizer may count differently.
  std::optional<std::uint64_t> measured_input_tokens;
  // The model invocations one attempt may make (1 without server tools), and the attempts the retry policy allows.
  std::optional<std::uint64_t> invocations;
  std::uint64_t attempts = 1;
};
struct WorstCase {
  // The largest amount the request can bill, rounded up once per attempt to whole nano-USD. Set exactly when
  // `unbounded` is empty.
  std::optional<UsdAmount> total;
  std::vector<UnboundedReason> unbounded;
  // How it was bounded, when it was: the entry and model whose prices were dearest, the input and output tokens one
  // attempt may bill in all its invocations, the largest input of one invocation, the fees of the capped server
  // tools in one attempt, and the dearest tool of each capped group with its cap.
  std::string entry, model;
  std::uint64_t input_tokens = 0, output_tokens = 0, invocation_input_tokens = 0, tool_fees_nano_usd = 0;
  std::uint64_t invocations = 0, attempts = 0;
  std::vector<std::pair<std::string, std::uint64_t>> tool_uses;
  // Priced at the route ceiling (a routing gateway) and not at the listed rates. The bound holds only if the request
  // carries `pin` as `provider.max_price`: the ceiling over every candidate (prompt, completion and, when an entry
  // lists a request fee, that fee), lowered field by field to the request's own cap. `pinned` is true when it already does.
  bool route_ceiling = false, pinned = false;
  std::optional<OpenRouterMaxPrice> pin;
};
// A bound on what one prepared request can bill, from its caps and the catalogue's prices; a stateless function of its
// arguments. It covers that request's own model invocations and retry attempts and nothing a host sends beyond it: a
// host that may send several requests (the steps of a run, a loop, a retry of its own) multiplies by their number.
// Per attempt it is the sum over the invocations of the input tokens at the dearest input rate and the output cap at
// the dearest output rate, the request fee once, and the capped server tools' fees once:
// - Input: with one invocation, the measured input, else the catalogue's input limit, never above that limit. With
//   several (server tools re-prompt the model with the tool results), the first invocation as above and each later
//   one at the limit, which must then exist.
// - Output: the cap on every invocation. Compaction iterations and Gemini's tool-use prompt tokens bill outside the
//   counters and are not bounded here.
// - Tools: each capped group at the dearest fee of its types times its cap; a group with no cap, or a tool with no
//   listed fee, is unbounded.
// - Models: the model and each fallback model, at the dearest candidate.
// - Gateway: at the route ceiling, the dearest over the candidates.
// The attempts multiply the sum. Nothing here is a reservation or a ledger.
// A request that names no service tier is priced at the standard rates (see WorstCaseInput::service_tier).
WorstCase worst_case(const WorstCaseInput&, const descriptor::ModelCatalogue&);

} // namespace sp::pricing
