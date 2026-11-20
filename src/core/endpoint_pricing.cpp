#include "core/endpoint_pricing.h"
#include "core/pricing.h"
#include "core/pricing_internal.h"
#include <algorithm>
#include <map>
#include <set>
#include <span>

namespace sp::pricing {
namespace {
// Calls `visit` with the key and value of each dimension `qualifier` (`quality=high,size=1024x1024`) names, until it
// returns true; returns whether one did.
template <class Visit> bool any_dimension(std::string_view qualifier, Visit visit) {
  for (std::size_t start = 0; !qualifier.empty();) {
    const auto comma = qualifier.find(',', start);
    const auto dimension = qualifier.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    const auto equals = dimension.find('=');
    if (visit(dimension.substr(0, equals), equals == std::string_view::npos ? std::string_view{} : dimension.substr(equals + 1))) return true;
    if (comma == std::string_view::npos) return false;
    start = comma + 1;
  }
  return false;
}
// The value a call offers for `key`, or nothing when it offers none. An empty value is a member the call has whose
// value no dimension can hold.
std::optional<std::string_view> offered_value(const std::vector<std::pair<std::string, std::string>>& dimensions, std::string_view key) {
  for (const auto& [name, value] : dimensions)
    if (name == key) return std::string_view(value);
  return std::nullopt;
}
// Whether `qualifier` names a dimension the call offers without a value: the row may apply, and nothing says whether.
bool names_unstated(std::string_view qualifier, const std::vector<std::pair<std::string, std::string>>& dimensions) {
  return any_dimension(qualifier, [&](std::string_view key, std::string_view) {
    const auto value = offered_value(dimensions, key);
    return value && value->empty();
  });
}
// Whether some row of `unit` under `tier`, in any band, names a dimension the call offers without a value.
bool unit_names_unstated(const descriptor::CatalogueEntry& entry, std::string_view unit, std::string_view tier,
                         const std::vector<std::pair<std::string, std::string>>& dimensions) {
  return std::any_of(entry.pricing.begin(), entry.pricing.end(), [&](const auto& price) {
    const auto item = descriptor::item_price_key(price.first);
    return item && item->unit == unit && item->tier == tier && names_unstated(item->qualifier, dimensions);
  });
}
// Whether a row qualified by `qualifier` can apply to the call: each dimension it names is offered with that value or
// without a value.
bool may_apply(std::string_view qualifier, const std::vector<std::pair<std::string, std::string>>& dimensions) {
  return !any_dimension(qualifier, [&](std::string_view key, std::string_view wanted) {
    const auto value = offered_value(dimensions, key);
    return !value || (!value->empty() && *value != wanted);
  });
}
// One row of an item unit that applies to a call.
struct ItemRow {
  std::string_view key, qualifier;
  std::size_t dimensions = 0;
  std::uint64_t above = 0, rate = 0;
};
// The rows of `unit` that apply to the call (endpoint_pricing.h), in the entry's order. Nothing when a row that applies
// has a price that is not a fixed amount, when a row of the unit names a dimension the call offers without a value, or
// when the call's dimensions are not sorted by key with each key once: a row skipped then could only make the dearest
// row a cheaper one.
std::optional<std::vector<ItemRow>> applying_rows(const descriptor::CatalogueEntry& entry, std::string_view unit,
                                                  const std::vector<std::pair<std::string, std::string>>& dimensions,
                                                  std::string_view tier, std::uint64_t input_tokens) {
  for (std::size_t i = 1; i < dimensions.size(); ++i)
    if (!(dimensions[i - 1].first < dimensions[i].first)) return std::nullopt;
  if (unit_names_unstated(entry, unit, tier, dimensions)) return std::nullopt;
  std::vector<ItemRow> rows;
  for (const auto& [key, text] : entry.pricing) {
    const auto item = descriptor::item_price_key(key);
    if (!item || item->unit != unit || item->tier != tier || (item->above && input_tokens <= item->above)) continue;
    if (!descriptor::qualifier_offered(item->qualifier, dimensions)) continue;
    const auto rate = per_million(text);
    if (!rate) return std::nullopt;
    rows.push_back({key, item->qualifier, item->dimensions, item->above, *rate});
  }
  return rows;
}
std::optional<std::pair<std::uint64_t, std::string>> priced(const ItemRow* row) {
  if (!row) return std::nullopt;
  return std::pair{row->rate, std::string(row->key)};
}
}  // namespace

std::optional<std::pair<std::uint64_t, std::string>> dearest_item_price(const descriptor::CatalogueEntry& entry, std::string_view unit,
    const std::vector<std::pair<std::string, std::string>>& dimensions, std::string_view tier, std::uint64_t input_tokens) {
  const auto rows = applying_rows(entry, unit, dimensions, tier, input_tokens);
  if (!rows) return std::nullopt;
  const ItemRow* best = nullptr;
  for (const auto& row : *rows)
    if (!best || row.rate > best->rate || (row.rate == best->rate && row.key < best->key)) best = &row;
  return priced(best);
}

std::optional<std::pair<std::uint64_t, std::string>> item_price(const descriptor::CatalogueEntry& entry, std::string_view unit,
    const std::vector<std::pair<std::string, std::string>>& dimensions, std::string_view tier, std::uint64_t input_tokens) {
  const auto rows = applying_rows(entry, unit, dimensions, tier, input_tokens);
  if (!rows) return std::nullopt;
  const ItemRow* best = nullptr;
  for (const auto& row : *rows) {
    // A row with the same qualifier and a higher band the call is above refines this one, as a band does a token unit.
    if (std::any_of(rows->begin(), rows->end(), [&](const ItemRow& other) { return other.qualifier == row.qualifier && other.above > row.above; }))
      continue;
    if (!best || row.dimensions > best->dimensions ||
        (row.dimensions == best->dimensions && (row.rate > best->rate || (row.rate == best->rate && row.key < best->key))))
      best = &row;
  }
  return priced(best);
}

namespace {
using detail::checked_add;
using detail::checked_mul;
using detail::parse_unit;
using detail::smallest_limit;
using detail::Wide;
constexpr std::uint64_t every_band = detail::kMax;
// The token units an endpoint call's input and output tokens bill at, by the plain name the catalogue reads
// (descriptor::plain_price_unit): the first is the unit a line of them needs, and the line takes the dearest of all.
// The input's image tokens bill at `input_image`.
constexpr std::string_view input_units[]{"input", "cache_read", "cache_write", "cache_write_1h", "input_audio", "input_image"};
constexpr std::string_view output_units[]{"output", "reasoning", "output_image", "output_audio"};
constexpr std::string_view image_token_units[]{"input_image"};
constexpr std::string_view request_units[]{"request"};
// The dimension every call offers: the operation it performs.
constexpr std::string_view operation_dimension = "operation";
// The route ceilings a call at a routing gateway is priced at: the input's, and the request fee's.
constexpr std::string_view ceiling_input_units[]{"ceiling_input"};
constexpr std::string_view ceiling_request_units[]{"ceiling_request"};
// The row a line priced at the request's own cap names: the member of the request that states it.
constexpr std::string_view pin_row = "provider.max_price.prompt";

// Whether a call at a routing gateway is bounded by the cap its request sets on the router's choice: every quantity it
// states is input tokens (`input`), which the cap's prompt amount covers, and the request states that amount. A call
// that states any other quantity (output tokens, the input's image tokens, an item unit) has no amount of the cap that
// is known to bind it.
bool pinned_input(const EndpointFacts& facts) {
  return facts.max_price && facts.max_price->prompt_nano_usd && !facts.quantities.empty() &&
         std::all_of(facts.quantities.begin(), facts.quantities.end(), [](const EndpointQuantity& q) { return q.unit == input_units[0]; });
}
// The tier a call is priced under: the facts' own, else the one the caller names (empty for the standard rates). Empty
// with `conflict` set when both name one and they differ.
std::string_view call_tier(const EndpointFacts& facts, std::string_view named, bool& conflict) {
  conflict = !facts.service_tier.empty() && !named.empty() && named != facts.service_tier;
  if (conflict) return {};
  return facts.service_tier.empty() ? named : std::string_view(facts.service_tier);
}
// A unit an endpoint call is priced by that `entry` lists at the standard rates and not under `tier`, in any band: a
// token unit (by its plain name), the request fee, a route ceiling, or an item unit (without its qualifier). Under
// `tier` such a unit would be priced at another unit's rate, or not at all, which is not the tier's price for it. Empty
// when `tier` is empty and when every such unit has a row in it.
std::string unit_outside_tier(const descriptor::CatalogueEntry& entry, std::string_view tier) {
  if (tier.empty()) return {};
  // The unit a price key is read as, and its tier; nothing for a key no endpoint call is priced by.
  const auto read = [](std::string_view key) -> std::optional<std::pair<std::string_view, std::string_view>> {
    if (const auto item = descriptor::item_price_key(key)) return std::pair{item->unit, item->tier};
    const auto unit = parse_unit(key);
    if (!unit) return std::nullopt;
    const auto name = descriptor::plain_price_unit(unit->name);
    for (const auto units : {std::span<const std::string_view>(input_units), std::span<const std::string_view>(output_units),
                             std::span<const std::string_view>(request_units), std::span<const std::string_view>(descriptor::route_ceiling_units)})
      if (std::find(units.begin(), units.end(), name) != units.end()) return std::pair{name, unit->tier};
    return std::nullopt;
  };
  for (const auto& price : entry.pricing) {
    const auto unit = read(price.first);
    if (!unit || !unit->second.empty()) continue;
    if (std::none_of(entry.pricing.begin(), entry.pricing.end(), [&](const auto& other) {
          const auto row = read(other.first);
          return row && row->second == tier && row->first == unit->first;
        }))
      return std::string(unit->first);
  }
  return {};
}

// The rows of the token units `units` under `tier` that apply to a call of at most `input_tokens` input tokens: each
// row of no band, and each row whose band such a call may exceed.
struct TokenRows {
  // The dearest row with a fixed rate (of equal rates, the first key), and whether the first unit has one.
  std::optional<std::pair<std::uint64_t, std::string_view>> dearest;
  bool base = false;
  // A row that applies with a price that is not a fixed amount.
  std::string_view variable;
};
TokenRows token_rows(const descriptor::CatalogueEntry& entry, std::span<const std::string_view> units, std::string_view tier,
                     std::uint64_t input_tokens) {
  TokenRows out;
  for (const auto& [key, text] : entry.pricing) {
    const auto unit = parse_unit(key);
    if (!unit || unit->tier != tier || (unit->above && input_tokens <= unit->above)) continue;
    const auto at = std::find(units.begin(), units.end(), descriptor::plain_price_unit(unit->name));
    if (at == units.end()) continue;
    const auto rate = per_million(text);
    if (!rate) {
      if (out.variable.empty()) out.variable = key;
      continue;
    }
    if (at == units.begin()) out.base = true;
    if (!out.dearest || *rate > out.dearest->first || (*rate == out.dearest->first && std::string_view(key) < out.dearest->second))
      out.dearest = std::pair<std::uint64_t, std::string_view>{*rate, key};
  }
  return out;
}
// Whether `qualifier` (`quality=high,size=1024x1024`) names the dimension `key`.
bool names_key(std::string_view qualifier, std::string_view key) {
  return any_dimension(qualifier, [&](std::string_view name, std::string_view) { return name == key; });
}
// An item unit with the dimensions of a call that its rows read, in the qualifier's spelling:
// `item:image[operation=generate,quality=high]`. A dimension is printed when it is the operation or when a row of the
// unit in the entry names its key; any other changes no price, so the request's own text (a user or session id) never
// reaches a reason. A dimension offered without a value is printed as its key alone (`item:image[operation=generate,style]`).
std::string with_dimensions(const descriptor::CatalogueEntry& entry, std::string_view unit,
                            const std::vector<std::pair<std::string, std::string>>& dimensions) {
  std::string out = std::string(unit) + "[";
  bool first = true;
  for (const auto& [key, value] : dimensions) {
    const bool read = key == operation_dimension || std::any_of(entry.pricing.begin(), entry.pricing.end(), [&](const auto& price) {
      const auto item = descriptor::item_price_key(price.first);
      return item && item->unit == unit && names_key(item->qualifier, key);
    });
    if (!read) continue;
    out += (first ? "" : ",") + key + (value.empty() ? "" : "=" + value);
    first = false;
  }
  return out + "]";
}

// The rate of a carried request's line: one nano-USD per unit, in nano-USD per million units, so that the line's
// quantity is the request's bound in nano-USD.
constexpr std::uint64_t nano_usd_rate = 1'000'000;
// The subject of a reason of the carried request `custom_id`: its custom id, then the reason's own subject.
std::string carried_subject(std::string_view custom_id, std::string_view subject) {
  return std::string(custom_id) + ":" + std::string(subject);
}
// A call that bills the chat requests it carries (a batch submit): each request bounded by worst_case under the facts'
// carried tier, summed exactly, then times the attempts the submit itself may bill.
EndpointBound carried_worst_case(const EndpointWorstCaseInput& in, const descriptor::ModelCatalogue& catalogue, std::uint64_t attempts) {
  EndpointBound out;
  const auto block = [&](UnboundedBy by, std::string subject, std::string detail) {
    out.unbounded.push_back({by, std::move(subject), std::move(detail)});
  };
  const auto& facts = *in.facts;
  const std::string call = facts.endpoint + "/" + facts.operation;
  if (facts.carried.empty()) {
    block(UnboundedBy::Unmodeled, call,
          facts.input_file.empty() ? "the call states none of the requests it carries" : "the batch's requests are in an uploaded file");
    return out;
  }
  // The requests are priced under the carried tier only: standard rates are not a bound on another tier's.
  if (facts.carried_tier.empty()) block(UnboundedBy::TierPrice, call, "the call names no tier for the requests it carries");
  if (facts.routed_gateway) block(UnboundedBy::RouteCeiling, call, "a request a call carries at a routing gateway takes no pin from this bound");
  if (in.attempts == 0) block(UnboundedBy::Invocations, {}, "the call states no attempts");
  if (!out.unbounded.empty()) return out;

  std::uint64_t sum = 0;
  bool overflow = false;
  std::optional<std::string> entry;
  bool one_entry = true;
  // The measured inputs by custom id, and how many times each is stated.
  std::map<std::string_view, std::pair<std::uint64_t, std::size_t>> measurements;
  for (const auto& [named, tokens] : in.measured_carried_input_tokens) {
    auto& [value, times] = measurements[named];
    value = tokens;
    ++times;
  }
  std::set<std::string_view> seen;
  for (const auto& request : facts.carried) {
    const auto& id = request.custom_id;
    if (!seen.insert(id).second) {
      block(UnboundedBy::Unmodeled, carried_subject(id, {}), "the custom id names more than one carried request");
      continue;
    }
    std::optional<std::uint64_t> measured;
    std::size_t stated = 0;
    if (const auto found = measurements.find(id); found != measurements.end()) {
      measured = found->second.first;
      stated = found->second.second;
    }
    if (stated > 1) {
      block(UnboundedBy::Unmodeled, carried_subject(id, {}), "the request's measured input tokens are stated more than once");
      continue;
    }
    // A tier of the request's own is not the carried tier, and nothing states how the two combine.
    if (!request.facts.service_tier.empty()) {
      block(UnboundedBy::TierPrice, carried_subject(id, request.model), "a batch item names its own service tier");
      continue;
    }
    // A unit the request's entry (or a fallback's) prices at the standard rates but not under the carried tier.
    std::string outside;
    std::vector<std::string_view> models{request.model};
    for (const auto& fallback : request.facts.fallback_models) models.push_back(fallback);
    for (const auto model : models)
      if (const auto* found = catalogue.find(facts.family, facts.origin, model).entry; found && outside.empty())
        outside = std::string(detail::unit_outside_tier(*found, facts.carried_tier));
    if (!outside.empty()) {
      block(UnboundedBy::TierPrice, carried_subject(id, request.model),
            "the entry prices " + outside + " at the standard rates but not under service tier '" + facts.carried_tier + "'");
      continue;
    }
    WorstCaseInput item;
    item.family = facts.family;
    item.origin = facts.origin;
    item.model = request.model;
    item.facts = &request.facts;
    item.service_tier = facts.carried_tier;
    item.output_tokens = request.output_tokens;
    item.measured_input_tokens = measured;
    item.invocations = request.invocations;
    item.attempts = 1;
    auto bound = worst_case(item, catalogue);
    if (!bound.total) {
      for (auto& reason : bound.unbounded) block(reason.by, carried_subject(id, reason.subject), std::move(reason.detail));
      continue;
    }
    out.lines.push_back({carried_subject(id, "nano_usd"), bound.entry, bound.total->nano_usd, nano_usd_rate});
    if (!checked_add(sum, bound.total->nano_usd, sum)) overflow = true;
    if (!entry) entry = bound.entry;
    else if (*entry != bound.entry) one_entry = false;
  }
  if (!out.unbounded.empty()) {
    out.lines.clear();
    return out;
  }
  std::uint64_t total = 0;
  if (overflow || !checked_mul(sum, attempts, total)) {
    block(UnboundedBy::Overflow, {}, overflow ? "the bound does not fit 64 bits" : "the bound over the attempts does not fit 64 bits");
    out.lines.clear();
    return out;
  }
  out.total = UsdAmount{total, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
  if (one_entry && entry) out.entry = *entry;
  out.model = facts.model;
  out.attempts = attempts;
  return out;
}
}  // namespace

EndpointBound endpoint_worst_case(const EndpointWorstCaseInput& in, const descriptor::ModelCatalogue& catalogue) {
  EndpointBound out;
  const auto block = [&](UnboundedBy by, std::string subject, std::string detail) {
    out.unbounded.push_back({by, std::move(subject), std::move(detail)});
  };
  if (!in.facts) {
    block(UnboundedBy::Unmodeled, {}, "the call states no facts");
    return out;
  }
  const auto& facts = *in.facts;
  const std::uint64_t attempts = in.charge_on_accept ? 1 : in.attempts;
  // What the policy states about the operation decides first: a call the provider does not bill needs no entry.
  if (facts.billing == descriptor::EndpointBilling::None) {
    out.total = UsdAmount{0, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
    out.model = facts.model;
    out.attempts = attempts;
    return out;
  }
  if (facts.billing == descriptor::EndpointBilling::Carried) return carried_worst_case(in, catalogue, attempts);
  if (facts.billing != descriptor::EndpointBilling::Model) {
    block(UnboundedBy::Billing, facts.endpoint + "/" + facts.operation, "the policy does not state whether the provider bills the operation");
    return out;
  }

  // The dimensions the call offers, its operation among them.
  if (facts.operation.empty()) block(UnboundedBy::Unmodeled, facts.endpoint, "the call names no operation");
  auto offered = facts.dimensions;
  bool sorted = true;
  for (std::size_t i = 1; i < offered.size(); ++i) sorted = sorted && offered[i - 1].first < offered[i].first;
  if (!sorted) {
    block(UnboundedBy::Unmodeled, "dimensions", "the call's dimensions are not sorted by key with each key once");
  } else {
    const auto at = std::lower_bound(offered.begin(), offered.end(), operation_dimension,
                                     [](const std::pair<std::string, std::string>& dimension, std::string_view key) { return dimension.first < key; });
    if (at == offered.end() || at->first != operation_dimension) offered.insert(at, {std::string(operation_dimension), facts.operation});
    else if (at->second != facts.operation) block(UnboundedBy::Unmodeled, "dimensions", "the call's dimensions name an operation other than the call's");
  }
  if (in.attempts == 0) block(UnboundedBy::Invocations, {}, "the call states no attempts");
  // The tier the facts name, which the input may repeat but not contradict.
  bool conflict = false;
  const std::string_view tier = call_tier(facts, in.service_tier, conflict);
  if (conflict)
    block(UnboundedBy::TierPrice, facts.model, "the call's facts name service tier '" + facts.service_tier + "' and the input names '" + std::string(in.service_tier) + "'");
  if (!out.unbounded.empty()) return out;

  const auto* entry = catalogue.find(facts.family, facts.origin, facts.model).entry;
  if (!entry || entry->pricing.empty()) block(UnboundedBy::ModelPrice, facts.model, "the catalogue has no price for the model at the origin");
  // At a routing gateway only a call whose request caps the router's choice, and whose quantities that cap covers, has a
  // bound: the input tokens at the lower of the route ceiling and the cap.
  const bool gateway = facts.routed_gateway;
  if (gateway && !pinned_input(facts))
    block(UnboundedBy::RouteCeiling, facts.model, "an endpoint call at a routing gateway has no per-item route ceiling and no pin");
  if (!out.unbounded.empty()) return out;
  bool in_tier = false, ceiling = false;
  for (const auto& [key, text] : entry->pricing) {
    const auto unit = parse_unit(key);
    if (!unit || unit->tier != tier) continue;
    in_tier = true;
    ceiling = ceiling || unit->name.starts_with(descriptor::item_ceiling_prefix) ||
              std::find(std::begin(descriptor::route_ceiling_units), std::end(descriptor::route_ceiling_units), unit->name) != std::end(descriptor::route_ceiling_units);
  }
  if (!in_tier) {
    block(UnboundedBy::TierPrice, facts.model,
          tier.empty() ? "the entry prices no unit at the standard rates" : "the entry prices no unit under service tier '" + std::string(tier) + "'");
    return out;
  }
  if (const auto outside = unit_outside_tier(*entry, tier); !outside.empty()) {
    block(UnboundedBy::TierPrice, facts.model, "the entry prices " + outside + " at the standard rates but not under service tier '" + std::string(tier) + "'");
    return out;
  }
  if (ceiling && !gateway) {
    block(UnboundedBy::RouteCeiling, facts.model, "the entry lists a route ceiling but the origin is not a routing gateway, so it cannot be pinned");
    return out;
  }

  // Each quantity's count, where the facts or the entry state one.
  const auto& quantities = facts.quantities;
  const auto input_limit = smallest_limit({entry->input_limit, entry->context_window});
  const auto output_limit = smallest_limit({entry->output_limit});
  std::vector<std::optional<std::uint64_t>> counts(quantities.size());
  for (std::size_t i = 0; i < quantities.size(); ++i) {
    const auto& q = quantities[i];
    const bool token = q.unit == input_units[0] || q.unit == output_units[0] || q.unit == image_token_units[0];
    if (!token && !q.unit.starts_with(descriptor::item_unit_prefix)) {
      block(UnboundedBy::Unmodeled, q.unit, "the bound prices no such unit");
      continue;
    }
    if (q.at_most) {
      counts[i] = q.at_most;
      continue;
    }
    switch (q.source) {
      case EndpointQuantity::Source::Request: break;
      case EndpointQuantity::Source::StatedSeconds: counts[i] = in.stated_seconds; break;
      case EndpointQuantity::Source::OutputLimit: counts[i] = output_limit; break;
      case EndpointQuantity::Source::InputLimit: {
        std::optional<std::uint64_t> limit;
        if (input_limit) {
          std::uint64_t product = 0;
          if (!checked_mul(*input_limit, q.per, product)) {
            block(UnboundedBy::Overflow, q.unit, "the input limit times the inputs does not fit 64 bits");
            break;
          }
          limit = product;
        }
        if (in.measured_input_tokens) counts[i] = limit ? (std::min)(*in.measured_input_tokens, *limit) : *in.measured_input_tokens;
        else counts[i] = limit;
        break;
      }
    }
  }
  if (!out.unbounded.empty()) return out;
  // The input length that picks the bands: every band when it is not stated.
  std::optional<std::uint64_t> stated_input, stated_images = 0;
  bool has_input = false, unstated_input = false, image_tokens = false;
  for (std::size_t i = 0; i < quantities.size(); ++i) {
    if (quantities[i].unit == image_token_units[0]) {
      if (!counts[i] || *counts[i] > 0) image_tokens = true;
      std::uint64_t sum = 0;
      if (!counts[i]) stated_images.reset();
      else if (stated_images && !checked_add(*stated_images, *counts[i], sum)) {
        block(UnboundedBy::Overflow, quantities[i].unit, "the image tokens do not fit 64 bits");
        return out;
      } else if (stated_images) stated_images = sum;
      continue;
    }
    if (quantities[i].unit != input_units[0]) continue;
    has_input = true;
    if (!counts[i]) {
      unstated_input = true;
      continue;
    }
    std::uint64_t sum = 0;
    if (!checked_add(stated_input.value_or(0), *counts[i], sum)) {
      block(UnboundedBy::Overflow, quantities[i].unit, "the input tokens do not fit 64 bits");
      return out;
    }
    stated_input = sum;
  }
  const std::uint64_t length = stated_input && !unstated_input ? *stated_input : every_band;
  // Without a stated length the call may be above no band, so a unit has a rate only when a row of it applies at every
  // length: a row without a band. The rate is then the dearest over every row, which every band may reach.
  const bool unstated_length = length == every_band;
  const auto unbanded = [&](std::span<const std::string_view> units) { return token_rows(*entry, units.first(1), tier, 0).base; };
  constexpr std::string_view below_band = "no row of the unit prices a call below its input band, and the call's input length is not stated";
  // The image tokens of the input are among its input tokens, which the input quantities count: image tokens that can
  // be above zero need an input quantity, and one that holds them when both counts are stated.
  if (image_tokens && (!has_input || (stated_input && !unstated_input && stated_images && *stated_images > *stated_input)))
    block(UnboundedBy::Unmodeled, std::string(image_token_units[0]), "the image tokens are not counted by the call's input tokens");

  Wide sum;  // nano-USD x 1e6
  const auto charge = [&](std::string_view unit, std::string_view row, std::uint64_t quantity, std::uint64_t rate) {
    out.lines.push_back({std::string(unit), std::string(row), quantity, rate});
    sum += Wide::product(quantity, rate);
  };
  // A line of tokens: their count over the quantities of `unit`, at the dearest rate of `units`, which needs a rate of
  // `units[0]`; none when no quantity can be above zero.
  const auto tokens = [&](std::span<const std::string_view> units, UnboundedBy unknown, std::string_view unknown_detail, bool images) {
    const auto unit = units[0];
    bool billed = false;
    std::optional<std::uint64_t> count = 0;
    for (std::size_t i = 0; i < quantities.size(); ++i) {
      if (quantities[i].unit != unit) continue;
      if (!counts[i]) { billed = true; count.reset(); continue; }
      billed = billed || *counts[i] > 0;
      std::uint64_t total = 0;
      if (count && checked_add(*count, *counts[i], total)) count = total;
      else if (count) { block(UnboundedBy::Overflow, std::string(unit), "the tokens do not fit 64 bits"); return; }
    }
    if (!billed) return;
    const auto rows = token_rows(*entry, units, tier, length);
    bool priced = true;
    if (!rows.variable.empty()) {
      block(UnboundedBy::ModelPrice, std::string(unit), "catalogue entry '" + entry->id + "' prices it at a variable price (" + std::string(rows.variable) + ")");
      priced = false;
    }
    if (!rows.base) {
      block(UnboundedBy::ModelPrice, std::string(unit), "catalogue entry '" + entry->id + "' lists no rate for the unit");
      priced = false;
    } else if (unstated_length && !unbanded(units)) {
      block(UnboundedBy::ModelPrice, std::string(unit), std::string(below_band));
      priced = false;
    }
    if (images && !token_rows(*entry, image_token_units, tier, length).base) {
      block(UnboundedBy::ModelPrice, std::string(image_token_units[0]), "catalogue entry '" + entry->id + "' lists no rate for the input's image tokens");
      priced = false;
    } else if (images && unstated_length && !unbanded(image_token_units)) {
      block(UnboundedBy::ModelPrice, std::string(image_token_units[0]), std::string(below_band));
      priced = false;
    }
    if (!count) {
      // A count is needed unless every rate is a declared zero.
      if (priced && rows.dearest->first == 0) charge(unit, rows.dearest->second, 0, 0);
      else block(unknown, std::string(unit), std::string(unknown_detail));
      return;
    }
    if (priced) charge(unit, rows.dearest->second, *count, rows.dearest->first);
  };
  std::optional<OpenRouterMaxPrice> pin;
  if (gateway) {
    // Every input token at the lower of the route ceiling, the dearest price of the endpoints the router may choose,
    // and the request's own cap, which the router does not exceed. The listed rates are not the bound's.
    std::optional<std::uint64_t> count = 0;
    for (std::size_t i = 0; i < quantities.size() && count; ++i) {
      std::uint64_t total = 0;
      if (!counts[i]) count.reset();
      else if (!checked_add(*count, *counts[i], total)) {
        block(UnboundedBy::Overflow, std::string(input_units[0]), "the tokens do not fit 64 bits");
        return out;
      } else count = total;
    }
    if (!count) block(UnboundedBy::InputBound, std::string(input_units[0]), "no measured input and no catalogue input limit");
    const auto rows = token_rows(*entry, ceiling_input_units, tier, length);
    if (!rows.variable.empty())
      block(UnboundedBy::ModelPrice, std::string(ceiling_input_units[0]),
            "catalogue entry '" + entry->id + "' prices it at a variable price (" + std::string(rows.variable) + ")");
    else if (!rows.base)
      block(UnboundedBy::RouteCeiling, facts.model, "the entry lists no route ceiling (ceiling_input), so the router may pick any price");
    // A request fee, listed or as a route ceiling: no amount of the request's cap is known to bind it.
    for (const auto units : {std::span<const std::string_view>(request_units), std::span<const std::string_view>(ceiling_request_units)}) {
      const auto fee = token_rows(*entry, units, tier, length);
      if (!fee.variable.empty())
        block(UnboundedBy::ModelPrice, std::string(units[0]), "catalogue entry '" + entry->id + "' prices it at a variable price (" + std::string(fee.variable) + ")");
      else if (fee.dearest && fee.dearest->first > 0)
        block(UnboundedBy::RouteCeiling, std::string(request_units[0]), "the entry lists a request fee, which the request's cap is not known to bind at a routing gateway");
    }
    if (out.unbounded.empty()) {
      const auto own = *facts.max_price->prompt_nano_usd, highest = rows.dearest->first;
      if (own < highest) charge(input_units[0], pin_row, *count, own);
      else charge(input_units[0], rows.dearest->second, *count, highest);
      // The pin the request must carry: its own cap, the prompt amount lowered to the route ceiling.
      pin = *facts.max_price;
      pin->prompt_nano_usd = (std::min)(own, highest);
    }
  } else {
    tokens(input_units, UnboundedBy::InputBound, "no measured input and no catalogue input limit", image_tokens);
    tokens(output_units, UnboundedBy::OutputCap, "the request states no output cap and the entry no output limit", false);
    // The request fee, once.
    if (const auto fee = token_rows(*entry, request_units, tier, length); !fee.variable.empty())
      block(UnboundedBy::ModelPrice, std::string(request_units[0]), "catalogue entry '" + entry->id + "' prices it at a variable price (" + std::string(fee.variable) + ")");
    else if (fee.dearest && unstated_length && !unbanded(request_units))
      block(UnboundedBy::ModelPrice, std::string(request_units[0]), std::string(below_band));
    else if (fee.dearest)
      charge(request_units[0], fee.dearest->second, 1, fee.dearest->first);
  }
  // The items, each at the dearest row the call's dimensions match.
  std::vector<std::string_view> counted;
  for (std::size_t i = 0; i < quantities.size(); ++i) {
    const auto& q = quantities[i];
    if (!q.unit.starts_with(descriptor::item_unit_prefix)) continue;
    counted.push_back(q.unit);
    if (counts[i] && *counts[i] == 0) continue;
    if (unit_names_unstated(*entry, q.unit, tier, offered)) {
      block(UnboundedBy::ModelPrice, with_dimensions(*entry, q.unit, offered), "a row of the unit is qualified by a member whose value the call does not state");
      continue;
    }
    const auto rate = dearest_item_price(*entry, q.unit, offered, tier, length);
    if (!rate) {
      block(UnboundedBy::ModelPrice, with_dimensions(*entry, q.unit, offered),
            applying_rows(*entry, q.unit, offered, tier, length) ? "catalogue entry '" + entry->id + "' has no row of the unit that applies to the call"
                                                                 : "a row of the unit that applies to the call has a variable price");
      continue;
    }
    // Rows of the qualifiers the call matches, without a band.
    if (const auto everywhere = applying_rows(*entry, q.unit, offered, tier, 0); unstated_length && everywhere && everywhere->empty()) {
      block(UnboundedBy::ModelPrice, with_dimensions(*entry, q.unit, offered), std::string(below_band));
      continue;
    }
    if (rate->first == 0) charge(q.unit, rate->second, counts[i].value_or(0), 0);
    else if (!counts[i]) block(UnboundedBy::Quantity, q.unit, "the request does not state the quantity");
    else charge(q.unit, rate->second, *counts[i], rate->first);
  }
  // A unit the entry bills for the call that none of the quantities count.
  for (const auto& [key, text] : entry->pricing) {
    const auto item = descriptor::item_price_key(key);
    if (!item || item->tier != tier || (item->above && length <= item->above)) continue;
    if (std::find(counted.begin(), counted.end(), item->unit) != counted.end() || !may_apply(item->qualifier, offered)) continue;
    if (const auto rate = per_million(text); rate && *rate == 0) continue;
    block(UnboundedBy::Unmodeled, key, "the entry prices a unit this operation cannot count");
  }
  if (!out.unbounded.empty()) {
    out.lines.clear();
    return out;
  }

  const auto one = detail::rounded_nano_usd(sum);
  std::uint64_t total = 0;
  if (!one || !checked_mul(*one, attempts, total)) {
    block(UnboundedBy::Overflow, {}, !one ? "the bound does not fit 64 bits" : "the bound over the attempts does not fit 64 bits");
    out.lines.clear();
    return out;
  }
  out.total = UsdAmount{total, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
  out.entry = entry->id;
  out.model = facts.model;
  out.attempts = attempts;
  if (pin) {
    out.route_ceiling = true;
    out.pinned = facts.max_price->prompt_nano_usd == pin->prompt_nano_usd;
    out.pin = std::move(pin);
  }
  return out;
}

namespace {
// The line an estimate's token charge belongs to, by the unit whose rate priced it: the output, the request fee, or
// the input.
std::string_view line_of(std::string_view unit) {
  if (std::find(std::begin(output_units), std::end(output_units), unit) != std::end(output_units)) return output_units[0];
  if (unit == request_units[0]) return request_units[0];
  return input_units[0];
}
}  // namespace

EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const EndpointCounted& counted, const descriptor::ModelCatalogue& catalogue,
                                   std::string_view service_tier) {
  EndpointEstimate out;
  const auto refuse = [&](std::string_view name) {
    if (std::find(out.unpriced.begin(), out.unpriced.end(), name) == out.unpriced.end()) out.unpriced.emplace_back(name);
  };
  const auto assume = [&](std::string rule) {
    if (std::find(out.assumptions.begin(), out.assumptions.end(), rule) == out.assumptions.end()) out.assumptions.push_back(std::move(rule));
  };
  // What the policy states about the operation decides first. A call the provider does not bill reads nothing of its
  // result: the usage a poll or a generation read reports is another call's.
  if (facts.billing == descriptor::EndpointBilling::None) {
    out.total = UsdAmount{0, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
    return out;
  }
  if (facts.billing == descriptor::EndpointBilling::Carried) {
    refuse("carried");
    return out;
  }
  if (facts.billing != descriptor::EndpointBilling::Model) {
    refuse("billing");
    return out;
  }
  const Usage* usage = counted.usage;
  if (usage) out.lower_bound = usage->stage == UsageStage::Partial || usage->quality == UsageQuality::Inconsistent;

  // The dimensions the call offers, its operation among them, as the bound reads them.
  if (facts.operation.empty()) refuse("operation");
  auto offered = facts.dimensions;
  bool sorted = true;
  for (std::size_t i = 1; i < offered.size(); ++i) sorted = sorted && offered[i - 1].first < offered[i].first;
  if (!sorted) {
    refuse("dimensions");
  } else {
    const auto at = std::lower_bound(offered.begin(), offered.end(), operation_dimension,
                                     [](const std::pair<std::string, std::string>& dimension, std::string_view key) { return dimension.first < key; });
    if (at == offered.end() || at->first != operation_dimension) offered.insert(at, {std::string(operation_dimension), facts.operation});
    else if (at->second != facts.operation) refuse("dimensions");
  }
  // The tier the facts name, which the caller may repeat but not contradict.
  bool conflict = false;
  const std::string_view tier = call_tier(facts, service_tier, conflict);
  if (conflict) refuse("tier");
  if (!out.unpriced.empty()) return out;

  // The entry: the served model's when the response names another model with a price in the tier, else the requested
  // one's, as the chat estimate chooses it.
  const descriptor::CatalogueEntry* entry = nullptr;
  bool other_tier = false;
  const auto priced = [&](std::string_view model) {
    if (model.empty()) return false;
    const auto* found = catalogue.find(facts.family, facts.origin, model).entry;
    if (!found || found->pricing.empty()) return false;
    if (std::none_of(found->pricing.begin(), found->pricing.end(), [&](const auto& price) {
          const auto unit = parse_unit(price.first);
          return unit && unit->tier == tier;
        })) {
      other_tier = true;
      return false;
    }
    entry = found;
    return true;
  };
  const auto& model = counted.model;
  const bool served = model.relation != ServedRelation::NotReported && !model.served.empty() && model.served != facts.model;
  const bool by_served = served && priced(model.served);
  if (!by_served && !priced(facts.model)) {
    refuse(other_tier ? "tier" : "model");
    return out;
  }
  if (!unit_outside_tier(*entry, tier).empty()) {
    refuse("tier");
    return out;
  }
  out.entry = entry->id;
  out.requested_entry = served && !by_served;
  // The bound at a routing gateway is at the route ceiling; the estimate takes the entry's listed rates.
  if (facts.routed_gateway) assume("listed rates");

  // What the operation counts: a side of tokens that can be above zero, the image tokens among the input, the item
  // units; a quantity of another unit is not priced.
  bool counts_input = false, counts_output = false, counts_images = false;
  std::vector<std::string_view> kinds;
  for (const auto& q : facts.quantities) {
    const bool billed = !q.at_most || *q.at_most > 0;
    if (q.unit == input_units[0]) counts_input = counts_input || billed;
    else if (q.unit == output_units[0]) counts_output = counts_output || billed;
    else if (q.unit == image_token_units[0]) counts_images = counts_images || billed;
    else if (q.unit.starts_with(descriptor::item_unit_prefix)) kinds.push_back(q.unit);
    else refuse(q.unit);
  }
  // The input length that picks bands: the usage's input tokens, cache reads and writes included, when it reports an
  // input counter. Without it no band can be chosen, so a band of a unit the estimate prices (the request fee, an item
  // kind the operation counts, and the output tokens a usage with output counters alone reports) leaves no total.
  const bool reported = usage && (usage->input_total || usage->input_uncached || usage->cache_read || usage->cache_write || usage->output_total ||
                                  usage->reasoning);
  const bool input_reported = usage && (usage->input_total || usage->input_uncached || usage->cache_read || usage->cache_write);
  std::optional<std::uint64_t> length;
  if (input_reported) {
    if (usage->input_total) {
      length = usage->input_total->value;
    } else {
      // A sum past 64 bits is above every band.
      std::uint64_t sum = 0;
      for (const auto& counter : {usage->input_uncached, usage->cache_read, usage->cache_write})
        if (counter && !checked_add(sum, counter->value, sum)) sum = detail::kMax;
      length = sum;
    }
  } else if (std::any_of(entry->pricing.begin(), entry->pricing.end(), [&](const auto& price) {
               const auto unit = parse_unit(price.first);
               if (!unit || unit->tier != tier || !unit->above) return false;
               const auto item = descriptor::item_price_key(price.first);
               const auto name = descriptor::plain_price_unit(unit->name);
               return name == request_units[0] || (item && std::find(kinds.begin(), kinds.end(), item->unit) != kinds.end()) ||
                      (reported && std::find(std::begin(output_units), std::end(output_units), name) != std::end(output_units));
             })) {
    refuse(input_units[0]);
  }
  const auto band = length.value_or(0);

  // The tokens and the request fee: the chat estimate's own sum, not rounded.
  auto selection = detail::select(*entry, tier, band, false);
  // At a routing gateway, a request that caps the router's choice of the input's price is billed at most that cap.
  if (facts.routed_gateway && pinned_input(facts)) {
    const auto cap = *facts.max_price->prompt_nano_usd;
    bool lowered = false;
    for (auto* rate : {&selection.price.input, &selection.price.cache_read, &selection.price.cache_write, &selection.price.cache_write_1h,
                       &selection.price.input_audio, &selection.input_image})
      if (*rate && **rate > cap) {
        *rate = cap;
        lowered = true;
      }
    if (lowered) assume("input at the pin");
  }
  detail::UsageRules rules;
  rules.input = reported && counts_input;
  rules.output = reported && counts_output;
  rules.input_image = counted.input_image_tokens;
  const Usage none;
  const auto summed = detail::usage_sum(selection, reported ? *usage : none, rules);
  for (const auto& name : summed.unpriced) refuse(name);
  for (const auto& rule : summed.assumptions) assume(rule);
  if (!reported) {
    // No token counter: each side the operation counts needs every rate of the side, in every band, to be a declared
    // zero, and a row of its first unit without a band, since the call may be above no band.
    const auto declared_zero = [&](std::span<const std::string_view> units) {
      const auto rows = token_rows(*entry, units, tier, every_band);
      return rows.variable.empty() && rows.base && rows.dearest->first == 0 && token_rows(*entry, units, tier, 0).base;
    };
    if (counts_input && !declared_zero(input_units)) refuse(input_units[0]);
    if (counts_images && !declared_zero(image_token_units)) refuse(image_token_units[0]);
    if (counts_output && !declared_zero(output_units)) refuse(output_units[0]);
  }
  for (const auto& charge : summed.charges)
    out.lines.push_back({std::string(line_of(charge.unit)), std::string(charge.key), charge.quantity, charge.rate});
  Wide sum = summed.sum;  // nano-USD x 1e6

  // The items, each at the most specific row that applies: the count the result gives, else what the request fixes,
  // else the most the request asks for.
  for (const auto& q : facts.quantities) {
    if (!q.unit.starts_with(descriptor::item_unit_prefix)) continue;
    const auto listed = std::count_if(counted.items.begin(), counted.items.end(), [&](const auto& item) { return item.first == q.unit; });
    if (listed > 1) {
      refuse(q.unit);
      continue;
    }
    std::optional<std::uint64_t> count;
    bool at_bound = false;
    if (listed == 1) {
      count = std::find_if(counted.items.begin(), counted.items.end(), [&](const auto& item) { return item.first == q.unit; })->second;
      // Fewer than the request asked for: the provider may bill the ones it did not return.
      if (q.at_most && *count < *q.at_most) out.lower_bound = true;
    } else if (q.at_most) {
      count = q.at_most;
      at_bound = !q.exact;
    }
    if (count && *count == 0) continue;
    const auto rate = item_price(*entry, q.unit, offered, tier, band);
    if (!rate) {
      refuse(q.unit);
      continue;
    }
    // A declared zero needs no count, and its line keeps the count the result gives.
    if (rate->first == 0) {
      out.lines.push_back({q.unit, rate->second, count.value_or(0), 0});
      continue;
    }
    if (!count) {
      refuse(q.unit);
      continue;
    }
    if (at_bound) assume(q.unit + " at the request's bound");
    out.lines.push_back({q.unit, rate->second, *count, rate->first});
    sum += Wide::product(*count, rate->first);
  }
  // A row of an item kind the operation cannot count, that applies to the call or may, and that is not a declared zero.
  const auto reach = length ? *length : every_band;
  for (const auto& [key, text] : entry->pricing) {
    const auto item = descriptor::item_price_key(key);
    if (!item || item->tier != tier || (item->above && reach <= item->above)) continue;
    if (std::find(kinds.begin(), kinds.end(), item->unit) != kinds.end() || !may_apply(item->qualifier, offered)) continue;
    if (const auto rate = per_million(text); rate && *rate == 0) continue;
    refuse(key);
  }
  if (!out.unpriced.empty()) {
    out.lines.clear();
    return out;
  }
  // Every part is rounded together, once.
  const auto nano = detail::rounded_nano_usd(sum);
  if (!nano) {
    refuse("overflow");
    out.lines.clear();
    return out;
  }
  out.total = UsdAmount{*nano, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
  return out;
}

std::vector<std::string> beyond_bound(const EndpointBound& bound, const EndpointEstimate& estimate) {
  std::vector<std::string> out;
  if (!bound.total || !estimate.total) return out;
  // Another entry's rates (the served model's) are not the bound's: the estimate may exceed it at every quantity within.
  if (estimate.entry != bound.entry) out.emplace_back("entry");
  // The quantity of `unit` over the lines, at most 2^64 - 1.
  const auto quantity = [](const std::vector<EndpointLine>& lines, std::string_view unit) {
    std::uint64_t sum = 0;
    for (const auto& line : lines)
      if (line.unit == unit && !checked_add(sum, line.quantity, sum)) return detail::kMax;
    return sum;
  };
  // Whether a line of `unit` has a rate above zero: a unit both price at zero costs nothing at any quantity.
  const auto billed = [](const std::vector<EndpointLine>& lines, std::string_view unit) {
    return std::any_of(lines.begin(), lines.end(), [&](const EndpointLine& line) { return line.unit == unit && line.rate > 0; });
  };
  for (const auto& line : estimate.lines)
    if (std::find(out.begin(), out.end(), line.unit) == out.end() && quantity(estimate.lines, line.unit) > quantity(bound.lines, line.unit) &&
        (billed(estimate.lines, line.unit) || billed(bound.lines, line.unit)))
      out.push_back(line.unit);
  if (estimate.total->nano_usd > bound.total->nano_usd) out.emplace_back("total");
  return out;
}

}  // namespace sp::pricing
