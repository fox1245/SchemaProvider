#include "core/endpoint_pricing.h"
#include "core/pricing.h"
#include "core/pricing_internal.h"
#include <algorithm>
#include <span>

namespace sp::pricing {
namespace {
// One row of an item unit that applies to a call.
struct ItemRow {
  std::string_view key, qualifier;
  std::size_t dimensions = 0;
  std::uint64_t above = 0, rate = 0;
};
// The rows of `unit` that apply to the call (endpoint_pricing.h), in the entry's order. Nothing when a row that applies
// has a price that is not a fixed amount, or when the call's dimensions are not sorted by key with each key once: a
// row skipped then could only make the dearest row a cheaper one.
std::optional<std::vector<ItemRow>> applying_rows(const descriptor::CatalogueEntry& entry, std::string_view unit,
                                                  const std::vector<std::pair<std::string, std::string>>& dimensions,
                                                  std::string_view tier, std::uint64_t input_tokens) {
  for (std::size_t i = 1; i < dimensions.size(); ++i)
    if (!(dimensions[i - 1].first < dimensions[i].first)) return std::nullopt;
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
// An item unit with the dimensions a call offers, in the qualifier's spelling: `item:image[operation=generate,quality=high]`.
std::string with_dimensions(std::string_view unit, const std::vector<std::pair<std::string, std::string>>& dimensions) {
  std::string out = std::string(unit) + "[";
  for (std::size_t i = 0; i < dimensions.size(); ++i) out += (i ? "," : "") + dimensions[i].first + "=" + dimensions[i].second;
  return out + "]";
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
  if (facts.billing == descriptor::EndpointBilling::Carried) {
    block(UnboundedBy::Unmodeled, facts.endpoint + "/" + facts.operation, "the call bills the chat requests it carries, which this bound does not price");
    return out;
  }
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
  if (!out.unbounded.empty()) return out;

  const auto* entry = catalogue.find(facts.family, facts.origin, facts.model).entry;
  if (!entry || entry->pricing.empty()) block(UnboundedBy::ModelPrice, facts.model, "the catalogue has no price for the model at the origin");
  if (facts.routed_gateway)
    block(UnboundedBy::RouteCeiling, facts.model, "an endpoint call at a routing gateway has no per-item route ceiling and no pin");
  if (!out.unbounded.empty()) return out;
  const std::string_view tier = in.service_tier;
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
  if (ceiling) {
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
    }
    if (images && !token_rows(*entry, image_token_units, tier, length).base) {
      block(UnboundedBy::ModelPrice, std::string(image_token_units[0]), "catalogue entry '" + entry->id + "' lists no rate for the input's image tokens");
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
  tokens(input_units, UnboundedBy::InputBound, "no measured input and no catalogue input limit", image_tokens);
  tokens(output_units, UnboundedBy::OutputCap, "the request states no output cap and the entry no output limit", false);
  // The request fee, once.
  if (const auto fee = token_rows(*entry, request_units, tier, length); !fee.variable.empty())
    block(UnboundedBy::ModelPrice, std::string(request_units[0]), "catalogue entry '" + entry->id + "' prices it at a variable price (" + std::string(fee.variable) + ")");
  else if (fee.dearest)
    charge(request_units[0], fee.dearest->second, 1, fee.dearest->first);
  // The items, each at the dearest row the call's dimensions match.
  std::vector<std::string_view> counted;
  for (std::size_t i = 0; i < quantities.size(); ++i) {
    const auto& q = quantities[i];
    if (!q.unit.starts_with(descriptor::item_unit_prefix)) continue;
    counted.push_back(q.unit);
    if (counts[i] && *counts[i] == 0) continue;
    const auto rate = dearest_item_price(*entry, q.unit, offered, tier, length);
    if (!rate) {
      block(UnboundedBy::ModelPrice, with_dimensions(q.unit, offered),
            applying_rows(*entry, q.unit, offered, tier, length) ? "catalogue entry '" + entry->id + "' has no row of the unit that applies to the call"
                                                                 : "a row of the unit that applies to the call has a variable price");
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
    if (std::find(counted.begin(), counted.end(), item->unit) != counted.end() || !descriptor::qualifier_offered(item->qualifier, offered)) continue;
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
  return out;
}

}  // namespace sp::pricing
