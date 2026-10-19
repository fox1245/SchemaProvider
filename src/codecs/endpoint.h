#pragma once
// The decoder side every non-chat endpoint shares (D6): one buffered response document becomes accumulator events,
// which the endpoint's `typed()` turns into its own result.
#include "codecs/accumulator.h"
#include "descriptor/descriptor.h"
#include <cstdint>
#include <memory>
#include <optional>
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
  std::string method = "POST";
  // A download written to this file as it arrives, at most `sink_limit` bytes, instead of held in memory.
  std::string sink{};
  std::uint64_t sink_limit = 0;
};
// A download the runtime wrote to a file: where, how many bytes, and their SHA-256 (hex).
struct Sink { std::string path; std::uint64_t bytes = 0; std::string sha256; };
// How the response ended, and its headers (lower-case names), which a binary body's decoder reads its type from.
struct Close {
  bool normal = true;
  ErrorKind error = ErrorKind::Truncated;
  std::vector<std::pair<std::string, std::string>> headers{};
  std::optional<Sink> sink{};
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
