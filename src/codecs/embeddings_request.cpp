#include "codecs/embeddings.h"
#include "codecs/reason.h"
#include "codecs/media_input.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <algorithm>
#include <utility>

namespace sp::embeddings {
namespace {
Error bad(const reason::Reason& message) { return reason::error(ErrorKind::InvalidRequest, message); }
bool token(std::string_view value, bool upper_case = true) {
  return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [&](char c) {
    return (c >= 'a' && c <= 'z') || (upper_case && c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}
}  // namespace

std::string_view operation(const Request&) noexcept { return "embed"; }
EndpointFacts endpoint_facts(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  auto facts = endpoints::call_facts(descriptor, "embeddings", operation(request), request.model);
  facts.dimensions = endpoints::dimensions(operation(request), {{"dimensions", endpoints::decimal(request.dimensions)}, {"task", request.task}}, request.extensions);
  facts.max_price = endpoints::max_price(request.extensions);
  const std::uint64_t inputs = request.inputs.size();
  facts.quantities.push_back(endpoints::input_limited("input", inputs));
  if (std::any_of(request.inputs.begin(), request.inputs.end(), [](const Input& input) { return !input.media.empty(); }))
    facts.quantities.push_back(endpoints::input_limited("input_image", inputs));
  return facts;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("embeddings", descriptor.base_url());
  if (!fact) return reason::error(ErrorKind::Unsupported, reason::embeddings_request::endpoint_not_admitted);
  const auto path = descriptor::endpoint_path(*fact, operation(request));
  if (!path) return reason::error(ErrorKind::Unsupported, reason::embeddings_request::path_missing);
  const bool google = fact->shape == "google", openai = fact->shape == "openai";
  if (request.model.empty() || request.inputs.empty()) return bad(reason::embeddings_request::model_and_inputs_required);
  // Google names the model in the path.
  if (google && !token(request.model)) return bad(reason::embeddings_request::model_segment_invalid);
  // A model takes the place of {model} in the path through the check every path shares (descriptor::model_path).
  auto resolved = descriptor::model_path(*path, request.model);
  if (resolved.empty()) return bad(reason::embeddings_request::model_path);
  if (request.dimensions && !*request.dimensions) return bad(reason::embeddings_request::dimensions_invalid);
  if (request.task && (openai || !token(*request.task))) return openai ? reason::error(ErrorKind::Unsupported, reason::embeddings_request::task_unsupported) : bad(reason::embeddings_request::task_invalid);
  bool media = false;
  for (const auto& input : request.inputs) {
    if (input.text.empty() && input.media.empty()) return bad(reason::embeddings_request::input_empty);
    for (const auto& item : input.media) {
      media = true;
      if (openai) return reason::error(ErrorKind::Unsupported, reason::embeddings_request::media_unsupported);
      if (!google && item.kind != MediaKind::Image) return reason::error(ErrorKind::Unsupported, reason::embeddings_request::media_kind_unsupported);
      if (google && item.source == MediaSource::Url) return reason::error(ErrorKind::Unsupported, reason::embeddings_request::media_source_unsupported);
      if (item.source != MediaSource::File) { if (auto error = media_input::admit(descriptor, item)) return *error; }
      else if (item.reference.empty()) return bad(reason::embeddings_request::media_reference_missing);
    }
  }
  if (request.extensions) {
    if (!fact->options) return reason::error(ErrorKind::Unsupported, reason::embeddings_request::options_not_admitted);
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad(reason::embeddings_request::options_invalid);
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
  if (!out.ok()) return reason::error(ErrorKind::ResourceLimit, reason::embeddings_request::json_limit);
  Encoded encoded{std::move(resolved), std::move(body), request.model, fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.context.inputs = request.inputs.size();
  return encoded;
}
}  // namespace sp::embeddings
