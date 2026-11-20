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
inline std::optional<reason::Reason> validate_routing(
    const descriptor::ValidatedDescriptor& descriptor, const OpenRouterRouting& routing) {
  if (!descriptor::contains(descriptor.family_policy().openrouter_origins, descriptor.base_url()))
    return reason::shared_request::controls_openrouter_routing_requires_a_declared;
  if (routing.data_collection && *routing.data_collection != "allow" && *routing.data_collection != "deny")
    return reason::shared_request::controls_data_collection_must_be_allow_or_deny;
  auto valid_list = [](const std::vector<std::string>& values) {
    if (values.size() > 64) return false;
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (!bounded_text(values[i], 128)) return false;
      for (std::size_t j = 0; j < i; ++j) if (values[i] == values[j]) return false;
    }
    return true;
  };
  if (!valid_list(routing.only) || !valid_list(routing.order) || !valid_list(routing.ignore))
    return reason::shared_request::controls_routing_lists_require_bounded_unique_nonempty;
  for (const auto& name : routing.ignore)
    if (descriptor::contains(routing.only, name) || descriptor::contains(routing.order, name))
      return reason::shared_request::controls_ignored_providers_cannot_be_selected_or;
  if (!routing.only.empty()) for (const auto& name : routing.order)
    if (!descriptor::contains(routing.only, name)) return reason::shared_request::controls_ordered_providers_must_belong_to_only;
  if (routing.max_price) {
    const auto& price = *routing.max_price;
    bool any = false;
    for (const auto* amount : {&price.prompt_nano_usd, &price.completion_nano_usd, &price.image_nano_usd,
                               &price.audio_nano_usd, &price.request_nano_usd}) {
      if (!*amount) continue;
      any = true;
      if (**amount > max_price_limit_nano_usd) return reason::shared_request::controls_max_price_amounts_cannot_exceed_2_53_nanousd;
    }
    if (!any) return reason::shared_request::controls_max_price_requires_at_least_one_amount;
  }
  return {};
}
inline std::optional<reason::Reason> validate_response_format(
    const ResponseFormat& format, const descriptor::CodecResources& resources) {
  if (format.kind == ResponseFormat::Kind::JsonObject) {
    if (!format.name.empty() || !format.description.empty() || format.schema || format.strict)
      return reason::shared_request::controls_json_object_cannot_carry_schema_controls;
    return {};
  }
  if (format.kind != ResponseFormat::Kind::JsonSchema) return reason::shared_request::controls_unknown_response_format_kind;
  if (!identifier(format.name) || format.description.size() > resources.request_bytes ||
      !format.schema || !format.schema->root().is_object())
    return reason::shared_request::controls_json_schema_requires_a_bounded_identifier_and;
  auto members = resources.native_members;
  const auto depth = std::min(resources.json_depth, resources.native_depth);
  if (!bounded_schema_value(format.schema->root(), 4, depth, members) || !schema_node(format.schema->root()))
    return reason::shared_request::controls_response_schema_exceeds_limits_or_contains;
  return {};
}
inline std::optional<reason::Reason> validate_image_config(const OpenRouterImageConfig& config) {
  for (const auto* field : {&config.aspect_ratio, &config.image_size, &config.size, &config.quality, &config.background,
                            &config.output_format, &config.moderation})
    if (*field && !config_token(**field)) return reason::shared_request::image_config_values_must_be_short_tokens;
  if (config.output_compression && *config.output_compression > 100) return reason::shared_request::image_config_output_compression_is_0_to_100;
  return std::nullopt;
}
// Policy-admitted top-level members (#32). Each one must be admitted by the family policy for the descriptor's
// origin, never name a member the encoder owns, and conform to its declared schema. Nothing else is sent.
struct ExtensionRefusal { bool admitted; reason::Reason cause; };
// A value the policy admits but that does not conform, and one the policy does not admit at all.
inline ExtensionRefusal invalid(const reason::Reason& cause) { return {true, cause}; }
inline ExtensionRefusal unsupported(const reason::Reason& cause) { return {false, cause}; }
inline std::optional<ExtensionRefusal> validate_extensions(const descriptor::ValidatedDescriptor& descriptor,
                                                           const std::shared_ptr<const json::Document>& extensions) {
  if (!extensions) return std::nullopt;
  const auto root = extensions->root();
  if (!root.is_object()) return invalid(reason::shared_request::extensions_extensions_must_be_an_object);
  const auto& family = descriptor.family_policy();
  for (auto member : root.members()) {
    if (descriptor::request_key_owned(family.family, member.key)) return invalid(reason::shared_request::extensions_an_extension_cannot_name_a_member_the_sdk);
    const auto fact = std::find_if(family.extensions.begin(), family.extensions.end(), [&](const descriptor::ExtensionFact& f) {
      return f.key == member.key && descriptor::contains(f.origins, descriptor.base_url());
    });
    if (fact == family.extensions.end()) return unsupported(reason::shared_request::extensions_an_extension_is_not_admitted_by_the_policy);
    if (descriptor::check_value(fact->value, member.value)) return invalid(reason::shared_request::extensions_an_extension_value_does_not_match_its);
  }
  return std::nullopt;
}
// OpenRouter plugins: each id must be admitted by the family policy, and its options must match the declared schema
// (#32). Admission says nothing about the origin; the encoder checks that it is a declared OpenRouter origin.
inline std::optional<ExtensionRefusal> validate_plugins(const descriptor::FamilyPolicy& family, const std::vector<OpenRouterPlugin>& plugins, std::size_t limit) {
  if (plugins.size() > limit) return invalid(reason::shared_request::plugins_count_limit_exceeded);
  std::set<std::string_view> ids;
  for (const auto& plugin : plugins) {
    if (!config_token(plugin.id) || !ids.insert(plugin.id).second) return invalid(reason::shared_request::plugins_require_unique_ids);
    const auto fact = std::find_if(family.openrouter_plugins.begin(), family.openrouter_plugins.end(), [&](const auto& f) { return f.name == plugin.id; });
    if (fact == family.openrouter_plugins.end()) return unsupported(reason::shared_request::plugins_an_openrouter_plugin_is_not_admitted_by_the);
    if (plugin.options && (!fact->options || descriptor::check_value(*fact->options, plugin.options->root())))
      return invalid(reason::shared_request::plugins_openrouter_plugin_options_do_not_match_the);
  }
  return std::nullopt;
}
// A value admitted by the policy for a typed container (a tool's options, a plugin's members): every member must be
// declared by `schema`. `what` names the container in the refusal.
inline std::optional<reason::Reason> validate_options(const std::optional<descriptor::ValueSchema>& schema,
                                                   const std::shared_ptr<const json::Document>& options) {
  if (!options) return std::nullopt;
  if (!schema) return reason::shared_request::options_options_are_not_admitted_by_the_policy;
  if (descriptor::check_value(*schema, options->root())) return reason::shared_request::options_options_do_not_match_the_policy_s_schema;
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
  if (!capabilities) return reason::error(ErrorKind::Unsupported, reason::shared_request::cache_not_admitted);
  if (capabilities->supports_explicit) return Admission{Disposition::Emit, *capabilities};
  if (capabilities->ignore_unsupported) return Admission{Disposition::Drop, *capabilities};
  return reason::error(ErrorKind::Unsupported, reason::shared_request::cache_unsupported);
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
  if (marks.size() > capabilities.max_breakpoints) return reason::error(ErrorKind::InvalidRequest, reason::shared_request::cache_breakpoint_limit);
  bool shorter = false;
  for (const auto ttl : marks) {
    if (ttl && *ttl != CacheTtl::FiveMinutes && *ttl != CacheTtl::OneHour) return reason::error(ErrorKind::InvalidRequest, reason::shared_request::cache_ttl_invalid);
    if (!lifetime(capabilities, ttl).offered) return reason::error(ErrorKind::Unsupported, reason::shared_request::cache_lifetime_not_offered);
    if (!capabilities.longest_first) continue;
    if (five_minutes(ttl)) shorter = true;
    else if (shorter) return reason::error(ErrorKind::InvalidRequest, reason::shared_request::cache_lifetime_order);
  }
  return std::nullopt;
}
// The services refuse a mark on an empty text block.
inline std::optional<Error> marked_text(const Text& text) {
  if (text.cache && text.value.empty()) return reason::error(ErrorKind::InvalidRequest, reason::shared_request::cache_empty_text);
  return std::nullopt;
}
}  // namespace sp::cache_marks
