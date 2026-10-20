#pragma once
// The decoder side every non-chat endpoint shares (D6): one buffered response document becomes accumulator events,
// which the endpoint's `typed()` turns into its own result.
#include "codecs/accumulator.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sp::endpoints {
// Bytes of a local file an upload sends: `size` bytes from `offset`, with their SHA-256 (lower-case hex) when the
// caller knows it. The transport checks the file's length before it sends and the digest before the last bytes.
struct FileSlice {
  std::string path;
  std::uint64_t offset = 0, size = 0;
  std::string sha256{};
};
// What the runtime hands a call's decoder beside the model, and no accessor of the prepared call shows: how many
// inputs the call sent and how many results it asked for (zero when it states none), and a value that must not reach
// a journal (a chunked upload's session URL).
struct Context {
  std::uint64_t inputs = 0, results = 0;
  std::string secret{};
};
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
  // Request headers beside the content and authentication headers (a resumable upload's commands).
  std::vector<std::pair<std::string, std::string>> headers{};
  // An upload read from a file as it is sent: `body`, then the slice, then `body_suffix`.
  std::optional<FileSlice> file{};
  std::string body_suffix{};
  Context context{};
};
// A download the runtime wrote to a file: where, how many bytes, and their SHA-256 (hex).
struct Sink { std::string path; std::uint64_t bytes = 0; std::string sha256; };
// How the response ended, and its headers (lower-case names), which a binary body's decoder reads its type from.
struct Close {
  bool normal = true;
  ErrorKind error = ErrorKind::Truncated;
  std::vector<std::pair<std::string, std::string>> headers{};
  std::optional<Sink> sink{};
  // The SHA-256 of an uploaded file slice, as the transport read it.
  std::string upload_sha256{};
  // The HTTP status of the response; zero when the decoder is run without a response.
  int status = 0;
};
// An RFC 3339 time (`2026-10-19T13:02:22.150902Z`, or with an offset) as Unix seconds.
std::optional<std::uint64_t> unix_seconds(std::string_view);
// The literal headers of the endpoint's fact, added to an encoded call.
inline void add_fact_headers(Encoded& encoded, const descriptor::EndpointFact& fact) {
  for (const auto& header : fact.headers) encoded.headers.push_back(header);
}
class Codec {
 public:
  virtual ~Codec() = default;
  virtual bool buffered(std::string_view body, Close close) = 0;
};
// The decoder of the endpoint `name` (`images`, `decisions`, ...), or null for a name without one.
std::unique_ptr<Codec> make_codec(std::string_view name, const descriptor::ValidatedDescriptor&, Accumulator&,
                                  std::string requested, const Context&, SemanticLimits);
}  // namespace sp::endpoints
