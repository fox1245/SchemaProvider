// Provider limits are policy data (#144, item 7): how many MCP servers, container skills, transcription terms and batch
// items a request may carry, how long a document title may be, and the audio sample and bit rates. A policy that does not
// state a limit takes the embedded policy's value, and its content digest covers what it takes.
#include "codecs/batches.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "codecs/messages_request.h"
#include "codecs/reason.h"
#include "crypto/crypto.h"
#include "descriptor/policy.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include "sp/config_defaults.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace sp;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)
// The constant of a source whose text is `text` has `code`.
bool listed(std::span<const reason::Reason> source, std::string_view text, std::string_view code) {
  return std::any_of(source.begin(), source.end(), [&](const reason::Reason& r) { return r.message == text && r.code == code; });
}

std::string policy_text() { return std::string(config_defaults::descriptor_policy_json); }
descriptor::PolicySnapshot load(const std::string& text) {
  auto loaded = descriptor::load_policy(text, config_defaults::codec_defaults_json);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded))
    throw std::runtime_error("policy refused at " + error->pointer + ": " + error->expected);
  return std::get<descriptor::PolicySnapshot>(std::move(loaded));
}
bool refused_at(const std::string& text, std::string_view pointer) {
  const auto loaded = descriptor::load_policy(text, config_defaults::codec_defaults_json);
  const auto* error = std::get_if<descriptor::ConfigError>(&loaded);
  return error && error->pointer == pointer;
}

// ---- policy text edits -------------------------------------------------------------------------------------------

// `entry` with `key` set to `raw` (replaced or added), or removed when `raw` is empty.
std::string with_member(json::Value entry, std::string_view key, std::string_view raw) {
  std::string out = "{";
  bool present = false;
  for (auto member : entry.members()) {
    if (member.key == key) {
      present = true;
      if (raw.empty()) continue;
    }
    if (out.size() > 1) out += ',';
    out += json::quote(member.key) + ':' + (member.key == key ? std::string(raw) : member.value.dump());
  }
  if (!present && !raw.empty()) out += std::string(out.size() > 1 ? "," : "") + json::quote(key) + ':' + std::string(raw);
  return out + '}';
}
// The policy text with `edit` applied to the array `array` of the policy, entry by entry.
template <class Edit> std::string edited_array(std::string_view text, std::string_view array, Edit edit) {
  auto parsed = json::parse(text);
  const auto root = std::get<json::Document>(parsed).root();
  std::string entries = "[";
  for (auto entry : root.get(array).elements()) {
    if (entries.size() > 1) entries += ',';
    entries += edit(entry);
  }
  entries += ']';
  std::string out = "{";
  for (auto member : root.members()) {
    if (out.size() > 1) out += ',';
    out += json::quote(member.key) + ':' + (member.key == array ? entries : member.value.dump());
  }
  return out + '}';
}
// A member of a family (every family when `family` is empty) set to `raw`, or removed when `raw` is empty.
std::string edited(std::string_view text, std::string_view family, std::string_view key, std::string_view raw) {
  return edited_array(text, "families", [&](json::Value entry) {
    return family.empty() || entry.get("family").as_string() == family ? with_member(entry, key, raw) : entry.dump();
  });
}
// A member of the endpoint fact of `endpoint` and `shape` (every fact when `endpoint` is empty).
std::string edited_endpoint(std::string_view text, std::string_view endpoint, std::string_view shape, std::string_view key, std::string_view raw) {
  return edited_array(text, "endpoints", [&](json::Value entry) {
    return endpoint.empty() || (entry.get("endpoint").as_string() == endpoint && entry.get("shape").as_string() == shape) ? with_member(entry, key, raw) : entry.dump();
  });
}
// A policy written before the limits existed.
std::string without_limits(std::string_view text) { return edited_endpoint(edited(text, "", "limits", ""), "", "", "max_items", ""); }

// ---- the embedded policy's limits --------------------------------------------------------------------------------

