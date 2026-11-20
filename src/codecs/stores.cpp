#include "codecs/stores.h"
#include "codecs/reason.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>
#include <charconv>

namespace sp::stores {
namespace {
namespace why = reason::stores;
using endpoints::unix_seconds;
std::string text_of(json::Value value) { return value.is_string() ? std::string(value.as_string()) : std::string(); }
// A member that is absent or null, or of the kind `check` accepts.
template <class Check> bool optional_member(json::Value value, Check check) { return !value.valid() || value.is_null() || check(value); }
bool is_string(json::Value value) { return value.is_string(); }
bool is_uint(json::Value value) { return value.is_uint(); }
bool is_object(json::Value value) { return value.is_object(); }
bool is_array(json::Value value) { return value.is_array(); }
bool is_bool(json::Value value) { return value.is_bool(); }
bool is_time(json::Value value) { return value.is_string() && unix_seconds(value.as_string()).has_value(); }
// A decimal count a server wrote as a string (Google's int64 members), when it is digits only and fits 64 bits.
std::optional<std::uint64_t> to_uint(std::string_view text) {
  if (text.empty() || text.front() < '0' || text.front() > '9') return std::nullopt;
  std::uint64_t value = 0;
  const auto end = text.data() + text.size();
  const auto [stop, error] = std::from_chars(text.data(), end, value);
  if (stop != end || error != std::errc{}) return std::nullopt;
  return value;
}
bool is_digits(json::Value value) { return value.is_string() && to_uint(value.as_string()); }
std::optional<std::uint64_t> uint_of(json::Value value) { return value.is_uint() ? std::optional<std::uint64_t>(value.as_uint()) : std::nullopt; }
std::optional<std::uint64_t> digits_of(json::Value value) { return value.is_string() ? to_uint(value.as_string()) : std::nullopt; }
std::optional<std::uint64_t> time_of(json::Value value) { return value.is_string() ? unix_seconds(value.as_string()) : std::nullopt; }
std::shared_ptr<const json::Document> copy(json::Value value) {
  if (!value.valid()) return nullptr;
  auto parsed = json::parse(value.dump());
  return std::holds_alternative<json::Document>(parsed) ? std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed))) : nullptr;
}
constexpr std::string_view google_stores = "fileSearchStores/";
// The store a Google name is under: `fileSearchStores/<id>` of `fileSearchStores/<id>/...`.
std::string google_store(std::string_view name) {
  if (!name.starts_with(google_stores)) return {};
  return std::string(name.substr(0, name.find('/', google_stores.size())));
}
// One path segment of a resource name: letters, digits, `.`, `_` and `-`, and not `.` or `..`.
bool segment(std::string_view value) {
  return !value.empty() && value != "." && value != ".." && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}
