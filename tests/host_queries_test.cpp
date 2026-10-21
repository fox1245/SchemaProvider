// The typed queries a host asks instead of restating provider facts, and the worst-case cost of a prepared request.
#include "codecs/chat.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "codecs/messages_request.h"
#include "codecs/responses_request.h"
#include "codecs/generations.h"
#include "core/enum_names.h"
#include "core/failure.h"
#include "descriptor/policy.h"
#include "descriptor/presets.h"
#include "runtime/client.h"
#include "sp/config_defaults.h"
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace sp;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)

// Every enumerator from zero has a distinct name that reads back, and the one past the last has none. `count` is the
// number of enumerators; a new enumerator makes it differ and fails here until the table and this count follow.
template <class Enum> void names_round_trip(std::size_t count) {
  std::set<std::string_view> seen;
  for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
    const auto value = static_cast<Enum>(ordinal);
    const auto name = to_string(value);
    CHECK(!name.empty() && seen.insert(name).second);
    const auto back = from_string<Enum>(name);
    CHECK(back && *back == value);
  }
  CHECK(to_string(static_cast<Enum>(count)).empty());
  CHECK(!from_string<Enum>("") && !from_string<Enum>("NoSuchEnumerator") && !from_string<Enum>(" "));
}

void enum_names() {
  names_round_trip<Role>(5); names_round_trip<ToolCallKind>(3); names_round_trip<InvalidReason>(6);
  names_round_trip<Evidence>(2); names_round_trip<UsageStage>(3); names_round_trip<UsageQuality>(2);
  names_round_trip<CostSource>(3); names_round_trip<CostStatus>(7); names_round_trip<CostRounding>(3);
  names_round_trip<StopKind>(12); names_round_trip<ErrorKind>(19); names_round_trip<RetryClass>(4);
  names_round_trip<RetrySafety>(4); names_round_trip<ServedRelation>(6); names_round_trip<ImageDetail>(4);
  names_round_trip<MediaKind>(4); names_round_trip<MediaSource>(3);
  names_round_trip<descriptor::StopKind>(11); names_round_trip<descriptor::ModelRelation>(4);
  // The last enumerator of each is the one a host used to bound an ordinal by.
  CHECK(from_string<ErrorKind>("Overloaded") == ErrorKind::Overloaded);
  CHECK(from_string<RetrySafety>("RejectedBeforeOutput") == RetrySafety::RejectedBeforeOutput);
  CHECK(from_string<CostSource>("OpenRouterUsd") == CostSource::OpenRouterUsd);
  CHECK(from_string<StopKind>("Compaction") == StopKind::Compaction);
  // The two StopKinds order their enumerators differently, so the same name is a different ordinal.
  CHECK(from_string<StopKind>("ToolUse") == StopKind::ToolUse && from_string<descriptor::StopKind>("ToolUse") == descriptor::StopKind::ToolUse);
  CHECK(static_cast<int>(StopKind::ToolUse) != static_cast<int>(descriptor::StopKind::ToolUse));
  // Names are case-sensitive.
  CHECK(!from_string<ErrorKind>("overloaded"));
}

// ---- policy queries -------------------------------------------------------------------------------------------

constexpr std::string_view families[]{"openai.chat", "anthropic.messages", "openai.responses", "google.generate", "google.interactions"};
constexpr descriptor::Control all_controls[]{
    descriptor::Control::MaxOutputTokens, descriptor::Control::Temperature, descriptor::Control::TopP,
    descriptor::Control::ThinkingBudget, descriptor::Control::IncludeThoughts, descriptor::Control::ReasoningEffort,
    descriptor::Control::ReasoningSummary, descriptor::Control::ThinkingLevel, descriptor::Control::ThinkingSummaries,
    descriptor::Control::ServiceTier, descriptor::Control::RequiredTool, descriptor::Control::Store,
    descriptor::Control::MaxToolCalls};
static_assert(std::size(all_controls) == descriptor::control_count);

