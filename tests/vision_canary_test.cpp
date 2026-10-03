#include "canary/canary.h"
#include "canary/qualification.h"
#include "support/qualification_fixture.h"
#include <array>
#include <iostream>

namespace {
using namespace sp::canary;
using runtime_test::require;
constexpr std::array<std::string_view, 5> providers{"vision_chat", "vision_responses", "vision_messages", "vision_gemini", "vision_interactions"};
constexpr std::array<std::string_view, 5> families{"openai.chat", "openai.responses", "anthropic.messages", "google.generate", "google.interactions"};
std::string profile(std::size_t lane, const std::string& model, std::uint64_t port) {
  const auto url = lane <= 1 ? "https://developers.openai.com/api/docs/pricing" : lane == 2 ? "https://platform.claude.com/docs/en/about-claude/pricing" : "https://ai.google.dev/gemini-api/docs/pricing";
  return "{\"version\":1,\"provider\":" + sp::json::quote(providers[lane]) + ",\"model\":" + sp::json::quote(model) +
      ",\"origin\":\"http://127.0.0.1:" + std::to_string(port) +
      "\",\"input_bound_kind\":\"model_context_window\",\"max_input_tokens\":10000,\"max_output_tokens\":8192,"
      "\"input_micro_usd_per_million\":100,\"output_micro_usd_per_million\":100,\"call_cap\":100,\"token_cap\":1000000000,\"micro_usd_cap\":19795635" +
      std::string(lane == 2 ? ",\"thinking_budget\":1024" : "") + ",\"provenance\":{\"model_url\":" + sp::json::quote(url) +
      ",\"pricing_url\":" + sp::json::quote(url) + ",\"verified_at\":\"2026-10-02\"}}";
}
qualification_test::CliResult cli(const char* executable, qualification_test::Campaign& campaign, std::size_t lane,
                                 const std::string& model, std::uint64_t port,
                                 std::string_view mode = "full", std::string_view repetitions = "1") {
  qualification_test::save(campaign.root / "profile", profile(lane, model, port));
  qualification_test::save(campaign.root / "keys", "OPENAI_API_KEY=VISION_SYNTHETIC_KEY\nANTHROPIC_API_KEY=VISION_SYNTHETIC_KEY\nGEMINI_API_KEY=VISION_SYNTHETIC_KEY\n");
  return qualification_test::cli(executable, {"--profile", (campaign.root / "profile").string(), "--project-root", campaign.root.string(),
      "--env-file", (campaign.root / "keys").string(), "--execute", "--test-loopback",
      "--mode", std::string(mode), "--repetitions", std::string(repetitions)});
}
void privacy(std::string_view bytes) {
  for (auto marker : {"VISION_SYNTHETIC_KEY", "VISION_PRIVATE_SIGNATURE", "VISION_FUNCTION_SIGNATURE"})
    require(bytes.find(marker) == std::string_view::npos, "public report or durable receipt leaked native material");
}
void five_family(runtime_test::Peer& peer, const std::filesystem::path& source, const char* executable) {
  std::vector<std::string> models;
  for (std::size_t lane = 0; lane < providers.size(); ++lane) models.push_back(peer.arm("campaign"));
  qualification_test::Campaign campaign(source, models);
  std::uint64_t cumulative = 0;
  for (std::size_t lane = 0; lane < providers.size(); ++lane) {
    const auto observed = cli(executable, campaign, lane, models[lane], peer.port);
    require(observed.code == (lane == 0 ? 0 : 2), "runner falsely certified ambiguous omission or failed vision prerequisites");
    const auto report = qualification_test::first_report(observed.text); const auto root = report.root();
    const auto count = lane == 0 ? 6U : lane == 4 ? 11U : lane == 3 ? 9U : 8U; cumulative += count;
    require(root.get("api_family").as_string() == families[lane] && root.get("meter").get("calls").as_uint() == cumulative,
        "five-family CLI bypassed shared metered attempts");
    require(root.get("replay").as_string() == (lane == 0 ? "not_applicable" : "ReplayAcceptanceUnobservable") &&
        !root.get("equivalence_admission").as_bool(), "synthetic acceptance fabricated live consumption proof");
    require(root.get("meter").get("baseline_vision_calls").as_uint() == 99 &&
        root.get("meter").get("baseline_vision_exposure_micro_usd").as_uint() == 18979680, "baseline was folded into new grant or lost");
    const auto cases = root.get("cases"); require(cases.size() == count, "scenario omitted");
    for (std::size_t i = 0; i < 6; ++i) {
      const auto item = cases.at(i);
      require(item.get("state").as_string() == "passed" && item.get("dispatched").as_bool() &&
          item.get("attempts").as_uint() == 1 && item.get("image_sent").as_bool() && item.get("vision_correct").as_bool(),
          "buffered/SSE changed-image or local tool oracle failed");
      require(item.get("settlement").as_string() == (lane == 2 ? "Exact" : "UpperBound"), "known final split or absent cache split mislabelled");
    }
    if (lane != 0) {
      require(cases.at(5).get("native_complete").as_bool(), "retained positive lost authenticated native replay authority");
      require(cases.at(6).get("settlement").as_string() == "UnknownHold" && cases.at(7).get("settlement").as_string() == "UnknownHold",
          "failed negative delivery released reservation");
      require(cases.at(6).get("reason").as_string() == (lane == 1 ? "ciphertext_rejected" : "signature_rejected") &&
          cases.at(7).get("reason").as_string() == "negative_inconclusive", "generic omission rejection became native consumption evidence");
    }
    if (lane >= 3) {
      require(cases.at(8).get("duplicate_control").as_bool() && cases.at(8).get("control_changed").as_bool() &&
          cases.at(8).get("reason").as_string() == "negative_accepted",
          "Google duplicate-carrier scenario did not execute; safe report=" + observed.text);
      require(cases.at(8).get("settlement").as_string() == "UpperBound" &&
          !cases.at(8).get("native_complete").as_bool(),
          "accepted duplicate lost final usage or minted native replay authority");
      require(cases.at(4).get("native_carriers").as_uint() == 2 &&
          cases.at(4).get("distinct_native_carriers").as_uint() == (lane == 3 ? 2U : 1U), "native duplicated/distinct carrier observation lost");
      if (lane == 4) {
        for (std::size_t position : {9U, 10U})
          require(cases.at(position).get("control_changed").as_bool() &&
              cases.at(position).get("settlement").as_string() == "UnknownHold" &&
              cases.at(position).get("reason").as_string() == "negative_inconclusive",
              "independent Interactions carrier omission fabricated consumption or released hold");
      }
    }
    peer.count(models[lane], count, lane == 0 ? 0 : lane == 4 ? 4 : 2);
    const auto stats = peer.stats(models[lane]); const auto wire = stats.root();
    require(wire.get("images").as_uint() == count && wire.get("scene_b").as_uint() == 1 && wire.get("tool_first").as_uint() == 1,
        "actual wire did not preserve scenes and local tool request");
    if (lane >= 3) require(wire.get("carrier_duplicate").as_uint() == 1, "duplicate native control not observed by peer");
    privacy(observed.text); campaign.preserved();
  }
  auto meter = campaign.open();
  auto existing = meter->totals(); const auto calls = std::get<sp::qualification::Totals>(existing).calls;
  for (std::uint64_t i = calls; i < 630; ++i) {
    const auto identity = sp::canary::detail::digest({"exhaust-vision", std::to_string(i)});
    auto reserved = meter->reserve({identity, identity, models.front(), "openai.chat", 0, 1});
    require(std::holds_alternative<sp::qualification::Claim>(reserved), "call exhaustion fixture denied");
    auto settled = meter->settle(std::get<sp::qualification::Claim>(reserved), {true, {0, 0, 0, 0, 0, std::nullopt, 0}});
    require(std::holds_alternative<sp::qualification::Settlement>(settled), "fixture zero settlement denied");
  }
  meter.reset(); const auto before = qualification_test::load(campaign.root / "build/qualification-meter.ledger");
  for (std::size_t lane = 0; lane < providers.size(); ++lane) {
    const auto denied = cli(executable, campaign, lane, models[lane], peer.port);
    const auto report = qualification_test::first_report(denied.text);
    require(denied.code == 3 && report.root().get("cases").at(0).get("reason").as_string() == "call_budget" &&
        report.root().get("meter").get("calls").as_uint() == 630, "process restart or another family renewed exhausted chat grant");
    peer.count(models[lane], lane == 0 ? 6 : lane == 4 ? 11 : lane == 3 ? 9 : 8, lane == 0 ? 0 : lane == 4 ? 4 : 2);
  }
  require(qualification_test::load(campaign.root / "build/qualification-meter.ledger") == before, "denied family wrote durable dispatch receipts");
  privacy(before); campaign.preserved();
}
void incorrect_oracle(runtime_test::Peer& peer, const std::filesystem::path& source, const char* executable) {
  for (std::size_t lane = 0; lane < providers.size(); ++lane) {
    const auto model = "incorrect-vision-" + std::to_string(lane);
    peer.command("{\"arm\":" + sp::json::quote(model) + ",\"scenario\":\"campaign\",\"wrong_oracle\":true}");
    qualification_test::Campaign campaign(source, {model});
    const auto observed = cli(executable, campaign, lane, model, peer.port);
    const auto report = qualification_test::first_report(observed.text);
    require(observed.code == 2 && report.root().get("cases").at(0).get("reason").as_string() == "incorrect_vision" &&
        !report.root().get("cases").at(0).get("vision_correct").as_bool(), "acceptance hid incorrect image answer");
    campaign.preserved();
  }
}
void selector_modes(runtime_test::Peer& peer, const std::filesystem::path& source, const char* executable) {
  for (std::size_t lane = 0; lane < providers.size(); ++lane) {
    const auto model = peer.arm("campaign"); qualification_test::Campaign campaign(source, {model});
    const auto observed = cli(executable, campaign, lane, model, peer.port, "baseline-pair", "2");
    require(observed.code == 0, "paired generation mode failed");
    const auto report = qualification_test::first_report(observed.text); const auto cases = report.root().get("cases");
    require(report.root().get("mode").as_string() == "baseline-pair" && cases.size() == 2 &&
        cases.at(0).get("name").as_string() == "vision_on_buffered" &&
        cases.at(1).get("name").as_string() == "vision_on_sse",
        "baseline mode included off/changed/tool/control requests instead of exact buffered/SSE pair");
    peer.count(model, 4);
    const auto stats = peer.stats(model); const auto wire = stats.root();
    require(wire.get("on").as_uint() == 4 && wire.get("off").as_uint() == 0 &&
        wire.get("sse").as_uint() == 2 && wire.get("scene_b").as_uint() == 0 && wire.get("tool_first").as_uint() == 0,
        "paired mode changed image/effective controls or performed diagnostics");
    campaign.preserved();
  }
  for (std::size_t lane : {3U, 4U}) {
    const auto model = peer.arm("campaign"); qualification_test::Campaign campaign(source, {model});
    const auto observed = cli(executable, campaign, lane, model, peer.port, "google-diagnostics", "3");
    require(observed.code == 2, "ambiguous omission was certified by diagnostics mode");
    const auto report = qualification_test::first_report(observed.text);
    require(report.root().get("mode").as_string() == "google-diagnostics" &&
        report.root().get("cases").size() == 5 && report.root().get("cases").at(0).get("name").as_string() == "vision_tool_first",
        "diagnostics included baseline requests or omitted native-generation prerequisite");
    peer.count(model, 15, lane == 3 ? 6 : 7);
    const auto stats = peer.stats(model); const auto wire = stats.root();
    require(wire.get("tool_first").as_uint() == 3 && wire.get("retained").as_uint() == 3 &&
        wire.get("signature_negative").as_uint() == (lane == 3 ? 3U : 2U) &&
        wire.get("reasoning_missing").as_uint() == (lane == 3 ? 3U : 5U) &&
        wire.get("carrier_duplicate").as_uint() == (lane == 3 ? 3U : 2U),
        "diagnostic selection lost bounded independent contrasts or spent over15 actual attempts");
    const auto before = qualification_test::load(campaign.root / "build/qualification-meter.ledger");
    const auto excessive = cli(executable, campaign, lane, model, peer.port, "google-diagnostics", "4");
    require(excessive.code == 2 && qualification_test::load(campaign.root / "build/qualification-meter.ledger") == before,
        "fourth diagnostic repetition acquired spending authority");
    peer.count(model, 15, lane == 3 ? 6 : 7); privacy(observed.text); campaign.preserved();
  }
  const auto model = peer.arm("campaign"); qualification_test::Campaign campaign(source, {model});
  const auto bounded = run(parse_profile(profile(4, model, peer.port), true), campaign.root.string(), "VISION_SYNTHETIC_KEY",
      {ExecutionMode::GoogleDiagnostics, 0, 2});
  require(bounded.cases[4].attempts == 1 && bounded.cases[5].attempts == 1 &&
      bounded.cases[6].reason == Reason::DiagnosticLimit && bounded.cases[6].attempts == 0,
      "remaining diagnostic attempt cap was not enforced before preparation/reservation");
  peer.count(model, 2); campaign.preserved();
}
} // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 4, "usage: sp_vision_canary_tests NODE VISION_SERVER CANARY_CLI");
    const auto source = std::filesystem::absolute(argv[2]).parent_path().parent_path().parent_path();
    runtime_test::Peer peer(argv[1], argv[2]); five_family(peer, source, argv[3]); incorrect_oracle(peer, source, argv[3]);
    selector_modes(peer, source, argv[3]);
    std::cout << "five-family metered vision CLI behavior passed\n"; return 0;
  } catch (const std::exception& error) {
    std::cerr << "vision canary behavior failed: " << error.what() << '\n'; return 1;
  }
}
