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
// Refused at `pointer`, expecting `expected` there.
void refused_as(std::string_view models, std::string_view pointer, std::string_view expected) {
    auto loaded = load_catalogue(std::string(R"({"version":1,"models":)") + std::string(models) + "}");
    auto* error = std::get_if<ConfigError>(&loaded);
    if (!error) {
        ++failures;
        std::fprintf(stderr, "CHECK FAILED: loaded %s\n", std::string(models).c_str());
        return;
    }
    if (error->pointer != pointer || error->expected != expected) {
        ++failures;
        std::fprintf(stderr, "CHECK FAILED: %s refused at %s expecting \"%s\", not at %s expecting \"%s\"\n", std::string(models).c_str(),
                     error->pointer.c_str(), error->expected.c_str(), std::string(pointer).c_str(), std::string(expected).c_str());
    }
}
// A price key of one entry refused at its own pointer, where a '/' is written "~1".
void refused_key(std::string_view key, std::string_view expected) {
    std::string pointer = "/models/0/pricing/";
    for (const char c : key) pointer += c == '/' ? std::string("~1") : c == '~' ? std::string("~0") : std::string(1, c);
    refused_as(R"([{"id":"vendor/model","pricing":{)" + sp::json::quote(key) + R"(:"0.04"}}])", pointer, expected);
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
    // Origins are compared with a descriptor's canonical origin, so an entry lists them in that spelling.
    for (const std::string_view spelling : {"https://API.example.test", "https://api.example.test:443", "http://127.0.0.1:80"})
        refused_as(std::string(R"([{"id":"x","origins":["https://a.example.test",")") + std::string(spelling) + R"("]}])",
                   "/models/0/origins/1", "origin in its canonical spelling: lower-case host, no default port");
    refused_as(R"([{"id":"x","origins":["https://api.example.test/"]}])", "/models/0/origins/0", "canonical origin without path");
    CHECK(std::holds_alternative<CatalogueSnapshot>(load_catalogue(R"({"version":1,"models":[{"id":"x","origins":["https://api.example.test:8443","http://127.0.0.1:443"]}]})")));
    for (const std::string_view spelling : {"https://OpenRouter.ai", "https://openrouter.ai:443"}) {
        const auto listed = parse_listing(Listing::OpenRouter, R"({"data":[]})", spelling);
        const auto* error = std::get_if<ConfigError>(&listed);
        CHECK(error && error->expected == "origin in its canonical spelling: lower-case host, no default port");
    }
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
    refused(R"([{"id":"x","match":"prefix"},{"id":"x","match":"prefix"}])", "/models");
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

    // A host catalogue, merged over the embedded one: a profile for a family of ids, and facts the embedded data does
    // not have. An exact entry that states temperature decides it over the embedded profile.
    const auto host = catalogue(R"([
        {"id":"acme-","match":"prefix","output_limit":100,"temperature":false,
         "defaults":{"openai.chat":{"reasoning_effort":"high","max_output_tokens":80}}},
        {"id":"gpt-5-sampled","temperature":true}])");
    if (!host) return;
    const CatalogueSnapshot layers[]{builtin_catalogue(), host};
    auto merged = merge_catalogues(layers);
    CHECK(std::holds_alternative<CatalogueSnapshot>(merged));
    if (!std::holds_alternative<CatalogueSnapshot>(merged)) return;
    const auto& with_builtin = std::get<CatalogueSnapshot>(merged);
    auto edited = policy->with_catalogue(with_builtin);
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
    // with_catalogue replaces the catalogue: a host catalogue attached without the embedded one has no embedded
    // temperature profiles, so the provider answers for those ids.
    auto replaced = policy->with_catalogue(host);
    CHECK(std::holds_alternative<PolicySnapshot>(replaced));
    if (std::holds_alternative<PolicySnapshot>(replaced)) {
        const auto replaced_chat = descriptor("openai.chat", "https://api.openai.com", std::get<PolicySnapshot>(replaced));
        CHECK(!temperature_forbidden(replaced_chat, "gpt-5-nano") && temperature_forbidden(replaced_chat, "acme-1"));
    }

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

    auto overload = load_policy(sp::config_defaults::descriptor_policy_json, sp::config_defaults::codec_defaults_json, with_builtin);
    CHECK(std::holds_alternative<PolicySnapshot>(overload) && std::get<PolicySnapshot>(overload)->content_digest() == custom->content_digest());
}

// A fact comes from the first entry that states it, in lookup order. An exact entry, or a longer profile, that says
// nothing about temperature does not hide the profile that does. Defaults and limits still come from the best match.
void exact_entry_does_not_hide_a_profile_fact() {
    const auto host = catalogue(R"([
        {"id":"profiled-","match":"prefix","temperature":false},
        {"id":"profiled-exact","output_limit":64},
        {"id":"profiled-longer-","match":"prefix","output_limit":32},
        {"id":"profiled-stated","temperature":true}])");
    if (!host) return;
    auto attached = builtin_policy()->with_catalogue(host);
    CHECK(std::holds_alternative<PolicySnapshot>(attached));
    if (!std::holds_alternative<PolicySnapshot>(attached)) return;
    const auto chat = descriptor("openai.chat", "https://api.example.test", std::get<PolicySnapshot>(attached));
    const auto exact = model_match(chat, "profiled-exact");
    CHECK(exact.entry && exact.relation == ModelRelation::Exact && exact.entry->output_limit == 64u);
    const auto longer = model_match(chat, "profiled-longer-model");
    CHECK(longer.entry && longer.relation == ModelRelation::Prefix && longer.entry->output_limit == 32u);
    for (const auto* model : {"profiled-exact", "profiled-longer-model", "profiled-other"}) {
        if (!temperature_forbidden(chat, model)) std::fprintf(stderr, "  temperature admitted for %s\n", model);
        CHECK(temperature_forbidden(chat, model));
        auto choices = effective_defaults(chat, model);
        choices.temperature = 0.5;
        const auto refused = validate_choices(chat, model, choices);
        CHECK(refused && *refused == "temperature is prohibited for the admitted model");
    }
    // An entry that states the fact decides it, over the profile.
    CHECK(!temperature_forbidden(chat, "profiled-stated"));
    // An entry that lists other families only does not state the fact for this one.
    const auto other_family = catalogue(R"([{"id":"profiled-elsewhere","families":["anthropic.messages"],"temperature":false}])");
    if (!other_family) return;
    auto elsewhere = builtin_policy()->with_catalogue(other_family);
    CHECK(std::holds_alternative<PolicySnapshot>(elsewhere));
    if (const auto* snapshot = std::get_if<PolicySnapshot>(&elsewhere)) {
        CHECK(model_match(descriptor("openai.chat", "https://api.example.test", *snapshot), "profiled-elsewhere").entry);
        CHECK(!(*snapshot)->temperature_admitted("openai.chat", "https://api.example.test", "profiled-elsewhere"));
        CHECK((*snapshot)->temperature_admitted("anthropic.messages", "https://api.example.test", "profiled-elsewhere") == false);
    }
}

// A model listing's supported parameters state the temperature fact at the listing's origin: the parameter is listed
// or it is not. An empty list states nothing, and the entry does not apply at another origin.
void listed_parameters_state_temperature() {
    auto fact = [](std::string_view parameters, std::string_view origin) -> std::optional<bool> {
        const auto listed = parse_listing(Listing::OpenRouter,
            std::string(R"({"data":[{"id":"vendor/listed-model","architecture":{"output_modalities":["text"]})") +
            std::string(parameters) + "}]}", "https://openrouter.ai");
        const auto* page = std::get_if<ListingPage>(&listed);
        CHECK(page && page->entries.size() == 1);
        if (!page || page->entries.size() != 1) return std::nullopt;
        auto built = make_catalogue(page->entries);
        CHECK(std::holds_alternative<CatalogueSnapshot>(built));
        if (!std::holds_alternative<CatalogueSnapshot>(built)) return std::nullopt;
        auto attached = builtin_policy()->with_catalogue(std::get<CatalogueSnapshot>(std::move(built)));
        CHECK(std::holds_alternative<PolicySnapshot>(attached));
        if (!std::holds_alternative<PolicySnapshot>(attached)) return std::nullopt;
        const auto& policy = std::get<PolicySnapshot>(attached);
        const auto answer = policy->temperature_admitted("openai.chat", origin, "vendor/listed-model");
        CHECK(temperature_forbidden(descriptor("openai.chat", origin, policy), "vendor/listed-model") == (answer == false));
        return answer;
    };
    const std::string_view gateway = "https://openrouter.ai";
    CHECK(fact(R"(,"supported_parameters":["max_tokens","tools"])", gateway) == false);
    CHECK(fact(R"(,"supported_parameters":["temperature","max_tokens"])", gateway) == true);
    CHECK(!fact(R"(,"supported_parameters":[])", gateway));
    CHECK(!fact("", gateway));
    CHECK(!fact(R"(,"supported_parameters":["max_tokens","tools"])", "https://api.example.test"));
    // In a file, `temperature` is read before `parameters`.
    const auto both = catalogue(R"([{"id":"vendor/listed-model","parameters":["max_tokens"],"temperature":true}])");
    if (!both) return;
    auto attached = builtin_policy()->with_catalogue(both);
    CHECK(std::holds_alternative<PolicySnapshot>(attached) &&
          std::get<PolicySnapshot>(attached)->temperature_admitted("openai.chat", gateway, "vendor/listed-model") == true);
}

// Attaching a catalogue checks its control values. Each entry's defaults, over every family, must be admitted by the
// list of the entry that decides the control for the entry's models at each origin where it applies, whichever entry
// that is; and an entry's list must share a value with each family it applies to that lists values for the control.
void control_values_meet_defaults_and_families() {
    auto attach = [](const std::string& models) {
        const auto c = catalogue(models);
        if (!c) return std::string("catalogue refused");
        auto attached = builtin_policy()->with_catalogue(c);
        if (const auto* error = std::get_if<ConfigError>(&attached)) return error->pointer + " | " + error->expected;
        return std::string("attached");
    };
    auto pointer_of = [&](const std::string& models) { const auto text = attach(models); return text.substr(0, text.find(" | ")); };
    // Another entry's list decides: the profile's list leaves out the exact entry's default.
    const std::string profile = R"({"id":"x-","match":"prefix","families":["google.interactions"],"control_values":{"thinking_level":["low"]}})";
    const auto refused_default = attach("[" + profile + R"(,{"id":"x-model","families":["google.interactions"],"defaults":{"google.interactions":{"thinking_level":"high"}}}])");
    if (refused_default.rfind("/model_catalogue/models/x-model/", 0) != 0) std::fprintf(stderr, "  attach gave: %s\n", refused_default.c_str());
    CHECK(refused_default == "/model_catalogue/models/x-model/defaults/google.interactions | a default thinking_level that prefix profile x- admits in control_values");
    CHECK(attach("[" + profile + R"(,{"id":"x-model","families":["google.interactions"],"defaults":{"google.interactions":{"thinking_level":"low"}}}])") == "attached");
    // Origins: a profile scoped to one origin meets an unscoped entry there, and not an entry scoped to another origin.
    const std::string scoped = R"({"id":"x-","match":"prefix","origins":["https://a.example.test"],"control_values":{"thinking_level":["low"]}})";
    CHECK(pointer_of("[" + scoped + R"(,{"id":"x-model","defaults":{"google.interactions":{"thinking_level":"high"}}}])") ==
          "/model_catalogue/models/x-model/defaults/google.interactions");
    CHECK(attach("[" + scoped + R"(,{"id":"x-model","origins":["https://b.example.test"],"defaults":{"google.interactions":{"thinking_level":"high"}}}])") == "attached");
    // A profile's defaults meet the shorter profiles: an exact entry with the profile's id names other models.
    CHECK(attach(R"([{"id":"x-","match":"prefix","control_values":{"thinking_level":["low"]}},{"id":"x-model-","control_values":{"thinking_level":["high"]}},
        {"id":"x-model-","match":"prefix","defaults":{"google.interactions":{"thinking_level":"high"}}}])") ==
          "/model_catalogue/models/x-model-/defaults/google.interactions | a default thinking_level that prefix profile x- admits in control_values");
    // The entry's own list decides first.
    CHECK(attach(R"([{"id":"x-","match":"prefix","control_values":{"thinking_level":["low"]}},{"id":"x-model","control_values":{"thinking_level":["high"]},
        "defaults":{"google.interactions":{"thinking_level":"high"}}}])") == "attached");

    // A list no value of which a family lists, for a family the entry applies to that lists values for the control.
    CHECK(attach(R"([{"id":"x-model","families":["google.interactions"],"control_values":{"thinking_level":["ultra"]}}])") ==
          "/model_catalogue/models/x-model/control_values/thinking_level | values the google.interactions family lists");
    CHECK(attach(R"([{"id":"x-model","families":["google.interactions"],"control_values":{"thinking_level":["ultra","low"]}}])") == "attached");
    CHECK(attach(R"([{"id":"x-model","families":["openai.chat"],"control_values":{"thinking_level":["ultra"]}}])") == "attached");
    CHECK(attach(R"([{"id":"x-model","control_values":{"thinking_level":["ultra"]}}])") ==
          "/model_catalogue/models/x-model/control_values/thinking_level | values the google.generate family lists");
}

// The embedded temperature profiles reproduce the refusals of the retired family lists, each at its provider's own
// origin and for the families that provider serves, and no embedded entry states control values.
void embedded_temperature_profiles() {
    const auto builtin = builtin_catalogue();
    std::size_t profiles = 0;
    for (const auto& entry : builtin->entries()) {
        CHECK(entry.control_values.empty());
        if (!entry.temperature) continue;
        ++profiles;
        if (entry.origins.size() != 1) std::fprintf(stderr, "  temperature profile %s is not scoped to one origin\n", entry.id.c_str());
        CHECK(entry.prefix && entry.temperature == false && entry.source == std::optional<std::string>("verified"));
        CHECK(entry.origins.size() == 1 && !entry.families.empty());
        const bool anthropic = entry.origins.size() == 1 && entry.origins[0] == "https://api.anthropic.com";
        const bool openai = entry.origins.size() == 1 && entry.origins[0] == "https://api.openai.com";
        CHECK(anthropic || openai);
        CHECK(entry.families == (anthropic ? std::vector<std::string>{"anthropic.messages"} : std::vector<std::string>{"openai.chat", "openai.responses"}));
    }
    CHECK(profiles == 11);
    CHECK(builtin->to_json().find("control_values") == std::string::npos);
}

// The embedded profiles take part in find() too, which picks defaults, limits and prices. A host's shorter profile is
// shadowed at the embedded profile's origin for the ids it covers, which have no price there; elsewhere, and for the
// ids no embedded profile covers, the host's profile still answers.
void embedded_profiles_shadow_shorter_ones() {
    const auto host = catalogue(R"([{"id":"gpt-","match":"prefix","output_limit":1000,"pricing":{"input":"0.000001","output":"0.000002"}}])");
    if (!host) return;
    const CatalogueSnapshot layers[]{host, builtin_catalogue()};
    auto merged = merge_catalogues(layers);
    CHECK(std::holds_alternative<CatalogueSnapshot>(merged));
    if (!std::holds_alternative<CatalogueSnapshot>(merged)) return;
    const auto& c = std::get<CatalogueSnapshot>(merged);
    const auto direct = c->find("openai.chat", "https://api.openai.com", "gpt-5-mini");
    CHECK(direct.entry && direct.relation == ModelRelation::Prefix && direct.entry->id == "gpt-5");
    CHECK(direct.entry && direct.entry->pricing.empty() && !direct.entry->output_limit);
    const auto elsewhere = c->find("openai.chat", "https://api.example.test", "gpt-5-mini");
    CHECK(elsewhere.entry && elsewhere.entry->id == "gpt-" && elsewhere.entry->output_limit == 1000u && !elsewhere.entry->pricing.empty());
    const auto uncovered = c->find("openai.chat", "https://api.openai.com", "gpt-4o");
    CHECK(uncovered.entry && uncovered.entry->id == "gpt-" && !uncovered.entry->pricing.empty());
}

// `control_values` lists, by control, the values a model takes. Each list is a set, the member is written only when
// set, and a later layer replaces one control's list and keeps the others.
void control_values_by_control() {
    const auto c = catalogue(R"([{"id":"x-model","control_values":{"thinking_level":["high","low"],"service_tier":["flex"]}}])");
    if (!c) return;
    const auto* entry = c->entry("x-model");
    using Values = std::vector<std::pair<std::string, std::vector<std::string>>>;
    CHECK(entry && entry->control_values == (Values{{"service_tier", {"flex"}}, {"thinking_level", {"high", "low"}}}));
    CHECK(c->to_json().find(R"("control_values":{"service_tier":["flex"],"thinking_level":["high","low"]})") != std::string::npos);
    auto again = load_catalogue(c->to_json());
    CHECK(std::holds_alternative<CatalogueSnapshot>(again) && std::get<CatalogueSnapshot>(again)->digest() == c->digest());
    auto rebuilt = make_catalogue(c->entries());
    CHECK(std::holds_alternative<CatalogueSnapshot>(rebuilt) && std::get<CatalogueSnapshot>(rebuilt)->digest() == c->digest());
    const auto reordered = catalogue(R"([{"id":"x-model","control_values":{"service_tier":["flex"],"thinking_level":["low","high"]}}])");
    CHECK(reordered && reordered->digest() == c->digest());
    const auto plain = catalogue(R"([{"id":"x-model"}])");
    CHECK(plain && plain->to_json().find("control_values") == std::string::npos);

    refused(R"([{"id":"x","control_values":[]}])", "/models/0/control_values");
    refused(R"([{"id":"x","control_values":{"temperature":["low"]}}])", "/models/0/control_values/temperature");
    refused(R"([{"id":"x","control_values":{"max_output_tokens":["1"]}}])", "/models/0/control_values/max_output_tokens");
    refused(R"([{"id":"x","control_values":{"thinking":["low"]}}])", "/models/0/control_values/thinking");
    refused(R"([{"id":"x","control_values":{"thinking_level":[]}}])", "/models/0/control_values/thinking_level");
    refused(R"([{"id":"x","control_values":{"thinking_level":"low"}}])", "/models/0/control_values/thinking_level");
    refused(R"([{"id":"x","control_values":{"thinking_level":["low","low"]}}])", "/models/0/control_values/thinking_level");
    refused(R"([{"id":"x","control_values":{"thinking_level":["not a token"]}}])", "/models/0/control_values/thinking_level/0");
    std::string many = "[";
    for (int i = 0; i < 17; ++i) many += (i ? "," : "") + sp::json::quote("v" + std::to_string(i));
    refused(R"([{"id":"x","control_values":{"service_tier":)" + many + "]}}]", "/models/0/control_values/service_tier");

    const auto earlier = catalogue(R"([{"id":"m","control_values":{"thinking_level":["low"],"service_tier":["flex"]}}])");
    const auto later = catalogue(R"([{"id":"m","control_values":{"thinking_level":["high","medium"]}},{"id":"n","control_values":{"reasoning_effort":["low"]}}])");
    if (!earlier || !later) return;
    const CatalogueSnapshot layers[]{earlier, later};
    auto merged = merge_catalogues(layers);
    CHECK(std::holds_alternative<CatalogueSnapshot>(merged));
    if (!std::holds_alternative<CatalogueSnapshot>(merged)) return;
    const auto& both = std::get<CatalogueSnapshot>(merged);
    CHECK(both->entry("m") && both->entry("m")->control_values == (Values{{"service_tier", {"flex"}}, {"thinking_level", {"high", "medium"}}}));
    CHECK(both->entry("n") && both->entry("n")->control_values == (Values{{"reasoning_effort", {"low"}}}));
    const auto diff = diff_catalogues(*earlier, *both);
    CHECK(diff.changed == std::vector<std::string>{"m"} && diff.added == std::vector<std::string>{"n"});
}

// A layer's origins scope the whole entry. A layer without origins applies at every origin, so over
// a listing's entry it widens the scope instead of inheriting it.
void origins_scope_the_whole_entry() {
    const std::string_view listing_origin = "https://api.example.test", gateway = "https://gateway.example.test";
    const auto listed = catalogue(R"([
        {"id":"m","origins":["https://api.example.test"],"name":"Listed M","input_limit":1000,"source":"openai"},
        {"id":"only-listed","origins":["https://api.example.test"],"source":"openai"}])");
    const auto verified = catalogue(R"([{"id":"m","served_as":["m-2026"],"output_limit":64,"source":"verified"}])");
    const auto scoped = catalogue(R"([{"id":"m","origins":["https://other.example.test"],"output_limit":32}])");
    if (!listed || !verified || !scoped) return;

    const CatalogueSnapshot widening[]{listed, verified};
    auto widened = merge_catalogues(widening);
    CHECK(std::holds_alternative<CatalogueSnapshot>(widened));
    if (std::holds_alternative<CatalogueSnapshot>(widened)) {
        const auto& c = std::get<CatalogueSnapshot>(widened);
        const auto* m = c->entry("m");
        CHECK(m && m->origins.empty() && m->name == std::optional<std::string>("Listed M") && m->output_limit == 64u);
        // The verified facts match at the listing's origin and at a gateway.
        for (const auto origin : {listing_origin, gateway}) {
            const auto exact = c->find("openai.chat", origin, "m");
            CHECK(exact.entry && exact.relation == ModelRelation::Exact && exact.entry->served_as == std::vector<std::string>{"m-2026"});
        }
        // An entry only the listing names keeps the listing's origin.
        CHECK(c->entry("only-listed")->origins == std::vector<std::string>{"https://api.example.test"});
        CHECK(!c->find("openai.chat", gateway, "only-listed").entry);
    }

    // Origins in the later layer replace the earlier ones; an unscoped earlier layer takes them on.
    const CatalogueSnapshot replacing[]{listed, scoped};
    auto replaced = merge_catalogues(replacing);
    CHECK(std::holds_alternative<CatalogueSnapshot>(replaced) &&
          std::get<CatalogueSnapshot>(replaced)->entry("m")->origins == std::vector<std::string>{"https://other.example.test"});
    const CatalogueSnapshot narrowing[]{verified, scoped};
    auto narrowed = merge_catalogues(narrowing);
    CHECK(std::holds_alternative<CatalogueSnapshot>(narrowed) &&
          std::get<CatalogueSnapshot>(narrowed)->entry("m")->origins == std::vector<std::string>{"https://other.example.test"});
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

// The price reader, the unit names and the bounds the listing reader shares with pricing, and the shapes of
// `pricing.overrides` it reads.
void listed_override_bounds() {
    // 2^64 - 1 nano-USD per million units is the largest amount; past the fifteenth fractional digit it rounds up.
    CHECK(price_per_million("18446.744073709551615") == std::optional<std::uint64_t>(18446744073709551615u));
    CHECK(!price_per_million("18446.744073709551616"));
    CHECK(price_per_million("18446.7440737095516141") == std::optional<std::uint64_t>(18446744073709551615u));
    CHECK(!price_per_million("18446.7440737095516151"));
    CHECK(price_per_million("0.0000000000000011") == std::optional<std::uint64_t>(2u));
    CHECK(price_per_million("0.000003") == std::optional<std::uint64_t>(3'000'000'000u) && price_per_million("5") == std::optional<std::uint64_t>(5'000'000'000'000'000u));
    CHECK(!price_per_million("-1") && !price_per_million("1e-7") && !price_per_million("1.2.3") && !price_per_million("") && !price_per_million(" 1"));
    CHECK(plain_price_unit("input_cache_write_1h") == "cache_write_1h" && plain_price_unit("input_cache_write") == "cache_write" &&
          plain_price_unit("prompt") == "input" && plain_price_unit("image") == "image");
    CHECK(unbanded_unit("prompt+272000") == "prompt" && unbanded_unit("prompt") == "prompt" && unbanded_unit("prompt+x") == "prompt+x" &&
          unbanded_unit("prompt+") == "prompt+" && unbanded_unit("prompt+12345678901234567890") == "prompt+12345678901234567890");
    using Prices = std::vector<std::pair<std::string, std::string>>;
    const auto page = [](std::string_view pricing) {
        auto parsed = parse_listing(Listing::OpenRouter, std::string(R"({"data":[{"id":"vendor/m","pricing":)") + std::string(pricing) + "}]}",
                                    "https://openrouter.ai");
        return std::get<ListingPage>(std::move(parsed));
    };
    const auto threshold = [&](std::string_view tokens) {
        return page(std::string(R"({"prompt":"0.000001","completion":"0.000002","overrides":[{"min_prompt_tokens":)") + std::string(tokens) +
                    R"(,"prompt":"0.000002","completion":null}]})");
    };
    // A threshold is a count of input tokens from 1 to the largest band of 19 digits.
    const auto widest = threshold("9999999999999999999");
    CHECK(widest.entries[0].pricing == (Prices{{"completion", "0.000002"}, {"prompt", "0.000001"}, {"prompt+9999999999999999999", "0.000002"}}));
    CHECK(widest.skipped_override_prices == 0 && widest.withheld.empty());
    CHECK(threshold("1").entries[0].pricing == (Prices{{"completion", "0.000002"}, {"prompt", "0.000001"}, {"prompt+1", "0.000002"}}));
    for (const char* tokens : {"10000000000000000000", "0", "-1", "1.5", "1e3", "\"1000\"", "true", "null"}) {
        const auto read = threshold(tokens);
        CHECK(read.entries[0].pricing == (Prices{{"completion", "0.000002"}}));
        CHECK(read.skipped_override_prices == 1 && (read.withheld == std::vector<WithheldUnit>{{"vendor/m", "prompt"}}));
    }
    // A listed price key is a unit name in the provider's words: a tier or a band of the grammar, a route ceiling and a
    // tool fee are skipped prices, in the base prices and in overrides. A listed band is skipped, and an override at
    // that threshold gives its own band.
    const auto listed_band = page(R"({"prompt":"10","prompt+128000":"2","overrides":[{"min_prompt_tokens":128000,"prompt":"8"}]})");
    CHECK(listed_band.entries[0].pricing == (Prices{{"prompt", "10"}, {"prompt+128000", "8"}}) && listed_band.skipped_prices == 1);
    const auto reserved = page(R"({"prompt":"0.000001","completion":"0.000002","prompt/flex":"0.1","ceiling_input":"0.000001",)"
                               R"("tool:web_search":"0.01","overrides":[{"utc_start":0,"utc_end":600,"ceiling_output":"0.1","tool:x":"1",)"
                               R"("prompt/flex":"2","prompt+1000":"3","prompt":"0.000003"}]})");
    CHECK(reserved.entries[0].pricing == (Prices{{"completion", "0.000002"}, {"prompt", "0.000003"}}));
    CHECK(reserved.skipped_prices == 3 + 4 && reserved.skipped_override_prices == 0 && reserved.window_prices_at_dearest == 1 && reserved.withheld.empty());
    // A band whose unit name is too long to hold is not held.
    const std::string wide(60, 'x');
    CHECK(page(R"({")" + wide + R"(":"0.000001","completion":"0.000002","overrides":[{"min_prompt_tokens":1000,")" + wide + R"(":"0.000002"}]})")
              .entries[0].pricing == (Prices{{"completion", "0.000002"}}));
    // Overrides that are not an array, or an entry that is not an object, cannot be read: every price is left out.
    for (const char* overrides : {R"({"min_prompt_tokens":1000})", R"("0.1")", R"([{"min_prompt_tokens":1000,"prompt":"0.000002"},7])"}) {
        const auto read = page(std::string(R"({"prompt":"0.000001","completion":"0.000002","overrides":)") + overrides + "}");
        CHECK(read.entries[0].pricing.empty() && read.skipped_override_prices == 1 && (read.withheld == std::vector<WithheldUnit>{{"vendor/m", ""}}));
    }
    // An id listed twice keeps out what an override of either listing left out: here the cache read and the prompt
    // price its tokens would otherwise be billed at. A unit only one copy prices is not claimed either.
    auto twice = parse_listing(Listing::OpenRouter, R"({"data":[{"id":"vendor/m","pricing":{"prompt":"0.000001",)"
        R"("overrides":[{"utc_start":0,"utc_end":600,"input_cache_read":"0.00001"}]}},)"
        R"({"id":"vendor/m","pricing":{"prompt":"0.000001","completion":"0.000002","input_cache_read":"0.0000001"}}]})", "https://openrouter.ai");
    const auto& merged = std::get<ListingPage>(twice);
    CHECK(merged.entries.size() == 1 && merged.entries[0].pricing.empty());
    CHECK((merged.withheld == std::vector<WithheldUnit>{{"vendor/m", "completion"}, {"vendor/m", "input_cache_read"}, {"vendor/m", "prompt"}}));
    // Two copies of an id that price a unit with the same bands keep the dearer price of each; a unit whose bands differ
    // between the copies, or that one copy leaves out, is not claimed (with the units its tokens would be billed at).
    auto copies = parse_listing(Listing::OpenRouter, R"({"data":[)"
        R"({"id":"vendor/m","pricing":{"prompt":"0.000002","completion":"0.000004","input_cache_read":"0.0000002","image":"0.01",)"
        R"("overrides":[{"min_prompt_tokens":1000,"prompt":"0.000003"}]}},)"
        R"({"id":"vendor/m","pricing":{"prompt":"0.000001","completion":"0.000005","input_cache_read":"0.0000002",)"
        R"("overrides":[{"min_prompt_tokens":1000,"prompt":"0.000004"},{"min_prompt_tokens":2000,"completion":"0.000006"}]}}]})", "https://openrouter.ai");
    const auto& dearer = std::get<ListingPage>(copies);
    CHECK(dearer.entries.size() == 1 &&
          dearer.entries[0].pricing == (Prices{{"input_cache_read", "0.0000002"}, {"prompt", "0.000002"}, {"prompt+1000", "0.000004"}}));
    CHECK((dearer.withheld == std::vector<WithheldUnit>{{"vendor/m", "completion"}, {"vendor/m", "image"}}));
    // The generated entries load as a catalogue.
    CHECK(std::holds_alternative<CatalogueSnapshot>(make_catalogue(widest.entries)));
}

// Item units: every kind, with and without a qualifier, a tier and a band, each held in its one spelling.
void item_units_load() {
    const auto c = catalogue(R"([{"id":"vendor/model","pricing":{"input":"0.000001","output":"0.000002",
        "item:image":"0.04",
        "item:image[quality=high,size=1024x1024]":"0.08",
        "item:image[quality=high,size=1024x1024]/batch":"0.04",
        "item:image[size=1024x1024]+200000":"0.1",
        "item:image[aspect_ratio=16:9,operation=generate]/batch+200000":"0.2",
        "item:image/flex+1":"0",
        "item:input_image":"0.01",
        "item:character":"0.000015",
        "item:second[audio=true,resolution=720p]":"0.1",
        "item:document":"0.001",
        "item:search_unit/flex":"0.002"}}])");
    if (!c) return;
    CHECK(c->entry("vendor/model")->pricing.size() == 13);
    // The canonical text holds each unit as written, and reads back as the same catalogue.
    const auto text = c->to_json();
    CHECK(text.find(R"("item:image[aspect_ratio=16:9,operation=generate]/batch+200000":"0.2")") != std::string::npos);
    auto again = load_catalogue(text);
    CHECK(std::holds_alternative<CatalogueSnapshot>(again) && std::get<CatalogueSnapshot>(again)->to_json() == text &&
          std::get<CatalogueSnapshot>(again)->digest() == c->digest());
    auto rebuilt = make_catalogue(c->entries());
    CHECK(std::holds_alternative<CatalogueSnapshot>(rebuilt) && std::get<CatalogueSnapshot>(rebuilt)->digest() == c->digest());
    // The parts of a unit.
    const auto full = item_price_key("item:image[aspect_ratio=16:9,operation=generate]/batch+200000");
    CHECK(full && full->unit == "item:image" && full->qualifier == "aspect_ratio=16:9,operation=generate" && full->dimensions == 2 &&
          full->tier == "batch" && full->above == 200000u);
    const auto bare = item_price_key("item:character");
    CHECK(bare && bare->unit == "item:character" && bare->qualifier.empty() && bare->dimensions == 0 && bare->tier.empty() && bare->above == 0);
    const auto widest = item_price_key("item:image[a=b]+9999999999999999999");
    CHECK(widest && widest->above == 9999999999999999999u && widest->dimensions == 1 && widest->tier.empty());
    for (const auto& kind : item_kinds) {
        const auto unit = std::string(item_unit_prefix) + std::string(kind.name);
        CHECK(item_price_key(unit) && item_price_key(unit)->unit == unit && !kind.counts.empty());
        CHECK(catalogue(R"([{"id":"vendor/model","pricing":{)" + sp::json::quote(unit) + R"(:"1"}}])") != nullptr);
    }
    CHECK(!item_price_key("input") && !item_price_key("tool:x") && !item_price_key("item:video") && !item_price_key("item:image[size=b,quality=a]") &&
          !item_price_key("item:image/") && !item_price_key("item:image+0") && !item_price_key("ceiling_item:image"));
    // A qualifier matches a call that offers each dimension it names with the same value.
    using Dimensions = std::vector<std::pair<std::string, std::string>>;
    const Dimensions offered{{"quality", "high"}, {"size", "1024x1024"}};
    CHECK(qualifier_offered("", offered) && qualifier_offered("", Dimensions{}));
    CHECK(qualifier_offered("quality=high", offered) && qualifier_offered("quality=high,size=1024x1024", offered) && qualifier_offered("size=1024x1024", offered));
    CHECK(!qualifier_offered("quality=low", offered) && !qualifier_offered("background=opaque", offered) && !qualifier_offered("quality=high", Dimensions{}));
    CHECK(!qualifier_offered("quality=High", offered) && !qualifier_offered("quality=hig", offered) && !qualifier_offered("quality=high,size=512x512", offered));
}

// A price key that starts with `item:` or holds a qualifier is refused unless it is an item unit in its one spelling:
// the unit, the qualifier, the tier and the band in that order, the qualifier's keys sorted and unique. Each refusal is
// at the key's pointer.
void item_units_refused() {
    constexpr std::string_view too_long = "item price unit of at most 256 bytes";
    constexpr std::string_view not_item = "a qualifier on an item unit only";
    constexpr std::string_view kind = "item unit of a known kind";
    constexpr std::string_view shape = "qualifier of key=value dimensions in brackets";
    constexpr std::string_view key = "qualifier key of 1 to 64 lower-case letters, digits or '_'";
    constexpr std::string_view value = "qualifier value of 1 to 64 letters, digits, ':', '.', '_' or '-'";
    constexpr std::string_view count = "at most 8 qualifier dimensions";
    constexpr std::string_view sorted = "qualifier keys sorted and unique";
    constexpr std::string_view order = "item unit, qualifier, tier and band in that order";
    constexpr std::string_view tier = "service tier of 1 to 64 letters, digits, ':', '.', '_' or '-'";
    constexpr std::string_view band = "input band of 1 to 19 digits without a leading zero";
    refused_as(R"([{"id":"vendor/model","pricing":{"item:image/batch[size=a]":"1"}}])", "/models/0/pricing/item:image~1batch[size=a]", order);
    // Canonical order: unsorted or repeated keys are refused, not reordered.
    refused_key("item:image[size=1024x1024,quality=high]", sorted);
    refused_key("item:image[size=a,size=b]", sorted);
    refused_key("item:image[quality=high,quality=high]", sorted);
    // Values from the request-token alphabet, keys of lower-case letters, digits and '_'.
    refused_key("item:image[size=]", value);
    refused_key("item:image[size=a/b]", value);
    refused_key("item:image[size=a+b]", value);
    refused_key("item:image[size=a[b]", value);
    refused_key("item:image[size=a=b]", value);
    refused_key("item:image[size=" + std::string(65, 'v') + "]", value);
    refused_key("item:image[Size=a]", key);
    refused_key("item:image[=a]", key);
    refused_key("item:image[size-x=a]", key);
    refused_key("item:image[" + std::string(65, 'k') + "=a]", key);
    refused_key("item:image[size=a,b]", shape);
    refused_key("item:image[]", shape);
    refused_key("item:image[size=a", shape);
    refused_key("item:image[size]", shape);
    refused_key("item:image[size=a,]", shape);
    refused_key("item:image[,size=a]", shape);
    refused_key("item:image[size=a]x", shape);
    refused_key("item:image[size=a][quality=b]", shape);
    refused_key("item:image[a=1,b=1,c=1,d=1,e=1,f=1,g=1,h=1,i=1]", count);
    CHECK(catalogue(R"([{"id":"vendor/model","pricing":{"item:image[a=1,b=1,c=1,d=1,e=1,f=1,g=1,h=1]":"1"}}])") != nullptr);
    // A kind of the kind table only.
    refused_key("item:video", kind);
    refused_key("item:", kind);
    refused_key("item:Image", kind);
    refused_key("item:image_output", kind);
    refused_key("item:video[size=a]", kind);
    refused_key("item:[size=a]", kind);
    // A qualifier only on an item unit.
    refused_key("input[size=a]", not_item);
    refused_key("tool:web_search[size=a]", not_item);
    refused_key("ceiling_item:image[size=a]", not_item);
    refused_key("[size=a]", not_item);
    std::string wide = "item:image[";
    for (char k = 'a'; k <= 'e'; ++k) wide += std::string(k == 'a' ? "" : ",") + k + "=" + std::string(60, 'v');
    refused_key(wide + "]", too_long);
    refused_key("input" + wide.substr(10) + "]", not_item);
    // The tier and the band follow the qualifier, the band last, each in one spelling.
    refused_key("item:image+1000[size=a]", order);
    refused_key("item:image[size=a]/batch[quality=b]", order);
    refused_key("item:image/batch]", order);
    refused_key("item:image/", tier);
    refused_key("item:image/+1000", tier);
    refused_key("item:image/batch/flex", tier);
    refused_key("item:image/ba=tch", tier);
    refused_key("item:image/" + std::string(65, 't'), tier);
    refused_key("item:image+1000/batch", band);
    refused_key("item:image+0", band);
    refused_key("item:image+0100", band);
    refused_key("item:image+", band);
    refused_key("item:image+1x", band);
    refused_key("item:image+1000+2000", band);
    refused_key("item:image+12345678901234567890", band);
    // Other keys keep the rule they had: a unit token, even one pricing reads as no unit.
    CHECK(catalogue(R"([{"id":"vendor/model","pricing":{"input/":"1","input+0":"1","input+x":"1","items":"1","item_image":"1",)"
                    R"("ceiling_item:image":"1","tool:x":"1","image":"1"}}])") != nullptr);
    refused_as(R"([{"id":"vendor/model","pricing":{"input,x":"1"}}])", "/models/0/pricing/input,x", "price unit token");
    refused_as(R"([{"id":"vendor/model","pricing":{"input]":"1"}}])", "/models/0/pricing/input]", "price unit token");
}