struct LimitName { std::string_view family, name; };
constexpr LimitName family_limits[]{
    {"anthropic.messages", "document_title_chars"}, {"anthropic.messages", "mcp_servers"}, {"anthropic.messages", "container_skills"},
    {"google.generate", "transcription_vocabulary"}, {"google.generate", "audio_sample_rate"}, {"google.generate", "audio_bit_rate"},
    {"google.interactions", "transcription_vocabulary"}};
constexpr std::string_view batch_shapes[]{"anthropic", "google"};
constexpr std::string_view families[]{"openai.chat", "anthropic.messages", "openai.responses", "google.generate", "google.interactions"};

json::Document embedded() {
  auto parsed = json::parse(config_defaults::descriptor_policy_json, {config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth});
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::get<json::Document>(std::move(parsed));
}
std::uint64_t embedded_limit(std::string_view family, std::string_view name) {
  const auto document = embedded();
  for (auto entry : document.root().get("families").elements())
    if (entry.get("family").as_string() == family) return entry.get("limits").get(name).as_uint();
  throw std::runtime_error("no such family");
}
std::uint64_t embedded_max_items(std::string_view shape) {
  const auto document = embedded();
  for (auto entry : document.root().get("endpoints").elements())
    if (entry.get("endpoint").as_string() == "batches" && entry.get("shape").as_string() == shape) return entry.get("max_items").as_uint();
  throw std::runtime_error("no such batch fact");
}

// ---- digest forms ------------------------------------------------------------------------------------------------

