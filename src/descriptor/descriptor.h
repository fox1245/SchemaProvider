#pragma once

#include <cstdint>
#include <string>
#include <string_view>
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
    StopKind stop_kind(std::string_view raw) const;
private:
    ValidatedDescriptor() = default;
    std::uint64_t revision_ = 0;
    std::string id_, family_, base_url_, buffered_path_, streaming_path_;
    std::string model_member_ = "model", messages_member_ = "messages", stream_member_ = "stream";
    std::string max_output_tokens_member_ = "max_tokens";
    std::vector<std::string> usage_path_{"usage"};
    std::vector<std::pair<std::string, std::string>> headers_;
    std::vector<std::pair<std::string, StopKind>> stop_reasons_{
        {"stop", StopKind::EndTurn}, {"length", StopKind::MaxTokens},
        {"tool_calls", StopKind::ToolUse}, {"content_filter", StopKind::ContentFilter},
        {"refusal", StopKind::Refusal}, {"pause_turn", StopKind::PauseTurn},
        {"context_length_exceeded", StopKind::ContextLimit}};
    Evidence evidence_;
    friend class Loader;
};
using LoadResult = std::variant<ValidatedDescriptor, ConfigError>;
LoadResult load(std::string_view source);

} // namespace sp::descriptor
