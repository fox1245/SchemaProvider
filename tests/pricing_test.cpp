#include "core/pricing.h"
#include "descriptor/catalogue.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace sp;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)
descriptor::CatalogueSnapshot catalogue(std::string_view json) {
  auto loaded = descriptor::load_catalogue(json);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("catalogue refused at " + error->pointer + ": " + error->expected);
  return std::get<descriptor::CatalogueSnapshot>(std::move(loaded));
}
Usage usage(std::optional<std::uint64_t> input, std::optional<std::uint64_t> output, std::uint64_t cache_read = 0, std::uint64_t cache_write = 0,
            std::uint64_t reasoning = 0) {
  Usage u;
  if (input) u.input_total = Count{*input, Evidence::Reported};
  if (output) u.output_total = Count{*output, Evidence::Reported};
  if (cache_read) u.cache_read = Count{cache_read, Evidence::Reported};
  if (cache_write) u.cache_write = Count{cache_write, Evidence::Reported};
  if (reasoning) u.reasoning = Count{reasoning, Evidence::Reported};
  return u;
}

// Listed decimal USD per unit becomes exact nano-USD per million units; OpenRouter's unit names and the plain ones
// are the same units.
void prices_from_entries() {
  const auto c = catalogue(R"({"version":1,"models":[
    {"id":"vendor/listed","pricing":{"prompt":"0.0000001","completion":"0.0000004","input_cache_read":"0.000000025","internal_reasoning":"0.0000008","request":"0.002","image":"0.04"}},
    {"id":"plain","pricing":{"input":"0.000003","prompt":"9","output":"0.000015","cache_write":"0.00000375"}},
    {"id":"router/auto","pricing":{"prompt":"-1","completion":"-1"}},
    {"id":"fine","pricing":{"input":"0.0000000000000011","output":"1"}}]})");
  const auto listed = pricing::price_of(*c->entry("vendor/listed"));
  CHECK(listed.input == 100'000'000u && listed.output == 400'000'000u && listed.cache_read == 25'000'000u && !listed.cache_write &&
        listed.reasoning == 800'000'000u && listed.request == 2'000'000'000'000u);
  const auto plain = pricing::price_of(*c->entry("plain"));
  CHECK(plain.input == 3'000'000'000u && plain.output == 15'000'000'000u && plain.cache_write == 3'750'000'000u);
  CHECK(pricing::price_of(*c->entry("router/auto")).empty());
  // Past the fifteenth fractional digit the price rounds up.
  const auto fine = pricing::price_of(*c->entry("fine"));
  CHECK(fine.input == 2u && fine.output == 1'000'000'000'000'000u);
}

void estimates() {
  const auto c = catalogue(R"({"version":1,"models":[
    {"id":"vendor/served","pricing":{"prompt":"0.000001","completion":"0.000002","input_cache_read":"0.0000001","internal_reasoning":"0.000004","request":"0.0005"}},
    {"id":"vendor/requested","served_as":["vendor/elsewhere"],"pricing":{"input":"0.000002","output":"0.000003"}},
    {"id":"vendor/free-text","pricing":{"input":"0"}},
    {"id":"vendor/","match":"prefix","pricing":{"input":"0.000005","output":"0.000005"}}]})");
  ServedModel exact{"vendor/served", "vendor/served", ServedRelation::Exact};
  // 1000 uncached input, 200 cache reads, 300 output of which 100 reasoning, and the request fee, in nano-USD:
  // 1'000'000 + 20'000 + 400'000 (200 output) + 400'000 (100 reasoning) + 500'000.
  auto e = pricing::estimate(*c, "openai.chat", "https://openrouter.ai", exact, usage(1200, 300, 200, 0, 100));
  CHECK(e.total && e.total->nano_usd == 2'320'000u && e.total->evidence == Evidence::Derived &&
        e.total->rounding == CostRounding::CeilingCatalogueProduct && e.entry == "vendor/served" && !e.requested_entry && e.assumptions.empty());
  // A served model with no priced entry falls back to the requested one, flagged.
  ServedModel routed{"vendor/requested", "vendor/unpriced", ServedRelation::Routed};
  // The prefix profile prices vendor/unpriced, so the served model's entry is used.
  e = pricing::estimate(*c, "openai.chat", "", routed, usage(10, 10));
  CHECK(e.entry == "vendor/" && !e.requested_entry && e.total->nano_usd == 100'000u);
  ServedModel elsewhere{"other/requested", "other/served", ServedRelation::Routed};
  e = pricing::estimate(*c, "openai.chat", "", elsewhere, usage(10, 10));
  CHECK(!e.total && e.unpriced == std::vector<std::string>{"model"});
  ServedModel by_request{"vendor/requested", "nobody/served", ServedRelation::Unexplained};
  e = pricing::estimate(*c, "openai.chat", "", by_request, usage(1000, 1000, 100, 50));
  // Cache counters without their own prices bill at the input price, and the rules are named.
  CHECK(e.requested_entry && e.entry == "vendor/requested" && e.total->nano_usd == 2'000'000u + 3'000'000u &&
        e.assumptions == (std::vector<std::string>{"cache_read at input", "cache_write at input"}));
  // input_uncached wins over the derived figure.
  auto direct = usage(1000, 0); direct.input_uncached = Count{400, Evidence::Reported};
  e = pricing::estimate(*c, "openai.chat", "", ServedModel{"vendor/requested", "", ServedRelation::NotReported}, direct);
  CHECK(e.total->nano_usd == 800'000u);
  // A billed counter without a price, or a counter the usage does not give, leaves no total.
  e = pricing::estimate(*c, "openai.chat", "", ServedModel{"vendor/free-text", "vendor/free-text", ServedRelation::Exact}, usage(100, 5));
  CHECK(!e.total && e.unpriced == std::vector<std::string>{"output"});
  e = pricing::estimate(*c, "openai.chat", "", ServedModel{"vendor/free-text", "vendor/free-text", ServedRelation::Exact}, usage(100, 0));
  CHECK(e.total && e.total->nano_usd == 0u);
  e = pricing::estimate(*c, "openai.chat", "", exact, usage(std::nullopt, 5));
  CHECK(!e.total && e.unpriced == std::vector<std::string>{"input"});
  // A Messages compaction iteration bills at the model's rates beside the top-level counters (#53):
  // (251 + 112197) input at 2000 and (40 + 319) output at 3000 nano-USD per token. Other families keep no such count.
  auto compacted = usage(251, 40);
  compacted.extra["compaction.input_tokens"] = Count{112'197, Evidence::Derived};
  compacted.extra["compaction.output_tokens"] = Count{319, Evidence::Derived};
  const ServedModel requested{"vendor/requested", "", ServedRelation::NotReported};
  e = pricing::estimate(*c, "anthropic.messages", "", requested, compacted);
  CHECK(e.total && e.total->nano_usd == 224'896'000u + 1'077'000u);
  e = pricing::estimate(*c, "openai.chat", "", requested, compacted);
  CHECK(e.total && e.total->nano_usd == 502'000u + 120'000u);
  // Rounding happens once, upward: 3 tokens at 0.0000000005 USD are 1.5 nano-USD, billed as 2.
  const auto half = catalogue(R"({"version":1,"models":[{"id":"m","pricing":{"input":"0.0000000005","output":"0"}}]})");
  e = pricing::estimate(*half, "openai.chat", "", ServedModel{"m", "m", ServedRelation::Exact}, usage(3, 1));
  CHECK(e.total->nano_usd == 2u);
}

// Products above 2^64 stay exact, and a total above 2^64 nano-USD is reported, not wrapped.
void wide_products() {
  const auto c = catalogue(R"({"version":1,"models":[{"id":"m","pricing":{"input":"1000","output":"0.000000000000001"}},
    {"id":"huge","pricing":{"input":"18446744073709551615","output":"0"}}]})");
  const ServedModel m{"m", "m", ServedRelation::Exact};
  // 1e6 tokens at 1000 USD are 1e24 in nano-USD x 1e6, above 2^64; the total is 1e18 nano-USD. One output token at
  // 1e-15 USD is 1e-6 nano-USD and rounds the total up once.
  auto e = pricing::estimate(*c, "openai.chat", "", m, usage(1'000'000, 1));
  CHECK(e.total && e.total->nano_usd == 1'000'000'000'000'000'001u);
  // 2e7 tokens at 1000 USD are 2e19 nano-USD, above 2^64.
  e = pricing::estimate(*c, "openai.chat", "", m, usage(20'000'000, 0));
  CHECK(!e.total && e.unpriced.size() == 1 && e.unpriced.front() == "overflow");
  // A price whose nano-USD per million units does not fit in 64 bits is not a price.
  CHECK(!pricing::price_of(*c->entry("huge")).input);
}
// Service tiers, the dearer rates, route ceilings and tool fees, and the reservation bound.
void bounds() {
  const auto c = catalogue(R"({"version":1,"models":[
    {"id":"direct","pricing":{"input":"0.000002","output":"0.000008","cache_read":"0.0000005","cache_write":"0.0000025",
      "cache_write_1h":"0.000004","output_image":"0.00003","request":"0.001","input:flex":"0.000001","output:flex":"0.000004",
      "tool:web_search_20250305":"0.01","tool:free_tool":"0"}},
    {"id":"vendor/routed","pricing":{"prompt":"0.0000001","completion":"0.0000004","ceiling_input":"0.0000024","ceiling_output":"0.0000096"}}]})");
  const auto direct = pricing::price_of(*c->entry("direct"));
  CHECK(direct.cache_write_1h == 4'000'000'000u && direct.output_image == 30'000'000'000u && direct.tool_fees.size() == 2 &&
        direct.tool_fees[1].first == "web_search_20250305" && direct.tool_fees[1].second == 10'000'000'000'000u && direct.tool_fees[0].second == 0u);
  // A tier's own units are its prices; the base units are not.
  const auto flex = pricing::price_of(*c->entry("direct"), "flex");
  CHECK(flex.input == 1'000'000'000u && flex.output == 4'000'000'000u && !flex.cache_read && !flex.request && flex.tool_fees.empty());
  CHECK(pricing::price_of(*c->entry("direct"), "priority").empty());
  // 1000 input tokens at the 1-hour write rate (4e-6), 100 output tokens at the image rate (3e-5), the request fee
  // (1e-3) and two web searches (0.01 each): 4'000'000 + 3'000'000 + 1'000'000 + 20'000'000 nano-USD.
  auto b = pricing::upper_bound(direct, {1000, 100, false, {{"web_search_20250305", 2}, {"free_tool", 5}}});
  CHECK(b.total && b.total->nano_usd == 28'000'000u && b.total->rounding == CostRounding::CeilingCatalogueProduct && b.unpriced.empty());
  b = pricing::upper_bound(direct, {1000, 100, false, {{"unknown_tool", 1}}});
  CHECK(!b.total && b.unpriced.size() == 1 && b.unpriced.front() == "tool:unknown_tool");
  b = pricing::upper_bound(flex, {10, 10, false, {}});
  CHECK(b.total && b.total->nano_usd == 50'000u);
  b = pricing::upper_bound(pricing::price_of(*c->entry("direct"), "priority"), {10, 10, false, {}});
  CHECK(!b.total && b.unpriced == std::vector<std::string>({"input", "output"}));
  // A routing gateway is bounded by its ceiling, never by its listed price.
  const auto routed = pricing::price_of(*c->entry("vendor/routed"));
  b = pricing::upper_bound(routed, {1000, 1000, true, {}});
  CHECK(b.total && b.total->nano_usd == 12'000'000u);
  CHECK(!pricing::upper_bound(direct, {1, 1, true, {}}).total);
  CHECK(pricing::upper_bound(direct, {1, 1, true, {}}).unpriced.front() == "ceiling");
  // The estimate of any usage within the bounds stays at or below the bound.
  std::uint64_t seed = 7;
  auto next = [&](std::uint64_t limit) { seed = seed * 6364136223846793005u + 1442695040888963407u; return (seed >> 33) % (limit + 1); };
  const ServedModel exact{"direct", "direct", ServedRelation::Exact};
  for (int i = 0; i < 2000; ++i) {
    const std::uint64_t input_limit = next(200'000), output_cap = next(50'000);
    const auto bound = pricing::upper_bound(direct, {input_limit, output_cap, false, {}});
    const auto output = next(output_cap), reasoning = next(output), read = next(input_limit);
    const auto write = next(input_limit - read), total = read + write + next(input_limit - read - write);
    const auto e = pricing::estimate(*c, "openai.chat", "", exact, usage(total, output, read, write, reasoning));
    CHECK(bound.total && e.total && e.total->nano_usd <= bound.total->nano_usd);
  }
}
} // namespace

int main() {
  try { prices_from_entries(); estimates(); wide_products(); bounds(); std::cout << "pricing properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "pricing test failed: " << error.what() << '\n'; return 1; }
}
