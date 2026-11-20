#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <string_view>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace sp::descriptor {

enum class StopKind {
    Unknown, EndTurn, MaxTokens, ToolUse, ContentFilter,
    Refusal, PauseTurn, ContextLimit, StopSequence, MalformedCall, Compaction
};
struct Evidence {
    std::vector<std::string> urls;
    std::string verified_at;
};
struct ConfigError {
    std::string pointer;
    std::string expected;
    std::uint64_t revision = 0;
    std::string message;
};
class DescriptorPolicy;
struct FamilyPolicy;
using PolicySnapshot = std::shared_ptr<const DescriptorPolicy>;
class Loader;
class ValidatedDescriptor {
public:
    std::string_view id() const { return id_; }
    std::uint64_t revision() const { return revision_; }
    std::string_view family() const { return family_; }
    // `connection.base_url` as its origin's serialization (canonical_origin): `https://API.example.com:443/` is held
    // as `https://api.example.com`. Policy facts, catalogue entries and the native digests read this spelling.
    std::string_view base_url() const { return base_url_; }
    std::string_view path(bool streaming) const { return streaming ? streaming_path_ : buffered_path_; }
    // True for a descriptor of any family whose paths name `{model}`: one descriptor serves every
    // model of its origin, and each request's model takes the place of the placeholder.
    bool model_routed() const noexcept { return model_routed_; }
    // The path a request for `model` is sent to: path(streaming) with `{model}` replaced on a
    // model-routed descriptor (descriptor::model_path), and path(streaming) itself otherwise. On a
    // model-routed descriptor it is empty for a model that route_model_substitute refuses and for a path
    // that would be longer than 2048 bytes: it never holds a model that was not checked. The encoders
    // refuse such a request before they build anything (descriptor::route_refusal).
    std::string resolved_path(bool streaming, std::string_view model) const;
    // The model a literal route names: the name its paths carry where the `{model}` templates of the route
    // fact of its origin have the placeholder (DescriptorPolicy::route). Empty on a model-routed descriptor and
    // where the fact has no placeholder or no fact applies. A literal google.generate route serves only it.
    std::string_view route_model() const noexcept;
    std::string_view request_model_member() const { return model_member_; }
    std::string_view request_messages_member() const { return messages_member_; }
    std::string_view request_stream_member() const { return stream_member_; }
    std::string_view max_output_tokens_member() const { return max_output_tokens_member_; }
    const std::vector<std::string>& usage_path() const { return usage_path_; }
    const std::vector<std::pair<std::string, std::string>>& headers() const { return headers_; }
    const Evidence& evidence() const { return evidence_; }
    const PolicySnapshot& policy() const noexcept { return policy_; }
    const FamilyPolicy& family_policy() const noexcept;
    // The kind of a raw stop value, as the codecs decode it: the row of stop_mappings(), else the embedded policy's
    // row for the family, else Unknown.
    StopKind stop_kind(std::string_view raw) const;
    const std::vector<std::pair<std::string, StopKind>>& stop_mappings() const noexcept { return stop_reasons_; }
    // The credential scheme `connection.auth` names (`bearer`, `x-api-key` or `x-goog-api-key`), empty when the
    // descriptor names none. descriptor::auth_scheme(descriptor) is the scheme its chat calls send the key with.
    std::string_view auth() const noexcept;
private:
    ValidatedDescriptor() = default;
    std::uint64_t revision_ = 0;
    bool model_routed_ = false;
    // `connection.auth`: zero for none, else one more than the scheme's place in descriptor::credential_schemes(); auth()
    // reads any other value as none. It takes the padding after model_routed_, so the size of the class and the
    // offsets of its members stay.
    std::uint8_t auth_ = 0;
    std::string id_, family_, base_url_, buffered_path_, streaming_path_;
    std::string model_member_, messages_member_, stream_member_, max_output_tokens_member_;
    std::vector<std::string> usage_path_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::vector<std::pair<std::string, StopKind>> stop_reasons_;
    PolicySnapshot policy_;
    Evidence evidence_;
    friend class Loader;
};
using LoadResult = std::variant<ValidatedDescriptor, ConfigError>;
LoadResult load(std::string_view source);
LoadResult load(std::string_view source, PolicySnapshot policy);
struct DeploymentHeaderEnvironment {
    std::optional<std::string> anthropic_workspace_id, anthropic_beta;
};
// Explicit host preprocessing. Descriptor admission remains the final authority;
// no environment reads or header mutation occur after admission.
LoadResult load_with_deployment_headers(
    std::string_view source,
    const std::vector<std::pair<std::string, std::string>>& overrides,
    const DeploymentHeaderEnvironment& environment,
    PolicySnapshot policy);
LoadResult load_with_environment_headers(
    std::string_view source,
    const std::vector<std::pair<std::string, std::string>>& overrides = {},
    PolicySnapshot policy = {});
bool valid_origin(std::string_view url);
// The serialization (RFC 6454 section 6.2) of an origin valid_origin admits: the host's ASCII letters lower-cased, an
// IPv6 literal kept in its brackets, a port that is the scheme's default (443 for https, 80 for http) left out and any
// other written as its decimal number, and no trailing slash. Empty when valid_origin refuses `url`. A descriptor holds
// its base URL in this spelling (ValidatedDescriptor::base_url).
std::string canonical_origin(std::string_view url);
// True when `url` is an origin spelled as canonical_origin writes it. Every origin a policy, a catalogue or a listing
// source names must be: it is compared byte for byte with a descriptor's base URL.
bool canonical_origin_spelling(std::string_view url);
// An origin-relative path as descriptor admission takes a literal one: segments of letters, digits, `-`, `_`,
// `.`, `~` and `:`, none of them empty, `.` or `..`, and no query, URL escape or placeholder.
bool valid_path(std::string_view path) noexcept;
// A path template a route fact may state: a valid_path in which `{model}` may stand once, ending in an optional
// query of `name=value` pairs joined by `&`, of letters, digits, `-`, `_`, `.` and `~`.
bool valid_route_template(std::string_view path) noexcept;
// A model name a route's path can carry: letters, digits, `-`, `.` and `_`. Admission reads the model of a
// literal route with it, and the Generate encoder a request's model.
bool route_model_name(std::string_view model) noexcept;
// A model name that may take the place of `{model}`: a route_model_name that is not `.` and holds no `..`, so
// the path it is put in keeps its segments, its query and its origin.
bool route_model_substitute(std::string_view model) noexcept;
// `path` with `model` in place of its `{model}`, the one function that writes a model into a path: `path` itself when
// it has no placeholder, and empty when route_model_substitute refuses the model or the path would be longer than
// 2048 bytes.
std::string model_path(std::string_view path, std::string_view model);

} // namespace sp::descriptor
