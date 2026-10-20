// sp_catalogue_refresh: builds a model catalogue from the provider listings and writes it to a
// file, for a host or a packager that refreshes by hand or from a scheduled job.
//
//   sp_catalogue_refresh --out models.json [--previous generated.json] [--overlay file]...
//                        [--builtin-overlay] [--source openrouter|gemini|anthropic|openai]...
//                        [--url source=url]... [--min-kept-percent n] [--allow-drop] [--write-partial]
//
// Keys come from OPENROUTER_API_KEY (optional), GEMINI_API_KEY, ANTHROPIC_API_KEY and
// OPENAI_API_KEY. Without --source, OpenRouter and every provider with a key are listed. With
// --generated, the provider facts alone are written there as well, to pass as --previous next time.
// --url serves a source's listing from another URL (a mirror; https, or http on loopback).
// A source that fails exits 3 and writes nothing, so a scheduled job keeps the file it had during
// an outage; --write-partial writes the catalogue anyway, with the previous entries of the failed
// sources. A source whose listing names no models, or fewer than --min-kept-percent percent (default
// 50) of the entries --previous has from it, fails; --allow-drop accepts such a listing.
// A file is replaced whole or not at all. Keys are never printed.
#include "descriptor/catalogue.h"
#include "transport/catalogue_refresh.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
using sp::descriptor::Listing;
std::string key(const char* name) {
  const char* value = std::getenv(name);
  std::string out = value ? value : "";
  while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}
sp::descriptor::CatalogueSnapshot load(const std::string& path) {
  auto loaded = sp::descriptor::load_catalogue_file(path);
  if (auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded)) {
    std::cerr << path << ": catalogue refused at '" << error->pointer << "': " << error->expected << '\n';
    std::exit(2);
  }
  return std::get<sp::descriptor::CatalogueSnapshot>(std::move(loaded));
}
// A file written next to its destination, to be renamed over it once every file is complete.
struct Staged {
  std::string path, temporary;
};
bool stage(Staged& file, const std::string& path, const sp::descriptor::ModelCatalogue& catalogue) {
  file.path = path;
  file.temporary = path + ".tmp";
  std::ofstream out(file.temporary, std::ios::binary | std::ios::trunc);
  out << catalogue.to_json() << '\n';
  out.flush();
  return static_cast<bool>(out);
}
void discard(const std::vector<Staged>& files) {
  std::error_code ignored;
  for (const auto& file : files) if (!file.temporary.empty()) std::filesystem::remove(file.temporary, ignored);
}
bool commit(const std::vector<Staged>& files) {
  for (const auto& file : files) {
    std::error_code error;
    std::filesystem::rename(file.temporary, file.path, error);
    if (error) return false;
  }
  return true;
}
} // namespace

int main(int argc, char** argv) {
  std::string out, generated_out;
  bool write_partial = false;
  sp::transport::RefreshRequest request;
  std::vector<Listing> chosen;
  std::map<Listing, std::string> urls;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) { std::cerr << arg << " needs a value\n"; std::exit(2); }
      return argv[++i];
    };
    if (arg == "--out") out = value();
    else if (arg == "--generated") generated_out = value();
    else if (arg == "--previous") request.previous = load(value());
    else if (arg == "--overlay") request.overlays.push_back(load(value()));
    else if (arg == "--builtin-overlay") request.overlays.push_back(sp::descriptor::builtin_catalogue());
    else if (arg == "--allow-drop") request.allow_drop = true;
    else if (arg == "--min-kept-percent") {
      const auto text = value();
      char* end = nullptr;
      const auto percent = std::strtoul(text.c_str(), &end, 10);
      if (text.empty() || *end != '\0' || percent > 100) { std::cerr << "--min-kept-percent needs a number from 0 to 100\n"; return 2; }
      request.min_kept_percent = static_cast<std::uint32_t>(percent);
    }
    else if (arg == "--write-partial") write_partial = true;
    else if (arg == "--source" || arg == "--url") {
      auto name = value();
      std::string url;
      if (arg == "--url") {
        const auto cut = name.find('=');
        if (cut == std::string::npos) { std::cerr << "--url needs source=url\n"; return 2; }
        url = name.substr(cut + 1);
        name.resize(cut);
      }
      Listing listing = Listing::OpenRouter;
      if (name == "openrouter") listing = Listing::OpenRouter;
      else if (name == "gemini") listing = Listing::Gemini;
      else if (name == "anthropic") listing = Listing::Anthropic;
      else if (name == "openai") listing = Listing::OpenAI;
      else { std::cerr << "unknown source " << name << '\n'; return 2; }
      if (arg == "--url") urls[listing] = url;
      else chosen.push_back(listing);
    } else { std::cerr << "unknown argument " << arg << '\n'; return 2; }
  }
  if (out.empty()) { std::cerr << "--out is required\n"; return 2; }
  const struct { Listing listing; const char* variable; } keys[]{{Listing::OpenRouter, "OPENROUTER_API_KEY"},
      {Listing::Gemini, "GEMINI_API_KEY"}, {Listing::Anthropic, "ANTHROPIC_API_KEY"}, {Listing::OpenAI, "OPENAI_API_KEY"}};
  for (const auto& [listing, variable] : keys) {
    const bool wanted = chosen.empty() ? (listing == Listing::OpenRouter || !key(variable).empty())
                                       : std::find(chosen.begin(), chosen.end(), listing) != chosen.end();
    if (wanted) request.sources.push_back({listing, urls[listing], key(variable), {}});
  }

  sp::transport::Transport transport;
  auto result = sp::transport::refresh_catalogue(transport, request);
  if (auto* error = std::get_if<sp::descriptor::ConfigError>(&result)) {
    std::cerr << "refresh refused at '" << error->pointer << "': " << error->expected << '\n';
    return 2;
  }
  const auto& refreshed = std::get<sp::transport::RefreshResult>(result);
  bool all_ok = true;
  for (const auto& report : refreshed.sources) {
    std::cout << sp::descriptor::listing_name(report.listing) << ": "
              << (report.ok ? "ok" : "failed (" + report.error + ")") << ", " << report.pages << " page(s), "
              << report.models << " model(s), " << report.skipped << " skipped, " << report.skipped_prices
              << " price(s) skipped\n";
    all_ok = all_ok && report.ok;
  }
  std::cout << "diff: " << refreshed.diff.added.size() << " added, " << refreshed.diff.removed.size() << " removed, "
            << refreshed.diff.changed.size() << " changed\n";
  if (!all_ok && !write_partial) {
    std::cout << "a source failed: nothing written (--write-partial writes the catalogue with the previous entries of the failed sources)\n";
    return 3;
  }
  std::vector<Staged> files(generated_out.empty() ? 1 : 2);
  const bool staged = (generated_out.empty() || stage(files[0], generated_out, *refreshed.generated)) &&
                      stage(files.back(), out, *refreshed.catalogue);
  if (!staged || !commit(files)) {
    discard(files);
    std::cerr << "could not write " << (generated_out.empty() ? out : generated_out + " or " + out) << '\n';
    return 1;
  }
  std::cout << "wrote " << refreshed.catalogue->entries().size() << " entries, sha256 " << refreshed.catalogue->digest_hex() << '\n';
  return all_ok ? 0 : 3;
}
