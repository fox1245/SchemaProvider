#pragma once
// Builds a model catalogue from the providers' model listings when the host asks for it. The SDK
// never refreshes on its own and never polls: a harness or developer calls refresh_catalogue at
// start-up, from a scheduled job or by hand, then attaches the result to a policy
// (DescriptorPolicy::with_catalogue) for the clients it builds next.
#include "descriptor/catalogue.h"
#include "descriptor/policy.h"
#include "transport/http_transport.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace sp::transport {

struct ListingSource {
  descriptor::Listing listing = descriptor::Listing::OpenRouter;
  // Where the listing is served: an https URL (http only on a loopback host) to which the
  // listing's path is appended. Empty: the listing's default origin in RefreshRequest::policy
  // (DescriptorPolicy::listing_origin), and a source of a listing without one is refused.
  std::string url;
  // Sent as the listing's credential header, which RefreshRequest::policy states. OpenRouter's listing needs none. Never written to a
  // report or an error.
  std::string api_key;
  // The descriptor origin the generated entries are scoped to. Empty: the listing's default origin, as for `url`.
  std::string origin;
  // OpenRouter only: the models whose route ceilings (`ceiling_input`, `ceiling_output`, `ceiling_request`) to
  // read from their endpoints listings, GET /api/v1/models/{author}/{slug}/endpoints at `url`, one request per
  // listed model after the listing. Each id is `author/slug`, where a `~` may start the author (an alias) and a
  // `:` variant may end the slug, and each id appears once; at most `max_ceiling_models` of
  // config/catalogue-refresh.json. Empty: no endpoints request, and no ceiling is written.
  std::vector<std::string> ceiling_models;
};

// A route ceiling the refresh was asked for and did not write from the model's endpoints, for one unit or for
// the whole model, and why.
struct CeilingGap {
  std::string model;  // the id as asked for
  // "ceiling_input", "ceiling_output" or "ceiling_request"; empty when the gap is the whole model.
  std::string unit;
  // The member of an endpoint's pricing that left the unit out ("prompt", "input_cache_write"); empty when no one
  // price did.
  std::string price;
  // Fixed text: never a URL, header, credential or response body.
  std::string reason;
  // The ceiling `previous` had for the model is kept, because the endpoints could not be fetched this time.
  bool kept = false;
};

// Endpoint rows (descriptor::CatalogueEntry::endpoints) of a model asked for in `ceiling_models` that the refresh did not
// write, for the whole model or for one endpoint's prices, and why.
struct EndpointGap {
  // The id as asked for. Empty for the gap that says every row of the source was dropped because the catalogue could
  // not hold the refresh's rows (refresh_catalogue then drops every row of every source and tries once more).
  std::string model;
  // The endpoint's tag; empty when the gap is the whole model, which then has no rows.
  std::string tag;
  // "discount", or a price member the row left out ("prompt"); empty for every price of the row or for the whole model.
  std::string price;
  // Fixed text: never a URL, header, credential or response body.
  std::string reason;
};

struct RefreshRequest {
  std::vector<ListingSource> sources;
  // The catalogue an earlier refresh generated. Its entries from a source that fails this time are
  // kept, so a failed or partial fetch never drops models, and the diff is taken against it.
  descriptor::CatalogueSnapshot previous;
  // Layers over the generated catalogue, in order: verified corrections, then the host's own file.
  std::vector<descriptor::CatalogueSnapshot> overlays;
  // Bound for each page request.
  std::chrono::milliseconds page_timeout{30000};
  std::string ca_file;  // optional extra trust anchor (tests, private CAs)
  // Bound for the whole refresh, every page of every source. A source that is not done when it
  // passes fails like one that did not answer, and keeps its previous entries.
  std::chrono::milliseconds total_timeout{300000};
  // A source whose listing names no models, or fewer than `min_kept_percent` percent of the entries
  // `previous` has from it, fails and keeps its previous entries: a proxy, a captive portal or an
  // outage can answer 200 with an empty or short list. 0 turns off the second test only.
  // `allow_drop` takes such a listing as it is.
  std::uint32_t min_kept_percent = 50;
  bool allow_drop = false;
  // The policy that states how a listing request sends the key: the credential scheme of the family the listing
  // belongs to at the source's origin (DescriptorPolicy::listing_family, auth_scheme) and that family's Messages
  // version header. Null: the built-in policy.
  descriptor::PolicySnapshot policy;
};

