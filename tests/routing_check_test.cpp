// check_routing (#234): a consumer validates an OpenRouter routing default against a descriptor once, when it builds its
// client, and gets the refusal the encoder gives at prepare. This test includes only installed headers, the way a
// consumer does.
#include "codecs/chat.h"
#include "codecs/messages_request.h"
#include "codecs/responses_request.h"
#include "core/request_controls.h"
#include "descriptor/descriptor.h"
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace sp;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)

descriptor::ValidatedDescriptor load(std::string_view family, std::string_view origin, std::string_view path) {
  const auto text = std::string(R"({"descriptor_version":1,"revision":1,"id":"routing-check","family":")") + std::string(family) +
                    R"(","connection":{"base_url":")" + std::string(origin) + R"(","paths":{"buffered":")" + std::string(path) +
                    R"(","streaming":")" + std::string(path) + R"("}}})";
  auto loaded = descriptor::load(text);
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
const descriptor::ValidatedDescriptor& chat_gateway() {
  static const auto d = load("openai.chat", "https://openrouter.ai", "/api/v1/chat/completions");
  return d;
}
const descriptor::ValidatedDescriptor& chat_direct() {
  static const auto d = load("openai.chat", "http://127.0.0.1:18080", "/v1/chat/completions");
  return d;
}
const descriptor::ValidatedDescriptor& messages_gateway() {
  static const auto d = load("anthropic.messages", "https://openrouter.ai", "/api/v1/messages");
  return d;
}
const descriptor::ValidatedDescriptor& responses_gateway() {
  static const auto d = load("openai.responses", "https://openrouter.ai", "/api/v1/responses");
  return d;
}

chat::Request chat_request(const OpenRouterRouting& routing) {
  chat::Request request; request.model = "fixture-model"; request.messages.push_back({Role::User, "hello"});
  request.provider = routing;
  return request;
}
// What `encode` of a chat request carrying `routing` refuses with, or nothing.
std::optional<Error> encoded(const descriptor::ValidatedDescriptor& d, const OpenRouterRouting& routing) {
  auto result = chat::encode(d, chat_request(routing), false);
  if (auto* error = std::get_if<Error>(&result)) return std::move(*error);
  return std::nullopt;
}

// The routings the check refuses, each with the reason the encoder gives for it.
struct Refusal { const char* name; OpenRouterRouting routing; const char* code; };
std::vector<Refusal> refusals() {
  std::vector<Refusal> list;
  OpenRouterRouting r;
  r.only = {""};
  list.push_back({"an empty only entry", r, "request.controls.routing_lists_require_bounded_unique_nonempty"});
  r = {}; r.order = {"a", "a"};
  list.push_back({"a repeated order entry", r, "request.controls.routing_lists_require_bounded_unique_nonempty"});
  r = {}; r.data_collection = "maybe";
  list.push_back({"a data collection value", r, "request.controls.data_collection_must_be_allow_or_deny"});
  r = {}; r.only = {"a"}; r.ignore = {"a"};
  list.push_back({"an ignored provider that is selected", r, "request.controls.ignored_providers_cannot_be_selected_or"});
  r = {}; r.only = {"a"}; r.order = {"b"};
  list.push_back({"an ordered provider outside only", r, "request.controls.ordered_providers_must_belong_to_only"});
  r = {}; r.max_price = OpenRouterMaxPrice{};
  list.push_back({"an empty max_price", r, "request.controls.max_price_requires_at_least_one_amount"});
  r = {}; r.max_price = OpenRouterMaxPrice{}; r.max_price->prompt_nano_usd = max_price_limit_nano_usd + 1;
  list.push_back({"a max_price above the limit", r, "request.controls.max_price_amounts_cannot_exceed_2_53_nanousd"});
  return list;
}

void valid_routing_passes() {
  OpenRouterRouting routing;
  CHECK(!request_controls::check_routing(chat_gateway(), routing));
  routing.zdr = true; routing.allow_fallbacks = false; routing.data_collection = "deny";
  routing.only = {"a", "b"}; routing.order = {"b"}; routing.ignore = {"c"};
  routing.max_price = OpenRouterMaxPrice{}; routing.max_price->prompt_nano_usd = max_price_limit_nano_usd;
  CHECK(!request_controls::check_routing(chat_gateway(), routing));
  CHECK(!encoded(chat_gateway(), routing));
  CHECK(!request_controls::check_routing(messages_gateway(), routing));
  CHECK(!request_controls::check_routing(responses_gateway(), routing));
}

// The refusal is the one the encoder gives for the same routing: its kind and reason, with nothing sent.
void refusal_matches_the_encoder() {
  for (const auto& refusal : refusals()) {
    const auto expected = encoded(chat_gateway(), refusal.routing);
    CHECK(expected && expected->kind == ErrorKind::InvalidRequest && expected->reason == refusal.code);
    const auto got = request_controls::check_routing(chat_gateway(), refusal.routing);
    if (!got) throw std::runtime_error(std::string("not refused: ") + refusal.name);
    CHECK(got->kind == expected->kind);
    CHECK(got->reason == expected->reason);
    CHECK(got->safe_message == expected->safe_message);
    CHECK(got->retry_safety == RetrySafety::NotSent);
    CHECK(got->retry_class == RetryClass::Never);
  }
}

void refusal_is_the_same_for_every_family() {
  for (const auto& refusal : refusals()) {
    messages::Request messages; messages.model = "fixture-model"; messages.account_scope = "fixture-account";
    messages.messages.push_back(Message::user_text("hello")); messages.provider = refusal.routing;
    auto m = messages::encode(messages_gateway(), messages, false);
    CHECK(std::holds_alternative<Error>(m) && std::get<Error>(m).reason == refusal.code);
    responses::Request responses; responses.model = "fixture-model"; responses.account_scope = "fixture-account";
    responses.messages.push_back(Message{"", Role::User, {Text{"hello"}}}); responses.provider = refusal.routing;
    auto r = responses::encode(responses_gateway(), responses, false);
    CHECK(std::holds_alternative<Error>(r) && std::get<Error>(r).reason == refusal.code);
    for (const auto* d : {&messages_gateway(), &responses_gateway()}) {
      const auto got = request_controls::check_routing(*d, refusal.routing);
      CHECK(got && got->kind == ErrorKind::InvalidRequest && got->reason == refusal.code && got->retry_safety == RetrySafety::NotSent);
    }
  }
}

// A routing at an origin that is not a declared OpenRouter origin is refused, even an empty one.
void other_origin_is_refused() {
  const OpenRouterRouting routing;
  const auto expected = encoded(chat_direct(), routing);
  CHECK(expected && expected->reason == "request.controls.openrouter_routing_requires_a_declared");
  const auto got = request_controls::check_routing(chat_direct(), routing);
  CHECK(got && got->kind == expected->kind && got->reason == expected->reason && got->safe_message == expected->safe_message);
  CHECK(got->retry_safety == RetrySafety::NotSent);
}

}  // namespace

int main() {
  struct Case { const char* name; void (*run)(); };
  const Case cases[]{{"valid_routing_passes", valid_routing_passes}, {"refusal_matches_the_encoder", refusal_matches_the_encoder},
                     {"refusal_is_the_same_for_every_family", refusal_is_the_same_for_every_family},
                     {"other_origin_is_refused", other_origin_is_refused}};
  int failed = 0;
  for (const auto& test : cases) {
    try {
      test.run();
    } catch (const std::exception& error) {
      std::cerr << test.name << ": " << error.what() << "\n";
      ++failed;
    }
  }
  return failed ? 1 : 0;
}
