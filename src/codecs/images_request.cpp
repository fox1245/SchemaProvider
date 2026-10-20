#include "codecs/images.h"
#include "codecs/media_input.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <algorithm>

namespace sp::images {
namespace {
Error bad(std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; }
// A token the provider defines (`1024x1024`, `high`, `16:9`): bounded, printable, no separators.
bool token(std::string_view value) {
  return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '.' || c == '_' || c == '-';
  });
}
}  // namespace

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("images", descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "image endpoints are not admitted at this origin"};
  const bool openai = fact->shape == "openai";
  const bool edit = openai && !request.references.empty();
  const auto path = descriptor::endpoint_path(*fact, edit ? "edit" : "generate");
  if (!path) return Error{ErrorKind::Unsupported, "the image endpoint has no path for this operation"};
  if (request.model.empty() || request.prompt.empty()) return bad("image model and prompt are required");
  for (const auto* value : {&request.size, &request.quality, &request.background, &request.output_format})
    if (*value && !token(**value)) return bad("image size, quality, background and format are tokens");
  if (request.count && !*request.count) return bad("image count must be positive");
  if (request.output_compression && *request.output_compression > 100) return bad("image output compression is 0 to 100");
  for (const auto& media : request.references) {
    if (media.kind != MediaKind::Image) return bad("image references are images");
    if (media.source == MediaSource::File) {
      if (!openai) return Error{ErrorKind::Unsupported, "image references by file id are OpenAI only"};
      if (!token(media.reference)) return bad("an image file id is a token");
    } else if (auto error = media_input::admit(descriptor, media)) return *error;
  }
  if (request.extensions) {
    if (!fact->options) return Error{ErrorKind::Unsupported, "the image endpoint admits no options"};
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad("image options do not match the policy's schema");
  }
  const auto& resources = descriptor.policy()->resources();
  std::string body;
  json::BoundedWriter out({resources.request_bytes, resources.json_depth}, &body);
  out.raw("{\"model\":").quoted(request.model).raw(",\"prompt\":").quoted(request.prompt);
  if (request.count) out.raw(",\"n\":").raw(std::to_string(*request.count));
  const std::pair<const char*, const std::optional<std::string>*> named[]{
      {"size", &request.size}, {"quality", &request.quality}, {"background", &request.background}, {"output_format", &request.output_format}};
  for (const auto& [key, value] : named) if (*value) out.raw(",").quoted(key).raw(":").quoted(**value);
  if (request.output_compression) out.raw(",\"output_compression\":").raw(std::to_string(*request.output_compression));
  if (!request.references.empty()) {
    // OpenAI edits take `images`: a URL (an inline image as a data URL) or a file id. OpenRouter takes
    // `input_references` as content parts.
    out.raw(openai ? ",\"images\":[" : ",\"input_references\":[");
    for (std::size_t i = 0; i < request.references.size(); ++i) {
      const auto& media = request.references[i];
      if (i) out.raw(",");
      if (media.source == MediaSource::File) { out.raw("{\"file_id\":").quoted(media.reference).raw("}"); continue; }
      // An admitted inline image has a MIME type and canonical base64; data_uri writes them with no escaping and no whitespace.
      out.raw(openai ? "{\"image_url\":" : "{\"type\":\"image_url\",\"image_url\":{\"url\":");
      if (media.source == MediaSource::Inline) media_input::data_uri(out, media);
      else out.quoted(media.reference);
      out.raw(openai ? "}" : "}}");
    }
    out.raw("]");
  }
  if (request.extensions) for (auto member : request.extensions->root().members()) out.raw(",").quoted(member.key).raw(":").value(member.value, 1);
  out.raw("}");
  if (!out.ok()) return Error{ErrorKind::ResourceLimit, "image request exceeds JSON limits"};
  Encoded encoded{std::string(*path), std::move(body), request.model, fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  return encoded;
}

}  // namespace sp::images
