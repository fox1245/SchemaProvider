#include "qualification/catalog.h"
#include "json/json.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sp::qualification {
namespace {
struct Failure { Error error; };
[[noreturn]] void fail(Denial code = Denial::Configuration) {
  throw Failure{{code, code == Denial::Overflow ? "price arithmetic overflow" :
      code == Denial::Bounds ? "token bounds or usage inconsistent" : "invalid model catalog"}};
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
  if (b > UINT64_MAX - a) fail(Denial::Overflow);
  return a + b;
}
std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
  if (b && a > UINT64_MAX / b) fail(Denial::Overflow);
  return a * b;
}
std::uint64_t number(json::Value value) {
  if (!value.is_uint()) fail();
  return value.as_uint();
}
std::optional<std::uint64_t> nullable(json::Value value) {
  if (!value.valid()) fail();
  return value.is_null() ? std::nullopt : std::optional(number(value));
}
std::string text(json::Value value) {
  if (!value.is_string() || value.as_string().empty()) fail();
  return std::string(value.as_string());
}
void keys(json::Value object, std::initializer_list<std::string_view> names) {
  if (!object.is_object() || object.size() != names.size()) fail();
  for (auto [name, value] : object.members()) {
    (void)value;
    if (std::find(names.begin(), names.end(), name) == names.end()) fail();
  }
}
std::uint64_t numerator(json::Value value) {
  if (!value.is_number()) fail();
  const double twice = value.as_double() * 2;
  // Modifiers are restricted to positive exact half-integers, not executable policy.
  if (!std::isfinite(twice) || twice < 2 || twice > 100 || std::floor(twice) != twice) fail();
  return static_cast<std::uint64_t>(twice);
}
std::uint64_t charge(std::uint64_t tokens, std::uint64_t rate, std::uint64_t factor) {
  const auto adjusted = multiply(rate, factor);
  constexpr std::uint64_t denominator = 2000000;
  // Decomposition permits huge token counts at small rates without avoidable overflow.
  auto whole = multiply(tokens / denominator, adjusted);
  auto remainder = multiply(tokens % denominator, adjusted);
  return add(whole, remainder / denominator + (remainder % denominator != 0));
}
void bounds(const ModelPrice& m, std::uint64_t in, std::uint64_t out) {
  if (in > m.max_input || out > m.max_output || add(in, out) > m.context_window)
    fail(Denial::Bounds);
}
}  // namespace
Result<std::shared_ptr<const Catalog>> Catalog::from_json(std::string_view raw) {
  try {
    auto parsed = json::parse(raw);
    auto* doc = std::get_if<json::Document>(&parsed);
    if (!doc) fail();
    const auto root = doc->root();
    keys(root, {"version", "currency", "rate_unit", "verified_at", "models"});
    if (number(root.get("version")) != 1 || text(root.get("currency")) != "USD" ||
        text(root.get("rate_unit")) != "micro_usd_per_million_tokens") fail();
    (void)text(root.get("verified_at"));
    auto models = root.get("models");
    if (!models.is_object() || models.size() == 0 || models.size() > 64) fail();
    auto catalog = std::shared_ptr<Catalog>(new Catalog);
    catalog->models_.reserve(models.size());
    for (auto [name, object] : models.members()) {
      keys(object, {"families", "context_window", "max_input_tokens", "max_output_tokens",
          "input_rate", "cache_read_rate", "cache_write_5m_rate", "cache_write_1h_rate",
          "output_rate", "long_context_threshold", "long_context_input_multiplier",
          "long_context_output_multiplier", "cache_minimum_tokens", "cache_activation",
          "cache_ttl_seconds", "output_includes_thinking", "model_url", "pricing_url", "caching_url"});
      if (name.empty() || name.size() > 128 || name.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._") != std::string_view::npos) fail();
      ModelPrice m;
      m.model = name;
      auto families = object.get("families");
      if (!families.is_array() || !families.size() || families.size() > 5) fail();
      for (auto family : families.elements()) {
        auto f = text(family);
        if (f != "openai.chat" && f != "openai.responses" && f != "anthropic.messages" &&
            f != "google.generate" && f != "google.interactions") fail();
        if (std::find(m.families.begin(), m.families.end(), f) != m.families.end()) fail();
        m.families.push_back(std::move(f));
      }
      m.context_window = number(object.get("context_window"));
      m.max_input = number(object.get("max_input_tokens"));
      m.max_output = number(object.get("max_output_tokens"));
      if (!m.context_window || !m.max_input || !m.max_output ||
          m.max_input > m.context_window || m.max_output > m.context_window) fail();
      m.input_rate = number(object.get("input_rate"));
      m.cache_read_rate = number(object.get("cache_read_rate"));
      m.cache_write_5m_rate = nullable(object.get("cache_write_5m_rate"));
      m.cache_write_1h_rate = nullable(object.get("cache_write_1h_rate"));
      m.output_rate = number(object.get("output_rate"));
      m.long_context_threshold = nullable(object.get("long_context_threshold"));
      if (m.long_context_threshold && (!*m.long_context_threshold || *m.long_context_threshold >= m.context_window)) fail();
      m.long_input_numerator = numerator(object.get("long_context_input_multiplier"));
      m.long_output_numerator = numerator(object.get("long_context_output_multiplier"));
      if (!m.long_context_threshold && (m.long_input_numerator != 2 || m.long_output_numerator != 2)) fail();
      (void)nullable(object.get("cache_minimum_tokens"));
      (void)nullable(object.get("cache_ttl_seconds"));
      auto activation = text(object.get("cache_activation"));
      if (activation != "implicit_default" && activation != "cache_control_required") fail();
      auto thinking = object.get("output_includes_thinking");
      if (!thinking.is_bool() || !thinking.as_bool()) fail();
      m.model_url = text(object.get("model_url"));
      m.pricing_url = text(object.get("pricing_url"));
      m.caching_url = text(object.get("caching_url"));
      for (const auto* url : {&m.model_url, &m.pricing_url, &m.caching_url})
        if (!url->starts_with("https://")) fail();
      // Reject unusable rates during loading, not at dispatch time.
      for (auto rate : {m.input_rate, m.cache_read_rate, m.cache_write_5m_rate.value_or(0),
                        m.cache_write_1h_rate.value_or(0), m.output_rate})
        (void)multiply(rate, std::max(m.long_input_numerator, m.long_output_numerator));
      catalog->models_.push_back(std::move(m));
    }
    return std::shared_ptr<const Catalog>(std::move(catalog));
  } catch (const Failure& f) { return f.error; }
}
const ModelPrice* Catalog::find(std::string_view model, std::string_view family) const noexcept {
  for (const auto& m : models_)
    if (m.model == model && std::find(m.families.begin(), m.families.end(), family) != m.families.end()) return &m;
  return nullptr;
}
Result<std::uint64_t> Catalog::reserve_cost(const ModelPrice& m, std::uint64_t in, std::uint64_t out) const {
  try {
    bounds(m, in, out);
    const bool long_context = m.long_context_threshold && in > *m.long_context_threshold;
    auto input_rate = std::max({m.input_rate, m.cache_read_rate,
        m.cache_write_5m_rate.value_or(0), m.cache_write_1h_rate.value_or(0)});
    auto input_cost = charge(in, input_rate, long_context ? m.long_input_numerator : 2);
    // Final disjoint bands round independently. Protect the maximum three
    // extra microUSD, without reserving a fictitious whole context window.
    if (in) input_cost = add(input_cost, std::min<std::uint64_t>(in, 4) - 1);
    return add(input_cost, charge(out, m.output_rate, long_context ? m.long_output_numerator : 2));
  } catch (const Failure& f) { return f.error; }
}
Result<PricedUsage> Catalog::usage_cost(const ModelPrice& m, const Usage& u,
                                        std::uint64_t in_bound, std::uint64_t out_bound) const {
  try {
    bounds(m, in_bound, out_bound);
    if (!u.output || *u.output > out_bound || (u.thinking && *u.thinking > *u.output))
      fail(Denial::Bounds);
    if ((u.cache_write_5m_input.value_or(0) && !m.cache_write_5m_rate) ||
        (u.cache_write_1h_input.value_or(0) && !m.cache_write_1h_rate)) fail(Denial::Bounds);
    const bool complete = u.uncached_input && u.cache_read_input &&
        u.cache_write_5m_input && u.cache_write_1h_input;
    const auto partial = add(add(u.uncached_input.value_or(0), u.cache_read_input.value_or(0)),
        add(u.cache_write_5m_input.value_or(0), u.cache_write_1h_input.value_or(0)));
    if (!complete && !u.total_input) fail(Denial::Bounds);
    const auto in = complete ? partial : *u.total_input;
    if (in > in_bound || (u.total_input && (*u.total_input > in_bound ||
        (complete ? partial != *u.total_input : partial > *u.total_input)))) fail(Denial::Bounds);
    const bool long_context = m.long_context_threshold && in > *m.long_context_threshold;
    auto factor = long_context ? m.long_input_numerator : 2;
    const auto output_cost = charge(*u.output, m.output_rate, long_context ? m.long_output_numerator : 2);
    if (!complete) {
      const auto rate = std::max({m.input_rate, m.cache_read_rate,
          m.cache_write_5m_rate.value_or(0), m.cache_write_1h_rate.value_or(0)});
      auto input_cost = charge(in, rate, factor);
      if (in) input_cost = add(input_cost, std::min<std::uint64_t>(in, 4) - 1);
      return PricedUsage{add(input_cost, output_cost), PriceKind::UpperBound};
    }
    auto cost = charge(*u.uncached_input, m.input_rate, factor);
    cost = add(cost, charge(*u.cache_read_input, m.cache_read_rate, factor));
    cost = add(cost, charge(*u.cache_write_5m_input, m.cache_write_5m_rate.value_or(0), factor));
    cost = add(cost, charge(*u.cache_write_1h_input, m.cache_write_1h_rate.value_or(0), factor));
    return PricedUsage{add(cost, output_cost), PriceKind::Exact};
  } catch (const Failure& f) { return f.error; }
}
}  // namespace sp::qualification
