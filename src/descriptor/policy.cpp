#include "descriptor/policy.h"
#include "crypto/crypto.h"
#include "sp/config_defaults.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace sp::descriptor {
namespace {
using json::Value;
[[noreturn]] void fail(std::string_view path, std::string_view expected) {
  throw ConfigError{std::string(path), std::string(expected), 0, "descriptor policy value rejected"};
}
void closed(Value value, std::initializer_list<std::string_view> keys, std::string_view path) {
  if (!value.is_object() || value.size() != keys.size()) fail(path, "complete closed object");
  for (auto member : value.members()) if (std::find(keys.begin(), keys.end(), member.key) == keys.end()) fail(path, "declared fields only");
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
  fail("/stop_reasons","typed StopKind");
}
// Stop safety remains compiled; policy may add facts, never rewrite standard causality.
bool safe_stop(std::string_view family, std::string_view raw, StopKind kind) {
  if(raw=="error" || raw=="failed" || raw=="cancelled" || raw=="incomplete") return false;
  struct Rule { std::string_view family, raw; StopKind kind; };
  static constexpr Rule rules[]{
    {"openai.chat","stop",StopKind::EndTurn},{"openai.chat","length",StopKind::MaxTokens},{"openai.chat","tool_calls",StopKind::ToolUse},{"openai.chat","content_filter",StopKind::ContentFilter},
    {"anthropic.messages","end_turn",StopKind::EndTurn},{"anthropic.messages","max_tokens",StopKind::MaxTokens},{"anthropic.messages","tool_use",StopKind::ToolUse},{"anthropic.messages","pause_turn",StopKind::PauseTurn},{"anthropic.messages","refusal",StopKind::Refusal},{"anthropic.messages","stop_sequence",StopKind::StopSequence},{"anthropic.messages","model_context_window_exceeded",StopKind::ContextLimit},
    {"openai.responses","completed",StopKind::EndTurn},{"openai.responses","max_output_tokens",StopKind::MaxTokens},{"openai.responses","content_filter",StopKind::ContentFilter},
    {"google.generate","STOP",StopKind::EndTurn},{"google.generate","MAX_TOKENS",StopKind::MaxTokens},{"google.generate","SAFETY",StopKind::ContentFilter},{"google.generate","MALFORMED_FUNCTION_CALL",StopKind::MalformedCall},
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
  PolicyResult run(std::string_view source,std::string_view resources) {
    std::uint64_t revision = 0;
    try {
      auto p=json::parse(source,{config_defaults::codec_admission_policy_bytes,config_defaults::codec_admission_policy_depth});
      auto r=json::parse(resources,{config_defaults::codec_admission_policy_bytes,config_defaults::codec_admission_policy_depth});
      auto* pd=std::get_if<json::Document>(&p); auto* rd=std::get_if<json::Document>(&r);
      if (pd && pd->root().get("revision").is_uint()) revision = pd->root().get("revision").as_uint();
      if(!pd || !rd)fail("","strict bounded JSON policy");
      auto policy=std::shared_ptr<DescriptorPolicy>(new DescriptorPolicy);
      auto root=pd->root(); auto rr=rd->root();
      closed(root,{"version","revision","families","models"},""); closed(rr,{"version","resources","admission"},"/resources");
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
        closed(v,{"family","bindings","stop_reasons","defaults","required_output_cap","temperature_range","top_p_range","thinking_minimum","thinking_top_p_minimum","reasoning_efforts","reasoning_summaries","thinking_levels","service_tiers","header_versions","header_version","server_tools","openrouter_origins","temperature_forbidden_model_prefixes","media","openrouter_media"},"/families");
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
        if (!f.openrouter_origins.empty() && f.family != "openai.chat" && f.family != "openai.responses" && f.family != "anthropic.messages")
          fail("/openrouter_origins", "routing only for declared OpenRouter API families");
        for (const auto& route : f.openrouter_origins)
          if (!valid_origin(route) || route.ends_with('/'))
            fail("/openrouter_origins", "canonical admitted origin without path");
        f.media = media_admission(v.get("media"), "/media", f.family);
        if (!v.get("openrouter_media").is_null()) {
          if (f.openrouter_origins.empty()) fail("/openrouter_media", "gateway media requires declared OpenRouter origins");
          f.openrouter_media = media_admission(v.get("openrouter_media"), "/openrouter_media", f.family);
        }
        if(f.family=="anthropic.messages" && (!f.header_version || !contains(f.header_versions,*f.header_version)))fail("/header_version","admitted Messages version");
        if(f.family!="anthropic.messages" && (f.header_version || !f.header_versions.empty()))fail("/header_version","no unsupported header version");
        auto tools=v.get("server_tools"); if(!tools.is_array() || tools.size()>64)fail("/server_tools","bounded reviewed tool facts");
        for (auto t : tools.elements()) {
          closed(t, {"type","name","max_uses"}, "/server_tools");
          if (f.family != "anthropic.messages" && f.family != "openai.responses")
            fail("/server_tools", "typed Messages or Responses tool facts only");
          ServerToolFact fact{text(t.get("type"),"/server_tools"),
                              text(t.get("name"),"/server_tools"),
                              boolean(t.get("max_uses"),"/server_tools")};
          if (f.family == "openai.responses") {
            constexpr std::string_view types[]{"web_search","image_generation","file_search","tool_search","shell"};
            if (fact.max_uses || fact.name != fact.type ||
                std::find(std::begin(types), std::end(types), fact.type) == std::end(types))
              fail("/server_tools", "declared typed Responses tool fact");
          }
          for (const auto& existing : f.server_tools)
            if (existing.type == fact.type) fail("/server_tools", "unique tool types");
          f.server_tools.push_back(std::move(fact));
        }
        if(auto error=validate(f,choices(f.defaults),{},false))fail("/defaults",*error);
        if(f.defaults.strict_tools && f.family!="openai.responses") fail("/defaults/strict_tools","Responses strict tools only");
        policy->families_.push_back(std::move(f));
      }
      auto models=root.get("models"); if(!models.is_array() || models.size()>1024)fail("/models","bounded model defaults");
      for (auto v : models.elements()) {
        closed(v, {"family","model","defaults","output_limit","input_limit"}, "/models");
        ModelDefaults m{text(v.get("family"),"/models"), text(v.get("model"),"/models"),
                        ::sp::descriptor::defaults(v.get("defaults")),
                        optional_integer(v.get("output_limit"),"/models"),
                        optional_integer(v.get("input_limit"),"/models")};
        const auto* f = policy->family(m.family);
        if (!f || (m.output_limit && !*m.output_limit) || (m.input_limit && !*m.input_limit))
          fail("/models", "declared family and positive optional input/output model limits");
        for (const auto& old : policy->models_)
          if (old.family == m.family && old.model == m.model) fail("/models", "unique model");
        if (auto error = validate(*f, choices(m.defaults), m.output_limit, false))
          fail("/models/defaults", *error);
        if (m.defaults.strict_tools && m.family != "openai.responses")
          fail("/models/defaults/strict_tools", "Responses strict tools only");
        policy->models_.push_back(std::move(m));
      }
      const auto family_identity = pd->root().dump();
      const auto resource_identity = rd->root().dump();
      crypto::Sha256 hash;
      hash.update(family_identity);
      hash.update("\n");
      hash.update(resource_identity);
      const auto digest = hash.finish();
      policy->identity_.assign(reinterpret_cast<const char*>(digest.data()), digest.size());
      // Reuse descriptor admission for slot collisions, escaped usage segments,
      // and reserved wire fields instead of introducing a second convention.
      for (const auto& family : policy->families_) {
        const auto descriptor_source =
            std::string("{\"descriptor_version\":1,\"revision\":1,\"id\":\"policy-admission\",\"family\":") +
            json::quote(family.family) +
            ",\"connection\":{\"base_url\":\"https://policy.invalid\",\"paths\":{\"buffered\":\"/policy\",\"streaming\":\"/policy\"}}}";
        auto admitted = load(descriptor_source, PolicySnapshot(policy));
        if (auto* error = std::get_if<ConfigError>(&admitted)) throw std::move(*error);
      }
      return PolicySnapshot(std::move(policy));
    } catch (ConfigError& error) { error.revision = revision; return std::move(error); }
  }
};
const FamilyPolicy* DescriptorPolicy::family(std::string_view name) const noexcept { for(const auto& f:families_)if(f.family==name)return &f; return nullptr; }
const RequestDefaults& DescriptorPolicy::defaults(std::string_view name,std::string_view model) const noexcept { for(const auto& m:models_)if(m.family==name && m.model==model)return m.defaults; return family(name)->defaults; }
std::optional<std::uint64_t> DescriptorPolicy::output_limit(std::string_view name,std::string_view model) const noexcept {for(const auto& m:models_)if(m.family==name && m.model==model)return m.output_limit;return {};}
std::optional<std::uint64_t> DescriptorPolicy::input_limit(std::string_view name,std::string_view model) const noexcept {for(const auto& m:models_)if(m.family==name && m.model==model)return m.input_limit;return {};}
PolicyResult load_policy(std::string_view f,std::string_view r){return PolicyLoader{}.run(f,r);}
PolicySnapshot builtin_policy(){static const auto value=[] {auto r=load_policy(config_defaults::descriptor_policy_json,config_defaults::codec_defaults_json); if(auto* e=std::get_if<ConfigError>(&r))throw std::logic_error(e->message);return std::get<PolicySnapshot>(std::move(r));}();return value;}
std::optional<std::string_view> borrowed(const std::optional<std::string>& value){if(value)return *value;return {};}
bool contains(const std::vector<std::string>& values,std::string_view value){return std::find(values.begin(),values.end(),value)!=values.end();}
EffectiveChoices effective_defaults(const ValidatedDescriptor& d,std::string_view model){return choices(d.policy()->defaults(d.family(),model));}
const MediaPolicy& media_policy(const ValidatedDescriptor& d) noexcept {
  const auto& family = d.family_policy();
  return family.openrouter_media && routed_gateway(d) ? *family.openrouter_media : family.media;
}
bool routed_gateway(const ValidatedDescriptor& d) noexcept { return contains(d.family_policy().openrouter_origins, d.base_url()); }
bool temperature_forbidden(const ValidatedDescriptor& d, std::string_view model) {
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
  return false;
}
std::optional<std::string> validate_choices(const ValidatedDescriptor& d,std::string_view model,const EffectiveChoices& c) {
  if (c.temperature && temperature_forbidden(d, model)) return "temperature is prohibited for the admitted model";
  return validate(d.family_policy(),c,d.policy()->output_limit(d.family(),model),true);
}
std::uint32_t interface_revision() noexcept { return 7; }
} // namespace sp::descriptor
