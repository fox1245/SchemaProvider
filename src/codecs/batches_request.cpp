#include "codecs/batches.h"
#include "core/media.h"
#include "codecs/reason.h"
#include "codecs/batches_internal.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <algorithm>
#include <set>
#include <type_traits>

namespace sp::batches {
namespace {
Error bad(const reason::Reason& message) { return reason::error(ErrorKind::InvalidRequest, message); }
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
  if (!fact) return reason::error(ErrorKind::Unsupported, reason::batches_request::endpoint_not_admitted);
  const auto path = descriptor::endpoint_path(*fact, operation);
  if (!path) return reason::error(ErrorKind::Unsupported, reason::batches_request::path_missing);
  return Route{fact, std::string(*path)};
}
// One item as its family encodes it for a buffered call: the path, the body and the headers, with the output cap and
// the model invocations the family encoder read from it.
struct ItemWire {
  std::string path, body, model;
  std::vector<std::pair<std::string, std::string>> headers;
  std::optional<std::uint64_t> max_output_tokens, model_invocation_limit;
};
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
    return ItemWire{std::move(value.path), std::move(value.body), typed.model, std::move(value.headers), value.max_output_tokens,
                    value.model_invocation_limit};
  }, request);
}
// The items of an inline batch, checked: no more than the endpoint fact's `max_items`, valid and unique ids, the family
// the origin serves.
template <class Family> std::optional<Error> check_items(const descriptor::ValidatedDescriptor& descriptor, const std::vector<Item>& items) {
  if (items.empty()) return bad(reason::batches_request::items_required);
  const auto limit = descriptor.policy()->endpoint_limit("batches", descriptor.base_url(), "max_items");
  if (limit && items.size() > *limit) return bad(reason::batches_request::items_limit);
  std::set<std::string_view> seen;
  for (const auto& item : items) {
    if (!custom_id_valid(item.custom_id) || !seen.insert(item.custom_id).second) return bad(reason::batches_request::custom_id_invalid);
    if (!std::holds_alternative<Family>(item.request)) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_family);
  }
  return std::nullopt;
}
std::variant<Encoded, Error> by_id(const descriptor::ValidatedDescriptor& descriptor, std::string_view id, std::string_view operation, std::string method) {
  auto found = route(descriptor, "batches", operation);
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  if (fact->shape == "google" ? !(id.starts_with("batches/") && token(id.substr(8))) : !token(id))
    return bad(reason::batches_request::id_invalid);
  if (const auto at = path.find("{id}"); at != std::string::npos) path.replace(at, 4, id);
  Encoded encoded{std::move(path), {}, std::string(id), false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.method = std::move(method);
  encoded.content_type.clear();
  return encoded;
}
// An Anthropic batch serves its results itself; the others leave them in a file of the origin, or (OpenRouter) in the
// batch object.
bool serves_results(const Batch& batch) { return batch.shape == "anthropic"; }

// The family whose API a request is of.
std::string_view family_of(const Request& request) noexcept {
  return std::visit([](const auto& typed) -> std::string_view {
    using T = std::decay_t<decltype(typed)>;
    if constexpr (std::is_same_v<T, chat::Request>) return "openai.chat";
    else if constexpr (std::is_same_v<T, messages::Request>) return "anthropic.messages";
    else if constexpr (std::is_same_v<T, responses::Request>) return "openai.responses";
    else if constexpr (std::is_same_v<T, gemini::Request>) return "google.generate";
    else static_assert(sizeof(T) == 0, "a family request has no family here");
  }, request);
}
// The media type of `mime` without parameters, in lower case.
std::string essence(std::string_view mime) {
  auto cut = mime.substr(0, mime.find(';'));
  while (!cut.empty() && (cut.back() == ' ' || cut.back() == '\t')) cut.remove_suffix(1);
  return lower(std::string(cut));
}
// Whether the items fact admits `media`: its kind from its source, and its media type when it names one.
bool admitted(const descriptor::BatchItemsFact& items, const Media& media) {
  const auto kind = static_cast<std::size_t>(media.kind);
  if (kind >= items.media.kinds.size()) return false;
  const auto& policy = items.media.kinds[kind];
  if (!(policy.sources & (1U << static_cast<unsigned>(media.source)))) return false;
  if (media.mime.empty()) return true;
  const auto type = essence(media.mime);
  return std::find(policy.mime.begin(), policy.mime.end(), type) != policy.mime.end();
}
// The media a message carries, its tool results' included, each admitted by the items fact.
bool media_admitted(const descriptor::BatchItemsFact& items, const std::vector<Message>& messages) {
  for (const auto& message : messages)
    for (const auto& part : message.parts) {
      if (const auto* media = std::get_if<Media>(&part); media && !admitted(items, *media)) return false;
      if (const auto* result = std::get_if<ToolResult>(&part))
        for (const auto& inner : result->content_parts)
          if (const auto* media = std::get_if<Media>(&inner); media && !admitted(items, *media)) return false;
    }
  return true;
}
// Whether the model id names a variant the batch path refuses: a segment after the first `:` of `vendor/model:a:b`.
bool refused_variant(const descriptor::BatchItemsFact& items, std::string_view model) {
  auto at = model.find(':');
  while (at != std::string_view::npos) {
    const auto next = model.find(':', at + 1);
    const auto variant = model.substr(at + 1, next == std::string_view::npos ? std::string_view::npos : next - at - 1);
    if (std::find(items.refused_variants.begin(), items.refused_variants.end(), variant) != items.refused_variants.end()) return true;
    at = next;
  }
  return false;
}
// Typed Chat messages carry their media beside the text.
bool media_admitted(const descriptor::BatchItemsFact& items, const std::vector<chat::InputMessage>& messages) {
  return std::all_of(messages.begin(), messages.end(), [&](const chat::InputMessage& message) {
    return std::all_of(message.media.begin(), message.media.end(), [&](const Media& media) { return admitted(items, media); });
  });
}
// A hosted tool the batch path does not run, by the type it has on the wire.
bool refused_tool(const descriptor::BatchItemsFact& items, const std::vector<responses::HostedTool>& tools) {
  return std::any_of(tools.begin(), tools.end(), [&](const responses::HostedTool& tool) {
    return std::find(items.refused_tools.begin(), items.refused_tools.end(), responses::tool_type(tool)) != items.refused_tools.end();
  });
}
bool refused_plugin(const descriptor::BatchItemsFact& items, const std::vector<OpenRouterPlugin>& plugins) {
  return std::any_of(plugins.begin(), plugins.end(), [&](const OpenRouterPlugin& plugin) {
    return std::find(items.refused_plugins.begin(), items.refused_plugins.end(), plugin.id) != items.refused_plugins.end();
  });
}
// What the batch path of a gateway refuses in one item, before anything is sent: a request of another family than the
// client's, a variant, a plugin or a hosted tool the items fact names, routing of its own (an item's `provider` does
// not route the batch), fallback models, image or audio output, and media the items fact does not admit.
std::optional<Error> gateway_item(const descriptor::ValidatedDescriptor& descriptor, const descriptor::BatchItemsFact& items, const Request& request) {
  if (family_of(request) != descriptor.family()) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_family);
  return std::visit([&](const auto& typed) -> std::optional<Error> {
    using T = std::decay_t<decltype(typed)>;
    if (refused_variant(items, typed.model)) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_variant);
    if constexpr (std::is_same_v<T, gemini::Request>) {
      return reason::error(ErrorKind::Unsupported, reason::batches_request::item_family);
    } else {
      if (typed.provider) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_routing);
      if constexpr (std::is_same_v<T, chat::Request>) {
        if (!typed.models.empty()) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_fallbacks);
        if (refused_plugin(items, typed.plugins)) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_plugin);
        const bool generated = typed.audio || typed.image_config ||
                               std::any_of(typed.modalities.begin(), typed.modalities.end(), [](chat::OutputModality m) { return m != chat::OutputModality::Text; });
        if (generated) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_output);
        if (!media_admitted(items, typed.canonical_messages) || !media_admitted(items, typed.messages))
          return reason::error(ErrorKind::Unsupported, reason::batches_request::item_media);
      } else if constexpr (std::is_same_v<T, responses::Request>) {
        if (refused_plugin(items, typed.plugins)) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_plugin);
        const bool generated = typed.image_config ||
                               std::any_of(typed.modalities.begin(), typed.modalities.end(), [](responses::OutputModality m) { return m != responses::OutputModality::Text; });
        if (generated) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_output);
        if (refused_tool(items, typed.hosted_tools)) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_tool);
        if (!media_admitted(items, typed.messages)) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_media);
      } else {
        // One batch call carries one set of headers: an item cannot ask for a beta of its own.
        if (!typed.betas.empty()) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_beta);
        if (!media_admitted(items, typed.messages)) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_media);
      }
      return std::nullopt;
    }
  }, request);
}
}  // namespace

