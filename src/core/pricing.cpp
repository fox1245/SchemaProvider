#include "core/pricing.h"
#include "descriptor/policy.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace sp::pricing {
namespace {
// An unsigned 128-bit value in two 64-bit halves (MSVC has no 128-bit integer). `overflow` records a carry out of
// 128 bits.
struct Wide {
  std::uint64_t high = 0, low = 0;
  bool overflow = false;

  static Wide product(std::uint64_t a, std::uint64_t b) {
    constexpr std::uint64_t mask = 0xffffffffu;
    const std::uint64_t ll = (a & mask) * (b & mask), lh = (a & mask) * (b >> 32), hl = (a >> 32) * (b & mask),
                        hh = (a >> 32) * (b >> 32);
    const std::uint64_t middle = (ll >> 32) + (lh & mask) + (hl & mask);
    return {hh + (lh >> 32) + (hl >> 32) + (middle >> 32), (middle << 32) | (ll & mask), false};
  }
  Wide& operator+=(const Wide& other) {
    const auto old_low = low, old_high = high;
    low += other.low;
    const std::uint64_t carry = low < old_low ? 1 : 0;
    high += other.high;
    const bool wrapped = high < old_high;
    const auto before_carry = high;
    high += carry;
    overflow = overflow || other.overflow || wrapped || high < before_carry;
    return *this;
  }
  Wide& operator+=(std::uint64_t value) { return *this += Wide{0, value, false}; }
  // value * 10 + digit
  Wide shifted(unsigned digit) const {
    auto result = product(low, 10);
    const auto top = product(high, 10);
    result.overflow = overflow || top.high != 0 || result.high > std::numeric_limits<std::uint64_t>::max() - top.low;
    result.high += top.low;
    result += digit;
    return result;
  }
  bool fits() const { return !overflow && high == 0; }
  // The quotient by a divisor below 2^32, by 32-bit long division.
  Wide divided(std::uint32_t divisor) const {
    const std::uint64_t limbs[4] = {high >> 32, high & 0xffffffffu, low >> 32, low & 0xffffffffu};
    std::uint64_t quotient[4] = {}, remainder = 0;
    for (int i = 0; i < 4; ++i) {
      const auto current = (remainder << 32) | limbs[i];
      quotient[i] = current / divisor;
      remainder = current % divisor;
    }
    return {(quotient[0] << 32) | quotient[1], (quotient[2] << 32) | quotient[3], overflow};
  }
};

// Decimal USD per unit as nano-USD per million units (USD x 1e15), rounded up past the fifteenth fractional digit.
std::optional<std::uint64_t> per_million(std::string_view text) {
  if (text.empty() || text.front() == '-') return std::nullopt;
  const auto dot = text.find('.');
  const auto whole = text.substr(0, dot);
  const auto fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  constexpr unsigned scale = 15;
  Wide value;
  for (const char c : whole) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value.shifted(static_cast<unsigned>(c - '0'));
    if (!value.fits()) return std::nullopt;
  }
  bool remainder = false;
  for (unsigned i = 0; i < scale; ++i) {
    const char c = i < fraction.size() ? fraction[i] : '0';
    if (c < '0' || c > '9') return std::nullopt;
    value = value.shifted(static_cast<unsigned>(c - '0'));
  }
  for (std::size_t i = scale; i < fraction.size(); ++i) {
    if (fraction[i] < '0' || fraction[i] > '9') return std::nullopt;
    remainder = remainder || fraction[i] != '0';
  }
  if (remainder) value += 1;
  if (!value.fits()) return std::nullopt;
  return value.low;
}
} // namespace

