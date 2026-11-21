// Decision endpoints (D6): the request each provider shape takes and the typed answers of a response.
#include "codecs/decisions.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include "sp/config_defaults.h"
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
  // An answer names its option by value, and OpenRouter keys a choice's criteria by it: a value listed twice would
  // write one JSON key twice.
  for (const auto* d : {&openai, &router}) {
    change(*d, [](auto& r) { r.questions[1].options[1].value = "payments"; }, ErrorKind::InvalidRequest);
    change(*d, [](auto& r) { r.questions[2].options[2].value = "low"; }, ErrorKind::InvalidRequest);
  }
  change(router, [](auto& r) { r.questions[0].name.clear(); }, ErrorKind::InvalidRequest);
  change(router, [](auto& r) { r.questions[0].when_true = text("only one"); }, ErrorKind::InvalidRequest);
  change(router, [](auto& r) { r.extensions = document(R"({"questions":{}})"); }, ErrorKind::InvalidRequest);
  auto box = request; box.questions = {{"where", decisions::Kind::Box, text("Where is the error dialog?")}};
  CHECK(document(encoded(router, box).body)->root().get("questions").get("where").get("type").as_string() == "box");
}

// OpenRouter's top-level `assets` member: images sent once under a key that the state refers to.
void assets() {
  const auto openai = at("https://api.openai.com"), router = at("https://openrouter.ai");
  const std::string png = "iVBORw0KGgo=", jpeg = "/9j/";
  const auto asset = [](std::string key, const std::string& mime, const std::string& data) {
    return decisions::Asset{std::move(key), Media::image(mime, std::make_shared<const std::string>(data))};
  };
  const auto base = [&] {
    decisions::Request request{"typesafe/jev-1.14", document(R"({"photo":"a3f1c2e8-found"})")};
    request.questions = {{"same_dog", decisions::Kind::Predicate, text("Is the dog in `photo` the dog in 77b0d4a1-poster?")}};
    return request;
  };
  auto request = base();
  // Without assets the body has no `assets` member.
  CHECK(!document(encoded(router, request).body)->root().get("assets").valid());
  request.assets = {asset("a3f1c2e8-found", "image/png", png), asset("77b0d4a1-poster", "image/jpeg", jpeg)};
  {
    const auto body = document(encoded(router, request).body);
    const auto root = body->root();
    CHECK(root.get("state").get("photo").as_string() == "a3f1c2e8-found" && root.get("questions").get("same_dog").get("type").as_string() == "noul");
    const auto sent = root.get("assets");
    CHECK(sent.is_object() && sent.size() == 2);
    CHECK(sent.get("a3f1c2e8-found").get("type").as_string() == "image" && sent.get("a3f1c2e8-found").get("format").as_string() == "png" &&
          sent.get("a3f1c2e8-found").get("data_base64").as_string() == png);
    CHECK(sent.get("77b0d4a1-poster").get("type").as_string() == "image" && sent.get("77b0d4a1-poster").get("format").as_string() == "jpg" &&
          sent.get("77b0d4a1-poster").get("data_base64").as_string() == jpeg);
    // The members of the request are still sent beside the assets.
    request.extensions = document(R"({"session_id":"s-2"})");
    CHECK(document(encoded(router, request).body)->root().get("session_id").as_string() == "s-2");
    request.extensions = {};
  }
  // An asset nothing refers to is still sent: the service resolves the references.
  {
    auto unreferenced = base();
    unreferenced.assets = {asset("unused-key", "image/png", png)};
    CHECK(document(encoded(router, unreferenced).body)->root().get("assets").get("unused-key").valid());
  }
  const auto refuse = [&](const descriptor::ValidatedDescriptor& d, const decisions::Request& r, ErrorKind kind, std::string_view reason) {
    auto result = decisions::encode(d, r);
    CHECK(std::holds_alternative<Error>(result));
    const auto& error = std::get<Error>(result);
    if (error.kind != kind || error.reason != reason) throw std::runtime_error("line " + std::to_string(__LINE__) + ": refused with " + error.reason);
  };
  // OpenAI's request has no such member.
  auto direct = request;
  direct.input = text("The dog in a3f1c2e8-found is the dog in 77b0d4a1-poster.");
  refuse(openai, direct, ErrorKind::Unsupported, "request.decisions.assets_unsupported");
  // A key is non-empty and unique; the typed member is a list, so a key listed twice would write one JSON key twice.
  auto edit = request;
  edit.assets[0].key.clear();
  refuse(router, edit, ErrorKind::InvalidRequest, "request.decisions.asset_key_invalid");
  edit = request;
  edit.assets[1].key = edit.assets[0].key;
  refuse(router, edit, ErrorKind::InvalidRequest, "request.decisions.asset_key_duplicate");
  edit = request;
  edit.assets[0].key = "bad\xff";
  refuse(router, edit, ErrorKind::InvalidRequest, "request.decisions.asset_key_invalid");
  // An asset is an inline image of a format the policy fact names.
  edit = request;
  edit.assets[0].media = Media::audio("audio/wav", std::make_shared<const std::string>("UklGRiQAAABXQVZFZm10IHN5bnRoZXRpYw=="));
  refuse(router, edit, ErrorKind::Unsupported, "request.decisions.asset_unsupported");
  edit = request;
  edit.assets[0].media = Media::image("image/png", {});
  edit.assets[0].media.source = MediaSource::Url;
  edit.assets[0].media.reference = "https://example.com/a.png";
  refuse(router, edit, ErrorKind::Unsupported, "request.decisions.asset_unsupported");
  // The policy admits WebP and GIF images at the gateway, but the member names only PNG and JPEG.
  edit = request;
  edit.assets[0].media = Media::image("image/gif", std::make_shared<const std::string>("R0lGODlh"));
  refuse(router, edit, ErrorKind::Unsupported, "request.decisions.asset_unsupported");
  // Media admission applies: the bytes must match the type, and the decoded size is capped.
  edit = request;
  edit.assets[0].media = Media::image("image/png", std::make_shared<const std::string>(jpeg));
  CHECK(refused(router, edit) == ErrorKind::InvalidRequest);
  {
    constexpr std::size_t limit = 5 * 1024 * 1024;
    std::string big = "iVBORw0KGgoA";
    for (std::size_t decoded = 9; decoded <= limit; decoded += 3) big += "AAAA";
    edit = request;
    edit.assets[0].media = Media::image("image/png", std::make_shared<const std::string>(std::move(big)));
    CHECK(refused(router, edit) == ErrorKind::ResourceLimit);
  }
  // `assets` is no extension member: the typed member is the only way, so it cannot be sent twice.
  for (const auto* d : {&openai, &router}) {
    edit = d == &openai ? direct : request;
    edit.assets.clear();
    edit.extensions = document(R"({"assets":{"k":{"type":"image","format":"png","data_base64":"iVBORw0KGgo="}}})");
    refuse(*d, edit, ErrorKind::InvalidRequest, "request.decisions.options_schema_invalid");
  }
  // With the typed member as well, the extension is still refused: it is never sent beside the typed one.
  edit = request;
  edit.extensions = document(R"({"assets":{}})");
  refuse(router, edit, ErrorKind::InvalidRequest, "request.decisions.options_schema_invalid");
  // The formats come from the policy fact, not from code: a fact that maps WebP to `webp` sends a WebP asset with that
  // format, and one that states none refuses every asset.
  {
    const auto with_formats = [&](std::string_view member) {
      auto parsed = json::parse(config_defaults::descriptor_policy_json);
      CHECK(std::holds_alternative<json::Document>(parsed));
      std::string text = "{";
      for (auto entry : std::get<json::Document>(parsed).root().members()) {
        if (text.size() > 1) text += ',';
        text += json::quote(entry.key) + ":" +
                (entry.key == "endpoints" ? R"([{"endpoint":"decisions","shape":"openrouter","origins":["https://openrouter.ai"],"paths":{"decide":"/api/alpha/decisions"})" +
                                                std::string(member) + "}]"
                                          : std::string(entry.value.dump()));
      }
      auto policy = descriptor::load_policy(text + "}", config_defaults::codec_defaults_json);
      CHECK(std::holds_alternative<descriptor::PolicySnapshot>(policy));
      auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"assets-fixture","family":"openai.chat","connection":{"base_url":"https://openrouter.ai","paths":{"buffered":"/api/v1/chat/completions","streaming":"/api/v1/chat/completions"}}})",
                                     std::get<descriptor::PolicySnapshot>(std::move(policy)));
      CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
      return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
    };
    const auto custom = with_formats(R"(,"asset_formats":{"image/webp":"webp"})");
    auto webp = base();
    webp.assets = {asset("w", "image/webp", "UklGRgAAAABXRUJQ")};
    const auto sent_body = document(encoded(custom, webp).body);
    const auto sent = sent_body->root().get("assets").get("w");
    CHECK(sent.get("format").as_string() == "webp" && sent.get("data_base64").as_string() == "UklGRgAAAABXRUJQ");
    // The fact lists WebP only, so a PNG has no format.
    auto picture = base();
    picture.assets = {asset("p", "image/png", png)};
    refuse(custom, picture, ErrorKind::Unsupported, "request.decisions.asset_unsupported");
    refuse(with_formats(""), picture, ErrorKind::Unsupported, "request.decisions.asset_unsupported");
  }
  // Money: the call bills input tokens up to the model's input limit however they are written, so an asset counts as
  // the same image placed in `state` does: both state the image tokens among the input (`input_image`, #237), which a
  // text-only state does not.
  {
    const auto with_asset = decisions::endpoint_facts(router, request);
    auto inline_image = base();
    inline_image.input = document(R"([{"type":"image_url","image_url":{"url":"data:image/png;base64,iVBORw0KGgo="}}])");
    const auto in_state = decisions::endpoint_facts(router, inline_image);
    const auto plain = decisions::endpoint_facts(router, base());
    CHECK(with_asset.quantities.size() == 3 && in_state.quantities.size() == 3 && plain.quantities.size() == 2);
    // A video's frames bill as image input too.
    auto video = base();
    video.input = document(R"([{"type":"video_url","video_url":{"url":"https://example.com/a.mp4"}}])");
    const auto with_video = decisions::endpoint_facts(router, video);
    CHECK(with_video.quantities.size() == 3 && with_video.quantities[2].unit == "input_image" &&
          with_video.quantities[2].source == EndpointQuantity::Source::InputLimit);
    for (std::size_t i = 0; i < with_asset.quantities.size(); ++i) {
      const auto &a = with_asset.quantities[i], &b = in_state.quantities[i];
      CHECK(a.unit == b.unit && a.at_most == b.at_most && a.source == b.source && a.per == b.per && a.exact == b.exact);
      if (i < plain.quantities.size()) CHECK(a.unit == plain.quantities[i].unit && a.source == plain.quantities[i].source);
    }
    CHECK(with_asset.quantities[0].unit == "input" && with_asset.quantities[0].source == EndpointQuantity::Source::InputLimit && !with_asset.quantities[0].at_most);
    CHECK(with_asset.quantities[2].unit == "input_image" && with_asset.quantities[2].source == EndpointQuantity::Source::InputLimit &&
          !with_asset.quantities[2].at_most);
  }
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
  try { requests(); assets(); answers(); std::cout << "decision endpoint properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "decision endpoint test failed: " << error.what() << '\n'; return 1; }
}
