#include "descriptor/catalogue.h"
#include "descriptor/policy.h"
#include "support/runtime_peer.h"
#include "transport/catalogue_refresh.h"
#include <algorithm>
#include <iostream>
#include <string>

namespace {
using namespace sp;
using descriptor::Listing;
using runtime_test::Peer;
using runtime_test::require;

std::string base(const Peer& peer, std::string_view scenario) {
  return "http://127.0.0.1:" + std::to_string(peer.port) + "/" + std::string(scenario);
}
transport::ListingSource source(Listing listing, std::string url, std::string key = {}) {
  return {listing, std::move(url), std::move(key), {}};
}
transport::RefreshResult refreshed(transport::Transport& transport, const transport::RefreshRequest& request) {
  auto result = transport::refresh_catalogue(transport, request);
  if (auto* error = std::get_if<descriptor::ConfigError>(&result)) throw std::runtime_error("refresh refused at " + error->pointer);
  return std::get<transport::RefreshResult>(std::move(result));
}
bool has(const std::vector<std::string>& values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
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
  require(text->pricing == (std::vector<std::pair<std::string, std::string>>{{"completion", "0.0000004"}, {"prompt", "0.0000001"}}),
          "string prices kept, structured overrides left out");
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
  refused_at({Listing::OpenAI, {}, "k", "https://api.example.test/path"}, "/sources/0/origin");
  refused_at(source(Listing::OpenAI, {}, "line\nbreak"), "/sources/0/api_key");
  require(transport::default_listing_url(Listing::Gemini) == "https://generativelanguage.googleapis.com", "default listing URL");
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
} // namespace

int main(int argc, char** argv) {
  if (argc != 3) { std::cerr << "usage: sp_catalogue_refresh_tests <node> <listing_server.mjs>\n"; return 2; }
  try {
    Peer peer(argv[1], argv[2]);
    transport::Transport transport;
    descriptor::CatalogueSnapshot generated;
    all_four_listings(transport, peer, generated);
    failures_keep_previous_entries(transport, peer, generated);
    invalid_requests(transport);
    embedded_facts_apply_at_every_origin(transport, peer);
    std::cout << "catalogue refresh properties passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "catalogue refresh test failed: " << error.what() << '\n';
    return 1;
  }
}
