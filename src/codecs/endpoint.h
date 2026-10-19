#pragma once
// The decoder side every non-chat endpoint shares (D6): one buffered response document becomes accumulator events,
// which the endpoint's `typed()` turns into its own result.
#include "codecs/accumulator.h"
#include "descriptor/descriptor.h"
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sp::endpoints {
// An encoded endpoint call: the path at the origin, the body and its media type, what the call accepts back, the
// model it names, the authentication header, and whether the provider charges once it accepts it.
struct Encoded {
  std::string path, body, model;
  bool charge_on_accept = false;
  std::string auth;
  std::string content_type = "application/json", accept = "application/json";
};
// How the response ended, and its headers (lower-case names), which a binary body's decoder reads its type from.
struct Close {
  bool normal = true;
  ErrorKind error = ErrorKind::Truncated;
  std::vector<std::pair<std::string, std::string>> headers{};
};
class Codec {
 public:
  virtual ~Codec() = default;
  virtual bool buffered(std::string_view body, Close close) = 0;
};
// The decoder of the endpoint `name` (`images`, `decisions`, ...), or null for a name without one.
std::unique_ptr<Codec> make_codec(std::string_view name, const descriptor::ValidatedDescriptor&, Accumulator&,
                                  std::string requested, SemanticLimits);
}  // namespace sp::endpoints
