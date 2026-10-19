// Image endpoints (D6): the request each provider shape takes, the typed result of a response, and the policy facts
// that admit them.
#include "codecs/images.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include "runtime/client.h"
#include "sp/config_defaults.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
// The built-in policy with its `endpoints` replaced.
std::variant<descriptor::PolicySnapshot, descriptor::ConfigError> policy_with(std::string_view endpoints) {
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  CHECK(std::holds_alternative<json::Document>(parsed));
  std::string text = "{";
  for (auto member : std::get<json::Document>(parsed).root().members()) {
    if (text.size() > 1) text += ',';
    text += json::quote(member.key) + ":" + (member.key == "endpoints" ? std::string(endpoints) : member.value.dump());
  }
  return descriptor::load_policy(text + "}", config_defaults::codec_defaults_json);
}
descriptor::ValidatedDescriptor at(std::string_view origin, std::string_view family = "openai.chat") {
  const std::string path = family == "openai.chat" ? (origin == "https://openrouter.ai" ? "/api/v1/chat/completions" : "/v1/chat/completions") : "/v1/responses";
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"images-fixture","family":)" + json::quote(family) +
                                 R"(,"connection":{"base_url":)" + json::quote(origin) + R"(,"paths":{"buffered":)" + json::quote(path) +
                                 R"(,"streaming":)" + json::quote(path) + "}}}");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::shared_ptr<const json::Document> document(std::string_view text) {
  auto parsed = json::parse(text);
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
images::Encoded encoded(const descriptor::ValidatedDescriptor& d, const images::Request& r) {
  auto result = images::encode(d, r);
  if (const auto* error = std::get_if<Error>(&result)) throw std::runtime_error("encode refused: " + error->safe_message);
  return std::get<images::Encoded>(std::move(result));
}
ErrorKind refused(const descriptor::ValidatedDescriptor& d, const images::Request& r) {
  auto result = images::encode(d, r);
  CHECK(std::holds_alternative<Error>(result));
  return std::get<Error>(result).kind;
}
const char png[] = "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEklEQVR4nGO4ckUEK2IYlRIBAPzqLqpk9vWsAAAAAElFTkSuQmCC";
images::Outcome decoded(const descriptor::ValidatedDescriptor& d, std::string_view body, std::string model = "gpt-image-2") {
  Accumulator accumulator;
  images::Codec codec(d, accumulator, std::move(model));
  codec.buffered(body, {});
  CHECK(accumulator.outcome());
  return images::typed(*accumulator.outcome());
}

void requests() {
  const auto openai = at("https://api.openai.com"), router = at("https://openrouter.ai");
  images::Request request{"gpt-image-2", "a red fox in snow"};
  request.count = 2; request.size = "1024x1024"; request.quality = "low"; request.output_format = "webp"; request.output_compression = 80;
  const auto generate = encoded(openai, request);
  CHECK(generate.path == "/v1/images/generations" && generate.charge_on_accept && generate.auth == "bearer" && generate.model == "gpt-image-2");
  {
    const auto body = document(generate.body);
    const auto root = body->root();
    CHECK(root.get("model").as_string() == "gpt-image-2" && root.get("n").as_uint() == 2 && root.get("size").as_string() == "1024x1024" &&
          root.get("quality").as_string() == "low" && root.get("output_format").as_string() == "webp" && root.get("output_compression").as_uint() == 80 &&
          !root.get("images").valid() && !root.get("stream").valid());
  }
  // Source images make it an edit: a data URL, a URL and a file id.
  auto edit = request;
  edit.references = {Media::image("image/png", std::make_shared<const std::string>(png)), Media::url(MediaKind::Image, "https://example.test/a.png", "image/png"),
                     Media::file(MediaKind::Image, "file-abc123", "image/png")};
  const auto edited = encoded(openai, edit);
  CHECK(edited.path == "/v1/images/edits");
  {
    const auto body = document(edited.body);
    const auto images = body->root().get("images");
    CHECK(images.size() == 3 && images.at(0).get("image_url").as_string() == std::string("data:image/png;base64,") + png &&
          images.at(1).get("image_url").as_string() == "https://example.test/a.png" && images.at(2).get("file_id").as_string() == "file-abc123");
  }
  // OpenRouter takes references as content parts, on one path, and has no file ids.
  auto routed = edit; routed.model = "google/gemini-3.1-flash-image"; routed.references.pop_back();
  routed.extensions = document(R"({"aspect_ratio":"16:9","seed":7,"provider":{"order":["google-vertex"],"allow_fallbacks":false}})");
  const auto via = encoded(router, routed);
  CHECK(via.path == "/api/v1/images");
  {
    const auto body = document(via.body);
    const auto root = body->root();
    CHECK(root.get("input_references").size() == 2 && root.get("input_references").at(0).get("type").as_string() == "image_url" &&
          root.get("input_references").at(1).get("image_url").get("url").as_string() == "https://example.test/a.png" &&
          root.get("aspect_ratio").as_string() == "16:9" && root.get("seed").as_uint() == 7 && root.get("provider").get("order").at(0).as_string() == "google-vertex");
  }
  auto file_ref = routed; file_ref.references = {Media::file(MediaKind::Image, "file-abc123", "image/png")};
  CHECK(refused(router, file_ref) == ErrorKind::Unsupported);
  // Refusals before any I/O: an origin without a fact, controls outside their shape, options outside the schema.
  CHECK(refused(at("http://127.0.0.1:18080"), request) == ErrorKind::Unsupported);
  auto change = [&](auto edit_request, ErrorKind kind) { auto r = request; edit_request(r); CHECK(refused(openai, r) == kind); };
  change([](auto& r) { r.prompt.clear(); }, ErrorKind::InvalidRequest);
  change([](auto& r) { r.size = "1024 x 1024"; }, ErrorKind::InvalidRequest);
  change([](auto& r) { r.count = 0u; }, ErrorKind::InvalidRequest);
  change([](auto& r) { r.output_compression = 101u; }, ErrorKind::InvalidRequest);
  change([](auto& r) { r.references = {Media::url(MediaKind::Audio, "https://example.test/a.wav", "audio/wav")}; }, ErrorKind::InvalidRequest);
  change([](auto& r) { r.extensions = document(R"({"stream":true})"); }, ErrorKind::InvalidRequest);
  change([](auto& r) { r.extensions = document(R"({"moderation":"low","unknown":1})"); }, ErrorKind::InvalidRequest);
  auto moderated = request; moderated.extensions = document(R"({"moderation":"low"})");
  CHECK(document(encoded(openai, moderated).body)->root().get("moderation").as_string() == "low");
}

void decoded_responses() {
  const auto openai = at("https://api.openai.com"), router = at("https://openrouter.ai");
  const auto result = decoded(openai, std::string(R"({"created":1760000000,"output_format":"webp","data":[{"b64_json":")") + png +
      R"(","revised_prompt":"A red fox standing in fresh snow"},{"url":"https://example.test/out.png"}],"usage":{"input_tokens":40,"output_tokens":272,"total_tokens":312,"input_tokens_details":{"text_tokens":40,"image_tokens":0}}})");
  CHECK(std::holds_alternative<images::Result>(result));
  const auto& images = std::get<images::Result>(result);
  CHECK(images.images.size() == 2 && images.images[0].source == MediaSource::Inline && images.images[0].mime == "image/webp" && *images.images[0].data == png &&
        images.images[1].source == MediaSource::Url && images.images[1].reference == "https://example.test/out.png");
  CHECK(images.revised_prompts == (std::vector<std::string>{"A red fox standing in fresh snow", ""}) && images.created == 1760000000u);
  CHECK(images.usage.input_total->value == 40 && images.usage.output_total->value == 272 && images.usage.total->value == 312 &&
        images.usage.extra.at("input_tokens_details.text_tokens").value == 40 && images.usage.stage == UsageStage::Final);
  CHECK(images.model.requested == "gpt-image-2" && images.model.relation == ServedRelation::NotReported);
  CHECK(images.raw_events.size() == 1 && images.raw_events[0].type == "images.response");
  // OpenRouter names each image's type and reports a cost.
  const auto routed = decoded(router, std::string(R"({"created":1,"data":[{"b64_json":")") + png +
      R"(","media_type":"image/png"}],"usage":{"prompt_tokens":10,"completion_tokens":1290,"total_tokens":1300,"cost":0.039}})", "google/gemini-3.1-flash-image");
  const auto& via = std::get<images::Result>(routed);
  CHECK(via.images.size() == 1 && via.images[0].mime == "image/png" && via.usage.output_total->value == 1290 &&
        via.usage.provider_cost.total && via.usage.provider_cost.total->nano_usd == 39'000'000u);
  // A response without images, with an error, or with an item that holds nothing.
  for (const auto& [body, kind] : std::initializer_list<std::pair<std::string, ErrorKind>>{
           {R"({"created":1})", ErrorKind::ProtocolCorrupt}, {R"({"error":{"message":"x"}})", ErrorKind::RemoteFailure},
           {R"({"created":1,"data":[{}]})", ErrorKind::ProtocolCorrupt}, {"[1]", ErrorKind::ProtocolCorrupt}, {"not json", ErrorKind::ProtocolCorrupt}}) {
    const auto failed = decoded(openai, body);
    CHECK(std::holds_alternative<Failure>(failed) && std::get<Failure>(failed).error.kind == kind && !std::get<Failure>(failed).error.reason.empty());
  }
  // No images is still a billed answer.
  CHECK(std::get<images::Result>(decoded(openai, R"({"created":1,"data":[]})")).images.empty());
}

