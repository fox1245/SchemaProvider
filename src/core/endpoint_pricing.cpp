#include "core/endpoint_pricing.h"
#include "core/pricing.h"
#include <algorithm>

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

}  // namespace sp::pricing
