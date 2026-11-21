// Recorded answers of the chat pricing (pricing::estimate, pricing::upper_bound and pricing::worst_case) over a table of
// catalogue entries and usages that reaches every rule they apply: input bands at, below and above a threshold, service
// tiers, cache reads and writes with and without the one-hour split, reasoning, audio and image modality counters, a
// unit the entry does not list, the request fee, route ceilings at a routing gateway, zero counters, and sums that round
// up. No entry of that table has a price that is not a fixed amount. A second catalogue records the answers for a row at
// a variable price beside a fixed row of the same unit. Every field of each answer is compared, so a change to any of
// them for any of these inputs fails here.
#include "core/enum_names.h"
#include "core/pricing.h"
#include "descriptor/catalogue.h"
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace sp;

descriptor::CatalogueSnapshot catalogue(std::string_view json) {
  auto loaded = descriptor::load_catalogue(json);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("catalogue refused at " + error->pointer + ": " + error->expected);
  return std::get<descriptor::CatalogueSnapshot>(std::move(loaded));
}

const auto prices = catalogue(R"({"version":1,"models":[
  {"id":"flat","input_limit":4000,"output_limit":1000,"pricing":{"input":"0.000002","output":"0.000008","cache_read":"0.0000005",
    "cache_write":"0.0000025","cache_write_1h":"0.000004","reasoning":"0.000006","output_image":"0.00003","input_audio":"0.000004",
    "output_audio":"0.000016","request":"0.001","tool:web_search":"0.01","tool:free_tool":"0"}},
  {"id":"banded","context_window":3000,"pricing":{"input":"0.000001","output":"0.000004","cache_read":"0.0000001","input+1000":"0.000002",
    "output+1000":"0.000006","cache_read+1000":"0.0000002","prompt+500":"0.0000015","request+1000":"0.002"}},
  {"id":"tiered","input_limit":2000,"pricing":{"input":"0.000002","output":"0.000008","input/flex":"0.000001","output/flex":"0.000004",
    "cache_read/flex":"0.0000002","input/flex+1000":"0.0000015"}},
  {"id":"flex-only","input_limit":2000,"pricing":{"input/flex":"0.000001","output/flex":"0.000004"}},
  {"id":"vendor/listed","input_limit":8000,"pricing":{"prompt":"0.000003","completion":"0.000015","input_cache_read":"0.0000003",
    "input_cache_write":"0.00000375","input_cache_write_1h":"0.000006","internal_reasoning":"0.00002","audio":"0.00004",
    "audio_output":"0.00008","request":"0.0005","image":"0.04"}},
  {"id":"vendor/zero","pricing":{"prompt":"0.000001","completion":"0.000002","internal_reasoning":"0","input_cache_read":"0"}},
  {"id":"vendor/free","input_limit":100,"pricing":{"prompt":"0","completion":"0"}},
  {"id":"half","input_limit":10,"pricing":{"input":"0.0000000005","output":"0.0000000003","request":"0.0000000001"}},
  {"id":"plain-cache","input_limit":4000,"pricing":{"input":"0.000002","output":"0.000008","cache_write":"0.0000025"}},
  {"id":"input-only","input_limit":4000,"pricing":{"input":"0.000002"}},
  {"id":"vendor/routed","context_window":5000,"pricing":{"prompt":"0.0000001","completion":"0.0000004","ceiling_input":"0.0000024",
    "ceiling_output":"0.0000096","ceiling_request":"0.0000005","request":"0.0000002"}},
  {"id":"vendor/routed-b","context_window":7000,"pricing":{"prompt":"0.0000002","completion":"0.0000003","ceiling_input":"0.000003",
    "ceiling_output":"0.000009"}},
  {"id":"vendor/unceiled","context_window":5000,"pricing":{"prompt":"0.0000001","completion":"0.0000004"}},
  {"id":"huge","input_limit":100000000,"pricing":{"input":"1000","output":"0.000000000000001"}},
  {"id":"vendor/requested","served_as":["vendor/elsewhere"],"pricing":{"input":"0.000002","output":"0.000003"}},
  {"id":"vendor/","match":"prefix","pricing":{"input":"0.000005","output":"0.000005"}}]})");

Usage usage(std::optional<std::uint64_t> input, std::optional<std::uint64_t> output, std::uint64_t cache_read = 0, std::uint64_t cache_write = 0,
            std::uint64_t reasoning = 0, std::initializer_list<std::pair<const char*, std::uint64_t>> extra = {}) {
  Usage u;
  if (input) u.input_total = Count{*input, Evidence::Reported};
  if (output) u.output_total = Count{*output, Evidence::Reported};
  if (cache_read) u.cache_read = Count{cache_read, Evidence::Reported};
  if (cache_write) u.cache_write = Count{cache_write, Evidence::Reported};
  if (reasoning) u.reasoning = Count{reasoning, Evidence::Reported};
  for (const auto& [key, value] : extra) u.extra[key] = Count{value, Evidence::Reported};
  return u;
}
std::string list(const std::vector<std::string>& items) {
  std::string out = "[";
  for (const auto& item : items) out += (out.size() > 1 ? "," : "") + item;
  return out + "]";
}
std::string amount(const std::optional<UsdAmount>& total) {
  if (!total) return "none";
  return std::to_string(total->nano_usd) + " " + std::string(to_string(total->evidence)) + " " + std::string(to_string(total->rounding));
}
std::string describe(const pricing::CostEstimate& e) {
  return amount(e.total) + " entry=" + e.entry + " requested=" + std::to_string(e.requested_entry) + " assumed=" + list(e.assumptions) +
         " unpriced=" + list(e.unpriced) + " lower=" + std::to_string(e.lower_bound);
}
std::string describe(const pricing::Bound& b) { return amount(b.total) + " unpriced=" + list(b.unpriced); }
std::string describe(const pricing::WorstCase& w) {
  std::string out = amount(w.total);
  for (const auto& r : w.unbounded) out += " | " + std::string(pricing::to_string(r.by)) + " [" + r.subject + "] " + r.detail;
  out += " | entry=" + w.entry + " model=" + w.model + " tokens=" + std::to_string(w.input_tokens) + "/" + std::to_string(w.output_tokens) + "/" +
         std::to_string(w.invocation_input_tokens) + " fees=" + std::to_string(w.tool_fees_nano_usd) + " invocations=" + std::to_string(w.invocations) +
         " attempts=" + std::to_string(w.attempts) + " tools=";
  for (const auto& [type, uses] : w.tool_uses) out += type + ":" + std::to_string(uses) + ",";
  out += " ceiling=" + std::to_string(w.route_ceiling) + " pinned=" + std::to_string(w.pinned) + " pin=";
  if (w.pin) {
    for (const auto& field : {w.pin->prompt_nano_usd, w.pin->completion_nano_usd, w.pin->image_nano_usd, w.pin->audio_nano_usd, w.pin->request_nano_usd})
      out += (field ? std::to_string(*field) : std::string("-")) + ",";
  } else {
    out += "none";
  }
  return out;
}

