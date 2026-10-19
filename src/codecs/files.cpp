#include "codecs/files.h"
#include "codecs/reason.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>

namespace sp::files {
namespace {
// RFC 3339 (`2026-10-19T13:02:22.150902Z`, or an offset) as Unix seconds.
std::optional<std::uint64_t> unix_seconds(std::string_view text) {
  const auto number = [&](std::size_t at, std::size_t digits) -> std::optional<int> {
    if (at + digits > text.size()) return std::nullopt;
    int value = 0;
    for (std::size_t i = at; i < at + digits; ++i) {
      if (text[i] < '0' || text[i] > '9') return std::nullopt;
      value = value * 10 + (text[i] - '0');
    }
    return value;
  };
  const auto year = number(0, 4), month = number(5, 2), day = number(8, 2), hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
  if (!year || !month || !day || !hour || !minute || !second || text.size() < 20 || text[4] != '-' || text[7] != '-' ||
      (text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':' || *month < 1 || *month > 12 || *day < 1 ||
      *day > 31 || *hour > 23 || *minute > 59 || *second > 60 || *year < 1970)
    return std::nullopt;
  std::size_t at = 19;
  if (text[at] == '.') {
    ++at;
    const auto start = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
    if (at == start) return std::nullopt;
  }
  std::int64_t offset = 0;
  if (at < text.size() && (text[at] == 'Z' || text[at] == 'z')) ++at;
  else if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
    const auto hours = number(at + 1, 2), minutes = number(at + 4, 2);
    if (!hours || !minutes || at + 6 > text.size() || text[at + 3] != ':') return std::nullopt;
    offset = (text[at] == '+' ? 1 : -1) * (std::int64_t{*hours} * 3600 + std::int64_t{*minutes} * 60);
    at += 6;
  } else return std::nullopt;
  if (at != text.size()) return std::nullopt;
  // Days from the civil date (Howard Hinnant's algorithm).
  const int y = *year - (*month <= 2), era = y / 400, yoe = y - era * 400;
  const int doy = (153 * (*month + (*month > 2 ? -3 : 9)) + 2) / 5 + *day - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + doe - 719468;
  const std::int64_t seconds = days * 86400 + std::int64_t{*hour} * 3600 + std::int64_t{*minute} * 60 + *second - offset;
  if (seconds < 0) return std::nullopt;
  return static_cast<std::uint64_t>(seconds);
}
std::string text_of(json::Value value) { return value.is_string() ? std::string(value.as_string()) : std::string(); }
// A member that is absent or null, or of the kind `check` accepts.
template <class Check> bool optional_member(json::Value value, Check check) { return !value.valid() || value.is_null() || check(value); }
bool is_string(json::Value value) { return value.is_string(); }
bool is_uint(json::Value value) { return value.is_uint(); }
bool is_time(json::Value value) { return value.is_string() && unix_seconds(value.as_string()).has_value(); }
bool is_digits(json::Value value) {
  return value.is_string() && !value.as_string().empty() && value.as_string().size() <= 20 &&
         std::all_of(value.as_string().begin(), value.as_string().end(), [](char c) { return c >= '0' && c <= '9'; });
}
// Google gives its SHA-256 as the base64 of the digest's hex, or of the digest itself.
std::string google_digest(std::string_view encoded) {
  std::string decoded;
  if (!base64_decode(encoded, decoded)) return {};
  if (decoded.size() == 64 && std::all_of(decoded.begin(), decoded.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
    return decoded;
  if (decoded.size() != 32) return {};
  std::string hex;
  for (const unsigned char byte : decoded) { hex += "0123456789abcdef"[byte >> 4]; hex += "0123456789abcdef"[byte & 15]; }
  return hex;
}
// A file object of the shape: false when it names no id or a member has the wrong kind.
bool file_of(json::Value item, std::string_view shape, std::string_view origin, File& file) {
  if (!item.is_object()) return false;
  file.origin = std::string(origin);
  if (shape == "google") {
    const auto name = item.get("name");
    if (!name.is_string() || !name.as_string().starts_with("files/") ||
        !optional_member(item.get("sizeBytes"), is_digits) || !optional_member(item.get("createTime"), is_time) ||
        !optional_member(item.get("expirationTime"), is_time) || !optional_member(item.get("state"), is_string) ||
        !optional_member(item.get("uri"), is_string) || !optional_member(item.get("mimeType"), is_string) ||
        !optional_member(item.get("displayName"), is_string) || !optional_member(item.get("sha256Hash"), is_string))
      return false;
    file.id = std::string(name.as_string());
    file.uri = text_of(item.get("uri"));
    file.name = text_of(item.get("displayName"));
    file.mime = text_of(item.get("mimeType"));
    if (item.get("sizeBytes").is_string()) file.bytes = std::stoull(std::string(item.get("sizeBytes").as_string()));
    if (item.get("createTime").is_string()) file.created_at = unix_seconds(item.get("createTime").as_string());
    if (item.get("expirationTime").is_string()) file.expires_at = unix_seconds(item.get("expirationTime").as_string());
    const auto state = text_of(item.get("state"));
    file.state = state == "ACTIVE" ? State::Active : state == "PROCESSING" ? State::Processing : state == "FAILED" ? State::Failed : State::Unknown;
    if (item.get("sha256Hash").is_string()) file.sha256 = google_digest(item.get("sha256Hash").as_string());
    return true;
  }
  const auto id = item.get("id");
  if (!id.is_string() || id.as_string().empty() || !optional_member(item.get("filename"), is_string)) return false;
  file.id = std::string(id.as_string());
  file.name = text_of(item.get("filename"));
  if (shape == "openai") {
    if (!optional_member(item.get("bytes"), is_uint) || !optional_member(item.get("created_at"), is_uint) ||
        !optional_member(item.get("expires_at"), is_uint) || !optional_member(item.get("purpose"), is_string) ||
        !optional_member(item.get("status"), is_string))
      return false;
    if (item.get("bytes").is_uint()) file.bytes = item.get("bytes").as_uint();
    if (item.get("created_at").is_uint()) file.created_at = item.get("created_at").as_uint();
    if (item.get("expires_at").is_uint()) file.expires_at = item.get("expires_at").as_uint();
    file.purpose = text_of(item.get("purpose"));
    const auto status = text_of(item.get("status"));
    // OpenAI files are usable once uploaded; `status` is a deprecated field some objects still carry.
    file.state = status == "error" ? State::Failed : State::Active;
    return true;
  }
  if (!optional_member(item.get("size_bytes"), is_uint) || !optional_member(item.get("created_at"), is_time) ||
      !optional_member(item.get("expires_at"), is_time) || !optional_member(item.get("mime_type"), is_string) ||
      !optional_member(item.get("downloadable"), [](json::Value value) { return value.is_bool(); }))
    return false;
  if (item.get("size_bytes").is_uint()) file.bytes = item.get("size_bytes").as_uint();
  if (item.get("created_at").is_string()) file.created_at = unix_seconds(item.get("created_at").as_string());
  if (item.get("expires_at").is_string()) file.expires_at = unix_seconds(item.get("expires_at").as_string());
  file.mime = text_of(item.get("mime_type"));
  if (item.get("downloadable").is_bool()) file.downloadable = item.get("downloadable").as_bool();
  file.state = State::Active;
  return true;
}
// The file object of a response: Google wraps an uploaded file in `file`.
json::Value file_object(json::Value root, std::string_view shape) {
  return shape == "google" && root.get("file").is_object() ? root.get("file") : root;
}
std::shared_ptr<const json::Document> copy(json::Value value) {
  auto parsed = json::parse(value.dump());
  return std::holds_alternative<json::Document>(parsed) ? std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed))) : nullptr;
}
struct Context { std::string shape, origin, subject, uploaded; json::Value response; };
// The shape, origin and id the call recorded, and the response document.
std::optional<Context> context_of(const Completion& completion) {
  Context context;
  bool seen = false;
  for (const auto& raw : completion.raw_events) {
    if (!raw.payload) continue;
    const auto root = raw.payload->root();
    if (raw.type == "files.context") {
      context.shape = text_of(root.get("shape"));
      context.origin = text_of(root.get("origin"));
      context.subject = text_of(root.get("subject"));
      context.uploaded = text_of(root.get("uploaded_sha256"));
    } else if (raw.type == "files.response") { context.response = root; seen = true; }
  }
  if (!seen) return std::nullopt;
  return context;
}
}  // namespace

