#include "codecs/transcription.h"
#include "codecs/media_input.h"
#include "crypto/crypto.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <algorithm>

namespace sp::transcription {
namespace {
Error bad(std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; }
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

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("transcription", descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "transcription endpoints are not admitted at this origin"};
  const auto path = descriptor::endpoint_path(*fact, request.translate ? "translate" : "transcribe");
  if (!path) return request.translate ? Error{ErrorKind::Unsupported, "this origin does not translate speech"}
                                      : Error{ErrorKind::Unsupported, "the transcription endpoint has no path"};
  if (request.translate && request.language) return bad("a translation takes no language");
  const bool openai = fact->shape == "openai";
  if (request.model.empty()) return bad("a transcription needs a model");
  const auto& audio = request.audio;
  if (audio.kind != MediaKind::Audio) return bad("a transcription takes audio");
  if (audio.source == MediaSource::File || (openai && audio.source == MediaSource::Url))
    return Error{ErrorKind::Unsupported, "this endpoint takes the audio inline or (OpenRouter) by URL"};
  if (audio.source == MediaSource::Inline) { if (auto error = media_input::admit(descriptor, audio)) return *error; }
  const auto format = media_input::audio_format(audio.mime);
  if (!format && (!openai || audio.mime.empty())) return Error{ErrorKind::Unsupported, "the audio type has no format name"};
  if (request.language && !token(*request.language)) return bad("a transcription language is a token");
  if (request.prompt && !openai) return Error{ErrorKind::Unsupported, "a transcription prompt is OpenAI only"};
  if (request.extensions) {
    if (!fact->options) return Error{ErrorKind::Unsupported, "the transcription endpoint admits no options"};
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad("transcription options do not match the policy's schema");
  }
  const auto& resources = descriptor.policy()->resources();
  Encoded encoded{std::string(*path), {}, request.model, fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  if (openai) {
    // A multipart upload: the audio's bytes, the model and the controls as form fields.
    std::string bytes;
    if (!base64_decode(*audio.data, bytes)) return bad("the audio is not base64");
    unsigned char random[12];
    if (!crypto::random_bytes(random, sizeof random)) return Error{ErrorKind::ResourceLimit, "no random boundary for the upload"};
    std::string boundary = "sp-boundary-";
    for (const auto byte : random) { boundary += "0123456789abcdef"[byte >> 4]; boundary += "0123456789abcdef"[byte & 15]; }
    auto& body = encoded.body;
    field(body, boundary, "model", request.model);
    field(body, boundary, "response_format", "json");
    if (request.language) field(body, boundary, "language", *request.language);
    if (request.prompt) field(body, boundary, "prompt", *request.prompt);
    if (request.extensions)
      for (auto member : request.extensions->root().members()) {
        // A scalar is one field; an array repeats the field as `name[]` for each item.
        const auto text = [](json::Value value) { return value.is_string() ? std::string(value.as_string()) : value.dump(); };
        if (member.value.is_array()) { for (auto item : member.value.elements()) field(body, boundary, std::string(member.key) + "[]", text(item)); }
        else field(body, boundary, member.key, text(member.value));
      }
    body.append("--").append(boundary).append("\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.")
        .append(format ? *format : std::string_view("bin")).append("\"\r\nContent-Type: ").append(audio.mime).append("\r\n\r\n").append(bytes).append("\r\n");
    body.append("--").append(boundary).append("--\r\n");
    if (body.size() > resources.request_bytes) return Error{ErrorKind::ResourceLimit, "transcription request exceeds the size limit"};
    encoded.content_type = "multipart/form-data; boundary=" + boundary;
    return encoded;
  }
  json::BoundedWriter out({resources.request_bytes, resources.json_depth}, &encoded.body);
  out.raw("{\"model\":").quoted(request.model).raw(",\"input_audio\":{");
  if (audio.source == MediaSource::Inline) out.raw("\"data\":\"").raw(*audio.data).raw("\"");
  else out.raw("\"url\":").quoted(audio.reference);
  if (format) out.raw(",\"format\":").quoted(*format);
  out.raw("}");
  if (request.language) out.raw(",\"language\":").quoted(*request.language);
  if (request.extensions) for (auto member : request.extensions->root().members()) out.raw(",").quoted(member.key).raw(":").value(member.value, 1);
  out.raw("}");
  if (!out.ok()) return Error{ErrorKind::ResourceLimit, "transcription request exceeds the size limit"};
  return encoded;
}
}  // namespace sp::transcription
