// Policy-admitted extensions (#32): the closed value schema, the policy facts, and every encoder's handling.
#include <algorithm>
#include "codecs/chat.h"
#include "codecs/gemini.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "codecs/messages.h"
#include "codecs/messages_request.h"
#include "codecs/responses.h"
#include "codecs/responses_request.h"
#include "core/native.h"
#include "descriptor/owned_keys.h"
#include "descriptor/policy.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include "sp/config_defaults.h"
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
using namespace sp;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)
json::Document parse(std::string_view text) {
  auto parsed = json::parse(text); CHECK(std::holds_alternative<json::Document>(parsed));
  return std::get<json::Document>(std::move(parsed));
}
std::shared_ptr<const json::Document> owned(std::string_view text) { return std::make_shared<const json::Document>(parse(text)); }
std::variant<descriptor::ValueSchema, descriptor::ConfigError> schema(std::string_view text) {
  const auto doc = parse(text);
  return descriptor::parse_value_schema(doc.root(), "/value");
}
bool conforms(const descriptor::ValueSchema& s, std::string_view value) { const auto doc = parse(value); return !descriptor::check_value(s, doc.root()); }

void value_schemas() {
  auto object = schema(R"({"type":"object","properties":{"mode":{"type":"enum","values":["a","b"]},"count":{"type":"integer","minimum":1,"maximum":5},
      "ratio":{"type":"number","maximum":1.5},"label":{"type":"string","max_length":8},"id":{"type":"string","max_length":32,"token":true},
      "flags":{"type":"array","items":{"type":"boolean"},"max_items":2},"either":{"one_of":[{"type":"null"},{"type":"integer"}]}},"required":["mode"]})");
  CHECK(std::holds_alternative<descriptor::ValueSchema>(object));
  const auto& s = std::get<descriptor::ValueSchema>(object);
  CHECK(conforms(s, R"({"mode":"a","count":5,"ratio":-2.5,"label":"short","id":"vendor/model-1","flags":[true,false],"either":null})"));
  CHECK(conforms(s, R"({"mode":"b","either":3})"));
  for (const auto* bad : {R"({"count":1})", R"({"mode":"c"})", R"({"mode":"a","count":6})", R"({"mode":"a","count":1.5})", R"({"mode":"a","ratio":2})",
                          R"({"mode":"a","label":"too long here"})", R"({"mode":"a","id":"has space"})", R"({"mode":"a","flags":[true,true,true]})",
                          R"({"mode":"a","either":"x"})", R"({"mode":"a","other":1})", R"([])"})
    CHECK(!conforms(s, bad));
  const auto doc = parse(R"({"mode":"a","extra":1})");
  CHECK(descriptor::check_value(s, doc.root()) == std::optional<std::string>("/extra: declared property"));
  for (const auto* bad : {R"({"type":"regex"})", R"({"type":"string"})", R"({"type":"string","max_length":4,"pattern":"x"})",
                          R"({"type":"enum","values":["a","a"]})", R"({"type":"object","properties":{},"required":["x"]})",
                          R"({"one_of":[{"type":"null"}]})", R"({"type":"integer","minimum":3,"maximum":1})", R"({"type":"integer","minimum":1.5})"})
    CHECK(std::holds_alternative<descriptor::ConfigError>(schema(bad)));
  // A map: keys the caller names (tool names) as name tokens, each value of one schema.
  auto map = schema(R"({"type":"map","max_items":2,"values":{"type":"object","properties":{"enabled":{"type":"boolean"}}}})");
  CHECK(std::holds_alternative<descriptor::ValueSchema>(map));
  const auto& m = std::get<descriptor::ValueSchema>(map);
  CHECK(conforms(m, R"({"resolve-library-id":{"enabled":true},"get.docs_2":{}})") && conforms(m, "{}"));
  for (const auto* bad : {R"({"a":{},"b":{},"c":{}})", R"({"has space":{}})", R"({"a":{"enabled":1}})", R"({"a":{"other":true}})", R"([])"})
    CHECK(!conforms(m, bad));
  for (const auto* bad : {R"({"type":"map","values":{"type":"null"}})", R"({"type":"map","max_items":2})", R"({"type":"map","max_items":2,"values":{"type":"null"},"keys":{}})"})
    CHECK(std::holds_alternative<descriptor::ConfigError>(schema(bad)));
  std::string deep = R"({"type":"null"})";
  for (int i = 0; i < 9; ++i) deep = R"({"type":"array","max_items":1,"items":)" + deep + "}";
  CHECK(std::holds_alternative<descriptor::ConfigError>(schema(deep)));
}

