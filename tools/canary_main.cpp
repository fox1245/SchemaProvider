#include "canary/canary.h"
#include <array>
#include <charconv>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

int main(int argc, char** argv) {
  using namespace sp::canary;
  bool execution_started = false;
  try {
    std::optional<std::string> profile_path, project_root, env_file, output_text, repetitions_text, mode_text;
    bool execute = false, loopback = false, help = false;
    for (int i = 1; i < argc; ++i) {
      std::string_view flag(argv[i]);
      auto path = [&](std::optional<std::string>& target) {
        if (target || i + 1 == argc || std::string_view(argv[i + 1]).empty() || std::string_view(argv[i + 1]).starts_with("--"))
          throw std::invalid_argument("invalid arguments");
        target = argv[++i];
      };
      if (flag == "--profile") path(profile_path);
      else if (flag == "--project-root") path(project_root);
      else if (flag == "--env-file") path(env_file);
      else if (flag == "--max-output-tokens") path(output_text);
      else if (flag == "--repetitions") path(repetitions_text);
      else if (flag == "--mode") path(mode_text);
      else if (flag == "--execute" && !execute) execute = true;
      else if (flag == "--test-loopback" && !loopback) loopback = true;
      else if (flag == "--help" && !help) help = true;
      else throw std::invalid_argument("invalid arguments");
    }
    std::optional<std::uint64_t> output_tokens;
    if (output_text) {
      std::uint64_t value{};
      const auto [end, error] = std::from_chars(output_text->data(), output_text->data() + output_text->size(), value);
      if (error != std::errc{} || end != output_text->data() + output_text->size() || value == 0)
        throw std::invalid_argument("invalid arguments");
      output_tokens = value;
    }
    ExecutionMode mode = ExecutionMode::Full;
    std::string_view mode_name = "full";
    if (mode_text) {
      if (*mode_text == "baseline-pair") { mode = ExecutionMode::BaselinePair; mode_name = "baseline-pair"; }
      else if (*mode_text == "google-diagnostics") { mode = ExecutionMode::GoogleDiagnostics; mode_name = "google-diagnostics"; }
      else if (*mode_text != "full") throw std::invalid_argument("invalid arguments");
    }
    std::uint64_t repetitions = 1;
    if (repetitions_text) {
      const auto [end, error] = std::from_chars(repetitions_text->data(), repetitions_text->data() + repetitions_text->size(), repetitions);
      if (error != std::errc{} || end != repetitions_text->data() + repetitions_text->size() || repetitions == 0 || repetitions > 630)
        throw std::invalid_argument("invalid arguments");
    }
    if ((mode == ExecutionMode::BaselinePair && repetitions > 60) ||
        (mode == ExecutionMode::GoogleDiagnostics && repetitions > 3))
      throw std::invalid_argument("invalid arguments");
    if (help) { std::cout << help_text(); return 0; }
    if (!execute) { std::cout << plan_json() << '\n'; return 0; }
    if (!profile_path || !project_root) throw std::invalid_argument("invalid arguments");
    auto profile = parse_profile(read_profile_file(*profile_path), loopback, output_tokens);
    if ((mode == ExecutionMode::BaselinePair && !vision_provider(profile.provider())) ||
        (mode == ExecutionMode::GoogleDiagnostics && profile.provider() != Provider::VisionGemini &&
         profile.provider() != Provider::VisionInteractions))
      throw std::invalid_argument("invalid arguments");
    auto key = credential(profile.provider(), env_file);
    execution_started = true;
    bool missing = false, failed = false;
    std::uint64_t completed = 0, attempts = 0, exact = 0, upper = 0, unknown = 0;
    std::uint64_t dispatched_pairs = 0, qualified_pairs = 0;
    std::uint64_t negative_accepted = 0, omission_accepted = 0, omission_rejected = 0, signature_rejected = 0, inconclusive = 0;
    constexpr std::array<std::string_view, 8> control_names{
        "signature_negative", "ciphertext_negative", "reasoning_missing", "vision_signature_negative",
        "vision_reasoning_missing", "vision_carrier_duplicate", "vision_thought_carrier_missing", "vision_call_carrier_missing"};
    struct ControlStats { std::uint64_t observed{}, accepted{}, rejected{}, inconclusive{}, not_run{}; };
    std::array<ControlStats, control_names.size()> controls{};
    for (std::uint64_t i = 0; i < repetitions; ++i) {
      if (mode == ExecutionMode::GoogleDiagnostics && attempts >= 15) break;
      auto report = run(profile, *project_root, key, {mode, i, mode == ExecutionMode::GoogleDiagnostics ? 15 - attempts : 15});
      ++completed;
      bool stop = false;
      for (const auto& item : report.cases) {
        if (!item.selected) continue;
        missing |= item.state == State::NotRun; failed |= item.state == State::Failed;
        attempts += item.attempts;
        stop |= item.admission_denial.has_value() || item.reason == Reason::MissingCredential || item.reason == Reason::DiagnosticLimit;
        if (item.settlement) {
          exact += *item.settlement == sp::qualification::SettlementKind::Exact;
          upper += *item.settlement == sp::qualification::SettlementKind::UpperBound;
          unknown += *item.settlement == sp::qualification::SettlementKind::UnknownHold;
        }
        negative_accepted += item.reason == Reason::NegativeAccepted;
        omission_accepted += item.reason == Reason::OmissionAccepted;
        omission_rejected += item.reason == Reason::OmissionRejected;
        signature_rejected += item.reason == Reason::SignatureRejected || item.reason == Reason::CiphertextRejected;
        inconclusive += item.reason == Reason::NegativeInconclusive;
        for (std::size_t c = 0; c < control_names.size(); ++c) if (item.name == control_names[c]) {
          auto& observation = controls[c];
          observation.observed += item.dispatched;
          observation.accepted += item.reason == Reason::NegativeAccepted || item.reason == Reason::OmissionAccepted;
          observation.rejected += item.reason == Reason::SignatureRejected || item.reason == Reason::CiphertextRejected ||
              item.reason == Reason::OmissionRejected;
          observation.inconclusive += item.reason == Reason::NegativeInconclusive;
          observation.not_run += item.state == State::NotRun;
        }
      }
      if (mode == ExecutionMode::BaselinePair) {
        dispatched_pairs += report.cases[1].dispatched && report.cases[2].dispatched;
        qualified_pairs += report.cases[1].state == State::Passed && report.cases[2].state == State::Passed;
      }
      std::cout << report_json(report) << '\n';
      if (stop) break;
    }
    std::cout << "{\"version\":1,\"kind\":\"empirical_qualification_summary\",\"requested_repetitions\":" << repetitions
              << ",\"mode\":\"" << mode_name << "\",\"completed_repetitions\":" << completed << ",\"attempts\":" << attempts
              << ",\"requested_pairs\":" << (mode == ExecutionMode::BaselinePair ? repetitions : 0)
              << ",\"dispatched_pairs\":" << dispatched_pairs << ",\"qualified_pairs\":" << qualified_pairs
              << ",\"Exact\":" << exact << ",\"UpperBound\":" << upper << ",\"UnknownHold\":" << unknown
              << ",\"control_observations\":{\"negative_accepted\":" << negative_accepted
              << ",\"omission_accepted\":" << omission_accepted << ",\"omission_rejected\":" << omission_rejected
              << ",\"signature_rejected\":" << signature_rejected << ",\"inconclusive\":" << inconclusive
              << "},\"by_control\":[";
    for (std::size_t c = 0; c < control_names.size(); ++c) {
      const auto& observation = controls[c];
      if (c) std::cout << ',';
      std::cout << "{\"name\":\"" << control_names[c] << "\",\"observed_attempts\":" << observation.observed
                << ",\"accepted\":" << observation.accepted << ",\"specific_rejection\":" << observation.rejected
                << ",\"inconclusive\":" << observation.inconclusive << ",\"not_run\":" << observation.not_run << '}';
    }
    std::cout << "],\"native_consumption\":\"unobservable\",\"statistical_scope\":\"empirical_frequencies_not_equivalence_or_consumption_proof\","
                 "\"equivalence_admission\":false}\n";
    return failed ? 2 : missing ? 3 : 0;
  } catch (...) {
    // Paths, parser context, credentials and live response errors are never echoed.
    if (execution_started)
      std::cerr << "{\"version\":1,\"state\":\"failed\",\"reason\":\"execution_failure\",\"reservations\":\"consult_fixed_meter_unknown_holds_retained\"}\n";
    else
      std::cerr << "{\"version\":1,\"state\":\"not_run\",\"reason\":\"invalid_input_or_persistence\"}\n";
    return 2;
  }
}
