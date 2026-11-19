#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "descriptor/stop_facts.h"
#include "json/json.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>

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
    static constexpr std::string_view connection[]{"base_url", "paths", "headers", "auth"};
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
constexpr std::string_view placeholder = "{model}";
bool segment_char(char c) { return ascii_alpha(c) || digit(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == ':'; }
bool query_char(char c) { return ascii_alpha(c) || digit(c) || c == '-' || c == '_' || c == '.' || c == '~'; }
bool names_model(std::string_view path) { return path.find(placeholder) != std::string_view::npos; }
// An origin-relative path without a query: no authority, no traversal, no URL escapes. `{model}` may stand once in
// it where `placeholder_allowed` is set; a second one, or any other brace, is refused.
bool relative_path(std::string_view path, bool placeholder_allowed) noexcept {
    if (path.empty() || path.front() != '/' || path.starts_with("//") || path.size() > 2048) return false;
    const auto at = placeholder_allowed ? path.find(placeholder) : std::string_view::npos;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i == at) { i += placeholder.size() - 1; continue; }
        if (path[i] != '/' && !segment_char(path[i])) return false;
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
// The query a route template may end in: `?` and name=value pairs joined by `&`.
bool route_query(std::string_view query) noexcept {
    if (!query.starts_with('?')) return false;
    query.remove_prefix(1);
    while (true) {
        const auto amp = query.find('&');
        const auto pair = query.substr(0, amp);
        const auto equals = pair.find('=');
        if (equals == std::string_view::npos || !equals || equals + 1 == pair.size()) return false;
        if (!std::all_of(pair.begin(), pair.begin() + equals, query_char) || !std::all_of(pair.begin() + equals + 1, pair.end(), query_char)) return false;
        if (amp == std::string_view::npos) return true;
        query.remove_prefix(amp + 1);
    }
}
// A path a descriptor or a route template states: an origin-relative path in which `{model}` may stand once, then a
// query where `queried` is set. A descriptor's path has a query only where a route fact applies, and then the pair
// must be the fact's (Loader::routes), so a query is never the descriptor's own.
bool stated_path(std::string_view path, bool queried) noexcept {
    const auto at = path.find('?');
    return relative_path(path.substr(0, at), true) && (at == std::string_view::npos || (queried && route_query(path.substr(at))));
}
// What `path` puts in the place of `{model}` in the template `pattern`: nullopt when the path is not the template
// filled; an empty view when the template has no placeholder and the path is the template; the placeholder itself
// when the path is the template as it is; otherwise a model name (route_model_name).
std::optional<std::string_view> filling(std::string_view path, std::string_view pattern) noexcept {
    const auto at = pattern.find(placeholder);
    if (at == std::string_view::npos) return path == pattern ? std::optional<std::string_view>(std::string_view{}) : std::nullopt;
    const auto prefix = pattern.substr(0, at), suffix = pattern.substr(at + placeholder.size());
    if (path.size() <= prefix.size() + suffix.size() || !path.starts_with(prefix) || !path.ends_with(suffix)) return std::nullopt;
    const auto model = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    if (model == placeholder || route_model_name(model)) return model;
    return std::nullopt;
}
// What a pair of paths fills a route fact with: the buffered path's filling when the streaming path is the
// streaming template filled the same way, otherwise nullopt.
std::optional<std::string_view> filling(std::string_view buffered, std::string_view streaming, const RouteFact& route) noexcept {
    const auto model = filling(buffered, route.buffered);
    if (!model || filling(streaming, route.streaming) != model) return std::nullopt;
    return model;
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

std::optional<StopKind> stop_kind_from_name(std::string_view name) noexcept {
    static constexpr std::pair<std::string_view, StopKind> names[]{
        {"Unknown", StopKind::Unknown}, {"EndTurn", StopKind::EndTurn}, {"MaxTokens", StopKind::MaxTokens},
        {"ToolUse", StopKind::ToolUse}, {"ContentFilter", StopKind::ContentFilter}, {"Refusal", StopKind::Refusal},
        {"PauseTurn", StopKind::PauseTurn}, {"ContextLimit", StopKind::ContextLimit}, {"StopSequence", StopKind::StopSequence},
        {"MalformedCall", StopKind::MalformedCall}, {"Compaction", StopKind::Compaction}};
    for (const auto& [spelled, kind] : names) if (spelled == name) return kind;
    return std::nullopt;
}
std::optional<FailureStop> failure_stop_class(std::string_view raw) noexcept {
    static constexpr std::pair<std::string_view, FailureStop> words[]{
        {"error", FailureStop::Failed}, {"failed", FailureStop::Failed},
        {"cancelled", FailureStop::Cancelled}, {"incomplete", FailureStop::Incomplete}};
    for (const auto& [word, failure] : words) if (word == raw) return failure;
    return std::nullopt;
}
bool failure_stop(std::string_view raw) noexcept { return failure_stop_class(raw).has_value(); }
bool contradicts(const StopRows& rows, std::string_view raw, StopKind kind) noexcept {
    return std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.first == raw && row.second != kind; });
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
            // The origin can change the default output-cap member (direct OpenAI rejects max_tokens);
            // an explicit binding below still wins.
            value_.max_output_tokens_member_ = std::string(value_.policy_->default_output_cap_member(value_.family_, value_.base_url_));
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
        routes();
        // The scheme this descriptor's chat calls send the key with, in place of its family policy's.
        if (auto auth = item.get("auth"); auth.valid()) {
            const auto* scheme = auth.is_string() ? credential_scheme(auth.as_string()) : nullptr;
            if (!scheme) fail("/connection/auth", credential_scheme_names());
            value_.auth_ = static_cast<std::uint8_t>(scheme - credential_schemes().data() + 1);
        }
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
    // The paths as the family's route fact for the origin (DescriptorPolicy::route) has them. Where a fact applies,
    // the pair is its templates, or both with one model name in place of `{model}`, and only then may a path carry
    // a query. Where none applies, `{model}` stands in both paths or in neither.
    void routes() {
        const std::string buffered_pointer = "/connection/paths/buffered", streaming_pointer = "/connection/paths/streaming";
        const std::string_view buffered = value_.buffered_path_, streaming = value_.streaming_path_;
        const auto* route = value_.policy_->route(value_.family_, value_.base_url_);
        if (!stated_path(buffered, route != nullptr)) fail(buffered_pointer, "origin-relative literal path without traversal or URL escapes");
        if (!stated_path(streaming, route != nullptr)) fail(streaming_pointer, "origin-relative literal path without traversal or URL escapes");
        if (route) {
            // An Interactions descriptor whose literal paths are not its origin's loads as it always has; its encoder
            // refuses it (route_admitted).
            const bool kept = value_.family_ == "google.interactions" && !names_model(buffered) && !names_model(streaming) &&
                buffered.find('?') == std::string_view::npos && streaming.find('?') == std::string_view::npos;
            if (const auto model = filling(buffered, route->buffered); !model) {
                if (!kept) fail(buffered_pointer, "a path of the route fact for the descriptor's origin");
            } else if (filling(streaming, route->streaming) != model) {
                if (!kept) fail(streaming_pointer, "the route fact's streaming path for the model of the buffered path");
            } else {
                value_.model_routed_ = *model == placeholder;
            }
        } else {
            if (names_model(buffered) != names_model(streaming)) fail(streaming_pointer, "{model} in both paths or in neither");
            value_.model_routed_ = names_model(buffered);
        }
        // A Generate request carries its model in the path alone, so a literal route needs a route fact that says
        // where in the path the model is.
        if (value_.family_ == "google.generate" && !value_.model_routed_ && value_.route_model().empty())
            fail(buffered_pointer, "a literal Generate route needs a route fact naming where its model is");
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
    // A descriptor adds raw stop values to its family's table. In every family it never gives a value another kind
    // than its policy or the embedded policy gives it, and never lists a value whose meaning the codecs decide in code.
    void stops(Value item) {
        if (!item.is_object() || item.size() > 64) fail("/stop_reasons", "object with at most 64 stop mappings");
        const std::string pointer = "/stop_reasons";
        for (auto member : item.members()) {
            if (member.key.empty() || member.key.size() > 128 || control(member.key)) fail(pointer, "bounded raw stop string");
            if (failure_stop(member.key)) fail(pointer, "non-failure stop reason");
            const auto kind = stop_kind_from_name(string(member.value, pointer, 32));
            if (!kind) fail(pointer, "existing StopKind");
            if (contradicts(value_.stop_reasons_, member.key, *kind) || contradicts(embedded_stops(value_.family_), member.key, *kind))
                fail(pointer, "unchanged standard family stop meaning");
            // A restated row keeps its place, so the table and the bindings that hash it stay as they were.
            if (std::none_of(value_.stop_reasons_.begin(), value_.stop_reasons_.end(), [&](const auto& entry) { return entry.first == member.key; }))
                value_.stop_reasons_.emplace_back(member.key, *kind);
        }
    }
};

StopKind ValidatedDescriptor::stop_kind(std::string_view raw) const {
    for (const auto& [key, value] : stop_reasons_) if (key == raw) return value;
    // A value the table has no row for takes the embedded policy's row, so a custom policy that leaves one out does
    // not change what the value means. The table itself, which native bindings hash, stays as it is.
    for (const auto& [key, value] : embedded_stops(family_)) if (key == raw) return value;
    return StopKind::Unknown;
}
const FamilyPolicy& ValidatedDescriptor::family_policy() const noexcept { return *policy_->family(family_); }
std::string_view ValidatedDescriptor::auth() const noexcept {
    // A value outside the table states no scheme, so the policy's facts decide.
    const auto& schemes = credential_schemes();
    return auth_ >= 1 && static_cast<std::size_t>(auth_) <= schemes.size() ? schemes[auth_ - 1u].scheme : std::string_view{};
}
LoadResult load(std::string_view source, PolicySnapshot policy) { return Loader{std::move(policy)}.run(source); }
LoadResult load(std::string_view source) { return load(source, builtin_policy()); }

LoadResult load_with_deployment_headers(
    std::string_view source,
    const std::vector<std::pair<std::string, std::string>>& overrides,
    const DeploymentHeaderEnvironment& environment, PolicySnapshot policy) {
    if (!policy) policy = builtin_policy();
    const auto& limits = policy->resources();
    auto bad = [](std::string expected) -> LoadResult {
        return ConfigError{"/connection/headers", std::move(expected), 0, "deployment headers rejected before descriptor admission"};
    };
    auto parsed = json::parse(source, {limits.descriptor_bytes, limits.descriptor_depth});
    const auto* document = std::get_if<json::Document>(&parsed);
    if (!document || !document->root().is_object()) return load(source, std::move(policy));
    const auto root = document->root(), connection = root.get("connection");
    if (!connection.is_object()) return load(source, std::move(policy));
    const auto existing = connection.get("headers");
    if (existing.valid() && !existing.is_object()) return bad("literal headers object");
    std::vector<std::pair<std::string, std::string>> headers;
    std::unordered_set<std::string> names;
    if (existing.valid()) for (auto member : existing.members()) {
        if (!member.value.is_string() || !names.insert(lower(member.key)).second)
            return bad("unique case-insensitive literal headers");
        headers.emplace_back(member.key, member.value.as_string());
    }
    auto replace = [&](std::string_view name, std::string_view value) {
        const auto key = lower(name);
        for (auto& header : headers) if (lower(header.first) == key) {
            header.first = name; header.second = value; return;
        }
        headers.emplace_back(name, value);
    };
    // Existing explicit descriptor headers outrank optional host environment.
    if (root.get("family").is_string() && root.get("family").as_string() == "anthropic.messages") {
        if (environment.anthropic_workspace_id && !environment.anthropic_workspace_id->empty() &&
            !names.contains("anthropic-workspace-id"))
            replace("anthropic-workspace-id", *environment.anthropic_workspace_id);
        if (environment.anthropic_beta && !environment.anthropic_beta->empty() &&
            !names.contains("anthropic-beta"))
            replace("anthropic-beta", *environment.anthropic_beta);
    }
    names.clear();
    if (overrides.size() > 32) return bad("at most 32 explicit overrides");
    for (const auto& [name, value] : overrides) {
        if (!names.insert(lower(name)).second) return bad("unique case-insensitive explicit overrides");
        replace(name, value);
    }
    auto build = [&](json::BoundedWriter& body) {
    body.raw("{");
    bool comma = false;
    for (auto member : root.members()) {
        if (comma) body.raw(",");
        comma = true;
        body.quoted(member.key).raw(":");
        if (member.key != "connection") { body.value(member.value, 1); continue; }
        body.raw("{");
        bool connection_comma = false;
        for (auto item : connection.members()) {
            if (item.key == "headers") continue;
            if (connection_comma) body.raw(",");
            connection_comma = true;
            body.quoted(item.key).raw(":").value(item.value, 2);
        }
        if (connection_comma) body.raw(",");
        body.raw("\"headers\":{");
        bool header_comma = false;
        for (const auto& [name, value] : headers) {
            if (header_comma) body.raw(",");
            header_comma = true;
            body.quoted(name).raw(":").quoted(value);
        }
        body.raw("}}");
    }
    body.raw("}");
    };
    json::BoundedWriter measure({limits.descriptor_bytes, limits.descriptor_depth});
    build(measure);
    if (!measure.ok()) return bad("bounded encodable descriptor");
    std::string encoded;
    encoded.reserve(measure.size());
    json::BoundedWriter writer({limits.descriptor_bytes, limits.descriptor_depth}, &encoded);
    build(writer);
    if (!writer.ok()) return bad("bounded encodable descriptor");
    return load(encoded, std::move(policy));
}
LoadResult load_with_environment_headers(
    std::string_view source,
    const std::vector<std::pair<std::string, std::string>>& overrides, PolicySnapshot policy) {
    DeploymentHeaderEnvironment environment;
    if (const char* value = std::getenv("ANTHROPIC_WORKSPACE_ID")) environment.anthropic_workspace_id = value;
    if (const char* value = std::getenv("ANTHROPIC_BETA")) environment.anthropic_beta = value;
    return load_with_deployment_headers(source, overrides, environment, std::move(policy));
}

} // namespace sp::descriptor

