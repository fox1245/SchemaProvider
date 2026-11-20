#include "descriptor/policy.h"
#include "descriptor/owned_keys.h"
#include "descriptor/stop_facts.h"
#include "crypto/crypto.h"
#include "sp/config_defaults.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace sp::descriptor {
namespace {
using json::Value;
// The controls that take one of a listed set of values, with the family's list and the effective value of each.
struct ValuedControl {
  Control control;
  std::vector<std::string> FamilyPolicy::*listed;
  std::optional<std::string_view> EffectiveChoices::*chosen;
};
constexpr ValuedControl valued_controls[]{
    {Control::ReasoningEffort, &FamilyPolicy::reasoning_efforts, &EffectiveChoices::reasoning_effort},
    {Control::ReasoningSummary, &FamilyPolicy::reasoning_summaries, &EffectiveChoices::reasoning_summary},
    {Control::ThinkingLevel, &FamilyPolicy::thinking_levels, &EffectiveChoices::thinking_level},
    {Control::ServiceTier, &FamilyPolicy::service_tiers, &EffectiveChoices::service_tier}};
// The mapping from a credential scheme to the header that carries the key, for the chat, endpoint and realtime paths.
constexpr std::array<CredentialScheme, credential_scheme_count> scheme_table{{
    {"bearer", "Authorization", "Bearer "}, {"x-api-key", "x-api-key", ""}, {"x-goog-api-key", "x-goog-api-key", ""}}};
// A bundle may carry a refreshed catalogue, which load_catalogue bounds to 8 MiB.
constexpr std::size_t bundle_catalogue_bytes = 8u * 1024u * 1024u;
[[noreturn]] void fail(std::string_view path, std::string_view expected) {
  throw ConfigError{std::string(path), std::string(expected), 0, "descriptor policy value rejected"};
}
// A fact's origin is compared byte for byte with a descriptor's base URL, which is its origin's serialization
// (canonical_origin), so the policy states the origin in that spelling. Called after the origin is known valid.
void canonical_spelling(std::string_view origin, std::string_view path) {
  if (!canonical_origin_spelling(origin)) fail(path, "origin in its canonical spelling: lower-case host, no default port");
}
// Every key must be present and no other may appear, except the listed optional ones: a snapshot
// written before an optional fact existed stays admissible.
void closed(Value value, std::initializer_list<std::string_view> keys, std::string_view path,
            std::initializer_list<std::string_view> optional = {}) {
  if (!value.is_object() || value.size() < keys.size() || value.size() > keys.size() + optional.size()) fail(path, "complete closed object");
  for (auto member : value.members())
    if (std::find(keys.begin(), keys.end(), member.key) == keys.end() &&
        std::find(optional.begin(), optional.end(), member.key) == optional.end()) fail(path, "declared fields only");
  for (auto key : keys) if (!value.get(key).valid()) fail(path, "complete closed object");
}
std::string text(Value v, std::string_view path) {
  if (!v.is_string() || v.as_string().empty() || v.as_string().size() > 128) fail(path, "bounded nonempty string");
  for (unsigned char c : v.as_string()) if (c < 32 || c == 127) fail(path, "string without controls");
  return std::string(v.as_string());
}
std::optional<std::string> optional_text(Value v, std::string_view path) { if (v.is_null()) return {}; return text(v,path); }
bool boolean(Value v, std::string_view path) { if (!v.is_bool()) fail(path,"boolean"); return v.as_bool(); }
std::optional<bool> optional_bool(Value v, std::string_view path) { if (v.is_null()) return {}; return boolean(v,path); }
std::uint64_t integer(Value v, std::string_view path) { if (!v.is_uint()) fail(path,"unsigned integer"); return v.as_uint(); }
std::optional<std::uint64_t> optional_integer(Value v, std::string_view path) { if (v.is_null()) return {}; return integer(v,path); }
double number(Value v, std::string_view path) { if (!v.is_number() || !std::isfinite(v.as_double())) fail(path,"finite number"); return v.as_double(); }
std::optional<double> optional_number(Value v, std::string_view path) { if (v.is_null()) return {}; return number(v,path); }
std::vector<std::string> strings(Value v, std::string_view path, bool nonempty = false) {
  if (!v.is_array() || v.size() > 64 || (nonempty && !v.size())) fail(path,"bounded string array");
  std::vector<std::string> values;
  for (auto element : v.elements()) { auto s = text(element,path); if (contains(values,s)) fail(path,"unique strings"); values.push_back(std::move(s)); }
  return values;
}
// Media admission lists are policy data. Entries are lowercase `type/subtype` essences; the
// top-level type must agree with the kind (documents are everything outside image/audio/video).
// generateContent's Blob.mimeType documents three spellings whose top-level type is not their
// meaning (https://ai.google.dev/api/generate-content); they classify by meaning, and only that
// family may list them. This file is below sp_core, so the table is local.
std::optional<std::string_view> generate_alias_kind(std::string_view mime) {
  if (mime == "video/audio/s16le" || mime == "video/audio/wav") return "audio";
  if (mime == "video/text/timestamp") return "document";
  return std::nullopt;
}
std::vector<std::string> media_types(Value v, std::string_view path, std::string_view kind, std::string_view family) {
  if (!v.is_array() || v.size() > 512) fail(path, "bounded MIME array");
  std::vector<std::string> values;
  for (auto element : v.elements()) {
    auto mime = text(element, path);
    bool valid;
    if (const auto alias = generate_alias_kind(mime)) {
      valid = family == "google.generate" && *alias == kind;
    } else {
      const auto slash = mime.find('/');
      valid = slash != std::string::npos && slash > 0 && slash + 1 < mime.size() &&
              mime.find('/', slash + 1) == std::string::npos;
      for (unsigned char c : mime) {
        if (c == '/') continue;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '!' || c == '#' || c == '$' || c == '%' ||
              c == '&' || c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' || c == '^' || c == '_' ||
              c == '`' || c == '|' || c == '~')) valid = false;
      }
      if (valid) {
        const auto top = std::string_view(mime).substr(0, slash);
        const bool typed_media = top == "image" || top == "audio" || top == "video";
        valid = kind == "document" ? !typed_media : top == kind;
      }
    }
    if (!valid || contains(values, mime)) fail(path, "unique lowercase MIME essences consistent with the media kind");
    values.push_back(std::move(mime));
  }
  return values;
}
MediaPolicy media_admission(Value v, std::string_view path, std::string_view family) {
  constexpr std::string_view names[]{"image", "audio", "video", "document"};
  closed(v, {"image", "audio", "video", "document"}, path);
  MediaPolicy policy;
  for (std::size_t i = 0; i < policy.kinds.size(); ++i) {
    const auto kind = v.get(names[i]);
    closed(kind, {"mime", "sources"}, path);
    auto& entry = policy.kinds[i];
    entry.mime = media_types(kind.get("mime"), path, names[i], family);
    const auto sources = kind.get("sources");
    if (!sources.is_array() || sources.size() > 3) fail(path, "bounded source array");
    for (auto source : sources.elements()) {
      const auto name = text(source, path);
      const std::uint8_t bit = name == "inline" ? 1 : name == "url" ? 2 : name == "file" ? 4 : 0;
      if (!bit || (entry.sources & bit)) fail(path, "unique inline, url or file sources");
      entry.sources = static_cast<std::uint8_t>(entry.sources | bit);
    }
    if (entry.mime.empty() != (entry.sources == 0)) fail(path, "an admitted kind lists both MIME types and sources");
  }
  return policy;
}
RequestDefaults defaults(Value v) {
  constexpr auto path = "/defaults";
  closed(v,{"max_output_tokens","thinking_budget","include_thoughts","thinking_summaries","reasoning_enabled","reasoning_effort","reasoning_summary","thinking_level","service_tier","temperature","top_p","strict_tools"},path);
  return {optional_integer(v.get("max_output_tokens"),path),optional_integer(v.get("thinking_budget"),path),optional_bool(v.get("include_thoughts"),path),optional_bool(v.get("thinking_summaries"),path),optional_bool(v.get("strict_tools"),path),boolean(v.get("reasoning_enabled"),path),optional_text(v.get("reasoning_effort"),path),optional_text(v.get("reasoning_summary"),path),optional_text(v.get("thinking_level"),path),optional_text(v.get("service_tier"),path),optional_number(v.get("temperature"),path),optional_number(v.get("top_p"),path)};
}
// A policy's stop row: a value whose meaning the codecs decide in code is refused, and so is a row that gives a value
// the embedded policy lists another kind. A policy may add rows.
std::pair<std::string, StopKind> stop_row(std::string_view family, const json::Member& row) {
  const auto kind = stop_kind_from_name(text(row.value, "/stop_reasons"));
  if (!kind) fail("/stop_reasons", "typed StopKind");
  if (row.key.empty() || row.key.size() > 128 || failure_stop(row.key) || contradicts(embedded_stops(family), row.key, *kind))
    fail("/stop_reasons", "unchanged standard causality");
  return {std::string(row.key), *kind};
}
std::array<double,2> range(Value v) { if(!v.is_array() || v.size()!=2)fail("/range","two finite bounds"); auto a=number(v.at(0),"/range"), b=number(v.at(1),"/range"); if(a>b)fail("/range","ordered bounds"); return {a,b}; }
EffectiveChoices choices(const RequestDefaults& d) {
  return {d.max_output_tokens,d.thinking_budget,d.include_thoughts,d.thinking_summaries,d.reasoning_enabled,borrowed(d.reasoning_effort),borrowed(d.reasoning_summary),borrowed(d.thinking_level),borrowed(d.service_tier),d.temperature,d.top_p};
}
// The bit set of the controls a policy lists, each name once.
std::uint32_t control_bits(Value controls) {
  std::uint32_t bits = 0;
  for (const auto& name : strings(controls, "/controls")) {
    const auto control = control_from_name(name);
    if (!control) fail("/controls", "declared control names");
    bits |= 1u << static_cast<unsigned>(*control);
  }
  return bits;
}
// The replay identity covers only the facts that decide how sealed history is written to and read
// from the wire. Families are hashed in name order and stop facts in key order, every text with its
// length, so the identity does not depend on the order of the file.
std::string replay_identity(const std::vector<FamilyPolicy>& families) {
  crypto::Sha256 hash;
  auto put = [&](std::string_view value) {
    const auto size = std::to_string(value.size());
    hash.update(size);
    hash.update(":");
    hash.update(value);
  };
  put("sp.descriptor-policy.replay.v1");
  std::vector<const FamilyPolicy*> ordered;
  for (const auto& family : families) ordered.push_back(&family);
  std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->family < b->family; });
  for (const auto* family : ordered) {
    put(family->family);
    put(family->model_member); put(family->messages_member); put(family->stream_member); put(family->cap_member);
    put(std::to_string(family->usage_path.size()));
    for (const auto& segment : family->usage_path) put(segment);
    auto stops = family->stops;
    std::sort(stops.begin(), stops.end());
    put(std::to_string(stops.size()));
    for (const auto& [raw, kind] : stops) { put(raw); put(std::to_string(static_cast<unsigned>(kind))); }
    put(family->header_version ? *family->header_version : std::string_view{});
  }
  const auto digest = hash.finish();
  return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}
bool within_depth(Value v, std::size_t remaining) {
  if (!v.is_array() && !v.is_object()) return true;
  if (!remaining) return false;
  if (v.is_array()) { for (auto child : v.elements()) if (!within_depth(child, remaining - 1)) return false; }
  else { for (auto child : v.members()) if (!within_depth(child.value, remaining - 1)) return false; }
  return true;
}
} // namespace
const StopRows& embedded_stops(std::string_view family) {
  // Parsed once from the embedded text. It never goes through builtin_policy(): this runs while that policy loads.
  static const auto tables = [] {
    std::vector<std::pair<std::string, StopRows>> parsed_tables;
    auto parsed = json::parse(config_defaults::descriptor_policy_json,
                              {config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth});
    const auto* document = std::get_if<json::Document>(&parsed);
    if (!document) throw std::logic_error("the embedded descriptor policy is not admissible JSON");
    for (auto entry : document->root().get("families").elements()) {
      StopRows rows;
      for (auto row : entry.get("stop_reasons").members()) {
        const auto kind = row.value.is_string() ? stop_kind_from_name(row.value.as_string()) : std::nullopt;
        if (!kind) throw std::logic_error("the embedded descriptor policy names an unknown stop kind");
        rows.emplace_back(std::string(row.key), *kind);
      }
      parsed_tables.emplace_back(std::string(entry.get("family").as_string()), std::move(rows));
    }
    return parsed_tables;
  }();
  for (const auto& [name, rows] : tables)
    if (name == family) return rows;
  static const StopRows none;
  return none;
}
// The model name the policy's own admission check puts in place of `{model}` in a route template.
constexpr std::string_view admission_model = "policy-admission";
// The provider limits a family's compiled encoder applies, by the names a policy states them under. A family states
// only these (a name another family's encoder reads is refused), and a policy that leaves one out has the embedded
// policy's value for it. The values are data; the names are the encoder's.
struct LimitName { std::string_view family, name; };
constexpr LimitName limit_names[]{
    {"anthropic.messages", "document_title_chars"}, {"anthropic.messages", "mcp_servers"}, {"anthropic.messages", "container_skills"},
    {"google.generate", "transcription_vocabulary"}, {"google.generate", "audio_sample_rate"}, {"google.generate", "audio_bit_rate"},
    {"google.interactions", "transcription_vocabulary"}};
