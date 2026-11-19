#pragma once
// The one table of the top-level request members each encoder owns. A chat family's encoder owns what it writes, the
// other spellings of a member it refuses, provider members the SDK does not model, and provider members a policy
// extension may still admit; an endpoint's encoder owns the members of its typed request. Every guard that keeps a
// caller or a descriptor off these members reads this table: the policy loader (an extension, an endpoint option, a
// server tool option), the encoders (an extension in a request) and the descriptor loader (a binding).
// tests/extension_test.cpp and tests/endpoints_test.cpp check the table against what the encoders write.
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>

namespace sp::descriptor {
// Why a member is in the table.
// - Written: the encoder writes it.
// - Alias: another spelling of a member the encoder writes, or a member of another family's request, which the encoder
//   refuses.
// - Held: a provider member the SDK does not model and the encoder never writes (provider-held state, a streaming
//   switch the codec chooses itself).
// - Reserved: a provider member a descriptor binding may not take but a policy extension may admit.
enum class MemberRole : std::uint8_t { Written, Alias, Held, Reserved };
struct OwnedMember {
  std::string_view name;
  MemberRole role;
};

namespace owned_table {
inline constexpr OwnedMember k_chat[] = {
    {"model", MemberRole::Written}, {"messages", MemberRole::Written}, {"stream", MemberRole::Written},
    {"stream_options", MemberRole::Written}, {"max_tokens", MemberRole::Written},
    {"max_completion_tokens", MemberRole::Written}, {"temperature", MemberRole::Written},
    {"top_p", MemberRole::Written}, {"tools", MemberRole::Written}, {"tool_choice", MemberRole::Written},
    {"provider", MemberRole::Written}, {"reasoning_effort", MemberRole::Written},
    {"service_tier", MemberRole::Written}, {"response_format", MemberRole::Written},
    {"modalities", MemberRole::Written}, {"audio", MemberRole::Written}, {"image_config", MemberRole::Written},
    {"reasoning", MemberRole::Written}, {"include_reasoning", MemberRole::Written}, {"usage", MemberRole::Written},
    {"models", MemberRole::Written}, {"plugins", MemberRole::Written}, {"input", MemberRole::Alias},
    {"contents", MemberRole::Alias}, {"max_output_tokens", MemberRole::Alias}, {"extensions", MemberRole::Alias},
    {"functions", MemberRole::Alias}, {"function_call", MemberRole::Alias}, {"n", MemberRole::Reserved},
    {"stop", MemberRole::Reserved},
};

inline constexpr OwnedMember k_messages[] = {
    {"model", MemberRole::Written}, {"messages", MemberRole::Written}, {"stream", MemberRole::Written},
    {"max_tokens", MemberRole::Written}, {"temperature", MemberRole::Written}, {"top_p", MemberRole::Written},
    {"tools", MemberRole::Written}, {"tool_choice", MemberRole::Written}, {"provider", MemberRole::Written},
    {"system", MemberRole::Written}, {"thinking", MemberRole::Written}, {"output_config", MemberRole::Written},
    {"cache_control", MemberRole::Written}, {"container", MemberRole::Written}, {"mcp_servers", MemberRole::Written},
    {"input", MemberRole::Alias}, {"contents", MemberRole::Alias}, {"stream_options", MemberRole::Alias},
    {"max_output_tokens", MemberRole::Alias}, {"max_completion_tokens", MemberRole::Alias},
    {"extensions", MemberRole::Alias}, {"metadata", MemberRole::Held}, {"n", MemberRole::Reserved},
    {"response_format", MemberRole::Reserved}, {"stop", MemberRole::Reserved},
    {"stop_sequences", MemberRole::Reserved}, {"top_k", MemberRole::Reserved},
    {"service_tier", MemberRole::Reserved}, {"context_management", MemberRole::Reserved},
};

inline constexpr OwnedMember k_responses[] = {
    {"model", MemberRole::Written}, {"input", MemberRole::Written}, {"stream", MemberRole::Written},
    {"max_output_tokens", MemberRole::Written}, {"temperature", MemberRole::Written}, {"top_p", MemberRole::Written},
    {"tools", MemberRole::Written}, {"tool_choice", MemberRole::Written}, {"provider", MemberRole::Written},
    {"store", MemberRole::Written}, {"previous_response_id", MemberRole::Written}, {"include", MemberRole::Written},
    {"instructions", MemberRole::Written}, {"max_tool_calls", MemberRole::Written},
    {"reasoning", MemberRole::Written}, {"service_tier", MemberRole::Written}, {"text", MemberRole::Written},
    {"parallel_tool_calls", MemberRole::Written}, {"truncation", MemberRole::Written},
    {"modalities", MemberRole::Written}, {"image_config", MemberRole::Written}, {"plugins", MemberRole::Written},
    {"messages", MemberRole::Alias}, {"contents", MemberRole::Alias}, {"stream_options", MemberRole::Alias},
    {"max_tokens", MemberRole::Alias}, {"max_completion_tokens", MemberRole::Alias},
    {"extensions", MemberRole::Alias}, {"conversation", MemberRole::Held}, {"background", MemberRole::Held},
    {"prompt", MemberRole::Held}, {"n", MemberRole::Reserved}, {"response_format", MemberRole::Reserved},
    {"stop", MemberRole::Reserved}, {"metadata", MemberRole::Reserved}, {"prompt_cache_key", MemberRole::Reserved},
    {"context_management", MemberRole::Reserved}, {"access_programs", MemberRole::Reserved},
};

inline constexpr OwnedMember k_generate[] = {
    {"contents", MemberRole::Written}, {"tools", MemberRole::Written}, {"generationConfig", MemberRole::Written},
    {"safetySettings", MemberRole::Written}, {"systemInstruction", MemberRole::Written},
    {"toolConfig", MemberRole::Written}, {"model", MemberRole::Alias}, {"messages", MemberRole::Alias},
    {"input", MemberRole::Alias}, {"stream", MemberRole::Alias}, {"stream_options", MemberRole::Alias},
    {"max_tokens", MemberRole::Alias}, {"max_output_tokens", MemberRole::Alias},
    {"max_completion_tokens", MemberRole::Alias}, {"temperature", MemberRole::Alias}, {"top_p", MemberRole::Alias},
    {"tool_choice", MemberRole::Alias}, {"provider", MemberRole::Alias}, {"extensions", MemberRole::Alias},
    {"generation_config", MemberRole::Alias}, {"safety_settings", MemberRole::Alias},
    {"system_instruction", MemberRole::Alias}, {"tool_config", MemberRole::Alias},
    {"cached_content", MemberRole::Alias}, {"cachedContent", MemberRole::Held}, {"n", MemberRole::Reserved},
    {"response_format", MemberRole::Reserved}, {"stop", MemberRole::Reserved}, {"serviceTier", MemberRole::Reserved},
    {"store", MemberRole::Reserved}, {"service_tier", MemberRole::Reserved},
    {"previous_interaction_id", MemberRole::Reserved}, {"background", MemberRole::Reserved},
    {"agent", MemberRole::Reserved}, {"environment", MemberRole::Reserved},
};

inline constexpr OwnedMember k_interactions[] = {
    {"model", MemberRole::Written}, {"input", MemberRole::Written}, {"stream", MemberRole::Written},
    {"tools", MemberRole::Written}, {"service_tier", MemberRole::Written}, {"agent", MemberRole::Written},
    {"store", MemberRole::Written}, {"background", MemberRole::Written},
    {"previous_interaction_id", MemberRole::Written}, {"generation_config", MemberRole::Written},
    {"system_instruction", MemberRole::Written}, {"response_format", MemberRole::Written},
    {"messages", MemberRole::Alias}, {"contents", MemberRole::Alias}, {"stream_options", MemberRole::Alias},
    {"max_tokens", MemberRole::Alias}, {"max_output_tokens", MemberRole::Alias},
    {"max_completion_tokens", MemberRole::Alias}, {"temperature", MemberRole::Alias}, {"top_p", MemberRole::Alias},
    {"tool_choice", MemberRole::Alias}, {"provider", MemberRole::Alias}, {"extensions", MemberRole::Alias},
    {"response_modalities", MemberRole::Held}, {"agent_config", MemberRole::Held}, {"n", MemberRole::Reserved},
    {"stop", MemberRole::Reserved}, {"systemInstruction", MemberRole::Reserved},
    {"generationConfig", MemberRole::Reserved}, {"toolConfig", MemberRole::Reserved},
    {"cachedContent", MemberRole::Reserved}, {"serviceTier", MemberRole::Reserved},
    {"environment", MemberRole::Reserved}, {"safetySettings", MemberRole::Reserved},
    {"safety_settings", MemberRole::Reserved},
};

inline constexpr OwnedMember k_images[] = {
    {"model", MemberRole::Written}, {"prompt", MemberRole::Written}, {"n", MemberRole::Written},
    {"size", MemberRole::Written}, {"quality", MemberRole::Written}, {"background", MemberRole::Written},
    {"output_format", MemberRole::Written}, {"output_compression", MemberRole::Written},
    {"images", MemberRole::Written}, {"input_references", MemberRole::Written}, {"stream", MemberRole::Held},
    {"partial_images", MemberRole::Held},
};

inline constexpr OwnedMember k_decisions[] = {
    {"model", MemberRole::Written}, {"input", MemberRole::Written}, {"state", MemberRole::Written},
    {"questions", MemberRole::Written},
};

inline constexpr OwnedMember k_embeddings[] = {
    {"model", MemberRole::Written}, {"input", MemberRole::Written}, {"dimensions", MemberRole::Written},
    {"encoding_format", MemberRole::Written}, {"input_type", MemberRole::Written}, {"requests", MemberRole::Written},
};

inline constexpr OwnedMember k_rerank[] = {
    {"model", MemberRole::Written}, {"query", MemberRole::Written}, {"documents", MemberRole::Written},
    {"top_n", MemberRole::Written},
};

inline constexpr OwnedMember k_moderation[] = {
    {"model", MemberRole::Written}, {"input", MemberRole::Written},
};

inline constexpr OwnedMember k_speech[] = {
    {"model", MemberRole::Written}, {"input", MemberRole::Written}, {"voice", MemberRole::Written},
    {"instructions", MemberRole::Written}, {"response_format", MemberRole::Written}, {"speed", MemberRole::Written},
    {"stream_format", MemberRole::Held},
};

inline constexpr OwnedMember k_transcription[] = {
    {"model", MemberRole::Written}, {"file", MemberRole::Written}, {"input_audio", MemberRole::Written},
    {"language", MemberRole::Written}, {"prompt", MemberRole::Written}, {"stream", MemberRole::Held},
};

inline constexpr OwnedMember k_videos[] = {
    {"model", MemberRole::Written}, {"prompt", MemberRole::Written}, {"input_references", MemberRole::Written},
    {"frame_images", MemberRole::Held}, {"duration", MemberRole::Written}, {"aspect_ratio", MemberRole::Written},
    {"resolution", MemberRole::Written}, {"size", MemberRole::Written}, {"generate_audio", MemberRole::Written},
};

inline constexpr OwnedMember k_files[] = {
    {"file", MemberRole::Written}, {"purpose", MemberRole::Written}, {"expires_after", MemberRole::Written},
};

inline constexpr OwnedMember k_realtime[] = {
    {"model", MemberRole::Written}, {"generationConfig", MemberRole::Written},
    {"systemInstruction", MemberRole::Written}, {"tools", MemberRole::Written},
    {"inputAudioTranscription", MemberRole::Written}, {"outputAudioTranscription", MemberRole::Written},
    {"sessionResumption", MemberRole::Written}, {"type", MemberRole::Written}, {"instructions", MemberRole::Written},
    {"output_modalities", MemberRole::Written}, {"audio", MemberRole::Written},
};

inline constexpr OwnedMember k_batches[] = {
    {"input_file_id", MemberRole::Written}, {"endpoint", MemberRole::Written},
    {"completion_window", MemberRole::Written}, {"requests", MemberRole::Written}, {"batch", MemberRole::Written},
};

inline constexpr OwnedMember k_tool_messages[] = {
    {"type", MemberRole::Written}, {"name", MemberRole::Written}, {"max_uses", MemberRole::Written},
    {"input_schema", MemberRole::Written}, {"description", MemberRole::Written},
    {"allowed_callers", MemberRole::Written}, {"allowed_domains", MemberRole::Written},
    {"blocked_domains", MemberRole::Written}, {"user_location", MemberRole::Written},
    {"citations", MemberRole::Written}, {"max_content_tokens", MemberRole::Written},
};

inline constexpr OwnedMember k_tool_responses[] = {
    {"type", MemberRole::Written}, {"name", MemberRole::Written}, {"max_uses", MemberRole::Written},
    {"input_schema", MemberRole::Written}, {"description", MemberRole::Written},
};

struct Scope {
  std::string_view name;
  std::span<const OwnedMember> members;
};
// The chat families, the endpoints and, for the options of a server tool, the families that declare server tools.
inline constexpr Scope families[] = {
    {"openai.chat", k_chat},
    {"anthropic.messages", k_messages},
    {"openai.responses", k_responses},
    {"google.generate", k_generate},
    {"google.interactions", k_interactions},
};
inline constexpr Scope endpoints[] = {
    {"images", k_images},
    {"decisions", k_decisions},
    {"embeddings", k_embeddings},
    {"rerank", k_rerank},
    {"moderation", k_moderation},
    {"speech", k_speech},
    {"transcription", k_transcription},
    {"videos", k_videos},
    {"files", k_files},
    {"realtime", k_realtime},
    {"batches", k_batches},
};
inline constexpr Scope tools[] = {
    {"anthropic.messages", k_tool_messages},
    {"openai.responses", k_tool_responses},
};
inline constexpr std::string_view family_names[] = {"openai.chat", "anthropic.messages", "openai.responses", "google.generate", "google.interactions"};
// A slot that names a member beside the one its policy binds it to.
struct SlotAlternate {
  std::string_view family, slot, member;
};
inline constexpr SlotAlternate alternates[] = {
    {"openai.chat", "max_output_tokens", "max_completion_tokens"},
};
template <std::size_t N>
constexpr std::span<const OwnedMember> find(const Scope (&scopes)[N], std::string_view name) noexcept {
  for (const auto& scope : scopes)
    if (scope.name == name) return scope.members;
  return {};
}
} // namespace owned_table

// The members the encoder of a chat family (`openai.chat`, `anthropic.messages`, `openai.responses`, `google.generate`,
// `google.interactions`) or of an endpoint (`images`, `decisions`, `embeddings`, `rerank`, `moderation`, `speech`,
// `transcription`, `videos`, `files`, `realtime`, `batches`) owns, with their roles; empty for a name the table does not
// know.
constexpr std::span<const OwnedMember> owned_members(std::string_view family_or_endpoint) noexcept {
  const auto members = owned_table::find(owned_table::families, family_or_endpoint);
  return members.empty() ? owned_table::find(owned_table::endpoints, family_or_endpoint) : members;
}
// The members of a server tool entry that its options may not name, for a family that declares server tools; empty
// for any other.
constexpr std::span<const OwnedMember> owned_tool_members(std::string_view family) noexcept {
  return owned_table::find(owned_table::tools, family);
}
// The chat families the table covers.
constexpr std::span<const std::string_view> owned_member_families() noexcept { return owned_table::family_names; }
// The role of `name` in `members`, in exactly that spelling; nullopt for a name the list does not hold.
constexpr std::optional<MemberRole> role_of(std::span<const OwnedMember> members, std::string_view name) noexcept {
  for (const auto& member : members)
    if (member.name == name) return member.role;
  return std::nullopt;
}
constexpr std::optional<MemberRole> member_role(std::string_view family_or_endpoint, std::string_view name) noexcept {
  return role_of(owned_members(family_or_endpoint), name);
}
// The member a descriptor binding slot of `family` may name besides the member the policy binds it to; empty for none.
constexpr std::string_view slot_alternate(std::string_view family, std::string_view slot) noexcept {
  for (const auto& alternate : owned_table::alternates)
    if (alternate.family == family && alternate.slot == slot) return alternate.member;
  return {};
}

// The members that policy extensions may not name (#32): every member of a chat family's table but the reserved ones, in
// any letter case. A family the table does not know owns every member.
inline bool request_key_owned(std::string_view family, std::string_view key) noexcept {
  const auto members = owned_table::find(owned_table::families, family);
  if (members.empty()) return true;
  auto lower_equal = [](std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
      return (x >= 'A' && x <= 'Z' ? static_cast<char>(x + 32) : x) == (y >= 'A' && y <= 'Z' ? static_cast<char>(y + 32) : y);
    });
  };
  return std::any_of(members.begin(), members.end(), [&](const OwnedMember& member) {
    return member.role != MemberRole::Reserved && lower_equal(member.name, key);
  });
}
} // namespace sp::descriptor
