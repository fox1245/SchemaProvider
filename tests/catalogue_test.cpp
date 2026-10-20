#include "descriptor/catalogue.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include "sp/config_defaults.h"
#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <variant>

namespace {
using namespace sp::descriptor;
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

CatalogueSnapshot catalogue(std::string_view models) {
    auto loaded = load_catalogue(std::string(R"({"version":1,"models":)") + std::string(models) + "}");
    if (auto* error = std::get_if<ConfigError>(&loaded)) {
        std::fprintf(stderr, "catalogue refused at %s: %s\n", error->pointer.c_str(), error->expected.c_str());
        ++failures;
        return nullptr;
    }
    return std::get<CatalogueSnapshot>(std::move(loaded));
}
void refused(std::string_view models, std::string_view pointer) {
    auto loaded = load_catalogue(std::string(R"({"version":1,"models":)") + std::string(models) + "}");
    auto* error = std::get_if<ConfigError>(&loaded);
    CHECK(error != nullptr);
    if (error && error->pointer != pointer) {
        std::fprintf(stderr, "  pointer %s, expected %s\n", error->pointer.c_str(), std::string(pointer).c_str());
        ++failures;
    }
}
ValidatedDescriptor descriptor(std::string_view family, std::string_view origin, PolicySnapshot policy) {
    const bool generate = family == "google.generate";
    const std::string paths = generate
        ? R"("buffered":"/v1beta/models/m:generateContent","streaming":"/v1beta/models/m:streamGenerateContent?alt=sse")"
        : R"("buffered":"/v1/x","streaming":"/v1/x")";
    const auto source = std::string(R"({"descriptor_version":1,"revision":1,"id":"catalogue-test","family":)") +
                        sp::json::quote(family) + R"(,"connection":{"base_url":)" + sp::json::quote(origin) +
                        R"(,"paths":{)" + paths + "}}}";
    auto loaded = load(source, std::move(policy));
    if (auto* error = std::get_if<ConfigError>(&loaded)) {
        std::fprintf(stderr, "descriptor refused at %s: %s\n", error->pointer.c_str(), error->expected.c_str());
        throw std::runtime_error("descriptor");
    }
    return std::get<ValidatedDescriptor>(std::move(loaded));
}

void builtin_catalogue_is_data() {
    const auto builtin = builtin_catalogue();
    CHECK(builtin && !builtin->entries().empty());
    CHECK(builtin->digest().size() == 32 && builtin->digest_hex().size() == 64);
    for (std::size_t i = 1; i < builtin->entries().size(); ++i) CHECK(builtin->entries()[i - 1].id < builtin->entries()[i].id);
    const auto* luna = builtin->entry("gpt-6-luna");
    CHECK(luna && luna->output_limit == std::optional<std::uint64_t>(128000) && luna->source == std::optional<std::string>("verified"));
    const auto omni = builtin->find("google.generate", "https://generativelanguage.googleapis.com", "gemini-omni-1.1-flash");
    CHECK(omni.entry && omni.relation == ModelRelation::Exact && !omni.family_listed);
    // The canonical text reads back as the same catalogue.
    auto again = load_catalogue(builtin->to_json());
    CHECK(std::holds_alternative<CatalogueSnapshot>(again) && std::get<CatalogueSnapshot>(again)->digest() == builtin->digest());
    CHECK(builtin_catalogue_json() == sp::config_defaults::model_catalogue_json);
}

void lookup_ranks_names_then_profiles() {
    const auto c = catalogue(R"([
        {"id":"acme-1","aliases":["acme-latest"],"families":["openai.chat"],"output_limit":10},
        {"id":"acme-","match":"prefix","output_limit":20},
        {"id":"acme-1-","match":"prefix","output_limit":30},
        {"id":"acme-2","origins":["https://proxy.example.test"],"output_limit":40},
        {"id":"acme-3","output_limit":50},
        {"id":"acme-3","match":"prefix","families":["openai.chat"],"output_limit":51},
        {"id":"vendor/acme-4:free","families":["anthropic.messages"]}])");
    if (!c) return;
    const std::string_view origin = "https://api.example.test";
    auto exact = c->find("openai.chat", origin, "acme-1");
    CHECK(exact.entry && exact.relation == ModelRelation::Exact && exact.family_listed && exact.entry->output_limit == 10u);
    auto alias = c->find("openai.chat", origin, "acme-latest");
    CHECK(alias.entry && alias.relation == ModelRelation::Alias && alias.entry->id == "acme-1");
    auto longest = c->find("openai.chat", origin, "acme-1-mini");
    CHECK(longest.entry && longest.relation == ModelRelation::Prefix && longest.entry->output_limit == 30u);
    auto shorter = c->find("openai.chat", origin, "acme-9");
    CHECK(shorter.entry && shorter.relation == ModelRelation::Prefix && shorter.entry->output_limit == 20u);
    // An entry scoped to another origin never matches; the profile does.
    auto elsewhere = c->find("openai.chat", origin, "acme-2");
    CHECK(elsewhere.entry && elsewhere.relation == ModelRelation::Prefix && elsewhere.entry->output_limit == 20u);
    auto scoped = c->find("openai.chat", "https://proxy.example.test", "acme-2");
    CHECK(scoped.entry && scoped.relation == ModelRelation::Exact && scoped.entry->output_limit == 40u);
    // An exact name wins over a profile, even one that lists the family.
    auto name_first = c->find("openai.chat", origin, "acme-3");
    CHECK(name_first.entry && name_first.relation == ModelRelation::Exact && name_first.entry->output_limit == 50u);
    // Listing other families never refuses: the entry matches and says so.
    auto other_family = c->find("openai.responses", origin, "acme-1");
    CHECK(other_family.entry && other_family.relation == ModelRelation::Exact && !other_family.family_listed);
    auto gateway = c->find("anthropic.messages", "https://openrouter.ai", "vendor/acme-4:free");
    CHECK(gateway.entry && gateway.family_listed);
    auto unknown = c->find("openai.chat", origin, "other-model");
    CHECK(!unknown.entry && unknown.relation == ModelRelation::Unknown);
    CHECK(c->entry("acme-3") && c->entry("acme-3", true) && c->entry("acme-3")->output_limit == 50u && c->entry("acme-3", true)->output_limit == 51u);
    CHECK(!c->entry("acme-9"));
}

