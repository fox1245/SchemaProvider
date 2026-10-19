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
}  // namespace

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Upload& upload) {
  auto found = route(descriptor, "upload");
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  const bool google = fact->shape == "google", openai = fact->shape == "openai";
  const auto& source = upload.source;
  const bool in_memory = source.bytes != nullptr, from_file = !source.file.path.empty();
  if (in_memory == from_file) return bad("an upload sends bytes or a file slice, not both");
  if (in_memory ? source.bytes->empty() : !source.file.size) return bad("an upload is not empty");
  if (from_file && !source.file.sha256.empty() && !sha256_hex(source.file.sha256)) return bad("a file slice digest is lower-case hex SHA-256");
  if (!file_name(upload.filename)) return bad("a file name is bounded text without quotes or control characters");
  if (!mime_type(upload.mime)) return bad("an upload names its MIME type");
  if (openai ? !token(upload.purpose, 64) : !upload.purpose.empty())
    return openai ? bad("an OpenAI file needs a purpose") : Error{ErrorKind::Unsupported, "only OpenAI files take a purpose or an expiry"};
  if (upload.expires_after && (!openai || !*upload.expires_after))
    return openai ? bad("a file expiry is positive") : Error{ErrorKind::Unsupported, "only OpenAI files take a purpose or an expiry"};
  unsigned char random[12];
  if (!crypto::random_bytes(random, sizeof random)) return Error{ErrorKind::ResourceLimit, "no random boundary for the upload"};
  std::string boundary = "sp-boundary-";
  for (const auto byte : random) { boundary += "0123456789abcdef"[byte >> 4]; boundary += "0123456789abcdef"[byte & 15]; }
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
  std::string suffix = "\r\n--" + boundary + "--\r\n";
  if (from_file) {
    encoded.file = source.file;
    encoded.body_suffix = std::move(suffix);
    return encoded;
  }
  // Bytes in memory are held to the request limit; a larger file is uploaded from its path.
  if (source.bytes->size() > descriptor.policy()->resources().request_bytes - std::min(descriptor.policy()->resources().request_bytes, prefix.size() + suffix.size()))
    return Error{ErrorKind::ResourceLimit, "an upload in memory exceeds the request limit"};
  prefix.append(*source.bytes).append(suffix);
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
