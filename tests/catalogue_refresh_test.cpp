#include "core/pricing.h"
#include "descriptor/catalogue.h"
#include "descriptor/policy.h"
#include "sp/config_defaults.h"
#include "support/policy_edit.h"
#include "support/runtime_peer.h"
#include "transport/catalogue_refresh.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>
#ifndef _WIN32
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace sp;
using descriptor::Listing;
using runtime_test::Peer;
using runtime_test::require;

std::string base(const Peer& peer, std::string_view scenario) {
  return "http://127.0.0.1:" + std::to_string(peer.port) + "/" + std::string(scenario);
}
transport::ListingSource source(Listing listing, std::string url, std::string key = {}) {
  return {listing, std::move(url), std::move(key), {}, {}};
}
transport::RefreshResult refreshed(transport::Transport& transport, const transport::RefreshRequest& request) {
  auto result = transport::refresh_catalogue(transport, request);
  if (auto* error = std::get_if<descriptor::ConfigError>(&result)) throw std::runtime_error("refresh refused at " + error->pointer);
  return std::get<transport::RefreshResult>(std::move(result));
}
bool has(const std::vector<std::string>& values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}
// The requests the peer received for a scenario: how many, and the model ids whose endpoints listing was asked
// for, in order.
struct Seen {
  std::uint64_t total = 0;
  std::vector<std::string> endpoints;
  // The headers of each request, in the order received: (name, value) as sent, without `host`.
  std::vector<std::vector<std::pair<std::string, std::string>>> headers;
};
Seen seen(Peer& peer, std::string_view scenario) {
  const auto answer = peer.command("{\"requests\":" + json::quote(scenario) + "}");
  const auto root = answer.root();
  Seen out;
  out.total = root.get("total").as_uint();
  for (auto id : root.get("endpoints").elements()) out.endpoints.emplace_back(id.as_string());
  for (auto request : root.get("headers").elements()) {
    auto& sent = out.headers.emplace_back();
    for (auto header : request.elements()) sent.emplace_back(std::string(header.at(0).as_string()), std::string(header.at(1).as_string()));
  }
  return out;
}
void passed(std::string_view name) { std::cout << "case passed: " << name << std::endl; }

// What a refresh of the `openrouter` listing generated before a refresh could read route ceilings or price overrides,
// which is what it generates from that listing without its override (scenario `openrouter-bare`). A refresh that asks
// for no ceilings still generates exactly these bytes from it, and sends the listing request only.
constexpr std::string_view openrouter_generated_text =
    R"({"version":1,"models":[{"id":"vendor/image-model","origins":["https://openrouter.ai"],"source":"openrouter",)"
    R"("input_modalities":["text"],"output_modalities":["image"],"pricing":{"image":"0.04"}},{"id":"vendor/retiring",)"
    R"("families":["anthropic.messages","openai.chat","openai.responses"],"origins":["https://openrouter.ai"],)"
    R"("retires_at":"2026-11-01","source":"openrouter","output_modalities":["text"]},{"id":"vendor/text-model",)"
    R"("families":["anthropic.messages","openai.chat","openai.responses"],"origins":["https://openrouter.ai"],)"
    R"("served_as":["vendor/text-model-20261001"],"name":"Vendor: Text Model","source":"openrouter","context_window":128000,)"
    R"("output_limit":8192,"input_modalities":["image","text"],"output_modalities":["text"],"parameters":["temperature","tools"],)"
    R"("pricing":{"completion":"0.0000004","prompt":"0.0000001"}},{"id":"~vendor/text-latest",)"
    R"("families":["anthropic.messages","openai.chat","openai.responses"],"origins":["https://openrouter.ai"],)"
    R"("alias_of":"vendor/text-model","source":"openrouter","output_modalities":["text"]}]})";
constexpr std::string_view openrouter_generated_sha256 = "c84eb4466ae281d479ca1735b88fc9c0c31c2f9fc48e55a1109f58488164c406";
// The `openrouter` listing itself: the override of vendor/text-model names no condition and a prompt price above the
// base one, so it may apply to any request and the entry leaves `prompt` out. Nothing else changes.
constexpr std::string_view openrouter_override_text =
    R"({"version":1,"models":[{"id":"vendor/image-model","origins":["https://openrouter.ai"],"source":"openrouter",)"
    R"("input_modalities":["text"],"output_modalities":["image"],"pricing":{"image":"0.04"}},{"id":"vendor/retiring",)"
    R"("families":["anthropic.messages","openai.chat","openai.responses"],"origins":["https://openrouter.ai"],)"
    R"("retires_at":"2026-11-01","source":"openrouter","output_modalities":["text"]},{"id":"vendor/text-model",)"
    R"("families":["anthropic.messages","openai.chat","openai.responses"],"origins":["https://openrouter.ai"],)"
    R"("served_as":["vendor/text-model-20261001"],"name":"Vendor: Text Model","source":"openrouter","context_window":128000,)"
    R"("output_limit":8192,"input_modalities":["image","text"],"output_modalities":["text"],"parameters":["temperature","tools"],)"
    R"("pricing":{"completion":"0.0000004"}},{"id":"~vendor/text-latest",)"
    R"("families":["anthropic.messages","openai.chat","openai.responses"],"origins":["https://openrouter.ai"],)"
    R"("alias_of":"vendor/text-model","source":"openrouter","output_modalities":["text"]}]})";
constexpr std::string_view openrouter_override_sha256 = "03a2a2917ab168def107afc530c38a87374d2fe9b958f41f2c7c2bb750a27801";
void no_ceilings_change_nothing(transport::Transport& transport, Peer& peer) {
  const auto pinned = [&](std::string_view scenario, std::string_view expected_text, std::string_view expected_sha256) {
    transport::RefreshRequest request;
    request.sources = {source(Listing::OpenRouter, base(peer, scenario))};
    const auto result = refreshed(transport, request);
    require(result.sources.size() == 1 && result.sources[0].ok && result.sources[0].pages == 1, "the pinned listing refreshed");
    const auto text = result.generated->to_json();
    require(text == expected_text, "a refresh without ceilings generates the pinned text, not " + text);
    require(result.generated->digest_hex() == expected_sha256,
            "a refresh without ceilings generates the pinned digest, not " + result.generated->digest_hex());
    const auto requests = seen(peer, scenario);
    require(requests.total == 1 && requests.endpoints.empty(), "a refresh without ceilings sends the listing request only");
    require(result.sources[0].ceiling_requests == 0 && result.sources[0].ceilings == 0 && result.sources[0].ceiling_gaps.empty(),
            "a refresh without ceilings reports none");
  };
  pinned("openrouter-bare", openrouter_generated_text, openrouter_generated_sha256);
  pinned("openrouter-pinned", openrouter_override_text, openrouter_override_sha256);
  passed("no ceilings asked: the listing request only, and the pinned text and digest, with and without the override");
}

// The headers each listing request carries, byte for byte, with a key and without one, and those of OpenRouter's
// endpoints request (scenarios `capture-*`; `tag` keeps the requests of one policy apart from another's).
using Sent = std::vector<std::pair<std::string, std::string>>;
std::string sent_text(const Sent& sent) {
  std::string out;
  for (const auto& [name, value] : sent) out += (out.empty() ? "" : "; ") + name + ": " + value;
  return out;
}
// The headers of the requests a refresh of one source sent: the listing request and, for OpenRouter, the endpoints
// request that follows it.
std::vector<Sent> captured(transport::Transport& transport, Peer& peer, Listing listing, std::string_view tag, bool with_key,
                           const descriptor::PolicySnapshot& policy) {
  const std::string scenario = "capture-" + std::string(descriptor::listing_name(listing)) + (with_key ? "-key-" : "-nokey-") + std::string(tag);
  transport::RefreshRequest request;
  auto listed = source(listing, base(peer, scenario), with_key ? "pin-key" : "");
  if (listing == Listing::OpenRouter) listed.ceiling_models = {"vendor/text-model"};
  request.sources = {listed};
  request.policy = policy;
  (void)refreshed(transport, request);
  auto requests = seen(peer, scenario);
  require(requests.headers.size() == (listing == Listing::OpenRouter ? 2u : 1u), scenario + " sends the requests of its listing");
  return std::move(requests.headers);
}
void require_sent(const std::vector<Sent>& requests, const Sent& expected, std::string_view what) {
  for (const auto& sent : requests)
    require(sent == expected, std::string(what) + " sends [" + sent_text(expected) + "], not [" + sent_text(sent) + "]");
}
const Sent accept_only{{"accept", "application/json"}};
Sent with_accept(Sent more) {
  Sent out = accept_only;
  out.insert(out.end(), more.begin(), more.end());
  return out;
}
// What the built-in policy has each listing send with `pin-key`.
Sent builtin_listing_headers(Listing listing) {
  switch (listing) {
    case Listing::OpenRouter:
    case Listing::OpenAI: return with_accept({{"authorization", "Bearer pin-key"}});
    case Listing::Gemini: return with_accept({{"x-goog-api-key", "pin-key"}});
    case Listing::Anthropic: return with_accept({{"x-api-key", "pin-key"}, {"anthropic-version", "2023-06-01"}});
  }
  return {};
}
void pin_every_listing(transport::Transport& transport, Peer& peer, std::string_view tag, const descriptor::PolicySnapshot& policy) {
  for (const Listing listing : descriptor::all_listings) {
    const auto name = std::string(descriptor::listing_name(listing));
    require_sent(captured(transport, peer, listing, tag, true, policy), builtin_listing_headers(listing), name + " listing with a key");
    require_sent(captured(transport, peer, listing, tag, false, policy), accept_only, name + " listing without a key");
  }
}
void listing_headers_are_pinned(transport::Transport& transport, Peer& peer) {
  pin_every_listing(transport, peer, "builtin", nullptr);
  passed("listing headers: every listing with and without a key, and OpenRouter's endpoints request, byte for byte");
}

