#include "transport/catalogue_refresh.h"
#include "core/pricing.h"
#include "json/json.h"
#include "sp/config_defaults.h"
#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
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
// The headers of a listing request. The key goes the way the policy states for the family the listing belongs to
// (not the family of an entry it lists: the OpenRouter listing lists Messages entries too, yet it is the gateway's
// Chat-shaped listing, which takes Bearer where Messages takes x-api-key at the same origin), at the source's
// origin. A family with a Messages version sends it in `anthropic-version`, a protocol header name.
std::vector<Header> listing_headers(const descriptor::DescriptorPolicy& policy, Listing listing, std::string_view origin,
                                    const std::string& key) {
  std::vector<Header> headers{{"accept", "application/json"}};
  if (key.empty()) return headers;
  const auto family = policy.listing_family(listing);
  if (auto credential = descriptor::credential_header(policy.auth_scheme(family, origin), key)) {
    for (auto& c : credential->first) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    headers.push_back({std::move(credential->first), std::move(credential->second)});
  }
  if (const auto* facts = policy.family(family); facts && facts->header_version) headers.push_back({"anthropic-version", *facts->header_version});
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

// Route ceilings, from OpenRouter's endpoints listing of each model asked for.

// An OpenRouter model id as its endpoints path takes it, `author/slug`: a `~` may start the author (an alias the
// provider resolves) and a `:` variant may end the slug (`vendor/model:free`). Each part starts with a letter or a
// digit and holds letters, digits, '-', '.' and '_' only, so an id never names another path (`.`, `..`, a third
// segment), escapes a character or carries a query.
bool id_part(std::string_view part, bool slug) noexcept {
  const auto alphanumeric = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
  return !part.empty() && alphanumeric(static_cast<unsigned char>(part.front())) &&
         std::all_of(part.begin(), part.end(), [&](unsigned char c) {
           return alphanumeric(c) || c == '-' || c == '.' || c == '_' || (slug && c == ':');
         });
}
bool openrouter_model_id(std::string_view id) noexcept {
  if (!descriptor::valid_model_name(id)) return false;
  const auto slash = id.find('/');
  if (slash == std::string_view::npos) return false;
  auto author = id.substr(0, slash);
  if (author.starts_with('~')) author.remove_prefix(1);
  return id_part(author, false) && id_part(id.substr(slash + 1), true);
}
std::string endpoints_url(std::string base, std::string_view id) {
  while (base.ends_with('/')) base.pop_back();
  return base + "/api/v1/models/" + std::string(id) + "/endpoints";
}

enum Side : std::size_t { Input, Output, Request };
constexpr const auto& ceiling_units = descriptor::route_ceiling_units;
bool ceiling_unit(std::string_view unit) noexcept {
  return std::find(std::begin(ceiling_units), std::end(ceiling_units), unit) != std::end(ceiling_units);
}
// The members of an endpoint's `pricing`, and of each entry of its `overrides`, that a route ceiling covers, by the
// unit that covers them, as OpenRouter's endpoints listing documents them. The ceiling prices every input token at
// `ceiling_input` and every output token at `ceiling_output` (pricing::estimate), so each is the dearest price per
// token of its side: cache reads and writes, audio, reasoning and image output tokens (`image_output`) included, not
// the prompt and completion prices alone. One `ceiling_output` is the pin's `completion`, the only token price
// `provider.max_price` states, so an image output token is covered by the same ceiling as a text one. `image_token`
// does not say which side it bills, so it counts on both. `prompt` and `completion` are
// required. An endpoint that leaves out an optional token price is read as billing those tokens at its prompt or
// completion price, as pricing::estimate does for an entry without them, and an override that leaves one out keeps
// the base price. `request` is the exception: nothing says what a fee left out costs, so a fee that some endpoints
// list, in their base pricing or in an override, and others leave out is missing where the others have it. The
// prices per input image and per web search are per item, which no usage counter counts, so no ceiling covers them
// (descriptor::listed_members names them for the bound). The prices the models listing gives for the model on a side
// count too, its input bands included, so a ceiling is never below a price the same refresh lists; a side with a listed price the entry leaves out for an
// override it cannot hold (descriptor::ListingPage::withheld) has no known floor and gets no ceiling.
struct PriceMember {
  std::string_view name;
  Side side;
  bool required;
};
constexpr PriceMember price_members[]{
    {"prompt", Input, true},          {"input_cache_read", Input, false},   {"input_cache_write", Input, false},
    {"input_cache_write_1h", Input, false}, {"audio", Input, false},      {"input_audio_cache", Input, false},
    {"image_token", Input, false},     {"completion", Output, true},          {"internal_reasoning", Output, false},
    {"audio_output", Output, false},   {"image_output", Output, false},       {"image_token", Output, false},
    {"request", Request, false}};

// Why a price cannot bound a ceiling: one of an endpoint, or one the models listing gives.
enum class Unusable : std::uint8_t { Missing, Variable, NotDecimal, TooLarge, ListedVariable, ListedTooLarge, ListedLeftOut };
constexpr std::string_view unusable_reasons[]{"price missing at an endpoint", "variable price at an endpoint",
                                              "price at an endpoint is not decimal text",
                                              "price at an endpoint is too large for the catalogue",
                                              "variable price in the listing",
                                              "price in the listing is too large for the catalogue",
                                              "price in the listing left out for a conditional price"};
bool present(json::Value value) { return value.valid() && !value.is_null(); }
// An endpoint's price, read as a listed price is: text an entry can hold (descriptor::valid_price_text), not
// negative, and exact in nano-USD per million units (pricing::per_million).
std::variant<std::uint64_t, Unusable> endpoint_price(json::Value value) {
  if (!value.is_string() || !descriptor::valid_price_text(value.as_string())) return Unusable::NotDecimal;
  if (value.as_string().front() == '-') return Unusable::Variable;
  if (const auto rate = pricing::per_million(value.as_string())) return *rate;
  return Unusable::TooLarge;
}

// What a model's endpoints listing says about its route ceiling.
struct EndpointsRead {
  enum class Shape : std::uint8_t { Listing, NotListing, OtherModel, Empty } shape = Shape::Listing;
  // By Side: the dearest price over every endpoint and every override.
  std::array<std::optional<std::uint64_t>, 3> dearest;
  // The prices that cannot bound their unit: (side, index in price_members, why), in that order.
  std::set<std::tuple<Side, std::size_t, Unusable>> unusable;
  // A discount of an endpoint or of one of its overrides is not a number from 0 to 1. The listing multiplies each
  // price by (1 - discount), so only such a discount keeps the listed prices an upper bound.
  bool discount = false;
};
EndpointsRead read_endpoints(std::string_view body, std::string_view id) {
  using Shape = EndpointsRead::Shape;
  EndpointsRead out;
  const auto parsed = json::parse(body, {max_page_bytes, 64});
  const auto* document = std::get_if<json::Document>(&parsed);
  if (!document) { out.shape = Shape::NotListing; return out; }
  const auto root = document->root();
  const auto data = root.get("data");
  const auto endpoints = data.get("endpoints");
  if (!root.is_object() || !data.is_object() || !data.get("id").is_string() || !endpoints.is_array()) { out.shape = Shape::NotListing; return out; }
  if (data.get("id").as_string() != id) { out.shape = Shape::OtherModel; return out; }
  if (endpoints.size() == 0) { out.shape = Shape::Empty; return out; }
  // Every endpoint is an object with a pricing object, and its overrides, if any, an array of objects.
  for (const auto endpoint : endpoints.elements()) {
    const auto prices = endpoint.get("pricing");
    if (!endpoint.is_object() || !prices.is_object()) { out.shape = Shape::NotListing; return out; }
    const auto overrides = prices.get("overrides");
    if (!present(overrides)) continue;
    if (!overrides.is_array()) { out.shape = Shape::NotListing; return out; }
    for (const auto entry : overrides.elements())
      if (!entry.is_object()) { out.shape = Shape::NotListing; return out; }
  }
  const auto read = [&](json::Value prices, bool base) {
    for (std::size_t i = 0; i < std::size(price_members); ++i) {
      const auto& member = price_members[i];
      const auto value = prices.get(member.name);
      if (!present(value)) {
        if (base && member.required) out.unusable.emplace(member.side, i, Unusable::Missing);
        continue;
      }
      const auto rate = endpoint_price(value);
      if (const auto* why = std::get_if<Unusable>(&rate)) {
        out.unusable.emplace(member.side, i, *why);
        continue;
      }
      auto& dearest = out.dearest[member.side];
      if (!dearest || std::get<std::uint64_t>(rate) > *dearest) dearest = std::get<std::uint64_t>(rate);
    }
  };
  const auto discounted = [&](json::Value prices) {
    const auto discount = prices.get("discount");
    if (present(discount) && !(discount.is_number() && discount.as_double() >= 0 && discount.as_double() <= 1)) out.discount = true;
  };
  const auto lists_fee = [](json::Value prices) {
    return std::any_of(std::begin(price_members), std::end(price_members),
                       [&](const PriceMember& member) { return member.side == Request && present(prices.get(member.name)); });
  };
  // The endpoints that list a request fee, in their base pricing or in any of their overrides.
  std::size_t with_fee = 0;
  for (const auto endpoint : endpoints.elements()) {
    const auto prices = endpoint.get("pricing");
    discounted(prices);
    read(prices, true);
    bool fee = lists_fee(prices);
    if (const auto overrides = prices.get("overrides"); present(overrides))
      for (const auto entry : overrides.elements()) {
        discounted(entry);
        read(entry, false);
        fee = fee || lists_fee(entry);
      }
    if (fee) ++with_fee;
  }
  if (with_fee != 0 && with_fee != endpoints.size())
    for (std::size_t i = 0; i < std::size(price_members); ++i)
      if (price_members[i].side == Request) out.unusable.emplace(Request, i, Unusable::Missing);
  return out;
}

using PriceUnits = std::vector<std::pair<std::string, std::string>>;
// The route ceiling `previous` has for the model at this source's origin.
PriceUnits previous_ceiling(const CatalogueSnapshot& previous, std::string_view id, std::string_view origin) {
  PriceUnits out;
  const auto* entry = previous ? previous->entry(id) : nullptr;
  if (!entry || !listed_by(*entry, descriptor::listing_name(Listing::OpenRouter)) ||
      std::find(entry->origins.begin(), entry->origins.end(), origin) == entry->origins.end())
    return out;
  for (const auto& price : entry->pricing)
    if (ceiling_unit(price.first)) out.push_back(price);
  return out;
}
// Puts the units on the entry when an entry can hold them beside its other prices.
bool hold(CatalogueEntry& entry, const PriceUnits& units) {
  if (units.empty()) return true;
  auto candidate = entry;
  candidate.pricing.insert(candidate.pricing.end(), units.begin(), units.end());
  std::sort(candidate.pricing.begin(), candidate.pricing.end());
  std::vector<CatalogueEntry> alone;
  alone.push_back(candidate);
  if (std::holds_alternative<ConfigError>(descriptor::make_catalogue(std::move(alone)))) return false;
  entry = std::move(candidate);
  return true;
}

struct Planned {
  ListingSource source;
  std::string url, origin;
  std::vector<std::string> ceilings;  // sorted
};
// Reads the route ceiling of each model asked for into the listed entries, in id order, one request per listed
// model, and reports every model and unit it could not write.
void read_ceilings(Transport& transport, const RefreshRequest& request, const descriptor::DescriptorPolicy& policy, const Planned& plan,
                   std::chrono::steady_clock::time_point deadline, std::vector<CatalogueEntry>& entries, SourceReport& report) {
  for (const auto& id : plan.ceilings) {
    const auto gap = [&](std::string_view unit, std::string_view price, std::string reason, bool kept = false) {
      report.ceiling_gaps.push_back({id, std::string(unit), std::string(price), std::move(reason), kept});
    };
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const CatalogueEntry& e) { return !e.prefix && e.id == id; });
    if (found == entries.end()) { gap({}, {}, "not in the listing"); continue; }
    auto& entry = *found;
    // A ceiling comes from the endpoints listing only; a unit of that name in the models listing is dropped, and so are
    // its input bands.
    std::erase_if(entry.pricing, [](const auto& price) { return ceiling_unit(descriptor::unbanded_unit(price.first)); });
    // The listed price members the entry left out for overrides it cannot hold. Conditional prices that cannot be read
    // at all (an empty member) leave no side a known floor.
    std::vector<std::string_view> left_out;
    for (const auto& unit : report.withheld)
      if (unit.model == id) left_out.push_back(unit.unit);
    if (std::find(left_out.begin(), left_out.end(), std::string_view{}) != left_out.end()) {
      gap({}, {}, "conditional prices in the listing cannot be read");
      continue;
    }
    // An endpoints request that fails keeps the ceiling `previous` had for the model.
    const auto keep_previous = [&](std::string reason) {
      auto units = previous_ceiling(request.previous, id, plan.origin);
      // A kept ceiling below a token price this listing gives for the model bounds nothing, and half a ceiling is no
      // better: the whole previous ceiling is dropped when either side is stale.
      bool stale = false;
      for (const auto& unit : units) {
        const bool input = unit.first == ceiling_units[Input], output = unit.first == ceiling_units[Output];
        const auto rate = pricing::per_million(unit.second);
        stale = stale || ((input || output) &&
                          (!rate || pricing::ceiling_below_listed(entry, input ? pricing::TokenSide::Input : pricing::TokenSide::Output, *rate)));
      }
      PriceUnits dropped;
      if (stale) dropped = std::exchange(units, PriceUnits{});
      gap({}, {}, std::move(reason), !units.empty() && hold(entry, units));
      for (const auto& unit : dropped) gap(unit.first, {}, "kept ceiling is dropped: a side of it is below a price the listing gives");
    };
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) { keep_previous("refresh deadline passed"); continue; }
    HttpRequest http;
    http.method = "GET";
    http.url = endpoints_url(plan.url, id);
    http.headers = listing_headers(policy, Listing::OpenRouter, plan.origin, plan.source.api_key);
    http.deadline = std::min(after(now, request.page_timeout), deadline);
    http.ca_file = request.ca_file;
    auto page = fetch(transport, std::move(http));
    ++report.ceiling_requests;
    if (page.result.status != Status::Completed) {
      keep_previous(std::chrono::steady_clock::now() >= deadline ? "refresh deadline passed" : "endpoints request did not complete");
      continue;
    }
    if (page.status != 200) { keep_previous("endpoints answered HTTP " + std::to_string(page.status)); continue; }
    if (page.too_large) { gap({}, {}, "endpoints listing exceeds 32 MiB"); continue; }
    auto read = read_endpoints(page.body, id);
    if (read.shape == EndpointsRead::Shape::NotListing) { gap({}, {}, "not an endpoints listing"); continue; }
    if (read.shape == EndpointsRead::Shape::OtherModel) { gap({}, {}, "endpoints listing names another model"); continue; }
    if (read.shape == EndpointsRead::Shape::Empty) { gap({}, {}, "no endpoints"); continue; }
    // The prices the models listing gives for the model bound each token side from below, as the endpoints do, its
    // input bands included. A listed price the entry left out is a price of no known amount.
    for (std::size_t i = 0; i < std::size(price_members); ++i) {
      const auto& member = price_members[i];
      if (member.side == Request) continue;
      // A unit counts under either of its names (descriptor::plain_price_unit).
      const auto plain = descriptor::plain_price_unit(member.name);
      if (std::any_of(left_out.begin(), left_out.end(), [&](std::string_view unit) { return descriptor::plain_price_unit(unit) == plain; }))
        read.unusable.emplace(member.side, i, Unusable::ListedLeftOut);
      for (const auto& [unit, text] : entry.pricing) {
        if (descriptor::plain_price_unit(descriptor::unbanded_unit(unit)) != plain) continue;
        const auto rate = pricing::per_million(text);
        if (!rate) {
          read.unusable.emplace(member.side, i, text.starts_with('-') ? Unusable::ListedVariable : Unusable::ListedTooLarge);
          continue;
        }
        auto& dearest = read.dearest[member.side];
        if (!dearest || *rate > *dearest) dearest = rate;
      }
    }
    const auto gaps_before = report.ceiling_gaps.size();
    bool unusable[3] = {false, false, false};
    for (const auto& [side, member, why] : read.unusable) {
      unusable[side] = true;
      gap(ceiling_units[side], price_members[member].name, std::string(unusable_reasons[static_cast<std::size_t>(why)]));
    }
    // Without a request ceiling a bound charges the listed request fee, so a fee that cannot be read leaves out the
    // whole ceiling rather than one that may be too low.
    if (read.discount) gap({}, "discount", "discount at an endpoint is not from 0 to 1");
    if (unusable[Request]) gap({}, {}, "no ceiling without every request fee");
    if (read.discount || unusable[Request]) continue;
    PriceUnits units;
    for (const Side side : {Input, Output})
      if (!unusable[side] && read.dearest[side]) units.emplace_back(ceiling_units[side], pricing::decimal_text(*read.dearest[side]));
    if (read.dearest[Request] && *read.dearest[Request] > 0)
      units.emplace_back(ceiling_units[Request], pricing::decimal_text(*read.dearest[Request]));
    if (!hold(entry, units)) { gap({}, {}, "the entry cannot hold the ceiling"); continue; }
    if (report.ceiling_gaps.size() == gaps_before) ++report.ceilings;
  }
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
    std::vector<Planned> planned;
    for (std::size_t i = 0; i < request.sources.size(); ++i) {
      const auto& source = request.sources[i];
      const auto pointer = "/sources/" + std::to_string(i);
      auto url = source.url.empty() ? default_listing_url(source.listing) : source.url;
      auto origin = source.origin.empty() ? default_listing_url(source.listing) : source.origin;
      if (!allowed_url(url)) throw ConfigError{pointer + "/url", "https URL, or http on a loopback host, without query", 0, "listing source rejected"};
      if (!descriptor::valid_origin(origin) || origin.ends_with('/'))
        throw ConfigError{pointer + "/origin", "canonical origin without path", 0, "listing source rejected"};
      // The origin scopes the generated entries and picks the listing's credential scheme, both compared byte for byte
      // with a descriptor's base URL, so it is the origin's serialization.
      if (!descriptor::canonical_origin_spelling(origin))
        throw ConfigError{pointer + "/origin", std::string(descriptor::canonical_origin_expected), 0, "listing source rejected"};
      if (source.api_key.find_first_of("\r\n") != std::string::npos)
        throw ConfigError{pointer + "/api_key", "credential without line breaks", 0, "listing source rejected"};
      auto ceilings = source.ceiling_models;
      if (!ceilings.empty()) {
        const auto at = pointer + "/ceiling_models";
        if (source.listing != Listing::OpenRouter)
          throw ConfigError{at, "route ceilings from an OpenRouter listing only", 0, "listing source rejected"};
        if (ceilings.size() > config_defaults::catalogue_refresh_max_ceiling_models)
          throw ConfigError{at, "at most " + std::to_string(config_defaults::catalogue_refresh_max_ceiling_models) + " model ids", 0,
                            "listing source rejected"};
        for (std::size_t j = 0; j < ceilings.size(); ++j)
          if (!openrouter_model_id(ceilings[j]))
            throw ConfigError{at + "/" + std::to_string(j), "OpenRouter model id author/slug", 0, "listing source rejected"};
        std::sort(ceilings.begin(), ceilings.end());
        if (std::adjacent_find(ceilings.begin(), ceilings.end()) != ceilings.end())
          throw ConfigError{at, "each model id once", 0, "listing source rejected"};
      }
      planned.push_back({source, std::move(url), std::move(origin), std::move(ceilings)});
    }

    const auto policy = request.policy ? request.policy : descriptor::builtin_policy();
    RefreshResult out;
    const auto deadline = after(std::chrono::steady_clock::now(), request.total_timeout);
    std::vector<CatalogueSnapshot> fresh;
    Refreshed refreshed;
    out.generated = generated_from(request.previous, refreshed, fresh);
    for (const auto& plan : planned) {
      const auto& [source, url, origin, ceilings] = plan;
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
        http.headers = listing_headers(*policy, source.listing, origin, source.api_key);
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
        report.skipped_override_prices += listed.skipped_override_prices;
        report.window_prices_at_dearest += listed.window_prices_at_dearest;
        report.withheld.insert(report.withheld.end(), listed.withheld.begin(), listed.withheld.end());
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
            if (!ceilings.empty()) {
              auto entries = listed->entries();
              read_ceilings(transport, request, *policy, plan, deadline, entries, report);
              listed = catalogue_of(std::move(entries));
            }
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
      // A source that fails keeps its previous entries whole, ceilings included.
      if (!report.ok && !ceilings.empty()) {
        report.ceilings = 0;
        report.ceiling_gaps.clear();
        for (const auto& id : ceilings)
          report.ceiling_gaps.push_back({id, {}, {}, "listing failed", !previous_ceiling(request.previous, id, origin).empty()});
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