std::string_view operation(const Submit&) noexcept { return "submit"; }
std::string_view operation(const Get&) noexcept { return "get"; }
std::string_view operation(const Cancel&) noexcept { return "cancel"; }
std::string_view operation(const Delete&) noexcept { return "delete"; }
std::string_view operation(const List&) noexcept { return "list"; }
std::pair<std::string_view, std::string_view> results_operation(const Results& results) noexcept {
  if (serves_results(results.batch)) return {"batches", "results"};
  return {"files", "content"};
}
std::variant<std::string, Error> jsonl(const descriptor::ValidatedDescriptor& family, const std::vector<Item>& items) {
  auto encoded = detail::encode_lines(family, items);
  if (auto* error = std::get_if<Error>(&encoded)) return std::move(*error);
  return std::move(std::get<detail::EncodedLines>(encoded).text);
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Submit& submit) {
  auto encoded = detail::encode_submit(descriptor, submit);
  if (auto* error = std::get_if<Error>(&encoded)) return std::move(*error);
  return std::move(std::get<detail::EncodedSubmit>(encoded).encoded);
}

namespace detail {
std::variant<EncodedLines, Error> encode_lines(const descriptor::ValidatedDescriptor& family, const std::vector<Item>& items) {
  if (family.family() != "openai.chat" && family.family() != "openai.responses") return reason::error(ErrorKind::Unsupported, reason::batches_request::file_family);
  if (items.empty()) return bad(reason::batches_request::items_required);
  std::set<std::string_view> seen;
  EncodedLines out;
  for (const auto& item : items) {
    if (!custom_id_valid(item.custom_id) || !seen.insert(item.custom_id).second) return bad(reason::batches_request::custom_id_invalid);
    auto wire = item_wire(family, item.request);
    if (auto* error = std::get_if<Error>(&wire)) return *error;
    auto& value = std::get<ItemWire>(wire);
    out.text += "{\"custom_id\":" + json::quote(item.custom_id) + ",\"method\":\"POST\",\"url\":" + json::quote(value.path) + ",\"body\":" + value.body + "}\n";
    out.items.push_back({item.custom_id, std::move(value.model), value.max_output_tokens, value.model_invocation_limit});
  }
  return out;
}

std::variant<EncodedSubmit, Error> encode_submit(const descriptor::ValidatedDescriptor& descriptor, const Submit& submit) {
  auto found = route(descriptor, "batches", operation(submit));
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  const auto& shape = fact->shape;
  if (submit.extensions) {
    if (!fact->options) return reason::error(ErrorKind::Unsupported, reason::batches_request::options_not_admitted);
    if (!submit.extensions->root().is_object() || descriptor::check_value(*fact->options, submit.extensions->root()))
      return bad(reason::batches_request::options_invalid);
  }
  std::string body;
  std::string model;
  std::vector<ItemCaps> items;
  if (shape == "openai") {
    if (!submit.items.empty()) return bad(reason::batches_request::items_unsupported);
    if (!token(submit.input_file)) return bad(reason::batches_request::input_file_required);
    if (!descriptor::valid_path(submit.endpoint)) return bad(reason::batches_request::endpoint_invalid);
    body = "{\"input_file_id\":" + json::quote(submit.input_file) + ",\"endpoint\":" + json::quote(submit.endpoint) + ",\"completion_window\":\"24h\"";
  } else if (shape == "anthropic") {
    if (auto error = check_items<messages::Request>(descriptor, submit.items)) return *error;
    body = "{\"requests\":[";
    for (std::size_t i = 0; i < submit.items.size(); ++i) {
      auto wire = item_wire(descriptor, submit.items[i].request);
      if (auto* error = std::get_if<Error>(&wire)) return *error;
      auto& value = std::get<ItemWire>(wire);
      // One batch call carries one set of headers: an item cannot ask for a beta of its own.
      for (const auto& header : value.headers)
        if (lower(header.first) == "anthropic-beta") return reason::error(ErrorKind::Unsupported, reason::batches_request::item_beta);
      body.append(i ? "," : "").append("{\"custom_id\":").append(json::quote(submit.items[i].custom_id)).append(",\"params\":").append(value.body).append("}");
      items.push_back({submit.items[i].custom_id, value.model, value.max_output_tokens, value.model_invocation_limit});
    }
    body += "]";
  } else if (shape == "openrouter") {
    // One model and one family per batch, every request inline; the batch's own members come before `requests`, which
    // the gateway reads last.
    const auto* items_fact = descriptor.policy()->batch_items("batches", descriptor.base_url());
    if (!items_fact) return reason::error(ErrorKind::Unsupported, reason::batches_request::endpoint_not_admitted);
    if (!submit.input_file.empty() || !submit.endpoint.empty()) return bad(reason::batches_request::file_unsupported);
    if (!submit.display_name.empty()) return bad(reason::batches_request::display_name_unsupported);
    // A callback is an https URL without credentials in it, checked as a media URL is.
    if (submit.extensions) {
      if (const auto callback = submit.extensions->root().get("callback_url"); callback.valid()) {
        const auto url = callback.is_string() ? callback.as_string() : std::string_view{};
        const auto authority = url.substr(0, url.find_first_of("/?#", 8));
        if (!url_syntax(url) || authority.find('@') != std::string_view::npos) return bad(reason::batches_request::callback_invalid);
      }
    }
    if (submit.items.empty()) return bad(reason::batches_request::items_required);
    const auto limit = descriptor.policy()->endpoint_limit("batches", descriptor.base_url(), "max_items");
    if (limit && submit.items.size() > *limit) return bad(reason::batches_request::items_limit);
    const auto item_path = std::find_if(items_fact->paths.begin(), items_fact->paths.end(), [&](const auto& entry) { return entry.first == descriptor.family(); });
    if (item_path == items_fact->paths.end()) return reason::error(ErrorKind::Unsupported, reason::batches_request::item_endpoint);
    std::set<std::string_view> seen;
    std::string requests;
    for (std::size_t i = 0; i < submit.items.size(); ++i) {
      const auto& item = submit.items[i];
      if (!custom_id_valid(item.custom_id) || !seen.insert(item.custom_id).second) return bad(reason::batches_request::custom_id_invalid);
      if (auto error = gateway_item(descriptor, *items_fact, item.request)) return *error;
      auto wire = item_wire(descriptor, item.request);
      if (auto* error = std::get_if<Error>(&wire)) return *error;
      auto& value = std::get<ItemWire>(wire);
      for (const auto& header : value.headers)
        if (lower(header.first) == "anthropic-beta") return reason::error(ErrorKind::Unsupported, reason::batches_request::item_beta);
      if (i && value.model != model) return bad(reason::batches_request::model_mismatch);
      model = value.model;
      requests.append(i ? "," : "").append("{\"custom_id\":").append(json::quote(item.custom_id)).append(",\"body\":").append(value.body).append("}");
      items.push_back({item.custom_id, value.model, value.max_output_tokens, value.model_invocation_limit});
    }
    body = "{\"endpoint\":" + json::quote(item_path->second) + ",\"model\":" + json::quote(model);
    if (submit.extensions) for (auto member : submit.extensions->root().members()) body += "," + json::quote(member.key) + ":" + member.value.dump();
    body += ",\"requests\":[" + requests + "]}";
    if (body.size() > descriptor.policy()->resources().request_bytes) return reason::error(ErrorKind::ResourceLimit, reason::batches_request::size_limit);
    Encoded encoded{std::move(path), std::move(body), std::move(model), fact->charge_on_accept, fact->auth};
    endpoints::add_fact_headers(encoded, *fact);
    return EncodedSubmit{std::move(encoded), std::move(items)};
  } else {
    if (auto error = check_items<gemini::Request>(descriptor, submit.items)) return *error;
    body = "{\"batch\":{";
    if (!submit.display_name.empty()) body += "\"display_name\":" + json::quote(submit.display_name) + ",";
    body += "\"input_config\":{\"requests\":{\"requests\":[";
    for (std::size_t i = 0; i < submit.items.size(); ++i) {
      auto wire = item_wire(descriptor, submit.items[i].request);
      if (auto* error = std::get_if<Error>(&wire)) return *error;
      auto& value = std::get<ItemWire>(wire);
      if (i && value.model != model) return bad(reason::batches_request::one_model);
      model = value.model;
      body.append(i ? "," : "").append("{\"request\":").append(value.body).append(",\"metadata\":{\"key\":").append(json::quote(submit.items[i].custom_id)).append("}}");
      items.push_back({submit.items[i].custom_id, value.model, value.max_output_tokens, value.model_invocation_limit});
    }
    body += "]}}}";
    // Google names the model in the path.
    if (!token(model)) return bad(reason::batches_request::model_invalid);
    // It takes the place of {model} through the check every path shares (descriptor::model_path).
    path = descriptor::model_path(path, model);
    if (path.empty()) return bad(reason::batches_request::model_path);
  }
  if (submit.extensions) for (auto member : submit.extensions->root().members()) body += "," + json::quote(member.key) + ":" + member.value.dump();
  body += "}";
  if (body.size() > descriptor.policy()->resources().request_bytes) return reason::error(ErrorKind::ResourceLimit, reason::batches_request::size_limit);
  Encoded encoded{std::move(path), std::move(body), std::move(model), fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  return EncodedSubmit{std::move(encoded), std::move(items)};
}
}  // namespace detail

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Get& get) {
  return by_id(descriptor, get.id, operation(get), "GET");
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Cancel& cancel) {
  // A batch endpoint without a cancel (OpenRouter's) is refused for that reason, before the id is read.
  if (const auto* fact = descriptor.policy()->endpoint("batches", descriptor.base_url()); fact && !descriptor::endpoint_path(*fact, operation(cancel)))
    return reason::error(ErrorKind::Unsupported, reason::batches_request::cancel_unsupported);
  return by_id(descriptor, cancel.id, operation(cancel), "POST");
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const List& list) {
  auto found = route(descriptor, "batches", operation(list));
  if (auto* error = std::get_if<Error>(&found)) return *error;
  auto& [fact, path] = std::get<Route>(found);
  if (list.limit && !*list.limit) return bad(reason::batches_request::limit_invalid);
  if (!list.after.empty() && !token(list.after)) return bad(reason::batches_request::cursor_invalid);
  std::string query;
  if (list.limit) query += "limit=" + std::to_string(*list.limit);
  if (!list.after.empty()) query += std::string(query.empty() ? "" : "&") + "after=" + list.after;
  if (!query.empty()) path += "?" + query;
  Encoded encoded{std::move(path), {}, {}, false, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.method = "GET";
  encoded.content_type.clear();
  return encoded;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Delete& del) {
  return by_id(descriptor, del.id, operation(del), "DELETE");
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Results& results) {
  if (!results.max_bytes) return bad(reason::batches_request::download_limit_invalid);
  const auto& batch = results.batch;
  if (batch.origin != descriptor.base_url()) return bad(reason::batches_request::origin_mismatch);
  const auto target = results_operation(results);
  if (serves_results(batch)) {
    if (results.errors) return reason::error(ErrorKind::Unsupported, reason::batches_request::errors_unsupported);
    auto found = by_id(descriptor, batch.id, target.second, "GET");
    if (auto* error = std::get_if<Error>(&found)) return *error;
    auto& encoded = std::get<Encoded>(found);
    encoded.accept = "*/*";
    encoded.sink = results.path;
    encoded.sink_limit = results.max_bytes;
    return encoded;
  }
  // OpenAI's output and error files, and Google's responses file, are files of the origin.
  const auto& file = results.errors ? batch.error_file : batch.output_file;
  if (results.errors && batch.shape != "openai") return reason::error(ErrorKind::Unsupported, reason::batches_request::errors_unsupported);
  if (file.empty()) return bad(reason::batches_request::results_missing);
  const auto* fact = descriptor.policy()->endpoint(target.first, descriptor.base_url());
  if (!fact) return reason::error(ErrorKind::Unsupported, reason::batches_request::endpoint_not_admitted);
  const auto content = descriptor::endpoint_path(*fact, target.second);
  if (!content) return reason::error(ErrorKind::Unsupported, reason::batches_request::path_missing);
  if (fact->shape == "google" ? !(file.starts_with("files/") && token(file.substr(6))) : !token(file)) return bad(reason::batches_request::results_file_invalid);
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
