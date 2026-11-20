#include "codecs/stores.h"
#include "codecs/reason.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>

namespace sp::stores {
namespace {
namespace why = reason::stores_request;
Error bad(const reason::Reason& message) { return reason::error(ErrorKind::InvalidRequest, message); }
Error unsupported(const reason::Reason& message) { return reason::error(ErrorKind::Unsupported, message); }
// A path segment the caller names. `.` and `..` are tokens by their characters but move the request elsewhere.
bool token(std::string_view value, std::size_t limit = 128) {
  return !value.empty() && value.size() <= limit && value != "." && value != ".." && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}
// `<prefix><token>`: a Google resource name of one more segment.
bool named(std::string_view value, std::string_view prefix) { return value.starts_with(prefix) && token(value.substr(prefix.size())); }
constexpr std::string_view google_stores = "fileSearchStores/";
bool google_shape(const descriptor::EndpointFact& fact) { return fact.shape == "google"; }
// A store as its shape names it: a token at OpenAI, `fileSearchStores/<token>` at Google.
bool store_id(const descriptor::EndpointFact& fact, std::string_view store) { return google_shape(fact) ? named(store, google_stores) : token(store); }
// A query value, percent-encoded but for the unreserved characters.
std::string query_value(std::string_view value) {
  std::string out;
  for (const unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') out += static_cast<char>(c);
    else { out += '%'; out += "0123456789ABCDEF"[c >> 4]; out += "0123456789ABCDEF"[c & 15]; }
  }
  return out;
}
const descriptor::EndpointFact* fact_of(const descriptor::ValidatedDescriptor& descriptor) {
  return descriptor.policy()->endpoint("stores", descriptor.base_url());
}
// A JSON body built member by member.
struct Body {
  std::string text = "{";
  void raw(std::string_view name, std::string_view value) { text.append(text.size() > 1 ? "," : "").append(json::quote(name)).append(":").append(value); }
  void string(std::string_view name, std::string_view value) { raw(name, json::quote(value)); }
  std::string done() { return text + "}"; }
};
std::string strings(const std::vector<std::string>& values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) out.append(i ? "," : "").append(json::quote(values[i]));
  return out + "]";
}
enum class Kind { Object, Array, ObjectOrNull };
// A provider object the caller passes: the kind the provider documents.
bool of_kind(const std::shared_ptr<const json::Document>& value, Kind kind) {
  if (!value) return true;
  const auto root = value->root();
  return kind == Kind::Array ? root.is_array() : root.is_object() || (kind == Kind::ObjectOrNull && root.is_null());
}
// The call to `operation` of the stores fact, with `{store}` and `{id}` of its path replaced by the checked names.
struct Call {
  const descriptor::EndpointFact* fact;
  Encoded encoded;
};
std::variant<Call, Error> call(const descriptor::ValidatedDescriptor& descriptor, std::string_view operation, std::string_view method,
                               std::string_view store, std::string_view id, std::string subject) {
  const auto* fact = fact_of(descriptor);
  if (!fact) return unsupported(why::endpoint_not_admitted);
  const auto path = descriptor::endpoint_path(*fact, operation);
  if (!path) return unsupported(why::path_missing);
  std::string resolved(*path);
  for (const auto& [placeholder, value] : {std::pair{std::string_view("{store}"), store}, std::pair{std::string_view("{id}"), id}})
    for (auto at = resolved.find(placeholder); at != std::string::npos; at = resolved.find(placeholder, at + value.size()))
      resolved.replace(at, placeholder.size(), value);
  // The provider bills what a create or an index starts once it accepts the call: such a call is not sent again once it
  // may have reached the provider.
  const auto billing = descriptor.policy()->endpoint_billing("stores", descriptor.base_url(), operation);
  const bool billed = billing == descriptor::EndpointBilling::Model || billing == descriptor::EndpointBilling::Carried ||
                      billing == descriptor::EndpointBilling::Unpriced;
  Encoded encoded{std::move(resolved), {}, std::move(subject), fact->charge_on_accept && billed, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.method = std::string(method);
  if (method != "POST") encoded.content_type.clear();
  return Call{fact, std::move(encoded)};
}
// The page query of a listing: OpenAI `limit` and `after`, Google `pageSize` and `pageToken`.
std::optional<Error> page_query(const descriptor::EndpointFact& fact, std::string& path, std::optional<std::uint32_t> limit, std::string_view after,
                                std::string_view filter = {}) {
  if (limit && !*limit) return bad(why::limit_invalid);
  if (after.size() > 4096) return bad(why::cursor_invalid);
  const bool google = google_shape(fact);
  std::string query;
  const auto add = [&](std::string_view name, std::string_view value) { query.append(query.empty() ? "?" : "&").append(name).append("=").append(query_value(value)); };
  if (limit) add(google ? "pageSize" : "limit", std::to_string(*limit));
  if (!after.empty()) add(google ? "pageToken" : "after", after);
  if (!filter.empty()) add("filter", filter);
  path += query;
  return std::nullopt;
}
// A document of the store `store` at Google: `<store>/documents/<token>`.
bool document_of(std::string_view store, std::string_view id) {
  return id.size() > store.size() && id.starts_with(store) && named(id.substr(store.size()), "/documents/");
}
// A batch or operation polled at the origin and shape that started it, in a store the shape names.
std::optional<Error> handle_checks(const descriptor::ValidatedDescriptor& descriptor, const Handle& handle) {
  const auto* fact = fact_of(descriptor);
  if (!fact) return unsupported(why::endpoint_not_admitted);
  if (handle.origin != descriptor.base_url() || handle.shape != fact->shape) return bad(why::handle_origin_mismatch);
  if (!store_id(*fact, handle.store)) return bad(why::store_invalid);
  if (!google_shape(*fact)) return token(handle.id) ? std::nullopt : std::optional<Error>(bad(why::handle_invalid));
  // Google names an import's operation `<store>/operations/<id>`, and an upload's `<store>/upload/operations/<id>`.
  const std::string_view id = handle.id;
  if (id.size() <= handle.store.size() || !id.starts_with(handle.store)) return bad(why::handle_invalid);
  const auto rest = id.substr(handle.store.size());
  if (!named(rest, "/operations/") && !named(rest, "/upload/operations/")) return bad(why::handle_invalid);
  return std::nullopt;
}
}  // namespace

std::string_view shape(const descriptor::ValidatedDescriptor& descriptor) noexcept {
  const auto* fact = fact_of(descriptor);
  return fact ? std::string_view(fact->shape) : std::string_view{};
}
std::string_view operation(std::string_view, const Create&) noexcept { return "create"; }
std::string_view operation(std::string_view, const Get&) noexcept { return "get"; }
std::string_view operation(std::string_view, const List&) noexcept { return "list"; }
std::string_view operation(std::string_view, const Update&) noexcept { return "update"; }
std::string_view operation(std::string_view, const Delete&) noexcept { return "delete"; }
std::string_view operation(std::string_view, const Search&) noexcept { return "search"; }
std::string_view operation(std::string_view, const AddFile&) noexcept { return "file_add"; }
std::string_view operation(std::string_view shape, const GetFile&) noexcept { return shape == "google" ? "document_get" : "file_get"; }
std::string_view operation(std::string_view shape, const ListFiles& list) noexcept {
  return shape == "google" ? "document_list" : list.batch.empty() ? "file_list" : "batch_files";
}
std::string_view operation(std::string_view, const UpdateFile&) noexcept { return "file_update"; }
std::string_view operation(std::string_view shape, const DeleteFile&) noexcept { return shape == "google" ? "document_delete" : "file_delete"; }
std::string_view operation(std::string_view, const FileContent&) noexcept { return "file_content"; }
std::string_view operation(std::string_view, const CreateBatch&) noexcept { return "batch_create"; }
std::string_view operation(std::string_view, const Import&) noexcept { return "import"; }
std::string_view operation(std::string_view shape, const Poll&) noexcept { return shape == "google" ? "operation_get" : "batch_get"; }
std::string_view operation(std::string_view, const Cancel&) noexcept { return "batch_cancel"; }

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Create& create) {
  auto found = call(descriptor, operation(shape(descriptor), create), "POST", {}, {}, {});
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, encoded] = std::get<Call>(found);
  Body body;
  if (google_shape(*fact)) {
    if (!create.file_ids.empty() || !create.description.empty() || create.expires_after_days || create.metadata || create.chunking_strategy)
      return unsupported(why::member_unsupported);
    if (!create.name.empty()) body.string("displayName", create.name);
    if (!create.embedding_model.empty()) body.string("embeddingModel", create.embedding_model);
  } else {
    if (!create.embedding_model.empty()) return unsupported(why::member_unsupported);
    if (std::any_of(create.file_ids.begin(), create.file_ids.end(), [](const std::string& id) { return !token(id); })) return bad(why::file_invalid);
    if (create.expires_after_days && !*create.expires_after_days) return bad(why::expiry_invalid);
    if (!of_kind(create.metadata, Kind::ObjectOrNull) || !of_kind(create.chunking_strategy, Kind::Object)) return bad(why::object_invalid);
    if (!create.name.empty()) body.string("name", create.name);
    if (!create.description.empty()) body.string("description", create.description);
    if (!create.file_ids.empty()) body.raw("file_ids", strings(create.file_ids));
    if (create.expires_after_days) body.raw("expires_after", "{\"anchor\":\"last_active_at\",\"days\":" + std::to_string(*create.expires_after_days) + "}");
    if (create.metadata) body.raw("metadata", create.metadata->root().dump());
    if (create.chunking_strategy) body.raw("chunking_strategy", create.chunking_strategy->root().dump());
  }
  encoded.body = body.done();
  return std::move(encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Get& get) {
  const auto* fact = fact_of(descriptor);
  if (fact && !store_id(*fact, get.store)) return bad(why::store_invalid);
  auto found = call(descriptor, operation(shape(descriptor), get), "GET", get.store, {}, get.store);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  return std::move(std::get<Call>(found).encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const List& list) {
  auto found = call(descriptor, operation(shape(descriptor), list), "GET", {}, {}, {});
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, encoded] = std::get<Call>(found);
  if (auto error = page_query(*fact, encoded.path, list.limit, list.after)) return *error;
  return std::move(encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Update& update) {
  const auto* fact = fact_of(descriptor);
  if (fact && !store_id(*fact, update.store)) return bad(why::store_invalid);
  auto found = call(descriptor, operation(shape(descriptor), update), "POST", update.store, {}, update.store);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& encoded = std::get<Call>(found).encoded;
  if (!update.name && !update.expires_after_days && !update.metadata) return bad(why::update_empty);
  if (update.expires_after_days && !*update.expires_after_days) return bad(why::expiry_invalid);
  if (!of_kind(update.metadata, Kind::ObjectOrNull)) return bad(why::object_invalid);
  Body body;
  if (update.name) body.string("name", *update.name);
  if (update.expires_after_days) body.raw("expires_after", "{\"anchor\":\"last_active_at\",\"days\":" + std::to_string(*update.expires_after_days) + "}");
  if (update.metadata) body.raw("metadata", update.metadata->root().dump());
  encoded.body = body.done();
  return std::move(encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Delete& del) {
  const auto* fact = fact_of(descriptor);
  if (fact && !store_id(*fact, del.store)) return bad(why::store_invalid);
  auto found = call(descriptor, operation(shape(descriptor), del), "DELETE", del.store, {}, del.store);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [checked, encoded] = std::get<Call>(found);
  if (del.force && !google_shape(*checked)) return unsupported(why::member_unsupported);
  if (del.force) encoded.path += "?force=true";
  return std::move(encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Search& search) {
  const auto* fact = fact_of(descriptor);
  if (fact && !store_id(*fact, search.store)) return bad(why::store_invalid);
  auto found = call(descriptor, operation(shape(descriptor), search), "POST", search.store, {}, search.store);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& encoded = std::get<Call>(found).encoded;
  if (search.query.empty() || std::any_of(search.query.begin(), search.query.end(), [](const std::string& q) { return q.empty(); }))
    return bad(why::query_empty);
  // The most results a search may ask for is the policy's (OpenAI documents 1 to 50).
  const auto most = descriptor.policy()->endpoint_limit("stores", descriptor.base_url(), "max_results");
  if (search.max_num_results && (!*search.max_num_results || (most && *search.max_num_results > *most))) return bad(why::results_invalid);
  if (!of_kind(search.filters, Kind::Object) || !of_kind(search.ranking_options, Kind::Object)) return bad(why::object_invalid);
  Body body;
  body.raw("query", search.query.size() == 1 ? json::quote(search.query.front()) : strings(search.query));
  if (search.max_num_results) body.raw("max_num_results", std::to_string(*search.max_num_results));
  if (search.filters) body.raw("filters", search.filters->root().dump());
  if (search.ranking_options) body.raw("ranking_options", search.ranking_options->root().dump());
  if (search.rewrite_query) body.raw("rewrite_query", *search.rewrite_query ? "true" : "false");
  encoded.body = body.done();
  return std::move(encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const AddFile& add) {
  const auto* fact = fact_of(descriptor);
  if (fact && !store_id(*fact, add.store)) return bad(why::store_invalid);
  auto found = call(descriptor, operation(shape(descriptor), add), "POST", add.store, {}, add.file);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& encoded = std::get<Call>(found).encoded;
  if (!token(add.file)) return bad(why::file_invalid);
  if (!of_kind(add.attributes, Kind::ObjectOrNull) || !of_kind(add.chunking_strategy, Kind::Object)) return bad(why::object_invalid);
  Body body;
  body.string("file_id", add.file);
  if (add.attributes) body.raw("attributes", add.attributes->root().dump());
  if (add.chunking_strategy) body.raw("chunking_strategy", add.chunking_strategy->root().dump());
  encoded.body = body.done();
  return std::move(encoded);
}
namespace {
// A call about one file of a store: OpenAI's `{store}` and `{id}`, Google's document `<store>/documents/<id>`.
std::variant<Call, Error> file_call(const descriptor::ValidatedDescriptor& descriptor, std::string_view operation, std::string_view method,
                                    std::string_view store, std::string_view id) {
  const auto* fact = fact_of(descriptor);
  if (fact && !store_id(*fact, store)) return bad(why::store_invalid);
  if (fact && !(google_shape(*fact) ? document_of(store, id) : token(id))) return bad(why::id_invalid);
  return call(descriptor, operation, method, store, id, std::string(id));
}
}  // namespace
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const GetFile& get) {
  auto found = file_call(descriptor, operation(shape(descriptor), get), "GET", get.store, get.id);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  return std::move(std::get<Call>(found).encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const ListFiles& list) {
  const auto* fact = fact_of(descriptor);
  if (fact && !store_id(*fact, list.store)) return bad(why::store_invalid);
  if (fact && google_shape(*fact) && (!list.batch.empty() || !list.filter.empty())) return unsupported(why::member_unsupported);
  if (!list.batch.empty() && !token(list.batch)) return bad(why::handle_invalid);
  if (!list.filter.empty() && !token(list.filter, 64)) return bad(why::filter_invalid);
  auto found = call(descriptor, operation(shape(descriptor), list), "GET", list.store, list.batch, list.store);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [checked, encoded] = std::get<Call>(found);
  if (auto error = page_query(*checked, encoded.path, list.limit, list.after, list.filter)) return *error;
  return std::move(encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const UpdateFile& update) {
  auto found = file_call(descriptor, operation(shape(descriptor), update), "POST", update.store, update.id);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& encoded = std::get<Call>(found).encoded;
  if (!of_kind(update.attributes, Kind::ObjectOrNull)) return bad(why::object_invalid);
  Body body;
  body.raw("attributes", update.attributes ? update.attributes->root().dump() : std::string("null"));
  encoded.body = body.done();
  return std::move(encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const DeleteFile& del) {
  auto found = file_call(descriptor, operation(shape(descriptor), del), "DELETE", del.store, del.id);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, encoded] = std::get<Call>(found);
  if (del.force && !google_shape(*fact)) return unsupported(why::member_unsupported);
  if (del.force) encoded.path += "?force=true";
  return std::move(encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const FileContent& content) {
  auto found = file_call(descriptor, operation(shape(descriptor), content), "GET", content.store, content.id);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  return std::move(std::get<Call>(found).encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const CreateBatch& batch) {
  const auto* fact = fact_of(descriptor);
  if (fact && !store_id(*fact, batch.store)) return bad(why::store_invalid);
  auto found = call(descriptor, operation(shape(descriptor), batch), "POST", batch.store, {}, batch.store);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& encoded = std::get<Call>(found).encoded;
  if (batch.file_ids.empty() == !batch.files) return bad(why::batch_files_invalid);
  if (std::any_of(batch.file_ids.begin(), batch.file_ids.end(), [](const std::string& id) { return !token(id); })) return bad(why::file_invalid);
  if (!of_kind(batch.files, Kind::Array) || !of_kind(batch.attributes, Kind::ObjectOrNull) || !of_kind(batch.chunking_strategy, Kind::Object))
    return bad(why::object_invalid);
  Body body;
  if (batch.files) body.raw("files", batch.files->root().dump());
  else body.raw("file_ids", strings(batch.file_ids));
  if (batch.attributes) body.raw("attributes", batch.attributes->root().dump());
  if (batch.chunking_strategy) body.raw("chunking_strategy", batch.chunking_strategy->root().dump());
  encoded.body = body.done();
  return std::move(encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Import& import) {
  const auto* fact = fact_of(descriptor);
  if (fact && !store_id(*fact, import.store)) return bad(why::store_invalid);
  auto found = call(descriptor, operation(shape(descriptor), import), "POST", import.store, {}, import.store);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& encoded = std::get<Call>(found).encoded;
  if (!named(import.file, "files/")) return bad(why::file_invalid);
  if (!of_kind(import.custom_metadata, Kind::Array) || !of_kind(import.chunking_config, Kind::Object)) return bad(why::object_invalid);
  Body body;
  body.string("fileName", import.file);
  if (import.custom_metadata) body.raw("customMetadata", import.custom_metadata->root().dump());
  if (import.chunking_config) body.raw("chunkingConfig", import.chunking_config->root().dump());
  encoded.body = body.done();
  return std::move(encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Poll& poll) {
  if (auto error = handle_checks(descriptor, poll.handle)) return *error;
  auto found = call(descriptor, operation(shape(descriptor), poll), "GET", poll.handle.store, poll.handle.id, poll.handle.id);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  return std::move(std::get<Call>(found).encoded);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Cancel& cancel) {
  if (auto error = handle_checks(descriptor, cancel.handle)) return *error;
  auto found = call(descriptor, operation(shape(descriptor), cancel), "POST", cancel.handle.store, cancel.handle.id, cancel.handle.id);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& encoded = std::get<Call>(found).encoded;
  encoded.content_type.clear();
  return std::move(encoded);
}
}  // namespace sp::stores
