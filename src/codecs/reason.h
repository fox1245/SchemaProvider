#pragma once
// Generated from the reason tables (one constant per fixed failure). Private: not installed.
#include "core/value.h"
#include "descriptor/reason.h"
#include <span>
#include <string_view>

namespace sp::reason {
// The constants of each decoder source and request encoder, by source: `reason::chat::finish_reason_invalid` is
// {"chat.finish_reason.invalid", "invalid finish reason"}. Names are the code's segments after the source prefix.
namespace chat {
inline constexpr Reason descriptor_family_mismatch{"chat.descriptor.family_mismatch", "descriptor family does not match Chat codec"};
inline constexpr Reason context_descriptor_mismatch{"chat.context.descriptor_mismatch", "Chat request context does not match descriptor"};
inline constexpr Reason buffered_unexpected{"chat.buffered.unexpected", "unexpected buffered document"};
inline constexpr Reason stream_frame_unexpected{"chat.stream.frame_unexpected", "unexpected stream frame"};
inline constexpr Reason stream_event_unknown{"chat.stream.event_unknown", "unknown stream event"};
inline constexpr Reason done_duplicate{"chat.done.duplicate", "duplicate DONE"};
inline constexpr Reason done_without_terminal{"chat.done.without_terminal", "DONE without family terminal evidence"};
inline constexpr Reason remote_error_event{"chat.remote.error_event", "remote error event"};
inline constexpr Reason response_byte_limit{"chat.response.byte_limit", "response byte limit"};
inline constexpr Reason response_json_invalid{"chat.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_object_required{"chat.response.object_required", "response must be object"};
inline constexpr Reason remote_error_property{"chat.remote.error_property", "remote error property"};
inline constexpr Reason done_frame_after{"chat.done.frame_after", "frame after DONE"};
inline constexpr Reason metadata_string_required{"chat.metadata.string_required", "required response metadata must be string"};
inline constexpr Reason object_tag_unknown{"chat.object.tag_unknown", "unknown response object tag"};
inline constexpr Reason created_required{"chat.created.required", "creation counter required"};
inline constexpr Reason identity_changed{"chat.identity.changed", "inconsistent response identity"};
inline constexpr Reason created_decreased{"chat.created.decreased", "creation counter decreased"};
inline constexpr Reason choices_array_required{"chat.choices.array_required", "choices must be array"};
inline constexpr Reason choices_multiple_unsupported{"chat.choices.multiple_unsupported", "multiple candidates unsupported"};
inline constexpr Reason usage_path_invalid{"chat.usage.path_invalid", "usage path crosses non-object"};
inline constexpr Reason choices_empty_unexplained{"chat.choices.empty_unexplained", "empty choices without usage or metadata heartbeat"};
inline constexpr Reason choice_object_required{"chat.choice.object_required", "choice must be object"};
inline constexpr Reason choice_shape_invalid{"chat.choice.shape_invalid", "choice has wrong response shape"};
inline constexpr Reason choice_index_required{"chat.choice.index_required", "choice index required"};
inline constexpr Reason choice_index_unsupported{"chat.choice.index_unsupported", "nonzero candidate index"};
inline constexpr Reason logprobs_invalid{"chat.logprobs.invalid", "invalid logprobs"};
inline constexpr Reason logprobs_placement_unsupported{"chat.logprobs.placement_unsupported", "logprobs outside first Chat cell"};
inline constexpr Reason finish_reason_invalid{"chat.finish_reason.invalid", "invalid finish reason"};
inline constexpr Reason remote_failure_terminal{"chat.remote.failure_terminal", "remote failure terminal"};
inline constexpr Reason finish_reason_cancelled{"chat.finish_reason.cancelled", "cancelled finish reason"};
inline constexpr Reason finish_reason_unsupported{"chat.finish_reason.unsupported", "unsupported finish reason"};
inline constexpr Reason terminal_tail_contradiction{"chat.terminal_tail.contradiction", "contradictory terminal usage tail"};
inline constexpr Reason finish_reason_role_changed{"chat.finish_reason.role_changed", "role changed after finish reason"};
inline constexpr Reason finish_reason_data_after{"chat.finish_reason.data_after", "data after finish reason"};
inline constexpr Reason audio_expiry_invalid{"chat.audio.expiry_invalid", "invalid final audio expiry"};
inline constexpr Reason finish_reason_empty{"chat.finish_reason.empty", "empty finish reason"};
inline constexpr Reason delta_object_required{"chat.delta.object_required", "message or delta must be object"};
inline constexpr Reason role_assistant_required{"chat.role.assistant_required", "assistant role required"};
inline constexpr Reason message_feature_unsupported{"chat.message.feature_unsupported", "unsupported known message feature"};
inline constexpr Reason reasoning_type_invalid{"chat.reasoning.type_invalid", "reasoning must be string or null"};
inline constexpr Reason reasoning_alias_contradiction{"chat.reasoning.alias_contradiction", "contradictory reasoning aliases"};
inline constexpr Reason message_content_required{"chat.message.content_required", "buffered content required"};
inline constexpr Reason tool_calls_array_required{"chat.tool_calls.array_required", "tool_calls must be array"};
inline constexpr Reason annotations_array_required{"chat.annotations.array_required", "annotations must be array"};
inline constexpr Reason annotations_count_limit{"chat.annotations.count_limit", "annotation count limit"};
inline constexpr Reason annotation_type_required{"chat.annotation.type_required", "annotation requires a type"};
inline constexpr Reason annotation_file_origin_required{"chat.annotation.file_origin_required", "file annotations require declared OpenRouter origin"};
inline constexpr Reason annotation_file_invalid{"chat.annotation.file_invalid", "invalid file annotation"};
inline constexpr Reason annotation_file_content_limit{"chat.annotation.file_content_limit", "file annotation content count limit"};
inline constexpr Reason annotation_file_part_invalid{"chat.annotation.file_part_invalid", "invalid parsed file part"};
inline constexpr Reason annotation_file_text_invalid{"chat.annotation.file_text_invalid", "invalid parsed file text"};
inline constexpr Reason annotation_file_image_invalid{"chat.annotation.file_image_invalid", "invalid parsed file image"};
inline constexpr Reason annotation_file_part_unsupported{"chat.annotation.file_part_unsupported", "unsupported parsed file part type"};
inline constexpr Reason annotation_url_citation_invalid{"chat.annotation.url_citation_invalid", "invalid URL citation annotation"};
inline constexpr Reason annotation_type_unsupported{"chat.annotation.type_unsupported", "unsupported annotation type"};
inline constexpr Reason content_array_origin_required{"chat.content.array_origin_required", "assistant content arrays require declared OpenRouter origin"};
inline constexpr Reason content_part_type_required{"chat.content.part_type_required", "assistant content part requires a type"};
inline constexpr Reason content_text_string_required{"chat.content.text_string_required", "content text must be string"};
inline constexpr Reason content_part_type_unsupported{"chat.content.part_type_unsupported", "unsupported assistant content part type"};
inline constexpr Reason text_type_invalid{"chat.text.type_invalid", "text channels must be strings or null"};
inline constexpr Reason reasoning_details_array_required{"chat.reasoning_details.array_required", "reasoning_details must be array"};
inline constexpr Reason reasoning_details_origin_required{"chat.reasoning_details.origin_required", "reasoning_details requires declared OpenRouter origin"};
inline constexpr Reason reasoning_detail_object_required{"chat.reasoning_detail.object_required", "reasoning detail must be object"};
inline constexpr Reason reasoning_details_metadata_limit{"chat.reasoning_details.metadata_limit", "reasoning details metadata limit"};
inline constexpr Reason reasoning_details_metadata_unretainable{"chat.reasoning_details.metadata_unretainable", "reasoning details metadata cannot be retained"};
inline constexpr Reason reasoning_detail_type_required{"chat.reasoning_detail.type_required", "reasoning detail requires a type"};
inline constexpr Reason reasoning_detail_identity_invalid{"chat.reasoning_detail.identity_invalid", "invalid reasoning detail identity"};
inline constexpr Reason reasoning_detail_encrypted_incomplete{"chat.reasoning_detail.encrypted_incomplete", "encrypted reasoning requires a complete opaque blob"};
inline constexpr Reason reasoning_detail_count_limit{"chat.reasoning_detail.count_limit", "reasoning detail count limit"};
inline constexpr Reason reasoning_detail_fragment_invalid{"chat.reasoning_detail.fragment_invalid", "invalid reasoning text or signature fragment"};
inline constexpr Reason reasoning_detail_metadata_contradiction{"chat.reasoning_detail.metadata_contradiction", "contradictory reasoning detail metadata"};
inline constexpr Reason reasoning_detail_payload_limit{"chat.reasoning_detail.payload_limit", "reasoning payload limit"};
inline constexpr Reason reasoning_detail_completed_metadata_limit{"chat.reasoning_detail.completed_metadata_limit", "completed reasoning metadata limit"};
inline constexpr Reason reasoning_detail_completed_unretainable{"chat.reasoning_detail.completed_unretainable", "completed reasoning metadata cannot be retained"};
inline constexpr Reason tool_call_object_required{"chat.tool_call.object_required", "tool call must be object"};
inline constexpr Reason tool_call_index_invalid{"chat.tool_call.index_invalid", "invalid tool index"};
inline constexpr Reason tool_call_id_invalid{"chat.tool_call.id_invalid", "invalid tool id"};
inline constexpr Reason tool_call_type_invalid{"chat.tool_call.type_invalid", "invalid tool type"};
inline constexpr Reason tool_call_type_unsupported{"chat.tool_call.type_unsupported", "unsupported tool type"};
inline constexpr Reason tool_call_function_invalid{"chat.tool_call.function_invalid", "invalid tool function"};
inline constexpr Reason tool_call_name_invalid{"chat.tool_call.name_invalid", "invalid function name"};
inline constexpr Reason tool_call_arguments_invalid{"chat.tool_call.arguments_invalid", "invalid arguments fragment"};
inline constexpr Reason tool_call_index_limit{"chat.tool_call.index_limit", "tool index exceeds part limit"};
inline constexpr Reason tool_call_ordering_ambiguous{"chat.tool_call.ordering_ambiguous", "mixed tool ordering is ambiguous"};
inline constexpr Reason tool_call_fragment_ambiguous{"chat.tool_call.fragment_ambiguous", "ambiguous unindexed tool fragment"};
inline constexpr Reason tool_call_identity_required{"chat.tool_call.identity_required", "new tool requires id and name"};
inline constexpr Reason tool_call_id_rebound{"chat.tool_call.id_rebound", "tool id rebound to another index"};
inline constexpr Reason tool_call_count_limit{"chat.tool_call.count_limit", "tool count limit"};
inline constexpr Reason tool_call_name_contradiction{"chat.tool_call.name_contradiction", "contradictory function name"};
inline constexpr Reason usage_object_required{"chat.usage.object_required", "usage must be object"};
inline constexpr Reason usage_counter_missing{"chat.usage.counter_missing", "required usage counter missing"};
inline constexpr Reason usage_counter_invalid{"chat.usage.counter_invalid", "usage counter must be unsigned integer"};
inline constexpr Reason usage_details_invalid{"chat.usage.details_invalid", "usage details must be object or null"};
inline constexpr Reason usage_sum_overflow{"chat.usage.sum_overflow", "usage sum overflow"};
inline constexpr Reason usage_cache_overflow{"chat.usage.cache_overflow", "cache sum overflow"};
inline constexpr Reason terminal_missing{"chat.terminal.missing", "missing family terminal evidence"};
inline constexpr Reason audio_object_required{"chat.audio.object_required", "audio must be object"};
inline constexpr Reason audio_fields_invalid{"chat.audio.fields_invalid", "audio fields must be strings"};
inline constexpr Reason audio_expiry_type_invalid{"chat.audio.expiry_type_invalid", "audio expiry must be a count"};
inline constexpr Reason audio_identity_changed{"chat.audio.identity_changed", "audio identity changed"};
inline constexpr Reason audio_transcript_limit{"chat.audio.transcript_limit", "audio transcript limit"};
inline constexpr Reason audio_data_invalid{"chat.audio.data_invalid", "audio data must be canonical base64"};
inline constexpr Reason audio_byte_limit{"chat.audio.byte_limit", "audio byte limit"};
inline constexpr Reason audio_fragment_invalid{"chat.audio.fragment_invalid", "invalid audio base64 fragment"};
inline constexpr Reason audio_quartet_incomplete{"chat.audio.quartet_incomplete", "incomplete audio base64 quartet"};
inline constexpr Reason audio_data_missing{"chat.audio.data_missing", "audio reply carries no data"};
inline constexpr Reason images_array_required{"chat.images.array_required", "images must be array"};
inline constexpr Reason image_object_required{"chat.image.object_required", "generated image must be object"};
inline constexpr Reason image_type_unsupported{"chat.image.type_unsupported", "unsupported generated image type"};
inline constexpr Reason image_url_required{"chat.image.url_required", "generated image requires image_url.url"};
inline constexpr Reason image_index_invalid{"chat.image.index_invalid", "generated image index must be a count"};
inline constexpr Reason image_indexed_changed{"chat.image.indexed_changed", "indexed generated image changed"};
inline constexpr Reason image_location_unsupported{"chat.image.location_unsupported", "unsupported generated image location"};
inline constexpr Reason image_detail_invalid{"chat.image.detail_invalid", "image detail must be string"};
inline constexpr Reason image_detail_unsupported{"chat.image.detail_unsupported", "unsupported image detail"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    descriptor_family_mismatch, context_descriptor_mismatch, buffered_unexpected, stream_frame_unexpected,
    stream_event_unknown, done_duplicate, done_without_terminal, remote_error_event, response_byte_limit,
    response_json_invalid, response_object_required, remote_error_property, done_frame_after,
    metadata_string_required, object_tag_unknown, created_required, identity_changed, created_decreased,
    choices_array_required, choices_multiple_unsupported, usage_path_invalid, choices_empty_unexplained,
    choice_object_required, choice_shape_invalid, choice_index_required, choice_index_unsupported, logprobs_invalid,
    logprobs_placement_unsupported, finish_reason_invalid, remote_failure_terminal, finish_reason_cancelled,
    finish_reason_unsupported, terminal_tail_contradiction, finish_reason_role_changed, finish_reason_data_after,
    audio_expiry_invalid, finish_reason_empty, delta_object_required, role_assistant_required,
    message_feature_unsupported, reasoning_type_invalid, reasoning_alias_contradiction, message_content_required,
    tool_calls_array_required, annotations_array_required, annotations_count_limit, annotation_type_required,
    annotation_file_origin_required, annotation_file_invalid, annotation_file_content_limit,
    annotation_file_part_invalid, annotation_file_text_invalid, annotation_file_image_invalid,
    annotation_file_part_unsupported, annotation_url_citation_invalid, annotation_type_unsupported,
    content_array_origin_required, content_part_type_required, content_text_string_required,
    content_part_type_unsupported, text_type_invalid, reasoning_details_array_required,
    reasoning_details_origin_required, reasoning_detail_object_required, reasoning_details_metadata_limit,
    reasoning_details_metadata_unretainable, reasoning_detail_type_required, reasoning_detail_identity_invalid,
    reasoning_detail_encrypted_incomplete, reasoning_detail_count_limit, reasoning_detail_fragment_invalid,
    reasoning_detail_metadata_contradiction, reasoning_detail_payload_limit,
    reasoning_detail_completed_metadata_limit, reasoning_detail_completed_unretainable, tool_call_object_required,
    tool_call_index_invalid, tool_call_id_invalid, tool_call_type_invalid, tool_call_type_unsupported,
    tool_call_function_invalid, tool_call_name_invalid, tool_call_arguments_invalid, tool_call_index_limit,
    tool_call_ordering_ambiguous, tool_call_fragment_ambiguous, tool_call_identity_required, tool_call_id_rebound,
    tool_call_count_limit, tool_call_name_contradiction, usage_object_required, usage_counter_missing,
    usage_counter_invalid, usage_details_invalid, usage_sum_overflow, usage_cache_overflow, terminal_missing,
    audio_object_required, audio_fields_invalid, audio_expiry_type_invalid, audio_identity_changed,
    audio_transcript_limit, audio_data_invalid, audio_byte_limit, audio_fragment_invalid, audio_quartet_incomplete,
    audio_data_missing, images_array_required, image_object_required, image_type_unsupported, image_url_required,
    image_index_invalid, image_indexed_changed, image_location_unsupported, image_detail_invalid,
    image_detail_unsupported,
};
}  // namespace chat
namespace messages {
inline constexpr Reason retained_json_invalid{"messages.retained.json_invalid", "invalid retained JSON"};
inline constexpr Reason buffered_unexpected{"messages.buffered.unexpected", "unexpected buffered response"};
inline constexpr Reason stream_frame_unexpected{"messages.stream.frame_unexpected", "unexpected stream frame"};
inline constexpr Reason context_required{"messages.context.required", "Messages request context required"};
inline constexpr Reason remote_error_event{"messages.remote.error_event", "remote error event"};
inline constexpr Reason response_byte_limit{"messages.response.byte_limit", "response byte limit"};
inline constexpr Reason response_json_invalid{"messages.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_object_required{"messages.response.object_required", "response must be object"};
inline constexpr Reason response_type_required{"messages.response.type_required", "response type required"};
inline constexpr Reason event_type_mismatch{"messages.event.type_mismatch", "SSE event and payload type disagree"};
inline constexpr Reason stream_event_after_stop{"messages.stream.event_after_stop", "event after message_stop"};
inline constexpr Reason message_start_duplicate{"messages.message_start.duplicate", "duplicate message_start"};
inline constexpr Reason event_unknown{"messages.event.unknown", "unknown Messages event"};
inline constexpr Reason stream_event_before_start{"messages.stream.event_before_start", "event before message_start"};
inline constexpr Reason message_delta_required{"messages.message_delta.required", "message delta required"};
inline constexpr Reason stream_content_after_delta{"messages.stream.content_after_delta", "content after message delta"};
inline constexpr Reason block_index_required{"messages.block.index_required", "block index required"};
inline constexpr Reason block_index_limit{"messages.block.index_limit", "block index limit"};
inline constexpr Reason identity_invalid{"messages.identity.invalid", "invalid message identity"};
inline constexpr Reason identity_changed{"messages.identity.changed", "inconsistent message identity"};
inline constexpr Reason metadata_invalid{"messages.metadata.invalid", "invalid known message metadata"};
inline constexpr Reason metadata_unsupported{"messages.metadata.unsupported", "unsupported semantic message metadata"};
inline constexpr Reason message_object_required{"messages.message.object_required", "message object required"};
inline constexpr Reason message_type_unknown{"messages.message.type_unknown", "unknown response object type"};
inline constexpr Reason message_duplicate{"messages.message.duplicate", "duplicate message"};
inline constexpr Reason message_content_invalid{"messages.message.content_invalid", "invalid initial content"};
inline constexpr Reason usage_object_required{"messages.usage.object_required", "usage object required"};
inline constexpr Reason usage_wrong_family{"messages.usage.wrong_family", "usage decoding is for the Messages family"};
inline constexpr Reason block_lifecycle_incomplete{"messages.block.lifecycle_incomplete", "incomplete block lifecycle or index gap"};
inline constexpr Reason stop_fields_invalid{"messages.stop.fields_invalid", "invalid stop fields"};
inline constexpr Reason stop_details_invalid{"messages.stop.details_invalid", "invalid stop details"};
inline constexpr Reason stop_details_unknown{"messages.stop.details_unknown", "unknown stop details type"};
inline constexpr Reason message_start_terminal_fields{"messages.message_start.terminal_fields", "terminal fields in message_start"};
inline constexpr Reason stop_reason_missing{"messages.stop.reason_missing", "stop metadata without reason"};
inline constexpr Reason remote_failure_stop{"messages.remote.failure_stop", "remote failure stop"};
inline constexpr Reason stop_cancelled{"messages.stop.cancelled", "cancelled stop reason"};
inline constexpr Reason stop_reason_unsupported{"messages.stop.reason_unsupported", "unsupported stop reason"};
inline constexpr Reason stop_metadata_contradiction{"messages.stop.metadata_contradiction", "contradictory stop metadata"};
inline constexpr Reason stop_reason_contradiction{"messages.stop.reason_contradiction", "contradictory stop reason"};
inline constexpr Reason caller_invalid{"messages.caller.invalid", "invalid tool caller"};
inline constexpr Reason caller_direct_with_id{"messages.caller.direct_with_id", "direct caller has tool id"};
inline constexpr Reason caller_type_unknown{"messages.caller.type_unknown", "unknown tool caller type"};
inline constexpr Reason caller_id_required{"messages.caller.id_required", "server caller id required"};
inline constexpr Reason block_count_limit{"messages.block.count_limit", "block count limit"};
inline constexpr Reason block_index_duplicate{"messages.block.index_duplicate", "duplicate block index"};
inline constexpr Reason block_type_required{"messages.block.type_required", "content block type required"};
inline constexpr Reason block_text_required{"messages.block.text_required", "text required"};
inline constexpr Reason block_citations_invalid{"messages.block.citations_invalid", "invalid citations"};
inline constexpr Reason block_citation_limit{"messages.block.citation_limit", "citation count limit"};
inline constexpr Reason block_citation_invalid{"messages.block.citation_invalid", "invalid citation"};
inline constexpr Reason block_thinking_invalid{"messages.block.thinking_invalid", "invalid thinking fields"};
inline constexpr Reason block_redacted_data_required{"messages.block.redacted_data_required", "redacted data required"};
inline constexpr Reason block_tool_invalid{"messages.block.tool_invalid", "invalid tool block"};
inline constexpr Reason block_mcp_server_missing{"messages.block.mcp_server_missing", "MCP call names no server"};
inline constexpr Reason block_toolset_invalid{"messages.block.toolset_invalid", "invalid toolset name"};
inline constexpr Reason block_tool_id_duplicate{"messages.block.tool_id_duplicate", "duplicate tool id"};
inline constexpr Reason block_tool_metadata_invalid{"messages.block.tool_metadata_invalid", "invalid tool metadata"};
inline constexpr Reason block_server_result_identity_invalid{"messages.block.server_result_identity_invalid", "invalid server result identity"};
inline constexpr Reason block_server_result_uncorrelated{"messages.block.server_result_uncorrelated", "uncorrelated server result"};
inline constexpr Reason block_type_unsupported{"messages.block.type_unsupported", "unsupported content block type"};
inline constexpr Reason delta_block_closed{"messages.delta.block_closed", "delta outside open block"};
inline constexpr Reason delta_type_required{"messages.delta.type_required", "delta type required"};
inline constexpr Reason delta_thinking_after_signature{"messages.delta.thinking_after_signature", "thinking after signature"};
inline constexpr Reason delta_unsupported{"messages.delta.unsupported", "unsupported semantic delta"};
inline constexpr Reason delta_kind_mismatch{"messages.delta.kind_mismatch", "delta kind differs from block"};
inline constexpr Reason delta_unknown{"messages.delta.unknown", "unknown content delta"};
inline constexpr Reason delta_kept_block_limit{"messages.delta.kept_block_limit", "kept block limit"};
inline constexpr Reason delta_string_required{"messages.delta.string_required", "delta string required"};
inline constexpr Reason delta_tool_snapshot_mixed{"messages.delta.tool_snapshot_mixed", "tool snapshot mixed with argument deltas"};
inline constexpr Reason block_stop_without_open{"messages.block.stop_without_open", "stop outside open block"};
inline constexpr Reason server_result_content_invalid{"messages.server_result.content_invalid", "invalid server result content"};
inline constexpr Reason usage_counter_invalid{"messages.usage.counter_invalid", "usage counter must be unsigned integer"};
inline constexpr Reason usage_counters_missing{"messages.usage.counters_missing", "required usage counters missing"};
inline constexpr Reason usage_details_invalid{"messages.usage.details_invalid", "usage details must be object"};
inline constexpr Reason usage_iterations_invalid{"messages.usage.iterations_invalid", "usage iterations must be array"};
inline constexpr Reason usage_iteration_invalid{"messages.usage.iteration_invalid", "usage iteration must be object"};
inline constexpr Reason usage_iteration_counter_invalid{"messages.usage.iteration_counter_invalid", "invalid iteration counter"};
inline constexpr Reason usage_metadata_invalid{"messages.usage.metadata_invalid", "invalid usage metadata"};
inline constexpr Reason usage_sum_overflow{"messages.usage.sum_overflow", "usage sum overflow"};
inline constexpr Reason terminal_missing{"messages.terminal.missing", "missing Messages terminal evidence"};
inline constexpr Reason remote_error_response{"messages.remote.error_response", "remote error response"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    retained_json_invalid, buffered_unexpected, stream_frame_unexpected, context_required, remote_error_event,
    response_byte_limit, response_json_invalid, response_object_required, response_type_required, event_type_mismatch,
    stream_event_after_stop, message_start_duplicate, event_unknown, stream_event_before_start,
    message_delta_required, stream_content_after_delta, block_index_required, block_index_limit, identity_invalid,
    identity_changed, metadata_invalid, metadata_unsupported, message_object_required, message_type_unknown,
    message_duplicate, message_content_invalid, usage_object_required, usage_wrong_family, block_lifecycle_incomplete,
    stop_fields_invalid, stop_details_invalid, stop_details_unknown, message_start_terminal_fields,
    stop_reason_missing, remote_failure_stop, stop_cancelled, stop_reason_unsupported, stop_metadata_contradiction,
    stop_reason_contradiction, caller_invalid, caller_direct_with_id, caller_type_unknown, caller_id_required,
    block_count_limit, block_index_duplicate, block_type_required, block_text_required, block_citations_invalid,
    block_citation_limit, block_citation_invalid, block_thinking_invalid, block_redacted_data_required,
    block_tool_invalid, block_mcp_server_missing, block_toolset_invalid, block_tool_id_duplicate,
    block_tool_metadata_invalid, block_server_result_identity_invalid, block_server_result_uncorrelated,
    block_type_unsupported, delta_block_closed, delta_type_required, delta_thinking_after_signature,
    delta_unsupported, delta_kind_mismatch, delta_unknown, delta_kept_block_limit, delta_string_required,
    delta_tool_snapshot_mixed, block_stop_without_open, server_result_content_invalid, usage_counter_invalid,
    usage_counters_missing, usage_details_invalid, usage_iterations_invalid, usage_iteration_invalid,
    usage_iteration_counter_invalid, usage_metadata_invalid, usage_sum_overflow, terminal_missing,
    remote_error_response,
};
}  // namespace messages
namespace responses {
inline constexpr Reason retained_metadata_limit{"responses.retained.metadata_limit", "retained response metadata limit"};
inline constexpr Reason retained_metadata_invalid{"responses.retained.metadata_invalid", "invalid retained response metadata"};
inline constexpr Reason buffered_unexpected{"responses.buffered.unexpected", "unexpected buffered response"};
inline constexpr Reason stream_frame_unexpected{"responses.stream.frame_unexpected", "unexpected stream frame"};
inline constexpr Reason sentinel_terminal_missing{"responses.sentinel.terminal_missing", "Responses sentinel lacks terminal envelope"};
inline constexpr Reason event_index_required{"responses.event.index_required", "event index required"};
inline constexpr Reason event_index_limit{"responses.event.index_limit", "response index limit"};
inline constexpr Reason event_item_mismatch{"responses.event.item_mismatch", "event item identity or lifecycle mismatch"};
inline constexpr Reason context_required{"responses.context.required", "Responses request context required"};
inline constexpr Reason remote_error_event{"responses.remote.error_event", "remote Responses error payload"};
inline constexpr Reason response_byte_limit{"responses.response.byte_limit", "response byte limit"};
inline constexpr Reason response_json_invalid{"responses.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_object_required{"responses.response.object_required", "response object required"};
inline constexpr Reason event_type_required{"responses.event.type_required", "event type required"};
inline constexpr Reason event_type_mismatch{"responses.event.type_mismatch", "SSE event and payload type disagree"};
inline constexpr Reason event_sequence_invalid{"responses.event.sequence_invalid", "event sequence regression or duplicate"};
inline constexpr Reason remote_failure{"responses.remote.failure", "remote response failure"};
inline constexpr Reason event_unsupported{"responses.event.unsupported", "unsupported semantic Responses event"};
inline constexpr Reason stream_output_after_terminal{"responses.stream.output_after_terminal", "output after response terminal"};
inline constexpr Reason stream_event_before_created{"responses.stream.event_before_created", "event before response.created"};
inline constexpr Reason arguments_lifecycle_mismatch{"responses.arguments.lifecycle_mismatch", "arguments event lifecycle mismatch"};
inline constexpr Reason arguments_string_required{"responses.arguments.string_required", "argument string required"};
inline constexpr Reason arguments_snapshot_contradiction{"responses.arguments.snapshot_contradiction", "argument snapshot contradicts prefix"};
inline constexpr Reason arguments_limit{"responses.arguments.limit", "tool argument limit"};
inline constexpr Reason annotation_item_mismatch{"responses.annotation.item_mismatch", "annotation event on non-message item"};
inline constexpr Reason annotation_invalid{"responses.annotation.invalid", "invalid annotation lifecycle or metadata"};
inline constexpr Reason server_event_item_mismatch{"responses.server_event.item_mismatch", "server event item type mismatch"};
inline constexpr Reason server_event_partial_image_invalid{"responses.server_event.partial_image_invalid", "partial image requires payload and index"};
inline constexpr Reason identity_invalid{"responses.identity.invalid", "invalid response identity metadata"};
inline constexpr Reason identity_changed{"responses.identity.changed", "response identity changed"};
inline constexpr Reason response_payload_required{"responses.response.payload_required", "response payload required"};
inline constexpr Reason response_error_invalid{"responses.response.error_invalid", "invalid response error metadata"};
inline constexpr Reason remote_error_response{"responses.remote.error_response", "remote error response"};
inline constexpr Reason response_status_or_output_invalid{"responses.response.status_or_output_invalid", "invalid response status or output"};
inline constexpr Reason remote_failure_status{"responses.remote.failure_status", "remote response failure status"};
inline constexpr Reason response_status_unsupported{"responses.response.status_unsupported", "unsupported response status"};
inline constexpr Reason buffered_not_terminal{"responses.buffered.not_terminal", "buffered response is not terminal"};
inline constexpr Reason done_envelope_required{"responses.done.envelope_required", "response.done requires a terminal response envelope"};
inline constexpr Reason response_status_mismatch{"responses.response.status_mismatch", "event and response status disagree"};
inline constexpr Reason incomplete_details_early{"responses.incomplete.details_early", "incomplete details before terminal"};
inline constexpr Reason incomplete_details_unexpected{"responses.incomplete.details_unexpected", "completed response has incomplete details"};
inline constexpr Reason incomplete_reason_required{"responses.incomplete.reason_required", "incomplete reason required"};
inline constexpr Reason incomplete_reason_unsupported{"responses.incomplete.reason_unsupported", "unsupported incomplete reason"};
inline constexpr Reason created_required{"responses.created.required", "response.created required"};
inline constexpr Reason created_duplicate{"responses.created.duplicate", "duplicate response.created"};
inline constexpr Reason response_status_regressed{"responses.response.status_regressed", "response status regressed"};
inline constexpr Reason created_output_not_empty{"responses.created.output_not_empty", "initial response output must be empty"};
inline constexpr Reason output_count_mismatch{"responses.output.count_mismatch", "aggregate output item count differs"};
inline constexpr Reason output_item_contradiction{"responses.output.item_contradiction", "aggregate output contradicts done item"};
inline constexpr Reason output_incomplete_item{"responses.output.incomplete_item", "completed response contains incomplete item"};
inline constexpr Reason output_part_limit{"responses.output.part_limit", "response part limit"};
inline constexpr Reason item_identity_required{"responses.item.identity_required", "output item identity required"};
inline constexpr Reason item_message_invalid{"responses.item.message_invalid", "invalid message item fields"};
inline constexpr Reason item_function_invalid{"responses.item.function_invalid", "invalid function item fields"};
inline constexpr Reason item_computer_call_invalid{"responses.item.computer_call_invalid", "invalid computer call item fields"};
inline constexpr Reason item_mcp_approval_request_invalid{"responses.item.mcp_approval_request_invalid", "invalid MCP approval request fields"};
inline constexpr Reason item_image_data_url_invalid{"responses.item.image_data_url_invalid", "invalid image data URL"};
inline constexpr Reason item_reasoning_invalid{"responses.item.reasoning_invalid", "invalid reasoning item fields"};
inline constexpr Reason item_server_status_invalid{"responses.item.server_status_invalid", "invalid server item status"};
inline constexpr Reason item_server_status_empty{"responses.item.server_status_empty", "empty server item status"};
inline constexpr Reason item_server_nonterminal{"responses.item.server_nonterminal", "nonterminal server item at done"};
inline constexpr Reason item_type_unsupported{"responses.item.type_unsupported", "unsupported Responses output item type"};
inline constexpr Reason item_metadata_changed{"responses.item.metadata_changed", "item metadata changed or disappeared"};
inline constexpr Reason item_limit{"responses.item.limit", "output item limit"};
inline constexpr Reason item_index_duplicate{"responses.item.index_duplicate", "duplicate output index"};
inline constexpr Reason item_id_duplicate{"responses.item.id_duplicate", "duplicate output item id"};
inline constexpr Reason item_call_id_duplicate{"responses.item.call_id_duplicate", "duplicate function call id"};
inline constexpr Reason text_snapshot_contradiction{"responses.text.snapshot_contradiction", "text snapshot contradicts streamed prefix"};
inline constexpr Reason content_index_limit{"responses.content.index_limit", "content index limit"};
inline constexpr Reason content_type_required{"responses.content.type_required", "content part type required"};
inline constexpr Reason content_type_unsupported{"responses.content.type_unsupported", "unsupported content part type"};
inline constexpr Reason content_text_required{"responses.content.text_required", "content text required"};
inline constexpr Reason content_text_metadata_invalid{"responses.content.text_metadata_invalid", "invalid output text metadata"};
inline constexpr Reason content_index_duplicate{"responses.content.index_duplicate", "duplicate content part index"};
inline constexpr Reason content_done_mismatch{"responses.content.done_mismatch", "content part done lifecycle mismatch"};
inline constexpr Reason annotation_snapshot_contradiction{"responses.annotation.snapshot_contradiction", "annotation snapshot contradiction"};
inline constexpr Reason annotation_initial_contradiction{"responses.annotation.initial_contradiction", "initial annotation contradiction"};
inline constexpr Reason text_item_mismatch{"responses.text.item_mismatch", "text event item type mismatch"};
inline constexpr Reason text_before_part{"responses.text.before_part", "text event before part added"};
inline constexpr Reason text_lifecycle_mismatch{"responses.text.lifecycle_mismatch", "text event lifecycle mismatch"};
inline constexpr Reason text_string_required{"responses.text.string_required", "text event string required"};
inline constexpr Reason text_logprobs_invalid{"responses.text.logprobs_invalid", "invalid text event logprobs"};
inline constexpr Reason text_byte_limit{"responses.text.byte_limit", "text byte limit"};
inline constexpr Reason reasoning_summary_limit{"responses.reasoning.summary_limit", "reasoning summary limit"};
inline constexpr Reason logprobs_initial_contradiction{"responses.logprobs.initial_contradiction", "initial logprobs contradict snapshot"};
inline constexpr Reason logprobs_delta_contradiction{"responses.logprobs.delta_contradiction", "delta logprobs contradict snapshot"};
inline constexpr Reason logprobs_done_contradiction{"responses.logprobs.done_contradiction", "done logprobs contradict part snapshot"};
inline constexpr Reason content_disappeared{"responses.content.disappeared", "content disappeared at item done"};
inline constexpr Reason content_array_required{"responses.content.array_required", "item content array required"};
inline constexpr Reason content_count_mismatch{"responses.content.count_mismatch", "done item content count differs"};
inline constexpr Reason content_item_contradiction{"responses.content.item_contradiction", "item done contradicts content done"};
inline constexpr Reason content_index_gap{"responses.content.index_gap", "content index gap"};
inline constexpr Reason item_done_mismatch{"responses.item.done_mismatch", "output item done lifecycle mismatch"};
inline constexpr Reason item_done_identity_mismatch{"responses.item.done_identity_mismatch", "output item done identity mismatch"};
inline constexpr Reason item_status_changed{"responses.item.status_changed", "item terminal status changed"};
inline constexpr Reason item_image_snapshot_changed{"responses.item.image_snapshot_changed", "terminal image item snapshot changed"};
inline constexpr Reason item_function_snapshot_contradiction{"responses.item.function_snapshot_contradiction", "function done snapshot contradiction"};
inline constexpr Reason usage_counter_invalid{"responses.usage.counter_invalid", "usage counter must be nonnegative integer or null"};
inline constexpr Reason usage_leaf_invalid{"responses.usage.leaf_invalid", "numeric usage leaf must be nonnegative integer"};
inline constexpr Reason usage_object_invalid{"responses.usage.object_invalid", "usage must be object or null"};
inline constexpr Reason usage_details_invalid{"responses.usage.details_invalid", "usage details must be object or null"};
inline constexpr Reason usage_total_overflow{"responses.usage.total_overflow", "usage total overflow"};
inline constexpr Reason terminal_missing{"responses.terminal.missing", "missing Responses terminal evidence"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    retained_metadata_limit, retained_metadata_invalid, buffered_unexpected, stream_frame_unexpected,
    sentinel_terminal_missing, event_index_required, event_index_limit, event_item_mismatch, context_required,
    remote_error_event, response_byte_limit, response_json_invalid, response_object_required, event_type_required,
    event_type_mismatch, event_sequence_invalid, remote_failure, event_unsupported, stream_output_after_terminal,
    stream_event_before_created, arguments_lifecycle_mismatch, arguments_string_required,
    arguments_snapshot_contradiction, arguments_limit, annotation_item_mismatch, annotation_invalid,
    server_event_item_mismatch, server_event_partial_image_invalid, identity_invalid, identity_changed,
    response_payload_required, response_error_invalid, remote_error_response, response_status_or_output_invalid,
    remote_failure_status, response_status_unsupported, buffered_not_terminal, done_envelope_required,
    response_status_mismatch, incomplete_details_early, incomplete_details_unexpected, incomplete_reason_required,
    incomplete_reason_unsupported, created_required, created_duplicate, response_status_regressed,
    created_output_not_empty, output_count_mismatch, output_item_contradiction, output_incomplete_item,
    output_part_limit, item_identity_required, item_message_invalid, item_function_invalid,
    item_computer_call_invalid, item_mcp_approval_request_invalid, item_image_data_url_invalid,
    item_reasoning_invalid, item_server_status_invalid, item_server_status_empty, item_server_nonterminal,
    item_type_unsupported, item_metadata_changed, item_limit, item_index_duplicate, item_id_duplicate,
    item_call_id_duplicate, text_snapshot_contradiction, content_index_limit, content_type_required,
    content_type_unsupported, content_text_required, content_text_metadata_invalid, content_index_duplicate,
    content_done_mismatch, annotation_snapshot_contradiction, annotation_initial_contradiction, text_item_mismatch,
    text_before_part, text_lifecycle_mismatch, text_string_required, text_logprobs_invalid, text_byte_limit,
    reasoning_summary_limit, logprobs_initial_contradiction, logprobs_delta_contradiction,
    logprobs_done_contradiction, content_disappeared, content_array_required, content_count_mismatch,
    content_item_contradiction, content_index_gap, item_done_mismatch, item_done_identity_mismatch,
    item_status_changed, item_image_snapshot_changed, item_function_snapshot_contradiction, usage_counter_invalid,
    usage_leaf_invalid, usage_object_invalid, usage_details_invalid, usage_total_overflow, terminal_missing,
};
}  // namespace responses
namespace gemini {
inline constexpr Reason retained_json_invalid{"gemini.retained.json_invalid", "invalid retained Gemini JSON"};
inline constexpr Reason buffered_unexpected{"gemini.buffered.unexpected", "unexpected buffered Gemini response"};
inline constexpr Reason stream_frame_unexpected{"gemini.stream.frame_unexpected", "unexpected Gemini stream frame"};
inline constexpr Reason response_id_changed{"gemini.response_id.changed", "Gemini response identity changed"};
inline constexpr Reason model_identity_changed{"gemini.model.identity_changed", "Gemini model identity changed"};
inline constexpr Reason remote_error_event{"gemini.remote.error_event", "remote Gemini error event"};
inline constexpr Reason event_unknown{"gemini.event.unknown", "unknown Gemini SSE event"};
inline constexpr Reason context_required{"gemini.context.required", "Gemini request context required"};
inline constexpr Reason response_byte_limit{"gemini.response.byte_limit", "Gemini response byte limit"};
inline constexpr Reason response_json_invalid{"gemini.response.json_invalid", "invalid Gemini response JSON"};
inline constexpr Reason response_object_required{"gemini.response.object_required", "Gemini response must be object"};
inline constexpr Reason remote_error_response{"gemini.remote.error_response", "remote Gemini error response"};
inline constexpr Reason prompt_feedback_invalid{"gemini.prompt_feedback.invalid", "invalid Gemini prompt feedback"};
inline constexpr Reason prompt_feedback_blocked{"gemini.prompt_feedback.blocked", "Gemini prompt blocked"};
inline constexpr Reason candidates_single_required{"gemini.candidates.single_required", "Gemini requires a single candidate"};
inline constexpr Reason candidate_invalid{"gemini.candidate.invalid", "invalid Gemini candidate"};
inline constexpr Reason candidate_index_mismatch{"gemini.candidate.index_mismatch", "Gemini candidate index mismatch"};
inline constexpr Reason finish_reason_invalid{"gemini.finish_reason.invalid", "invalid Gemini finish reason"};
inline constexpr Reason finish_reason_candidate_after{"gemini.finish_reason.candidate_after", "Gemini candidate after finish reason"};
inline constexpr Reason candidate_content_invalid{"gemini.candidate.content_invalid", "invalid Gemini candidate content"};
inline constexpr Reason candidate_role_invalid{"gemini.candidate.role_invalid", "invalid Gemini output role"};
inline constexpr Reason candidate_parts_required{"gemini.candidate.parts_required", "Gemini parts array required outside an empty terminal"};
inline constexpr Reason remote_generation_blocked{"gemini.remote.generation_blocked", "Gemini generation blocked or rejected"};
inline constexpr Reason finish_reason_failure{"gemini.finish_reason.failure", "Gemini failure finish reason"};
inline constexpr Reason finish_reason_cancelled{"gemini.finish_reason.cancelled", "Gemini cancelled finish reason"};
inline constexpr Reason finish_reason_unsupported{"gemini.finish_reason.unsupported", "unsupported Gemini finish reason"};
inline constexpr Reason part_invalid{"gemini.part.invalid", "invalid Gemini part"};
inline constexpr Reason part_limit{"gemini.part.limit", "Gemini part limit"};
inline constexpr Reason part_thought_invalid{"gemini.part.thought_invalid", "invalid Gemini thought metadata"};
inline constexpr Reason part_fields_conflict{"gemini.part.fields_conflict", "Gemini part contains conflicting data fields"};
inline constexpr Reason part_semantic_unknown{"gemini.part.semantic_unknown", "unknown Gemini part semantic"};
inline constexpr Reason part_text_invalid{"gemini.part.text_invalid", "Gemini text must be string"};
inline constexpr Reason part_transcription_invalid{"gemini.part.transcription_invalid", "invalid Gemini audio transcription"};
inline constexpr Reason function_call_identity_invalid{"gemini.function_call.identity_invalid", "invalid Gemini function identity"};
inline constexpr Reason function_call_id_invalid{"gemini.function_call.id_invalid", "invalid Gemini function call id"};
inline constexpr Reason function_call_undeclared{"gemini.function_call.undeclared", "undeclared Gemini client function"};
inline constexpr Reason function_call_partial_args_unsupported{"gemini.function_call.partial_args_unsupported", "streamed partial function arguments unsupported"};
inline constexpr Reason function_call_ownership_exhausted{"gemini.function_call.ownership_exhausted", "Gemini local ownership id exhausted"};
inline constexpr Reason function_call_id_duplicate{"gemini.function_call.id_duplicate", "duplicate Gemini function call id"};
inline constexpr Reason part_media_invalid{"gemini.part.media_invalid", "invalid Gemini media part"};
inline constexpr Reason part_opaque_invalid{"gemini.part.opaque_invalid", "invalid Gemini opaque part payload"};
inline constexpr Reason server_tool_call_invalid{"gemini.server_tool.call_invalid", "invalid Gemini server tool call"};
inline constexpr Reason server_tool_result_invalid{"gemini.server_tool.result_invalid", "invalid Gemini server tool result"};
inline constexpr Reason usage_invalid{"gemini.usage.invalid", "invalid Gemini usage"};
inline constexpr Reason usage_counter_invalid{"gemini.usage.counter_invalid", "invalid Gemini usage counter"};
inline constexpr Reason usage_sum_overflow{"gemini.usage.sum_overflow", "Gemini usage sum overflow"};
inline constexpr Reason usage_details_invalid{"gemini.usage.details_invalid", "invalid Gemini usage details"};
inline constexpr Reason usage_modality_invalid{"gemini.usage.modality_invalid", "invalid Gemini modality usage"};
inline constexpr Reason usage_modality_duplicate{"gemini.usage.modality_duplicate", "duplicate Gemini modality usage"};
inline constexpr Reason terminal_missing{"gemini.terminal.missing", "missing Gemini finish reason"};
inline constexpr Reason native_array_limit{"gemini.native.array_limit", "Gemini native array limit"};
inline constexpr Reason native_array_invalid{"gemini.native.array_invalid", "invalid Gemini native array"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    retained_json_invalid, buffered_unexpected, stream_frame_unexpected, response_id_changed, model_identity_changed,
    remote_error_event, event_unknown, context_required, response_byte_limit, response_json_invalid,
    response_object_required, remote_error_response, prompt_feedback_invalid, prompt_feedback_blocked,
    candidates_single_required, candidate_invalid, candidate_index_mismatch, finish_reason_invalid,
    finish_reason_candidate_after, candidate_content_invalid, candidate_role_invalid, candidate_parts_required,
    remote_generation_blocked, finish_reason_failure, finish_reason_cancelled, finish_reason_unsupported,
    part_invalid, part_limit, part_thought_invalid, part_fields_conflict, part_semantic_unknown, part_text_invalid,
    part_transcription_invalid, function_call_identity_invalid, function_call_id_invalid, function_call_undeclared,
    function_call_partial_args_unsupported, function_call_ownership_exhausted, function_call_id_duplicate,
    part_media_invalid, part_opaque_invalid, server_tool_call_invalid, server_tool_result_invalid, usage_invalid,
    usage_counter_invalid, usage_sum_overflow, usage_details_invalid, usage_modality_invalid,
    usage_modality_duplicate, terminal_missing, native_array_limit, native_array_invalid,
};
}  // namespace gemini
namespace interactions {
inline constexpr Reason retained_json_limit{"interactions.retained.json_limit", "retained JSON limit"};
inline constexpr Reason retained_json_invalid{"interactions.retained.json_invalid", "invalid retained JSON"};
inline constexpr Reason buffered_unexpected{"interactions.buffered.unexpected", "unexpected buffered interaction"};
inline constexpr Reason stream_frame_unexpected{"interactions.stream.frame_unexpected", "unexpected interaction frame"};
inline constexpr Reason stream_sentinel_unexpected{"interactions.stream.sentinel_unexpected", "unexpected stream sentinel"};
inline constexpr Reason identity_invalid{"interactions.identity.invalid", "invalid interaction identity or status"};
inline constexpr Reason resource_agent_unsupported{"interactions.resource.agent_unsupported", "agent or environment interaction unsupported"};
inline constexpr Reason remote_error{"interactions.remote.error", "remote interaction error"};
inline constexpr Reason identity_changed{"interactions.identity.changed", "interaction identity changed"};
inline constexpr Reason service_step_identity_invalid{"interactions.service_step.identity_invalid", "invalid service tool step identity"};
inline constexpr Reason service_step_delta_repeated{"interactions.service_step.delta_repeated", "repeated service tool step delta"};
inline constexpr Reason service_step_assembly_invalid{"interactions.service_step.assembly_invalid", "invalid assembled service tool step"};
inline constexpr Reason service_step_final_mismatch{"interactions.service_step.final_mismatch", "final service tool step identity mismatch"};
inline constexpr Reason service_step_arguments_invalid{"interactions.service_step.arguments_invalid", "service tool arguments must be object"};
inline constexpr Reason model_invalid{"interactions.model.invalid", "invalid interaction model"};
inline constexpr Reason model_changed{"interactions.model.changed", "interaction model changed"};
inline constexpr Reason context_required{"interactions.context.required", "Interactions request context required"};
inline constexpr Reason response_byte_limit{"interactions.response.byte_limit", "interaction response byte limit"};
inline constexpr Reason response_json_invalid{"interactions.response.json_invalid", "invalid interaction JSON"};
inline constexpr Reason response_object_required{"interactions.response.object_required", "interaction object required"};
inline constexpr Reason buffered_model_required{"interactions.buffered.model_required", "buffered interaction model required"};
inline constexpr Reason buffered_steps_invalid{"interactions.buffered.steps_invalid", "invalid buffered interaction steps"};
inline constexpr Reason event_discriminator_mismatch{"interactions.event.discriminator_mismatch", "interaction event discriminator mismatch"};
inline constexpr Reason stream_event_after_terminal{"interactions.stream.event_after_terminal", "event after terminal interaction"};
inline constexpr Reason created_duplicate{"interactions.created.duplicate", "duplicate interaction creation"};
inline constexpr Reason created_status_invalid{"interactions.created.status_invalid", "new interaction must be in progress"};
inline constexpr Reason created_steps_unexpected{"interactions.created.steps_unexpected", "creation contains unexpected steps"};
inline constexpr Reason stream_event_before_created{"interactions.stream.event_before_created", "event before interaction creation"};
inline constexpr Reason status_update_invalid{"interactions.status.update_invalid", "invalid interaction status update"};
inline constexpr Reason remote_failure_status{"interactions.remote.failure_status", "interaction failure status"};
inline constexpr Reason status_cancelled{"interactions.status.cancelled", "interaction cancelled status"};
inline constexpr Reason status_update_unsupported{"interactions.status.update_unsupported", "unsupported interaction status update"};
inline constexpr Reason event_unsupported{"interactions.event.unsupported", "unsupported interaction event"};
inline constexpr Reason step_index_invalid{"interactions.step.index_invalid", "invalid interaction step index"};
inline constexpr Reason step_delta_metadata_invalid{"interactions.step.delta_metadata_invalid", "invalid step delta metadata"};
inline constexpr Reason step_type_required{"interactions.step.type_required", "step type required"};
inline constexpr Reason step_index_repeated{"interactions.step.index_repeated", "invalid or repeated step index"};
inline constexpr Reason step_type_unsupported{"interactions.step.type_unsupported", "unsupported critical interaction step"};
inline constexpr Reason remote_step_error{"interactions.remote.step_error", "remote step error"};
inline constexpr Reason thought_signature_invalid{"interactions.thought.signature_invalid", "invalid thought signature"};
inline constexpr Reason thought_summary_invalid{"interactions.thought.summary_invalid", "invalid thought summary"};
inline constexpr Reason function_identity_invalid{"interactions.function.identity_invalid", "invalid function step identity"};
inline constexpr Reason function_undeclared{"interactions.function.undeclared", "undeclared interaction function"};
inline constexpr Reason output_content_invalid{"interactions.output.content_invalid", "invalid model output content"};
inline constexpr Reason output_block_invalid{"interactions.output.block_invalid", "invalid model output block"};
inline constexpr Reason output_content_unsupported{"interactions.output.content_unsupported", "unsupported model output content"};
inline constexpr Reason output_text_required{"interactions.output.text_required", "model output text required"};
inline constexpr Reason output_part_limit{"interactions.output.part_limit", "interaction part limit"};
inline constexpr Reason output_media_invalid{"interactions.output.media_invalid", "model output media needs a valid MIME type and exactly one of data or uri"};
inline constexpr Reason output_media_changed{"interactions.output.media_changed", "model output media description changed"};
inline constexpr Reason output_audio_invalid{"interactions.output.audio_invalid", "model output audio must be canonical base64"};
inline constexpr Reason output_audio_limit{"interactions.output.audio_limit", "model output audio limit"};
inline constexpr Reason output_media_data_invalid{"interactions.output.media_data_invalid", "model output media must be canonical base64"};
inline constexpr Reason output_media_location_invalid{"interactions.output.media_location_invalid", "model output media location is invalid"};
inline constexpr Reason thought_summary_content_invalid{"interactions.thought.summary_content_invalid", "invalid thought summary content"};
inline constexpr Reason annotation_array_required{"interactions.annotation.array_required", "text annotations must be an array"};
inline constexpr Reason annotation_discriminator_required{"interactions.annotation.discriminator_required", "text annotation discriminator required"};
inline constexpr Reason annotation_type_unsupported{"interactions.annotation.type_unsupported", "unsupported critical text annotation"};
inline constexpr Reason annotation_index_outside{"interactions.annotation.index_outside", "text annotation byte index outside content"};
inline constexpr Reason annotation_range_reversed{"interactions.annotation.range_reversed", "text annotation byte range reversed"};
inline constexpr Reason annotation_string_invalid{"interactions.annotation.string_invalid", "invalid text annotation string metadata"};
inline constexpr Reason annotation_metadata_invalid{"interactions.annotation.metadata_invalid", "invalid text annotation metadata"};
inline constexpr Reason annotation_word_contradiction{"interactions.annotation.word_contradiction", "transcribed word contradicts annotated text"};
inline constexpr Reason annotation_word_missing{"interactions.annotation.word_missing", "transcribed word missing from annotated text"};
inline constexpr Reason delta_step_closed{"interactions.delta.step_closed", "delta without open step"};
inline constexpr Reason delta_after_signature{"interactions.delta.after_signature", "delta after final thought signature"};
inline constexpr Reason delta_signature_invalid{"interactions.delta.signature_invalid", "invalid thought signature delta"};
inline constexpr Reason delta_text_required{"interactions.delta.text_required", "text delta required"};
inline constexpr Reason delta_annotation_without_text{"interactions.delta.annotation_without_text", "text annotation without current model output text"};
inline constexpr Reason delta_arguments_invalid{"interactions.delta.arguments_invalid", "invalid arguments delta"};
inline constexpr Reason delta_arguments_conflict{"interactions.delta.arguments_conflict", "arguments delta conflicts with complete initial object"};
inline constexpr Reason delta_arguments_limit{"interactions.delta.arguments_limit", "function argument limit"};
inline constexpr Reason delta_step_mismatch{"interactions.delta.step_mismatch", "unsupported or mismatched critical step delta"};
inline constexpr Reason step_stop_without_open{"interactions.step.stop_without_open", "stop without open step"};
inline constexpr Reason step_assembled_limit{"interactions.step.assembled_limit", "assembled step limit"};
inline constexpr Reason function_assembled_arguments_invalid{"interactions.function.assembled_arguments_invalid", "invalid assembled function arguments"};
inline constexpr Reason final_step_type_mismatch{"interactions.final.step_type_mismatch", "final step type mismatch"};
inline constexpr Reason remote_final_step_error{"interactions.remote.final_step_error", "final step error"};
inline constexpr Reason final_thought_invalid{"interactions.final.thought_invalid", "invalid final thought"};
inline constexpr Reason final_thought_summary_lost{"interactions.final.thought_summary_lost", "final thought summary lost"};
inline constexpr Reason final_thought_summary_mismatch{"interactions.final.thought_summary_mismatch", "final thought summary mismatch"};
inline constexpr Reason final_function_ownership_mismatch{"interactions.final.function_ownership_mismatch", "final function ownership mismatch"};
inline constexpr Reason final_function_arguments_mismatch{"interactions.final.function_arguments_mismatch", "final function arguments mismatch"};
inline constexpr Reason function_streamed_arguments_invalid{"interactions.function.streamed_arguments_invalid", "invalid streamed function arguments"};
inline constexpr Reason final_output_content_mismatch{"interactions.final.output_content_mismatch", "final output content mismatch"};
inline constexpr Reason final_output_block_invalid{"interactions.final.output_block_invalid", "invalid final output block"};
inline constexpr Reason final_output_content_unsupported{"interactions.final.output_content_unsupported", "unsupported final output content"};
inline constexpr Reason final_output_type_mismatch{"interactions.final.output_type_mismatch", "final output content type mismatch"};
inline constexpr Reason final_output_prefix_mismatch{"interactions.final.output_prefix_mismatch", "final output prefix mismatch"};
inline constexpr Reason final_annotations_lost{"interactions.final.annotations_lost", "final text annotations lost"};
inline constexpr Reason final_annotation_prefix_mismatch{"interactions.final.annotation_prefix_mismatch", "final text annotation prefix mismatch"};
inline constexpr Reason final_media_type_mismatch{"interactions.final.media_type_mismatch", "final output media type mismatch"};
inline constexpr Reason final_media_mismatch{"interactions.final.media_mismatch", "final output media mismatch"};
inline constexpr Reason final_media_description_mismatch{"interactions.final.media_description_mismatch", "final output media description mismatch"};
inline constexpr Reason function_arguments_object_required{"interactions.function.arguments_object_required", "function arguments must be object"};
inline constexpr Reason remote_failed{"interactions.remote.failed", "interaction failed"};
inline constexpr Reason terminal_cancelled{"interactions.terminal.cancelled", "interaction cancelled"};
inline constexpr Reason terminal_status_unsupported{"interactions.terminal.status_unsupported", "unsupported terminal interaction status"};
inline constexpr Reason terminal_action_mismatch{"interactions.terminal.action_mismatch", "terminal status disagrees with client action"};
inline constexpr Reason terminal_step_count_mismatch{"interactions.terminal.step_count_mismatch", "terminal step count mismatch"};
inline constexpr Reason terminal_steps_open{"interactions.terminal.steps_open", "terminal with missing or open steps"};
inline constexpr Reason atomic_output_limit{"interactions.atomic.output_limit", "atomic interaction output limit"};
inline constexpr Reason atomic_output_invalid{"interactions.atomic.output_invalid", "invalid atomic interaction output"};
inline constexpr Reason usage_count_invalid{"interactions.usage.count_invalid", "invalid interaction usage count"};
inline constexpr Reason usage_modality_duplicate{"interactions.usage.modality_duplicate", "duplicate interaction modality usage"};
inline constexpr Reason usage_leaf_invalid{"interactions.usage.leaf_invalid", "invalid numeric usage leaf"};
inline constexpr Reason usage_object_required{"interactions.usage.object_required", "interaction usage object required"};
inline constexpr Reason terminal_missing{"interactions.terminal.missing", "interaction lacks terminal resource"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    retained_json_limit, retained_json_invalid, buffered_unexpected, stream_frame_unexpected,
    stream_sentinel_unexpected, identity_invalid, resource_agent_unsupported, remote_error, identity_changed,
    service_step_identity_invalid, service_step_delta_repeated, service_step_assembly_invalid,
    service_step_final_mismatch, service_step_arguments_invalid, model_invalid, model_changed, context_required,
    response_byte_limit, response_json_invalid, response_object_required, buffered_model_required,
    buffered_steps_invalid, event_discriminator_mismatch, stream_event_after_terminal, created_duplicate,
    created_status_invalid, created_steps_unexpected, stream_event_before_created, status_update_invalid,
    remote_failure_status, status_cancelled, status_update_unsupported, event_unsupported, step_index_invalid,
    step_delta_metadata_invalid, step_type_required, step_index_repeated, step_type_unsupported, remote_step_error,
    thought_signature_invalid, thought_summary_invalid, function_identity_invalid, function_undeclared,
    output_content_invalid, output_block_invalid, output_content_unsupported, output_text_required, output_part_limit,
    output_media_invalid, output_media_changed, output_audio_invalid, output_audio_limit, output_media_data_invalid,
    output_media_location_invalid, thought_summary_content_invalid, annotation_array_required,
    annotation_discriminator_required, annotation_type_unsupported, annotation_index_outside,
    annotation_range_reversed, annotation_string_invalid, annotation_metadata_invalid, annotation_word_contradiction,
    annotation_word_missing, delta_step_closed, delta_after_signature, delta_signature_invalid, delta_text_required,
    delta_annotation_without_text, delta_arguments_invalid, delta_arguments_conflict, delta_arguments_limit,
    delta_step_mismatch, step_stop_without_open, step_assembled_limit, function_assembled_arguments_invalid,
    final_step_type_mismatch, remote_final_step_error, final_thought_invalid, final_thought_summary_lost,
    final_thought_summary_mismatch, final_function_ownership_mismatch, final_function_arguments_mismatch,
    function_streamed_arguments_invalid, final_output_content_mismatch, final_output_block_invalid,
    final_output_content_unsupported, final_output_type_mismatch, final_output_prefix_mismatch,
    final_annotations_lost, final_annotation_prefix_mismatch, final_media_type_mismatch, final_media_mismatch,
    final_media_description_mismatch, function_arguments_object_required, remote_failed, terminal_cancelled,
    terminal_status_unsupported, terminal_action_mismatch, terminal_step_count_mismatch, terminal_steps_open,
    atomic_output_limit, atomic_output_invalid, usage_count_invalid, usage_modality_duplicate, usage_leaf_invalid,
    usage_object_required, terminal_missing,
};
}  // namespace interactions
namespace accumulator {
inline constexpr Reason begin_duplicate{"accumulator.begin.duplicate", "duplicate begin"};
inline constexpr Reason message_begin_invalid{"accumulator.message.begin_invalid", "invalid message begin"};
inline constexpr Reason message_limit{"accumulator.message.limit", "message limit"};
inline constexpr Reason part_begin_invalid{"accumulator.part.begin_invalid", "invalid part begin"};
inline constexpr Reason part_limit{"accumulator.part.limit", "part limit"};
inline constexpr Reason part_order_duplicate{"accumulator.part.order_duplicate", "duplicate part order"};
inline constexpr Reason part_header_limit{"accumulator.part.header_limit", "part header limit"};
inline constexpr Reason media_header_invalid{"accumulator.media.header_invalid", "invalid generated media header"};
inline constexpr Reason media_header_payload{"accumulator.media.header_payload", "generated media header must not carry payload"};
inline constexpr Reason media_description_invalid{"accumulator.media.description_invalid", "invalid generated media description"};
inline constexpr Reason part_metadata_object_required{"accumulator.part.metadata_object_required", "tool metadata must be object"};
inline constexpr Reason part_metadata_limit{"accumulator.part.metadata_limit", "part metadata limit"};
inline constexpr Reason part_delta_invalid{"accumulator.part.delta_invalid", "invalid part delta"};
inline constexpr Reason part_delta_channel_invalid{"accumulator.part.delta_channel_invalid", "invalid delta channel"};
inline constexpr Reason part_signature_misplaced{"accumulator.part.signature_misplaced", "signature on non-thinking part"};
inline constexpr Reason part_content_limit{"accumulator.part.content_limit", "content limit"};
inline constexpr Reason part_seal_invalid{"accumulator.part.seal_invalid", "invalid part seal"};
inline constexpr Reason part_native_metadata_invalid{"accumulator.part.native_metadata_invalid", "invalid native part metadata"};
inline constexpr Reason part_native_snapshot_contradiction{"accumulator.part.native_snapshot_contradiction", "contradictory native part snapshot"};
inline constexpr Reason part_native_metadata_limit{"accumulator.part.native_metadata_limit", "native part metadata limit"};
inline constexpr Reason reasoning_item_invalid{"accumulator.reasoning.item_invalid", "invalid completed reasoning item"};
inline constexpr Reason reasoning_summary_contradiction{"accumulator.reasoning.summary_contradiction", "contradictory reasoning summary snapshot"};
inline constexpr Reason reasoning_summary_incomplete{"accumulator.reasoning.summary_incomplete", "incomplete reasoning summary snapshot"};
inline constexpr Reason thought_step_invalid{"accumulator.thought.step_invalid", "invalid completed thought step"};
inline constexpr Reason thought_fields_invalid{"accumulator.thought.fields_invalid", "invalid thought fields"};
inline constexpr Reason thought_summary_content_invalid{"accumulator.thought.summary_content_invalid", "invalid thought summary content"};
inline constexpr Reason thought_image_invalid{"accumulator.thought.image_invalid", "invalid thought image summary"};
inline constexpr Reason thought_summary_unsupported{"accumulator.thought.summary_unsupported", "unsupported thought summary content"};
inline constexpr Reason thought_summary_contradiction{"accumulator.thought.summary_contradiction", "contradictory thought summary snapshot"};
inline constexpr Reason thought_summary_incomplete{"accumulator.thought.summary_incomplete", "incomplete thought summary snapshot"};
inline constexpr Reason opaque_metadata_missing{"accumulator.opaque.metadata_missing", "missing opaque item metadata"};
inline constexpr Reason opaque_google_invalid{"accumulator.opaque.google_invalid", "invalid native Google opaque part"};
inline constexpr Reason opaque_metadata_invalid{"accumulator.opaque.metadata_invalid", "invalid opaque item metadata"};
inline constexpr Reason part_snapshot_limit{"accumulator.part.snapshot_limit", "snapshot limit"};
inline constexpr Reason part_sealed_contradiction{"accumulator.part.sealed_contradiction", "contradictory sealed snapshot"};
inline constexpr Reason part_snapshot_contradiction{"accumulator.part.snapshot_contradiction", "contradictory part snapshot"};
inline constexpr Reason media_payload_invalid{"accumulator.media.payload_invalid", "invalid generated media payload"};
inline constexpr Reason citations_not_text{"accumulator.citations.not_text", "citations on a part that is not text"};
inline constexpr Reason part_server_result_invalid{"accumulator.part.server_result_invalid", "invalid server result block"};
inline constexpr Reason message_seal_invalid{"accumulator.message.seal_invalid", "invalid message seal"};
inline constexpr Reason message_part_unsealed{"accumulator.message.part_unsealed", "unsealed message part"};
inline constexpr Reason message_native_output_invalid{"accumulator.message.native_output_invalid", "native output must be ordered array"};
inline constexpr Reason message_native_output_limit{"accumulator.message.native_output_limit", "native output metadata limit"};
inline constexpr Reason raw_event_invalid{"accumulator.raw.event_invalid", "invalid raw wire event"};
inline constexpr Reason raw_retention_limit{"accumulator.raw.retention_limit", "raw wire retention limit"};
inline constexpr Reason envelope_invalid{"accumulator.envelope.invalid", "invalid response envelope"};
inline constexpr Reason envelope_limit{"accumulator.envelope.limit", "response envelope limit"};
inline constexpr Reason usage_transition_invalid{"accumulator.usage.transition_invalid", "invalid usage transition"};
inline constexpr Reason stop_transition_invalid{"accumulator.stop.transition_invalid", "invalid stop transition"};
inline constexpr Reason commit_evidence_missing{"accumulator.commit.evidence_missing", "commit without evidence"};
inline constexpr Reason commit_message_open{"accumulator.commit.message_open", "commit with open message"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    begin_duplicate, message_begin_invalid, message_limit, part_begin_invalid, part_limit, part_order_duplicate,
    part_header_limit, media_header_invalid, media_header_payload, media_description_invalid,
    part_metadata_object_required, part_metadata_limit, part_delta_invalid, part_delta_channel_invalid,
    part_signature_misplaced, part_content_limit, part_seal_invalid, part_native_metadata_invalid,
    part_native_snapshot_contradiction, part_native_metadata_limit, reasoning_item_invalid,
    reasoning_summary_contradiction, reasoning_summary_incomplete, thought_step_invalid, thought_fields_invalid,
    thought_summary_content_invalid, thought_image_invalid, thought_summary_unsupported,
    thought_summary_contradiction, thought_summary_incomplete, opaque_metadata_missing, opaque_google_invalid,
    opaque_metadata_invalid, part_snapshot_limit, part_sealed_contradiction, part_snapshot_contradiction,
    media_payload_invalid, citations_not_text, part_server_result_invalid, message_seal_invalid,
    message_part_unsealed, message_native_output_invalid, message_native_output_limit, raw_event_invalid,
    raw_retention_limit, envelope_invalid, envelope_limit, usage_transition_invalid, stop_transition_invalid,
    commit_evidence_missing, commit_message_open,
};
}  // namespace accumulator
namespace chat_request {
inline constexpr Reason model_and_messages_are_required{"request.chat.model_and_messages_are_required", "model and messages are required"};
inline constexpr Reason choose_one_message_representation{"request.chat.choose_one_message_representation", "choose one message representation"};
inline constexpr Reason gateway_controls_require_declared_openrouter{"request.chat.gateway_controls_require_declared_openrouter", "gateway controls require declared OpenRouter origin"};
inline constexpr Reason reasoning_options_must_not_be_empty{"request.chat.reasoning_options_must_not_be_empty", "reasoning options must not be empty"};
inline constexpr Reason unsupported_reasoning_effort{"request.chat.unsupported_reasoning_effort", "unsupported reasoning effort"};
inline constexpr Reason reasoning_effort_and_max_tokens_are_mutually{"request.chat.reasoning_effort_and_max_tokens_are_mutually", "reasoning effort and max_tokens are mutually exclusive"};
inline constexpr Reason reasoning_max_tokens_outside_output_budget{"request.chat.reasoning_max_tokens_outside_output_budget", "reasoning max_tokens outside output budget"};
inline constexpr Reason disabled_reasoning_cannot_specify_effort_or{"request.chat.disabled_reasoning_cannot_specify_effort_or", "disabled reasoning cannot specify effort or budget"};
inline constexpr Reason scalar_and_nested_reasoning_controls_conflict{"request.chat.scalar_and_nested_reasoning_controls_conflict", "scalar and nested reasoning controls conflict"};
inline constexpr Reason excluded_reasoning_cannot_be_included{"request.chat.excluded_reasoning_cannot_be_included", "excluded reasoning cannot be included"};
inline constexpr Reason alternative_model_count_limit_exceeded{"request.chat.alternative_model_count_limit_exceeded", "alternative model count limit exceeded"};
inline constexpr Reason alternative_models_require_unique_nonempty_names{"request.chat.alternative_models_require_unique_nonempty_names", "alternative models require unique nonempty names"};
inline constexpr Reason request_count_limit_exceeded{"request.chat.request_count_limit_exceeded", "request count limit exceeded"};
inline constexpr Reason request_part_count_limit_exceeded{"request.chat.request_part_count_limit_exceeded", "request part count limit exceeded"};
inline constexpr Reason media_inputs_require_user_or_supported_assistant{"request.chat.media_inputs_require_user_or_supported_assistant", "media inputs require user or supported assistant role"};
inline constexpr Reason one_audio_reply_per_assistant_message{"request.chat.one_audio_reply_per_assistant_message", "one audio reply per assistant message"};
inline constexpr Reason invalid_message_role{"request.chat.invalid_message_role", "invalid message role"};
inline constexpr Reason tool_results_must_precede_the_next_message{"request.chat.tool_results_must_precede_the_next_message", "tool results must precede the next message"};
inline constexpr Reason a_tool_message_holds_only_tool_results_one_or{"request.chat.a_tool_message_holds_only_tool_results_one_or", "a Tool message holds only tool results, one or more"};
inline constexpr Reason tool_result_must_resolve_one_pending_call{"request.chat.tool_result_must_resolve_one_pending_call", "tool result must resolve one pending call"};
inline constexpr Reason tool_result_requires_tool_role{"request.chat.tool_result_requires_tool_role", "tool result requires tool role"};
inline constexpr Reason tool_calls_require_assistant_role{"request.chat.tool_calls_require_assistant_role", "tool calls require assistant role"};
inline constexpr Reason tool_calls_require_unique_ids_names_and_object{"request.chat.tool_calls_require_unique_ids_names_and_object", "tool calls require unique ids, names and object arguments"};
inline constexpr Reason tool_call_id_is_only_valid_on_tool_results{"request.chat.tool_call_id_is_only_valid_on_tool_results", "tool_call_id is only valid on tool results"};
inline constexpr Reason tool_calls_require_an_assistant_message{"request.chat.tool_calls_require_an_assistant_message", "tool calls require an assistant message"};
inline constexpr Reason missing_tool_results{"request.chat.missing_tool_results", "missing tool results"};
inline constexpr Reason tools_require_unique_names_and_object_schemas{"request.chat.tools_require_unique_names_and_object_schemas", "tools require unique names and object schemas"};
inline constexpr Reason chat_input_audio_has_no_format_name_for_this_mime{"request.chat.chat_input_audio_has_no_format_name_for_this_mime", "Chat input_audio has no format name for this MIME type"};
inline constexpr Reason descriptor_family_does_not_match_chat_codec{"request.chat.descriptor_family_does_not_match_chat_codec", "descriptor family does not match Chat codec"};
inline constexpr Reason assistant_media_inputs_require_openrouter_images{"request.chat.assistant_media_inputs_require_openrouter_images", "assistant media inputs require OpenRouter images"};
inline constexpr Reason assistant_images_require_declared_openrouter{"request.chat.assistant_images_require_declared_openrouter", "assistant images require declared OpenRouter origin"};
inline constexpr Reason native_replay_provenance_binding_or_content{"request.chat.native_replay_provenance_binding_or_content", "native replay provenance, binding or content mismatch"};
inline constexpr Reason native_chat_binding_could_not_be_captured{"request.chat.native_chat_binding_could_not_be_captured", "native Chat binding could not be captured"};
inline constexpr Reason invalid_native_chat_history_representation{"request.chat.invalid_native_chat_history_representation", "invalid native Chat history representation"};
inline constexpr Reason chat_assistant_media_requires_an_audio_reply_id{"request.chat.chat_assistant_media_requires_an_audio_reply_id", "Chat assistant media requires an audio reply id or OpenRouter image"};
inline constexpr Reason chat_requires_one_plain_unsigned_assistant{"request.chat.chat_requires_one_plain_unsigned_assistant", "Chat requires one plain unsigned assistant thinking part"};
inline constexpr Reason reasoning_frame_lacks_native_authority{"request.chat.reasoning_frame_lacks_native_authority", "reasoning frame lacks native authority"};
inline constexpr Reason invalid_retained_reasoning_frame{"request.chat.invalid_retained_reasoning_frame", "invalid retained reasoning frame"};
inline constexpr Reason chat_reasoning_details_require_authenticated{"request.chat.chat_reasoning_details_require_authenticated", "Chat reasoning details require authenticated native replay"};
inline constexpr Reason invalid_captured_reasoning_details_snapshot{"request.chat.invalid_captured_reasoning_details_snapshot", "invalid captured reasoning details snapshot"};
inline constexpr Reason chat_cannot_represent_this_ordered_part{"request.chat.chat_cannot_represent_this_ordered_part", "Chat cannot represent this ordered part"};
inline constexpr Reason reasoning_stream_lacks_complete_reconstructed{"request.chat.reasoning_stream_lacks_complete_reconstructed", "reasoning stream lacks complete reconstructed details"};
inline constexpr Reason request_exceeds_json_limits_or_encoding{"request.chat.request_exceeds_json_limits_or_encoding", "request exceeds JSON limits or encoding"};
inline constexpr Reason chat_tool_results_do_not_support_typed_media{"request.chat.chat_tool_results_do_not_support_typed_media", "Chat tool results do not support typed media content"};
inline constexpr Reason refusal_cannot_be_represented_by_chat{"request.chat.refusal_cannot_be_represented_by_chat", "refusal cannot be represented by Chat"};
inline constexpr Reason only_plain_client_function_calls_can_be_replayed{"request.chat.only_plain_client_function_calls_can_be_replayed", "only plain client function calls can be replayed"};
inline constexpr Reason output_modalities_require_unique_valid_values{"request.chat.output_modalities_require_unique_valid_values", "output modalities require unique valid values"};
inline constexpr Reason audio_output_needs_both_the_audio_modality_and{"request.chat.audio_output_needs_both_the_audio_modality_and", "audio output needs both the audio modality and audio parameters"};
inline constexpr Reason audio_output_needs_a_voice_and_a_supported_format{"request.chat.audio_output_needs_a_voice_and_a_supported_format", "audio output needs a voice and a supported format"};
inline constexpr Reason audio_output_needs_a_voice_or_a_voice_id{"request.chat.audio_output_needs_a_voice_or_a_voice_id", "audio output needs a voice or a voice id"};
inline constexpr Reason audio_output_takes_a_voice_or_a_voice_id_not_both{"request.chat.audio_output_takes_a_voice_or_a_voice_id_not_both", "audio output takes a voice or a voice id, not both"};
inline constexpr Reason image_config_needs_the_image_modality{"request.chat.image_config_needs_the_image_modality", "image_config needs the image modality"};
inline constexpr Reason image_output_requires_a_declared_openrouter{"request.chat.image_output_requires_a_declared_openrouter", "image output requires a declared OpenRouter origin"};
inline constexpr Reason cache_marks_text_only{"request.chat.cache_marks_text_only", "Chat carries cache marks on text parts only"};
inline constexpr Reason invalid_tool_choice_mode{"request.chat.invalid_tool_choice_mode", "invalid tool choice mode"};
inline constexpr Reason only_a_function_tool_choice_takes_a_name{"request.chat.only_a_function_tool_choice_takes_a_name", "only a function tool choice takes a name"};
inline constexpr Reason function_tool_choice_must_be_declared{"request.chat.function_tool_choice_must_be_declared", "a function tool choice must name a declared function"};
inline constexpr Reason allowed_functions_require_auto_or_required{"request.chat.allowed_functions_require_auto_or_required", "allowed functions require the auto or required mode"};
inline constexpr Reason allowed_functions_require_at_least_one{"request.chat.allowed_functions_require_at_least_one", "allowed functions require at least one function"};
inline constexpr Reason allowed_functions_must_be_declared{"request.chat.allowed_functions_must_be_declared", "allowed functions must be declared functions"};
inline constexpr Reason allowed_functions_must_be_unique{"request.chat.allowed_functions_must_be_unique", "allowed functions must name each function once"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    model_and_messages_are_required, choose_one_message_representation, gateway_controls_require_declared_openrouter,
    reasoning_options_must_not_be_empty, unsupported_reasoning_effort, reasoning_effort_and_max_tokens_are_mutually,
    reasoning_max_tokens_outside_output_budget, disabled_reasoning_cannot_specify_effort_or,
    scalar_and_nested_reasoning_controls_conflict, excluded_reasoning_cannot_be_included,
    alternative_model_count_limit_exceeded, alternative_models_require_unique_nonempty_names,
    request_count_limit_exceeded, request_part_count_limit_exceeded, media_inputs_require_user_or_supported_assistant,
    one_audio_reply_per_assistant_message, invalid_message_role, tool_results_must_precede_the_next_message,
    a_tool_message_holds_only_tool_results_one_or, tool_result_must_resolve_one_pending_call,
    tool_result_requires_tool_role, tool_calls_require_assistant_role, tool_calls_require_unique_ids_names_and_object,
    tool_call_id_is_only_valid_on_tool_results, tool_calls_require_an_assistant_message, missing_tool_results,
    tools_require_unique_names_and_object_schemas, chat_input_audio_has_no_format_name_for_this_mime,
    descriptor_family_does_not_match_chat_codec, assistant_media_inputs_require_openrouter_images,
    assistant_images_require_declared_openrouter, native_replay_provenance_binding_or_content,
    native_chat_binding_could_not_be_captured, invalid_native_chat_history_representation,
    chat_assistant_media_requires_an_audio_reply_id, chat_requires_one_plain_unsigned_assistant,
    reasoning_frame_lacks_native_authority, invalid_retained_reasoning_frame,
    chat_reasoning_details_require_authenticated, invalid_captured_reasoning_details_snapshot,
    chat_cannot_represent_this_ordered_part, reasoning_stream_lacks_complete_reconstructed,
    request_exceeds_json_limits_or_encoding, chat_tool_results_do_not_support_typed_media,
    refusal_cannot_be_represented_by_chat, only_plain_client_function_calls_can_be_replayed,
    output_modalities_require_unique_valid_values, audio_output_needs_both_the_audio_modality_and,
    audio_output_needs_a_voice_and_a_supported_format, audio_output_needs_a_voice_or_a_voice_id,
    audio_output_takes_a_voice_or_a_voice_id_not_both, image_config_needs_the_image_modality,
    image_output_requires_a_declared_openrouter, cache_marks_text_only, invalid_tool_choice_mode,
    only_a_function_tool_choice_takes_a_name, function_tool_choice_must_be_declared,
    allowed_functions_require_auto_or_required, allowed_functions_require_at_least_one,
    allowed_functions_must_be_declared, allowed_functions_must_be_unique,
};
}  // namespace chat_request
namespace messages_request {
inline constexpr Reason model_and_messages_are_required{"request.messages.model_and_messages_are_required", "model and messages are required"};
inline constexpr Reason temperature_is_prohibited_for_this_model{"request.messages.temperature_is_prohibited_for_this_model", "temperature is prohibited for this model"};
inline constexpr Reason temperature_outside_admitted_range{"request.messages.temperature_outside_admitted_range", "temperature outside admitted range"};
inline constexpr Reason manual_thinking_requires_a_positive_budget{"request.messages.manual_thinking_requires_a_positive_budget", "manual thinking requires a positive budget"};
inline constexpr Reason adaptive_and_disabled_thinking_forbid_budget{"request.messages.adaptive_and_disabled_thinking_forbid_budget", "adaptive and disabled thinking forbid budget"};
inline constexpr Reason invalid_thinking_mode{"request.messages.invalid_thinking_mode", "invalid thinking mode"};
inline constexpr Reason thinking_top_p_outside_admitted_range{"request.messages.thinking_top_p_outside_admitted_range", "thinking top_p outside admitted range"};
inline constexpr Reason invalid_output_effort{"request.messages.invalid_output_effort", "invalid output effort"};
inline constexpr Reason invalid_cache_ttl{"request.messages.invalid_cache_ttl", "invalid cache TTL"};
inline constexpr Reason invalid_tool_choice{"request.messages.invalid_tool_choice", "invalid tool choice"};
inline constexpr Reason named_tool_choice_requires_a_declared_client{"request.messages.named_tool_choice_requires_a_declared_client", "named tool choice requires a declared client tool"};
inline constexpr Reason only_named_tool_choice_accepts_a_name{"request.messages.only_named_tool_choice_accepts_a_name", "only named tool choice accepts a name"};
inline constexpr Reason forced_tool_choice_requires_tools_and{"request.messages.forced_tool_choice_requires_tools_and", "forced tool choice requires tools and thinking disabled"};
inline constexpr Reason none_tool_choice_cannot_control_parallel_use{"request.messages.none_tool_choice_cannot_control_parallel_use", "none tool choice cannot control parallel use"};
inline constexpr Reason request_count_limit_exceeded{"request.messages.request_count_limit_exceeded", "request count limit exceeded"};
inline constexpr Reason request_part_count_limit_exceeded{"request.messages.request_part_count_limit_exceeded", "request part count limit exceeded"};
inline constexpr Reason tools_require_unique_nonempty_names{"request.messages.tools_require_unique_nonempty_names", "tools require unique nonempty names"};
inline constexpr Reason client_tools_require_object_schemas_and_no{"request.messages.client_tools_require_object_schemas_and_no", "client tools require object schemas and no server fields"};
inline constexpr Reason server_tool_fields_do_not_match_typed_schema{"request.messages.server_tool_fields_do_not_match_typed_schema", "server tool fields do not match typed schema"};
inline constexpr Reason max_uses_is_unsupported_or_out_of_range{"request.messages.max_uses_is_unsupported_or_out_of_range", "max_uses is unsupported or out of range"};
inline constexpr Reason tool_role_messages_require_tool_results_only{"request.messages.tool_role_messages_require_tool_results_only", "tool role messages require tool results only"};
inline constexpr Reason client_tool_results_must_immediately_follow{"request.messages.client_tool_results_must_immediately_follow", "client tool results must immediately follow their assistant message"};
inline constexpr Reason server_continuation_requires_the_captured{"request.messages.server_continuation_requires_the_captured", "server continuation requires the captured assistant content"};
inline constexpr Reason pending_server_tools_require_tool_results{"request.messages.pending_server_tools_require_tool_results", "pending server tools require tool-results-only continuation"};
inline constexpr Reason thinking_belongs_to_assistant_messages{"request.messages.thinking_belongs_to_assistant_messages", "thinking belongs to assistant messages"};
inline constexpr Reason redacted_thinking_belongs_to_assistant{"request.messages.redacted_thinking_belongs_to_assistant", "redacted thinking belongs to assistant messages"};
inline constexpr Reason invalid_tool_call_history{"request.messages.invalid_tool_call_history", "invalid tool call history"};
inline constexpr Reason tool_call_type_or_ownership_mismatch{"request.messages.tool_call_type_or_ownership_mismatch", "tool call type or ownership mismatch"};
inline constexpr Reason tool_metadata_must_be_object{"request.messages.tool_metadata_must_be_object", "tool metadata must be object"};
inline constexpr Reason tool_metadata_collides_with_owned_fields{"request.messages.tool_metadata_collides_with_owned_fields", "tool metadata collides with owned fields"};
inline constexpr Reason tool_result_lacks_immediate_unique_client{"request.messages.tool_result_lacks_immediate_unique_client", "tool result lacks immediate unique client ownership"};
inline constexpr Reason server_result_lacks_unique_server_ownership{"request.messages.server_result_lacks_unique_server_ownership", "server result lacks unique server ownership"};
inline constexpr Reason server_result_metadata_mismatch{"request.messages.server_result_metadata_mismatch", "server result metadata mismatch"};
inline constexpr Reason all_client_tool_calls_need_immediate_results{"request.messages.all_client_tool_calls_need_immediate_results", "all client tool calls need immediate results"};
inline constexpr Reason unfinished_server_call_lacks_continuation{"request.messages.unfinished_server_call_lacks_continuation", "unfinished server call lacks continuation stop"};
inline constexpr Reason pause_continuation_cannot_await_client_tools{"request.messages.pause_continuation_cannot_await_client_tools", "pause continuation cannot await client tools"};
inline constexpr Reason client_tool_results_are_required_before{"request.messages.client_tool_results_are_required_before", "client tool results are required before dispatch"};
inline constexpr Reason messages_images_have_no_detail_control{"request.messages.messages_images_have_no_detail_control", "Messages images have no detail control"};
inline constexpr Reason messages_document_title_is_limited_to_500{"request.messages.messages_document_title_is_limited_to_500", "Messages document title length limit exceeded"};
inline constexpr Reason messages_url_documents_must_be_pdf{"request.messages.messages_url_documents_must_be_pdf", "Messages URL documents must be PDF"};
inline constexpr Reason decoded_text_documents_exceed_aggregate{"request.messages.decoded_text_documents_exceed_aggregate", "decoded text documents exceed aggregate request budget"};
inline constexpr Reason messages_text_documents_must_be_valid_utf_8{"request.messages.messages_text_documents_must_be_valid_utf_8", "Messages text documents must be valid UTF-8"};
inline constexpr Reason native_replay_provenance_binding_or_content{"request.messages.native_replay_provenance_binding_or_content", "native replay provenance, binding or content mismatch"};
inline constexpr Reason messages_encoder_requires_messages_descriptor{"request.messages.messages_encoder_requires_messages_descriptor", "Messages encoder requires Messages descriptor"};
inline constexpr Reason media_inputs_require_user_role{"request.messages.media_inputs_require_user_role", "media inputs require user role"};
inline constexpr Reason tool_result_content_and_content_parts_are{"request.messages.tool_result_content_and_content_parts_are", "tool result content and content_parts are mutually exclusive"};
inline constexpr Reason tool_result_part_count_limit_exceeded{"request.messages.tool_result_part_count_limit_exceeded", "tool result part count limit exceeded"};
inline constexpr Reason tool_results_require_user_or_tool_role{"request.messages.tool_results_require_user_or_tool_role", "tool results require user or tool role"};
inline constexpr Reason tool_result_media_contains_unsupported_output{"request.messages.tool_result_media_contains_unsupported_output", "tool result media contains unsupported output metadata"};
inline constexpr Reason native_binding_could_not_be_captured{"request.messages.native_binding_could_not_be_captured", "native binding could not be captured"};
inline constexpr Reason unsupported_server_tool_type{"request.messages.unsupported_server_tool_type", "unsupported server tool type"};
inline constexpr Reason request_exceeds_json_limits_or_encoding{"request.messages.request_exceeds_json_limits_or_encoding", "request exceeds JSON limits or encoding"};
inline constexpr Reason messages_history_supports_only_user_and{"request.messages.messages_history_supports_only_user_and", "Messages history supports only user and assistant roles"};
inline constexpr Reason approval_request_replay_is_unsupported{"request.messages.approval_request_replay_is_unsupported", "approval request replay is unsupported"};
inline constexpr Reason part_cannot_be_replayed_as_messages_input{"request.messages.part_cannot_be_replayed_as_messages_input", "part cannot be replayed as Messages input"};
inline constexpr Reason allowed_callers_invalid{"request.messages.allowed_callers_invalid", "allowed callers must be unique tokens"};
inline constexpr Reason domain_filters_invalid{"request.messages.domain_filters_invalid", "domain filters must be unique host names"};
inline constexpr Reason domain_filters_exclusive{"request.messages.domain_filters_exclusive", "allowed and blocked domains are mutually exclusive"};
inline constexpr Reason max_content_tokens_invalid{"request.messages.max_content_tokens_invalid", "max_content_tokens must be positive"};
inline constexpr Reason user_location_invalid{"request.messages.user_location_invalid", "user location fields must be bounded text"};
inline constexpr Reason container_invalid{"request.messages.container_invalid", "invalid code execution container id"};
inline constexpr Reason beta_count_limit{"request.messages.beta_count_limit", "anthropic-beta value count limit exceeded"};
inline constexpr Reason beta_invalid{"request.messages.beta_invalid", "anthropic-beta values must be single tokens"};
inline constexpr Reason beta_not_admitted{"request.messages.beta_not_admitted", "an anthropic-beta value is not admitted by the policy for this origin"};
inline constexpr Reason tool_beta_not_admitted{"request.messages.tool_beta_not_admitted", "a tool needs an anthropic-beta value this origin does not admit"};
inline constexpr Reason tool_options_not_admitted{"request.messages.tool_options_not_admitted", "the policy admits no options for this tool type"};
inline constexpr Reason tool_options_invalid{"request.messages.tool_options_invalid", "tool options do not match the policy's schema"};
inline constexpr Reason block_beta_not_admitted{"request.messages.block_beta_not_admitted", "a replayed block needs an anthropic-beta value this origin does not admit"};
inline constexpr Reason extension_beta_not_admitted{"request.messages.extension_beta_not_admitted", "an extension needs an anthropic-beta value this origin does not admit"};
inline constexpr Reason input_block_not_admitted{"request.messages.input_block_not_admitted", "an input block is not admitted by the policy"};
inline constexpr Reason input_block_metadata_mismatch{"request.messages.input_block_metadata_mismatch", "input block metadata mismatch"};
inline constexpr Reason input_block_invalid{"request.messages.input_block_invalid", "an input block does not match its declared schema"};
inline constexpr Reason skills_count_limit{"request.messages.skills_count_limit", "container skill count limit exceeded"};
inline constexpr Reason skills_need_code_execution{"request.messages.skills_need_code_execution", "container skills need the code execution tool"};
inline constexpr Reason skills_invalid{"request.messages.skills_invalid", "container skills require unique token ids, types and versions"};
inline constexpr Reason mcp_server_count_limit{"request.messages.mcp_server_count_limit", "MCP server count limit exceeded"};
inline constexpr Reason mcp_server_name_invalid{"request.messages.mcp_server_name_invalid", "MCP servers require unique bounded names"};
inline constexpr Reason mcp_server_url_invalid{"request.messages.mcp_server_url_invalid", "MCP server URLs must be https"};
inline constexpr Reason mcp_server_token_invalid{"request.messages.mcp_server_token_invalid", "MCP authorization tokens must be bounded text"};
inline constexpr Reason mcp_toolset_server_invalid{"request.messages.mcp_toolset_server_invalid", "an MCP toolset must name one declared MCP server"};
inline constexpr Reason mcp_server_without_toolset{"request.messages.mcp_server_without_toolset", "every MCP server needs its MCP toolset"};
inline constexpr Reason captured_block_mismatch{"request.messages.captured_block_mismatch", "captured block metadata mismatch"};
inline constexpr Reason system_blocks_conflict{"request.messages.system_blocks_conflict", "system and system_blocks are mutually exclusive"};
inline constexpr Reason cache_mark_in_tool_result{"request.messages.cache_mark_in_tool_result", "Messages takes a cache mark on a tool result, not on a part inside it"};
inline constexpr Reason system_block_empty{"request.messages.system_block_empty", "system blocks require nonempty text"};
inline constexpr Reason cache_control_duplicate{"request.messages.cache_control_duplicate", "cache_control is set both in a tool's options and as a cache mark"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    model_and_messages_are_required, temperature_is_prohibited_for_this_model, temperature_outside_admitted_range,
    manual_thinking_requires_a_positive_budget, adaptive_and_disabled_thinking_forbid_budget, invalid_thinking_mode,
    thinking_top_p_outside_admitted_range, invalid_output_effort, invalid_cache_ttl, invalid_tool_choice,
    named_tool_choice_requires_a_declared_client, only_named_tool_choice_accepts_a_name,
    forced_tool_choice_requires_tools_and, none_tool_choice_cannot_control_parallel_use, request_count_limit_exceeded,
    request_part_count_limit_exceeded, tools_require_unique_nonempty_names,
    client_tools_require_object_schemas_and_no, server_tool_fields_do_not_match_typed_schema,
    max_uses_is_unsupported_or_out_of_range, tool_role_messages_require_tool_results_only,
    client_tool_results_must_immediately_follow, server_continuation_requires_the_captured,
    pending_server_tools_require_tool_results, thinking_belongs_to_assistant_messages,
    redacted_thinking_belongs_to_assistant, invalid_tool_call_history, tool_call_type_or_ownership_mismatch,
    tool_metadata_must_be_object, tool_metadata_collides_with_owned_fields, tool_result_lacks_immediate_unique_client,
    server_result_lacks_unique_server_ownership, server_result_metadata_mismatch,
    all_client_tool_calls_need_immediate_results, unfinished_server_call_lacks_continuation,
    pause_continuation_cannot_await_client_tools, client_tool_results_are_required_before,
    messages_images_have_no_detail_control, messages_document_title_is_limited_to_500,
    messages_url_documents_must_be_pdf, decoded_text_documents_exceed_aggregate,
    messages_text_documents_must_be_valid_utf_8, native_replay_provenance_binding_or_content,
    messages_encoder_requires_messages_descriptor, media_inputs_require_user_role,
    tool_result_content_and_content_parts_are, tool_result_part_count_limit_exceeded,
    tool_results_require_user_or_tool_role, tool_result_media_contains_unsupported_output,
    native_binding_could_not_be_captured, unsupported_server_tool_type, request_exceeds_json_limits_or_encoding,
    messages_history_supports_only_user_and, approval_request_replay_is_unsupported,
    part_cannot_be_replayed_as_messages_input, allowed_callers_invalid, domain_filters_invalid,
    domain_filters_exclusive, max_content_tokens_invalid, user_location_invalid, container_invalid, beta_count_limit,
    beta_invalid, beta_not_admitted, tool_beta_not_admitted, tool_options_not_admitted, tool_options_invalid,
    block_beta_not_admitted, extension_beta_not_admitted, input_block_not_admitted, input_block_metadata_mismatch,
    input_block_invalid, skills_count_limit, skills_need_code_execution, skills_invalid, mcp_server_count_limit,
    mcp_server_name_invalid, mcp_server_url_invalid, mcp_server_token_invalid, mcp_toolset_server_invalid,
    mcp_server_without_toolset, captured_block_mismatch, system_blocks_conflict, cache_mark_in_tool_result,
    system_block_empty, cache_control_duplicate,
};
}  // namespace messages_request
namespace responses_request {
inline constexpr Reason an_openrouter_server_tool_is_not_admitted_by{"request.responses.an_openrouter_server_tool_is_not_admitted_by", "an OpenRouter server tool is not admitted by the policy"};
inline constexpr Reason openrouter_server_tool_parameters_do_not_match{"request.responses.openrouter_server_tool_parameters_do_not_match", "OpenRouter server tool parameters do not match the policy's schema"};
inline constexpr Reason a_generic_hosted_tool_s_options_do_not_match{"request.responses.a_generic_hosted_tool_s_options_do_not_match", "a generic hosted tool's options do not match the policy's schema"};
inline constexpr Reason openrouter_server_tools_require_a_declared{"request.responses.openrouter_server_tools_require_a_declared", "OpenRouter server tools require a declared OpenRouter origin"};
inline constexpr Reason openrouter_server_tools_require_an_openrouter{"request.responses.openrouter_server_tools_require_an_openrouter", "OpenRouter server tools require an openrouter: type and object parameters"};
inline constexpr Reason mcp_tools_require_unique_server_labels{"request.responses.mcp_tools_require_unique_server_labels", "MCP tools require unique server labels"};
inline constexpr Reason image_generation_size_must_be_auto_or{"request.responses.image_generation_size_must_be_auto_or", "image generation size must be auto or WIDTHxHEIGHT"};
inline constexpr Reason image_generation_options_must_be_lower_case{"request.responses.image_generation_options_must_be_lower_case", "image generation options must be lower-case tokens"};
inline constexpr Reason image_generation_model_or_compression_is{"request.responses.image_generation_model_or_compression_is", "image generation model or compression is invalid"};
inline constexpr Reason image_generation_mask_requires_a_file_id_or_an{"request.responses.image_generation_mask_requires_a_file_id_or_an", "image generation mask requires a file id or an image URL"};
inline constexpr Reason file_search_result_count_must_be_positive{"request.responses.file_search_result_count_must_be_positive", "file search result count must be positive"};
inline constexpr Reason web_search_domains_or_context_size_are_invalid{"request.responses.web_search_domains_or_context_size_are_invalid", "web search domains or context size are invalid"};
inline constexpr Reason web_search_location_fields_must_be_short_text{"request.responses.web_search_location_fields_must_be_short_text", "web search location fields must be short text"};
inline constexpr Reason a_code_interpreter_container_id_excludes{"request.responses.a_code_interpreter_container_id_excludes", "a code interpreter container id excludes automatic container settings"};
inline constexpr Reason code_interpreter_files_or_memory_limit_are{"request.responses.code_interpreter_files_or_memory_limit_are", "code interpreter files or memory limit are invalid"};
inline constexpr Reason a_network_policy_is_disabled_or_lists_unique{"request.responses.a_network_policy_is_disabled_or_lists_unique", "a network policy is disabled or lists unique allowed domains"};
inline constexpr Reason domain_secrets_require_an_allowed_domain_a{"request.responses.domain_secrets_require_an_allowed_domain_a", "domain secrets require an allowed domain, a unique name and a single-line value"};
inline constexpr Reason mcp_tools_require_one_https_server_url{"request.responses.mcp_tools_require_one_https_server_url", "MCP tools require one https server URL, connector or tunnel"};
inline constexpr Reason mcp_authorization_and_headers_require_single{"request.responses.mcp_authorization_and_headers_require_single", "MCP authorization and headers require single-line values"};
inline constexpr Reason mcp_approval_is_a_mode_or_filters_not_both{"request.responses.mcp_approval_is_a_mode_or_filters_not_both", "MCP approval is a mode or filters, not both"};
inline constexpr Reason mcp_approval_mode_must_be_a_lower_case_token{"request.responses.mcp_approval_mode_must_be_a_lower_case_token", "MCP approval mode must be a lower-case token"};
inline constexpr Reason mcp_tool_filters_require_unique_names_or_read{"request.responses.mcp_tool_filters_require_unique_names_or_read", "MCP tool filters require unique names or read_only"};
inline constexpr Reason mcp_server_description_must_be_text{"request.responses.mcp_server_description_must_be_text", "MCP server description must be text"};
inline constexpr Reason computer_use_requires_an_environment_and_a{"request.responses.computer_use_requires_an_environment_and_a", "computer use requires an environment and a display size"};
inline constexpr Reason output_modalities_and_image_config_require_a{"request.responses.output_modalities_and_image_config_require_a", "output modalities and image_config require a declared OpenRouter origin"};
inline constexpr Reason output_modalities_require_unique_valid_values{"request.responses.output_modalities_require_unique_valid_values", "output modalities require unique valid values"};
inline constexpr Reason image_config_needs_the_image_modality{"request.responses.image_config_needs_the_image_modality", "image_config needs the image modality"};
inline constexpr Reason mcp_approvals_require_unique_request_ids_and{"request.responses.mcp_approvals_require_unique_request_ids_and", "MCP approvals require unique request ids and text reasons"};
inline constexpr Reason captured_computer_call_requires_a_declared{"request.responses.captured_computer_call_requires_a_declared", "captured computer call requires a declared computer tool"};
inline constexpr Reason a_computer_call_result_is_one_screenshot_image{"request.responses.a_computer_call_result_is_one_screenshot_image", "a computer call result is one screenshot image"};
inline constexpr Reason a_computer_call_result_s_content_may_only{"request.responses.a_computer_call_result_s_content_may_only", "a computer call result's content may only acknowledge safety checks"};
inline constexpr Reason an_mcp_approval_gives_a_reason_only_when_it{"request.responses.an_mcp_approval_gives_a_reason_only_when_it", "an MCP approval gives a reason only when it refuses"};
inline constexpr Reason openrouter_plugins_require_a_declared{"request.responses.openrouter_plugins_require_a_declared", "OpenRouter plugins require a declared OpenRouter origin"};
inline constexpr Reason an_mcp_approval_answers_a_captured_approval{"request.responses.an_mcp_approval_answers_a_captured_approval", "an MCP approval answers a captured approval request"};
inline constexpr Reason model_and_messages_are_required{"request.responses.model_and_messages_are_required", "model and messages are required"};
inline constexpr Reason request_count_limit_exceeded{"request.responses.request_count_limit_exceeded", "request count limit exceeded"};
inline constexpr Reason invalid_previous_response_cursor{"request.responses.invalid_previous_response_cursor", "invalid previous response cursor"};
inline constexpr Reason previous_response_history_requires_a_cursor{"request.responses.previous_response_history_requires_a_cursor", "previous response history requires a cursor"};
inline constexpr Reason previous_response_history_count_limit{"request.responses.previous_response_history_count_limit", "previous response history count limit exceeded"};
inline constexpr Reason previous_response_part_count_limit_exceeded{"request.responses.previous_response_part_count_limit_exceeded", "previous response part count limit exceeded"};
inline constexpr Reason invalid_verbosity{"request.responses.invalid_verbosity", "invalid verbosity"};
inline constexpr Reason invalid_truncation{"request.responses.invalid_truncation", "invalid truncation"};
inline constexpr Reason invalid_or_duplicate_include_selection{"request.responses.invalid_or_duplicate_include_selection", "invalid or duplicate include selection"};
inline constexpr Reason hosted_tool_types_must_be_unique{"request.responses.hosted_tool_types_must_be_unique", "hosted tool types must be unique"};
inline constexpr Reason file_search_requires_bounded_vector_store{"request.responses.file_search_requires_bounded_vector_store", "file search requires bounded vector store ids"};
inline constexpr Reason vector_store_ids_must_be_nonempty_and_unique{"request.responses.vector_store_ids_must_be_nonempty_and_unique", "vector store ids must be nonempty and unique"};
inline constexpr Reason shell_skill_count_limit_exceeded{"request.responses.shell_skill_count_limit_exceeded", "shell skill count limit exceeded"};
inline constexpr Reason shell_skill_references_must_be_nonempty_and{"request.responses.shell_skill_references_must_be_nonempty_and", "shell skill references must be nonempty and unique"};
inline constexpr Reason deferred_functions_require_tool_search{"request.responses.deferred_functions_require_tool_search", "deferred functions require tool_search"};
inline constexpr Reason tool_search_requires_a_deferred_function{"request.responses.tool_search_requires_a_deferred_function", "tool_search requires a deferred function definition"};
inline constexpr Reason request_part_count_limit_exceeded{"request.responses.request_part_count_limit_exceeded", "request part count limit exceeded"};
inline constexpr Reason tools_require_unique_valid_function_names{"request.responses.tools_require_unique_valid_function_names", "tools require unique valid function names"};
inline constexpr Reason function_parameters_must_be_an_object_schema{"request.responses.function_parameters_must_be_an_object_schema", "function parameters must be an object schema"};
inline constexpr Reason strict_function{"request.responses.strict_function", "strict function "};
inline constexpr Reason required_tool_must_name_a_declared_client{"request.responses.required_tool_must_name_a_declared_client", "required_tool must name a declared client function"};
inline constexpr Reason tool_choice_and_required_tool_exclusive{"request.responses.tool_choice_and_required_tool_exclusive", "tool choice and required_tool are mutually exclusive"};
inline constexpr Reason invalid_tool_choice_mode{"request.responses.invalid_tool_choice_mode", "invalid tool choice mode"};
inline constexpr Reason only_a_named_tool_choice_takes_a_name{"request.responses.only_a_named_tool_choice_takes_a_name", "only a named tool choice takes a name"};
inline constexpr Reason function_tool_choice_must_be_declared{"request.responses.function_tool_choice_must_be_declared", "a function tool choice must name a declared function"};
inline constexpr Reason hosted_tool_choice_must_be_declared{"request.responses.hosted_tool_choice_must_be_declared", "a hosted tool choice must name a declared hosted tool"};
inline constexpr Reason mcp_server_is_not_a_hosted_tool_choice{"request.responses.mcp_server_is_not_a_hosted_tool_choice", "an MCP server is chosen by its label, not as a hosted tool"};
inline constexpr Reason mcp_tool_choice_must_be_declared{"request.responses.mcp_tool_choice_must_be_declared", "an MCP tool choice must name a declared MCP server"};
inline constexpr Reason only_an_mcp_tool_choice_names_a_server_tool{"request.responses.only_an_mcp_tool_choice_names_a_server_tool", "only an MCP tool choice names one tool of its server"};
inline constexpr Reason mcp_tool_choice_tool_name_must_be_short_text{"request.responses.mcp_tool_choice_tool_name_must_be_short_text", "an MCP tool choice's tool name must be short text"};
inline constexpr Reason allowed_tools_require_auto_or_required{"request.responses.allowed_tools_require_auto_or_required", "allowed tools require the auto or required mode"};
inline constexpr Reason allowed_tools_require_at_least_one{"request.responses.allowed_tools_require_at_least_one", "allowed tools require at least one tool"};
inline constexpr Reason invalid_allowed_tool_kind{"request.responses.invalid_allowed_tool_kind", "invalid allowed tool kind"};
inline constexpr Reason allowed_tools_must_be_declared{"request.responses.allowed_tools_must_be_declared", "allowed tools must be declared tools"};
inline constexpr Reason allowed_tools_must_be_unique{"request.responses.allowed_tools_must_be_unique", "allowed tools must name each tool once"};
inline constexpr Reason client_tool_results_must_precede_the_next{"request.responses.client_tool_results_must_precede_the_next", "client tool results must precede the next captured assistant group"};
inline constexpr Reason captured_client_call_has_invalid_or{"request.responses.captured_client_call_has_invalid_or", "captured client call has invalid or duplicate ownership"};
inline constexpr Reason captured_output_item_must_have_a_type{"request.responses.captured_output_item_must_have_a_type", "captured output item must have a type"};
inline constexpr Reason captured_output_item_ids_must_be_unique_and{"request.responses.captured_output_item_ids_must_be_unique_and", "captured output item ids must be unique and nonempty"};
inline constexpr Reason captured_function_item_lacks_its_original{"request.responses.captured_function_item_lacks_its_original", "captured function item lacks its original identity"};
inline constexpr Reason captured_function_item_lacks_unique_client{"request.responses.captured_function_item_lacks_unique_client", "captured function item lacks unique client ownership"};
inline constexpr Reason captured_function_call_is_missing_its_output{"request.responses.captured_function_call_is_missing_its_output", "captured function call is missing its output item"};
inline constexpr Reason plain_input_messages_require_content{"request.responses.plain_input_messages_require_content", "plain input messages require content"};
inline constexpr Reason tool_results_require_tool_or_user_role{"request.responses.tool_results_require_tool_or_user_role", "tool results require tool or user role"};
inline constexpr Reason tool_result_lacks_exactly_one_pending_client{"request.responses.tool_result_lacks_exactly_one_pending_client", "tool result lacks exactly one pending client call_id"};
inline constexpr Reason plain_input_role_or_pending_client_tool{"request.responses.plain_input_role_or_pending_client_tool", "plain input role or pending client tool results are invalid"};
inline constexpr Reason all_client_function_calls_require_results{"request.responses.all_client_function_calls_require_results", "all client function calls require results before dispatch"};
inline constexpr Reason responses_input_audio_has_no_format_name_for{"request.responses.responses_input_audio_has_no_format_name_for", "Responses input_audio has no format name for this MIME type"};
inline constexpr Reason native_replay_provenance_binding_or_content{"request.responses.native_replay_provenance_binding_or_content", "native replay provenance, binding or content mismatch"};
inline constexpr Reason responses_encoder_requires_responses{"request.responses.responses_encoder_requires_responses", "Responses encoder requires Responses descriptor"};
inline constexpr Reason hosted_responses_tool_is_not_admitted_by{"request.responses.hosted_responses_tool_is_not_admitted_by", "hosted Responses tool is not admitted by policy"};
inline constexpr Reason media_inputs_require_user_role{"request.responses.media_inputs_require_user_role", "media inputs require user role"};
inline constexpr Reason tool_result_content_and_content_parts_are{"request.responses.tool_result_content_and_content_parts_are", "tool result content and content_parts are mutually exclusive"};
inline constexpr Reason tool_result_part_count_limit_exceeded{"request.responses.tool_result_part_count_limit_exceeded", "tool result part count limit exceeded"};
inline constexpr Reason tool_result_blocks_not_admitted{"request.responses.tool_result_blocks_not_admitted", "tool result blocks are not admitted for this family"};
inline constexpr Reason responses_function_output_supports_images{"request.responses.responses_function_output_supports_images", "Responses function output supports images and files only"};
inline constexpr Reason function_output_media_contains_unsupported{"request.responses.function_output_media_contains_unsupported", "function output media contains unsupported controls or metadata"};
inline constexpr Reason native_binding_could_not_be_captured{"request.responses.native_binding_could_not_be_captured", "native binding could not be captured"};
inline constexpr Reason only_client_function_calls_authorize_tool{"request.responses.only_client_function_calls_authorize_tool", "only client function calls authorize tool results"};
inline constexpr Reason foreign_or_invalid_part_cannot_be_replayed{"request.responses.foreign_or_invalid_part_cannot_be_replayed", "foreign or invalid part cannot be replayed as Responses output"};
inline constexpr Reason responses_tool_results_have_no_typed_is{"request.responses.responses_tool_results_have_no_typed_is", "Responses tool results have no typed is_error field"};
inline constexpr Reason request_exceeds_json_byte_depth_or_encoding{"request.responses.request_exceeds_json_byte_depth_or_encoding", "request exceeds JSON byte, depth or encoding limits"};
inline constexpr Reason plain_responses_input_supports_typed_text{"request.responses.plain_responses_input_supports_typed_text", "plain Responses input supports typed text and media only"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    an_openrouter_server_tool_is_not_admitted_by, openrouter_server_tool_parameters_do_not_match,
    a_generic_hosted_tool_s_options_do_not_match, openrouter_server_tools_require_a_declared,
    openrouter_server_tools_require_an_openrouter, mcp_tools_require_unique_server_labels,
    image_generation_size_must_be_auto_or, image_generation_options_must_be_lower_case,
    image_generation_model_or_compression_is, image_generation_mask_requires_a_file_id_or_an,
    file_search_result_count_must_be_positive, web_search_domains_or_context_size_are_invalid,
    web_search_location_fields_must_be_short_text, a_code_interpreter_container_id_excludes,
    code_interpreter_files_or_memory_limit_are, a_network_policy_is_disabled_or_lists_unique,
    domain_secrets_require_an_allowed_domain_a, mcp_tools_require_one_https_server_url,
    mcp_authorization_and_headers_require_single, mcp_approval_is_a_mode_or_filters_not_both,
    mcp_approval_mode_must_be_a_lower_case_token, mcp_tool_filters_require_unique_names_or_read,
    mcp_server_description_must_be_text, computer_use_requires_an_environment_and_a,
    output_modalities_and_image_config_require_a, output_modalities_require_unique_valid_values,
    image_config_needs_the_image_modality, mcp_approvals_require_unique_request_ids_and,
    captured_computer_call_requires_a_declared, a_computer_call_result_is_one_screenshot_image,
    a_computer_call_result_s_content_may_only, an_mcp_approval_gives_a_reason_only_when_it,
    openrouter_plugins_require_a_declared, an_mcp_approval_answers_a_captured_approval,
    model_and_messages_are_required, request_count_limit_exceeded, invalid_previous_response_cursor,
    previous_response_history_requires_a_cursor, previous_response_history_count_limit,
    previous_response_part_count_limit_exceeded, invalid_verbosity, invalid_truncation,
    invalid_or_duplicate_include_selection, hosted_tool_types_must_be_unique,
    file_search_requires_bounded_vector_store, vector_store_ids_must_be_nonempty_and_unique,
    shell_skill_count_limit_exceeded, shell_skill_references_must_be_nonempty_and,
    deferred_functions_require_tool_search, tool_search_requires_a_deferred_function,
    request_part_count_limit_exceeded, tools_require_unique_valid_function_names,
    function_parameters_must_be_an_object_schema, strict_function, required_tool_must_name_a_declared_client,
    tool_choice_and_required_tool_exclusive, invalid_tool_choice_mode, only_a_named_tool_choice_takes_a_name,
    function_tool_choice_must_be_declared, hosted_tool_choice_must_be_declared,
    mcp_server_is_not_a_hosted_tool_choice, mcp_tool_choice_must_be_declared,
    only_an_mcp_tool_choice_names_a_server_tool, mcp_tool_choice_tool_name_must_be_short_text,
    allowed_tools_require_auto_or_required, allowed_tools_require_at_least_one, invalid_allowed_tool_kind,
    allowed_tools_must_be_declared, allowed_tools_must_be_unique, client_tool_results_must_precede_the_next,
    captured_client_call_has_invalid_or, captured_output_item_must_have_a_type,
    captured_output_item_ids_must_be_unique_and, captured_function_item_lacks_its_original,
    captured_function_item_lacks_unique_client, captured_function_call_is_missing_its_output,
    plain_input_messages_require_content, tool_results_require_tool_or_user_role,
    tool_result_lacks_exactly_one_pending_client, plain_input_role_or_pending_client_tool,
    all_client_function_calls_require_results, responses_input_audio_has_no_format_name_for,
    native_replay_provenance_binding_or_content, responses_encoder_requires_responses,
    hosted_responses_tool_is_not_admitted_by, media_inputs_require_user_role,
    tool_result_content_and_content_parts_are, tool_result_part_count_limit_exceeded, tool_result_blocks_not_admitted,
    responses_function_output_supports_images, function_output_media_contains_unsupported,
    native_binding_could_not_be_captured, only_client_function_calls_authorize_tool,
    foreign_or_invalid_part_cannot_be_replayed, responses_tool_results_have_no_typed_is,
    request_exceeds_json_byte_depth_or_encoding, plain_responses_input_supports_typed_text,
};
}  // namespace responses_request
namespace gemini_request {
inline constexpr Reason literal_model_name_required{"request.generate.literal_model_name_required", "literal model name required"};
inline constexpr Reason invalid_history_mode{"request.generate.invalid_history_mode", "invalid history mode"};
inline constexpr Reason invalid_thinking_level{"request.generate.invalid_thinking_level", "invalid thinking level"};
inline constexpr Reason thinking_level_and_budget_are_mutually{"request.generate.thinking_level_and_budget_are_mutually", "thinking level and budget are mutually exclusive"};
inline constexpr Reason safety_settings_require_valid_unique{"request.generate.safety_settings_require_valid_unique", "safety settings require valid unique categories and thresholds"};
inline constexpr Reason tool_choice_and_required_function_are{"request.generate.tool_choice_and_required_function_are", "tool choice and required function are mutually exclusive"};
inline constexpr Reason invalid_tool_choice_mode{"request.generate.invalid_tool_choice_mode", "invalid tool choice mode"};
inline constexpr Reason allowed_functions_require_any_or_validated{"request.generate.allowed_functions_require_any_or_validated", "allowed functions require ANY or VALIDATED mode"};
inline constexpr Reason invalid_response_modality{"request.generate.invalid_response_modality", "invalid response modality"};
inline constexpr Reason response_modalities_must_be_unique{"request.generate.response_modalities_must_be_unique", "response modalities must be unique"};
inline constexpr Reason single_and_multi_speaker_voice_configurations{"request.generate.single_and_multi_speaker_voice_configurations", "single and multi-speaker voice configurations are mutually exclusive"};
inline constexpr Reason voice_alternatives_are_mutually_exclusive{"request.generate.voice_alternatives_are_mutually_exclusive", "voice alternatives are mutually exclusive"};
inline constexpr Reason speaker_configuration_count_limit_exceeded{"request.generate.speaker_configuration_count_limit_exceeded", "speaker configuration count limit exceeded"};
inline constexpr Reason speaker_name_and_exclusive_voice{"request.generate.speaker_name_and_exclusive_voice", "speaker name and exclusive voice configuration required"};
inline constexpr Reason speech_language_code_must_be_a_language_tag{"request.generate.speech_language_code_must_be_a_language_tag", "speech language code must be a language tag"};
inline constexpr Reason image_aspect_ratio_must_be_w_h{"request.generate.image_aspect_ratio_must_be_w_h", "image aspect ratio must be W:H"};
inline constexpr Reason image_size_must_be_a_short_token{"request.generate.image_size_must_be_a_short_token", "image size must be a short token"};
inline constexpr Reason transcription_list_count_limit_exceeded{"request.generate.transcription_list_count_limit_exceeded", "transcription list count limit exceeded"};
inline constexpr Reason invalid_transcription_mode{"request.generate.invalid_transcription_mode", "invalid transcription mode"};
inline constexpr Reason timestamps_and_diarization_cannot_combine{"request.generate.timestamps_and_diarization_cannot_combine", "timestamps and diarization cannot combine with SMART or custom vocabulary"};
inline constexpr Reason audio_format_must_be_an_upper_case_token{"request.generate.audio_format_must_be_an_upper_case_token", "audio format must be an upper-case token"};
inline constexpr Reason built_in_tools_must_be_unique{"request.generate.built_in_tools_must_be_unique", "built-in tools must be unique"};
inline constexpr Reason audio_sample_or_bit_rate_is_out_of_range{"request.generate.audio_sample_or_bit_rate_is_out_of_range", "audio sample or bit rate is out of range"};
inline constexpr Reason search_time_range_requires_both_rfc_3339_ends{"request.generate.search_time_range_requires_both_rfc_3339_ends", "search time range requires both RFC 3339 ends"};
inline constexpr Reason computer_use_environment_and_policies_must_be{"request.generate.computer_use_environment_and_policies_must_be", "computer use environment and policies must be upper-case tokens"};
inline constexpr Reason excluded_computer_use_functions_require_unique{"request.generate.excluded_computer_use_functions_require_unique", "excluded computer use functions require unique names"};
inline constexpr Reason file_search_requires_unique_store_names{"request.generate.file_search_requires_unique_store_names", "file search requires unique store names"};
inline constexpr Reason file_search_filter_or_count_is_out_of_range{"request.generate.file_search_filter_or_count_is_out_of_range", "file search filter or count is out of range"};
inline constexpr Reason mcp_servers_require_unique_names_and_https_urls{"request.generate.mcp_servers_require_unique_names_and_https_urls", "MCP servers require unique names and https URLs"};
inline constexpr Reason mcp_server_headers_require_unique_names_and{"request.generate.mcp_server_headers_require_unique_names_and", "MCP server headers require unique names and single-line values"};
inline constexpr Reason mcp_server_timeouts_must_be_durations_in{"request.generate.mcp_server_timeouts_must_be_durations_in", "MCP server timeouts must be durations in seconds"};
inline constexpr Reason server_tool_invocations_require_a_service_run{"request.generate.server_tool_invocations_require_a_service_run", "server tool invocations require a service-run tool"};
inline constexpr Reason retrieval_location_or_language_is_invalid{"request.generate.retrieval_location_or_language_is_invalid", "retrieval location or language is invalid"};
inline constexpr Reason tool_result_content_must_be_a_json_object{"request.generate.tool_result_content_must_be_a_json_object", "tool result content must be a JSON object beside media parts"};
inline constexpr Reason request_count_limit_exceeded{"request.generate.request_count_limit_exceeded", "request count limit exceeded"};
inline constexpr Reason request_part_count_limit_exceeded{"request.generate.request_part_count_limit_exceeded", "request part count limit exceeded"};
inline constexpr Reason media_inputs_require_user_role{"request.generate.media_inputs_require_user_role", "media inputs require user role"};
inline constexpr Reason client_functions_require_unique_names_and{"request.generate.client_functions_require_unique_names_and", "client functions require unique names and object schemas"};
inline constexpr Reason required_function_must_be_declared{"request.generate.required_function_must_be_declared", "required function must be declared"};
inline constexpr Reason function_choice_requires_declared_functions{"request.generate.function_choice_requires_declared_functions", "function choice requires declared functions"};
inline constexpr Reason allowed_functions_require_unique_declared{"request.generate.allowed_functions_require_unique_declared", "allowed functions require unique declared names"};
inline constexpr Reason empty_content_message{"request.generate.empty_content_message", "empty content message"};
inline constexpr Reason all_function_results_must_precede_the_next{"request.generate.all_function_results_must_precede_the_next", "all function results must precede the next model message"};
inline constexpr Reason function_ownership_mismatch{"request.generate.function_ownership_mismatch", "function ownership mismatch"};
inline constexpr Reason portable_function_requires_unique_identity{"request.generate.portable_function_requires_unique_identity", "portable function requires unique identity, declared name and object arguments"};
inline constexpr Reason tool_result_lacks_pending_function{"request.generate.tool_result_lacks_pending_function", "tool result lacks pending function"};
inline constexpr Reason function_results_must_be_immediate_and{"request.generate.function_results_must_be_immediate_and", "function results must be immediate and complete"};
inline constexpr Reason media_cannot_appear_among_function_results{"request.generate.media_cannot_appear_among_function_results", "media cannot appear among function results"};
inline constexpr Reason function_result_lacks_unique_client_ownership{"request.generate.function_result_lacks_unique_client_ownership", "function result lacks unique client ownership"};
inline constexpr Reason function_result_must_be_json_object{"request.generate.function_result_must_be_json_object", "function result must be JSON object"};
inline constexpr Reason all_function_results_must_appear_together{"request.generate.all_function_results_must_appear_together", "all function results must appear together"};
inline constexpr Reason function_results_required_before_dispatch{"request.generate.function_results_required_before_dispatch", "function results required before dispatch"};
inline constexpr Reason captured_part_refused_by_the_service{"request.generate.captured_part_refused_by_the_service", "captured part is one the service refuses to take back"};
inline constexpr Reason native_replay_provenance_binding_or_content{"request.generate.native_replay_provenance_binding_or_content", "native replay provenance, binding or content mismatch"};
inline constexpr Reason gemini_encoder_requires_generate_descriptor{"request.generate.gemini_encoder_requires_generate_descriptor", "Gemini encoder requires generate descriptor"};
inline constexpr Reason gemini_descriptor_paths_must_target_the_same{"request.generate.gemini_descriptor_paths_must_target_the_same", "Gemini descriptor paths must target the same literal model"};
inline constexpr Reason tool_result_content_and_content_parts_are{"request.generate.tool_result_content_and_content_parts_are", "tool result content and content_parts are mutually exclusive"};
inline constexpr Reason tool_result_part_count_limit_exceeded{"request.generate.tool_result_part_count_limit_exceeded", "tool result part count limit exceeded"};
inline constexpr Reason tool_result_blocks_not_admitted{"request.generate.tool_result_blocks_not_admitted", "tool result blocks are not admitted for this family"};
inline constexpr Reason function_response_media_requires_inline_bytes{"request.generate.function_response_media_requires_inline_bytes", "function response media requires inline bytes"};
inline constexpr Reason function_response_media_has_unsupported{"request.generate.function_response_media_has_unsupported", "function response media has unsupported controls or metadata"};
inline constexpr Reason native_binding_could_not_be_captured{"request.generate.native_binding_could_not_be_captured", "native binding could not be captured"};
inline constexpr Reason native_parts_array_required{"request.generate.native_parts_array_required", "native parts array required"};
inline constexpr Reason portable_assistant_parts_must_be_text_or{"request.generate.portable_assistant_parts_must_be_text_or", "portable assistant parts must be text or client functions"};
inline constexpr Reason request_exceeds_json_limits_or_encoding{"request.generate.request_exceeds_json_limits_or_encoding", "request exceeds JSON limits or encoding"};
inline constexpr Reason unsupported_imported_gemini_part{"request.generate.unsupported_imported_gemini_part", "unsupported imported Gemini part"};
inline constexpr Reason gemini_supports_user_tool_and_captured_model{"request.generate.gemini_supports_user_tool_and_captured_model", "Gemini supports user, tool and captured model messages"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    literal_model_name_required, invalid_history_mode, invalid_thinking_level, thinking_level_and_budget_are_mutually,
    safety_settings_require_valid_unique, tool_choice_and_required_function_are, invalid_tool_choice_mode,
    allowed_functions_require_any_or_validated, invalid_response_modality, response_modalities_must_be_unique,
    single_and_multi_speaker_voice_configurations, voice_alternatives_are_mutually_exclusive,
    speaker_configuration_count_limit_exceeded, speaker_name_and_exclusive_voice,
    speech_language_code_must_be_a_language_tag, image_aspect_ratio_must_be_w_h, image_size_must_be_a_short_token,
    transcription_list_count_limit_exceeded, invalid_transcription_mode, timestamps_and_diarization_cannot_combine,
    audio_format_must_be_an_upper_case_token, built_in_tools_must_be_unique, audio_sample_or_bit_rate_is_out_of_range,
    search_time_range_requires_both_rfc_3339_ends, computer_use_environment_and_policies_must_be,
    excluded_computer_use_functions_require_unique, file_search_requires_unique_store_names,
    file_search_filter_or_count_is_out_of_range, mcp_servers_require_unique_names_and_https_urls,
    mcp_server_headers_require_unique_names_and, mcp_server_timeouts_must_be_durations_in,
    server_tool_invocations_require_a_service_run, retrieval_location_or_language_is_invalid,
    tool_result_content_must_be_a_json_object, request_count_limit_exceeded, request_part_count_limit_exceeded,
    media_inputs_require_user_role, client_functions_require_unique_names_and, required_function_must_be_declared,
    function_choice_requires_declared_functions, allowed_functions_require_unique_declared, empty_content_message,
    all_function_results_must_precede_the_next, function_ownership_mismatch,
    portable_function_requires_unique_identity, tool_result_lacks_pending_function,
    function_results_must_be_immediate_and, media_cannot_appear_among_function_results,
    function_result_lacks_unique_client_ownership, function_result_must_be_json_object,
    all_function_results_must_appear_together, function_results_required_before_dispatch,
    captured_part_refused_by_the_service, native_replay_provenance_binding_or_content,
    gemini_encoder_requires_generate_descriptor, gemini_descriptor_paths_must_target_the_same,
    tool_result_content_and_content_parts_are, tool_result_part_count_limit_exceeded, tool_result_blocks_not_admitted,
    function_response_media_requires_inline_bytes, function_response_media_has_unsupported,
    native_binding_could_not_be_captured, native_parts_array_required, portable_assistant_parts_must_be_text_or,
    request_exceeds_json_limits_or_encoding, unsupported_imported_gemini_part,
    gemini_supports_user_tool_and_captured_model,
};
}  // namespace gemini_request
namespace interactions_request {
inline constexpr Reason an_interaction_lifecycle_request_carries_no{"request.interactions.an_interaction_lifecycle_request_carries_no", "an interaction lifecycle request carries no extensions"};
inline constexpr Reason model_and_bounded_messages_tools_required{"request.interactions.model_and_bounded_messages_tools_required", "model and bounded messages/tools required"};
inline constexpr Reason response_format_count_limit_exceeded{"request.interactions.response_format_count_limit_exceeded", "response format count limit exceeded"};
inline constexpr Reason speech_configuration_count_limit_exceeded{"request.interactions.speech_configuration_count_limit_exceeded", "speech configuration count limit exceeded"};
inline constexpr Reason transcription_list_count_limit_exceeded{"request.interactions.transcription_list_count_limit_exceeded", "transcription list count limit exceeded"};
inline constexpr Reason invalid_transcription_mode{"request.interactions.invalid_transcription_mode", "invalid transcription mode"};
inline constexpr Reason timestamps_and_diarization_cannot_combine{"request.interactions.timestamps_and_diarization_cannot_combine", "timestamps and diarization cannot combine with smart mode or custom vocabulary"};
inline constexpr Reason request_part_count_limit_exceeded{"request.interactions.request_part_count_limit_exceeded", "request part count limit exceeded"};
inline constexpr Reason media_inputs_require_user_role{"request.interactions.media_inputs_require_user_role", "media inputs require user role"};
inline constexpr Reason unique_valid_functions_and_object{"request.interactions.unique_valid_functions_and_object", "unique valid functions and object parameters required"};
inline constexpr Reason required_tool_must_name_a_declared{"request.interactions.required_tool_must_name_a_declared", "required tool must name a declared function"};
inline constexpr Reason tool_choice_and_required_tool_exclusive{"request.interactions.tool_choice_and_required_tool_exclusive", "tool choice and required tool are mutually exclusive"};
inline constexpr Reason invalid_tool_choice_mode{"request.interactions.invalid_tool_choice_mode", "invalid tool choice mode"};
inline constexpr Reason allowed_tools_require_at_least_one{"request.interactions.allowed_tools_require_at_least_one", "allowed tools require at least one function"};
inline constexpr Reason allowed_tools_must_be_declared{"request.interactions.allowed_tools_must_be_declared", "allowed tools must be declared functions"};
inline constexpr Reason allowed_tools_must_be_unique{"request.interactions.allowed_tools_must_be_unique", "allowed tools must name each function once"};
inline constexpr Reason client_results_must_precede_next{"request.interactions.client_results_must_precede_next", "client results must precede next assistant generation"};
inline constexpr Reason captured_function_lacks_unique_declared{"request.interactions.captured_function_lacks_unique_declared", "captured function lacks unique declared ownership"};
inline constexpr Reason captured_step_requires_type{"request.interactions.captured_step_requires_type", "captured step requires type"};
inline constexpr Reason captured_function_identity_invalid{"request.interactions.captured_function_identity_invalid", "captured function identity invalid"};
inline constexpr Reason captured_function_ownership_mismatch{"request.interactions.captured_function_ownership_mismatch", "captured function ownership mismatch"};
inline constexpr Reason captured_function_step_missing{"request.interactions.captured_function_step_missing", "captured function step missing"};
inline constexpr Reason input_content_required{"request.interactions.input_content_required", "input content required"};
inline constexpr Reason tool_results_require_user_or_tool_role{"request.interactions.tool_results_require_user_or_tool_role", "tool results require user or tool role"};
inline constexpr Reason tool_results_cannot_mix_with_plain{"request.interactions.tool_results_cannot_mix_with_plain", "tool results cannot mix with plain content"};
inline constexpr Reason tool_result_requires_exactly_one_pending{"request.interactions.tool_result_requires_exactly_one_pending", "tool result requires exactly one pending client call"};
inline constexpr Reason plain_input_requires_user_role_and{"request.interactions.plain_input_requires_user_role_and", "plain input requires user role and resolved calls"};
inline constexpr Reason all_pending_functions_require_host{"request.interactions.all_pending_functions_require_host", "all pending functions require host results"};
inline constexpr Reason native_replay_provenance_binding_or{"request.interactions.native_replay_provenance_binding_or", "native replay provenance, binding or content mismatch"};
inline constexpr Reason interactions_requires_the_model{"request.interactions.interactions_requires_the_model", "Interactions requires the model interaction endpoint"};
inline constexpr Reason tool_result_content_and_content_parts_are{"request.interactions.tool_result_content_and_content_parts_are", "tool result content and content_parts are mutually exclusive"};
inline constexpr Reason tool_result_part_count_limit_exceeded{"request.interactions.tool_result_part_count_limit_exceeded", "tool result part count limit exceeded"};
inline constexpr Reason tool_result_blocks_not_admitted{"request.interactions.tool_result_blocks_not_admitted", "tool result blocks are not admitted for this family"};
inline constexpr Reason interactions_function_result_supports{"request.interactions.interactions_function_result_supports", "Interactions function result supports image and text subcontent only"};
inline constexpr Reason interactions_function_result_image_has{"request.interactions.interactions_function_result_image_has", "Interactions function result image has unsupported controls or metadata"};
inline constexpr Reason interactions_only_supports_a_media_name{"request.interactions.interactions_only_supports_a_media_name", "Interactions only supports a media name for video content"};
inline constexpr Reason native_binding_could_not_be_captured{"request.interactions.native_binding_could_not_be_captured", "native binding could not be captured"};
inline constexpr Reason unsupported_captured_interaction_part{"request.interactions.unsupported_captured_interaction_part", "unsupported captured interaction part"};
inline constexpr Reason unsupported_captured_interaction_step{"request.interactions.unsupported_captured_interaction_step", "unsupported captured interaction step"};
inline constexpr Reason request_exceeds_json_limits{"request.interactions.request_exceeds_json_limits", "request exceeds JSON limits"};
inline constexpr Reason unsupported_input_content{"request.interactions.unsupported_input_content", "unsupported input content"};
inline constexpr Reason invalid_delivery_mode{"request.interactions.invalid_delivery_mode", "invalid delivery mode"};
inline constexpr Reason audio_output_mime_type_must_be_an_audio{"request.interactions.audio_output_mime_type_must_be_an_audio", "audio output MIME type must be an audio MIME type"};
inline constexpr Reason image_output_mime_type_must_be_an_image{"request.interactions.image_output_mime_type_must_be_an_image", "image output MIME type must be an image MIME type"};
inline constexpr Reason image_aspect_ratio_must_be_w_h{"request.interactions.image_aspect_ratio_must_be_w_h", "image aspect ratio must be W:H"};
inline constexpr Reason image_size_must_be_a_short_token{"request.interactions.image_size_must_be_a_short_token", "image size must be a short token"};
inline constexpr Reason video_aspect_ratio_must_be_w_h{"request.interactions.video_aspect_ratio_must_be_w_h", "video aspect ratio must be W:H"};
inline constexpr Reason video_resolution_must_be_a_short_token{"request.interactions.video_resolution_must_be_a_short_token", "video resolution must be a short token"};
inline constexpr Reason video_duration_requires_protobuf_duration{"request.interactions.video_duration_requires_protobuf_duration", "video duration requires protobuf Duration syntax"};
inline constexpr Reason text_output_mime_type_must_be_a_mime_type{"request.interactions.text_output_mime_type_must_be_a_mime_type", "text output MIME type must be a MIME type"};
inline constexpr Reason text_schema_requires_application_json_and{"request.interactions.text_schema_requires_application_json_and", "text schema requires application/json and an object schema"};
inline constexpr Reason video_task_must_be_a_lower_case_token{"request.interactions.video_task_must_be_a_lower_case_token", "video task must be a lower-case token"};
inline constexpr Reason google_search_search_types_must_be_bounded{"request.interactions.google_search_search_types_must_be_bounded", "google_search search types must be bounded unique tokens"};
inline constexpr Reason invalid_previous_interaction_id{"request.interactions.invalid_previous_interaction_id", "invalid previous interaction id"};
inline constexpr Reason previous_interaction_input_not_new{"request.interactions.previous_interaction_input_not_new", "previous interaction input must be new input only"};
inline constexpr Reason model_or_agent_required{"request.interactions.model_or_agent_required", "exactly one of model or agent required"};
inline constexpr Reason invalid_agent{"request.interactions.invalid_agent", "invalid interaction agent"};
inline constexpr Reason invalid_interaction_id{"request.interactions.invalid_interaction_id", "invalid interaction id"};
inline constexpr Reason lifecycle_request_invalid{"request.interactions.lifecycle_request_invalid", "an interaction lifecycle request sends no input and is not streamed"};
inline constexpr Reason background_needs_store{"request.interactions.background_needs_store", "a background interaction needs store"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    an_interaction_lifecycle_request_carries_no, model_and_bounded_messages_tools_required,
    response_format_count_limit_exceeded, speech_configuration_count_limit_exceeded,
    transcription_list_count_limit_exceeded, invalid_transcription_mode, timestamps_and_diarization_cannot_combine,
    request_part_count_limit_exceeded, media_inputs_require_user_role, unique_valid_functions_and_object,
    required_tool_must_name_a_declared, tool_choice_and_required_tool_exclusive, invalid_tool_choice_mode,
    allowed_tools_require_at_least_one, allowed_tools_must_be_declared, allowed_tools_must_be_unique,
    client_results_must_precede_next, captured_function_lacks_unique_declared, captured_step_requires_type,
    captured_function_identity_invalid, captured_function_ownership_mismatch, captured_function_step_missing,
    input_content_required, tool_results_require_user_or_tool_role, tool_results_cannot_mix_with_plain,
    tool_result_requires_exactly_one_pending, plain_input_requires_user_role_and, all_pending_functions_require_host,
    native_replay_provenance_binding_or, interactions_requires_the_model, tool_result_content_and_content_parts_are,
    tool_result_part_count_limit_exceeded, tool_result_blocks_not_admitted, interactions_function_result_supports,
    interactions_function_result_image_has, interactions_only_supports_a_media_name,
    native_binding_could_not_be_captured, unsupported_captured_interaction_part,
    unsupported_captured_interaction_step, request_exceeds_json_limits, unsupported_input_content,
    invalid_delivery_mode, audio_output_mime_type_must_be_an_audio, image_output_mime_type_must_be_an_image,
    image_aspect_ratio_must_be_w_h, image_size_must_be_a_short_token, video_aspect_ratio_must_be_w_h,
    video_resolution_must_be_a_short_token, video_duration_requires_protobuf_duration,
    text_output_mime_type_must_be_a_mime_type, text_schema_requires_application_json_and,
    video_task_must_be_a_lower_case_token, google_search_search_types_must_be_bounded,
    invalid_previous_interaction_id, previous_interaction_input_not_new, model_or_agent_required, invalid_agent,
    invalid_interaction_id, lifecycle_request_invalid, background_needs_store,
};
}  // namespace interactions_request
namespace images {
inline constexpr Reason response_json_invalid{"images.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_not_object{"images.response.not_object", "response must be object"};
inline constexpr Reason response_remote_error{"images.response.remote_error", "remote error property"};
inline constexpr Reason data_required{"images.data.required", "image data required"};
inline constexpr Reason data_count_limit{"images.data.count_limit", "image count limit"};
inline constexpr Reason item_not_object{"images.item.not_object", "image item must be object"};
inline constexpr Reason item_no_data{"images.item.no_data", "image item has no data"};
inline constexpr Reason item_url_invalid{"images.item.url_invalid", "image URL is not an https location"};
inline constexpr Reason item_no_media_type{"images.item.no_media_type", "image item has no media type"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    response_json_invalid, response_not_object, response_remote_error, data_required, data_count_limit,
    item_not_object, item_no_data, item_url_invalid, item_no_media_type,
};
}  // namespace images
namespace images_request {
inline constexpr Reason endpoint_not_admitted{"request.images.endpoint_not_admitted", "image endpoints are not admitted at this origin"};
inline constexpr Reason operation_not_admitted{"request.images.operation_not_admitted", "the image endpoint has no path for this operation"};
inline constexpr Reason model_and_prompt_required{"request.images.model_and_prompt_required", "image model and prompt are required"};
inline constexpr Reason controls_invalid{"request.images.controls_invalid", "image size, quality, background and format are tokens"};
inline constexpr Reason count_invalid{"request.images.count_invalid", "image count must be positive"};
inline constexpr Reason compression_invalid{"request.images.compression_invalid", "image output compression is 0 to 100"};
inline constexpr Reason reference_not_image{"request.images.reference_not_image", "image references are images"};
inline constexpr Reason file_reference_unsupported{"request.images.file_reference_unsupported", "image references by file id are OpenAI only"};
inline constexpr Reason file_id_invalid{"request.images.file_id_invalid", "an image file id is a token"};
inline constexpr Reason options_not_admitted{"request.images.options_not_admitted", "the image endpoint admits no options"};
inline constexpr Reason options_invalid{"request.images.options_invalid", "image options do not match the policy's schema"};
inline constexpr Reason json_limit{"request.images.json_limit", "image request exceeds JSON limits"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, operation_not_admitted, model_and_prompt_required, controls_invalid, count_invalid,
    compression_invalid, reference_not_image, file_reference_unsupported, file_id_invalid, options_not_admitted,
    options_invalid, json_limit,
};
}  // namespace images_request
namespace decisions {
inline constexpr Reason response_json_invalid{"decisions.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_not_object{"decisions.response.not_object", "response must be object"};
inline constexpr Reason response_remote_error{"decisions.response.remote_error", "remote error property"};
inline constexpr Reason answers_required{"decisions.answers.required", "decision answers required"};
inline constexpr Reason answers_count_limit{"decisions.answers.count_limit", "decision answer count limit"};
inline constexpr Reason model_not_string{"decisions.model.not_string", "decision model must be string"};
inline constexpr Reason answer_type_required{"decisions.answer.type_required", "decision answer needs a type"};
inline constexpr Reason answer_number_invalid{"decisions.answer.number_invalid", "decision probabilities and scores are numbers"};
inline constexpr Reason answer_limit{"decisions.answer.limit", "decision answer limit"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    response_json_invalid, response_not_object, response_remote_error, answers_required, answers_count_limit,
    model_not_string, answer_type_required, answer_number_invalid, answer_limit,
};
}  // namespace decisions
namespace decisions_request {
inline constexpr Reason endpoint_not_admitted{"request.decisions.endpoint_not_admitted", "decision endpoints are not admitted at this origin"};
inline constexpr Reason path_missing{"request.decisions.path_missing", "the decision endpoint has no path"};
inline constexpr Reason model_and_questions_required{"request.decisions.model_and_questions_required", "a decision needs a model and questions"};
inline constexpr Reason input_invalid{"request.decisions.input_invalid", "decision input is a string, an array or (OpenRouter) an object"};
inline constexpr Reason names_invalid{"request.decisions.names_invalid", "decision questions need unique names"};
inline constexpr Reason guidance_invalid{"request.decisions.guidance_invalid", "decision instructions are a string or (OpenRouter) structured guidance"};
inline constexpr Reason box_unsupported{"request.decisions.box_unsupported", "box questions are OpenRouter only"};
inline constexpr Reason options_count_invalid{"request.decisions.options_count_invalid", "choice and score questions list at least two options; others list none"};
inline constexpr Reason option_invalid{"request.decisions.option_invalid", "decision options have a value and string or (OpenRouter) structured guidance"};
inline constexpr Reason option_values_duplicate{"request.decisions.option_values_duplicate", "decision options need unique values"};
inline constexpr Reason predicate_guidance_invalid{"request.decisions.predicate_guidance_invalid", "predicate guidance gives both a true and a false answer"};
inline constexpr Reason predicate_guidance_unsupported{"request.decisions.predicate_guidance_unsupported", "predicate guidance is OpenRouter only"};
inline constexpr Reason options_not_admitted{"request.decisions.options_not_admitted", "the decision endpoint admits no options"};
inline constexpr Reason options_schema_invalid{"request.decisions.options_schema_invalid", "decision options do not match the policy's schema"};
inline constexpr Reason json_limit{"request.decisions.json_limit", "decision request exceeds JSON limits"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, path_missing, model_and_questions_required, input_invalid, names_invalid, guidance_invalid,
    box_unsupported, options_count_invalid, option_invalid, option_values_duplicate, predicate_guidance_invalid,
    predicate_guidance_unsupported, options_not_admitted, options_schema_invalid, json_limit,
};
}  // namespace decisions_request
namespace embeddings {
inline constexpr Reason response_json_invalid{"embeddings.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_not_object{"embeddings.response.not_object", "response must be object"};
inline constexpr Reason response_remote_error{"embeddings.response.remote_error", "remote error property"};
inline constexpr Reason vectors_required{"embeddings.vectors.required", "embedding vectors required"};
inline constexpr Reason vector_invalid{"embeddings.vector.invalid", "an embedding is an array of numbers"};
inline constexpr Reason vector_index_invalid{"embeddings.vector.index_invalid", "an embedding names its input once"};
inline constexpr Reason vectors_count_mismatch{"embeddings.vectors.count_mismatch", "the response holds one embedding per input"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    response_json_invalid, response_not_object, response_remote_error, vectors_required, vector_invalid,
    vector_index_invalid, vectors_count_mismatch,
};
}  // namespace embeddings
namespace embeddings_request {
inline constexpr Reason model_path{"request.embeddings.model_path", "a model name cannot take the place of {model} in the endpoint's path"};
inline constexpr Reason endpoint_not_admitted{"request.embeddings.endpoint_not_admitted", "embedding endpoints are not admitted at this origin"};
inline constexpr Reason path_missing{"request.embeddings.path_missing", "the embedding endpoint has no path"};
inline constexpr Reason model_and_inputs_required{"request.embeddings.model_and_inputs_required", "embeddings need a model and inputs"};
inline constexpr Reason model_segment_invalid{"request.embeddings.model_segment_invalid", "a Google embedding model is a path segment"};
inline constexpr Reason dimensions_invalid{"request.embeddings.dimensions_invalid", "embedding dimensions must be positive"};
inline constexpr Reason task_unsupported{"request.embeddings.task_unsupported", "OpenAI embeddings take no task"};
inline constexpr Reason task_invalid{"request.embeddings.task_invalid", "an embedding task is a token"};
inline constexpr Reason input_empty{"request.embeddings.input_empty", "an embedding input has text or media"};
inline constexpr Reason media_unsupported{"request.embeddings.media_unsupported", "OpenAI embeddings take text only"};
inline constexpr Reason media_kind_unsupported{"request.embeddings.media_kind_unsupported", "OpenRouter embeddings take images as media"};
inline constexpr Reason media_source_unsupported{"request.embeddings.media_source_unsupported", "Google embeddings take inline or uploaded media"};
inline constexpr Reason media_reference_missing{"request.embeddings.media_reference_missing", "an embedding media file names its reference"};
inline constexpr Reason options_not_admitted{"request.embeddings.options_not_admitted", "the embedding endpoint admits no options"};
inline constexpr Reason options_invalid{"request.embeddings.options_invalid", "embedding options do not match the policy's schema"};
inline constexpr Reason json_limit{"request.embeddings.json_limit", "embedding request exceeds JSON limits"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    model_path, endpoint_not_admitted, path_missing, model_and_inputs_required, model_segment_invalid,
    dimensions_invalid, task_unsupported, task_invalid, input_empty, media_unsupported, media_kind_unsupported,
    media_source_unsupported, media_reference_missing, options_not_admitted, options_invalid, json_limit,
};
}  // namespace embeddings_request
namespace rerank {
inline constexpr Reason response_json_invalid{"rerank.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_not_object{"rerank.response.not_object", "response must be object"};
inline constexpr Reason response_remote_error{"rerank.response.remote_error", "remote error property"};
inline constexpr Reason results_required{"rerank.results.required", "rerank results required"};
inline constexpr Reason result_invalid{"rerank.result.invalid", "a rerank result has an index and a score"};
inline constexpr Reason result_index_invalid{"rerank.result.index_invalid", "a rerank result names a document of the request once"};
inline constexpr Reason results_count_mismatch{"rerank.results.count_mismatch", "the response holds the rerank results the call asked for"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    response_json_invalid, response_not_object, response_remote_error, results_required, result_invalid,
    result_index_invalid, results_count_mismatch,
};
}  // namespace rerank
namespace rerank_request {
inline constexpr Reason endpoint_not_admitted{"request.rerank.endpoint_not_admitted", "rerank endpoints are not admitted at this origin"};
inline constexpr Reason path_missing{"request.rerank.path_missing", "the rerank endpoint has no path"};
inline constexpr Reason fields_required{"request.rerank.fields_required", "a rerank needs a model, a query and documents"};
inline constexpr Reason top_invalid{"request.rerank.top_invalid", "a rerank top count must be positive"};
inline constexpr Reason options_not_admitted{"request.rerank.options_not_admitted", "the rerank endpoint admits no options"};
inline constexpr Reason options_invalid{"request.rerank.options_invalid", "rerank options do not match the policy's schema"};
inline constexpr Reason json_limit{"request.rerank.json_limit", "rerank request exceeds JSON limits"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, path_missing, fields_required, top_invalid, options_not_admitted, options_invalid,
    json_limit,
};
}  // namespace rerank_request
namespace moderation {
inline constexpr Reason response_json_invalid{"moderation.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_not_object{"moderation.response.not_object", "response must be object"};
inline constexpr Reason response_remote_error{"moderation.response.remote_error", "remote error property"};
inline constexpr Reason results_required{"moderation.results.required", "moderation results required"};
inline constexpr Reason result_flag_missing{"moderation.result.flag_missing", "a moderation result says whether it is flagged"};
inline constexpr Reason result_categories_invalid{"moderation.result.categories_invalid", "moderation categories are booleans"};
inline constexpr Reason result_scores_invalid{"moderation.result.scores_invalid", "moderation scores are numbers"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    response_json_invalid, response_not_object, response_remote_error, results_required, result_flag_missing,
    result_categories_invalid, result_scores_invalid,
};
}  // namespace moderation
namespace moderation_request {
inline constexpr Reason endpoint_not_admitted{"request.moderation.endpoint_not_admitted", "moderation endpoints are not admitted at this origin"};
inline constexpr Reason path_missing{"request.moderation.path_missing", "the moderation endpoint has no path"};
inline constexpr Reason model_and_inputs_required{"request.moderation.model_and_inputs_required", "a moderation needs a model and inputs"};
inline constexpr Reason input_empty{"request.moderation.input_empty", "a moderation input has text or media"};
inline constexpr Reason media_unsupported{"request.moderation.media_unsupported", "moderation media are images inline or by URL"};
inline constexpr Reason options_not_admitted{"request.moderation.options_not_admitted", "the moderation endpoint admits no options"};
inline constexpr Reason options_invalid{"request.moderation.options_invalid", "moderation options do not match the policy's schema"};
inline constexpr Reason json_limit{"request.moderation.json_limit", "moderation request exceeds JSON limits"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, path_missing, model_and_inputs_required, input_empty, media_unsupported,
    options_not_admitted, options_invalid, json_limit,
};
}  // namespace moderation_request
namespace speech {
inline constexpr Reason response_not_audio{"speech.response.not_audio", "speech response is not audio"};
inline constexpr Reason response_empty{"speech.response.empty", "speech response is empty"};
inline constexpr Reason audio_limit{"speech.audio.limit", "speech audio limit"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    response_not_audio, response_empty, audio_limit,
};
}  // namespace speech
namespace speech_request {
inline constexpr Reason endpoint_not_admitted{"request.speech.endpoint_not_admitted", "speech endpoints are not admitted at this origin"};
inline constexpr Reason path_missing{"request.speech.path_missing", "the speech endpoint has no path"};
inline constexpr Reason model_and_input_required{"request.speech.model_and_input_required", "speech needs a model and input"};
inline constexpr Reason voice_invalid{"request.speech.voice_invalid", "a speech voice is bounded text"};
inline constexpr Reason format_invalid{"request.speech.format_invalid", "a speech format is a token"};
inline constexpr Reason speed_invalid{"request.speech.speed_invalid", "speech speed must be positive"};
inline constexpr Reason options_not_admitted{"request.speech.options_not_admitted", "the speech endpoint admits no options"};
inline constexpr Reason options_invalid{"request.speech.options_invalid", "speech options do not match the policy's schema"};
inline constexpr Reason json_limit{"request.speech.json_limit", "speech request exceeds JSON limits"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, path_missing, model_and_input_required, voice_invalid, format_invalid, speed_invalid,
    options_not_admitted, options_invalid, json_limit,
};
}  // namespace speech_request
namespace transcription {
inline constexpr Reason response_json_invalid{"transcription.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_not_object{"transcription.response.not_object", "response must be object"};
inline constexpr Reason response_remote_error{"transcription.response.remote_error", "remote error property"};
inline constexpr Reason text_required{"transcription.text.required", "transcript text required"};
inline constexpr Reason duration_invalid{"transcription.duration.invalid", "an audio duration is a number"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    response_json_invalid, response_not_object, response_remote_error, text_required, duration_invalid,
};
}  // namespace transcription
namespace transcription_request {
inline constexpr Reason endpoint_not_admitted{"request.transcription.endpoint_not_admitted", "transcription endpoints are not admitted at this origin"};
inline constexpr Reason translate_unsupported{"request.transcription.translate_unsupported", "this origin does not translate speech"};
inline constexpr Reason translate_language{"request.transcription.translate_language", "a translation takes no language"};
inline constexpr Reason path_missing{"request.transcription.path_missing", "the transcription endpoint has no path"};
inline constexpr Reason model_required{"request.transcription.model_required", "a transcription needs a model"};
inline constexpr Reason audio_required{"request.transcription.audio_required", "a transcription takes audio"};
inline constexpr Reason audio_source_unsupported{"request.transcription.audio_source_unsupported", "this endpoint takes the audio inline or (OpenRouter) by URL"};
inline constexpr Reason audio_format_unknown{"request.transcription.audio_format_unknown", "the audio type has no format name"};
inline constexpr Reason language_invalid{"request.transcription.language_invalid", "a transcription language is a token"};
inline constexpr Reason prompt_unsupported{"request.transcription.prompt_unsupported", "a transcription prompt is OpenAI only"};
inline constexpr Reason options_not_admitted{"request.transcription.options_not_admitted", "the transcription endpoint admits no options"};
inline constexpr Reason options_invalid{"request.transcription.options_invalid", "transcription options do not match the policy's schema"};
inline constexpr Reason timestamps_need_verbose{"request.transcription.timestamps_need_verbose", "timestamp granularities need the verbose_json response format"};
inline constexpr Reason audio_not_base64{"request.transcription.audio_not_base64", "the audio is not base64"};
inline constexpr Reason boundary_unavailable{"request.transcription.boundary_unavailable", "no random boundary for the upload"};
inline constexpr Reason size_limit{"request.transcription.size_limit", "transcription request exceeds the size limit"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, translate_unsupported, translate_language, path_missing, model_required, audio_required,
    audio_source_unsupported, audio_format_unknown, language_invalid, prompt_unsupported, options_not_admitted,
    options_invalid, timestamps_need_verbose, audio_not_base64, boundary_unavailable, size_limit,
};
}  // namespace transcription_request
namespace jobs {
inline constexpr Reason response_json_invalid{"jobs.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_not_object{"jobs.response.not_object", "response must be object"};
inline constexpr Reason state_id_and_status_required{"jobs.state.id_and_status_required", "a job names its id and status"};
inline constexpr Reason state_id_mismatch{"jobs.state.id_mismatch", "a poll answers for the job it names"};
inline constexpr Reason state_outputs_invalid{"jobs.state.outputs_invalid", "job outputs are a list"};
inline constexpr Reason download_is_document{"jobs.download.is_document", "a download is not a document"};
inline constexpr Reason download_status{"jobs.download.status", "a download is the body of a 200 response"};
inline constexpr Reason download_empty{"jobs.download.empty", "a download is empty"};
inline constexpr Reason download_limit{"jobs.download.limit", "a download exceeds the content limit"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    response_json_invalid, response_not_object, state_id_and_status_required, state_id_mismatch,
    state_outputs_invalid, download_is_document, download_status, download_empty, download_limit,
};
}  // namespace jobs
namespace jobs_request {
inline constexpr Reason origin_mismatch{"request.jobs.origin_mismatch", "a job is reached at the origin that started it"};
inline constexpr Reason endpoint_not_admitted{"request.jobs.endpoint_not_admitted", "job endpoints are not admitted at this origin"};
inline constexpr Reason path_missing{"request.jobs.path_missing", "the job endpoint has no path for this operation"};
inline constexpr Reason id_invalid{"request.jobs.id_invalid", "a job id is a token"};
inline constexpr Reason download_limit_invalid{"request.jobs.download_limit_invalid", "a download needs a positive byte limit"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    origin_mismatch, endpoint_not_admitted, path_missing, id_invalid, download_limit_invalid,
};
}  // namespace jobs_request
namespace videos_request {
inline constexpr Reason endpoint_not_admitted{"request.videos.endpoint_not_admitted", "video endpoints are not admitted at this origin"};
inline constexpr Reason path_missing{"request.videos.path_missing", "the video endpoint has no submit path"};
inline constexpr Reason model_and_prompt_required{"request.videos.model_and_prompt_required", "a video needs a model and a prompt"};
inline constexpr Reason shape_invalid{"request.videos.shape_invalid", "video aspect ratio, resolution and size are tokens"};
inline constexpr Reason duration_invalid{"request.videos.duration_invalid", "a video duration must be positive"};
inline constexpr Reason reference_unsupported{"request.videos.reference_unsupported", "video references are images inline or by URL"};
inline constexpr Reason options_not_admitted{"request.videos.options_not_admitted", "the video endpoint admits no options"};
inline constexpr Reason options_invalid{"request.videos.options_invalid", "video options do not match the policy's schema"};
inline constexpr Reason json_limit{"request.videos.json_limit", "video request exceeds JSON limits"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, path_missing, model_and_prompt_required, shape_invalid, duration_invalid,
    reference_unsupported, options_not_admitted, options_invalid, json_limit,
};
}  // namespace videos_request
namespace realtime {
inline constexpr Reason turn_unassembled{"realtime.turn.unassembled", "a turn did not assemble"};
inline constexpr Reason message_invalid{"realtime.message.invalid", "a realtime message is a JSON object"};
inline constexpr Reason audio_not_base64{"realtime.audio.not_base64", "realtime audio is base64"};
inline constexpr Reason session_provider_error{"realtime.session.provider_error", "the provider reported an error in the session"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    turn_unassembled, message_invalid, audio_not_base64, session_provider_error,
};
}  // namespace realtime
namespace realtime_request {
inline constexpr Reason endpoint_not_admitted{"request.realtime.endpoint_not_admitted", "realtime sessions are not admitted at this origin"};
inline constexpr Reason path_missing{"request.realtime.path_missing", "the realtime endpoint has no path"};
inline constexpr Reason model_invalid{"request.realtime.model_invalid", "a realtime model is a token"};
inline constexpr Reason transcription_setup_invalid{"request.realtime.transcription_setup_invalid", "a transcription session takes no voice, instructions, tools, output transcription, output audio format or resumption"};
inline constexpr Reason audio_format_invalid{"request.realtime.audio_format_invalid", "a realtime audio format is one the endpoint's fact lists"};
inline constexpr Reason setup_bounds{"request.realtime.setup_bounds", "realtime setup values are bounded"};
inline constexpr Reason tool_invalid{"request.realtime.tool_invalid", "a realtime tool names itself and an object schema"};
inline constexpr Reason options_not_admitted{"request.realtime.options_not_admitted", "the realtime endpoint admits no setup options"};
inline constexpr Reason options_invalid{"request.realtime.options_invalid", "realtime setup options do not match the policy's schema"};
inline constexpr Reason tool_result_invalid{"request.realtime.tool_result_invalid", "a tool result names its call and tool"};
inline constexpr Reason cancel_unsupported{"request.realtime.cancel_unsupported", "Google Live has no cancel; speaking over the model interrupts it"};
inline constexpr Reason transcription_model_required{"request.realtime.transcription_model_required", "OpenAI transcribes the input with a transcription model"};
inline constexpr Reason resume_unsupported{"request.realtime.resume_unsupported", "OpenAI Realtime sessions do not resume"};
inline constexpr Reason turn_detection_unsupported{"request.realtime.turn_detection_unsupported", "Google Live turns automatic activity detection off through realtimeInputConfig"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, path_missing, model_invalid, transcription_setup_invalid, audio_format_invalid,
    setup_bounds, tool_invalid, options_not_admitted, options_invalid, tool_result_invalid, cancel_unsupported,
    transcription_model_required, resume_unsupported, turn_detection_unsupported,
};
}  // namespace realtime_request
namespace files {
inline constexpr Reason endpoint_not_admitted{"files.endpoint.not_admitted", "file endpoints are not admitted at this origin"};
inline constexpr Reason response_json_invalid{"files.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_not_object{"files.response.not_object", "response must be object"};
inline constexpr Reason file_id_required{"files.file.id_required", "a file object names its id"};
inline constexpr Reason page_list_required{"files.page.list_required", "a file listing holds a list"};
inline constexpr Reason page_limit{"files.page.limit", "a file listing exceeds the part limit"};
inline constexpr Reason page_cursor_invalid{"files.page.cursor_invalid", "a page cursor is a string"};
inline constexpr Reason page_cursor_missing{"files.page.cursor_missing", "a page that says more follow names its last id"};
inline constexpr Reason delete_unconfirmed{"files.delete.unconfirmed", "the response does not confirm the deletion"};
inline constexpr Reason media_origin_mismatch{"files.media.origin_mismatch", "a file is named only at the origin that holds it"};
inline constexpr Reason media_not_active{"files.media.not_active", "a file is named once it is active"};
inline constexpr Reason media_expired{"files.media.expired", "the file has expired"};
inline constexpr Reason session_unreadable{"files.session.unreadable", "the upload session is not readable"};
inline constexpr Reason session_status_required{"files.session.status_required", "a Google upload reports its status"};
inline constexpr Reason session_url_required{"files.session.url_required", "a Google upload begins with its upload URL"};
inline constexpr Reason session_file_required{"files.session.file_required", "a finished upload names its file"};
inline constexpr Reason session_size_invalid{"files.session.size_invalid", "a Google upload reports its size as digits"};
inline constexpr Reason session_cancel_unconfirmed{"files.session.cancel_unconfirmed", "the response does not confirm the cancellation"};
inline constexpr Reason session_id_required{"files.session.id_required", "an upload step names its id"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, response_json_invalid, response_not_object, file_id_required, page_list_required,
    page_limit, page_cursor_invalid, page_cursor_missing, delete_unconfirmed, media_origin_mismatch, media_not_active, media_expired,
    session_unreadable, session_status_required, session_url_required, session_file_required, session_size_invalid,
    session_cancel_unconfirmed, session_id_required,
};
}  // namespace files
namespace files_request {
inline constexpr Reason endpoint_not_admitted{"request.files.endpoint_not_admitted", "file endpoints are not admitted at this origin"};
inline constexpr Reason path_missing{"request.files.path_missing", "the file endpoint has no path for this operation"};
inline constexpr Reason id_invalid{"request.files.id_invalid", "a file id is a token, or files/<token> at Google"};
inline constexpr Reason source_ambiguous{"request.files.source_ambiguous", "an upload sends bytes or a file slice, not both"};
inline constexpr Reason source_empty{"request.files.source_empty", "an upload is not empty"};
inline constexpr Reason digest_invalid{"request.files.digest_invalid", "a file slice digest is lower-case hex SHA-256"};
inline constexpr Reason name_invalid{"request.files.name_invalid", "a file name is bounded text without quotes or control characters"};
inline constexpr Reason mime_invalid{"request.files.mime_invalid", "an upload names its MIME type"};
inline constexpr Reason purpose_required{"request.files.purpose_required", "an OpenAI file needs a purpose"};
inline constexpr Reason purpose_unsupported{"request.files.purpose_unsupported", "only OpenAI files take a purpose or an expiry"};
inline constexpr Reason expiry_invalid{"request.files.expiry_invalid", "a file expiry is positive"};
inline constexpr Reason boundary_unavailable{"request.files.boundary_unavailable", "no random boundary for the upload"};
inline constexpr Reason size_limit{"request.files.size_limit", "an upload in memory exceeds the request limit"};
inline constexpr Reason limit_invalid{"request.files.limit_invalid", "a page limit is positive"};
inline constexpr Reason cursor_invalid{"request.files.cursor_invalid", "a page cursor is bounded"};
inline constexpr Reason purpose_invalid{"request.files.purpose_invalid", "a file purpose is a token"};
inline constexpr Reason purpose_filter_unsupported{"request.files.purpose_filter_unsupported", "only OpenAI lists files by purpose"};
inline constexpr Reason download_limit_invalid{"request.files.download_limit_invalid", "a download needs a positive byte limit"};
inline constexpr Reason session_origin_mismatch{"request.files.session_origin_mismatch", "an upload continues at the origin that began it"};
inline constexpr Reason session_url_invalid{"request.files.session_url_invalid", "a Google upload URL is under the origin's upload path"};
inline constexpr Reason session_id_invalid{"request.files.session_id_invalid", "an upload id is a token"};
inline constexpr Reason part_too_large{"request.files.part_too_large", "a part fits in what remains of the upload"};
inline constexpr Reason part_size_limit{"request.files.part_size_limit", "a part is within the endpoint's part size limit"};
inline constexpr Reason part_offset_mismatch{"request.files.part_offset_mismatch", "a file part starts where the upload stands"};
inline constexpr Reason part_granularity{"request.files.part_granularity", "a Google chunk before the last is a multiple of the granularity"};
inline constexpr Reason complete_unsupported{"request.files.complete_unsupported", "a Google upload completes with its last chunk"};
inline constexpr Reason complete_early{"request.files.complete_early", "an upload completes once every part is sent"};
inline constexpr Reason part_id_invalid{"request.files.part_id_invalid", "an upload part id is a token"};
inline constexpr Reason query_unsupported{"request.files.query_unsupported", "only a Google upload says how much it holds"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, path_missing, id_invalid, source_ambiguous, source_empty, digest_invalid, name_invalid,
    mime_invalid, purpose_required, purpose_unsupported, expiry_invalid, boundary_unavailable, size_limit,
    limit_invalid, cursor_invalid, purpose_invalid, purpose_filter_unsupported, download_limit_invalid,
    session_origin_mismatch, session_url_invalid, session_id_invalid, part_too_large, part_size_limit,
    part_offset_mismatch, part_granularity, complete_unsupported, complete_early, part_id_invalid, query_unsupported,
};
}  // namespace files_request
namespace batches {
inline constexpr Reason endpoint_not_admitted{"batches.endpoint.not_admitted", "batch endpoints are not admitted at this origin"};
inline constexpr Reason response_json_invalid{"batches.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_not_object{"batches.response.not_object", "response must be object"};
inline constexpr Reason batch_id_required{"batches.batch.id_required", "a batch object names its id and status"};
inline constexpr Reason delete_unconfirmed{"batches.delete.unconfirmed", "the response does not confirm the deletion"};
inline constexpr Reason line_failed{"batches.line.failed", "the provider failed the batch request"};
inline constexpr Reason line_cancelled{"batches.line.cancelled", "the batch request was cancelled"};
inline constexpr Reason line_expired{"batches.line.expired", "the batch request expired"};
inline constexpr Reason line_invalid{"batches.line.invalid", "a result line is not a batch result"};
inline constexpr Reason line_undecided{"batches.line.undecided", "a result line did not decode to an outcome"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, response_json_invalid, response_not_object, batch_id_required, delete_unconfirmed,
    line_failed, line_cancelled, line_expired, line_invalid, line_undecided,
};
}  // namespace batches
namespace batches_request {
inline constexpr Reason model_path{"request.batches.model_path", "a model name cannot take the place of {model} in the endpoint's path"};
inline constexpr Reason endpoint_not_admitted{"request.batches.endpoint_not_admitted", "batch endpoints are not admitted at this origin"};
inline constexpr Reason path_missing{"request.batches.path_missing", "the batch endpoint has no path for this operation"};
inline constexpr Reason items_required{"request.batches.items_required", "a batch has items"};
inline constexpr Reason items_limit{"request.batches.items_limit", "batch item count limit exceeded"};
inline constexpr Reason custom_id_invalid{"request.batches.custom_id_invalid", "a batch item has a unique custom id of up to 64 letters, digits, _ and -"};
inline constexpr Reason item_family{"request.batches.item_family", "a batch item is a request of the origin's family"};
inline constexpr Reason id_invalid{"request.batches.id_invalid", "a batch id is a token, or batches/<token> at Google"};
inline constexpr Reason file_family{"request.batches.file_family", "an OpenAI batch file holds Chat or Responses requests"};
inline constexpr Reason options_not_admitted{"request.batches.options_not_admitted", "the batch endpoint admits no options"};
inline constexpr Reason options_invalid{"request.batches.options_invalid", "batch options do not match the policy's schema"};
inline constexpr Reason items_unsupported{"request.batches.items_unsupported", "an OpenAI batch reads its items from an uploaded file"};
inline constexpr Reason input_file_required{"request.batches.input_file_required", "an OpenAI batch names its uploaded input file"};
inline constexpr Reason endpoint_invalid{"request.batches.endpoint_invalid", "an OpenAI batch names the path its lines call"};
inline constexpr Reason item_beta{"request.batches.item_beta", "a batch item needs no beta header"};
inline constexpr Reason one_model{"request.batches.one_model", "a Google batch runs one model"};
inline constexpr Reason model_invalid{"request.batches.model_invalid", "a Google batch model is a path segment"};
inline constexpr Reason size_limit{"request.batches.size_limit", "a batch exceeds the request limit"};
inline constexpr Reason download_limit_invalid{"request.batches.download_limit_invalid", "a download needs a positive byte limit"};
inline constexpr Reason origin_mismatch{"request.batches.origin_mismatch", "a batch is reached at the origin that started it"};
inline constexpr Reason errors_unsupported{"request.batches.errors_unsupported", "only an OpenAI batch has a separate error file"};
inline constexpr Reason results_missing{"request.batches.results_missing", "the batch has no results file yet"};
inline constexpr Reason results_file_invalid{"request.batches.results_file_invalid", "a batch results file is a file id"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    model_path, endpoint_not_admitted, path_missing, items_required, items_limit, custom_id_invalid, item_family,
    id_invalid, file_family, options_not_admitted, options_invalid, items_unsupported, input_file_required,
    endpoint_invalid, item_beta, one_model, model_invalid, size_limit, download_limit_invalid, origin_mismatch,
    errors_unsupported, results_missing, results_file_invalid,
};
}  // namespace batches_request
namespace stores {
inline constexpr Reason endpoint_not_admitted{"stores.endpoint.not_admitted", "search store endpoints are not admitted at this origin"};
inline constexpr Reason response_json_invalid{"stores.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_not_object{"stores.response.not_object", "response must be object"};
inline constexpr Reason store_id_required{"stores.store.id_required", "a store object names its id"};
inline constexpr Reason file_id_required{"stores.file.id_required", "a store file object names its id"};
inline constexpr Reason batch_id_required{"stores.batch.id_required", "a file batch or operation names its id"};
inline constexpr Reason page_list_required{"stores.page.list_required", "a store listing holds a list"};
inline constexpr Reason page_cursor_invalid{"stores.page.cursor_invalid", "a store listing cursor is a string"};
inline constexpr Reason page_cursor_missing{"stores.page.cursor_missing", "a store listing that says more follow names its last id"};
inline constexpr Reason page_limit{"stores.page.limit", "a store listing exceeds the part limit"};
inline constexpr Reason search_invalid{"stores.search.results_invalid", "search results are objects that name their file"};
inline constexpr Reason content_invalid{"stores.content.parts_invalid", "a file content page holds text parts"};
inline constexpr Reason delete_unconfirmed{"stores.delete.unconfirmed", "the response does not confirm the deletion"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, response_json_invalid, response_not_object, store_id_required, file_id_required,
    batch_id_required, page_list_required, page_cursor_invalid, page_cursor_missing, page_limit, search_invalid, content_invalid,
    delete_unconfirmed,
};
}  // namespace stores
namespace stores_request {
inline constexpr Reason endpoint_not_admitted{"request.stores.endpoint_not_admitted", "search store endpoints are not admitted at this origin"};
inline constexpr Reason path_missing{"request.stores.path_missing", "the store endpoint has no path for this operation"};
inline constexpr Reason store_invalid{"request.stores.store_invalid", "a store id is a token, or fileSearchStores/<token> at Google"};
inline constexpr Reason id_invalid{"request.stores.id_invalid", "a store file id is a token, or <store>/documents/<token> at Google"};
inline constexpr Reason file_invalid{"request.stores.file_invalid", "a file to index is a token, or files/<token> at Google"};
inline constexpr Reason handle_origin_mismatch{"request.stores.handle_origin_mismatch", "a batch or operation is polled at the origin that started it"};
inline constexpr Reason handle_invalid{"request.stores.handle_invalid", "a batch id is a token, or <store>/operations/<token> at Google"};
inline constexpr Reason query_empty{"request.stores.query_empty", "a search names at least one query and none is empty"};
inline constexpr Reason results_invalid{"request.stores.results_invalid", "max_num_results is positive and within the endpoint's bound"};
inline constexpr Reason object_invalid{"request.stores.object_invalid", "a provider object member is of the kind the provider documents"};
inline constexpr Reason member_unsupported{"request.stores.member_unsupported", "the store endpoint of this shape does not take the member"};
inline constexpr Reason limit_invalid{"request.stores.limit_invalid", "a page limit is positive"};
inline constexpr Reason cursor_invalid{"request.stores.cursor_invalid", "a page cursor is bounded"};
inline constexpr Reason filter_invalid{"request.stores.filter_invalid", "a file status filter is a token"};
inline constexpr Reason expiry_invalid{"request.stores.expiry_invalid", "a store expiry is a positive number of days"};
inline constexpr Reason batch_files_invalid{"request.stores.batch_files_invalid", "a file batch names file_ids or files, one of them"};
inline constexpr Reason update_empty{"request.stores.update_empty", "a store update changes a member"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, path_missing, store_invalid, id_invalid, file_invalid, handle_origin_mismatch,
    handle_invalid, query_empty, results_invalid, object_invalid, member_unsupported, limit_invalid, cursor_invalid,
    filter_invalid, expiry_invalid, batch_files_invalid, update_empty,
};
}  // namespace stores_request
namespace tokens {
inline constexpr Reason response_json_invalid{"tokens.response.json_invalid", "invalid response JSON"};
inline constexpr Reason response_not_object{"tokens.response.not_object", "response must be object"};
inline constexpr Reason count_invalid{"tokens.count.invalid", "a token count is a number"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    response_json_invalid, response_not_object, count_invalid,
};
}  // namespace tokens
namespace tokens_request {
inline constexpr Reason model_path{"request.tokens.model_path", "a model name cannot take the place of {model} in the endpoint's path"};
inline constexpr Reason endpoint_not_admitted{"request.tokens.endpoint_not_admitted", "token counting is not admitted at this origin"};
inline constexpr Reason path_missing{"request.tokens.path_missing", "the token count endpoint has no path"};
inline constexpr Reason family_mismatch{"request.tokens.family_mismatch", "this origin counts the tokens of another family's request"};
inline constexpr Reason body_invalid{"request.tokens.body_invalid", "the request does not encode to a JSON object"};
inline constexpr Reason model_invalid{"request.tokens.model_invalid", "a Google model is a path segment"};
inline constexpr Reason size_limit{"request.tokens.size_limit", "the count request exceeds the request limit"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    model_path, endpoint_not_admitted, path_missing, family_mismatch, body_invalid, model_invalid, size_limit,
};
}  // namespace tokens_request
namespace webhooks {
inline constexpr Reason secret_invalid{"webhooks.secret.invalid", "a webhook secret is whsec_ and base64"};
inline constexpr Reason headers_missing{"webhooks.headers.missing", "a webhook carries its id, timestamp and signature headers"};
inline constexpr Reason timestamp_invalid{"webhooks.timestamp.invalid", "a webhook timestamp is Unix seconds"};
inline constexpr Reason timestamp_stale{"webhooks.timestamp.stale", "the webhook timestamp is outside the tolerance"};
inline constexpr Reason signature_mismatch{"webhooks.signature.mismatch", "the webhook signature does not match"};
inline constexpr Reason body_invalid{"webhooks.body.invalid", "a webhook body is a JSON object"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    secret_invalid, headers_missing, timestamp_invalid, timestamp_stale, signature_mismatch, body_invalid,
};
}  // namespace webhooks
namespace generations {
inline constexpr Reason response_json_invalid{"generations.response.json_invalid", "invalid response JSON"};
inline constexpr Reason data_id_required{"generations.data.id_required", "generation statistics name the generation"};
inline constexpr Reason data_tokens_invalid{"generations.data.tokens_invalid", "generation token counts are numbers"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    response_json_invalid, data_id_required, data_tokens_invalid,
};
}  // namespace generations
namespace generations_request {
inline constexpr Reason endpoint_not_admitted{"request.generations.endpoint_not_admitted", "generation statistics are not admitted at this origin"};
inline constexpr Reason path_missing{"request.generations.path_missing", "the generation endpoint has no path"};
inline constexpr Reason id_invalid{"request.generations.id_invalid", "a generation id is a token"};
// Every constant of the source, for the enumeration test.
inline constexpr Reason all[]{
    endpoint_not_admitted, path_missing, id_invalid,
};
}  // namespace generations_request
// The transport's texts for an abnormal close. The failure is the transport's, so it has no code.
namespace transport {
inline constexpr Reason response_close{"", "abnormal response close"};
inline constexpr Reason gemini_response_close{"", "abnormal Gemini response close"};
inline constexpr Reason interaction_close{"", "abnormal interaction close"};
// Every transport text, for the enumeration test.
inline constexpr Reason all[]{response_close, gemini_response_close, interaction_close};
}  // namespace transport
// An Error that carries the constant's text as safe_message and its code as reason.
inline Error error(ErrorKind kind, const Reason& r) {
  Error e{kind, std::string(r.message)};
  e.reason = std::string(r.code);
  return e;
}
// Every source with the prefix its codes start with, for the enumeration test.
struct Constants { std::string_view prefix; std::span<const Reason> all; };
inline constexpr Constants sources[]{
    {"chat", chat::all},
    {"messages", messages::all},
    {"responses", responses::all},
    {"gemini", gemini::all},
    {"interactions", interactions::all},
    {"accumulator", accumulator::all},
    {"request.chat", chat_request::all},
    {"request.messages", messages_request::all},
    {"request.responses", responses_request::all},
    {"request.generate", gemini_request::all},
    {"request.interactions", interactions_request::all},
    {"request", shared_request::all},
    {"images", images::all},
    {"request.images", images_request::all},
    {"decisions", decisions::all},
    {"request.decisions", decisions_request::all},
    {"embeddings", embeddings::all},
    {"request.embeddings", embeddings_request::all},
    {"rerank", rerank::all},
    {"request.rerank", rerank_request::all},
    {"moderation", moderation::all},
    {"request.moderation", moderation_request::all},
    {"speech", speech::all},
    {"request.speech", speech_request::all},
    {"transcription", transcription::all},
    {"request.transcription", transcription_request::all},
    {"jobs", jobs::all},
    {"request.jobs", jobs_request::all},
    {"request.videos", videos_request::all},
    {"realtime", realtime::all},
    {"request.realtime", realtime_request::all},
    {"files", files::all},
    {"request.files", files_request::all},
    {"batches", batches::all},
    {"request.batches", batches_request::all},
    {"stores", stores::all},
    {"request.stores", stores_request::all},
    {"tokens", tokens::all},
    {"request.tokens", tokens_request::all},
    {"webhooks", webhooks::all},
    {"generations", generations::all},
    {"request.generations", generations_request::all}
};
}  // namespace sp::reason
