// Provider-side search stores (D7, "Resources"): the request each operation of the policy's `stores` facts takes at
// OpenAI (vector stores) and Google (file search stores), the refusals before any byte leaves, the typed results of
// the documented response objects, and the money each call states: a create, an index or a search has no bound and no
// estimate (a fee that is not per request), a read is free.
#include "codecs/stores.h"
#include "core/endpoint_pricing.h"
#include "core/enum_names.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include "runtime/client.h"
#include "runtime/cost.h"
#include "sp/config_defaults.h"
#include "support/endpoint_fakes.h"
#include <functional>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>

namespace {
using namespace sp;
using namespace sp::test;
using namespace std::chrono_literals;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
constexpr std::string_view openai = "https://api.openai.com", google = "https://generativelanguage.googleapis.com";
descriptor::ValidatedDescriptor at(std::string_view origin, descriptor::PolicySnapshot policy = descriptor::builtin_policy()) {
  const bool g = origin == google;
  const std::string family = g ? "google.generate" : "openai.chat";
  const std::string path = g ? "/v1beta/models/{model}:generateContent" : "/v1/chat/completions";
  const std::string stream = g ? "/v1beta/models/{model}:streamGenerateContent?alt=sse" : path;
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"stores-fixture","family":)" + json::quote(family) +
                                 R"(,"connection":{"base_url":)" + json::quote(origin) + R"(,"paths":{"buffered":)" + json::quote(path) +
                                 R"(,"streaming":)" + json::quote(stream) + "}}}", std::move(policy));
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::shared_ptr<const json::Document> document(std::string_view text) {
  auto parsed = json::parse(text);
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
template <class Request> endpoints::Encoded encoded(std::string_view origin, const Request& r) {
  auto result = stores::encode(at(origin), r);
  if (const auto* error = std::get_if<Error>(&result)) throw std::runtime_error("encode refused: " + error->reason);
  return std::get<endpoints::Encoded>(std::move(result));
}
template <class Request> Error refused(std::string_view origin, const Request& r) {
  auto result = stores::encode(at(origin), r);
  CHECK(std::holds_alternative<Error>(result));
  return std::get<Error>(result);
}
template <class Request> bool invalid(std::string_view origin, const Request& r, std::string_view reason) {
  const auto error = refused(origin, r);
  return error.kind == ErrorKind::InvalidRequest && error.reason == reason;
}
template <class Request> bool unsupported(std::string_view origin, const Request& r) { return refused(origin, r).kind == ErrorKind::Unsupported; }
// A read: GET, no body and no content type, retried as a read.
bool read(const endpoints::Encoded& wire, std::string_view path) {
  return wire.method == "GET" && wire.path == path && wire.body.empty() && wire.content_type.empty() && !wire.charge_on_accept;
}

// The OpenAI vector store operations, as the API reference documents each request
// (https://developers.openai.com/api/docs/api-reference/vector-stores and its method pages).
void openai_requests() {
  stores::Create create{"Support FAQ", {"file-abc123"}, "FAQ", 7, document(R"({"team":"support"})"), document(R"({"type":"auto"})")};
  const auto made = encoded(openai, create);
  CHECK(made.path == "/v1/vector_stores" && made.method == "POST" && made.content_type == "application/json" && made.auth == "bearer" &&
        made.charge_on_accept && made.model.empty());
  CHECK(made.body == R"({"name":"Support FAQ","description":"FAQ","file_ids":["file-abc123"],"expires_after":{"anchor":"last_active_at","days":7},)"
                     R"("metadata":{"team":"support"},"chunking_strategy":{"type":"auto"}})");
  CHECK(encoded(openai, stores::Create{}).body == "{}");
  const auto got = encoded(openai, stores::Get{"vs_abc123"});
  CHECK(read(got, "/v1/vector_stores/vs_abc123") && got.model == "vs_abc123");
  CHECK(read(encoded(openai, stores::List{20, "vs_abc456"}), "/v1/vector_stores?limit=20&after=vs_abc456"));
  CHECK(read(encoded(openai, stores::List{}), "/v1/vector_stores"));
  const auto renamed = encoded(openai, stores::Update{"vs_abc123", "Support FAQ v2", 30});
  // Metadata, expiry and attributes changes are free (`none`), so they are retried as reads are.
  CHECK(renamed.path == "/v1/vector_stores/vs_abc123" && renamed.method == "POST" && !renamed.charge_on_accept &&
        renamed.body == R"({"name":"Support FAQ v2","expires_after":{"anchor":"last_active_at","days":30}})");
  const auto gone = encoded(openai, stores::Delete{"vs_abc123"});
  CHECK(gone.method == "DELETE" && gone.path == "/v1/vector_stores/vs_abc123" && gone.body.empty() && !gone.charge_on_accept);

  // Search: one query as a string, several as an array; the filter and ranking options as the provider's objects.
  stores::Search search{"vs_abc123", {"What is the return policy?"}, 5, document(R"({"type":"eq","key":"author","value":"Jane Smith"})"),
                        document(R"({"ranker":"auto","score_threshold":0.5})"), true};
  const auto searched = encoded(openai, search);
  CHECK(searched.path == "/v1/vector_stores/vs_abc123/search" && searched.method == "POST" && searched.model == "vs_abc123" && searched.charge_on_accept);
  CHECK(searched.body == R"({"query":"What is the return policy?","max_num_results":5,"filters":{"type":"eq","key":"author","value":"Jane Smith"},)"
                         R"("ranking_options":{"ranker":"auto","score_threshold":0.5},"rewrite_query":true})");
  CHECK(encoded(openai, stores::Search{"vs_a", {"a", "b"}}).body == R"({"query":["a","b"]})");
  CHECK(encoded(openai, stores::Search{"vs_a", {"a"}, 50}).body == R"({"query":"a","max_num_results":50})");

  // The files of a store.
  const auto added = encoded(openai, stores::AddFile{"vs_abc123", "file-abc123", document(R"({"category":"finance"})"),
                                                       document(R"({"type":"static","static":{"max_chunk_size_tokens":1200,"chunk_overlap_tokens":200}})")});
  CHECK(added.path == "/v1/vector_stores/vs_abc123/files" && added.method == "POST" && added.charge_on_accept && added.model == "file-abc123");
  CHECK(added.body == R"({"file_id":"file-abc123","attributes":{"category":"finance"},)"
                      R"("chunking_strategy":{"type":"static","static":{"max_chunk_size_tokens":1200,"chunk_overlap_tokens":200}}})");
  CHECK(read(encoded(openai, stores::GetFile{"vs_abc123", "file-abc123"}), "/v1/vector_stores/vs_abc123/files/file-abc123"));
  CHECK(read(encoded(openai, stores::ListFiles{"vs_abc123", {}, 10, "file-abc456", "completed"}),
             "/v1/vector_stores/vs_abc123/files?limit=10&after=file-abc456&filter=completed"));
  CHECK(read(encoded(openai, stores::ListFiles{"vs_abc123", "vsfb_abc123"}), "/v1/vector_stores/vs_abc123/file_batches/vsfb_abc123/files"));
  const auto retagged = encoded(openai, stores::UpdateFile{"vs_abc123", "file-abc123", document(R"({"year":2026,"draft":false})")});
  CHECK(retagged.path == "/v1/vector_stores/vs_abc123/files/file-abc123" && retagged.method == "POST" && !retagged.charge_on_accept && retagged.body == R"({"attributes":{"year":2026,"draft":false}})");
  CHECK(encoded(openai, stores::UpdateFile{"vs_abc123", "file-abc123", nullptr}).body == R"({"attributes":null})");
  const auto removed = encoded(openai, stores::DeleteFile{"vs_abc123", "file-abc123"});
  CHECK(removed.method == "DELETE" && removed.path == "/v1/vector_stores/vs_abc123/files/file-abc123" && removed.model == "file-abc123");
  CHECK(read(encoded(openai, stores::FileContent{"vs_abc123", "file-abc123"}), "/v1/vector_stores/vs_abc123/files/file-abc123/content"));

  // File batches: `file_ids` with shared members, or `files`; then polled and cancelled by their handle.
  const auto batch = encoded(openai, stores::CreateBatch{"vs_abc123", {"file-abc123", "file-abc456"}, document(R"({"category":"finance"})")});
  CHECK(batch.path == "/v1/vector_stores/vs_abc123/file_batches" && batch.method == "POST" && batch.charge_on_accept &&
        batch.body == R"({"file_ids":["file-abc123","file-abc456"],"attributes":{"category":"finance"}})");
  stores::CreateBatch listed{"vs_abc123"};
  listed.files = document(R"([{"file_id":"file-abc123","attributes":{"category":"finance"}}])");
  CHECK(encoded(openai, listed).body == R"({"files":[{"file_id":"file-abc123","attributes":{"category":"finance"}}]})");
  const stores::Handle handle{std::string(openai), "openai", "vs_abc123", "vsfb_abc123"};
  const auto polled = encoded(openai, stores::Poll{handle});
  CHECK(read(polled, "/v1/vector_stores/vs_abc123/file_batches/vsfb_abc123") && polled.model == "vs_abc123/vsfb_abc123");
  const auto cancelled = encoded(openai, stores::Cancel{handle});
  CHECK(cancelled.method == "POST" && cancelled.path == "/v1/vector_stores/vs_abc123/file_batches/vsfb_abc123/cancel" && cancelled.body.empty() &&
        cancelled.content_type.empty() && !cancelled.charge_on_accept && cancelled.model == "vs_abc123/vsfb_abc123");
}

// The Google file search store operations, as the REST reference documents each request
// (https://ai.google.dev/api/file-search/file-search-stores and .../documents, and the file search guide).
void google_requests() {
  const auto made = encoded(google, stores::Create{"My Store", {}, {}, {}, {}, {}, "models/fixture-embedding"});
  // A create indexes nothing, so it is free and retried; an import is billed and is not.
  CHECK(made.path == "/v1beta/fileSearchStores" && made.method == "POST" && made.auth == "x-goog-api-key" && !made.charge_on_accept &&
        made.body == R"({"displayName":"My Store","embeddingModel":"models/fixture-embedding"})");
  constexpr std::string_view store = "fileSearchStores/my-file-search-store-123a456b789c";
  CHECK(read(encoded(google, stores::Get{std::string(store)}), "/v1beta/fileSearchStores/my-file-search-store-123a456b789c"));
  CHECK(read(encoded(google, stores::List{10, "next+page/="}), "/v1beta/fileSearchStores?pageSize=10&pageToken=next%2Bpage%2F%3D"));
  const auto gone = encoded(google, stores::Delete{std::string(store), true});
  CHECK(gone.method == "DELETE" && gone.path == "/v1beta/fileSearchStores/my-file-search-store-123a456b789c?force=true" && !gone.charge_on_accept);
  CHECK(encoded(google, stores::Delete{std::string(store)}).path == "/v1beta/fileSearchStores/my-file-search-store-123a456b789c");
  const auto imported = encoded(google, stores::Import{std::string(store), "files/abc-123", document(R"([{"key":"author","stringValue":"Robert Graves"}])"),
                                                        document(R"({"whiteSpaceConfig":{"maxTokensPerChunk":200,"maxOverlapTokens":20}})")});
  CHECK(imported.path == "/v1beta/fileSearchStores/my-file-search-store-123a456b789c:importFile" && imported.method == "POST" && imported.charge_on_accept);
  CHECK(imported.body == R"({"fileName":"files/abc-123","customMetadata":[{"key":"author","stringValue":"Robert Graves"}],)"
                         R"("chunkingConfig":{"whiteSpaceConfig":{"maxTokensPerChunk":200,"maxOverlapTokens":20}}})");
  const std::string doc = std::string(store) + "/documents/my-awesome-doc-123a456b789c";
  const auto got = encoded(google, stores::GetFile{std::string(store), doc});
  CHECK(read(got, "/v1beta/" + doc) && got.model == doc);
  CHECK(read(encoded(google, stores::ListFiles{std::string(store), {}, 20, "t2"}), "/v1beta/" + std::string(store) + "/documents?pageSize=20&pageToken=t2"));
  const auto removed = encoded(google, stores::DeleteFile{std::string(store), doc, true});
  CHECK(removed.method == "DELETE" && removed.path == "/v1beta/" + doc + "?force=true");
  // An import's operation, and an upload's, polled by name under the store.
  const std::string operation = std::string(store) + "/operations/op-1";
  const auto polled = encoded(google, stores::Poll{{std::string(google), "google", std::string(store), operation}});
  CHECK(read(polled, "/v1beta/" + operation) && polled.model == operation);
  const std::string upload = std::string(store) + "/upload/operations/op-2";
  CHECK(read(encoded(google, stores::Poll{{std::string(google), "google", std::string(store), upload}}), "/v1beta/" + upload));
}

void refusals() {
  constexpr auto store_invalid = "request.stores.store_invalid", id_invalid = "request.stores.id_invalid", file_invalid = "request.stores.file_invalid",
                 handle_invalid = "request.stores.handle_invalid";
  // `.` and `..` are made of token characters but are not names: they move the request to another path.
  for (const char* id : {".", "..", "", "vs/../files", "vs a"}) {
    CHECK(invalid(openai, stores::Get{id}, store_invalid));
    CHECK(invalid(openai, stores::Delete{id}, store_invalid));
    CHECK(invalid(openai, stores::Search{id, {"q"}}, store_invalid));
    CHECK(invalid(openai, stores::GetFile{"vs_a", id}, id_invalid));
    CHECK(invalid(openai, stores::AddFile{"vs_a", id}, file_invalid));
    CHECK(invalid(openai, stores::Poll{{std::string(openai), "openai", "vs_a", id}}, handle_invalid));
  }
  CHECK(invalid(openai, stores::ListFiles{"vs_a", ".."}, handle_invalid));
  CHECK(invalid(openai, stores::CreateBatch{"vs_a", {"file-a", ".."}}, file_invalid));
  CHECK(invalid(openai, stores::Create{"x", {".."}}, file_invalid));
  // A Google store is `fileSearchStores/<name>`: not a bare name, another resource, a path out of it, or a URL.
  for (const char* id : {"my-store", "files/abc", "fileSearchStores/", "fileSearchStores/..", "fileSearchStores/a/b",
                         "https://generativelanguage.googleapis.com/v1beta/fileSearchStores/a", "https://example.com/v1beta/fileSearchStores/a"}) {
    CHECK(invalid(google, stores::Get{id}, store_invalid));
    CHECK(invalid(google, stores::Import{id, "files/abc"}, store_invalid));
  }
  // A document and an operation are named under the store the call names.
  CHECK(invalid(google, stores::GetFile{"fileSearchStores/a", "fileSearchStores/b/documents/d"}, id_invalid));
  CHECK(invalid(google, stores::GetFile{"fileSearchStores/a", "fileSearchStores/a/documents/.."}, id_invalid));
  CHECK(invalid(google, stores::DeleteFile{"fileSearchStores/a", "fileSearchStores/a/documents/d/x"}, id_invalid));
  CHECK(invalid(google, stores::Poll{{std::string(google), "google", "fileSearchStores/a", "fileSearchStores/b/operations/o"}}, handle_invalid));
  CHECK(invalid(google, stores::Poll{{std::string(google), "google", "fileSearchStores/a", "fileSearchStores/a/documents/o"}}, handle_invalid));
  CHECK(invalid(google, stores::Import{"fileSearchStores/a", "abc"}, file_invalid));
  CHECK(invalid(google, stores::Import{"fileSearchStores/a", "files/../models"}, file_invalid));
  // A batch or operation is polled at the origin and shape that started it.
  CHECK(invalid(openai, stores::Poll{{std::string(google), "openai", "vs_a", "vsfb_1"}}, "request.stores.handle_origin_mismatch"));
  CHECK(invalid(google, stores::Poll{{std::string(google), "openai", "fileSearchStores/a", "fileSearchStores/a/operations/o"}},
                "request.stores.handle_origin_mismatch"));
  CHECK(invalid(openai, stores::Cancel{{"https://api.openai.com.evil.example", "openai", "vs_a", "vsfb_1"}}, "request.stores.handle_origin_mismatch"));
  // A search names a query, none empty, and asks for 1 to the fact's `max_results` (50) results.
  CHECK(invalid(openai, stores::Search{"vs_a", {}}, "request.stores.query_empty"));
  CHECK(invalid(openai, stores::Search{"vs_a", {""}}, "request.stores.query_empty"));
  CHECK(invalid(openai, stores::Search{"vs_a", {"a", ""}}, "request.stores.query_empty"));
  CHECK(invalid(openai, stores::Search{"vs_a", {"a"}, 0}, "request.stores.results_invalid"));
  CHECK(invalid(openai, stores::Search{"vs_a", {"a"}, 51}, "request.stores.results_invalid"));
  // The provider's objects are the kind it documents.
  CHECK(invalid(openai, stores::Search{"vs_a", {"a"}, std::nullopt, document("[]")}, "request.stores.object_invalid"));
  CHECK(invalid(openai, stores::AddFile{"vs_a", "file-a", document("\"x\"")}, "request.stores.object_invalid"));
  CHECK(invalid(google, stores::Import{"fileSearchStores/a", "files/a", document("{}")}, "request.stores.object_invalid"));
  CHECK(invalid(openai, stores::CreateBatch{"vs_a"}, "request.stores.batch_files_invalid"));
  stores::CreateBatch both{"vs_a", {"file-a"}};
  both.files = document(R"([{"file_id":"file-b"}])");
  CHECK(invalid(openai, both, "request.stores.batch_files_invalid"));
  CHECK(invalid(openai, stores::Update{"vs_a"}, "request.stores.update_empty"));
  CHECK(invalid(openai, stores::Update{"vs_a", std::nullopt, 0}, "request.stores.expiry_invalid"));
  CHECK(invalid(openai, stores::List{0}, "request.stores.limit_invalid"));
  CHECK(invalid(openai, stores::ListFiles{"vs_a", {}, std::nullopt, {}, "in progress"}, "request.stores.filter_invalid"));
  // An operation or a member the shape does not have, and an origin without a stores fact.
  CHECK(unsupported(google, stores::Search{"fileSearchStores/a", {"q"}}));
  CHECK(unsupported(google, stores::Update{"fileSearchStores/a", "x"}));
  CHECK(unsupported(google, stores::AddFile{"fileSearchStores/a", "file-a"}));
  CHECK(unsupported(google, stores::Cancel{{std::string(google), "google", "fileSearchStores/a", "fileSearchStores/a/operations/o"}}));
  CHECK(unsupported(google, stores::Create{"x", {"file-a"}}));
  CHECK(unsupported(google, stores::ListFiles{"fileSearchStores/a", {}, std::nullopt, {}, "completed"}));
  CHECK(unsupported(openai, stores::Import{"vs_a", "file-a"}));
  CHECK(unsupported(openai, stores::Create{"x", {}, {}, {}, {}, {}, "models/fixture-embedding"}));
  CHECK(unsupported(openai, stores::Delete{"vs_a", true}));
  CHECK(unsupported("https://openrouter.ai", stores::List{}));
  CHECK(refused("https://openrouter.ai", stores::Get{"vs_a"}).reason == "request.stores.endpoint_not_admitted");
}

sp::Outcome decoded(std::string_view origin, stores::Codec::Kind kind, std::string_view body, std::string subject = {}) {
  const auto d = at(origin);
  Accumulator accumulator;
  stores::Codec codec(d, accumulator, kind, std::move(subject));
  codec.buffered(body, endpoints::Close{});
  CHECK(accumulator.outcome());
  return *accumulator.outcome();
}
// The documented response objects, and what each becomes. OpenAI: the response examples of
// https://developers.openai.com/api/reference/resources/vector_stores/methods/{create,list,search} and of
// .../vector_stores/subresources/{files,file_batches}/methods/{retrieve,list,create,content}.
void decoded_openai() {
  using Kind = stores::Codec::Kind;
  const auto s = std::get<stores::Store>(stores::typed_store(decoded(openai, Kind::Store,
      R"({"id":"vs_abc123","object":"vector_store","created_at":1699061776,"name":"Support FAQ","description":"Contains commonly asked questions.",)"
      R"("file_counts":{"in_progress":0,"completed":3,"failed":0,"cancelled":0,"total":3},"usage_bytes":139920,"status":"completed",)"
      R"("last_active_at":null,"metadata":{},"expires_after":{"anchor":"last_active_at","days":7},"expires_at":1699666576})")));
  CHECK(s.origin == openai && s.id == "vs_abc123" && s.name == "Support FAQ" && s.status == stores::Status::Completed && s.raw_status == "completed" &&
        s.files.total == 3u && s.files.completed == 3u && s.files.in_progress == 0u && s.bytes == 139920u && s.created_at == 1699061776u &&
        !s.last_active_at && s.expires_after_days == 7u && s.expires_at == 1699666576u && s.raw && s.raw->root().get("description").is_string());
  CHECK(std::get<stores::Store>(stores::typed_store(decoded(openai, Kind::Store, R"({"id":"vs_x","status":"expired"})"))).status == stores::Status::Expired);
  const auto page = std::get<stores::Page>(stores::typed_page(decoded(openai, Kind::Stores,
      R"({"object":"list","data":[{"id":"vs_abc123","object":"vector_store","usage_bytes":1},{"id":"vs_abc456","object":"vector_store"}],)"
      R"("first_id":"vs_abc123","last_id":"vs_abc456","has_more":true})")));
  CHECK(page.stores.size() == 2 && page.files.empty() && page.stores[1].id == "vs_abc456" && page.next == "vs_abc456" && page.stores[0].raw);
  CHECK(std::get<stores::Page>(stores::typed_page(decoded(openai, Kind::Stores, R"({"data":[],"has_more":false,"last_id":null})"))).next.empty());

  const auto f = std::get<stores::StoreFile>(stores::typed_file(decoded(openai, Kind::File,
      R"({"id":"file-abc123","object":"vector_store.file","created_at":1699061776,"vector_store_id":"vs_abc123","status":"failed",)"
      R"("last_error":{"code":"unsupported_file","message":"The file type is not supported."},"usage_bytes":1234,"attributes":{"category":"finance"}})")));
  CHECK(f.id == "file-abc123" && f.store == "vs_abc123" && f.status == stores::Status::Failed && f.last_error && f.last_error->code == "unsupported_file" &&
        f.bytes == 1234u && f.created_at == 1699061776u && f.attributes && f.attributes->root().get("category").as_string() == "finance");
  const auto files = std::get<stores::Page>(stores::typed_page(decoded(openai, Kind::Files,
      R"({"object":"list","data":[{"id":"file-abc123","object":"vector_store.file","vector_store_id":"vs_abc123","usage_bytes":1234,"status":"completed","last_error":null}],)"
      R"("first_id":"file-abc123","last_id":"file-abc123","has_more":false})")));
  CHECK(files.files.size() == 1 && files.stores.empty() && files.files[0].status == stores::Status::Completed && !files.files[0].last_error);

  const auto b = std::get<stores::Batch>(stores::typed_batch(decoded(openai, Kind::Batch,
      R"({"id":"vsfb_abc123","object":"vector_store.file_batch","created_at":1699061776,"vector_store_id":"vs_abc123","status":"in_progress",)"
      R"("file_counts":{"in_progress":1,"completed":1,"failed":0,"cancelled":0,"total":0}})")));
  CHECK(b.handle.origin == openai && b.handle.shape == "openai" && b.handle.store == "vs_abc123" && b.handle.id == "vsfb_abc123" &&
        b.status == jobs::Status::Running && b.files.in_progress == 1u && b.created_at == 1699061776u);
  // The handle polls the same batch.
  CHECK(encoded(openai, stores::Poll{b.handle}).path == "/v1/vector_stores/vs_abc123/file_batches/vsfb_abc123");
  CHECK(std::get<stores::Batch>(stores::typed_batch(decoded(openai, Kind::Batch, R"({"id":"vsfb_1","status":"cancelled"})"))).status == jobs::Status::Cancelled);
  // An answer that omits the store is in the store the call named; one that names another store or batch is refused.
  const auto omitted = std::get<stores::Batch>(stores::typed_batch(decoded(openai, Kind::Batch, R"({"id":"vsfb_1","status":"in_progress"})", "vs_abc123/vsfb_1")));
  CHECK(omitted.handle.store == "vs_abc123" && omitted.handle.id == "vsfb_1");
  CHECK(std::get<stores::Batch>(stores::typed_batch(decoded(openai, Kind::Batch, R"({"id":"vsfb_2"})", "vs_abc123"))).handle.store == "vs_abc123");
  for (const auto& [body, subject] : std::initializer_list<std::pair<std::string_view, std::string>>{
           {R"({"id":"vsfb_1","vector_store_id":"vs_other"})", "vs_abc123/vsfb_1"}, {R"({"id":"vsfb_1","vector_store_id":"vs_other"})", "vs_abc123"},
           {R"({"id":"vsfb_2","vector_store_id":"vs_abc123"})", "vs_abc123/vsfb_1"}})
    CHECK(std::get<Failure>(decoded(openai, Kind::Batch, body, subject)).error.reason == "stores.batch.id_required");
  // A page that says more follow and names no last id is refused, never read as the last page.
  for (const char* body : {R"({"data":[],"has_more":true})", R"({"data":[],"has_more":true,"last_id":null})", R"({"data":[],"has_more":true,"last_id":""})"})
    for (const auto kind : {Kind::Stores, Kind::Files})
      CHECK(std::get<Failure>(decoded(openai, kind, body)).error.reason == "stores.page.cursor_missing");
  // Each shape's own status words; another is Unknown and kept as it came.
  const auto foreign = std::get<stores::StoreFile>(stores::typed_file(decoded(openai, Kind::File, R"({"id":"file-a","status":"STATE_ACTIVE"})")));
  CHECK(foreign.status == stores::Status::Unknown && foreign.raw_status == "STATE_ACTIVE");

  const auto found = std::get<stores::SearchPage>(stores::typed_search(decoded(openai, Kind::Search,
      R"({"object":"vector_store.search_results.page","search_query":["What is the return policy?"],"data":[)"
      R"({"file_id":"file_123","filename":"document.pdf","score":0.95,"attributes":{"author":"John Doe","date":"2023-01-01"},"content":[{"type":"text","text":"Relevant chunk"}]},)"
      R"({"file_id":"file_456","filename":"notes.txt","score":0.89,"attributes":{"author":"Jane Smith"},"content":[{"type":"text","text":"Sample text content."}]}],)"
      R"("has_more":false,"next_page":null})")));
  CHECK(found.results.size() == 2 && found.results[0].file_id == "file_123" && found.results[0].filename == "document.pdf" &&
        found.results[0].score == 0.95 && found.results[0].content.size() == 1 && found.results[0].content[0].text == "Relevant chunk" &&
        found.results[1].attributes && found.search_query == std::vector<std::string>{"What is the return policy?"} && !found.has_more && found.raw);

  const auto content = std::get<stores::Content>(stores::typed_content(decoded(openai, Kind::Content,
      R"({"object":"vector_store.file_content.page","data":[{"type":"text","text":"file content"}],"has_more":true,"next_page":"p2"})")));
  CHECK(content.parts.size() == 1 && content.parts[0].type == "text" && content.parts[0].text == "file content" && content.has_more &&
        content.raw->root().get("next_page").as_string() == "p2");

  CHECK(std::get<stores::Deleted>(stores::typed_deleted(decoded(openai, Kind::Deleted, R"({"id":"vs_abc123","object":"vector_store.deleted","deleted":true})", "vs_abc123"))).id == "vs_abc123");
  CHECK(std::holds_alternative<Failure>(stores::typed_deleted(decoded(openai, Kind::Deleted, R"({"id":"vs_abc123","deleted":false})", "vs_abc123"))));
}
// Google: the resources of https://aistudio.google.com/api-reference/file-search/file-search-stores (FileSearchStore, the
// operation of .../import-file and .../get-file-search-store-operation) and .../file-search/documents (Document).
void decoded_google() {
  using Kind = stores::Codec::Kind;
  const auto s = std::get<stores::Store>(stores::typed_store(decoded(google, Kind::Store,
      R"({"name":"fileSearchStores/my-awesome-file-search-store-123a456b789c","displayName":"Docs on Semantic Retriever",)"
      R"("createTime":"2026-11-20T10:00:00Z","updateTime":"2026-11-20T10:05:00Z","activeDocumentsCount":"2","pendingDocumentsCount":"1",)"
      R"("failedDocumentsCount":"0","sizeBytes":"1024","embeddingModel":"models/fixture-embedding"})")));
  CHECK(s.origin == google && s.id == "fileSearchStores/my-awesome-file-search-store-123a456b789c" && s.name == "Docs on Semantic Retriever" &&
        s.files.completed == 2u && s.files.in_progress == 1u && s.files.failed == 0u && !s.files.total && s.bytes == 1024u &&
        s.created_at == 1795168800u && s.status == stores::Status::Unknown && s.raw->root().get("embeddingModel").is_string());
  const auto page = std::get<stores::Page>(stores::typed_page(decoded(google, Kind::Stores,
      R"({"fileSearchStores":[{"name":"fileSearchStores/a"},{"name":"fileSearchStores/b"}],"nextPageToken":"t2"})")));
  CHECK(page.stores.size() == 2 && page.stores[1].id == "fileSearchStores/b" && page.next == "t2");
  // Google leaves out an empty list.
  CHECK(std::get<stores::Page>(stores::typed_page(decoded(google, Kind::Stores, "{}"))).stores.empty());

  const auto d = std::get<stores::StoreFile>(stores::typed_file(decoded(google, Kind::File,
      R"({"name":"fileSearchStores/s/documents/my-awesome-doc-123a456b789c","displayName":"Semantic Retriever Documentation",)"
      R"("customMetadata":[{"key":"author","stringValue":"Robert Graves"}],"createTime":"2026-11-20T10:00:00Z","state":"STATE_ACTIVE",)"
      R"("sizeBytes":"30","mimeType":"text/plain"})")));
  CHECK(d.id == "fileSearchStores/s/documents/my-awesome-doc-123a456b789c" && d.store == "fileSearchStores/s" && d.status == stores::Status::Completed &&
        d.raw_status == "STATE_ACTIVE" && d.bytes == 30u && d.mime == "text/plain" && d.name == "Semantic Retriever Documentation" &&
        d.attributes && d.attributes->root().is_array());
  CHECK(std::get<stores::StoreFile>(stores::typed_file(decoded(google, Kind::File, R"({"name":"fileSearchStores/s/documents/d","state":"STATE_PENDING"})"))).status ==
        stores::Status::InProgress);
  const auto docs = std::get<stores::Page>(stores::typed_page(decoded(google, Kind::Files, R"({"documents":[{"name":"fileSearchStores/s/documents/d"}]})")));
  CHECK(docs.files.size() == 1 && docs.next.empty());

  // An import's operation: running, then done with the document it made, or failed.
  const auto running = std::get<stores::Batch>(stores::typed_batch(decoded(google, Kind::Batch, R"({"name":"fileSearchStores/s/operations/op-1","metadata":{}})")));
  CHECK(running.status == jobs::Status::Running && running.handle.store == "fileSearchStores/s" && running.handle.id == "fileSearchStores/s/operations/op-1" &&
        running.handle.shape == "google");
  CHECK(encoded(google, stores::Poll{running.handle}).path == "/v1beta/fileSearchStores/s/operations/op-1");
  const auto done = std::get<stores::Batch>(stores::typed_batch(decoded(google, Kind::Batch,
      R"({"name":"fileSearchStores/s/operations/op-1","done":true,"response":{"parent":"fileSearchStores/s","documentName":"fileSearchStores/s/documents/doc-1"}})")));
  CHECK(done.status == jobs::Status::Completed && done.document == "fileSearchStores/s/documents/doc-1" && !done.error);
  // As the live import answered on 2026-11-21: the document by its id alone, which is named under the operation's store.
  const auto live = std::get<stores::Batch>(stores::typed_batch(decoded(google, Kind::Batch,
      R"({"name":"fileSearchStores/s/operations/op-1","done":true,"response":{"parent":"fileSearchStores/s","documentName":"57gzoxi2tpag-mh7gsfipmor0"}})")));
  CHECK(live.document == "fileSearchStores/s/documents/57gzoxi2tpag-mh7gsfipmor0");
  CHECK(encoded(google, stores::GetFile{live.handle.store, live.document}).path == "/v1beta/fileSearchStores/s/documents/57gzoxi2tpag-mh7gsfipmor0");
  CHECK(encoded(google, stores::DeleteFile{live.handle.store, live.document, true}).method == "DELETE");
  for (const char* document : {"fileSearchStores/other/documents/d", "a/b", "..", "", "fileSearchStores/s/documents/d/x"})
    CHECK(std::get<Failure>(decoded(google, Kind::Batch, std::string(R"({"name":"fileSearchStores/s/operations/op-1","done":true,"response":{"documentName":)") +
                                                             json::quote(document) + "}}")).error.reason == "stores.batch.id_required");
  const auto openai_word = std::get<stores::StoreFile>(stores::typed_file(decoded(google, Kind::File, R"({"name":"fileSearchStores/s/documents/d","state":"completed"})")));
  CHECK(openai_word.status == stores::Status::Unknown && openai_word.raw_status == "completed");
  const auto failed = std::get<stores::Batch>(stores::typed_batch(decoded(google, Kind::Batch,
      R"({"name":"fileSearchStores/s/operations/op-1","done":true,"error":{"code":3,"message":"unsupported file"}})")));
  CHECK(failed.status == jobs::Status::Failed && failed.error && failed.error->code == "3" && failed.error->message == "unsupported file");

  CHECK(std::get<stores::Deleted>(stores::typed_deleted(decoded(google, Kind::Deleted, "{}", "fileSearchStores/s"))).id == "fileSearchStores/s");
  CHECK(std::get<stores::Deleted>(stores::typed_deleted(decoded(google, Kind::Deleted, "", "fileSearchStores/s"))).id == "fileSearchStores/s");
}
void malformed() {
  using Kind = stores::Codec::Kind;
  for (const auto& [origin, kind, body] : std::initializer_list<std::tuple<std::string_view, Kind, std::string_view>>{
           {openai, Kind::Store, R"({"name":"x"})"}, {openai, Kind::Store, R"({"id":"vs_a","usage_bytes":"1"})"},
           {openai, Kind::Store, R"({"id":"vs_a","file_counts":{"total":"3"}})"}, {google, Kind::Store, R"({"name":"corpora/x"})"},
           {google, Kind::Store, R"({"name":"fileSearchStores/x","sizeBytes":30})"}, {google, Kind::Store, R"({"name":"fileSearchStores/a/b"})"},
           {google, Kind::Store, R"({"name":"fileSearchStores/.."})"}, {openai, Kind::File, R"({"id":"file-a","last_error":"bad"})"},
           {google, Kind::File, R"({"name":"fileSearchStores/s"})"}, {openai, Kind::Batch, R"({"status":"completed"})"},
           {google, Kind::Batch, R"({"name":"fileSearchStores/s/operations/o","done":"yes"})"}, {openai, Kind::Stores, R"({"data":{}})"},
           {openai, Kind::Search, R"({"data":[{"filename":"x"}]})"}, {openai, Kind::Search, R"({"data":[{"file_id":"f","score":"high"}]})"},
           {openai, Kind::Content, R"({"data":[{"text":7}]})"}, {openai, Kind::Store, "[]"}, {google, Kind::Stores, R"({"fileSearchStores":[],"nextPageToken":7})"}}) {
    const auto result = decoded(origin, kind, body);
    CHECK(std::holds_alternative<Failure>(result) && std::get<Failure>(result).error.kind == ErrorKind::ProtocolCorrupt &&
          std::get<Failure>(result).error.reason.starts_with("stores."));
  }
}

// The policy's facts: the billing of each operation, the search bound, and the loader's checks of both.
void policy_facts() {
  const auto policy = descriptor::builtin_policy();
  using Billing = descriptor::EndpointBilling;
  for (const char* operation : {"create", "search", "file_add", "batch_create"})
    CHECK(policy->endpoint_billing("stores", openai, operation) == Billing::Unpriced);
  for (const char* operation : {"get", "list", "update", "delete", "file_get", "file_list", "file_update", "file_delete", "file_content", "batch_get",
                                "batch_cancel", "batch_files"})
    CHECK(policy->endpoint_billing("stores", openai, operation) == Billing::None);
  CHECK(policy->endpoint_billing("stores", google, "import") == Billing::Unpriced);
  for (const char* operation : {"create", "get", "list", "delete", "document_get", "document_list", "document_delete", "operation_get"})
    CHECK(policy->endpoint_billing("stores", google, operation) == Billing::None);
  CHECK(policy->endpoint_limit("stores", openai, "max_results") == 50u && !policy->endpoint_limit("stores", google, "max_results"));
  CHECK(!policy->endpoint_limit("files", openai, "max_results"));
  CHECK(to_string(Billing::Unpriced) == "Unpriced" && from_string<Billing>("Unpriced") == Billing::Unpriced);

  // The embedded text has its keys sorted and no spaces, as tools/embed_configuration.py writes it.
  const std::string text(config_defaults::descriptor_policy_json);
  const auto edited = [&](std::string_view from, std::string_view to) {
    auto out = text;
    const auto at = out.find(from);
    if (at == std::string::npos) throw std::runtime_error("no " + std::string(from) + " in the embedded policy");
    return out.replace(at, from.size(), to);
  };
  const auto pointer = [](const std::string& family_json) {
    const auto loaded = descriptor::load_policy(family_json, config_defaults::codec_defaults_json);
    const auto* error = std::get_if<descriptor::ConfigError>(&loaded);
    return error ? error->pointer : std::string();
  };
  constexpr std::string_view bound = R"("max_results":50,)";
  CHECK(pointer(text).empty());
  CHECK(pointer(edited(bound, R"("max_results":0,)")) == "/endpoints/max_results");
  CHECK(pointer(edited(bound, R"("max_results":"50",)")) == "/endpoints/max_results");
  CHECK(pointer(edited(R"("max_part_bytes":67108864,)", R"("max_part_bytes":67108864,"max_results":5,)")) == "/endpoints/max_results");
  // A stores fact without the bound has the embedded policy's for its shape, which its content digest covers, so a
  // search past it is still refused before it is sent.
  const auto borrowed = descriptor::load_policy(edited(bound, ""), config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(borrowed));
  const auto& custom = std::get<descriptor::PolicySnapshot>(borrowed);
  CHECK(custom->endpoint_limit("stores", openai, "max_results") == 50u && !custom->endpoint_limit("stores", google, "max_results"));
  CHECK(custom->content_digest_hex() != policy->content_digest_hex());
  const auto bounded = stores::encode(at(openai, custom), stores::Search{"vs_a", {"q"}, 500});
  CHECK(std::holds_alternative<Error>(bounded) && std::get<Error>(bounded).reason == "request.stores.results_invalid");
  CHECK(std::holds_alternative<endpoints::Encoded>(stores::encode(at(openai, custom), stores::Search{"vs_a", {"q"}, 50})));
  // `unpriced` is admitted for any operation, and a fact that charges once it accepts a call may bill by it alone.
  CHECK(pointer(edited(R"("file_update":"none","get":"none")", R"("file_update":"none","get":"unpriced")")).empty());
  // ... and only on a stores fact.
  CHECK(pointer(edited(R"("billing":"none","endpoint":"files")", R"("billing":"unpriced","endpoint":"files")")) == "/endpoints/billing");
  CHECK(pointer(edited(R"("billing":{"connect":"model"})", R"("billing":{"connect":"unpriced"})")) == "/endpoints/billing");
  CHECK(pointer(edited(R"("batch_get":"none","create":"unpriced")", R"("batch_get":"none","create":"model")")) == "/endpoints/billing");
  CHECK(pointer(edited(R"("batch_get":"none","create":"unpriced")", R"("batch_get":"none","create":"Unpriced")")) == "/endpoints/billing");
  // An operation the shape does not have.
  CHECK(pointer(edited(R"("create":"/v1beta/fileSearchStores",)", R"("create":"/v1beta/fileSearchStores","search":"/v1beta/x",)")) == "/endpoints/paths");
}

// What each call bills by: a create, a search or an index has no bound and no estimate, never zero; a read is free.
void money() {
  runtime::Client client(at(openai), std::string("synthetic-key"));
  runtime::Client gemini(at(google), std::string("synthetic-key"));
  runtime::RunOptions run;
  run.deadline = std::chrono::steady_clock::now() + 30s;
  const auto unbounded = [](const auto& prepared, std::string_view subject) {
    const auto& p = prepared.prepared();
    const auto worst = runtime::worst_case_cost(p);
    const auto endpoint = runtime::endpoint_worst_case_cost(p);
    const auto estimate = pricing::endpoint_estimate(p.endpoint_facts(), {}, *descriptor::builtin_policy()->catalogue());
    return p.endpoint_facts().billing == descriptor::EndpointBilling::Unpriced && !worst.total && worst.unbounded.size() == 1 &&
           worst.unbounded[0].by == pricing::UnboundedBy::Unmodeled && worst.unbounded[0].subject == subject &&
           worst.unbounded[0].detail == pricing::unpriced_detail && !endpoint.total && endpoint.unbounded.size() == 1 &&
           endpoint.unbounded[0].by == pricing::UnboundedBy::Unmodeled && endpoint.unbounded[0].subject == subject && !estimate.total &&
           estimate.unpriced == std::vector<std::string>{"unpriced"};
  };
  const auto zero = [](const auto& prepared) {
    const auto& p = prepared.prepared();
    const auto worst = runtime::worst_case_cost(p);
    return p.endpoint_facts().billing == descriptor::EndpointBilling::None && worst.total && worst.total->nano_usd == 0;
  };
  CHECK(unbounded(client.prepare(stores::Create{"Support FAQ"}, run), "stores/create"));
  CHECK(unbounded(client.prepare(stores::AddFile{"vs_a", "file-a"}, run), "stores/file_add"));
  CHECK(unbounded(client.prepare(stores::CreateBatch{"vs_a", {"file-a"}}, run), "stores/batch_create"));
  CHECK(unbounded(client.prepare(stores::Search{"vs_a", {"q"}}, run), "stores/search"));
  CHECK(unbounded(gemini.prepare(stores::Import{"fileSearchStores/s", "files/a"}, run), "stores/import"));
  CHECK(zero(client.prepare(stores::Get{"vs_a"}, run)));
  CHECK(zero(client.prepare(stores::Update{"vs_a", "renamed"}, run)));
  CHECK(zero(client.prepare(stores::UpdateFile{"vs_a", "file-a", nullptr}, run)));
  CHECK(zero(gemini.prepare(stores::Create{"My Store"}, run)));
  CHECK(zero(client.prepare(stores::ListFiles{"vs_a", "vsfb_1"}, run)));
  CHECK(zero(client.prepare(stores::Delete{"vs_a"}, run)));
  CHECK(zero(gemini.prepare(stores::Poll{{std::string(google), "google", "fileSearchStores/s", "fileSearchStores/s/operations/o"}}, run)));
  CHECK(zero(gemini.prepare(stores::DeleteFile{"fileSearchStores/s", "fileSearchStores/s/documents/d", true}, run)));
  const auto listing = client.prepare(stores::ListFiles{"vs_a", "vsfb_1"}, run);
  const auto& facts = listing.prepared().endpoint_facts();
  CHECK(facts.endpoint == "stores" && facts.operation == "batch_files" && facts.model.empty());
}

// Through the runtime: the call's URL, method and credential; a create that may have reached the provider is not sent
// again, a read is.
void runtime_calls() {
  auto executor = std::make_shared<QueueExecutor>();
  auto wire = std::make_shared<ScriptTransport>();
  runtime::Options options;
  options.api_key = "synthetic-key";
  auto client = runtime::detail::ClientAccess::make(at(openai), options, executor, wire);
  runtime::RunOptions run;
  run.retry = runtime::RetryPolicy{true, false, 3, 0ms, 0ms};
  const auto run_for = [&] { auto value = run; value.deadline = executor->now() + 10s; return value; };
  const auto complete = [&](auto prepared) { auto call = client.start(std::move(prepared)); executor->drain(); return call.join(); };
  wire->script = [](Exchange& e) {
    e.reply(200, "application/json", R"({"id":"vs_abc123","object":"vector_store","name":"Support FAQ","status":"completed","file_counts":{"total":0},"usage_bytes":0})");
  };
  const auto made = complete(client.prepare(stores::Create{"Support FAQ"}, run_for()));
  CHECK(wire->exchanges.size() == 1);
  const auto& request = wire->exchanges.back()->request;
  CHECK(request.method == "POST" && request.url == "https://api.openai.com/v1/vector_stores" && request.body == R"({"name":"Support FAQ"})" &&
        header(request, "Authorization") == "Bearer synthetic-key" && header(request, "Content-Type") == "application/json");
  CHECK(std::get<stores::Store>(made).id == "vs_abc123" && std::get<stores::Store>(made).attempt.attempts == 1);

  std::size_t calls = 0;
  wire->script = [&](Exchange& e) {
    if (++calls == 1) { e.head(200, {{"content-type", "application/json"}}); e.finish(transport::Status::Failed, transport::FailureKind::Receive); }
    else e.reply(200, "application/json", R"({"id":"vs_abc123","object":"vector_store"})");
  };
  CHECK(std::holds_alternative<Failure>(complete(client.prepare(stores::Create{"Support FAQ"}, run_for()))) && calls == 1);
  calls = 0;
  CHECK(std::holds_alternative<stores::Store>(complete(client.prepare(stores::Get{"vs_abc123"}, run_for()))) && calls == 2);

  // A search's results, typed.
  wire->script = [](Exchange& e) {
    e.reply(200, "application/json", R"({"object":"vector_store.search_results.page","search_query":["q"],"data":[{"file_id":"file-a","filename":"a.txt","score":0.5,"content":[{"type":"text","text":"x"}]}],"has_more":false})");
  };
  const auto found = complete(client.prepare(stores::Search{"vs_abc123", {"q"}, 3}, run_for()));
  CHECK(std::get<stores::SearchPage>(found).results.size() == 1 && wire->exchanges.back()->request.url == "https://api.openai.com/v1/vector_stores/vs_abc123/search");

  // Google: the key header of its fact, and the import's operation.
  auto gemini = runtime::detail::ClientAccess::make(at(google), options, executor, wire);
  wire->script = [](Exchange& e) { e.reply(200, "application/json", R"({"name":"fileSearchStores/s/operations/op-1"})"); };
  auto call = gemini.start(gemini.prepare(stores::Import{"fileSearchStores/s", "files/abc-123"}, run_for()));
  executor->drain();
  const auto started = call.join();
  CHECK(!wire->exchanges.empty());
  const auto& sent = wire->exchanges.back()->request;
  CHECK(sent.url == "https://generativelanguage.googleapis.com/v1beta/fileSearchStores/s:importFile" && header(sent, "x-goog-api-key") == "synthetic-key" &&
        header(sent, "Authorization").empty());
  CHECK(std::get<stores::Batch>(started).status == jobs::Status::Running && std::get<stores::Batch>(started).handle.store == "fileSearchStores/s");
}
}  // namespace

int main() {
  // Each case runs on its own, so one failure does not hide another's.
  int failed = 0;
  for (const auto& [name, run] : std::initializer_list<std::pair<const char*, std::function<void()>>>{
           {"openai_requests", openai_requests}, {"google_requests", google_requests}, {"refusals", refusals}, {"decoded_openai", decoded_openai},
           {"decoded_google", decoded_google}, {"malformed", malformed}, {"policy_facts", policy_facts}, {"money", money}, {"runtime_calls", runtime_calls}}) {
    try { run(); std::cout << "case passed: " << name << '\n'; }
    catch (const std::exception& error) { ++failed; std::cerr << "case failed: " << name << ": " << error.what() << '\n'; }
  }
  if (failed) { std::cerr << "store test failed: " << failed << " case(s)\n"; return 1; }
  std::cout << "store properties passed\n";
  return 0;
}
