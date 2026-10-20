#include "transport/catalogue_refresh.h"
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

namespace sp::transport {
namespace {
using descriptor::CatalogueEntry;
using descriptor::CatalogueSnapshot;
using descriptor::ConfigError;
using descriptor::Listing;

constexpr std::size_t max_page_bytes = 32u * 1024u * 1024u;
constexpr std::size_t max_pages = 64;

// The credential leaves only over TLS, except to a loopback test server.
bool allowed_url(std::string_view url) {
  if (url.empty() || url.size() > 2048) return false;
  if (std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 0x20 || c >= 0x7f || c == '?' || c == '#'; })) return false;
  if (url.starts_with("https://")) return url.size() > 8;
  for (const std::string_view host : {"http://127.0.0.1", "http://localhost", "http://[::1]"})
    if (url.starts_with(host) && (url.size() == host.size() || url[host.size()] == ':' || url[host.size()] == '/')) return true;
  return false;
}
// The time `wait` after `now`; a wait too long to represent is cut to a day.
std::chrono::steady_clock::time_point after(std::chrono::steady_clock::time_point now, std::chrono::milliseconds wait) {
  return now + std::min(wait, std::chrono::milliseconds(std::chrono::hours(24)));
}
std::string encoded(std::string_view value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') out += static_cast<char>(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}
std::string page_url(Listing listing, std::string base, const std::string& next) {
  while (base.ends_with('/')) base.pop_back();
  switch (listing) {
    case Listing::OpenRouter: return base + "/api/v1/models?output_modalities=all";
    case Listing::Gemini: return base + "/v1beta/models?pageSize=1000" + (next.empty() ? "" : "&pageToken=" + encoded(next));
    case Listing::Anthropic: return base + "/v1/models?limit=1000" + (next.empty() ? "" : "&after_id=" + encoded(next));
    case Listing::OpenAI: return base + "/v1/models";
  }
  return base;
}
std::vector<Header> listing_headers(Listing listing, const std::string& key) {
  std::vector<Header> headers{{"accept", "application/json"}};
  if (key.empty()) return headers;
  switch (listing) {
    case Listing::OpenRouter:
    case Listing::OpenAI: headers.push_back({"authorization", "Bearer " + key}); break;
    case Listing::Gemini: headers.push_back({"x-goog-api-key", key}); break;
    case Listing::Anthropic:
      headers.push_back({"x-api-key", key});
      headers.push_back({"anthropic-version", "2023-06-01"});
      break;
  }
  return headers;
}

struct Page {
  Result result;
  int status = 0;
  std::string body;
  bool too_large = false;
};
Page fetch(Transport& transport, HttpRequest request) {
  // The callbacks run on an I/O thread; join() orders their writes before the reads below.
  auto page = std::make_shared<Page>();
  Callbacks callbacks;
  callbacks.on_head = [page](const ResponseHead& head) { page->status = head.status; };
  callbacks.on_body = [page](std::string_view bytes) {
    if (page->too_large || page->body.size() + bytes.size() > max_page_bytes) page->too_large = true;
    else page->body.append(bytes);
    return true;
  };
  callbacks.on_done = [](const Result&) {};
  auto operation = transport.start(std::move(request), std::move(callbacks));
  page->result = operation.join();
  return std::move(*page);
}

// An entry that sources of different kinds both list (one id at two origins) keeps every origin and
// family, and the name of every listing that gave it, joined with '+' in order ("gemini+openai").
constexpr char source_separator = '+';
std::vector<std::string_view> source_names(const CatalogueEntry& entry) {
  std::vector<std::string_view> names;
  if (!entry.source) return names;
  std::string_view rest = *entry.source;
  for (;;) {
    const auto cut = rest.find(source_separator);
    names.push_back(rest.substr(0, cut));
    if (cut == std::string_view::npos) return names;
    rest.remove_prefix(cut + 1);
  }
}
bool listed_by(const CatalogueEntry& entry, std::string_view name) {
  const auto names = source_names(entry);
  return std::find(names.begin(), names.end(), name) != names.end();
}
CatalogueSnapshot combine(const std::vector<CatalogueSnapshot>& layers) {
  struct Scope { std::set<std::string> origins, families, sources; };
  std::map<std::pair<std::string, bool>, Scope> scope;
  for (const auto& layer : layers)
    for (const auto& entry : layer->entries()) {
      auto& found = scope[{entry.id, entry.prefix}];
      found.origins.insert(entry.origins.begin(), entry.origins.end());
      found.families.insert(entry.families.begin(), entry.families.end());
      for (const auto name : source_names(entry)) found.sources.emplace(name);
    }
  auto merged = descriptor::merge_catalogues(layers);
  if (auto* error = std::get_if<ConfigError>(&merged)) throw std::move(*error);
  auto entries = std::get<CatalogueSnapshot>(merged)->entries();
  for (auto& entry : entries) {
    const auto& [origins, families, sources] = scope[{entry.id, entry.prefix}];
    entry.origins.assign(origins.begin(), origins.end());
    entry.families.assign(families.begin(), families.end());
    if (!sources.empty()) {
      std::string joined;
      for (const auto& name : sources) {
        if (!joined.empty()) joined += source_separator;
        joined += name;
      }
      entry.source = std::move(joined);
    }
  }
  auto made = descriptor::make_catalogue(std::move(entries));
  if (auto* error = std::get_if<ConfigError>(&made)) throw std::move(*error);
  return std::get<CatalogueSnapshot>(std::move(made));
}
CatalogueSnapshot catalogue_of(std::vector<CatalogueEntry> entries) {
  auto made = descriptor::make_catalogue(std::move(entries));
  if (auto* error = std::get_if<ConfigError>(&made)) throw std::move(*error);
  return std::get<CatalogueSnapshot>(std::move(made));
}

using Refreshed = std::vector<std::pair<std::string, std::string>>;  // (listing name, origin) that listed

// A listed entry is owned by the sources that listed it: every origin it applies at is served by a source
// that listed this time and is one of the entry's listings.
bool owned(const CatalogueEntry& entry, const Refreshed& refreshed) {
  return entry.source && !entry.origins.empty() &&
         std::all_of(entry.origins.begin(), entry.origins.end(), [&](const std::string& origin) {
           return std::any_of(refreshed.begin(), refreshed.end(),
                              [&](const auto& item) { return item.second == origin && listed_by(entry, item.first); });
         });
}
// The entries of the previous catalogue stay unless a source that listed this time owns them; the
// listings come over them.
CatalogueSnapshot generated_from(const CatalogueSnapshot& previous, const Refreshed& refreshed,
                                 const std::vector<CatalogueSnapshot>& fresh) {
  std::vector<CatalogueEntry> kept;
  if (previous)
    for (const auto& entry : previous->entries())
      if (!owned(entry, refreshed)) kept.push_back(entry);
  std::vector<CatalogueSnapshot> layers{catalogue_of(std::move(kept))};
  layers.insert(layers.end(), fresh.begin(), fresh.end());
  return combine(layers);
}
// How many entries of the previous catalogue the source at this origin listed.
std::size_t listed_before(const CatalogueSnapshot& previous, std::string_view name, std::string_view origin) {
  if (!previous) return 0;
  return static_cast<std::size_t>(std::count_if(previous->entries().begin(), previous->entries().end(), [&](const CatalogueEntry& entry) {
    return listed_by(entry, name) && std::find(entry.origins.begin(), entry.origins.end(), origin) != entry.origins.end();
  }));
}
} // namespace

