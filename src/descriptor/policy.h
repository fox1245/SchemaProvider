#pragma once
#include "descriptor/catalogue.h"
#include "descriptor/descriptor.h"
#include "descriptor/value_schema.h"
#include "json/json.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sp::reason { struct Reason; }
namespace sp::descriptor {
struct EffectiveChoices;
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
// `options`: the tool's other members the policy admits, with their value schema (#32). `generic`: a Responses tool
// type the SDK has no struct for, sent through responses::GenericHostedTool. Messages only: `client`, a typed tool the
// caller executes (memory, bash, a toolset); an empty `name`, a tool entry that carries none (a toolset); `beta`, the
// anthropic-beta value a request needs while it declares the tool or replays its blocks; `container`, a tool that runs
// in the container Agent Skills load into (code execution).
struct ServerToolFact {
  std::string type, name; bool max_uses; std::optional<ValueSchema> options{}; bool generic = false;
  bool client = false; std::string beta{}; bool container = false;
};
// A typed request control: the request member of that name (`reasoning_effort` is Chat's member and the `effort` of
// Responses' `reasoning`). A family's policy lists the ones its typed request carries, in `controls`. New values go
// at the end.
enum class Control : std::uint8_t {
  MaxOutputTokens, Temperature, TopP, ThinkingBudget, IncludeThoughts, ReasoningEffort, ReasoningSummary,
  ThinkingLevel, ThinkingSummaries, ServiceTier, RequiredTool, Store, MaxToolCalls, ToolChoice
};
inline constexpr std::size_t control_count = 14;
// The policy's spelling of a control: `max_output_tokens`, `top_p`, `service_tier`.
std::string_view control_name(Control) noexcept;
std::optional<Control> control_from_name(std::string_view) noexcept;
// True for a control that takes one of a listed set of values: `reasoning_effort`, `reasoning_summary`,
// `thinking_level` and `service_tier`. A family lists the values it admits, and a catalogue entry's
// `control_values` may narrow them for one model.
bool control_takes_values(Control) noexcept;
// What a policy says about one tool type of a family: whether it lists the type, whether the caller executes it (it
// bills tokens only), whether the tool takes a `max_uses` cap, and the catalogue unit its per-use fee is listed under.
// A type the policy does not list is unknown: a host treats it as a server tool the catalogue prices under `fee_unit`.
struct ToolFacts { bool known = false, client = false, admits_max_uses = false; std::string fee_unit; };
// An anthropic-beta value the policy admits at the listed origins. Messages only.
struct BetaFact { std::string value; std::vector<std::string> origins; };
// A Messages response block the SDK keeps whole and replays as captured (`fallback`, `mcp_tool_listing`), with the
// anthropic-beta value a request that replays it needs: `signed_beta` in place of `beta` when the block carries a
// `signature`. A block that `opens_history` (`compaction`) stands for the conversation before it: a request sends
// the history from the last captured message that carries one, and such a message may begin a history.
struct ResponseBlockFact { std::string type; std::string beta{}; std::string signed_beta{}; bool opens_history = false; };
// A Messages content block the caller writes (`search_result`), as an Opaque part of a user message or of a tool
// result, checked against the schema of the whole block and sent as given.
struct InputBlockFact { std::string type; ValueSchema value; };
// How an extension's value changes the price of a call (#207): the call bills under the catalogue tier `tier` (its
// `input/<tier>`, `output/<tier>`, ... rows), or, when `unmodeled`, in a way no catalogue row states.
struct ExtensionPricing { std::string tier; bool unmodeled = false; };
// The values of an extension that change the price (the fact's `prices`): each of `values` as it states; with
// `other`, every value but null and those of `values` and `standard` as `other` states. Null, and a value neither
// names, bill at the standard rates. Empty: no value changes the price.
struct ExtensionPrices {
  std::vector<std::pair<std::string, ExtensionPricing>> values;
  std::vector<std::string> standard;
  std::optional<ExtensionPricing> other;
};
// A top-level request member the policy admits for the listed origins (#32). A replay-ineligible extension makes the
// turn's answer not continuable natively; an eligible one joins the configuration digest unless it is unbound
// (`bound` false): a per-request service control that does not change what the conversation means, which may change
// from turn to turn (#96). A Messages extension may name the anthropic-beta value a request that sets it needs.
// `prices` says which of its values change the price; `bound` is about native replay, not money.
struct ExtensionFact { std::string key; std::vector<std::string> origins; ValueSchema value; bool replay_eligible = false; bool bound = true; std::string beta{}; ExtensionPrices prices{}; };
// How `value`, an extension's value in a request, changes the price; empty for the standard rates (null always is).
std::optional<ExtensionPricing> extension_pricing(const ExtensionFact& fact, json::Value value);
// An audio format a realtime endpoint takes or sends: its media type as the provider spells it (`audio/pcm;rate=24000`,
// `audio/pcmu`), the samples per second and the bytes of one sample.
struct AudioFormatFact { std::string mime; std::uint32_t rate = 0, sample_bytes = 0; };
// A WebSocket close code a realtime provider ends a session with to report a failure, and the HTTP status that the
// runtime error policy classifies it as (Google's 1007 is a 401). The code alone decides: the close reason is the
// provider's free text.
struct CloseCodeFact { std::uint16_t code = 0; int status = 0; };
// The audio a transcription endpoint takes: each MIME type with the name the provider reads for it (the upload's
// file extension, or `input_audio.format`), and the largest decoded size, zero to leave the descriptor's audio limit.
struct AcceptedAudioFact {
  std::vector<std::pair<std::string, std::string>> types;
  std::uint64_t max_bytes = 0;
};
// A non-chat endpoint the policy admits (D6): what it does (`images`), the wire shape a compiled codec speaks
// (`openai`, `openrouter`), the origins that serve it, the path of each operation (`generate`, `edit`), the
// schema of the members a request may add beside the typed ones, the authentication header, whether the
// provider charges once it accepts a call (such a call is not retried once it may have reached the provider), and
// literal headers every call sends (Anthropic's `anthropic-version`) unless the descriptor sets the same header,
// and, for an operation that takes a chat request's body (token counting), the members of that body it keeps.
struct EndpointFact {
  std::string endpoint, shape;
  std::vector<std::string> origins;
  std::vector<std::pair<std::string, std::string>> paths;
  std::optional<ValueSchema> options{};
  std::string auth = "bearer";
  bool charge_on_accept = false;
  std::vector<std::pair<std::string, std::string>> headers{};
  std::vector<std::string> keep{};
  // Realtime: the formats of the audio sent and received, the first of each the default, and the close codes that
  // report a failure.
  std::vector<AudioFormatFact> audio_input{}, audio_output{};
  std::vector<CloseCodeFact> close_codes{};
  // The size a buffered response of the endpoint may reach where that exceeds the client's own bound (a base64
  // image, a spoken reply); zero leaves the client's bound.
  std::uint64_t max_response_bytes = 0;
  // A transcription endpoint: the audio it takes. Zero for none; otherwise the descriptor's media policy applies.
  std::optional<AcceptedAudioFact> accepted_audio{};
  // A files endpoint: the largest part of a chunked upload, zero for none.
  std::uint64_t max_part_bytes = 0;
};
// The path of one operation of an endpoint fact, if it has one.
std::optional<std::string_view> endpoint_path(const EndpointFact&, std::string_view operation) noexcept;
// Whether the provider bills an operation of an endpoint, as the fact's `billing` states it (`none`, `model`,
// `carried`). `Unstated` when the policy does not say: a fact without `billing`, or a per-operation statement that does
// not name the operation. Unstated never means not billed. New values go at the end.
enum class EndpointBilling : std::uint8_t {
  Unstated,
  // The provider does not bill the call.
  None,
  // The call bills the model its request names, by that model's catalogue entry.
  Model,
  // The call bills the chat requests it carries (a batch submit), each by its own model's entry under the fact's
  // `carried_tier`.
  Carried
};
// How a call sends the API key, by the scheme a policy or a descriptor names: `bearer` as `Authorization: Bearer
// <key>`, `x-api-key` and `x-goog-api-key` as a header of that name whose value is the key. The chat, endpoint and
// realtime paths take their credential header from here, and there is no other scheme.
struct CredentialScheme { std::string_view scheme, header, prefix; };
inline constexpr std::size_t credential_scheme_count = 3;
// The schemes, in a fixed order.
const std::array<CredentialScheme, credential_scheme_count>& credential_schemes() noexcept;
// The scheme of that name, or null for a name that is not one.
const CredentialScheme* credential_scheme(std::string_view scheme) noexcept;
// The names of the schemes as a refusal lists them: `bearer, x-api-key or x-goog-api-key`.
std::string credential_scheme_names();
// The header that carries `key` under `scheme`, or nullopt for a name that is not a scheme, and then none is sent.
std::optional<std::pair<std::string, std::string>> credential_header(std::string_view scheme, std::string_view key);
// True when `name`, in any letter case, is the header of a scheme (`authorization`, `x-api-key`, `x-goog-api-key`).
bool credential_header_name(std::string_view name) noexcept;
// What an origin does with a cache mark (sp::CacheControl, core/cache_control.h), as the policy states it. A mark's
// lifetime is its TTL, or the provider's default of five minutes when it has none; core/cache_marks.h reads the
// fact for a TTL.
struct CacheLifetime {
  // The origin takes a mark with this lifetime.
  bool offered = false;
  // What a write of this lifetime costs as a multiple of the base input price; unset when the policy does not state it.
  std::optional<double> write_multiplier;
};
struct CacheCapabilities {
  // The origin takes marks and puts a breakpoint where one is written. False: the provider caches a prefix by
  // itself, or not at all, and no mark is sent.
  bool supports_explicit = false;
  // The most marks the origin honors in one request. Zero when marks are not supported.
  std::uint32_t max_breakpoints = 0;
  CacheLifetime five_minutes, one_hour;
  // The shortest prompt the origin caches, as a family and origin floor; a model may need more. Unset when the policy
  // does not state one. A mark on a shorter prompt is accepted and caches nothing.
  std::optional<std::uint64_t> min_cacheable_tokens;
  // A mark with a longer lifetime must come before one with a shorter lifetime in prompt order (tools, system, then
  // messages).
  bool longest_first = false;
  // Without explicit support, a mark is dropped and reported (`CacheMarkReport`) rather than refused.
  bool ignore_unsupported = false;
};
// A cache fact of a family: the capabilities at the listed origins and, for a gateway origin, at the models whose id
// starts with one of the `routes` (OpenRouter names a model `<upstream>/<model>`). Empty `origins` apply at every origin and
// empty `routes` to every model. The first fact that matches wins.
struct CacheFact {
  std::vector<std::string> origins, routes;
  CacheCapabilities capabilities;
};
// Where a chat family's paths are at the listed origins, or at every origin when `origins` is empty (`routes`): the
// buffered and the streaming path template. A template is an origin-relative path (descriptor::valid_route_template)
// that names the request's model with `{model}` in both templates or in neither, and only a template carries a
// query. At an origin, the fact that lists it applies, else the family's fact without origins; an origin where none
// applies takes any valid pair of paths.
struct RouteFact {
  std::vector<std::string> origins;
  std::string buffered, streaming;
};
// An OpenRouter plugin id or server tool type the policy admits at its declared origins, with the schema of the
// plugin's other members or of the tool's `parameters` (none: no options).
struct GatewayFact { std::string name; std::optional<ValueSchema> options; };
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
  // Deprecated, always empty: temperature facts are model catalogue facts (DescriptorPolicy::temperature_admitted).
  // The member stays so the installed layout does not change within interface 7.
  std::vector<std::string> temperature_forbidden_model_prefixes;
  std::vector<ServerToolFact> server_tools;
  std::vector<ExtensionFact> extensions;
  std::vector<GatewayFact> openrouter_plugins, openrouter_server_tools;
  std::vector<BetaFact> betas;
  std::vector<ResponseBlockFact> response_blocks;
  std::vector<InputBlockFact> input_blocks;
  // Captured response parts the service refuses when they are sent back: a part whose data member `member` lacks
  // the member `missing`. Native replay of such a message is refused before dispatch. Generate only.
  std::vector<std::pair<std::string, std::string>> unreplayable_parts;
  MediaPolicy media;
  // Replaces `media` for a base URL in openrouter_origins, whose gateway admits more content.
  std::optional<MediaPolicy> openrouter_media;
  // Cache mark facts, most specific first (CacheFact). Empty when the policy states none.
  std::vector<CacheFact> cache;
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
  // know gets the family defaults and no limits; it is never refused for that. Here and in every query below that takes
  // an `origin`, the origin is compared byte for byte with the origins the policy and the catalogue list, so pass its
  // canonical spelling (descriptor::canonical_origin), as a descriptor's base_url() is; another spelling matches nothing.
  const RequestDefaults& defaults(std::string_view family, std::string_view origin, std::string_view model) const noexcept;
  std::optional<std::uint64_t> output_limit(std::string_view family, std::string_view origin, std::string_view model) const noexcept;
  std::optional<std::uint64_t> input_limit(std::string_view family, std::string_view origin, std::string_view model) const noexcept;
  // ModelCatalogue::find of the policy's catalogue, with `origin` canonical as above.
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
  // Whether the family's typed request carries `control`, from the family's `controls`. nullopt when the family is
  // not in the policy or its `controls` are not declared (a policy written before they existed): the policy then
  // says nothing, and a host sends the control and lets the encoder answer. The request checks (validate_choices) read
  // the same list, and a family without one has the embedded policy's there. The list can only narrow what the encoder
  // writes: the loader refuses one that names more than the embedded list of the family.
  std::optional<bool> control_admitted(std::string_view family, Control control) const noexcept;
  // The service tier a catalogue prices `tier` under at the family: empty for a tier that bills at the standard
  // rates (`standard_service_tiers`, so "default" at Chat and "standard" at Interactions), `tier` itself for any
  // other. A tier the policy does not know is passed through, and the catalogue prices it or it has no price.
  std::string_view billing_tier(std::string_view family, std::string_view tier) const noexcept;
  // Whether `model` takes `temperature` at `origin` for `family`, from the model catalogue. The
  // entries that apply are visited in lookup order (ModelCatalogue::find: the exact entry, its
  // aliases, then the prefix profiles from the longest), and the first one that states the fact
  // decides: its `temperature` when set, otherwise its `parameters` when not empty (listed or not).
  // An entry that states neither does not hide a later one that does, and an entry scoped to other
  // origins, or one that lists other families only, is skipped. nullopt when no entry states it: the
  // request is sent and the provider answers.
  std::optional<bool> temperature_admitted(std::string_view family, std::string_view origin, std::string_view model) const noexcept;
  // Whether `model` takes `value` for `control` at `origin` for `family`: the first entry, in the
  // same order, whose `control_values` name the control decides. nullopt when none does, or when
  // `control` takes no listed values. A family's own list still applies on top (validate_choices).
  std::optional<bool> control_value_admitted(std::string_view family, std::string_view origin, std::string_view model,
                                             Control control, std::string_view value) const noexcept;
  // Deprecated, always false: temperature facts are model catalogue facts (temperature_admitted).
  // The member stays so code built for interface 7 keeps compiling.
  bool temperature_forbidden_by_generation(std::string_view family, std::string_view model) const noexcept;
  // The request member the output cap is written to when a descriptor does not bind one. It is the
  // family's own member, except on the origins a family lists in `max_completion_tokens_origins`
  // (direct OpenAI Chat, whose current models reject `max_tokens`).
  std::string_view default_output_cap_member(std::string_view family, std::string_view origin) const noexcept;
  // What `family` does with a cache mark at `origin` for `model`: the first of the family's `cache` facts that matches.
  // nullopt when the family is not in the policy or no fact matches (a policy written before the facts existed): the
  // policy then says nothing, and an encoder refuses a mark. `origin` is compared byte for byte: pass its canonical spelling (descriptor::canonical_origin), as a descriptor's base_url() is.
  std::optional<CacheCapabilities> cache_capabilities(std::string_view family, std::string_view origin, std::string_view model) const noexcept;
  // The fact of a non-chat endpoint (`images`) at `origin`, or null. `origin` is compared byte for byte: pass its canonical spelling (descriptor::canonical_origin), as a descriptor's base_url() is.
  const EndpointFact* endpoint(std::string_view endpoint, std::string_view origin) const noexcept;
  const std::vector<EndpointFact>& endpoints() const noexcept { return endpoints_; }
  // How the provider bills `operation` of the endpoint fact for `endpoint` at `origin`: Unstated when there is no such
  // fact, when the fact states no billing, or when its per-operation billing does not name the operation. A fact that
  // states one value for the endpoint answers it for every operation.
  EndpointBilling endpoint_billing(std::string_view endpoint, std::string_view origin, std::string_view operation) const noexcept;
  // The catalogue tier carried requests are priced under (a batch submit's `carried_tier`), empty when the fact carries
  // none.
  std::string_view carried_tier(std::string_view endpoint, std::string_view origin) const noexcept;
  // The credential scheme a chat call of `family` at `origin` sends the API key with: the first of the family's
  // `auth` facts that lists `origin`, else its last fact, which is for every origin. A policy that does not state
  // the facts for a family has the embedded policy's. Empty for a family the policy does not have. A descriptor's
  // `connection.auth` wins over it (descriptor::auth_scheme). `origin` is compared byte for byte: pass its canonical spelling (descriptor::canonical_origin), as a descriptor's base_url() is.
  std::string_view auth_scheme(std::string_view family, std::string_view origin) const noexcept;
  // A provider limit of `family`'s encoder in force, by name: `document_title_chars`, `mcp_servers` and
  // `container_skills` for Messages; `transcription_vocabulary`, `audio_sample_rate` and `audio_bit_rate` for
  // Generate; `transcription_vocabulary` for Interactions. The policy's own value, or the embedded policy's for a limit
  // it does not state (the encoders refuse at the same value). nullopt for a family or a name that has no such limit.
  std::optional<std::uint64_t> limit(std::string_view family, std::string_view name) const noexcept;
  // The same for an endpoint fact at `origin`: `max_items` of the Anthropic and Google batch facts, the most requests
  // one inline batch carries. `origin` is compared byte for byte: pass its canonical spelling (descriptor::canonical_origin), as a descriptor's base_url() is.
  std::optional<std::uint64_t> endpoint_limit(std::string_view endpoint, std::string_view origin, std::string_view name) const noexcept;
  // The family whose API a model listing belongs to, from the policy's `listings` facts (a policy that leaves a listing
  // out has the embedded policy's): the family whose credential scheme (`auth_scheme`) and Messages version header
  // the listing request sends. The OpenRouter listing is the OpenAI Chat listing of the gateway, so it sends Chat's
  // scheme and not Messages' at the same origin. A family of the policy, never empty.
  std::string_view listing_family(Listing listing) const noexcept;
  // The route facts of `family` in the policy's order, empty for a family without any or not in the policy. A policy
  // that states no `routes` for a family has the embedded policy's.
  const std::vector<RouteFact>& routes(std::string_view family) const noexcept;
  // The route fact for `family` at `origin`: the one that lists the origin, else the one without origins. Null when
  // none applies, and then a descriptor there may have any valid pair of paths. `origin` is compared byte for byte: pass its canonical spelling (descriptor::canonical_origin), as a descriptor's base_url() is.
  const RouteFact* route(std::string_view family, std::string_view origin) const noexcept;
 private:
  DescriptorPolicy() = default;
  // The first entry, in lookup order, whose `control_values` name `control` (ModelCatalogue::for_each_match).
  static const CatalogueEntry* naming(const ModelCatalogue& catalogue, std::string_view family, std::string_view origin,
                                      std::string_view model, std::string_view control, bool profiles_only) noexcept;
  // New facts are kept here, after the members above, so the installed FamilyPolicy layout stays as it was.
  CodecResources resources_{};
  std::vector<FamilyPolicy> families_;
  // Defaults of one catalogue entry for one family, resolved over the family's defaults.
  struct ResolvedDefaults { std::size_t entry; std::string family; RequestDefaults defaults; };
  std::string identity_, content_digest_, pair_digest_;
  CatalogueSnapshot catalogue_;
  std::vector<ResolvedDefaults> resolved_;
  std::vector<std::string> retired_;  // Unused: holds the place of a retired member, so the installed layout of interface 7 is unchanged.
  std::vector<std::pair<std::string, std::string>> completion_cap_origins_;  // family, origin
  std::vector<EndpointFact> endpoints_;
  std::vector<std::pair<std::string, std::uint32_t>> controls_;  // family, bit set of Control
  std::vector<std::pair<std::string, std::vector<std::string>>> standard_tiers_;  // family, tiers
  // The billing statement of the endpoint fact at the same index of endpoints_: each operation with its value, one
  // entry with an empty operation for a value that covers every call, none when the fact states no billing; and the
  // tier carried requests are priced under.
  struct EndpointBillingFact {
    std::vector<std::pair<std::string, EndpointBilling>> operations;
    std::string carried_tier;
  };
  std::vector<EndpointBillingFact> billing_;
  // Members added after the ones above, so the offsets the inline accessors read stay as they were.
  // A family's `auth` facts in order: the origins a fact applies at (none: every origin) and its scheme.
  struct AuthFact { std::vector<std::string> origins; std::string scheme; };
  std::vector<std::pair<std::string, std::vector<AuthFact>>> auth_;  // family, facts
  // The values the policy takes from the embedded policy because it does not state them, in the form the content
  // digest mixes in; empty when it takes none.
  std::string borrowed_;
  // What the request checks of a family read (check): the controls its typed request carries, as a bit set of
  // Control, and the rules that bind a request's members to each other. A family that states no `controls` has the
  // embedded policy's here, while control_admitted still answers "undeclared" for it. A rule a family does not state
  // is the embedded policy's for that family, and off where the embedded policy states none.
  struct ValidationFacts {
    std::uint32_t controls = 0;
    // The largest output cap the typed request can carry, when it has a bound.
    std::optional<std::uint64_t> output_cap_maximum;
    // The thinking budget must be below the output cap (otherwise it may equal it).
    bool thinking_budget_below_cap = false;
    // A thinking budget and a thinking level may not both be set.
    bool thinking_budget_excludes_level = false;
    // A thinking budget excludes a temperature and a top_p below the family's `thinking_top_p_minimum`.
    bool thinking_budget_excludes_temperature = false;
  };
  std::vector<std::pair<std::string, ValidationFacts>> validation_;  // family, facts
  const ValidationFacts* validation(std::string_view family) const noexcept;
  std::vector<std::pair<std::string, std::vector<RouteFact>>> routes_;  // family, facts
  // The provider limits in force per family, by name, and the `max_items` of the endpoint fact at the same index of
  // endpoints_. A limit a policy does not state is the embedded policy's, and its content digest covers it (borrowed_).
  std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::uint64_t>>>> limits_;  // family, (name, value)
  std::vector<std::optional<std::uint64_t>> max_items_;
  // The family each listing belongs to (listing_family), in the order of all_listings.
  std::vector<std::pair<Listing, std::string>> listings_;
  static std::optional<reason::Reason> check(const FamilyPolicy&, const ValidationFacts&, const EffectiveChoices&,
                                             std::optional<std::uint64_t> limit, bool require_cap);
  friend std::optional<reason::Reason> choices_refusal(const ValidatedDescriptor&, std::string_view, const EffectiveChoices&);
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
// True when the catalogue states that `model` does not take `temperature` at this descriptor's family and origin
// (DescriptorPolicy::temperature_admitted is false). A model no entry states it for is not refused.
bool temperature_forbidden(const ValidatedDescriptor&, std::string_view model);
// The policy-level checks of the effective controls: the catalogue's temperature fact, the family's rules and
// limits, then the catalogue's `control_values` for each control that takes listed values.
std::optional<std::string> validate_choices(const ValidatedDescriptor&, std::string_view model, const EffectiveChoices&);
// The media admission in force for this descriptor's origin.
const MediaPolicy& media_policy(const ValidatedDescriptor&) noexcept;
// What this descriptor's family does with a cache mark at its origin for `model` (DescriptorPolicy::cache_capabilities).
std::optional<CacheCapabilities> cache_capabilities(const ValidatedDescriptor&, std::string_view model) noexcept;
// True when the descriptor's paths are the route fact of its origin (DescriptorPolicy::route), filled or not, or when
// no fact applies there. Admission holds every family to it, apart from an Interactions descriptor with literal paths,
// which loads and whose encoder refuses it.
bool route_admitted(const ValidatedDescriptor&) noexcept;
// Why a request for `model` cannot go through this descriptor's route: on a model-routed descriptor, a model that
// cannot take the place of `{model}` in either path (resolved_path is empty). Empty when it can. Encoders answer it
// with InvalidRequest.
std::optional<std::string> route_refusal(const ValidatedDescriptor&, std::string_view model);
// Why a request for `model` does not fit this descriptor's literal route: a descriptor whose paths name a model
// (ValidatedDescriptor::route_model) serves requests for that model only. Empty when it fits, and for a descriptor
// whose paths name no model. Encoders answer it with InvalidConfig.
std::optional<std::string> literal_route_refusal(const ValidatedDescriptor&, std::string_view model);
// The credential scheme this descriptor's chat calls send the API key with: its `connection.auth`, else its policy's
// auth_scheme for its family and origin. Endpoint calls and realtime sessions use their endpoint fact's `auth`.
std::string_view auth_scheme(const ValidatedDescriptor&) noexcept;
// Whether `count` is above the limit `name` the descriptor's policy states for its family (DescriptorPolicy::limit).
// False when the policy has no such limit.
bool over_limit(const ValidatedDescriptor&, std::string_view name, std::uint64_t count) noexcept;
// True when this descriptor targets an origin its family policy declares as a routing gateway
// (openrouter_origins). Such a gateway may serve a different concrete model than the one requested.
bool routed_gateway(const ValidatedDescriptor&) noexcept;
// True when the family can carry OpenRouter routing at all: it declares at least one routing origin
// (`openrouter_origins`). routed_gateway says whether this descriptor's origin is one.
bool family_admits_routing(const FamilyPolicy&) noexcept;
// The catalogue unit a server tool's per-use fee is listed under: `tool:` and the tool's type.
inline constexpr std::string_view tool_fee_prefix = "tool:";
std::string tool_fee_unit(std::string_view type);
// The policy's facts for a tool type of the family: its server-tool fact, or for an OpenRouter server tool its
// gateway fact.
ToolFacts tool_facts(const FamilyPolicy&, std::string_view type);
} // namespace sp::descriptor
