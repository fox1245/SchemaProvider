#include "codecs/transcription.h"
#include "codecs/reason.h"
#include "codecs/media_input.h"
#include "crypto/crypto.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <algorithm>

namespace sp::transcription {
namespace {
Error bad(const reason::Reason& message) { return reason::error(ErrorKind::InvalidRequest, message); }
bool token(std::string_view value) {
  return !value.empty() && value.size() <= 32 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}
// One multipart/form-data field.
void field(std::string& body, std::string_view boundary, std::string_view name, std::string_view value) {
  body.append("--").append(boundary).append("\r\nContent-Disposition: form-data; name=\"").append(name).append("\"\r\n\r\n").append(value).append("\r\n");
}
}  // namespace

std::string_view operation(const Request& request) noexcept { return request.translate ? "translate" : "transcribe"; }
EndpointFacts endpoint_facts(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  auto facts = endpoints::call_facts(descriptor, "transcription", operation(request), request.model);
  facts.dimensions = endpoints::dimensions(operation(request), {{"language", request.language}}, request.extensions);
  auto seconds = endpoints::requested("item:second", std::nullopt);
  seconds.source = EndpointQuantity::Source::StatedSeconds;
  facts.quantities.push_back(std::move(seconds));
  facts.quantities.push_back(endpoints::input_limited("input"));
  facts.quantities.push_back(endpoints::output_limited());
  return facts;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("transcription", descriptor.base_url());
  if (!fact) return reason::error(ErrorKind::Unsupported, reason::transcription_request::endpoint_not_admitted);
  const auto path = descriptor::endpoint_path(*fact, operation(request));
  if (!path) return request.translate ? reason::error(ErrorKind::Unsupported, reason::transcription_request::translate_unsupported)
                                      : reason::error(ErrorKind::Unsupported, reason::transcription_request::path_missing);
  if (request.translate && request.language) return bad(reason::transcription_request::translate_language);
  const bool openai = fact->shape == "openai";
  if (request.model.empty()) return bad(reason::transcription_request::model_required);
  const auto& audio = request.audio;
  if (audio.kind != MediaKind::Audio) return bad(reason::transcription_request::audio_required);
  if (audio.source == MediaSource::File || (openai && audio.source == MediaSource::Url))
    return reason::error(ErrorKind::Unsupported, reason::transcription_request::audio_source_unsupported);
  // A fact that lists the audio it takes admits by its own list and size; otherwise the family's media policy does.
  const auto* listed = fact->accepted_audio ? &*fact->accepted_audio : nullptr;
  if (audio.source == MediaSource::Inline) {
    if (auto error = listed ? media_input::admit_audio(descriptor, audio, *listed) : media_input::admit(descriptor, audio)) return *error;
  }
  const auto format = listed ? media_input::audio_format(*listed, audio.mime) : media_input::audio_format(audio.mime);
  if (!format && (!openai || audio.mime.empty())) return reason::error(ErrorKind::Unsupported, reason::transcription_request::audio_format_unknown);
  if (request.language && !token(*request.language)) return bad(reason::transcription_request::language_invalid);
  if (request.prompt && !openai) return reason::error(ErrorKind::Unsupported, reason::transcription_request::prompt_unsupported);
  if (request.extensions) {
    if (!fact->options) return reason::error(ErrorKind::Unsupported, reason::transcription_request::options_not_admitted);
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad(reason::transcription_request::options_invalid);
  }
  const auto& resources = descriptor.policy()->resources();
  // The request limit holds what surrounds the audio when the fact states the audio's own size limit.
  const auto room = [&](std::size_t audio_bytes) {
    return resources.request_bytes + (listed && listed->max_bytes && audio.source == MediaSource::Inline ? audio_bytes : 0);
  };
  Encoded encoded{std::string(*path), {}, request.model, fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  if (openai) {
    // A multipart upload: the audio's bytes, the model and the controls as form fields.
    std::string bytes;
    if (!base64_decode(*audio.data, bytes)) return bad(reason::transcription_request::audio_not_base64);
    unsigned char random[12];
    if (!crypto::random_bytes(random, sizeof random)) return reason::error(ErrorKind::ResourceLimit, reason::transcription_request::boundary_unavailable);
    std::string boundary = "sp-boundary-";
    for (const auto byte : random) { boundary += "0123456789abcdef"[byte >> 4]; boundary += "0123456789abcdef"[byte & 15]; }
    auto& body = encoded.body;
    // JSON unless the request asks for another form. Timestamps come only in `verbose_json`, so asking for them
    // asks for it, and a request that names another form with them is refused.
    std::string_view response_format = "json";
    if (request.extensions) {
      const auto chosen = request.extensions->root().get("response_format");
      const bool timestamps = request.extensions->root().get("timestamp_granularities").valid();
      if (chosen.valid() && !chosen.is_string()) return bad(reason::transcription_request::options_invalid);
      if (chosen.valid()) response_format = chosen.as_string();
      else if (timestamps) response_format = "verbose_json";
      if (timestamps && response_format != "verbose_json") return bad(reason::transcription_request::timestamps_need_verbose);
    }
    field(body, boundary, "model", request.model);
    field(body, boundary, "response_format", response_format);
    if (request.language) field(body, boundary, "language", *request.language);
    if (request.prompt) field(body, boundary, "prompt", *request.prompt);
    if (request.extensions)
      for (auto member : request.extensions->root().members()) {
        if (member.key == "response_format") continue;
        // A scalar is one field; an array repeats the field as `name[]` for each item.
        const auto text = [](json::Value value) { return value.is_string() ? std::string(value.as_string()) : value.dump(); };
        if (member.value.is_array()) { for (auto item : member.value.elements()) field(body, boundary, std::string(member.key) + "[]", text(item)); }
        else field(body, boundary, member.key, text(member.value));
      }
    body.append("--").append(boundary).append("\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.")
        .append(format ? *format : std::string_view("bin")).append("\"\r\nContent-Type: ").append(audio.mime).append("\r\n\r\n").append(bytes).append("\r\n");
    body.append("--").append(boundary).append("--\r\n");
    if (body.size() > room(bytes.size())) return reason::error(ErrorKind::ResourceLimit, reason::transcription_request::size_limit);
    encoded.content_type = "multipart/form-data; boundary=" + boundary;
    return encoded;
  }
  json::BoundedWriter out({room(audio.data ? audio.data->size() : 0), resources.json_depth}, &encoded.body);
  out.raw("{\"model\":").quoted(request.model).raw(",\"input_audio\":{");
  if (audio.source == MediaSource::Inline) out.raw("\"data\":\"").raw(*audio.data).raw("\"");
  else out.raw("\"url\":").quoted(audio.reference);
  if (format) out.raw(",\"format\":").quoted(*format);
  out.raw("}");
  if (request.language) out.raw(",\"language\":").quoted(*request.language);
  if (request.extensions) for (auto member : request.extensions->root().members()) out.raw(",").quoted(member.key).raw(":").value(member.value, 1);
  out.raw("}");
  if (!out.ok()) return reason::error(ErrorKind::ResourceLimit, reason::transcription_request::size_limit);
  return encoded;
}
}  // namespace sp::transcription
