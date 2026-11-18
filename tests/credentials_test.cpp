// Which header carries the API key. A chat call sends it the way the descriptor's `connection.auth` names, else the
// way the first of its family's `auth` facts that matches the origin names; an endpoint call and a realtime session
// the way their endpoint fact names. One mapping turns a scheme into a header. A key goes only to the descriptor's
// origin, and never twice.
#include "codecs/chat.h"
#include "codecs/embeddings.h"
#include "codecs/files.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "codecs/messages_request.h"
#include "codecs/realtime.h"
#include "codecs/rerank.h"
#include "codecs/responses_request.h"
#include "crypto/crypto.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include "runtime/client.h"
#include "sp/config_defaults.h"
#include "support/endpoint_fakes.h"

#include <cstdio>
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
}  // namespace

int main() {
  builtin_headers();
  older_policy_without_facts();
  content_digests();
  descriptor_scheme();
  policy_scheme();
  policy_grammar();
  literal_credential_headers();
  endpoint_and_realtime_headers();
  queries();
  if (failures) { std::fprintf(stderr, "%d credential checks failed\n", failures); return 1; }
  std::puts("credential headers passed");
  return 0;
}
