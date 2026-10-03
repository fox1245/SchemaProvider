#include "canary/canary.h"
#include "canary/qualification.h"
#include "runtime/testing.h"
#include "support/qualification_fixture.h"
#include <iostream>

namespace {
using namespace sp::canary;
using runtime_test::require;
using qualification_test::Campaign;
constexpr auto secret = "CANARY_SECRET_KEY_MARKER";
std::string profile(std::string_view provider, std::string_view model, std::uint64_t port) {
  const bool messages = provider == "anthropic";
  const auto url = messages ? "https://platform.claude.com/docs/en/about-claude/pricing" : "https://developers.openai.com/api/docs/pricing";
  return "{\"version\":1,\"provider\":" + sp::json::quote(provider) + ",\"model\":" + sp::json::quote(model) +
      ",\"origin\":\"http://127.0.0.1:" + std::to_string(port) +
      "\",\"input_bound_kind\":\"model_context_window\",\"max_input_tokens\":4096,\"max_output_tokens\":2048,"
      "\"input_micro_usd_per_million\":100,\"output_micro_usd_per_million\":100,\"call_cap\":16,\"token_cap\":1000000,\"micro_usd_cap\":1000000" +
      (messages ? std::string(",\"thinking_budget\":1024") : std::string()) +
      ",\"provenance\":{\"model_url\":" + sp::json::quote(url) + ",\"pricing_url\":" + sp::json::quote(url) + ",\"verified_at\":\"2026-10-02\"}}";
}
void private_report(std::string_view text) {
  for (auto marker : {secret, "CANARY_SIGNATURE_MARKER", "CANARY_THINKING_MARKER", "CANARY_REDACTED_MARKER", "CANARY_RESPONSE_MARKER", "CANARY_ACCOUNT_ID_MARKER", "CANARY_CIPHER_MARKER"})
    require(text.find(marker) == std::string_view::npos, "runner leaked private material");
}
void wire_count(runtime_test::Peer& peer, const std::string& model, std::size_t expected,
                std::size_t faults, const Report& report) {
  try { peer.count(model, expected, faults); }
  catch (const std::exception& error) {
    throw std::runtime_error(std::string(error.what()) + "; safe report=" + report_json(report));
  }
}
template<class F> void rejects(F&& action) {
  bool denied = false;
  try { action(); } catch (const std::exception& error) { denied = true; private_report(error.what()); }
  require(denied, "unsafe profile or credential input accepted");
}
void profile_and_credential_boundaries(const std::filesystem::path& source) {
  Campaign campaign(source, {"synthetic"});
  const auto valid = profile("anthropic", "synthetic", 18080);
  rejects([&] { (void)parse_profile(valid); });
  auto changed = [&](std::string_view from, std::string_view to) {
    auto value = valid; const auto at = value.find(from);
    require(at != std::string::npos, "profile adversary missing"); value.replace(at, from.size(), to); return value;
  };
  for (const auto& invalid : {
      changed("\"version\":1", "\"version\":1,\"version\":1"),
      changed("\"version\":1", "\"version\":1,\"extra\":1"),
      changed("\"version\":1", "\"version\":2"),
      changed("127.0.0.1", "localhost"),
      changed("127.0.0.1", "127.0.0.1.evil.test"),
      changed("model_context_window", "character_guess"),
      changed("\"max_input_tokens\":4096", "\"max_input_tokens\":18446744073709551615"),
      changed("\"thinking_budget\":1024", "\"thinking_budget\":2048"),
      changed("2026-10-02", "2026-02-30")})
    rejects([&] { (void)parse_profile(invalid, true); });
  const auto file = (campaign.root / "keys").string();
  qualification_test::save(file, "OPENAI_API_KEY=CANARY_SECRET_KEY_MARKER\nANTHROPIC_API_KEY=other\nGEMINI_API_KEY=google\n");
  require(credential(Provider::OpenAI, file) == secret && credential(Provider::Anthropic, file) == "other" &&
      credential(Provider::VisionInteractions, file) == "google", "credentials crossed provider-family origin");
  qualification_test::save(file, "ANTHROPIC_API_KEY=other\n");
  require(credential(Provider::OpenAI, file).empty(), "missing explicit credential fell back to environment");
  qualification_test::save(file, "OPENAI_API_KEY=one\nOPENAI_API_KEY=two\n");
  rejects([&] { (void)credential(Provider::OpenAI, file); });
  for (auto value : {"$(private-shell-command)", "${PRIVATE_KEY}", "$PRIVATE_KEY", "`private-shell-command`", "private\\ key"}) {
    qualification_test::save(file, "OPENAI_API_KEY=" + std::string(value) + "\n");
    rejects([&] { (void)credential(Provider::OpenAI, file); });
  }
  qualification_test::save(file, "OPENAI_API_KEY=private\n");
  require(::chmod(file.c_str(), 0644) == 0, "fixture chmod failed");
  rejects([&] { (void)credential(Provider::OpenAI, file); });
  const auto link = (campaign.root / "link").string();
  require(::symlink(file.c_str(), link.c_str()) == 0, "fixture symlink failed");
  rejects([&] { (void)credential(Provider::OpenAI, link); });
}
void replay(runtime_test::Peer& peer, const std::filesystem::path& source) {
  for (auto scenario : {"reject", "accept", "absent", "misleading", "unrelated"}) {
    const auto model = peer.arm(scenario); Campaign campaign(source, {model});
    auto result = run(parse_profile(profile("anthropic", model, peer.port), true), campaign.root.string(), secret);
    const bool absent = std::string_view(scenario) == "absent", rejected = std::string_view(scenario) == "reject";
    wire_count(peer, model, absent ? 4 : 5, absent || std::string_view(scenario) == "accept" ? 0 : 1, result);
    require(result.positive_retained && result.signature_mutated == !absent, "native controls did not reach exact wire");
    require(result.replay == Replay::ReplayAcceptanceUnobservable, "loopback fabricated native consumption proof");
    require(result.cases[4].reason == (absent ? Reason::MissingSignature : rejected ? Reason::SignatureRejected :
        std::string_view(scenario) == "accept" ? Reason::NegativeAccepted : Reason::NegativeInconclusive), "signature-specific evidence confused with generic failure");
    if (std::string_view(scenario) == "accept")
      require(result.cases[3].native_complete && !result.cases[4].native_complete &&
          result.cases[4].usage_stage == sp::UsageStage::Final,
          "accepted signature control lost outcome data or minted replay authority");
    require(result.meter_totals && result.meter_totals->calls == (absent ? 4 : 5), "control attempt omitted durable call grant");
    require(result.cases[0].settlement == sp::qualification::SettlementKind::UnknownHold &&
        !result.cases[0].input_tokens && result.cases[0].input_uncached == 2, "absent cache split became zero or released unknown input hold");
    const auto ledger = qualification_test::load(campaign.root / "build/qualification-meter.ledger");
    private_report(ledger); private_report(report_json(result)); campaign.preserved();
  }
  for (auto scenario : {"responses-omit-reject", "responses-accept", "responses-misleading"}) {
    const auto model = peer.arm(scenario); Campaign campaign(source, {model});
    const auto result = run(parse_profile(profile("openai_responses", model, peer.port), true), campaign.root.string(), secret);
    wire_count(peer, model, 6, std::string_view(scenario) == "responses-accept" ? 0 : std::string_view(scenario) == "responses-omit-reject" ? 2 : 1, result);
    require(result.positive_retained && result.reasoning_removed && result.ciphertext_mutated, "Responses controls lost original replay");
    require(result.meter_totals && result.meter_totals->calls == 6, "Responses control attempts were not metered");
    require(result.cases[4].reason == (std::string_view(scenario) == "responses-omit-reject" ? Reason::OmissionRejected : Reason::OmissionAccepted), "omission confused with signature rejection");
    if (std::string_view(scenario) == "responses-accept")
      require(result.cases[3].native_complete && !result.cases[4].native_complete &&
          !result.cases[5].native_complete && result.cases[5].reason == Reason::NegativeAccepted &&
          result.cases[4].usage_stage == sp::UsageStage::Final && result.cases[5].usage_stage == sp::UsageStage::Final,
          "accepted Responses controls lost native-free final outcomes or positive authority");
    campaign.preserved(); private_report(report_json(result));
  }
}
void settlements(runtime_test::Peer& peer, const std::filesystem::path& source) {
  for (auto scenario : {"reject", "inconsistent-usage", "remote-error"}) {
    const auto model = peer.arm(scenario); Campaign campaign(source, {model});
    auto result = run(parse_profile(profile("openai", model, peer.port), true), campaign.root.string(), secret);
    const auto expected = std::string_view(scenario) == "reject" ? sp::qualification::SettlementKind::UpperBound : sp::qualification::SettlementKind::UnknownHold;
    require(result.cases[0].settlement == expected, "final total without split or inconsistent delivery incorrectly settled");
    require(result.cases[0].attempts == 1, "automatic paid provider retry enabled");
    if (std::string_view(scenario) == "remote-error") {
      wire_count(peer, model, 3, 3, result);
      for (std::size_t i = 0; i < 3; ++i)
        require(result.cases[i].dispatched && result.cases[i].attempts == 1 &&
            result.cases[i].state == State::Failed && result.cases[i].http_status == 503 &&
            result.cases[i].settlement == sp::qualification::SettlementKind::UnknownHold,
            "independent failed probes retried or released unreported usage");
      require(result.cases[3].state == State::NotRun && result.cases[3].reason == Reason::PrerequisiteFailed &&
          !result.cases[3].dispatched && result.cases[3].attempts == 0 && !result.cases[3].reserved_micro_usd,
          "failed tool generation fabricated a metered continuation");
    }
    else wire_count(peer, model, 4, 0, result);
    if (expected == sp::qualification::SettlementKind::UnknownHold)
      require(result.meter_totals && result.meter_totals->held_micro_usd > 0, "unknown/inconsistent attempt refunded hold");
    campaign.preserved();
  }
  const auto model = peer.arm("known-usage"); Campaign campaign(source, {model});
  const auto result = run(parse_profile(profile("anthropic", model, peer.port), true), campaign.root.string(), secret);
  require(result.cases[0].settlement == sp::qualification::SettlementKind::Exact &&
      !result.cases[0].reasoning && result.cases[0].charged_micro_usd == 2, "missing thinking was added to inclusive output or exact split lost");
  require(result.cases[1].settlement == sp::qualification::SettlementKind::UnknownHold, "partial cache counters were promoted to exact totals");
  wire_count(peer, model, 5, 1, result); campaign.preserved();
}
void prior_attempt_holds(const std::filesystem::path& source) {
  Campaign campaign(source, {"attempt-evidence"});
  auto meter = campaign.open();
  std::uint64_t held = 0;
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    const auto identity = sp::canary::detail::digest({"attempt-evidence", std::to_string(scenario)});
    auto reserved = meter->reserve({identity, identity, "attempt-evidence", "openai.chat", 100, 100});
    require(std::holds_alternative<sp::qualification::Claim>(reserved), "attempt evidence reservation denied");
    auto claim = std::get<sp::qualification::Claim>(std::move(reserved));
    sp::Completion completion;
    completion.usage.stage = sp::UsageStage::Final;
    completion.usage.input_total = sp::Count{10};
    completion.usage.input_uncached = sp::Count{10};
    completion.usage.cache_read = sp::Count{0};
    completion.usage.cache_write = sp::Count{0};
    completion.usage.output_total = sp::Count{5};
    completion.attempt.prior_usage_unknown = scenario == 1;
    completion.attempt.transport_internal_resends = scenario == 2 ? 1 : 0;
    auto settled = meter->settle(claim, sp::canary::detail::metered_outcome(
        std::make_shared<const sp::Outcome>(std::move(completion))));
    require(std::holds_alternative<sp::qualification::Settlement>(settled), "attempt evidence settlement denied");
    const auto& settlement = std::get<sp::qualification::Settlement>(settled);
    if (scenario) held += claim.reserved_micro_usd();
    require(settlement.kind == (scenario ? sp::qualification::SettlementKind::UnknownHold :
        sp::qualification::SettlementKind::Exact) && settlement.totals.held_micro_usd == held,
        "final current-attempt counters released uncertain earlier attempt charges");
  }
  meter.reset();
  const auto reopened = campaign.open()->totals();
  require(std::holds_alternative<sp::qualification::Totals>(reopened) &&
      std::get<sp::qualification::Totals>(reopened).held_micro_usd == held,
      "reopening released uncertain attempt holds");
  campaign.preserved();
}
void invalid_and_restart(runtime_test::Peer& peer, const std::filesystem::path& source) {
  const auto model = peer.arm("reject"); Campaign campaign(source, {model});
  const auto p = parse_profile(profile("openai", model, peer.port), true);
  const auto loaded = sp::descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"preflight\",\"family\":\"openai.chat\",\"connection\":{\"base_url\":\"http://127.0.0.1:" + std::to_string(peer.port) + "\",\"paths\":{\"buffered\":\"/v1/chat/completions\",\"streaming\":\"/v1/chat/completions\"}}}");
  sp::runtime::Options options; options.api_key = secret;
  sp::runtime::Client client(std::get<sp::descriptor::ValidatedDescriptor>(loaded), options);
  sp::chat::Request invalid; invalid.model = model; // absent messages
  sp::runtime::RunOptions run_options; run_options.retry.emplace(); run_options.retry->enabled = false; run_options.retry->max_attempts = 1;
  auto prepared = client.prepare(invalid, run_options);
  sp::canary::detail::Qualification authority(campaign.root.string()); Case item{"text_buffered"}; Report report;
  require(!authority.admit(p, prepared, true, item, report), "invalid preparation got durable authority");
  require(!std::filesystem::exists(campaign.root / "build/qualification-meter.ledger"), "invalid preflight created durable grant receipt");
  peer.count(model, 0);
  auto meter = campaign.open();
  for (std::uint64_t i = 1; i <= 630; ++i) {
    const auto identity = sp::canary::detail::digest({std::to_string(i)});
    auto reserved = meter->reserve({identity, identity, model, "openai.chat", 0, 1});
    require(std::holds_alternative<sp::qualification::Claim>(reserved), "call boundary fixture denied");
    sp::qualification::Outcome zero{true, {0, 0, 0, 0, 0, std::nullopt, 0}};
    require(std::holds_alternative<sp::qualification::Settlement>(meter->settle(std::get<sp::qualification::Claim>(reserved), zero)), "call fixture failed settlement");
  }
  const auto before = qualification_test::load(campaign.root / "build/qualification-meter.ledger"); meter.reset();
  for (unsigned restart = 0; restart < 2; ++restart) {
    const auto denied = run(p, campaign.root.string(), secret);
    require(denied.cases[0].state == State::NotRun && denied.cases[0].reason == Reason::CallBudget &&
        denied.meter_totals && denied.meter_totals->calls == 630, "reopened CLI runner renewed call grant");
  }
  require(qualification_test::load(campaign.root / "build/qualification-meter.ledger") == before, "denied dispatch wrote new receipt");
  peer.count(model, 0); campaign.preserved();
}
void money_denial(runtime_test::Peer& peer, const std::filesystem::path& source) {
  const auto model = peer.arm("reject"); Campaign campaign(source, {model});
  auto catalog = qualification_test::load(campaign.root / "config/model-catalog.json");
  const auto rate = catalog.find("\"output_rate\":100");
  require(rate != std::string::npos, "money fixture rate missing");
  catalog.replace(rate, std::string_view("\"output_rate\":100").size(), "\"output_rate\":1000000");
  qualification_test::save(campaign.root / "config/model-catalog.json", catalog);
  auto meter = campaign.open();
  for (std::uint64_t i = 0; i < 16; ++i) {
    const auto identity = sp::canary::detail::digest({"money-hold", std::to_string(i)});
    auto reserved = meter->reserve({identity, identity, model, "openai.chat", 0, i == 15 ? 40000U : 64000U});
    require(std::holds_alternative<sp::qualification::Claim>(reserved), "money fixture denied early");
    // Abandoned real durable claims retain their full authorized holds.
  }
  meter.reset();
  const auto p = parse_profile(profile("openai", model, peer.port), true);
  for (unsigned restart = 0; restart < 2; ++restart) {
    const auto result = run(p, campaign.root.string(), secret);
    require(result.cases[0].state == State::NotRun && result.cases[0].reason == Reason::CostBudget &&
        result.cases[0].attempts == 0 && result.meter_totals && result.meter_totals->held_micro_usd == 1000000,
        "runner bypassed persistent $1 money stop or restart renewed abandoned holds");
  }
  peer.count(model, 0); campaign.preserved();
}
void cli_repeat(runtime_test::Peer& peer, const std::filesystem::path& source, const char* executable) {
  const auto model = peer.arm("reject"); Campaign campaign(source, {model});
  qualification_test::save(campaign.root / "profile", profile("openai", model, peer.port));
  qualification_test::save(campaign.root / "keys", "OPENAI_API_KEY=CANARY_SECRET_KEY_MARKER\n");
  const auto result = qualification_test::cli(executable, {"--profile", (campaign.root / "profile").string(), "--project-root", campaign.root.string(),
      "--env-file", (campaign.root / "keys").string(), "--execute", "--test-loopback", "--repetitions", "2"});
  require(result.code == 0, "bounded repeated CLI qualification failed");
  private_report(result.text); peer.count(model, 8);
  const auto last = result.text.rfind('\n', result.text.size() - 2);
  const auto summary = runtime_test::parse(std::string_view(result.text).substr(last + 1));
  require(summary.root().get("completed_repetitions").as_uint() == 2 && summary.root().get("attempts").as_uint() == 8,
      "empirical summary omitted repeated paid attempts");
  campaign.preserved();
}
} // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 4, "usage: sp_canary_tests NODE CANARY_SERVER CANARY_CLI");
    const auto source = std::filesystem::absolute(argv[2]).parent_path().parent_path().parent_path();
    runtime_test::Peer peer(argv[1], argv[2]);
    profile_and_credential_boundaries(source);
    replay(peer, source); settlements(peer, source); prior_attempt_holds(source); invalid_and_restart(peer, source); money_denial(peer, source); cli_repeat(peer, source, argv[3]);
    std::cout << "prepared metered canary behavior passed\n"; return 0;
  } catch (const std::exception& error) {
    std::cerr << "canary behavior failed: " << error.what() << '\n'; return 1;
  }
}
