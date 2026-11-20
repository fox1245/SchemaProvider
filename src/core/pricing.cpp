#include "core/pricing.h"
#include "core/pricing_internal.h"
#include "descriptor/policy.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace sp::pricing {
namespace {
using detail::Wide;
// The fractional digits of a rate: nano-USD per million units is USD x 1e15.
constexpr unsigned rate_digits = 15;
} // namespace

// The catalogue reads listed prices the same way (descriptor::price_per_million), so one reader decides what a price is.
std::optional<std::uint64_t> per_million(std::string_view text) { return descriptor::price_per_million(text); }

std::string decimal_text(std::uint64_t rate) {
  std::string fraction(rate_digits, '0');
  for (auto digit = fraction.rbegin(); digit != fraction.rend(); ++digit, rate /= 10) *digit = static_cast<char>('0' + rate % 10);
  while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
  const auto whole = std::to_string(rate);
  return fraction.empty() ? whole : whole + '.' + fraction;
}

namespace {
using detail::parse_unit;
using detail::select;
// The plain name of a unit OpenRouter names otherwise, as the catalogue reads it.
std::string_view canonical(std::string_view name) { return descriptor::plain_price_unit(name); }
// The member of ModelPrice that holds the unit of plain name `name`, or none.
std::optional<std::uint64_t> ModelPrice::* slot_of(std::string_view name) {
  constexpr std::pair<std::string_view, std::optional<std::uint64_t> ModelPrice::*> slots[]{
      {"input", &ModelPrice::input},           {"output", &ModelPrice::output},
      {"cache_read", &ModelPrice::cache_read}, {"cache_write", &ModelPrice::cache_write},
      {"reasoning", &ModelPrice::reasoning},   {"request", &ModelPrice::request},
      {"cache_write_1h", &ModelPrice::cache_write_1h}, {"output_image", &ModelPrice::output_image},
      {"input_audio", &ModelPrice::input_audio}, {"output_audio", &ModelPrice::output_audio},
      {"ceiling_input", &ModelPrice::ceiling_input}, {"ceiling_output", &ModelPrice::ceiling_output},
      {"ceiling_request", &ModelPrice::ceiling_request}};
  for (const auto& [unit, slot] : slots)
    if (unit == name) return slot;
  return nullptr;
}
// The unit of the image tokens of an input, which only the endpoint pricing reads.
constexpr std::string_view input_image_unit = "input_image";
// Whether the selection reads the unit of plain name `name`.
bool held(std::string_view name) {
  return slot_of(name) || name.starts_with(descriptor::tool_fee_prefix) || name == input_image_unit;
}
}  // namespace

namespace detail {
// Exact: each unit's price from the highest band the request is above, the plain name before OpenRouter's in one band.
// Dearest: each unit's highest price over every band the request may reach. Either way a unit with a row that applies
// at a price that is not a fixed amount has no price, since the provider may bill that row's price.
Selection select(const descriptor::CatalogueEntry& entry, std::string_view service_tier, std::uint64_t input_tokens, bool dearest) {
  struct Pick { std::uint64_t above = 0; bool plain = false; std::optional<std::uint64_t> rate; std::string_view key; };
  std::vector<std::pair<std::string, Pick>> picks;
  Selection out;
  for (const auto& [text_unit, text] : entry.pricing) {
    const auto unit = parse_unit(text_unit);
    if (!unit || unit->tier != service_tier || (unit->above && input_tokens <= unit->above)) continue;
    const auto name = canonical(unit->name);
    const auto rate = per_million(text);
    if (!rate) {
      if (held(name) && !out.is_variable(name)) out.variable.emplace_back(std::string(name), text_unit);
      continue;
    }
    const bool plain = name == unit->name;
    auto found = std::find_if(picks.begin(), picks.end(), [&](const auto& p) { return p.first == name; });
    if (found == picks.end()) { picks.emplace_back(std::string(name), Pick{unit->above, plain, rate, text_unit}); continue; }
    auto& pick = found->second;
    if (dearest) {
      if (*rate > *pick.rate) pick = Pick{unit->above, plain, rate, text_unit};
    } else if (unit->above > pick.above || (unit->above == pick.above && plain && !pick.plain)) {
      pick = Pick{unit->above, plain, rate, text_unit};
    }
  }
  for (const auto& [name, pick] : picks) {
    const auto slot = slot_of(name);
    const bool tool = name.starts_with(descriptor::tool_fee_prefix);
    out.priced = out.priced || slot || tool;
    if (out.is_variable(name) || !held(name)) continue;
    if (slot) out.price.*slot = pick.rate;
    else if (tool) out.price.tool_fees.emplace_back(name.substr(descriptor::tool_fee_prefix.size()), *pick.rate);
    else out.input_image = pick.rate;
    out.keys.emplace_back(name, pick.key);
  }
  std::sort(out.price.tool_fees.begin(), out.price.tool_fees.end());
  return out;
}
}  // namespace detail