std::string policy_text() { return std::string(config_defaults::descriptor_policy_json); }
descriptor::PolicySnapshot load(const std::string& family_json) {
  auto loaded = descriptor::load_policy(family_json, config_defaults::codec_defaults_json);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded))
    throw std::runtime_error("policy refused at " + error->pointer + ": " + error->expected);
  return std::get<descriptor::PolicySnapshot>(std::move(loaded));
}
bool refused(const std::string& family_json, std::string_view pointer) {
  const auto loaded = descriptor::load_policy(family_json, config_defaults::codec_defaults_json);
  const auto* error = std::get_if<descriptor::ConfigError>(&loaded);
  return error && error->pointer == pointer;
}
std::string replaced(std::string text, std::string_view from, std::string_view to) {
  const auto at = text.find(from);
  if (at == std::string::npos) throw std::runtime_error("policy text lacks: " + std::string(from));
  text.replace(at, from.size(), to);
  return text;
}
descriptor::ValidatedDescriptor descriptor_of(std::string_view family, descriptor::PolicySnapshot policy = {}) {
  namespace presets = descriptor::presets;
  presets::PresetOptions options;
  options.policy = std::move(policy);
  auto loaded = family == "openai.chat" ? presets::try_openai_chat(options)
      : family == "anthropic.messages" ? presets::try_anthropic_messages(options)
      : family == "openai.responses" ? presets::try_openai_responses(options)
      : family == "google.generate" ? presets::try_google_generate(options)
      : presets::try_google_interactions(options);
  if (auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("descriptor refused: " + error->pointer);
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}

// Whether the family's policy-level checks (validate_choices) take the control alone, with an otherwise valid request.
bool policy_accepts(const descriptor::ValidatedDescriptor& d, descriptor::Control control) {
  using descriptor::Control;
  const auto& f = d.family_policy();
  descriptor::EffectiveChoices choices{};
  choices.max_output_tokens = 4096;
  auto listed = [](const std::vector<std::string>& values) { return values.empty() ? std::string_view("unlisted") : std::string_view(values.front()); };
  switch (control) {
    case Control::MaxOutputTokens: break;
    case Control::Temperature: choices.temperature = 0.5; break;
    case Control::TopP: choices.top_p = 0.5; break;
    case Control::ThinkingBudget: choices.thinking_budget = 2048; break;
    case Control::IncludeThoughts: choices.include_thoughts = true; break;
    case Control::ReasoningEffort: choices.reasoning_effort = listed(f.reasoning_efforts); break;
    case Control::ReasoningSummary: choices.reasoning_summary = listed(f.reasoning_summaries); break;
    case Control::ThinkingLevel: choices.thinking_level = listed(f.thinking_levels); break;
    case Control::ThinkingSummaries: choices.thinking_summaries = true; break;
    case Control::ServiceTier: choices.service_tier = listed(f.service_tiers); break;
    default: throw std::runtime_error("not a policy-level control");
  }
  return !descriptor::validate_choices(d, "probe-model", choices);
}
// Whether the family's typed request has the member the control names; nullopt where the control is a nested member
// (Chat's reasoning effort, Responses' reasoning summary) that a presence check cannot see.
template <class Request> std::optional<bool> carries(descriptor::Control control) {
  using descriptor::Control;
  switch (control) {
    case Control::MaxOutputTokens: return requires(Request r) { r.max_output_tokens; } || requires(Request r) { r.max_tokens; };
    case Control::Temperature: return requires(Request r) { r.temperature; };
    case Control::TopP: return requires(Request r) { r.top_p; };
    case Control::ThinkingBudget: return requires(Request r) { r.thinking_budget; };
    case Control::IncludeThoughts: return requires(Request r) { r.include_thoughts; };
    case Control::ThinkingLevel: return requires(Request r) { r.thinking_level; };
    case Control::ThinkingSummaries: return requires(Request r) { r.thinking_summaries; };
    case Control::ServiceTier: return requires(Request r) { r.service_tier; };
    case Control::RequiredTool: return requires(Request r) { r.required_tool; };
    case Control::Store: return requires(Request r) { r.store; };
    case Control::MaxToolCalls: return requires(Request r) { r.max_tool_calls; };
    case Control::ReasoningEffort: case Control::ReasoningSummary: return std::nullopt;
  }
  return std::nullopt;
}
std::optional<bool> structure(std::string_view family, descriptor::Control control) {
  if (family == "openai.chat") return carries<chat::Request>(control);
  if (family == "anthropic.messages") return carries<messages::Request>(control);
  if (family == "openai.responses") return carries<responses::Request>(control);
  if (family == "google.generate") return carries<gemini::Request>(control);
  return carries<interactions::Request>(control);
}
template <class Request> constexpr bool has_routing = requires(Request r) { r.provider; };
bool routing_member(std::string_view family) {
  if (family == "openai.chat") return has_routing<chat::Request>;
  if (family == "anthropic.messages") return has_routing<messages::Request>;
  if (family == "openai.responses") return has_routing<responses::Request>;
  if (family == "google.generate") return has_routing<gemini::Request>;
  return has_routing<interactions::Request>;
}

// Whether the policy's controls agree, for every family, with what the family's checks refuse and with the members its
// typed request has.
bool controls_agree(const descriptor::PolicySnapshot& policy) {
  for (const auto family : families) {
    const auto d = descriptor_of(family, policy);
    for (const auto control : all_controls) {
      const auto declared = policy->control_admitted(family, control);
      if (!declared) return false;
      if (control != descriptor::Control::RequiredTool && control != descriptor::Control::Store &&
          control != descriptor::Control::MaxToolCalls && policy_accepts(d, control) != *declared) return false;
      if (const auto member = structure(family, control); member && *member != *declared) return false;
    }
  }
  return true;
}

// The shipped controls agree with the checks and the request members. A new control, or a changed rule, fails here until
// the data follows; and the comparison does fail for a control the data gets wrong.
void controls_match_the_encoders() {
  const auto policy = descriptor::builtin_policy();
  CHECK(controls_agree(policy));
  const auto text = policy_text();
  const std::string chat_controls = R"("controls":["max_output_tokens","temperature","top_p","reasoning_effort","service_tier"])";
  CHECK(!controls_agree(load(replaced(text, chat_controls, R"("controls":["max_output_tokens","temperature","reasoning_effort","service_tier"])"))));
  CHECK(!controls_agree(load(replaced(text, chat_controls, R"("controls":["max_output_tokens","temperature","top_p","reasoning_effort","service_tier","store"])"))));
  CHECK(!controls_agree(load(replaced(text, chat_controls, R"("controls":["max_output_tokens","temperature","top_p","service_tier"])"))));
  for (const auto control : all_controls) {
    const auto name = descriptor::control_name(control);
    CHECK(!name.empty() && descriptor::control_from_name(name) == control);
  }
  CHECK(!descriptor::control_from_name("") && !descriptor::control_from_name("max_tokens") && !descriptor::control_from_name("TopP"));
  CHECK(descriptor::control_name(static_cast<descriptor::Control>(descriptor::control_count)).empty());
  // The questions the hosts' reject lists ask.
  CHECK(policy->control_admitted("openai.chat", descriptor::Control::TopP) == true);
  CHECK(policy->control_admitted("google.generate", descriptor::Control::TopP) == false);
  CHECK(policy->control_admitted("google.interactions", descriptor::Control::Temperature) == false);
  CHECK(policy->control_admitted("anthropic.messages", descriptor::Control::ServiceTier) == false);
  CHECK(policy->control_admitted("openai.responses", descriptor::Control::MaxToolCalls) == true);
  CHECK(policy->control_admitted("openai.chat", descriptor::Control::MaxToolCalls) == false);
}

// Removes every member `"key":[...]` from the minified policy text, with the comma that follows it.
std::string without_member(std::string text, std::string_view key) {
  const auto needle = "\"" + std::string(key) + "\":[";
  for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle)) {
    const auto close = text.find("],", at);
    if (close == std::string::npos) throw std::runtime_error("member not followed by another: " + std::string(key));
    text.erase(at, close + 2 - at);
  }
  return text;
}

