#pragma once
// The checks the request encoders run on request controls and cache marks, each refusal naming the constant of the
// check (codecs/reason.h). Private: not installed; the types they check stay in core/request_controls.h and
// core/cache_marks.h.
#include "codecs/reason.h"
#include "core/cache_marks.h"
#include "core/request_controls.h"
#include "descriptor/owned_keys.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sp::request_controls {
inline std::optional<std::string> validate_routing(
    const descriptor::ValidatedDescriptor& descriptor, const OpenRouterRouting& routing) {
  if (!descriptor::contains(descriptor.family_policy().openrouter_origins, descriptor.base_url()))
    return "OpenRouter routing requires a declared OpenRouter origin";
  if (routing.data_collection && *routing.data_collection != "allow" && *routing.data_collection != "deny")
    return "data_collection must be allow or deny";
  auto valid_list = [](const std::vector<std::string>& values) {
    if (values.size() > 64) return false;
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (!bounded_text(values[i], 128)) return false;
      for (std::size_t j = 0; j < i; ++j) if (values[i] == values[j]) return false;
    }
    return true;
  };
  if (!valid_list(routing.only) || !valid_list(routing.order) || !valid_list(routing.ignore))
    return "routing lists require bounded unique nonempty provider names";
  for (const auto& name : routing.ignore)
    if (descriptor::contains(routing.only, name) || descriptor::contains(routing.order, name))
      return "ignored providers cannot be selected or ordered";
  if (!routing.only.empty()) for (const auto& name : routing.order)
    if (!descriptor::contains(routing.only, name)) return "ordered providers must belong to only";
  if (routing.max_price) {
    const auto& price = *routing.max_price;
    bool any = false;
    for (const auto* amount : {&price.prompt_nano_usd, &price.completion_nano_usd, &price.image_nano_usd,
                               &price.audio_nano_usd, &price.request_nano_usd}) {
      if (!*amount) continue;
      any = true;
      if (**amount > max_price_limit_nano_usd) return "max_price amounts cannot exceed 2^53 nanoUSD";
    }
    if (!any) return "max_price requires at least one amount";
  }
  return {};
}
inline std::optional<std::string> validate_response_format(
    const ResponseFormat& format, const descriptor::CodecResources& resources) {
  if (format.kind == ResponseFormat::Kind::JsonObject) {
    if (!format.name.empty() || !format.description.empty() || format.schema || format.strict)
      return "json_object cannot carry schema controls";
    return {};
  }
  if (format.kind != ResponseFormat::Kind::JsonSchema) return "unknown response format kind";
  if (!identifier(format.name) || format.description.size() > resources.request_bytes ||
      !format.schema || !format.schema->root().is_object())
    return "json_schema requires a bounded identifier and object schema";
  auto members = resources.native_members;
  const auto depth = std::min(resources.json_depth, resources.native_depth);
  if (!bounded_schema_value(format.schema->root(), 4, depth, members) || !schema_node(format.schema->root()))
    return "response schema exceeds limits or contains unsupported keyword types";
  return {};
}
inline std::optional<std::string> validate_image_config(const OpenRouterImageConfig& config) {
  for (const auto* field : {&config.aspect_ratio, &config.image_size, &config.size, &config.quality, &config.background,
                            &config.output_format, &config.moderation})
    if (*field && !config_token(**field)) return "image_config values must be short tokens";
  if (config.output_compression && *config.output_compression > 100) return "image_config output_compression is 0 to 100";
  return std::nullopt;
}
// Policy-admitted top-level members (#32). Each one must be admitted by the family policy for the descriptor's
// origin, never name a member the encoder owns, and conform to its declared schema. Nothing else is sent.
struct ExtensionRefusal { bool admitted; std::string message; };
// A value the policy admits but that does not conform, and one the policy does not admit at all.
inline ExtensionRefusal invalid(std::string message) { return {true, std::move(message)}; }
inline ExtensionRefusal unsupported(std::string message) { return {false, std::move(message)}; }
inline std::optional<ExtensionRefusal> validate_extensions(const descriptor::ValidatedDescriptor& descriptor,
                                                           const std::shared_ptr<const json::Document>& extensions) {
  if (!extensions) return std::nullopt;
  const auto root = extensions->root();
  if (!root.is_object()) return invalid("extensions must be an object");
  const auto& family = descriptor.family_policy();
  for (auto member : root.members()) {
    if (descriptor::request_key_owned(family.family, member.key)) return invalid("an extension cannot name a member the SDK writes");
    const auto fact = std::find_if(family.extensions.begin(), family.extensions.end(), [&](const descriptor::ExtensionFact& f) {
      return f.key == member.key && descriptor::contains(f.origins, descriptor.base_url());
    });
    if (fact == family.extensions.end()) return unsupported("an extension is not admitted by the policy for this origin");
    if (descriptor::check_value(fact->value, member.value)) return invalid("an extension value does not match its declared schema");
  }
  return std::nullopt;
}
// OpenRouter plugins: each id must be admitted by the family policy, and its options must match the declared schema
// (#32). Admission says nothing about the origin; the encoder checks that it is a declared OpenRouter origin.
inline std::optional<ExtensionRefusal> validate_plugins(const descriptor::FamilyPolicy& family, const std::vector<OpenRouterPlugin>& plugins, std::size_t limit) {
  if (plugins.size() > limit) return invalid("OpenRouter plugin count limit exceeded");
  std::set<std::string_view> ids;
  for (const auto& plugin : plugins) {
    if (!config_token(plugin.id) || !ids.insert(plugin.id).second) return invalid("OpenRouter plugins require unique ids");
    const auto fact = std::find_if(family.openrouter_plugins.begin(), family.openrouter_plugins.end(), [&](const auto& f) { return f.name == plugin.id; });
    if (fact == family.openrouter_plugins.end()) return unsupported("an OpenRouter plugin is not admitted by the policy");
    if (plugin.options && (!fact->options || descriptor::check_value(*fact->options, plugin.options->root())))
      return invalid("OpenRouter plugin options do not match the policy's schema");
  }
  return std::nullopt;
}
// A value admitted by the policy for a typed container (a tool's options, a plugin's members): every member must be
// declared by `schema`. `what` names the container in the refusal.
inline std::optional<std::string> validate_options(const std::optional<descriptor::ValueSchema>& schema,
                                                   const std::shared_ptr<const json::Document>& options) {
  if (!options) return std::nullopt;
  if (!schema) return "options are not admitted by the policy";
  if (descriptor::check_value(*schema, options->root())) return "options do not match the policy's schema";
  return std::nullopt;
}
}  // namespace sp::request_controls

