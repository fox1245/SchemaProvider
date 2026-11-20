#pragma once
// Batches (D7): many chat requests the provider runs later, at a discount. OpenAI `/v1/batches` reads them from an
// uploaded JSONL file (`jsonl` writes it); Anthropic `/v1/messages/batches`, Google `batchGenerateContent` and
// OpenRouter `/api/v1/batches` take them inline. A `Batch` is a plain value a host journals and polls. Its results are
// JSONL lines, downloaded with `Results` (Google's and OpenRouter's inline results come with the batch:
// `inline_lines`, and `results` decodes OpenRouter's against the items), and `decode` turns each line into the outcome
// its request would have had, through the family codec of that request.
#include "codecs/endpoint.h"
#include "codecs/family_request.h"
#include "codecs/jobs.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sp::reason { struct Reason; }
namespace sp::batches {
// One request of a batch: what the host would otherwise send on its own.
using Request = FamilyRequest;
struct Item {
  // Up to 64 letters, digits, `_` and `-`; unique in the batch.
  std::string custom_id;
  Request request;
};
// OpenAI's input file: one line per item, encoded for the family descriptor (Chat or Responses). Upload it with
// `files::Upload` and purpose `batch`.
std::variant<std::string, Error> jsonl(const descriptor::ValidatedDescriptor& family, const std::vector<Item>&);

struct Submit {
  // Anthropic, Google and OpenRouter: the items, sent inline. A Google or OpenRouter batch runs one model, which every
  // item names; an OpenRouter batch's items are requests of the client's family.
  std::vector<Item> items{};
  // OpenAI: the uploaded input file, and the path its lines call (`/v1/responses`, `/v1/chat/completions`).
  std::string input_file{}, endpoint{};
  // Google: the batch's display name.
  std::string display_name{};
  // Members the endpoint's policy fact admits (OpenAI `metadata`; OpenRouter `callback_url` and `provider`), sent as
  // given.
  std::shared_ptr<const json::Document> extensions{};
};
struct Counts { std::uint64_t total = 0, pending = 0, succeeded = 0, failed = 0, cancelled = 0, expired = 0; };
struct Batch {
  std::string origin, shape;
  // OpenAI `batch_…`, Anthropic `msgbatch_…`, Google `batches/…`, OpenRouter `batch-…`.
  std::string id;
  // Completed once the provider ended the batch, whatever became of its requests. A status the SDK does not know is
  // Unknown, with the provider's word in `raw_status`.
  jobs::Status status = jobs::Status::Unknown;
  std::string raw_status;
  Counts counts;
  // Where the results are: OpenAI's output and error files, Google's responses file.
  std::string output_file{}, error_file{};
  // Anthropic: the results can be downloaded. Google and OpenRouter: the batch carries its results inline.
  bool results_ready = false;
  std::optional<std::uint64_t> created_at{}, expires_at{}, ended_at{};
  Usage usage;
  // The provider's batch object, kept whole.
  std::shared_ptr<const json::Document> raw{};
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events{};
};
using Outcome = std::variant<Batch, Failure>;
struct Get { std::string id; };
// OpenAI, Anthropic and Google. OpenRouter has no cancel: refused (`request.batches.cancel_unsupported`).
struct Cancel { std::string id; };
// Anthropic, Google and OpenRouter: an ended batch.
struct Delete { std::string id; };
// OpenRouter: one page of the batches of the key's workspace, newest first, without their results. Pass a page's
// `next` as the following request's `after`.
struct List {
  std::optional<std::uint32_t> limit{};
  std::string after{};
};
struct Page {
  std::vector<Batch> batches;
  // Empty on the last page.
  std::string next;
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events;
};
using PageOutcome = std::variant<Page, Failure>;
struct Deleted {
  std::string id;
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events;
};
using DeleteOutcome = std::variant<Deleted, Failure>;
// The results of an ended batch as JSONL, to a file or to memory, as `jobs::Download` fetches: OpenAI's output file
// (with `errors`, its error file), Anthropic's results, Google's responses file.
struct Results {
  Batch batch;
  std::string path{};
  std::uint64_t max_bytes = std::uint64_t{1} << 32;
  bool errors = false;
};
// A Google or OpenRouter batch's inline results, one JSONL line each.
std::vector<std::string> inline_lines(const Batch&);
// The id a result line answers: OpenAI and Anthropic `custom_id`, Google `key` or `metadata.key`.
std::optional<std::string> custom_id(std::string_view line);
// One result line, decoded as the outcome `original` would have had. `family` is the descriptor `original` is
// encoded for (the batch client's, or at OpenAI the Chat or Responses one). A request the provider failed is a
// `Failure` with the provider's error document in its raw events.
struct Line {
  std::string custom_id;
  sp::Outcome outcome;
};
std::variant<Line, Error> decode(const descriptor::ValidatedDescriptor& family, const Request& original, std::string_view line,
                                 SemanticLimits = {});
// An OpenRouter batch's inline results, each decoded (as `decode` does) against the item of its custom id, in the
// order the batch lists them, which is not the items' order. `family` is the descriptor the items were submitted
// through: its family must be the one whose requests the batch carries (the batch's `endpoint`, by the policy's items
// fact), else `batches.results.family`. A result of no item, or of one item twice, is `batches.results.custom_id`, and
// a batch without results `batches.results.missing`.
std::variant<std::vector<Line>, Error> results(const descriptor::ValidatedDescriptor& family, const Batch& batch,
                                               const std::vector<Item>& items, SemanticLimits = {});

using Encoded = endpoints::Encoded;
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Submit&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Get&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Cancel&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Delete&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Results&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const List&);
// The operation of the batches fact each call performs: `submit`, `get`, `cancel` and `delete`. The encoder sends the
// call to that operation's path, and the policy states its billing under it.
std::string_view operation(const Submit&) noexcept;
std::string_view operation(const Get&) noexcept;
std::string_view operation(const Cancel&) noexcept;
std::string_view operation(const Delete&) noexcept;
std::string_view operation(const List&) noexcept;
// The endpoint fact and operation a results download reaches: the batches fact's `results` for an Anthropic batch,
// which serves its results itself, and the files fact's `content` for OpenAI and Google, whose results are a file of
// the origin.
std::pair<std::string_view, std::string_view> results_operation(const Results&) noexcept;

using Close = endpoints::Close;
// A batch object, a deletion, or a page of batch objects, as the endpoint's shape answers it.
class Codec final : public endpoints::Codec {
 public:
  enum class Kind { Batch, Deleted, Page };
  // `subject` is the batch id the call named.
  Codec(const descriptor::ValidatedDescriptor&, Accumulator&, Kind, std::string subject, SemanticLimits = {});
  bool buffered(std::string_view body, Close close) override;
 private:
  bool fail(ErrorKind, const reason::Reason&);
  bool emit(const Event&);
  const descriptor::ValidatedDescriptor& descriptor_;
  Accumulator& accumulator_;
  Kind kind_;
  std::string subject_;
  SemanticLimits limits_;
};
Outcome typed(const sp::Outcome&);
DeleteOutcome typed_deleted(const sp::Outcome&);
PageOutcome typed_page(const sp::Outcome&);
}  // namespace sp::batches