// Layers replace an item row by its exact key, as any other unit, and the diff and the digest see item rows as any
// other price. A catalogue without item units keeps its text and digest.
void item_units_merge_and_diff() {
    using Prices = std::vector<std::pair<std::string, std::string>>;
    const auto base = catalogue(R"([{"id":"vendor/model","pricing":{"input":"0.000001","item:image":"0.04","item:image[quality=high]":"0.08",)"
                                R"("item:image[quality=high]/batch":"0.04"}}])");
    const auto host = catalogue(R"([{"id":"vendor/model","pricing":{"item:image[quality=high]":"0.06","item:image[quality=low]":"0.02"}}])");
    const auto same = catalogue(R"([{"id":"vendor/model","pricing":{"item:image[quality=high]":"0.08"}}])");
    if (!base || !host || !same) return;
    const CatalogueSnapshot layers[]{base, host};
    auto merged = merge_catalogues(layers);
    CHECK(std::holds_alternative<CatalogueSnapshot>(merged));
    if (!std::holds_alternative<CatalogueSnapshot>(merged)) return;
    const auto& c = std::get<CatalogueSnapshot>(merged);
    CHECK(c->entry("vendor/model")->pricing == (Prices{{"input", "0.000001"}, {"item:image", "0.04"}, {"item:image[quality=high]", "0.06"},
                                                       {"item:image[quality=high]/batch", "0.04"}, {"item:image[quality=low]", "0.02"}}));
    const auto diff = diff_catalogues(*base, *c);
    CHECK(diff.changed == std::vector<std::string>{"vendor/model"} && diff.added.empty() && diff.removed.empty());
    CHECK(diff_catalogues(*c, *c).empty());
    auto again = load_catalogue(c->to_json());
    CHECK(std::holds_alternative<CatalogueSnapshot>(again) && std::get<CatalogueSnapshot>(again)->digest() == c->digest());
    // A layer that states a row again changes nothing.
    const CatalogueSnapshot restated[]{base, same};
    auto unchanged = merge_catalogues(restated);
    CHECK(std::holds_alternative<CatalogueSnapshot>(unchanged) && std::get<CatalogueSnapshot>(unchanged)->digest() == base->digest() &&
          diff_catalogues(*base, *std::get<CatalogueSnapshot>(unchanged)).empty());
    // Without item units: tiers, bands, a tool fee, a route ceiling and a listed unit nothing reads, in these bytes.
    const auto plain = catalogue(R"([{"id":"vendor/model","pricing":{"prompt":"0.000001","completion":"0.000002","input/flex":"0.0000005",)"
                                 R"("prompt+200000":"0.000002","tool:web_search":"0.01","ceiling_input":"0.000003","image":"0.04","output/batch+1000":"0.000001"}}])");
    if (!plain) return;
    CHECK(plain->to_json() == R"({"version":1,"models":[{"id":"vendor/model","pricing":{"ceiling_input":"0.000003","completion":"0.000002",)"
                              R"("image":"0.04","input/flex":"0.0000005","output/batch+1000":"0.000001","prompt":"0.000001","prompt+200000":"0.000002",)"
                              R"("tool:web_search":"0.01"}}]})");
    CHECK(plain->digest_hex() == "8185bd8c7a0b473bea215247c71dd5abfe83d127d4a03d6613161954fa7f51b7");
}