void loader_is_closed_and_bounded() {
    auto version = load_catalogue(R"({"version":2,"models":[]})");
    CHECK(std::holds_alternative<ConfigError>(version) && std::get<ConfigError>(version).pointer == "/version");
    auto extra = load_catalogue(R"({"version":1,"models":[],"more":1})");
    CHECK(std::holds_alternative<ConfigError>(extra) && std::get<ConfigError>(extra).pointer == "/more");
    refused(R"([{"model":"x"}])", "/models/0/model");
    refused(R"([{"name":"x"}])", "/models/0/id");
    refused(R"([{"id":"has space"}])", "/models/0/id");
    refused(R"([{"id":""}])", "/models/0/id");
    refused(R"([{"id":"x","match":"glob"}])", "/models/0/match");
    refused(R"([{"id":"x","families":["openai.chat","bogus.family"]}])", "/models/0/families/1");
    refused(R"([{"id":"x","families":["openai.chat","openai.chat"]}])", "/models/0/families");
    refused(R"([{"id":"x","origins":["https://api.example.test/"]}])", "/models/0/origins/0");
    refused(R"([{"id":"x","origins":["ftp://api.example.test"]}])", "/models/0/origins/0");
    refused(R"([{"id":"x","match":"prefix","aliases":["y"]}])", "/models/0");
    refused(R"([{"id":"x","output_limit":0}])", "/models/0/output_limit");
    refused(R"([{"id":"x","pricing":{"prompt":"1e-7"}}])", "/models/0/pricing/prompt");
    refused(R"([{"id":"x","pricing":{"prompt":0.1}}])", "/models/0/pricing/prompt");
    refused(R"([{"id":"x","pricing":{"bad unit":"0.1"}}])", "/models/0/pricing/bad unit");
    refused(R"([{"id":"x","defaults":{"bogus.family":{}}}])", "/models/0/defaults/bogus.family");
    refused(R"([{"id":"x","defaults":{"openai.chat":{"max_tokens":1}}}])", "/models/0/defaults/openai.chat/max_tokens");
    refused(R"([{"id":"x","defaults":{"openai.chat":{"reasoning_enabled":null}}}])", "/models/0/defaults/openai.chat/reasoning_enabled");
    refused(R"([{"id":"x","temperature":"no"}])", "/models/0/temperature");
    refused(R"([{"id":"x"},{"id":"x"}])", "/models");
    // An exact entry and a profile may share an id; listed nulls mean not set.
    CHECK(catalogue(R"([{"id":"x"},{"id":"x","match":"prefix"},{"id":"y","name":null,"output_limit":null,"pricing":{"prompt":"-1","image":"0.04"}}])") != nullptr);
    std::string many = "[";
    for (int i = 0; i < 257; ++i) many += (i ? "," : "") + sp::json::quote("p" + std::to_string(i));
    refused(R"([{"id":"x","parameters":)" + many + "]}]", "/models/0/parameters");
}

void layers_merge_member_by_member() {
    const auto generated = catalogue(R"([
        {"id":"m","families":["openai.chat"],"output_limit":100,"pricing":{"prompt":"0.1","completion":"0.2"},
         "defaults":{"openai.chat":{"reasoning_effort":"low","max_output_tokens":50}},"source":"openrouter"},
        {"id":"gone"}])");
    const auto overrides = catalogue(R"([
        {"id":"m","output_limit":64,"pricing":{"completion":"0.3"},"defaults":{"openai.chat":{"max_output_tokens":null}},"source":"verified"},
        {"id":"extra","match":"prefix","temperature":false}])");
    if (!generated || !overrides) return;
    const CatalogueSnapshot layers[]{generated, nullptr, overrides};
    auto merged = merge_catalogues(layers);
    CHECK(std::holds_alternative<CatalogueSnapshot>(merged));
    if (!std::holds_alternative<CatalogueSnapshot>(merged)) return;
    const auto& c = std::get<CatalogueSnapshot>(merged);
    const auto* m = c->entry("m");
    CHECK(m && m->output_limit == 64u && m->families == std::vector<std::string>{"openai.chat"} && m->source == std::optional<std::string>("verified"));
    CHECK(m && m->pricing == (std::vector<std::pair<std::string, std::string>>{{"completion", "0.3"}, {"prompt", "0.1"}}));
    CHECK(m && m->defaults.size() == 1 && m->defaults[0].reasoning_effort == std::optional<std::optional<std::string>>(std::string("low")));
    CHECK(m && m->defaults[0].max_output_tokens.has_value() && !m->defaults[0].max_output_tokens->has_value());
    CHECK(c->entry("gone") && c->entry("extra", true) && c->entry("extra", true)->temperature == false);

    const auto diff = diff_catalogues(*generated, *c);
    CHECK(diff.added == std::vector<std::string>{"extra*"} && diff.removed.empty() && diff.changed == std::vector<std::string>{"m"});
    const auto back = diff_catalogues(*c, *generated);
    CHECK(back.removed == std::vector<std::string>{"extra*"} && back.added.empty());
    CHECK(diff_catalogues(*c, *c).empty());
}

