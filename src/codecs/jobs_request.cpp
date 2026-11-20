#include "codecs/jobs.h"
#include "codecs/reason.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>

namespace sp::jobs {
namespace {
Error bad(const reason::Reason& message) { return reason::error(ErrorKind::InvalidRequest, message); }
// A path segment the handle names; `.` and `..` would move the request elsewhere.
bool token(std::string_view value) {
  return !value.empty() && value.size() <= 128 && value != "." && value != ".." && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '.' || c == '_' || c == '-';
  });
}
// The fact and path of one operation on a job the handle names, at the origin that started it.
std::variant<std::pair<const descriptor::EndpointFact*, std::string>, Error> job_path(const descriptor::ValidatedDescriptor& descriptor,
                                                                                       const Handle& handle, std::string_view operation) {
  if (handle.origin != descriptor.base_url()) return bad(reason::jobs_request::origin_mismatch);
  const auto* fact = descriptor.policy()->endpoint(handle.endpoint, descriptor.base_url());
  if (!fact) return reason::error(ErrorKind::Unsupported, reason::jobs_request::endpoint_not_admitted);
  const auto path = descriptor::endpoint_path(*fact, operation);
  if (!path) return reason::error(ErrorKind::Unsupported, reason::jobs_request::path_missing);
  if (!token(handle.id)) return bad(reason::jobs_request::id_invalid);
  std::string resolved(*path);
  if (const auto at = resolved.find("{id}"); at != std::string::npos) resolved.replace(at, 4, handle.id);
  return std::pair{fact, std::move(resolved)};
}
}  // namespace

std::string_view operation(const Poll&) noexcept { return "poll"; }
std::string_view operation(const Download&) noexcept { return "download"; }
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Poll& poll) {
  auto found = job_path(descriptor, poll.handle, operation(poll));
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<0>(found);
  Encoded encoded{std::move(path), {}, poll.handle.model, false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.context.job = poll.handle.id;
  encoded.method = "GET";
  encoded.content_type.clear();
  return encoded;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Download& download) {
  auto found = job_path(descriptor, download.handle, operation(download));
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<0>(found);
  if (!download.max_bytes) return bad(reason::jobs_request::download_limit_invalid);
  Encoded encoded{std::move(path) + "?index=" + std::to_string(download.index), {}, download.handle.model, false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.method = "GET";
  encoded.content_type.clear();
  encoded.accept = "*/*";
  encoded.sink = download.path;
  encoded.sink_limit = download.max_bytes;
  return encoded;
}

}  // namespace sp::jobs