void policy_facts() {
  auto refused_at = [](std::string_view endpoints, std::string_view pointer) {
    const auto loaded = policy_with(endpoints);
    CHECK(std::holds_alternative<descriptor::ConfigError>(loaded) && std::get<descriptor::ConfigError>(loaded).pointer == pointer);
  };
  const std::string fact = R"({"endpoint":"images","shape":"openai","origins":["https://api.openai.com"],"paths":{"generate":"/v1/images/generations"}})";
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(policy_with("[" + fact + "]")));
  refused_at(R"([{"endpoint":"videos","shape":"openai","origins":["https://api.openai.com"],"paths":{"generate":"/v1/videos"}}])", "/endpoints/shape");
  refused_at(R"([{"endpoint":"images","shape":"openai","origins":["https://api.openai.com/v1"],"paths":{"generate":"/v1/images/generations"}}])", "/endpoints/origins");
  refused_at(R"([{"endpoint":"images","shape":"openai","origins":["https://api.openai.com"],"paths":{"generate":"v1/images"}}])", "/endpoints/paths");
  refused_at(R"([{"endpoint":"images","shape":"openai","origins":["https://api.openai.com"],"paths":{"generate":"/v1/images?x=1"}}])", "/endpoints/paths");
  refused_at(R"([{"endpoint":"images","shape":"openrouter","origins":["https://openrouter.ai"],"paths":{"edit":"/api/v1/images"}}])", "/endpoints/paths");
  refused_at(R"([{"endpoint":"images","shape":"openai","origins":["https://api.openai.com"],"paths":{"generate":"/v1/images"},"options":{"type":"object","properties":{"prompt":{"type":"null"}}}}])", "/endpoints/options");
  refused_at(R"([{"endpoint":"images","shape":"openai","origins":["https://api.openai.com"],"paths":{"generate":"/v1/images"},"auth":"basic"}])", "/endpoints/auth");
  refused_at("[" + fact + "," + fact + "]", "/endpoints/origins");
  const auto* built_in = descriptor::builtin_policy()->endpoint("images", "https://openrouter.ai");
  CHECK(built_in && built_in->shape == "openrouter" && built_in->charge_on_accept && descriptor::endpoint_path(*built_in, "generate") == "/api/v1/images" &&
        !descriptor::endpoint_path(*built_in, "edit"));
}

// The endpoint types leave the chat request and outcome as they were (D6).
static_assert(std::variant_size_v<runtime::Request> == 5);
static_assert(std::variant_size_v<Outcome> == 2);
}  // namespace

int main() {
  try { requests(); decoded_responses(); policy_facts(); std::cout << "image endpoint properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "image endpoint test failed: " << error.what() << '\n'; return 1; }
}
