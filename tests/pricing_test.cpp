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
using ModelPricePairs = std::vector<std::pair<std::string, std::uint64_t>>;
// Service tiers, the dearer rates, route ceilings and tool fees, and the reservation bound.
void bounds() {
  const auto c = catalogue(R"({"version":1,"models":[
    {"id":"direct","pricing":{"input":"0.000002","output":"0.000008","cache_read":"0.0000005","cache_write":"0.0000025",
      "cache_write_1h":"0.000004","output_image":"0.00003","request":"0.001","input/flex":"0.000001","output/flex":"0.000004",
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
  // A tool type may itself hold a colon; a tier and a band follow it.
  const auto typed = catalogue(R"({"version":1,"models":[{"id":"m","pricing":{"input":"0","output":"0",
    "tool:openrouter:web_search":"0.004","tool:openrouter:web_search/flex":"0.002","input/flex+10":"0.1"}}]})");
  CHECK(pricing::price_of(*typed->entry("m")).tool_fees == ModelPricePairs({{"openrouter:web_search", 4'000'000'000'000u}}));
  const auto typed_flex = pricing::price_of(*typed->entry("m"), "flex", 11);
  CHECK(typed_flex.tool_fees == ModelPricePairs({{"openrouter:web_search", 2'000'000'000'000u}}) &&
        typed_flex.input == 100'000'000'000'000u && !typed_flex.output);
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
// Input bands: above a band's input length, its units price the whole call.
void bands() {
  const auto c = catalogue(R"({"version":1,"models":[{"id":"long","pricing":{"input":"0.000001","output":"0.000004",
    "cache_read":"0.0000001","input+200000":"0.000002","output+200000":"0.000006","cache_read+200000":"0.0000002",
    "prompt+100000":"0.000009"}}]})");
  const auto& entry = *c->entry("long");
  // A plain name wins over OpenRouter's in the same band, but a higher band wins over both.
  CHECK(pricing::price_of(entry, {}, 150'000).input == 9'000'000'000u);
  const auto base = pricing::price_of(entry, {}, 200'000), band = pricing::price_of(entry, {}, 200'001);
  CHECK(base.input == 9'000'000'000u && base.output == 4'000'000'000u && base.cache_read == 100'000'000u);
  CHECK(band.input == 2'000'000'000u && band.output == 6'000'000'000u && band.cache_read == 200'000'000u);
  CHECK(pricing::price_of(entry).input == 1'000'000'000u);
  // The input length counts cache reads: 150'000 uncached and 60'000 read are above the 200'000 band.
  const ServedModel exact{"long", "long", ServedRelation::Exact};
  auto e = pricing::estimate(*c, "openai.chat", "", exact, usage(210'000, 1'000, 60'000));
  // 150'000 x 2e-6 + 60'000 x 2e-7 + 1'000 x 6e-6 USD = 0.3 + 0.012 + 0.006.
  CHECK(e.total && e.total->nano_usd == 318'000'000u);
  e = pricing::estimate(*c, "openai.chat", "", exact, usage(1'000, 1'000));
  CHECK(e.total && e.total->nano_usd == 5'000'000u);
  // The dearest price over every band a request may reach.
  const auto dear = pricing::dearest_price_of(entry, {}, 250'000);
  CHECK(dear.input == 9'000'000'000u && dear.output == 6'000'000'000u && dear.cache_read == 200'000'000u);
  CHECK(pricing::dearest_price_of(entry, {}, 1'000).input == 1'000'000'000u);
  std::uint64_t seed = 11;
  auto next = [&](std::uint64_t limit) { seed = seed * 6364136223846793005u + 1442695040888963407u; return (seed >> 33) % (limit + 1); };
  for (int i = 0; i < 2000; ++i) {
    const std::uint64_t input_limit = next(400'000), output_cap = next(50'000);
    const auto bound = pricing::upper_bound(pricing::dearest_price_of(entry, {}, input_limit), {input_limit, output_cap, false, {}});
    const auto read = next(input_limit), total = read + next(input_limit - read);
    e = pricing::estimate(*c, "openai.chat", "", exact, usage(total, next(output_cap), read));
    CHECK(bound.total && e.total && e.total->nano_usd <= bound.total->nano_usd);
  }
  // A malformed band is no unit.
  const auto odd = catalogue(R"({"version":1,"models":[{"id":"m","pricing":{"input":"0.000001","input+":"1","input+x":"1",
    "input+200000/flex":"1"}}]})");
  CHECK(pricing::price_of(*odd->entry("m"), {}, 300'000).input == 1'000'000'000u);
}
// The dearer rates a call bills: 1-hour cache writes, image output tokens and a route ceiling.
void dearer_rates() {
  const auto c = catalogue(R"({"version":1,"models":[
    {"id":"direct","pricing":{"input":"0.000002","output":"0.000008","cache_write":"0.0000025","cache_write_1h":"0.000004",
      "output_image":"0.00003","request":"0.001"}},
    {"id":"plain","pricing":{"input":"0.000002","output":"0.000008","cache_write":"0.0000025"}},
    {"id":"vendor/routed","pricing":{"prompt":"0.0000001","completion":"0.0000004","ceiling_input":"0.0000024","ceiling_output":"0.0000096"}},
    {"id":"vendor/unbounded","pricing":{"prompt":"0.0000001","completion":"0.0000004"}}]})");
  const ServedModel direct{"direct", "direct", ServedRelation::Exact};
  // 700 uncached at 2e-6, 100 five-minute writes at 2.5e-6, 200 one-hour writes at 4e-6, 10 text output tokens at
  // 8e-6 and the request fee: 1'400'000 + 250'000 + 800'000 + 80'000 + 1'000'000 nano-USD.
  auto u = usage(1'000, 10, 0, 300);
  u.extra["cache_creation.ephemeral_5m_input_tokens"] = Count{100, Evidence::Reported};
  u.extra["cache_creation.ephemeral_1h_input_tokens"] = Count{200, Evidence::Reported};
  u.extra["candidatesTokensDetails.TEXT"] = Count{10, Evidence::Reported};
  auto e = pricing::estimate(*c, "anthropic.messages", "", direct, u);
  CHECK(e.total && e.total->nano_usd == 3'530'000u && e.assumptions.empty());
  // Without the split, every write bills at the 1-hour rate; without a modality breakdown, every output token at the
  // image rate.
  e = pricing::estimate(*c, "anthropic.messages", "", direct, usage(1'000, 10, 0, 300));
  CHECK(e.total && e.total->nano_usd == 3'530'000u + 150'000u + 220'000u);
  CHECK(e.assumptions == std::vector<std::string>({"cache_write at cache_write_1h", "output at output_image"}));
  // Four image tokens of ten, by either Gemini wire.
  for (const char* key : {"candidatesTokensDetails.IMAGE", "output_tokens_by_modality.image"}) {
    auto images = usage(1'000, 10);
    images.extra[key] = Count{4, Evidence::Reported};
    e = pricing::estimate(*c, "google.generate", "", direct, images);
    CHECK(e.total && e.total->nano_usd == 2'000'000u + 120'000u + 48'000u + 1'000'000u && e.assumptions.empty());
  }
  // One-hour writes on an entry with no 1-hour price bill at the cache-write rate, and say so.
  auto hour = usage(1'000, 0, 0, 100);
  hour.extra["cache_creation.ephemeral_5m_input_tokens"] = Count{0, Evidence::Reported};
  hour.extra["cache_creation.ephemeral_1h_input_tokens"] = Count{100, Evidence::Reported};
  e = pricing::estimate(*c, "anthropic.messages", "", ServedModel{"plain", "plain", ServedRelation::Exact}, hour);
  CHECK(e.total && e.total->nano_usd == 1'800'000u + 250'000u && e.assumptions == std::vector<std::string>({"cache_write_1h at cache_write"}));
  // A routing gateway's estimate is at its route ceiling: 1000 input at 2.4e-6 and 1000 output at 9.6e-6.
  const ServedModel routed{"vendor/routed", "vendor/routed", ServedRelation::Exact};
  e = pricing::estimate(*c, "openai.chat", "", routed, usage(1'000, 1'000, 200), {}, true);
  CHECK(e.total && e.total->nano_usd == 12'000'000u && e.assumptions == std::vector<std::string>({"route ceiling"}));
  e = pricing::estimate(*c, "openai.chat", "", ServedModel{"vendor/unbounded", "vendor/unbounded", ServedRelation::Exact},
                        usage(1'000, 1'000), {}, true);
  CHECK(!e.total && e.unpriced == std::vector<std::string>({"ceiling"}));
  // The estimate at the ceiling stays within the bound at the ceiling.
  const auto bound = pricing::upper_bound(pricing::price_of(*c->entry("vendor/routed")), {1'000, 1'000, true, {}});
  CHECK(bound.total && bound.total->nano_usd >= 12'000'000u);
}
// A usage count the estimate cannot price is named in `unpriced`, not left out of a complete-looking total.
void unpriced_usage() {
  const auto c = catalogue(R"({"version":1,"models":[
    {"id":"vendor/audio","pricing":{"prompt":"0.0000025","completion":"0.00001","audio":"0.000032","audio_output":"0.000064"}},
    {"id":"vendor/plain","pricing":{"prompt":"0.0000025","completion":"0.00001"}}]})");
  const ServedModel audio{"vendor/audio", "vendor/audio", ServedRelation::Exact}, plain{"vendor/plain", "vendor/plain", ServedRelation::Exact};
  const auto rates = pricing::price_of(*c->entry("vendor/audio"));
  CHECK(rates.input_audio == 32'000'000'000u && rates.output_audio == 64'000'000'000u);
  // 800 text and 200 audio tokens in, 200 text and 100 audio tokens out: 800 x 2.5e-6 + 200 x 3.2e-5 + 200 x 1e-5 +
  // 100 x 6.4e-5 USD. Chat counts them in `input_audio` and `audio_tokens`; Generate and Interactions in a breakdown.
  auto chat = usage(1'000, 300);
  chat.extra["input_audio"] = Count{200, Evidence::Reported};
  chat.extra["audio_tokens"] = Count{100, Evidence::Reported};
  auto e = pricing::estimate(*c, "openai.chat", "", audio, chat);
  CHECK(e.total && e.total->nano_usd == 16'800'000u && e.unpriced.empty() && e.assumptions.empty());
  auto generate = usage(1'000, 300);
  generate.extra["promptTokensDetails.TEXT"] = Count{800, Evidence::Reported};
  generate.extra["promptTokensDetails.AUDIO"] = Count{200, Evidence::Reported};
  generate.extra["candidatesTokensDetails.TEXT"] = Count{200, Evidence::Reported};
  generate.extra["candidatesTokensDetails.AUDIO"] = Count{100, Evidence::Reported};
  e = pricing::estimate(*c, "google.generate", "", audio, generate);
  CHECK(e.total && e.total->nano_usd == 16'800'000u);
  auto interactions = usage(1'000, 300);
  interactions.extra["input_tokens_by_modality.audio"] = Count{200, Evidence::Reported};
  interactions.extra["output_tokens_by_modality.audio"] = Count{100, Evidence::Reported};
  e = pricing::estimate(*c, "google.interactions", "", audio, interactions);
  CHECK(e.total && e.total->nano_usd == 16'800'000u);
  // Without an audio rate the audio tokens are not priced at the text rate.
  e = pricing::estimate(*c, "openai.chat", "", plain, chat);
  CHECK(!e.total && e.unpriced == std::vector<std::string>({"input_audio", "output_audio"}));
  // A reported zero is no audio.
  auto none = usage(1'000, 300);
  none.extra["input_audio"] = Count{0, Evidence::Reported};
  none.extra["audio_tokens"] = Count{0, Evidence::Reported};
  e = pricing::estimate(*c, "openai.chat", "", plain, none);
  CHECK(e.total && e.total->nano_usd == 5'500'000u);
  // More audio input than uncached input: the rest sits in the cache counters, which have no audio rate.
  auto cached = usage(1'000, 10, 900);
  cached.extra["input_audio"] = Count{200, Evidence::Reported};
  e = pricing::estimate(*c, "openai.chat", "", audio, cached);
  CHECK(!e.total && e.unpriced == std::vector<std::string>({"input_audio"}));
  // The bound prices every input token at the audio rate and every output token at the audio output rate.
  const auto bound = pricing::upper_bound(rates, {1'000, 300, false, {}});
  CHECK(bound.total && bound.total->nano_usd == 32'000'000u + 19'200'000u);
  CHECK(bound.total->nano_usd >= pricing::estimate(*c, "openai.chat", "", audio, chat).total->nano_usd);

  // Gemini bills the tool-use prompt beyond the prompt count.
  auto tool = usage(100, 10);
  tool.extra["toolUsePromptTokenCount"] = Count{50, Evidence::Reported};
  e = pricing::estimate(*c, "google.generate", "", plain, tool);
  CHECK(!e.total && e.unpriced == std::vector<std::string>({"toolUsePromptTokenCount"}));
  tool.extra["toolUsePromptTokenCount"] = Count{0, Evidence::Reported};
  e = pricing::estimate(*c, "google.generate", "", plain, tool);
  CHECK(e.total && e.unpriced.empty());
  // Image output tokens with no image rate are not priced at the text rate either.
  auto image = usage(100, 10);
  image.extra["candidatesTokensDetails.IMAGE"] = Count{4, Evidence::Reported};
  e = pricing::estimate(*c, "google.generate", "", plain, image);
  CHECK(!e.total && e.unpriced == std::vector<std::string>({"output_image"}));
}

// A zero rate under a priced base unit is applied and named.
void zero_rates() {
  const auto c = catalogue(R"({"version":1,"models":[
    {"id":"vendor/zero","pricing":{"prompt":"0.000001","completion":"0.000002","internal_reasoning":"0","input_cache_read":"0"}},
    {"id":"vendor/free","pricing":{"prompt":"0","completion":"0"}}]})");
  const ServedModel zero{"vendor/zero", "vendor/zero", ServedRelation::Exact}, free{"vendor/free", "vendor/free", ServedRelation::Exact};
  // 900 uncached at 1e-6 and 200 text output at 2e-6; 100 cache reads and 100 reasoning tokens are free.
  auto e = pricing::estimate(*c, "openai.chat", "", zero, usage(1'000, 300, 100, 0, 100));
  CHECK(e.total && e.total->nano_usd == 1'300'000u &&
        e.assumptions == std::vector<std::string>({"cache_read at zero", "reasoning at zero"}));
  // No such tokens, no such assumption.
  e = pricing::estimate(*c, "openai.chat", "", zero, usage(1'000, 300));
  CHECK(e.total && e.total->nano_usd == 1'600'000u && e.assumptions.empty());
  // A model priced at zero throughout is declared free, not assumed.
  e = pricing::estimate(*c, "openai.chat", "", free, usage(1'000, 300));
  CHECK(e.total && e.total->nano_usd == 0u && e.assumptions.empty());
}

// An entry priced outside the asked service tier says "tier", not "model".
void tier_reasons() {
  const auto c = catalogue(R"({"version":1,"models":[
    {"id":"vendor/tiered","pricing":{"input":"0.000001","output":"0.000002","input/flex":"0.0000005","output/flex":"0.000001"}},
    {"id":"vendor/flex-only","pricing":{"input/flex":"0.0000005","output/flex":"0.000001"}}]})");
  const ServedModel tiered{"vendor/tiered", "vendor/tiered", ServedRelation::Exact};
  const auto u = usage(1'000, 1'000);
  auto e = pricing::estimate(*c, "openai.chat", "", tiered, u);
  CHECK(e.total && e.total->nano_usd == 3'000'000u);
  e = pricing::estimate(*c, "openai.chat", "", tiered, u, "flex");
  CHECK(e.total && e.total->nano_usd == 1'500'000u);
  // The tier is matched exactly: a response's `default` or `auto` selects no rows.
  for (const char* name : {"default", "auto", "priority"}) {
    e = pricing::estimate(*c, "openai.chat", "", tiered, u, name);
    CHECK(!e.total && e.unpriced == std::vector<std::string>({"tier"}));
  }
  e = pricing::estimate(*c, "openai.chat", "", ServedModel{"vendor/flex-only", "vendor/flex-only", ServedRelation::Exact}, u);
  CHECK(!e.total && e.unpriced == std::vector<std::string>({"tier"}));
  e = pricing::estimate(*c, "openai.chat", "", ServedModel{"vendor/none", "vendor/none", ServedRelation::Exact}, u, "flex");
  CHECK(!e.total && e.unpriced == std::vector<std::string>({"model"}));
}

// A partial or contradictory usage is marked as a lower bound.
void usage_stage() {
  const auto c = catalogue(R"({"version":1,"models":[{"id":"m","pricing":{"input":"0.000001","output":"0.000002"}}]})");
  const ServedModel m{"m", "m", ServedRelation::Exact};
  auto u = usage(1'000, 1'000);
  auto e = pricing::estimate(*c, "openai.chat", "", m, u);
  CHECK(e.total && !e.lower_bound);
  u.stage = UsageStage::Final;
  CHECK(!pricing::estimate(*c, "openai.chat", "", m, u).lower_bound);
  u.stage = UsageStage::Partial;
  e = pricing::estimate(*c, "openai.chat", "", m, u);
  CHECK(e.total && e.lower_bound);
  u.stage = UsageStage::Final;
  u.quality = UsageQuality::Inconsistent;
  e = pricing::estimate(*c, "openai.chat", "", m, u);
  CHECK(e.total && e.lower_bound);
}

// A reservation built from `price_of` skips the input bands; the bound that takes the entry does not.
void reservation_from_entry() {
  const auto c = catalogue(R"({"version":1,"models":[{"id":"long","pricing":{"input":"0.000001","output":"0.000004",
    "input+200000":"0.000002","output+200000":"0.000006"}}]})");
  const auto& entry = *c->entry("long");
  const pricing::BoundRequest request{250'000, 1'000, false, {}};
  const auto actual = pricing::estimate(*c, "openai.chat", "", ServedModel{"long", "long", ServedRelation::Exact}, usage(250'000, 1'000));
  const auto base = pricing::upper_bound(pricing::price_of(entry), request);
  const auto bound = pricing::upper_bound(entry, {}, request);
  CHECK(actual.total && base.total && bound.total);
  CHECK(base.total->nano_usd < actual.total->nano_usd && bound.total->nano_usd >= actual.total->nano_usd);
  CHECK(bound.total->nano_usd == pricing::upper_bound(pricing::dearest_price_of(entry, {}, 250'000), request).total->nano_usd);
  CHECK(!pricing::upper_bound(entry, "flex", request).total);
}

// A listed price the catalogue cannot hold is counted, not dropped without a trace.
void skipped_prices() {
  std::string members;
  for (int i = 0; i < 70; ++i) members += ",\"unit" + std::to_string(i) + "\":\"0.5\"";
  const auto body = R"({"data":[{"id":"vendor/m","pricing":{"prompt":"1e-7","completion":"0.0000004","web_search":0.01,)"
                    R"("overrides":[{"prompt":"0.1"}],"bad unit":"1","audio":"-1")" + members + R"(}}]})";
  const auto parsed = descriptor::parse_listing(descriptor::Listing::OpenRouter, body, "https://openrouter.ai");
  const auto* page = std::get_if<descriptor::ListingPage>(&parsed);
  CHECK(page && page->entries.size() == 1);
  // Kept: completion, audio and 62 of the units (64 in all). Left out: "1e-7", the number, the unit named "bad unit"
  // and the 8 units past 64; the structured override is no price.
  CHECK(page->entries[0].pricing.size() == 64);
  CHECK(page->skipped_prices == 3 + 8 && page->skipped == 0);
}
} // namespace

int main() {
  try {
    prices_from_entries(); estimates(); wide_products(); bounds(); bands(); dearer_rates();
    unpriced_usage(); zero_rates(); tier_reasons(); usage_stage(); reservation_from_entry(); skipped_prices();
    std::cout << "pricing properties passed\n"; return 0;
  }
  catch (const std::exception& error) { std::cerr << "pricing test failed: " << error.what() << '\n'; return 1; }
}
