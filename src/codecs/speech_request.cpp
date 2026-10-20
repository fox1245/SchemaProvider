#include "codecs/speech.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace sp::speech {
namespace {
Error bad(std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; }
bool token(std::string_view value) {
  return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}
}  // namespace

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("speech", descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "speech endpoints are not admitted at this origin"};
  const auto path = descriptor::endpoint_path(*fact, "speak");
  if (!path) return Error{ErrorKind::Unsupported, "the speech endpoint has no path"};
  if (request.model.empty() || request.input.empty()) return bad("speech needs a model and input");
  if (!request.voice.empty() && request.voice.size() > 256) return bad("a speech voice is bounded text");
  if (request.format && !token(*request.format)) return bad("a speech format is a token");
  if (request.speed && (!std::isfinite(*request.speed) || *request.speed <= 0)) return bad("speech speed must be positive");
  if (request.extensions) {
    if (!fact->options) return Error{ErrorKind::Unsupported, "the speech endpoint admits no options"};
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad("speech options do not match the policy's schema");
  }
  const auto& resources = descriptor.policy()->resources();
  std::string body;
  json::BoundedWriter out({resources.request_bytes, resources.json_depth}, &body);
  out.raw("{\"model\":").quoted(request.model).raw(",\"input\":").quoted(request.input);
  if (!request.voice.empty()) out.raw(",\"voice\":").quoted(request.voice);
  if (request.instructions) out.raw(",\"instructions\":").quoted(*request.instructions);
  if (request.format) out.raw(",\"response_format\":").quoted(*request.format);
  if (request.speed) {
    char text[32]; const auto written = std::to_chars(text, text + sizeof text, *request.speed);
    out.raw(",\"speed\":").raw(std::string_view(text, written.ptr - text));
  }
  if (request.extensions) for (auto member : request.extensions->root().members()) out.raw(",").quoted(member.key).raw(":").value(member.value, 1);
  out.raw("}");
  if (!out.ok()) return Error{ErrorKind::ResourceLimit, "speech request exceeds JSON limits"};
  Encoded encoded{std::string(*path), std::move(body), request.model, fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.accept = "audio/*";
  return encoded;
}
}  // namespace sp::speech
