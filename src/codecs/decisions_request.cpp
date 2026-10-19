#include "codecs/decisions.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <set>
#include <utility>

namespace sp::decisions {
namespace {
Error bad(std::string message) { return Error{ErrorKind::InvalidRequest, std::move(message)}; }
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

std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request) {
  const auto* fact = descriptor.policy()->endpoint("decisions", descriptor.base_url());
  if (!fact) return Error{ErrorKind::Unsupported, "decision endpoints are not admitted at this origin"};
  const bool openai = fact->shape == "openai";
  const auto path = descriptor::endpoint_path(*fact, "decide");
  if (!path) return Error{ErrorKind::Unsupported, "the decision endpoint has no path"};
  if (request.model.empty() || request.questions.empty()) return bad("a decision needs a model and questions");
  // OpenAI takes a string or message items; OpenRouter a string, an object or an array.
  const auto input = request.input ? request.input->root() : json::Value{};
  if (!input.is_string() && !input.is_array() && (openai || !input.is_object())) return bad("decision input is a string, an array or (OpenRouter) an object");
  std::set<std::string_view> names;
  for (const auto& question : request.questions) {
    if ((!openai && question.name.empty()) || (!question.name.empty() && !names.insert(question.name).second))
      return bad("decision questions need unique names");
    if (!guidance(question.instructions, !openai)) return bad("decision instructions are a string or (OpenRouter) structured guidance");
    if (openai && question.kind == Kind::Box) return Error{ErrorKind::Unsupported, "box questions are OpenRouter only"};
    const bool listed = question.kind == Kind::Choice || question.kind == Kind::Score;
    if (listed != (question.options.size() >= 2) || (!listed && !question.options.empty()))
      return bad("choice and score questions list at least two options; others list none");
    for (const auto& option : question.options)
      if (option.value.empty() || (option.guidance && !guidance(option.guidance, !openai)))
        return bad("decision options have a value and string or (OpenRouter) structured guidance");
    if (bool(question.when_true) != bool(question.when_false) || (question.when_true && question.kind != Kind::Predicate))
      return bad("predicate guidance gives both a true and a false answer");
    if (question.when_true && openai) return Error{ErrorKind::Unsupported, "predicate guidance is OpenRouter only"};
    if (question.when_true && (!guidance(question.when_true, true) || !guidance(question.when_false, true)))
      return bad("decision instructions are a string or (OpenRouter) structured guidance");
  }
  if (request.extensions) {
    if (!fact->options) return Error{ErrorKind::Unsupported, "the decision endpoint admits no options"};
    if (!request.extensions->root().is_object() || descriptor::check_value(*fact->options, request.extensions->root()))
      return bad("decision options do not match the policy's schema");
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
  if (!out.ok()) return Error{ErrorKind::ResourceLimit, "decision request exceeds JSON limits"};
  return Encoded{std::string(*path), std::move(body), request.model, fact->charge_on_accept, fact->auth};
}
}  // namespace sp::decisions