ModelPrice price_of(const descriptor::CatalogueEntry& entry, std::string_view service_tier, std::uint64_t input_tokens) {
  return select(entry, service_tier, input_tokens, false).price;
}
namespace {
// Whether the entry has prices in a service tier other than `service_tier`.
bool priced_in_other_tier(const descriptor::CatalogueEntry& entry, std::string_view service_tier, std::uint64_t input_tokens) {
  for (const auto& [text_unit, text] : entry.pricing) {
    const auto unit = parse_unit(text_unit);
    if (unit && unit->tier != service_tier && per_million(text) && select(entry, unit->tier, input_tokens, false).priced) return true;
  }
  return false;
}
}  // namespace
// With a variable row that applies, the price has no rate at all: a bound over a price that only left that unit out would
// read the other units' rates and be lower than the bill.
ModelPrice dearest_price_of(const descriptor::CatalogueEntry& entry, std::string_view service_tier,
                            std::uint64_t input_tokens) {
  auto selected = select(entry, service_tier, input_tokens, true);
  return selected.variable.empty() ? std::move(selected.price) : ModelPrice{};
}

namespace {
// The Usage::extra keys of a modality breakdown: Generate `<group>Details.<MODALITY>` and Interactions
// `<group>_by_modality.<modality>`.
// The Google Live codec writes the output breakdown under `responseTokensDetails.`.
constexpr std::array<std::string_view, 2> input_breakdown{"promptTokensDetails.", "input_tokens_by_modality."};
constexpr std::array<std::string_view, 3> output_breakdown{"candidatesTokensDetails.", "responseTokensDetails.", "output_tokens_by_modality."};
// The counters that hold the tokens of one modality when the usage has no breakdown, the first one present winning:
// Chat's own (`input_audio`, `audio_tokens`, `completion_tokens_details.image_tokens`) and the OpenAI realtime codec's
// dotted paths.
constexpr std::array<std::string_view, 2> input_audio_counters{"input_audio", "input_token_details.audio_tokens"};
constexpr std::array<std::string_view, 2> output_audio_counters{"audio_tokens", "output_token_details.audio_tokens"};
constexpr std::array<std::string_view, 1> output_image_counters{"completion_tokens_details.image_tokens"};
// The tokens of one modality in a breakdown, matched without regard to case. Empty when the usage has none.
std::optional<std::uint64_t> modality_tokens(const Usage& usage, std::span<const std::string_view> prefixes,
                                             std::string_view modality) {
  std::optional<std::uint64_t> tokens;
  for (const std::string_view prefix : prefixes) {
    for (auto it = usage.extra.lower_bound(std::string(prefix)); it != usage.extra.end() && it->first.starts_with(prefix); ++it) {
      const std::string_view name = std::string_view(it->first).substr(prefix.size());
      if (name.find('.') != std::string_view::npos) continue;
      if (!tokens) tokens = 0;
      const bool match = name.size() == modality.size() && std::equal(name.begin(), name.end(), modality.begin(),
          [](char a, char b) { return (a >= 'A' && a <= 'Z' ? a + 32 : a) == b; });
      if (match) *tokens = it->second.value > std::numeric_limits<std::uint64_t>::max() - *tokens
                               ? std::numeric_limits<std::uint64_t>::max() : *tokens + it->second.value;
    }
  }
  return tokens;
}
}  // namespace

