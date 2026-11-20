#include "codecs/moderation.h"
#include "codecs/reason.h"
#include "codecs/media_input.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <algorithm>
#include <utility>

namespace sp::moderation {
namespace {
Error bad(const reason::Reason& message) { return reason::error(ErrorKind::InvalidRequest, message); }
}  // namespace

std::string_view operation(const Request&) noexcept { return "check"; }
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("moderation", descriptor.base_url());
  if (!fact) return reason::error(ErrorKind::Unsupported, reason::moderation_request::endpoint_not_admitted);
  const auto path = descriptor::endpoint_path(*fact, operation(request));
  if (!path) return reason::error(ErrorKind::Unsupported, reason::moderation_request::path_missing);
  if (request.model.empty() || request.inputs.empty()) return bad(reason::moderation_request::model_and_inputs_required);
  bool media = false;
  for (const auto& input : request.inputs) {
    if (input.text.empty() && input.media.empty()) return bad(reason::moderation_request::input_empty);
    for (const auto& item : input.media) {
      media = true;
      if (item.kind != MediaKind::Image || item.source == MediaSource::File) return reason::error(ErrorKind::Unsupported, reason::moderation_request::media_unsupported);
      if (auto error = media_input::admit(descriptor, item)) return *error;
    }
  }
  if (request.extensions) {
    if (!fact->options) return reason::error(ErrorKind::Unsupported, reason::moderation_request::options_not_admitted);
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad(reason::moderation_request::options_invalid);
  }
  const auto& resources = descriptor.policy()->resources();
  std::string body;
  json::BoundedWriter out({resources.request_bytes, resources.json_depth}, &body);
  out.raw("{\"model\":").quoted(request.model).raw(",\"input\":[");
  bool comma = false;
  // Text alone goes as strings; with images, every input becomes typed parts of one list.
  for (const auto& input : request.inputs) {
    if (!media) { if (std::exchange(comma, true)) out.raw(","); out.quoted(input.text); continue; }
    if (!input.text.empty()) { if (std::exchange(comma, true)) out.raw(","); out.raw("{\"type\":\"text\",\"text\":").quoted(input.text).raw("}"); }
    for (const auto& item : input.media) {
      if (std::exchange(comma, true)) out.raw(",");
      out.raw("{\"type\":\"image_url\",\"image_url\":{\"url\":");
      if (item.source == MediaSource::Inline) media_input::data_uri(out, item);
      else out.quoted(item.reference);
      out.raw("}}");
    }
  }
  out.raw("]");
  if (request.extensions) for (auto member : request.extensions->root().members()) out.raw(",").quoted(member.key).raw(":").value(member.value, 1);
  out.raw("}");
  if (!out.ok()) return reason::error(ErrorKind::ResourceLimit, reason::moderation_request::json_limit);
  Encoded encoded{std::string(*path), std::move(body), request.model, fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  return encoded;
}
}  // namespace sp::moderation
