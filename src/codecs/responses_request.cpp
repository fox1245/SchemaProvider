#include "codecs/responses_request.h"
#include "codecs/reason.h"
#include "descriptor/policy_refusals.h"
#include "codecs/request_checks.h"
#include "codecs/media_input.h"
#include "core/cache_marks.h"
#include "core/native.h"
#include "json/json.h"
#include "descriptor/policy.h"
#include <map>
#include <set>
#include <type_traits>
#include <charconv>
#include <limits>

namespace sp::responses {
std::string_view tool_type(const HostedTool& tool) noexcept {
  return std::visit([](const auto& value) -> std::string_view {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, WebSearchTool>) return "web_search";
    else if constexpr (std::is_same_v<T, ImageGenerationTool>) return "image_generation";
    else if constexpr (std::is_same_v<T, FileSearchTool>) return "file_search";
    else if constexpr (std::is_same_v<T, ToolSearchTool>) return "tool_search";
    else if constexpr (std::is_same_v<T, ShellTool>) return "shell";
    else if constexpr (std::is_same_v<T, CodeInterpreterTool>) return "code_interpreter";
    else if constexpr (std::is_same_v<T, McpTool>) return "mcp";
    else if constexpr (std::is_same_v<T, ComputerTool>) return "computer";
    else if constexpr (std::is_same_v<T, ComputerUsePreviewTool>) return "computer_use_preview";
    else if constexpr (std::is_same_v<T, OpenRouterServerTool> || std::is_same_v<T, GenericHostedTool>) return value.type;
    else static_assert(sizeof(T) == 0, "a hosted tool alternative has no type here");
  }, tool);
}
namespace {
std::string lower(std::string_view text) {
  std::string value(text);
  for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return value;
}
std::string_view role_name(Role role) {
  switch (role) {
    case Role::System: return "system";
    case Role::Developer: return "developer";
    case Role::User: return "user";
    default: return {};
  }
}
bool function_name(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  for (const char c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}
// The service's enumerations are lower-case names; the SDK checks only their shape.
bool lower_token(std::string_view value, std::size_t maximum = 32) {
  return !value.empty() && value.size() <= maximum && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
  });
}
bool image_size(std::string_view value) {
  if (value == "auto") return true;
  const auto x = value.find('x');
  const auto digits = [](std::string_view part) {
    return !part.empty() && part.size() <= 5 && std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; });
  };
  return x != std::string_view::npos && digits(value.substr(0, x)) && digits(value.substr(x + 1));
}
bool single_line(std::string_view value, std::size_t maximum) {
  return value.size() <= maximum && value.find_first_of(std::string_view("\r\n\0", 3)) == std::string_view::npos;
}
bool header_name(std::string_view value) {
  return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
  });
}
bool https_url(std::string_view value) {
  return value.size() > 8 && value.size() <= 2048 && value.starts_with("https://") &&
         std::none_of(value.begin(), value.end(), [](unsigned char c) { return c <= 0x20 || c >= 0x7f; });
}
bool unique_texts(const std::vector<std::string>& values, std::size_t limit, bool allow_empty_list = true) {
  if ((!allow_empty_list && values.empty()) || values.size() > limit) return false;
  std::set<std::string_view> seen;
  for (const auto& value : values) if (!request_controls::bounded_text(value, 256) || !seen.insert(value).second) return false;
  return true;
}
std::optional<reason::Reason> filter_error(const McpToolFilter& filter, std::size_t limit) {
  if (!unique_texts(filter.tool_names, limit) || (filter.tool_names.empty() && !filter.read_only)) return reason::responses_request::mcp_tool_filters_require_unique_names_or_read;
  return std::nullopt;
}
std::string_view verbosity_name(Verbosity value) {
  switch (value) {
    case Verbosity::Low: return "low";
    case Verbosity::Medium: return "medium";
    case Verbosity::High: return "high";
  }
  return {};
}
std::string_view truncation_name(Truncation value) {
  switch (value) {
    case Truncation::Disabled: return "disabled";
    case Truncation::Auto: return "auto";
  }
  return {};
}
std::string_view include_name(Include value) {
  switch (value) {
    case Include::ReasoningEncryptedContent: return "reasoning.encrypted_content";
    case Include::WebSearchSources: return "web_search_call.action.sources";
    case Include::FileSearchResults: return "file_search_call.results";
    case Include::MessageOutputTextLogprobs: return "message.output_text.logprobs";
    case Include::ComputerCallOutputImageUrl: return "computer_call_output.output.image_url";
    case Include::CodeInterpreterCallOutputs: return "code_interpreter_call.outputs";
  }
  return {};
}
bool cursor_valid(std::string_view id) {
  if (id.empty() || id.size() > 256) return false;
  for (const char c : id) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}