bool limit_named(std::string_view family, std::string_view name) {
  return std::any_of(std::begin(limit_names), std::end(limit_names), [&](const LimitName& limit) { return limit.family == family && limit.name == name; });
}
// Whether the batch fact of this shape sends its requests inline, so that `max_items` bounds them.
bool inline_batch(std::string_view endpoint, std::string_view shape) { return endpoint == "batches" && (shape == "anthropic" || shape == "google"); }
class PolicyLoader {
 public:
  PolicyResult run(std::string_view source,std::string_view resources,CatalogueSnapshot catalogue) {
    std::uint64_t revision = 0;
    // The members a family takes from the embedded policy, by family and member, with their canonical JSON.
    std::vector<std::pair<std::pair<std::string, std::string>, std::string>> borrowed;
    try {
      auto p=json::parse(source,{config_defaults::codec_admission_policy_bytes,config_defaults::codec_admission_policy_depth});
      auto r=json::parse(resources,{config_defaults::codec_admission_policy_bytes,config_defaults::codec_admission_policy_depth});
      auto* pd=std::get_if<json::Document>(&p); auto* rd=std::get_if<json::Document>(&r);
      if (pd && pd->root().get("revision").is_uint()) revision = pd->root().get("revision").as_uint();
      if(!pd || !rd)fail("","strict bounded JSON policy");
      auto policy=std::shared_ptr<DescriptorPolicy>(new DescriptorPolicy);
      auto root=pd->root(); auto rr=rd->root();
      closed(root,{"version","revision","families"},"",{"endpoints","listings"}); closed(rr,{"version","resources","admission"},"/resources");
      if(integer(root.get("version"),"/version")!=1 || integer(rr.get("version"),"/version")!=1 || !integer(root.get("revision"),"/revision"))fail("/version","version 1 and positive revision");
      auto rv=rr.get("resources"), av=rr.get("admission");
      closed(rv,{"json_bytes","json_depth","request_bytes","chat_text_request_bytes","request_messages","request_tools","request_parts","native_bytes","native_depth","native_members","image_decoded_bytes","audio_decoded_bytes","video_decoded_bytes","document_decoded_bytes","descriptor_bytes","descriptor_depth","policy_bytes","policy_depth"},"/resources");
      closed(av,{"json_bytes","json_depth","request_bytes","chat_text_request_bytes","request_messages","request_tools","request_parts","native_bytes","native_depth","native_members","image_decoded_bytes","audio_decoded_bytes","video_decoded_bytes","document_decoded_bytes","descriptor_bytes","descriptor_depth","policy_bytes","policy_depth"},"/admission");
#define SP_RESOURCE(name) do { auto value=integer(rv.get(#name),"/resources/" #name); auto ceiling=integer(av.get(#name),"/admission/" #name); if(!value || value>ceiling || ceiling>config_defaults::codec_admission_##name || value>std::numeric_limits<std::size_t>::max())fail("/resources/" #name,"positive representable resource within immutable admission"); policy->resources_.name=static_cast<std::size_t>(value); } while(false)
      SP_RESOURCE(json_bytes); SP_RESOURCE(json_depth); SP_RESOURCE(request_bytes); SP_RESOURCE(chat_text_request_bytes); SP_RESOURCE(request_messages); SP_RESOURCE(request_tools); SP_RESOURCE(request_parts); SP_RESOURCE(native_bytes); SP_RESOURCE(native_depth); SP_RESOURCE(native_members); SP_RESOURCE(image_decoded_bytes); SP_RESOURCE(audio_decoded_bytes); SP_RESOURCE(video_decoded_bytes); SP_RESOURCE(document_decoded_bytes); SP_RESOURCE(descriptor_bytes); SP_RESOURCE(descriptor_depth); SP_RESOURCE(policy_bytes); SP_RESOURCE(policy_depth);
#undef SP_RESOURCE
      if(source.size()>policy->resources_.policy_bytes || resources.size()>policy->resources_.policy_bytes)fail("","configured policy byte limit");
      if (!within_depth(pd->root(), policy->resources_.policy_depth) || !within_depth(rd->root(), policy->resources_.policy_depth))
        fail("", "configured policy depth limit");
      auto families=root.get("families"); if(!families.is_array() || families.size()!=5)fail("/families","all five typed families");
      for(auto v:families.elements()) {
        closed(v,{"family","bindings","stop_reasons","defaults","required_output_cap","temperature_range","top_p_range","thinking_minimum","thinking_top_p_minimum","reasoning_efforts","reasoning_summaries","thinking_levels","service_tiers","header_versions","header_version","server_tools","openrouter_origins","media","openrouter_media"},"/families",{"temperature_forbidden_model_prefixes","temperature_forbidden_generations","max_completion_tokens_origins","unreplayable_parts","extensions","openrouter_plugins","openrouter_server_tools","betas","response_blocks","input_blocks","controls","standard_service_tiers","cache","auth","output_cap_maximum","thinking_budget_below_cap","thinking_budget_excludes_level","thinking_budget_excludes_temperature","routes","limits"});
        FamilyPolicy f; f.family=text(v.get("family"),"/families");
        if(f.family!="openai.chat" && f.family!="anthropic.messages" && f.family!="openai.responses" && f.family!="google.generate" && f.family!="google.interactions")fail("/families","typed family");
        if(policy->family(f.family))fail("/families","unique families");
        auto b=v.get("bindings"); closed(b,{"model","messages","stream","max_output_tokens","usage"},"/bindings");
        f.model_member=text(b.get("model"),"/bindings"); f.messages_member=text(b.get("messages"),"/bindings"); f.stream_member=text(b.get("stream"),"/bindings"); f.cap_member=text(b.get("max_output_tokens"),"/bindings"); f.usage_path=strings(b.get("usage"),"/bindings/usage",true);
        auto stops=v.get("stop_reasons"); if(!stops.is_object() || stops.size()>64)fail("/stop_reasons","bounded stop facts");
        for(auto s:stops.members()) f.stops.push_back(stop_row(f.family, s));
        f.defaults=::sp::descriptor::defaults(v.get("defaults")); f.required_output_cap=boolean(v.get("required_output_cap"),"/required_output_cap");
        if(f.required_output_cap!=(f.family=="anthropic.messages"))fail("/required_output_cap","protocol output requirement");
        f.temperature_range=range(v.get("temperature_range")); f.top_p_range=range(v.get("top_p_range")); f.thinking_minimum=integer(v.get("thinking_minimum"),"/thinking_minimum"); f.thinking_top_p_minimum=number(v.get("thinking_top_p_minimum"),"/thinking_top_p_minimum");
        f.reasoning_efforts=strings(v.get("reasoning_efforts"),"/reasoning_efforts"); f.reasoning_summaries=strings(v.get("reasoning_summaries"),"/reasoning_summaries"); f.thinking_levels=strings(v.get("thinking_levels"),"/thinking_levels"); f.service_tiers=strings(v.get("service_tiers"),"/service_tiers"); f.header_versions=strings(v.get("header_versions"),"/header_versions"); f.header_version=optional_text(v.get("header_version"),"/header_version");
        f.openrouter_origins = strings(v.get("openrouter_origins"), "/openrouter_origins");
        // Retired: temperature facts are model catalogue facts. An empty list, as older snapshots wrote it for the
        // families without facts, still loads; a fact in either key is refused.
        for (const std::string_view key : {"temperature_forbidden_model_prefixes", "temperature_forbidden_generations"})
          if (const auto retired = v.get(key); retired.valid() && (!retired.is_array() || retired.size()))
            fail(std::string("/") + std::string(key), "temperature facts in the model catalogue");
        if (!f.openrouter_origins.empty() && f.family != "openai.chat" && f.family != "openai.responses" && f.family != "anthropic.messages")
          fail("/openrouter_origins", "routing only for declared OpenRouter API families");
        for (const auto& route : f.openrouter_origins) {
          if (!valid_origin(route) || route.ends_with('/'))
            fail("/openrouter_origins", "canonical admitted origin without path");
          canonical_spelling(route, "/openrouter_origins");
        }
        // Optional: origins whose API rejects max_tokens, so the output cap defaults to max_completion_tokens there.
        if (auto origins = v.get("max_completion_tokens_origins"); origins.valid()) {
          const auto listed = strings(origins, "/max_completion_tokens_origins");
          if (!listed.empty() && f.family != "openai.chat") fail("/max_completion_tokens_origins", "Chat output-cap origins only");
          for (const auto& origin : listed) {
            if (!valid_origin(origin) || origin.ends_with('/') || contains(f.openrouter_origins, origin))
              fail("/max_completion_tokens_origins", "canonical admitted origin without path, apart from the OpenRouter origins");
            canonical_spelling(origin, "/max_completion_tokens_origins");
            policy->completion_cap_origins_.emplace_back(f.family, origin);
          }
        }
        // Optional: part shapes the service refuses to take back, observed live and lifted by removing the entry.
        if (auto parts = v.get("unreplayable_parts"); parts.valid()) {
          if (f.family != "google.generate") fail("/unreplayable_parts", "Generate part shapes only");
          if (!parts.is_array() || parts.size() > 16) fail("/unreplayable_parts", "bounded array of part shapes");
          auto member_name = [](std::string_view name) {
            return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
              return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            });
          };
          for (auto shape : parts.elements()) {
            closed(shape, {"member", "missing"}, "/unreplayable_parts");
            auto member = text(shape.get("member"), "/unreplayable_parts"), missing = text(shape.get("missing"), "/unreplayable_parts");
            if (!member_name(member) || !member_name(missing)) fail("/unreplayable_parts", "part and field member names");
            for (const auto& existing : f.unreplayable_parts)
              if (existing.first == member && existing.second == missing) fail("/unreplayable_parts", "unique part shapes");
            f.unreplayable_parts.emplace_back(std::move(member), std::move(missing));
          }
        }
        f.media = media_admission(v.get("media"), "/media", f.family);
        if (!v.get("openrouter_media").is_null()) {
          if (f.openrouter_origins.empty()) fail("/openrouter_media", "gateway media requires declared OpenRouter origins");
          f.openrouter_media = media_admission(v.get("openrouter_media"), "/openrouter_media", f.family);
        }
        if(f.family=="anthropic.messages" && (!f.header_version || !contains(f.header_versions,*f.header_version)))fail("/header_version","admitted Messages version");
        if(f.family!="anthropic.messages" && (f.header_version || !f.header_versions.empty()))fail("/header_version","no unsupported header version");
        // Optional: the typed controls the family's request carries, and the service tiers that bill at the standard rates.
        std::optional<std::uint32_t> declared_controls;
        if (auto controls = v.get("controls"); controls.valid()) {
          const auto bits = control_bits(controls);
          declared_controls = bits;
          // A control that takes one of a listed set of values is carried only when the family lists values.
          for (const auto& valued : valued_controls)
            if ((bits & (1u << static_cast<unsigned>(valued.control))) && (f.*valued.listed).empty())
              fail("/controls", "a control that takes listed values only where the family lists some");
          // The embedded lists state what the compiled encoders write: a list may only leave controls out.
          if (const auto embedded = embedded_member(f.family, "controls"); embedded.valid() && (bits & ~control_bits(embedded)))
            fail("/controls", "controls of the family's embedded list; a list may only leave controls out");
          policy->controls_.emplace_back(f.family, bits);
        }
        if (auto tiers = v.get("standard_service_tiers"); tiers.valid()) {
          auto listed = strings(tiers, "/standard_service_tiers");
          for (const auto& tier : listed)
            if (!contains(f.service_tiers, tier)) fail("/standard_service_tiers", "values of the family's service_tiers");
          policy->standard_tiers_.emplace_back(f.family, std::move(listed));
        }
        auto schema_of = [](Value value, const std::string& pointer) {
          auto parsed = parse_value_schema(value, pointer);
          if (auto* error = std::get_if<ConfigError>(&parsed)) fail(error->pointer, error->expected);
          return std::get<ValueSchema>(std::move(parsed));
        };
        // anthropic-beta values the policy admits, each at declared origins. Tool facts below may name one.
        if (auto betas = v.get("betas"); betas.valid()) {
          if (f.family != "anthropic.messages") fail("/betas", "Messages beta facts only");
          if (!betas.is_array() || betas.size() > 64) fail("/betas", "bounded array of beta facts");
          for (auto b : betas.elements()) {
            closed(b, {"value","origins"}, "/betas");
            BetaFact fact{text(b.get("value"), "/betas/value"), strings(b.get("origins"), "/betas/origins", true)};
            if (!std::all_of(fact.value.begin(), fact.value.end(), [](unsigned char c) {
                  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'; }))
              fail("/betas/value", "lower-case beta token");
            for (const auto& origin : fact.origins) {
              if (!valid_origin(origin) || origin.ends_with('/')) fail("/betas/origins", "canonical origins without path");
              canonical_spelling(origin, "/betas/origins");
            }
            for (const auto& existing : f.betas) if (existing.value == fact.value) fail("/betas/value", "unique beta values");
            f.betas.push_back(std::move(fact));
          }
        }
        // Response blocks kept whole and replayed as captured: never a block the codec types.
        if (auto blocks = v.get("response_blocks"); blocks.valid()) {
          if (f.family != "anthropic.messages") fail("/response_blocks", "Messages response block facts only");
          if (!blocks.is_array() || blocks.size() > 64) fail("/response_blocks", "bounded array of block facts");
          for (auto b : blocks.elements()) {
            closed(b, {"type"}, "/response_blocks", {"beta","signed_beta","opens_history"});
            ResponseBlockFact fact{text(b.get("type"), "/response_blocks/type")};
            if (!std::all_of(fact.type.begin(), fact.type.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }) ||
                fact.type == "text" || fact.type == "thinking" || fact.type == "redacted_thinking" || fact.type == "tool_use" ||
                fact.type == "server_tool_use" || fact.type == "mcp_tool_use" || fact.type.ends_with("_tool_result"))
              fail("/response_blocks/type", "a lower-case block type the codec does not type");
            for (const auto& [member, target] : {std::pair<std::string_view, std::string*>{"beta", &fact.beta}, {"signed_beta", &fact.signed_beta}}) {
              if (!b.get(member).valid()) continue;
              const std::string pointer = "/response_blocks/" + std::string(member);
              *target = text(b.get(member), pointer);
              if (std::none_of(f.betas.begin(), f.betas.end(), [&](const BetaFact& beta) { return beta.value == *target; }))
                fail(pointer, "a value of the family's betas");
            }
            if (b.get("opens_history").valid()) fact.opens_history = boolean(b.get("opens_history"), "/response_blocks/opens_history");
            for (const auto& existing : f.response_blocks) if (existing.type == fact.type) fail("/response_blocks/type", "unique block types");
            f.response_blocks.push_back(std::move(fact));
          }
        }
        // Input blocks the caller writes, each checked against the schema of the whole block: never a block the codec
        // types.
        if (auto blocks = v.get("input_blocks"); blocks.valid()) {
          if (f.family != "anthropic.messages") fail("/input_blocks", "Messages input block facts only");
          if (!blocks.is_array() || blocks.size() > 64) fail("/input_blocks", "bounded array of block facts");
          for (auto b : blocks.elements()) {
            closed(b, {"type","value"}, "/input_blocks");
            InputBlockFact fact{text(b.get("type"), "/input_blocks/type"), schema_of(b.get("value"), "/input_blocks/value")};
            if (!std::all_of(fact.type.begin(), fact.type.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }) ||
                fact.type.empty() || fact.type == "text" || fact.type == "image" || fact.type == "document" || fact.type == "tool_result" ||
                fact.type == "tool_use" || fact.type == "thinking" || fact.type == "redacted_thinking" || fact.type == "server_tool_use" ||
                fact.type == "mcp_tool_use" || fact.type.ends_with("_tool_result"))
              fail("/input_blocks/type", "a lower-case block type the codec does not type");
            // The schema is the whole block's, so it declares the block's type.
            if (fact.value.kind != ValueSchema::Kind::Object || !std::binary_search(fact.value.names.begin(), fact.value.names.end(), "type"))
              fail("/input_blocks/value", "object schema that declares type");
            for (const auto& existing : f.input_blocks) if (existing.type == fact.type) fail("/input_blocks/type", "unique block types");
            f.input_blocks.push_back(std::move(fact));
          }
        }
        auto tools=v.get("server_tools"); if(!tools.is_array() || tools.size()>64)fail("/server_tools","bounded reviewed tool facts");
        for (auto t : tools.elements()) {
          const bool messages = f.family == "anthropic.messages";
          // A Messages tool entry may carry no name (a toolset); every other fact names its tool.
          if (messages) closed(t, {"type","max_uses"}, "/server_tools", {"name","options","client","beta","container"});
          else closed(t, {"type","name","max_uses"}, "/server_tools", {"options","generic"});
          if (!messages && f.family != "openai.responses")
            fail("/server_tools", "typed Messages or Responses tool facts only");
          ServerToolFact fact{text(t.get("type"),"/server_tools"),
                              t.get("name").valid() ? text(t.get("name"),"/server_tools") : std::string(),
                              boolean(t.get("max_uses"),"/server_tools")};
          if (t.get("generic").valid()) fact.generic = boolean(t.get("generic"), "/server_tools/generic");
          if (t.get("client").valid()) fact.client = boolean(t.get("client"), "/server_tools/client");
          if (t.get("container").valid()) fact.container = boolean(t.get("container"), "/server_tools/container");
          if (t.get("beta").valid()) {
            fact.beta = text(t.get("beta"), "/server_tools/beta");
            if (std::none_of(f.betas.begin(), f.betas.end(), [&](const BetaFact& b) { return b.value == fact.beta; }))
              fail("/server_tools/beta", "a value of the family's betas");
          }
          if (fact.client && fact.max_uses) fail("/server_tools/max_uses", "no use limit on a tool the caller executes");
          if (t.get("options").valid()) {
            fact.options = schema_of(t.get("options"), "/server_tools/options");
            // The options are the tool's other members: an object whose properties never name an owned member.
            if (fact.options->kind != ValueSchema::Kind::Object) fail("/server_tools/options", "object schema");
            for (const auto& name : fact.options->names)
              if (role_of(owned_tool_members(f.family), name)) fail("/server_tools/options", "properties other than the tool's typed members");
          }
          if (f.family == "openai.responses") {
            // The typed hosted tools the Responses encoder can write, or a generic one with declared options.
            // OpenRouter's server tools have their own facts.
            constexpr std::string_view types[]{"web_search","image_generation","file_search","tool_search","shell",
                                               "code_interpreter","mcp","computer","computer_use_preview"};
            const bool typed = std::find(std::begin(types), std::end(types), fact.type) != std::end(types);
            const bool token = !fact.type.empty() && fact.type.size() <= 64 && std::all_of(fact.type.begin(), fact.type.end(), [](unsigned char c) {
              return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            });
            if (fact.max_uses || fact.name != fact.type || (typed ? fact.generic || fact.options : !fact.generic || !fact.options || !token))
              fail("/server_tools", "declared typed Responses tool fact, or a generic one with options");
          } else if (fact.generic) fail("/server_tools/generic", "Responses generic tools only");
          for (const auto& existing : f.server_tools)
            if (existing.type == fact.type) fail("/server_tools", "unique tool types");
          f.server_tools.push_back(std::move(fact));
        }
        // Policy-admitted extensions (#32): never an owned member, each for declared origins.
        if (auto extensions = v.get("extensions"); extensions.valid()) {
          if (!extensions.is_array() || extensions.size() > 64) fail("/extensions", "bounded array of extension facts");
          for (auto e : extensions.elements()) {
            closed(e, {"key","origins","value","replay_eligible"}, "/extensions", {"bound","beta"});
            ExtensionFact fact{text(e.get("key"), "/extensions/key"), strings(e.get("origins"), "/extensions/origins"),
                               schema_of(e.get("value"), "/extensions/value"), boolean(e.get("replay_eligible"), "/extensions/replay_eligible")};
            if (e.get("bound").valid()) fact.bound = boolean(e.get("bound"), "/extensions/bound");
            if (!fact.bound && !fact.replay_eligible) fail("/extensions/bound", "an unbound value is replay-eligible");
            if (e.get("beta").valid()) {
              if (f.family != "anthropic.messages") fail("/extensions/beta", "Messages extension betas only");
              fact.beta = text(e.get("beta"), "/extensions/beta");
              if (std::none_of(f.betas.begin(), f.betas.end(), [&](const BetaFact& beta) { return beta.value == fact.beta; }))
                fail("/extensions/beta", "a value of the family's betas");
            }
            if (fact.key.empty() || fact.key.size() > 64 || !std::all_of(fact.key.begin(), fact.key.end(), [](unsigned char c) {
                  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }))
              fail("/extensions/key", "member name token");
            if (request_key_owned(f.family, fact.key)) fail("/extensions/key", "a member the encoder does not own");
            if (fact.origins.empty()) fail("/extensions/origins", "at least one origin");
            for (const auto& origin : fact.origins) {
              if (!valid_origin(origin) || origin.ends_with('/')) fail("/extensions/origins", "canonical origins without path");
              canonical_spelling(origin, "/extensions/origins");
            }
            for (const auto& existing : f.extensions) if (existing.key == fact.key) fail("/extensions/key", "unique extension keys");
            f.extensions.push_back(std::move(fact));
          }
        }
        // OpenRouter plugins and server tools: admitted names with the schema of their options.
        for (const auto& [member, target] : {std::pair<std::string_view, std::vector<GatewayFact>*>{"openrouter_plugins", &f.openrouter_plugins},
                                             std::pair<std::string_view, std::vector<GatewayFact>*>{"openrouter_server_tools", &f.openrouter_server_tools}}) {
          const auto list = v.get(member);
          if (!list.valid()) continue;
          const std::string pointer = "/" + std::string(member);
          const bool plugins = member == "openrouter_plugins";
          if ((f.family != "openai.chat" && f.family != "openai.responses") || f.openrouter_origins.empty())
            fail(pointer, "Chat or Responses families with declared OpenRouter origins");
          if (!list.is_array() || list.size() > 64) fail(pointer, "bounded array of facts");
          for (auto item : list.elements()) {
            closed(item, {plugins ? "id" : "type"}, pointer, {plugins ? "options" : "parameters"});
            GatewayFact fact{text(item.get(plugins ? "id" : "type"), pointer)};
            if (!plugins && (!fact.name.starts_with("openrouter:") || fact.name.size() <= 11)) fail(pointer, "openrouter: tool type");
            if (const auto schema = item.get(plugins ? "options" : "parameters"); schema.valid() && !schema.is_null()) {
              fact.options = schema_of(schema, pointer + (plugins ? "/options" : "/parameters"));
              if (fact.options->kind != ValueSchema::Kind::Object) fail(pointer, "object schema");
              if (plugins && std::binary_search(fact.options->names.begin(), fact.options->names.end(), std::string("id"))) fail(pointer, "options without id");
            }
            for (const auto& existing : *target) if (existing.name == fact.name) fail(pointer, "unique names");
            target->push_back(std::move(fact));
          }
        }
        // Cache mark facts: what an origin (and, at a gateway, a model route) does with a mark. The first match wins, so a
        // fact for every origin and model comes last.
        if (auto facts = v.get("cache"); facts.valid()) {
          if (!facts.is_array() || facts.size() > 16) fail("/cache", "bounded array of cache facts");
          for (auto c : facts.elements()) {
            closed(c, {"explicit"}, "/cache", {"origins","routes","max_breakpoints","ttls","write_multiplier","min_cacheable_tokens","longest_first","unsupported"});
            CacheFact fact;
            auto& caps = fact.capabilities;
            if (c.get("origins").valid()) fact.origins = strings(c.get("origins"), "/cache/origins", true);
            for (const auto& origin : fact.origins) {
              if (!valid_origin(origin) || origin.ends_with('/')) fail("/cache/origins", "canonical origins without path");
              canonical_spelling(origin, "/cache/origins");
            }
            if (c.get("routes").valid()) {
              fact.routes = strings(c.get("routes"), "/cache/routes", true);
              if (fact.origins.empty() || f.openrouter_origins.empty() ||
                  std::any_of(fact.origins.begin(), fact.origins.end(), [&](const auto& origin) { return !contains(f.openrouter_origins, origin); }))
                fail("/cache/routes", "routes only at the family's declared OpenRouter origins");
              for (const auto& route : fact.routes)
                if (route.size() > 64 || std::any_of(route.begin(), route.end(), [](unsigned char ch) {
                      return !((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~' || ch == '/');
                    })) fail("/cache/routes", "lowercase model id prefixes");
            }
            for (const auto& earlier : f.cache)
              if (earlier.origins.empty() && earlier.routes.empty()) fail("/cache", "no fact after the one for every origin and model");
            caps.supports_explicit = boolean(c.get("explicit"), "/cache/explicit");
            if (c.get("min_cacheable_tokens").valid()) {
              const auto minimum = integer(c.get("min_cacheable_tokens"), "/cache/min_cacheable_tokens");
              if (!minimum) fail("/cache/min_cacheable_tokens", "positive token count");
              caps.min_cacheable_tokens = minimum;
            }
            if (caps.supports_explicit) {
              if (c.get("unsupported").valid()) fail("/cache/unsupported", "absent where marks are supported");
              const auto breakpoints = integer(c.get("max_breakpoints"), "/cache/max_breakpoints");
              if (!breakpoints || breakpoints > 16) fail("/cache/max_breakpoints", "between 1 and 16 breakpoints");
              caps.max_breakpoints = static_cast<std::uint32_t>(breakpoints);
              const auto ttls = strings(c.get("ttls"), "/cache/ttls", true);
              for (const auto& ttl : ttls) {
                if (ttl != "5m" && ttl != "1h") fail("/cache/ttls", "5m or 1h");
                (ttl == "5m" ? caps.five_minutes : caps.one_hour).offered = true;
              }
              if (auto multipliers = c.get("write_multiplier"); multipliers.valid()) {
                if (!multipliers.is_object() || multipliers.size() > 2) fail("/cache/write_multiplier", "multipliers by TTL");
                for (auto m : multipliers.members()) {
                  const auto value = number(m.value, "/cache/write_multiplier");
                  auto& lifetime = m.key == "5m" ? caps.five_minutes : caps.one_hour;
                  if ((m.key != "5m" && m.key != "1h") || !lifetime.offered || value < 1 || value > 100)
                    fail("/cache/write_multiplier", "a multiplier from 1 to 100 for each offered TTL");
                  lifetime.write_multiplier = value;
                }
              }
              if (c.get("longest_first").valid()) caps.longest_first = boolean(c.get("longest_first"), "/cache/longest_first");
            } else {
              for (const char* key : {"max_breakpoints", "ttls", "write_multiplier", "longest_first"})
                if (c.get(key).valid()) fail("/cache", "no breakpoints, TTLs or multipliers where marks are not supported");
              if (auto unsupported = c.get("unsupported"); unsupported.valid()) {
                const auto mode = text(unsupported, "/cache/unsupported");
                if (mode != "ignore" && mode != "reject") fail("/cache/unsupported", "ignore or reject");
                caps.ignore_unsupported = mode == "ignore";
              }
            }
            f.cache.push_back(std::move(fact));
          }
        }
        // How a chat call sends the API key. A policy written before the facts has the embedded policy's for the
        // family, and its content digest covers them (borrowed_).
        if (auto facts = v.get("auth"); facts.valid()) policy->auth_.emplace_back(f.family, auth_facts(facts));
        else {
          const auto* embedded = embedded_auth(f.family);
          if (!embedded) fail("/auth", "credential facts for the family in the embedded policy");
          policy->auth_.emplace_back(f.family, embedded->facts);
          borrowed.push_back({{f.family, "auth"}, embedded->json});
        }
        // Where the family's paths are. A policy written before the facts has the embedded policy's for the family,
        // where it states some, and its content digest covers them (borrowed_).
        if (auto facts = v.get("routes"); facts.valid()) {
          policy->routes_.emplace_back(f.family, route_facts(facts, f.family));
        } else if (const auto stated = embedded_member(f.family, "routes"); stated.valid()) {
          policy->routes_.emplace_back(f.family, route_facts(stated, f.family));
          borrowed.push_back({{f.family, "routes"}, stated.dump()});
        }
        // What the request checks read: the family's controls and the rules that bind a request's members. A policy
        // that states none for a family has the embedded policy's, and its content digest covers them (borrowed_).
        DescriptorPolicy::ValidationFacts rules;
        if (declared_controls) {
          rules.controls = *declared_controls;
        } else {
          const auto stated = embedded_member(f.family, "controls");
          if (!stated.valid()) fail("/controls", "controls for the family in the embedded policy");
          rules.controls = control_bits(stated);
          borrowed.push_back({{f.family, "controls"}, stated.dump()});
        }
        // A rule the policy states is read as stated; one it leaves out is the embedded policy's for the family, and
        // off where the embedded policy states none.
        auto rule = [&](std::string_view key, bool& declared) {
          declared = v.get(key).valid();
          if (declared) return v.get(key);
          const auto stated = embedded_member(f.family, key);
          if (stated.valid()) borrowed.push_back({{f.family, std::string(key)}, stated.dump()});
          return stated;
        };
        bool declared = false;
        if (const auto maximum = rule("output_cap_maximum", declared); maximum.valid()) {
          const auto value = integer(maximum, "/output_cap_maximum");
          if (!value || value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            fail("/output_cap_maximum", "positive integer within the signed 64-bit range");
          rules.output_cap_maximum = value;
        }
        if (const auto below = rule("thinking_budget_below_cap", declared); below.valid())
          rules.thinking_budget_below_cap = boolean(below, "/thinking_budget_below_cap");
        if (const auto exclusive = rule("thinking_budget_excludes_level", declared); exclusive.valid())
          rules.thinking_budget_excludes_level = boolean(exclusive, "/thinking_budget_excludes_level");
        if (const auto exclusive = rule("thinking_budget_excludes_temperature", declared); exclusive.valid()) {
          rules.thinking_budget_excludes_temperature = boolean(exclusive, "/thinking_budget_excludes_temperature");
          // The rule is about two controls the family must carry; a stated rule over controls it does not list is
          // an error. A rule taken from the embedded policy is read as it is.
          constexpr auto both = (1u << static_cast<unsigned>(Control::Temperature)) | (1u << static_cast<unsigned>(Control::ThinkingBudget));
          if (declared && rules.thinking_budget_excludes_temperature && (rules.controls & both) != both)
            fail("/thinking_budget_excludes_temperature", "Temperature and ThinkingBudget among the family's controls");
        }
        policy->validation_.emplace_back(f.family, rules);
        // The provider limits the family's encoder applies. One the policy states is read as stated; one it leaves out
        // is the embedded policy's for the family, and its content digest covers it (borrowed_).
        {
          const auto stated = v.get("limits");
          if (stated.valid() && !stated.is_object()) fail("/limits", "object of limit values");
          std::vector<std::pair<std::string, std::uint64_t>> in_force;
          if (stated.valid())
            for (auto member : stated.members()) {
              if (!limit_named(f.family, member.key)) fail("/limits", "limits the family's encoder applies");
              const auto pointer = "/limits/" + std::string(member.key);
              const auto value = integer(member.value, pointer);
              if (!value) fail(pointer, "positive integer");
              in_force.emplace_back(std::string(member.key), value);
            }
          for (const auto& limit : limit_names) {
            if (limit.family != f.family || std::any_of(in_force.begin(), in_force.end(), [&](const auto& listed) { return listed.first == limit.name; })) continue;
            const auto embedded = embedded_member(f.family, "limits").get(limit.name);
            const auto pointer = "/limits/" + std::string(limit.name);
            if (!embedded.valid()) fail(pointer, "a value for every limit of the family in the embedded policy");
            const auto value = integer(embedded, pointer);
            if (!value) fail(pointer, "positive integer");
            in_force.emplace_back(std::string(limit.name), value);
            borrowed.push_back({{f.family, "limits." + std::string(limit.name)}, embedded.dump()});
          }
          policy->limits_.emplace_back(f.family, std::move(in_force));
        }
        if (auto error = DescriptorPolicy::check(f, rules, choices(f.defaults), {}, false)) fail("/defaults", *error);
        if(f.defaults.strict_tools && f.family!="openai.responses") fail("/defaults/strict_tools","Responses strict tools only");
        policy->families_.push_back(std::move(f));
      }
      // The family each model listing belongs to. A listing the policy leaves out (or every listing, for a policy
      // written before the facts) has the embedded policy's, and its content digest covers them (borrowed_). The
      // embedded policy states every listing.
      {
        std::vector<std::optional<std::string>> stated(all_listings.size());
        if (const auto facts = root.get("listings"); facts.valid()) {
          if (!facts.is_array() || facts.size() > all_listings.size()) fail("/listings", "array of at most one fact per listing");
          for (auto item : facts.elements()) {
            closed(item, {"listing", "family"}, "/listings");
            const auto name = item.get("listing");
            const auto at = std::find_if(all_listings.begin(), all_listings.end(),
                                         [&](Listing listing) { return name.is_string() && listing_name(listing) == name.as_string(); });
            if (at == all_listings.end()) fail("/listings/listing", "a listing the SDK reads");
            auto& slot = stated[static_cast<std::size_t>(at - all_listings.begin())];
            if (slot) fail("/listings/listing", "each listing once");
            slot = text(item.get("family"), "/listings/family");
            if (!policy->family(*slot)) fail("/listings/family", "a family of the policy");
          }
        }
        for (std::size_t i = 0; i < all_listings.size(); ++i) {
          if (stated[i]) {
            policy->listings_.emplace_back(all_listings[i], *stated[i]);
            continue;
          }
          const auto embedded = embedded_listing(all_listings[i]);
          if (!embedded) fail("/listings", "a family for every listing in the embedded policy");
          if (!policy->family(*embedded)) fail("/listings/family", "a family of the policy");
          policy->listings_.emplace_back(all_listings[i], *embedded);
          borrowed.push_back({{std::string(), "listing:" + std::string(listing_name(all_listings[i]))}, "\"" + *embedded + "\""});
        }
      }
      // Non-chat endpoints (D6): each names a compiled codec shape, its origins, the path of each operation and
      // the schema of the members a request may add beside the typed ones.
      if (auto endpoints = root.get("endpoints"); endpoints.valid()) {
        if (!endpoints.is_array() || endpoints.size() > 64) fail("/endpoints", "bounded array of endpoint facts");
        const auto operations_of = [](std::string_view endpoint, std::string_view shape) -> std::vector<std::string_view> {
          if (endpoint == "images" && shape == "openai") return {"generate", "edit"};
          if (endpoint == "images" && shape == "openrouter") return {"generate"};
          if (endpoint == "decisions" && (shape == "openai" || shape == "openrouter")) return {"decide"};
          if (endpoint == "embeddings" && (shape == "openai" || shape == "openrouter" || shape == "google")) return {"embed"};
          if (endpoint == "rerank" && shape == "openrouter") return {"rank"};
          if (endpoint == "moderation" && shape == "openai") return {"check"};
          if (endpoint == "speech" && (shape == "openai" || shape == "openrouter")) return {"speak"};
          if (endpoint == "transcription" && shape == "openai") return {"transcribe", "translate"};
          if (endpoint == "transcription" && shape == "openrouter") return {"transcribe"};
          // A job: submit starts it, poll reads its state, download fetches one output.
          if (endpoint == "videos" && shape == "openrouter") return {"submit", "poll", "download"};
          // OpenAI and Google also upload a file in parts: OpenAI Uploads, and Google's resumable upload, whose later
          // steps go to the upload URL its start returns.
          if (endpoint == "files" && shape == "openai") return {"upload", "get", "list", "delete", "content", "start", "part", "complete", "cancel"};
          if (endpoint == "files" && shape == "google") return {"upload", "get", "list", "delete", "content", "start"};
          // A realtime session (D8): the WebSocket path it connects to.
          if (endpoint == "realtime" && (shape == "google" || shape == "openai")) return {"connect"};
          if (endpoint == "files" && shape == "anthropic") return {"upload", "get", "list", "delete", "content"};
          // Batches: OpenAI reads results from its files, Anthropic serves them itself.
          if (endpoint == "batches" && shape == "openai") return {"submit", "get", "cancel"};
          if (endpoint == "batches" && shape == "anthropic") return {"submit", "get", "cancel", "delete", "results"};
          if (endpoint == "batches" && shape == "google") return {"submit", "get", "cancel", "delete"};
          if (endpoint == "tokens" && (shape == "openai" || shape == "anthropic" || shape == "google")) return {"count"};
          if (endpoint == "generations" && shape == "openrouter") return {"get"};
          return {};
        };
        // What an operation's billing may be: `none` for any; `model` where the call's request names the model it bills
        // by; `carried` for the batch submit, which bills the chat requests it carries.
        const auto names_model = [](std::string_view endpoint, std::string_view operation) {
          return (endpoint == "images" && (operation == "generate" || operation == "edit")) || (endpoint == "decisions" && operation == "decide") ||
                 (endpoint == "embeddings" && operation == "embed") || (endpoint == "rerank" && operation == "rank") ||
                 (endpoint == "moderation" && operation == "check") || (endpoint == "speech" && operation == "speak") ||
                 (endpoint == "transcription" && (operation == "transcribe" || operation == "translate")) ||
                 (endpoint == "videos" && operation == "submit") || (endpoint == "tokens" && operation == "count") ||
                 (endpoint == "realtime" && operation == "connect");
        };
        const auto admitted = [&](std::string_view endpoint, std::string_view operation, EndpointBilling value) {
          if (value == EndpointBilling::Model) return names_model(endpoint, operation);
          if (value == EndpointBilling::Carried) return endpoint == "batches" && operation == "submit";
          return value == EndpointBilling::None;
        };
        const auto billing_value = [](Value v) {
          const auto name = v.is_string() ? v.as_string() : std::string_view{};
          if (name == "none") return EndpointBilling::None;
          if (name == "model") return EndpointBilling::Model;
          if (name == "carried") return EndpointBilling::Carried;
          fail("/endpoints/billing", "none, model or carried");
        };
        for (auto e : endpoints.elements()) {
          closed(e, {"endpoint","shape","origins","paths"}, "/endpoints", {"options","auth","charge_on_accept","headers","keep","audio","close_codes","max_response_bytes","accepted_audio","max_part_bytes","billing","carried_tier","max_items"});
          EndpointFact fact{text(e.get("endpoint"), "/endpoints/endpoint"), text(e.get("shape"), "/endpoints/shape"),
                            strings(e.get("origins"), "/endpoints/origins")};
          const auto operations = operations_of(fact.endpoint, fact.shape);
          if (operations.empty()) fail("/endpoints/shape", "an endpoint and shape the SDK has a codec for");
          if (fact.origins.empty()) fail("/endpoints/origins", "at least one origin");
          for (const auto& origin : fact.origins) {
            if (!valid_origin(origin) || origin.ends_with('/')) fail("/endpoints/origins", "canonical origins without path");
            canonical_spelling(origin, "/endpoints/origins");
          }
          const auto paths = e.get("paths");
          if (!paths.is_object() || !paths.size()) fail("/endpoints/paths", "object of operation paths");
          for (auto member : paths.members()) {
            const auto path = text(member.value, "/endpoints/paths");
            if (std::find(operations.begin(), operations.end(), member.key) == operations.end())
              fail("/endpoints/paths", "an operation of the endpoint's shape");
            if (!path.starts_with('/') || path.size() > 256 || path.find_first_of("?# \\\"") != std::string::npos ||
                std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 0x21 || c > 0x7e; }))
              fail("/endpoints/paths", "an absolute path without query");
            fact.paths.emplace_back(std::string(member.key), path);
          }
          if (e.get("options").valid()) {
            auto parsed = parse_value_schema(e.get("options"), "/endpoints/options");
            if (auto* error = std::get_if<ConfigError>(&parsed)) fail(error->pointer, error->expected);
            fact.options = std::get<ValueSchema>(std::move(parsed));
            if (fact.options->kind != ValueSchema::Kind::Object) fail("/endpoints/options", "object schema");
            for (const auto& name : fact.options->names)
              if (member_role(fact.endpoint, name)) fail("/endpoints/options", "properties other than the endpoint's typed members");
          }
          if (e.get("auth").valid()) {
            fact.auth = text(e.get("auth"), "/endpoints/auth");
            if (!credential_scheme(fact.auth)) fail("/endpoints/auth", credential_scheme_names());
          }
          if (e.get("charge_on_accept").valid()) fact.charge_on_accept = boolean(e.get("charge_on_accept"), "/endpoints/charge_on_accept");
          if (e.get("max_response_bytes").valid()) {
            fact.max_response_bytes = integer(e.get("max_response_bytes"), "/endpoints/max_response_bytes");
            if (!fact.max_response_bytes || fact.max_response_bytes > std::numeric_limits<std::size_t>::max())
              fail("/endpoints/max_response_bytes", "a positive size in bytes");
          }
          if (e.get("max_part_bytes").valid()) {
            fact.max_part_bytes = integer(e.get("max_part_bytes"), "/endpoints/max_part_bytes");
            if (fact.endpoint != "files" || !fact.max_part_bytes || fact.max_part_bytes > std::numeric_limits<std::size_t>::max())
              fail("/endpoints/max_part_bytes", "a positive size in bytes on a files fact");
          }
          if (const auto audio = e.get("accepted_audio"); audio.valid()) {
            if (fact.endpoint != "transcription") fail("/endpoints/accepted_audio", "a transcription fact");
            closed(audio, {"types"}, "/endpoints/accepted_audio", {"max_bytes"});
            const auto types = audio.get("types");
            if (!types.is_object() || !types.size() || types.size() > 32) fail("/endpoints/accepted_audio/types", "one to 32 audio MIME types");
            AcceptedAudioFact input;
            for (auto member : types.members()) {
              std::string mime(member.key);
              auto name = text(member.value, "/endpoints/accepted_audio/types");
              const auto subtype = std::string_view(mime).substr(std::min<std::size_t>(mime.size(), 6));
              const auto word = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
              if (!mime.starts_with("audio/") || subtype.empty() || subtype.size() > 64 ||
                  !std::all_of(subtype.begin(), subtype.end(), [&](char c) { return word(c) || c == '-' || c == '.' || c == '+' || c == '_'; }))
                fail("/endpoints/accepted_audio/types", "lowercase audio MIME essences");
              if (name.size() > 16 || !std::all_of(name.begin(), name.end(), word))
                fail("/endpoints/accepted_audio/types", "format names of lowercase letters and digits");
              input.types.emplace_back(std::move(mime), std::move(name));
            }
            if (audio.get("max_bytes").valid()) {
              input.max_bytes = integer(audio.get("max_bytes"), "/endpoints/accepted_audio/max_bytes");
              if (!input.max_bytes || input.max_bytes > config_defaults::codec_admission_audio_decoded_bytes)
                fail("/endpoints/accepted_audio/max_bytes", "a positive size within the audio admission ceiling");
            }
            fact.accepted_audio = std::move(input);
          }
          if (e.get("keep").valid()) {
            fact.keep = strings(e.get("keep"), "/endpoints/keep");
            if (fact.keep.size() > 64 || std::any_of(fact.keep.begin(), fact.keep.end(), [](const std::string& name) {
                  return name.empty() || name.size() > 64 || !std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
                }))
              fail("/endpoints/keep", "at most 64 member names");
          }
          if (const auto headers = e.get("headers"); headers.valid()) {
            if (!headers.is_object() || headers.size() > 8) fail("/endpoints/headers", "object with at most 8 literal headers");
            for (auto member : headers.members()) {
              std::string name(member.key);
              std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
              // A credential header carries the key, which only the call's scheme writes. The opening handshake of a
              // WebSocket endpoint sets the last five itself.
              constexpr std::string_view reserved[]{"proxy-authorization", "cookie", "host",
                                                    "connection", "content-type", "content-length", "accept", "transfer-encoding", "expect",
                                                    "upgrade", "sec-websocket-key", "sec-websocket-version", "sec-websocket-extensions",
                                                    "sec-websocket-protocol", "sec-websocket-accept"};
              if (name.empty() || name.size() > 64 || !std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }) ||
                  credential_header_name(name) || std::find(std::begin(reserved), std::end(reserved), name) != std::end(reserved))
                fail("/endpoints/headers", "a non-reserved ASCII header name");
              const auto value = text(member.value, "/endpoints/headers");
              if (value.empty() || value.size() > 256 || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 0x20 && c < 0x7f; }))
                fail("/endpoints/headers", "a printable header value");
              fact.headers.emplace_back(std::string(member.key), value);
            }
          }
          // A realtime session (D8) pins the formats of its audio and names the close codes that report a failure.
          if (fact.endpoint != "realtime" && (e.get("audio").valid() || e.get("close_codes").valid()))
            fail("/endpoints", "audio formats and close codes on realtime endpoints only");
          if (fact.endpoint == "realtime") {
            const auto audio = e.get("audio");
            if (!audio.valid()) fail("/endpoints/audio", "the audio formats of a realtime endpoint");
            closed(audio, {"input", "output"}, "/endpoints/audio");
            const auto formats = [&](Value list, std::string_view path) {
              if (!list.is_array() || !list.size() || list.size() > 8) fail(path, "one to eight audio formats");
              std::vector<AudioFormatFact> out;
              for (auto item : list.elements()) {
                closed(item, {"mime", "rate", "sample_bytes"}, path);
                AudioFormatFact format{text(item.get("mime"), path)};
                const auto rate = integer(item.get("rate"), path), width = integer(item.get("sample_bytes"), path);
                if (!rate || rate > 192000 || !width || width > 4) fail(path, "a sample rate up to 192000 and 1 to 4 bytes per sample");
                format.rate = static_cast<std::uint32_t>(rate);
                format.sample_bytes = static_cast<std::uint32_t>(width);
                // `audio/pcm;rate=24000`: a lower-case type/subtype, and when it has a parameter, the sample rate.
                const auto semicolon = format.mime.find(';');
                const auto essence = std::string_view(format.mime).substr(0, semicolon);
                const auto slash = essence.find('/');
                if (slash == std::string_view::npos || slash == 0 || slash + 1 == essence.size() || essence.find('/', slash + 1) != std::string_view::npos ||
                    !std::all_of(essence.begin(), essence.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '+' || c == '-'; }) ||
                    !essence.starts_with("audio/") ||
                    (semicolon != std::string::npos && format.mime.substr(semicolon) != ";rate=" + std::to_string(rate)))
                  fail(path, "an audio media type, with the sample rate as its only parameter");
                for (const auto& existing : out) if (existing.mime == format.mime) fail(path, "unique audio formats");
                out.push_back(std::move(format));
              }
              return out;
            };
            fact.audio_input = formats(audio.get("input"), "/endpoints/audio/input");
            fact.audio_output = formats(audio.get("output"), "/endpoints/audio/output");
            if (const auto closes = e.get("close_codes"); closes.valid()) {
              if (!closes.is_array() || closes.size() > 32) fail("/endpoints/close_codes", "at most 32 close codes");
              for (auto item : closes.elements()) {
                closed(item, {"code", "status"}, "/endpoints/close_codes", {"reason"});
                CloseCodeFact close;
                const auto code = integer(item.get("code"), "/endpoints/close_codes/code");
                const auto status = integer(item.get("status"), "/endpoints/close_codes/status");
                // A close code is 1000 to 4999 (RFC 6455 7.4).
                if (code < 1000 || code > 4999 || status < 100 || status > 599) fail("/endpoints/close_codes", "a close code and an HTTP status");
                close.code = static_cast<std::uint16_t>(code);
                close.status = static_cast<int>(status);
                if (item.get("reason").valid()) close.reason = text(item.get("reason"), "/endpoints/close_codes/reason");
                for (const auto& existing : fact.close_codes) if (existing.code == close.code && existing.reason == close.reason) fail("/endpoints/close_codes", "unique close codes");
                fact.close_codes.push_back(std::move(close));
              }
            }
          }
          // Whether the provider bills each operation: one value for every call of the endpoint (the steps of Google's
          // resumable upload, which have no operation of their own, included), or a value per operation that names every
          // operation of `paths`. Without the member the policy states nothing, and every operation is Unstated.
          DescriptorPolicy::EndpointBillingFact billed;
          if (const auto billing = e.get("billing"); billing.valid()) {
            if (billing.is_object()) {
              for (auto member : billing.members()) {
                if (std::find(operations.begin(), operations.end(), member.key) == operations.end())
                  fail("/endpoints/billing", "operations of the endpoint's shape");
                const auto value = billing_value(member.value);
                if (!admitted(fact.endpoint, member.key, value))
                  fail("/endpoints/billing", "model where the request names a model, carried for a batch submit");
                billed.operations.emplace_back(std::string(member.key), value);
              }
              for (const auto& [operation, path] : fact.paths)
                if (std::none_of(billed.operations.begin(), billed.operations.end(), [&](const auto& stated) { return stated.first == operation; }))
                  fail("/endpoints/billing", "a value for every operation of paths");
            } else {
              const auto value = billing_value(billing);
              for (const auto operation : operations)
                if (!admitted(fact.endpoint, operation, value))
                  fail("/endpoints/billing", "a value every operation of the endpoint's shape admits");
              billed.operations.emplace_back(std::string(), value);
            }
            // A fact that charges once it accepts a call bills at least one of its operations.
            if (fact.charge_on_accept && std::none_of(billed.operations.begin(), billed.operations.end(), [](const auto& stated) {
                  return stated.second == EndpointBilling::Model || stated.second == EndpointBilling::Carried;
                }))
              fail("/endpoints/billing", "a billed operation on a fact that charges once it accepts a call");
          }
          // The catalogue tier carried requests are priced under: required with them and refused without. A tier is the
          // part of a price key after `/`, so it holds neither `/` nor `+`.
          const bool carries = std::any_of(billed.operations.begin(), billed.operations.end(),
                                           [](const auto& stated) { return stated.second == EndpointBilling::Carried; });
          if (const auto tier = e.get("carried_tier"); tier.valid()) {
            if (!carries) fail("/endpoints/carried_tier", "a tier only for a fact that bills carried requests");
            billed.carried_tier = text(tier, "/endpoints/carried_tier");
            if (billed.carried_tier.size() > 64 || !std::all_of(billed.carried_tier.begin(), billed.carried_tier.end(), [](char c) {
                  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ':';
                }))
              fail("/endpoints/carried_tier", "a catalogue tier name");
          } else if (carries) {
            fail("/endpoints/carried_tier", "the catalogue tier carried requests are priced under");
          }
          // The most requests one inline batch carries. A fact that sends its requests inline and states no bound has the
          // embedded policy's for its shape, and its content digest covers it (borrowed_).
          std::optional<std::uint64_t> max_items;
          if (const auto stated = e.get("max_items"); stated.valid()) {
            if (!inline_batch(fact.endpoint, fact.shape)) fail("/endpoints/max_items", "a batch fact whose requests go inline");
            max_items = integer(stated, "/endpoints/max_items");
            if (!*max_items) fail("/endpoints/max_items", "positive integer");
          } else if (inline_batch(fact.endpoint, fact.shape)) {
            const auto embedded = embedded_endpoint_member(fact.endpoint, fact.shape, "max_items");
            if (!embedded.valid()) fail("/endpoints/max_items", "a bound for the shape in the embedded policy");
            max_items = integer(embedded, "/endpoints/max_items");
            if (!*max_items) fail("/endpoints/max_items", "positive integer");
            std::pair<std::pair<std::string, std::string>, std::string> entry{{"/endpoints/" + fact.endpoint + "/" + fact.shape, "max_items"}, embedded.dump()};
            if (std::find(borrowed.begin(), borrowed.end(), entry) == borrowed.end()) borrowed.push_back(std::move(entry));
          }
          for (const auto& existing : policy->endpoints_)
            if (existing.endpoint == fact.endpoint)
              for (const auto& origin : fact.origins)
                if (contains(existing.origins, origin)) fail("/endpoints/origins", "one fact per endpoint and origin");
          policy->max_items_.push_back(max_items);
          policy->endpoints_.push_back(std::move(fact));
          policy->billing_.push_back(std::move(billed));
        }
      }
      // In family and member order, each text with its length, so the form does not depend on the file's order.
      std::sort(borrowed.begin(), borrowed.end());
      for (const auto& [where, value] : borrowed)
        for (const std::string_view text : {std::string_view(where.first), std::string_view(where.second), std::string_view(value)})
          policy->borrowed_ += std::to_string(text.size()) + ":" + std::string(text);
      {
        crypto::Sha256 hash;
        hash.update(pd->root().dump());
        hash.update("\n");
        hash.update(rd->root().dump());
        const auto digest = hash.finish();
        policy->pair_digest_.assign(reinterpret_cast<const char*>(digest.data()), digest.size());
      }
      policy->identity_ = replay_identity(policy->families_);
      attach(*policy, std::move(catalogue));
      // Reuse descriptor admission for slot collisions, escaped usage segments,
      // and reserved wire fields instead of introducing a second convention.
      for (const auto& family : policy->families_) {
        // The paths are the family's own route: its first fact that names {model}, filled with a model name, else
        // its first fact, at the fact's first origin. A family without facts takes a {model} pair anywhere.
        const auto& facts = policy->routes(family.family);
        const auto named = std::find_if(facts.begin(), facts.end(), [](const RouteFact& fact) { return fact.buffered.find("{model}") != std::string::npos; });
        const RouteFact* chosen = named != facts.end() ? &*named : facts.empty() ? nullptr : &facts.front();
        const std::string base = chosen && !chosen->origins.empty() ? chosen->origins.front() : std::string("https://policy.invalid");
        const std::string buffered = chosen ? model_path(chosen->buffered, admission_model) : std::string("/policy/{model}");
        const std::string streaming = chosen ? model_path(chosen->streaming, admission_model) : std::string("/policy/{model}");
        const auto descriptor_source =
            std::string("{\"descriptor_version\":1,\"revision\":1,\"id\":\"policy-admission\",\"family\":") +
            json::quote(family.family) + ",\"connection\":{\"base_url\":" + json::quote(base) + ",\"paths\":{\"buffered\":" +
            json::quote(buffered) + ",\"streaming\":" + json::quote(streaming) + "}}}";
        auto admitted = load(descriptor_source, PolicySnapshot(policy));
        if (auto* error = std::get_if<ConfigError>(&admitted)) throw std::move(*error);
      }
      return PolicySnapshot(std::move(policy));
    } catch (ConfigError& error) { error.revision = revision; return std::move(error); }
  }
  // Resolves each catalogue entry's defaults over its family's and checks them like family
  // defaults; then binds the catalogue into the content digest.
  static void attach(DescriptorPolicy& policy, CatalogueSnapshot catalogue) {
    if (!catalogue) fail("/model_catalogue", "model catalogue");
    policy.resolved_.clear();
    const auto& entries = catalogue->entries();
    // Each control some entry's `control_values` name, with the origins of the scoped entries that name it.
    std::vector<std::pair<std::string_view, std::vector<std::string_view>>> named;
    for (const auto& entry : entries)
      for (const auto& listed : entry.control_values) {
        auto found = std::find_if(named.begin(), named.end(), [&](const auto& n) { return n.first == listed.first; });
        if (found == named.end()) {
          named.emplace_back(listed.first, std::vector<std::string_view>{});
          found = std::prev(named.end());
        }
        for (const auto& origin : entry.origins)
          if (std::find(found->second.begin(), found->second.end(), origin) == found->second.end()) found->second.push_back(origin);
      }
    for (std::size_t i = 0; i < entries.size(); ++i) {
      auto entry_pointer = [&] {
        std::string pointer = "/model_catalogue/models/";
        for (const char c : entries[i].id) pointer += c == '~' ? std::string("~0") : c == '/' ? std::string("~1") : std::string(1, c);
        return pointer;
      };
      auto pointer_of = [&](std::string_view family) { return entry_pointer() + "/defaults/" + std::string(family); };
      for (const auto& overlay : entries[i].defaults) {
        const auto* f = policy.family(overlay.family);
        if (!f) fail("/model_catalogue", "declared family");
        const auto pointer = pointer_of(overlay.family);
        auto resolved = over(f->defaults, overlay);
        if (auto error = DescriptorPolicy::check(*f, *policy.validation(f->family), choices(resolved), entries[i].output_limit, false)) fail(pointer, *error);
        if (resolved.strict_tools && f->family != "openai.responses") fail(pointer + "/strict_tools", "Responses strict tools only");
        policy.resolved_.push_back({i, overlay.family, std::move(resolved)});
      }
      // A list shares a value with each family the entry applies to that lists values for the control; otherwise
      // every value would be refused for the model there.
      for (const auto& [name, values] : entries[i].control_values)
        for (const auto& valued : valued_controls) {
          if (control_name(valued.control) != name) continue;
          for (const auto& f : policy.families_) {
            if (!entries[i].families.empty() && !contains(entries[i].families, f.family)) continue;
            const auto& listed = f.*valued.listed;
            if (!listed.empty() && std::none_of(values.begin(), values.end(), [&](const auto& value) { return contains(listed, value); }))
              fail(entry_pointer() + "/control_values/" + name, "values the " + f.family + " family lists");
          }
        }
      // The entry's defaults, over each family, are admitted by the list of the entry that decides the control for
      // its models (DescriptorPolicy::control_value_admitted), at each origin where it applies.
      if (named.empty()) continue;
      for (const auto& f : policy.families_) {
        const auto overlay = std::find_if(entries[i].defaults.begin(), entries[i].defaults.end(),
                                          [&](const auto& o) { return o.family == f.family; });
        const auto resolved = overlay == entries[i].defaults.end() ? f.defaults : over(f.defaults, *overlay);
        const auto effective = choices(resolved);
        for (const auto& valued : valued_controls) {
          const auto& chosen = effective.*valued.chosen;
          const auto name = control_name(valued.control);
          const auto stated = std::find_if(named.begin(), named.end(), [&](const auto& n) { return n.first == name; });
          if (!chosen || stated == named.end()) continue;
          // An entry scoped to origins meets the entries that apply there. An unscoped one meets the scoped entries
          // that name the control at their origins, and the unscoped ones at any other origin, which the empty
          // origin, never an entry's, stands for.
          std::vector<std::string_view> origins(entries[i].origins.begin(), entries[i].origins.end());
          if (origins.empty()) {
            origins.assign(stated->second.begin(), stated->second.end());
            origins.emplace_back();
          }
          for (const auto origin : origins) {
            const auto* decides = DescriptorPolicy::naming(*catalogue, f.family, origin, entries[i].id, name, entries[i].prefix);
            if (!decides) continue;
            const auto& listed = std::find_if(decides->control_values.begin(), decides->control_values.end(),
                                              [&](const auto& c) { return c.first == name; })->second;
            if (!contains(listed, *chosen))
              fail(pointer_of(f.family), "a default " + std::string(name) + " that " + (decides->prefix ? "prefix profile " : "entry ") +
                                             decides->id + " admits in control_values");
          }
        }
      }
    }
    crypto::Sha256 hash;
    hash.update("sp.descriptor-policy.content.v2\n");
    hash.update(policy.pair_digest_);
    hash.update(catalogue->digest());
    // What the policy takes from the embedded policy, so a pin covers it too. A policy that takes nothing keeps the
    // digest it had before.
    if (!policy.borrowed_.empty()) {
      hash.update("sp.descriptor-policy.borrowed.v1\n");
      hash.update(policy.borrowed_);
    }
    const auto digest = hash.finish();
    policy.content_digest_.assign(reinterpret_cast<const char*>(digest.data()), digest.size());
    policy.catalogue_ = std::move(catalogue);
  }
  // A family's `auth` facts: the scheme a chat call sends the key with at the listed origins, the first match winning,
  // and a last fact without origins for every other origin. Origins follow the rules of the cache facts.
  static std::vector<DescriptorPolicy::AuthFact> auth_facts(Value list) {
    if (!list.is_array() || !list.size() || list.size() > 16) fail("/auth", "array of 1 to 16 credential facts");
    std::vector<DescriptorPolicy::AuthFact> facts;
    for (auto item : list.elements()) {
      closed(item, {"scheme"}, "/auth", {"origins"});
      if (!facts.empty() && facts.back().origins.empty()) fail("/auth", "no fact after the one for every origin");
      DescriptorPolicy::AuthFact fact;
      const auto scheme = item.get("scheme");
      if (!scheme.is_string() || !credential_scheme(scheme.as_string())) fail("/auth/scheme", credential_scheme_names());
      fact.scheme = std::string(scheme.as_string());
      if (item.get("origins").valid()) {
        fact.origins = strings(item.get("origins"), "/auth/origins", true);
        for (const auto& origin : fact.origins) {
          if (!valid_origin(origin) || origin.ends_with('/')) fail("/auth/origins", "canonical origins without path");
          canonical_spelling(origin, "/auth/origins");
          for (const auto& earlier : facts)
            if (contains(earlier.origins, origin)) fail("/auth/origins", "each origin in one fact");
        }
      }
      facts.push_back(std::move(fact));
    }
    if (!facts.back().origins.empty()) fail("/auth", "a last fact for every origin");
    return facts;
  }
  // A family's `routes` facts: at most 16, each a buffered and a streaming template with optional origins, as the
  // cache facts have them. An origin is in one fact at most, and one fact at most is for every origin.
  static std::vector<RouteFact> route_facts(Value list, std::string_view family) {
    if (!list.is_array() || list.size() > 16) fail("/routes", "array of at most 16 route facts");
    std::vector<RouteFact> facts;
    auto path = [](Value item, std::string_view pointer) {
      if (!item.is_string() || !valid_route_template(item.as_string()))
        fail(pointer, "origin-relative path template without traversal or URL escapes, with {model} once at most and a name=value query");
      // The policy's own admission check fills the template with a model name, so the template leaves room for it.
      if (model_path(item.as_string(), admission_model).empty())
        fail(pointer, "path template that stays within the path limit with " + std::string(admission_model) + " in place of {model}");
      return std::string(item.as_string());
    };
    for (auto item : list.elements()) {
      closed(item, {"buffered", "streaming"}, "/routes", {"origins"});
      RouteFact fact;
      fact.buffered = path(item.get("buffered"), "/routes/buffered");
      fact.streaming = path(item.get("streaming"), "/routes/streaming");
      const bool named = fact.buffered.find("{model}") != std::string::npos;
      if (named != (fact.streaming.find("{model}") != std::string::npos)) fail("/routes", "{model} in both templates or in neither");
      // A Generate request carries its model in the path alone (descriptor.cpp).
      if (family == "google.generate" && !named) fail("/routes", "Generate templates that name {model}");
      if (item.get("origins").valid()) {
        fact.origins = strings(item.get("origins"), "/routes/origins", true);
        for (const auto& origin : fact.origins) {
          if (!valid_origin(origin) || origin.ends_with('/')) fail("/routes/origins", "canonical origins without path");
          canonical_spelling(origin, "/routes/origins");
          for (const auto& earlier : facts)
            if (contains(earlier.origins, origin)) fail("/routes/origins", "each origin in one fact");
        }
      } else if (std::any_of(facts.begin(), facts.end(), [](const RouteFact& earlier) { return earlier.origins.empty(); })) {
        fail("/routes", "one fact at most for every origin");
      }
      facts.push_back(std::move(fact));
    }
    return facts;
  }
  struct EmbeddedAuth { std::vector<DescriptorPolicy::AuthFact> facts; std::string json; };
  // The embedded policy's `auth` facts of a family, with their canonical JSON, or null when it states none. Parsed
  // once from the embedded text; it never goes through builtin_policy(), which runs this loader.
  static const EmbeddedAuth* embedded_auth(std::string_view family) {
    static const auto tables = [] {
      std::vector<std::pair<std::string, EmbeddedAuth>> parsed_tables;
      auto parsed = json::parse(config_defaults::descriptor_policy_json,
                                {config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth});
      const auto* document = std::get_if<json::Document>(&parsed);
      if (!document) throw std::logic_error("the embedded descriptor policy is not admissible JSON");
      for (auto entry : document->root().get("families").elements())
        if (const auto facts = entry.get("auth"); facts.valid() && entry.get("family").is_string())
          parsed_tables.emplace_back(std::string(entry.get("family").as_string()), EmbeddedAuth{auth_facts(facts), facts.dump()});
      return parsed_tables;
    }();
    for (const auto& [name, auth] : tables)
      if (name == family) return &auth;
    return nullptr;
  }
  // The family the embedded policy states for a listing, if it does.
  static std::optional<std::string> embedded_listing(Listing listing) {
    const auto facts = embedded_root().get("listings");
    if (facts.valid() && facts.is_array())
      for (auto item : facts.elements())
        if (item.get("listing").is_string() && item.get("listing").as_string() == listing_name(listing) && item.get("family").is_string())
          return std::string(item.get("family").as_string());
    return std::nullopt;
  }
  // The embedded policy's document. It is parsed once and never goes through builtin_policy(), which runs this loader.
  static Value embedded_root() {
    static const auto parsed = json::parse(config_defaults::descriptor_policy_json,
                                           {config_defaults::codec_admission_policy_bytes, config_defaults::codec_admission_policy_depth});
    const auto* document = std::get_if<json::Document>(&parsed);
    if (!document) throw std::logic_error("the embedded descriptor policy is not admissible JSON");
    return document->root();
  }
  // A member of a family in the embedded policy, or an invalid value when it states none.
  static Value embedded_member(std::string_view family, std::string_view key) {
    for (auto entry : embedded_root().get("families").elements())
      if (entry.get("family").is_string() && entry.get("family").as_string() == family) return entry.get(key);
    return {};
  }
  // A member of the first endpoint fact of `endpoint` and `shape` in the embedded policy, or an invalid value.
  static Value embedded_endpoint_member(std::string_view endpoint, std::string_view shape, std::string_view key) {
    for (auto entry : embedded_root().get("endpoints").elements())
      if (entry.get("endpoint").is_string() && entry.get("endpoint").as_string() == endpoint &&
          entry.get("shape").is_string() && entry.get("shape").as_string() == shape) return entry.get(key);
    return {};
  }
  static RequestDefaults over(const RequestDefaults& base, const DefaultsOverlay& o) {
    RequestDefaults r = base;
    if (o.max_output_tokens) r.max_output_tokens = *o.max_output_tokens;
    if (o.thinking_budget) r.thinking_budget = *o.thinking_budget;
    if (o.include_thoughts) r.include_thoughts = *o.include_thoughts;
    if (o.thinking_summaries) r.thinking_summaries = *o.thinking_summaries;
    if (o.strict_tools) r.strict_tools = *o.strict_tools;
    if (o.reasoning_enabled) r.reasoning_enabled = *o.reasoning_enabled;
    if (o.reasoning_effort) r.reasoning_effort = *o.reasoning_effort;
    if (o.reasoning_summary) r.reasoning_summary = *o.reasoning_summary;
    if (o.thinking_level) r.thinking_level = *o.thinking_level;
    if (o.service_tier) r.service_tier = *o.service_tier;
    if (o.temperature) r.temperature = *o.temperature;
    if (o.top_p) r.top_p = *o.top_p;
    return r;
  }
};
std::string_view DescriptorPolicy::listing_family(Listing listing) const noexcept {
  for (const auto& [kind, family] : listings_) if (kind == listing) return family;
  return {};
}
const std::vector<RouteFact>& DescriptorPolicy::routes(std::string_view family) const noexcept {
  for (const auto& [name, facts] : routes_) if (name == family) return facts;
  static const std::vector<RouteFact> none;
  return none;
}
const RouteFact* DescriptorPolicy::route(std::string_view family, std::string_view origin) const noexcept {
  const auto& facts = routes(family);
  for (const auto& fact : facts) if (contains(fact.origins, origin)) return &fact;
  for (const auto& fact : facts) if (fact.origins.empty()) return &fact;
  return nullptr;
}
std::optional<std::string> route_refusal(const ValidatedDescriptor& descriptor, std::string_view model) {
  // The model takes the place of {model} in the path, so it may not add, remove or replace a segment, and the path
  // stays within its bound.
  if (descriptor.model_routed() && (descriptor.resolved_path(false, model).empty() || descriptor.resolved_path(true, model).empty()))
    return "model name cannot take the place of {model} in the descriptor's paths";
  return std::nullopt;
}
std::optional<std::string> literal_route_refusal(const ValidatedDescriptor& descriptor, std::string_view model) {
  if (const auto named = descriptor.route_model(); !named.empty() && named != model) return "the descriptor's paths name another model";
  return std::nullopt;
}
const DescriptorPolicy::ValidationFacts* DescriptorPolicy::validation(std::string_view name) const noexcept {
  for (const auto& [family, facts] : validation_) if (family == name) return &facts;
  return nullptr;
}
// The checks of the effective controls that need no model: the family's own lists and ranges, the rules that bind the
// members to each other, and the controls the family's typed request carries. They run in this order, and the first
// failure is the answer.
std::optional<std::string> DescriptorPolicy::check(const FamilyPolicy& f, const ValidationFacts& rules, const EffectiveChoices& c, std::optional<std::uint64_t> limit, bool require_cap) {
  const auto carries = [&](Control control) { return (rules.controls >> static_cast<unsigned>(control) & 1u) != 0; };
  if ((require_cap && f.required_output_cap && !c.max_output_tokens) || (c.max_output_tokens && !*c.max_output_tokens)) return "positive output cap required";
  if (limit && c.max_output_tokens && *c.max_output_tokens > *limit) return "output cap exceeds admitted model limit";
  if (rules.output_cap_maximum && c.max_output_tokens && *c.max_output_tokens > *rules.output_cap_maximum) return "output cap is not representable as int32";
  auto valid_number=[](const auto& value,const auto& bounds){return !value || (std::isfinite(*value) && *value>=bounds[0] && *value<=bounds[1]);};
  if(!valid_number(c.temperature,f.temperature_range) || !valid_number(c.top_p,f.top_p_range)) return "sampling parameter outside admitted range";
  if(c.thinking_budget && (*c.thinking_budget<f.thinking_minimum || (c.max_output_tokens && (rules.thinking_budget_below_cap ? *c.thinking_budget>=*c.max_output_tokens : *c.thinking_budget>*c.max_output_tokens)))) return "thinking budget incompatible with output cap";
  if (rules.thinking_budget_excludes_level && c.thinking_budget && c.thinking_level) return "thinking budget and level are mutually exclusive";
  if(rules.thinking_budget_excludes_temperature && c.thinking_budget && (c.temperature || (c.top_p && *c.top_p<f.thinking_top_p_minimum))) return "manual thinking sampling parameters are incompatible";
  auto valid_text=[](const auto& value,const auto& allowed){return !value || contains(allowed,*value);};
  // A family that carries neither sampling control refuses either; one that carries temperature only refuses top_p.
  if ((c.temperature && !carries(Control::Temperature)) || (c.top_p && !carries(Control::Temperature) && !carries(Control::TopP))) return "sampling control unsupported by typed request";
  if (c.top_p && !carries(Control::TopP)) return "top_p unsupported by typed Generate request";
  if(!valid_text(c.reasoning_effort,f.reasoning_efforts) || !valid_text(c.reasoning_summary,f.reasoning_summaries) || !valid_text(c.thinking_level,f.thinking_levels) || !valid_text(c.service_tier,f.service_tiers)) return "unsupported typed semantic control";
  if(c.reasoning_enabled && !(carries(Control::ReasoningEffort) && carries(Control::ReasoningSummary))) return "reasoning object unsupported for family";
  if(c.reasoning_enabled && (!c.reasoning_effort || !c.reasoning_summary)) return "complete effective reasoning options required";
  if(c.include_thoughts && !carries(Control::IncludeThoughts))return "includeThoughts unsupported for family";
  if(c.thinking_summaries && !carries(Control::ThinkingSummaries))return "thinking summaries unsupported for family";
  if(c.thinking_budget && !carries(Control::ThinkingBudget))return "thinking budget unsupported for family";
  if(c.reasoning_effort && !carries(Control::ReasoningEffort)) return "reasoning effort unsupported for family";
  if(c.reasoning_summary && !carries(Control::ReasoningSummary)) return "reasoning summary unsupported for family";
  if(c.thinking_level && !carries(Control::ThinkingLevel)) return "thinking level unsupported for family";
  if(c.service_tier && !carries(Control::ServiceTier)) return "service tier unsupported for family";
  return {};
}
const FamilyPolicy* DescriptorPolicy::family(std::string_view name) const noexcept { for(const auto& f:families_)if(f.family==name)return &f; return nullptr; }
ModelMatch DescriptorPolicy::model(std::string_view name, std::string_view origin, std::string_view model) const noexcept {
  return catalogue_ ? catalogue_->find(name, origin, model) : ModelMatch{};
}
const RequestDefaults& DescriptorPolicy::defaults(std::string_view name, std::string_view origin, std::string_view model) const noexcept {
  if (const auto match = this->model(name, origin, model); match.entry) {
    const auto index = static_cast<std::size_t>(match.entry - catalogue_->entries().data());
    for (const auto& resolved : resolved_)
      if (resolved.entry == index && resolved.family == name) return resolved.defaults;
  }
  return family(name)->defaults;
}
std::optional<std::uint64_t> DescriptorPolicy::output_limit(std::string_view name, std::string_view origin, std::string_view model) const noexcept {
  const auto match = this->model(name, origin, model);
  return match.entry ? match.entry->output_limit : std::nullopt;
}
std::optional<std::uint64_t> DescriptorPolicy::input_limit(std::string_view name, std::string_view origin, std::string_view model) const noexcept {
  const auto match = this->model(name, origin, model);
  return match.entry ? match.entry->input_limit : std::nullopt;
}
PolicyResult DescriptorPolicy::with_catalogue(CatalogueSnapshot catalogue) const {
  try {
    auto copy = std::shared_ptr<DescriptorPolicy>(new DescriptorPolicy(*this));
    PolicyLoader::attach(*copy, std::move(catalogue));
    return PolicySnapshot(std::move(copy));
  } catch (ConfigError& error) { return std::move(error); }
}
namespace {
std::string lower_hex(std::string_view bytes) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    out.push_back(hex[(byte >> 4) & 0x0fU]);
    out.push_back(hex[byte & 0x0fU]);
  }
  return out;
}
} // namespace
std::string DescriptorPolicy::identity_hex() const { return lower_hex(identity_); }
std::string DescriptorPolicy::content_digest_hex() const { return lower_hex(content_digest_); }
PolicyResult load_policy(std::string_view f,std::string_view r){return PolicyLoader{}.run(f,r,builtin_catalogue());}
PolicyResult load_policy(std::string_view f,std::string_view r,CatalogueSnapshot c){return PolicyLoader{}.run(f,r,std::move(c));}
namespace {
bool matches_sha256_hex(std::string_view actual_lower_hex, std::string_view expected_hex) noexcept {
  if (expected_hex.size() != 64 || actual_lower_hex.size() != 64) return false;
  for (std::size_t i = 0; i < 64; ++i) {
    char c = expected_hex[i];
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c + ('a' - 'A'));
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    if (c != actual_lower_hex[i]) return false;
  }
  return true;
}
std::variant<std::string, ConfigError> read_bounded_policy_file(
    std::string_view path, std::size_t max_bytes, std::string_view pointer) {
  if (path.empty() || path.find('\0') != std::string_view::npos) {
    return ConfigError{std::string(pointer), "readable bounded policy file", 0,
                       "policy file path is invalid"};
  }
  std::ifstream stream{std::string(path), std::ios::binary};
  if (!stream.is_open()) {
    return ConfigError{std::string(pointer), "readable bounded policy file", 0,
                       "policy file could not be opened"};
  }
  std::string contents;
  char buffer[4096];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
    const auto count = static_cast<std::size_t>(stream.gcount());
    if (contents.size() + count > max_bytes) {
      return ConfigError{std::string(pointer), "policy file within admission byte limit", 0,
                         "policy file exceeds admission byte limit"};
    }
    contents.append(buffer, count);
  }
  if (stream.bad()) {
    return ConfigError{std::string(pointer), "readable bounded policy file", 0,
                       "policy file read failed"};
  }
  return contents;
}
} // namespace
namespace {
PolicyResult verified_policy(std::string_view family_json, std::string_view resource_json, CatalogueSnapshot catalogue,
                             std::string_view expected_sha256_hex) {
  auto loaded = load_policy(family_json, resource_json, std::move(catalogue));
  auto* snapshot = std::get_if<PolicySnapshot>(&loaded);
  if (!snapshot || expected_sha256_hex.empty()) return loaded;
  if (!matches_sha256_hex((*snapshot)->content_digest_hex(), expected_sha256_hex)) {
    return ConfigError{"/content_digest", "matching 64-character SHA-256 policy content digest", 0,
                       "policy content SHA-256 digest mismatch"};
  }
  return loaded;
}
} // namespace
PolicyResult load_verified_policy(std::string_view family_json, std::string_view resource_json,
                                  std::string_view expected_sha256_hex) {
  return verified_policy(family_json, resource_json, builtin_catalogue(), expected_sha256_hex);
}
PolicyResult load_policy_files(std::string_view family_json_path, std::string_view resource_json_path,
                               std::string_view expected_sha256_hex) {
  auto family_file = read_bounded_policy_file(
      family_json_path, config_defaults::codec_admission_policy_bytes, "/descriptor_policy");
  if (auto* error = std::get_if<ConfigError>(&family_file)) return std::move(*error);
  auto resource_file = read_bounded_policy_file(
      resource_json_path, config_defaults::codec_admission_policy_bytes, "/codec_defaults");
  if (auto* error = std::get_if<ConfigError>(&resource_file)) return std::move(*error);
  return load_verified_policy(std::get<std::string>(family_file),
                              std::get<std::string>(resource_file),
                              expected_sha256_hex);
}
PolicyResult load_policy_bundle(std::string_view bundle_json,
                                std::string_view expected_sha256_hex) {
  const json::Limits limits{config_defaults::codec_admission_policy_bytes * 2 + bundle_catalogue_bytes,
                            std::max<std::size_t>(config_defaults::codec_admission_policy_depth, 6) + 2};
  auto parsed = json::parse(bundle_json, limits);
  if (auto* error = std::get_if<json::ParseError>(&parsed)) {
    return ConfigError{error->pointer, "valid JSON policy bundle", 0,
                       "policy bundle JSON rejected"};
  }
  try {
    const auto root = std::get<json::Document>(parsed).root();
    closed(root, {"version", "descriptor_policy", "codec_defaults"}, "", {"model_catalogue"});
    if (!root.get("version").is_uint() || root.get("version").as_uint() != 1)
      fail("/version", "integer version 1");
    const auto family_node = root.get("descriptor_policy");
    const auto resource_node = root.get("codec_defaults");
    if (!family_node.is_object()) fail("/descriptor_policy", "descriptor policy object");
    if (!resource_node.is_object()) fail("/codec_defaults", "codec defaults object");
    auto catalogue = builtin_catalogue();
    if (const auto node = root.get("model_catalogue"); node.valid()) {
      if (!node.is_object()) fail("/model_catalogue", "model catalogue object");
      auto loaded = load_catalogue(node.dump());
      if (auto* error = std::get_if<ConfigError>(&loaded)) {
        error->pointer = "/model_catalogue" + error->pointer;
        return std::move(*error);
      }
      catalogue = std::get<CatalogueSnapshot>(std::move(loaded));
    }
    return verified_policy(family_node.dump(), resource_node.dump(), std::move(catalogue), expected_sha256_hex);
  } catch (ConfigError& error) {
    return std::move(error);
  }
}
PolicyResult load_policy_bundle_file(std::string_view bundle_path,
                                     std::string_view expected_sha256_hex) {
  auto bundle_file = read_bounded_policy_file(
      bundle_path, config_defaults::codec_admission_policy_bytes * 2 + bundle_catalogue_bytes, "");
  if (auto* error = std::get_if<ConfigError>(&bundle_file)) return std::move(*error);
  return load_policy_bundle(std::get<std::string>(bundle_file), expected_sha256_hex);
}
std::string builtin_policy_bundle_json() {
  return std::string("{\"version\":1,\"descriptor_policy\":") +
         std::string(config_defaults::descriptor_policy_json) +
         ",\"codec_defaults\":" +
         std::string(config_defaults::codec_defaults_json) +
         ",\"model_catalogue\":" + builtin_catalogue_json() + "}";
}
PolicySnapshot builtin_policy(){static const auto value=[] {auto r=load_policy(config_defaults::descriptor_policy_json,config_defaults::codec_defaults_json,builtin_catalogue()); if(auto* e=std::get_if<ConfigError>(&r))throw std::logic_error(e->message);return std::get<PolicySnapshot>(std::move(r));}();return value;}
std::optional<std::string_view> borrowed(const std::optional<std::string>& value){if(value)return *value;return {};}
bool contains(const std::vector<std::string>& values,std::string_view value){return std::find(values.begin(),values.end(),value)!=values.end();}
EffectiveChoices effective_defaults(const ValidatedDescriptor& d,std::string_view model){return choices(model_defaults(d,model));}
const RequestDefaults& model_defaults(const ValidatedDescriptor& d, std::string_view model) noexcept {
  return d.policy()->defaults(d.family(), d.base_url(), model);
}
ModelMatch model_match(const ValidatedDescriptor& d, std::string_view model) noexcept {
  return d.policy()->model(d.family(), d.base_url(), model);
}
const MediaPolicy& media_policy(const ValidatedDescriptor& d) noexcept {
  const auto& family = d.family_policy();
  return family.openrouter_media && routed_gateway(d) ? *family.openrouter_media : family.media;
}
std::optional<CacheCapabilities> cache_capabilities(const ValidatedDescriptor& d, std::string_view model) noexcept {
  return d.policy()->cache_capabilities(d.family(), d.base_url(), model);
}
bool routed_gateway(const ValidatedDescriptor& d) noexcept { return contains(d.family_policy().openrouter_origins, d.base_url()); }
bool DescriptorPolicy::temperature_forbidden_by_generation(std::string_view, std::string_view) const noexcept { return false; }
std::optional<bool> DescriptorPolicy::temperature_admitted(std::string_view family, std::string_view origin, std::string_view model) const noexcept {
  if (!catalogue_) return std::nullopt;
  // An entry states the fact with `temperature`, or with a parameter list that names the control or does not.
  const auto* entry = catalogue_->for_each_match(family, origin, model, [](const CatalogueEntry& e, const void*) noexcept {
    return e.temperature.has_value() || !e.parameters.empty();
  }, nullptr);
  if (!entry) return std::nullopt;
  if (entry->temperature) return *entry->temperature;
  return contains(entry->parameters, control_name(Control::Temperature));
}
std::optional<bool> DescriptorPolicy::control_value_admitted(std::string_view family, std::string_view origin, std::string_view model,
                                                             Control control, std::string_view value) const noexcept {
  if (!catalogue_ || !control_takes_values(control)) return std::nullopt;
  const auto name = control_name(control);
  const auto* entry = naming(*catalogue_, family, origin, model, name, false);
  if (!entry) return std::nullopt;
  for (const auto& [listed, values] : entry->control_values)
    if (listed == name) return contains(values, value);
  return std::nullopt;
}
const CatalogueEntry* DescriptorPolicy::naming(const ModelCatalogue& catalogue, std::string_view family, std::string_view origin,
                                               std::string_view model, std::string_view control, bool profiles_only) noexcept {
  return catalogue.for_each_match(family, origin, model, [](const CatalogueEntry& e, const void* context) noexcept {
    const auto wanted = *static_cast<const std::string_view*>(context);
    return std::any_of(e.control_values.begin(), e.control_values.end(), [&](const auto& listed) { return listed.first == wanted; });
  }, &control, profiles_only);
}
namespace {
constexpr std::string_view control_names[control_count]{
    "max_output_tokens", "temperature", "top_p", "thinking_budget", "include_thoughts", "reasoning_effort",
    "reasoning_summary", "thinking_level", "thinking_summaries", "service_tier", "required_tool", "store",
    "max_tool_calls", "tool_choice"};
} // namespace
std::string_view control_name(Control control) noexcept {
  const auto index = static_cast<std::size_t>(control);
  return index < control_count ? control_names[index] : std::string_view{};
}
std::optional<Control> control_from_name(std::string_view name) noexcept {
  for (std::size_t i = 0; i < control_count; ++i)
    if (control_names[i] == name) return static_cast<Control>(i);
  return std::nullopt;
}
bool control_takes_values(Control control) noexcept {
  return std::any_of(std::begin(valued_controls), std::end(valued_controls), [&](const auto& valued) { return valued.control == control; });
}
std::optional<bool> DescriptorPolicy::control_admitted(std::string_view family, Control control) const noexcept {
  const auto index = static_cast<std::size_t>(control);
  if (index >= control_count) return std::nullopt;
  for (const auto& [name, bits] : controls_)
    if (name == family) return (bits >> index & 1u) != 0;
  return std::nullopt;
}
std::string_view DescriptorPolicy::billing_tier(std::string_view family, std::string_view tier) const noexcept {
  for (const auto& [name, standard] : standard_tiers_)
    if (name == family && contains(standard, tier)) return {};
  return tier;
}
bool family_admits_routing(const FamilyPolicy& family) noexcept { return !family.openrouter_origins.empty(); }
std::string tool_fee_unit(std::string_view type) { return std::string(tool_fee_prefix) + std::string(type); }
ToolFacts tool_facts(const FamilyPolicy& family, std::string_view type) {
  ToolFacts facts;
  facts.fee_unit = tool_fee_unit(type);
  for (const auto& tool : family.server_tools)
    if (tool.type == type) {
      facts.known = true;
      facts.client = tool.client;
      facts.admits_max_uses = tool.max_uses;
      return facts;
    }
  // An OpenRouter server tool is a gateway's fact: the service runs it and it takes no `max_uses`.
  for (const auto& tool : family.openrouter_server_tools)
    if (tool.name == type) facts.known = true;
  return facts;
}
std::optional<CacheCapabilities> DescriptorPolicy::cache_capabilities(std::string_view name, std::string_view origin, std::string_view model) const noexcept {
  const auto* family = this->family(name);
  if (!family) return std::nullopt;
  for (const auto& fact : family->cache) {
    if (!fact.origins.empty() && !contains(fact.origins, origin)) continue;
    if (!fact.routes.empty() && std::none_of(fact.routes.begin(), fact.routes.end(), [&](const auto& route) { return model.starts_with(route); })) continue;
    return fact.capabilities;
  }
  return std::nullopt;
}
std::string_view DescriptorPolicy::auth_scheme(std::string_view family, std::string_view origin) const noexcept {
  for (const auto& [name, facts] : auth_)
    if (name == family)
      for (const auto& fact : facts)
        if (fact.origins.empty() || contains(fact.origins, origin)) return fact.scheme;
  return {};
}
std::string_view auth_scheme(const ValidatedDescriptor& d) noexcept {
  if (const auto stated = d.auth(); !stated.empty()) return stated;
  return d.policy()->auth_scheme(d.family(), d.base_url());
}
const std::array<CredentialScheme, credential_scheme_count>& credential_schemes() noexcept { return scheme_table; }
const CredentialScheme* credential_scheme(std::string_view scheme) noexcept {
  for (const auto& known : scheme_table)
    if (known.scheme == scheme) return &known;
  return nullptr;
}
std::string credential_scheme_names() {
  std::string names;
  for (std::size_t i = 0; i < scheme_table.size(); ++i) {
    if (i) names += i + 1 == scheme_table.size() ? " or " : ", ";
    names += scheme_table[i].scheme;
  }
  return names;
}
std::optional<std::pair<std::string, std::string>> credential_header(std::string_view scheme, std::string_view key) {
  const auto* known = credential_scheme(scheme);
  if (!known) return std::nullopt;
  return std::pair<std::string, std::string>{std::string(known->header), std::string(known->prefix) + std::string(key)};
}
bool credential_header_name(std::string_view name) noexcept {
  const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
  return std::any_of(scheme_table.begin(), scheme_table.end(), [&](const CredentialScheme& known) {
    return known.header.size() == name.size() &&
           std::equal(name.begin(), name.end(), known.header.begin(), [&](char a, char b) {
             return fold(static_cast<unsigned char>(a)) == fold(static_cast<unsigned char>(b));
           });
  });
}
const EndpointFact* DescriptorPolicy::endpoint(std::string_view endpoint, std::string_view origin) const noexcept {
  for (const auto& fact : endpoints_)
    if (fact.endpoint == endpoint && std::find(fact.origins.begin(), fact.origins.end(), origin) != fact.origins.end()) return &fact;
  return nullptr;
}
std::optional<std::string_view> endpoint_path(const EndpointFact& fact, std::string_view operation) noexcept {
  for (const auto& [name, path] : fact.paths) if (name == operation) return path;
  return std::nullopt;
}
EndpointBilling DescriptorPolicy::endpoint_billing(std::string_view endpoint, std::string_view origin, std::string_view operation) const noexcept {
  for (std::size_t i = 0; i < endpoints_.size() && i < billing_.size(); ++i) {
    const auto& fact = endpoints_[i];
    if (fact.endpoint != endpoint || !contains(fact.origins, origin)) continue;
    // One value for the whole endpoint is kept under an empty operation; any other entry names its operation.
    for (const auto& [name, value] : billing_[i].operations)
      if (name.empty() || name == operation) return value;
    return EndpointBilling::Unstated;
  }
  return EndpointBilling::Unstated;
}
std::optional<std::uint64_t> DescriptorPolicy::limit(std::string_view family, std::string_view name) const noexcept {
  for (const auto& [listed, values] : limits_)
    if (listed == family)
      for (const auto& [limit, value] : values)
        if (limit == name) return value;
  return std::nullopt;
}
std::optional<std::uint64_t> DescriptorPolicy::endpoint_limit(std::string_view endpoint, std::string_view origin, std::string_view name) const noexcept {
  if (name != "max_items") return std::nullopt;
  for (std::size_t i = 0; i < endpoints_.size() && i < max_items_.size(); ++i)
    if (endpoints_[i].endpoint == endpoint && contains(endpoints_[i].origins, origin)) return max_items_[i];
  return std::nullopt;
}
bool over_limit(const ValidatedDescriptor& descriptor, std::string_view name, std::uint64_t count) noexcept {
  const auto limit = descriptor.policy()->limit(descriptor.family(), name);
  return limit && count > *limit;
}
std::string_view DescriptorPolicy::carried_tier(std::string_view endpoint, std::string_view origin) const noexcept {
  for (std::size_t i = 0; i < endpoints_.size() && i < billing_.size(); ++i)
    if (endpoints_[i].endpoint == endpoint && contains(endpoints_[i].origins, origin)) return billing_[i].carried_tier;
  return {};
}
std::string_view DescriptorPolicy::default_output_cap_member(std::string_view family, std::string_view origin) const noexcept {
  for (const auto& [listed_family, listed_origin] : completion_cap_origins_)
    if (listed_family == family && listed_origin == origin) return "max_completion_tokens";
  const auto* declared = this->family(family);
  return declared ? std::string_view(declared->cap_member) : std::string_view{};
}
bool temperature_forbidden(const ValidatedDescriptor& d, std::string_view model) {
  return d.policy()->temperature_admitted(d.family(), d.base_url(), model) == false;
}
std::optional<std::string> validate_choices(const ValidatedDescriptor& d,std::string_view model,const EffectiveChoices& c) {
  if (c.temperature && temperature_forbidden(d, model)) return "temperature is prohibited for the admitted model";
  // A family the policy has no facts for carries no control, so every control set is refused.
  static const DescriptorPolicy::ValidationFacts none{};
  const auto* rules = d.policy()->validation(d.family());
  if (auto error = DescriptorPolicy::check(d.family_policy(), rules ? *rules : none, c, d.policy()->output_limit(d.family(),d.base_url(),model), true)) return error;
  // The family admits the value; the model's catalogue facts may narrow it.
  for (const auto& valued : valued_controls)
    if (const auto& chosen = c.*valued.chosen;
        chosen && d.policy()->control_value_admitted(d.family(), d.base_url(), model, valued.control, *chosen) == false)
      return "control value is not admitted for the model";
  return {};
}
std::uint32_t interface_revision() noexcept { return 7; }
} // namespace sp::descriptor