void controls_data() {
  // The built-in text is minified with sorted keys, so a member is matched by its own text.
  const auto text = policy_text();
  // A family the policy does not have is unknown, not refused.
  CHECK(!descriptor::builtin_policy()->control_admitted("zz.unknown", descriptor::Control::Temperature));
  const std::string chat_controls = R"("controls":["max_output_tokens","temperature","top_p","reasoning_effort","service_tier"])";
  CHECK(refused(replaced(text, chat_controls, R"("controls":["max_tokens"])"), "/controls"));
  CHECK(refused(replaced(text, chat_controls, R"("controls":["temperature","temperature"])"), "/controls"));
  // A control that takes listed values needs values: Messages lists no reasoning efforts.
  CHECK(refused(replaced(text, R"("controls":["max_output_tokens","temperature","top_p","thinking_budget"])", R"("controls":["reasoning_effort"])"), "/controls"));
  CHECK(refused(replaced(text, R"("standard_service_tiers":["default"])", R"("standard_service_tiers":["nope"])"), "/standard_service_tiers"));
  // A policy written before the members: it loads, declares nothing, and a tier passes through.
  const auto old = without_member(without_member(text, "controls"), "standard_service_tiers");
  CHECK(old.find("\"controls\"") == std::string::npos && old.find("standard_service_tiers") == std::string::npos);
  const auto before = load(old);
  for (const auto family : families)
    for (const auto control : all_controls) CHECK(!before->control_admitted(family, control));
  CHECK(before->billing_tier("openai.chat", "default") == "default");
}

