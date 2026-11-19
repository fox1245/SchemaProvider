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
  // Measured input tokens of the chat requests a call carries, by custom id. No bound reads them yet: a call that bills
  // the requests it carries has no total.
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
  // its rate, and the request fee) and the attempts that multiply them.
  std::string entry, model;
  std::vector<EndpointLine> lines;
  std::uint64_t attempts = 0;
};
// A bound on what one endpoint call can bill, from the facts of the call and the catalogue's prices; a stateless
// function of its arguments. By the billing of the call's operation:
// - `None`: exactly zero, with no catalogue lookup, so the model needs no entry.
// - `Unstated` (or a value this version does not know): no total, `Billing`, subject `<endpoint>/<operation>`.
// - `Carried`: no total, `Unmodeled`, subject `<endpoint>/<operation>`: the requests it carries are not priced here.
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
//     every band applies when the length is not stated.
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

} // namespace sp::pricing
