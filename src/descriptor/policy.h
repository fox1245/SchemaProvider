#pragma once
#include "descriptor/catalogue.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <array>
#include <memory>
#include <optional>
#include <utility>

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
class DescriptorPolicy;
using PolicySnapshot = std::shared_ptr<const DescriptorPolicy>;
using PolicyResult = std::variant<PolicySnapshot, ConfigError>;
class DescriptorPolicy final {
 public:
  const CodecResources& resources() const noexcept { return resources_; }
  const FamilyPolicy* family(std::string_view) const noexcept;
  // What the model catalogue says about `model` at `origin`: the family's defaults with the
  // matching entry's defaults over them, and the entry's limits. A model the catalogue does not
  // know gets the family defaults and no limits; it is never refused for that.
  const RequestDefaults& defaults(std::string_view family, std::string_view origin, std::string_view model) const noexcept;
  std::optional<std::uint64_t> output_limit(std::string_view family, std::string_view origin, std::string_view model) const noexcept;
  std::optional<std::uint64_t> input_limit(std::string_view family, std::string_view origin, std::string_view model) const noexcept;
  ModelMatch model(std::string_view family, std::string_view origin, std::string_view model) const noexcept;
  const CatalogueSnapshot& catalogue() const noexcept { return catalogue_; }
  // This policy with another catalogue. The replay identity stays; the content digest changes.
  // Entry defaults are checked against the family rules, and a refusal points into the catalogue
  // (`/model_catalogue/models/<id>/defaults/<family>`, with the entry id in place of an index).
  PolicyResult with_catalogue(CatalogueSnapshot catalogue) const;
  // The replay identity: SHA-256 over the facts that decide how sealed history is written to and
  // read from the wire (each family's bindings, stop reasons and Messages header version). It is
  // part of every native origin and archive, so a policy that changes one of them makes earlier
  // sealed history ReplayIneligible. Admission data (model defaults and limits, media allowlists,
  // sampling rules, server-tool facts, gateway origins, codec resources) is not part of it: editing
  // that data keeps sealed history replayable.
  std::string_view identity() const noexcept { return identity_; }
  std::string identity_hex() const;
  // SHA-256 over the whole policy pair and the model catalogue, admission data included. This is
  // what a pin (`expected_sha256_hex`) checks. It is computed from the parsed documents, not the
  // file bytes.
  std::string_view content_digest() const noexcept { return content_digest_; }
  std::string content_digest_hex() const;
  // True when `model` belongs to a series that the family's `temperature_forbidden_generations`
  // names, at or after the generation given there. Matching is ASCII case-insensitive against the
  // full model or the part after its last `/`. A model of the series whose generation cannot be
  // read counts as the newest one. This complements the exact `temperature_forbidden_model_prefixes`.
  bool temperature_forbidden_by_generation(std::string_view family, std::string_view model) const noexcept;
  // The request member the output cap is written to when a descriptor does not bind one. It is the
  // family's own member, except on the origins a family lists in `max_completion_tokens_origins`
  // (direct OpenAI Chat, whose current models reject `max_tokens`).
  std::string_view default_output_cap_member(std::string_view family, std::string_view origin) const noexcept;
 private:
  DescriptorPolicy() = default;
  // New facts are kept here, after the members above, so the installed FamilyPolicy layout stays as it was.
  struct GenerationRule { std::string family, series; std::uint32_t major = 0, minor = 0; };
  CodecResources resources_{};
  std::vector<FamilyPolicy> families_;
  // Defaults of one catalogue entry for one family, resolved over the family's defaults.
  struct ResolvedDefaults { std::size_t entry; std::string family; RequestDefaults defaults; };
  std::string identity_, content_digest_, pair_digest_;
  CatalogueSnapshot catalogue_;
  std::vector<ResolvedDefaults> resolved_;
  std::vector<GenerationRule> generation_rules_;
  std::vector<std::pair<std::string, std::string>> completion_cap_origins_;  // family, origin
  friend class PolicyLoader;
};
// Without a catalogue argument, a policy uses builtin_catalogue().
PolicyResult load_policy(std::string_view family_json, std::string_view resource_json);
PolicyResult load_policy(std::string_view family_json, std::string_view resource_json, CatalogueSnapshot catalogue);
PolicyResult load_verified_policy(std::string_view family_json, std::string_view resource_json,
                                  std::string_view expected_sha256_hex = {});
PolicyResult load_policy_files(std::string_view family_json_path, std::string_view resource_json_path,
                               std::string_view expected_sha256_hex = {});
PolicyResult load_policy_bundle(std::string_view bundle_json,
                                std::string_view expected_sha256_hex = {});
PolicyResult load_policy_bundle_file(std::string_view bundle_path,
                                     std::string_view expected_sha256_hex = {});
std::string builtin_policy_bundle_json();
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
// The policy's defaults for `model` at this descriptor's family and origin.
const RequestDefaults& model_defaults(const ValidatedDescriptor&, std::string_view model) noexcept;
// How the policy's catalogue matched `model` at this descriptor's family and origin.
ModelMatch model_match(const ValidatedDescriptor&, std::string_view model) noexcept;
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
