#pragma once
// Internal: the grammar of OpenRouter's endpoints listing (GET /api/v1/models/{author}/{slug}/endpoints) that the route
// ceiling of a refresh (transport/catalogue_refresh.cpp) and the endpoint rows (descriptor::parse_endpoints_listing) read
// alike. Not installed.
#include "descriptor/catalogue.h"
#include "json/json.h"
#include <cstddef>
#include <string_view>

namespace sp::descriptor::endpoint_prices {

// The side of a call a price member bills: input tokens, output tokens, or the request fee.
enum Side : std::size_t { Input, Output, Request };
// A member of an endpoint's `pricing` that a route ceiling covers, as the endpoints listing documents it, and whether
// every endpoint lists it.
struct Member {
  std::string_view name;
  Side side;
  bool required;
};
// One table for the route ceiling and the rows. `image_token` does not say which side it bills, so it counts on both.
inline constexpr Member members[]{
    {"prompt", Input, true},          {"input_cache_read", Input, false},   {"input_cache_write", Input, false},
    {"input_cache_write_1h", Input, false}, {"audio", Input, false},      {"input_audio_cache", Input, false},
    {"image_token", Input, false},     {"completion", Output, true},          {"internal_reasoning", Output, false},
    {"audio_output", Output, false},   {"image_output", Output, false},       {"image_token", Output, false},
    {"request", Request, false}};

// parse_endpoints_listing over a body already parsed: `root` is the document's root.
EndpointsResult read_endpoints_listing(json::Value root, std::string_view model);

} // namespace sp::descriptor::endpoint_prices
