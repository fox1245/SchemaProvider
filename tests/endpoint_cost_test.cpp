// The cost bound of a prepared non-chat endpoint call (runtime::endpoint_worst_case_cost and runtime::worst_case_cost):
// what each billing value of the policy answers; the quantities and dimensions each request type states; and, per
// endpoint kind, the bound of a call prepared through a client, against amounts computed here with integers from a
// small catalogue written here, with every refusal a request can cause.
#include "core/enum_names.h"
#include "crypto/crypto.h"
#include "core/pricing.h"
#include "descriptor/catalogue.h"
#include "descriptor/policy.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include "runtime/client.h"
#include "runtime/cost.h"
#include "sp/config_defaults.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace sp;
using namespace std::chrono_literals;
using pricing::UnboundedBy;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)

constexpr std::string_view openai = "https://api.openai.com";

descriptor::CatalogueSnapshot catalogue_of(const std::string& json) {
  auto loaded = descriptor::load_catalogue(json);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("catalogue refused at " + error->pointer + ": " + error->expected);
  return std::get<descriptor::CatalogueSnapshot>(std::move(loaded));
}
descriptor::PolicySnapshot load(const std::string& family_json) {
  auto loaded = descriptor::load_policy(family_json, config_defaults::codec_defaults_json);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("policy refused at " + error->pointer + ": " + error->expected);
  return std::get<descriptor::PolicySnapshot>(std::move(loaded));
}
// The built-in policy with each endpoint fact written by `edit` (its text; empty leaves it out) and `added` facts after them.
std::string policy_with(const std::function<std::string(json::Value)>& edit, const std::vector<std::string>& added = {}) {
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  const auto root = std::get<json::Document>(parsed).root();
  std::string facts = "[";
  for (auto fact : root.get("endpoints").elements()) {
    const auto text = edit(fact);
    if (text.empty()) continue;
    facts += (facts.size() > 1 ? "," : "") + text;
  }
  for (const auto& fact : added) facts += (facts.size() > 1 ? "," : "") + fact;
  facts += "]";
  std::string out = "{";
  for (auto member : root.members()) {
    if (out.size() > 1) out += ',';
    out += json::quote(member.key) + ":" + (member.key == "endpoints" ? facts : member.value.dump());
  }
  return out + "}";
}
// A fact's text with each member `skip` names left out and `replace` members written in their place.
std::string fact_text(json::Value fact, std::vector<std::string_view> skip, std::vector<std::pair<std::string, std::string>> replace = {}) {
  std::string out = "{";
  for (auto member : fact.members()) {
    if (std::find(skip.begin(), skip.end(), member.key) != skip.end()) continue;
    std::string value = member.value.dump();
    for (const auto& [key, text] : replace) if (key == member.key) value = text;
    out += (out.size() > 1 ? "," : "") + json::quote(member.key) + ":" + value;
  }
  return out + "}";
}
descriptor::ValidatedDescriptor openai_chat(descriptor::PolicySnapshot policy = {}) {
  descriptor::presets::PresetOptions options;
  options.policy = std::move(policy);
  auto loaded = descriptor::presets::try_openai_chat(options);
  if (auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("descriptor refused: " + error->pointer);
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
descriptor::ValidatedDescriptor anthropic_messages() {
  auto loaded = descriptor::presets::try_anthropic_messages({});
  if (auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("descriptor refused: " + error->pointer);
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
descriptor::ValidatedDescriptor openrouter_chat() {
  auto loaded = descriptor::presets::try_openrouter_chat({});
  if (auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("descriptor refused: " + error->pointer);
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
runtime::Client client_of(descriptor::ValidatedDescriptor d) { return runtime::Client(std::move(d), "not-a-key"); }
// Retry on with `attempts` attempts, or off with one.
runtime::RunOptions run_with(std::uint32_t attempts) {
  runtime::RunOptions run;
  run.retry = runtime::RetryPolicy{attempts > 1, false, attempts, 0ms, 0ms};
  return run;
}
template <class Outcome> const runtime::PreparedRequest& ok(const runtime::PreparedEndpoint<Outcome>& call) {
  const auto& prepared = call.prepared();
  if (!prepared.valid() || prepared.error())
    throw std::runtime_error("call did not prepare: " + (prepared.error() ? prepared.error()->safe_message + " / " + prepared.error()->reason : std::string("invalid")));
  return prepared;
}
// Every member of a bound, so that a test states the whole answer.
std::string describe(const pricing::WorstCase& wc) {
  std::string out = wc.total ? "total " + std::to_string(wc.total->nano_usd) + " " + std::string(to_string(wc.total->evidence)) + " " +
                                   std::string(to_string(wc.total->rounding))
                             : std::string("no total");
  for (const auto& reason : wc.unbounded) out += " | " + std::string(pricing::to_string(reason.by)) + " [" + reason.subject + "] " + reason.detail;
  out += " | entry '" + wc.entry + "' model '" + wc.model + "' tokens " + std::to_string(wc.input_tokens) + "/" + std::to_string(wc.output_tokens) + "/" +
         std::to_string(wc.invocation_input_tokens) + " fees " + std::to_string(wc.tool_fees_nano_usd) + " invocations " + std::to_string(wc.invocations) +
         " attempts " + std::to_string(wc.attempts) + " tools " + std::to_string(wc.tool_uses.size()) + " ceiling " + std::to_string(wc.route_ceiling) +
         " pinned " + std::to_string(wc.pinned) + " pin " + std::to_string(wc.pin.has_value());
  return out;
}
std::shared_ptr<const json::Document> document(std::string_view text) {
  auto parsed = json::parse(text);
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
decisions::Request decision(std::string model) {
  decisions::Request request{std::move(model), document(R"("The checkout page charged me twice.")")};
  request.questions.push_back({"is_bug", decisions::Kind::Predicate, document(R"("Is the customer reporting a defect?")")});
  return request;
}
transcription::Request transcript(std::string model, bool translate = false) {
  transcription::Request request{std::move(model), Media::audio("audio/wav", std::make_shared<const std::string>("UklGRiQAAABXQVZFZm10IHN5bnRoZXRpYw=="))};
  request.translate = translate;
  return request;
}

// A rate in nano-USD per million units (USD x 1e15) as the decimal USD text a catalogue holds.
std::string usd(std::uint64_t rate) {
  constexpr std::uint64_t scale = 1'000'000'000'000'000u;
  std::string fraction = std::to_string(rate % scale);
  fraction.insert(0, 15 - fraction.size(), '0');
  return std::to_string(rate / scale) + "." + fraction;
}
// One attempt's amount from the exact sum of rates times quantities: rounded up once to whole nano-USD.
std::uint64_t rounded(std::uint64_t sum) { return sum / 1'000'000 + (sum % 1'000'000 != 0 ? 1 : 0); }

// Rates in nano-USD per million units. Tokens: input, the image tokens of an input, output.
constexpr std::uint64_t in_rate = 1'000'000'000, image_token_rate = 2'000'000'000, out_rate = 4'000'000'000, small_out_rate = 2'000'000'000;
// Images: each image made, by qualifier; each reference taken.
constexpr std::uint64_t image_rate = 40'000'000'000'000, high_rate = 80'000'000'000'000, edit_rate = 100'000'000'000'000,
                        moderated_rate = 60'000'000'000'000, reference_rate = 10'000'000'000'000;
constexpr std::uint64_t embed_rate = 100'000'000, document_rate = 1'000'000'000'000, search_unit_rate = 2'000'000'000'000;
// Speech: a fraction of a nano-USD per byte, so the bound rounds up.
constexpr std::uint64_t character_rate = 15'000'500'000, paced_rate = 30'001'000'000;
constexpr std::uint64_t second_rate = 100'000'000'000, translate_rate = 200'000'000'000, video_second_rate = 50'000'000'000'000,
                        video_1080p_rate = 100'000'000'000'000;
// The model limits of the entries.
constexpr std::uint64_t image_input = 1000, image_output = 4000, decider_input = 2000, decider_output = 100, embed_input = 512,
                        long_input = 4096, long_output = 2000;

// One catalogue entry: its id, its other members, and its prices as unit and rate (nano-USD per million units).
std::string entry(std::string_view id, std::string_view members, const std::vector<std::pair<std::string, std::uint64_t>>& prices) {
  std::string out = "{\"id\":" + json::quote(id);
  if (!members.empty()) out += "," + std::string(members);
  out += ",\"pricing\":{";
  for (std::size_t i = 0; i < prices.size(); ++i) out += (i ? "," : "") + json::quote(prices[i].first) + ":" + json::quote(usd(prices[i].second));
  return out + "}}";
}
std::string limits(std::uint64_t input, std::optional<std::uint64_t> output = std::nullopt) {
  return "\"input_limit\":" + std::to_string(input) + (output ? ",\"output_limit\":" + std::to_string(*output) : std::string());
}
const auto priced = catalogue_of("{\"version\":1,\"models\":[" +
    entry("vendor/image", limits(image_input, image_output),
          {{"input", in_rate}, {"input_image", image_token_rate}, {"output", out_rate}, {"item:image", image_rate}, {"item:image[quality=high]", high_rate},
           {"item:image[operation=edit]", edit_rate}, {"item:image[moderation=low]", moderated_rate}, {"item:input_image", reference_rate}}) + "," +
    // No rate for the image tokens of an input.
    entry("vendor/image-plain", limits(image_input, image_output),
          {{"input", in_rate}, {"output", out_rate}, {"item:image", image_rate}, {"item:input_image", reference_rate}}) + "," +
    // Each image priced only by a row keyed on an extension member.
    entry("vendor/image-moderated", limits(image_input, image_output),
          {{"input", in_rate}, {"input_image", image_token_rate}, {"output", out_rate}, {"item:image[moderation=low]", moderated_rate}, {"item:input_image", 0}}) + "," +
    // Each image priced by the operation, and dearer for one value of an extension member.
    entry("vendor/image-styled", limits(image_input, image_output),
          {{"input", in_rate}, {"input_image", image_token_rate}, {"output", out_rate}, {"item:image[operation=generate]", image_rate},
           {"item:image[operation=generate,style=1.5]", high_rate}, {"item:input_image", 0}}) + "," +
    // Priced by tokens alone: each image and each reference a declared zero.
    entry("vendor/image-tokens", limits(image_input, image_output),
          {{"input", in_rate}, {"input_image", image_token_rate}, {"output", out_rate}, {"item:image", 0}, {"item:input_image", 0}}) + "," +
    entry("vendor/decider", limits(decider_input, decider_output), {{"input", in_rate}, {"output", small_out_rate}}) + "," +
    entry("vendor/decider-open", limits(decider_input), {{"input", in_rate}, {"output", small_out_rate}}) + "," +
    entry("vendor/embed", limits(embed_input), {{"input", embed_rate}}) + "," +
    entry("vendor/ranker", limits(long_input), {{"input", in_rate}, {"item:document", document_rate}, {"item:search_unit", 0}}) + "," +
    entry("vendor/ranker-units", limits(long_input), {{"input", in_rate}, {"item:document", document_rate}, {"item:search_unit", search_unit_rate}}) + "," +
    entry("vendor/speaker", limits(long_input, long_output), {{"input", in_rate}, {"output", small_out_rate}, {"item:character", character_rate}}) + "," +
    // Dearer characters at one speed; and seconds at one speed, which a speech call does not count.
    entry("vendor/speaker-paced", limits(long_input, long_output),
          {{"input", in_rate}, {"output", small_out_rate}, {"item:character", character_rate}, {"item:character[operation=speak,speed=1.5]", paced_rate}}) + "," +
    entry("vendor/speaker-timed", limits(long_input, long_output),
          {{"input", in_rate}, {"output", small_out_rate}, {"item:character", character_rate}, {"item:second[speed=1.5]", second_rate}}) + "," +
    entry("vendor/transcriber", limits(long_input, long_output),
          {{"input", in_rate}, {"output", small_out_rate}, {"item:second", second_rate}, {"item:second[operation=translate]", translate_rate}}) + "," +
    entry("vendor/video", limits(long_input, long_output),
          {{"input", in_rate}, {"input_image", image_token_rate}, {"output", small_out_rate}, {"item:second", video_second_rate},
           {"item:second[resolution=1080p]", video_1080p_rate}, {"item:input_image", reference_rate}}) + "]}");

// The built-in policy with the rerank and video facts admitted at OpenAI's origin too, which is not a routing gateway,
// and the video fact taking `audio` and `seed` as extensions there; OpenAI's image fact taking a `style` that may be a
// number, text or an object, and its speech fact a `format` and an `operation`.
const auto direct_policy = load(policy_with([](json::Value fact) {
  const auto name = fact.get("endpoint").as_string();
  if (fact.get("shape").as_string() != "openai") return fact.dump();
  if (name == "images")
    return fact_text(fact, {}, {{"options", R"({"type":"object","properties":{"moderation":{"type":"string","max_length":32,"token":true},)"
                                            R"("user":{"type":"string","max_length":256},"style":{"one_of":[{"type":"number"},)"
                                            R"({"type":"string","max_length":64},{"type":"object","properties":{"name":{"type":"string","max_length":16}}}]}}})"}});
  if (name == "speech") {
    auto text = fact.dump();
    text.insert(text.size() - 1, R"(,"options":{"type":"object","properties":{"format":{"type":"string","max_length":16,"token":true},)"
                                 R"("operation":{"type":"string","max_length":16,"token":true}}})");
    return text;
  }
  return fact.dump();
}, [] {
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  const auto root = std::get<json::Document>(parsed).root();
  std::vector<std::string> added;
  for (auto fact : root.get("endpoints").elements()) {
    const auto name = fact.get("endpoint").as_string();
    if (name == "rerank") added.push_back(fact_text(fact, {}, {{"origins", R"(["https://api.openai.com"])"}}));
    if (name == "videos")
      added.push_back(fact_text(fact, {}, {{"origins", R"(["https://api.openai.com"])"},
                                           {"options", R"({"type":"object","properties":{"audio":{"type":"boolean"},"seed":{"type":"integer","minimum":0}}})"}}));
  }
  return added;
}()));
// The built-in policy with OpenAI's image fact not charged on acceptance, so the runtime retries its calls.
const auto retried_policy = load(policy_with([](json::Value fact) {
  return fact.get("endpoint").as_string() == "images" && fact.get("shape").as_string() == "openai" ? fact_text(fact, {"charge_on_accept"}) : fact.dump();
}));
// The built-in policy with moderation billing its model, which the SDK states no quantities for.
const auto billed_moderation = load(policy_with([](json::Value fact) {
  return fact.get("endpoint").as_string() == "moderation" ? fact_text(fact, {}, {{"billing", R"("model")"}}) : fact.dump();
}));

bool has(const std::vector<pricing::UnboundedReason>& reasons, UnboundedBy by, std::string_view subject) {
  return std::any_of(reasons.begin(), reasons.end(), [&](const auto& reason) { return reason.by == by && reason.subject == subject; });
}
std::string reasons_of(const pricing::EndpointBound& bound) {
  std::string out;
  for (const auto& reason : bound.unbounded) out += std::string(pricing::to_string(reason.by)) + " [" + reason.subject + "] " + reason.detail + "; ";
  return out;
}
// The total of a bound that must have one.
std::uint64_t nano(const pricing::EndpointBound& bound) {
  if (!bound.total) throw std::runtime_error("no bound: " + reasons_of(bound));
  return bound.total->nano_usd;
}
// The line of `unit`, as `<row> <quantity> x <rate>`.
std::string line_of(const pricing::EndpointBound& bound, std::string_view unit) {
  for (const auto& line : bound.lines)
    if (line.unit == unit) return line.row + " " + std::to_string(line.quantity) + " x " + std::to_string(line.rate);
  return "(none)";
}
template <class Outcome> pricing::EndpointBound bound_of(const runtime::PreparedEndpoint<Outcome>& call, runtime::EndpointCostOptions options = {}) {
  return runtime::endpoint_worst_case_cost(ok(call), *priced, options);
}
runtime::EndpointCostOptions measured(std::uint64_t tokens) {
  runtime::EndpointCostOptions options;
  options.measured_input_tokens = tokens;
  return options;
}
runtime::EndpointCostOptions seconds(std::uint64_t value) {
  runtime::EndpointCostOptions options;
  options.audio_seconds = value;
  return options;
}
images::Request image(std::string model, std::optional<std::uint32_t> count) {
  images::Request request{std::move(model), "A flat icon of a fox"};
  request.count = count;
  return request;
}
// The quantities a call states, as `<unit>[=<at_most>][<source>][x<per>][!]` (`!`: exact), and its dimensions.
std::string quantities_of(const EndpointFacts& facts) {
  std::string out;
  for (const auto& q : facts.quantities) {
    if (!out.empty()) out += ' ';
    out += q.unit;
    if (q.at_most) out += "=" + std::to_string(*q.at_most);
    switch (q.source) {
      case EndpointQuantity::Source::Request: break;
      case EndpointQuantity::Source::InputLimit: out += "<input"; break;
      case EndpointQuantity::Source::OutputLimit: out += "<output"; break;
      case EndpointQuantity::Source::StatedSeconds: out += "<seconds"; break;
    }
    if (q.per != 1) out += "x" + std::to_string(q.per);
    if (q.exact) out += "!";
  }
  return out;
}
std::string dimensions_of(const EndpointFacts& facts) {
  std::string out;
  for (const auto& [key, value] : facts.dimensions) out += (out.empty() ? "" : ",") + key + "=" + value;
  return out;
}

// ---- what worst_case_cost answers, per billing value ------------------------------------------------------------

void answers_per_billing() {
  auto direct = client_of(openai_chat());
  auto router = client_of(openrouter_chat());
  // A billed operation is bounded by the quantities its request states, projected onto a chat request's bound: the
  // token lines, one invocation, the attempts.
  const auto one_image = rounded(image_rate + image_input * image_token_rate + image_output * out_rate);
  CHECK(describe(runtime::worst_case_cost(ok(direct.prepare(image("vendor/image", 1), run_with(3))), *priced)) ==
        "total " + std::to_string(one_image) + " Derived CeilingCatalogueProduct | entry 'vendor/image' model 'vendor/image' tokens 1000/4000/1000 "
        "fees 0 invocations 1 attempts 1 tools 0 ceiling 0 pinned 0 pin 0");
  CHECK(describe(runtime::worst_case_cost(ok(direct.prepare(image("vendor/image", 1))), *priced, runtime::WorstCaseOptions{300})) ==
        "total " + std::to_string(rounded(image_rate + 300 * image_token_rate + image_output * out_rate)) +
        " Derived CeilingCatalogueProduct | entry 'vendor/image' model 'vendor/image' tokens 300/4000/300 fees 0 invocations 1 attempts 1 tools 0 "
        "ceiling 0 pinned 0 pin 0");
  // Its reasons, when it has no bound: here the audio's length, which only endpoint_worst_case_cost takes.
  CHECK(describe(runtime::worst_case_cost(ok(direct.prepare(transcript("vendor/transcriber"))), *priced)) ==
        "no total | Quantity [item:second] the audio's length is not stated (EndpointCostOptions::audio_seconds) | entry '' model '' tokens 0/0/0 "
        "fees 0 invocations 0 attempts 0 tools 0 ceiling 0 pinned 0 pin 0");
  // At a routing gateway every billed endpoint call keeps no bound.
  CHECK(has(runtime::worst_case_cost(ok(router.prepare(rerank::Request{"vendor/ranker", "fox", {"a fox", "a dog"}})), *priced).unbounded,
            UnboundedBy::RouteCeiling, "vendor/ranker"));

  // Not billed: exactly zero, with the attempts the retry policy allows, and no entry for the model.
  CHECK(describe(runtime::worst_case_cost(ok(direct.prepare(moderation::Request{"vendor/moderator", {{"a sentence"}}}, run_with(3))), *priced)) ==
        "total 0 Derived CeilingCatalogueProduct | entry '' model 'vendor/moderator' tokens 0/0/0 fees 0 invocations 0 attempts 3 tools 0 ceiling 0 pinned 0 pin 0");
  const jobs::Handle job{"videos", "https://openrouter.ai", "job-1", "vendor/video"};
  CHECK(describe(runtime::worst_case_cost(ok(router.prepare(jobs::Poll{job}, run_with(2))), *priced)) ==
        "total 0 Derived CeilingCatalogueProduct | entry '' model 'vendor/video' tokens 0/0/0 fees 0 invocations 0 attempts 2 tools 0 ceiling 0 pinned 0 pin 0");
  // Not stated: a policy whose facts state no billing.
  const auto unstated = load(policy_with([](json::Value fact) { return fact_text(fact, {"billing", "carried_tier"}); }));
  auto older = client_of(openai_chat(unstated));
  CHECK(describe(runtime::worst_case_cost(ok(older.prepare(moderation::Request{"vendor/moderator", {{"a sentence"}}})), *priced)) ==
        "no total | Billing [moderation/check] the policy does not state whether the provider bills the operation | entry '' model '' tokens 0/0/0 fees 0 "
        "invocations 0 attempts 0 tools 0 ceiling 0 pinned 0 pin 0");
  CHECK(describe(runtime::worst_case_cost(ok(older.prepare(images::Request{"vendor/image", "A flat icon of a fox"})), *priced)) ==
        "no total | Billing [images/generate] the policy does not state whether the provider bills the operation | entry '' model '' tokens 0/0/0 fees 0 "
        "invocations 0 attempts 0 tools 0 ceiling 0 pinned 0 pin 0");
  // Carried: a batch submit bills the requests it carries, each priced under the policy's carried tier, which this
  // catalogue prices nothing under (batches_bound has the totals).
  auto messages_api = client_of(anthropic_messages());
  CHECK(describe(runtime::worst_case_cost(ok(messages_api.prepare(batches::Submit{{{"a-1", messages::Request::simple("vendor/decider", "Say: alpha", 16)}}})), *priced)) ==
        "no total | TierPrice [a-1:vendor/decider] the entry prices no unit under service tier 'batch' | entry '' model '' tokens 0/0/0 fees 0 "
        "invocations 0 attempts 0 tools 0 ceiling 0 pinned 0 pin 0");

  // The bound with its lines answers the same, and nothing for a chat request or a call that did not prepare.
  CHECK(nano(bound_of(direct.prepare(moderation::Request{"vendor/moderator", {{"a sentence"}}}))) == 0);
  CHECK(has(bound_of(older.prepare(moderation::Request{"vendor/moderator", {{"a sentence"}}})).unbounded, UnboundedBy::Billing, "moderation/check"));
  chat::Request chat;
  chat.model = "vendor/decider";
  chat.messages.push_back({Role::User, "hello"});
  chat.max_output_tokens = 10;
  const auto chat_call = direct.prepare(chat);
  CHECK(!chat_call.error() && has(runtime::endpoint_worst_case_cost(chat_call, *priced).unbounded, UnboundedBy::Unmodeled, "request"));
  CHECK(has(runtime::endpoint_worst_case_cost(runtime::PreparedRequest{}).unbounded, UnboundedBy::Unmodeled, "request"));
  CHECK(has(runtime::endpoint_worst_case_cost(direct.prepare(moderation::Request{"", {}}).prepared(), *priced).unbounded, UnboundedBy::Unmodeled, "request"));
  // An operation that bills its model and states no quantities has no bound, rather than its request fee alone.
  auto billing_moderation = client_of(openai_chat(billed_moderation));
  const auto moderated = billing_moderation.prepare(moderation::Request{"vendor/moderator", {{"a sentence"}}});
  CHECK(ok(moderated).endpoint_facts().billing == descriptor::EndpointBilling::Model && ok(moderated).endpoint_facts().quantities.empty());
  CHECK(reasons_of(bound_of(moderated)) == "Unmodeled [moderation/check] the call states no quantities for the operation; ");
  CHECK(has(runtime::worst_case_cost(ok(moderated), *priced).unbounded, UnboundedBy::Unmodeled, "moderation/check"));
  // The capability: granted by the runtime, required only by a host that bounds endpoint calls.
  static_assert((capability::RequiredProvider & capability::EndpointCostBounds) == 0);
  CHECK((runtime::interface_contract().capabilities & capability::EndpointCostBounds) != 0);
  runtime::require_interface_contract(EXPECTED_INTERFACE_REVISION, capability::EndpointCostBounds);
}

// ---- the quantities and dimensions of each request type -----------------------------------------------------------

void stated_facts() {
  auto direct = client_of(openai_chat(direct_policy));
  auto router = client_of(openrouter_chat());
  // Images: the count asked for, the references, the tokens; the typed tokens that are set, sorted by key.
  auto request = image("vendor/image", 2);
  request.size = "1024x1024";
  request.quality = "high";
  request.background = "transparent";
  request.output_compression = 80;
  auto facts = ok(direct.prepare(request)).endpoint_facts();
  CHECK(facts.endpoint == "images" && facts.operation == "generate" && facts.model == "vendor/image" && !facts.routed_gateway);
  CHECK(quantities_of(facts) == "item:image=2 item:input_image=0! input<input output<output");
  CHECK(dimensions_of(facts) == "background=transparent,operation=generate,output_compression=80,quality=high,size=1024x1024");
  // With references, an edit at OpenAI, whose image tokens are among its input tokens; an unset count is not stated.
  auto edit = image("vendor/image", std::nullopt);
  edit.references = {Media::file(MediaKind::Image, "file-abc"), Media::file(MediaKind::Image, "file-def")};
  facts = ok(direct.prepare(edit)).endpoint_facts();
  CHECK(facts.operation == "edit" && quantities_of(facts) == "item:image item:input_image=2! input<input input_image<input output<output");
  CHECK(dimensions_of(facts) == "operation=edit");
  // Extensions under their own names: a string, a boolean or an unsigned integer in the qualifier grammar with its value,
  // and an object, or text outside the grammar, without one.
  auto routed = image("vendor/image", 1);
  routed.extensions = document(R"({"provider":{"sort":"price"},"aspect_ratio":"16:9","seed":7,"user":"a person"})");
  facts = ok(router.prepare(routed)).endpoint_facts();
  CHECK(facts.routed_gateway && dimensions_of(facts) == "aspect_ratio=16:9,operation=generate,provider=,seed=7,user=");
  auto moderated = image("vendor/image", 1);
  moderated.extensions = document(R"({"moderation":"low"})");
  CHECK(dimensions_of(ok(direct.prepare(moderated)).endpoint_facts()) == "moderation=low,operation=generate");
  // Decisions: tokens up to the limits.
  facts = ok(direct.prepare(decision("vendor/decider"))).endpoint_facts();
  CHECK(quantities_of(facts) == "input<input output<output" && dimensions_of(facts) == "operation=decide");
  // Embeddings: the input limit counts per input.
  embeddings::Request embed{"vendor/embed", {{"a"}, {"b"}, {"c"}}};
  embed.dimensions = 256;
  facts = ok(direct.prepare(embed)).endpoint_facts();
  CHECK(quantities_of(facts) == "input<inputx3" && dimensions_of(facts) == "dimensions=256,operation=embed");
  // Rerank: the documents, and search units nothing bounds.
  rerank::Request rank{"vendor/ranker", "fox", {"a fox", "a dog"}};
  rank.top = 1;
  facts = ok(direct.prepare(rank)).endpoint_facts();
  CHECK(quantities_of(facts) == "item:document=2! item:search_unit input<inputx2" && dimensions_of(facts) == "operation=rank,top=1");
  // Speech: the input's bytes; a voice outside the grammar without its value.
  speech::Request speak{"vendor/speaker", "Hello there.", "alloy"};
  speak.format = "mp3";
  facts = ok(direct.prepare(speak)).endpoint_facts();
  CHECK(quantities_of(facts) == "item:character=12! input<input output<output" && dimensions_of(facts) == "format=mp3,operation=speak,voice=alloy");
  CHECK(dimensions_of(ok(direct.prepare(speech::Request{"vendor/speaker", "Hello there.", "Alloy Two"})).endpoint_facts()) == "operation=speak,voice=");
  // Transcription: the seconds the host states.
  auto heard = transcript("vendor/transcriber");
  heard.language = "en";
  facts = ok(direct.prepare(heard)).endpoint_facts();
  CHECK(facts.operation == "transcribe" && quantities_of(facts) == "item:second<seconds input<input output<output" && dimensions_of(facts) == "language=en,operation=transcribe");
  CHECK(ok(direct.prepare(transcript("vendor/transcriber", true))).endpoint_facts().operation == "translate");
  // Videos: the duration, the references, the tokens.
  videos::Request video{"vendor/video", "A fox in the snow."};
  video.duration = 5;
  video.resolution = "720p";
  video.aspect_ratio = "16:9";
  video.audio = true;
  facts = ok(direct.prepare(video)).endpoint_facts();
  CHECK(quantities_of(facts) == "item:second=5 item:input_image=0! input<input output<output" &&
        dimensions_of(facts) == "aspect_ratio=16:9,audio=true,operation=submit,resolution=720p");
  // A call that bills nothing states nothing.
  facts = ok(direct.prepare(moderation::Request{"vendor/moderator", {{"a sentence"}}})).endpoint_facts();
  CHECK(facts.quantities.empty() && facts.dimensions.empty());
  CHECK(ok(direct.prepare(files::Get{"file-1"})).endpoint_facts().quantities.empty());
}

// ---- the bound of each endpoint kind ---------------------------------------------------------------------------------

void images_bound() {
  auto direct = client_of(openai_chat());
  const std::uint64_t tokens = image_input * image_token_rate + image_output * out_rate;
  // Two images at the dearest row their dimensions match (a high quality), the input at its dearest rate (image tokens)
  // up to the input limit, the output up to the output limit. An edit row does not price a generation.
  auto request = image("vendor/image", 2);
  request.quality = "high";
  request.size = "1024x1024";
  auto bound = bound_of(direct.prepare(request));
  CHECK(nano(bound) == rounded(2 * high_rate + tokens) && bound.attempts == 1 && bound.entry == "vendor/image" && bound.model == "vendor/image");
  CHECK(line_of(bound, "item:image") == "item:image[quality=high] 2 x " + std::to_string(high_rate));
  CHECK(line_of(bound, "input") == "input_image 1000 x " + std::to_string(image_token_rate) && line_of(bound, "output") == "output 4000 x " + std::to_string(out_rate));
  // An unset quality matches only the row without a qualifier.
  CHECK(nano(bound_of(direct.prepare(image("vendor/image", 1)))) == rounded(image_rate + tokens));
  // An extension is a dimension too.
  auto moderated = image("vendor/image", 1);
  moderated.extensions = document(R"({"moderation":"low"})");
  CHECK(nano(bound_of(direct.prepare(moderated))) == rounded(moderated_rate + tokens));
  // With references: an edit, at the edit row, and each reference.
  auto edit = image("vendor/image", 1);
  edit.references = {Media::file(MediaKind::Image, "file-abc"), Media::file(MediaKind::Image, "file-def")};
  bound = bound_of(direct.prepare(edit));
  CHECK(nano(bound) == rounded(edit_rate + 2 * reference_rate + tokens));
  CHECK(line_of(bound, "item:image") == "item:image[operation=edit] 1 x " + std::to_string(edit_rate));
  CHECK(line_of(bound, "item:input_image") == "item:input_image 2 x " + std::to_string(reference_rate));
  // The prepared facts' dimensions are complete: the item price queries over them give the rate the bound priced at.
  {
    const auto prepared_edit = direct.prepare(edit);
    const auto& facts = ok(prepared_edit).endpoint_facts();
    const auto* found = priced->find(facts.family, facts.origin, facts.model).entry;
    CHECK(found && found->id == "vendor/image");
    const auto dearest = pricing::dearest_item_price(*found, "item:image", facts.dimensions, "", image_input);
    CHECK(dearest && dearest->first == edit_rate && dearest->second == "item:image[operation=edit]");
    CHECK(line_of(bound, "item:image") == dearest->second + " 1 x " + std::to_string(dearest->first));
    const auto specific = pricing::item_price(*found, "item:image", facts.dimensions, "", image_input);
    CHECK(specific && specific->first == edit_rate && specific->second == "item:image[operation=edit]");
  }
  // The host's count of the input, capped at the limit.
  CHECK(nano(bound_of(direct.prepare(image("vendor/image", 1)), measured(300))) == rounded(image_rate + 300 * image_token_rate + image_output * out_rate));
  CHECK(nano(bound_of(direct.prepare(image("vendor/image", 1)), measured(50'000))) == rounded(image_rate + tokens));
  // An unset count: the provider's default is not known, so a priced image has no bound; a declared zero needs none.
  bound = bound_of(direct.prepare(image("vendor/image", std::nullopt)));
  CHECK(!bound.total && has(bound.unbounded, UnboundedBy::Quantity, "item:image") && bound.unbounded.size() == 1);
  CHECK(nano(bound_of(direct.prepare(image("vendor/image-tokens", std::nullopt)))) == rounded(tokens));
  auto token_edit = edit;
  token_edit.model = "vendor/image-tokens";
  CHECK(nano(bound_of(direct.prepare(token_edit))) == rounded(tokens));
  // References need a rate for the image tokens of the input; without references none is needed.
  auto plain_edit = edit;
  plain_edit.model = "vendor/image-plain";
  bound = bound_of(direct.prepare(plain_edit));
  CHECK(!bound.total && has(bound.unbounded, UnboundedBy::ModelPrice, "input_image"));
  CHECK(nano(bound_of(direct.prepare(image("vendor/image-plain", 1)))) == rounded(image_rate + image_input * in_rate + image_output * out_rate));
  // A charge-on-accept fact bounds one attempt whatever the retry policy allows; a billed fact that is not charged on
  // acceptance is retried, and bounded at every attempt.
  bound = bound_of(direct.prepare(image("vendor/image", 1), run_with(3)));
  CHECK(nano(bound) == rounded(image_rate + tokens) && bound.attempts == 1);
  auto retried = client_of(openai_chat(retried_policy));
  bound = bound_of(retried.prepare(image("vendor/image", 1), run_with(3)));
  CHECK(nano(bound) == 3 * rounded(image_rate + tokens) && bound.attempts == 3);
  CHECK(nano(bound_of(retried.prepare(image("vendor/image", 1), run_with(1)))) == rounded(image_rate + tokens));
  CHECK(runtime::worst_case_cost(ok(retried.prepare(image("vendor/image", 1), run_with(3))), *priced).attempts == 3);
  // A model the catalogue does not price.
  CHECK(has(bound_of(direct.prepare(image("vendor/unknown", 1))).unbounded, UnboundedBy::ModelPrice, "vendor/unknown"));
}

// A refusal's subject names what prices: the unit, with the offered dimensions whose keys a row of the unit names, and
// the operation. The values of other members (a user id, a session) change no price and are not printed.
void refusal_subjects() {
  auto direct = client_of(openai_chat());
  const std::uint64_t tokens = image_input * image_token_rate + image_output * out_rate;
  // A row keyed on an extension member applies.
  auto low = image("vendor/image-moderated", 1);
  low.extensions = document(R"({"moderation":"low","user":"person-42","safety_identifier":"hash-9"})");
  auto bound = bound_of(direct.prepare(low));
  CHECK(nano(bound) == rounded(moderated_rate + tokens));
  for (const auto& line : bound.lines) CHECK(line.row.find("person-42") == std::string::npos && line.row.find("hash-9") == std::string::npos);
  // No row applies: the key a row names is printed with the call's value, the others are not.
  auto other = low;
  other.extensions = document(R"({"moderation":"auto","user":"person-42","safety_identifier":"hash-9"})");
  bound = bound_of(direct.prepare(other));
  CHECK(reasons_of(bound) == "ModelPrice [item:image[moderation=auto,operation=generate]] catalogue entry 'vendor/image-moderated' has no row of the "
                             "unit that applies to the call; ");
  auto unnamed = image("vendor/image-moderated", 1);
  unnamed.quality = "high";
  unnamed.extensions = document(R"({"user":"person-42"})");
  bound = bound_of(direct.prepare(unnamed));
  CHECK(reasons_of(bound) == "ModelPrice [item:image[operation=generate]] catalogue entry 'vendor/image-moderated' has no row of the unit that "
                             "applies to the call; ");
  // An entry with no row of the unit at all names the unit and the operation alone.
  auto unpriced = unnamed;
  unpriced.model = "vendor/decider";
  CHECK(reasons_of(bound_of(direct.prepare(unpriced))).find("ModelPrice [item:image[operation=generate]] ") == 0);
  for (const auto* request : {&other, &unnamed, &unpriced})
    for (const auto& reason : bound_of(direct.prepare(*request)).unbounded)
      CHECK(reason.subject.find("person-42") == std::string::npos && reason.detail.find("person-42") == std::string::npos &&
            reason.subject.find("hash-9") == std::string::npos && reason.detail.find("hash-9") == std::string::npos);
}

// A member that is set and whose value the qualifier grammar cannot hold is offered without a value: a row qualified by
// it could apply, so the unit it would price has no total, and the price queries give no rate.
void unstated_members() {
  auto direct = client_of(openai_chat(direct_policy));
  const std::uint64_t tokens = image_input * image_token_rate + image_output * out_rate;
  const std::string refused = "ModelPrice [item:image[operation=generate,style]] a row of the unit is qualified by a member whose value the call does not "
                              "state; ";
  const auto styled = [](std::string style, std::string model = "vendor/image-styled") {
    auto request = image(std::move(model), 1);
    request.extensions = document("{\"style\":" + style + "}");
    return request;
  };
  // A number with a fraction, a signed integer, an object, text outside the grammar.
  for (const auto* style : {"1.5", "-2", R"({"name":"x"})", R"("a b")"}) {
    const auto call = direct.prepare(styled(style));
    const auto facts = ok(call).endpoint_facts();
    CHECK(dimensions_of(facts) == "operation=generate,style=");
    CHECK(reasons_of(bound_of(call)) == refused);
    const auto* found = priced->find(facts.family, facts.origin, facts.model).entry;
    CHECK(found && !pricing::dearest_item_price(*found, "item:image", facts.dimensions, "", image_input) &&
          !pricing::item_price(*found, "item:image", facts.dimensions, "", image_input));
    // An entry with no row qualified by the member bounds the call as before.
    CHECK(nano(bound_of(direct.prepare(styled(style, "vendor/image")))) == rounded(image_rate + tokens));
  }
  // A value the grammar holds is offered, and matches only its own rows.
  CHECK(nano(bound_of(direct.prepare(styled(R"("vivid")")))) == rounded(image_rate + tokens));
  CHECK(nano(bound_of(direct.prepare(styled(R"("1.5")")))) == rounded(high_rate + tokens));
  // A typed member no dimension can hold: speech `speed`.
  speech::Request paced{"vendor/speaker-paced", "Hello there.", "alloy"};
  paced.speed = 1.5;
  CHECK(dimensions_of(ok(direct.prepare(paced)).endpoint_facts()) == "operation=speak,speed=,voice=alloy");
  CHECK(reasons_of(bound_of(direct.prepare(paced))) ==
        "ModelPrice [item:character[operation=speak,speed]] a row of the unit is qualified by a member whose value the call does not state; ");
  const auto spoken = rounded(12 * character_rate + long_input * in_rate + long_output * small_out_rate);
  auto unpaced = paced;
  unpaced.speed.reset();
  CHECK(nano(bound_of(direct.prepare(unpaced))) == spoken);
  auto plain = paced;
  plain.model = "vendor/speaker";
  CHECK(nano(bound_of(direct.prepare(plain))) == spoken);
  // A row of a unit the call does not count, qualified by such a member, could apply: no bound.
  auto timed = paced;
  timed.model = "vendor/speaker-timed";
  CHECK(reasons_of(bound_of(direct.prepare(timed))) == "Unmodeled [item:second[speed=1.5]] the entry prices a unit this operation cannot count; ");
  timed.speed.reset();
  CHECK(nano(bound_of(direct.prepare(timed))) == spoken);
}

// A key offered twice: a typed member and an extension of the same name, or an extension named `operation`. The bound
// refuses without printing either value.
void repeated_keys() {
  auto direct = client_of(openai_chat(direct_policy));
  const std::string refused = "Unmodeled [dimensions] the call's dimensions are not sorted by key with each key once; ";
  speech::Request formats{"vendor/speaker", "Hello there.", "alloy"};
  formats.format = "mp3";
  formats.extensions = document(R"({"format":"wav"})");
  CHECK(dimensions_of(ok(direct.prepare(formats)).endpoint_facts()) == "format=mp3,format=wav,operation=speak,voice=alloy");
  CHECK(reasons_of(bound_of(direct.prepare(formats))) == refused);
  speech::Request operations{"vendor/speaker", "Hello there.", "alloy"};
  operations.extensions = document(R"({"operation":"sing"})");
  CHECK(dimensions_of(ok(direct.prepare(operations)).endpoint_facts()) == "operation=speak,operation=sing,voice=alloy");
  CHECK(reasons_of(bound_of(direct.prepare(operations))) == refused);
}

void decisions_bound() {
  auto direct = client_of(openai_chat());
  auto bound = bound_of(direct.prepare(decision("vendor/decider")));
  CHECK(nano(bound) == rounded(decider_input * in_rate + decider_output * small_out_rate) && bound.attempts == 1);
  CHECK(nano(bound_of(direct.prepare(decision("vendor/decider")), measured(500))) == rounded(500 * in_rate + decider_output * small_out_rate));
  // No output limit: nothing bounds the output.
  bound = bound_of(direct.prepare(decision("vendor/decider-open")));
  CHECK(!bound.total && has(bound.unbounded, UnboundedBy::OutputCap, "output"));
}

void embeddings_bound() {
  auto direct = client_of(openai_chat());
  const embeddings::Request three{"vendor/embed", {{"a"}, {"b"}, {"c"}}};
  // The entry's limit counts per input: three inputs, three times the limit.
  auto bound = bound_of(direct.prepare(three));
  CHECK(nano(bound) == rounded(3 * embed_input * embed_rate) && line_of(bound, "input") == "input 1536 x " + std::to_string(embed_rate));
  CHECK(nano(bound_of(direct.prepare(embeddings::Request{"vendor/embed", {{"a"}}}))) == rounded(embed_input * embed_rate));
  // A measured count, capped at the limit times the inputs.
  CHECK(nano(bound_of(direct.prepare(three), measured(700))) == rounded(700 * embed_rate));
  CHECK(nano(bound_of(direct.prepare(three), measured(5000))) == rounded(3 * embed_input * embed_rate));
}

void rerank_bound() {
  auto direct = client_of(openai_chat(direct_policy));
  const rerank::Request rank{"vendor/ranker", "fox", {"a fox", "a dog"}};
  // Each document, and the input up to the limit for each document; search units are a declared zero.
  auto bound = bound_of(direct.prepare(rank));
  CHECK(nano(bound) == rounded(2 * document_rate + 2 * long_input * in_rate));
  CHECK(line_of(bound, "item:document") == "item:document 2 x " + std::to_string(document_rate));
  // Search units priced above zero: nothing in the request bounds them.
  auto units = rank;
  units.model = "vendor/ranker-units";
  bound = bound_of(direct.prepare(units));
  CHECK(!bound.total && has(bound.unbounded, UnboundedBy::Quantity, "item:search_unit") && bound.unbounded.size() == 1);
  // At the routing gateway, the only origin the built-in policy admits rerank at, it keeps no bound.
  auto router = client_of(openrouter_chat());
  bound = bound_of(router.prepare(rank));
  CHECK(!bound.total && has(bound.unbounded, UnboundedBy::RouteCeiling, "vendor/ranker"));
}

void speech_bound() {
  auto direct = client_of(openai_chat());
  // Characters as UTF-8 bytes: "héllo 世界" is 13 bytes in 8 code points.
  const std::string text = "h\xc3\xa9llo \xe4\xb8\x96\xe7\x95\x8c";
  CHECK(text.size() == 13);
  auto bound = bound_of(direct.prepare(speech::Request{"vendor/speaker", text, "alloy"}));
  CHECK(nano(bound) == rounded(13 * character_rate + long_input * in_rate + long_output * small_out_rate));
  CHECK(line_of(bound, "item:character") == "item:character 13 x " + std::to_string(character_rate));
  // The rate is a fraction of a nano-USD per byte: the sum is rounded up once.
  CHECK((13 * character_rate + long_input * in_rate + long_output * small_out_rate) % 1'000'000 != 0);
}

void transcription_bound() {
  auto direct = client_of(openai_chat());
  // The audio's length the host states.
  auto bound = bound_of(direct.prepare(transcript("vendor/transcriber")), seconds(30));
  CHECK(nano(bound) == rounded(30 * second_rate + long_input * in_rate + long_output * small_out_rate));
  CHECK(line_of(bound, "item:second") == "item:second 30 x " + std::to_string(second_rate));
  // A translation is its own operation, priced by its own row.
  bound = bound_of(direct.prepare(transcript("vendor/transcriber", true)), seconds(30));
  CHECK(nano(bound) == rounded(30 * translate_rate + long_input * in_rate + long_output * small_out_rate));
  // Without the length, seconds priced above zero have no bound.
  bound = bound_of(direct.prepare(transcript("vendor/transcriber")));
  CHECK(reasons_of(bound) == "Quantity [item:second] the audio's length is not stated (EndpointCostOptions::audio_seconds); ");
}

void videos_bound() {
  auto direct = client_of(openai_chat(direct_policy));
  const std::uint64_t tokens = long_input * image_token_rate + long_output * small_out_rate;
  videos::Request video{"vendor/video", "A fox in the snow."};
  video.duration = 5;
  video.resolution = "720p";
  auto bound = bound_of(direct.prepare(video));
  CHECK(nano(bound) == rounded(5 * video_second_rate + tokens));
  // The resolution picks its own row.
  auto sharp = video;
  sharp.resolution = "1080p";
  CHECK(nano(bound_of(direct.prepare(sharp))) == rounded(5 * video_1080p_rate + tokens));
  // A reference image.
  auto referenced = video;
  referenced.references = {Media::url(MediaKind::Image, "https://example.com/fox.png", "image/png")};
  bound = bound_of(direct.prepare(referenced));
  CHECK(nano(bound) == rounded(5 * video_second_rate + reference_rate + tokens));
  // An unset duration: nothing bounds the seconds.
  auto open = video;
  open.duration.reset();
  bound = bound_of(direct.prepare(open));
  CHECK(!bound.total && has(bound.unbounded, UnboundedBy::Quantity, "item:second") && bound.unbounded.size() == 1);
  // An extension named like a typed member offers its key twice, and the bound refuses rather than choose one.
  auto twice = video;
  twice.audio = true;
  twice.extensions = document(R"({"audio":false})");
  const auto twice_facts = ok(direct.prepare(twice)).endpoint_facts();
  CHECK(dimensions_of(twice_facts) == "audio=true,audio=false,operation=submit,resolution=720p");
  bound = bound_of(direct.prepare(twice));
  CHECK(!bound.total && has(bound.unbounded, UnboundedBy::Unmodeled, "dimensions"));
  // At the routing gateway, the only origin the built-in policy admits videos at, it keeps no bound.
  auto router = client_of(openrouter_chat());
  CHECK(has(bound_of(router.prepare(video)).unbounded, UnboundedBy::RouteCeiling, "vendor/video"));
}

// ---- the estimate of a finished call, from each result type ------------------------------------------------------

Usage counted(std::optional<std::uint64_t> input, std::optional<std::uint64_t> output, std::initializer_list<std::pair<const char*, std::uint64_t>> extra = {}) {
  Usage usage;
  if (input) usage.input_total = Count{*input, Evidence::Reported};
  if (output) usage.output_total = Count{*output, Evidence::Reported};
  for (const auto& [key, value] : extra) usage.extra[key] = Count{value, Evidence::Reported};
  usage.stage = UsageStage::Final;
  return usage;
}
std::string unpriced_of(const pricing::EndpointEstimate& e) {
  std::string out;
  for (const auto& name : e.unpriced) out += (out.empty() ? "" : ",") + name;
  return out;
}
// The total of an estimate that must have one.
std::uint64_t nano(const pricing::EndpointEstimate& e) {
  if (!e.total) throw std::runtime_error("no estimate: " + unpriced_of(e));
  return e.total->nano_usd;
}
// Entries whose token rates are declared zeros, for results that count no tokens.
const auto items_priced = catalogue_of("{\"version\":1,\"models\":[" +
    entry("vendor/ranker-free", limits(long_input), {{"input", 0}, {"item:document", document_rate}, {"item:search_unit", search_unit_rate}}) + "," +
    entry("vendor/speaker-free", limits(long_input, long_output), {{"input", 0}, {"output", 0}, {"item:character", character_rate}}) + "," +
    entry("vendor/video-free", limits(long_input, long_output),
          {{"input", 0}, {"input_image", 0}, {"output", 0}, {"item:second", video_second_rate}, {"item:second[resolution=1080p]", video_1080p_rate},
           {"item:input_image", reference_rate}}) + "]}");

// ---- endpoint calls at the routing gateway -----------------------------------------------------------------------

// Entries at the routing gateway, with listed rates and route ceilings for every unit an endpoint counts.
constexpr std::uint64_t routed_embed_rate = 100'000'000, routed_embed_ceiling = 150'000'000;
const auto routed = catalogue_of("{\"version\":1,\"models\":[" +
    entry("vendor/embed", limits(embed_input), {{"input", routed_embed_rate}, {"ceiling_input", routed_embed_ceiling}}) + "," +
    // A ceiling of 0.02 USD per million tokens, whose nearest JSON number reads as one nano-USD more.
    entry("vendor/embed-small", limits(embed_input), {{"input", 10'000'000}, {"ceiling_input", 20'000'000}}) + "," +
    entry("vendor/image", limits(image_input, image_output),
          {{"input", in_rate}, {"input_image", image_token_rate}, {"output", out_rate}, {"item:image", image_rate}, {"item:input_image", reference_rate},
           {"ceiling_input", in_rate}, {"ceiling_output", out_rate}}) + "," +
    entry("vendor/decider", limits(decider_input, decider_output),
          {{"input", in_rate}, {"output", small_out_rate}, {"ceiling_input", in_rate}, {"ceiling_output", small_out_rate}}) + "," +
    entry("vendor/ranker", limits(long_input),
          {{"input", in_rate}, {"item:document", document_rate}, {"item:search_unit", 0}, {"ceiling_input", in_rate}}) + "," +
    entry("vendor/speaker", limits(long_input, long_output),
          {{"input", in_rate}, {"output", small_out_rate}, {"item:character", character_rate}, {"ceiling_input", in_rate}, {"ceiling_output", small_out_rate}}) + "," +
    entry("vendor/transcriber", limits(long_input, long_output),
          {{"input", in_rate}, {"output", small_out_rate}, {"item:second", second_rate}, {"ceiling_input", in_rate}, {"ceiling_output", small_out_rate}}) + "," +
    entry("vendor/video", limits(long_input, long_output),
          {{"input", in_rate}, {"input_image", image_token_rate}, {"output", small_out_rate}, {"item:second", video_second_rate},
           {"item:input_image", reference_rate}, {"ceiling_input", in_rate}, {"ceiling_output", small_out_rate}}) + "]}");
// Three inputs, with `extensions` when given.
embeddings::Request embedding(std::string_view extensions = {}) {
  embeddings::Request request{"vendor/embed", {{"a"}, {"b"}, {"c"}}};
  if (!extensions.empty()) request.extensions = document(extensions);
  return request;
}
// A nano-USD amount as the decimal USD text a host writes it in (20000000 is 0.02).
std::string usd_text(std::uint64_t nano_usd) {
  std::string fraction = std::to_string(nano_usd % 1'000'000'000);
  fraction.insert(0, 9 - fraction.size(), '0');
  while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
  return std::to_string(nano_usd / 1'000'000'000) + (fraction.empty() ? "" : "." + fraction);
}
std::string capped(std::string_view prompt) { return R"({"provider":{"sort":"price","max_price":{"prompt":)" + std::string(prompt) + "}}}"; }
const std::string unpinned = "an endpoint call at a routing gateway has no per-item route ceiling and no pin";

// Embeddings at the routing gateway: the request's `provider.max_price.prompt` caps the router's choice, so the input
// is bounded at the lower of that cap and the route ceiling, and the bound states the pin the request must carry.
void gateway_embeddings_bound() {
  auto router = client_of(openrouter_chat());
  const auto bound_at = [&](const embeddings::Request& request, runtime::EndpointCostOptions options = {}) {
    return runtime::endpoint_worst_case_cost(ok(router.prepare(request)), *routed, options);
  };
  // Without a cap, no bound.
  auto bound = bound_at(embedding());
  CHECK(reasons_of(bound) == "RouteCeiling [vendor/embed] " + unpinned + "; " && !bound.route_ceiling && !bound.pin);
  CHECK(!ok(router.prepare(embedding())).endpoint_facts().max_price);
  CHECK(reasons_of(bound_at(embedding(R"({"provider":{"sort":"price"}})"))) == "RouteCeiling [vendor/embed] " + unpinned + "; ");
  // 0.12 USD per million tokens: the binary number is just below 0.12, so its exact value rounds up to 120000000 nano-USD.
  const auto pinned = embedding(capped("0.12"));
  const auto facts = ok(router.prepare(pinned)).endpoint_facts();
  CHECK(facts.max_price && facts.max_price->prompt_nano_usd == 120'000'000 && !facts.max_price->completion_nano_usd && facts.service_tier.empty());
  bound = bound_at(pinned);
  CHECK(nano(bound) == rounded(3 * embed_input * 120'000'000) && line_of(bound, "input") == "provider.max_price.prompt 1536 x 120000000");
  CHECK(bound.route_ceiling && bound.pinned && bound.pin && bound.pin->prompt_nano_usd == 120'000'000 && bound.attempts == 1);
  CHECK(nano(bound_at(pinned, measured(700))) == rounded(std::uint64_t{700} * 120'000'000));
  // The projection onto a chat request's bound carries the ceiling and the pin.
  CHECK(describe(runtime::worst_case_cost(ok(router.prepare(pinned, run_with(3))), *routed)) ==
        "total " + std::to_string(rounded(3 * embed_input * 120'000'000)) +
            " Derived CeilingCatalogueProduct | entry 'vendor/embed' model 'vendor/embed' tokens 1536/0/1536 fees 0 invocations 1 attempts 1 tools 0 "
            "ceiling 1 pinned 1 pin 1");
  // The binary values of 0.1 and 1e-9 are just above them: one nano-USD more, never less.
  CHECK(ok(router.prepare(embedding(capped("0.1")))).endpoint_facts().max_price->prompt_nano_usd == 100'000'001);
  CHECK(ok(router.prepare(embedding(capped("1e-9")))).endpoint_facts().max_price->prompt_nano_usd == 2);
  CHECK(ok(router.prepare(embedding(capped("1")))).endpoint_facts().max_price->prompt_nano_usd == 1'000'000'000);
  CHECK(ok(router.prepare(embedding(capped("0")))).endpoint_facts().max_price->prompt_nano_usd == 0);
  // A cap of zero is no cap the bound takes: how the gateway answers it is not known.
  CHECK(reasons_of(bound_at(embedding(capped("0")))) == "RouteCeiling [vendor/embed] " + unpinned + "; ");
  // A cap above the route ceiling: the ceiling bounds the call, and the pin to carry is the ceiling.
  bound = bound_at(embedding(capped("0.2")));
  CHECK(nano(bound) == rounded(3 * embed_input * routed_embed_ceiling) && line_of(bound, "input") == "ceiling_input 1536 x 150000000");
  CHECK(bound.route_ceiling && !bound.pinned && bound.pin && bound.pin->prompt_nano_usd == routed_embed_ceiling);
  // The pin written back, as a host writes its amount, is carried: 0.02 reads as 20000001 nano-USD, one above the ceiling.
  auto small = embedding(capped("0.03"));
  small.model = "vendor/embed-small";
  bound = bound_at(small);
  CHECK(!bound.pinned && bound.pin && bound.pin->prompt_nano_usd == 20'000'000 && usd_text(*bound.pin->prompt_nano_usd) == "0.02");
  auto written = embedding(capped(usd_text(*bound.pin->prompt_nano_usd)));
  written.model = "vendor/embed-small";
  CHECK(ok(router.prepare(written)).endpoint_facts().max_price->prompt_nano_usd == 20'000'001);
  bound = bound_at(written);
  CHECK(nano(bound) == rounded(3 * embed_input * 20'000'000) && line_of(bound, "input") == "ceiling_input 1536 x 20000000" && bound.pinned &&
        bound.pin->prompt_nano_usd == 20'000'000);
  // A cap above the largest amount max_price takes is no cap the bound reads.
  CHECK(!ok(router.prepare(embedding(capped("10000000")))).endpoint_facts().max_price);
  CHECK(reasons_of(bound_at(embedding(capped("10000000")))) == "RouteCeiling [vendor/embed] " + unpinned + "; ");
  // A cap the policy does not admit: text, a negative number, another amount.
  for (const auto* refused : {R"("0.12")", "-1"}) {
    const auto call = router.prepare(embedding(capped(refused)));
    CHECK(call.prepared().error() && call.prepared().error()->reason == "request.embeddings.options_invalid");
  }
  const auto completion = router.prepare(embedding(R"({"provider":{"max_price":{"prompt":0.1,"completion":0.1}}})"));
  CHECK(completion.prepared().error() && completion.prepared().error()->reason == "request.embeddings.options_invalid");
  // At an origin that is not a routing gateway the policy admits no cap: the call is bounded at the listed rate.
  auto direct = client_of(openai_chat());
  CHECK(direct.prepare(embedding(capped("0.12"))).prepared().error());
  CHECK(nano(bound_of(direct.prepare(embedding()))) == rounded(3 * embed_input * embed_rate));

  // The estimate: the reported tokens at the listed rate, and at the cap where the cap is lower.
  embeddings::Result result;
  result.usage = counted(900, std::nullopt);
  result.model = ServedModel{"vendor/embed", "vendor/embed", ServedRelation::Exact};
  auto e = runtime::endpoint_estimate(facts, result, *routed);
  CHECK(nano(e) == rounded(900 * routed_embed_rate) && e.assumptions == std::vector<std::string>{"listed rates"} && e.total->nano_usd <= nano(bound_at(pinned)));
  const auto low = ok(router.prepare(embedding(capped("0.05")))).endpoint_facts();
  e = runtime::endpoint_estimate(low, result, *routed);
  CHECK(nano(e) == rounded(std::uint64_t{900} * 50'000'001) && e.assumptions == (std::vector<std::string>{"listed rates", "input at the pin"}));
  CHECK(e.total->nano_usd <= nano(bound_at(embedding(capped("0.05")))));
}

// Every other endpoint at the routing gateway keeps no bound, with an entry that lists every unit and the route
// ceilings: decisions, images, rerank, speech, transcription and videos. Their policy facts admit no cap.
void gateway_refusals() {
  auto router = client_of(openrouter_chat());
  const auto refused = [&](const auto& call, std::string_view model) {
    const auto bound = runtime::endpoint_worst_case_cost(ok(call), *routed, seconds(30));
    return reasons_of(bound) == "RouteCeiling [" + std::string(model) + "] " + unpinned + "; " && !bound.route_ceiling && !bound.pin &&
           !ok(call).endpoint_facts().max_price;
  };
  CHECK(refused(router.prepare(decision("vendor/decider")), "vendor/decider"));
  CHECK(refused(router.prepare(image("vendor/image", 1)), "vendor/image"));
  CHECK(refused(router.prepare(rerank::Request{"vendor/ranker", "fox", {"a fox", "a dog"}}), "vendor/ranker"));
  CHECK(refused(router.prepare(speech::Request{"vendor/speaker", "Hello there.", "alloy"}), "vendor/speaker"));
  CHECK(refused(router.prepare(transcript("vendor/transcriber")), "vendor/transcriber"));
  videos::Request video{"vendor/video", "A fox in the snow."};
  video.duration = 5;
  CHECK(refused(router.prepare(video), "vendor/video"));
  // A cap on a decision, an image or a video is not admitted.
  auto capped_decision = decision("vendor/decider");
  capped_decision.extensions = document(capped("0.1"));
  CHECK(router.prepare(capped_decision).prepared().error());
  auto capped_image = image("vendor/image", 1);
  capped_image.extensions = document(capped("0.1"));
  CHECK(router.prepare(capped_image).prepared().error());
  auto capped_video = video;
  capped_video.extensions = document(capped("0.1"));
  CHECK(router.prepare(capped_video).prepared().error());
}

// No endpoint request names a service tier: no endpoint fact of the built-in policy admits one as an option, so every
// prepared endpoint call states none and is priced at the standard rates. A fact that admits one must carry it into
// EndpointFacts::service_tier.
void no_endpoint_names_a_tier() {
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  const auto root = std::get<json::Document>(parsed).root();
  std::size_t facts = 0;
  for (auto fact : root.get("endpoints").elements()) {
    ++facts;
    const auto options = fact.get("options");
    if (!options.valid()) continue;
    CHECK(!options.get("properties").get("service_tier").valid());
  }
  CHECK(facts > 0);
  auto direct = client_of(openai_chat());
  auto router = client_of(openrouter_chat());
  CHECK(ok(direct.prepare(image("vendor/image", 1))).endpoint_facts().service_tier.empty());
  CHECK(ok(direct.prepare(embedding())).endpoint_facts().service_tier.empty());
  CHECK(ok(router.prepare(decision("vendor/decider"))).endpoint_facts().service_tier.empty());
}

void estimates_per_result() {
  auto direct = client_of(openai_chat());
  // Images: two asked at high quality; 300 input tokens of which 100 are image tokens, 50 output tokens, and both images.
  auto two = image("vendor/image", 2);
  two.quality = "high";
  const auto image_facts = ok(direct.prepare(two)).endpoint_facts();
  images::Result made;
  made.images = {Media::url(MediaKind::Image, "https://example.com/a.png", "image/png"), Media::url(MediaKind::Image, "https://example.com/b.png", "image/png")};
  made.usage = counted(300, 50, {{"input_tokens_details.image_tokens", 100}, {"input_tokens_details.text_tokens", 200}});
  made.model = ServedModel{"vendor/image", "", ServedRelation::NotReported};
  auto e = runtime::endpoint_estimate(image_facts, made, *priced);
  CHECK(nano(e) == rounded(200 * in_rate + 100 * image_token_rate + 50 * out_rate + 2 * high_rate) && !e.lower_bound && e.entry == "vendor/image");
  const auto image_bound = bound_of(direct.prepare(two), measured(300));
  CHECK(nano(image_bound) >= nano(e) && pricing::beyond_bound(image_bound, e).empty());
  // One of the two returned: priced as returned, and the provider may bill the other.
  made.images.pop_back();
  e = runtime::endpoint_estimate(image_facts, made, *priced);
  CHECK(nano(e) == rounded(200 * in_rate + 100 * image_token_rate + 50 * out_rate + high_rate) && e.lower_bound);
  // Decisions and embeddings: their tokens.
  const auto decision_facts = ok(direct.prepare(decision("vendor/decider"))).endpoint_facts();
  decisions::Result decided;
  decided.usage = counted(120, 30);
  decided.model = ServedModel{"vendor/decider", "vendor/decider", ServedRelation::Exact};
  CHECK(nano(runtime::endpoint_estimate(decision_facts, decided, *priced)) == rounded(120 * in_rate + 30 * small_out_rate));
  const auto embed_facts = ok(direct.prepare(embeddings::Request{"vendor/embed", {{"a"}, {"b"}, {"c"}}})).endpoint_facts();
  embeddings::Result embedded;
  embedded.usage = counted(40, std::nullopt);
  CHECK(nano(runtime::endpoint_estimate(embed_facts, embedded, *priced)) == rounded(40 * embed_rate));
  embedded.usage = Usage{};
  CHECK(unpriced_of(runtime::endpoint_estimate(embed_facts, embedded, *priced)) == "input");
  // Rerank: the request's documents and the search units the result reports; an input rate needs an input counter.
  auto ranker = client_of(openai_chat(direct_policy));
  const auto rank_facts = ok(ranker.prepare(rerank::Request{"vendor/ranker-free", "fox", {"a fox", "a dog"}})).endpoint_facts();
  rerank::Result ranked;
  ranked.usage = counted(std::nullopt, std::nullopt, {{"search_units", 3}});
  CHECK(nano(runtime::endpoint_estimate(rank_facts, ranked, *items_priced)) == rounded(2 * document_rate + 3 * search_unit_rate));
  ranked.usage = Usage{};
  CHECK(unpriced_of(runtime::endpoint_estimate(rank_facts, ranked, *items_priced)) == "item:search_unit");
  const auto priced_rank = ok(ranker.prepare(rerank::Request{"vendor/ranker-units", "fox", {"a fox", "a dog"}})).endpoint_facts();
  ranked.usage = counted(std::nullopt, std::nullopt, {{"search_units", 3}});
  CHECK(unpriced_of(runtime::endpoint_estimate(priced_rank, ranked, *priced)) == "input");
  // Speech: the request's bytes; the result counts no tokens, so a model with token rates has no estimate.
  const std::string text = "h\xc3\xa9llo \xe4\xb8\x96\xe7\x95\x8c";
  speech::Result spoken;
  e = runtime::endpoint_estimate(ok(direct.prepare(speech::Request{"vendor/speaker-free", text, "alloy"})).endpoint_facts(), spoken, *items_priced);
  CHECK(nano(e) == rounded(13 * character_rate) && e.assumptions.empty());
  CHECK(unpriced_of(runtime::endpoint_estimate(ok(direct.prepare(speech::Request{"vendor/speaker", text, "alloy"})).endpoint_facts(), spoken, *priced)) ==
        "input,output");
  // Transcription: the seconds the result reports; a translation by its own row; reported seconds above the stated ones.
  transcription::Result heard;
  heard.usage = counted(100, 20, {{"seconds", 3}});
  const auto transcribe_facts = ok(direct.prepare(transcript("vendor/transcriber"))).endpoint_facts();
  e = runtime::endpoint_estimate(transcribe_facts, heard, *priced);
  CHECK(nano(e) == rounded(3 * second_rate + 100 * in_rate + 20 * small_out_rate));
  CHECK(nano(runtime::endpoint_estimate(ok(direct.prepare(transcript("vendor/transcriber", true))).endpoint_facts(), heard, *priced)) ==
        rounded(3 * translate_rate + 100 * in_rate + 20 * small_out_rate));
  const auto stated = bound_of(direct.prepare(transcript("vendor/transcriber")), seconds(2));
  CHECK(pricing::beyond_bound(stated, e) == std::vector<std::string>{"item:second"});
  heard.usage = counted(100, 20);
  CHECK(unpriced_of(runtime::endpoint_estimate(transcribe_facts, heard, *priced)) == "item:second");
  // A video's final poll, priced with its submit's facts: the duration asked for, assumed; a job not completed has none.
  auto videos = client_of(openai_chat(direct_policy));
  videos::Request video{"vendor/video-free", "A fox in the snow."};
  video.duration = 5;
  video.resolution = "1080p";
  const auto video_facts = ok(videos.prepare(video)).endpoint_facts();
  jobs::State state;
  state.status = jobs::Status::Completed;
  e = runtime::endpoint_estimate(video_facts, state, *items_priced);
  CHECK(nano(e) == rounded(5 * video_1080p_rate) && e.assumptions == std::vector<std::string>{"item:second at the request's bound"});
  for (const auto status : {jobs::Status::Pending, jobs::Status::Running, jobs::Status::Failed, jobs::Status::Cancelled, jobs::Status::Expired, jobs::Status::Unknown}) {
    state.status = status;
    e = runtime::endpoint_estimate(video_facts, state, *items_priced);
    CHECK(!e.total && unpriced_of(e) == "job");
  }
  // An operation the policy states is not billed: zero, whatever its result reports.
  auto poll_facts = video_facts;
  poll_facts.operation = "poll";
  poll_facts.billing = descriptor::EndpointBilling::None;
  state.status = jobs::Status::Completed;
  state.usage = counted(100, 100);
  e = runtime::endpoint_estimate(poll_facts, state, *items_priced);
  CHECK(e.total && e.total->nano_usd == 0);
  std::cout << "case passed: the estimate of each result type\n";
}

// ---- batches: the requests a submit carries, priced under the policy's carried tier ----------------------------

// Comparisons that keep going and report every mismatch with both sides, so a red run states what the code answered.
std::vector<std::string> mismatches;
void expect(int line, const std::string& actual, const std::string& expected) {
  if (actual != expected) mismatches.push_back("line " + std::to_string(line) + ":\n    got      " + actual + "\n    expected " + expected);
}
#define EXPECT(actual, expected) expect(__LINE__, (actual), (expected))
void report_mismatches(const char* name) {
  if (mismatches.empty()) return;
  std::string all = std::string(name) + ": " + std::to_string(mismatches.size()) + " mismatch(es)";
  for (const auto& m : mismatches) all += "\n  " + m;
  mismatches.clear();
  throw std::runtime_error(all);
}
// Every member of an endpoint bound, so that a test states the whole answer.
std::string described(const pricing::EndpointBound& b) {
  std::string out = b.total ? "total " + std::to_string(b.total->nano_usd) : std::string("no total");
  for (const auto& reason : b.unbounded) out += " | " + std::string(pricing::to_string(reason.by)) + " [" + reason.subject + "] " + reason.detail;
  out += " | entry '" + b.entry + "' model '" + b.model + "' attempts " + std::to_string(b.attempts) + " lines";
  for (const auto& l : b.lines) out += " " + l.unit + "=" + l.row + ":" + std::to_string(l.quantity) + "x" + std::to_string(l.rate);
  return out;
}
// What a submit carries, as `<custom_id>/<model>/<output cap>/<invocations>/<tier>`.
std::string carried_of(const EndpointFacts& facts) {
  std::string out;
  const auto number = [](const std::optional<std::uint64_t>& value) { return value ? std::to_string(*value) : std::string("-"); };
  for (const auto& r : facts.carried)
    out += (out.empty() ? "" : " ") + r.custom_id + "/" + r.model + "/" + number(r.output_tokens) + "/" + number(r.invocations) + "/" + r.facts.service_tier;
  return out + " file '" + facts.input_file + "'";
}
// Batch rates, in nano-USD per million tokens, each with a fraction of a nano-USD per token so a bound rounds up; the
// standard rates of the same entries are dearer, so a bound at them is visible.
constexpr std::uint64_t batch_in = 500'000'001, batch_out = 2'000'000'003, batch_fee = 1'500'000'000'007, standard_in = 1'000'000'000,
                        standard_out = 4'000'000'000, fee_in = 250'000'000, fee_out = 1'000'000'000;
constexpr std::uint64_t batched_input = 2000, batched_output = 100, fee_input = 3000;
const auto batch_priced = catalogue_of("{\"version\":1,\"models\":[" +
    entry("vendor/batched", limits(batched_input, batched_output),
          {{"input", standard_in}, {"output", standard_out}, {"input/batch", batch_in}, {"output/batch", batch_out}}) + "," +
    entry("vendor/batched-fee", limits(fee_input),
          {{"input", standard_in}, {"output", standard_out}, {"input/batch", fee_in}, {"output/batch", fee_out}, {"request/batch", batch_fee}}) + "," +
    // A Google batch names its model in the path, as one segment.
    entry("vendor-batched", limits(batched_input, batched_output),
          {{"input", standard_in}, {"output", standard_out}, {"input/batch", batch_in}, {"output/batch", batch_out}}) + "," +
    // Standard rates only: nothing prices a batch.
    entry("vendor/standard-only", limits(batched_input, batched_output), {{"input", standard_in}, {"output", standard_out}}) + "]}");
descriptor::ValidatedDescriptor google_generate() {
  auto loaded = descriptor::presets::try_google_generate();
  if (auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("descriptor refused: " + error->pointer);
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}

void batches_bound() {
  // Anthropic, inline: each request bounded at the batch rates, the first at its measured input, the second at its
  // model's input limit; the submit is charged on acceptance, so one attempt of three.
  auto messages_api = client_of(anthropic_messages());
  const auto inline_submit = messages_api.prepare(batches::Submit{{{"a-1", messages::Request::simple("vendor/batched", "Say: alpha", 16)},
                                                                   {"a-2", messages::Request::simple("vendor/batched-fee", "Say: beta", 50)}}},
                                                  run_with(3));
  EXPECT(carried_of(ok(inline_submit).endpoint_facts()), "a-1/vendor/batched/16/1/ a-2/vendor/batched-fee/50/1/ file ''");
  runtime::EndpointCostOptions measured_first;
  measured_first.measured_carried_input_tokens = {{"a-1", 300}};
  const auto first = rounded(300 * batch_in + 16 * batch_out), second = rounded(fee_input * fee_in + 50 * fee_out + batch_fee);
  EXPECT(described(runtime::endpoint_worst_case_cost(ok(inline_submit), *batch_priced, measured_first)),
         "total " + std::to_string(first + second) + " | entry '' model '' attempts 1 lines a-1:nano_usd=vendor/batched:" + std::to_string(first) +
         "x1000000 a-2:nano_usd=vendor/batched-fee:" + std::to_string(second) + "x1000000");
  // worst_case_cost answers the same total and subjects, with no tokens or invocations of the submit's own.
  const auto unmeasured_first = rounded(batched_input * batch_in + 16 * batch_out);
  EXPECT(describe(runtime::worst_case_cost(ok(inline_submit), *batch_priced)),
         "total " + std::to_string(unmeasured_first + second) + " Derived CeilingCatalogueProduct | entry '' model '' tokens 0/0/0 fees 0 invocations 0 "
         "attempts 1 tools 0 ceiling 0 pinned 0 pin 0");

  // Google, inline: one model for every request.
  auto generate_api = client_of(google_generate());
  const auto google_submit = generate_api.prepare(batches::Submit{{{"g-1", gemini::Request::simple("vendor-batched", "Say: alpha", 10)},
                                                                   {"g-2", gemini::Request::simple("vendor-batched", "Say: beta", 20)}}});
  EXPECT(carried_of(ok(google_submit).endpoint_facts()), "g-1/vendor-batched/10/1/ g-2/vendor-batched/20/1/ file ''");
  const auto g1 = rounded(batched_input * batch_in + 10 * batch_out), g2 = rounded(batched_input * batch_in + 20 * batch_out);
  EXPECT(described(runtime::endpoint_worst_case_cost(ok(google_submit), *batch_priced)),
         "total " + std::to_string(g1 + g2) + " | entry 'vendor-batched' model '' attempts 1 lines g-1:nano_usd=vendor-batched:" + std::to_string(g1) +
         "x1000000 g-2:nano_usd=vendor-batched:" + std::to_string(g2) + "x1000000");

  // No batch rows: the standard rates are not a bound on a batch's; the reasons keep their kind, prefixed by the
  // custom id. A request without a cap of its own takes the family's default cap, as it would on its own.
  const auto unpriced_submit = messages_api.prepare(batches::Submit{{{"a-1", messages::Request::simple("vendor/standard-only", "Say: alpha", 16)},
                                                                     {"a-2", messages::Request::simple("vendor/unlisted", "Say: beta")}}});
  EXPECT(carried_of(ok(unpriced_submit).endpoint_facts()), "a-1/vendor/standard-only/16/1/ a-2/vendor/unlisted/1024/1/ file ''");
  EXPECT(reasons_of(runtime::endpoint_worst_case_cost(ok(unpriced_submit), *batch_priced)),
         "TierPrice [a-1:vendor/standard-only] the entry prices no unit under service tier 'batch'; ModelPrice [a-2:vendor/unlisted] the catalogue has no "
         "price for the model at the origin; ");
  EXPECT(describe(runtime::worst_case_cost(ok(unpriced_submit), *batch_priced)),
         "no total | TierPrice [a-1:vendor/standard-only] the entry prices no unit under service tier 'batch' | ModelPrice [a-2:vendor/unlisted] the "
         "catalogue has no price for the model at the origin | entry '' model '' tokens 0/0/0 fees 0 invocations 0 attempts 0 tools 0 ceiling 0 pinned 0 pin 0");

  // OpenAI: the requests are in the uploaded file the submit names, which the prepared call does not read.
  auto chat_api = client_of(openai_chat());
  const auto file_submit = chat_api.prepare(batches::Submit{{}, "file-abc", "/v1/chat/completions"});
  EXPECT(carried_of(ok(file_submit).endpoint_facts()), " file 'file-abc'");
  EXPECT(reasons_of(runtime::endpoint_worst_case_cost(ok(file_submit), *batch_priced)),
         "Unmodeled [batches/submit] the batch's requests are in an uploaded file; ");
  EXPECT(describe(runtime::worst_case_cost(ok(file_submit), *batch_priced)),
         "no total | Unmodeled [batches/submit] the batch's requests are in an uploaded file | entry '' model '' tokens 0/0/0 fees 0 invocations 0 "
         "attempts 0 tools 0 ceiling 0 pinned 0 pin 0");

  // The reads bill nothing.
  EXPECT(described(runtime::endpoint_worst_case_cost(messages_api.prepare(batches::Get{"msgbatch_1"}).prepared(), *batch_priced)),
         "total 0 | entry '' model '' attempts 1 lines");
  EXPECT(described(runtime::endpoint_worst_case_cost(messages_api.prepare(batches::Cancel{"msgbatch_1"}).prepared(), *batch_priced)),
         "total 0 | entry '' model '' attempts 1 lines");
  EXPECT(described(runtime::endpoint_worst_case_cost(messages_api.prepare(batches::Delete{"msgbatch_1"}).prepared(), *batch_priced)),
         "total 0 | entry '' model '' attempts 1 lines");
  batches::Batch ended;
  ended.origin = "https://api.anthropic.com";
  ended.shape = "anthropic";
  ended.id = "msgbatch_1";
  EXPECT(described(runtime::endpoint_worst_case_cost(messages_api.prepare(batches::Results{ended}).prepared(), *batch_priced)),
         "total 0 | entry '' model '' attempts 1 lines");
  report_mismatches("batches_bound");
  std::cout << "case passed: the bound of a batch submit\n";
}

// Lower-case hex SHA-256, written here as files::File::uploaded_sha256 spells it.
std::string hex_sha256(std::string_view text) {
  std::string out;
  for (const auto byte : crypto::sha256(text)) {
    out += "0123456789abcdef"[byte >> 4];
    out += "0123456789abcdef"[byte & 15];
  }
  return out;
}
// The file an upload of `text` returns, under `id` at `origin`.
files::File uploaded(std::string id, std::string_view text, std::string_view origin = openai) {
  files::File file;
  file.origin = std::string(origin);
  file.id = std::move(id);
  file.uploaded_sha256 = hex_sha256(text);
  return file;
}

void openai_batch_bound() {
  // The items the host wrote the uploaded file from, bounded as an inline batch's: the first at its measured input.
  auto chat_api = client_of(openai_chat());
  const auto family = openai_chat();
  std::vector<batches::Item> items{{"o-1", chat::Request::simple("vendor/batched", "Say: alpha", 16)},
                                   {"o-2", chat::Request::simple("vendor/batched-fee", "Say: beta", 50)}};
  const auto text = std::get<std::string>(batches::jsonl(family, items));
  const auto file = uploaded("file-abc", text);
  const auto submit = chat_api.prepare(batches::Submit{{}, "file-abc", "/v1/chat/completions"}, run_with(3));
  runtime::EndpointCostOptions measured_first;
  measured_first.measured_carried_input_tokens = {{"o-1", 300}};
  const auto first = rounded(300 * batch_in + 16 * batch_out), second = rounded(fee_input * fee_in + 50 * fee_out + batch_fee);
  const std::string bounded = "total " + std::to_string(first + second) + " | entry '' model '' attempts 1 lines o-1:nano_usd=vendor/batched:" +
                              std::to_string(first) + "x1000000 o-2:nano_usd=vendor/batched-fee:" + std::to_string(second) + "x1000000";
  EXPECT(described(runtime::batch_worst_case_cost(ok(submit), family, items, file, *batch_priced, measured_first)), bounded);
  // Without a catalogue, the policy's, which prices neither model.
  EXPECT(reasons_of(runtime::batch_worst_case_cost(ok(submit), family, items, file)),
         "ModelPrice [o-1:vendor/batched] the catalogue has no price for the model at the origin; ModelPrice [o-2:vendor/batched-fee] the catalogue "
         "has no price for the model at the origin; ");

  // Not the uploaded file: other bytes, another file, a file whose upload digest is unknown (a get's), the items in
  // another order, and items another family would write.
  constexpr const char* not_the_file = "Unmodeled [batches/submit] the items are not the uploaded input file; ";
  EXPECT(reasons_of(runtime::batch_worst_case_cost(ok(submit), family, items, uploaded("file-abc", text + "\n"), *batch_priced)), not_the_file);
  EXPECT(reasons_of(runtime::batch_worst_case_cost(ok(submit), family, items, uploaded("file-other", text), *batch_priced)), not_the_file);
  // The same id and bytes, held at another origin.
  EXPECT(reasons_of(runtime::batch_worst_case_cost(ok(submit), family, items, uploaded("file-abc", text, "https://files.example"), *batch_priced)), not_the_file);
  EXPECT(reasons_of(runtime::batch_worst_case_cost(ok(submit), family, items, uploaded("file-abc", text, ""), *batch_priced)), not_the_file);
  files::File fetched;
  fetched.origin = std::string(openai);
  fetched.id = "file-abc";
  EXPECT(reasons_of(runtime::batch_worst_case_cost(ok(submit), family, items, fetched, *batch_priced)), not_the_file);
  const std::vector<batches::Item> reordered{items[1], items[0]};
  EXPECT(reasons_of(runtime::batch_worst_case_cost(ok(submit), family, reordered, file, *batch_priced)), not_the_file);
  auto responses_family = descriptor::presets::openai_responses();
  EXPECT(reasons_of(runtime::batch_worst_case_cost(ok(submit), responses_family, items, file, *batch_priced)), not_the_file);
  // Items whose bytes are the file, but under another family descriptor of another origin.
  EXPECT(reasons_of(runtime::batch_worst_case_cost(ok(submit), anthropic_messages(), items, file, *batch_priced)),
         "Unmodeled [batches/submit] the items' descriptor is not at the batch's origin; ");
  // A call that reads no uploaded file: an inline submit, a read, a chat request.
  auto messages_api = client_of(anthropic_messages());
  const auto inline_submit = messages_api.prepare(batches::Submit{{{"a-1", messages::Request::simple("vendor/batched", "Say: alpha", 16)}}});
  EXPECT(reasons_of(runtime::batch_worst_case_cost(ok(inline_submit), family, items, file, *batch_priced)),
         "Unmodeled [batches/submit] the call reads no uploaded input file; ");
  EXPECT(reasons_of(runtime::batch_worst_case_cost(ok(chat_api.prepare(batches::Get{"batch_1"})), family, items, file, *batch_priced)),
         "Unmodeled [batches/get] the call reads no uploaded input file; ");
  chat::Request chat = chat::Request::simple("vendor/batched", "hello", 10);
  EXPECT(reasons_of(runtime::batch_worst_case_cost(chat_api.prepare(chat), family, items, file, *batch_priced)),
         "Unmodeled [request] a chat request is bounded by worst_case_cost; ");

  // An item's own tier: a standard tier is the standard rates' name and is bounded as none; another is refused.
  auto tiered = items;
  std::get<chat::Request>(tiered[0].request).service_tier = "default";
  const auto tiered_text = std::get<std::string>(batches::jsonl(family, tiered));
  CHECK(tiered_text != text);
  EXPECT(described(runtime::batch_worst_case_cost(ok(submit), family, tiered, uploaded("file-abc", tiered_text), *batch_priced, measured_first)), bounded);
  std::get<chat::Request>(tiered[0].request).service_tier = "flex";
  const auto flex_text = std::get<std::string>(batches::jsonl(family, tiered));
  EXPECT(reasons_of(runtime::batch_worst_case_cost(ok(submit), family, tiered, uploaded("file-abc", flex_text), *batch_priced)),
         "TierPrice [o-1:vendor/batched] a batch item names its own service tier; ");
  report_mismatches("openai_batch_bound");
  std::cout << "case passed: the bound of an OpenAI batch from its uploaded file\n";
}

// A finished request's usage: input tokens (cache reads among them), output tokens (reasoning among them).
Usage request_usage(std::uint64_t input, std::uint64_t output, std::uint64_t cache_read = 0, std::uint64_t reasoning = 0) {
  Usage usage;
  usage.stage = UsageStage::Final;
  usage.input_total = Count{input};
  usage.output_total = Count{output};
  if (cache_read) usage.cache_read = Count{cache_read};
  if (reasoning) usage.reasoning = Count{reasoning};
  return usage;
}
batches::Line answered(std::string id, std::string model, Usage usage) {
  Completion completion;
  completion.model.requested = std::move(model);
  completion.usage = std::move(usage);
  return batches::Line{std::move(id), completion};
}
batches::Line failed_line(std::string id) {
  Failure failure;
  failure.error = Error{ErrorKind::RemoteFailure, "the provider failed the batch request"};
  return batches::Line{std::move(id), failure};
}
std::string estimated(const pricing::EndpointEstimate& e) {
  std::string out = e.total ? "total " + std::to_string(e.total->nano_usd) : std::string("no total");
  for (const auto& name : e.unpriced) out += " | unpriced " + name;
  for (const auto& rule : e.assumptions) out += " | assumed " + rule;
  out += " | entry '" + e.entry + "' requested " + std::to_string(e.requested_entry) + " lower " + std::to_string(e.lower_bound) + " lines";
  for (const auto& l : e.lines) out += " " + l.unit + "=" + l.row + ":" + std::to_string(l.quantity) + "x" + std::to_string(l.rate);
  return out;
}
std::string joined(const std::vector<std::string>& names) {
  std::string out;
  for (const auto& name : names) out += (out.empty() ? "" : " ") + name;
  return out;
}

// A unit the entry prices at the standard rates and not under the carried tier refuses the request, in the bound and
// the estimate, rather than pricing its tokens at the tier's other rates: here cache writes of a request with a mark.
void units_outside_the_tier() {
  constexpr std::uint64_t write = 1'250'000'000;
  const auto cached = catalogue_of("{\"version\":1,\"models\":[" +
      entry("vendor/cached", limits(batched_input, batched_output),
            {{"input", standard_in}, {"output", standard_out}, {"cache_write", write}, {"input/batch", batch_in}, {"output/batch", batch_out}}) + "," +
      entry("vendor/cached-batch", limits(batched_input, batched_output),
            {{"input", standard_in}, {"output", standard_out}, {"cache_write", write}, {"input/batch", batch_in}, {"output/batch", batch_out},
             {"cache_write/batch", write / 2}}) + "]}");
  auto messages_api = client_of(anthropic_messages());
  const auto marked = [](std::string model) {
    auto request = messages::Request::simple(std::move(model), "Say: alpha", 16);
    request.messages[0].parts[0] = Text{"Say: alpha", {}, CacheControl{}};
    return request;
  };
  const auto submit = messages_api.prepare(batches::Submit{{{"a-1", marked("vendor/cached")}, {"a-2", marked("vendor/cached-batch")}}});
  EXPECT(reasons_of(runtime::endpoint_worst_case_cost(ok(submit), *cached)),
         "TierPrice [a-1:vendor/cached] the entry prices cache_write at the standard rates but not under service tier 'batch'; ");
  EXPECT(describe(runtime::worst_case_cost(ok(submit), *cached)),
         "no total | TierPrice [a-1:vendor/cached] the entry prices cache_write at the standard rates but not under service tier 'batch' | entry '' "
         "model '' tokens 0/0/0 fees 0 invocations 0 attempts 0 tools 0 ceiling 0 pinned 0 pin 0");
  // The second request alone: its entry prices cache writes under the tier, so it is bounded at the dearest input rate.
  const auto priced_submit = messages_api.prepare(batches::Submit{{{"a-2", marked("vendor/cached-batch")}}});
  EXPECT(described(runtime::endpoint_worst_case_cost(ok(priced_submit), *cached)),
         "total " + std::to_string(rounded(batched_input * (write / 2) + 16 * batch_out)) + " | entry 'vendor/cached-batch' model '' attempts 1 lines "
         "a-2:nano_usd=vendor/cached-batch:" + std::to_string(rounded(batched_input * (write / 2) + 16 * batch_out)) + "x1000000");
  // The estimate: the line of the first request is unpriced, not priced with its cache writes at the batch input rate.
  auto usage = request_usage(100, 5);
  usage.cache_write = Count{40};
  const auto facts = ok(submit).endpoint_facts();
  EXPECT(estimated(runtime::batch_estimate(facts, anthropic_messages(), {answered("a-1", "vendor/cached", usage), answered("a-2", "vendor/cached-batch", usage)}, *cached)),
         "no total | unpriced a-1:tier | entry '' requested 0 lower 0 lines");
  const auto e = runtime::batch_estimate(facts, anthropic_messages(), {answered("a-2", "vendor/cached-batch", usage)}, *cached);
  EXPECT(e.total ? std::to_string(e.total->nano_usd) : "none", std::to_string(rounded(60 * batch_in + 40 * (write / 2) + 5 * batch_out)));
  report_mismatches("units_outside_the_tier");
  std::cout << "case passed: units outside the carried tier\n";
}

void batch_estimates() {
  auto messages_api = client_of(anthropic_messages());
  const auto family = anthropic_messages();
  const auto submit = messages_api.prepare(batches::Submit{{{"a-1", messages::Request::simple("vendor/batched", "Say: alpha", 16)},
                                                            {"a-2", messages::Request::simple("vendor/batched-fee", "Say: beta", 50)}}});
  const EndpointFacts facts = ok(submit).endpoint_facts();
  runtime::EndpointCostOptions measured_first;
  measured_first.measured_carried_input_tokens = {{"a-1", 300}};
  const auto bound = runtime::endpoint_worst_case_cost(ok(submit), *batch_priced, measured_first);
  // Each line priced as a chat call's usage under the carried tier and rounded on its own, the lines summed.
  const auto first = rounded(120 * batch_in + 7 * batch_out), second = rounded(900 * fee_in + 30 * fee_out + batch_fee);
  auto e = runtime::batch_estimate(facts, family, {answered("a-1", "vendor/batched", request_usage(120, 7)), answered("a-2", "vendor/batched-fee", request_usage(900, 30))},
                                   *batch_priced);
  EXPECT(estimated(e), "total " + std::to_string(first + second) + " | entry '' requested 0 lower 0 lines a-1:nano_usd=vendor/batched:" +
                           std::to_string(first) + "x1000000 a-2:nano_usd=vendor/batched-fee:" + std::to_string(second) + "x1000000");
  EXPECT(joined(pricing::beyond_bound(bound, e)), "");
  // At the requests' caps the estimate reaches the bound, and no further.
  e = runtime::batch_estimate(facts, family, {answered("a-1", "vendor/batched", request_usage(300, 16)), answered("a-2", "vendor/batched-fee", request_usage(fee_input, 50))},
                              *batch_priced);
  EXPECT(e.total ? std::to_string(e.total->nano_usd) : "none", std::to_string(nano(bound)));
  EXPECT(joined(pricing::beyond_bound(bound, e)), "");
  // Beyond its request (more input than the host measured): named by the request's line, and the total.
  e = runtime::batch_estimate(facts, family, {answered("a-1", "vendor/batched", request_usage(301, 16)), answered("a-2", "vendor/batched-fee", request_usage(fee_input, 50))},
                              *batch_priced);
  EXPECT(joined(pricing::beyond_bound(bound, e)), "a-1:nano_usd total");
  // A failed line adds what it reports, here nothing, and the provider may bill more.
  e = runtime::batch_estimate(facts, family, {answered("a-1", "vendor/batched", request_usage(120, 7)), failed_line("a-2")}, *batch_priced);
  EXPECT(estimated(e), "total " + std::to_string(first) + " | entry 'vendor/batched' requested 0 lower 1 lines a-1:nano_usd=vendor/batched:" +
                           std::to_string(first) + "x1000000");
  // One that reports usage is priced.
  Failure partial;
  partial.error = Error{ErrorKind::RemoteFailure, "the provider failed the batch request"};
  partial.partial.usage = request_usage(900, 30);
  partial.partial.model.requested = "vendor/batched-fee";
  e = runtime::batch_estimate(facts, family, {batches::Line{"a-2", partial}}, *batch_priced);
  EXPECT(estimated(e), "total " + std::to_string(second) + " | entry 'vendor/batched-fee' requested 0 lower 1 lines a-2:nano_usd=vendor/batched-fee:" +
                           std::to_string(second) + "x1000000");
  // Two lines of one request: refused, not priced twice.
  e = runtime::batch_estimate(facts, family, {answered("a-1", "vendor/batched", request_usage(120, 7)), answered("a-1", "vendor/batched", request_usage(120, 7))},
                              *batch_priced);
  EXPECT(estimated(e), "no total | unpriced a-1:duplicate | entry '' requested 0 lower 0 lines");
  // Unpriced: a line of a request the submit did not carry, a model without batch rows, and the names a line's
  // estimate leaves unpriced, each prefixed by its custom id; no lines and no partial total.
  e = runtime::batch_estimate(facts, family, {answered("a-1", "vendor/standard-only", request_usage(120, 7)), answered("x-9", "vendor/batched", request_usage(1, 1)),
                                              answered("a-2", "vendor/unknown", request_usage(1, 1))}, *batch_priced);
  EXPECT(estimated(e), "no total | unpriced a-1:tier | unpriced x-9:custom_id | unpriced a-2:model | entry '' requested 0 lower 0 lines");
  // The submit's facts decide first: a read is zero, and a submit without a carried tier, at another origin, or of
  // another family than the lines' prices nothing.
  EXPECT(estimated(runtime::batch_estimate(ok(messages_api.prepare(batches::Get{"msgbatch_1"})).endpoint_facts(), family,
                                           {answered("a-1", "vendor/batched", request_usage(120, 7))}, *batch_priced)),
         "total 0 | entry '' requested 0 lower 0 lines");
  auto untiered = facts;
  untiered.carried_tier.clear();
  EXPECT(estimated(runtime::batch_estimate(untiered, family, {}, *batch_priced)), "no total | unpriced tier | entry '' requested 0 lower 0 lines");
  EXPECT(estimated(runtime::batch_estimate(facts, google_generate(), {}, *batch_priced)),
         "no total | unpriced origin | unpriced family | entry '' requested 0 lower 0 lines");
  auto unstated = facts;
  unstated.billing = descriptor::EndpointBilling::Unstated;
  EXPECT(estimated(runtime::batch_estimate(unstated, family, {}, *batch_priced)), "no total | unpriced billing | entry '' requested 0 lower 0 lines");
  // An OpenAI submit carries no inline requests: its lines are priced by the family that wrote the file.
  auto chat_api = client_of(openai_chat());
  const auto file_facts = ok(chat_api.prepare(batches::Submit{{}, "file-abc", "/v1/chat/completions"})).endpoint_facts();
  e = runtime::batch_estimate(file_facts, openai_chat(), {answered("o-1", "vendor/batched", request_usage(120, 7))}, *batch_priced);
  EXPECT(estimated(e), "total " + std::to_string(first) + " | entry 'vendor/batched' requested 0 lower 0 lines o-1:nano_usd=vendor/batched:" +
                           std::to_string(first) + "x1000000");
  // With the items the file was written from, a line of no item is refused, and no items prices nothing.
  const std::vector<batches::Item> file_items{{"o-1", chat::Request::simple("vendor/batched", "Say: alpha", 16)}};
  e = runtime::batch_estimate(file_facts, openai_chat(), file_items, {answered("o-1", "vendor/batched", request_usage(120, 7))}, *batch_priced);
  EXPECT(e.total ? std::to_string(e.total->nano_usd) : "none", std::to_string(first));
  e = runtime::batch_estimate(file_facts, openai_chat(), file_items,
                              {answered("o-1", "vendor/batched", request_usage(120, 7)), answered("o-9", "vendor/batched", request_usage(1, 1))}, *batch_priced);
  EXPECT(estimated(e), "no total | unpriced o-9:custom_id | entry '' requested 0 lower 0 lines");
  EXPECT(estimated(runtime::batch_estimate(file_facts, openai_chat(), std::vector<batches::Item>{}, {}, *batch_priced)),
         "no total | unpriced items | entry '' requested 0 lower 0 lines");
  report_mismatches("batch_estimates");
  std::cout << "case passed: the estimate of a batch from its lines\n";
}

// For result lines within their requests' caps, priced by the entries the bound used, the estimate of a batch never
// exceeds its bound: generated catalogues (batch rows with and without bands, standard rows, declared zeros, models
// without batch rows), requests with caps and measured inputs, inline (Anthropic) and from an uploaded file (OpenAI),
// and lines within each request, some of them failed.
void batch_estimate_within_the_bound() {
  std::uint64_t seed = 20261120;
  auto next = [&](std::uint64_t limit) { seed = seed * 6364136223846793005u + 1442695040888963407u; return (seed >> 33) % (limit + 1); };
  const auto messages_family = anthropic_messages();
  const auto chat_family = openai_chat();
  auto messages_api = client_of(anthropic_messages());
  auto chat_api = client_of(openai_chat());
  std::size_t rounds = 0, both = 0, bound_only = 0, estimate_only = 0, neither = 0, tight = 0, both_inline = 0, both_file = 0;
  const char* const units[]{"input", "cache_read", "cache_write", "output", "reasoning", "request"};
  for (; rounds < 2000; ++rounds) {
    // The models: limits, and rows under the carried tier and the standard rates.
    const std::size_t models = 1 + next(2);
    std::vector<std::uint64_t> input_limits;
    std::string json = "{\"version\":1,\"models\":[";
    for (std::size_t m = 0; m < models; ++m) {
      const auto limit = 1 + next(4000);
      input_limits.push_back(limit);
      std::vector<std::pair<std::string, std::uint64_t>> rows;
      const bool batch_rows = next(9) != 0;
      for (const auto* unit : units) {
        const bool base = std::string_view(unit) == "input" || std::string_view(unit) == "output";
        const auto rate = [&] { return next(4) == 0 ? std::uint64_t{0} : (1 + next(4999)) * 1'000'000 + next(999); };
        // A unit priced at the standard rates mostly has a batch row too: one without refuses the request.
        const bool standard = next(5) != 0;
        if (standard) rows.emplace_back(unit, rate());
        if (batch_rows && (base || standard ? next(19) != 0 : next(2) == 0)) {
          rows.emplace_back(std::string(unit) + "/batch", rate());
          if (next(3) == 0) rows.emplace_back(std::string(unit) + "/batch+" + std::to_string(1 + next(limit)), rate());
        }
      }
      json += (m ? "," : "") + entry("vendor/p" + std::to_string(m), limits(limit), rows);
    }
    const auto catalogue = catalogue_of(json + "]}");
    // The requests, and the lines within them.
    const bool from_file = next(2) == 0;
    const std::size_t count = 1 + next(3);
    std::vector<batches::Item> items;
    std::vector<std::uint64_t> caps, models_of;
    runtime::EndpointCostOptions options;
    std::vector<batches::Line> lines;
    for (std::size_t i = 0; i < count; ++i) {
      const auto id = "r-" + std::to_string(i);
      const auto m = next(models - 1);
      const auto cap = 1 + next(300);
      const auto model = "vendor/p" + std::to_string(m);
      if (from_file) items.push_back({id, chat::Request::simple(model, "Say: alpha", cap)});
      else items.push_back({id, messages::Request::simple(model, "Say: alpha", cap)});
      std::uint64_t input_bound = input_limits[m];
      if (next(2) == 0) {
        const auto counted_tokens = next(input_limits[m] + 50);
        options.measured_carried_input_tokens.emplace_back(id, counted_tokens);
        input_bound = (std::min)(input_bound, counted_tokens);
      }
      if (next(9) == 0) {
        lines.push_back(failed_line(id));
        continue;
      }
      const auto input = next(input_bound), output = next(cap);
      lines.push_back(answered(id, model, request_usage(input, output, next(input), next(output))));
    }
    pricing::EndpointBound bound;
    EndpointFacts facts;
    if (from_file) {
      const auto text = std::get<std::string>(batches::jsonl(chat_family, items));
      const auto submit = chat_api.prepare(batches::Submit{{}, "file-p", "/v1/chat/completions"});
      facts = ok(submit).endpoint_facts();
      bound = runtime::batch_worst_case_cost(ok(submit), chat_family, items, uploaded("file-p", text), *catalogue, options);
    } else {
      const auto submit = messages_api.prepare(batches::Submit{items});
      facts = ok(submit).endpoint_facts();
      bound = runtime::endpoint_worst_case_cost(ok(submit), *catalogue, options);
    }
    const auto estimate = runtime::batch_estimate(facts, from_file ? chat_family : messages_family, lines, *catalogue);
    if (bound.total && estimate.total) {
      ++both;
      ++(from_file ? both_file : both_inline);
      if (estimate.total->nano_usd > bound.total->nano_usd)
        throw std::runtime_error("round " + std::to_string(rounds) + ": estimate " + estimated(estimate) + " above bound " + described(bound));
      const auto beyond = pricing::beyond_bound(bound, estimate);
      if (std::find(beyond.begin(), beyond.end(), "total") != beyond.end()) throw std::runtime_error("round " + std::to_string(rounds) + ": beyond the total");
      if (estimate.total->nano_usd == bound.total->nano_usd) ++tight;
    } else if (bound.total) ++bound_only;
    else if (estimate.total) ++estimate_only;
    else ++neither;
    // Every reason of a bound names the request it is about, or the submit.
    for (const auto& reason : bound.unbounded)
      if (!reason.subject.starts_with("r-") && reason.subject != "batches/submit")
        throw std::runtime_error("round " + std::to_string(rounds) + ": a reason names no request: " + reason.subject);
  }
  // The property holds over a fair share of priced calls.
  if (both < rounds / 4 || both_inline < rounds / 10 || both_file < rounds / 10)
    throw std::runtime_error("too few batches priced both ways: " + std::to_string(both_inline) + " inline, " + std::to_string(both_file) + " from a file");
  std::cout << "case passed: batch estimates within the bound (" << both << " both, " << bound_only << " bound only, " << estimate_only
            << " estimate only, " << neither << " neither, " << tight << " at the bound)\n";
}

// ---- extension values that change the price: a tier's rows, or no bound --------------------------------------

// Fast rates dearer than the standard ones, each with a fraction of a nano-USD per token so a bound rounds up.
constexpr std::uint64_t fast_in = 2'000'000'001, fast_out = 8'000'000'003;
const auto fast_priced = catalogue_of("{\"version\":1,\"models\":[" +
    entry("vendor/fast", limits(batched_input, batched_output),
          {{"input", standard_in}, {"output", standard_out}, {"input/fast", fast_in}, {"output/fast", fast_out}, {"input/batch", batch_in},
           {"output/batch", batch_out}}) + "," +
    // No fast rows: nothing prices a fast request.
    entry("vendor/no-fast", limits(batched_input, batched_output),
          {{"input", standard_in}, {"output", standard_out}, {"input/batch", batch_in}, {"output/batch", batch_out}}) + "]}");
// The built-in policy with each Messages extension fact written by `edit` (its text; empty keeps the fact as it is).
// The built-in policy with `member` of `family` set to `value`, a JSON text.
std::string family_member_with(std::string_view name, std::string_view member, std::string_view value) {
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  const auto root = std::get<json::Document>(parsed).root();
  std::string out = "{";
  for (auto top : root.members()) {
    out += (out.size() > 1 ? "," : "") + json::quote(top.key) + ":";
    if (top.key != "families") { out += top.value.dump(); continue; }
    std::string families = "[";
    for (auto family : top.value.elements()) {
      if (families.size() > 1) families += ",";
      if (family.get("family").as_string() != name) { families += family.dump(); continue; }
      std::string object = "{";
      for (auto field : family.members())
        if (field.key != member) object += (object.size() > 1 ? "," : "") + json::quote(field.key) + ":" + field.value.dump();
      families += object + "," + json::quote(member) + ":" + std::string(value) + "}";
    }
    out += families + "]";
  }
  return out + "}";
}
std::string messages_extensions_with(const std::function<std::string(json::Value)>& edit) {
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  const auto root = std::get<json::Document>(parsed).root();
  std::string out = "{";
  for (auto top : root.members()) {
    out += (out.size() > 1 ? "," : "") + json::quote(top.key) + ":";
    if (top.key != "families") { out += top.value.dump(); continue; }
    std::string families = "[";
    for (auto family : top.value.elements()) {
      if (families.size() > 1) families += ",";
      if (family.get("family").as_string() != "anthropic.messages") { families += family.dump(); continue; }
      std::string object = "{";
      for (auto field : family.members()) {
        object += (object.size() > 1 ? "," : "") + json::quote(field.key) + ":";
        if (field.key != "extensions") { object += field.value.dump(); continue; }
        std::string facts = "[";
        for (auto fact : field.value.elements()) {
          const auto text = edit(fact);
          facts += (facts.size() > 1 ? "," : "") + (text.empty() ? fact.dump() : text);
        }
        object += facts + "]";
      }
      families += object + "}";
    }
    out += families + "]";
  }
  return out + "}";
}
std::optional<descriptor::ValidatedDescriptor> anthropic_messages_with(const std::string& policy_text, std::string& refusal) {
  auto loaded = descriptor::load_policy(policy_text, config_defaults::codec_defaults_json);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded)) {
    refusal = "policy refused at " + error->pointer + ": " + error->expected;
    return std::nullopt;
  }
  descriptor::presets::PresetOptions options;
  options.policy = std::get<descriptor::PolicySnapshot>(std::move(loaded));
  auto made = descriptor::presets::try_anthropic_messages(options);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&made)) {
    refusal = "descriptor refused at " + error->pointer;
    return std::nullopt;
  }
  return std::get<descriptor::ValidatedDescriptor>(std::move(made));
}
messages::Request extended(std::string model, std::string_view extensions) {
  auto request = messages::Request::simple(std::move(model), "Say: alpha", 16);
  if (!extensions.empty()) request.extensions = document(extensions);
  return request;
}
// A bound as its total, or as its reasons.
std::string bounded_as(const std::optional<UsdAmount>& total, const std::vector<pricing::UnboundedReason>& unbounded) {
  if (total) return "total " + std::to_string(total->nano_usd);
  std::string out = "no total";
  for (const auto& reason : unbounded) out += " | " + std::string(pricing::to_string(reason.by)) + " [" + reason.subject + "] " + reason.detail;
  return out;
}
// An estimate as its total, or as the names it leaves unpriced.
std::string estimated_as(const pricing::EndpointEstimate& e) {
  if (e.total) return "total " + std::to_string(e.total->nano_usd);
  std::string out = "no total";
  for (const auto& name : e.unpriced) out += " | unpriced " + name;
  return out;
}

void priced_extensions() {
  constexpr std::string_view unmodeled_detail = "the extension's value changes the price in a way no catalogue row states";
  const auto standard = "total " + std::to_string(rounded(300 * standard_in + 16 * standard_out));
  const auto fast = "total " + std::to_string(rounded(300 * fast_in + 16 * fast_out));
  const auto unmodeled_geo = "no total | Unmodeled [inference_geo] " + std::string(unmodeled_detail);
  // A chat request: `speed: fast` under the fast rows, `inference_geo` other than `global` without a bound; a value the
  // policy does not price, and null, at the standard rates.
  const auto chat_bound = [&](runtime::Client& client, std::string model, std::string_view extensions) {
    auto prepared = client.prepare(extended(std::move(model), extensions), run_with(1));
    if (!prepared.valid() || prepared.error()) return "did not prepare: " + (prepared.error() ? prepared.error()->safe_message : std::string("invalid"));
    const auto bound = runtime::worst_case_cost(prepared, *fast_priced, runtime::WorstCaseOptions{300});
    return bounded_as(bound.total, bound.unbounded);
  };
  auto messages_api = client_of(anthropic_messages());
  EXPECT(chat_bound(messages_api, "vendor/fast", ""), standard);
  EXPECT(chat_bound(messages_api, "vendor/fast", R"({"speed":"standard"})"), standard);
  EXPECT(chat_bound(messages_api, "vendor/fast", R"({"speed":null})"), standard);
  EXPECT(chat_bound(messages_api, "vendor/fast", R"({"speed":"fast"})"), fast);
  EXPECT(chat_bound(messages_api, "vendor/no-fast", R"({"speed":"fast"})"),
         "no total | TierPrice [vendor/no-fast] the entry prices no unit under service tier 'fast'");
  EXPECT(chat_bound(messages_api, "vendor/fast", R"({"inference_geo":"us"})"), unmodeled_geo);
  EXPECT(chat_bound(messages_api, "vendor/fast", R"({"inference_geo":"eu"})"), unmodeled_geo);
  EXPECT(chat_bound(messages_api, "vendor/fast", R"({"inference_geo":"global"})"), standard);
  EXPECT(chat_bound(messages_api, "vendor/fast", R"({"inference_geo":null})"), standard);
  EXPECT(chat_bound(messages_api, "vendor/fast", R"({"speed":"fast","inference_geo":"us"})"), unmodeled_geo);
  EXPECT(chat_bound(messages_api, "vendor/fast", R"({"speed":"fast","inference_geo":"global"})"), fast);
  // The prepared facts name each priced extension; a value at the standard rates is left out.
  {
    const auto prepared = messages_api.prepare(extended("vendor/fast", R"({"speed":"fast","inference_geo":"us","diagnostics":null})"), run_with(1));
    std::string named;
    for (const auto& priced : prepared.facts().priced_extensions) named += priced.key + "/" + priced.tier + "/" + std::to_string(priced.unmodeled) + " ";
    EXPECT(named, "speed/fast/0 inference_geo//1 ");
    EXPECT(std::to_string(messages_api.prepare(extended("vendor/fast", R"({"speed":"standard","inference_geo":"global"})"), run_with(1)).facts().priced_extensions.size()), "0");
  }

  // The estimate of a chat call, from the facts it was prepared with.
  const auto chat_estimate = [&](std::string_view extensions) {
    const auto prepared = messages_api.prepare(extended("vendor/fast", extensions), run_with(1));
    ServedModel served;
    served.requested = "vendor/fast";
    const auto& facts = prepared.facts();
    const auto e = pricing::estimate(*fast_priced, prepared.family(), prepared.descriptor()->base_url(), served, request_usage(120, 7), facts,
                                     prepared.descriptor()->policy()->billing_tier(prepared.family(), facts.service_tier));
    if (e.total) return "total " + std::to_string(e.total->nano_usd);
    std::string out = "no total";
    for (const auto& name : e.unpriced) out += " | unpriced " + name;
    return out;
  };
  const auto standard_estimate = "total " + std::to_string(rounded(120 * standard_in + 7 * standard_out));
  EXPECT(chat_estimate(""), standard_estimate);
  EXPECT(chat_estimate(R"({"speed":"standard"})"), standard_estimate);
  EXPECT(chat_estimate(R"({"speed":"fast"})"), "total " + std::to_string(rounded(120 * fast_in + 7 * fast_out)));
  EXPECT(chat_estimate(R"({"inference_geo":"us"})"), "no total | unpriced extension:inference_geo");
  EXPECT(chat_estimate(R"({"inference_geo":"global"})"), standard_estimate);

  // Two tiers, a request's and an extension's or two extensions': no bound and no estimate. The same tier twice is it.
  {
    RequestFacts facts;
    facts.priced_extensions = {{"speed", "fast", false}};
    pricing::WorstCaseInput in;
    in.family = "anthropic.messages";
    in.origin = "https://api.anthropic.com";
    in.model = "vendor/fast";
    in.facts = &facts;
    in.output_tokens = 16;
    in.measured_input_tokens = 300;
    in.invocations = 1;
    in.attempts = 1;
    const auto bound = [&](std::string_view tier) {
      in.service_tier = tier;
      const auto wc = pricing::worst_case(in, *fast_priced);
      return bounded_as(wc.total, wc.unbounded);
    };
    EXPECT(bound("priority"), "no total | TierPrice [speed] the extension's value bills under service tier 'fast' and the request under service tier 'priority'");
    EXPECT(bound("fast"), fast);
    EXPECT(bound(""), fast);
    ServedModel served;
    served.requested = "vendor/fast";
    const auto e = pricing::estimate(*fast_priced, in.family, in.origin, served, request_usage(120, 7), facts, "priority");
    EXPECT(e.total ? std::string("total") : e.unpriced.empty() ? std::string("none") : e.unpriced.front(), "tier");
    facts.priced_extensions.push_back({"other", "priority", false});
    EXPECT(bound(""), "no total | TierPrice [other] the extension's value bills under service tier 'priority' and the request under service tier 'fast'");
    facts.priced_extensions.back().tier = "fast";
    EXPECT(bound(""), fast);
  }

  // A batch item: the batch tier is not the fast tier, and nothing states how the two combine; an unmodeled value has
  // no bound there either. The estimate of the item's line likewise, from the submit's facts and from the items.
  const auto batched = "total " + std::to_string(rounded(batched_input * batch_in + 16 * batch_out));
  const auto line_total = "total " + std::to_string(rounded(120 * batch_in + 7 * batch_out));
  const auto batch_of = [&](std::string_view extensions) {
    const auto submit = messages_api.prepare(batches::Submit{{{"a-1", extended("vendor/fast", extensions)}}});
    const auto bound = runtime::endpoint_worst_case_cost(ok(submit), *fast_priced);
    const auto line = answered("a-1", "vendor/fast", request_usage(120, 7));
    const auto& facts = ok(submit).endpoint_facts();
    return bounded_as(bound.total, bound.unbounded) + " / " + estimated_as(runtime::batch_estimate(facts, anthropic_messages(), {line}, *fast_priced)) +
           " / " + estimated_as(runtime::batch_estimate(facts, anthropic_messages(), {batches::Item{"a-1", extended("vendor/fast", extensions)}}, {line}, *fast_priced));
  };
  EXPECT(batch_of(""), batched + " / " + line_total + " / " + line_total);
  EXPECT(batch_of(R"({"speed":"standard"})"), batched + " / " + line_total + " / " + line_total);
  EXPECT(batch_of(R"({"speed":"fast"})"),
         "no total | TierPrice [a-1:speed] the extension's value bills under service tier 'fast' and the request under service tier 'batch' / "
         "no total | unpriced a-1:tier / no total | unpriced a-1:tier");
  EXPECT(batch_of(R"({"inference_geo":"us"})"), "no total | Unmodeled [a-1:inference_geo] " + std::string(unmodeled_detail) +
                                                    " / no total | unpriced a-1:extension:inference_geo / no total | unpriced a-1:extension:inference_geo");
  EXPECT(batch_of(R"({"inference_geo":"global"})"), batched + " / " + line_total + " / " + line_total);

  // A policy whose facts leave `prices` out takes the embedded policy's; one that states none (`{}`) prices no value.
  const auto chat_bound_under = [&](const std::string& policy_text, std::string_view extensions) {
    std::string refusal;
    auto descriptor = anthropic_messages_with(policy_text, refusal);
    if (!descriptor) return refusal;
    auto client = client_of(std::move(*descriptor));
    return chat_bound(client, "vendor/fast", extensions);
  };
  const auto left_out = messages_extensions_with([](json::Value fact) { return fact_text(fact, {"prices"}); });
  EXPECT(chat_bound_under(left_out, R"({"speed":"fast"})"), fast);
  EXPECT(chat_bound_under(left_out, R"({"inference_geo":"us"})"), unmodeled_geo);
  const auto none_for_speed = messages_extensions_with([](json::Value fact) {
    if (fact.get("key").as_string() != "speed") return std::string();
    auto text = fact_text(fact, {"prices"});
    text.pop_back();
    return text + ",\"prices\":{}}";
  });
  EXPECT(chat_bound_under(none_for_speed, R"({"speed":"fast"})"), standard);
  EXPECT(chat_bound_under(none_for_speed, R"({"inference_geo":"us"})"), unmodeled_geo);

  // Under a tier, a unit the entry prices at the standard rates and not in the tier: no bound and no estimate, whether
  // the tier is an extension's or the request's own (the rule a batch's carried requests already follow).
  const auto cached = catalogue_of("{\"version\":1,\"models\":[" +
      entry("vendor/fast-cached", limits(batched_input, batched_output),
            {{"input", standard_in}, {"output", standard_out}, {"cache_write", 1'250'000'000}, {"cache_write_1h", 2'000'000'000},
             {"input/fast", fast_in}, {"output/fast", fast_out}}) + "]}");
  {
    auto request = extended("vendor/fast-cached", R"({"speed":"fast"})");
    request.messages[0].parts[0] = Text{"Say: alpha", {}, CacheControl{}};
    const auto prepared = messages_api.prepare(request, run_with(1));
    const std::string outside = "no total | TierPrice [vendor/fast-cached] the entry prices cache_write at the standard rates but not under service tier 'fast'";
    const auto bound = runtime::worst_case_cost(prepared, *cached, runtime::WorstCaseOptions{300});
    EXPECT(bounded_as(bound.total, bound.unbounded), outside);
    ServedModel served;
    served.requested = "vendor/fast-cached";
    auto usage = request_usage(120, 7);
    usage.cache_write = Count{40};
    const auto first_unpriced = [](const pricing::CostEstimate& e) {
      return e.total ? std::string("total") : e.unpriced.empty() ? std::string("none") : e.unpriced.front();
    };
    EXPECT(first_unpriced(pricing::estimate(*cached, prepared.family(), prepared.descriptor()->base_url(), served, usage, prepared.facts(), "")), "tier");
    RequestFacts none;
    pricing::WorstCaseInput in;
    in.family = "anthropic.messages";
    in.origin = "https://api.anthropic.com";
    in.model = "vendor/fast-cached";
    in.facts = &none;
    in.service_tier = "fast";
    in.output_tokens = 16;
    in.measured_input_tokens = 300;
    in.invocations = 1;
    in.attempts = 1;
    const auto by_tier = pricing::worst_case(in, *cached);
    EXPECT(bounded_as(by_tier.total, by_tier.unbounded), outside);
    EXPECT(first_unpriced(pricing::estimate(*cached, in.family, in.origin, served, usage, "fast")), "tier");
    // At the standard rates nothing changes.
    EXPECT(first_unpriced(pricing::estimate(*cached, in.family, in.origin, served, usage)), "total");
  }

  // A Chat extension a custom policy prices: the prepared facts name it and the bound prices under its tier; an
  // OpenAI file batch's lines are estimated only from its items, and two items of one custom id are refused.
  const auto chat_policy = load(family_member_with("openai.chat", "extensions",
      R"([{"key":"fixture_speed","origins":["https://api.openai.com"],"value":{"type":"enum","values":["slow","quick"]},"replay_eligible":true,)"
      R"("prices":{"values":{"quick":{"tier":"priority"}}}}])"));
  const auto chat_family = openai_chat(chat_policy);
  auto chat_api = client_of(openai_chat(chat_policy));
  const auto chat_priced = catalogue_of("{\"version\":1,\"models\":[" +
      entry("vendor/quick", limits(batched_input, batched_output),
            {{"input", standard_in}, {"output", standard_out}, {"input/priority", fast_in}, {"output/priority", fast_out}, {"input/batch", batch_in},
             {"output/batch", batch_out}}) + "]}");
  const auto chat_request = [](std::string_view speed) {
    auto request = chat::Request::simple("vendor/quick", "Say: alpha", 16);
    request.extensions = document("{\"fixture_speed\":\"" + std::string(speed) + "\"}");
    return request;
  };
  {
    const auto quick = chat_api.prepare(chat_request("quick"), run_with(1));
    std::string named;
    for (const auto& priced : quick.facts().priced_extensions) named += priced.key + "/" + priced.tier + "/" + std::to_string(priced.unmodeled) + " ";
    EXPECT(named, "fixture_speed/priority/0 ");
    auto bound = runtime::worst_case_cost(quick, *chat_priced, runtime::WorstCaseOptions{300});
    EXPECT(bounded_as(bound.total, bound.unbounded), fast);
    bound = runtime::worst_case_cost(chat_api.prepare(chat_request("slow"), run_with(1)), *chat_priced, runtime::WorstCaseOptions{300});
    EXPECT(bounded_as(bound.total, bound.unbounded), standard);
  }
  {
    const auto submit = chat_api.prepare(batches::Submit{{}, "file-abc", "/v1/chat/completions"});
    const auto& facts = ok(submit).endpoint_facts();
    const auto line = answered("o-1", "vendor/quick", request_usage(120, 7));
    EXPECT(estimated_as(runtime::batch_estimate(facts, chat_family, {line}, *chat_priced)), "no total | unpriced extensions");
    EXPECT(estimated_as(runtime::batch_estimate(facts, chat_family, {batches::Item{"o-1", chat_request("quick")}}, {line}, *chat_priced)),
           "no total | unpriced o-1:tier");
    EXPECT(estimated_as(runtime::batch_estimate(facts, chat_family, {batches::Item{"o-1", chat_request("slow")}}, {line}, *chat_priced)), line_total);
    EXPECT(estimated_as(runtime::batch_estimate(facts, chat_family, {batches::Item{"o-1", chat_request("slow")}, batches::Item{"o-1", chat_request("quick")}},
                                                {line}, *chat_priced)),
           "no total | unpriced o-1:duplicate");
    // The built-in Chat policy prices no extension: the lines are estimated without the items, as before.
    const auto builtin_submit = client_of(openai_chat()).prepare(batches::Submit{{}, "file-abc", "/v1/chat/completions"});
    EXPECT(estimated_as(runtime::batch_estimate(ok(builtin_submit).endpoint_facts(), openai_chat(), {line}, *chat_priced)), line_total);
  }
  // Two carried requests of one custom id in a submit's facts.
  {
    const auto submit = messages_api.prepare(batches::Submit{{{"a-1", extended("vendor/fast", "")}}});
    auto facts = ok(submit).endpoint_facts();
    facts.carried.push_back(facts.carried.front());
    EXPECT(estimated_as(runtime::batch_estimate(facts, anthropic_messages(), {answered("a-1", "vendor/fast", request_usage(120, 7))}, *fast_priced)),
           "no total | unpriced a-1:duplicate");
  }
  report_mismatches("priced_extensions");
  std::cout << "case passed: extension values that change the price\n";
}
}  // namespace

int main() {
  try {
    // The bounds first, so a wrong count shows as a wrong amount.
    images_bound();
    refusal_subjects();
    repeated_keys();
    unstated_members();
    decisions_bound();
    embeddings_bound();
    rerank_bound();
    speech_bound();
    transcription_bound();
    videos_bound();
    gateway_embeddings_bound();
    gateway_refusals();
    no_endpoint_names_a_tier();
    answers_per_billing();
    stated_facts();
    estimates_per_result();
    batches_bound();
    openai_batch_bound();
    units_outside_the_tier();
    batch_estimates();
    batch_estimate_within_the_bound();
    priced_extensions();
    std::cout << "endpoint cost passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "endpoint cost test failed: " << error.what() << '\n';
    return 1;
  }
}
