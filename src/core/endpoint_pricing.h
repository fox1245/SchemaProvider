#pragma once
// Item prices from the model catalogue: the rate an entry's item units (`item:<kind>`, descriptor::item_kinds) give a
// call that offers a set of request dimensions; and the most a non-chat endpoint call can bill, from the facts of the
// call (EndpointFacts) and the catalogue's prices.
#include "core/endpoint_facts.h"
#include "core/pricing.h"
#include "descriptor/catalogue.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sp::pricing {

// The rate of the item unit `unit` (`item:image`, without a qualifier, tier or band) for a call that offers
// `dimensions` (sorted by key, each key once), in nano-USD per million units, with the price key that gave it.
// A row of the entry applies to the call when:
// - it is a row of `unit` under the service tier `tier` (empty for the standard rates);
// - the call is above its input band: strictly more than its `+<tokens>` of the `input_tokens`;
// - the call offers every dimension its qualifier names, each with a byte-equal value. A request field the call leaves
//   unset is offered by no dimension, so only a row without a qualifier prices a call that leaves it unset.
// A dimension offered with an empty value is a member the call has whose value no dimension can hold: it matches no
// row, and a row that names its key may apply. Nothing when no row applies, when a row that applies has a price that is
// not a fixed amount (a variable price), when a row of the unit under `tier`, in any band, names a dimension offered
// with an empty value, or when `dimensions` are not sorted by key with each key once.

// The dearest row that applies, over every band a call of at most `input_tokens` input tokens may fall in: the rate to
// reserve with. Of equal rates, the first key.
std::optional<std::pair<std::uint64_t, std::string>> dearest_item_price(const descriptor::CatalogueEntry& entry, std::string_view unit,
    const std::vector<std::pair<std::string, std::string>>& dimensions, std::string_view tier, std::uint64_t input_tokens);
// The most specific row that applies, the rate to estimate with: of the rows with one qualifier, the one of the
// highest band the call is above; of those, a row whose qualifier names the most dimensions; of those, the dearest;
// and of equal rates, the first key. It is never dearer than dearest_item_price for the same call.
std::optional<std::pair<std::uint64_t, std::string>> item_price(const descriptor::CatalogueEntry& entry, std::string_view unit,
    const std::vector<std::pair<std::string, std::string>>& dimensions, std::string_view tier, std::uint64_t input_tokens);

