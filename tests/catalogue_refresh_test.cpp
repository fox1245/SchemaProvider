#include "descriptor/catalogue.h"
#include "descriptor/policy.h"
#include "support/runtime_peer.h"
#include "transport/catalogue_refresh.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
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
  request.sources = {{Listing::OpenAI, base(peer, "openai-wide-a"), "openai-test-key", "https://a.example.test"},
                     {Listing::OpenAI, base(peer, "openai-wide-b"), "openai-test-key", "https://b.example.test"},
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
    return std::vector<transport::ListingSource>{{Listing::OpenAI, base(peer, openai), "openai-test-key", "https://gw-a.example.test"},
                                                 {Listing::Gemini, base(peer, gemini), "gemini-test-key", "https://gw-b.example.test"}};
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
int run_tool(const std::string& tool, std::vector<std::string> arguments) {
  std::vector<std::string> words{tool};
  words.insert(words.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& word : words) argv.push_back(word.data());
  argv.push_back(nullptr);
  const auto pid = fork();
  require(pid >= 0, "fork failed");
  if (pid == 0) {
    ::freopen("/dev/null", "w", stdout);
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
  if (argc != 3 && argc != 4) { std::cerr << "usage: sp_catalogue_refresh_tests <node> <listing_server.mjs> [sp_catalogue_refresh]\n"; return 2; }
  try {
    Peer peer(argv[1], argv[2]);
    transport::Transport transport;
    descriptor::CatalogueSnapshot generated;
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
    std::cout << "catalogue refresh properties passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "catalogue refresh test failed: " << error.what() << '\n';
    return 1;
  }
}
