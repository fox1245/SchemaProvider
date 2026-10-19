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
  // Rounding happens once, upward: 3 tokens at 0.0000000005 USD are 1.5 nano-USD, billed as 2.
  const auto half = catalogue(R"({"version":1,"models":[{"id":"m","pricing":{"input":"0.0000000005","output":"0"}}]})");
  e = pricing::estimate(*half, "openai.chat", "", ServedModel{"m", "m", ServedRelation::Exact}, usage(3, 1));
  CHECK(e.total->nano_usd == 2u);
}
} // namespace

int main() {
  try { prices_from_entries(); estimates(); std::cout << "pricing properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "pricing test failed: " << error.what() << '\n'; return 1; }
}
