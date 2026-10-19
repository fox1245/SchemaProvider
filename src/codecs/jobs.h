#pragma once
// Provider jobs (D6): a submit starts work the provider finishes later (an OpenRouter video). The job is a
// `Handle`, a value a host journals and resumes from; ordinary calls poll it and download what it made.
#include "codecs/endpoint.h"
#include "core/value.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sp::jobs {
struct Handle {
  // The endpoint that started the job (`videos`), and the origin it was submitted to.
  std::string endpoint, origin;
  // The provider's job id.
  std::string id;
  std::string model;
};
enum class Status { Pending, Running, Completed, Failed, Cancelled, Expired, Unknown };
// What a submit or a poll learned about a job.
struct State {
  Handle handle;
  Status status = Status::Unknown;
  // The provider's own status word.
  std::string raw_status;
  // How many outputs a finished job made; download each by its index.
  std::size_t outputs = 0;
  // When the outputs expire, in Unix seconds, if the provider says.
  std::optional<std::uint64_t> expires_at{};
  // The provider's generation id, for its usage records.
  std::string generation;
  Usage usage;
  ServedModel model;
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events;
};
using Outcome = std::variant<State, Failure>;

struct Poll { Handle handle; };
// One output of a finished job. With `path`, the runtime writes it to that file as it arrives (at most `max_bytes`)
// and keeps only its size and SHA-256; without, it is held in memory under the client's response limit.
struct Download {
  Handle handle;
  std::size_t index = 0;
  std::string path{};
  std::uint64_t max_bytes = std::uint64_t{1} << 32;
};
struct Content {
  // In memory, or empty when written to `path`.
  std::optional<Media> media{};
  std::string path, content_type, sha256;
  std::uint64_t bytes = 0;
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events;
};
using DownloadOutcome = std::variant<Content, Failure>;

using Encoded = endpoints::Encoded;
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Poll&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Download&);

using Close = endpoints::Close;
// The state of a job, from a submit's or a poll's response.
class StateCodec final : public endpoints::Codec {
 public:
  StateCodec(const descriptor::ValidatedDescriptor&, Accumulator&, std::string requested, std::string endpoint, SemanticLimits = {});
  bool buffered(std::string_view body, Close close) override;
 private:
  bool fail(ErrorKind, std::string);
  bool emit(const Event&);
  const descriptor::ValidatedDescriptor& descriptor_;
  Accumulator& accumulator_;
  std::string requested_, endpoint_;
  SemanticLimits limits_;
};
// A downloaded output: its body in memory, or the file the runtime wrote it to.
class ContentCodec final : public endpoints::Codec {
 public:
  ContentCodec(Accumulator&, SemanticLimits = {});
  bool buffered(std::string_view body, Close close) override;
 private:
  bool fail(ErrorKind, std::string);
  bool emit(const Event&);
  Accumulator& accumulator_;
  SemanticLimits limits_;
};
Outcome typed(const sp::Outcome&);
DownloadOutcome typed_content(const sp::Outcome&);
}  // namespace sp::jobs