// The status a shape's own word names: OpenAI's `status`, Google's document `state`. Another word is Unknown, and the
// caller keeps it as the raw status.
Status status_word(std::string_view shape, std::string_view word) {
  if (shape == "google") {
    if (word == "STATE_ACTIVE") return Status::Completed;
    if (word == "STATE_PENDING") return Status::InProgress;
    if (word == "STATE_FAILED") return Status::Failed;
    return Status::Unknown;
  }
  if (word == "completed") return Status::Completed;
  if (word == "in_progress") return Status::InProgress;
  if (word == "failed") return Status::Failed;
  if (word == "cancelled") return Status::Cancelled;
  if (word == "expired") return Status::Expired;
  return Status::Unknown;
}
// OpenAI's `file_counts`: each count a number when present.
bool counts_valid(json::Value counts) {
  if (!counts.valid() || counts.is_null()) return true;
  if (!counts.is_object()) return false;
  for (const char* name : {"total", "in_progress", "completed", "failed", "cancelled"})
    if (!optional_member(counts.get(name), is_uint)) return false;
  return true;
}
Counts counts_of(json::Value counts) {
  return {uint_of(counts.get("total")), uint_of(counts.get("in_progress")), uint_of(counts.get("completed")), uint_of(counts.get("failed")),
          uint_of(counts.get("cancelled"))};
}
// A store object of the shape: false when it names no id or a member has the wrong kind.
bool store_of(json::Value item, std::string_view shape, std::string_view origin, Store& store) {
  if (!item.is_object()) return false;
  store.origin = std::string(origin);
  if (shape == "google") {
    const auto name = item.get("name");
    if (!name.is_string() || !name.as_string().starts_with(google_stores) || !segment(name.as_string().substr(google_stores.size())) ||
        !optional_member(item.get("displayName"), is_string) || !optional_member(item.get("createTime"), is_time) ||
        !optional_member(item.get("updateTime"), is_time) || !optional_member(item.get("sizeBytes"), is_digits))
      return false;
    for (const char* count : {"activeDocumentsCount", "pendingDocumentsCount", "failedDocumentsCount"})
      if (!optional_member(item.get(count), is_digits)) return false;
    store.id = std::string(name.as_string());
    store.name = text_of(item.get("displayName"));
    store.files.completed = digits_of(item.get("activeDocumentsCount"));
    store.files.in_progress = digits_of(item.get("pendingDocumentsCount"));
    store.files.failed = digits_of(item.get("failedDocumentsCount"));
    store.bytes = digits_of(item.get("sizeBytes"));
    store.created_at = time_of(item.get("createTime"));
    return true;
  }
  const auto id = item.get("id");
  const auto expiry = item.get("expires_after");
  if (!id.is_string() || id.as_string().empty() || !optional_member(item.get("name"), is_string) || !optional_member(item.get("status"), is_string) ||
      !counts_valid(item.get("file_counts")) || !optional_member(item.get("usage_bytes"), is_uint) ||
      !optional_member(item.get("created_at"), is_uint) || !optional_member(item.get("expires_at"), is_uint) ||
      !optional_member(item.get("last_active_at"), is_uint) || !optional_member(expiry, is_object) ||
      (expiry.is_object() && !optional_member(expiry.get("days"), is_uint)))
    return false;
  store.id = std::string(id.as_string());
  store.name = text_of(item.get("name"));
  store.raw_status = text_of(item.get("status"));
  store.status = status_word(shape, store.raw_status);
  store.files = counts_of(item.get("file_counts"));
  store.bytes = uint_of(item.get("usage_bytes"));
  store.created_at = uint_of(item.get("created_at"));
  store.expires_at = uint_of(item.get("expires_at"));
  store.last_active_at = uint_of(item.get("last_active_at"));
  if (expiry.is_object()) store.expires_after_days = uint_of(expiry.get("days"));
  return true;
}
// A file of a store: an OpenAI vector store file, or a Google document.
bool file_of(json::Value item, std::string_view shape, std::string_view origin, StoreFile& file) {
  if (!item.is_object()) return false;
  file.origin = std::string(origin);
  if (shape == "google") {
    const auto name = item.get("name");
    if (!name.is_string() || name.as_string().find("/documents/") == std::string_view::npos || google_store(name.as_string()).empty() ||
        !optional_member(item.get("displayName"), is_string) || !optional_member(item.get("state"), is_string) ||
        !optional_member(item.get("sizeBytes"), is_digits) || !optional_member(item.get("mimeType"), is_string) ||
        !optional_member(item.get("createTime"), is_time) || !optional_member(item.get("updateTime"), is_time) ||
        !optional_member(item.get("customMetadata"), is_array))
      return false;
    file.id = std::string(name.as_string());
    file.store = google_store(file.id);
    file.name = text_of(item.get("displayName"));
    file.mime = text_of(item.get("mimeType"));
    file.raw_status = text_of(item.get("state"));
    file.status = status_word(shape, file.raw_status);
    file.bytes = digits_of(item.get("sizeBytes"));
    file.created_at = time_of(item.get("createTime"));
    if (item.get("customMetadata").is_array()) file.attributes = copy(item.get("customMetadata"));
    return true;
  }
  const auto id = item.get("id");
  const auto error = item.get("last_error");
  if (!id.is_string() || id.as_string().empty() || !optional_member(item.get("vector_store_id"), is_string) ||
      !optional_member(item.get("status"), is_string) || !optional_member(item.get("usage_bytes"), is_uint) ||
      !optional_member(item.get("created_at"), is_uint) || !optional_member(item.get("attributes"), is_object) ||
      !optional_member(error, is_object) ||
      (error.is_object() && (!optional_member(error.get("code"), is_string) || !optional_member(error.get("message"), is_string))))
    return false;
  file.id = std::string(id.as_string());
  file.store = text_of(item.get("vector_store_id"));
  file.raw_status = text_of(item.get("status"));
  file.status = status_word(shape, file.raw_status);
  if (error.is_object()) file.last_error = FileError{text_of(error.get("code")), text_of(error.get("message"))};
  if (item.get("attributes").is_object()) file.attributes = copy(item.get("attributes"));
  file.bytes = uint_of(item.get("usage_bytes"));
  file.created_at = uint_of(item.get("created_at"));
  return true;
}
// An OpenAI file batch, or a Google long-running operation. `subject` is what the call named: at OpenAI the store a
// create names, or `<store>/<batch>` for a poll and a cancel, whose answer must be that batch of that store.
bool batch_of(json::Value item, std::string_view shape, std::string_view origin, std::string_view subject, Batch& batch) {
  if (!item.is_object()) return false;
  batch.handle.origin = std::string(origin);
  batch.handle.shape = std::string(shape);
  if (shape == "google") {
    const auto name = item.get("name");
    const auto error = item.get("error");
    const auto response = item.get("response");
    if (!name.is_string() || name.as_string().find("/operations/") == std::string_view::npos || google_store(name.as_string()).empty() ||
        !optional_member(item.get("done"), is_bool) || !optional_member(error, is_object) || !optional_member(response, is_object) ||
        (error.is_object() && (!optional_member(error.get("code"), [](json::Value v) { return v.is_int() || v.is_uint(); }) ||
                               !optional_member(error.get("message"), is_string))) ||
        (response.is_object() && !optional_member(response.get("documentName"), is_string)))
      return false;
    batch.handle.id = std::string(name.as_string());
    batch.handle.store = google_store(batch.handle.id);
    const bool done = item.get("done").is_bool() && item.get("done").as_bool();
    batch.raw_status = done ? "done" : "running";
    batch.status = !done ? jobs::Status::Running : error.is_object() ? jobs::Status::Failed : jobs::Status::Completed;
    if (error.is_object()) {
      const auto code = error.get("code");
      batch.error = FileError{code.is_int() ? std::to_string(code.as_int()) : code.is_uint() ? std::to_string(code.as_uint()) : std::string(),
                              text_of(error.get("message"))};
    }
    // Google names the document an import made by its id alone, or by its full name under the store.
    if (response.is_object() && response.get("documentName").is_string()) {
      const auto document = response.get("documentName").as_string();
      const std::string under = batch.handle.store + "/documents/";
      if (segment(document)) batch.document = under + std::string(document);
      else if (document.starts_with(under) && segment(document.substr(under.size()))) batch.document = std::string(document);
      else return false;
    }
    return true;
  }
  const auto id = item.get("id");
  if (!id.is_string() || id.as_string().empty() || !optional_member(item.get("vector_store_id"), is_string) ||
      !optional_member(item.get("status"), is_string) || !counts_valid(item.get("file_counts")) || !optional_member(item.get("created_at"), is_uint))
    return false;
  const auto slash = subject.find('/');
  const auto called_store = subject.substr(0, slash);
  const auto called_batch = slash == std::string_view::npos ? std::string_view{} : subject.substr(slash + 1);
  const auto stated_store = text_of(item.get("vector_store_id"));
  // The answer is about the store and batch the call named; one that omits the store is in the call's.
  if ((!stated_store.empty() && !called_store.empty() && stated_store != called_store) || (!called_batch.empty() && id.as_string() != called_batch))
    return false;
  batch.handle.id = std::string(id.as_string());
  batch.handle.store = stated_store.empty() ? std::string(called_store) : stated_store;
  batch.raw_status = text_of(item.get("status"));
  const auto& word = batch.raw_status;
  batch.status = word == "in_progress" ? jobs::Status::Running : word == "completed" ? jobs::Status::Completed
               : word == "failed" ? jobs::Status::Failed : word == "cancelled" ? jobs::Status::Cancelled : jobs::Status::Unknown;
  batch.files = counts_of(item.get("file_counts"));
  batch.created_at = uint_of(item.get("created_at"));
  return true;
}
// OpenAI's `{type, text}` parts.
bool parts_valid(json::Value parts) {
  if (!parts.is_array()) return false;
  for (auto part : parts.elements())
    if (!part.is_object() || !optional_member(part.get("type"), is_string) || !optional_member(part.get("text"), is_string)) return false;
  return true;
}
std::vector<ContentPart> parts_of(json::Value parts) {
  std::vector<ContentPart> out;
  if (parts.is_array())
    for (auto part : parts.elements()) out.push_back({text_of(part.get("type")), text_of(part.get("text"))});
  return out;
}
bool result_valid(json::Value item) {
  return item.is_object() && item.get("file_id").is_string() && !item.get("file_id").as_string().empty() &&
         optional_member(item.get("filename"), is_string) && optional_member(item.get("score"), [](json::Value v) { return v.is_number(); }) &&
         optional_member(item.get("attributes"), is_object) && (!item.get("content").valid() || parts_valid(item.get("content")));
}
// The list member of a page: OpenAI `data`, Google `fileSearchStores` or `documents`.
std::string_view list_member(std::string_view shape, Codec::Kind kind) {
  if (shape != "google") return "data";
  return kind == Codec::Kind::Stores ? "fileSearchStores" : "documents";
}
struct Context { std::string shape, origin, subject, kind; json::Value response; };
std::optional<Context> context_of(const Completion& completion) {
  Context context;
  bool seen = false;
  for (const auto& raw : completion.raw_events) {
    if (!raw.payload) continue;
    const auto root = raw.payload->root();
    if (raw.type == "stores.context") {
      context.shape = text_of(root.get("shape"));
      context.origin = text_of(root.get("origin"));
      context.subject = text_of(root.get("subject"));
      context.kind = text_of(root.get("kind"));
    } else if (raw.type == "stores.response") { context.response = root; seen = true; }
  }
  if (!seen) return std::nullopt;
  return context;
}
}  // namespace

Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator, Kind kind, std::string subject, SemanticLimits limits)
    : descriptor_(descriptor), accumulator_(accumulator), kind_(kind), subject_(std::move(subject)), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event); }
