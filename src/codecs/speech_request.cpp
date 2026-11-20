#include "codecs/speech.h"
#include "codecs/reason.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace sp::speech {
namespace {
Error bad(const reason::Reason& message) { return reason::error(ErrorKind::InvalidRequest, message); }
bool token(std::string_view value) {
  return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}
}  // namespace

std::string_view operation(const Request&) noexcept { return "speak"; }
EndpointFacts endpoint_facts(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  auto facts = endpoints::call_facts(descriptor, "speech", operation(request), request.model);
  std::optional<std::string> voice;
  if (!request.voice.empty()) voice = request.voice;
  facts.dimensions = endpoints::dimensions(operation(request), {{"format", request.format}, {"speed", endpoints::unstated(request.speed.has_value())}, {"voice", voice}},
                                           request.extensions);
  facts.quantities.push_back(endpoints::requested("item:character", request.input.size(), true));
  facts.quantities.push_back(endpoints::input_limited("input"));
  facts.quantities.push_back(endpoints::output_limited());
  return facts;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("speech", descriptor.base_url());
  if (!fact) return reason::error(ErrorKind::Unsupported, reason::speech_request::endpoint_not_admitted);
  const auto path = descriptor::endpoint_path(*fact, operation(request));
  if (!path) return reason::error(ErrorKind::Unsupported, reason::speech_request::path_missing);
  if (request.model.empty() || request.input.empty()) return bad(reason::speech_request::model_and_input_required);
  if (!request.voice.empty() && request.voice.size() > 256) return bad(reason::speech_request::voice_invalid);
  if (request.format && !token(*request.format)) return bad(reason::speech_request::format_invalid);
  if (request.speed && (!std::isfinite(*request.speed) || *request.speed <= 0)) return bad(reason::speech_request::speed_invalid);
  if (request.extensions) {
    if (!fact->options) return reason::error(ErrorKind::Unsupported, reason::speech_request::options_not_admitted);
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad(reason::speech_request::options_invalid);
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
  if (!out.ok()) return reason::error(ErrorKind::ResourceLimit, reason::speech_request::json_limit);
  Encoded encoded{std::string(*path), std::move(body), request.model, fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.accept = "audio/*";
  return encoded;
}
}  // namespace sp::speech
