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
#include <stdexcept>
#include <string>

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

// The owned lists cover every top-level member the encoders write with their typed controls set.
void owned_keys_cover_encoder_output() {
  const auto policy = descriptor::builtin_policy();
  auto keys_of = [](const std::string& body) {
    const auto document = parse(body);
    std::vector<std::string> out;
    for (auto m : document.root().members()) out.emplace_back(m.key);
    return out;
  };
  auto check = [&](std::string_view family, const std::string& body) {
    for (const auto& key : keys_of(body)) if (!descriptor::request_key_owned(family, key)) throw std::runtime_error(std::string(family) + " writes unowned " + key);
  };
  {
    const auto d = load("anthropic.messages", "https://api.anthropic.com", policy);
    auto r = messages::Request::simple("fixture-model", "hi", 64, "system"); r.temperature = 0.5; r.top_p = 0.9;
    r.output_effort = messages::OutputEffort::High; r.cache_control = messages::CacheControl{}; r.container = "cntr_1";
    r.tools = {messages::ToolDefinition{"lookup", "d", owned(R"({"type":"object"})"), {}, {}, {}, {}, {}, {}, {}, {}}};
    r.tool_choice = messages::ToolChoice{messages::ToolChoiceMode::Auto, {}, {}};
    for (bool stream : {false, true}) { auto e = messages::encode(d, r, stream); CHECK(std::holds_alternative<messages::EncodedRequest>(e)); check("anthropic.messages", std::get<messages::EncodedRequest>(e).body); }
  }
  {
    const auto d = load("openai.chat", "https://openrouter.ai", policy);
    auto r = chat::Request::simple("fixture-model", "hi", 64); r.temperature = 0.5; r.top_p = 0.9; r.service_tier = "auto";
    r.response_format = ResponseFormat{}; r.reasoning = chat::ReasoningOptions{std::string("low"), {}, {}, {}}; r.include_reasoning = true;
    r.usage_include = true; r.models = {"a/b"}; r.plugins = {{"response-healing", {}}}; r.provider = OpenRouterRouting{};
    r.modalities = {chat::OutputModality::Text, chat::OutputModality::Image}; r.image_config = chat::ImageOutput{};
    for (bool stream : {false, true}) { auto e = chat::encode(d, r, stream); CHECK(std::holds_alternative<chat::EncodedRequest>(e)); check("openai.chat", std::get<chat::EncodedRequest>(e).body); }
  }
  {
    const auto d = load("openai.responses", "https://openrouter.ai", policy);
    auto r = responses::Request::simple("fixture-model", "hi", 64, "instructions"); r.temperature = 0.5; r.top_p = 0.9; r.store = true;
    r.reasoning = responses::ReasoningOptions{std::string("low"), std::string("auto")}; r.service_tier = "auto"; r.max_tool_calls = 3;
    r.parallel_tool_calls = true; r.verbosity = responses::Verbosity::Low; r.truncation = responses::Truncation::Auto; r.response_format = ResponseFormat{};
    r.plugins = {{"response-healing", {}}}; r.modalities = {responses::OutputModality::Image}; r.image_config = OpenRouterImageConfig{};
    r.provider = OpenRouterRouting{}; r.required_tool = "lookup";
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
} // namespace

int main() {
  try { value_schemas(); policy_facts(); extensions_on_every_family(); extension_replay(); owned_keys_cover_encoder_output(); admitted_options();
        std::cout << "extension properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "extension test failed: " << error.what() << '\n'; return 1; }
}