// The text and the digest of a catalogue follow its content, not the process locale.
void canonical_text_ignores_the_locale() {
    constexpr std::string_view models = R"([{"id":"m","defaults":{"openai.chat":{"temperature":0.7,"top_p":0.95},"google.generate":{"temperature":1e-7}}}])";
    const auto reference = catalogue(models);
    if (!reference) return;
    const auto text = reference->to_json();
    CHECK(text.find("\"temperature\":0.7") != std::string::npos && text.find("\"top_p\":0.95") != std::string::npos);
    CHECK(text.find("\"temperature\":1e-07") != std::string::npos);

    const char* saved_name = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = saved_name ? saved_name : "C";
    bool comma = false;
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR.utf8", "fr_FR", "nl_NL.UTF-8", "nl_NL.utf8"}) {
        if (!std::setlocale(LC_NUMERIC, name)) continue;
        char probe[16];
        std::snprintf(probe, sizeof probe, "%.1f", 0.5);
        if (std::strchr(probe, ',')) { comma = true; break; }
    }
    if (!comma) {
        std::setlocale(LC_NUMERIC, saved.c_str());
        std::puts("note: no decimal-comma locale is installed; the locale check was skipped");
        return;
    }
    // Loading, writing, merging and rebuilding all give the text and digest the C locale gives.
    const auto loaded = catalogue(models);
    CHECK(loaded && loaded->to_json() == text && loaded->digest() == reference->digest());
    const CatalogueSnapshot layers[]{reference, reference};
    auto merged = merge_catalogues(layers);
    CHECK(std::holds_alternative<CatalogueSnapshot>(merged) && std::get<CatalogueSnapshot>(merged)->digest() == reference->digest());
    auto rebuilt = make_catalogue(reference->entries());
    CHECK(std::holds_alternative<CatalogueSnapshot>(rebuilt) && std::get<CatalogueSnapshot>(rebuilt)->digest() == reference->digest());
    CHECK(loaded && loaded->entry("m")->defaults.size() == 2 && loaded->entry("m")->defaults[1].temperature == std::optional<std::optional<double>>(0.7));
    std::setlocale(LC_NUMERIC, saved.c_str());
}

