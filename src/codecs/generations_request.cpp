#include "codecs/generations.h"
#include "descriptor/policy.h"
#include <algorithm>

namespace sp::generations {
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Get& get) {
  const auto* fact = descriptor.policy()->endpoint("generations", descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "generation statistics are not admitted at this origin"};
  const auto path = descriptor::endpoint_path(*fact, "get");
  if (!path) return Error{ErrorKind::Unsupported, "the generation endpoint has no path"};
  if (get.id.empty() || get.id.size() > 128 || !std::all_of(get.id.begin(), get.id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
      }))
    return Error{ErrorKind::InvalidRequest, "a generation id is a token"};
  Encoded encoded{std::string(*path) + "?id=" + get.id, {}, {}, false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.method = "GET";
  encoded.content_type.clear();
  return encoded;
}
}  // namespace sp::generations
