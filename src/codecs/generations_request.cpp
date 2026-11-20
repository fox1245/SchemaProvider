#include "codecs/generations.h"
#include "codecs/reason.h"
#include "descriptor/policy.h"
#include <algorithm>

namespace sp::generations {
std::string_view operation(const Get&) noexcept { return "get"; }
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Get& get) {
  const auto* fact = descriptor.policy()->endpoint("generations", descriptor.base_url());
  if (!fact) return reason::error(ErrorKind::Unsupported, reason::generations_request::endpoint_not_admitted);
  const auto path = descriptor::endpoint_path(*fact, operation(get));
  if (!path) return reason::error(ErrorKind::Unsupported, reason::generations_request::path_missing);
  if (get.id.empty() || get.id.size() > 128 || !std::all_of(get.id.begin(), get.id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
      }))
    return reason::error(ErrorKind::InvalidRequest, reason::generations_request::id_invalid);
  Encoded encoded{std::string(*path) + "?id=" + get.id, {}, {}, false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.method = "GET";
  encoded.content_type.clear();
  return encoded;
}
}  // namespace sp::generations
