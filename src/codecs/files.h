#pragma once
// Provider files (D7): upload, read, list, download and delete files at OpenAI and Anthropic (`/v1/files`) and
// Google (`/upload/v1beta/files`, `/v1beta/files`). A file is one `files::File` whatever the provider, and
// `media()` turns it into the `Media::file` reference a chat request names. Calls are dispatched by
// `runtime::Client::prepare`; the policy's `files` facts name the origins.
#include "codecs/endpoint.h"
#include "codecs/jobs.h"
#include "core/value.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sp::files {
enum class State { Unknown, Processing, Active, Failed };
struct File {
  // The origin that holds the file, and its id there: OpenAI `file-…`, Anthropic `file_…`, Google `files/…`.
  std::string origin, id;
  // Google's URI, which a generateContent request names; empty at the other providers.
  std::string uri{};
  std::string name{}, mime{};
  std::optional<std::uint64_t> bytes{};
  // Unix seconds.
  std::optional<std::uint64_t> created_at{}, expires_at{};
  State state = State::Unknown;
  // OpenAI's purpose (`user_data`, `batch`, `batch_output`).
  std::string purpose{};
  // Anthropic: whether its content can be downloaded (only files the service made).
  std::optional<bool> downloadable{};
  // Google's SHA-256 of the content, and that of the bytes this call uploaded, as lower-case hex.
  std::string sha256{}, uploaded_sha256{};
  // The provider's file object, kept whole.
  std::shared_ptr<const json::Document> raw{};
  // The call that returned the file; empty for a file in a listing.
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events{};
};
using Outcome = std::variant<File, Failure>;

// What an upload sends: bytes in memory, or a slice of a local file read while it is sent (`file.path` set).
struct Source {
  std::shared_ptr<const std::string> bytes{};
  endpoints::FileSlice file{};
};
// One request with the whole file: multipart form data at OpenAI and Anthropic, Google's multipart upload.
struct Upload {
  Source source;
  std::string filename, mime;
  // OpenAI only: the purpose (`user_data`, `batch`, `assistants`), required there, and when the file expires,
  // in seconds after its creation.
  std::string purpose{};
  std::optional<std::uint64_t> expires_after{};
};
// At Google `id` may also be the file's URI at this origin, as a result delivered by URI names it.
struct Get { std::string id; };
struct Delete { std::string id; };
// One page of the provider's files, newest first where the provider orders them. Pass a page's `next` as the
// following request's `after`.
struct List {
  std::optional<std::uint32_t> limit{};
  std::string after{};
  // OpenAI only: files of this purpose.
  std::string purpose{};
};
struct Page {
  std::vector<File> files;
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
// A file's content, written to `path` as it arrives (see `jobs::Download`) or held in memory. Providers serve the
// content of files they made: OpenAI batch output, Anthropic files a tool wrote, Google generated media.
struct Download {
  // As for `Get`.
  std::string id;
  std::string path{};
  std::uint64_t max_bytes = std::uint64_t{1} << 32;
};

// A chunked upload: OpenAI Uploads (start, parts, complete) or Google's resumable upload (start, then chunks, the
// last of which completes the file). Each step is an ordinary call; the session is a plain value a host journals
// between them, and resumes after a restart.
struct Session {
  std::string origin, shape;
  // OpenAI's upload id.
  std::string id{};
  // Google's upload URL. It lets anyone write the file without the API key: keep it as private as a credential.
  std::string url{};
  std::uint64_t size = 0, sent = 0;
  // Google: every chunk but the last is a multiple of this many bytes.
  std::uint64_t granularity = 0;
  // OpenAI: the ids of the parts sent so far, in order.
  std::vector<std::string> parts{};
  std::optional<std::uint64_t> expires_at{};
  // The provider's word for the upload (`pending`, `active`, `final`, `completed`, `cancelled`).
  std::string status{};
};
struct Progress {
  Session session;
  // The file, once the upload completed it.
  std::optional<File> file{};
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events;
};
using ProgressOutcome = std::variant<Progress, Failure>;
struct Start {
  std::string filename, mime;
  std::uint64_t size = 0;
  // OpenAI only, as for `Upload`.
  std::string purpose{};
  std::optional<std::uint64_t> expires_after{};
};
// The file's next bytes, from `session.sent`, in memory or as a file slice. An OpenAI part is at most 64 MB. At
// Google every chunk but the last is a multiple of the session's granularity, and the last one completes the file.
struct Part { Session session; Source source; };
// OpenAI: assemble the parts, in order, into the file.
struct Complete { Session session; };
// Google: how many bytes the provider holds, to resume a session after a call whose answer was lost.
struct Query { Session session; };
struct Cancel { Session session; };

using Encoded = endpoints::Encoded;
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Start&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Part&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Complete&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Query&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Cancel&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Upload&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Get&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Delete&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const List&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Download&);

// The file as a chat request's reference: Google's URI, or the OpenAI or Anthropic id, with its MIME type. It is
// refused for another origin than the file's, while the file is not active, and once it expired (`now` in Unix
// seconds).
std::variant<Media, Error> media(const File&, std::string_view origin, std::uint64_t now);

using Close = endpoints::Close;
// A file, a page of files, or a deletion, as the response of the endpoint's shape describes it.
class Codec final : public endpoints::Codec {
 public:
  enum class Kind { File, Page, Deleted };
  // `subject` is the file id the call named (a deletion's answer may not repeat it).
  Codec(const descriptor::ValidatedDescriptor&, Accumulator&, Kind, std::string subject, SemanticLimits = {});
  bool buffered(std::string_view body, Close close) override;
 private:
  bool fail(ErrorKind, std::string);
  bool emit(const Event&);
  const descriptor::ValidatedDescriptor& descriptor_;
  Accumulator& accumulator_;
  Kind kind_;
  std::string subject_;
  SemanticLimits limits_;
};
// A step of a chunked upload. `subject` is the session the call advanced, as `session_document` writes it.
class SessionCodec final : public endpoints::Codec {
 public:
  enum class Step { Start, Part, Complete, Query, Cancel };
  SessionCodec(const descriptor::ValidatedDescriptor&, Accumulator&, Step, std::string subject, SemanticLimits = {});
  bool buffered(std::string_view body, Close close) override;
 private:
  bool fail(ErrorKind, std::string);
  bool emit(const Event&);
  const descriptor::ValidatedDescriptor& descriptor_;
  Accumulator& accumulator_;
  Step step_;
  std::string subject_;
  SemanticLimits limits_;
};
// A session, and the size of the part a call sends, as the JSON a step's decoder reads back.
std::string session_document(const Session&, std::uint64_t part = 0);
// Whether `url` is a Google resumable upload URL at `origin`: under the origin's `/upload/` path, whose segments are
// names (no `.`, `..`, empty or percent-encoded segment), without control characters, quotes or a fragment.
bool upload_url(std::string_view origin, std::string_view url);
Outcome typed(const sp::Outcome&);
ProgressOutcome typed_progress(const sp::Outcome&);
PageOutcome typed_page(const sp::Outcome&);
DeleteOutcome typed_deleted(const sp::Outcome&);
}  // namespace sp::files
