#include "codecs/request_checks.h"
#include "core/request_controls.h"

namespace sp::request_controls {
std::optional<Error> check_routing(const descriptor::ValidatedDescriptor& descriptor, const OpenRouterRouting& routing) {
  const auto refusal = validate_routing(descriptor, routing);
  if (!refusal) return std::nullopt;
  auto error = reason::error(ErrorKind::InvalidRequest, *refusal);
  error.retry_safety = RetrySafety::NotSent;
  return error;
}
}  // namespace sp::request_controls
