#include "codecs/videos.h"
#include "codecs/media_input.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <algorithm>

namespace sp::videos {
namespace {
Error bad(std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; }
bool token(std::string_view value) {
  return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '.' || c == '_' || c == '-';
  });
}
}  // namespace

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("videos", descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "video endpoints are not admitted at this origin"};
  const auto path = descriptor::endpoint_path(*fact, "submit");
  if (!path) return Error{ErrorKind::Unsupported, "the video endpoint has no submit path"};
  if (request.model.empty() || request.prompt.empty()) return bad("a video needs a model and a prompt");
  for (const auto* value : {&request.aspect_ratio, &request.resolution, &request.size})
    if (*value && !token(**value)) return bad("video aspect ratio, resolution and size are tokens");
  if (request.duration && !*request.duration) return bad("a video duration must be positive");
  for (const auto& media : request.references) {
    if (media.kind != MediaKind::Image || media.source == MediaSource::File) return Error{ErrorKind::Unsupported, "video references are images inline or by URL"};
    if (auto error = media_input::admit(descriptor, media)) return *error;
  }
  if (request.extensions) {
    if (!fact->options) return Error{ErrorKind::Unsupported, "the video endpoint admits no options"};
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad("video options do not match the policy's schema");
  }
  const auto& resources = descriptor.policy()->resources();
  std::string body;
  json::BoundedWriter out({resources.request_bytes, resources.json_depth}, &body);
  out.raw("{\"model\":").quoted(request.model).raw(",\"prompt\":").quoted(request.prompt);
  if (request.duration) out.raw(",\"duration\":").raw(std::to_string(*request.duration));
  const std::pair<const char*, const std::optional<std::string>*> named[]{{"aspect_ratio", &request.aspect_ratio}, {"resolution", &request.resolution}, {"size", &request.size}};
  for (const auto& [key, value] : named) if (*value) out.raw(",").quoted(key).raw(":").quoted(**value);
  if (request.audio) out.raw(",\"generate_audio\":").raw(*request.audio ? "true" : "false");
  if (!request.references.empty()) {
    out.raw(",\"input_references\":[");
    for (std::size_t i = 0; i < request.references.size(); ++i) {
      const auto& media = request.references[i];
      out.raw(i ? ",{\"type\":\"image_url\",\"image_url\":{\"url\":" : "{\"type\":\"image_url\",\"image_url\":{\"url\":");
      if (media.source == MediaSource::Inline) out.raw("\"data:").raw(media.mime).raw(";base64,").raw(*media.data).raw("\"");
      else out.quoted(media.reference);
      out.raw("}}");
    }
    out.raw("]");
  }
  if (request.extensions) for (auto member : request.extensions->root().members()) out.raw(",").quoted(member.key).raw(":").value(member.value, 1);
  out.raw("}");
  if (!out.ok()) return Error{ErrorKind::ResourceLimit, "video request exceeds JSON limits"};
  return Encoded{std::string(*path), std::move(body), request.model, fact->charge_on_accept, fact->auth};
}
}  // namespace sp::videos
