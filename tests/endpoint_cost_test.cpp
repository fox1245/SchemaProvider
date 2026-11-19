// The cost bound of a prepared non-chat endpoint call (runtime::endpoint_worst_case_cost and runtime::worst_case_cost):
// what each billing value of the policy answers; the quantities and dimensions each request type states; and, per
// endpoint kind, the bound of a call prepared through a client, against amounts computed here with integers from a
// small catalogue written here, with every refusal a request can cause.
#include "core/enum_names.h"
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
  // Carried: a batch submit bills the requests it carries, which are not priced yet.
  auto messages_api = client_of(anthropic_messages());
  CHECK(describe(runtime::worst_case_cost(ok(messages_api.prepare(batches::Submit{{{"a-1", messages::Request::simple("vendor/decider", "Say: alpha", 16)}}})), *priced)) ==
        "no total | Unmodeled [batches] an endpoint call has no catalogue bound | entry '' model '' tokens 0/0/0 fees 0 invocations 0 attempts 0 tools 0 "
        "ceiling 0 pinned 0 pin 0");

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
    answers_per_billing();
    stated_facts();
    std::cout << "endpoint cost passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "endpoint cost test failed: " << error.what() << '\n';
    return 1;
  }
}
