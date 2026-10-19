#pragma once
// The decoder side every non-chat endpoint shares (D6): one buffered response document becomes accumulator events,
// which the endpoint's `typed()` turns into its own result.
#include "codecs/accumulator.h"
#include "descriptor/descriptor.h"
#include <memory>
#include <string>
#include <string_view>

namespace sp::endpoints {
struct Close { bool normal = true; ErrorKind error = ErrorKind::Truncated; };
class Codec {
 public:
  virtual ~Codec() = default;
  virtual bool buffered(std::string_view body, Close close) = 0;
};
// The decoder of the endpoint `name` (`images`, `decisions`, ...), or null for a name without one.
std::unique_ptr<Codec> make_codec(std::string_view name, const descriptor::ValidatedDescriptor&, Accumulator&,
                                  std::string requested, SemanticLimits);
}  // namespace sp::endpoints