bool Codec::fail(ErrorKind kind, const reason::Reason& text) {
  Error error = reason::error(kind, text);
  emit(Fail{std::move(error)});
  return false;
}
bool Codec::buffered(std::string_view body, Close close) {
  if (!close.normal) return fail(close.error, reason::transport::response_close);
  const auto* fact = descriptor_.policy()->endpoint("stores", descriptor_.base_url());
  if (!fact) return fail(ErrorKind::Unsupported, why::endpoint_not_admitted);
  const std::string_view shape = fact->shape;
  const std::string_view origin = descriptor_.base_url();
  // Google answers a deletion with an empty object, or nothing at all.
  auto parsed = json::parse(body.empty() ? std::string_view("{}") : body, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto* error = std::get_if<json::ParseError>(&parsed))
    return fail(error->code == json::ParseCode::SizeExceeded || error->code == json::ParseCode::DepthExceeded
                    ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, why::response_json_invalid);
  auto wire = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = wire->root();
  if (!root.is_object()) return fail(ErrorKind::ProtocolCorrupt, why::response_not_object);
  switch (kind_) {
    case Kind::Store: {
      Store probe;
      if (!store_of(root, shape, origin, probe)) return fail(ErrorKind::ProtocolCorrupt, why::store_id_required);
      break;
    }
    case Kind::File: {
      StoreFile probe;
      if (!file_of(root, shape, origin, probe)) return fail(ErrorKind::ProtocolCorrupt, why::file_id_required);
      break;
    }
    case Kind::Batch: {
      Batch probe;
      if (!batch_of(root, shape, origin, subject_, probe)) return fail(ErrorKind::ProtocolCorrupt, why::batch_id_required);
      break;
    }
    case Kind::Stores:
    case Kind::Files: {
      const auto items = root.get(list_member(shape, kind_));
      // Google leaves out an empty list.
      if (!(items.is_array() || (shape == "google" && !items.valid()))) return fail(ErrorKind::ProtocolCorrupt, why::page_list_required);
      if (items.valid() && items.size() >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, why::page_limit);
      if (items.valid())
        for (auto item : items.elements()) {
          Store store;
          StoreFile file;
          if (kind_ == Kind::Stores ? !store_of(item, shape, origin, store) : !file_of(item, shape, origin, file))
            return fail(ErrorKind::ProtocolCorrupt, kind_ == Kind::Stores ? why::store_id_required : why::file_id_required);
        }
      for (const char* cursor : {"nextPageToken", "last_id"})
        if (!optional_member(root.get(cursor), is_string)) return fail(ErrorKind::ProtocolCorrupt, why::page_cursor_invalid);
      if (!optional_member(root.get("has_more"), is_bool)) return fail(ErrorKind::ProtocolCorrupt, why::page_cursor_invalid);
      // A page that says more follow names where they start: without `last_id` it would read as the last page.
      if (root.get("has_more").is_bool() && root.get("has_more").as_bool() &&
          !(root.get("last_id").is_string() && !root.get("last_id").as_string().empty()))
        return fail(ErrorKind::ProtocolCorrupt, why::page_cursor_missing);
      break;
    }
    case Kind::Search: {
      const auto items = root.get("data");
      if (!items.is_array()) return fail(ErrorKind::ProtocolCorrupt, why::search_invalid);
      if (items.size() >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, why::page_limit);
      for (auto item : items.elements()) if (!result_valid(item)) return fail(ErrorKind::ProtocolCorrupt, why::search_invalid);
      const auto query = root.get("search_query");
      if (!optional_member(query, is_array) || !optional_member(root.get("has_more"), is_bool))
        return fail(ErrorKind::ProtocolCorrupt, why::search_invalid);
      if (query.is_array())
        for (auto q : query.elements()) if (!q.is_string()) return fail(ErrorKind::ProtocolCorrupt, why::search_invalid);
      break;
    }
    case Kind::Content: {
      const auto parts = root.get("data");
      if (!parts_valid(parts) || !optional_member(root.get("next_page"), is_string) || !optional_member(root.get("has_more"), is_bool))
        return fail(ErrorKind::ProtocolCorrupt, why::content_invalid);
      if (parts.size() >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, why::page_limit);
      break;
    }
    case Kind::Deleted:
      // OpenAI says `deleted`; Google answers an empty object.
      if (shape != "google" && !(root.get("deleted").is_bool() && root.get("deleted").as_bool()))
        return fail(ErrorKind::ProtocolCorrupt, why::delete_unconfirmed);
      break;
  }
  // What the call asked for: a listing of stores and one of files share OpenAI's `data`.
  const std::string_view listed = kind_ == Kind::Stores ? "stores" : kind_ == Kind::Files ? "files" : "";
  auto context = json::parse("{\"shape\":" + json::quote(shape) + ",\"origin\":" + json::quote(origin) + ",\"subject\":" + json::quote(subject_) +
                             ",\"kind\":" + json::quote(listed) + "}");
  if (!emit(RawWire{"stores.context", std::make_shared<const json::Document>(std::move(std::get<json::Document>(context)))}) ||
      !emit(RawWire{"stores.response", wire}))
    return false;
  if (!emit(Begin{}) || !emit(MessageBegin{{0}, std::nullopt, Role::Assistant, nullptr})) return false;
  return emit(UsageUpdate{Usage{}}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) && emit(MessageSeal{{0}}) &&
         emit(Commit{"stores.response+normal_close"});
}

