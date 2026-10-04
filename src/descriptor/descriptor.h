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
    Refusal, PauseTurn, ContextLimit, StopSequence, MalformedCall
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
    std::string_view base_url() const { return base_url_; }
    std::string_view path(bool streaming) const { return streaming ? streaming_path_ : buffered_path_; }
    std::string_view request_model_member() const { return model_member_; }
    std::string_view request_messages_member() const { return messages_member_; }
    std::string_view request_stream_member() const { return stream_member_; }
    std::string_view max_output_tokens_member() const { return max_output_tokens_member_; }
    const std::vector<std::string>& usage_path() const { return usage_path_; }
    const std::vector<std::pair<std::string, std::string>>& headers() const { return headers_; }
    const Evidence& evidence() const { return evidence_; }
    const PolicySnapshot& policy() const noexcept { return policy_; }
    const FamilyPolicy& family_policy() const noexcept;
    StopKind stop_kind(std::string_view raw) const;
    const std::vector<std::pair<std::string, StopKind>>& stop_mappings() const noexcept { return stop_reasons_; }
private:
    ValidatedDescriptor() = default;
    std::uint64_t revision_ = 0;
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

} // namespace sp::descriptor