void billing_tiers() {
  const auto policy = descriptor::builtin_policy();
  // The standard tier prices as no tier; any other tier is the catalogue's to price, or it has no price.
  CHECK(policy->billing_tier("openai.chat", "default").empty() && policy->billing_tier("openai.responses", "default").empty());
  CHECK(policy->billing_tier("google.interactions", "standard").empty());
  CHECK(policy->billing_tier("openai.chat", "flex") == "flex" && policy->billing_tier("openai.responses", "priority") == "priority");
  CHECK(policy->billing_tier("openai.chat", "auto") == "auto" && policy->billing_tier("openai.chat", "standard") == "standard");
  CHECK(policy->billing_tier("google.interactions", "default") == "default");
  CHECK(policy->billing_tier("anthropic.messages", "default") == "default" && policy->billing_tier("zz.unknown", "default") == "default");
  CHECK(policy->billing_tier("openai.chat", "").empty());
}

void routing_and_tools() {
  const auto policy = descriptor::builtin_policy();
  for (const auto family : families) {
    const auto* declared = policy->family(family);
    CHECK(declared);
    CHECK(descriptor::family_admits_routing(*declared) == routing_member(family));
  }
  CHECK(descriptor::tool_fee_unit("web_search") == "tool:web_search");
  const auto& messages = *policy->family("anthropic.messages");
  const auto memory = descriptor::tool_facts(messages, "memory_20250818");
  CHECK(memory.known && memory.client && !memory.admits_max_uses && memory.fee_unit == "tool:memory_20250818");
  const auto search = descriptor::tool_facts(messages, "web_search_20250305");
  CHECK(search.known && !search.client && search.admits_max_uses && search.fee_unit == "tool:web_search_20250305");
  const auto code = descriptor::tool_facts(messages, "code_execution_20250825");
  CHECK(code.known && !code.client && !code.admits_max_uses);
  // Every Messages client tool the policy lists reads as a client tool; the rest do not.
  for (const auto& tool : messages.server_tools) {
    const auto facts = descriptor::tool_facts(messages, tool.type);
    CHECK(facts.known && facts.client == tool.client && facts.admits_max_uses == tool.max_uses);
  }
  const auto& responses = *policy->family("openai.responses");
  const auto image = descriptor::tool_facts(responses, "image_generation");
  CHECK(image.known && !image.client && !image.admits_max_uses && image.fee_unit == "tool:image_generation");
  CHECK(descriptor::tool_facts(responses, "openrouter:web_search").known);
  // A type the policy does not list is unknown, not a client tool and not free; the unit is still named.
  const auto unknown = descriptor::tool_facts(messages, "zz_future_tool_20300101");
  CHECK(!unknown.known && !unknown.client && !unknown.admits_max_uses && unknown.fee_unit == "tool:zz_future_tool_20300101");
  CHECK(!descriptor::tool_facts(responses, "memory_20250818").known);
}

