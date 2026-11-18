#pragma once
// Item prices from the model catalogue: the rate an entry's item units (`item:<kind>`, descriptor::item_kinds) give a
// call that offers a set of request dimensions.
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
// Nothing when no row applies, when a row that applies has a price that is not a fixed amount (a variable price), or
// when `dimensions` are not sorted by key with each key once.

// The dearest row that applies, over every band a call of at most `input_tokens` input tokens may fall in: the rate to
// reserve with. Of equal rates, the first key.
std::optional<std::pair<std::uint64_t, std::string>> dearest_item_price(const descriptor::CatalogueEntry& entry, std::string_view unit,
    const std::vector<std::pair<std::string, std::string>>& dimensions, std::string_view tier, std::uint64_t input_tokens);
// The most specific row that applies, the rate to estimate with: of the rows with one qualifier, the one of the
// highest band the call is above; of those, a row whose qualifier names the most dimensions; of those, the dearest;
// and of equal rates, the first key. It is never dearer than dearest_item_price for the same call.
std::optional<std::pair<std::uint64_t, std::string>> item_price(const descriptor::CatalogueEntry& entry, std::string_view unit,
    const std::vector<std::pair<std::string, std::string>>& dimensions, std::string_view tier, std::uint64_t input_tokens);

} // namespace sp::pricing