// One priced quantity of an endpoint call: the unit (`input`, `output`, `request`, `item:image`), the price key that
// priced it, the quantity and its rate in nano-USD per million units.
struct EndpointLine {
  std::string unit, row;
  std::uint64_t quantity = 0, rate = 0;
};
// An endpoint call, as endpoint_worst_case reads it.
struct EndpointWorstCaseInput {
  const EndpointFacts* facts = nullptr;
  // The catalogue tier the call is priced under: empty for the standard rates.
  std::string_view service_tier;
  // The input tokens of the call as the host counted them. It must not understate the input; it is not checked, and it
  // is capped at the entry's input limit times the quantity's `per`.
  std::optional<std::uint64_t> measured_input_tokens;
  // The length of the audio the call takes, in whole seconds rounded up: the quantity of a `StatedSeconds` unit. It
  // must not understate the audio; it is not checked.
  std::optional<std::uint64_t> stated_seconds;
  // The input tokens of each chat request a call carries as the host counted them, by custom id, each id once (worst_case's
  // `measured_input_tokens` for that request). A request without one is bounded at its model's input limit.
  std::vector<std::pair<std::string, std::uint64_t>> measured_carried_input_tokens;
  // The attempts the retry policy allows the call.
  std::uint64_t attempts = 1;
  // The provider charges the operation once it accepts a call (descriptor::EndpointFact::charge_on_accept). The runtime
  // sends such a call again only when the provider never accepted it, so one attempt bounds it, whatever `attempts`
  // allows. False multiplies by `attempts`.
  bool charge_on_accept = false;
};
struct EndpointBound {
  // The largest amount the call can bill, rounded up once per attempt to whole nano-USD, times the attempts. Set exactly
  // when `unbounded` is empty.
  std::optional<UsdAmount> total;
  std::vector<UnboundedReason> unbounded;
  // How it was bounded, when it was: the entry that priced it, the model, the lines of one attempt (each quantity at
  // its rate, and the request fee) and the attempts that multiply them. A call that bills the requests it carries has
  // one line per request (endpoint_worst_case), and the entry is the one every request was priced by, empty when they
  // were priced by several.
  std::string entry, model;
  std::vector<EndpointLine> lines;
  std::uint64_t attempts = 0;
};
// A bound on what one endpoint call can bill, from the facts of the call and the catalogue's prices; a stateless
// function of its arguments. By the billing of the call's operation:
// - `None`: exactly zero, with no catalogue lookup, so the model needs no entry.
// - `Unstated` (or a value this version does not know): no total, `Billing`, subject `<endpoint>/<operation>`.
// - `Carried` (a batch submit): the sum of a bound of each chat request it carries (`facts->carried`), each by
//   worst_case with the facts' family and origin, the request's model, facts, output cap and invocations, its measured
//   input tokens from `measured_carried_input_tokens`, one attempt, and the service tier `facts->carried_tier`: an
//   entry with no unit in that tier is `TierPrice`, never priced at other rates. Each request's reasons keep their kind
//   with the subject `<custom_id>:<subject>` (`a-1:` for a reason without a subject). Also refused: no carried
//   requests (`Unmodeled`, subject `<endpoint>/<operation>`; "the batch's requests are in an uploaded file" when the
//   facts name an `input_file`), no `carried_tier` (`TierPrice`, same subject), a routing gateway (`RouteCeiling`, same
//   subject), a custom id named by two requests or measured twice (`Unmodeled`, subject `<custom_id>:`), and a request
//   whose facts name a service tier of its own (`TierPrice`, subject `<custom_id>:<model>`, "a batch item names its
//   own service tier"). Each request's bound is rounded up on its own; their sum is exact, and one line per request
//   holds it: unit `<custom_id>:nano_usd`, the entry that priced it, the request's bound in nano-USD as the quantity
//   and a rate of one nano-USD per unit (1000000). The sum is multiplied by the attempts as below.
// - `Model`: by the entry ModelCatalogue::find gives for the facts' family, origin and model, under `service_tier`:
//   - No entry, or one without prices: `ModelPrice`, subject the model. At a routing gateway: `RouteCeiling`, subject
//     the model, since no route ceiling or pin covers an endpoint call's prices. An entry with no price unit in the
//     tier: `TierPrice`. An entry with a route ceiling at an origin that is not a routing gateway: `RouteCeiling`.
//   - The call offers its dimensions and `operation` with the facts' operation; dimensions that are not sorted by key
//     with each key once, or that name another operation, are `Unmodeled`.
//   - Each quantity is its `at_most`, else by its source: the entry's input limit (the smaller of `input_limit` and
//     `context_window`) times `per`, or the measured input tokens capped at that; the entry's `output_limit`; the
//     `stated_seconds`. A `Request` quantity without `at_most` is not stated.
//   - The input length is the `input` quantity: rows above an input band apply when the call may exceed the band, and
//     every band applies when the length is not stated. The call may then be above no band, so a unit has a rate only
//     when a row of it applies at every length, a row without a band in the tier, and the rate is the dearest over every
//     row: otherwise `ModelPrice`, subject the unit, for the first unit of the input or the output tokens (`input`,
//     `output`; the other units of a set are taken at the dearest of their rows, as they bill only above a row of the
//     first), for `input_image` when the call has image tokens, for the request fee, and for an item unit whose rows the
//     call's dimensions match are all banded.
//   - `input` is priced at the dearest input rate (`input`, `cache_read`, `cache_write`, `cache_write_1h`,
//     `input_audio`, `input_image`), which needs an `input` rate. An `input_image` quantity counts image tokens of
//     the input: one that can be above zero needs an `input_image` rate, and an `input` quantity whose count, when
//     both counts are stated, is at least its own; otherwise `Unmodeled`, subject `input_image`. `output` is priced at the
//     dearest output rate (`output`, `reasoning`, `output_image`, `output_audio`), which needs an `output` rate. The
//     request fee (`request`) is charged once when the entry lists it.
//   - An `item:<kind>` quantity is priced at dearest_item_price for the call's dimensions. When a row of its unit under
//     the tier, in any band, names a dimension the call offers with an empty value (a member whose value the call does
//     not state), it is `ModelPrice`, subject the unit with the dimensions as below and that key without a value.
//   - A quantity that can be above zero needs a rate, and a rate of zero is a declared zero: a unit the entry does not
//     list is `ModelPrice` (subject the unit; an item unit with, in brackets, the call's operation and each offered
//     dimension whose key a row of the unit in the entry names, so no other request value is printed), never free, and so
//     is a row that applies with a variable price. A rate above zero needs the quantity: `InputBound` for `input`,
//     `OutputCap` for `output` and `Quantity` for an item unit, each with the unit as subject. A quantity whose rates
//     are all declared zeros needs none.
//   - A row of an item unit that applies to the call, or may apply because it names a dimension the call offers with an
//     empty value, of a kind none of the quantities count, and that is not a declared zero: `Unmodeled`, subject the
//     row's key.
//   - A quantity of another unit: `Unmodeled`, subject the unit.
// The lines are summed exactly and rounded up once to whole nano-USD; then multiplied by one attempt for a
// charge-on-accept operation and by `attempts` otherwise. Zero attempts are `Invocations`, and a bound past 64 bits is
// `Overflow`. With any reason there is no total and no lines. Nothing here is a reservation or a ledger.
EndpointBound endpoint_worst_case(const EndpointWorstCaseInput& input, const descriptor::ModelCatalogue& catalogue);

