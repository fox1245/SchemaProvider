// Token counting (D7): each shape's request keeps only the members its fact lists (Google wraps a whole generate
// request), and each answer becomes the input count.
#include "codecs/tokens.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
std::shared_ptr<const json::Document> document(std::string_view text) {
  auto parsed = json::parse(text);
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
endpoints::Encoded encoded(const descriptor::ValidatedDescriptor& d, FamilyRequest request) {
  auto result = tokens::encode(d, tokens::Count{std::move(request)});
  if (const auto* error = std::get_if<Error>(&result)) throw std::runtime_error("encode refused: " + error->safe_message);
  return std::get<endpoints::Encoded>(std::move(result));
}
std::string_view header(const endpoints::Encoded& wire, std::string_view name) {
  for (const auto& [key, value] : wire.headers) if (key == name) return value;
  return {};
}
tokens::Outcome counted(const descriptor::ValidatedDescriptor& d, std::string_view body) {
  Accumulator accumulator;
  tokens::Codec codec(d, accumulator);
  codec.buffered(body, {});
  CHECK(accumulator.outcome());
  return tokens::typed(*accumulator.outcome());
}

void requests() {
  const auto anthropic = descriptor::presets::anthropic_messages();
  auto messages = messages::Request::simple("claude-haiku-4-5", "Hello there", 64, "Be brief.");
  messages.temperature = 0.5;
  const auto claude = encoded(anthropic, messages);
  {
    const auto body = document(claude.body);
    const auto root = body->root();
    // Generation controls are not counted, and the count endpoint refuses them.
    CHECK(claude.path == "/v1/messages/count_tokens" && !claude.charge_on_accept && header(claude, "anthropic-version") == "2023-06-01" &&
          root.get("model").as_string() == "claude-haiku-4-5" && root.get("messages").size() == 1 && root.get("system").valid() &&
          !root.get("max_tokens").valid() && !root.get("temperature").valid() && !root.get("stream").valid());
  }
  const auto openai = descriptor::presets::openai_responses();
  auto responses = responses::Request::simple("gpt-5-nano", "Hello there", 64);
  responses.reasoning = responses::ReasoningOptions{"low", std::nullopt};
  const auto gpt = encoded(openai, responses);
  {
    const auto body = document(gpt.body);
    const auto root = body->root();
    CHECK(gpt.path == "/v1/responses/input_tokens" && root.get("input").valid() && root.get("reasoning").get("effort").as_string() == "low" &&
          !root.get("max_output_tokens").valid() && !root.get("store").valid() && !root.get("include").valid() && !root.get("stream").valid());
  }
  const auto google = descriptor::presets::google_generate();
  const auto gemini = encoded(google, gemini::Request::simple("gemini-3.5-flash-lite", "Hello there", 64));
  {
    const auto body = document(gemini.body);
    const auto request = body->root().get("generateContentRequest");
    CHECK(gemini.path == "/v1beta/models/gemini-3.5-flash-lite:countTokens" && request.get("model").as_string() == "models/gemini-3.5-flash-lite" &&
          request.get("contents").size() == 1 && request.get("generationConfig").get("maxOutputTokens").as_uint() == 64);
  }
  // A family the origin does not count.
  CHECK(std::get<Error>(tokens::encode(anthropic, tokens::Count{responses})).kind == ErrorKind::Unsupported);
  CHECK(std::get<Error>(tokens::encode(descriptor::presets::openai_chat(), tokens::Count{chat::Request::simple("gpt-5-nano", "x")})).kind == ErrorKind::Unsupported);
  CHECK(std::get<Error>(tokens::encode(descriptor::presets::openrouter_chat(), tokens::Count{chat::Request::simple("x/y", "x")})).kind == ErrorKind::Unsupported);
}

void answers() {
  const auto anthropic = descriptor::presets::anthropic_messages();
  CHECK(std::get<tokens::Result>(counted(anthropic, R"({"input_tokens":9})")).input == 9);
  CHECK(std::get<tokens::Result>(counted(descriptor::presets::openai_responses(), R"({"object":"response.input_tokens","input_tokens":8})")).input == 8);
  const auto google = std::get<tokens::Result>(counted(descriptor::presets::google_generate(),
      R"({"totalTokens":3,"cachedContentTokenCount":"2","promptTokensDetails":[{"modality":"TEXT","tokenCount":3}]})"));
  CHECK(google.input == 3 && google.cached == 2u && google.modalities.size() == 1 && google.modalities[0] == (std::pair<std::string, std::uint64_t>{"TEXT", 3}));
  for (const auto* body : {R"({"input_tokens":"x"})", R"({"tokens":3})", "[]"})
    CHECK(std::holds_alternative<Failure>(counted(anthropic, body)) && std::get<Failure>(counted(anthropic, body)).error.reason.starts_with("tokens."));
}
}  // namespace

int main() {
  try { requests(); answers(); std::cout << "token properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "token test failed: " << error.what() << '\n'; return 1; }
}
