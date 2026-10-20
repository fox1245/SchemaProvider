#include "codecs/batches.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <algorithm>
#include <set>
#include <type_traits>

namespace sp::batches {
namespace {
Error bad(std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; }
// A path segment the caller names; `.` and `..` would move the request elsewhere.
bool token(std::string_view value, std::size_t limit = 128) {
  return !value.empty() && value.size() <= limit && value != "." && value != ".." && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}
bool custom_id_valid(std::string_view value) {
  return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}
std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}
struct Route { const descriptor::EndpointFact* fact; std::string path; };
std::variant<Route, Error> route(const descriptor::ValidatedDescriptor& descriptor, std::string_view endpoint, std::string_view operation) {
  const auto* fact = descriptor.policy()->endpoint(endpoint, descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "batch endpoints are not admitted at this origin"};
  const auto path = descriptor::endpoint_path(*fact, operation);
  if (!path) return Error{ErrorKind::Unsupported, "the batch endpoint has no path for this operation"};
  return Route{fact, std::string(*path)};
}
// One item as its family encodes it for a buffered call: the path, the body and the headers.
struct ItemWire { std::string path, body, model; std::vector<std::pair<std::string, std::string>> headers; };
std::variant<ItemWire, Error> item_wire(const descriptor::ValidatedDescriptor& family, const Request& request) {
  return std::visit([&](const auto& typed) -> std::variant<ItemWire, Error> {
    using T = std::decay_t<decltype(typed)>;
    auto encoded = [&] {
      if constexpr (std::is_same_v<T, chat::Request>) return chat::encode(family, typed, false);
      else if constexpr (std::is_same_v<T, messages::Request>) return messages::encode(family, typed, false);
      else if constexpr (std::is_same_v<T, responses::Request>) return responses::encode(family, typed, false);
      else return gemini::encode(family, typed, false);
    }();
    if (auto* error = std::get_if<Error>(&encoded)) return *error;
    auto& value = std::get<0>(encoded);
    return ItemWire{std::move(value.path), std::move(value.body), typed.model, std::move(value.headers)};
  }, request);
}
// The items of an inline batch, checked: valid and unique ids, the family the origin serves.
template <class Family> std::optional<Error> check_items(const std::vector<Item>& items) {
  if (items.empty()) return bad("a batch has items");
  if (items.size() > 100000) return bad("a batch has at most 100000 items");
  std::set<std::string_view> seen;
  for (const auto& item : items) {
    if (!custom_id_valid(item.custom_id) || !seen.insert(item.custom_id).second) return bad("a batch item has a unique custom id of up to 64 letters, digits, _ and -");
    if (!std::holds_alternative<Family>(item.request)) return Error{ErrorKind::Unsupported, "a batch item is a request of the origin's family"};
  }
  return std::nullopt;
}
std::variant<Encoded, Error> by_id(const descriptor::ValidatedDescriptor& descriptor, std::string_view id, std::string_view operation, std::string method) {
  auto found = route(descriptor, "batches", operation);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  if (fact->shape == "google" ? !(id.starts_with("batches/") && token(id.substr(8))) : !token(id))
    return bad("a batch id is a token, or batches/<token> at Google");
  if (const auto at = path.find("{id}"); at != std::string::npos) path.replace(at, 4, id);
  Encoded encoded{std::move(path), {}, std::string(id), false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.method = std::move(method);
  encoded.content_type.clear();
  return encoded;
}
}  // namespace

std::variant<std::string, Error> jsonl(const descriptor::ValidatedDescriptor& family, const std::vector<Item>& items) {
  if (family.family() != "openai.chat" && family.family() != "openai.responses") return Error{ErrorKind::Unsupported, "an OpenAI batch file holds Chat or Responses requests"};
  if (items.empty()) return bad("a batch has items");
  std::set<std::string_view> seen;
  std::string out;
  for (const auto& item : items) {
    if (!custom_id_valid(item.custom_id) || !seen.insert(item.custom_id).second) return bad("a batch item has a unique custom id of up to 64 letters, digits, _ and -");
    auto wire = item_wire(family, item.request);
    if (auto* error = std::get_if<Error>(&wire)) return *error;
    auto& value = std::get<ItemWire>(wire);
    out += "{\"custom_id\":" + json::quote(item.custom_id) + ",\"method\":\"POST\",\"url\":" + json::quote(value.path) + ",\"body\":" + value.body + "}\n";
  }
  return out;
}

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Submit& submit) {
  auto found = route(descriptor, "batches", "submit");
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  const auto& shape = fact->shape;
  if (submit.extensions) {
    if (!fact->options) return Error{ErrorKind::Unsupported, "the batch endpoint admits no options"};
    if (!submit.extensions->root().is_object() || descriptor::check_value(*fact->options, submit.extensions->root()))
      return bad("batch options do not match the policy's schema");
  }
  std::string body;
  std::string model;
  if (shape == "openai") {
    if (!submit.items.empty()) return bad("an OpenAI batch reads its items from an uploaded file");
    if (!token(submit.input_file)) return bad("an OpenAI batch names its uploaded input file");
    if (!submit.endpoint.starts_with("/v1/") || !std::all_of(submit.endpoint.begin(), submit.endpoint.end(), [](char c) { return c == '/' || c == '_' || c == '-' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }))
      return bad("an OpenAI batch names the path its lines call");
    body = "{\"input_file_id\":" + json::quote(submit.input_file) + ",\"endpoint\":" + json::quote(submit.endpoint) + ",\"completion_window\":\"24h\"";
  } else if (shape == "anthropic") {
    if (auto error = check_items<messages::Request>(submit.items)) return *error;
    body = "{\"requests\":[";
    for (std::size_t i = 0; i < submit.items.size(); ++i) {
      auto wire = item_wire(descriptor, submit.items[i].request);
      if (auto* error = std::get_if<Error>(&wire)) return *error;
      auto& value = std::get<ItemWire>(wire);
      // One batch call carries one set of headers: an item cannot ask for a beta of its own.
      for (const auto& header : value.headers)
        if (lower(header.first) == "anthropic-beta") return Error{ErrorKind::Unsupported, "a batch item needs no beta header"};
      body.append(i ? "," : "").append("{\"custom_id\":").append(json::quote(submit.items[i].custom_id)).append(",\"params\":").append(value.body).append("}");
    }
    body += "]";
  } else {
    if (auto error = check_items<gemini::Request>(submit.items)) return *error;
    body = "{\"batch\":{";
    if (!submit.display_name.empty()) body += "\"display_name\":" + json::quote(submit.display_name) + ",";
    body += "\"input_config\":{\"requests\":{\"requests\":[";
    for (std::size_t i = 0; i < submit.items.size(); ++i) {
      auto wire = item_wire(descriptor, submit.items[i].request);
      if (auto* error = std::get_if<Error>(&wire)) return *error;
      auto& value = std::get<ItemWire>(wire);
      if (i && value.model != model) return bad("a Google batch runs one model");
      model = value.model;
      body.append(i ? "," : "").append("{\"request\":").append(value.body).append(",\"metadata\":{\"key\":").append(json::quote(submit.items[i].custom_id)).append("}}");
    }
    body += "]}}}";
    // Google names the model in the path.
    if (!token(model)) return bad("a Google batch model is a path segment");
    if (const auto at = path.find("{model}"); at != std::string::npos) path.replace(at, 7, model);
  }
  if (submit.extensions) for (auto member : submit.extensions->root().members()) body += "," + json::quote(member.key) + ":" + member.value.dump();
  body += "}";
  if (body.size() > descriptor.policy()->resources().request_bytes) return Error{ErrorKind::ResourceLimit, "a batch exceeds the request limit"};
  Encoded encoded{std::move(path), std::move(body), std::move(model), fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  return encoded;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Get& get) {
  return by_id(descriptor, get.id, "get", "GET");
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Cancel& cancel) {
  return by_id(descriptor, cancel.id, "cancel", "POST");
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Delete& del) {
  return by_id(descriptor, del.id, "delete", "DELETE");
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Results& results) {
  if (!results.max_bytes) return bad("a download needs a positive byte limit");
  const auto& batch = results.batch;
  if (batch.origin != descriptor.base_url()) return bad("a batch is reached at the origin that started it");
  if (batch.shape == "anthropic") {
    if (results.errors) return Error{ErrorKind::Unsupported, "only an OpenAI batch has a separate error file"};
    auto found = by_id(descriptor, batch.id, "results", "GET");
    if (auto* error = std::get_if<Error>(&found)) return *error;
    auto& encoded = std::get<Encoded>(found);
    encoded.accept = "*/*";
    encoded.sink = results.path;
    encoded.sink_limit = results.max_bytes;
    return encoded;
  }
  // OpenAI's output and error files, and Google's responses file, are files of the origin.
  const auto& file = results.errors ? batch.error_file : batch.output_file;
  if (results.errors && batch.shape != "openai") return Error{ErrorKind::Unsupported, "only an OpenAI batch has a separate error file"};
  if (file.empty()) return bad("the batch has no results file yet");
  const auto* fact = descriptor.policy()->endpoint("files", descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "batch endpoints are not admitted at this origin"};
  const auto content = descriptor::endpoint_path(*fact, "content");
  if (!content) return Error{ErrorKind::Unsupported, "the batch endpoint has no path for this operation"};
  if (fact->shape == "google" ? !(file.starts_with("files/") && token(file.substr(6))) : !token(file)) return bad("a batch results file is a file id");
  std::string path(*content);
  if (const auto at = path.find("{id}"); at != std::string::npos) path.replace(at, 4, file);
  if (fact->shape == "google") path += "?alt=media";
  Encoded encoded{std::move(path), {}, batch.id, false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.method = "GET";
  encoded.content_type.clear();
  encoded.accept = "*/*";
  encoded.sink = results.path;
  encoded.sink_limit = results.max_bytes;
  return encoded;
}
}  // namespace sp::batches
