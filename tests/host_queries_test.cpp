// The typed queries a host asks instead of restating provider facts, and the worst-case cost of a prepared request.
#include "codecs/chat.h"
#include "codecs/gemini_request.h"
#include "codecs/messages.h"
#include "codecs/interactions_request.h"
#include "codecs/messages_request.h"
#include "codecs/responses_request.h"
#include "codecs/generations.h"
#include "core/enum_names.h"
#include "core/failure.h"
#include "core/native.h"
#include "core/pricing.h"
#include "descriptor/policy.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include "runtime/client.h"
#include "runtime/cost.h"
#include "sp/config_defaults.h"
#include <algorithm>
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
  names_round_trip<descriptor::EndpointBilling>(4);
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
    descriptor::Control::MaxToolCalls, descriptor::Control::ToolChoice};
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
    case Control::ToolChoice: return requires(Request r) { r.tool_choice; };
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
          control != descriptor::Control::MaxToolCalls && control != descriptor::Control::ToolChoice &&
          policy_accepts(d, control) != *declared) return false;
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
  const std::string chat_controls = R"("controls":["max_output_tokens","temperature","top_p","reasoning_effort","service_tier","tool_choice"])";
  CHECK(!controls_agree(load(replaced(text, chat_controls, R"("controls":["max_output_tokens","temperature","reasoning_effort","service_tier","tool_choice"])"))));
  CHECK(!controls_agree(load(replaced(text, chat_controls, R"("controls":["max_output_tokens","temperature","top_p","reasoning_effort","service_tier","tool_choice","store"])"))));
  CHECK(!controls_agree(load(replaced(text, chat_controls, R"("controls":["max_output_tokens","temperature","top_p","service_tier","tool_choice"])"))));
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
  // Every family's typed request carries a tool choice; a family whose list leaves it out answers false, and the
  // comparison with the request members fails until the data follows.
  for (const auto family : families) CHECK(policy->control_admitted(family, descriptor::Control::ToolChoice) == true);
  const std::string responses_controls = R"("controls":["max_output_tokens","temperature","top_p","reasoning_effort","reasoning_summary","service_tier","required_tool","store","max_tool_calls","tool_choice"])";
  const auto without_choice = load(replaced(text, responses_controls, R"("controls":["max_output_tokens","temperature","top_p","reasoning_effort","reasoning_summary","service_tier","required_tool","store","max_tool_calls"])"));
  CHECK(without_choice->control_admitted("openai.responses", descriptor::Control::ToolChoice) == false);
  CHECK(without_choice->control_admitted("openai.chat", descriptor::Control::ToolChoice) == true);
  CHECK(!controls_agree(without_choice));
  CHECK(descriptor::control_name(descriptor::Control::ToolChoice) == "tool_choice" && !descriptor::control_takes_values(descriptor::Control::ToolChoice));
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
  const std::string chat_controls = R"("controls":["max_output_tokens","temperature","top_p","reasoning_effort","service_tier","tool_choice"])";
  CHECK(refused(replaced(text, chat_controls, R"("controls":["max_tokens"])"), "/controls"));
  CHECK(refused(replaced(text, chat_controls, R"("controls":["temperature","temperature"])"), "/controls"));
  // A control that takes listed values needs values: Messages lists no reasoning efforts.
  CHECK(refused(replaced(text, R"("controls":["max_output_tokens","temperature","top_p","thinking_budget","tool_choice"])", R"("controls":["reasoning_effort"])"), "/controls"));
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

// ---- the worst-case cost ---------------------------------------------------------------------------------------

descriptor::CatalogueSnapshot catalogue_of(std::string_view json) {
  auto loaded = descriptor::load_catalogue(json);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("catalogue refused at " + error->pointer + ": " + error->expected);
  return std::get<descriptor::CatalogueSnapshot>(std::move(loaded));
}
// Prices in USD per token. 1e-6 is 1000 nano-USD a token. The models and the tools are not in the shipped data. zz-model-1
// bills cache writes dearer than input, so its input is bounded at 1250 nano-USD a token.
constexpr std::uint64_t zz_input = 1250, zz_output = 4000;
const auto priced = catalogue_of(R"({"version":1,"models":[
  {"id":"zz-model-1","input_limit":100000,"context_window":120000,
   "pricing":{"input":"0.000001","output":"0.000004","cache_read":"0.0000001","cache_write":"0.00000125",
              "tool:web_search_20250305":"0.01","tool:zz_search_20300101":"0.02","tool:web_search":"0.01","tool:image_generation":"0.04"}},
  {"id":"zz-flex","input_limit":1000,"pricing":{"input":"0.000001","output":"0.000001","input/flex":"0.0000005","output/flex":"0.0000005"}},
  {"id":"zz-dear","input_limit":50000,"pricing":{"input":"0.000003","output":"0.000009"}},
  {"id":"zz-unpriced","input_limit":50000},
  {"id":"zz-nolimit","pricing":{"input":"0.000001","output":"0.000001"}},
  {"id":"zz-band","input_limit":300000,"pricing":{"input":"0.000001","output":"0.000001","input+200000":"0.000002"}},
  {"id":"zz-big","input_limit":4000000000000,"pricing":{"input":"1000","output":"1000"}},
  {"id":"vendor/zz-routed","input_limit":10000,"origins":["https://openrouter.ai"],
   "pricing":{"input":"0.000001","output":"0.000002","ceiling_input":"0.000002","ceiling_output":"0.000004"}},
  {"id":"vendor/zz-second","input_limit":10000,"origins":["https://openrouter.ai"],
   "pricing":{"input":"0.000001","output":"0.000001","ceiling_input":"0.000003","ceiling_output":"0.000003"}},
  {"id":"vendor/zz-requested","input_limit":10000,"origins":["https://openrouter.ai"],
   "pricing":{"input":"0.000001","output":"0.000001","request":"0.0001","ceiling_input":"0.000002","ceiling_output":"0.000002","ceiling_request":"0.0003"}},
  {"id":"vendor/zz-fee-only","input_limit":10000,"origins":["https://openrouter.ai"],
   "pricing":{"input":"0.000001","output":"0.000001","request":"0.0002","ceiling_input":"0.000002","ceiling_output":"0.000002"}},
  {"id":"vendor/zz-unceilinged","input_limit":10000,"origins":["https://openrouter.ai"],"pricing":{"input":"0.000001","output":"0.000001"}},
  {"id":"vendor/zz-flat","input_limit":10000,"pricing":{"input":"0.000001","output":"0.000001","ceiling_input":"0.000002","ceiling_output":"0.000002"}}]})");

std::string dump(const pricing::WorstCase& wc) {
  std::string out = wc.total ? "total " + std::to_string(wc.total->nano_usd) : "no total";
  for (const auto& reason : wc.unbounded) out += "\n  " + std::string(pricing::to_string(reason.by)) + " [" + reason.subject + "] " + reason.detail;
  return out;
}
std::string detail_of(const pricing::WorstCase& wc, pricing::UnboundedBy by, std::string_view subject) {
  for (const auto& reason : wc.unbounded)
    if (reason.by == by && reason.subject == subject) return reason.detail;
  return "(none)";
}
bool has(const pricing::WorstCase& wc, pricing::UnboundedBy by, std::string_view subject = {}) {
  for (const auto& reason : wc.unbounded)
    if (reason.by == by && (subject.empty() || reason.subject == subject)) return true;
  return false;
}
std::uint64_t nano(const pricing::WorstCase& wc) {
  if (!wc.total) throw std::runtime_error("no bound: " + (wc.unbounded.empty() ? std::string("(no reason)") : std::string(pricing::to_string(wc.unbounded[0].by)) + " " + wc.unbounded[0].subject + " " + wc.unbounded[0].detail));
  return wc.total->nano_usd;
}
pricing::WorstCase bound(runtime::Client& client, runtime::Request request, std::optional<std::uint64_t> measured = std::nullopt,
                         std::uint32_t attempts = 1) {
  auto prepared = prepared_ok(client, std::move(request), attempts);
  runtime::WorstCaseOptions options;
  options.measured_input_tokens = measured;
  return runtime::worst_case_cost(prepared, *priced, options);
}

