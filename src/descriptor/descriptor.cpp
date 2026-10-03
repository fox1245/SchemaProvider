#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>

namespace sp::descriptor {
namespace {
using json::Value;
bool ascii_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool control(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string lower(std::string_view s) {
    std::string result(s);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}
std::span<const std::string_view> schema_keys(std::string_view pointer) {
    static constexpr std::string_view root[]{"descriptor_version", "revision", "id", "family", "evidence", "connection", "bindings", "stop_reasons"};
    static constexpr std::string_view connection[]{"base_url", "paths", "headers"};
    static constexpr std::string_view paths[]{"buffered", "streaming"};
    static constexpr std::string_view evidence[]{"urls", "verified_at"};
    static constexpr std::string_view bindings[]{"model", "messages", "stream", "max_output_tokens", "usage"};
    if (pointer.empty()) return root;
    if (pointer == "/connection") return connection;
    if (pointer == "/connection/paths") return paths;
    if (pointer == "/evidence") return evidence;
    if (pointer == "/bindings") return bindings;
    return {};
}
// Only schema-owned keys and actual array indices may appear in diagnostics.
// Raw map keys and keys below an unknown member stop at their trusted ancestor.
std::string diagnostic_pointer(std::string_view pointer, Value root) {
    std::size_t trusted = 0;
    auto node = root;
    while (trusted < pointer.size() && pointer[trusted] == '/') {
        const auto next = pointer.find('/', trusted + 1);
        const auto end = next == std::string_view::npos ? pointer.size() : next;
        const auto key = pointer.substr(trusted + 1, end - trusted - 1);
        const auto ancestor = pointer.substr(0, trusted);
        const auto keys = schema_keys(ancestor);
        if (node.is_object() && std::find(keys.begin(), keys.end(), key) != keys.end()) {
            node = node.get(key);
        } else if (node.is_array() && (ancestor == "/evidence/urls" || ancestor == "/bindings/usage")) {
            std::size_t index = 0;
            const auto parsed = std::from_chars(key.data(), key.data() + key.size(), index);
            if (key.empty() || (key.size() > 1 && key.front() == '0') ||
                parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || index >= node.size()) break;
            node = node.at(index);
        } else {
            break;
        }
        trusted = end;
    }
    return std::string(pointer.substr(0, trusted));
}
std::uint64_t unambiguous_revision(Value root) {
    if (!root.is_object()) return 0;
    Value revision;
    for (auto member : root.members()) {
        if (member.key != "revision") continue;
        if (revision.valid()) return 0;
        revision = member.value;
    }
    return revision.is_uint() ? revision.as_uint() : 0;
}
bool member_name(std::string_view s) {
    return !s.empty() && s.size() <= 128 && (ascii_alpha(s[0]) || s[0] == '_') &&
        std::all_of(s.begin(), s.end(), [](char c) { return ascii_alpha(c) || digit(c) || c == '_'; });
}
bool ipv4(std::string_view host, bool& loopback) {
    unsigned first = 0;
    for (unsigned index = 0; index < 4; ++index) {
        auto dot = host.find('.');
        auto part = host.substr(0, dot);
        unsigned number = 0;
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part[0] == '0')) return false;
        auto result = std::from_chars(part.data(), part.data() + part.size(), number);
        if (result.ec != std::errc{} || result.ptr != part.data() + part.size() || number > 255) return false;
        if (!index) first = number;
        if (index == 3) {
            if (dot != std::string_view::npos) return false;
        } else {
            if (dot == std::string_view::npos) return false;
            host.remove_prefix(dot + 1);
        }
    }
    loopback = first == 127;
    return true;
}
bool ipv6(std::string_view host) {
    const auto compressed = host.find("::");
    if (compressed != std::string_view::npos && host.find("::", compressed + 2) != std::string_view::npos) return false;
    unsigned groups = 0;
    auto side = [&groups](std::string_view s) {
        if (s.empty()) return true;
        while (true) {
            auto colon = s.find(':');
            auto group = s.substr(0, colon);
            if (group.empty() || group.size() > 4 || !std::all_of(group.begin(), group.end(), hex)) return false;
            ++groups;
            if (colon == std::string_view::npos) return true;
            s.remove_prefix(colon + 1);
        }
    };
    if (compressed == std::string_view::npos) return side(host) && groups == 8;
    return side(host.substr(0, compressed)) && side(host.substr(compressed + 2)) && groups < 8;
}
bool origin(std::string_view url, bool https_only = false) {
    bool secure = url.starts_with("https://");
    if (!secure && (https_only || !url.starts_with("http://"))) return false;
    auto authority = url.substr(secure ? 8 : 7);
    if (authority.ends_with('/')) authority.remove_suffix(1);
    if (authority.empty() || authority.find_first_of("/@?#%\\ \t\r\n") != std::string_view::npos || control(authority)) return false;
    std::string_view host, port;
    bool has_port = false, loopback = false;
    if (authority.front() == '[') {
        auto close = authority.find(']');
        if (close == std::string_view::npos) return false;
        host = authority.substr(1, close - 1);
        if (!ipv6(host)) return false;
        loopback = host == "::1" || host == "0:0:0:0:0:0:0:1";
        auto suffix = authority.substr(close + 1);
        if (!suffix.empty()) {
            if (suffix.front() != ':') return false;
            has_port = true;
            port = suffix.substr(1);
        }
    } else {
        auto colon = authority.find(':');
        host = authority.substr(0, colon);
        has_port = colon != std::string_view::npos;
        if (has_port) port = authority.substr(colon + 1);
        if (host.empty() || host.size() > 253) return false;
        const bool numeric = std::all_of(host.begin(), host.end(), [](char c) { return digit(c) || c == '.'; });
        if (numeric) {
            if (!ipv4(host, loopback)) return false;
        } else {
            loopback = lower(host) == "localhost";
            auto rest = host;
            while (true) {
                auto dot = rest.find('.');
                auto label = rest.substr(0, dot);
                if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
                    !std::all_of(label.begin(), label.end(), [](char c) { return ascii_alpha(c) || digit(c) || c == '-'; })) return false;
                if (dot == std::string_view::npos) break;
                rest.remove_prefix(dot + 1);
            }
        }
    }
    if (has_port) {
        unsigned number = 0;
        auto result = std::from_chars(port.data(), port.data() + port.size(), number);
        if (port.empty() || result.ec != std::errc{} || result.ptr != port.data() + port.size() || !number || number > 65535) return false;
    }
    return secure || loopback;
}
bool relative_path(std::string_view path, bool google_generate = false) {
    if (path.empty() || path.front() != '/' || path.starts_with("//") || path.size() > 2048) return false;
    if (google_generate && path.ends_with("?alt=sse")) path.remove_suffix(8);
    for (unsigned char c : path) {
        if (!(ascii_alpha(static_cast<char>(c)) || digit(static_cast<char>(c)) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || (google_generate && c == ':'))) return false;
    }
    path.remove_prefix(1);
    while (!path.empty()) {
        auto slash = path.find('/');
        auto part = path.substr(0, slash);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
    return true;
}
bool date(std::string_view s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (std::size_t i = 0; i < s.size(); ++i) if (i != 4 && i != 7 && !digit(s[i])) return false;
    const unsigned year = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + s[3]-'0';
    const unsigned month = (s[5]-'0')*10 + s[6]-'0', day = (s[8]-'0')*10 + s[9]-'0';
    constexpr unsigned days[]{31,28,31,30,31,30,31,31,30,31,30,31};
    if (!year || !month || month > 12 || !day) return false;
    return day <= days[month-1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}
}

class Loader {
public:
    explicit Loader(PolicySnapshot policy) { value_.policy_ = std::move(policy); }
    LoadResult run(std::string_view source) {
        if (!value_.policy_) return ConfigError{"", "admitted immutable descriptor policy", 0, "missing descriptor policy"};
        const auto& limits = value_.policy_->resources();
        auto parsed = json::parse(source, {limits.descriptor_bytes, limits.descriptor_depth});
        if (auto* error = std::get_if<json::ParseError>(&parsed)) {
            const auto root = error->context.root();
            return ConfigError{diagnostic_pointer(error->pointer, root),
                "strict bounded JSON descriptor",
                unambiguous_revision(root), error->message};
        }
        auto root = std::get<json::Document>(parsed).root();
        auto revision = root.get("revision");
        if (revision.is_uint()) value_.revision_ = revision.as_uint();
        try {
            object(root, "");
            auto version = root.get("descriptor_version");
            if (!version.is_uint() || version.as_uint() != 1) fail("/descriptor_version", "integer 1");
            if (!revision.is_uint() || !revision.as_uint()) fail("/revision", "positive uint64 integer");
            value_.id_ = string(root.get("id"), "/id", 128);
            if (!std::all_of(value_.id_.begin(), value_.id_.end(), [](char c) { return ascii_alpha(c) || digit(c) || c == '-' || c == '_' || c == '.'; })) fail("/id", "stable ASCII identifier");
            value_.family_ = string(root.get("family"), "/family", 64);
            if (value_.family_ != "openai.chat" && value_.family_ != "anthropic.messages" && value_.family_ != "openai.responses" &&
                value_.family_ != "google.generate" && value_.family_ != "google.interactions")
                fail("/family", "supported Chat, Messages, Responses, Google generate or Interactions family");
            const auto* family = value_.policy_->family(value_.family_);
            if (!family) fail("/family", "family in admitted policy");
            value_.model_member_ = family->model_member;
            value_.messages_member_ = family->messages_member;
            value_.stream_member_ = family->stream_member;
            value_.max_output_tokens_member_ = family->cap_member;
            value_.usage_path_ = family->usage_path;
            value_.stop_reasons_ = family->stops;
            std::string default_bindings = "{\"usage\":[";
            for (const auto& segment : family->usage_path) {
                if (default_bindings.back() != '[') default_bindings += ',';
                default_bindings += json::quote(segment);
            }
            default_bindings += "]}";
            auto defaults = json::parse(default_bindings);
            bindings(std::get<json::Document>(defaults).root());
            connection(root.get("connection"));
            if (auto item = root.get("evidence"); item.valid()) evidence(item);
            if (auto item = root.get("bindings"); item.valid()) bindings(item);
            if (auto item = root.get("stop_reasons"); item.valid()) stops(item);
            return std::move(value_);
        } catch (ConfigError& error) { return std::move(error); }
    }
private:
    ValidatedDescriptor value_;
    [[noreturn]] void fail(std::string pointer, std::string expected, std::string message = "descriptor value rejected") const {
        throw ConfigError{std::move(pointer), std::move(expected), value_.revision_, std::move(message)};
    }
    void object(Value item, const std::string& pointer) const {
        if (!item.is_object()) fail(pointer, "object");
        const auto keys = schema_keys(pointer);
        for (auto member : item.members())
            if (std::find(keys.begin(), keys.end(), member.key) == keys.end())
                fail(pointer, "declared family field", "unknown or unsupported descriptor member");
    }
    std::string string(Value item, const std::string& pointer, std::size_t maximum, bool empty = false) const {
        if (!item.is_string()) fail(pointer, "string");
        auto text = item.as_string();
        if ((!empty && text.empty()) || text.size() > maximum || control(text)) fail(pointer, "bounded string without control characters");
        return std::string(text);
    }
    void connection(Value item) {
        object(item, "/connection");
        value_.base_url_ = string(item.get("base_url"), "/connection/base_url", 2048);
        if (!origin(value_.base_url_)) fail("/connection/base_url", "HTTPS origin, or HTTP canonical loopback origin");
        if (value_.base_url_.ends_with('/')) value_.base_url_.pop_back();
        auto paths = item.get("paths");
        object(paths, "/connection/paths");
        value_.buffered_path_ = string(paths.get("buffered"), "/connection/paths/buffered", 2048);
        value_.streaming_path_ = string(paths.get("streaming"), "/connection/paths/streaming", 2048);
        const bool generate = value_.family_ == "google.generate";
        if (!relative_path(value_.buffered_path_, generate)) fail("/connection/paths/buffered", "origin-relative literal path without traversal or URL escapes");
        if (!relative_path(value_.streaming_path_, generate)) fail("/connection/paths/streaming", "origin-relative literal path without traversal or URL escapes");
        auto headers = item.get("headers");
        if (!headers.valid()) return;
        if (!headers.is_object() || headers.size() > 32) fail("/connection/headers", "object with at most 32 literal headers");
        std::unordered_set<std::string> names;
        const std::string pointer = "/connection/headers";
        for (auto member : headers.members()) {
            if (member.key.empty() || member.key.size() > 128 || !std::all_of(member.key.begin(), member.key.end(), [](char c) { return ascii_alpha(c) || digit(c) || c == '-'; })) fail(pointer, "ASCII header name");
            auto name = lower(member.key);
            constexpr std::array reserved{"authorization", "proxy-authorization", "cookie", "set-cookie", "host", "connection", "upgrade", "te", "trailer", "transfer-encoding", "content-length", "content-type", "accept", "accept-encoding", "expect"};
            if (std::find(reserved.begin(), reserved.end(), name) != reserved.end() || name.starts_with("proxy-") || !names.insert(name).second)
                fail(pointer, "unique non-reserved literal header name");
            value_.headers_.emplace_back(member.key, string(member.value, pointer, 4096, true));
            if (value_.family_ == "anthropic.messages" && name == "anthropic-version" &&
                !contains(value_.family_policy().header_versions, value_.headers_.back().second))
                fail(pointer, "admitted Messages API version");
        }
    }
    void evidence(Value item) {
        object(item, "/evidence");
        auto urls = item.get("urls");
        if (!urls.is_array() || !urls.size() || urls.size() > 16) fail("/evidence/urls", "array of 1..16 HTTPS documentation URLs");
        std::size_t index = 0;
        for (auto url : urls.elements()) {
            auto pointer = "/evidence/urls/" + std::to_string(index++);
            auto text = string(url, pointer, 2048);
            auto end = text.find_first_of("/?#", 8);
            auto authority = std::string_view(text).substr(0, end);
            if (!origin(authority, true) || text.find_first_of(" \\") != std::string::npos) fail(pointer, "HTTPS documentation URL");
            value_.evidence_.urls.push_back(std::move(text));
        }
        value_.evidence_.verified_at = string(item.get("verified_at"), "/evidence/verified_at", 10);
        if (!date(value_.evidence_.verified_at)) fail("/evidence/verified_at", "calendar date YYYY-MM-DD");
    }
    void bindings(Value item) {
        object(item, "/bindings");
        std::array slots{
            std::pair{"model", &value_.model_member_}, std::pair{"messages", &value_.messages_member_},
            std::pair{"stream", &value_.stream_member_}, std::pair{"max_output_tokens", &value_.max_output_tokens_member_}};
        std::unordered_set<std::string> destinations{"tools", "tool_choice", "temperature", "top_p", "stream_options", "n", "response_format", "stop", "max_completion_tokens", "provider"};
        if (value_.family_ == "openai.chat") { destinations.insert("reasoning_effort"); destinations.insert("service_tier"); }
        if (value_.family_ == "anthropic.messages") {
            for (const auto* reserved : {"system", "thinking", "metadata", "stop_sequences", "top_k", "output_config", "service_tier", "context_management", "container", "mcp_servers"})
                destinations.insert(reserved);
        }
        if (value_.family_ == "openai.responses") {
            for (const auto* reserved : {"reasoning", "include", "store", "instructions", "conversation",
                    "previous_response_id", "parallel_tool_calls", "max_tool_calls", "background", "truncation", "text",
                    "metadata", "service_tier", "prompt_cache_key", "context_management", "access_programs"})
                destinations.insert(reserved);
        }
        if (value_.family_ == "google.generate" || value_.family_ == "google.interactions") {
            for (const auto* reserved : {"systemInstruction", "generationConfig", "toolConfig", "cachedContent", "serviceTier",
                    "system_instruction", "generation_config", "store", "service_tier", "previous_interaction_id",
                    "background", "agent", "environment", "safetySettings", "safety_settings"})
                destinations.insert(reserved);
        }
        for (auto [name, target] : slots) {
            const std::string pointer = std::string("/bindings/") + name;
            if (auto field = item.get(name); field.valid()) *target = string(field, pointer, 128);
            if (!member_name(*target)) fail(pointer, "literal object member identifier");
            // max_completion_tokens is the family's alternate declared token slot.
            if (value_.family_ == "openai.chat" && std::string_view(name) == "max_output_tokens" && *target == "max_completion_tokens") destinations.erase(*target);
            if (!destinations.insert(*target).second) fail(pointer, "distinct non-reserved request destination", "request destination collision");
        }
        auto usage = item.get("usage");
        if (!usage.valid()) return;
        if (!usage.is_array() || !usage.size() || usage.size() > 8) fail("/bindings/usage", "array of 1..8 escaped object-member segments");
        value_.usage_path_.clear();
        std::size_t index = 0;
        for (auto part : usage.elements()) {
            auto pointer = "/bindings/usage/" + std::to_string(index++);
            auto raw = string(part, pointer, 128);
            std::string decoded;
            for (std::size_t i = 0; i < raw.size(); ++i) {
                char c = raw[i];
                if (c == '~') {
                    if (++i == raw.size() || (raw[i] != '0' && raw[i] != '1')) fail(pointer, "RFC 6901 escaped object-member segment");
                    c = raw[i] == '0' ? '~' : '/';
                } else if (c == '/') fail(pointer, "RFC 6901 escaped object-member segment");
                if (c == '*' || c == '[' || c == ']' || c == '?' || c == '{' || c == '}') fail(pointer, "literal object-member segment without selectors");
                decoded += c;
            }
            if (std::all_of(decoded.begin(), decoded.end(), digit)) fail(pointer, "object-member segment, not array index");
            value_.usage_path_.push_back(std::move(decoded));
        }
        constexpr std::array reserved{"choices", "error", "id", "object", "created", "model", "system_fingerprint", "service_tier"};
        if (std::find(reserved.begin(), reserved.end(), value_.usage_path_.front()) != reserved.end())
            fail("/bindings/usage", "usage root disjoint from family response fields", "response path collision");
        if (value_.family_ == "anthropic.messages") {
            constexpr std::array message_fields{"type", "role", "content", "stop_reason", "stop_sequence", "stop_details", "input_transformations", "container"};
            if (std::find(message_fields.begin(), message_fields.end(), value_.usage_path_.front()) != message_fields.end())
                fail("/bindings/usage", "usage root disjoint from Messages response fields", "response path collision");
        }
        if (value_.family_ == "openai.responses") {
            constexpr std::array response_fields{"created_at", "status", "output", "incomplete_details", "reasoning", "instructions"};
            if (std::find(response_fields.begin(), response_fields.end(), value_.usage_path_.front()) != response_fields.end())
                fail("/bindings/usage", "usage root disjoint from Responses response fields", "response path collision");
        }
        if (value_.family_ == "google.generate" || value_.family_ == "google.interactions") {
            constexpr std::array google_fields{"candidates", "promptFeedback", "modelVersion", "responseId", "steps", "status", "created", "updated"};
            if (std::find(google_fields.begin(), google_fields.end(), value_.usage_path_.front()) != google_fields.end())
                fail("/bindings/usage", "usage root disjoint from Google response fields", "response path collision");
        }
    }
    void stops(Value item) {
        if (!item.is_object() || item.size() > 64) fail("/stop_reasons", "object with at most 64 stop mappings");
        const std::string pointer = "/stop_reasons";
        for (auto member : item.members()) {
            if (member.key.empty() || member.key.size() > 128 || control(member.key)) fail(pointer, "bounded raw stop string");
            if (member.key == "error" || member.key == "failed" || member.key == "cancelled" || member.key == "incomplete") fail(pointer, "non-failure stop reason");
            auto mapped = string(member.value, pointer, 32);
            StopKind kind;
            if (mapped == "Unknown") kind = StopKind::Unknown;
            else if (mapped == "EndTurn") kind = StopKind::EndTurn;
            else if (mapped == "MaxTokens") kind = StopKind::MaxTokens;
            else if (mapped == "ToolUse") kind = StopKind::ToolUse;
            else if (mapped == "ContentFilter") kind = StopKind::ContentFilter;
            else if (mapped == "Refusal") kind = StopKind::Refusal;
            else if (mapped == "PauseTurn") kind = StopKind::PauseTurn;
            else if (mapped == "ContextLimit") kind = StopKind::ContextLimit;
            else if (mapped == "StopSequence") kind = StopKind::StopSequence;
            else if (mapped == "MalformedCall") kind = StopKind::MalformedCall;
            else fail(pointer, "existing StopKind");
            auto found = std::find_if(value_.stop_reasons_.begin(), value_.stop_reasons_.end(), [&](const auto& entry) { return entry.first == member.key; });
            if ((value_.family_ == "anthropic.messages" || value_.family_ == "openai.responses") &&
                found != value_.stop_reasons_.end() && found->second != kind)
                fail(pointer, "unchanged standard family stop meaning");
            if (found == value_.stop_reasons_.end()) value_.stop_reasons_.emplace_back(member.key, kind);
            else found->second = kind;
        }
    }
};

StopKind ValidatedDescriptor::stop_kind(std::string_view raw) const {
    for (const auto& [key, value] : stop_reasons_) if (key == raw) return value;
    return StopKind::Unknown;
}
const FamilyPolicy& ValidatedDescriptor::family_policy() const noexcept { return *policy_->family(family_); }
LoadResult load(std::string_view source, PolicySnapshot policy) { return Loader{std::move(policy)}.run(source); }
LoadResult load(std::string_view source) { return load(source, builtin_policy()); }

} // namespace sp::descriptor

namespace sp::descriptor {
bool valid_origin(std::string_view url) { return origin(url); }
}
