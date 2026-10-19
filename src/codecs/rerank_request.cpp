#include "codecs/rerank.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"

namespace sp::rerank {
namespace {
Error bad(std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; }
}  // namespace

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("rerank", descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "rerank endpoints are not admitted at this origin"};
  const auto path = descriptor::endpoint_path(*fact, "rank");
  if (!path) return Error{ErrorKind::Unsupported, "the rerank endpoint has no path"};
  if (request.model.empty() || request.query.empty() || request.documents.empty()) return bad("a rerank needs a model, a query and documents");
  if (request.top && !*request.top) return bad("a rerank top count must be positive");
  if (request.extensions) {
    if (!fact->options) return Error{ErrorKind::Unsupported, "the rerank endpoint admits no options"};
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad("rerank options do not match the policy's schema");
  }
  const auto& resources = descriptor.policy()->resources();
  std::string body;
  json::BoundedWriter out({resources.request_bytes, resources.json_depth}, &body);
  out.raw("{\"model\":").quoted(request.model).raw(",\"query\":").quoted(request.query).raw(",\"documents\":[");
  for (std::size_t i = 0; i < request.documents.size(); ++i) { if (i) out.raw(","); out.quoted(request.documents[i]); }
  out.raw("]");
  if (request.top) out.raw(",\"top_n\":").raw(std::to_string(*request.top));
  if (request.extensions) for (auto member : request.extensions->root().members()) out.raw(",").quoted(member.key).raw(":").value(member.value, 1);
  out.raw("}");
  if (!out.ok()) return Error{ErrorKind::ResourceLimit, "rerank request exceeds JSON limits"};
  return Encoded{std::string(*path), std::move(body), request.model, fact->charge_on_accept, fact->auth};
}
}  // namespace sp::rerank
