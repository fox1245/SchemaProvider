#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include "sp/config_defaults.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
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
    auto overrides = load(descriptor(R"(,"stop_reasons":{"withheld":"Unknown","pause":"PauseTurn","context":"ContextLimit","sequence":"StopSequence","bad-call":"MalformedCall","declined":"Refusal"})"));
    CHECK(std::holds_alternative<ValidatedDescriptor>(overrides));
    if (auto* configured = std::get_if<ValidatedDescriptor>(&overrides)) {
        CHECK(configured->stop_kind("withheld") == StopKind::Unknown);
        CHECK(configured->stop_kind("pause") == StopKind::PauseTurn);
        CHECK(configured->stop_kind("context") == StopKind::ContextLimit);
        CHECK(configured->stop_kind("sequence") == StopKind::StopSequence);
        CHECK(configured->stop_kind("bad-call") == StopKind::MalformedCall);
        CHECK(configured->stop_kind("declined") == StopKind::Refusal);
    }
}
void messages_family_boundaries() {
    using namespace sp::descriptor;
    auto messages = [](std::string_view extra = {}) {
        auto source = descriptor(extra, "https://api.example.test", "/v1/messages");
        source.replace(source.find("openai.chat"), std::string_view("openai.chat").size(), "anthropic.messages");
        return source;
    };
    auto valid = load(messages(R"(,"bindings":{"model":"deployment","usage":["metrics","tokens"]},"stop_reasons":{"vendor_pause":"PauseTurn"})"));
    CHECK(std::holds_alternative<ValidatedDescriptor>(valid));
    if (const auto* value = std::get_if<ValidatedDescriptor>(&valid)) {
        CHECK(value->stop_kind("refusal") == StopKind::Refusal);
        CHECK(value->stop_kind("model_context_window_exceeded") == StopKind::ContextLimit);
        CHECK(value->stop_kind("vendor_pause") == StopKind::PauseTurn);
        CHECK(value->stop_kind("stop") == StopKind::Unknown);
    }
    for (auto raw : {"end_turn", "max_tokens", "tool_use", "pause_turn", "refusal", "stop_sequence", "model_context_window_exceeded"})
        rejected(messages(",\"stop_reasons\":{\"" + std::string(raw) + "\":\"Unknown\"}"), "/stop_reasons");
    for (auto key : {"system", "thinking", "metadata", "stop_sequences", "output_config", "container"})
        rejected(messages(",\"bindings\":{\"messages\":\"" + std::string(key) + "\"}"), "/bindings/messages");
    for (auto key : {"content", "type", "role", "stop_reason", "stop_sequence", "stop_details", "input_transformations"})
        rejected(messages(",\"bindings\":{\"usage\":[\"" + std::string(key) + "\"]}"), "/bindings/usage");
    for (auto version : {"2023-06-01", "2099-01-01", ""}) {
        auto source = descriptor({}, "https://api.example.test", "/v1/messages",
                                 ",\"headers\":{\"Anthropic-Version\":" + sp::json::quote(version) + "}");
        source.replace(source.find("openai.chat"), std::string_view("openai.chat").size(), "anthropic.messages");
        if (std::string_view(version) == "2023-06-01") CHECK(std::holds_alternative<ValidatedDescriptor>(load(source)));
        else rejected(std::move(source), "/connection/headers");
    }
    auto unknown = messages();
    unknown.replace(unknown.find("anthropic.messages"), std::string_view("anthropic.messages").size(), "uninstalled.family");
    rejected(std::move(unknown), "/family");
}
void closed_inventory_and_nested_types() {
    for (auto key : {"models", "options", "usage", "errors", "constraints", "events", "terminal", "request_json", "retry_safety", "hooks"})
        rejected(descriptor(",\"" + std::string(key) + "\":{}"), "");
    for (auto key : {"request_id", "required_env", "optional_env"})
        rejected(descriptor({}, "https://api.example.test", "/chat", ",\"" + std::string(key) + "\":{}"), "/connection");
    // `auth` names one of the credential schemes; it cannot describe how to authenticate.
    rejected(descriptor({}, "https://api.example.test", "/chat", R"(,"auth":{})"), "/connection/auth");
    rejected(descriptor({}, "https://api.example.test", "/chat", R"(,"auth":{"scheme":"bearer","header":"Authorization"})"), "/connection/auth");
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
    for (auto path : {"//evil.test/chat", "https://evil.test/chat", "/../chat", "/v1/./chat", "/v1/%2e%2e/chat", "/chat?SECRET", "/chat#SECRET", "/chat\\evil", "/chat/{models}", "/chat/{model}/{model}", "/chat//other"})
        rejected(descriptor({}, "https://example.test", path), "/connection/paths/buffered");
    rejected(descriptor({}, "https://example.test", "/chat", R"(,"headers":{"Authorization":"SECRET"})"), "/connection/headers");
    rejected(descriptor({}, "https://example.test", "/chat", R"(,"headers":{"X-Trace":"SECRET\r\nInjected: x"})"), "/connection/headers");
    rejected(descriptor({}, "https://example.test", "/chat", R"(,"headers":{"X-Trace":"a","x-trace":"b"})"), "/connection/headers");
}
std::string generate_descriptor(std::string_view buffered, std::string_view streaming) {
    return "{\"descriptor_version\":1,\"revision\":7,\"id\":\"synthetic-generate\",\"family\":\"google.generate\",\"connection\":{\"base_url\":\"https://api.example.test\",\"paths\":{\"buffered\":"
        + sp::json::quote(buffered) + ",\"streaming\":" + sp::json::quote(streaming) + "}}}";
}
// A google.generate route is a literal model route: the encoder accepts one buffered and one streaming path for the
// request's model, so the loader holds the pair to that shape instead of leaving every request to fail at dispatch.
void generate_route_shape() {
    using namespace sp::descriptor;
    const auto root = std::string("/v1beta/models/");
    const auto buffered = [&](const std::string& model) { return root + model + ":generateContent"; };
    const auto streaming = [&](const std::string& model) { return root + model + ":streamGenerateContent?alt=sse"; };
    for (const std::string& model : std::vector<std::string>{"gemini-2.5-flash", "fixture-model", "Model_1.0-a", "a", std::string(128, 'm')}) {
        auto loaded = load(generate_descriptor(buffered(model), streaming(model)));
        CHECK(std::holds_alternative<ValidatedDescriptor>(loaded));
        if (auto* value = std::get_if<ValidatedDescriptor>(&loaded)) CHECK(value->path(false) == buffered(model) && value->path(true) == streaming(model));
    }
    // The streaming path equal to the buffered one is the mistake this guards against; each of these is refused where it is written.
    for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
             {buffered("m"), buffered("m")},
             {buffered("m"), root + "m:streamGenerateContent"},
             {buffered("m"), root + "m:streamGenerateContent?alt=json"},
             {buffered("m"), streaming("other")},
             {buffered("m"), root + "other:streamGenerateContent?alt=sse"},
             {buffered("m"), "/v1beta/models/m:streamGenerateContent?alt=sse&x=1"},
             {buffered("m"), "/v1/models/m:streamGenerateContent?alt=sse"},
             {buffered("m"), streaming("m") + "/"},
             {buffered("m"), streaming("M")},
             {buffered("m"), "/chat"},
         })
        rejected(generate_descriptor(pair.first, pair.second), "/connection/paths/streaming");
    // A buffered path the encoder never accepts is refused too, whatever the streaming path says.
    for (const std::string& path : std::vector<std::string>{"/chat", root + "m", root + "m:generate", root + "m:streamGenerateContent", root + "m:generateContent?alt=sse",
                                                              "/v1/models/m:generateContent", root + ":generateContent", root + "a:b:generateContent", root + "a~b:generateContent",
                                                              root + "a/b:generateContent", "/v1beta/models:generateContent"})
        rejected(generate_descriptor(path, streaming("m")), "/connection/paths/buffered");
    // `{model}` in both paths makes one descriptor serve every model of the origin.
    {
        auto routed = load(generate_descriptor(buffered("{model}"), streaming("{model}")));
        CHECK(std::holds_alternative<ValidatedDescriptor>(routed));
        if (auto* value = std::get_if<ValidatedDescriptor>(&routed)) {
            CHECK(value->model_routed() && value->path(false) == buffered("{model}"));
            CHECK(value->resolved_path(false, "gemini-3.5-flash") == buffered("gemini-3.5-flash"));
            CHECK(value->resolved_path(true, "gemini-3.5-flash") == streaming("gemini-3.5-flash"));
        }
        auto literal = load(generate_descriptor(buffered("m"), streaming("m")));
        if (auto* value = std::get_if<ValidatedDescriptor>(&literal))
            CHECK(!value->model_routed() && value->resolved_path(false, "other") == buffered("m"));
        rejected(generate_descriptor(buffered("{model}"), streaming("m")), "/connection/paths/streaming");
        rejected(generate_descriptor(buffered("m"), streaming("{model}")), "/connection/paths/streaming");
        for (const std::string& path : {buffered("{models}"), buffered("{model}{model}"), buffered("x{model}"), root + "{model}/{model}:generateContent"})
            rejected(generate_descriptor(path, streaming("{model}")), "/connection/paths/buffered");
        // Every family takes a placeholder where no route fact of its policy applies: the built-in policy states
        // none for Chat, Messages and Responses. It stands in both paths or in neither, and a query is a fact's alone.
        for (const auto* path : {"/v1/{model}/chat", "/{model}", "/v1/models/{model}:chat"}) {
            auto any = load(descriptor({}, "https://example.test", path));
            CHECK(std::holds_alternative<ValidatedDescriptor>(any) && std::get<ValidatedDescriptor>(any).model_routed());
        }
        const auto chat_pair = [](std::string_view buffered, std::string_view streaming) {
            return "{\"descriptor_version\":1,\"revision\":7,\"id\":\"synthetic-chat\",\"family\":\"openai.chat\",\"connection\":{\"base_url\":\"https://example.test\",\"paths\":{\"buffered\":"
                + sp::json::quote(buffered) + ",\"streaming\":" + sp::json::quote(streaming) + "}}}";
        };
        rejected(chat_pair("/v1/{model}/chat", "/v1/chat"), "/connection/paths/streaming");
        rejected(chat_pair("/v1/chat", "/v1/{model}/chat"), "/connection/paths/streaming");
        rejected(chat_pair("/v1/{model}/chat", "/v1/{model}/chat?alt=sse"), "/connection/paths/streaming");
        // `:` is a path character of every family.
        CHECK(std::holds_alternative<ValidatedDescriptor>(load(descriptor({}, "https://example.test", "/v1/models/m:chat"))));
        // Interactions keeps the embedded path at Google's origin and everywhere else; a descriptor with another
        // literal path still loads, as before, and its requests are refused by the encoder.
        CHECK(std::holds_alternative<ValidatedDescriptor>(load(R"({"descriptor_version":1,"revision":1,"id":"interactions","family":"google.interactions","connection":{"base_url":"https://example.test","paths":{"buffered":"/v1beta/other","streaming":"/v1beta/interactions"}}})")));
        rejected(R"({"descriptor_version":1,"revision":7,"id":"interactions","family":"google.interactions","connection":{"base_url":"https://example.test","paths":{"buffered":"/v1beta/{model}/interactions","streaming":"/v1beta/{model}/interactions"}}})", "/connection/paths/buffered");
        rejected(R"({"descriptor_version":1,"revision":7,"id":"interactions","family":"google.interactions","connection":{"base_url":"https://example.test","paths":{"buffered":"/v1beta/interactions?x=1","streaming":"/v1beta/interactions"}}})", "/connection/paths/buffered");
        const auto preset = presets::try_google_generate();
        CHECK(std::holds_alternative<ValidatedDescriptor>(preset) && std::get<ValidatedDescriptor>(preset).model_routed());
    }
    // Other families keep their paths: Chat and Responses may route both modes through one path.
    CHECK(std::holds_alternative<ValidatedDescriptor>(load(descriptor({}, "https://example.test", "/chat"))));
    CHECK(std::holds_alternative<ValidatedDescriptor>(load(R"({"descriptor_version":1,"revision":1,"id":"interactions","family":"google.interactions","connection":{"base_url":"https://example.test","paths":{"buffered":"/v1beta/interactions","streaming":"/v1beta/interactions"}}})")));
    // The presets and the embedded policy's own route still load.
    CHECK(std::holds_alternative<ValidatedDescriptor>(presets::try_google_generate("gemini-2.5-flash")));
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
void deployment_header_admission() {
    using namespace sp::descriptor;
    auto source = descriptor({}, "https://api.example.test", "/v1/messages",
                             R"(,"headers":{"ANTHROPIC-BETA":"descriptor-beta","X-Tenant":"original"})");
    source.replace(source.find("openai.chat"), std::string_view("openai.chat").size(), "anthropic.messages");
    DeploymentHeaderEnvironment environment{"workspace-env", "beta-env"};
    auto admitted = load_with_deployment_headers(source, {{"anthropic-beta", "override-beta"}, {"x-tenant", "new"}}, environment, {});
    CHECK(std::holds_alternative<ValidatedDescriptor>(admitted));
    if (const auto* value = std::get_if<ValidatedDescriptor>(&admitted)) {
        CHECK((value->headers() == std::vector<std::pair<std::string, std::string>>{
            {"anthropic-beta", "override-beta"}, {"x-tenant", "new"}, {"anthropic-workspace-id", "workspace-env"}}));
    }
    auto explicit_only = load_with_deployment_headers(source, {}, environment, {});
    CHECK(std::holds_alternative<ValidatedDescriptor>(explicit_only));
    if (const auto* value = std::get_if<ValidatedDescriptor>(&explicit_only))
        CHECK(value->headers()[0].second == "descriptor-beta");
    auto plain = descriptor({}, "https://api.example.test", "/v1/messages");
    plain.replace(plain.find("openai.chat"), std::string_view("openai.chat").size(), "anthropic.messages");
    auto absent = load_with_deployment_headers(plain, {}, {}, {});
    CHECK(std::holds_alternative<ValidatedDescriptor>(absent));
    if (const auto* value = std::get_if<ValidatedDescriptor>(&absent)) CHECK(value->headers().empty());
    auto other_family = load_with_deployment_headers(descriptor(), {}, environment, {});
    CHECK(std::holds_alternative<ValidatedDescriptor>(other_family));
    if (const auto* value = std::get_if<ValidatedDescriptor>(&other_family)) CHECK(value->headers().empty());
    for (const auto& overrides : {
        std::vector<std::pair<std::string, std::string>>{{"X-One", "a"}, {"x-one", "b"}},
        std::vector<std::pair<std::string, std::string>>{{"Authorization", "forbidden"}},
        std::vector<std::pair<std::string, std::string>>{{"X-Tenant", "bad\r\nheader"}},
        std::vector<std::pair<std::string, std::string>>{{"anthropic-version", "2099-01-01"}}})
        CHECK(std::holds_alternative<ConfigError>(load_with_deployment_headers(plain, overrides, {}, {})));
    CHECK(std::holds_alternative<ConfigError>(load_with_deployment_headers(plain, {}, {"bad\nworkspace", {}}, {})));
}
// Whether `temperature` is refused for `model` by a descriptor of `family` at `origin` under the built-in policy.
bool temperature_refused(std::string_view family, std::string_view origin, std::string_view model) {
    using namespace sp::descriptor;
    auto source = descriptor({}, origin, family == "anthropic.messages" ? "/v1/messages" : "/v1/chat/completions");
    source.replace(source.find("openai.chat"), std::string_view("openai.chat").size(), family);
    auto loaded = load(source);
    CHECK(std::holds_alternative<ValidatedDescriptor>(loaded));
    if (!std::holds_alternative<ValidatedDescriptor>(loaded)) return false;
    const auto& value = std::get<ValidatedDescriptor>(loaded);
    auto choices = effective_defaults(value, model);
    choices.temperature = 0.7;
    const auto refused = validate_choices(value, model, choices);
    CHECK(!refused || *refused == "temperature is prohibited for the admitted model");
    CHECK(refused.has_value() == temperature_forbidden(value, model));
    return refused.has_value();
}
// Temperature facts are the catalogue's, per origin. At the direct origins the embedded profiles refuse their ids;
// another origin, and a spelling no entry has (another case, a gateway namespace), states nothing, so the request is
// sent and the provider answers.
void model_sampling_admission() {
    for (auto model : {"gpt-5-mini", "gpt-6", "o3-mini", "o4-test", "o1-preview"}) {
        CHECK(temperature_refused("openai.chat", "https://api.openai.com", model));
        CHECK(temperature_refused("openai.responses", "https://api.openai.com", model));
        CHECK(!temperature_refused("openai.chat", "https://api.example.test", model));
    }
    for (auto model : {"GPT-6", "openai/O3-mini", "gateway/OpenAI/O4-test", "openai/gpt-5-mini", "gpt-4o-mini"})
        CHECK(!temperature_refused("openai.chat", "https://api.openai.com", model));
    for (auto model : {"claude-opus-4-8-test", "claude-opus-5", "claude-sonnet-5", "claude-fable-test"}) {
        CHECK(temperature_refused("anthropic.messages", "https://api.anthropic.com", model));
        CHECK(!temperature_refused("anthropic.messages", "https://api.example.test", model));
    }
    for (auto model : {"anthropic/CLAUDE-OPUS-4-7", "CLAUDE-OPUS-4-7", "anthropic/claude-opus-4-7"})
        CHECK(!temperature_refused("anthropic.messages", "https://api.anthropic.com", model));
    // A profile lists its families: the OpenAI profiles say nothing for a Messages request at the OpenAI origin.
    CHECK(!temperature_refused("anthropic.messages", "https://api.openai.com", "gpt-5-mini"));
}

// The Claude models that refuse `temperature` at the Anthropic origin are the catalogue's profiles: Opus 4.7, Opus 4.8
// and the Claude 5 families, dated or not. A model no profile names is sent, and the provider answers; another origin
// states nothing, so every id is sent there.
void claude_temperature_by_origin() {
    struct Listed { std::string_view model; bool refused; };
    const Listed listed[] = {
        // The profiles: refused by the API with "`temperature` is deprecated for this model".
        {"claude-haiku-5-5", true}, {"claude-opus-5", true}, {"claude-opus-5-5", true}, {"claude-sonnet-5", true},
        {"claude-sonnet-5-5", true}, {"claude-fable-5", true}, {"claude-fable-5-1", true},
        {"claude-opus-4-7", true}, {"claude-opus-4-8", true}, {"claude-opus-4-7-20271001", true}, {"claude-haiku-5-20270101", true},
        // Accepted: everything before Opus 4.7, with and without the date suffix.
        {"claude-opus-4-6", false}, {"claude-sonnet-4-6", false},
        {"claude-opus-4-5", false}, {"claude-opus-4-5-20251101", false},
        {"claude-sonnet-4-5", false}, {"claude-sonnet-4-5-20250929", false},
        {"claude-haiku-4-5", false}, {"claude-haiku-4-5-20251001", false},
        {"claude-opus-4-1", false}, {"claude-opus-4-1-20250805", false},
        {"claude-opus-4-0", false}, {"claude-opus-4-20250514", false},
        {"claude-sonnet-4-0", false}, {"claude-sonnet-4-20250514", false},
        {"claude-3-7-sonnet-20250219", false}, {"claude-3-5-sonnet-20241022", false},
        {"claude-3-5-haiku-20241022", false}, {"claude-3-haiku-20240307", false}, {"claude-3-opus-20240229", false},
        // No profile names these: sent, and the provider answers.
        {"claude-haiku-6", false}, {"claude-sonnet-6-2", false}, {"claude-opus-4-9", false}, {"claude-opus-4-10", false},
        {"claude-newfamily-5-1", false}, {"claude-future-7-20300101", false}, {"claude-experimental", false}, {"claude-opus-latest", false},
        // Gateway spellings and another case are not the catalogue's ids.
        {"anthropic/claude-haiku-5.5", false}, {"anthropic/claude-opus-4.7", false}, {"Anthropic/CLAUDE-HAIKU-5-5", false},
        {"anthropic/claude-opus-4.6", false}, {"anthropic/claude-sonnet-4.5", false}, {"anthropic/claude-3.5-sonnet", false},
        {"anthropic/claude-3.7-sonnet:thinking", false},
        {"gpt-4o", false}, {"vendor/some-model-5", false},
    };
    for (const auto& entry : listed) {
        const bool direct = temperature_refused("anthropic.messages", "https://api.anthropic.com", entry.model);
        const bool elsewhere = temperature_refused("anthropic.messages", "https://api.example.test", entry.model);
        if (direct != entry.refused || elsewhere)
            std::fprintf(stderr, "temperature classification differs for %.*s\n", static_cast<int>(entry.model.size()), entry.model.data());
        CHECK(direct == entry.refused);
        CHECK(!elsewhere);
    }
}
void preset_builders_and_validation() {
    using namespace sp::descriptor;
    const auto d_openai_chat = presets::openai_chat();
    CHECK(d_openai_chat.family() == "openai.chat" && d_openai_chat.base_url() == "https://api.openai.com" && d_openai_chat.path(false) == "/v1/chat/completions");
    const auto d_openai_resp = presets::openai_responses();
    CHECK(d_openai_resp.family() == "openai.responses" && d_openai_resp.path(false) == "/v1/responses");
    const auto d_anthropic = presets::anthropic_messages();
    CHECK(d_anthropic.family() == "anthropic.messages" && d_anthropic.base_url() == "https://api.anthropic.com" && d_anthropic.path(false) == "/v1/messages");
    const auto d_google_gen = presets::google_generate("gemini-2.5-flash");
    CHECK(d_google_gen.family() == "google.generate" && d_google_gen.path(false) == "/v1beta/models/gemini-2.5-flash:generateContent" && d_google_gen.path(true) == "/v1beta/models/gemini-2.5-flash:streamGenerateContent?alt=sse");
    const auto d_google_int = presets::google_interactions();
    CHECK(d_google_int.family() == "google.interactions" && d_google_int.path(false) == "/v1beta/interactions");
    const auto d_or_chat = presets::openrouter_chat();
    CHECK(d_or_chat.family() == "openai.chat" && d_or_chat.base_url() == "https://openrouter.ai" && d_or_chat.path(false) == "/api/v1/chat/completions");
    const auto d_or_resp = presets::openrouter_responses();
    CHECK(d_or_resp.family() == "openai.responses" && d_or_resp.path(false) == "/api/v1/responses");
    const auto d_or_msg = presets::openrouter_messages();
    CHECK(d_or_msg.family() == "anthropic.messages" && d_or_msg.path(false) == "/api/v1/messages");
    const auto d_loopback = presets::loopback_chat("http://127.0.0.1:8080");
    CHECK(d_loopback.family() == "openai.chat" && d_loopback.base_url() == "http://127.0.0.1:8080" && d_loopback.path(false) == "/v1/chat/completions");

    presets::PresetOptions opts;
    opts.id = "custom-preset-id";
    opts.revision = 42;
    opts.headers = {{"X-Title", "SchemaProvider"}};
    const auto d_custom = presets::openrouter_chat(opts);
    CHECK(d_custom.id() == "custom-preset-id" && d_custom.revision() == 42 && d_custom.headers().size() == 1);

    for (std::string_view bad_model : {"", "models/gemini-2.5-flash", "gemini-2.5-flash:generateContent", "gemini?key=x", "gemini#frag"}) {
        const auto res = presets::try_google_generate(bad_model);
        CHECK(std::holds_alternative<ConfigError>(res));
        bool threw = false;
        try { (void)presets::google_generate(bad_model); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
    }
    CHECK(std::holds_alternative<ConfigError>(presets::try_loopback_chat("http://api.openai.com")));
    CHECK(std::holds_alternative<ConfigError>(presets::try_loopback_chat("http://127.0.0.1:8080", "relative/path")));
}
// Replaces (or appends) one member of a JSON object, keeping the others in order.
std::string with_member(sp::json::Value object, std::string_view key, std::string_view raw) {
    std::string result = "{";
    bool present = false;
    for (auto member : object.members()) {
        if (result.size() > 1) result += ',';
        result += sp::json::quote(member.key) + ':';
        if (member.key == key) { result += raw; present = true; }
        else result += member.value.dump();
    }
    if (!present) result += std::string(result.size() > 1 ? "," : "") + sp::json::quote(key) + ':' + std::string(raw);
    return result + '}';
}
// The embedded descriptor policy with one member of one family replaced.
std::string with_family_member(std::string_view family, std::string_view key, std::string_view raw) {
    auto parsed = sp::json::parse(sp::config_defaults::descriptor_policy_json);
    const auto root = std::get<sp::json::Document>(parsed).root();
    std::string families = "[";
    for (auto entry : root.get("families").elements()) {
        if (families.size() > 1) families += ',';
        families += entry.get("family").as_string() == family ? with_member(entry, key, raw) : entry.dump();
    }
    return with_member(root, "families", families + ']');
}
// Each temperature profile of the embedded catalogue refuses at the provider's own origin and nowhere else. The
// exact gpt-6-luna entry states no temperature, so the gpt-6 profile still decides for it at the OpenAI origin.
void temperature_profiles_by_origin() {
    struct Profile { std::string_view family, origin, model; };
    const Profile profiles[] = {
        {"openai.chat", "https://api.openai.com", "gpt-5"}, {"openai.chat", "https://api.openai.com", "gpt-5-mini"},
        {"openai.chat", "https://api.openai.com", "gpt-6"}, {"openai.chat", "https://api.openai.com", "gpt-6-luna"},
        {"openai.chat", "https://api.openai.com", "o1-preview"}, {"openai.chat", "https://api.openai.com", "o3-mini"},
        {"openai.chat", "https://api.openai.com", "o4-mini"},
        {"openai.responses", "https://api.openai.com", "gpt-5-nano"}, {"openai.responses", "https://api.openai.com", "gpt-6-luna"},
        {"openai.responses", "https://api.openai.com", "o1"}, {"openai.responses", "https://api.openai.com", "o3"},
        {"openai.responses", "https://api.openai.com", "o4-mini"}, {"openai.responses", "https://api.openai.com", "gpt-6"},
        {"anthropic.messages", "https://api.anthropic.com", "claude-opus-4-7"}, {"anthropic.messages", "https://api.anthropic.com", "claude-opus-4-8"},
        {"anthropic.messages", "https://api.anthropic.com", "claude-opus-5"}, {"anthropic.messages", "https://api.anthropic.com", "claude-sonnet-5"},
        {"anthropic.messages", "https://api.anthropic.com", "claude-haiku-5"}, {"anthropic.messages", "https://api.anthropic.com", "claude-fable-5"},
    };
    for (const auto& profile : profiles) {
        const bool own = temperature_refused(profile.family, profile.origin, profile.model);
        const bool elsewhere = temperature_refused(profile.family, "https://api.example.test", profile.model);
        if (!own || elsewhere)
            std::fprintf(stderr, "temperature profile differs for %.*s at %.*s\n", static_cast<int>(profile.model.size()), profile.model.data(),
                         static_cast<int>(profile.origin.size()), profile.origin.data());
        CHECK(own);
        CHECK(!elsewhere);
    }
}
// The family policy's temperature keys are retired. An empty list still loads, as older snapshots wrote it for the
// families without facts; a fact in either key is refused with a pointer to the key.
void retired_temperature_keys() {
    using namespace sp::descriptor;
    auto every_family = [](std::string_view raw) {
        auto parsed = sp::json::parse(sp::config_defaults::descriptor_policy_json);
        const auto root = std::get<sp::json::Document>(parsed).root();
        std::string families = "[";
        for (auto entry : root.get("families").elements()) {
            if (families.size() > 1) families += ',';
            auto prefixes = sp::json::parse(with_member(entry, "temperature_forbidden_model_prefixes", raw));
            families += with_member(std::get<sp::json::Document>(prefixes).root(), "temperature_forbidden_generations", raw);
        }
        return with_member(root, "families", families + ']');
    };
    auto outcome = [](const std::string& text) {
        auto loaded = load_policy(text, sp::config_defaults::codec_defaults_json);
        if (const auto* error = std::get_if<ConfigError>(&loaded)) return error->pointer + " " + error->expected;
        return std::string("loaded");
    };
    CHECK(outcome(every_family("[]")) == "loaded");
    CHECK(outcome(with_family_member("google.generate", "temperature_forbidden_model_prefixes", "[]")) == "loaded");
    CHECK(outcome(with_family_member("openai.chat", "temperature_forbidden_model_prefixes", R"(["gpt-5"])")) ==
          "/temperature_forbidden_model_prefixes temperature facts in the model catalogue");
    CHECK(outcome(with_family_member("anthropic.messages", "temperature_forbidden_generations", R"([{"series":"claude-","from":"4.7"}])")) ==
          "/temperature_forbidden_generations temperature facts in the model catalogue");
    CHECK(outcome(with_family_member("google.interactions", "temperature_forbidden_generations", R"({})")) ==
          "/temperature_forbidden_generations temperature facts in the model catalogue");
}
void replay_identity_covers_wire_facts_only() {
    using namespace sp::descriptor;
    const auto builtin = builtin_policy();
    CHECK(builtin->identity().size() == 32 && builtin->content_digest().size() == 32);
    CHECK(builtin->identity_hex() != builtin->content_digest_hex());
    auto load_pair = [](std::string_view family_json, std::string_view resource_json = sp::config_defaults::codec_defaults_json) {
        auto loaded = load_policy(family_json, resource_json);
        CHECK(std::holds_alternative<PolicySnapshot>(loaded));
        return std::holds_alternative<PolicySnapshot>(loaded) ? std::get<PolicySnapshot>(std::move(loaded)) : PolicySnapshot{};
    };
    auto parsed = sp::json::parse(sp::config_defaults::descriptor_policy_json);
    const auto root = std::get<sp::json::Document>(parsed).root();
    auto codec = sp::json::parse(sp::config_defaults::codec_defaults_json);
    const auto codec_root = std::get<sp::json::Document>(codec).root();

    // Admission data: the replay identity stays, the content digest changes.
    std::string reversed = "[";
    for (std::size_t i = root.get("families").size(); i-- > 0;) {
        if (reversed.size() > 1) reversed += ',';
        reversed += root.get("families").at(i).dump();
    }
    reversed += ']';
    auto chat = std::get<sp::json::Document>(parsed).root().get("families").at(0);
    CHECK(chat.get("family").as_string() == "openai.chat");
    std::string origins = chat.get("openrouter_origins").dump();
    origins.insert(origins.size() - 1, std::string(origins.size() > 2 ? "," : "") + "\"https://gateway.example.test\"");
    const auto anthropic_defaults = with_member(root.get("families").at(1).get("defaults"), "max_output_tokens", "321");
    CHECK(root.get("families").at(1).get("family").as_string() == "anthropic.messages");
    const std::string admission_edits[] = {
        with_member(root, "families", reversed),
        with_member(root, "revision", std::to_string(root.get("revision").as_uint() + 1)),
        with_family_member("openai.chat", "temperature_range", "[0,1.5]"),
        with_family_member("openai.chat", "openrouter_origins", origins),
        with_family_member("anthropic.messages", "defaults", anthropic_defaults),
        with_family_member("anthropic.messages", "header_versions", R"(["2023-06-01","2026-01-01"])"),
    };
    for (const auto& edited : admission_edits) {
        const auto policy = load_pair(edited);
        if (!policy) continue;
        CHECK(policy->identity() == builtin->identity());
        CHECK(policy->content_digest() != builtin->content_digest());
    }
    {
        auto catalogue = load_catalogue(R"({"version":1,"models":[{"id":"policy-test-added-model","output_limit":4096}]})");
        CHECK(std::holds_alternative<CatalogueSnapshot>(catalogue));
        auto added = builtin->with_catalogue(std::get<CatalogueSnapshot>(std::move(catalogue)));
        CHECK(std::holds_alternative<PolicySnapshot>(added));
        if (auto* policy = std::get_if<PolicySnapshot>(&added)) {
            CHECK((*policy)->identity() == builtin->identity());
            CHECK((*policy)->content_digest() != builtin->content_digest());
        }
    }
    const auto resources = with_member(codec_root.get("resources"), "request_bytes", "4096");
    if (const auto policy = load_pair(sp::config_defaults::descriptor_policy_json, with_member(codec_root, "resources", resources))) {
        CHECK(policy->identity() == builtin->identity());
        CHECK(policy->content_digest() != builtin->content_digest());
    }

    // Wire facts: the replay identity changes.
    std::string stops = chat.get("stop_reasons").dump();
    stops.insert(stops.size() - 1, ",\"policy_test_stop\":\"Unknown\"");
    if (const auto policy = load_pair(with_family_member("openai.chat", "stop_reasons", stops)))
        CHECK(policy->identity() != builtin->identity());
    {
        auto versions = sp::json::parse(with_family_member("anthropic.messages", "header_versions", R"(["2023-06-01","2026-01-01"])"));
        const auto edited_root = std::get<sp::json::Document>(versions).root();
        std::string families = "[";
        for (auto entry : edited_root.get("families").elements()) {
            if (families.size() > 1) families += ',';
            families += entry.get("family").as_string() == "anthropic.messages" ? with_member(entry, "header_version", "\"2026-01-01\"") : entry.dump();
        }
        const auto policy = load_pair(with_member(edited_root, "families", families + ']'));
        if (policy) CHECK(policy->identity() != builtin->identity());
    }
}
// A family's stop table may grow, but a raw value keeps the kind the embedded policy gives it. A custom policy may add
// rows and may not give an embedded row another kind; a descriptor may add rows and may not give a row of its policy,
// or of the embedded policy, another kind. A value whose meaning the codecs decide in code is refused in both, in
// every family.
void stop_tables_grow_but_never_change() {
    using namespace sp::descriptor;
    using sp::json::quote;
    const auto builtin = builtin_policy();
    auto parsed = sp::json::parse(sp::config_defaults::descriptor_policy_json);
    const auto root = std::get<sp::json::Document>(parsed).root();
    // A kind other than `name`, which a table may not give the same raw value.
    const auto other = [](std::string_view name) { return std::string(name == "EndTurn" ? "MaxTokens" : "EndTurn"); };
    const auto policy_error = [](const std::string& text) {
        auto loaded = load_policy(text, sp::config_defaults::codec_defaults_json);
        const auto* error = std::get_if<ConfigError>(&loaded);
        return error ? error->pointer : std::string("loaded");
    };
    // A descriptor of `family` whose `stop_reasons` is `table`.
    const auto source = [](const std::string& family, std::string_view table) {
        const bool generate = family == "google.generate";
        return "{\"descriptor_version\":1,\"revision\":7,\"id\":\"stop-table\",\"family\":" + quote(family) +
               ",\"connection\":{\"base_url\":\"https://api.example.test\",\"paths\":{\"buffered\":" +
               quote(generate ? "/v1beta/models/stop-table:generateContent" : "/v1/stop-table") + ",\"streaming\":" +
               quote(generate ? "/v1beta/models/stop-table:streamGenerateContent?alt=sse" : "/v1/stop-table") +
               "}},\"stop_reasons\":" + std::string(table) + "}";
    };
    std::size_t families = 0;
    for (auto entry : root.get("families").elements()) {
        const std::string family(entry.get("family").as_string());
        const auto stops = entry.get("stop_reasons");
        const auto* listed = builtin->family(family);
        CHECK(listed && stops.size() > 0 && listed->stops.size() == stops.size());
        if (!listed) continue;
        ++families;
        // Added values decode as declared and follow the policy's rows, which keep their order.
        auto added = load(source(family, R"({"vendor_new_stop":"MaxTokens","vendor_other_stop":"Unknown"})"));
        CHECK(std::holds_alternative<ValidatedDescriptor>(added));
        if (const auto* value = std::get_if<ValidatedDescriptor>(&added)) {
            CHECK(value->stop_kind("vendor_new_stop") == StopKind::MaxTokens && value->stop_kind("vendor_other_stop") == StopKind::Unknown);
            CHECK(value->stop_mappings().size() == listed->stops.size() + 2);
            CHECK(std::equal(listed->stops.begin(), listed->stops.end(), value->stop_mappings().begin()));
        }
        for (auto row : stops.members()) {
            const std::string raw(row.key), name(row.value.as_string());
            // Restating a row changes nothing.
            auto restated = load(source(family, "{" + quote(raw) + ":" + quote(name) + "}"));
            CHECK(std::holds_alternative<ValidatedDescriptor>(restated));
            if (const auto* value = std::get_if<ValidatedDescriptor>(&restated)) CHECK(value->stop_mappings() == listed->stops);
            // Any other kind is a change, Unknown included.
            rejected(source(family, "{" + quote(raw) + ":" + quote(other(name)) + "}"), "/stop_reasons");
            rejected(source(family, "{" + quote(raw) + ":\"Unknown\"}"), "/stop_reasons");
            CHECK(policy_error(with_family_member(family, "stop_reasons", with_member(stops, raw, quote(other(name))))) == "/stop_reasons");
        }
        for (const std::string raw : {"error", "failed", "cancelled", "incomplete"}) {
            rejected(source(family, "{" + quote(raw) + ":\"Unknown\"}"), "/stop_reasons");
            CHECK(policy_error(with_family_member(family, "stop_reasons", with_member(stops, raw, "\"Unknown\""))) == "/stop_reasons");
        }
        // A policy may add a row. Stop facts are wire facts, so the replay identity moves.
        auto grown = load_policy(with_family_member(family, "stop_reasons", with_member(stops, "vendor_new_stop", "\"MaxTokens\"")),
                                 sp::config_defaults::codec_defaults_json);
        CHECK(std::holds_alternative<PolicySnapshot>(grown));
        if (const auto* policy = std::get_if<PolicySnapshot>(&grown)) {
            CHECK((*policy)->identity() != builtin->identity());
            CHECK((*policy)->family(family) && (*policy)->family(family)->stops.size() == listed->stops.size() + 1);
        }
    }
    CHECK(families == 5);
    // A policy that leaves a row out loads, and the descriptor's own table, which native bindings hash, lacks it;
    // stop_kind answers with the embedded row, as the codecs decode. A descriptor under it may state the value again,
    // but only with the kind the embedded policy gives it.
    std::string without_length = "{";
    for (auto entry : root.get("families").elements()) {
        if (entry.get("family").as_string() != "openai.chat") continue;
        for (auto row : entry.get("stop_reasons").members()) {
            if (row.key == "length") continue;
            if (without_length.size() > 1) without_length += ',';
            without_length += quote(row.key) + ':' + row.value.dump();
        }
    }
    auto dropped = load_policy(with_family_member("openai.chat", "stop_reasons", without_length + '}'), sp::config_defaults::codec_defaults_json);
    CHECK(std::holds_alternative<PolicySnapshot>(dropped));
    if (const auto* policy = std::get_if<PolicySnapshot>(&dropped)) {
        auto plain = load(source("openai.chat", "{}"), *policy);
        CHECK(std::holds_alternative<ValidatedDescriptor>(plain));
        if (const auto* value = std::get_if<ValidatedDescriptor>(&plain)) {
            CHECK(std::none_of(value->stop_mappings().begin(), value->stop_mappings().end(), [](const auto& row) { return row.first == "length"; }));
            CHECK(value->stop_kind("length") == StopKind::MaxTokens && value->stop_kind("future_finish_reason") == StopKind::Unknown);
        }
        auto restored = load(source("openai.chat", R"({"length":"MaxTokens"})"), *policy);
        CHECK(std::holds_alternative<ValidatedDescriptor>(restored) && std::get<ValidatedDescriptor>(restored).stop_kind("length") == StopKind::MaxTokens);
        auto remapped = load(source("openai.chat", R"({"length":"EndTurn"})"), *policy);
        CHECK(std::holds_alternative<ConfigError>(remapped) && std::get<ConfigError>(remapped).pointer == "/stop_reasons");
    }
}
void policy_bundle_and_identity_pinning() {
    using namespace sp::descriptor;
    const auto def = builtin_policy();
    CHECK(def != nullptr);
    const auto hex = def->content_digest_hex();
    CHECK(hex.size() == 64);
    for (char c : hex) CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));

    const auto verified = load_verified_policy(sp::config_defaults::descriptor_policy_json, sp::config_defaults::codec_defaults_json, hex);
    CHECK(std::holds_alternative<PolicySnapshot>(verified) && std::get<PolicySnapshot>(verified)->content_digest_hex() == hex);

    std::string wrong_hex = hex;
    wrong_hex[0] = (wrong_hex[0] == '0') ? '1' : '0';
    const auto mismatch = load_verified_policy(sp::config_defaults::descriptor_policy_json, sp::config_defaults::codec_defaults_json, wrong_hex);
    CHECK(std::holds_alternative<ConfigError>(mismatch) && std::get<ConfigError>(mismatch).pointer == "/content_digest");
    const auto malformed_hex = load_verified_policy(sp::config_defaults::descriptor_policy_json, sp::config_defaults::codec_defaults_json, "not-a-sha256");
    CHECK(std::holds_alternative<ConfigError>(malformed_hex) && std::get<ConfigError>(malformed_hex).pointer == "/content_digest");

    const auto bundle_json = builtin_policy_bundle_json();
    const auto bundle_loaded = load_policy_bundle(bundle_json, hex);
    CHECK(std::holds_alternative<PolicySnapshot>(bundle_loaded) && std::get<PolicySnapshot>(bundle_loaded)->content_digest_hex() == hex);
    CHECK(std::holds_alternative<ConfigError>(load_policy_bundle(bundle_json, wrong_hex)));
    CHECK(std::holds_alternative<ConfigError>(load_policy_bundle(R"({"version":2,"descriptor_policy":{},"codec_defaults":{}})")));
    CHECK(std::holds_alternative<ConfigError>(load_policy_bundle(R"({"version":1,"descriptor_policy":{},"codec_defaults":{},"extra":1})")));

    // A directory only this run created: parallel runs (plain and sanitizer builds) and stale
    // files from an earlier run can neither collide with nor feed these files.
    std::filesystem::path tmp_dir;
    {
        std::random_device entropy;
        std::error_code create_error;
        for (int attempt = 0; attempt < 16 && tmp_dir.empty(); ++attempt) {
            char suffix[40];
            std::snprintf(suffix, sizeof suffix, "sp_policy_test_%08x%08x",
                          static_cast<unsigned>(entropy()),
                          static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const auto candidate = std::filesystem::temp_directory_path() / suffix;
            if (std::filesystem::create_directory(candidate, create_error) && !create_error) tmp_dir = candidate;
        }
    }
    CHECK(!tmp_dir.empty());
    if (tmp_dir.empty()) return;
    const auto bundle_path = tmp_dir / "policy_bundle.json";
    const auto desc_path = tmp_dir / "descriptor_policy.json";
    const auto codec_path = tmp_dir / "codec_defaults.json";
    const auto huge_path = tmp_dir / "huge_policy.json";
    {
        std::ofstream(bundle_path, std::ios::binary) << bundle_json;
        std::ofstream(desc_path, std::ios::binary) << sp::config_defaults::descriptor_policy_json;
        std::ofstream(codec_path, std::ios::binary) << sp::config_defaults::codec_defaults_json;
        std::ofstream(huge_path, std::ios::binary) << std::string(140000, ' ');
    }
    const auto from_files = load_policy_files(desc_path.string(), codec_path.string(), hex);
    CHECK(std::holds_alternative<PolicySnapshot>(from_files) && std::get<PolicySnapshot>(from_files)->content_digest_hex() == hex);
    const auto from_bundle_file = load_policy_bundle_file(bundle_path.string(), hex);
    CHECK(std::holds_alternative<PolicySnapshot>(from_bundle_file) && std::get<PolicySnapshot>(from_bundle_file)->content_digest_hex() == hex);
    CHECK(std::holds_alternative<ConfigError>(load_policy_bundle_file((tmp_dir / "nonexistent_sp_bundle.json").string())));
    CHECK(std::holds_alternative<ConfigError>(load_policy_bundle_file(huge_path.string())));
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir, ec);
}
}
int main() {
    json_validation_and_ownership();
    accepted_configuration_is_typed();
    messages_family_boundaries();
    closed_inventory_and_nested_types();
    endpoint_security_and_paths();
    generate_route_shape();
    collisions_and_selector_rejection();
    diagnostics_exclude_untrusted_keys_and_controls();
    duplicate_errors_retain_only_unambiguous_revision();
    deployment_header_admission();
    model_sampling_admission();
    claude_temperature_by_origin();
    temperature_profiles_by_origin();
    retired_temperature_keys();
    preset_builders_and_validation();
    policy_bundle_and_identity_pinning();
    replay_identity_covers_wire_facts_only();
    stop_tables_grow_but_never_change();
    if (failures) std::fprintf(stderr, "%d descriptor/JSON checks failed\n", failures);
    else std::puts("descriptor/JSON properties passed");
    return failures ? 1 : 0;
}