void worst_case_tokens() {
  auto chat = client_of(descriptor_of("openai.chat"));
  // Measured input and the output cap, at the dearest input rate and the output rate.
  auto wc = bound(chat, chat_request("zz-model-1", 1000), 2000);
  CHECK(nano(wc) == 2000u * zz_input + 1000u * zz_output && wc.unbounded.empty() && wc.entry == "zz-model-1" && wc.model == "zz-model-1");
  CHECK(wc.input_tokens == 2000u && wc.output_tokens == 1000u && wc.invocations == 1u && wc.attempts == 1u && wc.tool_uses.empty());
  CHECK(wc.total->evidence == Evidence::Derived && wc.total->rounding == CostRounding::CeilingCatalogueProduct);
  CHECK(!wc.route_ceiling && !wc.pin && !wc.pinned);
  // Without a count, the catalogue's input limit bounds the input; a count above the limit is bounded by it.
  CHECK(nano(bound(chat, chat_request("zz-model-1", 1000))) == 100'000u * zz_input + 1000u * zz_output);
  CHECK(nano(bound(chat, chat_request("zz-model-1", 1000), 5'000'000)) == 100'000u * zz_input + 1000u * zz_output);
  // The attempts the retry policy allows multiply the bound.
  CHECK(nano(bound(chat, chat_request("zz-model-1", 1000), 2000, 3)) == 3 * (2000u * zz_input + 1000u * zz_output));
  CHECK(bound(chat, chat_request("zz-model-1", 1000), 2000, 3).attempts == 3u);
  // The dearest rate over the counters an input may bill (cache writes here), and over the bands: a request that
  // may pass 200000 input tokens is bounded at the long-context rate.
  CHECK(nano(bound(chat, chat_request("zz-band", 10), 1000)) == 1000u * 1000u + 10u * 1000u);
  CHECK(nano(bound(chat, chat_request("zz-band", 10))) == 300'000u * 2000u + 10u * 1000u);
  // A count the host measured for a model with no limit stands in for the limit.
  CHECK(nano(bound(chat, chat_request("zz-nolimit", 10), 500)) == 500u * 1000u + 10u * 1000u);
}

void worst_case_unbounded() {
  auto chat = client_of(descriptor_of("openai.chat"));
  // No output cap: the reason names it, and no total.
  auto wc = bound(chat, chat_request("zz-model-1", std::nullopt), 2000);
  CHECK(!wc.total && has(wc, pricing::UnboundedBy::OutputCap) && wc.unbounded.size() == 1);
  // A model the catalogue does not price, at all or for the figure the bound needs.
  wc = bound(chat, chat_request("zz-not-in-the-catalogue", 100), 100);
  CHECK(!wc.total && has(wc, pricing::UnboundedBy::ModelPrice, "zz-not-in-the-catalogue"));
  CHECK(has(bound(chat, chat_request("zz-unpriced", 100), 100), pricing::UnboundedBy::ModelPrice, "zz-unpriced"));
  // Neither a count nor a limit: nothing bounds the input.
  CHECK(has(bound(chat, chat_request("zz-nolimit", 100)), pricing::UnboundedBy::InputBound, "zz-nolimit"));
  // Several reasons are all named.
  wc = bound(chat, chat_request("zz-nolimit", std::nullopt));
  CHECK(has(wc, pricing::UnboundedBy::OutputCap) && has(wc, pricing::UnboundedBy::InputBound) && wc.unbounded.size() == 2);
  // Tokens past 64 bits.
  CHECK(has(bound(chat, chat_request("zz-big", 100), 4'000'000'000'000u), pricing::UnboundedBy::Overflow, "zz-big"));
  // The reasons have names.
  CHECK(pricing::to_string(pricing::UnboundedBy::ToolCap) == "ToolCap" && pricing::to_string(pricing::UnboundedBy::OutputCap) == "OutputCap");
  // A request that did not prepare, a handle with nothing, an endpoint call and plugins have no bound either.
  runtime::PreparedRequest none;
  CHECK(!runtime::worst_case_cost(none, *priced).total && has(runtime::worst_case_cost(none, *priced), pricing::UnboundedBy::Unmodeled));
  CHECK(!runtime::worst_case_cost(none).total);
  auto gateway = client_of(openrouter_chat());
  auto request = chat_request("vendor/zz-routed", 100);
  request.plugins.push_back(OpenRouterPlugin{"moderation", nullptr});
  wc = bound(gateway, request, 100);
  CHECK(!wc.total && has(wc, pricing::UnboundedBy::Unmodeled, "OpenRouter plugins"));
  // A generation statistics read is not billed: its bound is zero, though its result carries another call's cost.
  auto generations = client_of(openrouter_chat());
  auto endpoint = generations.prepare(generations::Get{"gen-1"});
  CHECK(endpoint.valid());
  wc = runtime::worst_case_cost(endpoint.prepared(), *priced);
  CHECK(wc.total && wc.total->nano_usd == 0 && wc.unbounded.empty());
}

void worst_case_tools() {
  // Messages server tools: three invocations (the request and two uses); each later one is re-prompted with the
  // tool results, so it is bounded at the limit. 2000 + 2 x 100000 input tokens, 3 x 1000 output tokens, 2 uses.
  auto messages = client_of(descriptor_of("anthropic.messages"));
  auto request = messages::Request::simple("zz-model-1", "hi", 1000);
  request.tools.push_back(messages_tool("memory_20250818", "memory"));
  request.tools.push_back(messages_tool("web_search_20250305", "web_search", 2));
  auto wc = bound(messages, request, 2000);
  CHECK(wc.invocations == 3u && wc.input_tokens == 202'000u && wc.output_tokens == 3000u && wc.invocation_input_tokens == 100'000u);
  CHECK(nano(wc) == 202'000u * zz_input + 3000u * zz_output + 2u * 10'000'000u);
  CHECK(wc.tool_fees_nano_usd == 20'000'000u && wc.tool_uses.size() == 1 && wc.tool_uses[0].first == "web_search_20250305" && wc.tool_uses[0].second == 2u);
  // Several invocations need the limit, even with a count.
  CHECK(has(bound(messages, [] { auto r = messages::Request::simple("zz-nolimit", "hi", 10); r.tools.push_back(messages_tool("web_search_20250305", "web_search", 1)); return r; }(), 100),
            pricing::UnboundedBy::InputBound, "zz-nolimit"));
  // The tools' fees are once per attempt; the tokens are per invocation.
  CHECK(nano(bound(messages, request, 2000, 2)) == 2 * nano(wc));
  // No cap on a tool that takes one, and a tool that takes none: the reasons differ.
  request.tools[1].max_uses.reset();
  wc = bound(messages, request, 2000);
  CHECK(!wc.total && detail_of(wc, pricing::UnboundedBy::ToolCap, "web_search_20250305").find("sets no max_uses") != std::string::npos);
  auto code = messages::Request::simple("zz-model-1", "hi", 1000);
  code.tools.push_back(messages_tool("code_execution_20250825", "code_execution"));
  wc = bound(messages, code, 2000);
  CHECK(!wc.total && detail_of(wc, pricing::UnboundedBy::ToolCap, "code_execution_20250825").find("admits no max_uses") != std::string::npos);
  // A tool the entry lists no fee for.
  auto unfeed = messages::Request::simple("zz-dear", "hi", 100);
  unfeed.tools.push_back(messages_tool("web_search_20250305", "web_search", 1));
  wc = bound(messages, unfeed, 100);
  CHECK(!wc.total && detail_of(wc, pricing::UnboundedBy::ToolFee, "web_search_20250305").find("tool:web_search_20250305") != std::string::npos);

  // Responses: one cap for the hosted tools, bounded at the dearest fee of the group.
  auto responses = client_of(descriptor_of("openai.responses"));
  auto hosted = responses::Request::simple("zz-model-1", "hi", 100);
  hosted.hosted_tools.push_back(responses::WebSearchTool{});
  hosted.hosted_tools.push_back(responses::ImageGenerationTool{});
  hosted.max_tool_calls = 3;
  wc = bound(responses, hosted, 1000);
  CHECK(wc.invocations == 4u && wc.tool_uses.size() == 1 && wc.tool_uses[0].first == "image_generation" && wc.tool_uses[0].second == 3u);
  CHECK(nano(wc) == (1000u + 3u * 100'000u) * zz_input + 4u * 100u * zz_output + 3u * 40'000'000u);
  hosted.max_tool_calls.reset();
  wc = bound(responses, hosted, 1000);
  CHECK(!wc.total && detail_of(wc, pricing::UnboundedBy::ToolCap, "web_search").find("sets no max_tool_calls") != std::string::npos);
  // Without the cap the request does not bound its invocations either; both are named.
  CHECK(has(wc, pricing::UnboundedBy::Invocations) && wc.unbounded.size() == 2);
  // Google's built-in tools take no cap.
  auto interactions = client_of(descriptor_of("google.interactions"));
  auto search = interactions::Request::simple("zz-model-1", "hi", 100);
  search.builtin_tools.push_back(interactions::GoogleSearchTool{});
  wc = bound(interactions, search, 100);
  CHECK(!wc.total && detail_of(wc, pricing::UnboundedBy::ToolCap, "google_search").find("no cap") != std::string::npos);
  // Without server tools the same Interactions request is bounded: the standard tier bills at the unlabelled rates.
  CHECK(nano(bound(interactions, interactions::Request::simple("zz-model-1", "hi", 100), 100)) == 100u * zz_input + 100u * zz_output);
}

void worst_case_tiers_and_fallbacks() {
  auto chat = client_of(descriptor_of("openai.chat"));
  auto flex = chat_request("zz-flex", 100);
  flex.service_tier = "flex";
  CHECK(nano(bound(chat, flex, 100)) == 100u * 500u + 100u * 500u);
  // The standard tier's own name is the unlabelled rates; a tier the entry does not price has no price.
  auto standard = chat_request("zz-flex", 100);
  standard.service_tier = "default";
  CHECK(nano(bound(chat, standard, 100)) == 100u * 1000u + 100u * 1000u);
  auto priority = chat_request("zz-flex", 100);
  priority.service_tier = "priority";
  auto wc = bound(chat, priority, 100);
  CHECK(!wc.total && detail_of(wc, pricing::UnboundedBy::TierPrice, "zz-flex").find("'priority'") != std::string::npos);
  auto automatic = chat_request("zz-flex", 100);
  automatic.service_tier = "auto";
  CHECK(has(bound(chat, automatic, 100), pricing::UnboundedBy::TierPrice, "zz-flex"));
  // Fallback models: the dearest candidate sets the bound, and an unpriced one has none. The count is the requested
  // model's; a fallback is bounded by its own limit.
  auto gateway = client_of(openrouter_chat());
  auto request = chat_request("vendor/zz-routed", 100);
  request.models = {"vendor/zz-second", "vendor/zz-routed"};
  request.provider = OpenRouterRouting{};
  request.provider->max_price = OpenRouterMaxPrice{3'000'000'000u, 4'000'000'000u, std::nullopt, std::nullopt, std::nullopt};
  wc = bound(gateway, request, 500);
  // Routed: priced at the route ceiling, the dearest over the candidates (3 USD and 4 USD per million tokens).
  CHECK(wc.route_ceiling && wc.pin && wc.pin->prompt_nano_usd == 3'000'000'000u && wc.pin->completion_nano_usd == 4'000'000'000u && wc.pinned);
  // The requested model, counted at 500 tokens, costs less than the fallback at its own limit of 10000.
  CHECK(nano(wc) == 10'000u * 3000u + 100u * 4000u && wc.model == "vendor/zz-second" && wc.entry == "vendor/zz-second");
  request.models = {"vendor/zz-second", "vendor/zz-nowhere"};
  CHECK(has(bound(gateway, request, 500), pricing::UnboundedBy::ModelPrice, "vendor/zz-nowhere"));
}

void worst_case_route_ceiling() {
  auto gateway = client_of(openrouter_chat());
  auto request = chat_request("vendor/zz-routed", 100);
  // The ceiling is the price; the request must carry it as provider.max_price, which it does not yet.
  auto wc = bound(gateway, request, 1000);
  CHECK(wc.route_ceiling && nano(wc) == 1000u * 2000u + 100u * 4000u);
  CHECK(wc.pin && wc.pin->prompt_nano_usd == 2'000'000'000u && wc.pin->completion_nano_usd == 4'000'000'000u && !wc.pin->image_nano_usd && !wc.pinned);
  // The request's own cap below the ceiling lowers the pin, field by field; it is pinned when it already says so.
  request.provider = OpenRouterRouting{};
  request.provider->max_price = OpenRouterMaxPrice{1'000'000'000u, std::nullopt, std::nullopt, std::nullopt, 7'000'000'000u};
  wc = bound(gateway, request, 1000);
  CHECK(wc.pin && wc.pin->prompt_nano_usd == 1'000'000'000u && wc.pin->completion_nano_usd == 4'000'000'000u && wc.pin->request_nano_usd == 7'000'000'000u && !wc.pinned);
  request.provider->max_price = OpenRouterMaxPrice{1'000'000'000u, 4'000'000'000u, std::nullopt, std::nullopt, 7'000'000'000u};
  wc = bound(gateway, request, 1000);
  CHECK(wc.pinned && wc.pin->prompt_nano_usd == 1'000'000'000u);
  // A cap above the ceiling does not raise it.
  request.provider->max_price = OpenRouterMaxPrice{9'000'000'000u, 9'000'000'000u, std::nullopt, std::nullopt, std::nullopt};
  wc = bound(gateway, request, 1000);
  CHECK(wc.pin->prompt_nano_usd == 2'000'000'000u && !wc.pinned);
  // A gateway entry without a ceiling: the router may pick any price.
  wc = bound(gateway, chat_request("vendor/zz-unceilinged", 100), 1000);
  CHECK(!wc.total && has(wc, pricing::UnboundedBy::RouteCeiling, "vendor/zz-unceilinged"));
  // An entry with a ceiling at an origin that is not a routing gateway cannot be pinned there.
  auto direct = client_of(descriptor_of("openai.chat"));
  wc = bound(direct, chat_request("vendor/zz-flat", 100), 1000);
  CHECK(!wc.total && has(wc, pricing::UnboundedBy::RouteCeiling, "vendor/zz-flat"));
  // A listed per-request ceiling is pinned like the token ceilings and charged once. 0.0003 USD is 300000 nano-USD.
  auto fee = bound(gateway, chat_request("vendor/zz-requested", 100), 1000);
  CHECK(nano(fee) == 1000u * 2000u + 100u * 2000u + 300'000u);
  CHECK(fee.pin && fee.pin->request_nano_usd == 300'000u && fee.pin->prompt_nano_usd == 2'000'000'000u && !fee.pinned);
  // The request's own cap lowers it, field by field, and it is pinned once the request says so.
  request = chat_request("vendor/zz-requested", 100);
  request.provider = OpenRouterRouting{};
  request.provider->max_price = OpenRouterMaxPrice{2'000'000'000u, 2'000'000'000u, std::nullopt, std::nullopt, 100'000u};
  fee = bound(gateway, request, 1000);
  CHECK(fee.pin && fee.pin->request_nano_usd == 100'000u && fee.pinned && nano(fee) == 1000u * 2000u + 100u * 2000u + 300'000u);
  request.provider->max_price->request_nano_usd = 900'000u;
  fee = bound(gateway, request, 1000);
  CHECK(fee.pin->request_nano_usd == 300'000u && !fee.pinned);
  // One pin covers every candidate: a fallback that lists no per-request ceiling is bounded at the pinned one too, and
  // at the dearest token ceilings (3 USD a million), on its own limit of 10000 input tokens.
  request = chat_request("vendor/zz-requested", 100);
  request.models = {"vendor/zz-second"};
  fee = bound(gateway, request, 1000);
  CHECK(nano(fee) == 10'000u * 3000u + 100u * 3000u + 300'000u && fee.model == "vendor/zz-second");
  CHECK(fee.pin && fee.pin->request_nano_usd == 300'000u && fee.pin->prompt_nano_usd == 3'000'000'000u);
  // A request fee without a ceiling is charged and pinned as well, and an entry with no request fee pins none.
  fee = bound(gateway, chat_request("vendor/zz-fee-only", 100), 1000);
  CHECK(nano(fee) == 1000u * 2000u + 100u * 2000u + 200'000u && fee.pin && fee.pin->request_nano_usd == 200'000u && !fee.pinned);
  CHECK(!bound(gateway, chat_request("vendor/zz-routed", 100), 1000).pin->request_nano_usd);
  // The route ceiling over the largest max_price amount.
  const auto huge = catalogue_of(R"({"version":1,"models":[{"id":"vendor/zz-huge","input_limit":10,"pricing":{"input":"1","output":"1","ceiling_input":"10000000","ceiling_output":"1"}}]})");
  auto prepared = prepared_ok(gateway, chat_request("vendor/zz-huge", 1), 1);
  CHECK(has(runtime::worst_case_cost(prepared, *huge), pricing::UnboundedBy::RouteCeiling, "vendor/zz-huge"));
}

// The bound never falls under what the catalogue would charge for usage that reaches the caps, counter by counter.
void worst_case_covers_usage() {
  auto messages = client_of(descriptor_of("anthropic.messages"));
  auto request = messages::Request::simple("zz-model-1", "hi", 1000);
  request.tools.push_back(messages_tool("web_search_20250305", "web_search", 2));
  const auto wc = bound(messages, request, 2000);
  const ServedModel served{"zz-model-1", "zz-model-1", ServedRelation::Exact};
  // The dearest mix of input counters: all of the bound's input as cache writes (the dearest input rate).
  for (const bool writes : {false, true}) {
    Usage usage;
    usage.input_total = Count{wc.input_tokens, Evidence::Reported};
    usage.output_total = Count{wc.output_tokens, Evidence::Reported};
    if (writes) usage.cache_write = Count{wc.input_tokens, Evidence::Reported};
    const auto estimate = pricing::estimate(*priced, "anthropic.messages", "https://api.anthropic.com", served, usage);
    CHECK(estimate.total && estimate.total->nano_usd + wc.tool_fees_nano_usd <= wc.total->nano_usd);
    if (writes) CHECK(estimate.total->nano_usd > wc.input_tokens * 1000u);
  }
}

// The synthetic policy (an extra tool, an unknown model) through the estimator unchanged.
void worst_case_synthetic_policy() {
  auto text = policy_text();
  const std::string memory = R"({"client":true,"max_uses":false,"name":"memory","type":"memory_20250818"},)";
  text = replaced(text, memory, R"({"max_uses":true,"name":"zz_search","type":"zz_search_20300101"},)" + memory);
  const auto policy = load(text);
  auto with_priced = policy->with_catalogue(priced);
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(with_priced));
  auto snapshot = std::get<descriptor::PolicySnapshot>(with_priced);
  auto messages = client_of(descriptor_of("anthropic.messages", snapshot));
  auto request = messages::Request::simple("zz-model-1", "hi", 1000);
  request.tools.push_back(messages_tool("zz_search_20300101", "zz_search", 4));
  auto prepared = prepared_ok(messages, request);
  // The policy's own catalogue answers when none is passed, and the new tool is priced under its own unit.
  const auto wc = runtime::worst_case_cost(prepared, runtime::WorstCaseOptions{2000});
  CHECK(wc.invocations == 5u && wc.tool_uses.size() == 1 && wc.tool_uses[0].first == "zz_search_20300101" && wc.tool_fees_nano_usd == 4u * 20'000'000u);
  // Without a fee for the new tool in the catalogue, the unit is named.
  const auto bare = catalogue_of(R"({"version":1,"models":[{"id":"zz-model-1","input_limit":1000,"pricing":{"input":"0.000001","output":"0.000001"}}]})");
  const auto without = runtime::worst_case_cost(prepared, *bare);
  CHECK(!without.total && detail_of(without, pricing::UnboundedBy::ToolFee, "zz_search_20300101").find("tool:zz_search_20300101") != std::string::npos);
  // The shipped catalogue knows no zz-model-1 at all.
  const auto shipped = runtime::worst_case_cost(prepared, *descriptor::builtin_catalogue());
  CHECK(!shipped.total && has(shipped, pricing::UnboundedBy::ModelPrice, "zz-model-1"));
}

// ---- model facts from the catalogue --------------------------------------------------------------------------

// What the catalogue states about a model's temperature and control values, the questions a host's reject list asks.
void model_facts() {
  using descriptor::Control;
  const auto policy = descriptor::builtin_policy();
  CHECK(policy->temperature_admitted("openai.chat", "https://api.openai.com", "gpt-6-luna") == false);
  CHECK(policy->temperature_admitted("openai.responses", "https://api.openai.com", "o3-mini") == false);
  CHECK(policy->temperature_admitted("anthropic.messages", "https://api.anthropic.com", "claude-opus-4-8") == false);
  // Nothing stated: send it, and the provider answers.
  CHECK(!policy->temperature_admitted("openai.chat", "https://api.example.test", "gpt-6-luna"));
  CHECK(!policy->temperature_admitted("anthropic.messages", "https://api.anthropic.com", "claude-haiku-4-5-20251001"));
  CHECK(!policy->temperature_admitted("anthropic.messages", "https://openrouter.ai", "anthropic/claude-opus-4.7"));
  CHECK(!policy->temperature_admitted("zz.family", "https://zz.example", "zz-model-1"));
  for (const auto family : families)
    for (const auto control : all_controls) CHECK(!policy->control_value_admitted(family, "https://api.openai.com", "gpt-6-luna", control, "low"));
  // The retired members say nothing.
  CHECK(!policy->temperature_forbidden_by_generation("anthropic.messages", "claude-opus-4-8"));
  for (const auto family : families) CHECK(policy->family(family)->temperature_forbidden_model_prefixes.empty());
  for (const auto control : all_controls)
    CHECK(descriptor::control_takes_values(control) == (control == Control::ReasoningEffort || control == Control::ReasoningSummary ||
                                                         control == Control::ThinkingLevel || control == Control::ServiceTier));

  // A model whose entry narrows the family's thinking levels.
  const auto facts = catalogue_of(R"({"version":1,"models":[{"id":"x-model","families":["google.interactions"],"control_values":{"thinking_level":["low","high"]}}]})");
  auto attached = policy->with_catalogue(facts);
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(attached));
  const auto snapshot = std::get<descriptor::PolicySnapshot>(attached);
  const auto d = descriptor_of("google.interactions", snapshot);
  const auto origin = d.base_url();
  CHECK(snapshot->control_value_admitted("google.interactions", origin, "x-model", Control::ThinkingLevel, "high") == true);
  CHECK(snapshot->control_value_admitted("google.interactions", origin, "x-model", Control::ThinkingLevel, "medium") == false);
  CHECK(!snapshot->control_value_admitted("google.interactions", origin, "x-model", Control::ServiceTier, "flex"));
  CHECK(!snapshot->control_value_admitted("google.interactions", origin, "x-model", Control::Temperature, "low"));
  CHECK(!snapshot->control_value_admitted("google.interactions", origin, "zz-model-1", Control::ThinkingLevel, "medium"));
  CHECK(!snapshot->control_value_admitted("google.generate", origin, "x-model", Control::ThinkingLevel, "medium"));
  auto client = client_of(d);
  auto request = interactions::Request::simple("x-model", "hi", 1000);
  request.thinking_level = "high";
  prepared_ok(client, request);
  auto reason_of = [&](const interactions::Request& value) {
    auto prepared = client.prepare(value, run_with(1));
    CHECK(prepared.error() && prepared.error()->kind == ErrorKind::InvalidRequest);
    return prepared.error() ? prepared.error()->reason : std::string();
  };
  request.thinking_level = "medium";
  CHECK(reason_of(request) == "request.choice.control_value_not_admitted_for_the_model");
  // A value the family does not list keeps its own refusal; a model no entry names takes any value the family lists.
  request.thinking_level = "ultra";
  CHECK(reason_of(request) == "request.choice.unsupported_typed_semantic_control");
  auto other = interactions::Request::simple("zz-model-1", "hi", 1000);
  other.thinking_level = "medium";
  prepared_ok(client, other);

  // An entry's defaults, over each family, must take a value the deciding list admits. The family's default counts:
  // Responses summarizes at "auto" and reasons at "low" by default.
  auto pointer = [&](std::string_view models) {
    const auto loaded = policy->with_catalogue(catalogue_of(std::string(R"({"version":1,"models":[)") + std::string(models) + "]}"));
    const auto* error = std::get_if<descriptor::ConfigError>(&loaded);
    return error ? error->pointer : std::string("attached");
  };
  CHECK(pointer(R"({"id":"x-model","families":["google.interactions"],"control_values":{"thinking_level":["low","high"]},"defaults":{"google.interactions":{"thinking_level":"medium"}}})") ==
        "/model_catalogue/models/x-model/defaults/google.interactions");
  CHECK(pointer(R"({"id":"x-model","families":["openai.responses"],"control_values":{"reasoning_summary":["concise"]}})") ==
        "/model_catalogue/models/x-model/defaults/openai.responses");
  // A list that shares no value with a family that lists values for the control.
  CHECK(pointer(R"({"id":"x-model","families":["google.interactions"],"control_values":{"service_tier":["flex"]}})") ==
        "/model_catalogue/models/x-model/control_values/service_tier");
  CHECK(pointer(R"({"id":"x-model","control_values":{"reasoning_effort":["high"]}})") == "/model_catalogue/models/x-model/defaults/openai.responses");
  CHECK(pointer(R"({"id":"x-model","control_values":{"reasoning_effort":["high"]},"defaults":{"openai.responses":{"reasoning_effort":"high"}}})") == "attached");
  CHECK(pointer(R"({"id":"x-model","families":["openai.chat"],"control_values":{"reasoning_effort":["high"]}})") == "attached");
  // Across entries: a profile's list that leaves out an exact entry's default is refused when attached; with the
  // default inside the list the request that inherits it is sent, and an explicit value outside it is refused.
  const std::string profile = R"({"id":"x-","match":"prefix","families":["google.interactions"],"control_values":{"thinking_level":["low"]}})";
  CHECK(pointer(profile + R"(,{"id":"x-model","families":["google.interactions"],"defaults":{"google.interactions":{"thinking_level":"high"}}})") ==
        "/model_catalogue/models/x-model/defaults/google.interactions");
  auto inherited = policy->with_catalogue(catalogue_of(R"({"version":1,"models":[)" + profile +
      R"(,{"id":"x-model","families":["google.interactions"],"defaults":{"google.interactions":{"thinking_level":"low"}}}]})"));
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(inherited));
  auto inheriting = client_of(descriptor_of("google.interactions", std::get<descriptor::PolicySnapshot>(inherited)));
  prepared_ok(inheriting, interactions::Request::simple("x-model", "hi", 1000));
  auto explicit_level = interactions::Request::simple("x-model", "hi", 1000);
  explicit_level.thinking_level = "high";
  auto prepared = inheriting.prepare(explicit_level, run_with(1));
  CHECK(prepared.error() && prepared.error()->reason == "request.choice.control_value_not_admitted_for_the_model");
}

// The embedded temperature profiles take part in the lookup that prices a call. A host's shorter profile with prices,
// merged under the embedded catalogue, is shadowed for the ids an embedded profile covers at that profile's origin, so
// the bound there names the model; at another origin, and for an id no embedded profile covers, the host's prices bound it.
void worst_case_shadowed_profile() {
  const auto host = catalogue_of(R"({"version":1,"models":[{"id":"gpt-","match":"prefix","input_limit":1000,"pricing":{"input":"0.000001","output":"0.000001"}}]})");
  const descriptor::CatalogueSnapshot layers[]{host, descriptor::builtin_catalogue()};
  auto merged = descriptor::merge_catalogues(layers);
  CHECK(std::holds_alternative<descriptor::CatalogueSnapshot>(merged));
  const auto& catalogue = *std::get<descriptor::CatalogueSnapshot>(merged);
  runtime::WorstCaseOptions options;
  options.measured_input_tokens = 100;
  auto direct = client_of(descriptor_of("openai.chat"));
  const auto shadowed = runtime::worst_case_cost(prepared_ok(direct, chat_request("gpt-5-mini", 100)), catalogue, options);
  CHECK(!shadowed.total && detail_of(shadowed, pricing::UnboundedBy::ModelPrice, "gpt-5-mini") == "the catalogue has no price for the model at the origin");
  CHECK(nano(runtime::worst_case_cost(prepared_ok(direct, chat_request("gpt-4o", 100)), catalogue, options)) == 100u * 1000u + 100u * 1000u);
  auto local = client_of(descriptor::presets::loopback_chat());
  CHECK(nano(runtime::worst_case_cost(prepared_ok(local, chat_request("gpt-5-mini", 100)), catalogue, options)) == 100u * 1000u + 100u * 1000u);
}

// ---- endpoint billing ----------------------------------------------------------------------------------------

using Billing = descriptor::EndpointBilling;
// The built-in policy text with `endpoints` in place of its endpoint facts.
std::string policy_with_endpoints(std::string_view endpoints) {
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  const auto root = std::get<json::Document>(parsed).root();
  std::string text = "{";
  for (auto member : root.members()) {
    if (text.size() > 1) text += ',';
    text += json::quote(member.key) + ":" + (member.key == "endpoints" ? std::string(endpoints) : member.value.dump());
  }
  return text + "}";
}
// The built-in endpoint facts without `billing` and `carried_tier`, as a policy written before they existed states them.
std::string facts_without_billing() {
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  const auto root = std::get<json::Document>(parsed).root();
  std::string out = "[";
  for (auto fact : root.get("endpoints").elements()) {
    if (out.size() > 1) out += ',';
    std::string members;
    for (auto member : fact.members()) {
      if (member.key == "billing" || member.key == "carried_tier") continue;
      members += (members.empty() ? "" : ",") + json::quote(member.key) + ":" + member.value.dump();
    }
    out += "{" + members + "}";
  }
  return out + "]";
}
// One endpoint fact: `fact` (its text without the closing brace) and `members`.
std::string one_fact(std::string_view fact, std::string_view members = {}) {
  return policy_with_endpoints("[" + std::string(fact) + std::string(members) + "}]");
}
constexpr std::string_view openai = "https://api.openai.com", anthropic = "https://api.anthropic.com",
                           google = "https://generativelanguage.googleapis.com", openrouter = "https://openrouter.ai";
const std::string images_fact = R"({"endpoint":"images","shape":"openai","origins":["https://api.openai.com"],"paths":{"generate":"/v1/images/generations","edit":"/v1/images/edits"},"charge_on_accept":true)";
const std::string videos_fact = R"({"endpoint":"videos","shape":"openrouter","origins":["https://openrouter.ai"],"paths":{"submit":"/api/v1/videos","poll":"/api/v1/videos/{id}","download":"/api/v1/videos/{id}/content"},"charge_on_accept":true)";
const std::string files_fact = R"({"endpoint":"files","shape":"google","origins":["https://generativelanguage.googleapis.com"],"paths":{"upload":"/upload/v1beta/files","get":"/v1beta/{id}","start":"/upload/v1beta/files"},"auth":"x-goog-api-key")";
const std::string batches_fact = R"({"endpoint":"batches","shape":"anthropic","origins":["https://api.anthropic.com"],"paths":{"submit":"/v1/messages/batches","get":"/v1/messages/batches/{id}"},"auth":"x-api-key","charge_on_accept":true)";
const std::string moderation_fact = R"({"endpoint":"moderation","shape":"openai","origins":["https://api.openai.com"],"paths":{"check":"/v1/moderations"})";

void endpoint_billing_forms() {
  // One value for the whole endpoint: every call, a Google upload step that has no path of its own included.
  auto policy = load(one_fact(files_fact, R"(,"billing":"none")"));
  for (const auto operation : {"upload", "get", "start", "list", "content", "part", "query", "cancel"})
    CHECK(policy->endpoint_billing("files", google, operation) == Billing::None);
  CHECK(policy->carried_tier("files", google).empty());
  // At another origin, or for another endpoint, the policy says nothing.
  CHECK(policy->endpoint_billing("files", openai, "get") == Billing::Unstated && policy->endpoint_billing("batches", google, "get") == Billing::Unstated);
  // Per operation.
  policy = load(one_fact(images_fact, R"(,"billing":{"generate":"model","edit":"model"})"));
  CHECK(policy->endpoint_billing("images", openai, "generate") == Billing::Model && policy->endpoint_billing("images", openai, "edit") == Billing::Model);
  CHECK(policy->endpoint_billing("images", openai, "") == Billing::Unstated && policy->endpoint_billing("images", openai, "zz") == Billing::Unstated);
  policy = load(one_fact(videos_fact, R"(,"billing":{"submit":"model","poll":"none","download":"none"})"));
  CHECK(policy->endpoint_billing("videos", openrouter, "submit") == Billing::Model && policy->endpoint_billing("videos", openrouter, "poll") == Billing::None &&
        policy->endpoint_billing("videos", openrouter, "download") == Billing::None);
  // Carried requests, priced under the fact's tier. An operation of the shape without a path may be named, and one the
  // statement leaves out is unstated.
  policy = load(one_fact(batches_fact, R"(,"billing":{"submit":"carried","get":"none","delete":"none"},"carried_tier":"batch")"));
  CHECK(policy->endpoint_billing("batches", anthropic, "submit") == Billing::Carried && policy->endpoint_billing("batches", anthropic, "get") == Billing::None &&
        policy->endpoint_billing("batches", anthropic, "delete") == Billing::None && policy->endpoint_billing("batches", anthropic, "cancel") == Billing::Unstated);
  CHECK(policy->carried_tier("batches", anthropic) == "batch" && policy->carried_tier("batches", openai).empty());
  // A fact without charge_on_accept may state any admitted value.
  CHECK(load(one_fact(moderation_fact, R"(,"billing":"none")"))->endpoint_billing("moderation", openai, "check") == Billing::None);
  CHECK(load(one_fact(moderation_fact, R"(,"billing":"model")"))->endpoint_billing("moderation", openai, "check") == Billing::Model);
  // Without the member, every operation is unstated.
  policy = load(one_fact(images_fact));
  CHECK(policy->endpoint_billing("images", openai, "generate") == Billing::Unstated && policy->endpoint_billing("images", openai, "edit") == Billing::Unstated &&
        policy->carried_tier("images", openai).empty());
  // The statement survives another catalogue.
  auto recatalogued = descriptor::builtin_policy()->with_catalogue(descriptor::builtin_catalogue());
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(recatalogued));
  const auto& other = std::get<descriptor::PolicySnapshot>(recatalogued);
  CHECK(other->endpoint_billing("videos", openrouter, "poll") == Billing::None && other->endpoint_billing("videos", openrouter, "submit") == Billing::Model &&
        other->carried_tier("batches", openai) == "batch");
  // The names.
  CHECK(to_string(Billing::Unstated) == "Unstated" && to_string(Billing::Carried) == "Carried" && from_string<Billing>("None") == Billing::None);
}

void endpoint_billing_refusals() {
  constexpr auto billing = "/endpoints/billing", tier = "/endpoints/carried_tier";
  // An operation the shape does not have, and a statement that leaves out an operation of `paths`.
  CHECK(refused(one_fact(images_fact, R"(,"billing":{"generate":"model","edit":"model","speak":"model"})"), billing));
  CHECK(refused(one_fact(R"({"endpoint":"images","shape":"openrouter","origins":["https://openrouter.ai"],"paths":{"generate":"/api/v1/images"},"charge_on_accept":true)",
                         R"(,"billing":{"generate":"model","edit":"model"})"), billing));
  CHECK(refused(one_fact(images_fact, R"(,"billing":{"generate":"model"})"), billing));
  CHECK(refused(one_fact(images_fact, R"(,"billing":{})"), billing));
  // A value other than none, model or carried.
  for (const auto value : {R"("free")", R"("Model")", R"("unstated")", "1", "null", "true", R"(["none"])", R"("")"})
    CHECK(refused(one_fact(moderation_fact, std::string(R"(,"billing":)") + value), billing));
  CHECK(refused(one_fact(images_fact, R"(,"billing":{"generate":"model","edit":"free"})"), billing));
  CHECK(refused(one_fact(images_fact, R"(,"billing":{"generate":"model","edit":null})"), billing));
  // `carried` only for a batch submit, `model` only where the request names a model.
  CHECK(refused(one_fact(images_fact, R"(,"billing":{"generate":"carried","edit":"model"},"carried_tier":"batch")"), billing));
  CHECK(refused(one_fact(batches_fact, R"(,"billing":{"submit":"carried","get":"carried"},"carried_tier":"batch")"), billing));
  CHECK(refused(one_fact(batches_fact, R"(,"billing":"carried","carried_tier":"batch")"), billing));
  CHECK(refused(one_fact(batches_fact, R"(,"billing":{"submit":"model","get":"none"})"), billing));
  CHECK(refused(one_fact(files_fact, R"(,"billing":{"upload":"model","get":"none","start":"none"})"), billing));
  CHECK(refused(one_fact(files_fact, R"(,"billing":"model")"), billing));
  CHECK(refused(one_fact(videos_fact, R"(,"billing":{"submit":"model","poll":"model","download":"none"})"), billing));
  CHECK(refused(one_fact(videos_fact, R"(,"billing":"model")"), billing));
  // A fact that charges once it accepts a call bills one of its operations.
  CHECK(refused(one_fact(images_fact, R"(,"billing":{"generate":"none","edit":"none"})"), billing));
  CHECK(refused(one_fact(videos_fact, R"(,"billing":"none")"), billing));
  CHECK(refused(one_fact(batches_fact, R"(,"billing":{"submit":"none","get":"none"})"), billing));
  // The tier: with carried requests only, and a tier name.
  CHECK(refused(one_fact(batches_fact, R"(,"billing":{"submit":"carried","get":"none"})"), tier));
  CHECK(refused(one_fact(moderation_fact, R"(,"billing":"none","carried_tier":"batch")"), tier));
  CHECK(refused(one_fact(batches_fact, R"(,"carried_tier":"batch")"), tier));
  for (const auto value : {R"("")", R"("a/b")", R"("a+b")", R"("a b")", "7", "null"})
    CHECK(refused(one_fact(batches_fact, std::string(R"(,"billing":{"submit":"carried","get":"none"},"carried_tier":)") + value), tier));
  // An empty tier is refused by the text check every policy string passes: a tier is 1 to 64 bytes.
  const auto empty_tier = descriptor::load_policy(one_fact(batches_fact, R"(,"billing":{"submit":"carried","get":"none"},"carried_tier":"")"),
                                                  config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::ConfigError>(empty_tier) && std::get<descriptor::ConfigError>(empty_tier).pointer == tier &&
        std::get<descriptor::ConfigError>(empty_tier).expected == "bounded nonempty string");
  CHECK(load(one_fact(batches_fact, R"(,"billing":{"submit":"carried","get":"none"},"carried_tier":"b")"))->carried_tier("batches", anthropic) == "b");
  CHECK(refused(one_fact(batches_fact, R"(,"billing":{"submit":"carried","get":"none"},"carried_tier":")" + std::string(65, 'b') + "\""), tier));
  // A statement that names an operation twice never reaches the loader: the JSON reader refuses a repeated member,
  // so the policy fails to load as a whole.
  const auto repeated = one_fact(batches_fact, R"(,"billing":{"submit":"none","submit":"carried"},"carried_tier":"batch")");
  {
    const auto parsed = json::parse(repeated);
    const auto* error = std::get_if<json::ParseError>(&parsed);
    CHECK(error && error->code == json::ParseCode::DuplicateKey && error->pointer == "/endpoints/0/billing/submit");
    const auto loaded = descriptor::load_policy(repeated, config_defaults::codec_defaults_json);
    CHECK(std::holds_alternative<descriptor::ConfigError>(loaded) && std::get<descriptor::ConfigError>(loaded).pointer.empty() &&
          std::get<descriptor::ConfigError>(loaded).expected == "strict bounded JSON policy");
  }
  // A fact whose endpoint or shape has no operation table cannot load, with or without a statement, so no value is kept
  // for it.
  for (const auto& unknown : {std::string(R"({"endpoint":"zz","shape":"openai","origins":["https://api.openai.com"],"paths":{"check":"/v1/zz"})"),
                              std::string(R"({"endpoint":"moderation","shape":"google","origins":["https://api.openai.com"],"paths":{"check":"/v1/moderations"})")}) {
    CHECK(refused(one_fact(unknown), "/endpoints/shape"));
    CHECK(refused(one_fact(unknown, R"(,"billing":"none")"), "/endpoints/shape"));
    CHECK(refused(one_fact(unknown, R"(,"billing":{"check":"none"})"), "/endpoints/shape"));
  }
}

// What the built-in policy states, per endpoint, shape and operation. Every operation of every fact's `paths` is here,
// and so are the steps of Google's resumable upload, which have no path of their own.
struct BuiltinBilling { std::string_view endpoint, shape, operation; Billing billing; };
constexpr BuiltinBilling builtin_billing[]{
    {"images", "openai", "generate", Billing::Model}, {"images", "openai", "edit", Billing::Model},
    {"images", "openrouter", "generate", Billing::Model},
    {"decisions", "openai", "decide", Billing::Model}, {"decisions", "openrouter", "decide", Billing::Model},
    {"embeddings", "openai", "embed", Billing::Model}, {"embeddings", "openrouter", "embed", Billing::Model},
    {"embeddings", "google", "embed", Billing::Model},
    {"rerank", "openrouter", "rank", Billing::Model},
    {"moderation", "openai", "check", Billing::None},
    {"speech", "openai", "speak", Billing::Model}, {"speech", "openrouter", "speak", Billing::Model},
    {"transcription", "openai", "transcribe", Billing::Model}, {"transcription", "openai", "translate", Billing::Model},
    {"transcription", "openrouter", "transcribe", Billing::Model},
    {"videos", "openrouter", "submit", Billing::Model}, {"videos", "openrouter", "poll", Billing::None},
    {"videos", "openrouter", "download", Billing::None},
    {"files", "openai", "upload", Billing::None}, {"files", "openai", "get", Billing::None}, {"files", "openai", "list", Billing::None},
    {"files", "openai", "delete", Billing::None}, {"files", "openai", "content", Billing::None}, {"files", "openai", "start", Billing::None},
    {"files", "openai", "part", Billing::None}, {"files", "openai", "complete", Billing::None}, {"files", "openai", "cancel", Billing::None},
    {"files", "anthropic", "upload", Billing::None}, {"files", "anthropic", "get", Billing::None}, {"files", "anthropic", "list", Billing::None},
    {"files", "anthropic", "delete", Billing::None}, {"files", "anthropic", "content", Billing::None},
    {"files", "google", "upload", Billing::None}, {"files", "google", "get", Billing::None}, {"files", "google", "list", Billing::None},
    {"files", "google", "delete", Billing::None}, {"files", "google", "content", Billing::None}, {"files", "google", "start", Billing::None},
    {"files", "google", "part", Billing::None}, {"files", "google", "query", Billing::None}, {"files", "google", "cancel", Billing::None},
    {"batches", "openai", "submit", Billing::Carried}, {"batches", "openai", "get", Billing::None}, {"batches", "openai", "cancel", Billing::None},
    {"batches", "anthropic", "submit", Billing::Carried}, {"batches", "anthropic", "get", Billing::None},
    {"batches", "anthropic", "cancel", Billing::None}, {"batches", "anthropic", "delete", Billing::None},
    {"batches", "anthropic", "results", Billing::None},
    {"batches", "google", "submit", Billing::Carried}, {"batches", "google", "get", Billing::None}, {"batches", "google", "cancel", Billing::None},
    {"batches", "google", "delete", Billing::None},
    {"tokens", "openai", "count", Billing::None}, {"tokens", "anthropic", "count", Billing::None}, {"tokens", "google", "count", Billing::None},
    {"generations", "openrouter", "get", Billing::None},
    {"realtime", "google", "connect", Billing::Model}, {"realtime", "openai", "connect", Billing::Model}};

void endpoint_billing_builtin() {
  const auto policy = descriptor::builtin_policy();
  const auto expected = [&](const descriptor::EndpointFact& fact, std::string_view operation) -> const BuiltinBilling* {
    for (const auto& row : builtin_billing)
      if (row.endpoint == fact.endpoint && row.shape == fact.shape && row.operation == operation) return &row;
    return nullptr;
  };
  // Every operation the policy has a path for is in the table, with the value the policy states.
  std::size_t paths = 0;
  for (const auto& fact : policy->endpoints())
    for (const auto& [operation, path] : fact.paths) {
      const auto* row = expected(fact, operation);
      if (!row) throw std::runtime_error("no expected billing for " + fact.endpoint + " " + fact.shape + " " + operation);
      for (const auto& origin : fact.origins)
        if (policy->endpoint_billing(fact.endpoint, origin, operation) != row->billing)
          throw std::runtime_error("billing of " + fact.endpoint + " " + fact.shape + " " + operation + " at " + origin + " is " +
                                   std::string(to_string(policy->endpoint_billing(fact.endpoint, origin, operation))));
      ++paths;
    }
  // Every row names a fact of the policy, and the rows without a path (Google's upload steps) read as the table says.
  for (const auto& row : builtin_billing) {
    const auto fact = std::find_if(policy->endpoints().begin(), policy->endpoints().end(),
                                   [&](const auto& f) { return f.endpoint == row.endpoint && f.shape == row.shape; });
    CHECK(fact != policy->endpoints().end());
    CHECK(policy->endpoint_billing(row.endpoint, fact->origins.front(), row.operation) == row.billing);
  }
  CHECK(paths + 3 == std::size(builtin_billing));
  // Carried requests are priced under the batch tier; nothing else carries any.
  for (const auto& fact : policy->endpoints())
    for (const auto& origin : fact.origins)
      CHECK(policy->carried_tier(fact.endpoint, origin) == (fact.endpoint == "batches" ? "batch" : ""));
  // A policy written before the members states nothing about any operation.
  const auto older = load(policy_with_endpoints(facts_without_billing()));
  CHECK(older->endpoints().size() == policy->endpoints().size());
  for (const auto& row : builtin_billing)
    for (const auto& fact : older->endpoints())
      if (fact.endpoint == row.endpoint && fact.shape == row.shape)
        CHECK(older->endpoint_billing(row.endpoint, fact.origins.front(), row.operation) == Billing::Unstated && older->carried_tier(row.endpoint, fact.origins.front()).empty());
}

// The facts of one prepared endpoint call, checked against what it should report.
template <class Outcome>
void reports(const runtime::PreparedEndpoint<Outcome>& call, std::string_view endpoint, std::string_view operation, Billing billing,
             std::string_view model = {}) {
  const auto& prepared = call.prepared();
  const auto label = std::string(endpoint) + "/" + std::string(operation);
  if (!prepared.valid() || prepared.error())
    throw std::runtime_error(label + " did not prepare: " + (prepared.error() ? prepared.error()->safe_message + " / " + prepared.error()->reason : std::string("invalid")));
  const auto& facts = prepared.endpoint_facts();
  const auto* d = prepared.descriptor();
  if (facts.endpoint != endpoint || facts.operation != operation || facts.billing != billing || facts.model != model ||
      facts.family != d->family() || facts.origin != d->base_url() || facts.routed_gateway != descriptor::routed_gateway(*d) ||
      facts.carried_tier != (billing == Billing::Carried ? "batch" : "") || !facts.quantities.empty() || !facts.carried.empty())
    throw std::runtime_error(label + " reports " + facts.endpoint + "/" + facts.operation + " " + std::string(to_string(facts.billing)) +
                             " model '" + facts.model + "' at " + facts.origin);
}
std::shared_ptr<const json::Document> json_text(std::string_view value) {
  auto parsed = json::parse(json::quote(value));
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
decisions::Request decision(std::string model) {
  decisions::Request request{std::move(model), json_text("The checkout page charged me twice.")};
  request.questions.push_back({"is_bug", decisions::Kind::Predicate, json_text("Is the customer reporting a defect?")});
  return request;
}
std::shared_ptr<const std::string> bytes(std::size_t size) { return std::make_shared<const std::string>(std::string(size, 'x')); }
transcription::Request transcript(std::string model, bool translate = false) {
  transcription::Request request{std::move(model), Media::audio("audio/wav", std::make_shared<const std::string>("UklGRiQAAABXQVZFZm10IHN5bnRoZXRpYw=="))};
  request.translate = translate;
  return request;
}
batches::Batch batch_at(std::string_view origin, std::string_view shape, std::string id, std::string output_file = {}) {
  batches::Batch batch;
  batch.origin = std::string(origin);
  batch.shape = std::string(shape);
  batch.id = std::move(id);
  batch.output_file = std::move(output_file);
  return batch;
}

void endpoint_facts_of_prepared_calls() {
  auto direct = client_of(descriptor_of("openai.chat"));
  reports(direct.prepare(images::Request{"zz-image", "A flat icon of a fox"}), "images", "generate", Billing::Model, "zz-image");
  auto edit = images::Request{"zz-image", "The same fox, in blue"};
  edit.references.push_back(Media::file(MediaKind::Image, "file-abc"));
  reports(direct.prepare(edit), "images", "edit", Billing::Model, "zz-image");
  reports(direct.prepare(decision("zz-decider")), "decisions", "decide", Billing::Model, "zz-decider");
  reports(direct.prepare(embeddings::Request{"zz-embed", {{"a"}}}), "embeddings", "embed", Billing::Model, "zz-embed");
  reports(direct.prepare(moderation::Request{"zz-moderation", {{"a sentence"}}}), "moderation", "check", Billing::None, "zz-moderation");
  reports(direct.prepare(speech::Request{"zz-speech", "Hello there.", "alloy"}), "speech", "speak", Billing::Model, "zz-speech");
  reports(direct.prepare(transcript("zz-transcribe")), "transcription", "transcribe", Billing::Model, "zz-transcribe");
  reports(direct.prepare(transcript("zz-transcribe", true)), "transcription", "translate", Billing::Model, "zz-transcribe");
  reports(direct.prepare(files::Upload{{bytes(4)}, "note.txt", "text/plain", "user_data"}), "files", "upload", Billing::None);
  reports(direct.prepare(files::Get{"file-1"}), "files", "get", Billing::None);
  reports(direct.prepare(files::List{}), "files", "list", Billing::None);
  reports(direct.prepare(files::Delete{"file-1"}), "files", "delete", Billing::None);
  reports(direct.prepare(files::Download{"file-1"}), "files", "content", Billing::None);
  reports(direct.prepare(files::Start{"note.txt", "text/plain", 4, "user_data"}), "files", "start", Billing::None);
  files::Session uploads{std::string(openai), "openai", "upload_1", {}, 4};
  reports(direct.prepare(files::Part{uploads, {bytes(4)}}), "files", "part", Billing::None);
  auto whole = uploads; whole.sent = 4; whole.parts = {"part_1"};
  reports(direct.prepare(files::Complete{whole}), "files", "complete", Billing::None);
  reports(direct.prepare(files::Cancel{uploads}), "files", "cancel", Billing::None);
  reports(direct.prepare(batches::Submit{{}, "file-in", "/v1/responses"}), "batches", "submit", Billing::Carried);
  reports(direct.prepare(batches::Get{"batch_1"}), "batches", "get", Billing::None);
  reports(direct.prepare(batches::Cancel{"batch_1"}), "batches", "cancel", Billing::None);
  // An OpenAI batch's results are its output file.
  reports(direct.prepare(batches::Results{batch_at(openai, "openai", "batch_1", "file-out")}), "files", "content", Billing::None);
  auto responses = client_of(descriptor_of("openai.responses"));
  reports(responses.prepare(tokens::Count{responses::Request::simple("zz-count", "hi", 16)}), "tokens", "count", Billing::None, "zz-count");

  auto messages_api = client_of(descriptor_of("anthropic.messages"));
  reports(messages_api.prepare(files::Upload{{bytes(4)}, "note.txt", "text/plain"}), "files", "upload", Billing::None);
  reports(messages_api.prepare(files::Get{"file_1"}), "files", "get", Billing::None);
  reports(messages_api.prepare(files::List{}), "files", "list", Billing::None);
  reports(messages_api.prepare(files::Delete{"file_1"}), "files", "delete", Billing::None);
  reports(messages_api.prepare(files::Download{"file_1"}), "files", "content", Billing::None);
  reports(messages_api.prepare(batches::Submit{{{"a-1", messages::Request::simple("zz-messages", "Say: alpha", 16)}}}), "batches", "submit", Billing::Carried);
  reports(messages_api.prepare(batches::Get{"msgbatch_1"}), "batches", "get", Billing::None);
  reports(messages_api.prepare(batches::Cancel{"msgbatch_1"}), "batches", "cancel", Billing::None);
  reports(messages_api.prepare(batches::Delete{"msgbatch_1"}), "batches", "delete", Billing::None);
  // An Anthropic batch serves its results itself.
  reports(messages_api.prepare(batches::Results{batch_at(anthropic, "anthropic", "msgbatch_1")}), "batches", "results", Billing::None);
  reports(messages_api.prepare(tokens::Count{messages::Request::simple("zz-messages", "hi", 16)}), "tokens", "count", Billing::None, "zz-messages");

  auto generate_api = client_of(descriptor_of("google.generate"));
  reports(generate_api.prepare(embeddings::Request{"zz-embed", {{"a"}}}), "embeddings", "embed", Billing::Model, "zz-embed");
  reports(generate_api.prepare(files::Upload{{bytes(4)}, "note.txt", "text/plain"}), "files", "upload", Billing::None);
  reports(generate_api.prepare(files::Get{"files/abc"}), "files", "get", Billing::None);
  reports(generate_api.prepare(files::List{}), "files", "list", Billing::None);
  reports(generate_api.prepare(files::Delete{"files/abc"}), "files", "delete", Billing::None);
  reports(generate_api.prepare(files::Download{"files/abc"}), "files", "content", Billing::None);
  reports(generate_api.prepare(files::Start{"note.txt", "text/plain", 4}), "files", "start", Billing::None);
  // The steps of a resumable upload go to the session's URL; the policy's one value for files covers them.
  files::Session resumable{std::string(google), "google", {}, std::string(google) + "/upload/v1beta/files?upload_id=AB-cd_12&upload_protocol=resumable", 4};
  reports(generate_api.prepare(files::Part{resumable, {bytes(4)}}), "files", "part", Billing::None);
  reports(generate_api.prepare(files::Query{resumable}), "files", "query", Billing::None);
  reports(generate_api.prepare(files::Cancel{resumable}), "files", "cancel", Billing::None);
  reports(generate_api.prepare(batches::Submit{{{"g-1", gemini::Request::simple("zz-gemini", "Say: alpha")}}}), "batches", "submit", Billing::Carried);
  reports(generate_api.prepare(batches::Get{"batches/b1"}), "batches", "get", Billing::None);
  reports(generate_api.prepare(batches::Cancel{"batches/b1"}), "batches", "cancel", Billing::None);
  reports(generate_api.prepare(batches::Delete{"batches/b1"}), "batches", "delete", Billing::None);
  reports(generate_api.prepare(batches::Results{batch_at(google, "google", "batches/b1", "files/out")}), "files", "content", Billing::None);
  reports(generate_api.prepare(tokens::Count{gemini::Request::simple("zz-gemini", "hi", 16)}), "tokens", "count", Billing::None, "zz-gemini");

  auto router = client_of(openrouter_chat());
  reports(router.prepare(images::Request{"vendor/zz-image", "A flat icon of a fox"}), "images", "generate", Billing::Model, "vendor/zz-image");
  reports(router.prepare(decision("vendor/zz-decider")), "decisions", "decide", Billing::Model, "vendor/zz-decider");
  reports(router.prepare(embeddings::Request{"vendor/zz-embed", {{"a"}}}), "embeddings", "embed", Billing::Model, "vendor/zz-embed");
  reports(router.prepare(rerank::Request{"vendor/zz-rank", "fox", {"a fox", "a dog"}}), "rerank", "rank", Billing::Model, "vendor/zz-rank");
  reports(router.prepare(speech::Request{"vendor/zz-speech", "Hello there.", "alloy"}), "speech", "speak", Billing::Model, "vendor/zz-speech");
  reports(router.prepare(transcript("vendor/zz-transcribe")), "transcription", "transcribe", Billing::Model, "vendor/zz-transcribe");
  reports(router.prepare(videos::Request{"vendor/zz-video", "A fox in the snow."}), "videos", "submit", Billing::Model, "vendor/zz-video");
  // A poll and a download reach the job's fact as their own operations; the codec that reads them stays the submit's.
  const jobs::Handle job{"videos", std::string(openrouter), "job-1", "vendor/zz-video"};
  const auto poll = router.prepare(jobs::Poll{job});
  reports(poll, "videos", "poll", Billing::None, "vendor/zz-video");
  CHECK(poll.prepared().endpoint() == "videos");
  reports(router.prepare(jobs::Download{job, 0}), "videos", "download", Billing::None, "vendor/zz-video");
  reports(router.prepare(generations::Get{"gen-1"}), "generations", "get", Billing::None);
  CHECK(router.prepare(generations::Get{"gen-1"}).prepared().endpoint_facts().routed_gateway);
  // A chat request, a call that did not prepare, and an empty handle report nothing.
  CHECK(prepared_ok(direct, chat_request("zz-model-1", 10)).endpoint_facts().endpoint.empty());
  const auto refused_call = direct.prepare(moderation::Request{"", {}});
  CHECK(refused_call.prepared().error() && refused_call.prepared().endpoint_facts().operation.empty() &&
        refused_call.prepared().endpoint_facts().billing == Billing::Unstated);
  CHECK(runtime::PreparedRequest{}.endpoint_facts().billing == Billing::Unstated);
}

template <class Outcome> pricing::WorstCase bound_of(const runtime::PreparedEndpoint<Outcome>& call) {
  return runtime::worst_case_cost(call.prepared(), *priced);
}
bool zero(const pricing::WorstCase& wc) {
  return wc.total && wc.total->nano_usd == 0 && wc.total->evidence == Evidence::Derived && wc.total->rounding == CostRounding::CeilingCatalogueProduct &&
         wc.unbounded.empty();
}

void worst_case_endpoint_calls() {
  // An operation the policy states the provider does not bill is bounded at zero, with no catalogue entry for its model.
  auto direct = client_of(descriptor_of("openai.chat"));
  CHECK(zero(bound_of(direct.prepare(moderation::Request{"zz-not-in-the-catalogue", {{"a sentence"}}}))));
  CHECK(zero(runtime::worst_case_cost(direct.prepare(moderation::Request{"zz-not-in-the-catalogue", {{"a sentence"}}}).prepared())));
  CHECK(zero(bound_of(direct.prepare(files::Get{"file-1"}))));
  auto responses = client_of(descriptor_of("openai.responses"));
  CHECK(zero(bound_of(responses.prepare(tokens::Count{responses::Request::simple("zz-not-in-the-catalogue", "hi", 16)}))));
  auto messages_api = client_of(descriptor_of("anthropic.messages"));
  CHECK(zero(bound_of(messages_api.prepare(tokens::Count{messages::Request::simple("zz-not-in-the-catalogue", "hi", 16)}))));
  CHECK(zero(bound_of(messages_api.prepare(batches::Results{batch_at(anthropic, "anthropic", "msgbatch_1")}))));
  auto generate_api = client_of(descriptor_of("google.generate"));
  CHECK(zero(bound_of(generate_api.prepare(tokens::Count{gemini::Request::simple("zz-not-in-the-catalogue", "hi", 16)}))));
  auto router = client_of(openrouter_chat());
  const jobs::Handle job{"videos", std::string(openrouter), "job-1", "vendor/zz-not-in-the-catalogue"};
  CHECK(zero(bound_of(router.prepare(jobs::Poll{job}))));
  CHECK(zero(bound_of(router.prepare(jobs::Download{job, 0}))));
  CHECK(zero(bound_of(router.prepare(generations::Get{"gen-1"}))));
  // A billed operation keeps no bound: neither one that bills its model nor a batch that bills the requests it carries.
  auto wc = bound_of(direct.prepare(images::Request{"zz-model-1", "A flat icon of a fox"}));
  CHECK(!wc.total && has(wc, pricing::UnboundedBy::Unmodeled, "images"));
  CHECK(has(bound_of(direct.prepare(decision("zz-model-1"))), pricing::UnboundedBy::Unmodeled, "decisions"));
  CHECK(has(bound_of(router.prepare(videos::Request{"vendor/zz-video", "A fox in the snow."})), pricing::UnboundedBy::Unmodeled, "videos"));
  wc = bound_of(messages_api.prepare(batches::Submit{{{"a-1", messages::Request::simple("zz-model-1", "Say: alpha", 16)}}}));
  CHECK(!wc.total && has(wc, pricing::UnboundedBy::Unmodeled, "batches"));
  // A policy that does not state the billing: no total, and the reason names the endpoint and operation.
  const auto older = load(policy_with_endpoints(facts_without_billing()));
  auto unstated = client_of(descriptor_of("openai.chat", older));
  wc = bound_of(unstated.prepare(moderation::Request{"zz-not-in-the-catalogue", {{"a sentence"}}}));
  CHECK(!wc.total && wc.unbounded.size() == 1 &&
        detail_of(wc, pricing::UnboundedBy::Billing, "moderation/check") == "the policy does not state whether the provider bills the operation");
  CHECK(has(bound_of(unstated.prepare(files::Get{"file-1"})), pricing::UnboundedBy::Billing, "files/get"));
  auto unstated_router = client_of(openrouter_chat(older));
  CHECK(has(bound_of(unstated_router.prepare(jobs::Poll{job})), pricing::UnboundedBy::Billing, "videos/poll"));
  CHECK(pricing::to_string(pricing::UnboundedBy::Billing) == "Billing" && pricing::to_string(pricing::UnboundedBy::Quantity) == "Quantity");
}

// ---- usage ---------------------------------------------------------------------------------------------------

std::string show(const Usage& usage) {
  auto count = [](const std::optional<Count>& c) { return c ? std::to_string(c->value) + (c->evidence == Evidence::Derived ? "d" : "r") : std::string("-"); };
  std::string out = std::string(to_string(usage.stage)) + "/" + std::string(to_string(usage.quality)) + " in=" + count(usage.input_total) +
      " out=" + count(usage.output_total) + " total=" + count(usage.total) + " uncached=" + count(usage.input_uncached) +
      " read=" + count(usage.cache_read) + " write=" + count(usage.cache_write) + " reasoning=" + count(usage.reasoning);
  for (const auto& [name, value] : usage.extra) out += " " + name + "=" + count(value);
  for (const auto& conflict : usage.conflicts) out += " !" + conflict.counter + ":" + conflict.detail;
  out += " cost=" + std::string(to_string(usage.provider_cost.source));
  return out;
}
// The usage a whole response carries, read by the codec the way a call reads it.
std::variant<Usage, Error> usage_of_response(const descriptor::ValidatedDescriptor& d, const std::string& usage) {
  messages::Request request;
  request.model = "fixture-model";
  request.account_scope = "fixture-account";
  request.messages.push_back(Message{"", Role::User, {Text{"Hello"}}});
  auto encoded = messages::encode(d, request, false);
  const auto* ok = std::get_if<messages::EncodedRequest>(&encoded);
  if (!ok) throw std::runtime_error("fixture request did not encode");
  Accumulator accumulator;
  messages::Codec codec(d, messages::Mode::Buffered, accumulator, ok->context);
  codec.buffered(R"({"type":"message","id":"msg-1","role":"assistant","model":"fixture-model","content":[{"type":"text","text":"hi"}],"stop_reason":"end_turn","usage":)" + usage + "}", {});
  const auto& outcome = accumulator.outcome();
  if (const auto* completion = outcome ? std::get_if<Completion>(&*outcome) : nullptr) return completion->usage;
  return std::get<Failure>(*outcome).error;
}

void decoded_usage() {
  const auto d = descriptor_of("anthropic.messages");
  const std::string cases[] = {
      R"({"input_tokens":10,"cache_creation_input_tokens":2,"cache_read_input_tokens":5,"output_tokens":7,"server_tool_use":{"web_search_requests":1,"web_fetch_requests":0},"service_tier":"standard","cache_creation":{"ephemeral_5m_input_tokens":2,"ephemeral_1h_input_tokens":0}})",
      R"({"input_tokens":3,"output_tokens":9,"cache_read_input_tokens":null,"cache_creation_input_tokens":null,"output_tokens_details":{"thinking_tokens":4}})",
      R"({"input_tokens":5,"output_tokens":6,"cache_read_input_tokens":0,"cache_creation_input_tokens":0,"iterations":[{"type":"compaction","input_tokens":100,"output_tokens":20,"cache_read_input_tokens":0,"cache_creation_input_tokens":0},{"type":"message","input_tokens":5,"output_tokens":6}]})",
      R"({"input_tokens":1,"output_tokens":1,"cache_read_input_tokens":0,"cache_creation_input_tokens":4,"cache_creation":{"ephemeral_5m_input_tokens":1,"ephemeral_1h_input_tokens":1}})",
      R"({"input_tokens":0,"output_tokens":0})"};
  for (const auto& usage : cases) {
    const auto decoded = messages::decode_usage(d, usage);
    const auto response = usage_of_response(d, usage);
    const auto* a = std::get_if<Usage>(&decoded);
    const auto* b = std::get_if<Usage>(&response);
    CHECK(a && b);
    CHECK(show(*a) == show(*b));
    CHECK(a->stage == UsageStage::Final);
  }
  // The values a coding agent's result carries, read as the codec reads a response's.
  const auto first = std::get<Usage>(messages::decode_usage(d, cases[0]));
  CHECK(first.input_uncached && first.input_uncached->value == 10 && first.cache_read && first.cache_read->value == 5 &&
        first.cache_write && first.cache_write->value == 2);
  CHECK(first.input_total && first.input_total->value == 17 && first.input_total->evidence == Evidence::Derived);
  CHECK(first.output_total && first.output_total->value == 7 && first.total && first.total->value == 24);
  CHECK(first.extra.at("server_tool_use.web_search_requests").value == 1 && first.extra.at("cache_creation.ephemeral_5m_input_tokens").value == 2);
  CHECK(first.quality == UsageQuality::Consistent && first.conflicts.empty());
  const auto details = std::get<Usage>(messages::decode_usage(d, cases[1]));
  CHECK(details.reasoning && details.reasoning->value == 4 && !details.cache_read && !details.input_total);
  const auto contradicted = std::get<Usage>(messages::decode_usage(d, cases[3]));
  CHECK(contradicted.quality == UsageQuality::Inconsistent && !contradicted.conflicts.empty());
  // Anything the codec would refuse in a response is an Error here.
  auto refused = [&](std::string_view text, ErrorKind kind) {
    const auto decoded = messages::decode_usage(d, text);
    const auto* error = std::get_if<Error>(&decoded);
    return error && error->kind == kind;
  };
  CHECK(refused(R"({"input_tokens":1})", ErrorKind::ProtocolCorrupt));
  CHECK(refused(R"({"output_tokens":1})", ErrorKind::ProtocolCorrupt));
  CHECK(refused(R"({"input_tokens":-1,"output_tokens":1})", ErrorKind::ProtocolCorrupt));
  CHECK(refused(R"({"input_tokens":"1","output_tokens":1})", ErrorKind::ProtocolCorrupt));
  CHECK(refused(R"({"input_tokens":18446744073709551615,"output_tokens":1,"cache_read_input_tokens":1,"cache_creation_input_tokens":0})", ErrorKind::ProtocolCorrupt));
  CHECK(refused("[1]", ErrorKind::ProtocolCorrupt) && refused("{", ErrorKind::ProtocolCorrupt) && refused("", ErrorKind::ProtocolCorrupt));
  // The codec's refusals keep their reason codes, and a non-object has the one the codec gives a missing usage object.
  CHECK(std::get<Error>(messages::decode_usage(d, "[1]")).reason == "messages.usage.object_required");
  CHECK(std::get<Error>(messages::decode_usage(d, R"({"input_tokens":1})")).reason == "messages.usage.counters_missing");
  CHECK(refused(R"({"input_tokens":1,"output_tokens":1,"service_tier":7})", ErrorKind::ProtocolCorrupt));
  // Another family's descriptor is not asked.
  const auto chat = descriptor_of("openai.chat");
  const auto other = messages::decode_usage(chat, cases[4]);
  CHECK(std::holds_alternative<Error>(other) && std::get<Error>(other).kind == ErrorKind::Misuse &&
        std::get<Error>(other).reason == "messages.usage.wrong_family");
  // A descriptor that keeps its usage elsewhere is read there.
  auto moved = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"usage-path","family":"anthropic.messages","connection":{"base_url":"https://usage.example.test","paths":{"buffered":"/m","streaming":"/m"}},"bindings":{"model":"model","messages":"messages","stream":"stream","max_output_tokens":"max_tokens","usage":["metrics","tokens"]}})");
  if (const auto* error = std::get_if<descriptor::ConfigError>(&moved)) throw std::runtime_error("descriptor refused: " + error->pointer);
  const auto& relocated = std::get<descriptor::ValidatedDescriptor>(moved);
  CHECK(show(std::get<Usage>(messages::decode_usage(relocated, cases[0]))) == show(first));
}
} // namespace

int main() {
  try {
    enum_names();
    controls_match_the_encoders(); controls_data(); billing_tiers(); routing_and_tools(); synthetic_policy();
    refusal_predicate(); facts_of_prepared_requests();
    worst_case_tokens(); worst_case_unbounded(); worst_case_tools(); worst_case_tiers_and_fallbacks(); worst_case_route_ceiling();
    worst_case_covers_usage(); worst_case_synthetic_policy(); worst_case_shadowed_profile();
    endpoint_billing_forms(); endpoint_billing_refusals(); endpoint_billing_builtin(); endpoint_facts_of_prepared_calls();
    worst_case_endpoint_calls();
    model_facts();
    decoded_usage();
    std::cout << "host query properties passed\n"; return 0;
  }
  catch (const std::exception& error) { std::cerr << "host query test failed: " << error.what() << '\n'; return 1; }
}