struct Pin {
  std::string name, expected;
};
std::vector<std::pair<std::string, std::string>> answers;

void estimates() {
  const auto& c = *prices;
  const auto at = [&](const char* name, std::string_view family, std::string_view model, const Usage& u, std::string_view tier = {}, bool ceiling = false,
                      ServedRelation relation = ServedRelation::Exact, std::string_view served = {}) {
    const ServedModel m{std::string(model), std::string(served.empty() ? model : served), relation};
    answers.emplace_back(std::string("estimate ") + name, describe(pricing::estimate(c, family, "", m, u, tier, ceiling)));
  };
  at("flat basic", "openai.chat", "flat", usage(1200, 300, 200, 0, 100));
  at("flat zero counters", "openai.chat", "flat", usage(0, 0));
  at("flat no counters", "openai.chat", "flat", Usage{});
  at("flat no input", "openai.chat", "flat", usage(std::nullopt, 10));
  at("flat no output", "openai.chat", "flat", usage(10, std::nullopt));
  auto direct = usage(1000, 0); direct.input_uncached = Count{400, Evidence::Reported};
  at("flat uncached", "openai.chat", "flat", direct);
  at("flat cache split", "anthropic.messages", "flat", usage(1000, 10, 0, 300, 0, {{"cache_creation.ephemeral_5m_input_tokens", 100}, {"cache_creation.ephemeral_1h_input_tokens", 200}}));
  at("flat cache no split", "anthropic.messages", "flat", usage(1000, 10, 0, 300));
  at("flat cache bad split", "anthropic.messages", "flat", usage(1000, 10, 0, 300, 0, {{"cache_creation.ephemeral_5m_input_tokens", 100}, {"cache_creation.ephemeral_1h_input_tokens", 50}}));
  at("plain cache hour", "anthropic.messages", "plain-cache", usage(1000, 0, 0, 100, 0, {{"cache_creation.ephemeral_5m_input_tokens", 0}, {"cache_creation.ephemeral_1h_input_tokens", 100}}));
  at("plain cache write", "anthropic.messages", "plain-cache", usage(1000, 5, 0, 100));
  at("plain cache read at input", "openai.chat", "plain-cache", usage(1000, 5, 400, 0));
  at("flat chat audio", "openai.chat", "flat", usage(1000, 300, 0, 0, 0, {{"input_audio", 200}, {"audio_tokens", 100}}));
  at("flat audio above uncached", "openai.chat", "flat", usage(1000, 10, 900, 0, 0, {{"input_audio", 200}}));
  at("flat generate audio", "google.generate", "flat", usage(1000, 300, 0, 0, 0, {{"promptTokensDetails.TEXT", 800}, {"promptTokensDetails.AUDIO", 200},
                                                                                 {"candidatesTokensDetails.TEXT", 200}, {"candidatesTokensDetails.AUDIO", 100}}));
  at("flat interactions audio", "google.interactions", "flat", usage(1000, 300, 0, 0, 0, {{"input_tokens_by_modality.audio", 200}, {"output_tokens_by_modality.audio", 100}}));
  at("flat image breakdown", "google.generate", "flat", usage(1000, 10, 0, 0, 0, {{"candidatesTokensDetails.IMAGE", 4}}));
  at("flat image interactions", "google.interactions", "flat", usage(1000, 10, 0, 0, 2, {{"output_tokens_by_modality.image", 6}}));
  at("flat image and audio", "google.generate", "flat", usage(1000, 50, 0, 0, 5, {{"candidatesTokensDetails.IMAGE", 20}, {"candidatesTokensDetails.AUDIO", 30}}));
  at("plain image unpriced", "google.generate", "plain-cache", usage(1000, 10, 0, 0, 0, {{"candidatesTokensDetails.IMAGE", 4}}));
  at("plain audio unpriced", "openai.chat", "plain-cache", usage(1000, 10, 0, 0, 0, {{"input_audio", 5}, {"audio_tokens", 3}}));
  at("plain audio zero", "openai.chat", "plain-cache", usage(1000, 10, 0, 0, 0, {{"input_audio", 0}, {"audio_tokens", 0}}));
  at("flat tool use prompt", "google.generate", "flat", usage(100, 10, 0, 0, 0, {{"toolUsePromptTokenCount", 50}}));
  at("flat tool use zero", "google.generate", "flat", usage(100, 10, 0, 0, 0, {{"toolUsePromptTokenCount", 0}}));
  at("flat compaction", "anthropic.messages", "flat", usage(251, 40, 0, 0, 0, {{"compaction.input_tokens", 112'197}, {"compaction.output_tokens", 319},
                                                                              {"compaction.cache_read_input_tokens", 10}, {"compaction.cache_creation_input_tokens", 7}}));
  at("flat compaction elsewhere", "openai.chat", "flat", usage(251, 40, 0, 0, 0, {{"compaction.input_tokens", 112'197}, {"compaction.output_tokens", 319}}));
  at("plain compaction fallback", "anthropic.messages", "plain-cache", usage(10, 1, 0, 0, 0, {{"compaction.cache_read_input_tokens", 10}}));
  at("listed names", "anthropic.messages", "vendor/listed", usage(3000, 400, 500, 300, 100, {{"cache_creation.ephemeral_5m_input_tokens", 100}, {"cache_creation.ephemeral_1h_input_tokens", 200},
                                                                                          {"input_audio", 50}, {"audio_tokens", 20}}));
  at("zero rates named", "openai.chat", "vendor/zero", usage(1000, 300, 100, 0, 100));
  at("zero rates unused", "openai.chat", "vendor/zero", usage(1000, 300));
  at("free model", "openai.chat", "vendor/free", usage(1000, 300));
  at("half rounds up", "openai.chat", "half", usage(3, 1));
  at("half exact", "openai.chat", "half", usage(2, 0));
  at("half sum rounds up", "openai.chat", "half", usage(1, 1));
  at("input only output billed", "openai.chat", "input-only", usage(100, 5));
  at("input only no output", "openai.chat", "input-only", usage(100, 0));
  at("huge fits", "openai.chat", "huge", usage(1'000'000, 1));
  at("huge overflow", "openai.chat", "huge", usage(20'000'000, 0));
  for (const std::uint64_t input : {499u, 500u, 501u, 999u, 1000u, 1001u})
    at(("banded input " + std::to_string(input)).c_str(), "openai.chat", "banded", usage(input, 100));
  at("banded cache above", "openai.chat", "banded", usage(1001, 10, 600));
  at("banded cache counts length", "openai.chat", "banded", usage(std::nullopt, 10, 600, 401));
  at("tiered standard", "openai.chat", "tiered", usage(500, 100));
  at("tiered flex", "openai.chat", "tiered", usage(500, 100, 100), "flex");
  at("tiered flex above", "openai.chat", "tiered", usage(1001, 100), "flex");
  at("tiered priority", "openai.chat", "tiered", usage(500, 100), "priority");
  at("flex only standard", "openai.chat", "flex-only", usage(500, 100));
  at("flex only flex", "openai.chat", "flex-only", usage(500, 100), "flex");
  at("unknown model", "openai.chat", "nobody/none", usage(10, 10));
  at("unknown model flex", "openai.chat", "nobody/none", usage(10, 10), "flex");
  at("served by prefix", "openai.chat", "vendor/requested", usage(10, 10), {}, false, ServedRelation::Routed, "vendor/unpriced");
  at("served unpriced", "openai.chat", "vendor/requested", usage(1000, 1000, 100, 50), {}, false, ServedRelation::Unexplained, "nobody/served");
  at("served not reported", "openai.chat", "vendor/requested", usage(10, 10), {}, false, ServedRelation::NotReported, "vendor/unpriced");
  at("routed ceiling", "openai.chat", "vendor/routed", usage(1000, 1000, 200), {}, true);
  at("routed ceiling cache write", "openai.chat", "vendor/routed", usage(1000, 1000, 0, 300), {}, true);
  at("routed ceiling no input", "openai.chat", "vendor/routed", usage(std::nullopt, 1000), {}, true);
  at("routed ceiling no output", "openai.chat", "vendor/routed", usage(1000, std::nullopt), {}, true);
  at("routed ceiling compaction", "anthropic.messages", "vendor/routed", usage(10, 10, 0, 0, 0, {{"compaction.input_tokens", 100}, {"compaction.output_tokens", 7}}), {}, true);
  at("routed ceiling fee only", "openai.chat", "vendor/routed-b", usage(10, 10), {}, true);
  at("unceiled at ceiling", "openai.chat", "vendor/unceiled", usage(1000, 1000), {}, true);
  at("routed listed", "openai.chat", "vendor/routed", usage(1000, 1000));
  auto partial = usage(1000, 1000); partial.stage = UsageStage::Partial;
  at("partial", "openai.chat", "flat", partial);
  auto inconsistent = usage(1000, 1000); inconsistent.stage = UsageStage::Final; inconsistent.quality = UsageQuality::Inconsistent;
  at("inconsistent", "openai.chat", "flat", inconsistent);
}

void bounds() {
  const auto& c = *prices;
  const auto at = [&](const char* name, const pricing::Bound& b) { answers.emplace_back(std::string("bound ") + name, describe(b)); };
  const auto& flat = *c.entry("flat");
  const auto& banded = *c.entry("banded");
  at("flat price tools", pricing::upper_bound(pricing::price_of(flat), {1000, 100, false, {{"web_search", 2}, {"free_tool", 5}}}));
  at("flat unknown tool", pricing::upper_bound(pricing::price_of(flat), {1000, 100, false, {{"unknown_tool", 1}, {"web_search", 0}}}));
  at("flat entry", pricing::upper_bound(flat, {}, {1000, 100, false, {}}));
  at("flat zero", pricing::upper_bound(flat, {}, {0, 0, false, {}}));
  at("flat at ceiling", pricing::upper_bound(flat, {}, {1, 1, true, {}}));
  at("flat priority", pricing::upper_bound(flat, "priority", {10, 10, false, {}}));
  at("flat priority zero", pricing::upper_bound(flat, "priority", {0, 0, false, {}}));
  at("input only", pricing::upper_bound(*c.entry("input-only"), {}, {100, 5, false, {}}));
  at("input only no output", pricing::upper_bound(*c.entry("input-only"), {}, {100, 0, false, {}}));
  at("listed", pricing::upper_bound(*c.entry("vendor/listed"), {}, {1000, 10, false, {}}));
  at("zero rates", pricing::upper_bound(*c.entry("vendor/zero"), {}, {1000, 10, false, {}}));
  at("half", pricing::upper_bound(*c.entry("half"), {}, {3, 1, false, {}}));
  at("half sum", pricing::upper_bound(*c.entry("half"), {}, {1, 1, false, {}}));
  at("huge fits", pricing::upper_bound(*c.entry("huge"), {}, {1'000'000, 1, false, {}}));
  at("huge overflow", pricing::upper_bound(*c.entry("huge"), {}, {20'000'000, 0, false, {}}));
  for (const std::uint64_t input : {499u, 500u, 501u, 999u, 1000u, 1001u})
    at(("banded " + std::to_string(input)).c_str(), pricing::upper_bound(banded, {}, {input, 100, false, {}}));
  at("banded base price", pricing::upper_bound(pricing::price_of(banded), {1001, 100, false, {}}));
  at("banded dearest price", pricing::upper_bound(pricing::dearest_price_of(banded, {}, 1001), {1001, 100, false, {}}));
  at("tiered flex", pricing::upper_bound(*c.entry("tiered"), "flex", {500, 100, false, {}}));
  at("tiered flex above", pricing::upper_bound(*c.entry("tiered"), "flex", {1001, 100, false, {}}));
  at("flex only standard", pricing::upper_bound(*c.entry("flex-only"), {}, {500, 100, false, {}}));
  at("routed ceiling", pricing::upper_bound(*c.entry("vendor/routed"), {}, {1000, 1000, true, {}}));
  at("routed listed", pricing::upper_bound(*c.entry("vendor/routed"), {}, {1000, 1000, false, {}}));
  at("unceiled at ceiling", pricing::upper_bound(*c.entry("vendor/unceiled"), {}, {1000, 1000, true, {}}));
  const auto describe_price = [](const pricing::ModelPrice& p) {
    std::string out;
    for (const auto& rate : {p.input, p.output, p.cache_read, p.cache_write, p.reasoning, p.request, p.cache_write_1h, p.output_image, p.input_audio,
                             p.output_audio, p.ceiling_input, p.ceiling_output, p.ceiling_request})
      out += (rate ? std::to_string(*rate) : std::string("-")) + ",";
    for (const auto& [type, fee] : p.tool_fees) out += type + "=" + std::to_string(fee) + ",";
    return out;
  };
  for (const std::uint64_t input : {0u, 500u, 501u, 1001u}) {
    answers.emplace_back("price banded " + std::to_string(input), describe_price(pricing::price_of(banded, {}, input)));
    answers.emplace_back("dearest banded " + std::to_string(input), describe_price(pricing::dearest_price_of(banded, {}, input)));
  }
  answers.emplace_back("price flat", describe_price(pricing::price_of(flat)));
  answers.emplace_back("price listed", describe_price(pricing::price_of(*c.entry("vendor/listed"))));
  answers.emplace_back("price tiered flex 1001", describe_price(pricing::price_of(*c.entry("tiered"), "flex", 1001)));
  answers.emplace_back("dearest tiered flex 1001", describe_price(pricing::dearest_price_of(*c.entry("tiered"), "flex", 1001)));
}

void worst_cases() {
  const auto& c = *prices;
  struct Case {
    const char* name;
    pricing::WorstCaseInput input;
    RequestFacts facts;
  };
  const auto in = [](std::string_view model, std::optional<std::uint64_t> output, std::optional<std::uint64_t> measured = std::nullopt,
                     std::uint64_t invocations = 1, std::uint64_t attempts = 1, std::string_view tier = {}, bool routed = false) {
    pricing::WorstCaseInput input;
    input.family = "openai.chat";
    input.origin = "";
    input.model = model;
    input.output_tokens = output;
    input.measured_input_tokens = measured;
    input.invocations = invocations;
    input.attempts = attempts;
    input.service_tier = tier;
    input.routed_gateway = routed;
    return input;
  };
  RequestFacts none;
  RequestFacts tools;
  tools.tools.push_back({{"web_search", "free_tool"}, 3, "max_tool_calls", true});
  RequestFacts uncapped;
  uncapped.tools.push_back({{"web_search"}, std::nullopt, "max_uses", true});
  RequestFacts unlisted;
  unlisted.tools.push_back({{"web_search", "code_execution"}, 2, "max_tool_calls", true});
  RequestFacts plugins;
  plugins.plugins = true;
  RequestFacts fallbacks;
  fallbacks.fallback_models = {"vendor/routed-b", "vendor/routed"};
  RequestFacts pinned;
  pinned.routing = OpenRouterRouting{};
  pinned.routing->max_price = OpenRouterMaxPrice{2'400, 9'600, std::nullopt, std::nullopt, 1};
  RequestFacts capped;
  capped.routing = OpenRouterRouting{};
  capped.routing->max_price = OpenRouterMaxPrice{1'000, std::nullopt, 5, std::nullopt, std::nullopt};
  const Case cases[]{
      {"flat limit", in("flat", 100), none},
      {"flat measured", in("flat", 100, 1234), none},
      {"flat measured above limit", in("flat", 100, 9000), none},
      {"flat invocations", in("flat", 100, 1234, 3, 2), none},
      {"flat tools", in("flat", 100, 1234), tools},
      {"flat tools uncapped", in("flat", 100, 1234), uncapped},
      {"flat tools unlisted", in("flat", 100, 1234), unlisted},
      {"flat plugins", in("flat", 100, 1234), plugins},
      {"flat no output cap", in("flat", std::nullopt, 1234), none},
      {"flat no invocations", in("flat", 100, 1234, 0), none},
      {"flat no attempts", in("flat", 100, 1234, 1, 0), none},
      {"flat attempts", in("flat", 100, 1234, 1, 4), none},
      {"flat priority", in("flat", 100, 1234, 1, 1, "priority"), none},
      {"banded limit", in("banded", 100), none},
      {"banded below", in("banded", 100, 999), none},
      {"banded at", in("banded", 100, 1000), none},
      {"banded above", in("banded", 100, 1001), none},
      {"banded invocations", in("banded", 100, 400, 2), none},
      {"tiered flex", in("tiered", 100, 500, 1, 1, "flex"), none},
      {"tiered flex above", in("tiered", 100, 1500, 1, 1, "flex"), none},
      {"flex only standard", in("flex-only", 100, 500), none},
      {"input only", in("input-only", 100, 500), none},
      {"input only no output", in("input-only", 0, 500), none},
      {"no limit measured", in("vendor/zero", 100, 500), none},
      {"no limit", in("vendor/zero", 100), none},
      {"no limit invocations", in("vendor/zero", 100, 500, 2), none},
      {"free", in("vendor/free", 100), none},
      {"half", in("half", 1, 3), none},
      {"huge overflow", in("huge", 0), none},
      {"huge attempts overflow", in("huge", 0, 18'000, 1, 2'000), none},
      {"output overflow", in("flat", 18'446'744'073'709'551'615u, 1, 2), none},
      {"unknown model", in("nobody/none", 100, 10), none},
      {"direct with ceiling", in("vendor/routed", 100, 1000), none},
      {"gateway", in("vendor/routed", 100, 1000, 1, 1, {}, true), none},
      {"gateway limit", in("vendor/routed", 100, std::nullopt, 1, 1, {}, true), none},
      {"gateway pinned", in("vendor/routed", 100, 1000, 1, 1, {}, true), pinned},
      {"gateway capped", in("vendor/routed", 100, 1000, 1, 1, {}, true), capped},
      {"gateway fallbacks", in("vendor/routed", 100, 1000, 1, 1, {}, true), fallbacks},
      {"gateway unceiled", in("vendor/unceiled", 100, 1000, 1, 1, {}, true), none},
      {"gateway no fee", in("vendor/routed-b", 100, 1000, 1, 1, {}, true), none},
  };
  for (const auto& test : cases) {
    auto input = test.input;
    input.facts = &test.facts;
    answers.emplace_back(std::string("worst ") + test.name, describe(pricing::worst_case(input, c)));
  }
}

// A variable row above a band beside a fixed row of its unit, for a cache read and for the request fee.
void variable_rows() {
  const auto c = catalogue(R"({"version":1,"models":[
    {"id":"variable-cache","pricing":{"input":"0.000003","output":"0.000008","cache_read":"0.000004","cache_read+1000":"-1"}},
    {"id":"variable-fee","pricing":{"input":"0.000003","output":"0.000008","request":"0.001","request+1000":"-1"}}]})");
  const auto describe_price = [](const pricing::ModelPrice& p) {
    std::string out;
    for (const auto& rate : {p.input, p.output, p.cache_read, p.cache_write, p.reasoning, p.request, p.cache_write_1h, p.output_image, p.input_audio,
                             p.output_audio, p.ceiling_input, p.ceiling_output, p.ceiling_request})
      out += (rate ? std::to_string(*rate) : std::string("-")) + ",";
    for (const auto& [type, fee] : p.tool_fees) out += type + "=" + std::to_string(fee) + ",";
    return out;
  };
  for (const auto& [name, id] : {std::pair{"cache", "variable-cache"}, std::pair{"fee", "variable-fee"}}) {
    const auto& entry = *c->entry(id);
    const auto at = [&](const char* what) { return std::string("variable ") + what + " " + name; };
    answers.emplace_back(at("dearest 2000"), describe_price(pricing::dearest_price_of(entry, {}, 2'000)));
    answers.emplace_back(at("dearest 500"), describe_price(pricing::dearest_price_of(entry, {}, 500)));
    answers.emplace_back(at("exact 2000"), describe_price(pricing::price_of(entry, {}, 2'000)));
    answers.emplace_back(at("bound price 2000"), describe(pricing::upper_bound(pricing::dearest_price_of(entry, {}, 2'000), {2'000, 10, false, {}})));
    answers.emplace_back(at("bound entry 2000"), describe(pricing::upper_bound(entry, {}, {2'000, 10, false, {}})));
    answers.emplace_back(at("bound entry 500"), describe(pricing::upper_bound(entry, {}, {500, 10, false, {}})));
    pricing::WorstCaseInput input;
    input.family = "openai.chat";
    input.model = id;
    input.output_tokens = 10;
    input.measured_input_tokens = 2'000;
    input.invocations = 1;
    answers.emplace_back(at("worst 2000"), describe(pricing::worst_case(input, *c)));
    const ServedModel model{id, id, ServedRelation::Exact};
    answers.emplace_back(at("estimate 2000 reads"), describe(pricing::estimate(*c, "openai.chat", "", model, usage(2'000, 10, 100))));
    answers.emplace_back(at("estimate 2000"), describe(pricing::estimate(*c, "openai.chat", "", model, usage(2'000, 10))));
  }
}

// The recorded answers, by name.
const std::vector<Pin> pins{
    {"estimate flat basic", "9700000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[output at output_image] unpriced=[] lower=0"},
    {"estimate flat zero counters", "1000000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate flat no counters", "none entry=flat requested=0 assumed=[] unpriced=[input,output] lower=0"},
    {"estimate flat no input", "none entry=flat requested=0 assumed=[output at output_image] unpriced=[input] lower=0"},
    {"estimate flat no output", "none entry=flat requested=0 assumed=[] unpriced=[output] lower=0"},
    {"estimate flat uncached", "1800000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate flat cache split", "3750000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[output at output_image] unpriced=[] lower=0"},
    {"estimate flat cache no split", "3900000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[cache_write at cache_write_1h,output at output_image] unpriced=[] lower=0"},
    {"estimate flat cache bad split", "3900000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[cache_write at cache_write_1h,output at output_image] unpriced=[] lower=0"},
    {"estimate plain cache hour", "2050000 Derived CeilingCatalogueProduct entry=plain-cache requested=0 assumed=[cache_write_1h at cache_write] unpriced=[] lower=0"},
    {"estimate plain cache write", "2090000 Derived CeilingCatalogueProduct entry=plain-cache requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate plain cache read at input", "2040000 Derived CeilingCatalogueProduct entry=plain-cache requested=0 assumed=[cache_read at input] unpriced=[] lower=0"},
    {"estimate flat chat audio", "12400000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[output at output_image] unpriced=[] lower=0"},
    {"estimate flat audio above uncached", "none entry=flat requested=0 assumed=[output at output_image] unpriced=[input_audio] lower=0"},
    {"estimate flat generate audio", "6600000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate flat interactions audio", "6600000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate flat image breakdown", "3168000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate flat image interactions", "3208000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate flat image and audio", "4030000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate plain image unpriced", "none entry=plain-cache requested=0 assumed=[] unpriced=[output_image] lower=0"},
    {"estimate plain audio unpriced", "none entry=plain-cache requested=0 assumed=[] unpriced=[input_audio,output_audio] lower=0"},
    {"estimate plain audio zero", "2080000 Derived CeilingCatalogueProduct entry=plain-cache requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate flat tool use prompt", "none entry=flat requested=0 assumed=[output at output_image] unpriced=[toolUsePromptTokenCount] lower=0"},
    {"estimate flat tool use zero", "1500000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[output at output_image] unpriced=[] lower=0"},
    {"estimate flat compaction", "229670500 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[output at output_image] unpriced=[] lower=0"},
    {"estimate flat compaction elsewhere", "2702000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[output at output_image] unpriced=[] lower=0"},
    {"estimate plain compaction fallback", "48000 Derived CeilingCatalogueProduct entry=plain-cache requested=0 assumed=[cache_read at input] unpriced=[] lower=0"},
    {"estimate listed names", "18475000 Derived CeilingCatalogueProduct entry=vendor/listed requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate zero rates named", "1300000 Derived CeilingCatalogueProduct entry=vendor/zero requested=0 assumed=[cache_read at zero,reasoning at zero] unpriced=[] lower=0"},
    {"estimate zero rates unused", "1600000 Derived CeilingCatalogueProduct entry=vendor/zero requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate free model", "0 Derived CeilingCatalogueProduct entry=vendor/free requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate half rounds up", "2 Derived CeilingCatalogueProduct entry=half requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate half exact", "2 Derived CeilingCatalogueProduct entry=half requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate half sum rounds up", "1 Derived CeilingCatalogueProduct entry=half requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate input only output billed", "none entry=input-only requested=0 assumed=[] unpriced=[output] lower=0"},
    {"estimate input only no output", "200000 Derived CeilingCatalogueProduct entry=input-only requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate huge fits", "1000000000000000001 Derived CeilingCatalogueProduct entry=huge requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate huge overflow", "none entry=huge requested=0 assumed=[] unpriced=[overflow] lower=0"},
    {"estimate banded input 499", "899000 Derived CeilingCatalogueProduct entry=banded requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate banded input 500", "900000 Derived CeilingCatalogueProduct entry=banded requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate banded input 501", "1151500 Derived CeilingCatalogueProduct entry=banded requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate banded input 999", "1898500 Derived CeilingCatalogueProduct entry=banded requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate banded input 1000", "1900000 Derived CeilingCatalogueProduct entry=banded requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate banded input 1001", "4602000 Derived CeilingCatalogueProduct entry=banded requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate banded cache above", "2982000 Derived CeilingCatalogueProduct entry=banded requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate banded cache counts length", "none entry=banded requested=0 assumed=[cache_write at input] unpriced=[input] lower=0"},
    {"estimate tiered standard", "1800000 Derived CeilingCatalogueProduct entry=tiered requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate tiered flex", "820000 Derived CeilingCatalogueProduct entry=tiered requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate tiered flex above", "1901500 Derived CeilingCatalogueProduct entry=tiered requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate tiered priority", "none entry= requested=0 assumed=[] unpriced=[tier] lower=0"},
    {"estimate flex only standard", "none entry= requested=0 assumed=[] unpriced=[tier] lower=0"},
    {"estimate flex only flex", "900000 Derived CeilingCatalogueProduct entry=flex-only requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate unknown model", "none entry= requested=0 assumed=[] unpriced=[model] lower=0"},
    {"estimate unknown model flex", "none entry= requested=0 assumed=[] unpriced=[model] lower=0"},
    {"estimate served by prefix", "100000 Derived CeilingCatalogueProduct entry=vendor/ requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate served unpriced", "5000000 Derived CeilingCatalogueProduct entry=vendor/requested requested=1 assumed=[cache_read at input,cache_write at input] unpriced=[] lower=0"},
    {"estimate served not reported", "50000 Derived CeilingCatalogueProduct entry=vendor/requested requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate routed ceiling", "12000500 Derived CeilingCatalogueProduct entry=vendor/routed requested=0 assumed=[route ceiling] unpriced=[] lower=0"},
    {"estimate routed ceiling cache write", "12000500 Derived CeilingCatalogueProduct entry=vendor/routed requested=0 assumed=[route ceiling] unpriced=[] lower=0"},
    {"estimate routed ceiling no input", "none entry=vendor/routed requested=0 assumed=[route ceiling] unpriced=[input] lower=0"},
    {"estimate routed ceiling no output", "none entry=vendor/routed requested=0 assumed=[route ceiling] unpriced=[output] lower=0"},
    {"estimate routed ceiling compaction", "427700 Derived CeilingCatalogueProduct entry=vendor/routed requested=0 assumed=[route ceiling] unpriced=[] lower=0"},
    {"estimate routed ceiling fee only", "120000 Derived CeilingCatalogueProduct entry=vendor/routed-b requested=0 assumed=[route ceiling] unpriced=[] lower=0"},
    {"estimate unceiled at ceiling", "none entry=vendor/unceiled requested=0 assumed=[] unpriced=[ceiling] lower=0"},
    {"estimate routed listed", "500200 Derived CeilingCatalogueProduct entry=vendor/routed requested=0 assumed=[] unpriced=[] lower=0"},
    {"estimate partial", "33000000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[output at output_image] unpriced=[] lower=1"},
    {"estimate inconsistent", "33000000 Derived CeilingCatalogueProduct entry=flat requested=0 assumed=[output at output_image] unpriced=[] lower=1"},
    {"bound flat price tools", "28000000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound flat unknown tool", "none unpriced=[tool:unknown_tool]"},
    {"bound flat entry", "8000000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound flat zero", "1000000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound flat at ceiling", "none unpriced=[ceiling]"},
    {"bound flat priority", "none unpriced=[input,output]"},
    {"bound flat priority zero", "0 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound input only", "none unpriced=[output]"},
    {"bound input only no output", "200000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound listed", "none unpriced=[image]"},
    {"bound zero rates", "1020000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound half", "2 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound half sum", "1 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound huge fits", "1000000000000000001 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound huge overflow", "none unpriced=[overflow]"},
    {"bound banded 499", "899000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound banded 500", "900000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound banded 501", "1151500 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound banded 999", "1898500 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound banded 1000", "1900000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound banded 1001", "4602000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound banded base price", "1401000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound banded dearest price", "4602000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound tiered flex", "900000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound tiered flex above", "1901500 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound flex only standard", "none unpriced=[input,output]"},
    {"bound routed ceiling", "12000500 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound routed listed", "500200 Derived CeilingCatalogueProduct unpriced=[]"},
    {"bound unceiled at ceiling", "none unpriced=[ceiling]"},
    {"price banded 0", "1000000000,4000000000,100000000,-,-,-,-,-,-,-,-,-,-,"},
    {"dearest banded 0", "1000000000,4000000000,100000000,-,-,-,-,-,-,-,-,-,-,"},
    {"price banded 500", "1000000000,4000000000,100000000,-,-,-,-,-,-,-,-,-,-,"},
    {"dearest banded 500", "1000000000,4000000000,100000000,-,-,-,-,-,-,-,-,-,-,"},
    {"price banded 501", "1500000000,4000000000,100000000,-,-,-,-,-,-,-,-,-,-,"},
    {"dearest banded 501", "1500000000,4000000000,100000000,-,-,-,-,-,-,-,-,-,-,"},
    {"price banded 1001", "2000000000,6000000000,200000000,-,-,2000000000000,-,-,-,-,-,-,-,"},
    {"dearest banded 1001", "2000000000,6000000000,200000000,-,-,2000000000000,-,-,-,-,-,-,-,"},
    {"price flat", "2000000000,8000000000,500000000,2500000000,6000000000,1000000000000,4000000000,30000000000,4000000000,16000000000,-,-,-,free_tool=0,web_search=10000000000000,"},
    {"price listed", "3000000000,15000000000,300000000,3750000000,20000000000,500000000000,6000000000,-,40000000000,80000000000,-,-,-,"},
    {"price tiered flex 1001", "1500000000,4000000000,200000000,-,-,-,-,-,-,-,-,-,-,"},
    {"dearest tiered flex 1001", "1500000000,4000000000,200000000,-,-,-,-,-,-,-,-,-,-,"},
    {"worst flat limit", "20000000 Derived CeilingCatalogueProduct | entry=flat model=flat tokens=4000/100/4000 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst flat measured", "8936000 Derived CeilingCatalogueProduct | entry=flat model=flat tokens=1234/100/1234 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst flat measured above limit", "20000000 Derived CeilingCatalogueProduct | entry=flat model=flat tokens=4000/100/4000 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst flat invocations", "93872000 Derived CeilingCatalogueProduct | entry=flat model=flat tokens=9234/300/4000 fees=0 invocations=3 attempts=2 tools= ceiling=0 pinned=0 pin=none"},
    {"worst flat tools", "38936000 Derived CeilingCatalogueProduct | entry=flat model=flat tokens=1234/100/1234 fees=30000000 invocations=1 attempts=1 tools=web_search:3, ceiling=0 pinned=0 pin=none"},
    {"worst flat tools uncapped", "none | ToolCap [web_search] the request sets no max_uses | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst flat tools unlisted", "none | ToolFee [code_execution] catalogue entry 'flat' lists no fee (unit tool:code_execution) | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst flat plugins", "none | Unmodeled [OpenRouter plugins] the catalogue prices no plugin | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst flat no output cap", "none | OutputCap [] the request states no output cap | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst flat no invocations", "none | Invocations [] the request does not state how many model invocations it may bill | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst flat no attempts", "none | Invocations [] the request does not state how many model invocations it may bill | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=0 tools= ceiling=0 pinned=0 pin=none"},
    {"worst flat attempts", "35744000 Derived CeilingCatalogueProduct | entry=flat model=flat tokens=1234/100/1234 fees=0 invocations=1 attempts=4 tools= ceiling=0 pinned=0 pin=none"},
    {"worst flat priority", "none | TierPrice [flat] the entry prices no unit under service tier 'priority' | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst banded limit", "8600000 Derived CeilingCatalogueProduct | entry=banded model=banded tokens=3000/100/3000 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst banded below", "1898500 Derived CeilingCatalogueProduct | entry=banded model=banded tokens=999/100/999 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst banded at", "1900000 Derived CeilingCatalogueProduct | entry=banded model=banded tokens=1000/100/1000 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst banded above", "4602000 Derived CeilingCatalogueProduct | entry=banded model=banded tokens=1001/100/1001 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst banded invocations", "10000000 Derived CeilingCatalogueProduct | entry=banded model=banded tokens=3400/200/3000 fees=0 invocations=2 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst tiered flex", "900000 Derived CeilingCatalogueProduct | entry=tiered model=tiered tokens=500/100/500 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst tiered flex above", "2650000 Derived CeilingCatalogueProduct | entry=tiered model=tiered tokens=1500/100/1500 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst flex only standard", "none | TierPrice [flex-only] the entry prices no unit at the standard rates | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst input only", "none | ModelPrice [input-only] no output rate | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst input only no output", "1000000 Derived CeilingCatalogueProduct | entry=input-only model=input-only tokens=500/0/500 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst no limit measured", "700000 Derived CeilingCatalogueProduct | entry=vendor/zero model=vendor/zero tokens=500/100/500 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst no limit", "none | InputBound [vendor/zero] no measured input and no catalogue input limit | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst no limit invocations", "none | InputBound [vendor/zero] later invocations need a catalogue input limit | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst free", "0 Derived CeilingCatalogueProduct | entry=vendor/free model=vendor/free tokens=100/100/100 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst half", "2 Derived CeilingCatalogueProduct | entry=half model=half tokens=3/1/3 fees=0 invocations=1 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst huge overflow", "none | Overflow [huge] the bound does not fit 64 bits | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst huge attempts overflow", "none | Overflow [] the bound over the attempts does not fit 64 bits | entry=huge model=huge tokens=18000/0/18000 fees=0 invocations=0 attempts=2000 tools= ceiling=0 pinned=0 pin=none"},
    {"worst output overflow", "none | Overflow [] the output tokens do not fit 64 bits | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst unknown model", "none | ModelPrice [nobody/none] the catalogue has no price for the model at the origin | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst direct with ceiling", "none | RouteCeiling [vendor/routed] the entry lists a route ceiling but the origin is not a routing gateway, so it cannot be pinned | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"worst gateway", "3360500 Derived CeilingCatalogueProduct | entry=vendor/routed model=vendor/routed tokens=1000/100/1000 fees=0 invocations=1 attempts=1 tools= ceiling=1 pinned=0 pin=2400000000,9600000000,-,-,500,"},
    {"worst gateway limit", "12960500 Derived CeilingCatalogueProduct | entry=vendor/routed model=vendor/routed tokens=5000/100/5000 fees=0 invocations=1 attempts=1 tools= ceiling=1 pinned=0 pin=2400000000,9600000000,-,-,500,"},
    {"worst gateway pinned", "3360500 Derived CeilingCatalogueProduct | entry=vendor/routed model=vendor/routed tokens=1000/100/1000 fees=0 invocations=1 attempts=1 tools= ceiling=1 pinned=1 pin=2400,9600,-,-,1,"},
    {"worst gateway capped", "3360500 Derived CeilingCatalogueProduct | entry=vendor/routed model=vendor/routed tokens=1000/100/1000 fees=0 invocations=1 attempts=1 tools= ceiling=1 pinned=0 pin=1000,9600000000,5,-,500,"},
    {"worst gateway fallbacks", "21960500 Derived CeilingCatalogueProduct | entry=vendor/routed-b model=vendor/routed-b tokens=7000/100/7000 fees=0 invocations=1 attempts=1 tools= ceiling=1 pinned=0 pin=3000000000,9600000000,-,-,500,"},
    {"worst gateway unceiled", "none | RouteCeiling [vendor/unceiled] the entry lists no route ceiling (ceiling_input, ceiling_output), so the router may pick any price | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=1 pinned=0 pin=none"},
    {"worst gateway no fee", "3900000 Derived CeilingCatalogueProduct | entry=vendor/routed-b model=vendor/routed-b tokens=1000/100/1000 fees=0 invocations=1 attempts=1 tools= ceiling=1 pinned=0 pin=3000000000,9000000000,-,-,-,"},
    {"variable dearest 2000 cache", "-,-,-,-,-,-,-,-,-,-,-,-,-,"},
    {"variable dearest 500 cache", "3000000000,8000000000,4000000000,-,-,-,-,-,-,-,-,-,-,"},
    {"variable exact 2000 cache", "3000000000,8000000000,-,-,-,-,-,-,-,-,-,-,-,"},
    {"variable bound price 2000 cache", "none unpriced=[input,output]"},
    {"variable bound entry 2000 cache", "none unpriced=[cache_read]"},
    {"variable bound entry 500 cache", "2080000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"variable worst 2000 cache", "none | ModelPrice [variable-cache] catalogue entry 'variable-cache' prices cache_read at a variable price (cache_read+1000) | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"variable estimate 2000 reads cache", "none entry=variable-cache requested=0 assumed=[] unpriced=[cache_read] lower=0"},
    {"variable estimate 2000 cache", "6080000 Derived CeilingCatalogueProduct entry=variable-cache requested=0 assumed=[] unpriced=[] lower=0"},
    {"variable dearest 2000 fee", "-,-,-,-,-,-,-,-,-,-,-,-,-,"},
    {"variable dearest 500 fee", "3000000000,8000000000,-,-,-,1000000000000,-,-,-,-,-,-,-,"},
    {"variable exact 2000 fee", "3000000000,8000000000,-,-,-,-,-,-,-,-,-,-,-,"},
    {"variable bound price 2000 fee", "none unpriced=[input,output]"},
    {"variable bound entry 2000 fee", "none unpriced=[request]"},
    {"variable bound entry 500 fee", "2580000 Derived CeilingCatalogueProduct unpriced=[]"},
    {"variable worst 2000 fee", "none | ModelPrice [variable-fee] catalogue entry 'variable-fee' prices request at a variable price (request+1000) | entry= model= tokens=0/0/0 fees=0 invocations=0 attempts=1 tools= ceiling=0 pinned=0 pin=none"},
    {"variable estimate 2000 reads fee", "none entry=variable-fee requested=0 assumed=[cache_read at input] unpriced=[request] lower=0"},
    {"variable estimate 2000 fee", "none entry=variable-fee requested=0 assumed=[] unpriced=[request] lower=0"},
};
}  // namespace

int main() {
  try {
    estimates();
    bounds();
    worst_cases();
    variable_rows();
    std::size_t failed = 0;
    for (const auto& [name, actual] : answers) {
      const Pin* pin = nullptr;
      for (const auto& candidate : pins)
        if (candidate.name == name) pin = &candidate;
      if (pin && pin->expected == actual) continue;
      ++failed;
      std::cout << "{\"" << name << "\", \"" << actual << "\"},\n";
    }
    if (pins.size() != answers.size()) {
      std::cout << "the table records " << pins.size() << " answers for " << answers.size() << " cases\n";
      ++failed;
    }
    if (failed) throw std::runtime_error(std::to_string(failed) + " answers differ from the record");
    std::cout << "pricing pins passed (" << answers.size() << " answers)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pricing pins failed: " << error.what() << '\n';
    return 1;
  }
}