// Builds a policy from the built-in one with `fragment` added to a family's object, in place of the member of the same
// name the family already has. The built-in text is minified, so members are found by a scan that tracks strings and
// nesting.
std::variant<descriptor::PolicySnapshot, descriptor::ConfigError> policy_with(std::string_view family, std::string_view fragment) {
  std::string text(config_defaults::descriptor_policy_json);
  const std::string anchor = "\"family\":\"" + std::string(family) + "\"";
  auto at = text.find(anchor); CHECK(at != std::string::npos);
  const auto key = std::string(fragment.substr(0, fragment.find(':') + 1));
  // The family's object opens at nesting depth 2 (root, families array) and its own members sit at depth 3.
  std::size_t open = 0, member = std::string::npos;
  int depth = 0; bool quoted = false;
  for (std::size_t i = 0; i < text.size() && member == std::string::npos; ++i) {
    const char c = text[i];
    if (quoted) { if (c == '\\') ++i; else if (c == '"') quoted = false; continue; }
    if (c == '{' || c == '[') { if (++depth == 3) open = i; }
    else if (c == '}' || c == ']') --depth;
    else if (c == '"') {
      if (depth == 3 && open < at && text.compare(i, key.size(), key) == 0 && (text[i - 1] == '{' || text[i - 1] == ',')) {
        // A member of the family's object: its object closes after the anchor.
        int inner = 0; bool in = false; std::size_t close = open;
        for (; close < text.size(); ++close) {
          const char d = text[close];
          if (in) { if (d == '\\') ++close; else if (d == '"') in = false; continue; }
          if (d == '"') in = true; else if (d == '{' || d == '[') ++inner; else if ((d == '}' || d == ']') && --inner == 0) break;
        }
        if (close > at) member = i;
      }
      quoted = true;
    }
  }
  if (member != std::string::npos) {
    std::size_t end = member + key.size();
    int inner = 0; bool in = false;
    for (; end < text.size(); ++end) {
      const char c = text[end];
      if (in) { if (c == '\\') ++end; else if (c == '"') in = false; continue; }
      if (c == '"') in = true;
      else if (c == '[' || c == '{') ++inner;
      else if (c == ']' || c == '}') { if (inner == 0) { --end; break; } if (--inner == 0) break; }
      else if (c == ',' && inner == 0) { --end; break; }
    }
    // The value and one comma next to it.
    const bool comma_after = end + 1 < text.size() && text[end + 1] == ',';
    if (comma_after) text.erase(member, end + 2 - member);
    else text.erase(member - 1, end + 2 - member);
    at = text.find(anchor);
  }
  text.insert(at, std::string(fragment) + ",");
  return descriptor::load_policy(text, config_defaults::codec_defaults_json);
}
descriptor::PolicySnapshot admitted(std::string_view family, std::string_view fragment) {
  auto loaded = policy_with(family, fragment);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("policy refused at " + error->pointer + ": " + error->expected);
  return std::get<descriptor::PolicySnapshot>(std::move(loaded));
}
void policy_facts() {
  const std::string speed = R"("extensions":[{"key":"speed","origins":["https://api.anthropic.com"],"value":{"type":"enum","values":["standard","fast"]},"replay_eligible":true}])";
  CHECK(admitted("anthropic.messages", speed)->family("anthropic.messages")->extensions.size() == 1);
  // A member the encoder owns is never admitted, in any letter case, nor an alias or provider-held state.
  for (const auto& [family, key] : std::vector<std::pair<std::string, std::string>>{{"anthropic.messages", "model"}, {"anthropic.messages", "MAX_TOKENS"},
           {"openai.chat", "functions"}, {"openai.responses", "conversation"}, {"anthropic.messages", "mcp_servers"}, {"google.generate", "generationConfig"}}) {
    const auto fragment = R"("extensions":[{"key":")" + key + R"(","origins":["https://example.test"],"value":{"type":"boolean"},"replay_eligible":true}])";
    const auto loaded = policy_with(family, fragment);
    CHECK(std::holds_alternative<descriptor::ConfigError>(loaded) && std::get<descriptor::ConfigError>(loaded).pointer == "/extensions/key");
  }
  for (const auto* bad : {R"("extensions":[{"key":"x","origins":[],"value":{"type":"boolean"},"replay_eligible":true}])",
                          R"("extensions":[{"key":"x","origins":["https://example.test/path"],"value":{"type":"boolean"},"replay_eligible":true}])",
                          R"("extensions":[{"key":"x","origins":["https://example.test"],"value":{"type":"string"},"replay_eligible":true}])",
                          R"("extensions":[{"key":"x","origins":["https://example.test"],"value":{"type":"boolean"}}])",
                          R"("extensions":[{"key":"x","origins":["https://a.test"],"value":{"type":"boolean"},"replay_eligible":true},{"key":"x","origins":["https://b.test"],"value":{"type":"boolean"},"replay_eligible":true}])"})
    CHECK(std::holds_alternative<descriptor::ConfigError>(policy_with("anthropic.messages", bad)));
  // Gateway facts belong to families with declared OpenRouter origins, and plugin options never name `id`.
  CHECK(std::holds_alternative<descriptor::ConfigError>(policy_with("google.generate", R"("openrouter_plugins":[{"id":"web"}])")));
  CHECK(std::holds_alternative<descriptor::ConfigError>(policy_with("openai.chat", R"("openrouter_server_tools":[{"type":"web"}])")));
  CHECK(descriptor::builtin_policy()->family("openai.chat")->openrouter_plugins.size() > 5);
  // Messages tool facts (#53): a tool the caller runs takes no use limit, a fact's beta is one of the family's betas,
  // and options never name a member the encoder types. Beta facts belong to Messages.
  auto refused_at = [](std::string_view family, std::string_view fragment, std::string_view pointer) {
    const auto loaded = policy_with(family, fragment);
    CHECK(std::holds_alternative<descriptor::ConfigError>(loaded) && std::get<descriptor::ConfigError>(loaded).pointer == pointer);
  };
  // The built-in betas plus `extra`, without `dropped`.
  auto betas_with = [](std::string_view extra, std::string_view dropped = {}) {
    std::string fragment = R"("betas":[)";
    for (const auto& beta : descriptor::builtin_policy()->family("anthropic.messages")->betas)
      if (beta.value != dropped) fragment += R"({"value":")" + beta.value + R"(","origins":["https://api.anthropic.com"]},)";
    return fragment + R"({"value":")" + std::string(extra) + R"(","origins":["https://api.anthropic.com"]}])";
  };
  const auto builtin_betas = descriptor::builtin_policy()->family("anthropic.messages")->betas.size();
  CHECK(admitted("anthropic.messages", betas_with("fixture-beta-2026-01-01"))->family("anthropic.messages")->betas.size() == builtin_betas + 1);
  // The built-in advisor fact names its beta, so dropping that beta refuses the policy.
  refused_at("anthropic.messages", betas_with("fixture-beta-2026-01-01", "advisor-tool-2026-03-01"), "/server_tools/beta");
  refused_at("anthropic.messages", betas_with("fixture-beta-2026-01-01", "mcp-client-2026-09-15"), "/response_blocks/beta");
  // Compaction (#53): a signed block's beta and an extension's beta are betas of the family, and only Messages
  // extensions name one.
  refused_at("anthropic.messages", betas_with("fixture-beta-2026-01-01", "compact-2026-09-04"), "/response_blocks/signed_beta");
  refused_at("anthropic.messages", R"("response_blocks":[{"type":"compaction","opens_history":"yes"}])", "/response_blocks/opens_history");
  refused_at("anthropic.messages", R"("extensions":[{"key":"compaction","origins":["https://api.anthropic.com"],"value":{"type":"null"},"replay_eligible":true,"bound":false,"beta":"fixture-missing"}])", "/extensions/beta");
  refused_at("openai.chat", R"("extensions":[{"key":"fixture","origins":["https://api.openai.com"],"value":{"type":"null"},"replay_eligible":true,"beta":"compact-2026-09-04"}])", "/extensions/beta");
  // Extension prices (#207): each listed value one the fact's schema takes, priced under a catalogue tier or as no
  // catalogue row states; values at the standard rates are listed only beside a price for every other value.
  const auto priced_fact = [](std::string_view prices) {
    return R"("extensions":[{"key":"speed","origins":["https://api.anthropic.com"],"value":{"one_of":[{"type":"enum","values":["standard","fast"]},{"type":"null"}]},"replay_eligible":true,"bound":false,"prices":)" +
           std::string(prices) + "}]";
  };
  for (const auto* good : {R"({})", R"({"values":{"fast":{"tier":"fast"}}})", R"({"values":{"fast":{"unmodeled":true}}})",
                           R"({"standard":["standard"],"other":{"unmodeled":true}})", R"({"other":{"tier":"priority"}})",
                           R"({"values":{"fast":{"tier":"fast"}},"standard":["standard"],"other":{"unmodeled":true}})"})
    CHECK(std::holds_alternative<descriptor::PolicySnapshot>(policy_with("anthropic.messages", priced_fact(good))));
  for (const auto* bad : {R"([])", R"({"values":{"warp":{"tier":"fast"}}})", R"({"values":{"fast":{"tier":"fast","unmodeled":true}}})",
                          R"({"values":{"fast":{}}})", R"({"values":{"fast":{"unmodeled":false}}})", R"({"values":{"fast":{"tier":"has space"}}})",
                          R"({"values":{"fast":{"tier":""}}})", R"({"values":{"fast":"fast"}})", R"({"values":[]})", R"({"standard":["standard"]})",
                          R"({"standard":["warp"],"other":{"unmodeled":true}})", R"({"standard":["standard","standard"],"other":{"unmodeled":true}})",
                          R"({"values":{"fast":{"tier":"fast"}},"standard":["fast"],"other":{"unmodeled":true}})", R"({"other":"fast"})", R"({"extra":1})"})
    refused_at("anthropic.messages", priced_fact(bad), "/extensions/prices");
  {
    // The built-in prices: `speed: fast` under the fast tier; `inference_geo` other than `global` as no row states.
    const auto& facts = descriptor::builtin_policy()->family("anthropic.messages")->extensions;
    const auto of = [&](std::string_view key) {
      const auto found = std::find_if(facts.begin(), facts.end(), [&](const auto& f) { return f.key == key; });
      CHECK(found != facts.end()); return *found;
    };
    const auto speed = of("speed"), geo = of("inference_geo");
    CHECK(speed.prices.values.size() == 1 && speed.prices.values[0].first == "fast" && speed.prices.values[0].second.tier == "fast" &&
          !speed.prices.values[0].second.unmodeled && speed.prices.standard.empty() && !speed.prices.other);
    CHECK(geo.prices.values.empty() && geo.prices.standard == std::vector<std::string>{"global"} && geo.prices.other && geo.prices.other->unmodeled &&
          geo.prices.other->tier.empty());
    const auto fast = parse(R"("fast")"), standard = parse(R"("standard")"), null = parse("null"), us = parse(R"("us")"), global = parse(R"("global")");
    CHECK(descriptor::extension_pricing(speed, fast.root())->tier == "fast");
    CHECK(!descriptor::extension_pricing(speed, standard.root()) && !descriptor::extension_pricing(speed, null.root()));
    CHECK(descriptor::extension_pricing(geo, us.root())->unmodeled);
    CHECK(!descriptor::extension_pricing(geo, global.root()) && !descriptor::extension_pricing(geo, null.root()));
    // Every other fact prices nothing.
    for (const auto& fact : facts)
      if (fact.key != "speed" && fact.key != "inference_geo") CHECK(fact.prices.values.empty() && fact.prices.standard.empty() && !fact.prices.other);
  }
  // Input blocks (#53) belong to Messages, never name a typed block, and declare the whole block in an object schema.
  refused_at("openai.chat", R"("input_blocks":[{"type":"search_result","value":{"type":"object","properties":{"type":{"type":"enum","values":["search_result"]}}}}])", "/input_blocks");
  refused_at("anthropic.messages", R"("input_blocks":[{"type":"text","value":{"type":"object","properties":{"type":{"type":"enum","values":["text"]}}}}])", "/input_blocks/type");
  refused_at("anthropic.messages", R"("input_blocks":[{"type":"widget","value":{"type":"object","properties":{"x":{"type":"null"}}}}])", "/input_blocks/value");
  refused_at("anthropic.messages", R"("input_blocks":[{"type":"widget","value":{"type":"null"}}])", "/input_blocks/value");
  {
    const auto* messages = descriptor::builtin_policy()->family("anthropic.messages");
    const auto compaction = std::find_if(messages->response_blocks.begin(), messages->response_blocks.end(), [](const auto& fact) { return fact.type == "compaction"; });
    CHECK(compaction != messages->response_blocks.end() && compaction->opens_history && compaction->signed_beta == "compact-2026-09-04");
  }
  // Response blocks never name a block the codec types.
  for (const auto* type : {"text", "tool_use", "mcp_tool_use", "web_search_tool_result", "Upper"})
    refused_at("anthropic.messages", std::string(R"("response_blocks":[{"type":")") + type + R"("}])", "/response_blocks/type");
  refused_at("openai.chat", R"("response_blocks":[{"type":"fallback"}])", "/response_blocks");
  refused_at("google.generate", R"("betas":[{"value":"fixture-beta","origins":["https://example.test"]}])", "/betas");
  refused_at("anthropic.messages", R"("betas":[{"value":"Upper-Case","origins":["https://example.test"]}])", "/betas/value");
  refused_at("anthropic.messages", R"("server_tools":[{"type":"memory_20250818","name":"memory","max_uses":true,"client":true}])", "/server_tools/max_uses");
  refused_at("anthropic.messages", R"("server_tools":[{"type":"advisor_20260301","name":"advisor","max_uses":true,"beta":"unlisted-beta"}])", "/server_tools/beta");
  refused_at("anthropic.messages", R"("server_tools":[{"type":"text_editor_20250728","name":"e","max_uses":false,"options":{"type":"object","properties":{"allowed_callers":{"type":"boolean"}}}}])",
             "/server_tools/options");
  refused_at("openai.responses", R"("server_tools":[{"type":"computer","name":"computer","max_uses":false,"client":true}])", "/server_tools");
  const auto& messages = *descriptor::builtin_policy()->family("anthropic.messages");
  auto fact = [&](std::string_view type) {
    const auto found = std::find_if(messages.server_tools.begin(), messages.server_tools.end(), [&](const auto& f) { return f.type == type; });
    CHECK(found != messages.server_tools.end()); return *found;
  };
  CHECK(fact("memory_20250818").client && fact("memory_20250818").name == "memory" && !fact("memory_20250818").options);
  CHECK(fact("computer_toolset_20260801").client && fact("computer_toolset_20260801").name.empty());
  CHECK(!fact("advisor_20260301").client && fact("advisor_20260301").beta == "advisor-tool-2026-03-01");
}

descriptor::ValidatedDescriptor load(std::string_view family, std::string_view base_url, const descriptor::PolicySnapshot& policy) {
  const bool gemini = family == "google.generate";
  const std::string path = gemini ? "/v1beta/models/fixture-model:generateContent" : family == "google.interactions" ? "/v1beta/interactions" :
      family == "anthropic.messages" ? "/v1/messages" : family == "openai.chat" ? "/v1/chat/completions" : "/v1/responses";
  const std::string stream = gemini ? "/v1beta/models/fixture-model:streamGenerateContent?alt=sse" : path;
  auto loaded = descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"extensions\",\"family\":" + json::quote(family) +
      ",\"connection\":{\"base_url\":" + json::quote(base_url) + ",\"paths\":{\"buffered\":" + json::quote(path) + ",\"streaming\":" + json::quote(stream) + "}}}", policy);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("descriptor refused at " + error->pointer);
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
template<class Result> ErrorKind refused(const Result& result) { CHECK(std::holds_alternative<Error>(result)); return std::get<Error>(result).kind; }

// Every family sends an admitted member after the typed ones, refuses anything else, and binds it for replay.
void extensions_on_every_family() {
  const auto fragment = [](std::string_view origin) {
    return R"("extensions":[{"key":"speed","origins":[")" + std::string(origin) + R"("],"value":{"type":"enum","values":["standard","fast"]},"replay_eligible":true},)"
           R"({"key":"trace_note","origins":[")" + std::string(origin) + R"("],"value":{"type":"string","max_length":16},"replay_eligible":false}])";
  };
  struct Case { std::string family, origin; };
  for (const auto& [family, origin] : std::vector<Case>{{"anthropic.messages", "https://api.anthropic.com"}, {"openai.chat", "https://api.openai.com"},
           {"openai.responses", "https://api.openai.com"}, {"google.generate", "https://generativelanguage.googleapis.com"},
           {"google.interactions", "https://generativelanguage.googleapis.com"}}) {
    const auto policy = admitted(family, fragment(origin));
    const auto d = load(family, origin, policy);
    const auto elsewhere = load(family, "https://proxy.example.test", policy);
    auto encode = [&](const descriptor::ValidatedDescriptor& at, std::shared_ptr<const json::Document> extensions) -> std::variant<std::string, Error> {
      auto pick = [](auto encoded) -> std::variant<std::string, Error> {
        if (auto* e = std::get_if<Error>(&encoded)) return *e;
        return std::visit([](auto& r) -> std::string { if constexpr (requires { r.body; }) return r.body; else return {}; }, encoded);
      };
      if (family == "anthropic.messages") { auto r = messages::Request::simple("fixture-model", "hi", 64); r.extensions = extensions; return pick(messages::encode(at, r, false)); }
      if (family == "openai.chat") { auto r = chat::Request::simple("fixture-model", "hi", 64); r.extensions = extensions; return pick(chat::encode(at, r, false)); }
      if (family == "openai.responses") { auto r = responses::Request::simple("fixture-model", "hi", 64); r.extensions = extensions; return pick(responses::encode(at, r, false)); }
      if (family == "google.generate") { auto r = gemini::Request::simple("fixture-model", "hi", 64); r.extensions = extensions; return pick(gemini::encode(at, r, false)); }
      auto r = interactions::Request::simple("fixture-model", "hi", 64); r.extensions = extensions; return pick(interactions::encode(at, r, false));
    };
    const auto sent = encode(d, owned(R"({"speed":"fast"})"));
    CHECK(std::holds_alternative<std::string>(sent));
    CHECK(parse(std::get<std::string>(sent)).root().get("speed").as_string() == "fast");
    CHECK(refused(encode(d, owned(R"({"speed":"warp"})"))) == ErrorKind::InvalidRequest);
    CHECK(refused(encode(d, owned(R"({"unlisted":true})"))) == ErrorKind::Unsupported);
    CHECK(refused(encode(d, owned(R"({"Model":"other"})"))) == ErrorKind::InvalidRequest);
    CHECK(refused(encode(d, owned(R"({"stream":true})"))) == ErrorKind::InvalidRequest);
    CHECK(refused(encode(elsewhere, owned(R"({"speed":"fast"})"))) == ErrorKind::Unsupported);
    CHECK(refused(encode(d, owned(R"([1])"))) == ErrorKind::InvalidRequest);
    CHECK(std::holds_alternative<std::string>(encode(d, owned(R"({"trace_note":"run 7"})"))));
  }

}

// Replay: an eligible extension is configuration; an ineligible one leaves the answer not continuable natively.
void extension_replay() {
  const auto policy = admitted("anthropic.messages", R"("extensions":[{"key":"speed","origins":["https://api.anthropic.com"],"value":{"type":"enum","values":["standard","fast"]},"replay_eligible":true},)"
                                                     R"({"key":"trace_note","origins":["https://api.anthropic.com"],"value":{"type":"string","max_length":16},"replay_eligible":false}])");
  const auto d = load("anthropic.messages", "https://api.anthropic.com", policy);
  auto first = messages::Request::simple("fixture-model", "hi", 64); first.extensions = owned(R"({"speed":"fast"})");
  auto answer = [&](const messages::Request& r) {
    auto encoded = messages::encode(d, r, false); CHECK(std::holds_alternative<messages::EncodedRequest>(encoded));
    Accumulator acc; messages::Codec codec(d, messages::Mode::Buffered, acc, std::get<messages::EncodedRequest>(encoded).context);
    codec.buffered(R"({"id":"m1","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"text","text":"hello"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})", {});
    CHECK(acc.outcome() && std::holds_alternative<Completion>(*acc.outcome()));
    return std::get<Completion>(*acc.outcome()).messages.front();
  };
  auto next = first; next.messages.push_back(answer(first)); next.messages.push_back(Message::user_text("again"));
  CHECK(std::holds_alternative<messages::EncodedRequest>(messages::encode(d, next, false)));
  auto changed = next; changed.extensions = owned(R"({"speed":"standard"})");
  CHECK(refused(messages::encode(d, changed, false)) == ErrorKind::ReplayIneligible);
  changed = next; changed.extensions.reset();
  CHECK(refused(messages::encode(d, changed, false)) == ErrorKind::ReplayIneligible);

  // The answer of a turn with an ineligible extension has no complete native seal.
  auto traced = messages::Request::simple("fixture-model", "hi", 64); traced.extensions = owned(R"({"trace_note":"run 7"})");
  const auto unsealed = answer(traced);
  CHECK(!unsealed.native || !unsealed.native->complete());
  CHECK(answer(first).native && answer(first).native->complete());

  // An unbound value (#96) may change every turn, or be dropped, without breaking the conversation; the bound
  // members next to it still bind.
  const auto loose = admitted("anthropic.messages", R"("extensions":[{"key":"speed","origins":["https://api.anthropic.com"],"value":{"type":"enum","values":["standard","fast"]},"replay_eligible":true},)"
                                                    R"({"key":"diagnostics","origins":["https://api.anthropic.com"],"value":{"type":"object","properties":{"previous_message_id":{"one_of":[{"type":"null"},{"type":"string","max_length":64}]}}},"replay_eligible":true,"bound":false}])");
  const auto ld = load("anthropic.messages", "https://api.anthropic.com", loose);
  auto reply = [&](const messages::Request& r) {
    auto encoded = messages::encode(ld, r, false); CHECK(std::holds_alternative<messages::EncodedRequest>(encoded));
    Accumulator acc; messages::Codec codec(ld, messages::Mode::Buffered, acc, std::get<messages::EncodedRequest>(encoded).context);
    codec.buffered(R"({"id":"m1","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"text","text":"hello"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})", {});
    CHECK(acc.outcome() && std::holds_alternative<Completion>(*acc.outcome()));
    return std::get<Completion>(*acc.outcome()).messages.front();
  };
  auto opening = messages::Request::simple("fixture-model", "hi", 64); opening.extensions = owned(R"({"diagnostics":{"previous_message_id":null}})");
  const auto sealed = reply(opening);
  CHECK(sealed.native && sealed.native->complete());
  auto follow = opening; follow.messages.push_back(sealed); follow.messages.push_back(Message::user_text("again"));
  follow.extensions = owned(R"({"diagnostics":{"previous_message_id":"m1"}})");
  CHECK(std::holds_alternative<messages::EncodedRequest>(messages::encode(ld, follow, false)));
  follow.extensions.reset();
  CHECK(std::holds_alternative<messages::EncodedRequest>(messages::encode(ld, follow, false)));
  // With a bound member beside it, only the bound one decides.
  auto mixed = messages::Request::simple("fixture-model", "hi", 64); mixed.extensions = owned(R"({"speed":"fast","diagnostics":{"previous_message_id":null}})");
  auto mixed_next = mixed; mixed_next.messages.push_back(reply(mixed)); mixed_next.messages.push_back(Message::user_text("again"));
  mixed_next.extensions = owned(R"({"diagnostics":{"previous_message_id":"m1"},"speed":"fast"})");
  CHECK(std::holds_alternative<messages::EncodedRequest>(messages::encode(ld, mixed_next, false)));
  mixed_next.extensions = owned(R"({"speed":"standard","diagnostics":{"previous_message_id":"m1"}})");
  CHECK(refused(messages::encode(ld, mixed_next, false)) == ErrorKind::ReplayIneligible);
  // An unbound value is replay-eligible by definition.
  const auto contradictory = policy_with("anthropic.messages", R"("extensions":[{"key":"diagnostics","origins":["https://api.anthropic.com"],"value":{"type":"null"},"replay_eligible":false,"bound":false}])");
  CHECK(std::holds_alternative<descriptor::ConfigError>(contradictory) && std::get<descriptor::ConfigError>(contradictory).pointer == "/extensions/bound");
  for (const char* key : {"speed", "inference_geo", "diagnostics"}) {
    const auto& facts = descriptor::builtin_policy()->family("anthropic.messages")->extensions;
    const auto fact = std::find_if(facts.begin(), facts.end(), [&](const auto& f) { return f.key == key; });
    CHECK(fact != facts.end() && fact->replay_eligible && !fact->bound);
  }
}

// The table of owned members holds every top-level member the encoders write with their typed controls set, as written,
// and each member it holds as written is written by some request of its family.
void owned_keys_cover_encoder_output() {
  const auto policy = descriptor::builtin_policy();
  std::map<std::string, std::set<std::string>> written;
  auto keys_of = [](const std::string& body) {
    const auto document = parse(body);
    std::vector<std::string> out;
    for (auto m : document.root().members()) out.emplace_back(m.key);
    return out;
  };
  auto check = [&](std::string_view family, const std::string& body) {
    for (const auto& key : keys_of(body)) {
      if (descriptor::member_role(family, key) != descriptor::MemberRole::Written) throw std::runtime_error(std::string(family) + " writes " + key + ", which the table does not hold as written");
      if (!descriptor::request_key_owned(family, key)) throw std::runtime_error(std::string(family) + " writes unowned " + key);
      written[std::string(family)].insert(key);
    }
  };
  {
    const auto d = load("anthropic.messages", "https://api.anthropic.com", policy);
    auto r = messages::Request::simple("fixture-model", "hi", 64, "system"); r.temperature = 0.5; r.top_p = 0.9;
    r.output_effort = messages::OutputEffort::High; r.cache_control = messages::CacheControl{}; r.container = "cntr_1";
    r.tools = {messages::ToolDefinition{"lookup", "d", owned(R"({"type":"object"})"), {}, {}, {}, {}, {}, {}, {}, {}}};
    r.tool_choice = messages::ToolChoice{messages::ToolChoiceMode::Auto, {}, {}};
    for (bool stream : {false, true}) { auto e = messages::encode(d, r, stream); CHECK(std::holds_alternative<messages::EncodedRequest>(e)); check("anthropic.messages", std::get<messages::EncodedRequest>(e).body); }
    auto thinking = messages::Request::simple("fixture-model", "hi", 64);
    thinking.thinking_mode = messages::ThinkingMode::Adaptive;
    auto thought = messages::encode(d, thinking, false); CHECK(std::holds_alternative<messages::EncodedRequest>(thought)); check("anthropic.messages", std::get<messages::EncodedRequest>(thought).body);
    // Routing is for the OpenRouter origin.
    auto routed = messages::Request::simple("fixture-model", "hi", 64); routed.provider = OpenRouterRouting{};
    auto sent = messages::encode(load("anthropic.messages", "https://openrouter.ai", policy), routed, false);
    CHECK(std::holds_alternative<messages::EncodedRequest>(sent)); check("anthropic.messages", std::get<messages::EncodedRequest>(sent).body);
    // MCP servers are named by a toolset entry.
    auto mcp = messages::Request::simple("fixture-model", "hi", 64);
    mcp.mcp_servers = {messages::McpServer{"ctx", "https://mcp.example.test/mcp", std::nullopt}};
    messages::ToolDefinition toolset{"", {}, {}, "mcp_toolset", std::nullopt};
    toolset.options = owned(R"({"mcp_server_name":"ctx"})");
    mcp.tools = {toolset};
    auto e = messages::encode(d, mcp, false); CHECK(std::holds_alternative<messages::EncodedRequest>(e)); check("anthropic.messages", std::get<messages::EncodedRequest>(e).body);
  }
  {
    // OpenRouter keeps `max_tokens`; direct OpenAI names its output cap `max_completion_tokens`.
    const auto d = load("openai.chat", "https://openrouter.ai", policy);
    auto r = chat::Request::simple("fixture-model", "hi", 64); r.temperature = 0.5; r.top_p = 0.9; r.service_tier = "auto";
    r.response_format = ResponseFormat{}; r.reasoning = chat::ReasoningOptions{std::string("low"), {}, {}, {}}; r.include_reasoning = true;
    r.usage_include = true; r.models = {"a/b"}; r.plugins = {{"response-healing", {}}}; r.provider = OpenRouterRouting{};
    r.modalities = {chat::OutputModality::Text, chat::OutputModality::Image}; r.image_config = chat::ImageOutput{};
    r.tools = {chat::ToolDefinition{"lookup", "d", owned(R"({"type":"object"})")}}; r.tool_choice = chat::ToolChoice{chat::ToolChoiceMode::Auto, {}, std::nullopt};
    for (bool stream : {false, true}) { auto e = chat::encode(d, r, stream); CHECK(std::holds_alternative<chat::EncodedRequest>(e)); check("openai.chat", std::get<chat::EncodedRequest>(e).body); }
    const auto direct = load("openai.chat", "https://api.openai.com", policy);
    auto spoken = chat::Request::simple("fixture-model", "hi", 64);
    spoken.modalities = {chat::OutputModality::Text, chat::OutputModality::Audio}; spoken.audio = chat::AudioOutput{"alloy", "wav"}; spoken.reasoning_effort = "low";
    auto e = chat::encode(direct, spoken, false); CHECK(std::holds_alternative<chat::EncodedRequest>(e)); check("openai.chat", std::get<chat::EncodedRequest>(e).body);
  }
  {
    const auto d = load("openai.responses", "https://openrouter.ai", policy);
    auto r = responses::Request::simple("fixture-model", "hi", 64, "instructions"); r.temperature = 0.5; r.top_p = 0.9; r.store = true;
    r.reasoning = responses::ReasoningOptions{std::string("low"), std::string("auto")}; r.service_tier = "auto"; r.max_tool_calls = 3;
    r.parallel_tool_calls = true; r.verbosity = responses::Verbosity::Low; r.truncation = responses::Truncation::Auto; r.response_format = ResponseFormat{};
    r.plugins = {{"response-healing", {}}}; r.modalities = {responses::OutputModality::Image}; r.image_config = OpenRouterImageConfig{};
    r.provider = OpenRouterRouting{}; r.required_tool = "lookup"; r.previous_response_id = "resp_1";
    r.tools = {responses::ToolDefinition{"lookup", "d", owned(R"({"type":"object","properties":{},"additionalProperties":false,"required":[]})"), {}, {}}};
    for (bool stream : {false, true}) { auto e = responses::encode(d, r, stream); CHECK(std::holds_alternative<responses::EncodedRequest>(e)); check("openai.responses", std::get<responses::EncodedRequest>(e).body); }
  }
  {
    const auto d = load("google.generate", "https://generativelanguage.googleapis.com", policy);
    auto r = gemini::Request::simple("fixture-model", "hi", 64, "system"); r.temperature = 0.5;
    r.safety_settings = {gemini::SafetySetting{}}; r.tools = {gemini::ToolDefinition{"lookup", "d", owned(R"({"type":"object"})")}};
    r.tool_choice = gemini::ToolChoice{gemini::ToolChoiceMode::Auto, {}}; r.builtin_tools = {gemini::GoogleSearchTool{}};
    for (bool stream : {false, true}) { auto e = gemini::encode(d, r, stream); CHECK(std::holds_alternative<gemini::EncodedRequest>(e)); check("google.generate", std::get<gemini::EncodedRequest>(e).body); }
  }
  {
    const auto d = load("google.interactions", "https://generativelanguage.googleapis.com", policy);
    auto r = interactions::Request::simple("fixture-model", "hi", 64, "system"); r.store = true; r.service_tier = "standard";
    r.response_format = {interactions::TextResponseFormat{}}; r.builtin_tools = {interactions::UrlContextTool{}};
    for (bool stream : {false, true}) { auto e = interactions::encode(d, r, stream); CHECK(std::holds_alternative<interactions::EncodedRequest>(e)); check("google.interactions", std::get<interactions::EncodedRequest>(e).body); }
    // An agent runs a stored interaction in the background, in place of a model.
    auto agent = interactions::Request::simple("", "hi", 64, "system"); agent.agent = "deep-research"; agent.store = true; agent.background = true; agent.previous_interaction_id = "int_1";
    auto e = interactions::encode(d, agent, false); CHECK(std::holds_alternative<interactions::EncodedRequest>(e)); check("google.interactions", std::get<interactions::EncodedRequest>(e).body);
  }
  // Each member held as written is written by some request, and no other member is written.
  for (const auto family : {"openai.chat", "anthropic.messages", "openai.responses", "google.generate", "google.interactions"}) {
    std::set<std::string> table;
    for (const auto& member : descriptor::owned_members(family)) if (member.role == descriptor::MemberRole::Written) table.insert(std::string(member.name));
    for (const auto& name : table) if (!written[family].contains(name)) std::cerr << family << ": the table holds " << name << " as written, and no test request writes it\n";
    CHECK(!table.empty() && written[family] == table);
  }
}

// OpenRouter plugins and server tools, and generic Responses tools, are admitted by the policy with their schemas.
void admitted_options() {
  const auto policy = descriptor::builtin_policy();
  const auto chat_d = load("openai.chat", "https://openrouter.ai", policy);
  auto chat = chat::Request::simple("fixture-model", "hi", 64);
  chat.plugins = {{"web", owned(R"({"max_results":2})")}};
  CHECK(std::holds_alternative<chat::EncodedRequest>(chat::encode(chat_d, chat, false)));
  chat.plugins = {{"future-plugin", {}}}; CHECK(refused(chat::encode(chat_d, chat, false)) == ErrorKind::Unsupported);
  chat.plugins = {{"web", owned(R"({"max_results":"two"})")}}; CHECK(refused(chat::encode(chat_d, chat, false)) == ErrorKind::InvalidRequest);
  chat.plugins = {{"web", owned(R"({"unlisted":1})")}}; CHECK(refused(chat::encode(chat_d, chat, false)) == ErrorKind::InvalidRequest);
  chat.plugins = {{"moderation", owned(R"({"x":1})")}}; CHECK(refused(chat::encode(chat_d, chat, false)) == ErrorKind::InvalidRequest);
  const auto responses_d = load("openai.responses", "https://openrouter.ai", policy);
  auto r = responses::Request::simple("fixture-model", "hi", 64);
  r.hosted_tools = {responses::OpenRouterServerTool{"openrouter:datetime", {}}};
  CHECK(std::holds_alternative<responses::EncodedRequest>(responses::encode(responses_d, r, false)));
  r.hosted_tools = {responses::OpenRouterServerTool{"openrouter:future", {}}}; CHECK(refused(responses::encode(responses_d, r, false)) == ErrorKind::Unsupported);
  r.hosted_tools = {responses::OpenRouterServerTool{"openrouter:datetime", owned(R"({"unlisted":1})")}}; CHECK(refused(responses::encode(responses_d, r, false)) == ErrorKind::InvalidRequest);
  // A generic tool: a type the SDK has no struct for, admitted with the schema of its members.
  std::string text(config_defaults::descriptor_policy_json);
  const std::string last = R"("type":"computer_use_preview"}])";
  const auto at = text.find(last); CHECK(at != std::string::npos);
  text.replace(at, last.size(), R"("type":"computer_use_preview"},{"generic":true,"max_uses":false,"name":"apply_patch","options":{"type":"object","properties":{"mode":{"type":"enum","values":["diff"]}}},"type":"apply_patch"}])");
  auto loaded = descriptor::load_policy(text, config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(loaded));
  const auto patch_d = load("openai.responses", "https://api.openai.com", std::get<descriptor::PolicySnapshot>(loaded));
  r.hosted_tools = {responses::GenericHostedTool{"apply_patch", owned(R"({"mode":"diff"})")}};
  auto sent = responses::encode(patch_d, r, false); CHECK(std::holds_alternative<responses::EncodedRequest>(sent));
  CHECK(parse(std::get<responses::EncodedRequest>(sent).body).root().get("tools").at(0).get("mode").as_string() == "diff");
  r.hosted_tools = {responses::GenericHostedTool{"apply_patch", owned(R"({"mode":"full"})")}}; CHECK(refused(responses::encode(patch_d, r, false)) == ErrorKind::InvalidRequest);
  r.hosted_tools = {responses::GenericHostedTool{"apply_patch", {}}}; CHECK(std::holds_alternative<responses::EncodedRequest>(responses::encode(patch_d, r, false)));
  r.hosted_tools = {responses::GenericHostedTool{"apply_patch", owned(R"({"mode":"diff"})")}}; CHECK(refused(responses::encode(responses_d, r, false)) == ErrorKind::Unsupported);
  r.hosted_tools = {responses::GenericHostedTool{"web_search", {}}}; CHECK(refused(responses::encode(patch_d, r, false)) == ErrorKind::Unsupported);
  // A generic fact needs options, and a typed one may not have them.
  text = std::string(config_defaults::descriptor_policy_json);
  text.replace(text.find(last), last.size(), R"("type":"computer_use_preview"},{"generic":true,"max_uses":false,"name":"apply_patch","type":"apply_patch"}])");
  CHECK(std::holds_alternative<descriptor::ConfigError>(descriptor::load_policy(text, config_defaults::codec_defaults_json)));
}
// What each guard that names the members an encoder owns answers, for every member name any of them holds. A name
// absent from the record below is admitted everywhere: not named by an extension, free for each of the four binding
// slots, a free option of every endpoint and of every tool. A binding digit names the slot (1 model, 2 messages,
// 3 stream, 4 max_output_tokens) at whose pointer the descriptor was refused; a dash admits it.
constexpr std::string_view recorded_names[] = {
    "access_programs", "agent", "agent_config", "allowed_callers", "allowed_domains", "aspect_ratio", "audio",
    "background", "batch", "blocked_domains", "cache_control", "cachedContent", "cached_content", "citations",
    "compaction", "completion_window", "container", "contents", "context_management", "conversation", "description",
    "diagnostics", "dimensions", "documents", "duration", "encoding_format", "endpoint", "environment",
    "expires_after", "extensions", "file", "frame_images", "function_call", "functions", "generate_audio",
    "generationConfig", "generation_config", "image_config", "images", "include", "include_reasoning",
    "inference_geo", "input", "inputAudioTranscription", "input_audio", "input_file_id", "input_references",
    "input_schema", "input_type", "instructions", "language", "max_completion_tokens", "max_content_tokens",
    "max_output_tokens", "max_tokens", "max_tool_calls", "max_uses", "mcp_servers", "messages", "metadata",
    "modalities", "model", "models", "n", "name", "outputAudioTranscription", "output_compression", "output_config",
    "output_format", "output_modalities", "parallel_tool_calls", "partial_images", "plugins",
    "previous_interaction_id", "previous_response_id", "prompt", "prompt_cache_key", "provider", "purpose",
    "quality", "query", "questions", "reasoning", "reasoning_effort", "requests", "resolution", "response_format",
    "response_modalities", "safetySettings", "safety_settings", "serviceTier", "service_tier", "sessionResumption",
    "size", "speed", "state", "stop", "stop_sequences", "store", "stream", "stream_format", "stream_options",
    "system", "systemInstruction", "system_instruction", "temperature", "text", "thinking", "toolConfig",
    "tool_choice", "tool_config", "tools", "top_k", "top_n", "top_p", "truncation", "type", "usage", "user_location",
    "voice",
};
constexpr std::string_view recorded_answers[] = {
    "openai.chat audio ext=R bind=1234",
    "openai.chat contents ext=R bind=1-34",
    "openai.chat extensions ext=R bind=1234",
    "openai.chat function_call ext=R bind=1234",
    "openai.chat functions ext=R bind=1234",
    "openai.chat image_config ext=R bind=1234",
    "openai.chat include_reasoning ext=R bind=1234",
    "openai.chat input ext=R bind=1-34",
    "openai.chat max_completion_tokens ext=R bind=123-",
    "openai.chat max_output_tokens ext=R bind=123-",
    "openai.chat max_tokens ext=R bind=444-",
    "openai.chat messages ext=R bind=2-34",
    "openai.chat modalities ext=R bind=1234",
    "openai.chat model ext=R bind=-234",
    "openai.chat models ext=R bind=1234",
    "openai.chat n ext=A bind=1234",
    "openai.chat plugins ext=R bind=1234",
    "openai.chat provider ext=R bind=1234",
    "openai.chat reasoning ext=R bind=1234",
    "openai.chat reasoning_effort ext=R bind=1234",
    "openai.chat response_format ext=R bind=1234",
    "openai.chat service_tier ext=R bind=1234",
    "openai.chat stop ext=A bind=1234",
    "openai.chat stream ext=R bind=33-4",
    "openai.chat stream_options ext=R bind=1234",
    "openai.chat temperature ext=R bind=1234",
    "openai.chat tool_choice ext=R bind=1234",
    "openai.chat tools ext=R bind=1234",
    "openai.chat top_p ext=R bind=1234",
    "openai.chat usage ext=R bind=1234",
    "anthropic.messages cache_control ext=R bind=1234",
    "anthropic.messages compaction ext=A bind=1234",
    "anthropic.messages container ext=R bind=1234",
    "anthropic.messages contents ext=R bind=1-34",
    "anthropic.messages context_management ext=A bind=1234",
    "anthropic.messages diagnostics ext=A bind=1234",
    "anthropic.messages extensions ext=R bind=1234",
    "anthropic.messages inference_geo ext=A bind=1234",
    "anthropic.messages input ext=R bind=1-34",
    "anthropic.messages max_completion_tokens ext=R bind=1234",
    "anthropic.messages max_output_tokens ext=R bind=123-",
    "anthropic.messages max_tokens ext=R bind=444-",
    "anthropic.messages mcp_servers ext=R bind=1234",
    "anthropic.messages messages ext=R bind=2-34",
    "anthropic.messages metadata ext=R bind=1234",
    "anthropic.messages model ext=R bind=-234",
    "anthropic.messages n ext=A bind=1234",
    "anthropic.messages output_config ext=R bind=1234",
    "anthropic.messages provider ext=R bind=1234",
    "anthropic.messages response_format ext=A bind=1234",
    "anthropic.messages service_tier ext=A bind=1234",
    "anthropic.messages speed ext=A bind=1234",
    "anthropic.messages stop ext=A bind=1234",
    "anthropic.messages stop_sequences ext=A bind=1234",
    "anthropic.messages stream ext=R bind=33-4",
    "anthropic.messages stream_options ext=R bind=1234",
    "anthropic.messages system ext=R bind=1234",
    "anthropic.messages temperature ext=R bind=1234",
    "anthropic.messages thinking ext=R bind=1234",
    "anthropic.messages tool_choice ext=R bind=1234",
    "anthropic.messages tools ext=R bind=1234",
    "anthropic.messages top_k ext=A bind=1234",
    "anthropic.messages top_p ext=R bind=1234",
    "openai.responses access_programs ext=A bind=1234",
    "openai.responses background ext=R bind=1234",
    "openai.responses contents ext=R bind=1-34",
    "openai.responses context_management ext=A bind=1234",
    "openai.responses conversation ext=R bind=1234",
    "openai.responses extensions ext=R bind=1234",
    "openai.responses image_config ext=R bind=1234",
    "openai.responses include ext=R bind=1234",
    "openai.responses input ext=R bind=2-34",
    "openai.responses instructions ext=R bind=1234",
    "openai.responses max_completion_tokens ext=R bind=1234",
    "openai.responses max_output_tokens ext=R bind=444-",
    "openai.responses max_tokens ext=R bind=123-",
    "openai.responses max_tool_calls ext=R bind=1234",
    "openai.responses messages ext=R bind=1-34",
    "openai.responses metadata ext=A bind=1234",
    "openai.responses modalities ext=R bind=1234",
    "openai.responses model ext=R bind=-234",
    "openai.responses n ext=A bind=1234",
    "openai.responses parallel_tool_calls ext=R bind=1234",
    "openai.responses plugins ext=R bind=1234",
    "openai.responses previous_response_id ext=R bind=1234",
    "openai.responses prompt ext=R bind=1234",
    "openai.responses prompt_cache_key ext=A bind=1234",
    "openai.responses provider ext=R bind=1234",
    "openai.responses reasoning ext=R bind=1234",
    "openai.responses response_format ext=A bind=1234",
    "openai.responses service_tier ext=R bind=1234",
    "openai.responses stop ext=A bind=1234",
    "openai.responses store ext=R bind=1234",
    "openai.responses stream ext=R bind=33-4",
    "openai.responses stream_options ext=R bind=1234",
    "openai.responses temperature ext=R bind=1234",
    "openai.responses text ext=R bind=1234",
    "openai.responses tool_choice ext=R bind=1234",
    "openai.responses tools ext=R bind=1234",
    "openai.responses top_p ext=R bind=1234",
    "openai.responses truncation ext=R bind=1234",
    "google.generate agent ext=A bind=1234",
    "google.generate background ext=A bind=1234",
    "google.generate cachedContent ext=R bind=1234",
    "google.generate cached_content ext=R bind=1234",
    "google.generate contents ext=R bind=2-34",
    "google.generate environment ext=A bind=1234",
    "google.generate extensions ext=R bind=1234",
    "google.generate generationConfig ext=R bind=1234",
    "google.generate generation_config ext=R bind=1234",
    "google.generate input ext=R bind=1-34",
    "google.generate max_completion_tokens ext=R bind=1234",
    "google.generate max_output_tokens ext=R bind=123-",
    "google.generate max_tokens ext=R bind=123-",
    "google.generate messages ext=R bind=1-34",
    "google.generate model ext=R bind=-234",
    "google.generate n ext=A bind=1234",
    "google.generate previous_interaction_id ext=A bind=1234",
    "google.generate provider ext=R bind=1234",
    "google.generate response_format ext=A bind=1234",
    "google.generate safetySettings ext=R bind=1234",
    "google.generate safety_settings ext=R bind=1234",
    "google.generate serviceTier ext=A bind=1234",
    "google.generate service_tier ext=A bind=1234",
    "google.generate stop ext=A bind=1234",
    "google.generate store ext=A bind=1234",
    "google.generate stream ext=R bind=33-4",
    "google.generate stream_options ext=R bind=1234",
    "google.generate systemInstruction ext=R bind=1234",
    "google.generate system_instruction ext=R bind=1234",
    "google.generate temperature ext=R bind=1234",
    "google.generate toolConfig ext=R bind=1234",
    "google.generate tool_choice ext=R bind=1234",
    "google.generate tool_config ext=R bind=1234",
    "google.generate tools ext=R bind=1234",
    "google.generate top_p ext=R bind=1234",
    "google.interactions agent ext=R bind=1234",
    "google.interactions agent_config ext=R bind=1234",
    "google.interactions background ext=R bind=1234",
    "google.interactions cachedContent ext=A bind=1234",
    "google.interactions contents ext=R bind=1-34",
    "google.interactions environment ext=A bind=1234",
    "google.interactions extensions ext=R bind=1234",
    "google.interactions generationConfig ext=A bind=1234",
    "google.interactions generation_config ext=R bind=1234",
    "google.interactions input ext=R bind=2-34",
    "google.interactions max_completion_tokens ext=R bind=1234",
    "google.interactions max_output_tokens ext=R bind=444-",
    "google.interactions max_tokens ext=R bind=123-",
    "google.interactions messages ext=R bind=1-34",
    "google.interactions model ext=R bind=-234",
    "google.interactions n ext=A bind=1234",
    "google.interactions previous_interaction_id ext=R bind=1234",
    "google.interactions provider ext=R bind=1234",
    "google.interactions response_format ext=R bind=1234",
    "google.interactions response_modalities ext=R bind=1234",
    "google.interactions safetySettings ext=A bind=1234",
    "google.interactions safety_settings ext=A bind=1234",
    "google.interactions serviceTier ext=A bind=1234",
    "google.interactions service_tier ext=R bind=1234",
    "google.interactions stop ext=A bind=1234",
    "google.interactions store ext=R bind=1234",
    "google.interactions stream ext=R bind=33-4",
    "google.interactions stream_options ext=R bind=1234",
    "google.interactions systemInstruction ext=A bind=1234",
    "google.interactions system_instruction ext=R bind=1234",
    "google.interactions temperature ext=R bind=1234",
    "google.interactions toolConfig ext=A bind=1234",
    "google.interactions tool_choice ext=R bind=1234",
    "google.interactions tools ext=R bind=1234",
    "google.interactions top_p ext=R bind=1234",
    "openai.chat@https://api.openai.com audio bind=1234",
    "openai.chat@https://api.openai.com contents bind=1-34",
    "openai.chat@https://api.openai.com extensions bind=1234",
    "openai.chat@https://api.openai.com function_call bind=1234",
    "openai.chat@https://api.openai.com functions bind=1234",
    "openai.chat@https://api.openai.com image_config bind=1234",
    "openai.chat@https://api.openai.com include_reasoning bind=1234",
    "openai.chat@https://api.openai.com input bind=1-34",
    "openai.chat@https://api.openai.com max_completion_tokens bind=123-",
    "openai.chat@https://api.openai.com max_output_tokens bind=123-",
    "openai.chat@https://api.openai.com max_tokens bind=123-",
    "openai.chat@https://api.openai.com messages bind=2-34",
    "openai.chat@https://api.openai.com modalities bind=1234",
    "openai.chat@https://api.openai.com model bind=-234",
    "openai.chat@https://api.openai.com models bind=1234",
    "openai.chat@https://api.openai.com n bind=1234",
    "openai.chat@https://api.openai.com plugins bind=1234",
    "openai.chat@https://api.openai.com provider bind=1234",
    "openai.chat@https://api.openai.com reasoning bind=1234",
    "openai.chat@https://api.openai.com reasoning_effort bind=1234",
    "openai.chat@https://api.openai.com response_format bind=1234",
    "openai.chat@https://api.openai.com service_tier bind=1234",
    "openai.chat@https://api.openai.com stop bind=1234",
    "openai.chat@https://api.openai.com stream bind=33-4",
    "openai.chat@https://api.openai.com stream_options bind=1234",
    "openai.chat@https://api.openai.com temperature bind=1234",
    "openai.chat@https://api.openai.com tool_choice bind=1234",
    "openai.chat@https://api.openai.com tools bind=1234",
    "openai.chat@https://api.openai.com top_p bind=1234",
    "openai.chat@https://api.openai.com usage bind=1234",
    "endpoint:images background",
    "endpoint:images images",
    "endpoint:images input_references",
    "endpoint:images model",
    "endpoint:images n",
    "endpoint:images output_compression",
    "endpoint:images output_format",
    "endpoint:images partial_images",
    "endpoint:images prompt",
    "endpoint:images quality",
    "endpoint:images size",
    "endpoint:images stream",
    "endpoint:decisions input",
    "endpoint:decisions model",
    "endpoint:decisions questions",
    "endpoint:decisions state",
    "endpoint:embeddings dimensions",
    "endpoint:embeddings encoding_format",
    "endpoint:embeddings input",
    "endpoint:embeddings input_type",
    "endpoint:embeddings model",
    "endpoint:embeddings requests",
    "endpoint:rerank documents",
    "endpoint:rerank model",
    "endpoint:rerank query",
    "endpoint:rerank top_n",
    "endpoint:moderation input",
    "endpoint:moderation model",
    "endpoint:speech input",
    "endpoint:speech instructions",
    "endpoint:speech model",
    "endpoint:speech response_format",
    "endpoint:speech speed",
    "endpoint:speech stream_format",
    "endpoint:speech voice",
    "endpoint:transcription file",
    "endpoint:transcription input_audio",
    "endpoint:transcription language",
    "endpoint:transcription model",
    "endpoint:transcription prompt",
    "endpoint:transcription stream",
    "endpoint:videos aspect_ratio",
    "endpoint:videos duration",
    "endpoint:videos frame_images",
    "endpoint:videos generate_audio",
    "endpoint:videos input_references",
    "endpoint:videos model",
    "endpoint:videos prompt",
    "endpoint:videos resolution",
    "endpoint:videos size",
    "endpoint:files expires_after",
    "endpoint:files file",
    "endpoint:files purpose",
    "endpoint:realtime audio",
    "endpoint:realtime generationConfig",
    "endpoint:realtime inputAudioTranscription",
    "endpoint:realtime instructions",
    "endpoint:realtime model",
    "endpoint:realtime outputAudioTranscription",
    "endpoint:realtime output_modalities",
    "endpoint:realtime sessionResumption",
    "endpoint:realtime systemInstruction",
    "endpoint:realtime tools",
    "endpoint:realtime type",
    "endpoint:batches batch",
    "endpoint:batches completion_window",
    "endpoint:batches endpoint",
    "endpoint:batches input_file_id",
    "endpoint:batches requests",
    "tool:anthropic.messages allowed_callers",
    "tool:anthropic.messages allowed_domains",
    "tool:anthropic.messages blocked_domains",
    "tool:anthropic.messages citations",
    "tool:anthropic.messages description",
    "tool:anthropic.messages input_schema",
    "tool:anthropic.messages max_content_tokens",
    "tool:anthropic.messages max_uses",
    "tool:anthropic.messages name",
    "tool:anthropic.messages type",
    "tool:anthropic.messages user_location",
    "tool:openai.responses description",
    "tool:openai.responses input_schema",
    "tool:openai.responses max_uses",
    "tool:openai.responses name",
    "tool:openai.responses type",
};
constexpr std::string_view binding_slots[] = {"model", "messages", "stream", "max_output_tokens"};
constexpr std::string_view recorded_endpoints[] = {"images", "decisions", "embeddings", "rerank", "moderation", "speech",
                                                   "transcription", "videos", "files", "realtime", "batches"};
constexpr std::string_view recorded_families[] = {"openai.chat", "anthropic.messages", "openai.responses", "google.generate", "google.interactions"};

// A policy refusal at `pointer` is the answer "refused"; a loaded policy is "admitted"; any other refusal is a defect of the record.
bool refused_at_pointer(const std::variant<descriptor::PolicySnapshot, descriptor::ConfigError>& loaded, std::string_view pointer) {
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded)) {
    if (error->pointer != pointer) throw std::runtime_error("policy refused at " + error->pointer + ", not " + std::string(pointer) + ": " + error->expected);
    return true;
  }
  return false;
}
bool extension_refused(std::string_view family, std::string_view name) {
  return refused_at_pointer(policy_with(family, R"("extensions":[{"key":)" + json::quote(name) +
      R"(,"origins":["https://example.test"],"value":{"type":"boolean"},"replay_eligible":true}])"), "/extensions/key");
}
// The binding digits for `name`: each slot in turn bound to it, the others left at the family's own members.
// `origin` empty: the family's own fixed origin. The Chat output cap defaults to `max_completion_tokens` at direct OpenAI.
std::string binding_answer(std::string_view family, std::string_view name, std::string_view at = {}) {
  const bool google = family.starts_with("google.");
  const std::string origin = !at.empty() ? std::string(at) : google ? "https://generativelanguage.googleapis.com" : "https://example.test";
  const std::string path = family == "google.generate" ? "/v1beta/models/fixture-model:generateContent" : family == "google.interactions" ? "/v1beta/interactions" :
      family == "anthropic.messages" ? "/v1/messages" : family == "openai.chat" ? "/v1/chat/completions" : "/v1/responses";
  const std::string stream = family == "google.generate" ? "/v1beta/models/fixture-model:streamGenerateContent?alt=sse" : path;
  std::string digits;
  for (const auto slot : binding_slots) {
    const auto loaded = descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"owned-members\",\"family\":" + json::quote(family) +
        ",\"connection\":{\"base_url\":" + json::quote(origin) + ",\"paths\":{\"buffered\":" + json::quote(path) + ",\"streaming\":" + json::quote(stream) +
        "}},\"bindings\":{" + json::quote(slot) + ":" + json::quote(name) + "}}", descriptor::builtin_policy());
    const auto* error = std::get_if<descriptor::ConfigError>(&loaded);
    if (!error) { digits += '-'; continue; }
    std::size_t at = 0;
    while (at < std::size(binding_slots) && error->pointer != "/bindings/" + std::string(binding_slots[at])) ++at;
    if (at == std::size(binding_slots)) throw std::runtime_error(std::string(family) + " binding " + std::string(slot) + " to " + std::string(name) + " refused at " + error->pointer);
    digits += static_cast<char>('1' + at);
  }
  return digits;
}
// The policy text with one endpoint fact (the first of that endpoint) alone in `endpoints`, its options naming `member`.
struct EndpointPolicies {
  std::vector<std::pair<std::string, std::string>> root;
  std::map<std::string, std::string, std::less<>> facts;
  EndpointPolicies() {
    auto parsed = json::parse(config_defaults::descriptor_policy_json);
    const auto document = std::get<json::Document>(std::move(parsed));
    for (auto member : document.root().members()) if (member.key != "endpoints") root.emplace_back(std::string(member.key), member.value.dump());
    for (auto fact : document.root().get("endpoints").elements()) {
      const std::string endpoint(fact.get("endpoint").as_string());
      if (facts.contains(endpoint)) continue;
      std::string text = "{";
      for (auto member : fact.members()) if (member.key != "options") text += json::quote(member.key) + ":" + member.value.dump() + ",";
      facts.emplace(endpoint, text);
    }
  }
  std::string with_option(std::string_view endpoint, std::string_view member) const {
    std::string out = "{";
    for (const auto& [key, value] : root) out += json::quote(key) + ":" + value + ",";
    return out + "\"endpoints\":[" + facts.find(endpoint)->second + "\"options\":{\"type\":\"object\",\"properties\":{" + json::quote(member) + ":{\"type\":\"null\"}}}}]}";
  }
};
bool endpoint_option_refused(const EndpointPolicies& policies, std::string_view endpoint, std::string_view name) {
  return refused_at_pointer(descriptor::load_policy(policies.with_option(endpoint, name), config_defaults::codec_defaults_json), "/endpoints/options");
}
bool tool_option_refused(std::string_view family, std::string_view name) {
  const std::string options = R"("options":{"type":"object","properties":{)" + json::quote(name) + R"(:{"type":"null"}}}})";
  const std::string fact = family == "anthropic.messages" ? R"("server_tools":[{"type":"fixture_tool","name":"fixture","max_uses":false,)" + options + "]"
                                                          : R"("server_tools":[{"type":"fixture_tool","name":"fixture_tool","max_uses":false,"generic":true,)" + options + "]";
  return refused_at_pointer(policy_with(family, fact), "/server_tools/options");
}
std::string upper(std::string_view text) { std::string out(text); for (auto& c : out) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32); return out; }

void owned_members_recorded() {
  std::vector<std::string> actual;
  for (const auto family : recorded_families)
    for (const auto name : recorded_names) {
      const bool extension = extension_refused(family, name);
      // The encoders and the loader ask the same question, in any letter case.
      CHECK(extension == descriptor::request_key_owned(family, name));
      CHECK(extension == descriptor::request_key_owned(family, upper(name)));
      const auto digits = binding_answer(family, name);
      if (extension || digits != "----") actual.push_back(std::string(family) + " " + std::string(name) + " ext=" + (extension ? "R" : "A") + " bind=" + digits);
    }
  // The same descriptor at an origin whose default output cap is not the family's: the slot-member names are recorded there too.
  constexpr std::string_view direct = "https://api.openai.com";
  for (const auto name : recorded_names)
    if (const auto digits = binding_answer("openai.chat", name, direct); digits != "----") actual.push_back("openai.chat@" + std::string(direct) + " " + std::string(name) + " bind=" + digits);
  const EndpointPolicies policies;
  for (const auto endpoint : recorded_endpoints)
    for (const auto name : recorded_names)
      if (endpoint_option_refused(policies, endpoint, name)) actual.push_back("endpoint:" + std::string(endpoint) + " " + std::string(name));
  for (const auto family : {"anthropic.messages", "openai.responses"})
    for (const auto name : recorded_names)
      if (tool_option_refused(family, name)) actual.push_back("tool:" + std::string(family) + " " + std::string(name));
  const std::vector<std::string> expected(std::begin(recorded_answers), std::end(recorded_answers));
  bool same = actual == expected;
  for (const auto& line : actual) if (std::find(expected.begin(), expected.end(), line) == expected.end()) { std::cerr << "unrecorded: " << line << '\n'; same = false; }
  for (const auto& line : expected) if (std::find(actual.begin(), actual.end(), line) == actual.end()) { std::cerr << "missing: " << line << '\n'; same = false; }
  CHECK(same);
}
// A binding never names a key the family's policy admits as an extension: the body would hold the member twice.
void bindings_never_name_admitted_extensions() {
  constexpr std::string_view slots[]{"model", "messages", "stream", "max_output_tokens"};
  for (const auto family : recorded_families) {
    const auto policy = admitted(family, R"("extensions":[{"key":"fixture_member","origins":["https://example.test"],"value":{"type":"boolean"},"replay_eligible":true}])");
    for (const auto slot : slots) {
      const auto source = [&](std::string_view key) {
        const bool google = family.starts_with("google.");
        const std::string path = family == "google.generate" ? "/v1beta/models/fixture-model:generateContent" : family == "google.interactions" ? "/v1beta/interactions" :
            family == "anthropic.messages" ? "/v1/messages" : family == "openai.chat" ? "/v1/chat/completions" : "/v1/responses";
        const std::string stream = family == "google.generate" ? "/v1beta/models/fixture-model:streamGenerateContent?alt=sse" : path;
        return "{\"descriptor_version\":1,\"revision\":1,\"id\":\"admitted-key\",\"family\":" + json::quote(family) + ",\"connection\":{\"base_url\":" +
               json::quote(google ? "https://generativelanguage.googleapis.com" : "https://example.test") + ",\"paths\":{\"buffered\":" + json::quote(path) +
               ",\"streaming\":" + json::quote(stream) + "}},\"bindings\":{" + json::quote(slot) + ":" + json::quote(key) + "}}";
      };
      const auto refused_by_policy = descriptor::load(source("fixture_member"), policy);
      CHECK(std::holds_alternative<descriptor::ConfigError>(refused_by_policy));
      CHECK(std::get<descriptor::ConfigError>(refused_by_policy).pointer == "/bindings/" + std::string(slot));
      // The same name is free under a policy that admits no such extension, and another name is free under this one.
      CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(descriptor::load(source("fixture_member"), descriptor::builtin_policy())));
      CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(descriptor::load(source("fixture_other"), policy)));
    }
  }
}
} // namespace

int main() {
  try { value_schemas(); policy_facts(); extensions_on_every_family(); extension_replay(); owned_keys_cover_encoder_output(); admitted_options(); owned_members_recorded(); bindings_never_name_admitted_extensions();
        std::cout << "extension properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "extension test failed: " << error.what() << '\n'; return 1; }
}