std::string hex(const crypto::Sha256Digest& digest) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : digest) { out += digits[byte >> 4]; out += digits[byte & 15]; }
  return out;
}
// The content digest of a policy text: SHA-256 over the parsed policy pair and the catalogue, and the borrowed values
// behind their marker when there are any.
std::string plain_digest(std::string_view text, std::string_view borrowed = {}) {
  const json::Limits limits{config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth};
  auto family = json::parse(text, limits), resources = json::parse(config_defaults::codec_defaults_json, limits);
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
// One borrowed value: where it belongs (a family, or `/endpoints/batches/<shape>`), the member (`limits.<name>`, or
// `max_items`) and the embedded policy's value as canonical JSON.
struct Borrowed { std::string where, member, value; };
Borrowed borrowed_family_limit(const LimitName& limit) {
  return {std::string(limit.family), "limits." + std::string(limit.name), std::to_string(embedded_limit(limit.family, limit.name))};
}
Borrowed borrowed_batch_limit(std::string_view shape) {
  return {"/endpoints/batches/" + std::string(shape), "max_items", std::to_string(embedded_max_items(shape))};
}
// The form the digest takes them in: sorted, each text with its length.
std::string borrowed_form(std::vector<Borrowed> values) {
  std::sort(values.begin(), values.end(), [](const Borrowed& a, const Borrowed& b) { return std::tie(a.where, a.member, a.value) < std::tie(b.where, b.member, b.value); });
  std::string out;
  for (const auto& value : values)
    for (const auto* text : {&value.where, &value.member, &value.value}) out += std::to_string(text->size()) + ":" + *text;
  return out;
}
std::vector<Borrowed> every_limit() {
  std::vector<Borrowed> all;
  for (const auto& limit : family_limits) all.push_back(borrowed_family_limit(limit));
  for (const auto shape : batch_shapes) all.push_back(borrowed_batch_limit(shape));
  return all;
}

// ---- requests ----------------------------------------------------------------------------------------------------

descriptor::ValidatedDescriptor descriptor_of(std::string_view family, descriptor::PolicySnapshot policy = {}) {
  namespace presets = descriptor::presets;
  presets::PresetOptions options;
  options.policy = std::move(policy);
  auto loaded = family == "anthropic.messages" ? presets::try_anthropic_messages(options)
      : family == "google.generate" ? presets::try_google_generate(options)
      : presets::try_google_interactions(options);
  if (auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("descriptor refused: " + error->pointer);
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::shared_ptr<const json::Document> document(std::string_view bytes) {
  auto parsed = json::parse(bytes);
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::get<json::Document>(std::move(parsed)));
}
// The answer of an encoder: empty when it encoded, else the text of its refusal.
template <class Result> std::string refusal_of(const Result& result) {
  const auto* error = std::get_if<Error>(&result);
  return error ? error->safe_message : std::string();
}
// The code the refusal carries.
template <class Result> std::string code_of(const Result& result) {
  const auto* error = std::get_if<Error>(&result);
  return error ? error->reason : std::string();
}
messages::Request messages_request() { return messages::Request::simple("fixture-model", "hello", 64); }
messages::Request with_servers(std::size_t count) {
  auto request = messages_request();
  for (std::size_t i = 0; i < count; ++i) {
    const auto name = "server" + std::to_string(i);
    request.mcp_servers.push_back({name, "https://mcp.example.test/" + name, std::nullopt});
    messages::ToolDefinition toolset;
    toolset.type = "mcp_toolset";
    toolset.options = document(R"({"mcp_server_name":)" + json::quote(name) + "}");
    request.tools.push_back(std::move(toolset));
  }
  return request;
}
messages::Request with_skills(std::size_t count) {
  auto request = messages_request();
  messages::ToolDefinition code;
  code.name = "code_execution";
  code.type = "code_execution_20250825";
  request.tools.push_back(std::move(code));
  for (std::size_t i = 0; i < count; ++i) request.skills.push_back({"custom", "skill_" + std::to_string(i), std::nullopt});
  return request;
}
messages::Request with_title(std::size_t length) {
  auto request = messages_request();
  auto media = Media::url(MediaKind::Document, "https://example.test/report.pdf");
  media.name = std::string(length, 'n');
  request.messages[0].parts.push_back(std::move(media));
  return request;
}
gemini::Request with_vocabulary(std::size_t count) {
  auto request = gemini::Request::simple("fixture-model", "hello", 64);
  request.audio_transcription_config = gemini::AudioTranscriptionConfig{{"en-US"}, std::vector<std::string>(count, "term"), std::nullopt, std::nullopt, std::nullopt};
  return request;
}
gemini::Request with_rates(std::optional<std::uint32_t> sample_rate, std::optional<std::uint32_t> bit_rate) {
  auto request = gemini::Request::simple("fixture-model", "hello", 64);
  request.response_modalities = {gemini::ResponseModality::Audio};
  request.audio_format = gemini::AudioFormat{std::string("AUDIO_MP3"), sample_rate, bit_rate, std::nullopt};
  return request;
}
interactions::Request with_interaction_vocabulary(std::size_t count) {
  auto request = interactions::Request::simple("fixture-model", "hello", 64);
  interactions::TranscriptionConfig config;
  config.language_codes = {"en-US"};
  config.custom_vocabulary = std::vector<std::string>(count, "term");
  request.transcription_config = std::move(config);
  return request;
}
std::string encoded_messages(const descriptor::ValidatedDescriptor& d, const messages::Request& r) { return refusal_of(messages::encode(d, r, false)); }
std::string encoded_gemini(const descriptor::ValidatedDescriptor& d, const gemini::Request& r) { return refusal_of(gemini::encode(d, r, false)); }
std::string encoded_interactions(const descriptor::ValidatedDescriptor& d, const interactions::Request& r) { return refusal_of(interactions::encode(d, r, false)); }
std::vector<batches::Item> batch_items(std::size_t count) {
  std::vector<batches::Item> items;
  items.reserve(count);
  for (std::size_t i = 0; i < count; ++i) items.push_back({"item-" + std::to_string(i), messages::Request::simple("fixture-model", "hi", 16)});
  return items;
}
std::vector<batches::Item> gemini_batch_items(std::size_t count) {
  std::vector<batches::Item> items;
  items.reserve(count);
  for (std::size_t i = 0; i < count; ++i) items.push_back({"item-" + std::to_string(i), gemini::Request::simple("fixture-model", "hi", 16)});
  return items;
}
std::string submitted(const descriptor::ValidatedDescriptor& d, std::vector<batches::Item> items) {
  batches::Submit submit;
  submit.items = std::move(items);
  return refusal_of(batches::encode(d, submit));
}

// The texts of the refusals, which carry no number: the number is the policy's.
constexpr std::string_view servers_text = "MCP server count limit exceeded";
constexpr std::string_view skills_text = "container skill count limit exceeded";
constexpr std::string_view title_text = "Messages document title length limit exceeded";
constexpr std::string_view vocabulary_text = "transcription list count limit exceeded";
constexpr std::string_view rate_text = "audio sample or bit rate is out of range";
constexpr std::string_view items_text = "batch item count limit exceeded";

// ---- tests ---------------------------------------------------------------------------------------------------------

// The embedded policy states every limit for every family and batch shape it applies to, and nowhere else.
void embedded_states_every_limit() {
  const auto document = embedded();
  std::size_t seen = 0;
  for (auto entry : document.root().get("families").elements()) {
    const auto family = entry.get("family").as_string();
    std::size_t names = 0;
    for (const auto& limit : family_limits) {
      if (limit.family != family) continue;
      ++names;
      CHECK(entry.get("limits").get(limit.name).is_uint() && entry.get("limits").get(limit.name).as_uint() > 0);
    }
    CHECK(entry.get("limits").valid() == (names > 0));
    if (names) CHECK(entry.get("limits").size() == names);
    ++seen;
  }
  CHECK(seen == std::size(families));
  for (auto entry : document.root().get("endpoints").elements()) {
    const bool inline_batch = entry.get("endpoint").as_string() == "batches" &&
                              std::find(std::begin(batch_shapes), std::end(batch_shapes), entry.get("shape").as_string()) != std::end(batch_shapes);
    CHECK(entry.get("max_items").valid() == inline_batch);
    if (inline_batch) CHECK(entry.get("max_items").is_uint() && entry.get("max_items").as_uint() > 0);
  }
}

// Case 1: a custom policy's value is the limit in force, below and above the embedded one.
void declared_limits_are_in_force() {
  const auto text = policy_text();
  const auto narrow = load(edited(text, "anthropic.messages", "limits", R"({"document_title_chars":500,"mcp_servers":2,"container_skills":20})"));
  const auto d = descriptor_of("anthropic.messages", narrow);
  CHECK(encoded_messages(d, with_servers(2)).empty());
  CHECK(encoded_messages(d, with_servers(3)) == servers_text);
  const auto wide = load(edited(text, "anthropic.messages", "limits", R"({"document_title_chars":500,"mcp_servers":30,"container_skills":20})"));
  CHECK(encoded_messages(descriptor_of("anthropic.messages", wide), with_servers(21)).empty());
  CHECK(encoded_messages(descriptor_of("anthropic.messages", wide), with_servers(31)) == servers_text);
  // The other Messages limits.
  const auto skills = load(edited(text, "anthropic.messages", "limits", R"({"document_title_chars":10,"mcp_servers":20,"container_skills":3})"));
  const auto s = descriptor_of("anthropic.messages", skills);
  CHECK(encoded_messages(s, with_skills(3)).empty());
  CHECK(encoded_messages(s, with_skills(4)) == skills_text);
  CHECK(encoded_messages(s, with_title(10)).empty());
  CHECK(encoded_messages(s, with_title(11)) == title_text);
  // Generate and Interactions.
  const auto generate = load(edited(text, "google.generate", "limits", R"({"transcription_vocabulary":2,"audio_sample_rate":16000,"audio_bit_rate":64000})"));
  const auto g = descriptor_of("google.generate", generate);
  CHECK(encoded_gemini(g, with_vocabulary(2)).empty());
  CHECK(encoded_gemini(g, with_vocabulary(3)) == vocabulary_text);
  CHECK(encoded_gemini(g, with_rates(16000, 64000)).empty());
  CHECK(encoded_gemini(g, with_rates(16001, std::nullopt)) == rate_text);
  CHECK(encoded_gemini(g, with_rates(std::nullopt, 64001)) == rate_text);
  const auto interactions = load(edited(text, "google.interactions", "limits", R"({"transcription_vocabulary":2})"));
  const auto i = descriptor_of("google.interactions", interactions);
  CHECK(encoded_interactions(i, with_interaction_vocabulary(2)).empty());
  CHECK(encoded_interactions(i, with_interaction_vocabulary(3)) == vocabulary_text);
  // A batch fact's own bound, for each shape that carries items inline.
  const auto batch = load(edited_endpoint(text, "batches", "anthropic", "max_items", "2"));
  CHECK(submitted(descriptor_of("anthropic.messages", batch), batch_items(2)).empty());
  CHECK(submitted(descriptor_of("anthropic.messages", batch), batch_items(3)) == items_text);
  const auto google = load(edited_endpoint(text, "batches", "google", "max_items", "2"));
  CHECK(submitted(descriptor_of("google.generate", google), gemini_batch_items(2)).empty());
  CHECK(submitted(descriptor_of("google.generate", google), gemini_batch_items(3)) == items_text);
  // The embedded policy's bounds on the same descriptors.
  CHECK(submitted(descriptor_of("anthropic.messages"), batch_items(3)).empty());
}

// Case 2: the embedded policy's limits are the ones the encoders have always applied.
void embedded_limits_apply() {
  const auto messages = descriptor_of("anthropic.messages");
  CHECK(encoded_messages(messages, with_servers(20)).empty());
  CHECK(encoded_messages(messages, with_servers(21)) == servers_text);
  CHECK(encoded_messages(messages, with_skills(20)).empty());
  CHECK(encoded_messages(messages, with_skills(21)) == skills_text);
  CHECK(encoded_messages(messages, with_title(500)).empty());
  CHECK(encoded_messages(messages, with_title(501)) == title_text);
  const auto generate = descriptor_of("google.generate");
  CHECK(encoded_gemini(generate, with_vocabulary(1000)).empty());
  CHECK(encoded_gemini(generate, with_vocabulary(1001)) == vocabulary_text);
  CHECK(encoded_gemini(generate, with_rates(384000, 1000000)).empty());
  CHECK(encoded_gemini(generate, with_rates(384001, std::nullopt)) == rate_text);
  CHECK(encoded_gemini(generate, with_rates(std::nullopt, 1000001)) == rate_text);
  CHECK(encoded_gemini(generate, with_rates(0, std::nullopt)) == rate_text);
  const auto interactions = descriptor_of("google.interactions");
  CHECK(encoded_interactions(interactions, with_interaction_vocabulary(1000)).empty());
  CHECK(encoded_interactions(interactions, with_interaction_vocabulary(1001)) == vocabulary_text);
  CHECK(submitted(messages, batch_items(100001)) == items_text);
  CHECK(submitted(generate, gemini_batch_items(100001)) == items_text);
  CHECK(submitted(messages, {}) == "a batch has items");
  // The codes are the ones the refusals have always had.
  CHECK(listed(reason::messages_request::all, servers_text, "request.messages.mcp_server_count_limit"));
  CHECK(listed(reason::messages_request::all, skills_text, "request.messages.skills_count_limit"));
  CHECK(listed(reason::messages_request::all, title_text, "request.messages.messages_document_title_is_limited_to_500"));
  CHECK(listed(reason::batches_request::all, items_text, "request.batches.items_limit"));
  CHECK(listed(reason::gemini_request::all, vocabulary_text, "request.generate.transcription_list_count_limit_exceeded"));
  CHECK(listed(reason::gemini_request::all, rate_text, "request.generate.audio_sample_or_bit_rate_is_out_of_range"));
  CHECK(listed(reason::interactions_request::all, vocabulary_text, "request.interactions.transcription_list_count_limit_exceeded"));
  // And each refusal carries its code.
  CHECK(code_of(messages::encode(messages, with_servers(21), false)) == "request.messages.mcp_server_count_limit");
  CHECK(code_of(messages::encode(messages, with_skills(21), false)) == "request.messages.skills_count_limit");
  CHECK(code_of(messages::encode(messages, with_title(501), false)) == "request.messages.messages_document_title_is_limited_to_500");
  batches::Submit over;
  over.items = batch_items(100001);
  CHECK(code_of(batches::encode(messages, over)) == "request.batches.items_limit");
  CHECK(code_of(gemini::encode(generate, with_vocabulary(1001), false)) == "request.generate.transcription_list_count_limit_exceeded");
  CHECK(code_of(gemini::encode(generate, with_rates(384001, std::nullopt), false)) == "request.generate.audio_sample_or_bit_rate_is_out_of_range");
  CHECK(code_of(interactions::encode(interactions, with_interaction_vocabulary(1001), false)) ==
        "request.interactions.transcription_list_count_limit_exceeded");
}

// Case 3: a policy written before the limits has the embedded policy's, and its content digest covers them.
void older_policies_borrow_the_limits() {
  const auto builtin = descriptor::builtin_policy();
  const auto older_text = without_limits(policy_text());
  const auto older = load(older_text);
  // The embedded limits are in force.
  const auto messages = descriptor_of("anthropic.messages", older);
  CHECK(encoded_messages(messages, with_servers(20)).empty());
  CHECK(encoded_messages(messages, with_servers(21)) == servers_text);
  CHECK(encoded_messages(messages, with_skills(21)) == skills_text);
  CHECK(encoded_messages(messages, with_title(501)) == title_text);
  CHECK(encoded_gemini(descriptor_of("google.generate", older), with_vocabulary(1001)) == vocabulary_text);
  CHECK(encoded_gemini(descriptor_of("google.generate", older), with_rates(384001, std::nullopt)) == rate_text);
  CHECK(encoded_interactions(descriptor_of("google.interactions", older), with_interaction_vocabulary(1001)) == vocabulary_text);
  CHECK(submitted(messages, batch_items(100001)) == items_text);
  CHECK(submitted(descriptor_of("google.generate", older), gemini_batch_items(100001)) == items_text);
  // The digest is the digest it had, plus the borrowed section behind its marker: the unborrowed form is the one the
  // digest of the same text always had.
  CHECK(older->content_digest_hex() != plain_digest(older_text));
  CHECK(older->content_digest_hex() == plain_digest(older_text, borrowed_form(every_limit())));
  CHECK(older->content_digest_hex() != builtin->content_digest_hex());
  CHECK(older->identity_hex() == builtin->identity_hex());
  // The policy that states the embedded limits itself answers the same and borrows nothing.
  CHECK(builtin->content_digest_hex() == plain_digest(policy_text()));
}

// Case 4: a policy that states every limit borrows none, with any values.
void stated_limits_borrow_nothing() {
  const auto text = edited(edited(edited(edited_endpoint(edited_endpoint(policy_text(),
      "batches", "anthropic", "max_items", "7"), "batches", "google", "max_items", "9"),
      "anthropic.messages", "limits", R"({"document_title_chars":5,"mcp_servers":2,"container_skills":3})"),
      "google.generate", "limits", R"({"transcription_vocabulary":4,"audio_sample_rate":8000,"audio_bit_rate":32000})"),
      "google.interactions", "limits", R"({"transcription_vocabulary":6})");
  const auto policy = load(text);
  CHECK(policy->content_digest_hex() == plain_digest(text));
  CHECK(policy->content_digest_hex() != descriptor::builtin_policy()->content_digest_hex());
  CHECK(policy->identity_hex() == descriptor::builtin_policy()->identity_hex());
  CHECK(policy->limit("anthropic.messages", "mcp_servers") == 2 && policy->limit("google.generate", "audio_sample_rate") == 8000);
  CHECK(policy->endpoint_limit("batches", "https://api.anthropic.com", "max_items") == 7);
  CHECK(policy->endpoint_limit("batches", "https://generativelanguage.googleapis.com", "max_items") == 9);
}

// Case 5: a `limits` member that leaves names out, and a batch fact without `max_items`, borrow just those.
void partial_policies_borrow_what_they_leave_out() {
  const auto text = policy_text();
  // One Generate limit stated: the other two are borrowed and show in the digest.
  const auto generate_text = edited(text, "google.generate", "limits", R"({"transcription_vocabulary":2})");
  const auto generate = load(generate_text);
  const auto g = descriptor_of("google.generate", generate);
  CHECK(encoded_gemini(g, with_vocabulary(2)).empty());
  CHECK(encoded_gemini(g, with_vocabulary(3)) == vocabulary_text);
  CHECK(encoded_gemini(g, with_rates(384000, 1000000)).empty());
  CHECK(encoded_gemini(g, with_rates(384001, std::nullopt)) == rate_text);
  CHECK(encoded_gemini(g, with_rates(std::nullopt, 1000001)) == rate_text);
  CHECK(generate->content_digest_hex() == plain_digest(generate_text, borrowed_form({borrowed_family_limit({"google.generate", "audio_sample_rate"}),
                                                                                   borrowed_family_limit({"google.generate", "audio_bit_rate"})})));
  CHECK(generate->limit("google.generate", "transcription_vocabulary") == 2);
  CHECK(generate->limit("google.generate", "audio_sample_rate") == 384000);
  // An empty `limits` takes all of the family's.
  const auto empty_text = edited(text, "anthropic.messages", "limits", "{}");
  const auto empty = load(empty_text);
  CHECK(encoded_messages(descriptor_of("anthropic.messages", empty), with_servers(21)) == servers_text);
  CHECK(empty->content_digest_hex() == plain_digest(empty_text, borrowed_form({borrowed_family_limit({"anthropic.messages", "document_title_chars"}),
                                                                              borrowed_family_limit({"anthropic.messages", "mcp_servers"}),
                                                                              borrowed_family_limit({"anthropic.messages", "container_skills"})})));
  // A batch fact without `max_items` takes the embedded policy's for its shape; the other shape is untouched.
  const auto batch_text = edited_endpoint(text, "batches", "anthropic", "max_items", "");
  const auto batch = load(batch_text);
  CHECK(submitted(descriptor_of("anthropic.messages", batch), batch_items(100001)) == items_text);
  CHECK(batch->content_digest_hex() == plain_digest(batch_text, borrowed_form({borrowed_batch_limit("anthropic")})));
  CHECK(batch->endpoint_limit("batches", "https://api.anthropic.com", "max_items") == 100000);
  // A stated batch bound beats the embedded one, in a policy that borrows other limits.
  const auto mixed_text = edited_endpoint(without_limits(text), "batches", "google", "max_items", "2");
  const auto mixed = load(mixed_text);
  CHECK(submitted(descriptor_of("google.generate", mixed), gemini_batch_items(3)) == items_text);
  CHECK(submitted(descriptor_of("google.generate", mixed), gemini_batch_items(2)).empty());
  auto others = every_limit();
  others.erase(std::remove_if(others.begin(), others.end(), [](const Borrowed& b) { return b.where == "/endpoints/batches/google"; }), others.end());
  CHECK(mixed->content_digest_hex() == plain_digest(mixed_text, borrowed_form(others)));
}

// The query answers the limit in force, declared or borrowed.
void limit_queries() {
  const auto builtin = descriptor::builtin_policy();
  for (const auto& limit : family_limits) CHECK(builtin->limit(limit.family, limit.name) == embedded_limit(limit.family, limit.name));
  CHECK(builtin->limit("anthropic.messages", "mcp_servers") == 20);
  CHECK(builtin->limit("anthropic.messages", "document_title_chars") == 500);
  CHECK(builtin->limit("google.generate", "audio_bit_rate") == 1000000);
  CHECK(builtin->endpoint_limit("batches", "https://api.anthropic.com", "max_items") == 100000);
  CHECK(builtin->endpoint_limit("batches", "https://generativelanguage.googleapis.com", "max_items") == 100000);
  // A name the family does not have, a family without limits, an unknown family, a batch fact that carries no items.
  CHECK(!builtin->limit("anthropic.messages", "transcription_vocabulary"));
  CHECK(!builtin->limit("anthropic.messages", ""));
  CHECK(!builtin->limit("openai.chat", "mcp_servers"));
  CHECK(!builtin->limit("openai.responses", "container_skills"));
  CHECK(!builtin->limit("no.such.family", "mcp_servers"));
  CHECK(!builtin->endpoint_limit("batches", "https://api.openai.com", "max_items"));
  CHECK(!builtin->endpoint_limit("batches", "https://example.invalid", "max_items"));
  CHECK(!builtin->endpoint_limit("files", "https://api.anthropic.com", "max_items"));
  CHECK(!builtin->endpoint_limit("batches", "https://api.anthropic.com", "no_such_limit"));
  // An older policy answers the value in force.
  const auto older = load(without_limits(policy_text()));
  for (const auto& limit : family_limits) CHECK(older->limit(limit.family, limit.name) == embedded_limit(limit.family, limit.name));
  CHECK(older->endpoint_limit("batches", "https://api.anthropic.com", "max_items") == 100000);
  // A stated value is the one in force.
  const auto stated = load(edited(policy_text(), "anthropic.messages", "limits", R"({"document_title_chars":500,"mcp_servers":2,"container_skills":20})"));
  CHECK(stated->limit("anthropic.messages", "mcp_servers") == 2 && stated->limit("anthropic.messages", "container_skills") == 20);
}

// A limit that is not of its shape, or a family that has no such limit, is refused with a pointer to it.
void limits_are_validated() {
  const auto text = policy_text();
  const auto messages = [&](std::string_view limits) { return edited(text, "anthropic.messages", "limits", limits); };
  CHECK(load(messages(R"({"mcp_servers":1})")) != nullptr);
  CHECK(load(messages(R"({"mcp_servers":9007199254740991})")) != nullptr);
  for (const std::string_view value : {"0", "-1", "1.5", R"("20")", "null", "true", "[]", "{}", "18446744073709551616"})
    CHECK(refused_at(messages(R"({"mcp_servers":)" + std::string(value) + "}"), "/limits/mcp_servers"));
  for (const std::string_view value : {"1", R"("limits")", "null", "[]", "[1]", "true"}) CHECK(refused_at(messages(value), "/limits"));
  // Only the names the family's encoder reads.
  CHECK(refused_at(messages(R"({"transcription_vocabulary":5})"), "/limits"));
  CHECK(refused_at(messages(R"({"mcp_servers":2,"audio_bit_rate":5})"), "/limits"));
  CHECK(refused_at(messages(R"({"no_such_limit":5})"), "/limits"));
  CHECK(refused_at(edited(text, "openai.chat", "limits", R"({"mcp_servers":5})"), "/limits"));
  CHECK(refused_at(edited(text, "openai.responses", "limits", R"({"container_skills":5})"), "/limits"));
  CHECK(refused_at(edited(text, "google.generate", "limits", R"({"mcp_servers":5})"), "/limits"));
  CHECK(refused_at(edited(text, "google.interactions", "limits", R"({"audio_sample_rate":5})"), "/limits"));
  CHECK(load(edited(text, "openai.chat", "limits", "{}")) != nullptr);
  CHECK(load(edited(text, "google.interactions", "limits", R"({"transcription_vocabulary":5})")) != nullptr);
  // A batch bound is a positive integer on a fact whose shape carries items inline.
  for (const std::string_view value : {"0", "-1", "1.5", R"("100")", "null", "true", "18446744073709551616"})
    CHECK(refused_at(edited_endpoint(text, "batches", "anthropic", "max_items", value), "/endpoints/max_items"));
  CHECK(refused_at(edited_endpoint(text, "batches", "openai", "max_items", "5"), "/endpoints/max_items"));
  CHECK(refused_at(edited_endpoint(text, "files", "anthropic", "max_items", "5"), "/endpoints/max_items"));
  CHECK(refused_at(edited_endpoint(text, "embeddings", "openai", "max_items", "5"), "/endpoints/max_items"));
  CHECK(load(edited_endpoint(text, "batches", "google", "max_items", "5")) != nullptr);
}

}  // namespace

int main() {
  struct Case { const char* name; void (*run)(); };
  const Case cases[]{{"embedded_states_every_limit", embedded_states_every_limit}, {"embedded_limits_apply", embedded_limits_apply},
                     {"declared_limits_are_in_force", declared_limits_are_in_force}, {"older_policies_borrow_the_limits", older_policies_borrow_the_limits},
                     {"stated_limits_borrow_nothing", stated_limits_borrow_nothing},
                     {"partial_policies_borrow_what_they_leave_out", partial_policies_borrow_what_they_leave_out},
                     {"limit_queries", limit_queries}, {"limits_are_validated", limits_are_validated}};
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