// Each list member of an entry is a set: the order it is listed in changes nothing.
void list_order_changes_nothing() {
    const auto ordered = catalogue(R"([{"id":"m","families":["anthropic.messages","openai.chat"],"origins":["https://a.example.test","https://b.example.test"],
        "aliases":["m-a","m-b"],"served_as":["m-x","m-y"],"input_modalities":["image","text"],"output_modalities":["audio","text"],
        "methods":["countTokens","generateContent"],"parameters":["temperature","tools","top_p"]}])");
    const auto reversed = catalogue(R"([{"id":"m","families":["openai.chat","anthropic.messages"],"origins":["https://b.example.test","https://a.example.test"],
        "aliases":["m-b","m-a"],"served_as":["m-y","m-x"],"input_modalities":["text","image"],"output_modalities":["text","audio"],
        "methods":["generateContent","countTokens"],"parameters":["top_p","tools","temperature"]}])");
    if (!ordered || !reversed) return;
    CHECK(ordered->digest() == reversed->digest() && ordered->to_json() == reversed->to_json());
    CHECK(diff_catalogues(*ordered, *reversed).empty());
    CHECK(reversed->entry("m")->parameters == std::vector<std::string>({"temperature", "tools", "top_p"}));
    CHECK(reversed->find("openai.chat", "https://b.example.test", "m-b").relation == ModelRelation::Alias);

    // Entries built in memory in any order give the same catalogue.
    std::mt19937 random(7);
    for (int round = 0; round < 24; ++round) {
        auto entries = ordered->entries();
        for (auto& entry : entries)
            for (auto* values : {&entry.families, &entry.origins, &entry.aliases, &entry.served_as, &entry.input_modalities,
                                 &entry.output_modalities, &entry.methods, &entry.parameters})
                std::shuffle(values->begin(), values->end(), random);
        auto built = make_catalogue(std::move(entries));
        CHECK(std::holds_alternative<CatalogueSnapshot>(built) && std::get<CatalogueSnapshot>(built)->digest() == ordered->digest());
    }

    // A provider that reorders what it lists does not change the entry it gives.
    auto page = [](std::string_view parameters) {
        auto listed = parse_listing(Listing::OpenRouter, std::string(R"({"data":[{"id":"vendor/m","architecture":{"input_modalities":)") +
                                    std::string(parameters) + "}}]}", "https://openrouter.ai");
        return std::get<ListingPage>(std::move(listed));
    };
    auto first = make_catalogue(page(R"(["text","image","audio"],"output_modalities":["text"],"x":0)").entries);
    auto second = make_catalogue(page(R"(["audio","text","image"],"output_modalities":["text"],"x":1)").entries);
    CHECK(std::holds_alternative<CatalogueSnapshot>(first) && std::holds_alternative<CatalogueSnapshot>(second));
    if (std::holds_alternative<CatalogueSnapshot>(first) && std::holds_alternative<CatalogueSnapshot>(second))
        CHECK(std::get<CatalogueSnapshot>(first)->digest() == std::get<CatalogueSnapshot>(second)->digest());
}

