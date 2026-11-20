// Which header carries the API key. A chat call sends it the way the descriptor's `connection.auth` names, else the
// way the first of its family's `auth` facts that matches the origin names; an endpoint call and a realtime session
// the way their endpoint fact names. One mapping turns a scheme into a header. A key goes only to the descriptor's
// origin, and never twice.
#include "codecs/chat.h"
#include "codecs/embeddings.h"
#include "codecs/endpoint.h"
#include "codecs/files.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "codecs/messages_request.h"
#include "codecs/realtime.h"
#include "codecs/rerank.h"
#include "codecs/responses_request.h"
#include "core/native_archive_custody_internal.h"
#include "core/native_testing.h"
#include "crypto/crypto.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include "runtime/client.h"
#include "sp/config_defaults.h"
#include "support/endpoint_fakes.h"
#include "support/policy_edit.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace sp;
using namespace sp::test;
using namespace std::chrono_literals;
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

constexpr std::string_view key = "test-key-0001";
using Header = std::pair<std::string, std::string>;

std::string lower(std::string_view text) {
  std::string out(text);
  for (auto& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}
bool credential_name(std::string_view name) {
  const auto folded = lower(name);
  return folded == "authorization" || folded == "x-api-key" || folded == "x-goog-api-key";
}
// The header a scheme sends the key in, byte for byte.
Header expected(std::string_view scheme, std::string_view value = key) {
  if (scheme == "bearer") return {"Authorization", "Bearer " + std::string(value)};
  return {std::string(scheme), std::string(value)};
}
bool leaks(std::string_view text) { return text.find(key) != std::string_view::npos; }
void clean(const descriptor::ConfigError& error) {
  CHECK(!leaks(error.pointer) && !leaks(error.expected) && !leaks(error.message));
}
void clean(const Error& error) { CHECK(!leaks(error.safe_message) && !leaks(error.reason) && !leaks(error.vendor_code)); }
template <class Outcome> void clean_outcome(const Outcome& outcome) {
  if constexpr (requires { outcome.get(); }) {
    CHECK(outcome != nullptr);
    if (outcome) clean_outcome(*outcome);
  } else if (const auto* failure = std::get_if<Failure>(&outcome)) {
    clean(failure->error);
  }
}

struct FamilyCase { std::string_view family, buffered, streaming, scheme; };
constexpr FamilyCase families[]{
    {"openai.chat", "/v1/chat/completions", "/v1/chat/completions", "bearer"},
    {"openai.responses", "/v1/responses", "/v1/responses", "bearer"},
    {"anthropic.messages", "/v1/messages", "/v1/messages", "x-api-key"},
    {"google.generate", "/v1beta/models/{model}:generateContent", "/v1beta/models/{model}:streamGenerateContent?alt=sse", "x-goog-api-key"},
    {"google.interactions", "/v1beta/interactions", "/v1beta/interactions", "x-goog-api-key"}};
constexpr std::string_view schemes[]{"bearer", "x-api-key", "x-goog-api-key"};
constexpr std::string_view loopback = "http://127.0.0.1:9", other_loopback = "http://127.0.0.1:10", custom = "https://llm.example.com";

std::string descriptor_json(const FamilyCase& f, std::string_view origin, std::string_view connection_extra = {}) {
  return R"({"descriptor_version":1,"revision":1,"id":"credentials-fixture","family":)" + json::quote(f.family) +
         R"(,"connection":{"base_url":)" + json::quote(origin) + R"(,"paths":{"buffered":)" + json::quote(f.buffered) +
         R"(,"streaming":)" + json::quote(f.streaming) + "}" + std::string(connection_extra) + "}}";
}
std::optional<descriptor::ValidatedDescriptor> admitted_result(descriptor::LoadResult loaded) {
  if (auto* error = std::get_if<descriptor::ConfigError>(&loaded)) {
    std::fprintf(stderr, "descriptor refused at %s: %s\n", error->pointer.c_str(), error->expected.c_str());
    return std::nullopt;
  }
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::optional<descriptor::ValidatedDescriptor> admitted(const std::string& source, descriptor::PolicySnapshot policy = {}) {
  return admitted_result(descriptor::load(source, policy ? policy : descriptor::builtin_policy()));
}
// A refused descriptor names the pointer and carries no credential text.
void refused(const std::string& source, std::string_view pointer, descriptor::PolicySnapshot policy = {}) {
  auto loaded = descriptor::load(source, policy ? policy : descriptor::builtin_policy());
  const auto* error = std::get_if<descriptor::ConfigError>(&loaded);
  CHECK(error && error->pointer == pointer);
  if (error) clean(*error);
}

runtime::Request request_for(std::string_view family) {
  Message user;
  user.role = Role::User;
  user.parts.emplace_back(Text{"synthetic"});
  if (family == "openai.chat") {
    chat::Request r; r.model = "test-model"; r.messages.push_back({Role::User, "synthetic"}); return r;
  }
  if (family == "openai.responses") {
    responses::Request r; r.model = "test-model"; r.account_scope = "test-account"; r.messages.push_back(user); return r;
  }
  if (family == "anthropic.messages") {
    messages::Request r; r.model = "test-model"; r.account_scope = "test-account"; r.messages.push_back(user); return r;
  }
  if (family == "google.generate") {
    gemini::Request r; r.model = "test-model"; r.account_scope = "test-account"; r.messages.push_back(user); return r;
  }
  interactions::Request r; r.model = "test-model"; r.account_scope = "test-account"; r.messages.push_back(user); return r;
}

// What one call sent: the URL, every credential header in order, and whether the last header sent is one. The
// scripted provider refuses the key, and its answer quotes the key, which no error field may repeat.
struct Sent {
  bool reached = false, last = false;
  std::string url;
  std::vector<Header> credentials;
};
template <class Typed> Sent sent(const descriptor::ValidatedDescriptor& descriptor, Typed typed, std::string_view api_key = key) {
  Sent out;
  auto executor = std::make_shared<QueueExecutor>();
  auto wire = std::make_shared<ScriptTransport>();
  wire->script = [](Exchange& e) {
    e.reply(401, "application/json", R"({"error":{"type":"authentication_error","message":"the key test-key-0001 is not valid"}})");
  };
  runtime::Options options;
  options.api_key = std::string(api_key);
  auto client = runtime::detail::ClientAccess::make(descriptor, options, executor, wire);
  runtime::RunOptions run;
  run.deadline = executor->now() + 10s;
  run.retry = runtime::RetryPolicy{false, false, 1, 0ms, 0ms};
  auto call = client.start(client.prepare(std::move(typed), run));
  executor->drain();
  clean_outcome(call.join());
  if (wire->exchanges.size() != 1) return out;
  const auto& request = wire->exchanges.front()->request;
  out.reached = true;
  out.url = request.url;
  for (const auto& [name, value] : request.headers)
    if (credential_name(name)) out.credentials.emplace_back(name, value);
  out.last = !request.headers.empty() && credential_name(request.headers.back().name);
  return out;
}
// The call sent the key once, in the header of `scheme`, as the last header, to the descriptor's origin.
bool sends(const Sent& call, std::string_view scheme, std::string_view origin) {
  return call.reached && call.url.starts_with(std::string(origin) + "/") && call.credentials.size() == 1 &&
         call.credentials.front() == expected(scheme) && call.last;
}
bool sends_chat(const descriptor::ValidatedDescriptor& descriptor, std::string_view scheme) {
  return sends(sent(descriptor, request_for(descriptor.family())), scheme, descriptor.base_url());
}

descriptor::PolicySnapshot loaded_policy(const std::string& text) {
  auto loaded = descriptor::load_policy(text, config_defaults::codec_defaults_json);
  if (auto* error = std::get_if<descriptor::ConfigError>(&loaded)) {
    std::fprintf(stderr, "policy refused at %s: %s\n", error->pointer.c_str(), error->expected.c_str());
    return nullptr;
  }
  return std::get<descriptor::PolicySnapshot>(std::move(loaded));
}
void policy_refused(const std::string& text, std::string_view pointer) {
  auto loaded = descriptor::load_policy(text, config_defaults::codec_defaults_json);
  const auto* error = std::get_if<descriptor::ConfigError>(&loaded);
  CHECK(error && error->pointer == pointer);
  if (error) clean(*error);
}
// The embedded policy with `member` of `family` (of every family when `family` is empty) set to `value`, a JSON
// text, or left out when `value` is empty.
std::string edited(std::string_view family, std::string_view member, std::string_view value) {
  auto parsed = json::parse(config_defaults::descriptor_policy_json, {config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth});
  const auto root = std::get<json::Document>(parsed).root();
  std::string out = "{";
  for (auto top : root.members()) {
    if (out.size() > 1) out += ',';
    out += json::quote(top.key) + ':';
    if (top.key != "families") { out += top.value.dump(); continue; }
    out += '[';
    bool first = true;
    for (auto entry : top.value.elements()) {
      if (!std::exchange(first, false)) out += ',';
      if (!family.empty() && entry.get("family").as_string() != family) { out += entry.dump(); continue; }
      std::string object = "{";
      for (auto field : entry.members()) {
        if (field.key == member) continue;
        if (object.size() > 1) object += ',';
        object += json::quote(field.key) + ':' + field.value.dump();
      }
      if (!value.empty()) object += (object.size() > 1 ? "," : "") + json::quote(member) + ':' + std::string(value);
      out += object + '}';
    }
    out += ']';
  }
  return out + '}';
}

// With the built-in policy and the presets, each family sends the header it always sent: x-api-key for Messages,
// x-goog-api-key for Generate and Interactions, a bearer token for Chat and Responses, and the same at the routing
// gateway, at a loopback origin and at any other origin.
void builtin_headers() {
  CHECK(sends_chat(descriptor::presets::openai_chat(), "bearer"));
  CHECK(sends_chat(descriptor::presets::openai_responses(), "bearer"));
  CHECK(sends_chat(descriptor::presets::anthropic_messages(), "x-api-key"));
  CHECK(sends_chat(descriptor::presets::google_generate(), "x-goog-api-key"));
  CHECK(sends_chat(descriptor::presets::google_generate("test-model"), "x-goog-api-key"));
  CHECK(sends_chat(descriptor::presets::google_interactions(), "x-goog-api-key"));
  CHECK(sends_chat(descriptor::presets::openrouter_chat(), "bearer"));
  CHECK(sends_chat(descriptor::presets::openrouter_responses(), "bearer"));
  CHECK(sends_chat(descriptor::presets::openrouter_messages(), "x-api-key"));
  for (const auto& f : families)
    for (const auto origin : {loopback, custom}) {
      const auto d = admitted(descriptor_json(f, origin));
      CHECK(d && sends_chat(*d, f.scheme));
    }
  // Without a key no credential header is sent.
  const auto chat = descriptor::presets::openai_chat();
  const auto none = sent(chat, request_for(chat.family()), "");
  CHECK(none.reached && none.credentials.empty());
}

// The built-in policy states the scheme of every family as data, and a policy written before the facts reads the
// embedded policy's: it sends the same headers.
void older_policy_without_facts() {
  auto parsed = json::parse(config_defaults::descriptor_policy_json, {config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth});
  for (auto entry : std::get<json::Document>(parsed).root().get("families").elements()) {
    const auto facts = entry.get("auth");
    CHECK(facts.is_array() && facts.size() >= 1);
  }
  const auto old = loaded_policy(edited({}, "auth", {}));
  CHECK(old != nullptr);
  if (!old) return;
  CHECK(sends_chat(descriptor::presets::openai_chat({.policy = old}), "bearer"));
  CHECK(sends_chat(descriptor::presets::openai_responses({.policy = old}), "bearer"));
  CHECK(sends_chat(descriptor::presets::anthropic_messages({.policy = old}), "x-api-key"));
  CHECK(sends_chat(descriptor::presets::google_generate({.policy = old}), "x-goog-api-key"));
  CHECK(sends_chat(descriptor::presets::google_interactions({.policy = old}), "x-goog-api-key"));
  CHECK(sends_chat(descriptor::presets::openrouter_chat({.policy = old}), "bearer"));
  CHECK(sends_chat(descriptor::presets::openrouter_messages({.policy = old}), "x-api-key"));
  for (const auto& f : families) {
    const auto d = admitted(descriptor_json(f, loopback), old);
    CHECK(d && sends_chat(*d, f.scheme));
  }
  // Only Messages without the facts, the others as declared.
  const auto messages_only = loaded_policy(edited("anthropic.messages", "auth", {}));
  CHECK(messages_only && sends_chat(descriptor::presets::anthropic_messages({.policy = messages_only}), "x-api-key"));
  CHECK(messages_only && sends_chat(descriptor::presets::openai_chat({.policy = messages_only}), "bearer"));
}

// The content digest of a policy that borrows nothing is SHA-256 over the parsed policy pair and the catalogue, as
// before. A policy that borrows the facts mixes the borrowed values in behind a marker, so a pin covers them too.
std::string hex(const crypto::Sha256Digest& digest) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : digest) { out += digits[byte >> 4]; out += digits[byte & 15]; }
  return out;
}
std::string plain_digest(std::string_view policy_text, std::string_view borrowed = {}) {
  const json::Limits limits{config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth};
  auto family = json::parse(policy_text, limits), resources = json::parse(config_defaults::codec_defaults_json, limits);
  crypto::Sha256 pair;
  pair.update(std::get<json::Document>(family).root().dump());
  pair.update("\n");
  pair.update(std::get<json::Document>(resources).root().dump());
  const auto pair_digest = pair.finish();
  crypto::Sha256 content;
  content.update("sp.descriptor-policy.content.v2\n");
  content.update(pair_digest.data(), pair_digest.size());
  content.update(descriptor::builtin_catalogue()->digest());
  if (!borrowed.empty()) {
    content.update("sp.descriptor-policy.borrowed.v1\n");
    content.update(borrowed);
  }
  return hex(content.finish());
}
// The borrowed values as the digest takes them: per family in name order, the family, the member and the member's
// canonical JSON, each with its length.
std::string borrowed_values(const std::vector<std::string_view>& family_names) {
  auto parsed = json::parse(config_defaults::descriptor_policy_json, {config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth});
  std::string out;
  const auto put = [&](std::string_view text) { out += std::to_string(text.size()) + ":" + std::string(text); };
  for (const auto name : family_names)
    for (auto entry : std::get<json::Document>(parsed).root().get("families").elements())
      if (entry.get("family").as_string() == name) { put(name); put("auth"); put(entry.get("auth").dump()); }
  return out;
}
// The route facts a policy takes from the embedded policy join its content digest in the same form: per family in
// name order, then per member, the family, the member and the member's canonical JSON, each with its length.
void route_content_digests() {
  auto parsed = json::parse(config_defaults::descriptor_policy_json, {config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth});
  const auto root = std::get<json::Document>(parsed).root();
  const auto routes_of = [&](std::string_view name) {
    std::string out;
    const auto put = [&](std::string_view text) { out += std::to_string(text.size()) + ":" + std::string(text); };
    for (auto entry : root.get("families").elements())
      if (entry.get("family").as_string() == name) { put(name); put("routes"); put(entry.get("routes").dump()); }
    return out;
  };
  // Only Generate and Interactions state facts in the embedded policy, so only they are taken.
  const auto older_text = edited({}, "routes", {});
  const auto older = loaded_policy(older_text);
  CHECK(older && older->content_digest_hex() == plain_digest(older_text, routes_of("google.generate") + routes_of("google.interactions")));
  const auto generate_text = edited("google.generate", "routes", {});
  const auto generate = loaded_policy(generate_text);
  CHECK(generate && generate->content_digest_hex() == plain_digest(generate_text, routes_of("google.generate")));
  // A family without embedded facts takes none.
  const auto chat_text = edited("openai.chat", "routes", {});
  const auto chat = loaded_policy(chat_text);
  CHECK(chat && chat->content_digest_hex() == plain_digest(chat_text));
  // With the credential facts, the family's members are taken in name order.
  const auto both_text = [&] {
    auto parsed_text = json::parse(edited("google.generate", "routes", {}), {config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth});
    std::string out = "{";
    for (auto top : std::get<json::Document>(parsed_text).root().members()) {
      out += (out.size() > 1 ? "," : "") + json::quote(top.key) + ':';
      if (top.key != "families") { out += top.value.dump(); continue; }
      std::string families = "[";
      for (auto entry : top.value.elements()) {
        if (families.size() > 1) families += ',';
        if (entry.get("family").as_string() != "google.generate") { families += entry.dump(); continue; }
        std::string object = "{";
        for (auto field : entry.members())
          if (field.key != "auth") object += (object.size() > 1 ? "," : "") + json::quote(field.key) + ':' + field.value.dump();
        families += object + '}';
      }
      out += families + ']';
    }
    return out + '}';
  }();
  const auto both = loaded_policy(both_text);
  CHECK(both && both->content_digest_hex() == plain_digest(both_text, borrowed_values({"google.generate"}) + routes_of("google.generate")));
  // The facts are not part of the replay identity.
  CHECK(older && older->identity_hex() == descriptor::builtin_policy()->identity_hex());
}
// The prices of extension facts a policy takes from the embedded policy join its content digest in the same form: per
// fact in key order, the family, `extensions.<key>.prices` and the member's canonical JSON, each with its length. A
// policy that states `prices`, even as `{}`, takes none.
void extension_price_digests() {
  auto parsed = json::parse(config_defaults::descriptor_policy_json, {config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth});
  const auto root = std::get<json::Document>(parsed).root();
  constexpr std::string_view family = "anthropic.messages";
  std::vector<std::pair<std::string, std::string>> stated;
  std::string without = "[", empty = "[";
  for (auto entry : root.get("families").elements()) {
    if (entry.get("family").as_string() != family) continue;
    for (auto fact : entry.get("extensions").elements()) {
      std::string object = "{";
      for (auto field : fact.members())
        if (field.key != "prices") object += (object.size() > 1 ? "," : "") + json::quote(field.key) + ':' + field.value.dump();
      if (const auto prices = fact.get("prices"); prices.valid()) stated.emplace_back("extensions." + std::string(fact.get("key").as_string()) + ".prices", prices.dump());
      without += (without.size() > 1 ? "," : "") + object + '}';
      empty += (empty.size() > 1 ? "," : "") + object + ",\"prices\":{}}";
    }
  }
  without += ']';
  empty += ']';
  // The embedded policy prices `speed` and `inference_geo`.
  CHECK(std::any_of(stated.begin(), stated.end(), [](const auto& s) { return s.first == "extensions.speed.prices"; }));
  CHECK(std::any_of(stated.begin(), stated.end(), [](const auto& s) { return s.first == "extensions.inference_geo.prices"; }));
  std::sort(stated.begin(), stated.end());
  std::string borrowed;
  const auto put = [&](std::string_view text) { borrowed += std::to_string(text.size()) + ":" + std::string(text); };
  for (const auto& [member, value] : stated) { put(family); put(member); put(value); }
  const auto left_out_text = edited(family, "extensions", without);
  const auto left_out = loaded_policy(left_out_text);
  CHECK(left_out && left_out->content_digest_hex() == plain_digest(left_out_text, borrowed));
  CHECK(left_out && left_out->content_digest_hex() != plain_digest(left_out_text));
  const auto empty_text = edited(family, "extensions", empty);
  const auto declared = loaded_policy(empty_text);
  CHECK(declared && declared->content_digest_hex() == plain_digest(empty_text));
  // The prices are not part of the replay identity.
  CHECK(left_out && left_out->identity_hex() == descriptor::builtin_policy()->identity_hex());
  CHECK(declared && declared->identity_hex() == descriptor::builtin_policy()->identity_hex());
}
void content_digests() {
  const auto builtin = descriptor::builtin_policy();
  CHECK(builtin->content_digest_hex() == plain_digest(config_defaults::descriptor_policy_json));
  const auto declared_text = edited("openai.chat", "auth", R"([{"scheme":"bearer"}])");
  const auto declared = loaded_policy(declared_text);
  CHECK(declared && declared->content_digest_hex() == plain_digest(declared_text));
  const auto old_text = edited({}, "auth", {});
  const auto old = loaded_policy(old_text);
  CHECK(old && old->content_digest_hex() != plain_digest(old_text));
  CHECK(old && old->content_digest_hex() == plain_digest(old_text, borrowed_values({"anthropic.messages", "google.generate", "google.interactions",
                                                                                   "openai.chat", "openai.responses"})));
  const auto messages_text = edited("anthropic.messages", "auth", {});
  const auto messages_only = loaded_policy(messages_text);
  CHECK(messages_only && messages_only->content_digest_hex() == plain_digest(messages_text, borrowed_values({"anthropic.messages"})));
  // Another catalogue keeps the borrowed values in the digest.
  if (old) {
    auto recatalogued = old->with_catalogue(descriptor::builtin_catalogue());
    CHECK(std::holds_alternative<descriptor::PolicySnapshot>(recatalogued) &&
          std::get<descriptor::PolicySnapshot>(recatalogued)->content_digest_hex() == old->content_digest_hex());
  }
  // The facts are not part of the replay identity.
  CHECK(old && old->identity_hex() == builtin->identity_hex());
  const auto scoped = loaded_policy(edited("anthropic.messages", "auth", R"([{"origins":["https://llm.example.com"],"scheme":"bearer"},{"scheme":"x-api-key"}])"));
  CHECK(scoped && scoped->identity_hex() == builtin->identity_hex());
}

// A policy's `listings` facts name the family each model listing belongs to (DescriptorPolicy::listing_family) and the
// origin a source of it defaults to (`default_origin`, #221). The embedded policy states both for every listing; a
// policy that leaves a listing out has the embedded policy's family and origin, one that states a listing without
// `default_origin` the embedded origin, and its content digest takes each borrowed value in behind the marker. A
// `null` origin is none, and borrows nothing. A listing the SDK does not read, a family the policy does not have, a
// listing stated twice, a fact with other members and an origin that is not canonical are refused.
void listing_facts() {
  using descriptor::Listing;
  const std::pair<Listing, std::string_view> embedded[]{{Listing::OpenRouter, "openai.chat"}, {Listing::Gemini, "google.generate"},
                                                        {Listing::Anthropic, "anthropic.messages"}, {Listing::OpenAI, "openai.chat"}};
  const auto builtin = descriptor::builtin_policy();
  CHECK(descriptor::all_listings.size() == std::size(embedded));
  for (const auto& [listing, family] : embedded) CHECK(builtin->listing_family(listing) == family && builtin->family(family));
  for (const auto& [listing, origin] : std::initializer_list<std::pair<Listing, std::string_view>>{
           {Listing::OpenRouter, "https://openrouter.ai"}, {Listing::Gemini, "https://generativelanguage.googleapis.com"},
           {Listing::Anthropic, "https://api.anthropic.com"}, {Listing::OpenAI, "https://api.openai.com"}})
    CHECK(builtin->listing_origin(listing) == origin);
  // The embedded policy states every listing, so it borrows none.
  CHECK(builtin->content_digest_hex() == plain_digest(config_defaults::descriptor_policy_json));
  // Left out whole, or in part: the embedded value, behind the marker.
  const std::pair<Listing, std::string_view> origins[]{{Listing::OpenRouter, "https://openrouter.ai"},
                                                       {Listing::Gemini, "https://generativelanguage.googleapis.com"},
                                                       {Listing::Anthropic, "https://api.anthropic.com"}, {Listing::OpenAI, "https://api.openai.com"}};
  // The rows of the listings left out (`listings`: family and origin) and of the listings stated without an origin.
  const auto borrowed_of = [&](std::initializer_list<Listing> listings, std::initializer_list<Listing> origins_only = {}) {
    std::vector<std::pair<std::string, std::string>> rows;
    for (const auto listing : listings)
      for (const auto& [kind, family] : embedded)
        if (kind == listing) rows.emplace_back("listing:" + std::string(descriptor::listing_name(listing)), "\"" + std::string(family) + "\"");
    for (const auto& group : {listings, origins_only})
      for (const auto listing : group)
        for (const auto& [kind, origin] : origins)
          if (kind == listing) rows.emplace_back("listing_origin:" + std::string(descriptor::listing_name(listing)), "\"" + std::string(origin) + "\"");
    std::sort(rows.begin(), rows.end());
    std::string out;
    for (const auto& [member, value] : rows)
      for (const std::string_view text : {std::string_view(), std::string_view(member), std::string_view(value)})
        out += std::to_string(text.size()) + ":" + std::string(text);
    return out;
  };
  const auto none_text = policy_edit::edited({}, "listings", {});
  const auto none = loaded_policy(none_text);
  CHECK(none && none->content_digest_hex() == plain_digest(none_text, borrowed_of({Listing::OpenRouter, Listing::Gemini, Listing::Anthropic, Listing::OpenAI})));
  CHECK(none && none->content_digest_hex() != builtin->content_digest_hex());
  for (const auto& [listing, family] : embedded) CHECK(none && none->listing_family(listing) == family);
  const auto part_text = policy_edit::edited({}, "listings", R"([{"listing":"openrouter","family":"anthropic.messages"},{"listing":"gemini","family":"google.interactions"}])");
  const auto part = loaded_policy(part_text);
  CHECK(part && part->listing_family(Listing::OpenRouter) == "anthropic.messages" && part->listing_family(Listing::Gemini) == "google.interactions" &&
        part->listing_family(Listing::Anthropic) == "anthropic.messages" && part->listing_family(Listing::OpenAI) == "openai.chat");
  CHECK(part && part->content_digest_hex() == plain_digest(part_text, borrowed_of({Listing::Anthropic, Listing::OpenAI}, {Listing::OpenRouter, Listing::Gemini})));
  // Every listing stated with its origin, the embedded values: nothing is borrowed. A `null` origin borrows nothing either.
  const auto stated_text = policy_edit::edited({}, "listings",
      R"([{"listing":"openrouter","family":"openai.chat","default_origin":"https://openrouter.ai"},)"
      R"({"listing":"gemini","family":"google.generate","default_origin":"https://generativelanguage.googleapis.com"},)"
      R"({"listing":"anthropic","family":"anthropic.messages","default_origin":"https://api.anthropic.com"},)"
      R"({"listing":"openai","family":"openai.chat","default_origin":"https://api.openai.com"}])");
  const auto stated = loaded_policy(stated_text);
  CHECK(stated && stated->content_digest_hex() == plain_digest(stated_text));
  const auto no_default_text = policy_edit::edited({}, "listings",
      R"([{"listing":"openrouter","family":"openai.chat","default_origin":"https://openrouter.ai"},)"
      R"({"listing":"gemini","family":"google.generate","default_origin":"https://generativelanguage.googleapis.com"},)"
      R"({"listing":"anthropic","family":"anthropic.messages","default_origin":"https://api.anthropic.com"},)"
      R"({"listing":"openai","family":"openai.chat","default_origin":null}])");
  const auto no_default = loaded_policy(no_default_text);
  CHECK(no_default && no_default->content_digest_hex() == plain_digest(no_default_text));
  CHECK(no_default && no_default->listing_origin(Listing::OpenAI).empty() && no_default->listing_origin(Listing::Gemini) == "https://generativelanguage.googleapis.com");
  CHECK(part && part->listing_origin(Listing::OpenRouter) == "https://openrouter.ai" && none && none->listing_origin(Listing::Anthropic) == "https://api.anthropic.com");
  // The facts are not part of the replay identity, and another catalogue keeps them.
  CHECK(part && part->identity_hex() == builtin->identity_hex() && none->identity_hex() == builtin->identity_hex());
  if (part) {
    auto recatalogued = part->with_catalogue(descriptor::builtin_catalogue());
    CHECK(std::holds_alternative<descriptor::PolicySnapshot>(recatalogued) &&
          std::get<descriptor::PolicySnapshot>(recatalogued)->listing_family(Listing::OpenRouter) == "anthropic.messages");
  }
  policy_refused(policy_edit::edited({}, "listings", R"([{"listing":"cohere","family":"openai.chat"}])"), "/listings/listing");
  policy_refused(policy_edit::edited({}, "listings", R"([{"listing":"openai","family":"no.such.family"}])"), "/listings/family");
  policy_refused(policy_edit::edited({}, "listings", R"([{"listing":"openai","family":"openai.chat"},{"listing":"openai","family":"openai.responses"}])"), "/listings/listing");
  policy_refused(policy_edit::edited({}, "listings", R"([{"listing":"openai","family":"openai.chat","scheme":"bearer"}])"), "/listings");
  policy_refused(policy_edit::edited({}, "listings", R"({"openai":"openai.chat"})"), "/listings");
  policy_refused(policy_edit::edited({}, "listings", R"([{"listing":"openai"}])"), "/listings");
  for (const auto* origin : {R"("https://API.openai.com")", R"("https://api.openai.com:443")", R"("https://api.openai.com/v1")", R"("api.openai.com")", R"("")", "5"})
    policy_refused(policy_edit::edited({}, "listings", std::string(R"([{"listing":"openai","family":"openai.chat","default_origin":)") + origin + "}]"),
                   "/listings/default_origin");
}

// A descriptor states the scheme of its own calls with `connection.auth`, at any origin: the Messages or Generate
// shape at a host that takes a bearer token.
void descriptor_scheme() {
  for (const auto& f : families)
    for (const auto scheme : schemes)
      for (const auto origin : {loopback, custom}) {
        const auto d = admitted(descriptor_json(f, origin, R"(,"auth":)" + json::quote(scheme)));
        CHECK(d && sends_chat(*d, scheme));
      }
  const auto bearer_messages = admitted(descriptor_json(families[2], "https://api.anthropic.com", R"(,"auth":"bearer")"));
  CHECK(bearer_messages && sends_chat(*bearer_messages, "bearer"));
  // It is not a header: native identities and archive bindings do not change.
  const auto plain = admitted(descriptor_json(families[2], custom));
  const auto bearer = admitted(descriptor_json(families[2], custom, R"(,"auth":"bearer")"));
  CHECK(plain && bearer && plain->headers() == bearer->headers());
  // Only the three schemes, as lower-case tokens; the value is never repeated in the refusal.
  for (const std::string_view value : {R"("Bearer")", R"("basic")", R"("")", R"("x-api-key ")", R"("authorization")", R"("test-key-0001")",
                                       R"(7)", R"(null)", R"(["bearer"])", R"({"scheme":"bearer"})"})
    refused(descriptor_json(families[2], custom, R"(,"auth":)" + std::string(value)), "/connection/auth");
}

// A custom policy states the scheme per origin; the first fact that matches the descriptor's origin decides.
void policy_scheme() {
  const auto generate = loaded_policy(edited("google.generate", "auth", R"([{"origins":["http://127.0.0.1:9"],"scheme":"bearer"},{"scheme":"x-goog-api-key"}])"));
  CHECK(generate != nullptr);
  if (generate) {
    const auto here = admitted(descriptor_json(families[3], loopback), generate);
    const auto there = admitted(descriptor_json(families[3], other_loopback), generate);
    CHECK(here && sends_chat(*here, "bearer"));
    CHECK(there && sends_chat(*there, "x-goog-api-key"));
    CHECK(sends_chat(descriptor::presets::google_generate({.policy = generate}), "x-goog-api-key"));
    // The other families keep their facts.
    const auto chat = admitted(descriptor_json(families[0], loopback), generate);
    CHECK(chat && sends_chat(*chat, "bearer"));
    // The descriptor's own statement wins over the policy's.
    const auto stated = admitted(descriptor_json(families[3], loopback, R"(,"auth":"x-api-key")"), generate);
    CHECK(stated && sends_chat(*stated, "x-api-key"));
  }
  const auto messages = loaded_policy(edited("anthropic.messages", "auth", R"([{"origins":["https://llm.example.com","http://127.0.0.1:9"],"scheme":"bearer"},{"scheme":"x-api-key"}])"));
  CHECK(messages != nullptr);
  if (messages) {
    const auto gateway = admitted(descriptor_json(families[2], custom), messages);
    CHECK(gateway && sends_chat(*gateway, "bearer"));
    CHECK(sends_chat(descriptor::presets::anthropic_messages({.policy = messages}), "x-api-key"));
  }
  // An origin-scoped fact for every family at once.
  const auto every = loaded_policy(edited({}, "auth", R"([{"origins":["https://llm.example.com"],"scheme":"x-goog-api-key"},{"scheme":"bearer"}])"));
  CHECK(every != nullptr);
  if (every)
    for (const auto& f : families) {
      const auto at_custom = admitted(descriptor_json(f, custom), every);
      const auto at_loopback = admitted(descriptor_json(f, loopback), every);
      CHECK(at_custom && sends_chat(*at_custom, "x-goog-api-key"));
      CHECK(at_loopback && sends_chat(*at_loopback, "bearer"));
    }
}

// The loader holds the facts to their grammar: origins as the cache facts take them, one of the three schemes, and a
// last fact for every origin.
void policy_grammar() {
  const auto messages = [](std::string_view value) { return edited("anthropic.messages", "auth", value); };
  policy_refused(messages(R"({"scheme":"x-api-key"})"), "/auth");
  policy_refused(messages(R"([])"), "/auth");
  policy_refused(messages(R"(["x-api-key"])"), "/auth");
  policy_refused(messages(R"([{}])"), "/auth");
  policy_refused(messages(R"([{"scheme":"x-api-key","header":"x-api-key"}])"), "/auth");
  policy_refused(messages(R"([{"scheme":"Bearer"}])"), "/auth/scheme");
  policy_refused(messages(R"([{"scheme":"basic"}])"), "/auth/scheme");
  policy_refused(messages(R"([{"scheme":"test-key-0001"}])"), "/auth/scheme");
  policy_refused(messages(R"([{"scheme":7}])"), "/auth/scheme");
  policy_refused(messages(R"([{"origins":[],"scheme":"bearer"},{"scheme":"x-api-key"}])"), "/auth/origins");
  policy_refused(messages(R"([{"origins":["https://llm.example.com/v1"],"scheme":"bearer"},{"scheme":"x-api-key"}])"), "/auth/origins");
  policy_refused(messages(R"([{"origins":["https://llm.example.com/"],"scheme":"bearer"},{"scheme":"x-api-key"}])"), "/auth/origins");
  policy_refused(messages(R"([{"origins":["http://llm.example.com"],"scheme":"bearer"},{"scheme":"x-api-key"}])"), "/auth/origins");
  policy_refused(messages(R"([{"origins":["https://llm.example.com","https://llm.example.com"],"scheme":"bearer"},{"scheme":"x-api-key"}])"), "/auth/origins");
  policy_refused(messages(R"([{"origins":["https://llm.example.com"],"scheme":"bearer"},{"origins":["https://llm.example.com"],"scheme":"x-api-key"},{"scheme":"x-api-key"}])"), "/auth/origins");
  // The fact for every origin is the last one, and there is one.
  policy_refused(messages(R"([{"scheme":"x-api-key"},{"origins":["https://llm.example.com"],"scheme":"bearer"}])"), "/auth");
  policy_refused(messages(R"([{"scheme":"x-api-key"},{"scheme":"bearer"}])"), "/auth");
  policy_refused(messages(R"([{"origins":["https://llm.example.com"],"scheme":"bearer"}])"), "/auth");
  std::string many = "[";
  for (int i = 0; i < 16; ++i) many += R"({"origins":["https://h)" + std::to_string(i) + R"(.example.com"],"scheme":"bearer"},)";
  policy_refused(messages(many + R"({"scheme":"x-api-key"}])"), "/auth");
  // Every scheme on every family, and up to sixteen facts.
  for (const auto& f : families)
    for (const auto scheme : schemes)
      CHECK(loaded_policy(edited(f.family, "auth", R"([{"scheme":)" + json::quote(scheme) + "}]")) != nullptr);
  std::string sixteen = "[";
  for (int i = 0; i < 15; ++i) sixteen += R"({"origins":["https://h)" + std::to_string(i) + R"(.example.com"],"scheme":"bearer"},)";
  CHECK(loaded_policy(messages(sixteen + R"({"scheme":"x-api-key"}])")) != nullptr);
}

// With a key, a descriptor's literal header that is any of the three credential headers is refused, so the key is
// never sent twice. Authorization is refused when the descriptor loads; the others when the client is made.
std::optional<descriptor::ConfigError> client_refusal(const descriptor::ValidatedDescriptor& d, std::string_view api_key) {
  runtime::Options options;
  options.api_key = std::string(api_key);
  try {
    auto client = runtime::detail::ClientAccess::make(d, options, std::make_shared<QueueExecutor>(), std::make_shared<ScriptTransport>());
  } catch (const descriptor::ConfigError& error) {
    return error;
  }
  return std::nullopt;
}
void literal_credential_headers() {
  for (const auto& f : families) {
    for (const std::string_view name : {"x-goog-api-key", "X-Goog-Api-Key", "X-GOOG-API-KEY", "x-api-key", "X-Api-Key", "X-API-KEY"}) {
      const auto d = admitted(descriptor_json(f, loopback, R"(,"headers":{)" + json::quote(name) + R"(:"test-key-0002"})"));
      CHECK(d.has_value());
      if (!d) continue;
      const auto refusal = client_refusal(*d, key);
      CHECK(refusal && refusal->pointer == "/api_key");
      if (refusal) {
        clean(*refusal);
        CHECK(refusal->message.find("test-key-0002") == std::string::npos && refusal->expected.find("test-key-0002") == std::string::npos);
      }
      // Without a key the header is the only credential, sent as written.
      CHECK(!client_refusal(*d, ""));
      const auto call = sent(*d, request_for(f.family), "");
      CHECK(call.reached && call.credentials.size() == 1 && call.credentials.front() == (Header{std::string(name), "test-key-0002"}));
    }
    for (const std::string_view name : {"authorization", "Authorization", "AUTHORIZATION"})
      refused(descriptor_json(f, loopback, R"(,"headers":{)" + json::quote(name) + R"(:"Bearer test-key-0002"})"), "/connection/headers");
  }
  // A deployment header goes through the same check.
  const auto deployed = admitted_result(descriptor::load_with_deployment_headers(descriptor_json(families[3], loopback), {{"x-goog-api-key", "test-key-0002"}}, {}, {}));
  CHECK(deployed && client_refusal(*deployed, key));
  const auto spaced = admitted(descriptor_json(families[0], loopback, R"(,"headers":{"x-api-keys":"value"})"));
  CHECK(spaced && !client_refusal(*spaced, key));
}

// Endpoint calls and realtime sessions send the header of their endpoint fact, whatever the descriptor states.
void endpoint_and_realtime_headers() {
  const auto openai = descriptor::presets::openai_chat(), anthropic = descriptor::presets::anthropic_messages();
  const auto google = descriptor::presets::google_generate(), router = descriptor::presets::openrouter_chat();
  CHECK(sends(sent(openai, embeddings::Request{"text-embedding-3-small", {{"a"}}}), "bearer", openai.base_url()));
  CHECK(sends(sent(google, embeddings::Request{"gemini-embedding-2", {{"a"}}}), "x-goog-api-key", google.base_url()));
  CHECK(sends(sent(anthropic, files::Get{"file_a"}), "x-api-key", anthropic.base_url()));
  CHECK(sends(sent(router, rerank::Request{"cohere/rerank-v3.5", "q", {"a", "b"}}), "bearer", router.base_url()));
  const auto stated = admitted(descriptor_json(families[2], "https://api.anthropic.com", R"(,"auth":"bearer")"));
  CHECK(stated && sends(sent(*stated, files::Get{"file_a"}), "x-api-key", "https://api.anthropic.com"));
  realtime::Setup setup;
  setup.model = "test-realtime-model";
  const auto connect = [&](const descriptor::ValidatedDescriptor& d) -> std::vector<Header> {
    auto connection = realtime::connect(d, setup, key);
    CHECK(std::holds_alternative<realtime::Connection>(connection));
    if (!std::holds_alternative<realtime::Connection>(connection)) return {};
    std::vector<Header> credentials;
    const auto& headers = std::get<realtime::Connection>(connection).headers;
    for (const auto& header : headers) if (credential_name(header.first)) credentials.push_back(header);
    // The credential is the first header of the handshake.
    CHECK(!headers.empty() && credential_name(headers.front().first));
    return credentials;
  };
  CHECK(connect(google) == std::vector<Header>{expected("x-goog-api-key")});
  CHECK(connect(openai) == std::vector<Header>{expected("bearer")});
  const auto stated_google = admitted(descriptor_json(families[3], "https://generativelanguage.googleapis.com", R"(,"auth":"bearer")"));
  CHECK(stated_google && connect(*stated_google) == std::vector<Header>{expected("x-goog-api-key")});
}

// The queries a host asks instead of keeping its own table of headers.
void queries() {
  const auto builtin = descriptor::builtin_policy();
  for (const auto& f : families) {
    CHECK(builtin->auth_scheme(f.family, loopback) == f.scheme);
    CHECK(builtin->auth_scheme(f.family, "https://api.anthropic.com") == f.scheme);
  }
  CHECK(builtin->auth_scheme("zz.unknown", loopback).empty());
  const auto plain = admitted(descriptor_json(families[2], custom));
  const auto stated = admitted(descriptor_json(families[2], custom, R"(,"auth":"bearer")"));
  CHECK(plain && plain->auth().empty() && descriptor::auth_scheme(*plain) == "x-api-key");
  CHECK(stated && stated->auth() == "bearer" && descriptor::auth_scheme(*stated) == "bearer");
  CHECK(descriptor::credential_header("bearer", key) == expected("bearer"));
  CHECK(descriptor::credential_header("x-api-key", key) == expected("x-api-key"));
  CHECK(descriptor::credential_header("x-goog-api-key", key) == expected("x-goog-api-key"));
  CHECK(!descriptor::credential_header("basic", key) && !descriptor::credential_header("", key) && !descriptor::credential_header("Bearer", key));
  for (const std::string_view name : {"authorization", "Authorization", "X-API-KEY", "x-goog-api-key", "X-Goog-Api-Key"})
    CHECK(descriptor::credential_header_name(name));
  for (const std::string_view name : {"proxy-authorization", "x-api-keys", "x-goog-api", "", "bearer"})
    CHECK(!descriptor::credential_header_name(name));
  CHECK(descriptor::credential_scheme_names() == "bearer, x-api-key or x-goog-api-key");
  CHECK(descriptor::credential_schemes().size() == 3 && descriptor::credential_scheme("x-api-key") == &descriptor::credential_schemes()[1]);
  CHECK(!descriptor::credential_scheme("X-API-KEY"));
  const auto old = loaded_policy(edited({}, "auth", {}));
  CHECK(old && old->auth_scheme("anthropic.messages", custom) == "x-api-key" && old->auth_scheme("openai.chat", custom) == "bearer");
}

// With a key, a scheme that names no header refuses the call before it is sent, with a fixed text and a reason;
// without a key no credential is sent and nothing is refused.
void key_never_dropped() {
  for (const std::string_view scheme : {"", "basic", "Bearer", "authorization"}) {
    const auto refused = endpoints::credential(scheme, key);
    const auto* error = std::get_if<Error>(&refused);
    CHECK(error && error->kind == ErrorKind::InvalidConfig && error->reason == "credential.scheme.no_header");
    if (error) {
      clean(*error);
      CHECK(scheme.empty() || error->safe_message.find(scheme) == std::string::npos);
    }
    const auto keyless = endpoints::credential(scheme, "");
    CHECK(std::holds_alternative<std::optional<Header>>(keyless) && !std::get<std::optional<Header>>(keyless));
  }
  for (const auto scheme : schemes) {
    const auto header = endpoints::credential(scheme, key);
    CHECK(std::holds_alternative<std::optional<Header>>(header) && std::get<std::optional<Header>>(header) == expected(scheme));
    const auto keyless = endpoints::credential(scheme, "");
    CHECK(std::holds_alternative<std::optional<Header>>(keyless) && !std::get<std::optional<Header>>(keyless));
  }
}

// An endpoint call sends its endpoint fact's header even where the family's `auth` facts name another scheme for the
// descriptor's origin.
void endpoint_fact_over_family_facts() {
  const auto messages = loaded_policy(edited("anthropic.messages", "auth", R"([{"origins":["https://api.anthropic.com"],"scheme":"bearer"},{"scheme":"x-api-key"}])"));
  CHECK(messages != nullptr);
  if (messages) {
    const auto anthropic = descriptor::presets::anthropic_messages({.policy = messages});
    CHECK(sends_chat(anthropic, "bearer"));
    CHECK(sends(sent(anthropic, files::Get{"file_a"}), "x-api-key", anthropic.base_url()));
  }
  const auto chat = loaded_policy(edited("openai.chat", "auth", R"([{"origins":["https://api.openai.com"],"scheme":"x-goog-api-key"},{"scheme":"bearer"}])"));
  CHECK(chat != nullptr);
  if (chat) {
    const auto openai = descriptor::presets::openai_chat({.policy = chat});
    CHECK(sends_chat(openai, "x-goog-api-key"));
    CHECK(sends(sent(openai, files::Get{"file-a"}), "bearer", openai.base_url()));
    CHECK(sends(sent(openai, embeddings::Request{"text-embedding-3-small", {{"a"}}}), "bearer", openai.base_url()));
  }
}

// `connection.auth` is not part of what sealed history binds: the native origin digest and the archive binding of a
// descriptor that states a scheme are those of the same descriptor without it.
std::string bound(const descriptor::ValidatedDescriptor& d) {
  const auto origin = native_testing::origin_digest_of(d);
  CHECK(origin.has_value());
  std::string out = origin ? archive_internal::hex(*origin) : std::string();
  try {
    const auto limits = archive_internal::resolve_custody_limits({}, d).archive;
    out += "/" + archive_internal::hex(archive_internal::descriptor_binding_digest(d, limits));
  } catch (const archive_fs::Rejected&) {
    CHECK(false);
  }
  return out;
}
void scheme_is_not_bound() {
  for (const auto& f : families)
    for (const auto origin : {loopback, custom})
      for (const auto scheme : schemes) {
        const auto plain = admitted(descriptor_json(f, origin));
        const auto stated = admitted(descriptor_json(f, origin, R"(,"auth":)" + json::quote(scheme)));
        CHECK(plain && stated && bound(*plain) == bound(*stated));
      }
  const auto preset = admitted(descriptor_json(families[2], "https://api.anthropic.com"));
  const auto preset_bearer = admitted(descriptor_json(families[2], "https://api.anthropic.com", R"(,"auth":"bearer")"));
  CHECK(preset && preset_bearer && bound(*preset) == bound(*preset_bearer));
}

// A base URL may spell its origin with upper-case host letters, the scheme's default port or a trailing slash. The
// descriptor holds the origin's serialization (RFC 6454: lower-case host, no default port, no slash), so every fact
// the policy scopes to the origin applies to it as to the canonical spelling, and sealed history binds the same
// digests: those the identity test pins for the preset.
struct SpellingRow {
  bool loads = false, gateway = false;
  std::string base_url, chat_header, embeddings_header, bound;
};
std::string header_name(const Sent& call) {
  if (!call.reached) return "<not sent>";
  return call.credentials.empty() ? "<none>" : call.credentials.front().first;
}
template <class Embed>
SpellingRow spelling_row(const descriptor::LoadResult& loaded, const Embed& embed) {
  SpellingRow row;
  const auto* d = std::get_if<descriptor::ValidatedDescriptor>(&loaded);
  if (!d) return row;
  row.loads = true;
  row.base_url = std::string(d->base_url());
  row.gateway = descriptor::routed_gateway(*d);
  row.chat_header = header_name(sent(*d, request_for(d->family())));
  row.embeddings_header = header_name(sent(*d, embed));
  row.bound = bound(*d);
  return row;
}
// One line per spelling that does not behave as the canonical one; it names header names only, never a key.
void print_row(std::string_view label, std::string_view spelling, const SpellingRow& row) {
  std::fprintf(stderr, "ORIGIN ROW %.*s %.*s: loads=%d base_url=%s chat_header=%s gateway=%d embeddings_header=%s bound=%s\n",
               static_cast<int>(label.size()), label.data(), static_cast<int>(spelling.size()), spelling.data(), row.loads,
               row.base_url.c_str(), row.chat_header.c_str(), row.gateway, row.embeddings_header.c_str(), row.bound.c_str());
}
bool same_row(const SpellingRow& a, const SpellingRow& b) {
  return a.loads == b.loads && a.gateway == b.gateway && a.base_url == b.base_url && a.chat_header == b.chat_header &&
         a.embeddings_header == b.embeddings_header && a.bound == b.bound;
}
void origin_spellings() {
  const auto router_embed = embeddings::Request{"openai/text-embedding-3-small", {{"a"}}};
  const auto google_embed = embeddings::Request{"gemini-embedding-2", {{"a"}}};
  const auto router = [&](std::string_view base, descriptor::PolicySnapshot policy = {}) {
    return spelling_row(descriptor::presets::try_openrouter_chat({.base_url = std::string(base), .policy = std::move(policy)}), router_embed);
  };
  const auto google = [&](std::string_view base) {
    return spelling_row(descriptor::presets::try_google_generate({.base_url = std::string(base)}), google_embed);
  };
  // The pins of policy_identity_test for presets::openrouter_chat() and presets::google_generate().
  constexpr std::string_view router_pin = "b08080a4a008a2b0dcb64075383432de713057c48aacf23c90705d365510311d/"
                                          "29c45ed29b11c14b814b44832bb705e550d7bcf88ecacc9d32bbe9eaffa011ea";
  constexpr std::string_view google_pin = "32c10d13950ef6ce9f030093918ce74d46391c66183b62f794bb47a5ac28920e/"
                                          "c9fd48ef451c060feef5d1ea14829e02dc6158438b1116b12fda7507e6b98d9d";
  // A chat call at OpenRouter sends the key in x-api-key where a policy scopes that scheme to the origin.
  const auto scoped = loaded_policy(edited("openai.chat", "auth", R"([{"origins":["https://openrouter.ai"],"scheme":"x-api-key"},{"scheme":"bearer"}])"));
  CHECK(scoped != nullptr);
  const auto canonical_router = router("https://openrouter.ai"), canonical_google = google("https://generativelanguage.googleapis.com");
  const auto canonical_scoped = router("https://openrouter.ai", scoped);
  CHECK(canonical_router.loads && canonical_router.gateway && canonical_router.chat_header == "Authorization" &&
        canonical_router.embeddings_header == "Authorization" && canonical_router.bound == router_pin);
  CHECK(canonical_google.loads && !canonical_google.gateway && canonical_google.chat_header == "x-goog-api-key" &&
        canonical_google.embeddings_header == "x-goog-api-key" && canonical_google.bound == google_pin);
  CHECK(canonical_scoped.loads && canonical_scoped.chat_header == "x-api-key");
  for (const std::string_view spelling : {"https://OpenRouter.ai", "https://OPENROUTER.AI", "https://openrouter.ai:443",
                                          "https://openrouter.ai:0443", "https://openrouter.ai/", "https://OpenRouter.AI:443/"}) {
    const auto row = router(spelling), scoped_row = router(spelling, scoped);
    if (!same_row(row, canonical_router)) print_row("openrouter", spelling, row);
    if (!same_row(scoped_row, canonical_scoped)) print_row("openrouter-scoped-auth", spelling, scoped_row);
    CHECK(same_row(row, canonical_router));
    CHECK(same_row(scoped_row, canonical_scoped));
  }
  for (const std::string_view spelling : {"https://GenerativeLanguage.googleapis.com", "https://generativelanguage.googleapis.com:443",
                                          "https://generativelanguage.googleapis.com/", "https://GENERATIVELANGUAGE.GOOGLEAPIS.COM:443/"}) {
    const auto row = google(spelling);
    if (!same_row(row, canonical_google)) print_row("gemini", spelling, row);
    CHECK(same_row(row, canonical_google));
  }
  // Only the scheme's own default port is dropped, and a port is written as its number.
  const auto base = [](std::string_view spelling) -> std::string {
    auto loaded = descriptor::presets::try_openai_chat({.base_url = std::string(spelling)});
    const auto* d = std::get_if<descriptor::ValidatedDescriptor>(&loaded);
    return d ? std::string(d->base_url()) : std::string("<refused>");
  };
  const std::pair<std::string_view, std::string_view> spellings[]{
      {"https://openrouter.ai:80", "https://openrouter.ai:80"}, {"https://openrouter.ai:8443", "https://openrouter.ai:8443"},
      {"https://openrouter.ai:08443", "https://openrouter.ai:8443"}, {"http://127.0.0.1:443", "http://127.0.0.1:443"},
      {"http://127.0.0.1:80", "http://127.0.0.1"}, {"http://127.0.0.1:080/", "http://127.0.0.1"},
      {"http://LocalHost:9", "http://localhost:9"}, {"http://[::1]:80", "http://[::1]"},
      {"http://[0:0:0:0:0:0:0:1]:9", "http://[::1]:9"}, {"http://[::0001]", "http://[::1]"}, {"http://[0::1]:80", "http://[::1]"},
      {"https://[FE80::A1]:443", "https://[fe80::a1]"}, {"https://[2001:db8:0:0:0:0:0:1]", "https://[2001:db8::1]"},
      {"https://[2001:DB8::1]", "https://[2001:db8::1]"}, {"https://[2001:0DB8:0000::0001]:443", "https://[2001:db8::1]"},
      {"https://[2001:db8:0:0:1:0:0:1]", "https://[2001:db8::1:0:0:1]"}, {"https://[2001:db8:0:1:1:1:1:1]", "https://[2001:db8:0:1:1:1:1:1]"},
      {"https://[1:0:0:2:0:0:0:3]", "https://[1:0:0:2::3]"}, {"https://[0:0:0:0:0:0:0:0]", "https://[::]"},
      {"http://[0:0:0:0:0:0:0:2]", "<refused>"},
      {"https://Api.Example.com:65535", "https://api.example.com:65535"}, {"HTTPS://api.example.com", "<refused>"},
      {"Https://api.example.com", "<refused>"}, {"https://api.example.com:0", "<refused>"},
      // A host whose last label is a number (decimal digits, or 0x and hex digits) is read as an IPv4 address by a URL
      // parser or a resolver, whatever its other labels are; only the dotted-decimal literal is taken as one.
      {"https://0x7f000001", "<refused>"}, {"https://0x7f.0.0.1", "<refused>"}, {"https://2130706433", "<refused>"},
      {"https://127.1", "<refused>"}, {"https://example.0x10", "<refused>"}, {"http://0x7f000001", "<refused>"},
      {"http://0x7f000001:8080", "<refused>"}, {"https://example.0X1F", "<refused>"}, {"https://example.0x", "<refused>"},
      {"https://example.123", "<refused>"}, {"https://a.0.0x7f000001", "<refused>"}, {"https://localhost.0x1", "<refused>"},
      {"https://127.0.0.1", "https://127.0.0.1"}, {"http://127.0.0.1", "http://127.0.0.1"},
      {"https://api2.example.com", "https://api2.example.com"}, {"https://a1.b2", "https://a1.b2"},
      {"https://localhost", "https://localhost"}, {"https://1password.com", "https://1password.com"},
      {"https://example.0xg", "https://example.0xg"}, {"https://example.0x1g", "https://example.0x1g"},
      {"https://example.1e5", "https://example.1e5"}, {"https://example.x0", "https://example.x0"},
      {"https://0x7f.example.com", "https://0x7f.example.com"}, {"https://10.example.com", "https://10.example.com"}};
  for (const auto& [spelling, expected_base] : spellings) {
    const auto got = base(spelling);
    if (got != expected_base) std::fprintf(stderr, "ORIGIN BASE %.*s: expected %.*s, got %s\n", static_cast<int>(spelling.size()), spelling.data(),
                                           static_cast<int>(expected_base.size()), expected_base.data(), got.c_str());
    CHECK(got == expected_base);
  }
  // The function itself.
  CHECK(descriptor::canonical_origin("").empty() && !descriptor::canonical_origin_spelling(""));
  CHECK(descriptor::canonical_origin("ftp://a.com").empty() && !descriptor::canonical_origin_spelling("ftp://a.com"));
  CHECK(descriptor::canonical_origin("https://a.com/path").empty() && !descriptor::canonical_origin_spelling("https://a.com/path"));
  CHECK(descriptor::canonical_origin("https://a.com/") == "https://a.com" && !descriptor::canonical_origin_spelling("https://a.com/"));
  CHECK(descriptor::canonical_origin("https://A.com:443") == "https://a.com" && !descriptor::canonical_origin_spelling("https://A.com:443"));
  CHECK(descriptor::canonical_origin("https://a.com:08443") == "https://a.com:8443" && descriptor::canonical_origin_spelling("https://a.com:8443"));
  CHECK(descriptor::canonical_origin_spelling("https://a.com") && descriptor::canonical_origin_spelling("http://[::1]:9"));
  CHECK(!descriptor::canonical_origin_spelling("http://[0:0:0:0:0:0:0:1]:9"));
  // The policy and the catalogue admit an origin through the same check as a descriptor's base_url.
  for (const std::string_view spelling : {"https://0x7f000001", "https://0x7f.0.0.1", "https://2130706433", "https://127.1", "https://example.0x10",
                                          "http://0x7f000001"})
    CHECK(!descriptor::valid_origin(spelling) && descriptor::canonical_origin(spelling).empty() && !descriptor::canonical_origin_spelling(spelling));
  for (const std::string_view spelling : {"https://127.0.0.1", "https://api2.example.com", "https://a1.b2", "https://localhost"})
    CHECK(descriptor::valid_origin(spelling) && descriptor::canonical_origin_spelling(spelling));
  // A catalogue entry scoped to a provider's origin matches a descriptor that spells the origin another way.
  const auto builtin = descriptor::builtin_catalogue();
  const descriptor::CatalogueEntry* scoped_entry = nullptr;
  for (const auto& entry : builtin->entries())
    if (!scoped_entry && entry.origins == std::vector<std::string>{"https://api.anthropic.com"}) scoped_entry = &entry;
  CHECK(scoped_entry != nullptr);
  if (scoped_entry) {
    const auto model = scoped_entry->prefix ? scoped_entry->id + "test" : scoped_entry->id;
    for (const std::string_view spelling : {"https://api.anthropic.com", "https://API.Anthropic.com:443/"}) {
      auto loaded = descriptor::presets::try_anthropic_messages({.base_url = std::string(spelling)});
      const auto* d = std::get_if<descriptor::ValidatedDescriptor>(&loaded);
      const auto match = d ? d->policy()->model(d->family(), d->base_url(), model) : descriptor::ModelMatch{};
      CHECK(match.entry && match.entry->id == scoped_entry->id && match.entry->origins == scoped_entry->origins);
    }
    // Asked directly with another spelling, the query matches nothing scoped: its origin must be canonical.
    const auto raw = builtin->find("anthropic.messages", "https://API.anthropic.com", model);
    CHECK(!raw.entry || raw.entry->origins.empty());
  }
  // A realtime session opens at the canonical origin.
  realtime::Setup live;
  live.model = "test-realtime-model";
  const auto wss = [&](std::string_view spelling) -> std::string {
    auto loaded = descriptor::presets::try_openai_chat({.base_url = std::string(spelling)});
    const auto* d = std::get_if<descriptor::ValidatedDescriptor>(&loaded);
    if (!d) return "<refused>";
    auto connection = realtime::connect(*d, live, key);
    const auto* opened = std::get_if<realtime::Connection>(&connection);
    return opened ? opened->url : std::string("<not opened>");
  };
  const auto canonical_wss = wss("https://api.openai.com");
  CHECK(canonical_wss.starts_with("wss://api.openai.com/"));
  for (const std::string_view spelling : {"https://api.openai.com:443", "https://API.OpenAI.com:443/"}) CHECK(wss(spelling) == canonical_wss);
  // A port that is not the default keeps the descriptor off the gateway's facts.
  const auto other_port = router("https://openrouter.ai:80");
  CHECK(other_port.loads && !other_port.gateway && other_port.embeddings_header == "<not sent>");
}

// An origin a policy lists is compared with the descriptor's canonical origin, so the policy must list it in that
// spelling: one in any other is refused where it stands, and the embedded policy lists every origin canonically.
constexpr std::string_view canonical_origin_expected = "origin in its canonical spelling: lower-case host, no default port";
void policy_refused_as(const std::string& text, std::string_view pointer, std::string_view expected_text) {
  auto loaded = descriptor::load_policy(text, config_defaults::codec_defaults_json);
  const auto* error = std::get_if<descriptor::ConfigError>(&loaded);
  if (!error || error->pointer != pointer || error->expected != expected_text)
    std::fprintf(stderr, "POLICY ORIGIN expected refusal at %.*s, got %s\n", static_cast<int>(pointer.size()), pointer.data(),
                 error ? (error->pointer + ": " + error->expected).c_str() : "a loaded policy");
  CHECK(error && error->pointer == pointer && error->expected == expected_text);
}
// The embedded policy with every `from` in `member` (of `family`, or of the policy itself when `family` is empty)
// spelled `to`.
std::string respelled(std::string_view family, std::string_view member, std::string_view from, std::string_view to) {
  auto parsed = json::parse(config_defaults::descriptor_policy_json, {config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth});
  const auto root = std::get<json::Document>(parsed).root();
  std::string value;
  if (family.empty()) value = root.get(member).dump();
  for (auto entry : root.get("families").elements())
    if (!family.empty() && entry.get("family").as_string() == family) value = entry.get(member).dump();
  CHECK(value.find(from) != std::string::npos);
  for (auto at = value.find(from); at != std::string::npos; at = value.find(from, at + to.size())) value.replace(at, from.size(), to);
  return family.empty() ? policy_edit::edited({}, member, value) : edited(family, member, value);
}
void policy_origins_are_canonical() {
  CHECK(loaded_policy(std::string(config_defaults::descriptor_policy_json)) != nullptr);
  for (const std::string_view spelling : {"https://OpenRouter.ai", "https://openrouter.ai:443"})
    policy_refused_as(respelled("openai.chat", "openrouter_origins", "https://openrouter.ai", spelling), "/openrouter_origins", canonical_origin_expected);
  policy_refused_as(respelled("openai.chat", "max_completion_tokens_origins", "https://api.openai.com", "https://API.openai.com"),
                    "/max_completion_tokens_origins", canonical_origin_expected);
  policy_refused_as(respelled("anthropic.messages", "betas", "https://api.anthropic.com", "https://api.anthropic.com:443"), "/betas/origins",
                    canonical_origin_expected);
  policy_refused_as(respelled("anthropic.messages", "extensions", "https://api.anthropic.com", "https://Api.Anthropic.com"),
                    "/extensions/origins", canonical_origin_expected);
  policy_refused_as(respelled("openai.chat", "cache", "https://openrouter.ai", "https://OPENROUTER.AI"), "/cache/origins", canonical_origin_expected);
  policy_refused_as(edited("openai.chat", "auth", R"([{"origins":["https://OpenRouter.ai"],"scheme":"x-api-key"},{"scheme":"bearer"}])"),
                    "/auth/origins", canonical_origin_expected);
  policy_refused_as(edited("openai.chat", "routes", R"([{"origins":["https://LLM.example.com"],"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}])"),
                    "/routes/origins", canonical_origin_expected);
  policy_refused_as(respelled({}, "endpoints", "https://generativelanguage.googleapis.com", "https://generativelanguage.googleapis.com:443"),
                    "/endpoints/origins", canonical_origin_expected);
  // The refusals that were there keep their text.
  policy_refused_as(edited("openai.chat", "auth", R"([{"origins":["https://openrouter.ai/"],"scheme":"x-api-key"},{"scheme":"bearer"}])"),
                    "/auth/origins", "canonical origins without path");
  // A canonical origin with a port that is not the default loads.
  CHECK(loaded_policy(edited("openai.chat", "auth", R"([{"origins":["https://openrouter.ai:8443","http://127.0.0.1:443"],"scheme":"x-api-key"},{"scheme":"bearer"}])")) != nullptr);
}

// Each call carries exactly one header of its scheme's name, Authorization included, and none of the other two, in
// any letter case, in every family, whether the scheme is the policy's or the descriptor's.
void one_header_per_name() {
  const auto count = [](const Sent& call, std::string_view name) {
    return std::count_if(call.credentials.begin(), call.credentials.end(), [&](const Header& header) { return lower(header.first) == lower(name); });
  };
  for (const auto& f : families) {
    const auto policy_scheme = admitted(descriptor_json(f, loopback));
    CHECK(policy_scheme.has_value());
    if (policy_scheme) {
      const auto call = sent(*policy_scheme, request_for(f.family));
      for (const auto other : schemes) CHECK(count(call, expected(other).first) == (other == f.scheme ? 1 : 0));
    }
    for (const auto scheme : schemes) {
      const auto stated = admitted(descriptor_json(f, loopback, R"(,"auth":)" + json::quote(scheme)));
      CHECK(stated.has_value());
      if (!stated) continue;
      const auto call = sent(*stated, request_for(f.family));
      CHECK(call.reached);
      for (const auto other : schemes) CHECK(count(call, expected(other).first) == (other == scheme ? 1 : 0));
    }
  }
}
}  // namespace

int main() {
  builtin_headers();
  older_policy_without_facts();
  content_digests();
  route_content_digests();
  extension_price_digests();
  listing_facts();
  descriptor_scheme();
  policy_scheme();
  policy_grammar();
  literal_credential_headers();
  endpoint_and_realtime_headers();
  queries();
  key_never_dropped();
  endpoint_fact_over_family_facts();
  scheme_is_not_bound();
  one_header_per_name();
  origin_spellings();
  policy_origins_are_canonical();
  if (failures) { std::fprintf(stderr, "%d credential checks failed\n", failures); return 1; }
  std::puts("credential headers passed");
  return 0;
}