namespace detail {
UsageSum usage_sum(const Selection& selection, const Usage& usage, const UsageRules& rules) {
  UsageSum out;
  const auto& price = selection.price;
  auto count = [](const std::optional<Count>& c) -> std::optional<std::uint64_t> { return c ? std::optional<std::uint64_t>(c->value) : std::nullopt; };
  const auto cache_read = count(usage.cache_read).value_or(0), cache_write = count(usage.cache_write).value_or(0);
  std::optional<std::uint64_t> uncached = count(usage.input_uncached);
  if (!uncached && usage.input_total) {
    // Cache counters that sum past 64 bits are above any total.
    const auto total = usage.input_total->value;
    std::uint64_t cached = 0;
    if (!detail::checked_add(cache_read, cache_write, cached)) cached = detail::kMax;
    uncached = total >= cached ? total - cached : 0;
  }
  auto& sum = out.sum;  // nano-USD x 1e6
  auto& complete = out.complete;
  auto assume = [&](std::string_view rule) {
    if (std::find(out.assumptions.begin(), out.assumptions.end(), rule) == out.assumptions.end()) out.assumptions.emplace_back(rule);
  };
  // Tokens priced at the rate of `unit`, named `name` when they have no price. `base` is the rate of the unit this one
  // refines. A zero rate under a priced base bills those tokens as free.
  auto charge = [&](std::string_view unit, std::uint64_t units, const std::optional<std::uint64_t>& rate, std::string_view name,
                    const std::optional<std::uint64_t>& base = std::nullopt) {
    if (!units) return;
    if (!rate) { out.unpriced.emplace_back(name); complete = false; return; }
    if (!*rate && base && *base) assume(std::string(name) + " at zero");
    sum += Wide::product(units, *rate);
    out.charges.push_back({unit, key_of(selection.keys, unit), units, *rate});
  };
  auto extra = [&](std::string_view counter) -> std::uint64_t {
    const auto found = usage.extra.find(std::string(counter));
    return found == usage.extra.end() ? 0 : found->second.value;
  };
  // The tokens a usage reports in one of `counters`, the first one present; nothing when it reports none.
  auto counter_tokens = [&](std::span<const std::string_view> counters) -> std::optional<std::uint64_t> {
    for (const std::string_view counter : counters)
      if (const auto found = usage.extra.find(std::string(counter)); found != usage.extra.end()) return found->second.value;
    return std::nullopt;
  };
  // The audio tokens of the input or the output: a modality breakdown, else a counter that holds them.
  auto audio_of = [&](std::span<const std::string_view> counters, std::span<const std::string_view> prefixes) {
    const auto breakdown = modality_tokens(usage, prefixes, "audio");
    return breakdown ? *breakdown : counter_tokens(counters).value_or(0);
  };
  // Gemini bills the tool-use prompt beyond the prompt count; no price here covers it.
  if (extra("toolUsePromptTokenCount")) { out.unpriced.emplace_back("toolUsePromptTokenCount"); complete = false; }
  // A Messages compaction iteration is billed at the model's rates beside the top-level counters.
  const bool compaction = rules.compaction;
  if (rules.route_ceiling) {
    // The served endpoint is unknown: every input token at the ceiling's input rate, every output token at its
    // output rate.
    // A ceiling the entry does not list is "ceiling"; a ceiling row at a variable price is named, and so, when every
    // ceiling is listed, is a variable request fee.
    const bool absent = (!price.ceiling_input && !selection.is_variable("ceiling_input")) ||
                        (!price.ceiling_output && !selection.is_variable("ceiling_output"));
    bool variable_ceiling = false;
    if (absent) out.unpriced.emplace_back("ceiling");
    for (const std::string_view unit : {"ceiling_input", "ceiling_output"})
      if (selection.is_variable(unit)) { out.unpriced.emplace_back(unit); variable_ceiling = true; }
    if (!absent && variable_ceiling)
      for (const std::string_view unit : {"request", "ceiling_request"})
        if (selection.is_variable(unit)) out.unpriced.emplace_back(unit);
    if (absent || variable_ceiling) { complete = false; return out; }
    assume("route ceiling");
    if (!uncached) { if (rules.input) { out.unpriced.emplace_back("input"); complete = false; } }
    else charge("ceiling_input", *uncached, price.ceiling_input, "ceiling");
    charge("ceiling_input", cache_read, price.ceiling_input, "ceiling");
    charge("ceiling_input", cache_write, price.ceiling_input, "ceiling");
    if (!usage.output_total) { if (rules.output) { out.unpriced.emplace_back("output"); complete = false; } }
    else charge("ceiling_output", usage.output_total->value, price.ceiling_output, "ceiling");
    if (compaction) {
      for (const auto counter : {"compaction.input_tokens", "compaction.cache_read_input_tokens", "compaction.cache_creation_input_tokens"})
        charge("ceiling_input", extra(counter), price.ceiling_input, "ceiling");
      charge("ceiling_output", extra("compaction.output_tokens"), price.ceiling_output, "ceiling");
    }
    const auto fee = std::max(price.request.value_or(0), price.ceiling_request.value_or(0));
    if (!complete) return out;
    // A fee of a row with a price that is not a fixed amount is not known, not zero.
    for (const std::string_view unit : {"request", "ceiling_request"})
      if (selection.is_variable(unit)) { out.unpriced.emplace_back(unit); complete = false; }
    if (!complete) return out;
    sum += fee;
    if (fee) {
      const std::string_view unit = price.request.value_or(0) >= price.ceiling_request.value_or(0) ? "request" : "ceiling_request";
      out.charges.push_back({unit, key_of(selection.keys, unit), 1, fee});
    }
  } else {
    // Audio input bills at its own rate, out of the uncached input. Audio the cache counters hold has no split.
    std::uint64_t audio_in = 0;
    if (const auto audio = audio_of(input_audio_counters, input_breakdown); audio && uncached) {
      if (price.input_audio && audio <= *uncached) audio_in = audio;
      else { out.unpriced.emplace_back("input_audio"); complete = false; }
    }
    // The image tokens of the input, when the caller counts them, the same way at `input_image`.
    std::uint64_t image_in = 0;
    if (rules.input_image && *rules.input_image && uncached) {
      if (selection.input_image && *rules.input_image <= *uncached - audio_in) image_in = *rules.input_image;
      else { out.unpriced.emplace_back(input_image_unit); complete = false; }
    }
    if (!uncached) { if (rules.input) { out.unpriced.emplace_back("input"); complete = false; } }
    else {
      charge("input", *uncached - audio_in - image_in, price.input, "input");
      charge("input_audio", audio_in, price.input_audio, "input_audio", price.input);
      charge(input_image_unit, image_in, selection.input_image, input_image_unit, price.input);
    }
    // The rate a unit bills at, and the unit that gives it. A unit with a variable price is not billed at another
    // unit's rate.
    struct Rate { std::optional<std::uint64_t> value; std::string_view unit; };
    auto fallback = [&](std::string_view unit, const std::optional<std::uint64_t>& specific, const std::optional<std::uint64_t>& general,
                        std::string_view rule, std::uint64_t units) {
      if (specific || !units || selection.is_variable(unit)) return Rate{specific, unit};
      if (general) assume(rule);
      return Rate{general, "input"};
    };
    const auto read_rate = fallback("cache_read", price.cache_read, price.input, "cache_read at input", cache_read);
    charge(read_rate.unit, cache_read, read_rate.value, "cache_read", price.input);
    // Messages splits cache writes into 5-minute and 1-hour writes; without the split, all of them bill at the
    // dearer rate.
    const auto write = fallback("cache_write", price.cache_write, price.input, "cache_write at input", cache_write);
    const auto five = usage.extra.find("cache_creation.ephemeral_5m_input_tokens");
    const auto hour = usage.extra.find("cache_creation.ephemeral_1h_input_tokens");
    // The writes a variable write rate may price: the 5-minute or 1-hour ones by the split, else all of them.
    const bool split = cache_write && five != usage.extra.end() && hour != usage.extra.end() && five->second.value <= cache_write &&
                       hour->second.value == cache_write - five->second.value;
    const bool variable_write = selection.is_variable("cache_write") && (split ? five->second.value : cache_write) > 0;
    const bool variable_hour = selection.is_variable("cache_write_1h") && (split ? hour->second.value : cache_write) > 0;
    if (variable_write || variable_hour) {
      if (variable_write) { out.unpriced.emplace_back("cache_write"); complete = false; }
      if (variable_hour) { out.unpriced.emplace_back("cache_write_1h"); complete = false; }
    } else if (price.cache_write_1h && cache_write && five != usage.extra.end() && hour != usage.extra.end() &&
        five->second.value <= cache_write && hour->second.value == cache_write - five->second.value) {
      charge(write.unit, five->second.value, write.value, "cache_write", price.input);
      charge("cache_write_1h", hour->second.value, price.cache_write_1h, "cache_write_1h", write.value);
    } else if (price.cache_write_1h && cache_write && (!write.value || *price.cache_write_1h > *write.value)) {
      assume("cache_write at cache_write_1h");
      charge("cache_write_1h", cache_write, price.cache_write_1h, "cache_write_1h", write.value);
    } else {
      if (hour != usage.extra.end() && hour->second.value && !price.cache_write_1h) assume("cache_write_1h at cache_write");
      charge(write.unit, cache_write, write.value, "cache_write", price.input);
    }
    if (!usage.output_total) { if (rules.output) { out.unpriced.emplace_back("output"); complete = false; } }
    else {
      const auto output = usage.output_total->value;
      if (selection.is_variable("reasoning") && std::min(count(usage.reasoning).value_or(0), output)) { out.unpriced.emplace_back("reasoning"); complete = false; }
      const auto reasoning = price.reasoning ? std::min(count(usage.reasoning).value_or(0), output) : 0;
      charge("reasoning", reasoning, price.reasoning, "reasoning", price.output);
      // Gemini reports image output tokens by modality (Generate `candidatesTokensDetails.IMAGE`, Interactions
      // `output_tokens_by_modality.image`) and a gateway's Chat in `completion_tokens_details.image_tokens`. Without
      // either, all output bills at the dearer rate.
      auto image = modality_tokens(usage, output_breakdown, "image");
      if (!image) image = counter_tokens(output_image_counters);
      // Without a breakdown, any output token may be an image token at the variable rate.
      if (selection.is_variable("output_image") && !image && output > reasoning) { out.unpriced.emplace_back("output_image"); complete = false; }
      if (!price.output_image) {
        if (image && *image) { out.unpriced.emplace_back("output_image"); complete = false; }
        image.reset();
      } else if (!(output > reasoning && price.output_image != price.output)) {
        image.reset();
      } else if (!image && (!price.output || *price.output_image > *price.output)) {
        assume("output at output_image");
        image = output - reasoning;
      }
      const auto images = std::min(image.value_or(0), output - reasoning);
      charge("output_image", images, price.output_image, "output_image", price.output);
      // Audio output bills at its own rate, out of what is left.
      std::uint64_t audio_out = 0;
      if (const auto audio = audio_of(output_audio_counters, output_breakdown)) {
        if (price.output_audio) audio_out = std::min(audio, output - reasoning - images);
        else { out.unpriced.emplace_back("output_audio"); complete = false; }
      }
      charge("output_audio", audio_out, price.output_audio, "output_audio", price.output);
      charge("output", output - reasoning - images - audio_out, price.output, "output");
    }
    if (compaction) {
      const auto read = extra("compaction.cache_read_input_tokens"), written = extra("compaction.cache_creation_input_tokens");
      const auto compacted_output = extra("compaction.output_tokens");
      // The iteration's writes have no 5-minute and 1-hour split and its output no modality breakdown, so a variable
      // 1-hour or image output rate may price any of them, as it does the top-level counters without one. Named once.
      const auto variable = [&](std::string_view unit, std::uint64_t units) {
        if (!units || !selection.is_variable(unit)) return;
        if (std::find(out.unpriced.begin(), out.unpriced.end(), unit) == out.unpriced.end()) out.unpriced.emplace_back(unit);
        complete = false;
      };
      charge("input", extra("compaction.input_tokens"), price.input, "input");
      const auto compaction_read = fallback("cache_read", price.cache_read, price.input, "cache_read at input", read);
      charge(compaction_read.unit, read, compaction_read.value, "cache_read", price.input);
      const auto compaction_write = fallback("cache_write", price.cache_write, price.input, "cache_write at input", written);
      charge(compaction_write.unit, written, compaction_write.value, "cache_write", price.input);
      variable("cache_write_1h", written);
      charge("output", compacted_output, price.output, "output");
      variable("output_image", compacted_output);
    }
    if (selection.is_variable("request")) { out.unpriced.emplace_back("request"); complete = false; }
    else if (price.request) {
      sum += *price.request;
      out.charges.push_back({"request", key_of(selection.keys, "request"), 1, *price.request});
    }
  }
  return out;
}
}  // namespace detail

