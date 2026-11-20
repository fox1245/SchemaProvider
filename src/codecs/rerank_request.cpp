#include "codecs/rerank.h"
#include "codecs/reason.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"

namespace sp::rerank {
namespace {
Error bad(const reason::Reason& message) { return reason::error(ErrorKind::InvalidRequest, message); }
}  // namespace

std::string_view operation(const Request&) noexcept { return "rank"; }
EndpointFacts endpoint_facts(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  auto facts = endpoints::call_facts(descriptor, "rerank", operation(request), request.model);
  facts.dimensions = endpoints::dimensions(operation(request), {{"top", endpoints::decimal(request.top)}}, request.extensions);
  facts.quantities.push_back(endpoints::requested("item:document", request.documents.size(), true));
  facts.quantities.push_back(endpoints::requested("item:search_unit", std::nullopt));
  facts.quantities.push_back(endpoints::input_limited("input", request.documents.size()));
  return facts;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("rerank", descriptor.base_url());
  if (!fact) return reason::error(ErrorKind::Unsupported, reason::rerank_request::endpoint_not_admitted);
  const auto path = descriptor::endpoint_path(*fact, operation(request));
  if (!path) return reason::error(ErrorKind::Unsupported, reason::rerank_request::path_missing);
  if (request.model.empty() || request.query.empty() || request.documents.empty()) return bad(reason::rerank_request::fields_required);
  if (request.top && !*request.top) return bad(reason::rerank_request::top_invalid);
  if (request.extensions) {
    if (!fact->options) return reason::error(ErrorKind::Unsupported, reason::rerank_request::options_not_admitted);
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad(reason::rerank_request::options_invalid);
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
  if (!out.ok()) return reason::error(ErrorKind::ResourceLimit, reason::rerank_request::json_limit);
  Encoded encoded{std::string(*path), std::move(body), request.model, fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  encoded.context.inputs = request.documents.size();
  encoded.context.results = request.top.value_or(0);
  return encoded;
}
}  // namespace sp::rerank
