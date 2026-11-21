#pragma once
// Prices from the model catalogue, and an estimate of a call's cost from its usage. The provider's own figure is
// Usage::provider_cost; this is the catalogue's, for a provider that reports none or a host that budgets ahead.
#include "core/request_facts.h"
#include "core/value.h"
#include "descriptor/catalogue.h"
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sp::pricing {

// Decimal USD per unit as exact nano-USD per million units (USD x 1e15), rounded up past the fifteenth fractional
// digit: how every listed price below is read (descriptor::price_per_million). Nothing for a negative price (a
// variable one), a character other than a digit or one '.', or an amount that does not fit 64 bits.
std::optional<std::uint64_t> per_million(std::string_view price);
// The decimal USD text per unit that per_million reads as `rate`, without trailing fractional zeros ("0.000003").
std::string decimal_text(std::uint64_t rate);

// Exact nano-USD per million units: per million tokens, and for `request` per million requests. Parsed from the
// entry's decimal USD per unit. OpenRouter's unit names (prompt, completion, input_cache_read, input_cache_write,
// input_cache_write_1h, internal_reasoning, audio, audio_output, input_audio_cache) and the plain ones (input, output,
// cache_read, cache_write, cache_write_1h, reasoning, input_audio, output_audio, cache_read_audio) name the same units
// (descriptor::plain_price_unit); the plain name wins when both are present. A negative listed price (OpenRouter's
// "-1", a variable price) is no price, and a unit with such a row that applies to the request has no price at all,
// whatever its other rows (price_of). Other listed units (OpenRouter's `image`, `web_search`) are not read: nothing in a
// usage counts them.
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
  // A cached audio input token (`cache_read_audio`, OpenRouter's `input_audio_cache`), which no other rate stands in for.
  std::optional<std::uint64_t> cache_read_audio;
  bool empty() const noexcept {
    return !input && !output && !cache_read && !cache_write && !reasoning && !request && !cache_write_1h &&
           !output_image && !input_audio && !output_audio && !ceiling_input && !ceiling_output && !ceiling_request &&
           tool_fees.empty() && !cache_read_audio;
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
// A row applies to the request when it is of the tier and the request is above its band, under either spelling of its
// unit and whether or not a higher band or the plain spelling prices the request. A unit with a row that applies at a
// price that is not a fixed amount (a variable price) has no price, since the provider may bill that row's price.
ModelPrice price_of(const descriptor::CatalogueEntry& entry, std::string_view service_tier = {},
                    std::uint64_t input_tokens = 0);
// Each unit at its dearest over every band a request of at most `input_tokens` input tokens may fall in: the
// prices to reserve with. With a row in any of those bands at a price that is not a fixed amount, the price has no
// rate at all (empty()): a bound over a price that only left that unit out would read the other units' rates and be
// below the bill.
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
  // output_image", "input at input_image" and "route ceiling", and "<unit> at zero" where the entry prices a unit at zero while the unit it
  // refines ("input" for cache and audio input, "output" for reasoning, image and audio output) has a price.
  std::vector<std::string> assumptions;
  // Why there is no total: "model" (no priced entry), "tier" (an entry priced only outside the asked service
  // tier), "ceiling" (no route ceiling), a counter the call billed with no price or no count ("input", "output",
  // "input_audio", "output_audio", "output_image", "cache_read_audio", "cache_read_image"), a unit with a row that applies at a variable price that the call
  // billed ("cache_read", "cache_write", "cache_write_1h", "reasoning", "request", "ceiling_request", or one of
  // those), or a usage counter the estimate does not price ("toolUsePromptTokenCount").
  std::vector<std::string> unpriced;
  // The usage was still arriving (UsageStage::Partial) or its counters contradict each other
  // (UsageQuality::Inconsistent): the provider may bill more than `total`.
  bool lower_bound = false;
};

// Prices `usage` by the catalogue entry of the served model, else of the requested one. Never fetches: refresh the
// catalogue first (transport::refresh_catalogue) when prices may have changed.
// - Input: input_uncached at `input`, cache reads at `cache_read` and cache writes at `cache_write`. Without
//   input_uncached it is input_total less the cache counters.
// - Cached input by modality (#219): the cached audio tokens among the cache reads (OpenAI realtime
//   `input_token_details.cached_tokens_details.audio_tokens`, Generate and Live `cacheTokensDetails.AUDIO`, Interactions
//   `cached_tokens_by_modality.audio`) at `cache_read_audio`, and the rest of the cache reads at `cache_read`. Without a
//   `cache_read_audio` rate, or with one at a variable price, cached audio has no price ("cache_read_audio"): it is never
//   billed at another rate. Cached audio above the audio input tokens or the cache reads leaves no total either. The
//   audio count of the input includes its cached share, so the audio billed at `input_audio` is that count less the
//   cached audio. A realtime turn whose image tokens bill at `input_image` and whose cached share has image tokens
//   (`cached_tokens_details.image_tokens`, `cacheTokensDetails.IMAGE`) has no total ("cache_read_image"), since no unit
//   prices cached image tokens; a Generate usage's cached image tokens bill among the cache reads, as its image tokens
//   bill at the input rates.
//   A usage without the breakdown (Chat, Responses) that reports input audio, priced by an entry that lists
//   `cache_read_audio`, bills as many cache reads as there are audio tokens at the dearer of `cache_read` and
//   `cache_read_audio` ("cache_read at cache_read_audio" when that is the audio rate), since they may be cached audio;
//   a variable `cache_read_audio` leaves them unpriced.
// - An OpenAI realtime turn whose `input_token_details` text, audio and image tokens do not sum to its input total
//   contradicts it, and has no total ("input").
// - Output: with a `reasoning` price, reasoning tokens at it and the rest of output_total at `output`; otherwise
//   all of output_total at `output`.
// - Messages: the compaction iterations (Usage::extra `compaction.input_tokens`, `compaction.output_tokens` and
//   the cache counters) at the same rates, since the top-level counters leave them out.
// - Cache writes: Messages' 5-minute writes at `cache_write` and 1-hour writes at `cache_write_1h`. Without that
//   split, all of them at the dearer rate.
// - Image output: the image output tokens (Generate `candidatesTokensDetails.IMAGE`, Interactions
//   `output_tokens_by_modality.image`, Chat `completion_tokens_details.image_tokens`) at `output_image`. Without
//   any of them, all output that is not reasoning at the dearer of `output` and `output_image`. A usage with image
//   tokens and an entry without an `output_image` rate leaves no total ("output_image").
// - Audio: the audio part of the input (Chat `input_audio`, Generate `promptTokensDetails.AUDIO`, Interactions
//   `input_tokens_by_modality.audio`, realtime `input_token_details.audio_tokens`) at `input_audio`, and of the
//   output (Chat `audio_tokens`, Generate `candidatesTokensDetails.AUDIO`, Interactions
//   `output_tokens_by_modality.audio`, realtime `output_token_details.audio_tokens` and
//   `responseTokensDetails.AUDIO`) at `output_audio`, the input's cached audio aside (above). Audio tokens without that
//   rate, or more uncached audio input than uncached input, leave no total ("input_audio", "output_audio").
// - Image input: a realtime turn's image tokens (OpenAI `input_token_details.image_tokens`, Google Live
//   `realtime.image_tokens`) at `input_image`, out of the uncached input; without that rate no total ("input_image").
//   A usage that states no count of image tokens, no modality breakdown of the input and none of the OpenAI realtime
//   input counters (a Chat or Responses usage) does not say how many of its input tokens were image tokens: for a
//   request that may have carried an image, its uncached input that is not audio bills at the dearer of `input` and
//   `input_image` ("input at input_image"), and a variable `input_image` leaves no total ("input_image"). Without
//   the request's facts (this overload) the request may have carried an image; the overload that takes the facts reads
//   RequestFacts::image_input, so a text-only request bills at `input` (#237). The cache reads keep their rates.
// - A zero rate for a unit whose base unit has a price is applied and named in `assumptions` ("<unit> at zero").
// - `request` is charged once.
// - A unit with a row that applies at a price that is not a fixed amount (price_of) prices none of the tokens it may
//   bill, and they are not billed at another unit's rate: cache reads and writes are unpriced rather than billed at
//   `input`, reasoning tokens rather than at `output`, cache writes the 1-hour split does not place, output tokens
//   without a modality breakdown at a variable `output_image`, and every call at a variable `request`. A Messages
//   compaction iteration's cache writes and output tokens have no split and no breakdown: a variable
//   `cache_write_1h` or `output_image` leaves them unpriced, the unit named once.
// - Seconds: a usage that reports `seconds` (a realtime transcription turn) bills them at `item:second`, the entry's row
//   without a qualifier (endpoint_pricing.h item_price); no such row, or a row at a variable price, leaves no total
//   ("item:second"). Beside no token counter the turn bills no tokens, and an entry whose `input` or `output` is not a
//   declared zero in every band leaves no total ("input", "output").
// Server tool fees are not priced. Gemini's `toolUsePromptTokenCount` bills beyond the prompt count and is not
// priced either: a usage that reports it has no total. With a `service_tier`, the entry's prices for that tier, and
// the input band of the call's input tokens (`price_of`); the tier is matched exactly, so a response's `default` or
// `auto` selects no rows unless the entry lists them, and the reason is "tier". The usage does not say which tier
// billed the call: pass the one the request asked for. With `route_ceiling` (a routing gateway whose served
// endpoint is unknown), every input token at `ceiling_input`, every output token at `ceiling_output`, and the
// dearer of `request` and `ceiling_request`; an entry without a ceiling has no total ("ceiling"). A ceiling row at a
// variable price is named as its unit ("ceiling_input", "ceiling_output"), and then, when the entry lists both
// ceilings, a variable "request" or "ceiling_request" as well; beside a ceiling the entry does not list, the fees are
// not named. The entry is selected as ModelCatalogue::find does, so `origin` is in its canonical spelling
// (descriptor::canonical_origin), as a descriptor's base_url() is.
CostEstimate estimate(const descriptor::ModelCatalogue& catalogue, std::string_view family, std::string_view origin,
                      const ServedModel& model, const Usage& usage, std::string_view service_tier = {},
                      bool route_ceiling = false);
// As above, under the tier the request bills under (billed_tier(facts, service_tier), #207): the facts the request
// was prepared with (PreparedRequest::facts()) and its service tier as DescriptorPolicy::billing_tier maps it. The
// image input rule above reads whether the request carried an image (RequestFacts::image_input). With no
// such tier there is no total: "extension:<key>" for an extension whose value no catalogue row prices, "tier" for two
// tiers.
CostEstimate estimate(const descriptor::ModelCatalogue& catalogue, std::string_view family, std::string_view origin,
                      const ServedModel& model, const Usage& usage, const RequestFacts& facts, std::string_view service_tier,
                      bool route_ceiling = false);

// The Usage::extra key under which the Google Live codec copies the image tokens of a turn's input from its prompt
// breakdown (`promptTokensDetails.IMAGE`), for `estimate` to bill at `input_image`. The breakdown's key is also a
// Generate usage's, whose image tokens bill at the input rates, so the estimate does not read it for images.
inline constexpr std::string_view realtime_image_tokens = "realtime.image_tokens";

// A side of a call's tokens.
enum class TokenSide : std::uint8_t { Input, Output };
// The dearest token price the entry itself lists on a side, in the tier and over the bands a request of `input_tokens`
// input tokens may reach (the default reaches every band): input, cache reads and writes, audio, cached audio and image
// input on the input side; output, reasoning, image output and audio output on the output side; and the listing members a route
// ceiling covers on their side (descriptor::listed_members). `unknown` is set when a row of such a unit is at a price
// that is not a fixed amount: the floor is then not known.
struct ListedFloor {
  std::optional<std::uint64_t> rate;
  bool unknown = false;
};
ListedFloor listed_floor(const descriptor::CatalogueEntry& entry, TokenSide side, std::string_view service_tier = {},
                         std::uint64_t input_tokens = std::numeric_limits<std::uint64_t>::max());
// Whether the entry's route ceiling for `side` (`ceiling_input` or `ceiling_output`) is below what the entry lists on
// that side, or the listed price has no fixed amount: the ceiling does not bound the side. A bound or an estimate at
// the route ceiling refuses such a ceiling (`RouteCeiling`, `ceiling`), and a refresh does not keep one.
bool ceiling_below_listed(const descriptor::CatalogueEntry& entry, TokenSide side, std::uint64_t ceiling,
                          std::string_view service_tier = {},
                          std::uint64_t input_tokens = std::numeric_limits<std::uint64_t>::max());

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
  // a unit the bound reads with a row that applies at a variable price ("cache_read", "reasoning", "request", ...),
  // or "overflow".
  std::vector<std::string> unpriced;
};
// A reservation bound, from `dearest_price_of` when the entry prices input bands: input tokens at the dearest input
// rate (input, cache reads and writes, the 1-hour write, audio input, cached audio input), output tokens at the dearest output rate
// (output, reasoning, image output, audio output), the request fee, and each tool's fee times its uses. With
// `route_ceiling` the ceiling rates replace the listed ones. For usage within the request, `estimate` with the same
// prices never exceeds it. Messages compaction iterations and Gemini's tool-use prompt tokens bill outside the call's
// counters and are not bounded here. A price from `price_of` leaves out a unit with a row at a variable price, which
// this overload cannot tell from a unit the entry does not list; a price from `dearest_price_of` with such a row has no
// rate, so this overload has no total ("input", "output" for the sides with tokens, each tool fee). The overload that
// takes the entry names the unit. At the route ceiling a missing ceiling is "ceiling". ModelPrice holds no rate for
// the input's image tokens (`input_image`), so this overload does not read it and is below the bill of a request that
// carries an image where the entry prices `input_image` above its other input rates: bound such a request with the
// overload that takes the entry, or with worst_case.
Bound upper_bound(const ModelPrice& price, const BoundRequest& request);
// The bound with the entry's dearest prices over every input band the request may reach. The request may carry an
// image, so the input rates include `input_image` (#237). A unit with a row in one of
// them at a price that is not a fixed amount gives no total when the bound reads it: an input rate (`input`,
// `cache_read`, `cache_write`, `cache_write_1h`, `input_audio`, `cache_read_audio`, `input_image`) when the request has input tokens, an output rate
// (`output`, `reasoning`, `output_image`, `output_audio`) when it has output tokens, the request fee always, at the
// route ceiling `ceiling_input`, `ceiling_output` and `ceiling_request`, and a tool's fee when it has uses. At the
// route ceiling "ceiling" names only a ceiling the entry does not list; a variable ceiling row is named as its unit,
// and a variable fee only when both ceilings are listed.
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
  Billing,        // the policy does not state whether the provider bills an endpoint call's operation
  Quantity,       // the request does not state a quantity of a unit the entry prices above zero
};
std::string_view to_string(UnboundedBy) noexcept;
struct UnboundedReason {
  UnboundedBy by = UnboundedBy::Unmodeled;
  // What lacks the bound: a model id, a tool type, a unit, a tier.
  std::string subject;
  // Why, in a short phrase for a person; not a key.
  std::string detail;
};
// The tier a request bills under (#207): `service_tier`, the request's as DescriptorPolicy::billing_tier maps it (or a
// batch's carried tier), together with the tiers its priced extensions name (RequestFacts::priced_extensions). There
// is none (`refused`) when an extension's value changes the price in a way no catalogue row states (`Unmodeled`,
// subject the extension's key), or when two different tiers are named (`TierPrice`, subject the extension's key):
// nothing states how two tiers combine. The same tier named twice is that tier.
struct BilledTier {
  std::string tier;
  std::optional<UnboundedReason> refused;
};
BilledTier billed_tier(const RequestFacts& facts, std::string_view service_tier);
// The request, as the bound reads it. `family`, `origin` and `model` select catalogue entries as
// ModelCatalogue::find does, so `origin` is in its canonical spelling (descriptor::canonical_origin), as a descriptor's
// base_url() is.
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
// - Image input: for a request that carries an image (RequestFacts::image_input), and for one whose facts are not
//   given, the dearest input rate includes `input_image`, and a variable `input_image` is `ModelPrice` (#237). A
//   text-only request's bound does not read it.
// - Input: with one invocation, the measured input, else the catalogue's input limit, never above that limit. With
//   several (server tools re-prompt the model with the tool results), the first invocation as above and each later
//   one at the limit, which must then exist.
// - Output: the cap on every invocation. Compaction iterations and Gemini's tool-use prompt tokens bill outside the
//   counters and are not bounded here.
// - Tools: each capped group at the dearest fee of its types times its cap; a group with no cap, or a tool with no
//   listed fee, is unbounded.
// - Models: the model and each fallback model, at the dearest candidate.
// - Prices beyond tokens: an entry that lists a member no unit reads (descriptor::listed_members: the item members
//   `image` and `web_search`, or a member the table does not know) above zero, or at no fixed amount, has no total
//   (`ModelPrice`, the member in the detail) for a request that carries what an item member prices
//   (RequestFacts::image_input, RequestFacts::plugins), and for any request where the member is unknown.
// - Gateway: at the route ceiling, the dearest over the candidates. A ceiling below a token price the entry lists on
//   its side (ceiling_below_listed) is `RouteCeiling`.
// - Variable prices: a unit with a row that applies at a price that is not a fixed amount (upper_bound with the entry)
//   is `ModelPrice` (a tool fee `ToolFee`), the detail naming the unit and the row.
// The attempts multiply the sum. Nothing here is a reservation or a ledger.
// A request that names no service tier is priced at the standard rates (see WorstCaseInput::service_tier).
WorstCase worst_case(const WorstCaseInput&, const descriptor::ModelCatalogue&);

} // namespace sp::pricing