// A synthetic policy with a tool, a client tool and a model the shipped data does not know, through the queries
// unchanged. A host that asks the queries needs no change for them.
void synthetic_policy() {
  auto text = policy_text();
  const std::string memory = R"({"client":true,"max_uses":false,"name":"memory","type":"memory_20250818"},)";
  text = replaced(text, memory,
                  R"({"client":true,"max_uses":false,"name":"zz_local","type":"zz_local_tool_20300101"},)"
                  R"({"max_uses":true,"name":"zz_search","type":"zz_search_20300101"},)" + memory);
  const auto policy = load(text);
  const auto& messages = *policy->family("anthropic.messages");
  const auto local = descriptor::tool_facts(messages, "zz_local_tool_20300101");
  CHECK(local.known && local.client && !local.admits_max_uses && local.fee_unit == "tool:zz_local_tool_20300101");
  const auto remote = descriptor::tool_facts(messages, "zz_search_20300101");
  CHECK(remote.known && !remote.client && remote.admits_max_uses);
  // The shipped tools answer as before.
  CHECK(descriptor::tool_facts(messages, "memory_20250818").client);
  // A family the policy does not have answers "unknown" to every question, without throwing.
  for (const auto control : all_controls) CHECK(!policy->control_admitted("zz.family", control));
  CHECK(policy->billing_tier("zz.family", "turbo") == "turbo");
  CHECK(policy->family("zz.family") == nullptr);
  CHECK(!policy->model("zz.family", "https://zz.example", "zz-model-1").entry);
}

// ---- the refusal predicate ------------------------------------------------------------------------------------

Failure declined_failure() {
  Failure failure;
  failure.error.kind = ErrorKind::InvalidRequest;
  failure.error.retry_safety = RetrySafety::RejectedBeforeOutput;
  failure.error.http_status = 400;
  failure.error.attempt = AttemptEvidence{true, 271, true, 0, 1, false};
  failure.partial.raw_events.push_back(RawWire{std::string(http_error_event), nullptr});
  return failure;
}

void refusal_predicate() {
  CHECK(request_declined(declined_failure()));
  // No raw event at all is still a refusal; the diagnostic body is optional.
  auto bare = declined_failure(); bare.partial.raw_events.clear();
  CHECK(request_declined(bare));
  // Each piece of evidence the rule rests on, taken away or contradicted, ends it.
  auto changed = [](auto&& edit) { auto failure = declined_failure(); edit(failure); return request_declined(failure); };
  CHECK(!changed([](Failure& f) { f.error.retry_safety = RetrySafety::PossiblyAccepted; }));
  CHECK(!changed([](Failure& f) { f.error.retry_safety = RetrySafety::NotSent; }));
  CHECK(!changed([](Failure& f) { f.error.retry_safety = RetrySafety::OutputObserved; }));
  CHECK(!changed([](Failure& f) { f.error.attempt.request_may_have_left = false; }));
  CHECK(!changed([](Failure& f) { f.error.attempt.response_head_seen = false; }));
  CHECK(!changed([](Failure& f) { f.error.attempt.transport_internal_resends = 1; }));
  CHECK(!changed([](Failure& f) { f.error.attempt.prior_usage_unknown = true; }));
  CHECK(!changed([](Failure& f) { f.partial.messages.push_back(Message::assistant_text("partial")); }));
  CHECK(!changed([](Failure& f) { f.partial.stop = StopReason{}; }));
  CHECK(!changed([](Failure& f) { f.partial.raw_events.push_back(RawWire{"response.created", nullptr}); }));
  CHECK(!changed([](Failure& f) { f.partial.usage.stage = UsageStage::Partial; }));
  CHECK(!changed([](Failure& f) { f.partial.usage.input_total = Count{1, Evidence::Reported}; }));
  CHECK(!changed([](Failure& f) { f.partial.usage.extra["iterations.0.input_tokens"] = Count{1, Evidence::Reported}; }));
  CHECK(!changed([](Failure& f) { f.partial.usage.conflicts.push_back({"total", "x"}); }));
  CHECK(!changed([](Failure& f) { f.partial.usage.provider_cost.source = CostSource::OpenRouterUsd; }));
  CHECK(!changed([](Failure& f) { f.partial.usage.provider_cost.status[2] = CostStatus::Malformed; }));
  CHECK(!changed([](Failure& f) { f.partial.usage.provider_cost.is_byok = false; }));
  // Usage that reported zero is not usage that reported nothing.
  Usage zero; zero.input_total = Count{0, Evidence::Reported};
  CHECK(usage_reported_nothing(Usage{}) && !usage_reported_nothing(zero));
  CHECK(http_error_event == "http.error");
}

