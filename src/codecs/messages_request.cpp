#include "codecs/messages_request.h"
#include "codecs/reason.h"
#include "descriptor/policy_refusals.h"
#include "codecs/request_checks.h"
#include "codecs/media_input.h"
#include "core/cache_marks.h"
#include "core/native.h"
#include "json/json.h"
#include "descriptor/policy.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <span>
#include <limits>

namespace sp::messages {
namespace {
std::string lower(std::string_view text) {
  std::string value(text);
  for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return value;
}
// A server tool's result block is named after the tool: <tool>_tool_result answers a call named <tool>, or one of its
// variants named <tool>_<variant> (tool_search_tool_regex), as the decoder pairs them.
bool matching_result(std::string_view name, std::string_view type) {
  constexpr std::string_view suffix = "_tool_result";
  if (type.size() <= suffix.size() || !type.ends_with(suffix)) return false;
  const auto expected = type.substr(0, type.size() - suffix.size());
  return expected == name || (name.size() > expected.size() && name.starts_with(expected) && name[expected.size()] == '_');
}
// The comma-separated values of an anthropic-beta header, without surrounding spaces.
void split_betas(std::string_view header, std::vector<std::string>& out) {
  while (!header.empty()) {
    const auto comma = header.find(',');
    auto value = header.substr(0, comma);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    if (!value.empty() && std::find(out.begin(), out.end(), value) == out.end()) out.emplace_back(value);
    if (comma == std::string_view::npos) break;
    header.remove_prefix(comma + 1);
  }
}
bool native_part(const Part& part) {
  return !std::holds_alternative<Text>(part) && !std::holds_alternative<ToolResult>(part) &&
      !std::holds_alternative<Refusal>(part) && !std::holds_alternative<Media>(part);
}
// Messages media blocks: `image` and `document`, each with a base64, url or file source. A
// text/plain document carries its decoded UTF-8 text rather than base64.
bool plain_text(const Media& media) { return mime_equal(media.mime, "text/plain"); }
std::optional<Error> admit(const descriptor::ValidatedDescriptor& descriptor, const Media& media,
                           std::string& decoded_text, size_t remaining_decoded_bytes) {
  if (auto error = media_input::admit(descriptor, media)) return error;
  if (media.kind == MediaKind::Image && media.detail != ImageDetail::Auto)
    return reason::error(ErrorKind::Unsupported, reason::messages_request::messages_images_have_no_detail_control);
  if (media.kind != MediaKind::Document) return std::nullopt;
  if (descriptor::over_limit(descriptor, "document_title_chars", media.name.size()))
    return reason::error(ErrorKind::InvalidRequest, reason::messages_request::messages_document_title_is_limited_to_500);
  if (media.source == MediaSource::Url && !media.mime.empty() && !mime_equal(media.mime, "application/pdf"))
    return reason::error(ErrorKind::Unsupported, reason::messages_request::messages_url_documents_must_be_pdf);
  if (media.source == MediaSource::Inline && plain_text(media)) {
    if (base64_decoded_size(*media.data) > remaining_decoded_bytes)
      return reason::error(ErrorKind::ResourceLimit, reason::messages_request::decoded_text_documents_exceed_aggregate);
    if (!base64_decode(*media.data, decoded_text) || !media_input::valid_utf8(decoded_text))
      return reason::error(ErrorKind::InvalidRequest, reason::messages_request::messages_text_documents_must_be_valid_utf_8);
  }
  return std::nullopt;
}
void write_source(json::BoundedWriter& body, const Media& media, std::string_view decoded_text) {
  switch (media.source) {
    case MediaSource::Url:
      body.raw("{\"type\":\"url\",\"url\":").quoted(media.reference).raw("}");
      break;
    case MediaSource::File:
      body.raw("{\"type\":\"file\",\"file_id\":").quoted(media.reference).raw("}");
      break;
    case MediaSource::Inline:
      if (plain_text(media)) {
        body.raw("{\"type\":\"text\",\"media_type\":\"text/plain\",\"data\":").quoted(decoded_text).raw("}");
      } else {
        body.raw("{\"type\":\"base64\",\"media_type\":").quoted(lower(mime_essence(media.mime)))
            .raw(",\"data\":\"").raw(*media.data).raw("\"}");
      }
      break;
  }
}
void write_media(json::BoundedWriter& body, const Media& media, std::string_view decoded_text, bool marks) {
  body.raw(media.kind == MediaKind::Image ? "{\"type\":\"image\",\"source\":" : "{\"type\":\"document\",\"source\":");
  write_source(body, media, decoded_text);
  if (media.kind == MediaKind::Document && !media.name.empty()) body.raw(",\"title\":").quoted(media.name);
  if (marks && media.cache) cache_marks::write(body, *media.cache);
  body.raw("}");
}
} // namespace
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](const reason::Reason& message) -> EncodeResult { return reason::error(ErrorKind::InvalidRequest, message); };
  auto replay_bad = []() -> EncodeResult { return reason::error(ErrorKind::ReplayIneligible, reason::messages_request::native_replay_provenance_binding_or_content); };
  if (descriptor.family() != "anthropic.messages") return reason::error(ErrorKind::InvalidConfig, reason::messages_request::messages_encoder_requires_messages_descriptor);
  if (request.model.empty() || request.messages.empty()) return bad(reason::messages_request::model_and_messages_are_required);
  if (auto refusal = descriptor::route_refusal_reason(descriptor, request.model)) return bad(std::move(*refusal));
  if (auto refusal = descriptor::literal_route_refusal_reason(descriptor, request.model)) return reason::error(ErrorKind::InvalidConfig, *refusal);
  const auto& resources = descriptor.policy()->resources();
  const auto& family = descriptor.family_policy();
  auto effective = descriptor::effective_defaults(descriptor, request.model);
  if (request.max_tokens) effective.max_output_tokens = request.max_tokens;
  if (request.thinking_budget) effective.thinking_budget = request.thinking_budget;
  if (request.temperature) effective.temperature = request.temperature;
  if (request.top_p) effective.top_p = request.top_p;
  if (request.temperature && descriptor::temperature_forbidden(descriptor, request.model))
    return bad(reason::messages_request::temperature_is_prohibited_for_this_model);
  if (request.temperature && (!std::isfinite(*request.temperature) ||
      *request.temperature < family.temperature_range[0] || *request.temperature > family.temperature_range[1]))
    return bad(reason::messages_request::temperature_outside_admitted_range);
  std::optional<ThinkingMode> thinking_mode = request.thinking_mode;
  if (thinking_mode) {
    switch (*thinking_mode) {
      case ThinkingMode::Manual:
        if (!effective.thinking_budget || !*effective.thinking_budget) return bad(reason::messages_request::manual_thinking_requires_a_positive_budget);
        break;
      case ThinkingMode::Adaptive:
      case ThinkingMode::Disabled:
        if (request.thinking_budget) return bad(reason::messages_request::adaptive_and_disabled_thinking_forbid_budget);
        effective.thinking_budget.reset();
        break;
      default: return bad(reason::messages_request::invalid_thinking_mode);
    }
  } else if (effective.thinking_budget) thinking_mode = ThinkingMode::Manual;
  const bool thinking_enabled = thinking_mode && *thinking_mode != ThinkingMode::Disabled;
  if (thinking_enabled) {
    effective.temperature.reset();
    if (effective.top_p && *effective.top_p < family.thinking_top_p_minimum)
      return bad(reason::messages_request::thinking_top_p_outside_admitted_range);
  }
  if (auto error = descriptor::choices_refusal(descriptor, request.model, effective)) return bad(std::move(*error));
  std::string_view effort, ttl, tool_mode;
  if (request.output_effort) switch (*request.output_effort) {
    case OutputEffort::Low: effort = "low"; break;
    case OutputEffort::Medium: effort = "medium"; break;
    case OutputEffort::High: effort = "high"; break;
    case OutputEffort::Max: effort = "max"; break;
    case OutputEffort::XHigh: effort = "xhigh"; break;
    default: return bad(reason::messages_request::invalid_output_effort);
  }
  if (request.cache_control && request.cache_control->ttl) switch (*request.cache_control->ttl) {
    case CacheTtl::FiveMinutes: ttl = "5m"; break;
    case CacheTtl::OneHour: ttl = "1h"; break;
    default: return bad(reason::messages_request::invalid_cache_ttl);
  }
  const auto fact_of = [&](std::string_view type) -> const descriptor::ServerToolFact* {
    const auto found = std::find_if(family.server_tools.begin(), family.server_tools.end(), [&](const auto& fact) { return fact.type == type; });
    return found == family.server_tools.end() ? nullptr : &*found;
  };
  // anthropic-beta values: the descriptor's own, then the request's, then those a declared tool needs. Each one the
  // request adds must be admitted by the policy at this origin. Replayed history binds the tools it was answered
  // with, so a replayed advisor call always comes with its declared tool and beta.
  std::vector<std::string> betas;
  const auto beta_admitted = [&](std::string_view value) {
    return std::any_of(family.betas.begin(), family.betas.end(), [&](const auto& fact) {
      return fact.value == value && descriptor::contains(fact.origins, descriptor.base_url());
    });
  };
  for (const auto& header : descriptor.headers()) if (lower(header.first) == "anthropic-beta") split_betas(header.second, betas);
  if (request.betas.size() > 64) return bad(reason::messages_request::beta_count_limit);
  for (const auto& beta : request.betas) {
    if (beta.empty() || beta.find_first_of(", \t\r\n") != std::string::npos) return bad(reason::messages_request::beta_invalid);
    if (!beta_admitted(beta)) return reason::error(ErrorKind::Unsupported, reason::messages_request::beta_not_admitted);
    if (std::find(betas.begin(), betas.end(), beta) == betas.end()) betas.push_back(beta);
  }
  const auto need_beta = [&](const descriptor::ServerToolFact& fact) {
    if (fact.beta.empty()) return true;
    if (!beta_admitted(fact.beta)) return false;
    if (std::find(betas.begin(), betas.end(), fact.beta) == betas.end()) betas.push_back(fact.beta);
    return true;
  };
  for (const auto& tool : request.tools)
    if (const auto* fact = tool.type.empty() ? nullptr : fact_of(tool.type); fact && !need_beta(*fact))
      return reason::error(ErrorKind::Unsupported, reason::messages_request::tool_beta_not_admitted);
  // A block that stands for the conversation before it (`compaction`) is where the service reads the history from:
  // it ignores what comes earlier, or refuses it in front of a signed block. Only the history from the last captured
  // message that carries one is sent and bound to its seals; what comes earlier is neither.
  std::size_t first = 0;
  for (std::size_t i = request.messages.size(); i-- > 0;)
    if (opens_history(family, request.messages[i])) { first = i; break; }
  // A replayed block the service only takes with a beta (`mcp_tool_listing`) sends that beta again: a signed
  // `compaction` block its `signed_beta`.
  for (std::size_t i = first; i < request.messages.size(); ++i)
    for (const auto& part : request.messages[i].parts)
      if (const auto* opaque = std::get_if<Opaque>(&part)) {
        const auto* fact = kept_block(family, opaque->wire_type);
        if (!fact) continue;
        const bool signed_block = opaque->wire_metadata && opaque->wire_metadata->root().get("signature").valid();
        const auto& beta = signed_block && !fact->signed_beta.empty() ? fact->signed_beta : fact->beta;
        if (beta.empty()) continue;
        if (!beta_admitted(beta)) return reason::error(ErrorKind::Unsupported, reason::messages_request::block_beta_not_admitted);
        if (std::find(betas.begin(), betas.end(), beta) == betas.end()) betas.push_back(beta);
      }
  // MCP servers: bounded, unique names and https URLs, each named by exactly one `mcp_toolset` entry.
  if (descriptor::over_limit(descriptor, "mcp_servers", request.mcp_servers.size())) return bad(reason::messages_request::mcp_server_count_limit);
  std::set<std::string_view> server_names;
  const auto printable = [](std::string_view value, std::size_t limit) {
    return !value.empty() && value.size() <= limit && std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
  };
  for (const auto& server : request.mcp_servers) {
    if (!printable(server.name, 255) || !server_names.insert(server.name).second) return bad(reason::messages_request::mcp_server_name_invalid);
    if (!server.url.starts_with("https://") || server.url.size() <= 8 || !printable(server.url, 2048) || server.url.find_first_of(" \"\\") != std::string::npos)
      return bad(reason::messages_request::mcp_server_url_invalid);
    if (server.authorization_token && !printable(*server.authorization_token, 8192)) return bad(reason::messages_request::mcp_server_token_invalid);
  }
  std::map<std::string_view, int> referenced;
  for (const auto& tool : request.tools) if (tool.type == "mcp_toolset") {
    const auto name = tool.options ? tool.options->root().get("mcp_server_name") : json::Value{};
    if (!name.is_string() || !server_names.contains(name.as_string()) || ++referenced[name.as_string()] > 1)
      return bad(reason::messages_request::mcp_toolset_server_invalid);
  }
  if (referenced.size() != server_names.size()) return bad(reason::messages_request::mcp_server_without_toolset);
  if (request.tool_choice) {
    const auto& choice = *request.tool_choice;
    switch (choice.mode) {
      case ToolChoiceMode::Auto: tool_mode = "auto"; break;
      case ToolChoiceMode::Any: tool_mode = "any"; break;
      case ToolChoiceMode::None: tool_mode = "none"; break;
      case ToolChoiceMode::Tool: tool_mode = "tool"; break;
      default: return bad(reason::messages_request::invalid_tool_choice);
    }
    if (choice.mode == ToolChoiceMode::Tool) {
      // A custom tool, or a typed tool the caller executes (memory, bash), named by the request.
      if (choice.name.empty() || std::none_of(request.tools.begin(), request.tools.end(), [&](const auto& tool) {
            const auto* fact = tool.type.empty() ? nullptr : fact_of(tool.type);
            return tool.name == choice.name && (tool.type.empty() || (fact && fact->client));
          }))
        return bad(reason::messages_request::named_tool_choice_requires_a_declared_client);
    } else if (!choice.name.empty()) return bad(reason::messages_request::only_named_tool_choice_accepts_a_name);
    if ((choice.mode == ToolChoiceMode::Any || choice.mode == ToolChoiceMode::Tool) &&
        (request.tools.empty() || thinking_enabled))
      return bad(reason::messages_request::forced_tool_choice_requires_tools_and);
    if (choice.mode == ToolChoiceMode::None && choice.disable_parallel_tool_use)
      return bad(reason::messages_request::none_tool_choice_cannot_control_parallel_use);
  }
  if (request.provider)
    if (auto error = request_controls::validate_routing(descriptor, *request.provider)) return bad(std::move(*error));
  if (request.messages.size() > resources.request_messages || request.tools.size() > resources.request_tools)
    return bad(reason::messages_request::request_count_limit_exceeded);
  // Check seals before encoding or interpreting the edited contents. Removing a seal
  // never converts native parts or captured tool calls into imported, trusted input.
  bool captured_history = false;
  std::optional<Error> media_error;
  bool has_media = false;
  // Decode text documents once for admission; both bounded writer passes borrow these bytes.
  std::vector<std::string> decoded_documents;
  size_t retained_decoded_bytes = 0;
  auto retain_media = [&](const Media& media) {
    if (media_error) return;
    has_media = true;
    std::string decoded_text;
    media_error = admit(descriptor, media, decoded_text, resources.request_bytes - retained_decoded_bytes);
    if (!media_error && media.kind == MediaKind::Document && media.source == MediaSource::Inline && plain_text(media)) {
      retained_decoded_bytes += decoded_text.size();
      decoded_documents.push_back(std::move(decoded_text));
    }
  };
  // A block the caller writes is one the policy admits, matching the schema of the whole block.
  auto admit_block = [&](const Opaque& block) -> std::optional<Error> {
    const auto* fact = input_block(family, block.wire_type);
    if (!fact) return reason::error(ErrorKind::Unsupported, reason::messages_request::input_block_not_admitted);
    const auto raw = block.wire_metadata ? block.wire_metadata->root() : json::Value{};
    if (!raw.is_object() || !raw.get("type").is_string() || raw.get("type").as_string() != block.wire_type)
      return reason::error(ErrorKind::InvalidRequest, reason::messages_request::input_block_metadata_mismatch);
    if (descriptor::check_value(fact->value, raw)) return reason::error(ErrorKind::InvalidRequest, reason::messages_request::input_block_invalid);
    return std::nullopt;
  };
  for (size_t i = 0; i < request.messages.size(); ++i) {
    const auto& message = request.messages[i];
    if (message.parts.size() > resources.request_parts) return bad(reason::messages_request::request_part_count_limit_exceeded);
    // A block the caller writes in a user message (`search_result`) is input, not native state; it is admitted below.
    if (!message.native) for (const auto& part : message.parts)
      if (native_part(part) && (message.role == Role::Assistant || !std::holds_alternative<Opaque>(part))) return replay_bad();
    captured_history = captured_history || static_cast<bool>(message.native);
    if (i < first) continue;  // not sent
    for (const auto& part : message.parts) {
      if (const auto* block = std::get_if<Opaque>(&part); block && !media_error && message.role != Role::Assistant) {
        media_error = admit_block(*block);
        continue;
      }
      if (const auto* media = std::get_if<Media>(&part); media && !media_error) {
        if (message.role != Role::User) media_error = reason::error(ErrorKind::InvalidRequest, reason::messages_request::media_inputs_require_user_role);
        else retain_media(*media);
      } else if (const auto* tool = std::get_if<ToolResult>(&part); tool && !media_error) {
        if (!tool->content_parts.empty() && !tool->content.empty())
          media_error = reason::error(ErrorKind::InvalidRequest, reason::messages_request::tool_result_content_and_content_parts_are);
        else if (tool->content_parts.size() > resources.request_parts)
          media_error = reason::error(ErrorKind::InvalidRequest, reason::messages_request::tool_result_part_count_limit_exceeded);
        else if (!tool->content_parts.empty() && message.role != Role::User && message.role != Role::Tool)
          media_error = reason::error(ErrorKind::InvalidRequest, reason::messages_request::tool_results_require_user_or_tool_role);
        else for (const auto& nested : tool->content_parts) if (const auto* block = std::get_if<Opaque>(&nested); block && !media_error) {
          media_error = admit_block(*block);
        } else if (const auto* media = std::get_if<Media>(&nested)) {
          retain_media(*media);
          if (!media_error && (!media->id.empty() || !media->transcript.empty() ||
              (media->kind == MediaKind::Image && !media->name.empty()) ||
              (media->kind == MediaKind::Document && media->detail != ImageDetail::Auto)))
            media_error = reason::error(ErrorKind::Unsupported, reason::messages_request::tool_result_media_contains_unsupported_output);
          if (media_error) break;
        }
      }
    }
  }
  // Invalid ordinary input never reaches hashing. For captured history, retain
  // lineage mismatch precedence even when the edited prefix is no longer valid.
  if (media_error && !captured_history) return *media_error;
  if (auto refusal = request_controls::validate_extensions(descriptor, request.extensions))
    return reason::error(refusal->admitted ? ErrorKind::InvalidRequest : ErrorKind::Unsupported, refusal->cause);
  // An extension the service only takes with a beta (`compaction`, `context_management`) sends it.
  if (request.extensions) for (auto member : request.extensions->root().members())
    for (const auto& fact : family.extensions)
      if (fact.key == member.key && !fact.beta.empty() && descriptor::contains(fact.origins, descriptor.base_url())) {
        if (!beta_admitted(fact.beta)) return reason::error(ErrorKind::Unsupported, reason::messages_request::extension_beta_not_admitted);
        if (std::find(betas.begin(), betas.end(), fact.beta) == betas.end()) betas.push_back(fact.beta);
      }
  // Cache marks (core/cache_marks.h). The marks the typed request carries on what is sent ask the policy what the
  // origin does with them; marks written, they are listed in prompt order (tools, system, messages) for the limits.
  // A cache_control written inside a block or a tool's options joins the list but never asks the policy.
  if (!request.system.empty() && !request.system_blocks.empty()) return bad(reason::messages_request::system_blocks_conflict);
  if (request.system_blocks.size() > resources.request_parts) return bad(reason::messages_request::request_part_count_limit_exceeded);
  for (const auto& block : request.system_blocks) if (block.text.empty()) return bad(reason::messages_request::system_block_empty);
  CacheMarkReport mark_report;
  bool write_marks = false;
  {
    std::uint32_t requested = cache_marks::tally(std::span<const Message>(request.messages).subspan(first)).all;
    for (const auto& tool : request.tools) requested += tool.cache.has_value();
    for (const auto& block : request.system_blocks) requested += block.cache.has_value();
    auto admitted = cache_marks::admit(descriptor, request.model, requested);
    if (auto* refusal = std::get_if<Error>(&admitted)) return std::move(*refusal);
    const auto& admission = std::get<cache_marks::Admission>(admitted);
    mark_report.requested = requested;
    if (admission.disposition == cache_marks::Disposition::Emit) {
      std::vector<std::optional<CacheTtl>> lifetimes;
      const auto raw_mark = [&](json::Value holder) {
        const auto mark = holder.is_object() ? holder.get("cache_control") : json::Value{};
        if (!mark.is_object()) return;
        const auto ttl = mark.get("ttl");
        lifetimes.push_back(!ttl.is_string() ? std::nullopt : ttl.as_string() == "1h" ? std::optional<CacheTtl>(CacheTtl::OneHour) : std::optional<CacheTtl>(CacheTtl::FiveMinutes));
      };
      for (const auto& tool : request.tools) {
        if (tool.cache && tool.options && tool.options->root().is_object() && tool.options->root().get("cache_control").valid())
          return bad(reason::messages_request::cache_control_duplicate);
        if (tool.cache) lifetimes.push_back(tool.cache->ttl);
        else if (tool.options) raw_mark(tool.options->root());
      }
      for (const auto& block : request.system_blocks) if (block.cache) lifetimes.push_back(block.cache->ttl);
      for (std::size_t i = first; i < request.messages.size(); ++i)
        for (const auto& part : request.messages[i].parts) {
          if (const auto* text = std::get_if<Text>(&part)) {
            if (auto error = cache_marks::marked_text(*text)) return std::move(*error);
            if (text->cache) lifetimes.push_back(text->cache->ttl);
          } else if (const auto* media = std::get_if<Media>(&part)) {
            if (media->cache) lifetimes.push_back(media->cache->ttl);
          } else if (const auto* block = std::get_if<Opaque>(&part)) {
            if (block->wire_metadata && request.messages[i].role != Role::Assistant) raw_mark(block->wire_metadata->root());
          } else if (const auto* result = std::get_if<ToolResult>(&part)) {
            // The service refuses a cache_control inside `tool_result.content`; the mark of the result covers its parts.
            for (const auto& nested : result->content_parts) {
              if (const auto* text = std::get_if<Text>(&nested); text && text->cache)
                return reason::error(ErrorKind::Unsupported, reason::messages_request::cache_mark_in_tool_result);
              if (const auto* media = std::get_if<Media>(&nested); media && media->cache)
                return reason::error(ErrorKind::Unsupported, reason::messages_request::cache_mark_in_tool_result);
              if (const auto* block = std::get_if<Opaque>(&nested); block && block->wire_metadata) raw_mark(block->wire_metadata->root());
            }
            if (result->cache) lifetimes.push_back(result->cache->ttl);
          }
        }
      if (auto error = cache_marks::check(admission.capabilities, lifetimes)) return std::move(*error);
      // The request-level mark yields to the marks of the parts.
      write_marks = true;
      mark_report.emitted = requested;
      mark_report.tail_suppressed = request.cache_control.has_value();
    }
  }
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, effective, streaming));
  if (!context->history_valid_) return replay_bad();
  if (!context->valid_) return reason::error(ErrorKind::ResourceLimit, reason::messages_request::native_binding_could_not_be_captured);
  if (media_error) return *media_error;
  EncodedRequest result{"POST", descriptor.resolved_path(streaming, request.model), {}, {}, {}};
  std::string_view version = *family.header_version;
  for (const auto& header : descriptor.headers()) {
    auto key = lower(header.first);
    if (key != "content-type" && key != "accept" && key != "anthropic-version" && key != "anthropic-beta") result.headers.push_back(header);
    else if (key == "anthropic-version") version = header.second;
  }
  result.headers.emplace_back("Content-Type", "application/json");
  result.headers.emplace_back("Accept", streaming ? "text/event-stream" : "application/json");
  result.headers.emplace_back("anthropic-version", version);
  if (!betas.empty()) {
    std::string joined;
    for (const auto& beta : betas) { if (!joined.empty()) joined += ','; joined += beta; }
    result.headers.emplace_back("anthropic-beta", std::move(joined));
  }
  auto build = [&](json::BoundedWriter& body) -> std::optional<Error> {
  size_t text_document_index = 0;
  auto bad = [](const reason::Reason& message) { return reason::error(ErrorKind::InvalidRequest, message); };
  body.raw("{").quoted(descriptor.request_model_member()).raw(":").quoted(request.model);
  body.raw(",").quoted(descriptor.max_output_tokens_member()).raw(":").raw(std::to_string(*effective.max_output_tokens));
  body.raw(",").quoted(descriptor.request_stream_member()).raw(streaming ? ":true" : ":false");
  if (!request.system.empty()) body.raw(",\"system\":").quoted(request.system);
  if (!request.system_blocks.empty()) {
    body.raw(",\"system\":[");
    for (std::size_t i = 0; i < request.system_blocks.size(); ++i) {
      const auto& block = request.system_blocks[i];
      if (i) body.raw(",");
      body.raw("{\"type\":\"text\",\"text\":").quoted(block.text);
      if (write_marks && block.cache) cache_marks::write(body, *block.cache);
      body.raw("}");
    }
    body.raw("]");
  }
  if (request.container) {
    const auto& id = *request.container;
    if (id.empty() || id.size() > 256 || std::any_of(id.begin(), id.end(), [](unsigned char c) { return c <= 0x20 || c >= 0x7f || c == '"' || c == '\\'; }))
      return bad(reason::messages_request::container_invalid);
  }
  if (!request.skills.empty()) {
    // The service names skill types and versions: bounded tokens, not a fixed list.
    const auto token = [](std::string_view value) {
      return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
      });
    };
    std::set<std::pair<std::string_view, std::string_view>> skills;
    if (descriptor::over_limit(descriptor, "container_skills", request.skills.size())) return bad(reason::messages_request::skills_count_limit);
    // Skills load into the code-execution container, which only a declared code-execution tool provides.
    if (std::none_of(request.tools.begin(), request.tools.end(), [&](const auto& tool) {
          const auto* fact = tool.type.empty() ? nullptr : fact_of(tool.type);
          return fact && fact->container;
        }))
      return bad(reason::messages_request::skills_need_code_execution);
    for (const auto& skill : request.skills)
      if (!token(skill.type) || !token(skill.skill_id) || (skill.version && !token(*skill.version)) || !skills.emplace(skill.type, skill.skill_id).second)
        return bad(reason::messages_request::skills_invalid);
    body.raw(",\"container\":{");
    if (request.container) body.raw("\"id\":").quoted(*request.container).raw(",");
    body.raw("\"skills\":[");
    for (std::size_t i = 0; i < request.skills.size(); ++i) {
      const auto& skill = request.skills[i];
      if (i) body.raw(",");
      body.raw("{\"type\":").quoted(skill.type).raw(",\"skill_id\":").quoted(skill.skill_id);
      if (skill.version) body.raw(",\"version\":").quoted(*skill.version);
      body.raw("}");
    }
    body.raw("]}");
  } else if (request.container) body.raw(",\"container\":").quoted(*request.container);
  if (thinking_mode) {
    body.raw(",\"thinking\":{\"type\":");
    switch (*thinking_mode) {
      case ThinkingMode::Manual:
        body.quoted("enabled").raw(",\"budget_tokens\":").raw(std::to_string(*effective.thinking_budget)); break;
      case ThinkingMode::Adaptive: body.quoted("adaptive"); break;
      case ThinkingMode::Disabled: body.quoted("disabled"); break;
    }
    body.raw("}");
  }
  if (request.output_effort) body.raw(",\"output_config\":{\"effort\":").quoted(effort).raw("}");
  if (request.cache_control && !write_marks) {
    body.raw(",\"cache_control\":{\"type\":\"ephemeral\"");
    if (!ttl.empty()) body.raw(",\"ttl\":").quoted(ttl);
    body.raw("}");
  }
  if (request.tool_choice) {
    body.raw(",\"tool_choice\":{\"type\":").quoted(tool_mode);
    if (request.tool_choice->mode == ToolChoiceMode::Tool) body.raw(",\"name\":").quoted(request.tool_choice->name);
    if (request.tool_choice->disable_parallel_tool_use)
      body.raw(",\"disable_parallel_tool_use\":").raw(*request.tool_choice->disable_parallel_tool_use ? "true" : "false");
    body.raw("}");
  }
  if (request.provider) { body.raw(",\"provider\":"); request_controls::write_routing(body, *request.provider); }
  auto number = [&](std::string_view key, double value) {
    char bytes[64]; const auto converted = std::to_chars(bytes, bytes + sizeof bytes, value);
    body.raw(",").quoted(key).raw(":").raw(std::string_view(bytes, converted.ptr));
  };
  if (effective.temperature) number("temperature", *effective.temperature);
  if (effective.top_p) number("top_p", *effective.top_p);
  std::set<std::string_view> tool_names;
  if (!request.tools.empty()) {
    body.raw(",\"tools\":[");
    bool comma = false;
    for (const auto& tool : request.tools) {
      const auto* fact = tool.type.empty() ? nullptr : fact_of(tool.type);
      if (!tool.type.empty() && !fact) return reason::error(ErrorKind::Unsupported, reason::messages_request::unsupported_server_tool_type);
      // A toolset entry carries no name; every other tool has a unique one.
      if ((!fact || !fact->name.empty()) && (tool.name.empty() || !tool_names.insert(tool.name).second))
        return bad(reason::messages_request::tools_require_unique_nonempty_names);
      if (comma) body.raw(",");
      comma = true;
      const bool server_options = !tool.allowed_callers.empty() || !tool.allowed_domains.empty() || !tool.blocked_domains.empty() ||
                                  tool.user_location || tool.citations || tool.max_content_tokens;
      if (!fact) {
        if (!tool.input_schema || !tool.input_schema->root().is_object() || tool.max_uses || server_options || tool.options)
          return bad(reason::messages_request::client_tools_require_object_schemas_and_no);
        body.raw("{\"name\":").quoted(tool.name).raw(",\"description\":").quoted(tool.description).raw(",\"input_schema\":").value(tool.input_schema->root(), 3);
        if (write_marks && tool.cache) cache_marks::write(body, *tool.cache);
        body.raw("}");
      } else {
        if (tool.name != fact->name || tool.input_schema || !tool.description.empty()) return bad(reason::messages_request::server_tool_fields_do_not_match_typed_schema);
        if (tool.options && !fact->options) return reason::error(ErrorKind::Unsupported, reason::messages_request::tool_options_not_admitted);
        if (tool.options && descriptor::check_value(*fact->options, tool.options->root())) return bad(reason::messages_request::tool_options_invalid);
        if (tool.max_uses && (!*tool.max_uses || !fact->max_uses)) return bad(reason::messages_request::max_uses_is_unsupported_or_out_of_range);
        // Caller types and domains are values the service extends: bounded tokens and host names, not a fixed list.
        const auto token = [](std::string_view value) {
          return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
          });
        };
        const auto domain = [](std::string_view value) {
          return !value.empty() && value.size() <= 253 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return c > 0x20 && c < 0x7f && c != '"' && c != '\\';
          });
        };
        const auto bounded = [](const std::vector<std::string>& values, auto check) {
          std::set<std::string_view> seen;
          return values.size() <= 64 && std::all_of(values.begin(), values.end(), [&](const std::string& v) { return check(v) && seen.insert(v).second; });
        };
        if (!bounded(tool.allowed_callers, token)) return bad(reason::messages_request::allowed_callers_invalid);
        if (!bounded(tool.allowed_domains, domain) || !bounded(tool.blocked_domains, domain)) return bad(reason::messages_request::domain_filters_invalid);
        if (!tool.allowed_domains.empty() && !tool.blocked_domains.empty()) return bad(reason::messages_request::domain_filters_exclusive);
        if (tool.max_content_tokens && !*tool.max_content_tokens) return bad(reason::messages_request::max_content_tokens_invalid);
        if (tool.user_location) {
          for (const auto* field : {&tool.user_location->city, &tool.user_location->region, &tool.user_location->country, &tool.user_location->timezone})
            if (*field && (field->value().empty() || field->value().size() > 128)) return bad(reason::messages_request::user_location_invalid);
        }
        const auto strings = [&](std::string_view key, const std::vector<std::string>& values) {
          if (values.empty()) return;
          body.raw(",").quoted(key).raw(":[");
          for (std::size_t i = 0; i < values.size(); ++i) { if (i) body.raw(","); body.quoted(values[i]); }
          body.raw("]");
        };
        body.raw("{\"type\":").quoted(tool.type);
        if (!tool.name.empty()) body.raw(",\"name\":").quoted(tool.name);
        if (tool.max_uses) body.raw(",\"max_uses\":").raw(std::to_string(*tool.max_uses));
        strings("allowed_callers", tool.allowed_callers);
        strings("allowed_domains", tool.allowed_domains);
        strings("blocked_domains", tool.blocked_domains);
        if (tool.user_location) {
          body.raw(",\"user_location\":{\"type\":\"approximate\"");
          const auto& location = *tool.user_location;
          if (location.city) body.raw(",\"city\":").quoted(*location.city);
          if (location.region) body.raw(",\"region\":").quoted(*location.region);
          if (location.country) body.raw(",\"country\":").quoted(*location.country);
          if (location.timezone) body.raw(",\"timezone\":").quoted(*location.timezone);
          body.raw("}");
        }
        if (tool.citations) body.raw(",\"citations\":{\"enabled\":").raw(*tool.citations ? "true" : "false").raw("}");
        if (tool.max_content_tokens) body.raw(",\"max_content_tokens\":").raw(std::to_string(*tool.max_content_tokens));
        if (tool.options) for (auto member : tool.options->root().members()) body.raw(",").quoted(member.key).raw(":").value(member.value, 3);
        if (write_marks && tool.cache) cache_marks::write(body, *tool.cache);
        body.raw("}");
      }
      if (!body.ok()) return reason::error(has_media ? ErrorKind::ResourceLimit : ErrorKind::InvalidRequest, reason::messages_request::request_exceeds_json_limits_or_encoding);
    }
    body.raw("]");
  }
  if (!request.mcp_servers.empty()) {
    body.raw(",\"mcp_servers\":[");
    for (std::size_t i = 0; i < request.mcp_servers.size(); ++i) {
      const auto& server = request.mcp_servers[i];
      if (i) body.raw(",");
      body.raw("{\"type\":\"url\",\"url\":").quoted(server.url).raw(",\"name\":").quoted(server.name);
      if (server.authorization_token) body.raw(",\"authorization_token\":").quoted(*server.authorization_token);
      body.raw("}");
    }
    body.raw("]");
  }
  body.raw(",").quoted(descriptor.request_messages_member()).raw(":[");
  std::set<std::string_view> all_call_ids;
  std::map<std::string_view, std::string_view> client_pending;  // call id -> toolset name of the call, if any
  std::map<std::string_view, std::string_view> server_pending;
  bool message_comma = false;
  for (std::size_t index = first; index < request.messages.size(); ++index) {
    const auto& message = request.messages[index];
    const bool tool_role = message.role == Role::Tool;
    if (message.role != Role::User && message.role != Role::Assistant && !tool_role)
      return reason::error(ErrorKind::Unsupported, reason::messages_request::messages_history_supports_only_user_and);
    if (tool_role && (message.parts.empty() || std::any_of(message.parts.begin(), message.parts.end(),
        [](const Part& part) { return !std::holds_alternative<ToolResult>(part); })))
      return bad(reason::messages_request::tool_role_messages_require_tool_results_only);
    const bool assistant = message.role == Role::Assistant;
    if (assistant && !client_pending.empty()) return bad(reason::messages_request::client_tool_results_must_immediately_follow);
    if (!assistant && client_pending.empty() && !server_pending.empty()) return bad(reason::messages_request::server_continuation_requires_the_captured);
    const bool responding_to_tools = !client_pending.empty();
    const bool server_waiting = !server_pending.empty();
    bool text_seen = false;
    if (message_comma) body.raw(",");
    message_comma = true;
    body.raw(assistant ? "{\"role\":\"assistant\",\"content\":[" : "{\"role\":\"user\",\"content\":[");
    bool part_comma = false;
    for (const auto& part : message.parts) {
      if (part_comma) body.raw(",");
      part_comma = true;
      if (const auto* text = std::get_if<Text>(&part)) {
        if (!assistant && responding_to_tools && server_waiting) return bad(reason::messages_request::pending_server_tools_require_tool_results);
        text_seen = true;
        body.raw("{\"type\":\"text\",\"text\":").quoted(text->value);
        if (write_marks && text->cache) cache_marks::write(body, *text->cache);
        body.raw("}");
      } else if (const auto* media = std::get_if<Media>(&part)) {
        if (responding_to_tools && server_waiting) return bad(reason::messages_request::pending_server_tools_require_tool_results);
        text_seen = true; // Tool results, when present, must precede ordinary input.
        const auto decoded_text = media->source == MediaSource::Inline && plain_text(*media)
            ? std::string_view(decoded_documents.at(text_document_index++)) : std::string_view{};
        write_media(body, *media, decoded_text, write_marks);
      } else if (const auto* thinking = std::get_if<Thinking>(&part)) {
        if (!assistant) return bad(reason::messages_request::thinking_belongs_to_assistant_messages);
        body.raw("{\"type\":\"thinking\",\"thinking\":").quoted(thinking->text);
        if (thinking->signature) body.raw(",\"signature\":").quoted(*thinking->signature);
        body.raw("}");
      } else if (const auto* redacted = std::get_if<RedactedThinking>(&part)) {
        if (!assistant) return bad(reason::messages_request::redacted_thinking_belongs_to_assistant);
        body.raw("{\"type\":\"redacted_thinking\",\"data\":").quoted(redacted->data).raw("}");
      } else if (const auto* call = std::get_if<ToolCall>(&part)) {
        if (!assistant || call->id.empty() || call->name.empty() || !call->input || !call->input->root().is_object()) return bad(reason::messages_request::invalid_tool_call_history);
        if (call->kind == ToolCallKind::ApprovalRequest) return reason::error(ErrorKind::Unsupported, reason::messages_request::approval_request_replay_is_unsupported);
        const bool server = call->kind == ToolCallKind::ServerExecuted;
        const bool mcp = server && call->wire_type == "mcp_tool_use";
        const std::string_view type = mcp ? "mcp_tool_use" : server ? "server_tool_use" : "tool_use";
        if (call->wire_type != type || !all_call_ids.insert(call->id).second) return bad(reason::messages_request::tool_call_type_or_ownership_mismatch);
        // An MCP call is answered by `mcp_tool_result`, so it pairs as a call of the tool `mcp`.
        if (server) server_pending.emplace(call->id, mcp ? std::string_view("mcp") : std::string_view(call->name));
        else {
          const auto toolset = call->wire_metadata ? call->wire_metadata->root().get("toolset_name") : json::Value{};
          client_pending.emplace(call->id, toolset.is_string() ? toolset.as_string() : std::string_view{});
        }
        body.raw("{\"type\":").quoted(type).raw(",\"id\":").quoted(call->id).raw(",\"name\":").quoted(call->name).raw(",\"input\":").value(call->input->root(), 5);
        if (call->wire_metadata) {
          if (!call->wire_metadata->root().is_object()) return bad(reason::messages_request::tool_metadata_must_be_object);
          for (auto member : call->wire_metadata->root().members()) {
            if (member.key == "input" || member.key == "id" || member.key == "name" || member.key == "type") return bad(reason::messages_request::tool_metadata_collides_with_owned_fields);
            body.raw(",").quoted(member.key).raw(":").value(member.value, 5);
          }
        }
        body.raw("}");
      } else if (const auto* tool_result = std::get_if<ToolResult>(&part)) {
        const auto owner = client_pending.find(tool_result->tool_use_id);
        if (assistant || text_seen || owner == client_pending.end()) return bad(reason::messages_request::tool_result_lacks_immediate_unique_client);
        // A toolset member's result names the toolset of its call.
        const auto toolset = owner->second;
        client_pending.erase(owner);
        body.raw("{\"type\":\"tool_result\",\"tool_use_id\":").quoted(tool_result->tool_use_id);
        if (!toolset.empty()) body.raw(",\"toolset_name\":").quoted(toolset);
        body.raw(",\"content\":");
        if (tool_result->content_parts.empty()) body.quoted(tool_result->content);
        else {
          body.raw("["); bool nested_comma = false;
          for (const auto& nested : tool_result->content_parts) {
            if (nested_comma) body.raw(",");
            nested_comma = true;
            if (const auto* text = std::get_if<Text>(&nested))
              body.raw("{\"type\":\"text\",\"text\":").quoted(text->value).raw("}");
            else if (const auto* block = std::get_if<Opaque>(&nested)) body.value(block->wire_metadata->root(), 6);
            else {
              const auto& media = std::get<Media>(nested);
              const auto decoded = media.kind == MediaKind::Document && media.source == MediaSource::Inline && plain_text(media)
                  ? std::string_view(decoded_documents.at(text_document_index++)) : std::string_view{};
              write_media(body, media, decoded, false);
            }
            if (!body.ok()) return reason::error(has_media ? ErrorKind::ResourceLimit : ErrorKind::InvalidRequest, reason::messages_request::request_exceeds_json_limits_or_encoding);
          }
          body.raw("]");
        }
        body.raw(",\"is_error\":").raw(tool_result->is_error ? "true" : "false");
        if (write_marks && tool_result->cache) cache_marks::write(body, *tool_result->cache);
        body.raw("}");
      } else if (const auto* server_result = std::get_if<ServerToolResult>(&part)) {
        const auto found = server_pending.find(server_result->tool_use_id);
        if (!assistant || found == server_pending.end() || !matching_result(found->second, server_result->wire_type) || !server_result->content) return bad(reason::messages_request::server_result_lacks_unique_server_ownership);
        const auto raw = server_result->content->root();
        if (!raw.is_object() || !raw.get("type").is_string() || raw.get("type").as_string() != server_result->wire_type || !raw.get("tool_use_id").is_string() || raw.get("tool_use_id").as_string() != server_result->tool_use_id) return bad(reason::messages_request::server_result_metadata_mismatch);
        server_pending.erase(found);
        body.value(raw, 4);
      } else if (const auto* block = std::get_if<Opaque>(&part); block && !assistant) {
        // An input block the caller writes (`search_result`), admitted above, goes as given.
        if (responding_to_tools && server_waiting) return bad(reason::messages_request::pending_server_tools_require_tool_results);
        text_seen = true; // Tool results, when present, must precede ordinary input.
        body.value(block->wire_metadata->root(), 4);
      } else if (const auto* opaque = std::get_if<Opaque>(&part); opaque && assistant && opaque->wire_metadata && kept_block(family, opaque->wire_type)) {
        // A block the policy keeps whole (`fallback`, `mcp_tool_listing`, `compaction`) goes back exactly as captured, in place.
        const auto raw = opaque->wire_metadata->root();
        if (!raw.is_object() || !raw.get("type").is_string() || raw.get("type").as_string() != opaque->wire_type) return bad(reason::messages_request::captured_block_mismatch);
        body.value(raw, 4);
      } else return reason::error(ErrorKind::Unsupported, reason::messages_request::part_cannot_be_replayed_as_messages_input);
      if (!body.ok()) return reason::error(has_media ? ErrorKind::ResourceLimit : ErrorKind::InvalidRequest, reason::messages_request::request_exceeds_json_limits_or_encoding);
    }
    body.raw("]}");
    if (!assistant && !client_pending.empty()) return bad(reason::messages_request::all_client_tool_calls_need_immediate_results);
    if (assistant && !server_pending.empty() && (!message.native || (message.native->stop_ != StopKind::PauseTurn && message.native->stop_ != StopKind::ToolUse))) return bad(reason::messages_request::unfinished_server_call_lacks_continuation);
    if (assistant && message.native && message.native->stop_ == StopKind::PauseTurn && !client_pending.empty()) return bad(reason::messages_request::pause_continuation_cannot_await_client_tools);
  }
  if (!client_pending.empty()) return bad(reason::messages_request::client_tool_results_are_required_before);
  body.raw("]");
  request_controls::write_extensions(body, request.extensions);
  body.raw("}");
  if (!body.ok()) return reason::error(has_media ? ErrorKind::ResourceLimit : ErrorKind::InvalidRequest, reason::messages_request::request_exceeds_json_limits_or_encoding);
  return std::nullopt;
  };
  // Measure the entire aggregate before allocating its encoded representation.
  // Both passes use the same writer and syntax, including all document leaves.
  json::BoundedWriter measured({resources.request_bytes, resources.json_depth});
  if (auto error = build(measured)) return *error;
  result.body.reserve(measured.size());
  json::BoundedWriter body({measured.size(), resources.json_depth}, &result.body);
  if (auto error = build(body)) return *error;
  result.context = std::move(context);
  result.max_output_tokens = effective.max_output_tokens;
  result.cache_marks = mark_report;
  // Only the tools the service runs add model invocations; the caller runs memory, bash and the toolsets.
  for (const auto& tool : request.tools) if (const auto* fact = tool.type.empty() ? nullptr : fact_of(tool.type); fact && !fact->client) {
    if (!tool.max_uses || !result.model_invocation_limit ||
        *tool.max_uses > std::numeric_limits<std::uint64_t>::max() - *result.model_invocation_limit) {
      result.model_invocation_limit.reset();
      break;
    }
    *result.model_invocation_limit += *tool.max_uses;
  }
  return result;
}
} // namespace sp::messages
