#include "core/pricing.h"
#include "descriptor/catalogue.h"
#include "descriptor/policy.h"
#include "sp/config_defaults.h"
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
};
Seen seen(Peer& peer, std::string_view scenario) {
  const auto answer = peer.command("{\"requests\":" + json::quote(scenario) + "}");
  const auto root = answer.root();
  Seen out;
  out.total = root.get("total").as_uint();
  for (auto id : root.get("endpoints").elements()) out.endpoints.emplace_back(id.as_string());
  return out;
}
void passed(std::string_view name) { std::cout << "case passed: " << name << std::endl; }

// What a refresh of the `openrouter` listing generated before a refresh could read route ceilings. A refresh that
// asks for no ceilings still generates exactly these bytes, and sends the listing request only.
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
void no_ceilings_change_nothing(transport::Transport& transport, Peer& peer) {
  transport::RefreshRequest request;
  request.sources = {source(Listing::OpenRouter, base(peer, "openrouter-pinned"))};
  const auto result = refreshed(transport, request);
  require(result.sources.size() == 1 && result.sources[0].ok && result.sources[0].pages == 1, "the pinned listing refreshed");
  const auto text = result.generated->to_json();
  require(text == openrouter_generated_text, "a refresh without ceilings generates the pinned text, not " + text);
  require(result.generated->digest_hex() == openrouter_generated_sha256,
          "a refresh without ceilings generates the pinned digest, not " + result.generated->digest_hex());
  const auto requests = seen(peer, "openrouter-pinned");
  require(requests.total == 1 && requests.endpoints.empty(), "a refresh without ceilings sends the listing request only");
  require(result.sources[0].ceiling_requests == 0 && result.sources[0].ceilings == 0 && result.sources[0].ceiling_gaps.empty(),
          "a refresh without ceilings reports none");
  passed("no ceilings asked: the listing request only, and the pinned text and digest");
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
    "vendor/outage", "vendor/full", "vendor/missing-fee", "vendor/absent"};
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
  require(ceiling_of(c.entry("vendor/bad-discount")) == "- - -", "a discount that raises the price leaves out every unit");
  for (const char* id : {"~vendor/alias-latest", "vendor/malformed", "vendor/override-shape", "vendor/wrong-id", "vendor/not-json", "vendor/full", "vendor/unasked"})
    require(c.entry(id) && ceiling_of(c.entry(id)) == "- - -", "no ceiling for an alias, a malformed listing, a full entry or a model not asked for");
  require(!c.entry("vendor/absent"), "an id the listing does not name gets no entry");
  require(c.entry("vendor/full")->pricing.size() == 64, "the full entry keeps its listed prices");
  require(unit_price(c.entry("vendor/many"), "prompt") == std::optional<std::string>("0.000001"), "listed prices stay beside the ceiling");

  // Every gap is named, by model id in order, with fixed text.
  const std::vector<std::string> gaps{
      "vendor/absent|||not in the listing",
      "vendor/bad-cache|ceiling_input|input_cache_write|variable price at an endpoint",
      "vendor/bad-discount||discount|discount at an endpoint is not from 0 to 1",
      "vendor/bad-fee|ceiling_request|request|variable price at an endpoint",
      "vendor/bad-fee|||no ceiling without every request fee",
      "vendor/bad-override|ceiling_output|completion|price at an endpoint is not decimal text",
      "vendor/full|||the entry cannot hold the ceiling",
      "vendor/malformed|||not an endpoints listing",
      "vendor/missing-fee|ceiling_request|request|price missing at an endpoint",
      "vendor/missing-fee|||no ceiling without every request fee",
      "vendor/missing-prompt|ceiling_input|prompt|price missing at an endpoint",
      "vendor/negative|ceiling_input|prompt|variable price at an endpoint",
      "vendor/not-decimal|ceiling_output|completion|price at an endpoint is not decimal text",
      "vendor/not-json|||not an endpoints listing",
      "vendor/number|ceiling_input|prompt|price at an endpoint is not decimal text",
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
  require(report.ceilings == 6 && report.ceiling_requests == 22, "six ceilings written whole from 22 endpoints requests");
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
  require(report.ok && report.ceiling_requests == 22, "a failed endpoints request does not fail the listing");
  fixed_text_only(report);
  const auto& c = *result.generated;
  require(ceiling_of(c.entry("vendor/outage")) == "0.000001 0.000002 0.0001", "an outage keeps the previous ceiling");
  require(ceiling_of(c.entry("vendor/stall")) == "0.000002 0.000003 -", "a timeout keeps the previous ceiling");
  require(ceiling_of(c.entry("vendor/many")) == "0.000003 0.000006 -", "a changed endpoint price changes the ceiling");
  require(ceiling_of(c.entry("vendor/fee")) == "- - -", "an answer that is not a listing drops the previous ceiling");
  const auto lines = gap_lines(report);
  require(has(lines, "vendor/outage|||endpoints answered HTTP 500|kept") && has(lines, "vendor/stall|||endpoints request did not complete|kept") &&
          has(lines, "vendor/fee|||not an endpoints listing"), "the failed models are named, and whether the previous ceiling was kept");
  require(report.ceilings == 3, "vendor/many, vendor/zero-fee and vendor/cached are written whole; the kept and the dropped ones are not");
  // The diff reports a changed ceiling the way it reports a changed price.
  require(result.diff.changed == (std::vector<std::string>{"vendor/fee", "vendor/many"}) && result.diff.added.empty() && result.diff.removed.empty(),
          "the diff names the entries whose ceiling changed, and only those");

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
                             {"internal_reasoning", 1, "internal_reasoning"}, {"audio_output", 1, "audio_output"}, {"request", 2, "request"}};
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
    std::string command = "{\"set\":" + json::quote(scenario) + ",\"models\":{";
    for (int m = 0; m < 24; ++m) {
      const auto id = "prop/m-" + std::to_string(round) + "-" + std::to_string(m);
      auto& endpoints = generated[id];
      command += (m ? "," : "") + json::quote(id) + ":[";
      const auto count = 1 + pick(6);
      const bool fees = pick(2) == 0;  // every endpoint lists a request fee, or none does
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
      std::vector<Prices> conditions;  // every price set one endpoint can bill under
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
            require(member.side == 2 || *ceiling[member.side] >= *rate, "a ceiling is never below an endpoint's price");
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
#endif
} // namespace

int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) { std::cerr << "usage: sp_catalogue_refresh_tests <node> <listing_server.mjs> [sp_catalogue_refresh]\n"; return 2; }
  try {
    Peer peer(argv[1], argv[2]);
    transport::Transport transport;
    descriptor::CatalogueSnapshot generated;
    no_ceilings_change_nothing(transport, peer);
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
#ifndef _WIN32
    if (argc == 4) the_tool_reads_route_ceilings(peer, argv[3]);
#endif
    std::cout << "catalogue refresh properties passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "catalogue refresh test failed: " << error.what() << '\n';
    return 1;
  }
}