void policy_reads_the_catalogue() {
    const auto policy = builtin_policy();
    CHECK(policy->catalogue() == builtin_catalogue());
    const auto generate = descriptor("google.generate", "https://generativelanguage.googleapis.com", policy);
    CHECK(model_defaults(generate, "gemini-2.5-flash-lite").include_thoughts == std::optional<bool>(true));
    CHECK(model_defaults(generate, "gemini-2.5-flash-lite").max_output_tokens == generate.family_policy().defaults.max_output_tokens);
    CHECK(model_defaults(generate, "gemini-unlisted").include_thoughts == generate.family_policy().defaults.include_thoughts);
    CHECK(model_match(generate, "gemini-unlisted").relation == ModelRelation::Unknown);
    const auto interactions = descriptor("google.interactions", "https://generativelanguage.googleapis.com", policy);
    CHECK(model_defaults(interactions, "gemini-2.5-flash-lite").thinking_summaries == std::optional<bool>(true));
    CHECK(model_defaults(interactions, "gemini-2.5-flash-lite").service_tier == interactions.family_policy().defaults.service_tier);

    const auto chat = descriptor("openai.chat", "https://api.openai.com", policy);
    EffectiveChoices over_limit = effective_defaults(chat, "gpt-6-luna");
    over_limit.max_output_tokens = 128001;
    CHECK(validate_choices(chat, "gpt-6-luna", over_limit).has_value());
    CHECK(!validate_choices(chat, "gpt-6-luna-unlisted", over_limit).has_value());

    // A host catalogue: a profile for a family of ids, and facts the family rules do not have.
    const auto host = catalogue(R"([
        {"id":"acme-","match":"prefix","output_limit":100,"temperature":false,
         "defaults":{"openai.chat":{"reasoning_effort":"high","max_output_tokens":80}}},
        {"id":"gpt-5-sampled","temperature":true}])");
    if (!host) return;
    auto edited = policy->with_catalogue(host);
    CHECK(std::holds_alternative<PolicySnapshot>(edited));
    if (!std::holds_alternative<PolicySnapshot>(edited)) return;
    const auto& custom = std::get<PolicySnapshot>(edited);
    CHECK(custom->identity() == policy->identity() && custom->content_digest() != policy->content_digest());
    const auto custom_chat = descriptor("openai.chat", "https://api.openai.com", custom);
    CHECK(model_defaults(custom_chat, "acme-1").reasoning_effort == std::optional<std::string>("high"));
    CHECK(model_defaults(custom_chat, "acme-1").max_output_tokens == std::optional<std::uint64_t>(80));
    CHECK(custom->output_limit("openai.chat", "https://api.openai.com", "acme-1") == std::optional<std::uint64_t>(100));
    CHECK(temperature_forbidden(custom_chat, "acme-1"));
    CHECK(temperature_forbidden(custom_chat, "gpt-5-nano") && !temperature_forbidden(custom_chat, "gpt-5-sampled"));
    CHECK(model_defaults(custom_chat, "gpt-6-luna").reasoning_effort == chat.family_policy().defaults.reasoning_effort);

    // Entry defaults are held to the family rules; the refusal points into the catalogue.
    auto bad_effort = policy->with_catalogue(catalogue(R"([{"id":"acme/x","defaults":{"openai.chat":{"reasoning_effort":"bogus"}}}])"));
    CHECK(std::holds_alternative<ConfigError>(bad_effort) &&
          std::get<ConfigError>(bad_effort).pointer == "/model_catalogue/models/acme~1x/defaults/openai.chat");
    auto bad_strict = policy->with_catalogue(catalogue(R"([{"id":"y","defaults":{"openai.chat":{"strict_tools":true}}}])"));
    CHECK(std::holds_alternative<ConfigError>(bad_strict));
    auto bad_cap = policy->with_catalogue(catalogue(R"([{"id":"z","output_limit":10,"defaults":{"openai.chat":{"max_output_tokens":11}}}])"));
    CHECK(std::holds_alternative<ConfigError>(bad_cap));
    CHECK(std::holds_alternative<ConfigError>(policy->with_catalogue(nullptr)));

    // The policy file no longer lists models.
    auto parsed = sp::json::parse(sp::config_defaults::descriptor_policy_json);
    if (std::holds_alternative<sp::json::Document>(parsed)) {
        auto text = std::get<sp::json::Document>(parsed).root().dump();
        text.insert(text.size() - 1, R"(,"models":[])");
        CHECK(std::holds_alternative<ConfigError>(load_policy(text, sp::config_defaults::codec_defaults_json)));
    }

    auto overload = load_policy(sp::config_defaults::descriptor_policy_json, sp::config_defaults::codec_defaults_json, host);
    CHECK(std::holds_alternative<PolicySnapshot>(overload) && std::get<PolicySnapshot>(overload)->content_digest() == custom->content_digest());
}

