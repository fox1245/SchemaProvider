// sp_catalogue_refresh: builds a model catalogue from the provider listings and writes it to a
// file, for a host or a packager that refreshes by hand or from a scheduled job.
//
//   sp_catalogue_refresh --out models.json [--previous generated.json] [--overlay file]...
//                        [--builtin-overlay] [--source openrouter|gemini|anthropic|openai]...
//
// Keys come from OPENROUTER_API_KEY (optional), GEMINI_API_KEY, ANTHROPIC_API_KEY and
// OPENAI_API_KEY. Without --source, OpenRouter and every provider with a key are listed. With
// --generated, the provider facts alone are written there as well, to pass as --previous next time.
// Keys are never printed.
#include "descriptor/catalogue.h"
#include "transport/catalogue_refresh.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
bool write(const std::string& path, const sp::descriptor::ModelCatalogue& catalogue) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << catalogue.to_json() << '\n';
  return static_cast<bool>(out);
}
} // namespace

int main(int argc, char** argv) {
  std::string out, generated_out;
  sp::transport::RefreshRequest request;
  std::vector<Listing> chosen;
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
    else if (arg == "--source") {
      const auto name = value();
      if (name == "openrouter") chosen.push_back(Listing::OpenRouter);
      else if (name == "gemini") chosen.push_back(Listing::Gemini);
      else if (name == "anthropic") chosen.push_back(Listing::Anthropic);
      else if (name == "openai") chosen.push_back(Listing::OpenAI);
      else { std::cerr << "unknown source " << name << '\n'; return 2; }
    } else { std::cerr << "unknown argument " << arg << '\n'; return 2; }
  }
  if (out.empty()) { std::cerr << "--out is required\n"; return 2; }
  const struct { Listing listing; const char* variable; } keys[]{{Listing::OpenRouter, "OPENROUTER_API_KEY"},
      {Listing::Gemini, "GEMINI_API_KEY"}, {Listing::Anthropic, "ANTHROPIC_API_KEY"}, {Listing::OpenAI, "OPENAI_API_KEY"}};
  for (const auto& [listing, variable] : keys) {
    const bool wanted = chosen.empty() ? (listing == Listing::OpenRouter || !key(variable).empty())
                                       : std::find(chosen.begin(), chosen.end(), listing) != chosen.end();
    if (wanted) request.sources.push_back({listing, {}, key(variable), {}});
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
              << report.models << " model(s), " << report.skipped << " skipped\n";
    all_ok = all_ok && report.ok;
  }
  std::cout << "diff: " << refreshed.diff.added.size() << " added, " << refreshed.diff.removed.size() << " removed, "
            << refreshed.diff.changed.size() << " changed\n";
  if (!generated_out.empty() && !write(generated_out, *refreshed.generated)) { std::cerr << "could not write " << generated_out << '\n'; return 1; }
  if (!write(out, *refreshed.catalogue)) { std::cerr << "could not write " << out << '\n'; return 1; }
  std::cout << "wrote " << refreshed.catalogue->entries().size() << " entries, sha256 " << refreshed.catalogue->digest_hex() << '\n';
  return all_ok ? 0 : 3;
}