namespace {
// A price unit: its name (`input`, `tool:web_search`), its service tier (`/flex`) and the input length above which
// it applies (`+200000`, 0 for every request).
struct Unit {
  std::string_view name, tier;
  std::uint64_t above = 0;
};
std::optional<Unit> parse_unit(std::string_view unit) {
  Unit out;
  if (const auto plus = unit.rfind('+'); plus != std::string_view::npos) {
    const auto digits = unit.substr(plus + 1);
    if (digits.empty() || digits.size() > 19) return std::nullopt;
    for (const char c : digits) {
      if (c < '0' || c > '9') return std::nullopt;
      out.above = out.above * 10 + static_cast<std::uint64_t>(c - '0');
    }
    unit = unit.substr(0, plus);
  }
  if (const auto slash = unit.rfind('/'); slash != std::string_view::npos) {
    out.tier = unit.substr(slash + 1);
    unit = unit.substr(0, slash);
  }
  if (unit.empty() || (unit.starts_with(descriptor::tool_fee_prefix) && unit.size() == descriptor::tool_fee_prefix.size())) return std::nullopt;
  out.name = unit;
  return out;
}
// The plain name of a unit OpenRouter names otherwise.
std::string_view canonical(std::string_view name) {
  if (name == "prompt") return "input";
  if (name == "completion") return "output";
  if (name == "input_cache_read") return "cache_read";
  if (name == "input_cache_write") return "cache_write";
  if (name == "internal_reasoning") return "reasoning";
  if (name == "audio") return "input_audio";
  if (name == "audio_output") return "output_audio";
  return name;
}
// The entry's prices for a request in `service_tier` with `input_tokens` input tokens. Exact: each unit's price from
// the highest band the request is above. Dearest: each unit's highest price over every band the request may reach.
ModelPrice select(const descriptor::CatalogueEntry& entry, std::string_view service_tier, std::uint64_t input_tokens,
                  bool dearest) {
  struct Pick { std::uint64_t above = 0; bool plain = false; std::optional<std::uint64_t> rate; };
  std::vector<std::pair<std::string, Pick>> picks;
  for (const auto& [text_unit, text] : entry.pricing) {
    const auto unit = parse_unit(text_unit);
    if (!unit || unit->tier != service_tier || (unit->above && input_tokens <= unit->above)) continue;
    const auto rate = per_million(text);
    if (!rate) continue;
    const auto name = canonical(unit->name);
    const bool plain = name == unit->name;
    auto found = std::find_if(picks.begin(), picks.end(), [&](const auto& p) { return p.first == name; });
    if (found == picks.end()) { picks.emplace_back(std::string(name), Pick{unit->above, plain, rate}); continue; }
    auto& pick = found->second;
    if (dearest) {
      if (*rate > *pick.rate) pick = Pick{unit->above, plain, rate};
    } else if (unit->above > pick.above || (unit->above == pick.above && plain && !pick.plain)) {
      pick = Pick{unit->above, plain, rate};
    }
  }
  ModelPrice out;
  for (const auto& [name, pick] : picks) {
    if (name == "input") out.input = pick.rate;
    else if (name == "output") out.output = pick.rate;
    else if (name == "cache_read") out.cache_read = pick.rate;
    else if (name == "cache_write") out.cache_write = pick.rate;
    else if (name == "reasoning") out.reasoning = pick.rate;
    else if (name == "request") out.request = pick.rate;
    else if (name == "cache_write_1h") out.cache_write_1h = pick.rate;
    else if (name == "output_image") out.output_image = pick.rate;
    else if (name == "input_audio") out.input_audio = pick.rate;
    else if (name == "output_audio") out.output_audio = pick.rate;
    else if (name == "ceiling_input") out.ceiling_input = pick.rate;
    else if (name == "ceiling_output") out.ceiling_output = pick.rate;
    else if (name == "ceiling_request") out.ceiling_request = pick.rate;
    else if (name.starts_with(descriptor::tool_fee_prefix)) out.tool_fees.emplace_back(name.substr(descriptor::tool_fee_prefix.size()), *pick.rate);
  }
  std::sort(out.tool_fees.begin(), out.tool_fees.end());
  return out;
}
}  // namespace

ModelPrice price_of(const descriptor::CatalogueEntry& entry, std::string_view service_tier, std::uint64_t input_tokens) {
  return select(entry, service_tier, input_tokens, false);
}
namespace {
// Whether the entry has prices in a service tier other than `service_tier`.
bool priced_in_other_tier(const descriptor::CatalogueEntry& entry, std::string_view service_tier, std::uint64_t input_tokens) {
  for (const auto& [text_unit, text] : entry.pricing) {
    const auto unit = parse_unit(text_unit);
    if (unit && unit->tier != service_tier && per_million(text) && !select(entry, unit->tier, input_tokens, false).empty()) return true;
  }
  return false;
}
}  // namespace
ModelPrice dearest_price_of(const descriptor::CatalogueEntry& entry, std::string_view service_tier,
                            std::uint64_t input_tokens) {
  return select(entry, service_tier, input_tokens, true);
}

