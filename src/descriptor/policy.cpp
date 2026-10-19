#include "descriptor/policy.h"
#include "descriptor/owned_keys.h"
#include "crypto/crypto.h"
#include "sp/config_defaults.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace sp::descriptor {
namespace {
using json::Value;
// A bundle may carry a refreshed catalogue, which load_catalogue bounds to 8 MiB.
constexpr std::size_t bundle_catalogue_bytes = 8u * 1024u * 1024u;
[[noreturn]] void fail(std::string_view path, std::string_view expected) {
  throw ConfigError{std::string(path), std::string(expected), 0, "descriptor policy value rejected"};
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
StopKind stop(Value v) {
  const auto s=text(v,"/stop_reasons");
  if (s == "Unknown") return StopKind::Unknown;
  if (s == "EndTurn") return StopKind::EndTurn;
  if (s == "MaxTokens") return StopKind::MaxTokens;
  if (s == "ToolUse") return StopKind::ToolUse;
  if (s == "ContentFilter") return StopKind::ContentFilter;
  if (s == "Refusal") return StopKind::Refusal;
  if (s == "PauseTurn") return StopKind::PauseTurn;
  if (s == "ContextLimit") return StopKind::ContextLimit;
  if (s == "StopSequence") return StopKind::StopSequence;
  if (s == "MalformedCall") return StopKind::MalformedCall;
  if (s == "Compaction") return StopKind::Compaction;
  fail("/stop_reasons","typed StopKind");
}
// Stop safety remains compiled; policy may add facts, never rewrite standard causality.
bool safe_stop(std::string_view family, std::string_view raw, StopKind kind) {
  if(raw=="error" || raw=="failed" || raw=="cancelled" || raw=="incomplete") return false;
  struct Rule { std::string_view family, raw; StopKind kind; };
  static constexpr Rule rules[]{
    {"openai.chat","stop",StopKind::EndTurn},{"openai.chat","length",StopKind::MaxTokens},{"openai.chat","tool_calls",StopKind::ToolUse},{"openai.chat","content_filter",StopKind::ContentFilter},
    {"anthropic.messages","end_turn",StopKind::EndTurn},{"anthropic.messages","max_tokens",StopKind::MaxTokens},{"anthropic.messages","tool_use",StopKind::ToolUse},{"anthropic.messages","pause_turn",StopKind::PauseTurn},{"anthropic.messages","compaction",StopKind::Compaction},{"anthropic.messages","refusal",StopKind::Refusal},{"anthropic.messages","stop_sequence",StopKind::StopSequence},{"anthropic.messages","model_context_window_exceeded",StopKind::ContextLimit},
    {"openai.responses","completed",StopKind::EndTurn},{"openai.responses","max_output_tokens",StopKind::MaxTokens},{"openai.responses","content_filter",StopKind::ContentFilter},
    // google.generate mirrors the Gemini codec's compiled finishReason mapping (codecs/gemini.cpp).
    {"google.generate","STOP",StopKind::EndTurn},{"google.generate","MAX_TOKENS",StopKind::MaxTokens},
    {"google.generate","SAFETY",StopKind::ContentFilter},{"google.generate","RECITATION",StopKind::ContentFilter},{"google.generate","BLOCKLIST",StopKind::ContentFilter},
    {"google.generate","PROHIBITED_CONTENT",StopKind::ContentFilter},{"google.generate","SPII",StopKind::ContentFilter},{"google.generate","IMAGE_SAFETY",StopKind::ContentFilter},
    {"google.generate","IMAGE_PROHIBITED_CONTENT",StopKind::ContentFilter},{"google.generate","IMAGE_RECITATION",StopKind::ContentFilter},
    {"google.generate","ESCALATION",StopKind::ContentFilter},{"google.generate","PUP_LIMITED_DISABLED",StopKind::ContentFilter},
    {"google.generate","MALFORMED_FUNCTION_CALL",StopKind::MalformedCall},{"google.generate","UNEXPECTED_TOOL_CALL",StopKind::MalformedCall},{"google.generate","TOO_MANY_TOOL_CALLS",StopKind::MalformedCall},
    {"google.interactions","completed",StopKind::EndTurn},{"google.interactions","requires_action",StopKind::ToolUse},{"google.interactions","max_output_tokens",StopKind::MaxTokens}};
  for (const auto& r:rules) if(r.family==family && r.raw==raw) return r.kind==kind;
  return true;
}
std::array<double,2> range(Value v) { if(!v.is_array() || v.size()!=2)fail("/range","two finite bounds"); auto a=number(v.at(0),"/range"), b=number(v.at(1),"/range"); if(a>b)fail("/range","ordered bounds"); return {a,b}; }
EffectiveChoices choices(const RequestDefaults& d) {
  return {d.max_output_tokens,d.thinking_budget,d.include_thoughts,d.thinking_summaries,d.reasoning_enabled,borrowed(d.reasoning_effort),borrowed(d.reasoning_summary),borrowed(d.thinking_level),borrowed(d.service_tier),d.temperature,d.top_p};
}
std::optional<std::string> validate(const FamilyPolicy& f, const EffectiveChoices& c, std::optional<std::uint64_t> limit, bool require_cap) {
  if ((require_cap && f.required_output_cap && !c.max_output_tokens) || (c.max_output_tokens && !*c.max_output_tokens)) return "positive output cap required";
  if (limit && c.max_output_tokens && *c.max_output_tokens > *limit) return "output cap exceeds admitted model limit";
  if (f.family=="google.interactions" && c.max_output_tokens && *c.max_output_tokens > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) return "output cap is not representable as int32";
  auto valid_number=[](const auto& value,const auto& bounds){return !value || (std::isfinite(*value) && *value>=bounds[0] && *value<=bounds[1]);};
  if(!valid_number(c.temperature,f.temperature_range) || !valid_number(c.top_p,f.top_p_range)) return "sampling parameter outside admitted range";
  if(c.thinking_budget && (*c.thinking_budget<f.thinking_minimum || (c.max_output_tokens && (f.family=="anthropic.messages" ? *c.thinking_budget>=*c.max_output_tokens : *c.thinking_budget>*c.max_output_tokens)))) return "thinking budget incompatible with output cap";
  if (f.family=="google.generate" && c.thinking_budget && c.thinking_level) return "thinking budget and level are mutually exclusive";
  if(f.family=="anthropic.messages" && c.thinking_budget && (c.temperature || (c.top_p && *c.top_p<f.thinking_top_p_minimum))) return "manual thinking sampling parameters are incompatible";
  auto valid_text=[](const auto& value,const auto& allowed){return !value || contains(allowed,*value);};
  if (f.family=="google.interactions" && (c.temperature || c.top_p)) return "sampling control unsupported by typed request";
  if (f.family=="google.generate" && c.top_p) return "top_p unsupported by typed Generate request";
  if(!valid_text(c.reasoning_effort,f.reasoning_efforts) || !valid_text(c.reasoning_summary,f.reasoning_summaries) || !valid_text(c.thinking_level,f.thinking_levels) || !valid_text(c.service_tier,f.service_tiers)) return "unsupported typed semantic control";
  if(c.reasoning_enabled && f.family!="openai.responses") return "reasoning object unsupported for family";
  if(c.reasoning_enabled && (!c.reasoning_effort || !c.reasoning_summary)) return "complete effective reasoning options required";
  if(c.include_thoughts && f.family!="google.generate")return "includeThoughts unsupported for family";
  if(c.thinking_summaries && f.family!="google.interactions")return "thinking summaries unsupported for family";
  if(c.thinking_budget && f.family!="anthropic.messages" && f.family!="google.generate")return "thinking budget unsupported for family";
  if(c.reasoning_effort && f.family!="openai.chat" && f.family!="openai.responses") return "reasoning effort unsupported for family";
  if(c.reasoning_summary && f.family!="openai.responses") return "reasoning summary unsupported for family";
  if(c.thinking_level && f.family!="google.interactions" && f.family!="google.generate") return "thinking level unsupported for family";
  if(c.service_tier && f.family!="openai.chat" && f.family!="openai.responses" && f.family!="google.interactions") return "service tier unsupported for family";
  return {};
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
class PolicyLoader {
 public:
  PolicyResult run(std::string_view source,std::string_view resources,CatalogueSnapshot catalogue) {
    std::uint64_t revision = 0;
    try {
      auto p=json::parse(source,{config_defaults::codec_admission_policy_bytes,config_defaults::codec_admission_policy_depth});
      auto r=json::parse(resources,{config_defaults::codec_admission_policy_bytes,config_defaults::codec_admission_policy_depth});
      auto* pd=std::get_if<json::Document>(&p); auto* rd=std::get_if<json::Document>(&r);
      if (pd && pd->root().get("revision").is_uint()) revision = pd->root().get("revision").as_uint();
      if(!pd || !rd)fail("","strict bounded JSON policy");
      auto policy=std::shared_ptr<DescriptorPolicy>(new DescriptorPolicy);
      auto root=pd->root(); auto rr=rd->root();
      closed(root,{"version","revision","families"},"",{"endpoints"}); closed(rr,{"version","resources","admission"},"/resources");
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
        closed(v,{"family","bindings","stop_reasons","defaults","required_output_cap","temperature_range","top_p_range","thinking_minimum","thinking_top_p_minimum","reasoning_efforts","reasoning_summaries","thinking_levels","service_tiers","header_versions","header_version","server_tools","openrouter_origins","temperature_forbidden_model_prefixes","media","openrouter_media"},"/families",{"temperature_forbidden_generations","max_completion_tokens_origins","unreplayable_parts","extensions","openrouter_plugins","openrouter_server_tools","betas","response_blocks","input_blocks"});
        FamilyPolicy f; f.family=text(v.get("family"),"/families");
        if(f.family!="openai.chat" && f.family!="anthropic.messages" && f.family!="openai.responses" && f.family!="google.generate" && f.family!="google.interactions")fail("/families","typed family");
        if(policy->family(f.family))fail("/families","unique families");
        auto b=v.get("bindings"); closed(b,{"model","messages","stream","max_output_tokens","usage"},"/bindings");
        f.model_member=text(b.get("model"),"/bindings"); f.messages_member=text(b.get("messages"),"/bindings"); f.stream_member=text(b.get("stream"),"/bindings"); f.cap_member=text(b.get("max_output_tokens"),"/bindings"); f.usage_path=strings(b.get("usage"),"/bindings/usage",true);
        auto stops=v.get("stop_reasons"); if(!stops.is_object() || stops.size()>64)fail("/stop_reasons","bounded stop facts");
        for(auto s:stops.members()){auto kind=stop(s.value); if(s.key.empty() || s.key.size()>128 || !safe_stop(f.family,s.key,kind))fail("/stop_reasons","unchanged standard causality"); f.stops.emplace_back(s.key,kind);}
        f.defaults=::sp::descriptor::defaults(v.get("defaults")); f.required_output_cap=boolean(v.get("required_output_cap"),"/required_output_cap");
        if(f.required_output_cap!=(f.family=="anthropic.messages"))fail("/required_output_cap","protocol output requirement");
        f.temperature_range=range(v.get("temperature_range")); f.top_p_range=range(v.get("top_p_range")); f.thinking_minimum=integer(v.get("thinking_minimum"),"/thinking_minimum"); f.thinking_top_p_minimum=number(v.get("thinking_top_p_minimum"),"/thinking_top_p_minimum");
        f.reasoning_efforts=strings(v.get("reasoning_efforts"),"/reasoning_efforts"); f.reasoning_summaries=strings(v.get("reasoning_summaries"),"/reasoning_summaries"); f.thinking_levels=strings(v.get("thinking_levels"),"/thinking_levels"); f.service_tiers=strings(v.get("service_tiers"),"/service_tiers"); f.header_versions=strings(v.get("header_versions"),"/header_versions"); f.header_version=optional_text(v.get("header_version"),"/header_version");
        f.openrouter_origins = strings(v.get("openrouter_origins"), "/openrouter_origins");
        f.temperature_forbidden_model_prefixes = strings(v.get("temperature_forbidden_model_prefixes"), "/temperature_forbidden_model_prefixes");
        for (const auto& prefix : f.temperature_forbidden_model_prefixes)
          if (prefix.find('/') != std::string::npos ||
              std::any_of(prefix.begin(), prefix.end(), [](unsigned char c) {
                return !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.');
              })) fail("/temperature_forbidden_model_prefixes", "lowercase model prefixes without gateway namespace");
        // Optional: models of a series from a generation on. "from" is "major.minor".
        if (auto rules = v.get("temperature_forbidden_generations"); rules.valid()) {
          if (!rules.is_array() || rules.size() > 8) fail("/temperature_forbidden_generations", "bounded array of generation rules");
          for (auto rule : rules.elements()) {
            closed(rule, {"series", "from"}, "/temperature_forbidden_generations");
            auto series = text(rule.get("series"), "/temperature_forbidden_generations");
            if (series.back() != '-' || std::any_of(series.begin(), series.end(), [](unsigned char c) {
                  return !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.');
                })) fail("/temperature_forbidden_generations", "lowercase series prefix ending in '-' without gateway namespace");
            if (std::any_of(policy->generation_rules_.begin(), policy->generation_rules_.end(),
                            [&](const auto& old) { return old.family == f.family && old.series == series; }))
              fail("/temperature_forbidden_generations", "one generation rule per series");
            const auto from = text(rule.get("from"), "/temperature_forbidden_generations");
            const auto dot = from.find('.');
            auto component = [&](std::string_view digits) -> std::optional<std::uint32_t> {
              if (digits.empty() || digits.size() > 3 || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
              std::uint32_t value = 0;
              for (const char c : digits) value = value * 10 + static_cast<std::uint32_t>(c - '0');
              return value;
            };
            const auto major = dot == std::string::npos ? std::nullopt : component(std::string_view(from).substr(0, dot));
            const auto minor = dot == std::string::npos ? std::nullopt : component(std::string_view(from).substr(dot + 1));
            if (!major || !minor) fail("/temperature_forbidden_generations", "generation as major.minor digits");
            policy->generation_rules_.push_back({f.family, std::move(series), *major, *minor});
          }
        }
        if (!f.openrouter_origins.empty() && f.family != "openai.chat" && f.family != "openai.responses" && f.family != "anthropic.messages")
          fail("/openrouter_origins", "routing only for declared OpenRouter API families");
        for (const auto& route : f.openrouter_origins)
          if (!valid_origin(route) || route.ends_with('/'))
            fail("/openrouter_origins", "canonical admitted origin without path");
        // Optional: origins whose API rejects max_tokens, so the output cap defaults to max_completion_tokens there.
        if (auto origins = v.get("max_completion_tokens_origins"); origins.valid()) {
          const auto listed = strings(origins, "/max_completion_tokens_origins");
          if (!listed.empty() && f.family != "openai.chat") fail("/max_completion_tokens_origins", "Chat output-cap origins only");
          for (const auto& origin : listed) {
            if (!valid_origin(origin) || origin.ends_with('/') || contains(f.openrouter_origins, origin))
              fail("/max_completion_tokens_origins", "canonical admitted origin without path, apart from the OpenRouter origins");
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
            for (const auto& origin : fact.origins)
              if (!valid_origin(origin) || origin.ends_with('/')) fail("/betas/origins", "canonical origins without path");
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
          if (messages) closed(t, {"type","max_uses"}, "/server_tools", {"name","options","client","beta"});
          else closed(t, {"type","name","max_uses"}, "/server_tools", {"options","generic"});
          if (!messages && f.family != "openai.responses")
            fail("/server_tools", "typed Messages or Responses tool facts only");
          ServerToolFact fact{text(t.get("type"),"/server_tools"),
                              t.get("name").valid() ? text(t.get("name"),"/server_tools") : std::string(),
                              boolean(t.get("max_uses"),"/server_tools")};
          if (t.get("generic").valid()) fact.generic = boolean(t.get("generic"), "/server_tools/generic");
          if (t.get("client").valid()) fact.client = boolean(t.get("client"), "/server_tools/client");
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
              if (name == "type" || name == "name" || name == "max_uses" || name == "input_schema" || name == "description" ||
                  (messages && (name == "allowed_callers" || name == "allowed_domains" || name == "blocked_domains" ||
                                name == "user_location" || name == "citations" || name == "max_content_tokens")))
                fail("/server_tools/options", "properties other than the tool's typed members");
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
            for (const auto& origin : fact.origins)
              if (!valid_origin(origin) || origin.ends_with('/')) fail("/extensions/origins", "canonical origins without path");
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
        if(auto error=validate(f,choices(f.defaults),{},false))fail("/defaults",*error);
        if(f.defaults.strict_tools && f.family!="openai.responses") fail("/defaults/strict_tools","Responses strict tools only");
        policy->families_.push_back(std::move(f));
      }
      // Non-chat endpoints (D6): each names a compiled codec shape, its origins, the path of each operation and
      // the schema of the members a request may add beside the typed ones.
      if (auto endpoints = root.get("endpoints"); endpoints.valid()) {
        if (!endpoints.is_array() || endpoints.size() > 64) fail("/endpoints", "bounded array of endpoint facts");
        static constexpr std::string_view image_members[]{"model", "prompt", "n", "size", "quality", "background", "output_format",
                                                          "output_compression", "images", "input_references", "stream", "partial_images"};
        static constexpr std::string_view decision_members[]{"model", "input", "state", "questions"};
        static constexpr std::string_view embedding_members[]{"model", "input", "dimensions", "encoding_format", "input_type", "requests"};
        static constexpr std::string_view rerank_members[]{"model", "query", "documents", "top_n"};
        static constexpr std::string_view moderation_members[]{"model", "input"};
        static constexpr std::string_view speech_members[]{"model", "input", "voice", "instructions", "response_format", "speed", "stream_format"};
        static constexpr std::string_view transcription_members[]{"model", "file", "input_audio", "language", "prompt", "response_format", "stream"};
        static constexpr std::string_view video_members[]{"model", "prompt", "input_references", "frame_images", "duration", "aspect_ratio",
                                                          "resolution", "size", "generate_audio"};
        static constexpr std::string_view file_members[]{"file", "purpose", "expires_after"};
        static constexpr std::string_view realtime_members[]{"model", "generationConfig", "systemInstruction", "tools", "inputAudioTranscription",
                                                             "outputAudioTranscription", "sessionResumption", "type", "instructions",
                                                             "output_modalities", "audio"};
        static constexpr std::string_view batch_members[]{"input_file_id", "endpoint", "completion_window", "requests", "batch"};
        const auto owned_by = [](std::string_view endpoint, std::string_view name) {
          const auto in = [&](const auto& list) { return std::find(std::begin(list), std::end(list), name) != std::end(list); };
          return (endpoint == "images" && in(image_members)) || (endpoint == "decisions" && in(decision_members)) ||
                 (endpoint == "embeddings" && in(embedding_members)) || (endpoint == "rerank" && in(rerank_members)) ||
                 (endpoint == "moderation" && in(moderation_members)) || (endpoint == "speech" && in(speech_members)) ||
                 (endpoint == "transcription" && in(transcription_members)) || (endpoint == "videos" && in(video_members)) ||
                 (endpoint == "files" && in(file_members)) || (endpoint == "realtime" && in(realtime_members)) || (endpoint == "batches" && in(batch_members));
        };
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
        for (auto e : endpoints.elements()) {
          closed(e, {"endpoint","shape","origins","paths"}, "/endpoints", {"options","auth","charge_on_accept","headers","keep"});
          EndpointFact fact{text(e.get("endpoint"), "/endpoints/endpoint"), text(e.get("shape"), "/endpoints/shape"),
                            strings(e.get("origins"), "/endpoints/origins")};
          const auto operations = operations_of(fact.endpoint, fact.shape);
          if (operations.empty()) fail("/endpoints/shape", "an endpoint and shape the SDK has a codec for");
          if (fact.origins.empty()) fail("/endpoints/origins", "at least one origin");
          for (const auto& origin : fact.origins)
            if (!valid_origin(origin) || origin.ends_with('/')) fail("/endpoints/origins", "canonical origins without path");
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
              if (owned_by(fact.endpoint, name)) fail("/endpoints/options", "properties other than the endpoint's typed members");
          }
          if (e.get("auth").valid()) {
            fact.auth = text(e.get("auth"), "/endpoints/auth");
            if (fact.auth != "bearer" && fact.auth != "x-api-key" && fact.auth != "x-goog-api-key")
              fail("/endpoints/auth", "bearer, x-api-key or x-goog-api-key");
          }
          if (e.get("charge_on_accept").valid()) fact.charge_on_accept = boolean(e.get("charge_on_accept"), "/endpoints/charge_on_accept");
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
              constexpr std::string_view reserved[]{"authorization", "x-api-key", "x-goog-api-key", "proxy-authorization", "cookie", "host",
                                                    "connection", "content-type", "content-length", "accept", "transfer-encoding", "expect"};
              if (name.empty() || name.size() > 64 || !std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }) ||
                  std::find(std::begin(reserved), std::end(reserved), name) != std::end(reserved))
                fail("/endpoints/headers", "a non-reserved ASCII header name");
              const auto value = text(member.value, "/endpoints/headers");
              if (value.empty() || value.size() > 256 || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 0x20 && c < 0x7f; }))
                fail("/endpoints/headers", "a printable header value");
              fact.headers.emplace_back(std::string(member.key), value);
            }
          }
          for (const auto& existing : policy->endpoints_)
            if (existing.endpoint == fact.endpoint)
              for (const auto& origin : fact.origins)
                if (contains(existing.origins, origin)) fail("/endpoints/origins", "one fact per endpoint and origin");
          policy->endpoints_.push_back(std::move(fact));
        }
      }
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
        // A google.generate route has to name its model in both paths (descriptor.cpp).
        const bool generate = family.family == "google.generate";
        const auto descriptor_source =
            std::string("{\"descriptor_version\":1,\"revision\":1,\"id\":\"policy-admission\",\"family\":") +
            json::quote(family.family) +
            ",\"connection\":{\"base_url\":\"https://policy.invalid\",\"paths\":{\"buffered\":" +
            (generate ? "\"/v1beta/models/policy-admission:generateContent\",\"streaming\":\"/v1beta/models/policy-admission:streamGenerateContent?alt=sse\"}}}"
                      : "\"/policy\",\"streaming\":\"/policy\"}}}");
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
    for (std::size_t i = 0; i < entries.size(); ++i) {
      for (const auto& overlay : entries[i].defaults) {
        const auto* f = policy.family(overlay.family);
        if (!f) fail("/model_catalogue", "declared family");
        std::string pointer = "/model_catalogue/models/";
        for (const char c : entries[i].id) pointer += c == '~' ? std::string("~0") : c == '/' ? std::string("~1") : std::string(1, c);
        pointer += "/defaults/" + overlay.family;
        auto resolved = over(f->defaults, overlay);
        if (auto error = validate(*f, choices(resolved), entries[i].output_limit, false)) fail(pointer, *error);
        if (resolved.strict_tools && f->family != "openai.responses") fail(pointer + "/strict_tools", "Responses strict tools only");
        policy.resolved_.push_back({i, overlay.family, std::move(resolved)});
      }
    }
    crypto::Sha256 hash;
    hash.update("sp.descriptor-policy.content.v2\n");
    hash.update(policy.pair_digest_);
    hash.update(catalogue->digest());
    const auto digest = hash.finish();
    policy.content_digest_.assign(reinterpret_cast<const char*>(digest.data()), digest.size());
    policy.catalogue_ = std::move(catalogue);
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
bool routed_gateway(const ValidatedDescriptor& d) noexcept { return contains(d.family_policy().openrouter_origins, d.base_url()); }
namespace {
char ascii_lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
bool starts_with_ignore_case(std::string_view value, std::string_view prefix) noexcept {
  if (value.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) if (ascii_lower(value[i]) != prefix[i]) return false;
  return true;
}
// The generation after a series prefix: "opus-4-8-20260101" is 4.8, "3-5-sonnet-20241022" is 3.5,
// "sonnet-5" is 5.0 and gateway spellings with dots ("opus-4.7") read the same. The first component
// is the first token of one or two digits and the second is the token right after it when it is
// also one or two digits; a longer run of digits is a date and ends the version. No such token
// (or a date before any version) leaves the generation unknown.
std::optional<std::pair<std::uint32_t, std::uint32_t>> model_generation(std::string_view rest) noexcept {
  auto number = [](std::string_view token) -> std::optional<std::uint32_t> {
    if (token.empty() || token.size() > 2) return std::nullopt;
    std::uint32_t value = 0;
    for (const char c : token) {
      if (c < '0' || c > '9') return std::nullopt;
      value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return value;
  };
  auto next = [&](std::string_view& view) {
    const auto end = view.find_first_of("-.");
    const auto token = view.substr(0, end);
    view = end == std::string_view::npos ? std::string_view{} : view.substr(end + 1);
    return token;
  };
  while (!rest.empty()) {
    const auto token = next(rest);
    if (const auto major = number(token)) {
      std::uint32_t minor = 0;
      if (!rest.empty()) {
        auto look = rest;
        if (const auto second = number(next(look))) minor = *second;
      }
      return std::pair{*major, minor};
    }
    if (!token.empty() && std::all_of(token.begin(), token.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
  }
  return std::nullopt;
}
} // namespace
bool DescriptorPolicy::temperature_forbidden_by_generation(std::string_view family, std::string_view model) const noexcept {
  const auto slash = model.rfind('/');
  const auto suffix = slash == std::string_view::npos ? model : model.substr(slash + 1);
  for (const auto& rule : generation_rules_) {
    if (rule.family != family) continue;
    for (const auto candidate : {model, suffix}) {
      if (!starts_with_ignore_case(candidate, rule.series)) continue;
      const auto generation = model_generation(candidate.substr(rule.series.size()));
      if (!generation || *generation >= std::pair{rule.major, rule.minor}) return true;
    }
  }
  return false;
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
std::string_view DescriptorPolicy::default_output_cap_member(std::string_view family, std::string_view origin) const noexcept {
  for (const auto& [listed_family, listed_origin] : completion_cap_origins_)
    if (listed_family == family && listed_origin == origin) return "max_completion_tokens";
  const auto* declared = this->family(family);
  return declared ? std::string_view(declared->cap_member) : std::string_view{};
}
bool temperature_forbidden(const ValidatedDescriptor& d, std::string_view model) {
  // A catalogue entry that states the fact wins over the family's sampling rules.
  if (const auto match = model_match(d, model); match.entry && match.entry->temperature) return !*match.entry->temperature;
  const auto slash = model.rfind('/');
  const auto suffix = slash == std::string_view::npos ? model : model.substr(slash + 1);
  auto starts = [](std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
      auto c = value[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
      if (c != prefix[i]) return false;
    }
    return true;
  };
  for (const auto& prefix : d.family_policy().temperature_forbidden_model_prefixes)
    if (starts(model, prefix) || starts(suffix, prefix)) return true;
  return d.policy()->temperature_forbidden_by_generation(d.family(), model);
}
std::optional<std::string> validate_choices(const ValidatedDescriptor& d,std::string_view model,const EffectiveChoices& c) {
  if (c.temperature && temperature_forbidden(d, model)) return "temperature is prohibited for the admitted model";
  return validate(d.family_policy(),c,d.policy()->output_limit(d.family(),d.base_url(),model),true);
}
std::uint32_t interface_revision() noexcept { return 7; }
} // namespace sp::descriptor
