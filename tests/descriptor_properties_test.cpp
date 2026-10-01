#include "descriptor/descriptor.h"
#include "json/json.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

std::string descriptor(std::string_view extra = {}, std::string_view origin = "https://api.example.test", std::string_view path = "/v1/chat/completions", std::string_view connection_extra = {}) {
    return "{\"descriptor_version\":1,\"revision\":7,\"id\":\"synthetic-chat\",\"family\":\"openai.chat\",\"connection\":{\"base_url\":" + sp::json::quote(origin) + ",\"paths\":{\"buffered\":" + sp::json::quote(path) + ",\"streaming\":" + sp::json::quote(path) + "}" + std::string(connection_extra) + "}" + std::string(extra) + "}";
}
void clean_diagnostics(const sp::descriptor::ConfigError& error) {
    for (const auto* field : {&error.pointer, &error.expected, &error.message}) {
        CHECK(field->find("SECRET") == std::string::npos);
        for (unsigned char c : *field) CHECK(c >= 32 && c != 127);
    }
}
void rejected(std::string source, std::string_view pointer, std::uint64_t revision = 7) {
    auto result = sp::descriptor::load(source);
    auto* error = std::get_if<sp::descriptor::ConfigError>(&result);
    CHECK(error != nullptr);
    if (error) {
        CHECK(error->pointer == pointer);
        CHECK(error->revision == revision);
        clean_diagnostics(*error);
    }
}
void parse_rejected(std::string_view source, sp::json::ParseCode code, sp::json::Limits limits = {}) {
    auto result = sp::json::parse(source, limits);
    auto* error = std::get_if<sp::json::ParseError>(&result);
    CHECK(error && error->code == code);
}
void json_validation_and_ownership() {
    using namespace sp::json;
    parse_rejected(R"({"x":1,"x":2})", ParseCode::DuplicateKey);
    parse_rejected(R"({"nested":[{"a/b":0,"a\u002fb":1}]})", ParseCode::DuplicateKey);
    auto duplicate = parse(R"({"nested":[{"a/b":0,"a\u002fb":1}]})");
    CHECK(std::get<ParseError>(duplicate).pointer == "/nested/0/a~1b");
    auto context_owner = [] {
        const std::string source = R"({"revision":7,"nested":{"x":1,"x":2}})";
        auto result = parse(source);
        return std::move(std::get<ParseError>(result));
    }();
    auto context_view = context_owner.context.root();
    ParseError moved_error = std::move(context_owner);
    CHECK(!context_owner.context.root().valid());
    CHECK(context_view.get("revision").as_uint() == 7);
    CHECK(context_view.get("nested").size() == 2);
    CHECK(moved_error.pointer == "/nested/x");
    auto replacement = parse(R"({"discard":0,"discard":1})");
    std::get<ParseError>(replacement) = std::move(moved_error);
    CHECK(!moved_error.context.root().valid());
    CHECK(std::get<ParseError>(replacement).context.root().get("revision").as_uint() == 7);
    auto retained_members = context_view.get("nested").members().begin();
    CHECK((*retained_members++).value.as_uint() == 1);
    CHECK((*retained_members).value.as_uint() == 2);
    parse_rejected("{} {}", ParseCode::Syntax);
    parse_rejected("[1,]", ParseCode::Syntax);
    parse_rejected("{\"x\":\"\xC0\xAF\"}", ParseCode::Syntax);
    parse_rejected(R"("\ud800")", ParseCode::Syntax);
    parse_rejected("1e9999", ParseCode::Syntax);
    parse_rejected("null", ParseCode::SizeExceeded, {3,64});
    CHECK(std::holds_alternative<Document>(parse("null", {4,0})));
    parse_rejected("[]", ParseCode::DepthExceeded, {64,0});
    CHECK(std::holds_alternative<Document>(parse("[{}]", {64,2})));
    parse_rejected("[[{}]]", ParseCode::DepthExceeded, {64,2});
    CHECK(std::holds_alternative<Document>(parse(R"(["[{\"}]"])", {64,1})));
    CHECK(quote(std::string_view{}) == "\"\"");
    CHECK(quote("\xC0\xAF").empty());

    auto parsed = parse(R"({"z":[null,true,{"s":"hé\nllo"}],"n":18446744073709551615})");
    CHECK(std::holds_alternative<Document>(parsed));
    if (!std::holds_alternative<Document>(parsed)) return;
    Document owner = std::move(std::get<Document>(parsed));
    Value borrowed = owner.root();
    Document moved = std::move(owner);
    CHECK(!owner.root().valid());
    CHECK(borrowed.get("n").is_uint());
    CHECK(borrowed.get("n").as_uint() == UINT64_MAX);
    CHECK(!borrowed.get("absent").valid());
    CHECK(borrowed.get("z").at(0).is_null());
    CHECK(borrowed.get("z").at(1).as_bool());
    CHECK(!borrowed.get("z").at(3).valid());
    std::string order;
    for (auto member : borrowed.members()) order += member.key;
    CHECK(order == "zn");
    auto elements = borrowed.get("z").elements().begin();
    CHECK((*elements++).is_null());
    CHECK((*elements++).as_bool());
    CHECK((*elements++).get("s").as_string() == "hé\nllo");
    CHECK(elements == borrowed.get("z").elements().end());
    auto reordered = parse(R"({"n":18446744073709551615,"z":[null,true,{"s":"hé\nllo"}]})");
    CHECK(equal(borrowed, std::get<Document>(reordered).root()));
    auto changed = parse(R"({"n":18446744073709551615,"z":[true,null,{"s":"hé\nllo"}]})");
    CHECK(!equal(borrowed, std::get<Document>(changed).root()));
    auto roundtrip = parse(borrowed.dump());
    CHECK(equal(borrowed, std::get<Document>(roundtrip).root()));
    const std::string text("a\0b\n\"\\", 7);
    auto quoted = parse(quote(text));
    CHECK(std::get<Document>(quoted).root().as_string() == text);
}
void accepted_configuration_is_typed() {
    using namespace sp::descriptor;
    auto loaded = load(descriptor(R"(,"bindings":{"model":"deployment","messages":"conversation","stream":"streaming","max_output_tokens":"max_completion_tokens","usage":["metrics","token~1counts","a~0b"]},"stop_reasons":{"done":"EndTurn","new-limit":"MaxTokens"},"evidence":{"urls":["https://example.test/docs"],"verified_at":"2024-02-29"})", "http://127.9.8.7:8080/", "/chat", R"(,"headers":{"X-Fixture":"synthetic"})"));
    CHECK(std::holds_alternative<ValidatedDescriptor>(loaded));
    if (!std::holds_alternative<ValidatedDescriptor>(loaded)) return;
    const auto& value = std::get<ValidatedDescriptor>(loaded);
    CHECK(value.id() == "synthetic-chat" && value.revision() == 7);
    CHECK(value.family() == "openai.chat");
    CHECK(value.base_url() == "http://127.9.8.7:8080");
    CHECK(value.path(false) == "/chat" && value.path(true) == "/chat");
    CHECK(value.request_model_member() == "deployment");
    CHECK(value.request_messages_member() == "conversation");
    CHECK(value.request_stream_member() == "streaming");
    CHECK(value.max_output_tokens_member() == "max_completion_tokens");
    CHECK((value.usage_path() == std::vector<std::string>{"metrics", "token/counts", "a~b"}));
    CHECK((value.headers() == std::vector<std::pair<std::string,std::string>>{{"X-Fixture","synthetic"}}));
    CHECK(value.stop_kind("done") == StopKind::EndTurn);
    CHECK(value.stop_kind("new-limit") == StopKind::MaxTokens);
    CHECK(value.stop_kind("stop") == StopKind::EndTurn);
    CHECK(value.stop_kind("undocumented") == StopKind::Unknown);
    CHECK(value.evidence().verified_at == "2024-02-29");
    auto overrides = load(descriptor(R"(,"stop_reasons":{"refusal":"Unknown","pause":"PauseTurn","context":"ContextLimit","sequence":"StopSequence","bad-call":"MalformedCall","declined":"Refusal"})"));
    CHECK(std::holds_alternative<ValidatedDescriptor>(overrides));
    if (auto* configured = std::get_if<ValidatedDescriptor>(&overrides)) {
        CHECK(configured->stop_kind("refusal") == StopKind::Unknown);
        CHECK(configured->stop_kind("pause") == StopKind::PauseTurn);
        CHECK(configured->stop_kind("context") == StopKind::ContextLimit);
        CHECK(configured->stop_kind("sequence") == StopKind::StopSequence);
        CHECK(configured->stop_kind("bad-call") == StopKind::MalformedCall);
        CHECK(configured->stop_kind("declined") == StopKind::Refusal);
    }
}
void closed_inventory_and_nested_types() {
    for (auto key : {"models", "options", "usage", "errors", "constraints", "events", "terminal", "request_json", "retry_safety", "hooks"})
        rejected(descriptor(",\"" + std::string(key) + "\":{}"), "");
    for (auto key : {"auth", "request_id", "required_env", "optional_env"})
        rejected(descriptor({}, "https://api.example.test", "/chat", ",\"" + std::string(key) + "\":{}"), "/connection");
    rejected(descriptor(R"(,"bindings":{"unknown":"SECRET"})"), "/bindings");
    rejected(descriptor(R"(,"bindings":null)"), "/bindings");
    rejected(descriptor(R"(,"bindings":{"model":42})"), "/bindings/model");
    rejected(descriptor(R"(,"stop_reasons":{"done":false})"), "/stop_reasons");
    rejected(descriptor(R"(,"stop_reasons":{"done":"Success"})"), "/stop_reasons");
    rejected(descriptor(R"(,"stop_reasons":{"error":"EndTurn"})"), "/stop_reasons");
    rejected(descriptor(R"(,"evidence":{"urls":["https://example.test"],"verified_at":"2023-02-29"})"), "/evidence/verified_at");
    rejected(descriptor(R"(,"evidence":{"urls":["http://example.test"],"verified_at":"2024-02-29"})"), "/evidence/urls/0");
    rejected(descriptor(R"(,"evidence":{"urls":[false],"verified_at":"2024-02-29"})"), "/evidence/urls/0");
    rejected(descriptor(R"(,"bindings":{"model":"x","model":"y"})"), "/bindings/model");
    auto version = descriptor();
    version.replace(version.find("\"descriptor_version\":1"), 22, "\"descriptor_version\":2");
    rejected(version, "/descriptor_version");
    auto fractional = descriptor();
    fractional.replace(fractional.find("\"revision\":7"), 12, "\"revision\":7.0");
    auto result = sp::descriptor::load(fractional);
    CHECK(std::holds_alternative<sp::descriptor::ConfigError>(result));
    CHECK(std::get<sp::descriptor::ConfigError>(result).pointer == "/revision");
    auto oversized = descriptor();
    oversized.append(65536, ' ');
    CHECK(std::holds_alternative<sp::descriptor::ConfigError>(sp::descriptor::load(oversized)));
    rejected(descriptor(",\"bindings\":{\"model\":" + sp::json::quote(std::string(129, 'a')) + "}"), "/bindings/model");
    rejected(descriptor({}, "https://example.test", "/" + std::string(2048, 'a')), "/connection/paths/buffered");
}
void endpoint_security_and_paths() {
    for (auto url : {"https://example.test", "https://example.test:443", "https://[2001:db8::1]:443", "http://localhost", "http://127.0.0.1", "http://127.255.255.255:65535", "http://[::1]:80"})
        CHECK(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(sp::descriptor::load(descriptor({}, url))));
    for (auto url : {"http://example.test", "http://localhost.evil.test", "http://127.0.0.1.evil.test", "http://127.1", "http://2130706433", "http://0177.0.0.1", "http://127.0.0.256", "http://[::ffff:127.0.0.1]", "https://user:SECRET@example.test", "https://example.test?SECRET", "https://example.test/#SECRET", "https://example.test/path", "https://example.test:0", "https://example.test:65536", "https://example.test:", "https://example.test\\@evil.test", "https://[::::]", "https://example.test%2f.evil.test", "file:///tmp/a"})
        rejected(descriptor({}, url), "/connection/base_url");
    for (auto path : {"//evil.test/chat", "https://evil.test/chat", "/../chat", "/v1/./chat", "/v1/%2e%2e/chat", "/chat?SECRET", "/chat#SECRET", "/chat\\evil", "/chat/{model}", "/chat//other"})
        rejected(descriptor({}, "https://example.test", path), "/connection/paths/buffered");
    rejected(descriptor({}, "https://example.test", "/chat", R"(,"headers":{"Authorization":"SECRET"})"), "/connection/headers");
    rejected(descriptor({}, "https://example.test", "/chat", R"(,"headers":{"X-Trace":"SECRET\r\nInjected: x"})"), "/connection/headers");
    rejected(descriptor({}, "https://example.test", "/chat", R"(,"headers":{"X-Trace":"a","x-trace":"b"})"), "/connection/headers");
}
void collisions_and_selector_rejection() {
    rejected(descriptor(R"(,"bindings":{"model":"messages"})"), "/bindings/messages");
    rejected(descriptor(R"(,"bindings":{"model":"tools"})"), "/bindings/model");
    rejected(descriptor(R"(,"bindings":{"messages":"stream_options"})"), "/bindings/messages");
    rejected(descriptor(R"(,"bindings":{"model":"nested.model"})"), "/bindings/model");
    rejected(descriptor(R"(,"bindings":{"usage":["choices","usage"]})"), "/bindings/usage");
    for (auto path : {R"(["*"])", R"(["tokens[0]"])", R"(["0"])", R"(["a~2b"])", R"(["a/b"])", R"([])", R"([null])"}) {
        auto result = sp::descriptor::load(descriptor(",\"bindings\":{\"usage\":" + std::string(path) + "}"));
        CHECK(std::holds_alternative<sp::descriptor::ConfigError>(result));
    }
    rejected(descriptor(R"(,"models":[{"selector":"bad*middle"}])"), "");
    rejected(descriptor(R"(,"constraints":[{"kind":"omit","target":"temperature","when":{"in":["x"]}}])"), "");
    rejected(descriptor(R"(,"constraints":[{"kind":"require_greater","lhs":"a","rhs":"b"}])"), "");
    rejected(descriptor(R"(,"constraints":[{"kind":"RequireEqualWhen"}])"), "");
}
void diagnostics_exclude_untrusted_keys_and_controls() {
    // Every ASCII control byte, including NUL and DEL, must be absent from all
    // diagnostic fields even when it was decoded from a JSON member name.
    for (unsigned index = 0; index <= 32; ++index) {
        const char control = static_cast<char>(index == 32 ? 127 : index);
        const auto key = sp::json::quote(std::string("SECRET") + control + "~/");
        const auto duplicate = "{" + key + ":0," + key + ":1}";
        rejected(descriptor("," + key + ":0"), "");
        rejected(descriptor(",\"bindings\":{" + key + ":0}"), "/bindings");
        rejected(descriptor({}, "https://example.test", "/chat", ",\"headers\":{" + key + ":0}"), "/connection/headers");
        rejected(descriptor(",\"stop_reasons\":{" + key + ":\"EndTurn\"}"), "/stop_reasons");
        rejected(descriptor("," + key + ":0," + key + ":1"), "");
        rejected(descriptor("," + key + ":" + duplicate), "");
        rejected(descriptor(",\"bindings\":" + duplicate), "/bindings");
        rejected(descriptor({}, "https://example.test", "/chat", ",\"headers\":" + duplicate), "/connection/headers");
        rejected(descriptor(",\"stop_reasons\":" + duplicate), "/stop_reasons");
        rejected(descriptor(",\"bindings\":{\"usage\":[" + duplicate + "]}"), "/bindings/usage/0");
        rejected(descriptor(",\"evidence\":{\"urls\":[" + duplicate + "]}"), "/evidence/urls/0");
    }
    // Even otherwise valid map keys remain data, never diagnostic path segments.
    rejected(descriptor({}, "https://example.test", "/chat", R"(,"headers":{"SECRET":false})"), "/connection/headers");
    rejected(descriptor(R"(,"stop_reasons":{"SECRET":false})"), "/stop_reasons");
    rejected(descriptor(R"(,"bindings":{"SECRET":{"model":1,"model":2}})"), "/bindings");
    rejected(descriptor(R"(,"bindings":{"model":{"SECRET":1,"SECRET":2}})"), "/bindings/model");
    rejected(descriptor(R"(,"evidence":{"urls":{"0":{"SECRET":1,"SECRET":2}}})"), "/evidence/urls");
    rejected(descriptor({}, "https://example.test", "/chat", R"(,"SECRET":0)"), "/connection");
    rejected(descriptor(R"(,"evidence":{"SECRET":0})"), "/evidence");
    rejected(R"({"revision":7,"connection":{"paths":{"SECRET":1,"SECRET":2}}})", "/connection/paths");
    rejected(R"({"revision":7,"connection":{"paths":{"buffered":1,"buffered":2}}})", "/connection/paths/buffered");
}
void duplicate_errors_retain_only_unambiguous_revision() {
    rejected(R"({"revision":7,"bindings":{"model":"a","model":"b"}})", "/bindings/model");
    rejected(R"({"revision":7,"id":1,"id":2})", "/id");
    rejected(R"({"revision":18446744073709551615,"id":1,"id":2})", "/id", UINT64_MAX);
    rejected(R"({"id":1,"id":2})", "/id", 0);
    for (auto revision : {"0", "-1", "7.0", "7e0", "18446744073709551616", "true", "null", "\"7\"", "{}", "[]"}) {
        rejected("{\"revision\":" + std::string(revision) + ",\"id\":1,\"id\":2}", "/id", 0);
    }
    rejected(R"({"revision":7,"revision":7})", "/revision", 0);
    rejected(R"({"revision":7,"revi\u0073ion":8})", "/revision", 0);
    rejected(R"({"SECRET":0,"SECRET":1,"revision":7,"revision":7})", "", 0);
    rejected(R"({"revision":7,"bindings":{"revision":1,"revision":2}})", "/bindings");
    rejected(R"({"revision":7,"id":1,"id":2,})", "", 0);
}
}
int main() {
    json_validation_and_ownership();
    accepted_configuration_is_typed();
    closed_inventory_and_nested_types();
    endpoint_security_and_paths();
    collisions_and_selector_rejection();
    diagnostics_exclude_untrusted_keys_and_controls();
    duplicate_errors_retain_only_unambiguous_revision();
    if (failures) std::fprintf(stderr, "%d descriptor/JSON checks failed\n", failures);
    else std::puts("descriptor/JSON properties passed");
    return failures ? 1 : 0;
}