// Responses input content: input_image, input_audio, input_video and input_file. Admission has
// already established that the family and origin accept this kind and source form.
void write_media(json::BoundedWriter& body, const Media& media) {
  switch (media.kind) {
    case MediaKind::Image:
      body.raw("{\"type\":\"input_image\",");
      if (media.source == MediaSource::File) body.raw("\"file_id\":").quoted(media.reference);
      else { body.raw("\"image_url\":"); media_input::uri_or_url(body, media); }
      body.raw(",\"detail\":").quoted(image_detail_name(media.detail)).raw("}");
      break;
    case MediaKind::Audio:
      body.raw("{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"").raw(*media.data)
          .raw("\",\"format\":").quoted(*media_input::audio_format(media.mime)).raw("}}");
      break;
    case MediaKind::Video:
      body.raw("{\"type\":\"input_video\",\"video_url\":");
      media_input::uri_or_url(body, media);
      body.raw("}");
      break;
    case MediaKind::Document:
      body.raw("{\"type\":\"input_file\",");
      if (media.source == MediaSource::File) body.raw("\"file_id\":").quoted(media.reference);
      else if (media.source == MediaSource::Url) body.raw("\"file_url\":").quoted(media.reference);
      else {
        body.raw("\"filename\":").quoted(media_input::document_filename(media)).raw(",\"file_data\":");
        media_input::data_uri(body, media);
      }
      body.raw("}");
      break;
  }
}
std::optional<Error> admit(const descriptor::ValidatedDescriptor& descriptor, const Media& media) {
  if (auto error = media_input::admit(descriptor, media)) return error;
  if (media.kind == MediaKind::Audio && !media_input::audio_format(media.mime))
    return reason::error(ErrorKind::Unsupported, reason::responses_request::responses_input_audio_has_no_format_name_for);
  return std::nullopt;
}
std::string_view tool_choice_name(ToolChoiceMode mode) {
  switch (mode) {
    case ToolChoiceMode::Auto: return "auto";
    case ToolChoiceMode::Required: return "required";
    case ToolChoiceMode::None: return "none";
    default: return {};
  }
}
// One tool a choice names, in the shape of its kind. An MCP server is written with the type of its declared tool.
void write_chosen(json::BoundedWriter& body, AllowedToolKind kind, std::string_view name, const std::vector<HostedTool>& tools) {
  switch (kind) {
    case AllowedToolKind::Function: body.raw("{\"type\":\"function\",\"name\":").quoted(name); break;
    case AllowedToolKind::Hosted: body.raw("{\"type\":").quoted(name); break;
    case AllowedToolKind::Mcp:
      for (const auto& tool : tools)
        if (const auto* server = std::get_if<McpTool>(&tool); server && server->server_label == name) {
          body.raw("{\"type\":").quoted(tool_type(tool)).raw(",\"server_label\":").quoted(name);
          break;
        }
      break;
  }
}
void write_tool_choice(json::BoundedWriter& body, const ToolChoice& choice, const std::vector<HostedTool>& tools) {
  if (choice.allowed) {
    body.raw("{\"type\":\"allowed_tools\",\"mode\":").quoted(tool_choice_name(choice.mode)).raw(",\"tools\":[");
    for (std::size_t i = 0; i < choice.allowed->size(); ++i) {
      const auto& tool = (*choice.allowed)[i];
      if (i) body.raw(",");
      write_chosen(body, tool.kind, tool.name, tools);
      body.raw("}");
    }
    body.raw("]}");
    return;
  }
  switch (choice.mode) {
    case ToolChoiceMode::Function: write_chosen(body, AllowedToolKind::Function, choice.name, tools); break;
    case ToolChoiceMode::Hosted: write_chosen(body, AllowedToolKind::Hosted, choice.name, tools); break;
    case ToolChoiceMode::Mcp:
      write_chosen(body, AllowedToolKind::Mcp, choice.name, tools);
      if (choice.mcp_tool) body.raw(",\"name\":").quoted(*choice.mcp_tool);
      break;
    default: body.quoted(tool_choice_name(choice.mode)); return;
  }
  body.raw("}");
}
} // namespace
EncodeResult encode(const descriptor::ValidatedDescriptor& descriptor, const Request& request, bool streaming) {
  auto bad = [](const reason::Reason& message) -> EncodeResult { return reason::error(ErrorKind::InvalidRequest, message); };
  auto replay_bad = []() -> EncodeResult { return reason::error(ErrorKind::ReplayIneligible, reason::responses_request::native_replay_provenance_binding_or_content); };
  if (descriptor.family() != "openai.responses") return reason::error(ErrorKind::InvalidConfig, reason::responses_request::responses_encoder_requires_responses);
  if (request.model.empty() || request.messages.empty()) return bad(reason::responses_request::model_and_messages_are_required);
  if (auto refusal = descriptor::route_refusal_reason(descriptor, request.model)) return bad(std::move(*refusal));
  if (auto refusal = descriptor::literal_route_refusal_reason(descriptor, request.model)) return reason::error(ErrorKind::InvalidConfig, *refusal);
  CacheMarkReport mark_report;
  if (auto error = cache_marks::drop(descriptor, request.model, request.messages, mark_report)) return std::move(*error);
  const auto& resources = descriptor.policy()->resources();
  const auto& defaults = descriptor::model_defaults(descriptor, request.model);
  auto effective = descriptor::effective_defaults(descriptor, request.model);
  if (request.max_output_tokens) effective.max_output_tokens = request.max_output_tokens;
  if (request.service_tier) effective.service_tier = *request.service_tier;
  if (request.temperature) effective.temperature = request.temperature;
  if (request.top_p) effective.top_p = request.top_p;
  if (request.reasoning) {
    effective.reasoning_enabled = true;
    if (request.reasoning->effort) effective.reasoning_effort = *request.reasoning->effort;
    if (request.reasoning->summary) effective.reasoning_summary = *request.reasoning->summary;
  }
  if (auto error = descriptor::choices_refusal(descriptor, request.model, effective)) return bad(std::move(*error));
  if (request.messages.size() > resources.request_messages || request.tools.size() > resources.request_tools ||
      request.hosted_tools.size() > resources.request_tools - request.tools.size())
    return bad(reason::responses_request::request_count_limit_exceeded);
  if (request.previous_response_id && !cursor_valid(*request.previous_response_id)) return bad(reason::responses_request::invalid_previous_response_cursor);
  if (!request.previous_response_id && !request.previous_response_history.empty()) return bad(reason::responses_request::previous_response_history_requires_a_cursor);
  if (request.previous_response_history.size() > resources.request_messages) return bad(reason::responses_request::previous_response_history_count_limit);
  for (const auto& message : request.previous_response_history)
    if (message.parts.size() > resources.request_parts) return bad(reason::responses_request::previous_response_part_count_limit_exceeded);
  if (!request.previous_response_history.empty()) {
    const auto& previous = request.previous_response_history.back();
    if (previous.role != Role::Assistant || !previous.native || !previous.wire_output ||
        !previous.wire_output->root().is_array() || previous.id != *request.previous_response_id) return replay_bad();
  }
  if (request.verbosity && verbosity_name(*request.verbosity).empty()) return bad(reason::responses_request::invalid_verbosity);
  if (request.truncation && truncation_name(*request.truncation).empty()) return bad(reason::responses_request::invalid_truncation);
  if (request.include) {
    std::set<Include> selections;
    for (const auto selection : *request.include)
      if (include_name(selection).empty() || !selections.insert(selection).second) return bad(reason::responses_request::invalid_or_duplicate_include_selection);
  }
  {
    std::set<std::string_view> names;
    if (request.include) for (const auto selection : *request.include) names.insert(include_name(selection));
    else names.insert("reasoning.encrypted_content");
    if (request.include_names.size() > resources.request_parts) return bad(reason::responses_request::request_count_limit_exceeded);
    for (const auto& name : request.include_names)
      if (!lower_token(name, 128) || !names.insert(name).second) return bad(reason::responses_request::invalid_or_duplicate_include_selection);
  }
  if (request.provider)
    if (auto error = request_controls::validate_routing(descriptor, *request.provider)) return bad(std::move(*error));
  if (request.response_format)
    if (auto error = request_controls::validate_response_format(*request.response_format, resources)) return bad(std::move(*error));
  const bool router = descriptor::contains(descriptor.family_policy().openrouter_origins, descriptor.base_url());
  // The types of the declared MCP servers are kept apart: a server is chosen by its label, never by its type.
  std::set<std::string_view> hosted_types, mcp_labels, server_types;
  for (const auto& tool : request.hosted_tools) {
    const auto type = tool_type(tool);
    if (const auto* routed = std::get_if<OpenRouterServerTool>(&tool)) {
      // OpenRouter's server tools are its own catalogue: any openrouter: type is sent, at a declared origin.
      if (!router) return reason::error(ErrorKind::Unsupported, reason::responses_request::openrouter_server_tools_require_a_declared);
      if (!routed->type.starts_with("openrouter:") || routed->type.size() <= 11 || !request_controls::config_token(routed->type, 128) ||
          (routed->parameters && !routed->parameters->root().is_object()))
        return bad(reason::responses_request::openrouter_server_tools_require_an_openrouter);
      // The policy admits the tool and the schema of its parameters (#32).
      const auto& tools = descriptor.family_policy().openrouter_server_tools;
      const auto fact = std::find_if(tools.begin(), tools.end(), [&](const auto& f) { return f.name == routed->type; });
      if (fact == tools.end()) return reason::error(ErrorKind::Unsupported, reason::responses_request::an_openrouter_server_tool_is_not_admitted_by);
      if (auto error = request_controls::validate_options(fact->options, routed->parameters)) return bad(reason::responses_request::openrouter_server_tool_parameters_do_not_match);
    } else {
      // A typed tool needs a typed fact, and a generic one a generic fact whose schema its options match.
      const auto& facts = descriptor.family_policy().server_tools;
      const auto generic = std::get_if<GenericHostedTool>(&tool);
      const auto fact = std::find_if(facts.begin(), facts.end(), [&](const auto& f) { return f.type == type && f.generic == (generic != nullptr); });
      if (fact == facts.end()) return reason::error(ErrorKind::Unsupported, reason::responses_request::hosted_responses_tool_is_not_admitted_by);
      if (generic && request_controls::validate_options(fact->options, generic->options)) return bad(reason::responses_request::a_generic_hosted_tool_s_options_do_not_match);
    }
    if (const auto* mcp = std::get_if<McpTool>(&tool)) {
      if (!function_name(mcp->server_label) || !mcp_labels.insert(mcp->server_label).second) return bad(reason::responses_request::mcp_tools_require_unique_server_labels);
      server_types.insert(type);
    } else if (!hosted_types.insert(type).second) return bad(reason::responses_request::hosted_tool_types_must_be_unique);
    if (const auto* image = std::get_if<ImageGenerationTool>(&tool)) {
      if (image->size && !image_size(*image->size)) return bad(reason::responses_request::image_generation_size_must_be_auto_or);
      for (const auto* field : {&image->quality, &image->output_format, &image->action, &image->background, &image->moderation, &image->input_fidelity})
        if (*field && !lower_token(**field)) return bad(reason::responses_request::image_generation_options_must_be_lower_case);
      if ((image->model && !request_controls::config_token(*image->model, 128)) || (image->output_compression && *image->output_compression > 100))
        return bad(reason::responses_request::image_generation_model_or_compression_is);
      if ((image->mask_file_id && !request_controls::bounded_text(*image->mask_file_id, 256)) ||
          (image->mask_image_url && !single_line(*image->mask_image_url, resources.request_bytes)) ||
          (image->mask_image_url && image->mask_image_url->empty()))
        return bad(reason::responses_request::image_generation_mask_requires_a_file_id_or_an);
    }
    if (const auto* files = std::get_if<FileSearchTool>(&tool)) {
      if (files->vector_store_ids.empty() || files->vector_store_ids.size() > resources.request_parts)
        return bad(reason::responses_request::file_search_requires_bounded_vector_store);
      std::set<std::string_view> ids;
      for (const auto& id : files->vector_store_ids)
        if (id.empty() || !ids.insert(id).second) return bad(reason::responses_request::vector_store_ids_must_be_nonempty_and_unique);
      if (files->max_num_results && !*files->max_num_results) return bad(reason::responses_request::file_search_result_count_must_be_positive);
    }
    if (const auto* search = std::get_if<WebSearchTool>(&tool)) {
      if (!unique_texts(search->allowed_domains, resources.request_parts) || (search->search_context_size && !lower_token(*search->search_context_size)))
        return bad(reason::responses_request::web_search_domains_or_context_size_are_invalid);
      if (search->user_location) {
        const auto& at = *search->user_location;
        for (const auto* field : {&at.city, &at.country, &at.region, &at.timezone})
          if (*field && !request_controls::bounded_text(**field, 128)) return bad(reason::responses_request::web_search_location_fields_must_be_short_text);
      }
    }
    if (const auto* code = std::get_if<CodeInterpreterTool>(&tool)) {
      if (code->container_id && (!request_controls::bounded_text(*code->container_id, 256) || !code->file_ids.empty() || code->memory_limit || code->network_policy))
        return bad(reason::responses_request::a_code_interpreter_container_id_excludes);
      if (!unique_texts(code->file_ids, resources.request_parts) || (code->memory_limit && !lower_token(*code->memory_limit)))
        return bad(reason::responses_request::code_interpreter_files_or_memory_limit_are);
      if (code->network_policy) {
        const auto& policy = *code->network_policy;
        if (policy.disabled ? !policy.allowed_domains.empty() || !policy.domain_secrets.empty() : !unique_texts(policy.allowed_domains, resources.request_parts, false))
          return bad(reason::responses_request::a_network_policy_is_disabled_or_lists_unique);
        std::set<std::pair<std::string_view, std::string_view>> secrets;
        for (const auto& secret : policy.domain_secrets)
          if (std::find(policy.allowed_domains.begin(), policy.allowed_domains.end(), secret.domain) == policy.allowed_domains.end() ||
              !header_name(secret.name) || !single_line(secret.value, 8192) || !secrets.emplace(secret.domain, secret.name).second)
            return bad(reason::responses_request::domain_secrets_require_an_allowed_domain_a);
      }
    }
    if (const auto* mcp = std::get_if<McpTool>(&tool)) {
      const int sources = mcp->server_url.has_value() + mcp->connector_id.has_value() + mcp->tunnel_id.has_value();
      if (sources != 1 || (mcp->server_url && !https_url(*mcp->server_url)) || (mcp->connector_id && !lower_token(*mcp->connector_id, 64)) ||
          (mcp->tunnel_id && !request_controls::config_token(*mcp->tunnel_id, 128)))
        return bad(reason::responses_request::mcp_tools_require_one_https_server_url);
      if (mcp->authorization && (mcp->authorization->empty() || !single_line(*mcp->authorization, 8192)))
        return bad(reason::responses_request::mcp_authorization_and_headers_require_single);
      std::set<std::string> header_names;
      for (const auto& [name, value] : mcp->headers) {
        std::string folded(name);
        for (auto& c : folded) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (!header_name(name) || !single_line(value, 8192) || !header_names.insert(std::move(folded)).second)
          return bad(reason::responses_request::mcp_authorization_and_headers_require_single);
      }
      if (mcp->require_approval && (mcp->approval_always || mcp->approval_never)) return bad(reason::responses_request::mcp_approval_is_a_mode_or_filters_not_both);
      if (mcp->require_approval && !lower_token(*mcp->require_approval)) return bad(reason::responses_request::mcp_approval_mode_must_be_a_lower_case_token);
      for (const auto* filter : {&mcp->allowed_tools, &mcp->approval_always, &mcp->approval_never})
        if (*filter) if (auto error = filter_error(**filter, resources.request_parts)) return bad(std::move(*error));
      if (mcp->server_description && !request_controls::bounded_text(*mcp->server_description, 4096, true))
        return bad(reason::responses_request::mcp_server_description_must_be_text);
    }
    if (const auto* screen = std::get_if<ComputerUsePreviewTool>(&tool))
      if (!lower_token(screen->environment) || !screen->display_width || !screen->display_height)
        return bad(reason::responses_request::computer_use_requires_an_environment_and_a);
    if (const auto* shell = std::get_if<ShellTool>(&tool)) {
      if (shell->environment.skills.size() > resources.request_parts) return bad(reason::responses_request::shell_skill_count_limit_exceeded);
      std::set<std::string_view> ids;
      for (const auto& skill : shell->environment.skills)
        if (skill.skill_id.empty() || !ids.insert(skill.skill_id).second)
          return bad(reason::responses_request::shell_skill_references_must_be_nonempty_and);
    }
  }
  if (!request.plugins.empty()) {
    if (!router) return reason::error(ErrorKind::Unsupported, reason::responses_request::openrouter_plugins_require_a_declared);
    if (auto refusal = request_controls::validate_plugins(descriptor.family_policy(), request.plugins, resources.request_tools))
      return reason::error(refusal->admitted ? ErrorKind::InvalidRequest : ErrorKind::Unsupported, refusal->cause);
  }
  if (!request.modalities.empty() || request.image_config) {
    if (!router) return reason::error(ErrorKind::Unsupported, reason::responses_request::output_modalities_and_image_config_require_a);
    unsigned mask = 0;
    for (const auto modality : request.modalities) {
      const auto bit = 1U << static_cast<unsigned>(modality);
      if (static_cast<unsigned>(modality) > static_cast<unsigned>(OutputModality::Image) || (mask & bit)) return bad(reason::responses_request::output_modalities_require_unique_valid_values);
      mask |= bit;
    }
    if (request.image_config && !(mask & (1U << static_cast<unsigned>(OutputModality::Image)))) return bad(reason::responses_request::image_config_needs_the_image_modality);
    if (request.image_config)
      if (auto error = request_controls::validate_image_config(*request.image_config)) return bad(std::move(*error));
  }
  {
    std::set<std::string_view> approvals;
    if (request.mcp_approvals.size() > resources.request_parts) return bad(reason::responses_request::request_count_limit_exceeded);
    for (const auto& approval : request.mcp_approvals)
      if (!request_controls::bounded_text(approval.request_id, 256) || !approvals.insert(approval.request_id).second ||
          (approval.reason && !request_controls::bounded_text(*approval.reason, 4096, true)))
        return bad(reason::responses_request::mcp_approvals_require_unique_request_ids_and);
      else if (approval.reason && approval.approve) return bad(reason::responses_request::an_mcp_approval_gives_a_reason_only_when_it);
  }
  bool deferred = false;
  for (const auto& tool : request.tools) deferred = deferred || tool.defer_loading.value_or(false);
  if (deferred && !hosted_types.contains("tool_search"))
    return bad(reason::responses_request::deferred_functions_require_tool_search);
  if (hosted_types.contains("tool_search") && !deferred)
    return bad(reason::responses_request::tool_search_requires_a_deferred_function);
  if (request.tool_choice) {
    // Everything a choice names is declared by this request: a function, an MCP server by its label, or a hosted tool
    // by its type, which the policy's server-tool facts admitted above.
    const auto& choice = *request.tool_choice;
    if (request.required_tool) return bad(reason::responses_request::tool_choice_and_required_tool_exclusive);
    std::set<std::string_view> functions;
    for (const auto& tool : request.tools) functions.insert(tool.name);
    const auto hosted = [&](std::string_view type) {
      const auto facts = descriptor::tool_facts(descriptor.family_policy(), type);
      return hosted_types.contains(type) && facts.known && !facts.client;
    };
    // A type the request declares only through an MCP server.
    const auto server_only = [&](std::string_view type) { return server_types.contains(type) && !hosted_types.contains(type); };
    switch (choice.mode) {
      case ToolChoiceMode::Auto: case ToolChoiceMode::Required: case ToolChoiceMode::None:
        if (!choice.name.empty()) return bad(reason::responses_request::only_a_named_tool_choice_takes_a_name);
        break;
      case ToolChoiceMode::Function:
        if (!functions.contains(choice.name)) return bad(reason::responses_request::function_tool_choice_must_be_declared);
        break;
      case ToolChoiceMode::Hosted:
        if (server_only(choice.name)) return bad(reason::responses_request::mcp_server_is_not_a_hosted_tool_choice);
        if (!hosted(choice.name)) return bad(reason::responses_request::hosted_tool_choice_must_be_declared);
        break;
      case ToolChoiceMode::Mcp:
        if (!mcp_labels.contains(choice.name)) return bad(reason::responses_request::mcp_tool_choice_must_be_declared);
        break;
      default: return bad(reason::responses_request::invalid_tool_choice_mode);
    }
    if (choice.mcp_tool && choice.mode != ToolChoiceMode::Mcp) return bad(reason::responses_request::only_an_mcp_tool_choice_names_a_server_tool);
    if (choice.mcp_tool && !request_controls::bounded_text(*choice.mcp_tool, 256))
      return bad(reason::responses_request::mcp_tool_choice_tool_name_must_be_short_text);
    if (choice.allowed) {
      if (choice.mode != ToolChoiceMode::Auto && choice.mode != ToolChoiceMode::Required)
        return bad(reason::responses_request::allowed_tools_require_auto_or_required);
      if (choice.allowed->empty()) return bad(reason::responses_request::allowed_tools_require_at_least_one);
      std::set<std::pair<AllowedToolKind, std::string_view>> seen;
      for (const auto& tool : *choice.allowed) {
        bool declared = false;
        switch (tool.kind) {
          case AllowedToolKind::Function: declared = functions.contains(tool.name); break;
          case AllowedToolKind::Mcp: declared = mcp_labels.contains(tool.name); break;
          case AllowedToolKind::Hosted:
            if (server_only(tool.name)) return bad(reason::responses_request::mcp_server_is_not_a_hosted_tool_choice);
            declared = hosted(tool.name);
            break;
          default: return bad(reason::responses_request::invalid_allowed_tool_kind);
        }
        if (!declared) return bad(reason::responses_request::allowed_tools_must_be_declared);
        if (!seen.emplace(tool.kind, tool.name).second) return bad(reason::responses_request::allowed_tools_must_be_unique);
      }
    }
  }
  // Captured groups never degrade to caller-imported assistant history when their
  // seal or original items are removed. Only plain input and tool results are imports.
  bool captured_history = false;
  std::optional<Error> media_error;
  for (const auto& message : request.messages) {
    if (message.parts.size() > resources.request_parts) return bad(reason::responses_request::request_part_count_limit_exceeded);
    if (request.previous_response_id && (message.native || message.wire_output || message.role == Role::Assistant)) return replay_bad();
    if (message.native) {
      if (message.role != Role::Assistant || !message.wire_output || !message.wire_output->root().is_array()) return replay_bad();
    } else {
      if (message.role == Role::Assistant || message.wire_output) return replay_bad();
      for (const auto& part : message.parts) {
        if (!std::holds_alternative<Text>(part) && !std::holds_alternative<ToolResult>(part) &&
            !std::holds_alternative<Media>(part)) return replay_bad();
      }
    }
    captured_history = captured_history || static_cast<bool>(message.native);
    for (const auto& part : message.parts) {
      if (const auto* media = std::get_if<Media>(&part); media && !media_error && !message.native) {
        if (message.role != Role::User) media_error = reason::error(ErrorKind::InvalidRequest, reason::responses_request::media_inputs_require_user_role);
        else media_error = admit(descriptor, *media);
      } else if (const auto* tool = std::get_if<ToolResult>(&part); tool && !media_error) {
        // Content beside media-only parts is a computer call result's acknowledgement; the call's kind is checked
        // when the result is encoded.
        if (!tool->content_parts.empty() && !tool->content.empty() &&
            std::any_of(tool->content_parts.begin(), tool->content_parts.end(), [](const auto& nested) { return std::holds_alternative<Text>(nested); }))
          media_error = reason::error(ErrorKind::InvalidRequest, reason::responses_request::tool_result_content_and_content_parts_are);
        else if (tool->content_parts.size() > resources.request_parts)
          media_error = reason::error(ErrorKind::InvalidRequest, reason::responses_request::tool_result_part_count_limit_exceeded);
        else if (std::any_of(tool->content_parts.begin(), tool->content_parts.end(), [](const auto& nested) { return std::holds_alternative<Opaque>(nested); }))
          media_error = reason::error(ErrorKind::Unsupported, reason::responses_request::tool_result_blocks_not_admitted);
        else for (const auto& nested : tool->content_parts) {
          if (const auto* media = std::get_if<Media>(&nested)) {
            if ((media->kind == MediaKind::Audio || media->kind == MediaKind::Video) &&
                static_cast<unsigned>(media->source) <= 2)
              media_error = reason::error(ErrorKind::Unsupported, reason::responses_request::responses_function_output_supports_images);
            else media_error = admit(descriptor, *media);
            if (!media_error && (!media->id.empty() || !media->transcript.empty() ||
                (media->kind == MediaKind::Image && !media->name.empty()) ||
                (media->kind == MediaKind::Document && media->detail != ImageDetail::Auto) ||
                (media->kind == MediaKind::Document && media->source != MediaSource::Inline && !media->name.empty())))
              media_error = reason::error(ErrorKind::Unsupported, reason::responses_request::function_output_media_contains_unsupported);
            if (media_error) break;
          }
        }
      }
    }
  }
  // Preserve native-prefix mismatch precedence over payload errors in edited
  // captured history, but reject invalid ordinary input before hashing.
  if (media_error && !captured_history) return *media_error;
  if (auto refusal = request_controls::validate_extensions(descriptor, request.extensions))
    return reason::error(refusal->admitted ? ErrorKind::InvalidRequest : ErrorKind::Unsupported, refusal->cause);
  auto context = std::shared_ptr<const NativeContext>(new NativeContext(descriptor, request, effective, streaming));
  if (!context->history_valid_) return replay_bad();
  if (!context->valid_) return reason::error(ErrorKind::ResourceLimit, reason::responses_request::native_binding_could_not_be_captured);
  if (media_error) return *media_error;
  EncodedRequest result{"POST", descriptor.resolved_path(streaming, request.model), {}, {}, {}};
  for (const auto& header : descriptor.headers()) {
    const auto key = lower(header.first);
    if (key != "content-type" && key != "accept") result.headers.push_back(header);
  }
  result.headers.emplace_back("Content-Type", "application/json");
  result.headers.emplace_back("Accept", streaming ? "text/event-stream" : "application/json");
  // Over the size limit, a request with media is a resource limit; one of text alone is invalid.
  const bool has_media = media_input::carries_media(request.messages);
  const auto limit_error = [&] {
    return reason::error(has_media ? ErrorKind::ResourceLimit : ErrorKind::InvalidRequest, reason::responses_request::request_exceeds_json_byte_depth_or_encoding);
  };
  auto build = [&](json::BoundedWriter& body) -> std::optional<Error> {
    auto invalid = [](const reason::Reason& message) { return reason::error(ErrorKind::InvalidRequest, message); };
    body.raw("{").quoted(descriptor.request_model_member()).raw(":").quoted(request.model);
    body.raw(",").quoted(descriptor.request_stream_member()).raw(streaming ? ":true" : ":false");
    body.raw(",\"store\":").raw(request.store.value_or(false) ? "true" : "false");
    if (request.previous_response_id) body.raw(",\"previous_response_id\":").quoted(*request.previous_response_id);
    if (request.parallel_tool_calls) body.raw(",\"parallel_tool_calls\":").raw(*request.parallel_tool_calls ? "true" : "false");
    if (request.truncation) body.raw(",\"truncation\":").quoted(truncation_name(*request.truncation));
    body.raw(",\"include\":[");
    bool include_comma = false;
    auto include_one = [&](std::string_view name) { if (include_comma) body.raw(","); include_comma = true; body.quoted(name); };
    if (request.include) for (const auto selection : *request.include) include_one(include_name(selection));
    else include_one("reasoning.encrypted_content");
    for (const auto& name : request.include_names) include_one(name);
    body.raw("]");
    auto number = [&](std::string_view key, double value) {
      char bytes[64];
      const auto converted = std::to_chars(bytes, bytes + sizeof bytes, value);
      body.raw(",").quoted(key).raw(":").raw({bytes, static_cast<size_t>(converted.ptr - bytes)});
    };
    if (effective.temperature) number("temperature", *effective.temperature);
    if (effective.top_p) number("top_p", *effective.top_p);
    if (request.provider) {
      body.raw(",\"provider\":");
      request_controls::write_routing(body, *request.provider);
    }
    if (request.response_format || request.verbosity) {
      body.raw(",\"text\":{");
      if (request.response_format) {
        body.raw("\"format\":");
        request_controls::write_response_format(body, *request.response_format, true);
      }
      if (request.verbosity) {
        if (request.response_format) body.raw(",");
        body.raw("\"verbosity\":").quoted(verbosity_name(*request.verbosity));
      }
      body.raw("}");
    }
    if (!request.instructions.empty()) body.raw(",\"instructions\":").quoted(request.instructions);
    if (effective.max_output_tokens) body.raw(",").quoted(descriptor.max_output_tokens_member()).raw(":").raw(std::to_string(*effective.max_output_tokens));
    if (request.max_tool_calls) body.raw(",\"max_tool_calls\":").raw(std::to_string(*request.max_tool_calls));
    if (effective.reasoning_enabled) body.raw(",\"reasoning\":{\"effort\":").quoted(*effective.reasoning_effort)
        .raw(",\"summary\":").quoted(*effective.reasoning_summary).raw("}");
    if (effective.service_tier) body.raw(",\"service_tier\":").quoted(*effective.service_tier);
    std::set<std::string_view> tool_names;
    if (!request.tools.empty() || !request.hosted_tools.empty()) {
      body.raw(",\"tools\":[");
      bool comma = false;
      for (const auto& tool : request.tools) {
        if (!function_name(tool.name) || !tool_names.insert(tool.name).second) return invalid(reason::responses_request::tools_require_unique_valid_function_names);
        if (!tool.parameters || !tool.parameters->root().is_object()) return invalid(reason::responses_request::function_parameters_must_be_an_object_schema);
        // A policy strict default applies only to strict-compatible schemas and is
        // otherwise sent as false. An explicit strict request on an incompatible
        // schema fails here, naming the schema location, instead of as a vendor 400.
        auto strict = tool.strict ? tool.strict : defaults.strict_tools;
        if (strict && *strict) {
          if (const auto violation = request_controls::strict_schema_violation(tool.parameters->root())) {
            // A text composed from the function name and the schema location has no code, as before.
            if (tool.strict) return Error{ErrorKind::InvalidRequest, "strict function " + json::quote(tool.name) + " parameters are not strict-compatible at " +
                                          json::quote(violation->pointer) + ": " + std::string(violation->reason)};
            strict = false;
          }
        }
        if (comma) body.raw(",");
        comma = true;
        body.raw("{\"type\":\"function\",\"name\":").quoted(tool.name).raw(",\"description\":").quoted(tool.description)
            .raw(",\"parameters\":").value(tool.parameters->root(), 3);
        if (strict) body.raw(",\"strict\":").raw(*strict ? "true" : "false");
        if (tool.defer_loading) body.raw(",\"defer_loading\":").raw(*tool.defer_loading ? "true" : "false");
        body.raw("}");
      }
      for (const auto& tool : request.hosted_tools) {
        if (comma) body.raw(",");
        comma = true;
        body.raw("{\"type\":").quoted(tool_type(tool));
        auto text = [&](std::string_view key, const std::optional<std::string>& value) { if (value) body.raw(",").quoted(key).raw(":").quoted(*value); };
        auto flag = [&](std::string_view key, const std::optional<bool>& value) { if (value) body.raw(",").quoted(key).raw(*value ? ":true" : ":false"); };
        auto count = [&](std::string_view key, const std::optional<std::uint32_t>& value) { if (value) body.raw(",").quoted(key).raw(":").raw(std::to_string(*value)); };
        auto strings = [&](const std::vector<std::string>& values) {
          body.raw("[");
          for (std::size_t i = 0; i < values.size(); ++i) { if (i) body.raw(","); body.quoted(values[i]); }
          body.raw("]");
        };
        auto filter = [&](const McpToolFilter& value) {
          body.raw("{");
          bool filter_comma = false;
          if (!value.tool_names.empty()) { body.raw("\"tool_names\":"); strings(value.tool_names); filter_comma = true; }
          if (value.read_only) body.raw(filter_comma ? ",\"read_only\":" : "\"read_only\":").raw(*value.read_only ? "true" : "false");
          body.raw("}");
        };
        std::visit([&](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, ImageGenerationTool>) {
            text("size", value.size); text("quality", value.quality); text("output_format", value.output_format);
            text("model", value.model); text("action", value.action); text("background", value.background);
            text("moderation", value.moderation); text("input_fidelity", value.input_fidelity);
            count("output_compression", value.output_compression); count("partial_images", value.partial_images);
            if (value.mask_file_id || value.mask_image_url) {
              body.raw(",\"input_image_mask\":{");
              if (value.mask_file_id) body.raw("\"file_id\":").quoted(*value.mask_file_id);
              if (value.mask_image_url) body.raw(value.mask_file_id ? ",\"image_url\":" : "\"image_url\":").quoted(*value.mask_image_url);
              body.raw("}");
            }
          } else if constexpr (std::is_same_v<T, FileSearchTool>) {
            body.raw(",\"vector_store_ids\":");
            strings(value.vector_store_ids);
            count("max_num_results", value.max_num_results);
          } else if constexpr (std::is_same_v<T, WebSearchTool>) {
            if (!value.allowed_domains.empty()) { body.raw(",\"filters\":{\"allowed_domains\":"); strings(value.allowed_domains); body.raw("}"); }
            if (value.user_location) {
              const auto& at = *value.user_location;
              body.raw(",\"user_location\":{\"type\":\"approximate\"");
              text("city", at.city); text("country", at.country); text("region", at.region); text("timezone", at.timezone);
              body.raw("}");
            }
            text("search_context_size", value.search_context_size);
            flag("external_web_access", value.external_web_access);
          } else if constexpr (std::is_same_v<T, CodeInterpreterTool>) {
            if (value.container_id) body.raw(",\"container\":").quoted(*value.container_id);
            else {
              body.raw(",\"container\":{\"type\":\"auto\"");
              if (!value.file_ids.empty()) { body.raw(",\"file_ids\":"); strings(value.file_ids); }
              text("memory_limit", value.memory_limit);
              if (value.network_policy) {
                const auto& policy = *value.network_policy;
                if (policy.disabled) body.raw(",\"network_policy\":{\"type\":\"disabled\"}");
                else {
                  body.raw(",\"network_policy\":{\"type\":\"allowlist\",\"allowed_domains\":");
                  strings(policy.allowed_domains);
                  if (!policy.domain_secrets.empty()) {
                    body.raw(",\"domain_secrets\":[");
                    for (std::size_t i = 0; i < policy.domain_secrets.size(); ++i) {
                      const auto& secret = policy.domain_secrets[i];
                      if (i) body.raw(",");
                      body.raw("{\"domain\":").quoted(secret.domain).raw(",\"name\":").quoted(secret.name).raw(",\"value\":").quoted(secret.value).raw("}");
                    }
                    body.raw("]");
                  }
                  body.raw("}");
                }
              }
              body.raw("}");
            }
          } else if constexpr (std::is_same_v<T, McpTool>) {
            body.raw(",\"server_label\":").quoted(value.server_label);
            text("server_url", value.server_url); text("connector_id", value.connector_id); text("tunnel_id", value.tunnel_id);
            text("authorization", value.authorization);
            if (!value.headers.empty()) {
              body.raw(",\"headers\":{");
              for (std::size_t i = 0; i < value.headers.size(); ++i) { if (i) body.raw(","); body.quoted(value.headers[i].first).raw(":").quoted(value.headers[i].second); }
              body.raw("}");
            }
            if (value.allowed_tools) { body.raw(",\"allowed_tools\":"); filter(*value.allowed_tools); }
            if (value.require_approval) body.raw(",\"require_approval\":").quoted(*value.require_approval);
            else if (value.approval_always || value.approval_never) {
              body.raw(",\"require_approval\":{");
              if (value.approval_always) { body.raw("\"always\":"); filter(*value.approval_always); }
              if (value.approval_never) { if (value.approval_always) body.raw(","); body.raw("\"never\":"); filter(*value.approval_never); }
              body.raw("}");
            }
            text("server_description", value.server_description);
            flag("defer_loading", value.defer_loading);
          } else if constexpr (std::is_same_v<T, ComputerUsePreviewTool>) {
            body.raw(",\"environment\":").quoted(value.environment).raw(",\"display_width\":").raw(std::to_string(value.display_width))
                .raw(",\"display_height\":").raw(std::to_string(value.display_height));
          } else if constexpr (std::is_same_v<T, OpenRouterServerTool>) {
            if (value.parameters) body.raw(",\"parameters\":").value(value.parameters->root(), 3);
          } else if constexpr (std::is_same_v<T, GenericHostedTool>) {
            if (value.options) for (auto member : value.options->root().members()) body.raw(",").quoted(member.key).raw(":").value(member.value, 3);

          } else if constexpr (std::is_same_v<T, ShellTool>) {
            body.raw(",\"environment\":{\"type\":\"container_auto\"");
            if (!value.environment.skills.empty()) {
              body.raw(",\"skills\":[");
              bool skill_comma = false;
              for (const auto& skill : value.environment.skills) {
                if (skill_comma) body.raw(",");
                skill_comma = true;
                body.raw("{\"type\":\"skill_reference\",\"skill_id\":").quoted(skill.skill_id).raw("}");
              }
              body.raw("]");
            }
            body.raw("}");
          }
        }, tool);
        body.raw("}");
      }
      body.raw("]");
    }
    if (!request.modalities.empty()) {
      body.raw(",\"modalities\":[");
      for (std::size_t i = 0; i < request.modalities.size(); ++i) {
        if (i) body.raw(",");
        body.quoted(request.modalities[i] == OutputModality::Image ? "image" : "text");
      }
      body.raw("]");
    }
    if (request.image_config) {
      body.raw(",\"image_config\":");
      request_controls::write_image_config(body, *request.image_config);
    }
    if (!request.plugins.empty()) { body.raw(",\"plugins\":"); request_controls::write_plugins(body, request.plugins); }
    if (request.required_tool) {
      if (!tool_names.contains(*request.required_tool)) return invalid(reason::responses_request::required_tool_must_name_a_declared_client);
      body.raw(",\"tool_choice\":{\"type\":\"function\",\"name\":").quoted(*request.required_tool).raw("}");
    } else if (request.tool_choice) {
      body.raw(",\"tool_choice\":");
      write_tool_choice(body, *request.tool_choice, request.hosted_tools);
    }
    body.raw(",").quoted(descriptor.request_messages_member()).raw(":[");
    std::set<std::string_view> item_ids, call_ids, pending, computer_pending, approval_requests;
    const bool computer = hosted_types.contains("computer") || hosted_types.contains("computer_use_preview");
    bool input_comma = false;
    auto separator = [&] { if (input_comma) body.raw(","); input_comma = true; };
    auto captured = [&](const Message& message, bool emit_items) -> std::optional<Error> {
        if (!pending.empty()) return invalid(reason::responses_request::client_tool_results_must_precede_the_next);
        std::map<std::string_view, const ToolCall*> calls;
        for (const auto& part : message.parts) {
          if (const auto* call = std::get_if<ToolCall>(&part); call && call->kind == ToolCallKind::ApprovalRequest && call->wire_type == "mcp_approval_request") {
            // Answered by Request::mcp_approvals, not by a tool result.
            approval_requests.insert(call->id);
          } else if (call) {
            const bool action = call->wire_type == "computer_call";
            if (call->kind != ToolCallKind::ClientExecuted || (call->wire_type != "function_call" && !action)) return reason::error(ErrorKind::Unsupported, reason::responses_request::only_client_function_calls_authorize_tool);
            if (action && !computer) return invalid(reason::responses_request::captured_computer_call_requires_a_declared);
            if (call->id.empty() || !(action || tool_names.contains(call->name)) || !call->input || !call->input->root().is_object() ||
                !call_ids.insert(call->id).second || !calls.emplace(call->id, call).second) return invalid(reason::responses_request::captured_client_call_has_invalid_or);
            pending.insert(call->id);
            if (action) computer_pending.insert(call->id);
          } else if (!std::holds_alternative<Text>(part) && !std::holds_alternative<Refusal>(part) &&
              !std::holds_alternative<Reasoning>(part) && !std::holds_alternative<Opaque>(part) &&
              !std::holds_alternative<Media>(part)) {
            return reason::error(ErrorKind::Unsupported, reason::responses_request::foreign_or_invalid_part_cannot_be_replayed);
          }
        }
        // Keep the entire generation in its original order, including opaque
        // server items, reasoning, function item id/call_id and assistant phase.
        for (const auto item : message.wire_output->root().elements()) {
          if (!item.is_object() || !item.get("type").is_string()) return invalid(reason::responses_request::captured_output_item_must_have_a_type);
          const auto id = item.get("id");
          if (id.valid() && (!id.is_string() || id.as_string().empty() || !item_ids.insert(id.as_string()).second)) return invalid(reason::responses_request::captured_output_item_ids_must_be_unique_and);
          if (item.get("type").as_string() == "function_call") {
            const auto call_id = item.get("call_id"), name = item.get("name"), arguments = item.get("arguments");
            if (!id.is_string() || !call_id.is_string() || !name.is_string() || !arguments.is_string()) return invalid(reason::responses_request::captured_function_item_lacks_its_original);
            const auto found = calls.find(call_id.as_string());
            if (found == calls.end() || found->second->name != name.as_string()) return invalid(reason::responses_request::captured_function_item_lacks_unique_client);
            calls.erase(found);
          } else if (item.get("type").as_string() == "computer_call") {
            const auto call_id = item.get("call_id");
            const auto found = call_id.is_string() ? calls.find(call_id.as_string()) : calls.end();
            if (!id.is_string() || found == calls.end() || found->second->wire_type != "computer_call") return invalid(reason::responses_request::captured_function_item_lacks_unique_client);
            calls.erase(found);
          } else if (item.get("type").as_string() == "mcp_approval_request" && id.is_string()) {
            approval_requests.insert(id.as_string());
          }
          if (emit_items) { separator(); body.value(item, 2); }
        }
        if (!calls.empty()) return invalid(reason::responses_request::captured_function_call_is_missing_its_output);
        return std::nullopt;
    };
    if (!request.previous_response_history.empty())
      if (auto error = captured(request.previous_response_history.back(), false)) return error;
    for (const auto& message : request.messages) {
      if (message.native) {
        if (auto error = captured(message, true)) return error;
      } else {
        if (message.parts.empty()) return invalid(reason::responses_request::plain_input_messages_require_content);
        const auto role = role_name(message.role);
        const bool results = std::holds_alternative<ToolResult>(message.parts.front());
        if (results) {
          if (message.role != Role::Tool && message.role != Role::User) return invalid(reason::responses_request::tool_results_require_tool_or_user_role);
          for (const auto& part : message.parts) {
            const auto* tool_result = std::get_if<ToolResult>(&part);
            if (!tool_result || tool_result->tool_use_id.empty() || pending.erase(tool_result->tool_use_id) != 1) return invalid(reason::responses_request::tool_result_lacks_exactly_one_pending_client);
            if (tool_result->is_error) return reason::error(ErrorKind::Unsupported, reason::responses_request::responses_tool_results_have_no_typed_is);
            if (computer_pending.erase(tool_result->tool_use_id)) {
              // A computer action answers with one screenshot, and optionally the acknowledged safety checks.
              if (tool_result->content_parts.size() != 1 || !std::holds_alternative<Media>(tool_result->content_parts.front()) ||
                  std::get<Media>(tool_result->content_parts.front()).kind != MediaKind::Image)
                return invalid(reason::responses_request::a_computer_call_result_is_one_screenshot_image);
              std::optional<json::Document> extra;
              if (!tool_result->content.empty()) {
                auto parsed = json::parse(tool_result->content, {resources.request_bytes, resources.json_depth});
                auto* doc = std::get_if<json::Document>(&parsed);
                if (!doc || !doc->root().is_object() || doc->root().size() != 1 || !doc->root().get("acknowledged_safety_checks").is_array())
                  return invalid(reason::responses_request::a_computer_call_result_s_content_may_only);
                extra = std::move(*doc);
              }
              const auto& shot = std::get<Media>(tool_result->content_parts.front());
              separator(); body.raw("{\"type\":\"computer_call_output\",\"call_id\":").quoted(tool_result->tool_use_id)
                  .raw(",\"output\":{\"type\":\"computer_screenshot\",");
              if (shot.source == MediaSource::File) body.raw("\"file_id\":").quoted(shot.reference);
              else if (shot.source == MediaSource::Url) body.raw("\"image_url\":").quoted(shot.reference);
              else { body.raw("\"image_url\":"); media_input::data_uri(body, shot); }
              body.raw("}");
              if (extra) body.raw(",\"acknowledged_safety_checks\":").value(extra->root().get("acknowledged_safety_checks"), 3);
              body.raw("}");
              continue;
            }
            if (!tool_result->content_parts.empty() && !tool_result->content.empty())
              return invalid(reason::responses_request::tool_result_content_and_content_parts_are);
            separator(); body.raw("{\"type\":\"function_call_output\",\"call_id\":").quoted(tool_result->tool_use_id).raw(",\"output\":");
            if (tool_result->content_parts.empty()) body.quoted(tool_result->content);
            else {
              body.raw("["); bool nested_comma = false;
              for (const auto& nested : tool_result->content_parts) {
                if (nested_comma) body.raw(",");
                nested_comma = true;
                if (const auto* text = std::get_if<Text>(&nested))
                  body.raw("{\"type\":\"input_text\",\"text\":").quoted(text->value).raw("}");
                else write_media(body, std::get<Media>(nested));
                if (!body.ok()) return limit_error();
              }
              body.raw("]");
            }
            body.raw("}");
          }
        } else {
          if (role.empty() || !pending.empty()) return invalid(reason::responses_request::plain_input_role_or_pending_client_tool);
          separator(); body.raw("{\"role\":").quoted(role).raw(",\"content\":[");
          bool comma = false;
          for (const auto& part : message.parts) {
            if (comma) body.raw(",");
            comma = true;
            if (const auto* text = std::get_if<Text>(&part)) {
              body.raw("{\"type\":\"input_text\",\"text\":").quoted(text->value).raw("}");
            } else if (const auto* media = std::get_if<Media>(&part)) {
              write_media(body, *media);
            } else return reason::error(ErrorKind::Unsupported, reason::responses_request::plain_responses_input_supports_typed_text);
            if (!body.ok()) return limit_error();
          }
          body.raw("]}");
        }
      }
      if (!body.ok()) return limit_error();
    }
    if (!pending.empty()) return invalid(reason::responses_request::all_client_function_calls_require_results);
    for (const auto& approval : request.mcp_approvals) {
      if (!request.previous_response_id && !approval_requests.contains(approval.request_id))
        return invalid(reason::responses_request::an_mcp_approval_answers_a_captured_approval);
      separator(); body.raw("{\"type\":\"mcp_approval_response\",\"approval_request_id\":").quoted(approval.request_id)
          .raw(",\"approve\":").raw(approval.approve ? "true" : "false");
      if (approval.reason) body.raw(",\"reason\":").quoted(*approval.reason);
      body.raw("}");
    }
    body.raw("]");
    request_controls::write_extensions(body, request.extensions);
    body.raw("}");
    if (!body.ok()) return limit_error();
    return std::nullopt;
  };
  json::BoundedWriter measured({resources.request_bytes, resources.json_depth});
  if (auto error = build(measured)) return *error;
  result.body.reserve(measured.size());
  json::BoundedWriter body({measured.size(), resources.json_depth}, &result.body);
  if (auto error = build(body)) return *error;
  result.context = std::move(context);
  result.max_output_tokens = effective.max_output_tokens;
  result.cache_marks = mark_report;
  if (!request.hosted_tools.empty()) {
    if (!request.max_tool_calls || *request.max_tool_calls == std::numeric_limits<std::uint64_t>::max())
      result.model_invocation_limit.reset();
    else result.model_invocation_limit = *request.max_tool_calls + 1;
  }
  return result;
}
} // namespace sp::responses
