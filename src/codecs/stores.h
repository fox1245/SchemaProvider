#pragma once
// Provider-side search stores (D7, "Resources"): OpenAI vector stores (`/v1/vector_stores`, their files, file batches
// and search) and Google file search stores (`/v1beta/fileSearchStores`, their documents, and the operation an import
// starts). Each call resolves from the policy's closed `stores` facts by an operation id, so a caller never names a
// path or a header. A result is typed where the SDK gives it meaning (a store's and a file's status, counts and
// expiry, a pollable batch or operation, the results of a search); the rest stays in the provider's object.
#include "codecs/endpoint.h"
#include "codecs/jobs.h"
#include "core/value.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sp::reason { struct Reason; }
namespace sp::stores {
// A store's or a file's state, from OpenAI's `status` or Google's document `state`.
enum class Status { Unknown, InProgress, Completed, Failed, Cancelled, Expired };
// How many files are in each state. OpenAI counts every state; Google counts its documents as active
// (`completed`), pending (`in_progress`) and failed, and states no total.
struct Counts {
  std::optional<std::uint64_t> total{}, in_progress{}, completed{}, failed{}, cancelled{};
};
struct Store {
  // The origin that holds the store, and its id there: OpenAI `vs_…`, Google `fileSearchStores/…`.
  std::string origin, id;
  // OpenAI `name`, Google `displayName`.
  std::string name{};
  // OpenAI's status; Google states none (Unknown).
  Status status = Status::Unknown;
  std::string raw_status{};
  Counts files{};
  // OpenAI `usage_bytes`, Google `sizeBytes`.
  std::optional<std::uint64_t> bytes{};
  // OpenAI's expiry policy: the days after the store was last active; and when it expires. Unix seconds.
  std::optional<std::uint64_t> expires_after_days{}, expires_at{}, created_at{}, last_active_at{};
  // The provider's store object, kept whole.
  std::shared_ptr<const json::Document> raw{};
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events{};
};
using StoreOutcome = std::variant<Store, Failure>;
// Why the provider could not index a file: OpenAI's `last_error`.
struct FileError { std::string code, message; };
// A file in a store: an OpenAI vector store file, or a Google document.
struct StoreFile {
  // OpenAI's file id and `vector_store_id`; Google's document name (`fileSearchStores/…/documents/…`) and its store.
  std::string origin, id, store{};
  Status status = Status::Unknown;
  std::string raw_status{};
  std::optional<FileError> last_error{};
  // OpenAI `attributes`, Google `customMetadata`, as the provider wrote them; null when it states none.
  std::shared_ptr<const json::Document> attributes{};
  // OpenAI `usage_bytes` (the store's bytes for the file), Google `sizeBytes`.
  std::optional<std::uint64_t> bytes{}, created_at{};
  // Google `displayName` and `mimeType`.
  std::string name{}, mime{};
  std::shared_ptr<const json::Document> raw{};
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events{};
};
using FileOutcome = std::variant<StoreFile, Failure>;
// Work the provider finishes later, a value a host journals and polls (as jobs::Handle): an OpenAI file batch
// (`vsfb_…` in the store `store`), or the operation a Google import starts (`fileSearchStores/…/operations/…`).
struct Handle { std::string origin, shape, store, id; };
struct Batch {
  Handle handle;
  // Running until the provider ends it; Google's operation is Completed or Failed once `done`.
  jobs::Status status = jobs::Status::Unknown;
  std::string raw_status{};
  Counts files{};
  // Google: the document an import made, once done.
  std::string document{};
  // Google: the operation's error (`error.code` in decimal, `error.message`).
  std::optional<FileError> error{};
  std::optional<std::uint64_t> created_at{};
  std::shared_ptr<const json::Document> raw{};
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events{};
};
using BatchOutcome = std::variant<Batch, Failure>;
// A part of a file's content: OpenAI `{type, text}`.
struct ContentPart { std::string type, text; };
struct SearchResult {
  std::string file_id, filename;
  std::optional<double> score{};
  std::shared_ptr<const json::Document> attributes{};
  std::vector<ContentPart> content{};
};
struct SearchPage {
  std::vector<SearchResult> results;
  // The queries the provider searched (OpenAI `search_query`, rewritten when the request asked for it).
  std::vector<std::string> search_query{};
  bool has_more = false;
  std::shared_ptr<const json::Document> raw{};
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events;
};
using SearchOutcome = std::variant<SearchPage, Failure>;
// The parsed content of a file in a store (OpenAI `file_content`); `next` is the provider's next page token.
struct Content {
  std::vector<ContentPart> parts;
  std::string next;
  bool has_more = false;
  std::shared_ptr<const json::Document> raw{};
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events;
};
using ContentOutcome = std::variant<Content, Failure>;
// One page of stores, or of the files of a store or a batch. Pass `next` as the following request's `after`.
struct Page {
  std::vector<Store> stores;
  std::vector<StoreFile> files;
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

// The JSON members below that hold a provider's own object (`metadata`, `attributes`, `filters`, ...) are sent as given;
// each must be the kind the provider documents (an object, or for Google's `custom_metadata` an array), else the
// request is refused before it is sent.
struct Create {
  // OpenAI `name`, Google `displayName`.
  std::string name{};
  // OpenAI only: the files to index at once, a description, an expiry in days after the store was last active, its
  // `metadata` and `chunking_strategy`.
  std::vector<std::string> file_ids{};
  std::string description{};
  std::optional<std::uint64_t> expires_after_days{};
  std::shared_ptr<const json::Document> metadata{}, chunking_strategy{};
  // Google only: the embedding model's resource name (`models/…`).
  std::string embedding_model{};
};
// OpenAI `vs_…`, Google `fileSearchStores/…`.
struct Get { std::string store; };
struct List {
  std::optional<std::uint32_t> limit{};
  std::string after{};
};
// OpenAI only: a new name, expiry or metadata; members left unset are left as they are.
struct Update {
  std::string store;
  std::optional<std::string> name{};
  std::optional<std::uint64_t> expires_after_days{};
  std::shared_ptr<const json::Document> metadata{};
};
struct Delete {
  std::string store;
  // Google: delete the store's documents with it (otherwise a store that holds any is not deleted).
  bool force = false;
};
// OpenAI: the chunks of the store's files that answer the query.
struct Search {
  std::string store;
  // One query, or several; none is empty.
  std::vector<std::string> query;
  // At most the policy fact's `max_results`.
  std::optional<std::uint32_t> max_num_results{};
  // The provider's ComparisonFilter or CompoundFilter, and its `ranking_options`.
  std::shared_ptr<const json::Document> filters{}, ranking_options{};
  std::optional<bool> rewrite_query{};
};
// OpenAI: index a Files API file in the store.
struct AddFile {
  std::string store, file;
  std::shared_ptr<const json::Document> attributes{}, chunking_strategy{};
};
// OpenAI: the file `id` in the store; Google: the document `id` (`<store>/documents/…`).
struct GetFile { std::string store, id; };
struct ListFiles {
  std::string store;
  // OpenAI: the files of this file batch of the store instead of all of them.
  std::string batch{};
  std::optional<std::uint32_t> limit{};
  std::string after{};
  // OpenAI: files of this status (`in_progress`, `completed`, `failed`, `cancelled`).
  std::string filter{};
};
// OpenAI: replace the file's attributes (null clears them).
struct UpdateFile {
  std::string store, id;
  std::shared_ptr<const json::Document> attributes;
};
struct DeleteFile {
  std::string store, id;
  // Google: delete the document's chunks with it.
  bool force = false;
};
// OpenAI: the parsed content of a file in the store.
struct FileContent { std::string store, id; };
// OpenAI: index several Files API files at once: `file_ids` with shared `attributes` and `chunking_strategy`, or
// `files`, the provider's array of `{file_id, attributes, chunking_strategy}`.
struct CreateBatch {
  std::string store;
  std::vector<std::string> file_ids{};
  std::shared_ptr<const json::Document> attributes{}, chunking_strategy{}, files{};
};
// Google: index a Files API file (`files/…`) in the store; the answer is an operation to poll.
struct Import {
  std::string store, file;
  std::shared_ptr<const json::Document> custom_metadata{}, chunking_config{};
};
// A batch's or an operation's state, and an OpenAI batch's cancellation.
struct Poll { Handle handle; };
struct Cancel { Handle handle; };

using Encoded = endpoints::Encoded;
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Create&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Get&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const List&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Update&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Delete&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Search&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const AddFile&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const GetFile&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const ListFiles&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const UpdateFile&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const DeleteFile&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const FileContent&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const CreateBatch&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Import&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Poll&);
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Cancel&);
// The operation of the stores fact each call performs at the shape `shape` (`openai`, `google`): `create`, `get`,
// `list`, `update`, `delete`, `search`, `file_add`, `file_get`, `file_list`, `file_update`, `file_delete`,
// `file_content`, `batch_create`, `batch_get`, `batch_cancel` and `batch_files` at OpenAI; `create`, `get`, `list`,
// `delete`, `import`, `document_get`, `document_list`, `document_delete` and `operation_get` at Google. The encoder
// sends the call to that operation's path, and the policy states its billing under it.
std::string_view operation(std::string_view shape, const Create&) noexcept;
std::string_view operation(std::string_view shape, const Get&) noexcept;
std::string_view operation(std::string_view shape, const List&) noexcept;
std::string_view operation(std::string_view shape, const Update&) noexcept;
std::string_view operation(std::string_view shape, const Delete&) noexcept;
std::string_view operation(std::string_view shape, const Search&) noexcept;
std::string_view operation(std::string_view shape, const AddFile&) noexcept;
std::string_view operation(std::string_view shape, const GetFile&) noexcept;
std::string_view operation(std::string_view shape, const ListFiles&) noexcept;
std::string_view operation(std::string_view shape, const UpdateFile&) noexcept;
std::string_view operation(std::string_view shape, const DeleteFile&) noexcept;
std::string_view operation(std::string_view shape, const FileContent&) noexcept;
std::string_view operation(std::string_view shape, const CreateBatch&) noexcept;
std::string_view operation(std::string_view shape, const Import&) noexcept;
std::string_view operation(std::string_view shape, const Poll&) noexcept;
std::string_view operation(std::string_view shape, const Cancel&) noexcept;
// The shape of the stores fact at the descriptor's origin, empty when there is none.
std::string_view shape(const descriptor::ValidatedDescriptor&) noexcept;

using Close = endpoints::Close;
// A store, a file, a batch or operation, a page of stores or files, search results, a file's content, or a deletion,
// as the response of the fact's shape describes it.
class Codec final : public endpoints::Codec {
 public:
  enum class Kind { Store, File, Batch, Stores, Files, Search, Content, Deleted };
  // `subject` is the id the call named (a deletion's answer may not repeat it).
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
StoreOutcome typed_store(const sp::Outcome&);
FileOutcome typed_file(const sp::Outcome&);
BatchOutcome typed_batch(const sp::Outcome&);
PageOutcome typed_page(const sp::Outcome&);
SearchOutcome typed_search(const sp::Outcome&);
ContentOutcome typed_content(const sp::Outcome&);
DeleteOutcome typed_deleted(const sp::Outcome&);
}  // namespace sp::stores
