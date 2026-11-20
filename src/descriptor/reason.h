#pragma once
// Generated from the reason tables (one constant per fixed failure); see codecs/reason.h. Private: not installed.
#include <string>
#include <string_view>

namespace sp::reason {
// A fixed failure of the SDK: the stable code hosts read in Error::reason ("<source>.<subject>.<problem>", lower-case
// dotted segments) and the safe_message text that goes with it. Both are fixed here, so nothing that arrives on the
// wire can reach a code. A raise names its constant; no helper takes a bare string.
struct Reason {
  std::string_view code, message;
};
// The checks the request encoders share (policy choices, request controls, media, cache marks).
namespace shared_request {
inline constexpr Reason route_model_name{"request.route.model_name", "model name cannot take the place of {model} in the descriptor's paths"};
inline constexpr Reason route_literal_model{"request.route.literal_model", "the descriptor's paths name another model"};
inline constexpr Reason options_options_are_not_admitted_by_the_policy{"request.options.options_are_not_admitted_by_the_policy", "options are not admitted by the policy"};
inline constexpr Reason options_options_do_not_match_the_policy_s_schema{"request.options.options_do_not_match_the_policy_s_schema", "options do not match the policy's schema"};
inline constexpr Reason plugins_an_openrouter_plugin_is_not_admitted_by_the{"request.plugins.an_openrouter_plugin_is_not_admitted_by_the", "an OpenRouter plugin is not admitted by the policy"};
inline constexpr Reason plugins_openrouter_plugin_options_do_not_match_the{"request.plugins.openrouter_plugin_options_do_not_match_the", "OpenRouter plugin options do not match the policy's schema"};
inline constexpr Reason extensions_extensions_must_be_an_object{"request.extensions.extensions_must_be_an_object", "extensions must be an object"};
inline constexpr Reason extensions_an_extension_cannot_name_a_member_the_sdk{"request.extensions.an_extension_cannot_name_a_member_the_sdk", "an extension cannot name a member the SDK writes"};
inline constexpr Reason extensions_an_extension_is_not_admitted_by_the_policy{"request.extensions.an_extension_is_not_admitted_by_the_policy", "an extension is not admitted by the policy for this origin"};
inline constexpr Reason extensions_an_extension_value_does_not_match_its{"request.extensions.an_extension_value_does_not_match_its", "an extension value does not match its declared schema"};
inline constexpr Reason plugins_count_limit_exceeded{"request.plugins.count_limit_exceeded", "OpenRouter plugin count limit exceeded"};
inline constexpr Reason plugins_require_unique_ids{"request.plugins.require_unique_ids", "OpenRouter plugins require unique ids"};
inline constexpr Reason image_config_values_must_be_short_tokens{"request.image_config.values_must_be_short_tokens", "image_config values must be short tokens"};
inline constexpr Reason image_config_output_compression_is_0_to_100{"request.image_config.output_compression_is_0_to_100", "image_config output_compression is 0 to 100"};
inline constexpr Reason choice_positive_output_cap_required{"request.choice.positive_output_cap_required", "positive output cap required"};
inline constexpr Reason choice_output_cap_exceeds_admitted_model_limit{"request.choice.output_cap_exceeds_admitted_model_limit", "output cap exceeds admitted model limit"};
inline constexpr Reason choice_output_cap_is_not_representable_as_int32{"request.choice.output_cap_is_not_representable_as_int32", "output cap is not representable as int32"};
inline constexpr Reason choice_sampling_parameter_outside_admitted_range{"request.choice.sampling_parameter_outside_admitted_range", "sampling parameter outside admitted range"};
inline constexpr Reason choice_thinking_budget_incompatible_with_output_cap{"request.choice.thinking_budget_incompatible_with_output_cap", "thinking budget incompatible with output cap"};
inline constexpr Reason choice_thinking_budget_and_level_are_mutually{"request.choice.thinking_budget_and_level_are_mutually", "thinking budget and level are mutually exclusive"};
inline constexpr Reason choice_manual_thinking_sampling_parameters_are{"request.choice.manual_thinking_sampling_parameters_are", "manual thinking sampling parameters are incompatible"};
inline constexpr Reason choice_sampling_control_unsupported_by_typed_request{"request.choice.sampling_control_unsupported_by_typed_request", "sampling control unsupported by typed request"};
inline constexpr Reason choice_top_p_unsupported_by_typed_generate_request{"request.choice.top_p_unsupported_by_typed_generate_request", "top_p unsupported by typed Generate request"};
inline constexpr Reason choice_unsupported_typed_semantic_control{"request.choice.unsupported_typed_semantic_control", "unsupported typed semantic control"};
inline constexpr Reason choice_reasoning_object_unsupported_for_family{"request.choice.reasoning_object_unsupported_for_family", "reasoning object unsupported for family"};
inline constexpr Reason choice_complete_effective_reasoning_options_required{"request.choice.complete_effective_reasoning_options_required", "complete effective reasoning options required"};
inline constexpr Reason choice_includethoughts_unsupported_for_family{"request.choice.includethoughts_unsupported_for_family", "includeThoughts unsupported for family"};
inline constexpr Reason choice_thinking_summaries_unsupported_for_family{"request.choice.thinking_summaries_unsupported_for_family", "thinking summaries unsupported for family"};
inline constexpr Reason choice_thinking_budget_unsupported_for_family{"request.choice.thinking_budget_unsupported_for_family", "thinking budget unsupported for family"};
inline constexpr Reason choice_reasoning_effort_unsupported_for_family{"request.choice.reasoning_effort_unsupported_for_family", "reasoning effort unsupported for family"};
inline constexpr Reason choice_reasoning_summary_unsupported_for_family{"request.choice.reasoning_summary_unsupported_for_family", "reasoning summary unsupported for family"};
inline constexpr Reason choice_thinking_level_unsupported_for_family{"request.choice.thinking_level_unsupported_for_family", "thinking level unsupported for family"};
inline constexpr Reason choice_service_tier_unsupported_for_family{"request.choice.service_tier_unsupported_for_family", "service tier unsupported for family"};
inline constexpr Reason choice_temperature_is_prohibited_for_the_admitted{"request.choice.temperature_is_prohibited_for_the_admitted", "temperature is prohibited for the admitted model"};
inline constexpr Reason choice_control_value_not_admitted_for_the_model{"request.choice.control_value_not_admitted_for_the_model", "control value is not admitted for the model"};
inline constexpr Reason controls_openrouter_routing_requires_a_declared{"request.controls.openrouter_routing_requires_a_declared", "OpenRouter routing requires a declared OpenRouter origin"};
inline constexpr Reason controls_data_collection_must_be_allow_or_deny{"request.controls.data_collection_must_be_allow_or_deny", "data_collection must be allow or deny"};
inline constexpr Reason controls_routing_lists_require_bounded_unique_nonempty{"request.controls.routing_lists_require_bounded_unique_nonempty", "routing lists require bounded unique nonempty provider names"};
inline constexpr Reason controls_ignored_providers_cannot_be_selected_or{"request.controls.ignored_providers_cannot_be_selected_or", "ignored providers cannot be selected or ordered"};
inline constexpr Reason controls_ordered_providers_must_belong_to_only{"request.controls.ordered_providers_must_belong_to_only", "ordered providers must belong to only"};
inline constexpr Reason controls_max_price_amounts_cannot_exceed_2_53_nanousd{"request.controls.max_price_amounts_cannot_exceed_2_53_nanousd", "max_price amounts cannot exceed 2^53 nanoUSD"};
inline constexpr Reason controls_max_price_requires_at_least_one_amount{"request.controls.max_price_requires_at_least_one_amount", "max_price requires at least one amount"};
inline constexpr Reason controls_json_object_cannot_carry_schema_controls{"request.controls.json_object_cannot_carry_schema_controls", "json_object cannot carry schema controls"};
inline constexpr Reason controls_unknown_response_format_kind{"request.controls.unknown_response_format_kind", "unknown response format kind"};
inline constexpr Reason controls_json_schema_requires_a_bounded_identifier_and{"request.controls.json_schema_requires_a_bounded_identifier_and", "json_schema requires a bounded identifier and object schema"};
inline constexpr Reason controls_response_schema_exceeds_limits_or_contains{"request.controls.response_schema_exceeds_limits_or_contains", "response schema exceeds limits or contains unsupported keyword types"};
inline constexpr Reason media_media_kind_does_not_match_its_mime_type{"request.media.media_kind_does_not_match_its_mime_type", "media kind does not match its MIME type"};
inline constexpr Reason media_invalid_or_missing_media_mime_type{"request.media.invalid_or_missing_media_mime_type", "invalid or missing media MIME type"};
inline constexpr Reason media_invalid_media_source{"request.media.invalid_media_source", "invalid media source"};
inline constexpr Reason media_media_payload_is_not_canonical_base64{"request.media.media_payload_is_not_canonical_base64", "media payload is not canonical base64"};
inline constexpr Reason media_media_payload_exceeds_the_decoded_size_limit{"request.media.media_payload_exceeds_the_decoded_size_limit", "media payload exceeds the decoded size limit"};
inline constexpr Reason media_media_payload_does_not_match_its_mime_type{"request.media.media_payload_does_not_match_its_mime_type", "media payload does not match its MIME type"};
inline constexpr Reason media_invalid_image_detail{"request.media.invalid_image_detail", "invalid image detail"};
inline constexpr Reason media_invalid_media_name_or_identity{"request.media.invalid_media_name_or_identity", "invalid media name or identity"};
inline constexpr Reason media_invalid_media{"request.media.invalid_media", "invalid media"};
inline constexpr Reason cache_not_admitted{"request.cache.not_admitted", "the policy admits no cache marks at this origin"};
inline constexpr Reason cache_unsupported{"request.cache.unsupported", "cache marks are not supported at this origin"};
inline constexpr Reason cache_breakpoint_limit{"request.cache.breakpoint_limit", "cache mark count exceeds the origin's breakpoint limit"};
inline constexpr Reason cache_ttl_invalid{"request.cache.ttl_invalid", "invalid cache mark TTL"};
inline constexpr Reason cache_lifetime_not_offered{"request.cache.lifetime_not_offered", "cache mark lifetime is not offered at this origin"};
inline constexpr Reason cache_lifetime_order{"request.cache.lifetime_order", "longer cache lifetimes must come before shorter ones"};
inline constexpr Reason cache_empty_text{"request.cache.empty_text", "a cache mark cannot be set on an empty text block"};
inline constexpr Reason media_image_input_not_accepted{"request.media.image_input_not_accepted", "this family does not accept image input"};
inline constexpr Reason media_audio_input_not_accepted{"request.media.audio_input_not_accepted", "this family does not accept audio input"};
inline constexpr Reason media_video_input_not_accepted{"request.media.video_input_not_accepted", "this family does not accept video input"};
inline constexpr Reason media_document_input_not_accepted{"request.media.document_input_not_accepted", "this family does not accept document input"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    route_model_name, route_literal_model, options_options_are_not_admitted_by_the_policy,
    options_options_do_not_match_the_policy_s_schema, plugins_an_openrouter_plugin_is_not_admitted_by_the,
    plugins_openrouter_plugin_options_do_not_match_the, extensions_extensions_must_be_an_object,
    extensions_an_extension_cannot_name_a_member_the_sdk, extensions_an_extension_is_not_admitted_by_the_policy,
    extensions_an_extension_value_does_not_match_its, plugins_count_limit_exceeded, plugins_require_unique_ids,
    image_config_values_must_be_short_tokens, image_config_output_compression_is_0_to_100,
    choice_positive_output_cap_required, choice_output_cap_exceeds_admitted_model_limit,
    choice_output_cap_is_not_representable_as_int32, choice_sampling_parameter_outside_admitted_range,
    choice_thinking_budget_incompatible_with_output_cap, choice_thinking_budget_and_level_are_mutually,
    choice_manual_thinking_sampling_parameters_are, choice_sampling_control_unsupported_by_typed_request,
    choice_top_p_unsupported_by_typed_generate_request, choice_unsupported_typed_semantic_control,
    choice_reasoning_object_unsupported_for_family, choice_complete_effective_reasoning_options_required,
    choice_includethoughts_unsupported_for_family, choice_thinking_summaries_unsupported_for_family,
    choice_thinking_budget_unsupported_for_family, choice_reasoning_effort_unsupported_for_family,
    choice_reasoning_summary_unsupported_for_family, choice_thinking_level_unsupported_for_family,
    choice_service_tier_unsupported_for_family, choice_temperature_is_prohibited_for_the_admitted,
    choice_control_value_not_admitted_for_the_model, controls_openrouter_routing_requires_a_declared,
    controls_data_collection_must_be_allow_or_deny, controls_routing_lists_require_bounded_unique_nonempty,
    controls_ignored_providers_cannot_be_selected_or, controls_ordered_providers_must_belong_to_only,
    controls_max_price_amounts_cannot_exceed_2_53_nanousd, controls_max_price_requires_at_least_one_amount,
    controls_json_object_cannot_carry_schema_controls, controls_unknown_response_format_kind,
    controls_json_schema_requires_a_bounded_identifier_and, controls_response_schema_exceeds_limits_or_contains,
    media_media_kind_does_not_match_its_mime_type, media_invalid_or_missing_media_mime_type,
    media_invalid_media_source, media_media_payload_is_not_canonical_base64,
    media_media_payload_exceeds_the_decoded_size_limit, media_media_payload_does_not_match_its_mime_type,
    media_invalid_image_detail, media_invalid_media_name_or_identity, media_invalid_media, cache_not_admitted,
    cache_unsupported, cache_breakpoint_limit, cache_ttl_invalid, cache_lifetime_not_offered, cache_lifetime_order,
    cache_empty_text, media_image_input_not_accepted, media_audio_input_not_accepted, media_video_input_not_accepted,
    media_document_input_not_accepted,
};
}  // namespace shared_request
}  // namespace sp::reason
