// Decision endpoints (D6): the request each provider shape takes and the typed answers of a response.
#include "codecs/decisions.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
descriptor::ValidatedDescriptor at(std::string_view origin) {
  const std::string path = origin == "https://openrouter.ai" ? "/api/v1/chat/completions" : "/v1/chat/completions";
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"decisions-fixture","family":"openai.chat","connection":{"base_url":)" +
                                 json::quote(origin) + R"(,"paths":{"buffered":)" + json::quote(path) + R"(,"streaming":)" + json::quote(path) + "}}}");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::shared_ptr<const json::Document> document(std::string_view text) {
  auto parsed = json::parse(text);
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
std::shared_ptr<const json::Document> text(std::string_view value) { return document(json::quote(value)); }
decisions::Encoded encoded(const descriptor::ValidatedDescriptor& d, const decisions::Request& r) {
  auto result = decisions::encode(d, r);
  if (const auto* error = std::get_if<Error>(&result)) throw std::runtime_error("encode refused: " + error->safe_message);
  return std::get<decisions::Encoded>(std::move(result));
}
ErrorKind refused(const descriptor::ValidatedDescriptor& d, const decisions::Request& r) {
  auto result = decisions::encode(d, r);
  CHECK(std::holds_alternative<Error>(result));
  return std::get<Error>(result).kind;
}
decisions::Outcome decoded(const descriptor::ValidatedDescriptor& d, std::string_view body, std::string model) {
  Accumulator accumulator;
  decisions::Codec codec(d, accumulator, std::move(model));
  codec.buffered(body, {});
  CHECK(accumulator.outcome());
  return decisions::typed(*accumulator.outcome());
}
decisions::Request ticket() {
  decisions::Request request{"gpt-6-luna", text("The checkout page charged me twice and I need a refund today.")};
  request.questions = {
      {"is_bug", decisions::Kind::Predicate, text("Is the customer reporting a software defect?")},
      {"team", decisions::Kind::Choice, text("Which team should own this ticket?"),
       {{"payments", text("Checkout, billing, or payment issues.")}, {"frontend", text("Rendering or layout issues.")}, {"account", {}}}},
      {"urgency", decisions::Kind::Score, text("How urgent is it?"), {{"low"}, {"medium"}, {"high", text("Blocking revenue right now")}}}};
  return request;
}

void requests() {
  const auto openai = at("https://api.openai.com"), router = at("https://openrouter.ai");
  const auto request = ticket();
  const auto direct = encoded(openai, request);
  CHECK(direct.path == "/v1/decisions" && direct.charge_on_accept && direct.model == "gpt-6-luna");
  {
    const auto body = document(direct.body);
    const auto root = body->root();
    const auto questions = root.get("questions");
    CHECK(root.get("input").is_string() && !root.get("state").valid() && questions.size() == 3);
    CHECK(questions.at(0).get("type").as_string() == "predicate" && questions.at(0).get("name").as_string() == "is_bug");
    CHECK(questions.at(1).get("choices").size() == 3 && questions.at(1).get("choices").at(0).get("value").as_string() == "payments" &&
          questions.at(1).get("choices").at(0).get("description").as_string() == "Checkout, billing, or payment issues." &&
          !questions.at(1).get("choices").at(2).get("description").valid());
    CHECK(questions.at(2).get("levels").at(2).get("label").as_string() == "high" && questions.at(2).get("levels").at(2).get("description").valid());
  }
  // OpenRouter keys questions by name, calls a predicate `noul`, maps choices to guidance and lists score levels.
  auto routed = request; routed.model = "typesafe/jev-1.13";
  routed.input = document(R"({"ticket":"The checkout page charged me twice."})");
  routed.questions[0].when_true = text("Broken or unexpected behaviour."); routed.questions[0].when_false = text("A question or a feature request.");
  routed.extensions = document(R"({"session_id":"s-1","provider":{"order":["typesafe"]}})");
  const auto via = encoded(router, routed);
  CHECK(via.path == "/api/alpha/decisions");
  {
    const auto body = document(via.body);
    const auto root = body->root();
    const auto questions = root.get("questions");
    CHECK(root.get("state").get("ticket").is_string() && questions.is_object() && questions.get("is_bug").get("type").as_string() == "noul" &&
          questions.get("is_bug").get("criteria").get("true").as_string() == "Broken or unexpected behaviour.");
    CHECK(questions.get("team").get("criteria").get("payments").is_string() && questions.get("team").get("criteria").get("account").is_null());
    CHECK(questions.get("urgency").get("criteria").size() == 3 && questions.get("urgency").get("criteria").at(0).as_string() == "low" &&
          questions.get("urgency").get("criteria").at(2).as_string() == "Blocking revenue right now");
    CHECK(root.get("session_id").as_string() == "s-1" && root.get("provider").get("order").at(0).as_string() == "typesafe");
  }
  // Refusals before any I/O.
  CHECK(refused(at("http://127.0.0.1:18080"), request) == ErrorKind::Unsupported);
  auto change = [&](const descriptor::ValidatedDescriptor& d, auto edit, ErrorKind kind) { auto r = request; edit(r); CHECK(refused(d, r) == kind); };
  change(openai, [](auto& r) { r.questions.clear(); }, ErrorKind::InvalidRequest);
  change(openai, [](auto& r) { r.input = document(R"({"x":1})"); }, ErrorKind::InvalidRequest);
  change(openai, [](auto& r) { r.questions[1].name = "is_bug"; }, ErrorKind::InvalidRequest);
  change(openai, [](auto& r) { r.questions[1].options.resize(1); }, ErrorKind::InvalidRequest);
  change(openai, [](auto& r) { r.questions[0].instructions = document(R"({"structured":true})"); }, ErrorKind::InvalidRequest);
  change(openai, [](auto& r) { r.questions[0].kind = decisions::Kind::Box; }, ErrorKind::Unsupported);
  change(openai, [](auto& r) { r.questions[0].when_true = text("x"); r.questions[0].when_false = text("y"); }, ErrorKind::Unsupported);
  change(router, [](auto& r) { r.questions[0].name.clear(); }, ErrorKind::InvalidRequest);
  change(router, [](auto& r) { r.questions[0].when_true = text("only one"); }, ErrorKind::InvalidRequest);
  change(router, [](auto& r) { r.extensions = document(R"({"questions":{}})"); }, ErrorKind::InvalidRequest);
  auto box = request; box.questions = {{"where", decisions::Kind::Box, text("Where is the error dialog?")}};
  CHECK(document(encoded(router, box).body)->root().get("questions").get("where").get("type").as_string() == "box");
}

void answers() {
  const auto openai = at("https://api.openai.com"), router = at("https://openrouter.ai");
  const auto direct = decoded(openai, R"({"model":"gpt-6-luna","answers":[
      {"type":"predicate","name":"is_bug","probability":0.93},
      {"type":"choice","name":"team","choice":"payments","probabilities":[{"value":"payments","probability":0.88},{"value":"frontend","probability":0.12}],"confidence":0.88},
      {"type":"score","name":"urgency","score":1.9,"probabilities":[{"value":0,"label":"low","probability":0.01},{"value":2,"label":"high","probability":0.9}],"confidence":0.9},
      {"type":"refusal","name":null}],
      "usage":{"input_tokens":42,"input_tokens_details":{"cached_tokens":8,"cache_write_tokens":0},"output_tokens":0,"output_tokens_details":{"reasoning_tokens":0},"total_tokens":42}})", "gpt-6-luna");
  CHECK(std::holds_alternative<decisions::Result>(direct));
  const auto& result = std::get<decisions::Result>(direct);
  CHECK(result.answers.size() == 4);
  CHECK(result.answers[0].name == "is_bug" && result.answers[0].kind == decisions::Kind::Predicate && result.answers[0].probability == 0.93);
  CHECK(result.answers[1].choice == "payments" && result.answers[1].confidence == 0.88 && result.answers[1].probabilities.size() == 2 &&
        result.answers[1].probabilities[1] == (std::pair<std::string, double>{"frontend", 0.12}));
  CHECK(result.answers[2].score == 1.9 && result.answers[2].probabilities[1].first == "high");
  CHECK(result.answers[3].refused && !result.answers[3].kind && result.answers[3].name.empty());
  CHECK(result.usage.input_total->value == 42 && result.usage.cache_read->value == 8 && result.usage.reasoning->value == 0 && result.usage.total->value == 42);
  CHECK(result.model.relation == ServedRelation::Exact);
  // OpenRouter keys answers by question name, reports a cost, and serves a dated snapshot.
  const auto routed = decoded(router, R"({"id":"gen-dec-1","model":"typesafe/jev-1.13-20260917","provider":"TypeSafe","answers":{
      "is_bug":{"type":"noul","noul":0.96},
      "team":{"type":"choice","choice":"payments","confidence":0.75,"probabilities":{"account":0,"frontend":0.16,"payments":0.84}},
      "where":{"type":"box","box":{"top":0.1,"bottom":0.4,"left":0.2,"right":0.6},"confidence":0.7}},
      "usage":{"cost":1.9992e-05,"input_tokens":476,"output_tokens":70}})", "typesafe/jev-1.13");
  const auto& via = std::get<decisions::Result>(routed);
  CHECK(via.id == "gen-dec-1" && via.provider == "TypeSafe" && via.answers.size() == 3);
  CHECK(via.answers[0].name == "is_bug" && via.answers[0].probability == 0.96 && via.answers[1].probabilities.size() == 3);
  CHECK(via.answers[2].kind == decisions::Kind::Box && via.answers[2].raw->root().get("box").get("right").as_double() == 0.6);
  CHECK(via.usage.provider_cost.total && via.usage.provider_cost.total->nano_usd == 19'992u && via.model.relation == ServedRelation::Snapshot);
  for (const auto& [body, kind] : std::initializer_list<std::pair<std::string, ErrorKind>>{
           {R"({"model":"m"})", ErrorKind::ProtocolCorrupt}, {R"({"answers":[{"name":"x"}]})", ErrorKind::ProtocolCorrupt},
           {R"({"answers":[{"type":"predicate","probability":"high"}]})", ErrorKind::ProtocolCorrupt},
           {R"({"error":{"message":"x"}})", ErrorKind::RemoteFailure}, {R"({"model":7,"answers":[]})", ErrorKind::ProtocolCorrupt}}) {
    const auto failed = decoded(openai, body, "gpt-6-luna");
    CHECK(std::holds_alternative<Failure>(failed) && std::get<Failure>(failed).error.kind == kind && !std::get<Failure>(failed).error.reason.empty());
  }
}
}  // namespace

int main() {
  try { requests(); answers(); std::cout << "decision endpoint properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "decision endpoint test failed: " << error.what() << '\n'; return 1; }
}
