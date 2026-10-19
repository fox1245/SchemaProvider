#include "codecs/tokens.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>
#include <type_traits>

namespace sp::tokens {
namespace {
bool token(std::string_view value) {
  return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}
std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}
}  // namespace

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Count& count) {
  const auto* fact = descriptor.policy()->endpoint("tokens", descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "token counting is not admitted at this origin"};
  const auto path = descriptor::endpoint_path(*fact, "count");
  if (!path) return Error{ErrorKind::Unsupported, "the token count endpoint has no path"};
  // The family request the shape counts, encoded as a buffered call would send it.
  const auto& shape = fact->shape;
  const bool family_matches = (shape == "anthropic" && std::holds_alternative<messages::Request>(count.request)) ||
                              (shape == "openai" && std::holds_alternative<responses::Request>(count.request)) ||
                              (shape == "google" && std::holds_alternative<gemini::Request>(count.request));
  if (!family_matches) return Error{ErrorKind::Unsupported, "this origin counts the tokens of another family's request"};
  struct Wire { std::string body, model; std::vector<std::pair<std::string, std::string>> headers; };
  auto wire = std::visit([&](const auto& typed) -> std::variant<Wire, Error> {
    using T = std::decay_t<decltype(typed)>;
    auto encoded = [&] {
      if constexpr (std::is_same_v<T, chat::Request>) return chat::encode(descriptor, typed, false);
      else if constexpr (std::is_same_v<T, messages::Request>) return messages::encode(descriptor, typed, false);
      else if constexpr (std::is_same_v<T, responses::Request>) return responses::encode(descriptor, typed, false);
      else return gemini::encode(descriptor, typed, false);
    }();
    if (auto* error = std::get_if<Error>(&encoded)) return *error;
    auto& value = std::get<0>(encoded);
    return Wire{std::move(value.body), typed.model, std::move(value.headers)};
  }, count.request);
  if (auto* error = std::get_if<Error>(&wire)) return *error;
  auto& [family_body, model, family_headers] = std::get<Wire>(wire);
  auto parsed = json::parse(family_body);
  if (!std::holds_alternative<json::Document>(parsed) || !std::get<json::Document>(parsed).root().is_object())
    return Error{ErrorKind::InvalidRequest, "the request does not encode to a JSON object"};
  const auto root = std::get<json::Document>(parsed).root();
  // The members the count takes, in the family's order.
  std::string members;
  for (auto member : root.members()) {
    if (!fact->keep.empty() && std::find(fact->keep.begin(), fact->keep.end(), member.key) == fact->keep.end()) continue;
    if (shape == "google" && member.key == "model") continue;
    members.append(members.empty() ? "" : ",").append(json::quote(member.key)).append(":").append(member.value.dump());
  }
  std::string resolved(*path);
  std::string body;
  if (shape == "google") {
    // Google counts a whole generate request, which names its model.
    if (!token(model)) return Error{ErrorKind::InvalidRequest, "a Google model is a path segment"};
    if (const auto at = resolved.find("{model}"); at != std::string::npos) resolved.replace(at, 7, model);
    body = "{\"generateContentRequest\":{\"model\":" + json::quote("models/" + model) + (members.empty() ? "" : ",") + members + "}}";
  } else {
    body = "{" + members + "}";
  }
  if (body.size() > descriptor.policy()->resources().request_bytes) return Error{ErrorKind::ResourceLimit, "the count request exceeds the request limit"};
  Encoded encoded{std::move(resolved), std::move(body), std::move(model), fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  // A beta the request needs (an Anthropic MCP server) is needed to count it too.
  for (auto& [name, value] : family_headers)
    if (lower(name) == "anthropic-beta") encoded.headers.emplace_back(std::move(name), std::move(value));
  return encoded;
}
}  // namespace sp::tokens
