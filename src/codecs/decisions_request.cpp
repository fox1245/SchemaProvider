#include "codecs/decisions.h"
#include "codecs/reason.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <set>
#include <utility>

namespace sp::decisions {
namespace {
Error bad(const reason::Reason& message) { return reason::error(ErrorKind::InvalidRequest, message); }
// Guidance is a string; OpenRouter also takes a JSON object or array.
bool guidance(const std::shared_ptr<const json::Document>& value, bool structured) {
  if (!value) return false;
  const auto root = value->root();
  return root.is_string() || (structured && (root.is_object() || root.is_array()));
}
std::string_view kind_name(Kind kind, bool openai) {
  switch (kind) {
    case Kind::Predicate: return openai ? "predicate" : "noul";
    case Kind::Choice: return "choice";
    case Kind::Score: return "score";
    case Kind::Box: return "box";
  }
  return {};
}
}  // namespace

std::string_view operation(const Request&) noexcept { return "decide"; }
EndpointFacts endpoint_facts(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  auto facts = endpoints::call_facts(descriptor, "decisions", operation(request), request.model);
  facts.dimensions = endpoints::dimensions(operation(request), {}, request.extensions);
  facts.quantities.push_back(endpoints::input_limited("input"));
  facts.quantities.push_back(endpoints::output_limited());
  return facts;
}
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("decisions", descriptor.base_url());
  if (!fact) return reason::error(ErrorKind::Unsupported, reason::decisions_request::endpoint_not_admitted);
  const bool openai = fact->shape == "openai";
  const auto path = descriptor::endpoint_path(*fact, operation(request));
  if (!path) return reason::error(ErrorKind::Unsupported, reason::decisions_request::path_missing);
  if (request.model.empty() || request.questions.empty()) return bad(reason::decisions_request::model_and_questions_required);
  // OpenAI takes a string or message items; OpenRouter a string, an object or an array.
  const auto input = request.input ? request.input->root() : json::Value{};
  if (!input.is_string() && !input.is_array() && (openai || !input.is_object())) return bad(reason::decisions_request::input_invalid);
  std::set<std::string_view> names;
  for (const auto& question : request.questions) {
    if ((!openai && question.name.empty()) || (!question.name.empty() && !names.insert(question.name).second))
      return bad(reason::decisions_request::names_invalid);
    if (!guidance(question.instructions, !openai)) return bad(reason::decisions_request::guidance_invalid);
    if (openai && question.kind == Kind::Box) return reason::error(ErrorKind::Unsupported, reason::decisions_request::box_unsupported);
    const bool listed = question.kind == Kind::Choice || question.kind == Kind::Score;
    if (listed != (question.options.size() >= 2) || (!listed && !question.options.empty()))
      return bad(reason::decisions_request::options_count_invalid);
    std::set<std::string_view> values;
    for (const auto& option : question.options) {
      if (option.value.empty() || (option.guidance && !guidance(option.guidance, !openai)))
        return bad(reason::decisions_request::option_invalid);
      // An answer names its option by value, and OpenRouter keys a choice's criteria by it.
      if (!values.insert(option.value).second) return bad(reason::decisions_request::option_values_duplicate);
    }
    if (bool(question.when_true) != bool(question.when_false) || (question.when_true && question.kind != Kind::Predicate))
      return bad(reason::decisions_request::predicate_guidance_invalid);
    if (question.when_true && openai) return reason::error(ErrorKind::Unsupported, reason::decisions_request::predicate_guidance_unsupported);
    if (question.when_true && (!guidance(question.when_true, true) || !guidance(question.when_false, true)))
      return bad(reason::decisions_request::guidance_invalid);
  }
  if (request.extensions) {
    if (!fact->options) return reason::error(ErrorKind::Unsupported, reason::decisions_request::options_not_admitted);
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad(reason::decisions_request::options_schema_invalid);
  }
  const auto& resources = descriptor.policy()->resources();
  std::string body;
  json::BoundedWriter out({resources.request_bytes, resources.json_depth}, &body);
  out.raw("{\"model\":").quoted(request.model).raw(openai ? ",\"input\":" : ",\"state\":").value(input, 1);
  out.raw(openai ? ",\"questions\":[" : ",\"questions\":{");
  bool comma = false;
  for (const auto& question : request.questions) {
    if (std::exchange(comma, true)) out.raw(",");
    if (!openai) out.quoted(question.name).raw(":");
    out.raw("{\"type\":").quoted(kind_name(question.kind, openai));
    if (openai && !question.name.empty()) out.raw(",\"name\":").quoted(question.name);
    out.raw(",\"instructions\":").value(question.instructions->root(), 3);
    if (openai && question.kind == Kind::Choice) {
      out.raw(",\"choices\":[");
      for (std::size_t i = 0; i < question.options.size(); ++i) {
        const auto& option = question.options[i];
        out.raw(i ? ",{\"value\":" : "{\"value\":").quoted(option.value);
        if (option.guidance) out.raw(",\"description\":").value(option.guidance->root(), 5);
        out.raw("}");
      }
      out.raw("]");
    } else if (openai && question.kind == Kind::Score) {
      out.raw(",\"levels\":[");
      for (std::size_t i = 0; i < question.options.size(); ++i) {
        const auto& option = question.options[i];
        out.raw(i ? ",{\"label\":" : "{\"label\":").quoted(option.value);
        if (option.guidance) out.raw(",\"description\":").value(option.guidance->root(), 5);
        out.raw("}");
      }
      out.raw("]");
    } else if (question.kind == Kind::Choice) {
      // OpenRouter keys a choice's criteria by value; a value without guidance maps to null.
      out.raw(",\"criteria\":{");
      for (std::size_t i = 0; i < question.options.size(); ++i) {
        const auto& option = question.options[i];
        if (i) out.raw(",");
        out.quoted(option.value).raw(":");
        if (option.guidance) out.value(option.guidance->root(), 4); else out.raw("null");
      }
      out.raw("}");
    } else if (question.kind == Kind::Score) {
      // OpenRouter lists a score's levels as guidance, lowest first; a level without guidance is its label.
      out.raw(",\"criteria\":[");
      for (std::size_t i = 0; i < question.options.size(); ++i) {
        const auto& option = question.options[i];
        if (i) out.raw(",");
        if (option.guidance) out.value(option.guidance->root(), 4); else out.quoted(option.value);
      }
      out.raw("]");
    } else if (question.when_true) {
      out.raw(",\"criteria\":{\"true\":").value(question.when_true->root(), 4).raw(",\"false\":").value(question.when_false->root(), 4).raw("}");
    }
    out.raw("}");
  }
  out.raw(openai ? "]" : "}");
  if (request.extensions) for (auto member : request.extensions->root().members()) out.raw(",").quoted(member.key).raw(":").value(member.value, 1);
  out.raw("}");
  if (!out.ok()) return reason::error(ErrorKind::ResourceLimit, reason::decisions_request::json_limit);
  Encoded encoded{std::string(*path), std::move(body), request.model, fact->charge_on_accept, fact->auth};
  endpoints::add_fact_headers(encoded, *fact);
  return encoded;
}
}  // namespace sp::decisions