std::string default_listing_url(Listing listing) {
  switch (listing) {
    case Listing::OpenRouter: return "https://openrouter.ai";
    case Listing::Gemini: return "https://generativelanguage.googleapis.com";
    case Listing::Anthropic: return "https://api.anthropic.com";
    case Listing::OpenAI: return "https://api.openai.com";
  }
  return {};
}

std::variant<RefreshResult, ConfigError> refresh_catalogue(Transport& transport, const RefreshRequest& request) {
  try {
    struct Planned { ListingSource source; std::string url, origin; };
    std::vector<Planned> planned;
    for (std::size_t i = 0; i < request.sources.size(); ++i) {
      const auto& source = request.sources[i];
      const auto pointer = "/sources/" + std::to_string(i);
      auto url = source.url.empty() ? default_listing_url(source.listing) : source.url;
      auto origin = source.origin.empty() ? default_listing_url(source.listing) : source.origin;
      if (!allowed_url(url)) throw ConfigError{pointer + "/url", "https URL, or http on a loopback host, without query", 0, "listing source rejected"};
      if (!descriptor::valid_origin(origin) || origin.ends_with('/'))
        throw ConfigError{pointer + "/origin", "canonical origin without path", 0, "listing source rejected"};
      if (source.api_key.find_first_of("\r\n") != std::string::npos)
        throw ConfigError{pointer + "/api_key", "credential without line breaks", 0, "listing source rejected"};
      planned.push_back({source, std::move(url), std::move(origin)});
    }

    RefreshResult out;
    const auto deadline = after(std::chrono::steady_clock::now(), request.total_timeout);
    std::vector<CatalogueSnapshot> fresh;
    Refreshed refreshed;
    out.generated = generated_from(request.previous, refreshed, fresh);
    for (const auto& [source, url, origin] : planned) {
      SourceReport report;
      report.listing = source.listing;
      report.origin = origin;
      std::vector<CatalogueSnapshot> pages;
      std::string next;
      for (;;) {
        if (report.pages == max_pages) { report.error = "listing has more than 64 pages"; break; }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) { report.error = "refresh deadline passed"; break; }
        HttpRequest http;
        http.method = "GET";
        http.url = page_url(source.listing, url, next);
        http.headers = listing_headers(source.listing, source.api_key);
        http.deadline = std::min(after(now, request.page_timeout), deadline);
        http.ca_file = request.ca_file;
        auto page = fetch(transport, std::move(http));
        ++report.pages;
        report.http_status = page.status;
        if (page.result.status != Status::Completed) {
          report.error = std::chrono::steady_clock::now() >= deadline ? "refresh deadline passed" : "listing request did not complete";
          break;
        }
        if (page.status != 200) { report.error = "listing answered HTTP " + std::to_string(page.status); break; }
        if (page.too_large) { report.error = "listing page exceeds 32 MiB"; break; }
        auto parsed = descriptor::parse_listing(source.listing, page.body, origin);
        if (std::holds_alternative<ConfigError>(parsed)) { report.error = "listing page is not a model listing"; break; }
        auto& listed = std::get<descriptor::ListingPage>(parsed);
        report.skipped += listed.skipped;
        report.skipped_prices += listed.skipped_prices;
        try {
          pages.push_back(catalogue_of(std::move(listed.entries)));
        } catch (ConfigError&) { report.error = "listing exceeds the catalogue limits"; break; }
        if (listed.next.empty()) { report.ok = true; break; }
        if (listed.next == next) { report.error = "listing repeated its page token"; break; }
        next = std::move(listed.next);
      }
      if (report.ok) {
        const std::string name(descriptor::listing_name(source.listing));
        try {
          auto listed = combine(pages);
          report.models = listed->entries().size();
          // A listing with no models, or with far fewer than the source gave before, is not taken as
          // the truth: a proxy, a captive portal or an outage can answer 200 with an empty list.
          if (!request.allow_drop && report.models == 0) report.error = "listing named no models";
          else if (!request.allow_drop && static_cast<std::uint64_t>(report.models) * 100 <
                                              static_cast<std::uint64_t>(listed_before(request.previous, name, origin)) * request.min_kept_percent)
            report.error = "listing named far fewer models than before";
          else {
            // The catalogue has to hold this listing next to the others; if it cannot, only this source fails.
            auto with_refreshed = refreshed;
            with_refreshed.emplace_back(name, origin);
            auto with_fresh = fresh;
            with_fresh.push_back(std::move(listed));
            out.generated = generated_from(request.previous, with_refreshed, with_fresh);
            refreshed = std::move(with_refreshed);
            fresh = std::move(with_fresh);
          }
        } catch (ConfigError&) { report.error = "listing exceeds the catalogue limits"; }
        report.ok = report.error.empty();
      }
      out.sources.push_back(std::move(report));
    }

    std::vector<CatalogueSnapshot> overlays{out.generated};
    for (const auto& overlay : request.overlays) if (overlay) overlays.push_back(overlay);
    auto layered = descriptor::merge_catalogues(overlays);
    if (auto* error = std::get_if<ConfigError>(&layered)) {
      error->pointer = "/overlays" + error->pointer;
      return std::move(*error);
    }
    out.catalogue = std::get<CatalogueSnapshot>(std::move(layered));
    out.diff = descriptor::diff_catalogues(request.previous ? *request.previous : *catalogue_of({}), *out.generated);
    return out;
  } catch (ConfigError& error) { return std::move(error); }
}

} // namespace sp::transport