namespace {
// The Usage::extra keys of a modality breakdown: Generate `<group>Details.<MODALITY>` and Interactions
// `<group>_by_modality.<modality>`.
constexpr std::array<std::string_view, 2> input_breakdown{"promptTokensDetails.", "input_tokens_by_modality."};
constexpr std::array<std::string_view, 2> output_breakdown{"candidatesTokensDetails.", "output_tokens_by_modality."};
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

CostEstimate estimate(const descriptor::ModelCatalogue& catalogue, std::string_view family, std::string_view origin,
                      const ServedModel& model, const Usage& usage, std::string_view service_tier, bool route_ceiling) {
  CostEstimate out;
  out.lower_bound = usage.stage == UsageStage::Partial || usage.quality == UsageQuality::Inconsistent;
  const descriptor::CatalogueEntry* entry = nullptr;
  ModelPrice price;
  auto count_of = [](const std::optional<Count>& c) -> std::uint64_t { return c ? c->value : 0; };
  // The input length that picks the entry's band: every input token, cache reads and writes included.
  const auto input_length = usage.input_total ? usage.input_total->value
      : count_of(usage.input_uncached) + count_of(usage.cache_read) + count_of(usage.cache_write);
  bool other_tier = false;
  auto priced = [&](std::string_view name) {
    if (name.empty()) return false;
    const auto match = catalogue.find(family, origin, name);
    if (!match.entry) return false;
    auto found = price_of(*match.entry, service_tier, input_length);
    if (found.empty()) {
      // The entry prices the model, but not in this service tier.
      other_tier = other_tier || priced_in_other_tier(*match.entry, service_tier, input_length);
      return false;
    }
    entry = match.entry; price = found;
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
  auto count = [](const std::optional<Count>& c) -> std::optional<std::uint64_t> { return c ? std::optional<std::uint64_t>(c->value) : std::nullopt; };
  const auto cache_read = count(usage.cache_read).value_or(0), cache_write = count(usage.cache_write).value_or(0);
  std::optional<std::uint64_t> uncached = count(usage.input_uncached);
  if (!uncached && usage.input_total) {
    const auto total = usage.input_total->value;
    uncached = total >= cache_read + cache_write ? total - cache_read - cache_write : 0;
  }
  Wide sum;  // nano-USD x 1e6
  bool complete = true;
  auto assume = [&](std::string_view rule) {
    if (std::find(out.assumptions.begin(), out.assumptions.end(), rule) == out.assumptions.end()) out.assumptions.emplace_back(rule);
  };
  // `base` is the rate of the unit this one refines. A zero rate under a priced base bills those tokens as free.
  auto charge = [&](std::uint64_t units, const std::optional<std::uint64_t>& rate, std::string_view name,
                    const std::optional<std::uint64_t>& base = std::nullopt) {
    if (!units) return;
    if (!rate) { out.unpriced.emplace_back(name); complete = false; return; }
    if (!*rate && base && *base) assume(std::string(name) + " at zero");
    sum += Wide::product(units, *rate);
  };
  auto extra = [&](std::string_view counter) -> std::uint64_t {
    const auto found = usage.extra.find(std::string(counter));
    return found == usage.extra.end() ? 0 : found->second.value;
  };
  // The audio tokens of the input or the output: a modality breakdown, else Chat's own audio counter.
  auto audio_of = [&](std::string_view chat_counter, std::span<const std::string_view> prefixes) {
    const auto breakdown = modality_tokens(usage, prefixes, "audio");
    return breakdown ? *breakdown : extra(chat_counter);
  };
  // Gemini bills the tool-use prompt beyond the prompt count; no price here covers it.
  if (extra("toolUsePromptTokenCount")) { out.unpriced.emplace_back("toolUsePromptTokenCount"); complete = false; }
  // A Messages compaction iteration is billed at the model's rates beside the top-level counters.
  const bool compaction = family == "anthropic.messages";
  if (route_ceiling) {
    // The served endpoint is unknown: every input token at the ceiling's input rate, every output token at its
    // output rate.
    if (!price.ceiling_input || !price.ceiling_output) { out.unpriced.emplace_back("ceiling"); return out; }
    assume("route ceiling");
    if (!uncached) { out.unpriced.emplace_back("input"); complete = false; }
    else charge(*uncached, price.ceiling_input, "ceiling");
    charge(cache_read, price.ceiling_input, "ceiling");
    charge(cache_write, price.ceiling_input, "ceiling");
    if (!usage.output_total) { out.unpriced.emplace_back("output"); complete = false; }
    else charge(usage.output_total->value, price.ceiling_output, "ceiling");
    if (compaction) {
      for (const auto counter : {"compaction.input_tokens", "compaction.cache_read_input_tokens", "compaction.cache_creation_input_tokens"})
        charge(extra(counter), price.ceiling_input, "ceiling");
      charge(extra("compaction.output_tokens"), price.ceiling_output, "ceiling");
    }
    const auto fee = std::max(price.request.value_or(0), price.ceiling_request.value_or(0));
    if (!complete) return out;
    sum += fee;
  } else {
    // Audio input bills at its own rate, out of the uncached input. Audio the cache counters hold has no split.
    std::uint64_t audio_in = 0;
    if (const auto audio = audio_of("input_audio", input_breakdown); audio && uncached) {
      if (price.input_audio && audio <= *uncached) audio_in = audio;
      else { out.unpriced.emplace_back("input_audio"); complete = false; }
    }
    if (!uncached) { out.unpriced.emplace_back("input"); complete = false; }
    else {
      charge(*uncached - audio_in, price.input, "input");
      charge(audio_in, price.input_audio, "input_audio", price.input);
    }
    auto fallback = [&](const std::optional<std::uint64_t>& specific, const std::optional<std::uint64_t>& general, std::string_view rule, std::uint64_t units) {
      if (specific || !units) return specific;
      if (general) assume(rule);
      return general;
    };
    charge(cache_read, fallback(price.cache_read, price.input, "cache_read at input", cache_read), "cache_read", price.input);
    // Messages splits cache writes into 5-minute and 1-hour writes; without the split, all of them bill at the
    // dearer rate.
    const auto write = fallback(price.cache_write, price.input, "cache_write at input", cache_write);
    const auto five = usage.extra.find("cache_creation.ephemeral_5m_input_tokens");
    const auto hour = usage.extra.find("cache_creation.ephemeral_1h_input_tokens");
    if (price.cache_write_1h && cache_write && five != usage.extra.end() && hour != usage.extra.end() &&
        five->second.value <= cache_write && hour->second.value == cache_write - five->second.value) {
      charge(five->second.value, write, "cache_write", price.input);
      charge(hour->second.value, price.cache_write_1h, "cache_write_1h", write);
    } else if (price.cache_write_1h && cache_write && (!write || *price.cache_write_1h > *write)) {
      assume("cache_write at cache_write_1h");
      charge(cache_write, price.cache_write_1h, "cache_write_1h", write);
    } else {
      if (hour != usage.extra.end() && hour->second.value && !price.cache_write_1h) assume("cache_write_1h at cache_write");
      charge(cache_write, write, "cache_write", price.input);
    }
    if (!usage.output_total) { out.unpriced.emplace_back("output"); complete = false; }
    else {
      const auto output = usage.output_total->value;
      const auto reasoning = price.reasoning ? std::min(count(usage.reasoning).value_or(0), output) : 0;
      charge(reasoning, price.reasoning, "reasoning", price.output);
      // Gemini reports image output tokens by modality (Generate `candidatesTokensDetails.IMAGE`, Interactions
      // `output_tokens_by_modality.image`). Without that breakdown, all output bills at the dearer rate.
      auto image = modality_tokens(usage, output_breakdown, "image");
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
      charge(images, price.output_image, "output_image", price.output);
      // Audio output bills at its own rate, out of what is left.
      std::uint64_t audio_out = 0;
      if (const auto audio = audio_of("audio_tokens", output_breakdown)) {
        if (price.output_audio) audio_out = std::min(audio, output - reasoning - images);
        else { out.unpriced.emplace_back("output_audio"); complete = false; }
      }
      charge(audio_out, price.output_audio, "output_audio", price.output);
      charge(output - reasoning - images - audio_out, price.output, "output");
    }
    if (compaction) {
      const auto read = extra("compaction.cache_read_input_tokens"), written = extra("compaction.cache_creation_input_tokens");
      charge(extra("compaction.input_tokens"), price.input, "input");
      charge(read, fallback(price.cache_read, price.input, "cache_read at input", read), "cache_read", price.input);
      charge(written, fallback(price.cache_write, price.input, "cache_write at input", written), "cache_write", price.input);
      charge(extra("compaction.output_tokens"), price.output, "output");
    }
    if (price.request) sum += *price.request;
  }
  if (!complete) return out;
  sum += 999'999;
  const auto nano = sum.divided(1'000'000);
  if (!nano.fits()) { out.unpriced.emplace_back("overflow"); return out; }
  out.total = UsdAmount{nano.low, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
  return out;
}

Bound upper_bound(const ModelPrice& price, const BoundRequest& request) {
  Bound out;
  auto dearest = [](std::initializer_list<std::optional<std::uint64_t>> rates) {
    std::optional<std::uint64_t> result;
    for (const auto& rate : rates)
      if (rate && (!result || *rate > *result)) result = rate;
    return result;
  };
  std::optional<std::uint64_t> input, output, fee;
  if (request.route_ceiling) {
    input = price.ceiling_input;
    output = price.ceiling_output;
    fee = dearest({price.request, price.ceiling_request});
    if (!input || !output) out.unpriced.emplace_back("ceiling");
  } else {
    input = price.input ? dearest({price.input, price.cache_read, price.cache_write, price.cache_write_1h, price.input_audio}) : std::nullopt;
    output = price.output ? dearest({price.output, price.reasoning, price.output_image, price.output_audio}) : std::nullopt;
    fee = price.request;
    if (!input && request.input_tokens) out.unpriced.emplace_back("input");
    if (!output && request.output_tokens) out.unpriced.emplace_back("output");
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

Bound upper_bound(const descriptor::CatalogueEntry& entry, std::string_view service_tier, const BoundRequest& request) {
  return upper_bound(dearest_price_of(entry, service_tier, request.input_tokens), request);
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
  }
  return {};
}

namespace {
constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a != 0 && b > kMax / a) return false;
  out = a * b;
  return true;
}
bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (b > kMax - a) return false;
  out = a + b;
  return true;
}
// The smallest of the figures that state a limit; a zero is no limit.
std::optional<std::uint64_t> smallest_limit(std::initializer_list<std::optional<std::uint64_t>> values) {
  std::optional<std::uint64_t> result;
  for (const auto& value : values)
    if (value && *value > 0 && (!result || *value < *result)) result = value;
  return result;
}
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
    Candidate candidate{model, entry, *first, invocations > 1 ? *limit : *first, {}};
    std::uint64_t rest = 0;
    if (invocations > 1 && (!checked_mul(invocations - 1, *limit, rest) || !checked_add(candidate.input, rest, candidate.input))) {
      block(UnboundedBy::Overflow, model, "the input tokens do not fit 64 bits");
      continue;
    }
    candidate.price = dearest_price_of(*entry, in.service_tier, candidate.invocation_input);
    if (candidate.price.empty()) {
      block(UnboundedBy::TierPrice, model,
            in.service_tier.empty() ? "the entry prices no unit at the standard rates"
                                    : "the entry prices no unit under service tier '" + std::string(in.service_tier) + "'");
      continue;
    }
    const bool has_ceiling = candidate.price.ceiling_input || candidate.price.ceiling_output || candidate.price.ceiling_request;
    if (!in.routed_gateway && has_ceiling) {
      // The bound holds only if the router cannot pick a dearer endpoint, and only a routing gateway takes the pin.
      block(UnboundedBy::RouteCeiling, model, "the entry lists a route ceiling but the origin is not a routing gateway, so it cannot be pinned");
      continue;
    }
    if (in.routed_gateway && (!candidate.price.ceiling_input || !candidate.price.ceiling_output)) {
      block(UnboundedBy::RouteCeiling, model, "the entry lists no route ceiling (ceiling_input, ceiling_output), so the router may pick any price");
      continue;
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
          block(UnboundedBy::ToolFee, type, "catalogue entry '" + candidate.entry->id + "' lists no fee (unit " + descriptor::tool_fee_unit(type) + ")");
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
    const auto bound = upper_bound(candidate.price, {candidate.input, output, in.routed_gateway, uses});
    if (!bound.total) {
      for (const auto& name : bound.unpriced) {
        if (name == "overflow") block(UnboundedBy::Overflow, candidate.model, "the bound does not fit 64 bits");
        else if (name == "ceiling") block(UnboundedBy::RouteCeiling, candidate.model, "no route ceiling");
        else if (name.starts_with(descriptor::tool_fee_prefix)) block(UnboundedBy::ToolFee, name.substr(descriptor::tool_fee_prefix.size()), "no fee");
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
