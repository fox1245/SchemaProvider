#include "codecs/files.h"
#include "crypto/crypto.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>

namespace sp::files {
namespace {
Error bad(std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; }
bool token(std::string_view value, std::size_t limit = 128) {
  return !value.empty() && value.size() <= limit && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}
// A MIME type without parameters: one `type/subtype`.
bool mime_type(std::string_view value) {
  const auto slash = value.find('/');
  const auto allowed = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::string_view("!#$&^_.+-").find(c) != std::string_view::npos;
  };
  return value.size() <= 127 && slash != std::string_view::npos && slash && slash + 1 < value.size() &&
         value.find('/', slash + 1) == std::string_view::npos &&
         std::all_of(value.begin(), value.end(), [&](char c) { return c == '/' || allowed(c); });
}
// A name that goes inside a quoted multipart parameter or a JSON string.
bool file_name(std::string_view value) {
  return !value.empty() && value.size() <= 255 && std::none_of(value.begin(), value.end(), [](unsigned char c) {
    return c < 0x20 || c == 0x7f || c == '"' || c == '\\';
  });
}
bool sha256_hex(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
// A query value, percent-encoded but for the unreserved characters.
std::string query_value(std::string_view value) {
  std::string out;
  for (const unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') out += static_cast<char>(c);
    else { out += '%'; out += "0123456789ABCDEF"[c >> 4]; out += "0123456789ABCDEF"[c & 15]; }
  }
  return out;
}
struct Route { const descriptor::EndpointFact* fact; std::string path; };
std::variant<Route, Error> route(const descriptor::ValidatedDescriptor& descriptor, std::string_view operation) {
  const auto* fact = descriptor.policy()->endpoint("files", descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "file endpoints are not admitted at this origin"};
  const auto path = descriptor::endpoint_path(*fact, operation);
  if (!path) return Error{ErrorKind::Unsupported, "the file endpoint has no path for this operation"};
  return Route{fact, std::string(*path)};
}
// A call about one file: its id is a token at OpenAI and Anthropic, and `files/<token>` at Google.
std::variant<Encoded, Error> by_id(const descriptor::ValidatedDescriptor& descriptor, std::string_view id, std::string_view operation,
                                   std::string method) {
  auto found = route(descriptor, operation);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  if (fact->shape == "google" ? !(id.starts_with("files/") && token(id.substr(6))) : !token(id))
    return bad("a file id is a token, or files/<token> at Google");
  if (const auto at = path.find("{id}"); at != std::string::npos) path.replace(at, 4, id);
  // The decoder reports the file id the call named, which a deletion's answer may not repeat.
  Encoded encoded{std::move(path), {}, std::string(id), false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.method = std::move(method);
  encoded.content_type.clear();
  return encoded;
}
// The checks every upload's content passes; the content's length.
std::variant<std::uint64_t, Error> source_length(const Source& source) {
  const bool in_memory = source.bytes != nullptr, from_file = !source.file.path.empty();
  if (in_memory == from_file) return bad("an upload sends bytes or a file slice, not both");
  if (in_memory ? source.bytes->empty() : !source.file.size) return bad("an upload is not empty");
  if (from_file && !source.file.sha256.empty() && !sha256_hex(source.file.sha256)) return bad("a file slice digest is lower-case hex SHA-256");
  return in_memory ? std::uint64_t{source.bytes->size()} : source.file.size;
}
// Puts the content after `encoded.body`, and `suffix` after it: in memory under the request limit, or as a file slice.
std::optional<Error> attach(const descriptor::ValidatedDescriptor& descriptor, Encoded& encoded, const Source& source, std::string suffix) {
  if (source.bytes == nullptr) {
    encoded.file = source.file;
    encoded.body_suffix = std::move(suffix);
    return std::nullopt;
  }
  // Bytes in memory are held to the request limit; a larger file is uploaded from its path.
  const auto limit = descriptor.policy()->resources().request_bytes;
  if (source.bytes->size() > limit - std::min(limit, encoded.body.size() + suffix.size()))
    return Error{ErrorKind::ResourceLimit, "an upload in memory exceeds the request limit"};
  encoded.body.append(*source.bytes).append(suffix);
  return std::nullopt;
}
std::variant<std::string, Error> boundary() {
  unsigned char random[12];
  if (!crypto::random_bytes(random, sizeof random)) return Error{ErrorKind::ResourceLimit, "no random boundary for the upload"};
  std::string text = "sp-boundary-";
  for (const auto byte : random) { text += "0123456789abcdef"[byte >> 4]; text += "0123456789abcdef"[byte & 15]; }
  return text;
}
// A step of a chunked upload at the origin that began it: OpenAI's path for the operation, or Google's upload URL.
std::variant<Route, Error> session_route(const descriptor::ValidatedDescriptor& descriptor, const Session& session, std::string_view operation) {
  const auto* fact = descriptor.policy()->endpoint("files", descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "file endpoints are not admitted at this origin"};
  if (session.origin != descriptor.base_url() || session.shape != fact->shape) return bad("an upload continues at the origin that began it");
  if (fact->shape == "google") {
    const auto prefix = std::string(descriptor.base_url()) + "/upload/";
    if (!session.url.starts_with(prefix) || session.url.size() > 2048 ||
        std::any_of(session.url.begin(), session.url.end(), [](unsigned char c) { return c <= 0x20 || c >= 0x7f || c == '"' || c == '\\' || c == '#'; }))
      return bad("a Google upload URL is under the origin's upload path");
    return Route{fact, session.url.substr(descriptor.base_url().size())};
  }
  const auto path = descriptor::endpoint_path(*fact, operation);
  if (!path) return Error{ErrorKind::Unsupported, "the file endpoint has no path for this operation"};
  if (!token(session.id)) return bad("an upload id is a token");
  std::string resolved(*path);
  if (const auto at = resolved.find("{id}"); at != std::string::npos) resolved.replace(at, 4, session.id);
  return Route{fact, std::move(resolved)};
}
}  // namespace

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Start& begin) {
  auto found = route(descriptor, "start");
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  const bool google = fact->shape == "google";
  if (!file_name(begin.filename)) return bad("a file name is bounded text without quotes or control characters");
  if (!mime_type(begin.mime)) return bad("an upload names its MIME type");
  if (!begin.size) return bad("an upload is not empty");
  if (google ? !begin.purpose.empty() : !token(begin.purpose, 64))
    return google ? Error{ErrorKind::Unsupported, "only OpenAI files take a purpose or an expiry"} : bad("an OpenAI file needs a purpose");
  if (begin.expires_after && (google || !*begin.expires_after))
    return google ? Error{ErrorKind::Unsupported, "only OpenAI files take a purpose or an expiry"} : bad("a file expiry is positive");
  Session start{std::string(descriptor.base_url()), fact->shape};
  start.size = begin.size;
  Encoded encoded{std::move(path), {}, session_document(start), false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  if (google) {
    // Google's resumable start: the file's size and type in headers, its metadata in the body.
    encoded.headers.emplace_back("X-Goog-Upload-Protocol", "resumable");
    encoded.headers.emplace_back("X-Goog-Upload-Command", "start");
    encoded.headers.emplace_back("X-Goog-Upload-Header-Content-Length", std::to_string(begin.size));
    encoded.headers.emplace_back("X-Goog-Upload-Header-Content-Type", begin.mime);
    encoded.body = "{\"file\":{\"display_name\":" + json::quote(begin.filename) + "}}";
    return encoded;
  }
  encoded.body = "{\"filename\":" + json::quote(begin.filename) + ",\"purpose\":" + json::quote(begin.purpose) +
                 ",\"bytes\":" + std::to_string(begin.size) + ",\"mime_type\":" + json::quote(begin.mime);
  if (begin.expires_after) encoded.body += ",\"expires_after\":{\"anchor\":\"created_at\",\"seconds\":" + std::to_string(*begin.expires_after) + "}";
  encoded.body += "}";
  return encoded;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Part& part) {
  auto found = session_route(descriptor, part.session, "part");
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  const auto length = source_length(part.source);
  if (auto* error = std::get_if<Error>(&length)) return *error;
  const auto& session = part.session;
  const auto size = std::get<std::uint64_t>(length);
  if (session.sent >= session.size || size > session.size - session.sent) return bad("a part fits in what remains of the upload");
  Encoded encoded{std::move(path), {}, session_document(session, size), false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  std::string suffix;
  if (fact->shape == "google") {
    const bool last = session.sent + size == session.size;
    if (!last && session.granularity && size % session.granularity) return bad("a Google chunk before the last is a multiple of the granularity");
    encoded.headers.emplace_back("X-Goog-Upload-Command", last ? "upload, finalize" : "upload");
    encoded.headers.emplace_back("X-Goog-Upload-Offset", std::to_string(session.sent));
    encoded.content_type = "application/octet-stream";
  } else {
    auto mark = boundary();
    if (auto* error = std::get_if<Error>(&mark)) return *error;
    const auto& b = std::get<std::string>(mark);
    encoded.body.append("--").append(b).append("\r\nContent-Disposition: form-data; name=\"data\"; filename=\"part\"\r\nContent-Type: application/octet-stream\r\n\r\n");
    encoded.content_type = "multipart/form-data; boundary=" + b;
    suffix = "\r\n--" + b + "--\r\n";
  }
  if (auto error = attach(descriptor, encoded, part.source, std::move(suffix))) return *error;
  return encoded;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Complete& complete) {
  auto found = session_route(descriptor, complete.session, "complete");
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  if (fact->shape == "google") return Error{ErrorKind::Unsupported, "a Google upload completes with its last chunk"};
  const auto& session = complete.session;
  if (session.sent != session.size || session.parts.empty()) return bad("an upload completes once every part is sent");
  Encoded encoded{std::move(path), "{\"part_ids\":[", session_document(session), false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  for (std::size_t i = 0; i < session.parts.size(); ++i) {
    if (!token(session.parts[i])) return bad("an upload part id is a token");
    encoded.body.append(i ? "," : "").append(json::quote(session.parts[i]));
  }
  encoded.body += "]}";
  return encoded;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Query& query) {
  auto found = session_route(descriptor, query.session, "query");
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  if (fact->shape != "google") return Error{ErrorKind::Unsupported, "only a Google upload says how much it holds"};
  Encoded encoded{std::move(path), {}, session_document(query.session), false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.headers.emplace_back("X-Goog-Upload-Command", "query");
  encoded.content_type.clear();
  return encoded;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Cancel& cancel) {
  auto found = session_route(descriptor, cancel.session, "cancel");
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  Encoded encoded{std::move(path), {}, session_document(cancel.session), false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  if (fact->shape == "google") encoded.headers.emplace_back("X-Goog-Upload-Command", "cancel");
  encoded.content_type.clear();
  return encoded;
}

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Upload& upload) {
  auto found = route(descriptor, "upload");
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  const bool google = fact->shape == "google", openai = fact->shape == "openai";
  if (auto length = source_length(upload.source); auto* error = std::get_if<Error>(&length)) return *error;
  if (!file_name(upload.filename)) return bad("a file name is bounded text without quotes or control characters");
  if (!mime_type(upload.mime)) return bad("an upload names its MIME type");
  if (openai ? !token(upload.purpose, 64) : !upload.purpose.empty())
    return openai ? bad("an OpenAI file needs a purpose") : Error{ErrorKind::Unsupported, "only OpenAI files take a purpose or an expiry"};
  if (upload.expires_after && (!openai || !*upload.expires_after))
    return openai ? bad("a file expiry is positive") : Error{ErrorKind::Unsupported, "only OpenAI files take a purpose or an expiry"};
  auto mark = boundary();
  if (auto* error = std::get_if<Error>(&mark)) return *error;
  const auto& boundary = std::get<std::string>(mark);
  Encoded encoded{std::move(path), {}, {}, fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  auto& prefix = encoded.body;
  if (google) {
    // Google's multipart upload: the file's metadata, then its content.
    prefix.append("--").append(boundary).append("\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n{\"file\":{\"display_name\":")
        .append(json::quote(upload.filename)).append("}}\r\n--").append(boundary).append("\r\nContent-Type: ").append(upload.mime).append("\r\n\r\n");
    encoded.content_type = "multipart/related; boundary=" + boundary;
    encoded.headers.emplace_back("X-Goog-Upload-Protocol", "multipart");
  } else {
    const auto field = [&](std::string_view name, std::string_view value) {
      prefix.append("--").append(boundary).append("\r\nContent-Disposition: form-data; name=\"").append(name).append("\"\r\n\r\n").append(value).append("\r\n");
    };
    if (openai) field("purpose", upload.purpose);
    if (upload.expires_after) {
      field("expires_after[anchor]", "created_at");
      field("expires_after[seconds]", std::to_string(*upload.expires_after));
    }
    prefix.append("--").append(boundary).append("\r\nContent-Disposition: form-data; name=\"file\"; filename=\"").append(upload.filename)
        .append("\"\r\nContent-Type: ").append(upload.mime).append("\r\n\r\n");
    encoded.content_type = "multipart/form-data; boundary=" + boundary;
  }
  if (auto error = attach(descriptor, encoded, upload.source, "\r\n--" + boundary + "--\r\n")) return *error;
  return encoded;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Get& get) {
  return by_id(descriptor, get.id, "get", "GET");
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Delete& del) {
  return by_id(descriptor, del.id, "delete", "DELETE");
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const List& list) {
  auto found = route(descriptor, "list");
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  const bool google = fact->shape == "google", openai = fact->shape == "openai";
  if (list.limit && !*list.limit) return bad("a page limit is positive");
  if (list.after.size() > 4096) return bad("a page cursor is bounded");
  if (!list.purpose.empty() && (!openai || !token(list.purpose, 64)))
    return openai ? bad("a file purpose is a token") : Error{ErrorKind::Unsupported, "only OpenAI lists files by purpose"};
  std::string query;
  const auto add = [&](std::string_view name, std::string_view value) { query.append(query.empty() ? "?" : "&").append(name).append("=").append(query_value(value)); };
  if (list.limit) add(google ? "pageSize" : "limit", std::to_string(*list.limit));
  if (!list.after.empty()) add(google ? "pageToken" : openai ? "after" : "page", list.after);
  if (!list.purpose.empty()) add("purpose", list.purpose);
  Encoded encoded{path + query, {}, {}, false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.method = "GET";
  encoded.content_type.clear();
  return encoded;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Download& download) {
  if (!download.max_bytes) return bad("a download needs a positive byte limit");
  auto found = by_id(descriptor, download.id, "content", "GET");
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& encoded = std::get<Encoded>(found);
  // Google serves the bytes of a file as its media.
  if (descriptor.policy()->endpoint("files", descriptor.base_url())->shape == "google") encoded.path += "?alt=media";
  encoded.accept = "*/*";
  encoded.sink = download.path;
  encoded.sink_limit = download.max_bytes;
  return encoded;
}
}  // namespace sp::files