void bundle_carries_the_catalogue() {
    const auto policy = builtin_policy();
    const auto bundle = builtin_policy_bundle_json();
    auto loaded = load_policy_bundle(bundle, policy->content_digest_hex());
    CHECK(std::holds_alternative<PolicySnapshot>(loaded));
    auto parsed = sp::json::parse(bundle);
    CHECK(std::holds_alternative<sp::json::Document>(parsed));
    if (!std::holds_alternative<sp::json::Document>(parsed)) return;
    const auto root = std::get<sp::json::Document>(parsed).root();
    auto with = [&](std::string_view catalogue_json) {
        return std::string(R"({"version":1,"descriptor_policy":)") + root.get("descriptor_policy").dump() +
               R"(,"codec_defaults":)" + root.get("codec_defaults").dump() +
               (catalogue_json.empty() ? std::string() : std::string(R"(,"model_catalogue":)") + std::string(catalogue_json)) + "}";
    };
    auto without = load_policy_bundle(with({}));
    CHECK(std::holds_alternative<PolicySnapshot>(without) && std::get<PolicySnapshot>(without)->content_digest() == policy->content_digest());
    auto custom = load_policy_bundle(with(R"({"version":1,"models":[{"id":"bundle-model","output_limit":7}]})"));
    CHECK(std::holds_alternative<PolicySnapshot>(custom));
    if (auto* snapshot = std::get_if<PolicySnapshot>(&custom)) {
        CHECK((*snapshot)->identity() == policy->identity() && (*snapshot)->content_digest() != policy->content_digest());
        CHECK((*snapshot)->output_limit("openai.chat", "https://api.openai.com", "bundle-model") == std::optional<std::uint64_t>(7));
        CHECK(!(*snapshot)->output_limit("openai.chat", "https://api.openai.com", "gpt-6-luna"));
    }
    auto broken = load_policy_bundle(with(R"({"version":1,"models":[{"id":"x","output_limit":0}]})"));
    CHECK(std::holds_alternative<ConfigError>(broken) && std::get<ConfigError>(broken).pointer == "/model_catalogue/models/0/output_limit");
    CHECK(std::holds_alternative<ConfigError>(load_policy_bundle(with("[]"))));
}

void catalogue_files() {
    std::filesystem::path dir;
    std::random_device entropy;
    for (int attempt = 0; attempt < 16 && dir.empty(); ++attempt) {
        const auto candidate = std::filesystem::temp_directory_path() /
                               ("sp_catalogue_test_" + std::to_string(entropy()) + "_" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error) && !error) dir = candidate;
    }
    CHECK(!dir.empty());
    if (dir.empty()) return;
    const auto file = dir / "models.json";
    std::ofstream(file, std::ios::binary) << builtin_catalogue()->to_json();
    auto loaded = load_catalogue_file(file.string());
    CHECK(std::holds_alternative<CatalogueSnapshot>(loaded) && std::get<CatalogueSnapshot>(loaded)->digest() == builtin_catalogue()->digest());
    CHECK(std::holds_alternative<ConfigError>(load_catalogue_file((dir / "missing.json").string())));
    CHECK(std::holds_alternative<ConfigError>(load_catalogue_file("")));
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}
} // namespace

int main() {
    try {
        builtin_catalogue_is_data();
        lookup_ranks_names_then_profiles();
        loader_is_closed_and_bounded();
        layers_merge_member_by_member();
        canonical_text_ignores_the_locale();
        list_order_changes_nothing();
        policy_reads_the_catalogue();
        bundle_carries_the_catalogue();
        catalogue_files();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "catalogue test threw: %s\n", error.what());
        return 1;
    }
    if (failures) std::fprintf(stderr, "%d catalogue checks failed\n", failures);
    else std::puts("model catalogue properties passed");
    return failures ? 1 : 0;
}