StoreOutcome typed_store(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Store store;
  if (const auto context = context_of(completion)) {
    store_of(context->response, context->shape, context->origin, store);
    store.raw = copy(context->response);
  }
  store.attempt = completion.attempt;
  store.raw_events = completion.raw_events;
  return store;
}
FileOutcome typed_file(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  StoreFile file;
  if (const auto context = context_of(completion)) {
    file_of(context->response, context->shape, context->origin, file);
    file.raw = copy(context->response);
  }
  file.attempt = completion.attempt;
  file.raw_events = completion.raw_events;
  return file;
}
BatchOutcome typed_batch(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Batch batch;
  if (const auto context = context_of(completion)) {
    batch_of(context->response, context->shape, context->origin, context->subject, batch);
    batch.raw = copy(context->response);
  }
  batch.attempt = completion.attempt;
  batch.raw_events = completion.raw_events;
  return batch;
}
PageOutcome typed_page(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Page page;
  if (const auto context = context_of(completion)) {
    const auto& root = context->response;
    const bool google = context->shape == "google";
    // The call's decoder recorded what it listed.
    const auto stores = root.get(google ? "fileSearchStores" : "data");
    const auto files = root.get(google ? "documents" : "data");
    if (context->kind == "stores" && stores.is_array()) {
      for (auto item : stores.elements()) {
        Store store;
        store_of(item, context->shape, context->origin, store);
        store.raw = copy(item);
        page.stores.push_back(std::move(store));
      }
    } else if (context->kind == "files" && files.is_array()) {
      for (auto item : files.elements()) {
        StoreFile file;
        file_of(item, context->shape, context->origin, file);
        file.raw = copy(item);
        page.files.push_back(std::move(file));
      }
    }
    // Google names the next page; OpenAI says whether there is one after the last id.
    if (google) page.next = text_of(root.get("nextPageToken"));
    else if (root.get("has_more").is_bool() && root.get("has_more").as_bool()) page.next = text_of(root.get("last_id"));
  }
  page.attempt = completion.attempt;
  page.raw_events = completion.raw_events;
  return page;
}
SearchOutcome typed_search(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  SearchPage page;
  if (const auto context = context_of(completion)) {
    const auto& root = context->response;
    if (root.get("data").is_array())
      for (auto item : root.get("data").elements()) {
        SearchResult result{text_of(item.get("file_id")), text_of(item.get("filename"))};
        if (item.get("score").is_number()) result.score = item.get("score").as_double();
        if (item.get("attributes").is_object()) result.attributes = copy(item.get("attributes"));
        result.content = parts_of(item.get("content"));
        page.results.push_back(std::move(result));
      }
    if (root.get("search_query").is_array())
      for (auto q : root.get("search_query").elements()) page.search_query.push_back(text_of(q));
    page.has_more = root.get("has_more").is_bool() && root.get("has_more").as_bool();
    page.raw = copy(root);
  }
  page.attempt = completion.attempt;
  page.raw_events = completion.raw_events;
  return page;
}
ContentOutcome typed_content(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Content content;
  if (const auto context = context_of(completion)) {
    const auto& root = context->response;
    content.parts = parts_of(root.get("data"));
    content.has_more = root.get("has_more").is_bool() && root.get("has_more").as_bool();
    content.raw = copy(root);
  }
  content.attempt = completion.attempt;
  content.raw_events = completion.raw_events;
  return content;
}
DeleteOutcome typed_deleted(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Deleted deleted;
  if (const auto context = context_of(completion)) deleted.id = context->subject;
  deleted.attempt = completion.attempt;
  deleted.raw_events = completion.raw_events;
  return deleted;
}
}  // namespace sp::stores