struct SourceReport {
  descriptor::Listing listing = descriptor::Listing::OpenRouter;
  std::string origin;
  bool ok = false;
  int http_status = 0;  // of the last page requested
  std::size_t pages = 0, models = 0, skipped = 0;
  // Listed prices the catalogue could not hold and left out (ListingPage::skipped_prices).
  std::size_t skipped_prices = 0;
  // Prices of OpenRouter's `pricing.overrides` skipped or left out (ListingPage::skipped_override_prices), time-window
  // prices held at the dearest of the windows (ListingPage::window_prices_at_dearest), and the price members left out
  // of the entries (ListingPage::withheld), page by page.
  std::size_t skipped_override_prices = 0;
  std::size_t window_prices_at_dearest = 0;
  std::vector<descriptor::WithheldUnit> withheld;
  // Why the source failed, as fixed text: never a URL, header, credential or response body.
  std::string error;
  // Route ceilings (ListingSource::ceiling_models): the endpoints requests sent, the models whose ceiling was
  // written whole from their endpoints, and every model or unit asked for that was not, by model id in order.
  std::size_t ceiling_requests = 0, ceilings = 0;
  std::vector<CeilingGap> ceiling_gaps;
  // Endpoint rows from the same endpoints requests: the models whose rows were written, and every model asked for whose
  // rows were not, or whose row left out a price, by model id, then tag, in order; then each listing variant whose rows
  // its base entry could not hold under the variant's tier, and the gap with no model when every row was dropped. A
  // source that fails has no rows written and one gap per model, "listing failed", as its ceilings do. The rows one
  // refresh reads are at most `max_endpoint_rows` of config/catalogue-refresh.json: a model past it has no rows and a gap.
  std::size_t endpoint_rows = 0;
  std::vector<EndpointGap> endpoint_gaps;
};

struct RefreshResult {
  // The provider facts alone: what the sources listed, plus the previous entries of failed sources.
  descriptor::CatalogueSnapshot generated;
  // `generated` with the overlays applied: the catalogue to attach to a policy.
  descriptor::CatalogueSnapshot catalogue;
  // From `previous` (or an empty catalogue) to `generated`.
  descriptor::CatalogueDiff diff;
  std::vector<SourceReport> sources;
};

// Fetches every source in order and merges what they list; the request's sources each own the
// entries whose `source` is the listing's name and whose `origins` is that source's origin. An
// entry that sources of different kinds both list has their names joined with '+' ("gemini+openai"),
// and is owned by them together.
// Blocking: it waits for each page, so it must not run on one of the transport's I/O threads.
// A source that fails is reported in `sources` and keeps its previous entries. A source fails when
// a request does not complete, a page is not a listing, the listing names no models or fewer than
// `min_kept_percent` percent of the entries it gave before (unless `allow_drop`), a page token
// cannot continue the listing, the listing does not fit the catalogue's bounds, or the refresh's
// deadline passes. A ConfigError means the request itself is invalid (a non-https URL, an origin
// that is not canonical, a source without a url or an origin of a listing the policy states no default origin for, ceiling ids that are malformed, repeated, too many or not on an OpenRouter
// source) or the overlays do not merge.
// With ListingSource::ceiling_models, each listed model named there gets the route ceiling its endpoints
// listing gives, every unit at the dearest price of its side over every endpoint, or no ceiling for a unit
// that any endpoint's prices cannot bound (USAGE.md, refreshing the catalogue). A model whose endpoints
// request fails keeps the ceiling `previous` had and does not fail the source. The same listing gives the
// model's endpoint rows, every listed endpoint or none; a model whose endpoints request fails has none, while a
// source that fails keeps its previous entries, their rows and ceilings included.
std::variant<RefreshResult, descriptor::ConfigError> refresh_catalogue(Transport& transport, const RefreshRequest& request);

// The default origin the built-in policy states for a listing (DescriptorPolicy::listing_origin): the provider's public
// API URL and origin. refresh_catalogue reads the request's own policy instead.
std::string default_listing_url(descriptor::Listing listing);

} // namespace sp::transport
