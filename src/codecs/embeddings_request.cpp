#include "codecs/embeddings.h"
#include "codecs/media_input.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <algorithm>
#include <utility>

namespace sp::embeddings {
namespace {
Error bad(std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; }
bool token(std::string_view value, bool upper_case = true) {
  return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [&](char c) {
    return (c >= 'a' && c <= 'z') || (upper_case && c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}
}  // namespace

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("embeddings", descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "embedding endpoints are not admitted at this origin"};
  const auto path = descriptor::endpoint_path(*fact, "embed");
  if (!path) return Error{ErrorKind::Unsupported, "the embedding endpoint has no path"};
  const bool google = fact->shape == "google", openai = fact->shape == "openai";
  if (request.model.empty() || request.inputs.empty()) return bad("embeddings need a model and inputs");
  // Google names the model in the path.
  if (google && !token(request.model)) return bad("a Google embedding model is a path segment");
  if (request.dimensions && !*request.dimensions) return bad("embedding dimensions must be positive");
  if (request.task && (openai || !token(*request.task))) return openai ? Error{ErrorKind::Unsupported, "OpenAI embeddings take no task"} : bad("an embedding task is a token");
  bool media = false;
  for (const auto& input : request.inputs) {
    if (input.text.empty() && input.media.empty()) return bad("an embedding input has text or media");
    for (const auto& item : input.media) {
      media = true;
      if (openai) return Error{ErrorKind::Unsupported, "OpenAI embeddings take text only"};
      if (!google && item.kind != MediaKind::Image) return Error{ErrorKind::Unsupported, "OpenRouter embeddings take images as media"};
      if (google && item.source == MediaSource::Url) return Error{ErrorKind::Unsupported, "Google embeddings take inline or uploaded media"};
      if (item.source != MediaSource::File) { if (auto error = media_input::admit(descriptor, item)) return *error; }
      else if (item.reference.empty()) return bad("an embedding media file names its reference");
    }
  }
  if (request.extensions) {
    if (!fact->options) return Error{ErrorKind::Unsupported, "the embedding endpoint admits no options"};
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad("embedding options do not match the policy's schema");
  }
  const auto& resources = descriptor.policy()->resources();
  std::string body;
  json::BoundedWriter out({resources.request_bytes, resources.json_depth}, &body);
  if (google) {
    out.raw("{\"requests\":[");
    for (std::size_t i = 0; i < request.inputs.size(); ++i) {
      const auto& input = request.inputs[i];
      out.raw(i ? ",{\"model\":" : "{\"model\":").quoted("models/" + request.model).raw(",\"content\":{\"parts\":[");
      bool comma = false;
      if (!input.text.empty()) { out.raw("{\"text\":").quoted(input.text).raw("}"); comma = true; }
      for (const auto& item : input.media) {
        if (std::exchange(comma, true)) out.raw(",");
        if (item.source == MediaSource::File)
          out.raw("{\"file_data\":{\"mime_type\":").quoted(item.mime).raw(",\"file_uri\":").quoted(item.reference).raw("}}");
        else out.raw("{\"inline_data\":{\"mime_type\":").quoted(item.mime).raw(",\"data\":\"").raw(*item.data).raw("\"}}");
      }
      out.raw("]}");
      if (request.task) out.raw(",\"taskType\":").quoted(*request.task);
      if (request.dimensions) out.raw(",\"outputDimensionality\":").raw(std::to_string(*request.dimensions));
      out.raw("}");
    }
    out.raw("]");
  } else {
    out.raw("{\"model\":").quoted(request.model).raw(",\"input\":[");
    for (std::size_t i = 0; i < request.inputs.size(); ++i) {
      const auto& input = request.inputs[i];
      if (i) out.raw(",");
      if (!media) { out.quoted(input.text); continue; }
      // OpenRouter's multimodal input: one content list per input.
      out.raw("{\"content\":[");
      bool comma = false;
      if (!input.text.empty()) { out.raw("{\"type\":\"text\",\"text\":").quoted(input.text).raw("}"); comma = true; }
      for (const auto& item : input.media) {
        if (std::exchange(comma, true)) out.raw(",");
        out.raw("{\"type\":\"image_url\",\"image_url\":{\"url\":");
        if (item.source == MediaSource::Inline) media_input::data_uri(out, item);
        else out.quoted(item.reference);
        out.raw("}}");
      }
      out.raw("]}");
    }
    out.raw("],\"encoding_format\":\"float\"");
    if (request.dimensions) out.raw(",\"dimensions\":").raw(std::to_string(*request.dimensions));
    if (request.task) out.raw(",\"input_type\":").quoted(*request.task);
  }
  if (request.extensions) for (auto member : request.extensions->root().members()) out.raw(",").quoted(member.key).raw(":").value(member.value, 1);
  out.raw("}");
  if (!out.ok()) return Error{ErrorKind::ResourceLimit, "embedding request exceeds JSON limits"};
  std::string resolved(*path);
  if (const auto at = resolved.find("{model}"); at != std::string::npos) resolved.replace(at, 7, request.model);
  return Encoded{std::move(resolved), std::move(body), request.model, fact->charge_on_accept, fact->auth};
}
}  // namespace sp::embeddings