// A models listing never puts a price in the item namespace: a key that starts with `item:` or `ceiling_item:` is a
// skipped price, in the base prices and in overrides, and the entry is the one the listing gives without it.
void listing_gives_no_item_units() {
    using Prices = std::vector<std::pair<std::string, std::string>>;
    const auto page = [](std::string_view pricing) {
        auto parsed = parse_listing(Listing::OpenRouter, std::string(R"({"data":[{"id":"vendor/model","pricing":)") + std::string(pricing) + "}]}",
                                    "https://openrouter.ai");
        return std::get<ListingPage>(std::move(parsed));
    };
    const auto plain = page(R"({"prompt":"0.000001","completion":"0.000002","overrides":[{"min_prompt_tokens":1000,"prompt":"0.000002"}]})");
    const auto items = page(R"({"prompt":"0.000001","completion":"0.000002","item:image":"0.04","item:image[size=1]":"0.05","ceiling_item:image":"0.06",)"
                            R"("overrides":[{"min_prompt_tokens":1000,"prompt":"0.000002","item:image":"0.08","ceiling_item:image":"0.09"}]})");
    CHECK(plain.entries.size() == 1 && items.entries.size() == 1);
    if (plain.entries.size() != 1 || items.entries.size() != 1) return;
    CHECK(plain.skipped_prices == 0 && items.skipped_prices == 5);
    CHECK(items.entries[0].pricing == (Prices{{"completion", "0.000002"}, {"prompt", "0.000001"}, {"prompt+1000", "0.000002"}}));
    CHECK(items.entries[0].pricing == plain.entries[0].pricing && items.withheld == plain.withheld &&
          items.skipped_override_prices == plain.skipped_override_prices && items.window_prices_at_dearest == plain.window_prices_at_dearest);
    auto a = make_catalogue(plain.entries), b = make_catalogue(items.entries);
    CHECK(std::holds_alternative<CatalogueSnapshot>(a) && std::holds_alternative<CatalogueSnapshot>(b));
    if (std::holds_alternative<CatalogueSnapshot>(a) && std::holds_alternative<CatalogueSnapshot>(b))
        CHECK(std::get<CatalogueSnapshot>(a)->to_json() == std::get<CatalogueSnapshot>(b)->to_json() &&
              std::get<CatalogueSnapshot>(a)->digest() == std::get<CatalogueSnapshot>(b)->digest());
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
        exact_entry_does_not_hide_a_profile_fact();
        listed_parameters_state_temperature();
        embedded_temperature_profiles();
        embedded_profiles_shadow_shorter_ones();
        control_values_by_control();
        control_values_meet_defaults_and_families();
        origins_scope_the_whole_entry();
        bundle_carries_the_catalogue();
        listed_override_bounds();
        item_units_load();
        item_units_refused();
        item_units_merge_and_diff();
        listing_gives_no_item_units();
        catalogue_files();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "catalogue test threw: %s\n", error.what());
        return 1;
    }
    if (failures) std::fprintf(stderr, "%d catalogue checks failed\n", failures);
    else std::puts("model catalogue properties passed");
    return failures ? 1 : 0;
}
