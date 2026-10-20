#pragma once
// Builds a model catalogue from the providers' model listings when the host asks for it. The SDK
// never refreshes on its own and never polls: a harness or developer calls refresh_catalogue at
// start-up, from a scheduled job or by hand, then attaches the result to a policy
// (DescriptorPolicy::with_catalogue) for the clients it builds next.
#include "descriptor/catalogue.h"
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
  // listing's path is appended. Empty: the provider's public API.
  std::string url;
  // Sent as the listing's credential header. OpenRouter's listing needs none. Never written to a
  // report or an error.
  std::string api_key;
  // The descriptor origin the generated entries are scoped to. Empty: the provider's API origin.
  std::string origin;
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
};

struct SourceReport {
  descriptor::Listing listing = descriptor::Listing::OpenRouter;
  std::string origin;
  bool ok = false;
  int http_status = 0;  // of the last page requested
  std::size_t pages = 0, models = 0, skipped = 0;
  // Listed prices the catalogue could not hold and left out (ListingPage::skipped_prices).
  std::size_t skipped_prices = 0;
  // Why the source failed, as fixed text: never a URL, header, credential or response body.
  std::string error;
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
// that is not canonical) or the overlays do not merge.
std::variant<RefreshResult, descriptor::ConfigError> refresh_catalogue(Transport& transport, const RefreshRequest& request);

// The provider's public API URL and origin for a listing.
std::string default_listing_url(descriptor::Listing listing);

} // namespace sp::transport