descriptor::PolicySnapshot policy_of(const std::string& text) {
  auto loaded = descriptor::load_policy(text, config_defaults::codec_defaults_json);
  if (auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("policy refused at " + error->pointer);
  return std::get<descriptor::PolicySnapshot>(std::move(loaded));
}
// The headers come from the policy: the family a listing belongs to, that family's scheme at the source's origin and
// its Messages version.
void listing_headers_follow_the_policy(transport::Transport& transport, Peer& peer) {
  // A policy without `listings` is the embedded one for every listing, and so are the requests.
  pin_every_listing(transport, peer, "unlisted", policy_of(policy_edit::edited({}, "listings", {})));
  // The OpenRouter listing is Messages-shaped when the policy says so: x-api-key and the version header, on both of its requests.
  const auto routed = policy_of(policy_edit::edited(
      {}, "listings",
      R"([{"listing":"openrouter","family":"anthropic.messages"},{"listing":"openai","family":"openai.chat"}])"));
  require_sent(captured(transport, peer, Listing::OpenRouter, "as-messages", true, routed),
               with_accept({{"x-api-key", "pin-key"}, {"anthropic-version", "2023-06-01"}}), "an OpenRouter listing of the Messages family");
  require_sent(captured(transport, peer, Listing::Gemini, "as-messages", true, routed), builtin_listing_headers(Listing::Gemini),
               "a listing the policy leaves out stays with the embedded family");
  // The scheme of the family's `auth` fact: OpenAI's listing sends what Chat sends, here x-goog-api-key.
  const auto chat_scheme = policy_of(policy_edit::edited("openai.chat", "auth", R"([{"scheme":"x-goog-api-key"}])"));
  require_sent(captured(transport, peer, Listing::OpenAI, "scheme", true, chat_scheme), with_accept({{"x-goog-api-key", "pin-key"}}),
               "an OpenAI listing under Chat's x-goog-api-key scheme");
  require_sent(captured(transport, peer, Listing::OpenRouter, "scheme", true, chat_scheme), with_accept({{"x-goog-api-key", "pin-key"}}),
               "an OpenRouter listing under Chat's x-goog-api-key scheme");
  require_sent(captured(transport, peer, Listing::Anthropic, "scheme", true, chat_scheme), builtin_listing_headers(Listing::Anthropic),
               "an Anthropic listing keeps Messages' scheme");
  // The scheme at the source's origin, not at the URL the listing is fetched from.
  const auto scoped = policy_of(policy_edit::edited(
      "openai.chat", "auth", R"([{"origins":["https://openrouter.ai"],"scheme":"x-api-key"},{"scheme":"bearer"}])"));
  require_sent(captured(transport, peer, Listing::OpenRouter, "origin", true, scoped), with_accept({{"x-api-key", "pin-key"}}),
               "an OpenRouter listing at the origin the fact names");
  require_sent(captured(transport, peer, Listing::OpenAI, "origin", true, scoped), builtin_listing_headers(Listing::OpenAI),
               "an OpenAI listing at another origin");
  // The Messages version is the family's.
  const auto versions = policy_edit::edited("anthropic.messages", "header_versions", R"(["2023-06-01","2099-01-01"])");
  const auto newer = policy_of(policy_edit::edited(versions, "anthropic.messages", "header_version", R"("2099-01-01")"));
  require_sent(captured(transport, peer, Listing::Anthropic, "version", true, newer),
               with_accept({{"x-api-key", "pin-key"}, {"anthropic-version", "2099-01-01"}}), "an Anthropic listing under the policy's Messages version");
  // A key-less listing sends no version.
  require_sent(captured(transport, peer, Listing::Anthropic, "version", false, newer), accept_only, "an Anthropic listing without a key");
  passed("listing headers: the family, the scheme, the origin and the Messages version come from the policy");
}

// An entry's prices as "unit=price ...", in the entry's order.
std::string pricing_text(const descriptor::CatalogueEntry* entry) {
  if (!entry) return "(no entry)";
  std::string out;
  for (const auto& [unit, price] : entry->pricing) out += (out.empty() ? "" : " ") + unit + "=" + price;
  return out;
}
Usage usage_of(std::uint64_t input, std::uint64_t output) {
  Usage usage;
  usage.input_total = Count{input, Evidence::Reported};
  usage.output_total = Count{output, Evidence::Reported};
  return usage;
}

// The long-context overrides of OpenRouter's models listing become input bands. An override the price grammar cannot
// hold changes nothing when each price it lists is at most the entry's price for that key under every condition, and
// otherwise leaves the unit out (scenario `overrides`, one case per model).
void overrides_become_bands(transport::Transport& transport, Peer& peer) {
  transport::RefreshRequest request;
  request.sources = {source(Listing::OpenRouter, base(peer, "overrides"))};
  const auto result = refreshed(transport, request);
  const auto& report = result.sources[0];
  require(report.ok && report.models == 32 && report.skipped_prices == 0, "the overrides listing refreshed");
  const auto& c = *result.generated;
  // 62 bands fill an entry beside its two base prices.
  std::vector<std::pair<std::string, std::string>> full{{"completion", "0.000002"}, {"prompt", "0.000001"}};
  for (int i = 1; i <= 62; ++i) full.emplace_back("prompt+" + std::to_string(i * 1000), "0.000002");
  std::sort(full.begin(), full.end());
  std::string full_bands;
  for (const auto& [unit, price] : full) full_bands += (full_bands.empty() ? "" : " ") + unit + "=" + price;
  const std::vector<std::pair<std::string, std::string>> expected{
      // The example: each price of the override is the band of its key at the threshold.
      {"vendor/long", "completion=0.00004 completion+272000=0.00006 input_cache_read+272000=0.0000008 input_cache_write+272000=0.00002 "
                      "prompt=0.000008 prompt+272000=0.000016"},
      {"vendor/two-bands", "completion=0.000002 completion+100000=0.000003 completion+200000=0.000004 prompt=0.000001 "
                           "prompt+100000=0.000002 prompt+200000=0.000003"},
      // A key the override leaves out has no band: it keeps the base price.
      {"vendor/some-keys", "completion=0.000002 input_cache_read=0.0000001 prompt=0.000001 prompt+1000=0.000002"},
      {"vendor/hour-cache", "completion=0.000015 input_cache_write=0.00000375 input_cache_write_1h=0.000006 "
                            "input_cache_write_1h+200000=0.000012 prompt=0.000003 prompt+200000=0.000006"},
      // A band below the base price is a band; but where a later override leaves its key out, which may be read as
      // billing the base price again, it is not, and the key keeps its base price.
      {"vendor/cheap-band", "completion=0.000002 prompt=0.000001 prompt+1000=0.0000005"},
      {"vendor/cheap-carried", "completion=0.000002 completion+2000=0.000003 prompt=0.000001"},
      // A time window alone: each key it lists at the dearest of the base price and the windows' prices.
      {"vendor/window-cheap", "completion=0.000008 prompt=0.000002"},
      {"vendor/window-dear", "completion=0.000008 prompt=0.000003"},
      {"vendor/window-cache", "completion=0.000008 input_cache_read=0.000005 prompt=0.000002"},
      {"vendor/window-fee", "completion=0.000002 image=0.01 prompt=0.000001 request=0.002"},
      // A cache read the base does not price is billed at the prompt price outside the window, so it is left out, and
      // the prompt price with it.
      {"vendor/window-new-key", "completion=0.000002"},
      // A key with a band and a window is left out; the window's other key is held.
      {"vendor/band-and-window", "completion=0.000004"},
      // Windows the clock cannot read stay overrides the catalogue cannot hold. A cache read the entry leaves out would
      // be priced at the prompt price, and a request fee at nothing, so those go too.
      {"vendor/open-window", "completion=0.000008"},
      {"vendor/late-window-cache", "completion=0.000008"},
      {"vendor/minute-window-fee", "image=0.01"},
      {"vendor/text-window", "completion=0.000002"},
      // Other conditions, alone or with a threshold, and none at all.
      {"vendor/unknown-condition", "completion=0.000002"},
      {"vendor/combined", "prompt=0.000001"},
      {"vendor/no-condition", "completion=0.000002"},
      // Thresholds of one key out of listing order; the other key's single band is held.
      {"vendor/out-of-order", "completion=0.000002 completion+100000=0.000003"},
      // Thresholds that are not a count of input tokens above zero, a price that is not decimal text (the cheaper one
      // beside it changes nothing), a price written as a number, and a discount.
      {"vendor/fraction-threshold", "completion=0.000002"},
      {"vendor/zero-threshold", "completion=0.000002"},
      {"vendor/text-threshold", "completion=0.000002"},
      {"vendor/bad-price", "completion=0.000002"},
      {"vendor/number-price", "completion=0.000002"},
      {"vendor/discount", "completion=0.000002"},
      // A discount that may raise every price, and an override that is not an object, leave every unit out.
      {"vendor/raising-discount", ""},
      {"vendor/malformed", ""},
      {"vendor/full-bands", full_bands},
      {"vendor/too-many-bands", "completion=0.000002"},
      {"vendor/too-many", "completion=0.000002"},
      {"vendor/null-overrides", "completion=0.000002 prompt=0.000001"}};
  std::string wrong;
  for (const auto& [id, text] : expected)
    if (pricing_text(c.entry(id)) != text) wrong += "\n  " + id + ": " + pricing_text(c.entry(id)) + "\n    expected: " + text;
  if (!wrong.empty()) throw std::runtime_error("the generated prices are not the expected ones:" + wrong);
  // Every override price not held as a band is counted once, and every member left out is named, by model.
  require(report.skipped_override_prices == 156, "override prices skipped or left out: " + std::to_string(report.skipped_override_prices));
  require(report.window_prices_at_dearest == 7, "time-window prices held: " + std::to_string(report.window_prices_at_dearest));
  const std::vector<std::string> left_out{
      "vendor/bad-price|prompt", "vendor/band-and-window|prompt", "vendor/combined|completion", "vendor/discount|prompt",
      "vendor/fraction-threshold|prompt", "vendor/late-window-cache|input_cache_read", "vendor/late-window-cache|prompt",
      "vendor/malformed|", "vendor/minute-window-fee|completion", "vendor/minute-window-fee|prompt", "vendor/minute-window-fee|request",
      "vendor/no-condition|prompt", "vendor/number-price|prompt", "vendor/open-window|prompt", "vendor/out-of-order|prompt",
      "vendor/raising-discount|", "vendor/text-threshold|prompt", "vendor/text-window|prompt", "vendor/too-many|prompt",
      "vendor/too-many-bands|prompt", "vendor/unknown-condition|prompt", "vendor/window-new-key|input_cache_read",
      "vendor/window-new-key|prompt", "vendor/zero-threshold|prompt"};
  std::vector<std::string> named;
  for (const auto& unit : report.withheld) named.push_back(unit.model + "|" + unit.unit);
  if (named != left_out) {
    std::string all;
    for (const auto& line : named) all += "\n  " + line;
    throw std::runtime_error("the members left out are not the expected ones:" + all);
  }

  // Money from the generated entries. vendor/long: at the threshold the base prices, one token above it the band's.
  const auto& long_context = *c.entry("vendor/long");
  const auto at = pricing::price_of(long_context, {}, 272'000), above = pricing::price_of(long_context, {}, 272'001);
  require(at.input == 8'000'000'000u && at.output == 40'000'000'000u && !at.cache_read && !at.cache_write, "at the threshold: the base prices");
  require(above.input == 16'000'000'000u && above.output == 60'000'000'000u && above.cache_read == 800'000'000u &&
          above.cache_write == 20'000'000'000u, "one token above the threshold: the band's prices");
  const ServedModel long_model{"vendor/long", "vendor/long", ServedRelation::Exact};
  const auto cost = [&](const ServedModel& model, const Usage& usage) {
    const auto e = pricing::estimate(c, "openai.chat", "https://openrouter.ai", model, usage);
    return e.total ? e.total->nano_usd : 0;
  };
  // 100'000, 272'000 and 272'001 input tokens and 1'000 output tokens, in nano-USD: 100'000 x 8'000 + 1'000 x 40'000,
  // 272'000 x 8'000 + 1'000 x 40'000, and 272'001 x 16'000 + 1'000 x 60'000.
  require(cost(long_model, usage_of(100'000, 1'000)) == 840'000'000u, "below the threshold: the base prices");
  require(cost(long_model, usage_of(272'000, 1'000)) == 2'216'000'000u, "exactly at the threshold: the base prices");
  require(cost(long_model, usage_of(272'001, 1'000)) == 4'412'016'000u, "one token above the threshold: the band's prices");
  // The bound: up to 272'000 input tokens the base rates; one more and the band's dearest input rate (its cache write,
  // 2e-5 USD) and output rate.
  const auto bound_at = pricing::upper_bound(long_context, {}, {272'000, 1'000, false, {}});
  const auto bound_above = pricing::upper_bound(long_context, {}, {272'001, 1'000, false, {}});
  require(bound_at.total && bound_at.total->nano_usd == 2'216'000'000u, "the bound at the threshold");
  require(bound_above.total && bound_above.total->nano_usd == 5'500'020'000u, "the bound one token above the threshold");
  // The highest band the call is above wins.
  const auto& two = *c.entry("vendor/two-bands");
  require(pricing::price_of(two, {}, 150'000).input == 2'000'000'000u && pricing::price_of(two, {}, 200'000).input == 2'000'000'000u &&
          pricing::price_of(two, {}, 200'001).input == 3'000'000'000u && pricing::price_of(two, {}, 200'001).output == 4'000'000'000u,
          "the highest band wins");
  // A key the override leaves out keeps its base price above the threshold.
  const auto some = pricing::price_of(*c.entry("vendor/some-keys"), {}, 5'000);
  require(some.input == 2'000'000'000u && some.output == 2'000'000'000u && some.cache_read == 100'000'000u, "a key the override leaves out stays at the base");
  // A one-hour cache write is priced at OpenRouter's input_cache_write_1h: 700 uncached at 3e-6, 100 five-minute writes
  // at 3.75e-6, 200 one-hour writes at 6e-6 and 10 output tokens at 1.5e-5 USD.
  const auto hour = pricing::price_of(*c.entry("vendor/hour-cache"));
  require(hour.cache_write == 3'750'000'000u && hour.cache_write_1h == 6'000'000'000u &&
          pricing::price_of(*c.entry("vendor/hour-cache"), {}, 200'001).cache_write_1h == 12'000'000'000u, "the one-hour cache write price is read");
  auto split = usage_of(1'000, 10);
  split.cache_write = Count{300, Evidence::Reported};
  split.extra["cache_creation.ephemeral_5m_input_tokens"] = Count{100, Evidence::Reported};
  split.extra["cache_creation.ephemeral_1h_input_tokens"] = Count{200, Evidence::Reported};
  const auto hour_cost = pricing::estimate(c, "anthropic.messages", "https://openrouter.ai",
                                           ServedModel{"vendor/hour-cache", "vendor/hour-cache", ServedRelation::Exact}, split);
  require(hour_cost.total && hour_cost.total->nano_usd == 3'825'000u && hour_cost.assumptions.empty(), "one-hour cache writes at the one-hour price");
  // A unit left out is unpriced: the bound and the estimate refuse a call that bills it.
  const auto& open = *c.entry("vendor/open-window");
  const auto open_bound = pricing::upper_bound(open, {}, {1'000, 100, false, {}});
  require(!open_bound.total && open_bound.unpriced == std::vector<std::string>{"input"}, "the bound reports the unit left out as unpriced");
  const auto open_cost = pricing::estimate(c, "openai.chat", "https://openrouter.ai",
                                           ServedModel{"vendor/open-window", "vendor/open-window", ServedRelation::Exact}, usage_of(1'000, 100));
  require(!open_cost.total && open_cost.unpriced == std::vector<std::string>{"input"}, "the estimate reports the unit left out as not priced");
  require(pricing::price_of(open).output == 8'000'000'000u, "the other units are kept");
  auto reads = usage_of(1'000, 0);
  reads.cache_read = Count{1'000, Evidence::Reported};
  const auto read_cost = pricing::estimate(c, "openai.chat", "https://openrouter.ai",
                                           ServedModel{"vendor/late-window-cache", "vendor/late-window-cache", ServedRelation::Exact}, reads);
  require(!read_cost.total && read_cost.unpriced == std::vector<std::string>{"cache_read"}, "cache reads are not priced at the prompt price");
  require(pricing::upper_bound(*c.entry("vendor/minute-window-fee"), {}, {1, 1, false, {}}).unpriced == (std::vector<std::string>{"input", "output"}),
          "a request fee left out leaves the call unpriced");
  // A time window is held at its dearest price at every length and every hour: 1'000 input tokens at 3e-6 and 100
  // output tokens at 8e-6 USD, and the dearer request fee.
  require(pricing::price_of(*c.entry("vendor/window-cheap")).input == 2'000'000'000u, "a cheaper time window keeps the base price");
  const auto dear_cost = pricing::estimate(c, "openai.chat", "https://openrouter.ai",
                                           ServedModel{"vendor/window-dear", "vendor/window-dear", ServedRelation::Exact}, usage_of(1'000, 100));
  require(dear_cost.total && dear_cost.total->nano_usd == 3'800'000u, "a dearer time window is priced at its price");
  require(pricing::price_of(*c.entry("vendor/window-fee")).request == 2'000'000'000'000u, "a dearer request fee in a window is the fee");
  passed("price overrides: long-context bands, time windows, malformed and unbounded overrides, and their prices");
}

// A price key that is not a unit name a listing gives (a tier or a band of the grammar, a route ceiling, a tool fee,
// text that is no unit token) is skipped and counted, in the base prices and in overrides, and its text reaches no
// report.
void listing_keys_are_unit_names(transport::Transport& transport, Peer& peer) {
  transport::RefreshRequest request;
  request.sources = {source(Listing::OpenRouter, base(peer, "overrides-keys"))};
  const auto result = refreshed(transport, request);
  const auto& report = result.sources[0];
  const auto& c = *result.generated;
  for (const auto& unit : report.withheld)
    for (const std::string_view banned : {"secret", "example.test", "https", "@"})
      require(unit.model.find(banned) == std::string::npos && unit.unit.find(banned) == std::string::npos,
              "a report holds no listed key text, not " + std::string(banned));
  require(report.ok && report.skipped_prices == 8 && report.skipped_override_prices == 1,
          "keys that are not unit names are skipped prices: " + std::to_string(report.skipped_prices));
  require(pricing_text(c.entry("vendor/odd-keys")) == "completion=0.000002", "only unit names are prices: " + pricing_text(c.entry("vendor/odd-keys")));
  // A listed band is skipped, and the override at that threshold gives its own band.
  require(pricing_text(c.entry("vendor/listed-band")) == "completion=10 prompt=10 prompt+128000=8",
          "the override's band, not the listed one: " + pricing_text(c.entry("vendor/listed-band")));
  require(report.withheld == (std::vector<descriptor::WithheldUnit>{{"vendor/odd-keys", "prompt"}}), "only a unit name is named as left out");
  for (const auto& unit : report.withheld)
    for (const std::string_view banned : {"secret", "example.test", "https", "@"})
      require(unit.model.find(banned) == std::string::npos && unit.unit.find(banned) == std::string::npos, "a report holds no listed key text");
  passed("listing keys: tiers, bands, route ceilings, tool fees and other text are skipped, and no report names them");
}

// The listing's top-level prices are those of the time window current when it is fetched. A window's price is held at
// the dearest of the windows, so the same listing gives the same entries at an hour of either window.
void windows_give_one_entry_at_every_hour(transport::Transport& transport, Peer& peer) {
  const auto at = [&](std::string_view scenario) {
    transport::RefreshRequest request;
    request.sources = {source(Listing::OpenRouter, base(peer, scenario))};
    auto result = refreshed(transport, request);
    require(result.sources[0].ok, "the listing refreshed");
    return result;
  };
  const auto a = at("windows-a"), b = at("windows-b");
  const auto text = a.generated->to_json();
  require(text == b.generated->to_json() && a.generated->digest() == b.generated->digest(),
          "an hour of each window gives the same entries, not\n" + text + "\nand\n" + b.generated->to_json());
  require(pricing_text(a.generated->entry("vendor/two-windows")) == "completion=0.000008 input_cache_read=0.0000002 prompt=0.000002",
          "each key at the dearest window, in the window's text: " + pricing_text(a.generated->entry("vendor/two-windows")));
  // A reasoning price one window leaves out is billed at the completion price in that window, so it is left out, and
  // the completion price with it, at either hour.
  require(pricing_text(a.generated->entry("vendor/two-windows-part")) == "input_cache_read=0.0000002 prompt=0.000002",
          "a refining price only one window lists is left out: " + pricing_text(a.generated->entry("vendor/two-windows-part")));
  for (const auto* result : {&a, &b}) {
    const auto& report = result->sources[0];
    std::vector<std::string> named;
    for (const auto& unit : report.withheld) named.push_back(unit.model + "|" + unit.unit);
    require(report.skipped_override_prices == 1 && report.window_prices_at_dearest == 12 &&
                named == (std::vector<std::string>{"vendor/two-windows-part|completion", "vendor/two-windows-part|internal_reasoning"}),
            "the same report at either hour");
  }
  // 1'000 input and 100 output tokens at 2e-6 and 8e-6 USD at any hour.
  const auto cost = pricing::estimate(*b.generated, "openai.chat", "https://openrouter.ai",
                                      ServedModel{"vendor/two-windows", "vendor/two-windows", ServedRelation::Exact}, usage_of(1'000, 100));
  require(cost.total && cost.total->nano_usd == 2'800'000u, "priced at the dearest window");
  passed("time windows: the same entries at an hour of either window");
}

void all_four_listings(transport::Transport& transport, Peer& peer, descriptor::CatalogueSnapshot& generated) {
  transport::RefreshRequest request;
  request.sources = {source(Listing::OpenRouter, base(peer, "openrouter")),
                     source(Listing::Gemini, base(peer, "gemini"), "gemini-test-key"),
                     source(Listing::Anthropic, base(peer, "anthropic"), "anthropic-test-key"),
                     source(Listing::OpenAI, base(peer, "openai"), "openai-test-key")};
  request.overlays = {descriptor::builtin_catalogue()};
  const auto result = refreshed(transport, request);
  require(result.sources.size() == 4, "one report per source");
  const std::size_t pages[]{1, 2, 2, 1}, models[]{4, 2, 2, 2};
  for (std::size_t i = 0; i < 4; ++i) {
    const auto& report = result.sources[i];
    require(report.ok && report.error.empty() && report.http_status == 200, "every source listed");
    require(report.pages == pages[i] && report.models == models[i], "pages followed and models counted");
  }
  require(result.sources[0].skipped == 1 && result.sources[1].skipped == 1, "ids that are not model names are skipped");
  require(result.sources[0].skipped_prices == 1 && result.sources[1].skipped_prices == 0, "a price written 1e-2 is counted, a structured override is not");
  require(result.sources[0].skipped_override_prices == 1 && result.sources[0].withheld.size() == 1 &&
          result.sources[0].withheld[0] == descriptor::WithheldUnit{"vendor/text-model", "prompt"},
          "the override's price is counted apart, and the prompt price it may raise is named as left out");
  require(result.sources[0].origin == "https://openrouter.ai" && result.sources[2].origin == "https://api.anthropic.com",
          "entries are scoped to the provider origin, not the listing URL");

  const auto& c = *result.generated;
  require(c.entries().size() == 10, "every listed model is generated");
  const auto* text = c.entry("vendor/text-model");
  require(text && text->source == std::optional<std::string>("openrouter") && text->origins == std::vector<std::string>{"https://openrouter.ai"},
          "OpenRouter entry source and scope");
  require(text->output_limit == 8192u && text->context_window == 128000u && text->served_as == std::vector<std::string>{"vendor/text-model-20261001"},
          "OpenRouter limits and canonical slug");
  require(has(text->families, "openai.chat") && has(text->families, "anthropic.messages") && has(text->input_modalities, "image"),
          "OpenRouter text model families and modalities");
  require(text->pricing == (std::vector<std::pair<std::string, std::string>>{{"completion", "0.0000004"}}),
          "string prices kept, and the prompt price an override without a condition may raise left out");
  require(text->parameters == std::vector<std::string>{"temperature", "tools"}, "a parameter the catalogue cannot hold is left out");
  require(c.entry("vendor/image-model")->families.empty(), "an image-only model lists no chat family");
  require(c.entry("~vendor/text-latest")->alias_of == std::optional<std::string>("vendor/text-model"), "alias target");
  require(c.entry("vendor/retiring")->retires_at == std::optional<std::string>("2026-11-01"), "expiration date");
  const auto* flash = c.entry("gemini-test-flash");
  require(flash && flash->input_limit == 1048576u && flash->output_limit == 65536u && has(flash->families, "google.interactions"),
          "Gemini entry without the models/ prefix");
  require(c.entry("veo-test") && c.entry("veo-test")->families.empty() && has(c.entry("veo-test")->methods, "predictLongRunning"),
          "a Gemini model without generateContent lists no chat family");
  const auto* claude = c.entry("claude-test-a");
  require(claude && claude->input_limit == 200000u && claude->input_modalities == std::vector<std::string>{"image", "text"},
          "Anthropic limits and capabilities");
  require(c.entry("claude-test-b")->retires_at == std::optional<std::string>("2026-12-01T00:00:00Z") &&
          c.entry("claude-test-b")->lifecycle == std::optional<std::string>("deprecated"), "Anthropic lifecycle");
  require(c.entry("tts-test")->retires_at == std::optional<std::string>("2027-01-06") && c.entry("gpt-test-1"), "OpenAI ids");

  require(result.diff.added.size() == 10 && result.diff.removed.empty() && result.diff.changed.empty(), "first refresh adds everything");
  require(result.catalogue->entry("gpt-6-luna") && !result.generated->entry("gpt-6-luna"), "overlays apply to the catalogue only");

  // The result attaches to a policy like any catalogue.
  auto policy = descriptor::builtin_policy()->with_catalogue(result.catalogue);
  require(std::holds_alternative<descriptor::PolicySnapshot>(policy), "refreshed catalogue attaches");
  const auto& snapshot = std::get<descriptor::PolicySnapshot>(policy);
  require(snapshot->output_limit("anthropic.messages", "https://api.anthropic.com", "claude-test-a") == 64000u, "listed limit applies");
  require(!snapshot->output_limit("anthropic.messages", "https://proxy.example.test", "claude-test-a"), "only at the listed origin");
  require(snapshot->identity() == descriptor::builtin_policy()->identity(), "a refresh keeps the replay identity");
  generated = result.generated;
}

void failures_keep_previous_entries(transport::Transport& transport, Peer& peer, const descriptor::CatalogueSnapshot& previous) {
  transport::RefreshRequest request;
  request.previous = previous;
  request.sources = {source(Listing::OpenRouter, base(peer, "openrouter-v2")),
                     source(Listing::Gemini, base(peer, "broken"), "gemini-test-key"),
                     source(Listing::Anthropic, base(peer, "notjson"), "anthropic-test-key"),
                     source(Listing::OpenAI, base(peer, "openai"), "wrong-openai-key")};
  const auto result = refreshed(transport, request);
  require(result.sources[0].ok, "OpenRouter listed");
  require(!result.sources[1].ok && result.sources[1].error == "listing answered HTTP 500" && result.sources[1].http_status == 500, "outage reported");
  require(!result.sources[2].ok && result.sources[2].error == "listing page is not a model listing", "a body that is not a listing");
  require(!result.sources[3].ok && result.sources[3].error == "listing answered HTTP 401", "a refused credential");
  for (const auto& report : result.sources)
    require(report.error.find("key") == std::string::npos && report.error.find("127.0.0.1") == std::string::npos, "reports carry fixed text");
  const auto& c = *result.generated;
  require(c.entry("gemini-test-flash") && c.entry("claude-test-b") && c.entry("gpt-test-1"), "failed sources keep their previous entries");
  require(!c.entry("vendor/retiring"), "a model the listing dropped is removed");
  require(c.entry("vendor/text-model")->output_limit == 16384u, "a changed fact is updated");
  require(result.diff.removed == std::vector<std::string>{"vendor/retiring"} && result.diff.changed == std::vector<std::string>{"vendor/text-model"} &&
          result.diff.added.empty(), "diff against the previous catalogue");

  // A source that never ends its pages, and a refresh of one source that keeps the others.
  transport::RefreshRequest loop;
  loop.previous = previous;
  loop.sources = {source(Listing::Gemini, base(peer, "gemini-loop"), "gemini-test-key")};
  const auto looped = refreshed(transport, loop);
  require(!looped.sources[0].ok && looped.sources[0].error == "listing repeated its page token", "page loop stopped");
  require(looped.generated->digest() == previous->digest() && looped.diff.empty(), "nothing changes when the only source fails");
}

// A listing that names no models, or whose page token cannot continue it, is not a listing: its source fails
// and its entries stay.
void empty_and_partial_listings_fail_their_source(transport::Transport& transport, Peer& peer, const descriptor::CatalogueSnapshot& previous) {
  transport::RefreshRequest request;
  request.previous = previous;
  request.sources = {source(Listing::OpenAI, base(peer, "openai-empty"), "openai-test-key"),
                     source(Listing::Gemini, base(peer, "gemini-empty"), "gemini-test-key"),
                     source(Listing::Anthropic, base(peer, "anthropic-empty"), "anthropic-test-key")};
  const auto empty = refreshed(transport, request);
  for (const auto& report : empty.sources)
    require(!report.ok && report.http_status == 200 && report.models == 0 && report.error == "listing named no models", "an empty listing fails its source");
  require(empty.generated->digest() == previous->digest() && empty.diff.empty(), "an empty listing changes nothing");

  // The host can take the empty listing as the truth.
  request.allow_drop = true;
  const auto dropped = refreshed(transport, request);
  for (const auto& report : dropped.sources) require(report.ok && report.models == 0, "an empty listing is taken when the host allows it");
  require(dropped.diff.removed.size() == 6 && dropped.generated->entries().size() == 4, "the sources that listed nothing lose their entries");

  for (const char* scenario : {"gemini-token-empty", "gemini-token-space", "gemini-token-long"}) {
    transport::RefreshRequest tokens;
    tokens.previous = previous;
    tokens.sources = {source(Listing::Gemini, base(peer, scenario), "gemini-test-key")};
    const auto partial = refreshed(transport, tokens);
    require(!partial.sources[0].ok && partial.sources[0].error == "listing page is not a model listing", "a page token that cannot continue the listing fails it");
    require(partial.generated->digest() == previous->digest() && partial.diff.empty(), "a partial listing changes nothing");
  }
}

void shrunk_listings_fail_their_source(transport::Transport& transport, Peer& peer) {
  auto listing = [&](const char* scenario, const descriptor::CatalogueSnapshot& previous, bool allow_drop, std::uint32_t percent = 50) {
    transport::RefreshRequest request;
    request.previous = previous;
    request.allow_drop = allow_drop;
    request.min_kept_percent = percent;
    request.sources = {source(Listing::OpenAI, base(peer, scenario), "openai-test-key")};
    return refreshed(transport, request);
  };
  const auto large = listing("openai-large", nullptr, false);
  require(large.sources[0].ok && large.sources[0].models == 6, "a first listing has nothing to shrink from");
  require(transport::RefreshRequest{}.min_kept_percent == 50, "half of the models is the default share");
  const auto half = listing("openai-half", large.generated, false);
  require(half.sources[0].ok && half.generated->entries().size() == 3, "half of the models is accepted");
  const auto small = listing("openai-small", large.generated, false);
  require(!small.sources[0].ok && small.sources[0].models == 2 && small.sources[0].error == "listing named far fewer models than before",
          "far fewer models fail the source");
  require(small.generated->digest() == large.generated->digest() && small.diff.empty(), "the previous entries stay");
  const auto accepted = listing("openai-small", large.generated, true);
  require(accepted.sources[0].ok && accepted.generated->entries().size() == 2 && accepted.diff.removed.size() == 4, "the host can allow the drop");

  // The share is the host's to set: a third of the models passes at 30 percent and fails at 34, and half fails at 60.
  require(listing("openai-small", large.generated, false, 30).sources[0].ok, "two of six pass a share of 30 percent");
  require(!listing("openai-small", large.generated, false, 34).sources[0].ok, "two of six fail a share of 34 percent");
  require(!listing("openai-half", large.generated, false, 60).sources[0].ok, "three of six fail a share of 60 percent");
  require(listing("openai-small", large.generated, false, 0).sources[0].ok, "a share of zero turns the test off");
  const auto none = listing("openai-empty", large.generated, false, 0);
  require(!none.sources[0].ok && none.sources[0].error == "listing named no models", "a share of zero still refuses an empty listing");
}

// A listing that does not fit a catalogue fails its source; the other sources are listed.
void oversized_listings_fail_their_source(transport::Transport& transport, Peer& peer) {
  transport::RefreshRequest request;
  request.sources = {source(Listing::OpenAI, base(peer, "openai-huge"), "openai-test-key"),
                     source(Listing::OpenRouter, base(peer, "openrouter"))};
  const auto huge = refreshed(transport, request);
  require(!huge.sources[0].ok && huge.sources[0].error == "listing exceeds the catalogue limits", "a source over 16384 entries fails");
  require(huge.sources[1].ok && huge.generated->entry("vendor/text-model"), "the other sources are listed");

  // Each source fits; the two together do not. The source that does not fit fails, in order.
  request.sources = {{Listing::OpenAI, base(peer, "openai-wide-a"), "openai-test-key", "https://a.example.test", {}},
                     {Listing::OpenAI, base(peer, "openai-wide-b"), "openai-test-key", "https://b.example.test", {}},
                     source(Listing::OpenRouter, base(peer, "openrouter"))};
  const auto wide = refreshed(transport, request);
  require(wide.sources[0].ok && wide.sources[0].models == 10000, "the first wide source fits");
  require(!wide.sources[1].ok && wide.sources[1].models == 10000 && wide.sources[1].error == "listing exceeds the catalogue limits", "the one that does not fit fails");
  require(wide.sources[2].ok && wide.generated->entry("a-0") && !wide.generated->entry("b-0") && wide.generated->entry("vendor/text-model"), "the catalogue keeps what fits");
}

void a_deadline_bounds_the_whole_refresh(transport::Transport& transport, Peer& peer) {
  transport::RefreshRequest request;
  request.sources = {source(Listing::OpenAI, base(peer, "openai-stall"), "openai-test-key"),
                     source(Listing::OpenAI, base(peer, "openai-stall"), "openai-test-key")};
  request.total_timeout = std::chrono::milliseconds(300);
  const auto began = std::chrono::steady_clock::now();
  const auto result = refreshed(transport, request);
  require(std::chrono::steady_clock::now() - began < std::chrono::seconds(2), "the refresh stops at its deadline, not the page timeout");
  for (const auto& report : result.sources) require(!report.ok && report.error == "refresh deadline passed", "a source the deadline stopped fails");
  require(result.sources[1].pages == 0, "a source after the deadline sends nothing");
}

// An id two sources of different kinds list is owned by both: when both stop listing it, it goes.
void shared_ids_leave_with_their_sources(transport::Transport& transport, Peer& peer) {
  const auto sources = [&](const char* openai, const char* gemini) {
    return std::vector<transport::ListingSource>{{Listing::OpenAI, base(peer, openai), "openai-test-key", "https://gw-a.example.test", {}},
                                                 {Listing::Gemini, base(peer, gemini), "gemini-test-key", "https://gw-b.example.test", {}}};
  };
  transport::RefreshRequest request;
  request.sources = sources("openai-shared", "gemini-shared");
  const auto first = refreshed(transport, request);
  const auto* shared = first.generated->entry("shared-model");
  require(shared && shared->origins == (std::vector<std::string>{"https://gw-a.example.test", "https://gw-b.example.test"}) &&
          shared->source == std::optional<std::string>("gemini+openai"), "an id two sources list keeps both origins and both sources");
  require(first.generated->entry("solo-openai")->source == std::optional<std::string>("openai"), "an id one source lists keeps its source");

  // One source failing keeps the entry: the other origin may still list it.
  request.previous = first.generated;
  request.sources = sources("openai-shared-2", "broken");
  const auto outage = refreshed(transport, request);
  require(!outage.sources[1].ok && outage.generated->entry("shared-model"), "an entry stays while one of its sources failed");

  request.sources = sources("openai-shared-2", "gemini-shared-2");
  const auto gone = refreshed(transport, request);
  require(gone.sources[0].ok && gone.sources[1].ok, "both sources listed");
  require(!gone.generated->entry("shared-model") && gone.generated->entry("solo-openai") && gone.generated->entry("gemini-other"), "an id neither source lists any more is removed");
  require(gone.diff.removed == std::vector<std::string>{"shared-model"} && gone.diff.added == std::vector<std::string>{"gemini-other"}, "the diff names the removal");
}

#ifndef _WIN32
// sp_catalogue_refresh writes its files whole or not at all, and not when a source failed.
int run_tool(const std::string& tool, std::vector<std::string> arguments, const std::string& output = "/dev/null") {
  std::vector<std::string> words{tool};
  words.insert(words.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& word : words) argv.push_back(word.data());
  argv.push_back(nullptr);
  // A child would write the parent's unflushed output again.
  std::cout.flush();
  std::fflush(stdout);
  const auto pid = fork();
  require(pid >= 0, "fork failed");
  if (pid == 0) {
    ::freopen(output.c_str(), "w", stdout);
    execv(tool.c_str(), argv.data());
    _exit(127);
  }
  int status = 0;
  require(waitpid(pid, &status, 0) == pid && WIFEXITED(status), "the tool did not exit");
  return WEXITSTATUS(status);
}
std::string contents(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

void the_tool_keeps_its_files_when_a_source_fails(Peer& peer, const std::string& tool) {
  setenv("OPENAI_API_KEY", "openai-test-key", 1);
  const auto dir = std::filesystem::temp_directory_path() / ("sp_catalogue_refresh_tool_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto out = (dir / "models.json").string(), generated = (dir / "generated.json").string();
  const auto url = [&](const char* scenario) { return std::string("openai=") + base(peer, scenario); };
  const auto run = [&](const char* scenario, std::vector<std::string> more) {
    std::vector<std::string> arguments{"--source", "openai", "--url", url(scenario), "--out", out, "--generated", generated};
    arguments.insert(arguments.end(), more.begin(), more.end());
    return run_tool(tool, arguments);
  };
  const auto entries = [&](const std::string& path) {
    auto loaded = descriptor::load_catalogue_file(path);
    require(std::holds_alternative<descriptor::CatalogueSnapshot>(loaded), "the tool wrote a catalogue that loads");
    return std::get<descriptor::CatalogueSnapshot>(loaded)->entries().size();
  };
  const auto leftovers = [&] { return std::filesystem::exists(out + ".tmp") || std::filesystem::exists(generated + ".tmp"); };

  require(run("openai-large", {}) == 0 && entries(out) == 6 && entries(generated) == 6 && !leftovers(), "a refresh writes both files");
  const auto before_out = contents(out), before_generated = contents(generated);
  // A failed or empty source exits 3 and leaves both files as they were, though with no --previous the
  // catalogue it would write has nothing in it.
  require(run("broken", {}) == 3, "a failed source exits 3");
  require(run("openai-empty", {}) == 3, "an empty listing exits 3");
  require(contents(out) == before_out && contents(generated) == before_generated && !leftovers(), "a failed refresh leaves the files alone");
  // On request the catalogue is written anyway, with the failed source's previous entries.
  std::filesystem::remove(out);
  require(run("broken", {"--previous", generated, "--write-partial"}) == 3 && entries(out) == 6, "--write-partial writes the previous entries");
  // And the host can take a listing that dropped models.
  require(run("openai-small", {"--previous", generated}) == 3 && contents(generated) == before_generated, "far fewer models exit 3");
  require(run("openai-small", {"--previous", generated, "--allow-drop"}) == 0 && entries(out) == 2 && entries(generated) == 2, "--allow-drop takes the smaller listing");
  require(run("openai-large", {}) == 0 && entries(generated) == 6, "a full listing restores the entries");
  require(run("openai-small", {"--previous", generated, "--min-kept-percent", "34"}) == 3 && entries(generated) == 6, "a share of 34 percent refuses two of six");
  require(run("openai-small", {"--previous", generated, "--min-kept-percent", "30"}) == 0 && entries(generated) == 2, "a share of 30 percent takes two of six");
  require(run_tool(tool, {"--out", out, "--min-kept-percent", "101"}) == 2 && run_tool(tool, {"--out", out, "--min-kept-percent", "x"}) == 2, "a malformed share is refused");
  require(run_tool(tool, {"--out", out, "--url", "nonsense"}) == 2, "a malformed --url is refused");
  std::filesystem::remove_all(dir);
}

#endif

void invalid_requests(transport::Transport& transport) {
  auto refused_at = [&](transport::ListingSource s, std::string_view pointer) {
    transport::RefreshRequest request;
    request.sources = {std::move(s)};
    auto result = transport::refresh_catalogue(transport, request);
    require(std::holds_alternative<descriptor::ConfigError>(result) && std::get<descriptor::ConfigError>(result).pointer == pointer,
            "invalid source refused before any request");
  };
  refused_at(source(Listing::OpenAI, "http://api.example.test", "k"), "/sources/0/url");
  refused_at(source(Listing::OpenAI, "https://api.example.test/v1?x=1", "k"), "/sources/0/url");
  refused_at(source(Listing::OpenAI, "http://127.0.0.1.example.test", "k"), "/sources/0/url");
  refused_at({Listing::OpenAI, {}, "k", "https://api.example.test/path", {}}, "/sources/0/origin");
  // The origin scopes the generated entries and picks the listing's credential scheme, so it is the canonical spelling.
  for (const std::string_view spelling : {"https://OpenRouter.ai", "https://openrouter.ai:443"})
    refused_at({Listing::OpenRouter, {}, "k", std::string(spelling), {}}, "/sources/0/origin");
  refused_at(source(Listing::OpenAI, {}, "line\nbreak"), "/sources/0/api_key");
  require(transport::default_listing_url(Listing::Gemini) == "https://generativelanguage.googleapis.com", "default listing URL");
}

// The origin a source of each listing defaults to is the policy's data (#221), beside the family the listing belongs to.
// A policy of its own may name another one, take the embedded one by leaving it out, or state that a listing has none
// (`null`), and then a source of that listing names its `url` and its `origin`.
void default_origins_are_policy_data(transport::Transport& transport, Peer& peer) {
  // The built-in policy's: each provider's API origin.
  require(transport::default_listing_url(Listing::OpenRouter) == "https://openrouter.ai" &&
              transport::default_listing_url(Listing::Gemini) == "https://generativelanguage.googleapis.com" &&
              transport::default_listing_url(Listing::Anthropic) == "https://api.anthropic.com" &&
              transport::default_listing_url(Listing::OpenAI) == "https://api.openai.com",
          "the built-in default listing origins");
  // A default origin of the policy's own, here the loopback peer: a source without a url or an origin is fetched there
  // (the OpenAI listing path at the root of the origin) and its entries are scoped to it. A trust anchor that does not
  // exist keeps any other fetch from being sent.
  const auto own = "http://127.0.0.1:" + std::to_string(peer.port);
  const auto own_policy = policy_of(policy_edit::edited(
      {}, "listings", R"([{"listing":"openai","family":"openai.chat","default_origin":")" + own + R"("}])"));
  {
    transport::RefreshRequest request;
    request.sources = {source(Listing::OpenAI, {}, "openai-test-key")};
    request.policy = own_policy;
    request.ca_file = "/nonexistent/sp-test-no-trust-anchor.pem";
    request.total_timeout = std::chrono::milliseconds(10000);
    const auto result = refreshed(transport, request);
    require(result.sources.size() == 1 && result.sources[0].ok, "a source at the policy's default origin refreshed: " +
                                                                   (result.sources.empty() ? std::string("no report") : result.sources[0].error));
    require(result.sources[0].origin == own, "the source is scoped to the policy's default origin, not " + result.sources[0].origin);
    const auto* entry = result.generated->entry("gpt-test-1");
    require(entry && entry->origins == std::vector<std::string>{own}, "the listed entry is scoped to the policy's default origin");
    require(seen(peer, "v1").total == 1, "the listing was fetched from the policy's default origin");
  }
  // A listing the policy states without an origin takes the embedded one: a source served elsewhere is scoped to it.
  const auto borrowing = policy_of(policy_edit::edited({}, "listings", R"([{"listing":"openai","family":"openai.chat"}])"));
  {
    transport::RefreshRequest request;
    request.sources = {source(Listing::OpenAI, base(peer, "openai"), "openai-test-key")};
    request.policy = borrowing;
    const auto result = refreshed(transport, request);
    require(result.sources.size() == 1 && result.sources[0].ok && result.sources[0].origin == "https://api.openai.com",
            "a listing stated without an origin takes the embedded one");
  }
  // A listing with no default origin: a source of it without a url, or without an origin, is refused before any request.
  const auto none = policy_of(policy_edit::edited({}, "listings", R"([{"listing":"openai","family":"openai.chat","default_origin":null}])"));
  const auto refused_at = [&](transport::ListingSource listed, std::string_view pointer) {
    transport::RefreshRequest request;
    request.sources = {source(Listing::Gemini, base(peer, "gemini"), "gemini-test-key"), std::move(listed)};
    request.policy = none;
    auto result = transport::refresh_catalogue(transport, request);
    const auto* error = std::get_if<descriptor::ConfigError>(&result);
    require(error && error->pointer == pointer, "a source of a listing without a default origin refused at " + std::string(pointer) + ", not " +
                                                    (error ? error->pointer : std::string("a refresh")));
  };
  refused_at(source(Listing::OpenAI, {}, "openai-test-key"), "/sources/1/url");
  refused_at(source(Listing::OpenAI, base(peer, "openai"), "openai-test-key"), "/sources/1/origin");
  {
    transport::RefreshRequest request;
    request.sources = {{Listing::OpenAI, base(peer, "openai"), "openai-test-key", "https://api.openai.com", {}}};
    request.policy = none;
    const auto result = refreshed(transport, request);
    require(result.sources.size() == 1 && result.sources[0].ok, "a source that names its url and origin needs no default");
  }
  passed("default listing origins: the policy's data, borrowed from the embedded policy, or none");
}

void embedded_facts_apply_at_every_origin(transport::Transport& transport, Peer& peer) {
  transport::RefreshRequest request;
  request.sources = {source(Listing::OpenAI, base(peer, "openai-builtin"), "openai-test-key")};
  request.overlays = {descriptor::builtin_catalogue()};
  const auto result = refreshed(transport, request);
  require(result.sources[0].ok && result.sources[0].origin == "https://api.openai.com", "the listing is scoped to the provider origin");
  require(result.generated->entry("gpt-6-luna")->origins == std::vector<std::string>{"https://api.openai.com"}, "the listing scopes its entry");

  // The embedded entry applies wherever it applied before the refresh: at the listing's origin and at a gateway.
  const auto& c = *result.catalogue;
  require(c.entry("gpt-6-luna")->origins.empty() && c.entry("gpt-6-luna")->source == std::optional<std::string>("verified"), "the embedded entry is unscoped");
  for (const std::string_view origin : {"https://api.openai.com", "https://gateway.example.test"}) {
    const auto match = c.find("openai.chat", origin, "gpt-6-luna");
    require(match.entry && match.entry->output_limit == 128000u && match.family_listed, "an embedded model matches at every origin");
  }
  // A model only the listing names stays at the listing's origin.
  require(c.find("openai.chat", "https://api.openai.com", "gpt-test-1").entry &&
          !c.find("openai.chat", "https://gateway.example.test", "gpt-test-1").entry, "a listed model keeps the listing's origin");
  auto policy = descriptor::builtin_policy()->with_catalogue(result.catalogue);
  require(std::holds_alternative<descriptor::PolicySnapshot>(policy), "refreshed catalogue attaches");
  require(std::get<descriptor::PolicySnapshot>(policy)->output_limit("openai.chat", "https://gateway.example.test", "gpt-6-luna") == 128000u,
          "the embedded limit applies at a gateway");
}

// Route ceilings from OpenRouter's endpoints listings.
constexpr std::string_view ceiling_units[]{"ceiling_input", "ceiling_output", "ceiling_request"};
std::optional<std::string> unit_price(const descriptor::CatalogueEntry* entry, std::string_view unit) {
  if (!entry) return std::nullopt;
  for (const auto& [name, price] : entry->pricing) if (name == unit) return price;
  return std::nullopt;
}
// The entry's route ceiling, unit by unit, as "input output request" with "-" for a unit it lacks.
std::string ceiling_of(const descriptor::CatalogueEntry* entry) {
  std::string out;
  for (const auto unit : ceiling_units) {
    if (!out.empty()) out += ' ';
    out += unit_price(entry, unit).value_or("-");
  }
  return out;
}
std::vector<std::string> gap_lines(const transport::SourceReport& report) {
  std::vector<std::string> out;
  for (const auto& gap : report.ceiling_gaps)
    out.push_back(gap.model + "|" + gap.unit + "|" + gap.price + "|" + gap.reason + (gap.kept ? "|kept" : ""));
  return out;
}
// A report holds no URL, header, credential or body text.
void fixed_text_only(const transport::SourceReport& report) {
  for (const auto& gap : report.ceiling_gaps)
    for (const auto* text : {&gap.model, &gap.unit, &gap.price, &gap.reason})
      for (const std::string_view banned : {"127.0.0.1", "http", "Bearer", "openrouter-test-key", "synthetic", "captive", "<html>", "/api/"})
        require(text->find(banned) == std::string::npos, "a ceiling gap carries fixed text only");
}
descriptor::CatalogueSnapshot catalogue_text(std::string_view json) {
  auto loaded = descriptor::load_catalogue(json);
  require(std::holds_alternative<descriptor::CatalogueSnapshot>(loaded), "test catalogue loads");
  return std::get<descriptor::CatalogueSnapshot>(std::move(loaded));
}
transport::ListingSource ceiling_source(const Peer& peer, std::string_view scenario, std::vector<std::string> ids) {
  auto out = source(Listing::OpenRouter, base(peer, scenario), "openrouter-test-key");
  out.ceiling_models = std::move(ids);
  return out;
}
// Every model the `ceilings` scenario lists except vendor/unasked, out of order, and one it does not list.
const std::vector<std::string> asked_ceilings{
    "vendor/stall", "vendor/many", "vendor/fee", "vendor/zero-fee", "vendor/cached", "~vendor/alias-latest", "vendor/missing-prompt",
    "vendor/negative", "vendor/not-decimal", "vendor/number", "vendor/too-large", "vendor/bad-cache", "vendor/bad-override",
    "vendor/bad-fee", "vendor/bad-discount", "vendor/malformed", "vendor/override-shape", "vendor/wrong-id", "vendor/not-json",
    "vendor/outage", "vendor/full", "vendor/missing-fee", "vendor/override-fee", "vendor/override-fees", "vendor/override-discount",
    "vendor/listed-dear", "vendor/listed-variable", "vendor/absent"};
std::vector<std::string> listed_asked() {
  std::vector<std::string> out;
  for (const auto& id : asked_ceilings) if (id != "vendor/absent") out.push_back(id);
  std::sort(out.begin(), out.end());
  return out;
}

void ceilings_from_the_endpoints_listings(transport::Transport& transport, Peer& peer, descriptor::CatalogueSnapshot& generated) {
  transport::RefreshRequest request;
  request.sources = {ceiling_source(peer, "ceilings", asked_ceilings)};
  request.overlays = {catalogue_text(R"({"version":1,"models":[{"id":"vendor/many","pricing":{"ceiling_input":"0.00001"}}]})")};
  const auto result = refreshed(transport, request);
  const auto& report = result.sources[0];
  require(report.ok && report.error.empty() && report.pages == 1, "the listing refreshed");
  const auto& c = *result.generated;
  auto described = [&] {
    std::string out = std::to_string(report.ceiling_requests) + " request(s), " + std::to_string(report.ceilings) + " written whole";
    for (const auto& line : gap_lines(report)) out += "\n  " + line;
    return out;
  };

  // The dearest price of each unit over every endpoint, and the request fee only where an endpoint charges one.
  require(ceiling_of(c.entry("vendor/many")) == "0.000003 0.000005 -",
          "the dearest prompt and completion over three endpoints, not " + ceiling_of(c.entry("vendor/many")) + "; " + described());
  require(ceiling_of(c.entry("vendor/fee")) == "0.000001 0.000001 0.0002", "the dearest request fee, beside a zero fee and a lower one");
  require(ceiling_of(c.entry("vendor/zero-fee")) == "0.000001 0.000002 -", "no request ceiling where no endpoint charges a fee");
  require(ceiling_of(c.entry("vendor/outage")) == "0.000001 0.000002 0.0001" && ceiling_of(c.entry("vendor/stall")) == "0.000002 0.000003 -",
          "the models whose endpoints fail later");
  // Cache and reasoning prices above the prompt and completion prices, and a dearer long-context override.
  require(ceiling_of(c.entry("vendor/cached")) == "0.000006 0.00002 -", "the ceiling covers every token price and every override");
  // A unit with an unusable price at one endpoint gets no ceiling; the other units still do.
  require(ceiling_of(c.entry("vendor/missing-prompt")) == "- 0.000003 -", "a missing prompt price leaves out the input ceiling");
  require(ceiling_of(c.entry("vendor/negative")) == "- 0.000002 -", "a variable prompt price leaves out the input ceiling");
  require(ceiling_of(c.entry("vendor/not-decimal")) == "0.000002 - -", "a completion price that is not decimal text leaves out the output ceiling");
  require(ceiling_of(c.entry("vendor/number")) == "- 0.000002 -", "a price written as a number leaves out its ceiling");
  require(ceiling_of(c.entry("vendor/too-large")) == "- 0.000001 -", "a price too large for the catalogue leaves out its ceiling");
  require(ceiling_of(c.entry("vendor/bad-cache")) == "- 0.000002 -", "a variable cache price leaves out the input ceiling");
  require(ceiling_of(c.entry("vendor/bad-override")) == "0.000001 - -", "an override's unreadable price leaves out its ceiling");
  // A missing request ceiling would bound the fee at the listed one, so an unusable fee, or one only some endpoints
  // list, leaves out the whole ceiling.
  require(ceiling_of(c.entry("vendor/bad-fee")) == "- - -", "an unusable request fee leaves out every unit");
  require(ceiling_of(c.entry("vendor/missing-fee")) == "- - -", "a request fee only some endpoints list leaves out every unit");
  // A fee an override lists is a listed fee: at one endpoint of two it leaves out the ceiling, at both it is written.
  require(ceiling_of(c.entry("vendor/override-fee")) == "- - -", "a request fee only one endpoint's override lists leaves out every unit");
  require(ceiling_of(c.entry("vendor/override-fees")) == "0.000001 0.000002 0.002", "request fees every endpoint's overrides list give the ceiling");
  require(ceiling_of(c.entry("vendor/override-discount")) == "- - -", "an override's discount that raises its prices leaves out every unit");
  // A ceiling is never below a price the models listing gives for the model in the same refresh.
  require(ceiling_of(c.entry("vendor/listed-dear")) == "0.00002 0.000005 -", "a listed price above every endpoint's is the ceiling of its side");
  require(ceiling_of(c.entry("vendor/listed-variable")) == "- 0.000002 -", "a variable listed price leaves out the ceiling of its side");
  require(ceiling_of(c.entry("vendor/bad-discount")) == "- - -", "a discount that raises the price leaves out every unit");
  for (const char* id : {"~vendor/alias-latest", "vendor/malformed", "vendor/override-shape", "vendor/wrong-id", "vendor/not-json", "vendor/full", "vendor/unasked"})
    require(c.entry(id) && ceiling_of(c.entry(id)) == "- - -", "no ceiling for an alias, a malformed listing, a full entry or a model not asked for");
  require(!c.entry("vendor/absent"), "an id the listing does not name gets no entry");
  require(c.entry("vendor/full")->pricing.size() == 64, "the full entry keeps its listed prices");
  require(unit_price(c.entry("vendor/many"), "prompt") == std::optional<std::string>("0.0000001"), "listed prices stay beside the ceiling");

  // Every gap is named, by model id in order, with fixed text.
  const std::vector<std::string> gaps{
      "vendor/absent|||not in the listing",
      "vendor/bad-cache|ceiling_input|input_cache_write|variable price at an endpoint",
      "vendor/bad-discount||discount|discount at an endpoint is not from 0 to 1",
      "vendor/bad-fee|ceiling_request|request|variable price at an endpoint",
      "vendor/bad-fee|||no ceiling without every request fee",
      "vendor/bad-override|ceiling_output|completion|price at an endpoint is not decimal text",
      "vendor/full|||the entry cannot hold the ceiling",
      "vendor/listed-variable|ceiling_input|prompt|variable price in the listing",
      "vendor/malformed|||not an endpoints listing",
      "vendor/missing-fee|ceiling_request|request|price missing at an endpoint",
      "vendor/missing-fee|||no ceiling without every request fee",
      "vendor/missing-prompt|ceiling_input|prompt|price missing at an endpoint",
      "vendor/negative|ceiling_input|prompt|variable price at an endpoint",
      "vendor/not-decimal|ceiling_output|completion|price at an endpoint is not decimal text",
      "vendor/not-json|||not an endpoints listing",
      "vendor/number|ceiling_input|prompt|price at an endpoint is not decimal text",
      "vendor/override-discount||discount|discount at an endpoint is not from 0 to 1",
      "vendor/override-fee|ceiling_request|request|price missing at an endpoint",
      "vendor/override-fee|||no ceiling without every request fee",
      "vendor/override-shape|||not an endpoints listing",
      "vendor/too-large|ceiling_input|prompt|price at an endpoint is too large for the catalogue",
      "vendor/wrong-id|||endpoints listing names another model",
      "~vendor/alias-latest|||no endpoints"};
  const auto lines = gap_lines(report);
  if (lines != gaps) {
    std::string all;
    for (const auto& line : lines) all += "\n  " + line;
    throw std::runtime_error("the ceiling gaps are not the expected ones:" + all);
  }
  require(report.ceilings == 8 && report.ceiling_requests == 27, "eight ceilings written whole from 27 endpoints requests");
  // One request per listed id asked for, in id order, and none for an id the listing does not name or one not asked for.
  require(seen(peer, "ceilings").endpoints == listed_asked(), "the endpoints of each listed model asked for, once, in order");
  fixed_text_only(report);

  // The overlay wins over the generated ceiling, unit by unit.
  require(ceiling_of(result.catalogue->entry("vendor/many")) == "0.00001 0.000005 -", "an overlay's ceiling unit replaces the generated one");
  // Pricing at the route ceiling: 900 uncached and 100 cache-write input tokens at 6e-6 USD, and 100 output tokens
  // at 2e-5 USD, are 6'000'000 + 2'000'000 nano-USD.
  Usage usage;
  usage.input_total = Count{1000, Evidence::Reported};
  usage.cache_write = Count{100, Evidence::Reported};
  usage.output_total = Count{100, Evidence::Reported};
  usage.reasoning = Count{50, Evidence::Reported};
  const auto routed = pricing::estimate(c, "openai.chat", "https://openrouter.ai", ServedModel{"vendor/cached", "vendor/cached", ServedRelation::Exact},
                                        usage, {}, true);
  require(routed.total && routed.total->nano_usd == 8'000'000u, "the route ceiling prices cache writes and reasoning at the dearest token prices");
  require(result.diff.added.size() == c.entries().size(), "a first refresh adds every entry");
  generated = result.generated;
  passed("route ceilings: dearest units, request fees, unusable prices, malformed listings, aliases, unknown ids, overlays");
}

// A failed endpoints request keeps the ceiling `previous` had; an answer that is not usable does not.
void failed_endpoints_keep_previous_ceilings(transport::Transport& transport, Peer& peer, const descriptor::CatalogueSnapshot& previous) {
  transport::RefreshRequest request;
  request.previous = previous;
  request.page_timeout = std::chrono::milliseconds(300);
  request.sources = {ceiling_source(peer, "ceilings-v2", asked_ceilings)};
  const auto result = refreshed(transport, request);
  const auto& report = result.sources[0];
  require(report.ok && report.ceiling_requests == 27, "a failed endpoints request does not fail the listing");
  fixed_text_only(report);
  const auto& c = *result.generated;
  require(ceiling_of(c.entry("vendor/outage")) == "0.000001 0.000002 0.0001", "an outage keeps the previous ceiling");
  require(ceiling_of(c.entry("vendor/stall")) == "0.000002 0.000003 -", "a timeout keeps the previous ceiling");
  require(ceiling_of(c.entry("vendor/many")) == "0.000003 0.000006 -", "a changed endpoint price changes the ceiling");
  require(ceiling_of(c.entry("vendor/fee")) == "- - -", "an answer that is not a listing drops the previous ceiling");
  const auto lines = gap_lines(report);
  require(has(lines, "vendor/outage|||endpoints answered HTTP 500|kept") && has(lines, "vendor/stall|||endpoints request did not complete|kept") &&
          has(lines, "vendor/fee|||not an endpoints listing"), "the failed models are named, and whether the previous ceiling was kept");
  require(report.ceilings == 5, "vendor/many, vendor/zero-fee, vendor/cached, vendor/override-fees and vendor/listed-dear are written whole");
  // The diff reports a changed ceiling the way it reports a changed price, and the endpoint rows a failed request drops
  // (vendor/outage and vendor/stall keep their ceilings and lose their rows).
  require(result.diff.changed == (std::vector<std::string>{"vendor/fee", "vendor/many", "vendor/outage", "vendor/stall"}) && result.diff.added.empty() &&
              result.diff.removed.empty(),
          "the diff names the entries whose ceiling or rows changed, and only those");

  // A previous ceiling below a price the listing now gives for the model (written before that price was listed) is not
  // kept, and neither is its other side: here the previous output ceiling is half the listed 0.0000001 completion price.
  {
    std::vector<descriptor::CatalogueEntry> entries = previous->entries();
    for (auto& entry : entries)
      if (entry.id == "vendor/outage")
        for (auto& price : entry.pricing)
          if (price.first == "ceiling_output") price.second = "0.00000005";
    auto made = descriptor::make_catalogue(std::move(entries));
    require(std::holds_alternative<descriptor::CatalogueSnapshot>(made), "the previous catalogue with a low output ceiling loads");
    transport::RefreshRequest low;
    low.previous = std::get<descriptor::CatalogueSnapshot>(made);
    low.page_timeout = std::chrono::milliseconds(300);
    low.sources = {ceiling_source(peer, "ceilings-v2", {"vendor/outage"})};
    const auto kept = refreshed(transport, low);
    require(ceiling_of(kept.generated->entry("vendor/outage")) == "- - -",
            "a stale side drops the whole previous ceiling: " + ceiling_of(kept.generated->entry("vendor/outage")));
    const std::string reason = "kept ceiling is dropped: a side of it is below a price the listing gives";
    require(gap_lines(kept.sources[0]) == (std::vector<std::string>{"vendor/outage|||endpoints answered HTTP 500", "vendor/outage|ceiling_input||" + reason,
                                                                     "vendor/outage|ceiling_output||" + reason, "vendor/outage|ceiling_request||" + reason}),
            "the dropped units are named, and the ceiling is not reported as kept");
  }

  // Without a previous ceiling, a failed request keeps nothing.
  request.previous = nullptr;
  request.sources = {ceiling_source(peer, "ceilings-v2", {"vendor/outage"})};
  const auto bare = refreshed(transport, request);
  require(gap_lines(bare.sources[0]) == std::vector<std::string>{"vendor/outage|||endpoints answered HTTP 500"} &&
          ceiling_of(bare.generated->entry("vendor/outage")) == "- - -", "a failure with no previous ceiling writes none");

  // The refresh's deadline bounds the endpoints requests too: the one it stops and every one after it keep their
  // previous ceilings, and nothing is sent once it has passed.
  request.previous = previous;
  request.page_timeout = std::chrono::milliseconds(5000);
  request.total_timeout = std::chrono::milliseconds(500);
  request.sources = {ceiling_source(peer, "ceilings-v2", {"vendor/zero-fee", "vendor/stall"})};
  const auto began = std::chrono::steady_clock::now();
  const auto late = refreshed(transport, request);
  require(std::chrono::steady_clock::now() - began < std::chrono::seconds(3), "the refresh stops at its deadline, not the page timeout");
  require(late.sources[0].ok && late.sources[0].ceiling_requests == 1 &&
          gap_lines(late.sources[0]) == (std::vector<std::string>{"vendor/stall|||refresh deadline passed|kept", "vendor/zero-fee|||refresh deadline passed|kept"}) &&
          ceiling_of(late.generated->entry("vendor/zero-fee")) == "0.000001 0.000002 -", "a deadline keeps the previous ceilings and sends nothing more");
  request.page_timeout = std::chrono::milliseconds(300);
  request.total_timeout = std::chrono::milliseconds(300000);

  // A source whose listing fails sends no endpoints request; its previous entries, ceilings included, stay.
  request.sources = {ceiling_source(peer, "broken", {"vendor/many", "vendor/negative"})};
  const auto down = refreshed(transport, request);
  require(!down.sources[0].ok && down.sources[0].ceiling_requests == 0 && seen(peer, "broken").endpoints.empty(), "no endpoints request after a failed listing");
  require(gap_lines(down.sources[0]) == (std::vector<std::string>{"vendor/many|||listing failed|kept", "vendor/negative|||listing failed|kept"}),
          "the ids are named as not refreshed");
  require(ceiling_of(down.generated->entry("vendor/many")) == "0.000003 0.000005 -" && down.generated->digest() == previous->digest(),
          "a failed listing keeps the previous entries whole");

  // A refresh that asks for no ceilings this time writes none.
  request.sources = {source(Listing::OpenRouter, base(peer, "ceilings-plain"))};
  const auto plain = refreshed(transport, request);
  require(plain.sources[0].ok && plain.sources[0].ceiling_requests == 0 && plain.sources[0].ceilings == 0 && plain.sources[0].ceiling_gaps.empty() &&
          seen(peer, "ceilings-plain").total == 1, "no ids, no endpoints request");
  for (const auto& entry : plain.generated->entries()) require(ceiling_of(&entry) == "- - -", "no ids, no ceiling");
  passed("route ceilings: failures keep previous ceilings, unusable answers drop them, and the diff names the change");
}

// Ids are checked before anything is sent.
void ceiling_requests_are_checked(transport::Transport& transport, Peer& peer) {
  const std::string limit_pointer = "/sources/0/ceiling_models";
  auto refused_at = [&](transport::ListingSource s, std::string_view pointer, std::string_view why) {
    transport::RefreshRequest request;
    request.sources = {std::move(s)};
    auto result = transport::refresh_catalogue(transport, request);
    require(std::holds_alternative<descriptor::ConfigError>(result) && std::get<descriptor::ConfigError>(result).pointer == pointer,
            "refused before any request: " + std::string(why));
  };
  for (const std::string& bad : {std::string(),std::string("vendor"), std::string("vendor/"), std::string("/model"), std::string("vendor/model/extra"),
                                std::string("../model"), std::string("vendor/.."), std::string("vendor/."), std::string(".hidden/model"),
                                std::string("vendor/model?x=1"), std::string("vendor/model#x"), std::string("vendor/mod%2Fel"), std::string("vendor/mo del"),
                                std::string("vendor/model\n"), std::string("~/model"), std::string("vendor/~model"), std::string("ven:dor/model"),
                                std::string("vendor/-model"), std::string("vendor\\model"), std::string(300, 'a') + "/m"})
    refused_at(ceiling_source(peer, "ceilings-invalid", {"vendor/many", bad}), limit_pointer + "/1", "an id that is not author/slug");
  refused_at(ceiling_source(peer, "ceilings-invalid", {"vendor/many", "vendor/many"}), limit_pointer, "an id asked for twice");
  std::vector<std::string> ids;
  for (std::uint64_t i = 0; i <= config_defaults::catalogue_refresh_max_ceiling_models; ++i) ids.push_back("vendor/m-" + std::to_string(i));
  refused_at(ceiling_source(peer, "ceilings-invalid", ids), limit_pointer, "more ids than config/catalogue-refresh.json allows");
  refused_at({Listing::OpenAI, base(peer, "ceilings-invalid"), "openai-test-key", {}, {"vendor/many"}}, limit_pointer, "ids on a listing that is not OpenRouter's");
  require(seen(peer, "ceilings-invalid").total == 0, "an invalid request sends nothing");

  // The largest list passes, and an id the listing does not name costs no request.
  ids.pop_back();
  ids.insert(ids.end(), {"~vendor/x-latest", "vendor/model:free", "A1/b.c_d-e"});
  ids.erase(ids.begin(), ids.begin() + 3);
  transport::RefreshRequest request;
  request.sources = {ceiling_source(peer, "ceilings-max", ids)};
  const auto result = refreshed(transport, request);
  require(result.sources[0].ok && result.sources[0].ceiling_requests == 0 && result.sources[0].ceiling_gaps.size() == ids.size() &&
          seen(peer, "ceilings-max").total == 1, "ids the listing does not name are reported and not requested");
  for (const auto& gap : result.sources[0].ceiling_gaps) require(gap.reason == "not in the listing" && gap.unit.empty(), "an id not listed is named");
  require(std::is_sorted(result.sources[0].ceiling_gaps.begin(), result.sources[0].ceiling_gaps.end(),
                         [](const auto& a, const auto& b) { return a.model < b.model; }), "gaps come in id order");
  passed("route ceilings: invalid, repeated and too many ids are refused before any request");
}

// For generated endpoint prices, the written ceiling is the dearest price of its side, never below any endpoint's,
// and a usage priced at the route ceiling costs at least what it costs at any one endpoint.
void ceilings_bound_every_endpoint(transport::Transport& transport, Peer& peer) {
  std::mt19937_64 random(20261118);
  auto pick = [&random](std::uint64_t n) { return random() % n; };
  auto price = [&pick]() {
    std::string text = pick(8) == 0 ? std::to_string(pick(100)) : std::string("0");
    if (pick(10) == 0) return text;
    text += '.';
    for (auto digits = 1 + pick(18); digits; --digits) text += static_cast<char>('0' + pick(10));
    return text;
  };
  // The token and request prices of an endpoint by the side of the ceiling that covers them (0 input, 1 output,
  // 2 request), and the catalogue unit that prices the same thing at one endpoint (empty: none does).
  struct Member { const char* name; int side; const char* unit; };
  constexpr Member members[]{{"prompt", 0, "prompt"}, {"input_cache_read", 0, "input_cache_read"}, {"input_cache_write", 0, "input_cache_write"},
                             {"input_cache_write_1h", 0, "cache_write_1h"}, {"audio", 0, "audio"}, {"input_audio_cache", 0, ""},
                             {"image_token", 0, ""}, {"image_token", 1, ""}, {"completion", 1, "completion"},
                             {"internal_reasoning", 1, "internal_reasoning"}, {"audio_output", 1, "audio_output"},
                             {"image_output", 1, "image_output"}, {"request", 2, "request"}};
  using Prices = std::map<std::string, std::string>;
  struct Endpoint { Prices base; std::vector<Prices> overrides; };
  auto object = [](const Prices& prices) {
    std::string out;
    for (const auto& item : prices) out += (out.empty() ? "" : ",") + json::quote(item.first) + ":" + json::quote(item.second);
    return out;
  };
  std::size_t models = 0, comparisons = 0;
  for (int round = 0; round < 6; ++round) {
    const auto scenario = "ceilings-prop-" + std::to_string(round);
    std::map<std::string, std::vector<Endpoint>> generated;
    std::map<std::string, Prices> listed;  // what the models listing gives for each model
    std::string command = "{\"set\":" + json::quote(scenario) + ",\"models\":{";
    for (int m = 0; m < 24; ++m) {
      const auto id = "prop/m-" + std::to_string(round) + "-" + std::to_string(m);
      auto& endpoints = generated[id];
      command += (m ? "," : "") + json::quote(id) + ":[";
      const auto count = 1 + pick(6);
      const bool fees = pick(2) == 0;  // every endpoint lists a request fee, or none does
      auto& prices_listed = listed[id];
      prices_listed = {{"prompt", price()}, {"completion", price()}};
      for (const auto& member : members)
        if (member.side != 2 && prices_listed.count(member.name) == 0 && pick(4) == 0) prices_listed[member.name] = price();
      for (std::uint64_t e = 0; e < count; ++e) {
        Endpoint endpoint;
        endpoint.base = {{"prompt", price()}, {"completion", price()}};
        for (const auto& member : members)
          if (member.side != 2 ? endpoint.base.count(member.name) == 0 && pick(3) == 0 : fees) endpoint.base[member.name] = price();
        for (auto o = pick(3); o; --o) {
          Prices changed;
          for (const char* key : {"prompt", "completion", "input_cache_read", "input_cache_write", "internal_reasoning"})
            if (pick(2)) changed[key] = price();
          endpoint.overrides.push_back(std::move(changed));
        }
        command += std::string(e ? "," : "") + "{" + object(endpoint.base);
        if (pick(3) == 0) command += pick(2) ? ",\"discount\":0.5" : ",\"discount\":0";
        if (!endpoint.overrides.empty()) {
          command += ",\"overrides\":[";
          for (std::size_t o = 0; o < endpoint.overrides.size(); ++o) {
            const auto more = object(endpoint.overrides[o]);
            command += std::string(o ? "," : "") + "{\"min_prompt_tokens\":" + std::to_string(100000 * (o + 1)) + (more.empty() ? "" : "," + more) + "}";
          }
          command += "]";
        }
        command += "}";
        endpoints.push_back(std::move(endpoint));
      }
      command += "]";
    }
    command += "},\"listed\":{";
    for (auto item = listed.begin(); item != listed.end(); ++item)
      command += (item == listed.begin() ? "" : ",") + json::quote(item->first) + ":{" + object(item->second) + "}";
    peer.command(command + "}}");
    std::vector<std::string> ids;
    for (const auto& item : generated) ids.push_back(item.first);
    transport::RefreshRequest request;
    request.sources = {ceiling_source(peer, scenario, ids)};
    const auto result = refreshed(transport, request);
    const auto& report = result.sources[0];
    require(report.ok && report.ceilings == ids.size() && report.ceiling_gaps.empty() && report.ceiling_requests == ids.size(),
            "every generated ceiling is written");
    const auto& c = *result.generated;
    for (const auto& item : generated) {
      const auto& id = item.first;
      const auto* entry = c.entry(id);
      std::optional<std::uint64_t> ceiling[3];
      for (int side = 0; side < 3; ++side)
        if (const auto text = unit_price(entry, ceiling_units[side])) ceiling[side] = pricing::per_million(*text);
      require(ceiling[0] && ceiling[1], "input and output ceilings are written");
      // Each side's ceiling is the dearest price of that side over every endpoint and override.
      std::uint64_t dearest[3] = {0, 0, 0};
      std::vector<Prices> conditions;  // every price set one endpoint can bill under, and the listed prices
      conditions.push_back(listed[id]);
      for (const auto& endpoint : item.second) {
        conditions.push_back(endpoint.base);
        for (const auto& changed : endpoint.overrides) {
          auto merged = endpoint.base;
          for (const auto& price_item : changed) merged[price_item.first] = price_item.second;
          conditions.push_back(std::move(merged));
        }
      }
      for (const auto& condition : conditions)
        for (const auto& member : members)
          if (const auto found = condition.find(member.name); found != condition.end()) {
            const auto rate = pricing::per_million(found->second);
            require(rate.has_value(), "a generated price reads");
            require(member.side == 2 || *ceiling[member.side] >= *rate, "a ceiling is never below an endpoint's or a listed price");
            dearest[member.side] = std::max(dearest[member.side], *rate);
          }
      require(*ceiling[0] == dearest[0] && *ceiling[1] == dearest[1], "the ceiling is the dearest price, not more");
      require(dearest[2] ? ceiling[2] == dearest[2] : !ceiling[2], "a request ceiling is the dearest fee, written when an endpoint charges one");
      // The same usage at the route ceiling and at one endpoint under each price set it can bill under.
      const ServedModel served{id, id, ServedRelation::Exact};
      for (const auto& condition : conditions) {
        Prices units;
        for (const auto& member : members)
          if (const auto found = condition.find(member.name); found != condition.end() && *member.unit) units[member.unit] = found->second;
        const auto one = catalogue_text(R"({"version":1,"models":[{"id":)" + json::quote(id) + R"(,"pricing":{)" + object(units) + "}}]}");
        for (int u = 0; u < 4; ++u) {
          const auto input = 1 + pick(1'000'000), output = 1 + pick(100'000);
          Usage usage;
          usage.input_total = Count{input, Evidence::Reported};
          usage.cache_read = Count{pick(input / 4 + 1), Evidence::Reported};
          usage.cache_write = Count{pick(input / 4 + 1), Evidence::Reported};
          usage.output_total = Count{output, Evidence::Reported};
          usage.reasoning = Count{pick(output + 1), Evidence::Reported};
          if (pick(3) == 0) usage.extra["input_audio"] = Count{pick(input / 4 + 1), Evidence::Reported};
          if (pick(3) == 0) usage.extra["audio_tokens"] = Count{pick(output / 4 + 1), Evidence::Reported};
          if (pick(3) == 0) usage.extra["completion_tokens_details.image_tokens"] = Count{pick(output / 4 + 1), Evidence::Reported};
          const auto routed = pricing::estimate(c, "openai.chat", "https://openrouter.ai", served, usage, {}, true);
          const auto at_one = pricing::estimate(*one, "openai.chat", "https://openrouter.ai", served, usage);
          require(routed.total.has_value(), "the route ceiling prices every usage");
          if (at_one.total) {
            require(routed.total->nano_usd >= at_one.total->nano_usd, "the route ceiling costs at least what one endpoint costs");
            ++comparisons;
          }
          const auto bound = pricing::upper_bound(*entry, {}, {input, output, true, {}});
          const auto bound_one = pricing::upper_bound(*one->entry(id), {}, {input, output, false, {}});
          require(bound.total && bound_one.total && bound.total->nano_usd >= bound_one.total->nano_usd,
                  "the reservation at the route ceiling covers any one endpoint's");
        }
      }
      ++models;
    }
  }
  require(models == 144 && comparisons > 1000, "the property ran over every generated model");
  std::cout << "case passed: route ceilings bound every endpoint (" << models << " models, " << comparisons << " estimate comparisons)" << std::endl;
}

// The listed prices a ceiling is never below include the entry's input bands. A side whose listed price the entry
// leaves out for an override the catalogue cannot hold gets no ceiling, and conditional prices that cannot be read
// at all give none.
void ceilings_count_listed_bands(transport::Transport& transport, Peer& peer) {
  peer.command(R"({"set":"ceilings-bands","models":{"vendor/band-floor":[{"prompt":"0.000001","completion":"0.000002"}],)"
               R"("vendor/withheld-floor":[{"prompt":"0.000001","completion":"0.000002"}],"vendor/unread-floor":[{"prompt":"0.000001","completion":"0.000002"}],)"
               R"("vendor/window-floor":[{"prompt":"0.000001","completion":"0.000002"}]},)"
               R"("listed":{"vendor/band-floor":{"prompt":"0.0000001","completion":"0.0000001",)"
               R"("overrides":[{"min_prompt_tokens":100000,"prompt":"0.000005","completion":"0.000007"}]},)"
               R"("vendor/withheld-floor":{"prompt":"0.0000001","completion":"0.0000001","overrides":[{"utc_start":0,"prompt":"0.000009"}]},)"
               R"("vendor/window-floor":{"prompt":"0.0000001","completion":"0.0000001","overrides":[{"utc_start":0,"utc_end":600,"prompt":"0.000009"}]},)"
               R"("vendor/unread-floor":{"prompt":"0.0000001","completion":"0.0000001","overrides":["long context"]}}})");
  // A ceiling unit the models listing names is dropped, with its bands: a ceiling comes from the endpoints only.
  peer.command(R"({"set":"ceilings-band-unit","models":{"vendor/band-ceiling":[{"prompt":"0.000001","completion":"0.000002"}]},)"
               R"("listed":{"vendor/band-ceiling":{"prompt":"0.0000001","completion":"0.0000001",)"
               R"("overrides":[{"min_prompt_tokens":1000,"ceiling_input":"0.0000000001","ceiling_output":"0.0000000001"}]}}})");
  transport::RefreshRequest named;
  named.sources = {ceiling_source(peer, "ceilings-band-unit", {"vendor/band-ceiling"})};
  const auto named_result = refreshed(transport, named);
  const auto* band_ceiling = named_result.generated->entry("vendor/band-ceiling");
  require(pricing_text(band_ceiling) == "ceiling_input=0.000001 ceiling_output=0.000002 completion=0.0000001 prompt=0.0000001",
          "a listed ceiling band is dropped, not kept beside the ceiling: " + pricing_text(band_ceiling));
  transport::RefreshRequest request;
  request.sources = {ceiling_source(peer, "ceilings-bands", {"vendor/band-floor", "vendor/withheld-floor", "vendor/unread-floor", "vendor/window-floor"})};
  const auto result = refreshed(transport, request);
  const auto& report = result.sources[0];
  require(report.ok, "the listing refreshed");
  const auto& c = *result.generated;
  require(ceiling_of(c.entry("vendor/band-floor")) == "0.000005 0.000007 -",
          "a listed band above every endpoint's price is the ceiling of its side, not " + ceiling_of(c.entry("vendor/band-floor")));
  require(ceiling_of(c.entry("vendor/withheld-floor")) == "- 0.000002 -",
          "a side whose listed price is left out gets no ceiling, not " + ceiling_of(c.entry("vendor/withheld-floor")));
  require(ceiling_of(c.entry("vendor/unread-floor")) == "- - -", "conditional prices that cannot be read give no ceiling");
  require(ceiling_of(c.entry("vendor/window-floor")) == "0.000009 0.000002 -",
          "a time-window price held at its dearest is a listed price the ceiling covers, not " + ceiling_of(c.entry("vendor/window-floor")));
  const std::vector<std::string> gaps{"vendor/unread-floor|||conditional prices in the listing cannot be read",
                                      "vendor/withheld-floor|ceiling_input|prompt|price in the listing left out for a conditional price"};
  if (gap_lines(report) != gaps) {
    std::string all;
    for (const auto& line : gap_lines(report)) all += "\n  " + line;
    throw std::runtime_error("the ceiling gaps are not the expected ones:" + all);
  }
  fixed_text_only(report);
  passed("route ceilings: listed bands count, and listed prices left out leave out their side");
}

// OpenRouter lists the batch-priced version of a model as its own catalogue variant, `vendor/model:batch` (#228). The
// policy's `tier_variants` make a refresh write that entry's prices onto `vendor/model` under the tier `batch`, its
// route ceiling (read from the variant's endpoints) included; the variant's entry stays as listed. A model with no
// batch variant gets no batch row, and without the policy fact nothing is written.
void batch_variants_price_the_batch_tier(transport::Transport& transport, Peer& peer) {
  // A base entry listed with 63 prices has no room for the variant's two.
  std::string full_units = "{";
  for (int i = 0; i < 63; ++i) full_units += (i ? "," : "") + std::string("\"unit_") + std::to_string(i) + "\":\"0\"";
  full_units += "}";
  peer.command(std::string(R"({"set":"ceilings-batch","models":{"vendor/batched":[{"prompt":"0.000002","completion":"0.000008"}],)"
               R"("vendor/batched:batch":[{"prompt":"0.000001","completion":"0.000004"},{"prompt":"0.0000012","completion":"0.000003"}],)"
               R"("vendor/sync-only":[{"prompt":"0.000002","completion":"0.000008"}],)"
               R"("vendor/full-base":[{"prompt":"0.000002","completion":"0.000008"}],"vendor/full-base:batch":[{"prompt":"0.000001","completion":"0.000004"}]},)"
               R"("listed":{"vendor/full-base":)" + full_units + R"(,"vendor/full-base:batch":{"prompt":"0.000001","completion":"0.000004"},)"
               R"("vendor/batched":{"prompt":"0.000002","completion":"0.000008"},)"
               R"("vendor/batched:batch":{"prompt":"0.000001","completion":"0.000004","overrides":[{"min_prompt_tokens":200000,"prompt":"0.000002"}]},)"
               R"("vendor/sync-only":{"prompt":"0.000002","completion":"0.000008"}}})"));
  transport::RefreshRequest request;
  request.sources = {ceiling_source(peer, "ceilings-batch", {"vendor/batched", "vendor/batched:batch"})};
  const auto result = refreshed(transport, request);
  require(result.sources[0].ok, "the listing refreshed");
  const auto& c = *result.generated;
  require(pricing_text(c.entry("vendor/batched")) ==
              "ceiling_input=0.000002 ceiling_input/batch=0.000002 ceiling_output=0.000008 ceiling_output/batch=0.000004 completion=0.000008 "
              "completion/batch=0.000004 prompt=0.000002 prompt/batch=0.000001 prompt/batch+200000=0.000002",
          "the batch variant's prices are the batch tier's, not " + pricing_text(c.entry("vendor/batched")));
  require(pricing_text(c.entry("vendor/batched:batch")) ==
              "ceiling_input=0.000002 ceiling_output=0.000004 completion=0.000004 prompt=0.000001 prompt+200000=0.000002",
          "the variant's own entry stays as listed, not " + pricing_text(c.entry("vendor/batched:batch")));
  require(pricing_text(c.entry("vendor/sync-only")) == "completion=0.000008 prompt=0.000002",
          "a model without a batch variant gets no batch row, not " + pricing_text(c.entry("vendor/sync-only")));
  // A base entry that cannot hold the variant's rows gets none of them, and the report names the variant.
  require(pricing_text(c.entry("vendor/full-base")).find("/batch") == std::string::npos, "an entry without room gets no batch row");
  const auto lines = gap_lines(result.sources[0]);
  require(std::find(lines.begin(), lines.end(), "vendor/full-base:batch|||the base entry cannot hold the variant's prices under tier 'batch'") != lines.end(),
          "the report names the variant whose prices were not written");
  // A policy without the fact writes nothing.
  auto text = std::string(config_defaults::descriptor_policy_json);
  const auto at = text.find("\"tier_variants\"");
  require(at != std::string::npos, "the built-in policy states the batch variant");
  const auto comma = text.rfind(',', at), close = text.find('}', at);
  text.erase(comma, close + 1 - comma);
  auto loaded = descriptor::load_policy(text, config_defaults::codec_defaults_json);
  require(std::holds_alternative<descriptor::PolicySnapshot>(loaded), "the policy without the fact loads");
  request.policy = std::get<descriptor::PolicySnapshot>(loaded);
  const auto without = refreshed(transport, request);
  require(pricing_text(without.generated->entry("vendor/batched")) == "ceiling_input=0.000002 ceiling_output=0.000008 completion=0.000008 prompt=0.000002",
          "without the policy fact nothing is written, not " + pricing_text(without.generated->entry("vendor/batched")));
  passed("batch variants: the variant's prices and route ceiling are the batch tier of the base model");
}

// The canonical text of an entry's endpoint rows, or "-" when it writes none.
std::string rows_text(const descriptor::CatalogueSnapshot& catalogue, std::string_view id) {
  const auto text = catalogue->to_json();
  const auto at = text.find("{\"id\":" + json::quote(id) + ",");
  if (at == std::string::npos) return "no entry";
  const auto next = text.find("{\"id\":", at + 1);
  const auto entry = text.substr(at, next == std::string::npos ? std::string::npos : next - at);
  const auto rows = entry.find(",\"endpoints\":");
  if (rows == std::string::npos) return "-";
  // The array up to its own closing bracket; no text the rows hold has a bracket.
  const auto open = rows + 13;
  std::size_t depth = 0, end = open;
  for (; end < entry.size(); ++end) {
    if (entry[end] == '[' || entry[end] == '{') ++depth;
    else if ((entry[end] == ']' || entry[end] == '}') && --depth == 0) break;
  }
  return entry.substr(open, end + 1 - open);
}
// The rows the endpoints listings of the models asked for give: one per tag, from listings recorded whole.
void endpoint_rows_from_the_endpoints_listings(transport::Transport& transport, Peer& peer) {
  const std::vector<std::string> asked{"openai/gpt-5-nano", "openai/gpt-5-nano:batch", "vendor/two-tags", "vendor/bad-row-discount",
                                       "vendor/untagged", "vendor/same-tag", "vendor/flaky", "vendor/no-prompt"};
  transport::RefreshRequest request;
  request.sources = {ceiling_source(peer, "ceilings-rows", asked)};
  const auto result = refreshed(transport, request);
  require(result.sources[0].ok, "the listing refreshed");
  const auto& c = result.generated;
  const std::string chat_parameters =
      R"("supported_parameters":["include_reasoning","max_tokens","reasoning","reasoning_effort","response_format","seed","structured_outputs",)"
      R"("tool_choice","tools","verbosity"])";
  // The recorded listing: the flex tier and the standard endpoint of one provider are two rows, keyed by tag. The
  // performance statistics and the members no row holds are left out; a null member is not written.
  const std::string nano =
      R"([{"tag":"azure","provider_name":"Azure","quantization":"unknown","status":0,"context_length":400000,"max_completion_tokens":360000,)"
      R"("supported_parameters":["include_reasoning","max_completion_tokens","reasoning","reasoning_effort","response_format","seed",)"
      R"("structured_outputs","tool_choice","tools","verbosity"],"discount":0,)"
      R"("pricing":{"completion":"0.0000004","input_cache_read":"0.00000001","prompt":"0.00000005","web_search":"0.01"}},)"
      R"({"tag":"openai","provider_name":"OpenAI","quantization":"unknown","status":0,"context_length":400000,"max_prompt_tokens":272000,)"
      R"("max_completion_tokens":128000,)" + chat_parameters + R"(,"discount":0,)"
      R"("pricing":{"completion":"0.0000004","input_cache_read":"0.000000005","prompt":"0.00000005","web_search":"0.01"}},)"
      R"({"tag":"openai/flex","provider_name":"OpenAI","quantization":"unknown","service_tier":"flex","status":0,"context_length":400000,)"
      R"("max_prompt_tokens":272000,"max_completion_tokens":128000,)" + chat_parameters + R"(,"discount":0,)"
      R"("pricing":{"completion":"0.0000002","input_cache_read":"0.0000000025","prompt":"0.000000025","web_search":"0.01"}},)";
  // The batch variant's row is the base entry's under the tier, as its prices are; the variant keeps its own.
  const std::string batch_row =
      R"("provider_name":"OpenAI","quantization":"unknown","status":0,"context_length":400000,"max_completion_tokens":128000,)" + chat_parameters +
      R"(,"discount":0,"pricing":{"completion":"0.0000002","input_cache_read":"0.0000000025","prompt":"0.000000025","web_search":"0.01"}})";
  require(rows_text(c, "openai/gpt-5-nano") == nano + R"({"tag":"openai","tier":"batch",)" + batch_row + "]",
          "the recorded endpoints are rows by tag, the batch tier's beside them, not " + rows_text(c, "openai/gpt-5-nano"));
  require(rows_text(c, "openai/gpt-5-nano:batch") == R"([{"tag":"openai",)" + batch_row + "]",
          "the variant's own entry keeps its row, not " + rows_text(c, "openai/gpt-5-nano:batch"));
  // One provider under two tags; a long-context band is held as the entry's prices are, and the discount as listed.
  require(rows_text(c, "vendor/two-tags") ==
              R"([{"tag":"deepinfra/fp8","provider_name":"DeepInfra","quantization":"fp8","status":0,"context_length":400000,"max_completion_tokens":128000,)" +
                  chat_parameters + R"(,"discount":0,"pricing":{"completion":"0.0000008","prompt":"0.0000002","prompt+128000":"0.0000004","web_search":"0.01"}},)"
                  R"({"tag":"deepinfra/turbo","provider_name":"DeepInfra","quantization":"fp8","service_tier":"priority","status":-2,"context_length":400000,)"
                  R"("max_prompt_tokens":272000,"max_completion_tokens":128000,)" +
                  chat_parameters + R"(,"discount":0.25,"pricing":{"completion":"0.0000016","prompt":"0.0000004","web_search":"0.01"}}])",
          "two tags of one provider are two rows, not " + rows_text(c, "vendor/two-tags"));
  // A row whose discount may raise its prices keeps its tag and loses its prices, so the set stays complete.
  require(rows_text(c, "vendor/bad-row-discount") ==
              R"([{"tag":"alpha","provider_name":"Alpha","quantization":"unknown","status":0,"context_length":400000,"max_prompt_tokens":272000,)"
              R"("max_completion_tokens":128000,)" + chat_parameters + R"(,"discount":0,"pricing":{"completion":"0.000002","prompt":"0.000001","web_search":"0.01"}},)"
              R"({"tag":"beta","provider_name":"Beta","quantization":"unknown","status":0,"context_length":400000,"max_prompt_tokens":272000,)"
              R"("max_completion_tokens":128000,)" + chat_parameters + "}]",
          "an unusable discount leaves the row without prices, not " + rows_text(c, "vendor/bad-row-discount"));
  // An endpoint without a tag, or a tag twice, leaves the model without rows.
  require(rows_text(c, "vendor/untagged") == "-" && rows_text(c, "vendor/same-tag") == "-", "an incomplete set writes no row");
  require(rows_text(c, "vendor/unasked") == "-", "no rows for a model not asked for");
  // The report names every model without rows and every row without prices, with fixed text.
  const auto& report = result.sources[0];
  std::vector<std::string> lines;
  for (const auto& gap : report.endpoint_gaps) lines.push_back(gap.model + "|" + gap.tag + "|" + gap.price + "|" + gap.reason);
  const std::vector<std::string> expected{"vendor/bad-row-discount|beta|discount|discount at an endpoint is not from 0 to 1",
                                          "vendor/no-prompt|alpha|prompt|price missing at an endpoint",
                                          "vendor/no-prompt|beta|prompt|price at an endpoint is not decimal text",
                                          "vendor/same-tag|||two endpoints share a tag",
                                          "vendor/untagged|||an endpoint has no tag the catalogue can hold"};
  if (lines != expected) {
    std::string all;
    for (const auto& line : lines) all += "\n  " + line;
    throw std::runtime_error("the endpoint gaps are not the expected ones:" + all);
  }
  require(report.endpoint_rows == 6, "six models written: both openai entries, two-tags, bad-row-discount, flaky and no-prompt");
  // A row without a price every endpoint lists keeps its tag and its other prices.
  const auto unpriced = c->endpoints("vendor/no-prompt");
  require(unpriced.size() == 2 && unpriced[0].tag == "alpha" && unpriced[1].tag == "beta" &&
              unpriced[0].pricing == (std::vector<std::pair<std::string, std::string>>{{"completion", "0.000002"}, {"web_search", "0.01"}}) &&
              unpriced[1].pricing == unpriced[0].pricing,
          "rows without a readable prompt price keep their tags");
  // The accessor reads one tier's rows of the exact entry, by tag.
  const auto standard = c->endpoints("openai/gpt-5-nano");
  const auto batch = c->endpoints("openai/gpt-5-nano", "batch");
  require(standard.size() == 3 && standard[0].tag == "azure" && standard[1].tag == "openai" && standard[2].tag == "openai/flex" &&
              standard[2].service_tier == std::optional<std::string>("flex") && standard[0].max_prompt_tokens == std::nullopt &&
              standard[0].max_completion_tokens == std::optional<std::uint64_t>(360000),
          "the model's own rows, by tag");
  require(batch.size() == 1 && batch[0].tag == "openai" && batch[0].tier == "batch", "the batch tier's rows");
  require(c->endpoints("openai/gpt-5-nano", "flex").empty() && c->endpoints("vendor/absent").empty() && c->endpoints("vendor/untagged").empty(),
          "no rows for a tier, a model or an entry that states none");
  const auto turbo = c->endpoints("vendor/two-tags");
  require(turbo.size() == 2 && turbo[1].tag == "deepinfra/turbo" && turbo[1].status == std::optional<std::int64_t>(-2) &&
              turbo[1].discount == std::optional<double>(0.25) && turbo[1].provider_name == std::optional<std::string>("DeepInfra"),
          "a row's status and discount as listed");
  // The rows read back.
  auto again = descriptor::load_catalogue(c->to_json());
  require(std::holds_alternative<descriptor::CatalogueSnapshot>(again) && std::get<descriptor::CatalogueSnapshot>(again)->digest() == c->digest(),
          "the generated rows read back as the same catalogue");

  // A failed endpoints request keeps the previous ceiling and writes no row: a listed entry carries no earlier row.
  transport::RefreshRequest later;
  later.previous = c;
  later.sources = {ceiling_source(peer, "ceilings-rows-v2", {"vendor/flaky", "vendor/two-tags"})};
  const auto second = refreshed(transport, later);
  require(second.sources[0].ok, "the second listing refreshed");
  require(rows_text(c, "vendor/flaky") != "-" && rows_text(second.generated, "vendor/flaky") == "-" &&
              ceiling_of(second.generated->entry("vendor/flaky")) == ceiling_of(c->entry("vendor/flaky")),
          "an outage keeps the ceiling and drops the rows");
  require(rows_text(second.generated, "vendor/two-tags") == rows_text(c, "vendor/two-tags"), "a listing read again gives the same rows");
  require(second.sources[0].endpoint_rows == 1 && second.sources[0].endpoint_gaps.size() == 1 &&
              second.sources[0].endpoint_gaps[0].model == "vendor/flaky" && second.sources[0].endpoint_gaps[0].tag.empty() &&
              second.sources[0].endpoint_gaps[0].reason == "no endpoints listing read in this refresh",
          "the model without rows is named");
  for (const auto& gap : second.sources[0].endpoint_gaps)
    for (const std::string_view banned : {"127.0.0.1", "http", "Bearer", "openrouter-test-key", "synthetic"})
      require(gap.reason.find(banned) == std::string::npos, "an endpoint gap carries fixed text only");
  passed("endpoint rows: one per tag from recorded listings, batch tier, unusable discounts, incomplete sets, failures");
}

#ifndef _WIN32
// sp_catalogue_refresh --ceiling reads the route ceilings of the models it names and prints what it could not.
void the_tool_reads_route_ceilings(Peer& peer, const std::string& tool) {
  setenv("OPENROUTER_API_KEY", "openrouter-test-key", 1);
  const auto dir = std::filesystem::temp_directory_path() / ("sp_catalogue_refresh_ceilings_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto out = (dir / "models.json").string(), printed = (dir / "printed.txt").string();
  const std::vector<std::string> common{"--source", "openrouter", "--url", "openrouter=" + base(peer, "ceilings-tool"), "--out", out};
  auto with = [&](std::vector<std::string> more) {
    auto arguments = common;
    arguments.insert(arguments.end(), more.begin(), more.end());
    return arguments;
  };
  require(run_tool(tool, with({"--ceiling", "vendor/many", "--ceiling", "vendor/negative", "--ceiling", "vendor/absent"}), printed) == 0,
          "a refresh with ceilings exits 0");
  auto loaded = descriptor::load_catalogue_file(out);
  require(std::holds_alternative<descriptor::CatalogueSnapshot>(loaded), "the tool wrote a catalogue that loads");
  const auto& written = *std::get<descriptor::CatalogueSnapshot>(loaded);
  require(ceiling_of(written.entry("vendor/many")) == "0.000003 0.000005 -" && ceiling_of(written.entry("vendor/negative")) == "- 0.000002 -",
          "the tool writes the ceilings");
  const auto text = contents(printed);
  for (const std::string_view line : {"openrouter ceilings: 2 request(s), 1 written whole, 2 gap(s)\n",
                                      "openrouter ceiling vendor/absent: no ceiling (not in the listing)\n",
                                      "openrouter ceiling vendor/negative: ceiling_input not written (prompt: variable price at an endpoint)\n"})
    require(text.find(line) != std::string::npos, "the tool prints the ceiling report: " + std::string(line) + " in:\n" + text);
  require(seen(peer, "ceilings-tool").endpoints == (std::vector<std::string>{"vendor/many", "vendor/negative"}), "the tool asks for the named models only");
  require(run_tool(tool, with({"--ceiling", "../escape"})) == 2, "an id that is not author/slug is refused");
  require(run_tool(tool, {"--source", "openai", "--url", "openai=" + base(peer, "ceilings-tool"), "--out", out, "--ceiling", "vendor/many"}) == 2,
          "--ceiling needs the OpenRouter source");
  require(seen(peer, "ceilings-tool").total == 3, "a refused request sends nothing");
  std::filesystem::remove_all(dir);
  passed("route ceilings: the command-line tool");
}

// The tool names a unit left out, and prints no listed key that is not a unit name.
void the_tool_prints_no_listed_key_text(Peer& peer, const std::string& tool) {
  const auto dir = std::filesystem::temp_directory_path() / ("sp_catalogue_refresh_keys_" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto out = (dir / "models.json").string(), printed = (dir / "printed.txt").string();
  require(run_tool(tool, {"--source", "openrouter", "--url", "openrouter=" + base(peer, "overrides-keys"), "--out", out}, printed) == 0,
          "the refresh exits 0");
  const auto text = contents(printed);
  require(text.find("left out vendor/odd-keys: prompt") != std::string::npos, "the tool names the unit left out:\n" + text);
  for (const std::string_view banned : {"secret", "example.test", "https", "@"})
    require(text.find(banned) == std::string::npos, "the tool prints no listed key text:\n" + text);
  require(contents(out).find("secret") == std::string::npos, "the written catalogue holds no listed key text");
  std::filesystem::remove_all(dir);
  passed("listing keys: the command-line tool prints none of them");
}
#endif
} // namespace

int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) { std::cerr << "usage: sp_catalogue_refresh_tests <node> <listing_server.mjs> [sp_catalogue_refresh]\n"; return 2; }
  try {
    Peer peer(argv[1], argv[2]);
    transport::Transport transport;
    descriptor::CatalogueSnapshot generated;
    no_ceilings_change_nothing(transport, peer);
    listing_headers_are_pinned(transport, peer);
    listing_headers_follow_the_policy(transport, peer);
    overrides_become_bands(transport, peer);
    windows_give_one_entry_at_every_hour(transport, peer);
    listing_keys_are_unit_names(transport, peer);
    all_four_listings(transport, peer, generated);
    failures_keep_previous_entries(transport, peer, generated);
    empty_and_partial_listings_fail_their_source(transport, peer, generated);
    shrunk_listings_fail_their_source(transport, peer);
    oversized_listings_fail_their_source(transport, peer);
    a_deadline_bounds_the_whole_refresh(transport, peer);
    shared_ids_leave_with_their_sources(transport, peer);
#ifndef _WIN32
    if (argc == 4) the_tool_keeps_its_files_when_a_source_fails(peer, argv[3]);
#endif
    invalid_requests(transport);
    embedded_facts_apply_at_every_origin(transport, peer);
    descriptor::CatalogueSnapshot ceilings;
    ceilings_from_the_endpoints_listings(transport, peer, ceilings);
    failed_endpoints_keep_previous_ceilings(transport, peer, ceilings);
    ceiling_requests_are_checked(transport, peer);
    ceilings_bound_every_endpoint(transport, peer);
    ceilings_count_listed_bands(transport, peer);
    default_origins_are_policy_data(transport, peer);
    batch_variants_price_the_batch_tier(transport, peer);
    endpoint_rows_from_the_endpoints_listings(transport, peer);
#ifndef _WIN32
    if (argc == 4) the_tool_reads_route_ceilings(peer, argv[3]);
    if (argc == 4) the_tool_prints_no_listed_key_text(peer, argv[3]);
#endif
    std::cout << "catalogue refresh properties passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "catalogue refresh test failed: " << error.what() << '\n';
    return 1;
  }
}
