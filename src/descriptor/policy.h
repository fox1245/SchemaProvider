#pragma once
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <array>
#include <memory>
#include <optional>

namespace sp::descriptor {
// Decoded-byte ceilings per media kind are the *_decoded_bytes members, in sp::MediaKind order
// (image, audio, video, document).
struct CodecResources {
  std::size_t json_bytes, json_depth, request_bytes, chat_text_request_bytes;
  std::size_t request_messages, request_tools, request_parts;
  std::size_t native_bytes, native_depth, native_members, image_decoded_bytes;
  std::size_t audio_decoded_bytes, video_decoded_bytes, document_decoded_bytes;
  std::size_t descriptor_bytes, descriptor_depth, policy_bytes, policy_depth;
};
// Admitted media input for one kind. `mime` holds lowercase `type/subtype` essences; `sources` is
// a bit set indexed by sp::MediaSource (inline = 1, url = 2, file = 4). A kind without sources is
// not accepted. Policy only narrows what a family's encoder can express; it never adds an encoding.
struct MediaKindPolicy {
  std::vector<std::string> mime;
  std::uint8_t sources = 0;
};
// Indexed by sp::MediaKind: image, audio, video, document.
struct MediaPolicy {
  std::array<MediaKindPolicy, 4> kinds;
};
struct RequestDefaults {
  std::optional<std::uint64_t> max_output_tokens, thinking_budget;
  std::optional<bool> include_thoughts, thinking_summaries, strict_tools;
  bool reasoning_enabled;
  std::optional<std::string> reasoning_effort, reasoning_summary, thinking_level, service_tier;
  std::optional<double> temperature, top_p;
};
struct ServerToolFact { std::string type, name; bool max_uses; };
struct FamilyPolicy {
  std::string family, model_member, messages_member, stream_member, cap_member;
  std::vector<std::string> usage_path;
  std::vector<std::pair<std::string, StopKind>> stops;
  RequestDefaults defaults;
  bool required_output_cap;
  std::array<double, 2> temperature_range, top_p_range;
  std::uint64_t thinking_minimum;
  double thinking_top_p_minimum;
  std::vector<std::string> reasoning_efforts, reasoning_summaries, thinking_levels, service_tiers, header_versions;
  std::optional<std::string> header_version;
  std::vector<std::string> openrouter_origins;
  std::vector<std::string> temperature_forbidden_model_prefixes;
  std::vector<ServerToolFact> server_tools;
  MediaPolicy media;
  // Replaces `media` for a base URL in openrouter_origins, whose gateway admits more content.
  std::optional<MediaPolicy> openrouter_media;
};
struct ModelDefaults {
  std::string family, model;
  RequestDefaults defaults;
  std::optional<std::uint64_t> output_limit, input_limit;
};
class DescriptorPolicy;
using PolicySnapshot = std::shared_ptr<const DescriptorPolicy>;
using PolicyResult = std::variant<PolicySnapshot, ConfigError>;
class DescriptorPolicy final {
 public:
  const CodecResources& resources() const noexcept { return resources_; }
  const FamilyPolicy* family(std::string_view) const noexcept;
  const RequestDefaults& defaults(std::string_view family, std::string_view model) const noexcept;
  std::optional<std::uint64_t> output_limit(std::string_view family, std::string_view model) const noexcept;
  std::optional<std::uint64_t> input_limit(std::string_view family, std::string_view model) const noexcept;
  std::string_view identity() const noexcept { return identity_; }
 private:
  DescriptorPolicy() = default;
  CodecResources resources_{};
  std::vector<FamilyPolicy> families_;
  std::vector<ModelDefaults> models_;
  std::string identity_;
  friend class PolicyLoader;
};
PolicyResult load_policy(std::string_view family_json, std::string_view resource_json);
PolicySnapshot builtin_policy();
std::uint32_t interface_revision() noexcept;
// Borrowed effective controls, resolved once before writing and native capture.
struct EffectiveChoices {
  std::optional<std::uint64_t> max_output_tokens, thinking_budget;
  std::optional<bool> include_thoughts, thinking_summaries;
  bool reasoning_enabled;
  std::optional<std::string_view> reasoning_effort, reasoning_summary, thinking_level, service_tier;
  std::optional<double> temperature, top_p;
};
EffectiveChoices effective_defaults(const ValidatedDescriptor&, std::string_view model);
std::optional<std::string_view> borrowed(const std::optional<std::string>&);
bool contains(const std::vector<std::string>&, std::string_view);
bool temperature_forbidden(const ValidatedDescriptor&, std::string_view model);
std::optional<std::string> validate_choices(const ValidatedDescriptor&, std::string_view model, const EffectiveChoices&);
// The media admission in force for this descriptor's origin.
const MediaPolicy& media_policy(const ValidatedDescriptor&) noexcept;
// True when this descriptor targets an origin its family policy declares as a routing gateway
// (openrouter_origins). Such a gateway may serve a different concrete model than the one requested.
bool routed_gateway(const ValidatedDescriptor&) noexcept;
} // namespace sp::descriptor