// ---- the facts of a prepared request --------------------------------------------------------------------------

using namespace std::chrono_literals;
runtime::RunOptions run_with(std::uint32_t attempts) {
  runtime::RunOptions run;
  run.streaming = false;
  run.retry = runtime::RetryPolicy{attempts > 1, false, attempts, 0ms, 0ms};
  return run;
}
runtime::Client client_of(descriptor::ValidatedDescriptor d) { return runtime::Client(std::move(d), "sk-not-a-real-key"); }
runtime::PreparedRequest prepared_ok(runtime::Client& client, runtime::Request request, std::uint32_t attempts = 1) {
  auto prepared = client.prepare(std::move(request), run_with(attempts));
  if (!prepared.valid() || prepared.error()) throw std::runtime_error("request did not prepare: " + (prepared.error() ? prepared.error()->safe_message + " / " + prepared.error()->reason : std::string("invalid")));
  return prepared;
}
descriptor::ValidatedDescriptor openrouter_chat(descriptor::PolicySnapshot policy = {}) {
  descriptor::presets::PresetOptions options;
  options.policy = std::move(policy);
  auto loaded = descriptor::presets::try_openrouter_chat(options);
  if (auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("descriptor refused: " + error->pointer);
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
chat::Request chat_request(std::string model, std::optional<std::uint64_t> cap) {
  chat::Request request;
  request.model = std::move(model);
  request.messages.push_back({Role::User, "hello"});
  request.max_output_tokens = cap;
  return request;
}
messages::ToolDefinition messages_tool(std::string type, std::string name, std::optional<std::uint64_t> max_uses = std::nullopt) {
  messages::ToolDefinition tool;
  tool.type = std::move(type);
  tool.name = std::move(name);
  tool.max_uses = max_uses;
  return tool;
}

void facts_of_prepared_requests() {
  using runtime::PreparedRequest;
  // Messages: the caller's tools are left out; a service tool carries its own cap; code execution admits none.
  auto messages = client_of(descriptor_of("anthropic.messages"));
  {
    auto request = messages::Request::simple("zz-model-1", "hi", 1000);
    request.tools.push_back(messages_tool("memory_20250818", "memory"));
    request.tools.push_back(messages_tool("web_search_20250305", "web_search", 2));
    auto prepared = prepared_ok(messages, request);
    const auto& facts = prepared.facts();
    CHECK(facts.tools.size() == 1 && facts.tools[0].types == std::vector<std::string>{"web_search_20250305"});
    CHECK(facts.tools[0].uses == 2u && facts.tools[0].member == "max_uses" && facts.tools[0].cap_admitted);
    CHECK(!facts.plugins && !facts.routing && facts.fallback_models.empty() && facts.service_tier.empty());
  }
  {
    auto request = messages::Request::simple("zz-model-1", "hi", 1000);
    request.tools.push_back(messages_tool("code_execution_20250825", "code_execution"));
    auto prepared = prepared_ok(messages, request);
    const auto& facts = prepared.facts();
    CHECK(facts.tools.size() == 1 && !facts.tools[0].uses && !facts.tools[0].cap_admitted && facts.tools[0].member == "max_uses");
  }
  // Responses: one cap, `max_tool_calls`, for all the hosted tools; the tier is the request's or the policy's default.
  auto responses = client_of(descriptor_of("openai.responses"));
  {
    auto request = responses::Request::simple("zz-model-1", "hi", 1000);
    request.hosted_tools.push_back(responses::WebSearchTool{});
    request.hosted_tools.push_back(responses::ImageGenerationTool{});
    request.max_tool_calls = 3;
    request.service_tier = "flex";
    auto prepared = prepared_ok(responses, request);
    const auto& facts = prepared.facts();
    CHECK(facts.tools.size() == 1 && facts.tools[0].types == (std::vector<std::string>{"web_search", "image_generation"}));
    CHECK(facts.tools[0].uses == 3u && facts.tools[0].member == "max_tool_calls" && facts.tools[0].cap_admitted);
    CHECK(facts.service_tier == "flex");
    CHECK(prepared.model_invocation_limit() == 4u);
  }
  CHECK(prepared_ok(responses, responses::Request::simple("zz-model-1", "hi", 1000)).facts().tools.empty());
  // Interactions: no tier named, so the policy's default tier, which bills at the standard rates.
  auto interactions = client_of(descriptor_of("google.interactions"));
  {
    auto request = interactions::Request::simple("zz-model-1", "hi", 1000);
    request.builtin_tools.push_back(interactions::GoogleSearchTool{});
    auto prepared = prepared_ok(interactions, request);
    const auto& facts = prepared.facts();
    CHECK(facts.service_tier == "standard" && prepared.descriptor()->policy()->billing_tier("google.interactions", facts.service_tier).empty());
    CHECK(facts.tools.size() == 1 && facts.tools[0].types == std::vector<std::string>{"google_search"} && !facts.tools[0].uses && facts.tools[0].member.empty());
  }
  // Generate: built-in tools and MCP servers, none capped.
  auto generate = client_of(descriptor_of("google.generate"));
  {
    auto request = gemini::Request::simple("zz-model-1", "hi", 1000);
    request.builtin_tools.push_back(gemini::GoogleSearchTool{});
    request.builtin_tools.push_back(gemini::UrlContextTool{});
    gemini::McpServer server;
    server.name = "docs";
    server.url = "https://mcp.example.com/mcp";
    request.mcp_servers.push_back(server);
    auto prepared = prepared_ok(generate, request);
    const auto& facts = prepared.facts();
    CHECK(facts.tools.size() == 3 && facts.tools[0].types == std::vector<std::string>{"google_search"} &&
          facts.tools[1].types == std::vector<std::string>{"url_context"} && facts.tools[2].types == std::vector<std::string>{"mcp:docs"});
    for (const auto& cap : facts.tools) CHECK(!cap.uses && cap.member.empty() && !cap.cap_admitted);
  }
  // Chat at a routing gateway: fallback models, plugins and the routing the request sends, which the body carries too.
  auto gateway = client_of(openrouter_chat());
  {
    auto request = chat_request("vendor/zz-routed", 500);
    request.models = {"vendor/zz-second"};
    request.plugins.push_back(OpenRouterPlugin{"moderation", nullptr});
    request.provider = OpenRouterRouting{};
    request.provider->only = {"zz-host"};
    request.provider->max_price = OpenRouterMaxPrice{1'500'000'000u, 3'000'000'000u, std::nullopt, std::nullopt, std::nullopt};
    auto prepared = prepared_ok(gateway, request);
    const auto& facts = prepared.facts();
    CHECK(facts.fallback_models == std::vector<std::string>{"vendor/zz-second"} && facts.plugins);
    CHECK(facts.routing && facts.routing->only == std::vector<std::string>{"zz-host"});
    CHECK(facts.routing->max_price && facts.routing->max_price->prompt_nano_usd == 1'500'000'000u && facts.routing->max_price->completion_nano_usd == 3'000'000'000u);
    CHECK(prepared.encoded_body().find(R"("max_price":{"prompt":1.5,"completion":3})") != std::string_view::npos);
    CHECK(prepared.encoded_body().find(R"("only":["zz-host"])") != std::string_view::npos);
  }
  // A request that sets a tier names it; a call that is not a chat request has no facts.
  {
    auto request = chat_request("zz-model-1", 100);
    request.service_tier = "flex";
    auto chat = client_of(descriptor_of("openai.chat"));
    CHECK(prepared_ok(chat, request).facts().service_tier == "flex");
    CHECK(prepared_ok(chat, chat_request("zz-model-1", 100)).facts().service_tier.empty());
  }
  CHECK(PreparedRequest{}.facts().tools.empty() && PreparedRequest{}.facts().service_tier.empty());
}

} // namespace

int main() {
  try {
    enum_names();
    controls_match_the_encoders(); controls_data(); billing_tiers(); routing_and_tools(); synthetic_policy();
    refusal_predicate(); facts_of_prepared_requests();
    std::cout << "host query properties passed\n"; return 0;
  }
  catch (const std::exception& error) { std::cerr << "host query test failed: " << error.what() << '\n'; return 1; }
}