namespace sp::descriptor {
bool valid_origin(std::string_view url) { return origin(url); }
bool valid_path(std::string_view path) noexcept { return relative_path(path, false); }
bool valid_route_template(std::string_view path) noexcept { return path.size() <= 2048 && stated_path(path, true); }
bool route_model_name(std::string_view model) noexcept {
    return !model.empty() && model.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._") == std::string_view::npos;
}
bool route_model_substitute(std::string_view model) noexcept {
    return route_model_name(model) && model != "." && model.find("..") == std::string_view::npos;
}
std::string ValidatedDescriptor::resolved_path(bool streaming, std::string_view model) const {
    std::string resolved(path(streaming));
    if (!model_routed_) return resolved;
    if (const auto at = resolved.find(placeholder); at != std::string::npos) resolved.replace(at, placeholder.size(), model);
    return resolved;
}
std::string_view ValidatedDescriptor::route_model() const noexcept {
    // Read from the stored paths and the policy each time, so it holds for any copy of the descriptor.
    if (model_routed_ || !policy_) return {};
    const auto* route = policy_->route(family_, base_url_);
    if (!route) return {};
    const auto model = filling(buffered_path_, streaming_path_, *route);
    if (!model || *model == placeholder) return {};
    return *model;
}
bool route_admitted(const ValidatedDescriptor& descriptor) noexcept {
    const auto* route = descriptor.policy() ? descriptor.policy()->route(descriptor.family(), descriptor.base_url()) : nullptr;
    return !route || filling(descriptor.path(false), descriptor.path(true), *route).has_value();
}
}