std::variant<Media, Error> media(const File& file, std::string_view origin, std::uint64_t now) {
  if (file.origin != origin) return Error{ErrorKind::InvalidRequest, "a file is named only at the origin that holds it"};
  if (file.state != State::Active) return Error{ErrorKind::InvalidRequest, "a file is named once it is active"};
  if (file.expires_at && *file.expires_at <= now) return Error{ErrorKind::InvalidRequest, "the file has expired"};
  const std::string_view mime = file.mime;
  const auto kind = mime.starts_with("image/") ? MediaKind::Image : mime.starts_with("audio/") ? MediaKind::Audio
                  : mime.starts_with("video/") ? MediaKind::Video : MediaKind::Document;
  // Google names a file by its URI; OpenAI and Anthropic by its id.
  return Media::file(kind, file.uri.empty() ? file.id : file.uri, file.mime);
}

Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator, Kind kind, std::string subject, SemanticLimits limits)
    : descriptor_(descriptor), accumulator_(accumulator), kind_(kind), subject_(std::move(subject)), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event); }
bool Codec::fail(ErrorKind kind, std::string text) {
  Error error{kind, std::move(text)};
  error.reason = reason::code(reason::Source::Files, error.safe_message);
  emit(Fail{std::move(error)});
  return false;
}
bool Codec::buffered(std::string_view body, Close close) {
  if (!close.normal) return fail(close.error, "abnormal response close");
  const auto* fact = descriptor_.policy()->endpoint("files", descriptor_.base_url());
  if (!fact) return fail(ErrorKind::Unsupported, "file endpoints are not admitted at this origin");
  const std::string_view shape = fact->shape;
  // Google answers a deletion with an empty object, or nothing at all.
  auto parsed = json::parse(body.empty() ? std::string_view("{}") : body, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto* error = std::get_if<json::ParseError>(&parsed))
    return fail(error->code == json::ParseCode::SizeExceeded || error->code == json::ParseCode::DepthExceeded
                    ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, "invalid response JSON");
  auto wire = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = wire->root();
  if (!root.is_object()) return fail(ErrorKind::ProtocolCorrupt, "response must be object");
  auto context = json::parse("{\"shape\":" + json::quote(shape) + ",\"origin\":" + json::quote(descriptor_.base_url()) +
                             ",\"subject\":" + json::quote(subject_) + ",\"uploaded_sha256\":" + json::quote(close.upload_sha256) + "}");
  if (!emit(RawWire{"files.context", std::make_shared<const json::Document>(std::move(std::get<json::Document>(context)))}) ||
      !emit(RawWire{"files.response", wire}))
    return false;
  File probe;
  if (kind_ == Kind::File) {
    if (!file_of(file_object(root, shape), shape, descriptor_.base_url(), probe)) return fail(ErrorKind::ProtocolCorrupt, "a file object names its id");
  } else if (kind_ == Kind::Page) {
    const auto items = root.get(shape == "google" ? "files" : "data");
    // Google leaves out an empty list.
    if (!(items.is_array() || (shape == "google" && !items.valid()))) return fail(ErrorKind::ProtocolCorrupt, "a file listing holds a list");
    if (items.valid() && items.size() >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, "a file listing exceeds the part limit");
    if (items.valid())
      for (auto item : items.elements()) if (!file_of(item, shape, descriptor_.base_url(), probe)) return fail(ErrorKind::ProtocolCorrupt, "a file object names its id");
    for (const char* cursor : {"nextPageToken", "next_page", "last_id"})
      if (!optional_member(root.get(cursor), is_string)) return fail(ErrorKind::ProtocolCorrupt, "a page cursor is a string");
  } else {
    // OpenAI says `deleted`, Anthropic answers a `file_deleted` object, Google an empty one.
    const bool refused = (shape == "openai" && !(root.get("deleted").is_bool() && root.get("deleted").as_bool())) ||
                         (shape == "anthropic" && text_of(root.get("type")) != "file_deleted");
    if (refused) return fail(ErrorKind::ProtocolCorrupt, "the response does not confirm the deletion");
  }
  if (!emit(Begin{}) || !emit(MessageBegin{{0}, std::nullopt, Role::Assistant, nullptr})) return false;
  return emit(UsageUpdate{Usage{}}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) && emit(MessageSeal{{0}}) &&
         emit(Commit{"files.response+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  File file;
  if (const auto context = context_of(completion)) {
    const auto object = file_object(context->response, context->shape);
    file_of(object, context->shape, context->origin, file);
    file.raw = copy(object);
    file.uploaded_sha256 = context->uploaded;
  }
  file.attempt = completion.attempt;
  file.raw_events = completion.raw_events;
  return file;
}
PageOutcome typed_page(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Page page;
  if (const auto context = context_of(completion)) {
    const auto& root = context->response;
    const auto items = root.get(context->shape == "google" ? "files" : "data");
    if (items.is_array())
      for (auto item : items.elements()) {
        File file;
        file_of(item, context->shape, context->origin, file);
        file.raw = copy(item);
        page.files.push_back(std::move(file));
      }
    // Google and Anthropic name the next page; OpenAI says whether there is one after the last id.
    if (context->shape == "google") page.next = text_of(root.get("nextPageToken"));
    else if (root.get("next_page").valid()) page.next = text_of(root.get("next_page"));
    else if (root.get("has_more").is_bool() && root.get("has_more").as_bool()) page.next = text_of(root.get("last_id"));
  }
  page.attempt = completion.attempt;
  page.raw_events = completion.raw_events;
  return page;
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
}  // namespace sp::files