namespace sp::cache_marks {
// Applies the policy to `requested` marks at this descriptor's origin and `model`. A policy that states no fact for
// the origin admits none, and one that states marks unsupported refuses them unless it says to ignore them.
inline std::variant<Admission, Error> admit(const descriptor::ValidatedDescriptor& descriptor, std::string_view model,
                                            std::uint32_t requested) {
  if (!requested) return Admission{};
  const auto capabilities = descriptor::cache_capabilities(descriptor, model);
  if (!capabilities) return Error{ErrorKind::Unsupported, "the policy admits no cache marks at this origin"};
  if (capabilities->supports_explicit) return Admission{Disposition::Emit, *capabilities};
  if (capabilities->ignore_unsupported) return Admission{Disposition::Drop, *capabilities};
  return Error{ErrorKind::Unsupported, "cache marks are not supported at this origin"};
}
// For an encoder whose wire has no explicit mark: the marks are dropped and reported, or refused, as the policy says.
inline std::optional<Error> drop(const descriptor::ValidatedDescriptor& descriptor, std::string_view model,
                                 std::span<const Message> messages, CacheMarkReport& report) {
  const auto marks = tally(messages).all;
  auto admitted = admit(descriptor, model, marks);
  if (auto* error = std::get_if<Error>(&admitted)) return std::move(*error);
  report.requested = marks;
  return std::nullopt;
}
// The marks about to be written, in prompt order, against the capabilities the policy states: their number, whether
// each lifetime is known and offered, and, where the origin wants it, that a longer lifetime never follows a shorter.
inline std::optional<Error> check(const descriptor::CacheCapabilities& capabilities, std::span<const std::optional<CacheTtl>> marks) {
  if (marks.size() > capabilities.max_breakpoints) return Error{ErrorKind::InvalidRequest, "cache mark count exceeds the origin's breakpoint limit"};
  bool shorter = false;
  for (const auto ttl : marks) {
    if (ttl && *ttl != CacheTtl::FiveMinutes && *ttl != CacheTtl::OneHour) return Error{ErrorKind::InvalidRequest, "invalid cache mark TTL"};
    if (!lifetime(capabilities, ttl).offered) return Error{ErrorKind::Unsupported, "cache mark lifetime is not offered at this origin"};
    if (!capabilities.longest_first) continue;
    if (five_minutes(ttl)) shorter = true;
    else if (shorter) return Error{ErrorKind::InvalidRequest, "longer cache lifetimes must come before shorter ones"};
  }
  return std::nullopt;
}
// The services refuse a mark on an empty text block.
inline std::optional<Error> marked_text(const Text& text) {
  if (text.cache && text.value.empty()) return Error{ErrorKind::InvalidRequest, "a cache mark cannot be set on an empty text block"};
  return std::nullopt;
}
}  // namespace sp::cache_marks