// What a finished endpoint call counted (runtime::endpoint_estimate fills it from each typed result).
struct EndpointCounted {
  // The result's token counters. Null, or a usage without an input, output, cache or reasoning counter, when the result
  // reports none.
  const Usage* usage = nullptr;
  // What the result counted of item units, each unit once: `item:image` the images it returned, `item:search_unit`
  // the search units, `item:second` the audio's seconds. A unit not listed was not counted.
  std::vector<std::pair<std::string, std::uint64_t>> items;
  // The image tokens among the input tokens the usage counts, when the result reports them.
  std::optional<std::uint64_t> input_image_tokens;
  // The model the response names.
  ServedModel model;
};
struct EndpointEstimate {
  // Set when every unit the call billed has a price and a count. Rounded up once, to whole nano-USD.
  std::optional<UsdAmount> total;
  // The catalogue entry that priced the call, and whether it is the requested model's because the served model had
  // no priced entry.
  std::string entry;
  bool requested_entry = false;
  // What was priced: `input` and `output` token lines (one per rate they were priced at, with the price key), the
  // `request` fee, and each item unit at the price key that priced it. Empty when there is no total.
  std::vector<EndpointLine> lines;
  // Rules applied where the result says less than the bill: those of `estimate` for the tokens ("cache_read at
  // input", "<unit> at zero", ...), "<unit> at the request's bound" for an item unit the result does not count and the
  // request does not fix, priced at the most the request asks, and "listed rates" at a routing gateway, whose bound is
  // at the route ceiling.
  std::vector<std::string> assumptions;
  // Why there is no total: "billing" (the policy does not state how the operation bills), "carried" (the call bills
  // the requests it carries), "model", "tier", "operation" or "dimensions" (as the bound refuses them), a token side
  // or counter with no price or no count ("input", "output", "cache_read", "input_image", "request", ...), an item
  // unit with no row that applies, a row that applies at a variable price, or no count, a price key of an item unit
  // the operation cannot count, a quantity of a unit the estimate does not price, or "overflow".
  std::vector<std::string> unpriced;
  // The provider may bill more than `total`: the usage was partial or inconsistent, or the result counted fewer of an
  // item unit than the request asked for (fewer images than `count`).
  bool lower_bound = false;
};
// What a finished endpoint call cost by the catalogue: the facts of the call (journaled with it when it was
// prepared) and what its result counted, priced like the bound of the same call (endpoint_worst_case) but at the
// result's own quantities. The provider's own figure, where it reports one (Usage::provider_cost), is the cost; this is
// a cross-check. By the billing of the operation: `None` is exactly zero and reads nothing of the result; `Unstated`
// is "billing"; `Carried` is "carried". For `Model`:
// - The entry: the served model's when the response names another model and that model has an entry with a price
//   under `service_tier`, else the requested one's (`requested_entry`); "model" when neither has one, "tier" when one
//   prices only other tiers. At a routing gateway the entry's listed rates price the call ("listed rates").
// - The dimensions and the operation as the bound reads them.
// - Tokens, when the usage reports a counter: priced as `estimate` prices a usage, at the rates of the input band of
//   the usage's input tokens (cache reads and writes included), with the same cache, reasoning and modality rules. The
//   input's image tokens (`input_image_tokens`) are carved out of the uncached input and priced at `input_image`;
//   more of them than the uncached input, or none of that rate, is "input_image". A side the operation counts (it
//   states an `input` or `output` quantity) whose counter the usage lacks is unpriced. When the usage reports no
//   counter, each side the operation counts needs every rate of the side, in every band, to be a declared zero, with a
//   row of its first unit (`input`, `output`) without a band, and the image tokens the same of `input_image`; otherwise
//   the side is unpriced.
// - Items: the count the result gives; else the request's `at_most` when the request fixes it (`exact`); else the
//   request's `at_most`, with "<unit> at the request's bound"; else none. A count of zero needs no rate, a declared
//   zero needs no count, and a positive rate needs one. Each at the most specific row that applies (item_price).
// - The request fee, once.
// - Only an input counter (input, uncached input, cache reads or writes) states the input length; without one no band
//   can be chosen: a row in a band of the request fee, of an item unit the operation counts or, when the usage reports
//   output counters alone, of the output tokens is "input". A length rebuilt from the uncached input and the cache
//   counters past 64 bits is above every band.
// - A row that applies with a price that is not a fixed amount leaves its unit unpriced, as do the rows of an item
//   kind the operation cannot count and a quantity of another unit, as the bound refuses them.
// Every part is summed exactly and rounded up once. Any unpriced unit leaves no total and no lines: never a partial
// total. The estimate is never limited to the bound: a result beyond its request is priced as it came
// (beyond_bound names what exceeds the bound). For the same facts, catalogue and tier, a result whose every counted
// quantity is at most the bound's quantity of its unit, priced by the entry the bound used (the requested model's), is
// estimated at most at the bound. A served model's own entry may price it above the bound at every quantity within.
EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const EndpointCounted& counted, const descriptor::ModelCatalogue& catalogue,
                                   std::string_view service_tier = {});
// What of `estimate` exceeds `bound`: first "entry" when the estimate is priced by another entry than the bound's (the
// served model's, whose rates are not the bound's); then each unit whose estimated quantity (its lines summed) is above
// the bound's quantity of it (a unit the bound has no line for has none), unless every line of the unit in both is at a
// rate of zero, so that no quantity of it changes the cost (a declared zero the bound needs no count for has a line of
// none); and "total" when the estimate's total is above the bound's. Empty when either has no total.
std::vector<std::string> beyond_bound(const EndpointBound& bound, const EndpointEstimate& estimate);

} // namespace sp::pricing
