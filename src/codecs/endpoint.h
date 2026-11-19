#pragma once
// The decoder side every non-chat endpoint shares (D6): one buffered response document becomes accumulator events,
// which the endpoint's `typed()` turns into its own result.
#include "codecs/accumulator.h"
#include "core/endpoint_facts.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
// inputs the call sent and how many results it asked for (zero when it states none), a value that must not reach
// a journal (a chunked upload's session URL), and the job a poll asked about, which its answer must name.
struct Context {
  std::uint64_t inputs = 0, results = 0;
  std::string secret{};
  std::string job{};
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
  // The fact's bound for a buffered response of this endpoint, where it is above the client's; zero for none.
  std::uint64_t max_response_bytes = 0;
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
// The header a call sends `key` in under the credential `scheme` (descriptor::credential_header): none without a key.
// With a key and a scheme that names no header, the call would go out without the credential its caller gave it, so it
// is refused before it is sent: an InvalidConfig Error with the reason `credential.scheme.no_header` and a fixed text
// that names neither the scheme nor the key. The chat, endpoint and realtime paths take their credential from here.
std::variant<std::optional<std::pair<std::string, std::string>>, Error> credential(std::string_view scheme, std::string_view key);
// The literal headers and the response bound of the endpoint's fact, added to an encoded call.
inline void add_fact_headers(Encoded& encoded, const descriptor::EndpointFact& fact) {
  for (const auto& header : fact.headers) encoded.headers.push_back(header);
  encoded.max_response_bytes = fact.max_response_bytes;
}
// What every endpoint call states about what it bills by (core/endpoint_facts.h): the policy fact `fact` and the
// operation the call performs there, how the policy says the provider bills that operation (and the tier of the
// requests it carries), the descriptor's family and origin, whether that origin is a routing gateway, and the model
// the request names. A request type whose call bills its model adds its quantities and dimensions
// (`images::endpoint_facts`, ...); the other calls state none.
EndpointFacts call_facts(const descriptor::ValidatedDescriptor&, std::string_view fact, std::string_view operation, std::string_view model);
// The dimensions a call offers (EndpointFacts::dimensions), sorted by key: `operation` with the operation the call
// performs, each typed member of the request in `typed` that is set, under its typed name, and each member of
// `extensions` whose value is a string, a boolean (`true` or `false`) or an unsigned integer (in decimal), under its own
// name. A key or value outside the qualifier grammar
// (descriptor::valid_dimension_key and valid_dimension_value) is offered by no dimension, so no row that names it
// applies. A key offered twice (an extension named like a typed member) stays twice, and the bound refuses the call
// rather than choose one of them.
std::vector<std::pair<std::string, std::string>> dimensions(
    std::string_view operation, std::initializer_list<std::pair<std::string_view, std::optional<std::string>>> typed,
    const std::shared_ptr<const json::Document>& extensions);
// A typed member as a dimension's value: an unsigned count in decimal, a boolean as `true` or `false`.
std::optional<std::string> decimal(std::optional<std::uint64_t>);
std::optional<std::string> boolean(std::optional<bool>);
// A quantity the request states: `count` when it is set, and then `exact` when the request fixes it; not stated when it
// is unset, so a bound that needs it refuses.
EndpointQuantity requested(std::string unit, std::optional<std::uint64_t> count, bool exact = false);
// Tokens of `unit` (`input`, or `input_image` for the image tokens among them) bounded by the model's input limit,
// which counts per input `per` times, or by the host's measured input; and output tokens bounded by the model's output
// limit.
EndpointQuantity input_limited(std::string unit, std::uint64_t per = 1);
EndpointQuantity output_limited();
class Codec {
 public:
  virtual ~Codec() = default;
  virtual bool buffered(std::string_view body, Close close) = 0;
};
// The decoder of the endpoint `name` (`images`, `decisions`, ...), or null for a name without one.
std::unique_ptr<Codec> make_codec(std::string_view name, const descriptor::ValidatedDescriptor&, Accumulator&,
                                  std::string requested, const Context&, SemanticLimits);
}  // namespace sp::endpoints