CostEstimate estimate(const descriptor::ModelCatalogue& catalogue, std::string_view family, std::string_view origin,
                      const ServedModel& model, const Usage& usage, std::string_view service_tier, bool route_ceiling) {
  CostEstimate out;
  out.lower_bound = usage.stage == UsageStage::Partial || usage.quality == UsageQuality::Inconsistent;
  const descriptor::CatalogueEntry* entry = nullptr;
  detail::Selection selection;
  auto count_of = [](const std::optional<Count>& c) -> std::uint64_t { return c ? c->value : 0; };
  // The input length that picks the entry's band: every input token, cache reads and writes included. A sum past 64
  // bits is above every band.
  std::uint64_t input_length = 0;
  if (usage.input_total) input_length = usage.input_total->value;
  else
    for (const auto& counter : {usage.input_uncached, usage.cache_read, usage.cache_write})
      if (!detail::checked_add(input_length, count_of(counter), input_length)) input_length = detail::kMax;
  bool other_tier = false;
  auto priced = [&](std::string_view name) {
    if (name.empty()) return false;
    const auto match = catalogue.find(family, origin, name);
    if (!match.entry) return false;
    auto found = select(*match.entry, service_tier, input_length, false);
    if (!found.priced) {
      // The entry prices the model, but not in this service tier.
      other_tier = other_tier || priced_in_other_tier(*match.entry, service_tier, input_length);
      return false;
    }
    entry = match.entry; selection = std::move(found);
    return true;
  };
  const bool served = model.relation != ServedRelation::NotReported && !model.served.empty() && model.served != model.requested;
  const bool by_served = served && priced(model.served);
  if (!by_served && !priced(model.requested)) {
    out.unpriced.push_back(other_tier ? "tier" : "model");
    return out;
  }
  out.entry = entry->id;
  out.requested_entry = served && !by_served;
  detail::UsageRules rules;
  rules.compaction = family == "anthropic.messages";
  rules.route_ceiling = route_ceiling;
  auto summed = detail::usage_sum(selection, usage, rules);
  out.assumptions = std::move(summed.assumptions);
  out.unpriced = std::move(summed.unpriced);
  if (!summed.complete) return out;
  // Every part of the sum is rounded together, once.
  const auto nano = detail::rounded_nano_usd(summed.sum);
  if (!nano) { out.unpriced.emplace_back("overflow"); return out; }
  out.total = UsdAmount{*nano, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
  return out;
}

namespace {
// The bound of upper_bound, where `variable` names the units of `price` that a row applies to at a price that is not a
// fixed amount: a rate the bound reads among them is not known, and neither is the dearest of a set that holds it.
Bound bounded(const ModelPrice& price, const detail::UnitKeys& variable, const BoundRequest& request) {
  Bound out;
  auto dearest = [](std::initializer_list<std::optional<std::uint64_t>> rates) {
    std::optional<std::uint64_t> result;
    for (const auto& rate : rates)
      if (rate && (!result || *rate > *result)) result = rate;
    return result;
  };
  // A unit the price leaves out for a variable row is named; the unit a set needs is named when it is left out anyway.
  const auto name_variable = [&](std::initializer_list<std::string_view> units) {
    for (const auto unit : units)
      if (!detail::key_of(variable, unit).empty()) out.unpriced.emplace_back(unit);
  };
  std::optional<std::uint64_t> input, output, fee;
  if (request.route_ceiling) {
    input = price.ceiling_input;
    output = price.ceiling_output;
    fee = dearest({price.request, price.ceiling_request});
    // A ceiling the entry does not list is "ceiling"; a ceiling row at a variable price is named.
    // Beside a ceiling the entry does not list, the fees are not named, as the estimate and worst_case name them.
    const bool absent = (!input && detail::key_of(variable, "ceiling_input").empty()) || (!output && detail::key_of(variable, "ceiling_output").empty());
    if (absent) out.unpriced.emplace_back("ceiling");
    name_variable({"ceiling_input", "ceiling_output"});
    if (!absent) name_variable({"request", "ceiling_request"});
  } else {
    input = price.input ? dearest({price.input, price.cache_read, price.cache_write, price.cache_write_1h, price.input_audio}) : std::nullopt;
    output = price.output ? dearest({price.output, price.reasoning, price.output_image, price.output_audio}) : std::nullopt;
    fee = price.request;
    if (!input && request.input_tokens) out.unpriced.emplace_back("input");
    if (!output && request.output_tokens) out.unpriced.emplace_back("output");
    if (request.input_tokens) name_variable({"cache_read", "cache_write", "cache_write_1h", "input_audio"});
    if (request.output_tokens) name_variable({"reasoning", "output_image", "output_audio"});
    name_variable({"request"});
  }
  Wide sum;  // nano-USD x 1e6
  if (input) sum += Wide::product(request.input_tokens, *input);
  if (output) sum += Wide::product(request.output_tokens, *output);
  if (fee) sum += *fee;
  for (const auto& [type, uses] : request.tool_uses) {
    if (!uses) continue;
    const auto found = std::lower_bound(price.tool_fees.begin(), price.tool_fees.end(), type,
                                        [](const auto& entry, const std::string& name) { return entry.first < name; });
    if (found == price.tool_fees.end() || found->first != type) { out.unpriced.push_back(descriptor::tool_fee_unit(type)); continue; }
    sum += Wide::product(uses, found->second);
  }
  if (!out.unpriced.empty()) return out;
  sum += 999'999;
  const auto nano = sum.divided(1'000'000);
  if (!nano.fits()) { out.unpriced.emplace_back("overflow"); return out; }
  out.total = UsdAmount{nano.low, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
  return out;
}
}  // namespace

Bound upper_bound(const ModelPrice& price, const BoundRequest& request) { return bounded(price, {}, request); }

Bound upper_bound(const descriptor::CatalogueEntry& entry, std::string_view service_tier, const BoundRequest& request) {
  const auto selected = select(entry, service_tier, request.input_tokens, true);
  return bounded(selected.price, selected.variable, request);
}

std::string_view to_string(UnboundedBy by) noexcept {
  switch (by) {
    case UnboundedBy::OutputCap: return "OutputCap";
    case UnboundedBy::InputBound: return "InputBound";
    case UnboundedBy::Invocations: return "Invocations";
    case UnboundedBy::ModelPrice: return "ModelPrice";
    case UnboundedBy::TierPrice: return "TierPrice";
    case UnboundedBy::ToolFee: return "ToolFee";
    case UnboundedBy::ToolCap: return "ToolCap";
    case UnboundedBy::RouteCeiling: return "RouteCeiling";
    case UnboundedBy::Unmodeled: return "Unmodeled";
    case UnboundedBy::Overflow: return "Overflow";
    case UnboundedBy::Billing: return "Billing";
    case UnboundedBy::Quantity: return "Quantity";
  }
  return {};
}

namespace {
using detail::checked_add;
using detail::checked_mul;
using detail::smallest_limit;
constexpr std::optional<std::uint64_t> OpenRouterMaxPrice::* kMaxPriceFields[] = {
    &OpenRouterMaxPrice::prompt_nano_usd, &OpenRouterMaxPrice::completion_nano_usd, &OpenRouterMaxPrice::image_nano_usd,
    &OpenRouterMaxPrice::audio_nano_usd, &OpenRouterMaxPrice::request_nano_usd};
// Field by field, the lower of the route ceiling and the request's own cap; a field only the request caps keeps its amount.
OpenRouterMaxPrice lowered(OpenRouterMaxPrice ceiling, const std::optional<OpenRouterMaxPrice>& own) {
  if (!own) return ceiling;
  for (const auto member : kMaxPriceFields) {
    const auto& cap = (*own).*member;
    auto& pinned = ceiling.*member;
    if (cap && (!pinned || *cap < *pinned)) pinned = cap;
  }
  return ceiling;
}
bool same_max_price(const OpenRouterMaxPrice& left, const OpenRouterMaxPrice& right) {
  for (const auto member : kMaxPriceFields)
    if (left.*member != right.*member) return false;
  return true;
}
// The listed members (descriptor::unread_listed_members) the entry prices above zero, or at a price that is not a fixed
// amount, in any tier or band, whose feature the request carries: no bound reads them, so the bound has no total for
// the request.
std::vector<std::string_view> carried_unread_members(const descriptor::CatalogueEntry& entry, const RequestFacts& facts) {
  std::vector<std::string_view> out;
  for (const auto& [member, feature] : descriptor::unread_listed_members) {
    const bool carried = feature == descriptor::ListedFeature::ImageInput ? facts.image_input : facts.plugins;
    if (!carried) continue;
    for (const auto& [text_unit, text] : entry.pricing) {
      const auto unit = parse_unit(text_unit);
      if (!unit || unit->name != member) continue;
      const auto rate = per_million(text);
      if (!rate || *rate) { out.push_back(member); break; }
    }
  }
  return out;
}
// Why a unit has no rate: a row of it that applies has a price that is not a fixed amount.
std::string variable_rate(const descriptor::CatalogueEntry& entry, std::string_view unit, std::string_view key) {
  return "catalogue entry '" + entry.id + "' prices " + std::string(unit) + " at a variable price (" + std::string(key) + ")";
}
}  // namespace

WorstCase worst_case(const WorstCaseInput& in, const descriptor::ModelCatalogue& catalogue) {
  static const RequestFacts no_facts;
  const auto& facts = in.facts ? *in.facts : no_facts;
  WorstCase out;
  out.route_ceiling = in.routed_gateway;
  out.attempts = in.attempts;
  const auto block = [&](UnboundedBy by, std::string subject, std::string detail) {
    out.unbounded.push_back({by, std::move(subject), std::move(detail)});
  };
  if (facts.plugins) block(UnboundedBy::Unmodeled, "OpenRouter plugins", "the catalogue prices no plugin");
  if (!in.output_tokens) block(UnboundedBy::OutputCap, {}, "the request states no output cap");
  if (!in.invocations || *in.invocations == 0 || in.attempts == 0)
    block(UnboundedBy::Invocations, {}, "the request does not state how many model invocations it may bill");
  for (const auto& group : facts.tools) {
    if (group.uses || group.types.empty()) continue;
    block(UnboundedBy::ToolCap, group.types.front(),
          group.member.empty()   ? "the family takes no cap on the tool's uses"
          : !group.cap_admitted  ? "the policy admits no " + group.member + " for the tool"
                                 : "the request sets no " + group.member);
  }
  const std::uint64_t invocations = in.invocations && *in.invocations ? *in.invocations : 1;

  // Every model that may serve the call: the requested one, then the gateway's fallbacks.
  struct Candidate {
    std::string model;
    const descriptor::CatalogueEntry* entry = nullptr;
    std::uint64_t input = 0, invocation_input = 0;
    ModelPrice price;
    // The units a row applies to at a price that is not a fixed amount, with that row.
    detail::UnitKeys variable;
  };
  std::vector<Candidate> candidates;
  std::vector<std::string> models{std::string(in.model)};
  for (const auto& model : facts.fallback_models)
    if (std::find(models.begin(), models.end(), model) == models.end()) models.push_back(model);
  for (const auto& model : models) {
    const auto* entry = catalogue.find(in.family, in.origin, model).entry;
    if (!entry || entry->pricing.empty()) {
      block(UnboundedBy::ModelPrice, model, "the catalogue has no price for the model at the origin");
      continue;
    }
    const auto limit = smallest_limit({entry->input_limit, entry->context_window});
    const auto measured = model == in.model ? in.measured_input_tokens : std::nullopt;
    const auto first = measured ? (limit ? std::optional<std::uint64_t>((std::min)(*measured, *limit)) : measured) : limit;
    if (!first || (invocations > 1 && !limit)) {
      block(UnboundedBy::InputBound, model,
            !first ? "no measured input and no catalogue input limit" : "later invocations need a catalogue input limit");
      continue;
    }
    Candidate candidate{model, entry, *first, invocations > 1 ? *limit : *first, {}, {}};
    std::uint64_t rest = 0;
    if (invocations > 1 && (!checked_mul(invocations - 1, *limit, rest) || !checked_add(candidate.input, rest, candidate.input))) {
      block(UnboundedBy::Overflow, model, "the input tokens do not fit 64 bits");
      continue;
    }
    auto selected = select(*entry, in.service_tier, candidate.invocation_input, true);
    candidate.price = selected.price;
    candidate.variable = std::move(selected.variable);
    if (!selected.priced) {
      block(UnboundedBy::TierPrice, model,
            in.service_tier.empty() ? "the entry prices no unit at the standard rates"
                                    : "the entry prices no unit under service tier '" + std::string(in.service_tier) + "'");
      continue;
    }
    // A price beyond tokens that no unit reads, for something the request carries: the bound would be a total below the bill.
    if (const auto unread = carried_unread_members(*entry, facts); !unread.empty()) {
      for (const auto member : unread)
        block(UnboundedBy::ModelPrice, model,
              "catalogue entry '" + entry->id + "' lists a price for '" + std::string(member) + "' that no bound reads, and the request carries what it prices");
      continue;
    }
    const bool has_ceiling = candidate.price.ceiling_input || candidate.price.ceiling_output || candidate.price.ceiling_request;
    if (!in.routed_gateway && has_ceiling) {
      // The bound holds only if the router cannot pick a dearer endpoint, and only a routing gateway takes the pin.
      block(UnboundedBy::RouteCeiling, model, "the entry lists a route ceiling but the origin is not a routing gateway, so it cannot be pinned");
      continue;
    }
    if (in.routed_gateway) {
      // A ceiling the entry does not list leaves the router free; a ceiling row at a variable price is named, and so,
      // when every ceiling is listed, is a variable request fee, which the pin would state.
      const bool absent = (!candidate.price.ceiling_input && detail::key_of(candidate.variable, "ceiling_input").empty()) ||
                          (!candidate.price.ceiling_output && detail::key_of(candidate.variable, "ceiling_output").empty());
      if (absent) block(UnboundedBy::RouteCeiling, model, "the entry lists no route ceiling (ceiling_input, ceiling_output), so the router may pick any price");
      bool variable = false;
      for (const std::string_view unit : {"ceiling_input", "ceiling_output"})
        if (const auto key = detail::key_of(candidate.variable, unit); !key.empty()) {
          block(UnboundedBy::ModelPrice, model, variable_rate(*entry, unit, key));
          variable = true;
        }
      if (absent) continue;
      for (const std::string_view unit : {"request", "ceiling_request"})
        if (const auto key = detail::key_of(candidate.variable, unit); !key.empty()) {
          block(UnboundedBy::ModelPrice, model, variable_rate(*entry, unit, key));
          variable = true;
        }
      if (variable) continue;
    }
    candidates.push_back(std::move(candidate));
  }
  if (!out.unbounded.empty()) return out;

  if (in.routed_gateway) {
    // One pin covers every candidate, so each is bounded at the dearest ceiling.
    OpenRouterMaxPrice ceiling;
    std::optional<std::uint64_t> listed_request;
    for (const auto& candidate : candidates) {
      ceiling.prompt_nano_usd = (std::max)(ceiling.prompt_nano_usd.value_or(0), *candidate.price.ceiling_input);
      ceiling.completion_nano_usd = (std::max)(ceiling.completion_nano_usd.value_or(0), *candidate.price.ceiling_output);
      // The request fee the bound charges at a gateway is the dearer of the listed fee and the ceiling (upper_bound), so
      // the pin has to state it: the router could otherwise pick an endpoint with a dearer one.
      const auto fee = (std::max)(candidate.price.request.value_or(0), candidate.price.ceiling_request.value_or(0));
      if (fee) listed_request = (std::max)(listed_request.value_or(0), fee);
    }
    // A request fee is listed in the scale of the rates, a million times its nano-USD amount; the pin states whole
    // nano-USD, rounded up, and the bound charges exactly what the pin allows.
    constexpr std::uint64_t scale = 1'000'000;
    std::optional<std::uint64_t> pinned_request;
    if (listed_request) {
      ceiling.request_nano_usd = *listed_request / scale + (*listed_request % scale != 0 ? 1 : 0);
      std::uint64_t exact = 0;
      if (!checked_mul(*ceiling.request_nano_usd, scale, exact)) {
        block(UnboundedBy::Overflow, std::string(in.model), "the route ceiling does not fit 64 bits");
        return out;
      }
      pinned_request = exact;
    }
    if (*ceiling.prompt_nano_usd > max_price_limit_nano_usd || *ceiling.completion_nano_usd > max_price_limit_nano_usd ||
        ceiling.request_nano_usd.value_or(0) > max_price_limit_nano_usd) {
      block(UnboundedBy::RouteCeiling, std::string(in.model), "the route ceiling exceeds the largest provider.max_price amount");
      return out;
    }
    const auto own = facts.routing ? facts.routing->max_price : std::nullopt;
    out.pin = lowered(ceiling, own);
    out.pinned = own && same_max_price(*own, *out.pin);
    for (auto& candidate : candidates) {
      candidate.price.ceiling_input = ceiling.prompt_nano_usd;
      candidate.price.ceiling_output = ceiling.completion_nano_usd;
      candidate.price.ceiling_request = pinned_request;
    }
  }

  std::uint64_t output = 0;
  if (!checked_mul(*in.output_tokens, invocations, output)) {
    block(UnboundedBy::Overflow, {}, "the output tokens do not fit 64 bits");
    return out;
  }
  std::uint64_t best = 0;
  for (const auto& candidate : candidates) {
    // Each group of tools that shares a cap is bounded at its dearest fee.
    std::vector<std::pair<std::string, std::uint64_t>> uses;
    ModelPrice fees;
    bool priced = true;
    for (const auto& group : facts.tools) {
      std::optional<std::pair<std::string, std::uint64_t>> dearest;
      for (const auto& type : group.types) {
        const auto found = std::find_if(candidate.price.tool_fees.begin(), candidate.price.tool_fees.end(),
                                        [&](const auto& fee) { return fee.first == type; });
        if (found == candidate.price.tool_fees.end()) {
          const auto unit = descriptor::tool_fee_unit(type);
          const auto key = detail::key_of(candidate.variable, unit);
          block(UnboundedBy::ToolFee, type, key.empty() ? "catalogue entry '" + candidate.entry->id + "' lists no fee (unit " + unit + ")"
                                                        : variable_rate(*candidate.entry, unit, key));
          priced = false;
          break;
        }
        if (!dearest || found->second > dearest->second) dearest = *found;
      }
      if (!priced) break;
      if (!dearest) continue;
      uses.emplace_back(dearest->first, *group.uses);
      fees.tool_fees.push_back(*dearest);
    }
    if (!priced) continue;
    std::sort(fees.tool_fees.begin(), fees.tool_fees.end());
    const auto bound = bounded(candidate.price, candidate.variable, {candidate.input, output, in.routed_gateway, uses});
    if (!bound.total) {
      for (const auto& name : bound.unpriced) {
        const auto variable = detail::key_of(candidate.variable, name);
        if (name == "overflow") block(UnboundedBy::Overflow, candidate.model, "the bound does not fit 64 bits");
        else if (name == "ceiling") block(UnboundedBy::RouteCeiling, candidate.model, "no route ceiling");
        else if (name.starts_with(descriptor::tool_fee_prefix)) block(UnboundedBy::ToolFee, name.substr(descriptor::tool_fee_prefix.size()), "no fee");
        else if (!variable.empty()) block(UnboundedBy::ModelPrice, candidate.model, variable_rate(*candidate.entry, name, variable));
        else block(UnboundedBy::ModelPrice, candidate.model, "no " + name + " rate");
      }
      continue;
    }
    if (!out.entry.empty() && bound.total->nano_usd <= best) continue;
    best = bound.total->nano_usd;
    out.entry = candidate.entry->id;
    out.model = candidate.model;
    out.input_tokens = candidate.input;
    out.invocation_input_tokens = candidate.invocation_input;
    out.tool_uses = std::move(uses);
    const auto tools = upper_bound(fees, {0, 0, false, out.tool_uses});
    out.tool_fees_nano_usd = tools.total ? tools.total->nano_usd : 0;
  }
  if (!out.unbounded.empty()) { out.entry.clear(); out.model.clear(); out.tool_uses.clear(); return out; }
  std::uint64_t total = 0;
  if (!checked_mul(best, in.attempts, total)) {
    block(UnboundedBy::Overflow, {}, "the bound over the attempts does not fit 64 bits");
    return out;
  }
  out.output_tokens = output;
  out.invocations = invocations;
  out.total = UsdAmount{total, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
  return out;
}

} // namespace sp::pricing
