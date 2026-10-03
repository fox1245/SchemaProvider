#include "canary/canary.h"
#include "json/json.h"
#include "runtime/policy.h"

namespace sp::canary {
namespace {
std::string_view state(State value) {
  switch (value) { case State::Passed: return "passed"; case State::Failed: return "failed"; case State::NotRun: return "not_run"; }
  return "failed";
}
std::string_view reason(Reason value) {
  switch (value) {
    case Reason::None: return "none";
    case Reason::MissingCredential: return "missing_credential";
    case Reason::CallBudget: return "call_budget";
    case Reason::TokenBudget: return "token_budget";
    case Reason::CostBudget: return "cost_budget";
    case Reason::LedgerFailure: return "ledger_failure";
    case Reason::RuntimeFailure: return "runtime_failure";
    case Reason::MissingText: return "missing_text";
    case Reason::MissingTool: return "missing_tool";
    case Reason::InvalidTool: return "invalid_tool";
    case Reason::MissingSignature: return "missing_signature";
    case Reason::PrerequisiteFailed: return "prerequisite_failed";
    case Reason::RetentionMismatch: return "retention_mismatch";
    case Reason::MutationUnavailable: return "mutation_unavailable";
    case Reason::SignatureRejected: return "signature_rejected";
    case Reason::NegativeAccepted: return "negative_accepted";
    case Reason::NegativeInconclusive: return "negative_inconclusive";
    case Reason::InvalidUsage: return "invalid_usage";
    case Reason::MissingReasoning: return "missing_reasoning";
    case Reason::CiphertextRejected: return "ciphertext_rejected";
    case Reason::OmissionAccepted: return "omission_accepted";
    case Reason::OmissionRejected: return "omission_rejected";
    case Reason::IncorrectVision: return "incorrect_vision";
    case Reason::UnreadableVision: return "unreadable_vision";
    case Reason::DiagnosticLimit: return "diagnostic_limit";
  }
  return "runtime_failure";
}
std::string_view replay(Replay value) {
  switch (value) {
    case Replay::NotApplicable: return "not_applicable";
    case Replay::ReplayVerified: return "ReplayVerified";
    case Replay::ReplayAcceptanceUnobservable: return "ReplayAcceptanceUnobservable";
  }
  return "ReplayAcceptanceUnobservable";
}
std::string_view mode(ExecutionMode value) {
  switch (value) {
    case ExecutionMode::Full: return "full";
    case ExecutionMode::BaselinePair: return "baseline-pair";
    case ExecutionMode::GoogleDiagnostics: return "google-diagnostics";
  }
  return "full";
}
std::string nullable(std::optional<std::uint64_t> value) { return value ? std::to_string(*value) : "null"; }
std::string_view settlement(qualification::SettlementKind kind) {
  switch (kind) {
    case qualification::SettlementKind::Exact: return "Exact";
    case qualification::SettlementKind::UpperBound: return "UpperBound";
    case qualification::SettlementKind::UnknownHold: return "UnknownHold";
  }
  return "UnknownHold";
}
std::string_view denial(qualification::Denial kind) {
  switch (kind) {
    case qualification::Denial::Configuration: return "configuration";
    case qualification::Denial::Storage: return "storage";
    case qualification::Denial::Corruption: return "corruption";
    case qualification::Denial::Identity: return "identity";
    case qualification::Denial::Calls: return "calls";
    case qualification::Denial::Money: return "money";
    case qualification::Denial::Bounds: return "bounds";
    case qualification::Denial::Overflow: return "overflow";
    case qualification::Denial::UnknownModel: return "unknown_model";
    case qualification::Denial::DuplicateRequest: return "duplicate_request";
    case qualification::Denial::InvalidClaim: return "invalid_claim";
    case qualification::Denial::SettlementConflict: return "settlement_conflict";
  }
  return "configuration";
}
} // namespace
std::string report_json(const Report& report) {
  const bool vision = vision_provider(report.provider);
  const bool responses = report.provider == Provider::OpenAIResponses || report.provider == Provider::VisionResponses;
  const bool messages = report.provider == Provider::Anthropic || report.provider == Provider::VisionMessages;
  const auto provider = report.provider == Provider::OpenAI || report.provider == Provider::VisionChat || responses ? "openai" :
      messages ? "anthropic" : "gemini";
  const auto family = responses ? "openai.responses" : messages ? "anthropic.messages" :
      report.provider == Provider::VisionGemini ? "google.generate" :
      report.provider == Provider::VisionInteractions ? "google.interactions" : "openai.chat";
  std::string meter = "null", extension = "null", admitted_calls = "null", admitted_money = "null";
  if (report.meter_totals) {
    const auto& total = *report.meter_totals;
    admitted_calls = std::to_string(total.call_limit);
    admitted_money = std::to_string(total.money_limit_micro_usd);
    if (total.extension_authorization_events) {
      extension = "{\"authorization\":\"config/qualification-authorization-extension.json\","
          "\"authorization_events\":" + std::to_string(total.extension_authorization_events) +
          ",\"additional_calls\":" + std::to_string(total.extension_calls) +
          ",\"micro_usd\":" + std::to_string(total.extension_micro_usd) +
          ",\"activation\":\"same_SPQUAL1_append_only\"}";
    }
    meter = "{\"calls\":" + std::to_string(total.calls) +
        ",\"spent_micro_usd\":" + std::to_string(total.spent_micro_usd) +
        ",\"held_micro_usd\":" + std::to_string(total.held_micro_usd) +
        ",\"exact_settlements\":" + std::to_string(total.exact_settlements) +
        ",\"upper_bound_settlements\":" + std::to_string(total.upper_bound_settlements) +
        ",\"unknown_settlements\":" + std::to_string(total.unknown_settlements) +
        ",\"call_limit\":" + std::to_string(total.call_limit) +
        ",\"money_limit_micro_usd\":" + std::to_string(total.money_limit_micro_usd) +
        ",\"original_call_limit\":" + std::to_string(total.original_call_limit) +
        ",\"original_money_limit_micro_usd\":" + std::to_string(total.original_money_limit_micro_usd) +
        ",\"extension_authorization_events\":" + std::to_string(total.extension_authorization_events) +
        ",\"extension_calls\":" + std::to_string(total.extension_calls) +
        ",\"extension_micro_usd\":" + std::to_string(total.extension_micro_usd) +
        ",\"baseline_vision_calls\":" + std::to_string(total.baseline_vision_calls) +
        ",\"baseline_vision_exposure_micro_usd\":" + std::to_string(total.baseline_vision_exposure_micro_usd) + '}';
  }
  std::string out = "{\"version\":1,\"provider\":" + json::quote(provider) +
      ",\"api_family\":" + json::quote(family) +
      ",\"mode\":" + json::quote(mode(report.mode)) +
      ",\"verification_scope\":" + json::quote(report.mode == ExecutionMode::BaselinePair ? "paired_reasoning_generation" :
          report.mode == ExecutionMode::GoogleDiagnostics ? "google_native_diagnostics" :
          vision ? "vision_reasoning_function_probe" : responses ? "stateless_reasoning_poc" :
          report.provider == Provider::Gemini ? "text_only_compatibility_smoke" : "canary") +
      ",\"test_only\":" + (report.test_only ? "true" : "false") + ",\"replay\":" + json::quote(replay(report.replay)) +
      ",\"positive_retained\":" + (report.positive_retained ? "true" : "false") +
      ",\"signature_mutated\":" + (report.signature_mutated ? "true" : "false") +
      ",\"ciphertext_mutated\":" + (report.ciphertext_mutated ? "true" : "false") +
      ",\"reasoning_removed\":" + (report.reasoning_removed ? "true" : "false") +
      ",\"native_leaves\":" + std::to_string(report.native_leaves) +
      ",\"repetition\":" + std::to_string(report.repetition) + ",\"meter\":" + meter +
      ",\"grant\":{\"authorization\":\"config/qualification-authorization.json\",\"catalog\":\"config/model-catalog.json\",\"scope\":\"five_current_chat_api_qualification\","
      "\"admitted\":" + (report.meter_totals ? "true" : "false") +
      ",\"additional_calls\":" + admitted_calls + ",\"micro_usd\":" + admitted_money +
      ",\"original\":{\"additional_calls\":630,\"micro_usd\":1000000},\"extension\":" + extension +
      ",\"baseline\":\"SPCANARY1_immutable\",\"unknown_cost_policy\":\"retain_reservation\",\"unused_old_margin\":\"not_renewed\","
      "\"excluded_paid_scope\":[\"image_generation\",\"video_generation\",\"remote_graphrag\"]},"
      "\"billing\":\"catalogue_meter_not_provider_invoice\",\"native_consumption\":\"unobservable\","
      "\"usage_estimate\":\"reported_totals_at_profile_max_rates_not_meter_settlement\",\"equivalence_admission\":false,\"cases\":[";
  bool comma = false;
  std::size_t count = 0;
  for (const auto& item : report.cases) {
    if (!item.selected) continue;
    if (count++ == 11) break; // Public output stays bounded even for caller-edited reports.
    if (comma) out += ',';
    comma = true;
    // Do not serialize caller-owned identifiers even if a Report was edited.
    std::string_view name = "unknown";
    for (auto allowed : {"text_buffered", "text_sse", "tool_first", "tool_positive", "signature_negative",
                        "reasoning_missing", "ciphertext_negative", "vision_off_buffered", "vision_on_buffered",
                        "vision_on_sse", "vision_changed", "vision_tool_first", "vision_tool_positive",
                        "vision_signature_negative", "vision_reasoning_missing", "vision_carrier_duplicate",
                        "vision_thought_carrier_missing", "vision_call_carrier_missing"})
      if (item.name == allowed) name = allowed;
    out += "{\"name\":" + json::quote(name) + ",\"state\":" + json::quote(state(item.state)) +
        ",\"reason\":" + json::quote(reason(item.reason)) + ",\"dispatched\":" + (item.dispatched ? "true" : "false") +
        ",\"attempts\":" + std::to_string(item.attempts) + ",\"input_tokens\":" + nullable(item.input_tokens) +
        ",\"settlement\":" + (item.settlement ? json::quote(settlement(*item.settlement)) : "null") +
        ",\"reserved_micro_usd\":" + nullable(item.reserved_micro_usd) +
        ",\"charged_micro_usd\":" + nullable(item.charged_micro_usd) +
        ",\"admission_denial\":" + (item.admission_denial ? json::quote(denial(*item.admission_denial)) : "null") +
        ",\"native_carriers\":" + std::to_string(item.native_carriers) +
        ",\"distinct_native_carriers\":" + std::to_string(item.distinct_native_carriers) +
        ",\"duplicate_control\":" + (item.duplicate_control ? "true" : "false") +
        ",\"control_changed\":" + (item.control_changed ? "true" : "false") +
        ",\"output_tokens\":" + nullable(item.output_tokens) + ",\"estimated_micro_usd\":" + nullable(item.estimated_micro_usd) +
        ",\"input_uncached\":" + nullable(item.input_uncached) + ",\"cache_read\":" + nullable(item.cache_read) +
        ",\"cache_write\":" + nullable(item.cache_write) + ",\"reasoning\":" + nullable(item.reasoning) +
        ",\"usage_stage\":" + json::quote(item.usage_stage == UsageStage::Final ? "final" :
            item.usage_stage == UsageStage::Partial ? "partial" : "missing") +
        ",\"usage_quality\":" + json::quote(item.usage_quality == UsageQuality::Consistent ? "consistent" : "inconsistent") +
        ",\"safe_error\":" + (item.failure_kind ? json::quote(runtime::detail::safe_message(*item.failure_kind)) : "null") +
        ",\"http_status\":" + std::to_string(item.http_status) +
        ",\"reasoning_items\":" + std::to_string(item.reasoning_items) +
        ",\"summary_items\":" + std::to_string(item.summary_items) +
        ",\"encrypted_present\":" + (item.encrypted_present ? "true" : "false") +
        ",\"native_complete\":" + (item.native_complete ? "true" : "false") +
        ",\"image_sent\":" + (item.image_sent ? "true" : "false") +
        ",\"reasoning_requested\":" + (item.reasoning_requested ? "true" : "false") +
        ",\"reasoning_disabled\":" + (item.reasoning_disabled ? "true" : "false") +
        ",\"default_off\":" + (item.default_off ? "true" : "false") +
        ",\"visible_reasoning\":" + (item.visible_reasoning ? "true" : "false") +
        ",\"native_present\":" + (item.native_present ? "true" : "false") +
        ",\"vision_correct\":" + (item.vision_correct ? (*item.vision_correct ? "true" : "false") : "null") + '}';
  }
  return out + "]}";
}
std::string_view plan_json() {
  return R"({"version":1,"mode":"plan","state":"not_run","io_performed":false,"execution_modes":{"full":{"repetitions_max":630},"baseline-pair":{"cases":["vision_on_buffered","vision_on_sse"],"paired_runs_per_family":60,"requests_per_family":120,"five_family_requests":600,"same_image_prompt_controls":true},"google-diagnostics":{"repetitions_max":3,"requests_per_repetition_max":5,"requests_per_profile_max":15,"two_family_requests_max":30,"prerequisites":["vision_tool_first","vision_tool_positive"],"generate_controls":["vision_signature_negative","vision_reasoning_missing","vision_carrier_duplicate"],"interactions_round_controls":[["vision_signature_negative","vision_thought_carrier_missing","vision_call_carrier_missing"],["vision_signature_negative","vision_reasoning_missing","vision_carrier_duplicate"],["vision_carrier_duplicate","vision_thought_carrier_missing","vision_call_carrier_missing"]]}},"grant":{"spending_authority":false,"activation":"not_read_in_plan","original":{"additional_calls":630,"micro_usd":1000000},"optional_extension":{"authorization":"config/qualification-authorization-extension.json","additional_calls":480,"micro_usd":3000000},"baseline_calls":99,"baseline_exposure_micro_usd":18979680,"baseline_immutable":true,"restart_renews":false,"excluded_paid_scope":["image_generation","video_generation","remote_graphrag"]},"native_consumption":"unobservable","billing":"local_meter_and_stop_not_invoice","equivalence_admission":false})";
}
std::string_view help_text() {
  return "Usage: sp_canary [--profile FILE] [--project-root DIR] [--mode full|baseline-pair|google-diagnostics] [--env-file FILE] [--max-output-tokens N] [--repetitions N] [--execute] [--test-loopback]\n"
      "Without --execute: fixed plan only; no files, credentials, meter or network are read.\n"
      "--execute requires --profile and --project-root; fixed approved meter/baseline/config paths are used.\n"
      "--env-file overrides the environment; owner-only NAME=value regular file.\n"
      "--max-output-tokens is unchanged and inclusive of thinking; invalid bounds deny before dispatch.\n"
      "--mode full preserves the full historical smoke; --repetitions defaults to1 (max630).\n"
      "--mode baseline-pair --repetitions60 sends precisely on-buffered + on-SSE with the same image/prompt/controls:120/family,600/five families.\n"
      "--mode google-diagnostics --repetitions3 sends at most15 attempts/profile, including fresh native tool generation and retained positive prerequisites.\n"
      "Generate repeats mutation/combined omission/duplication; Interactions rotates independently omitted thought/call carriers across three bounded rounds.\n"
      "Baseline repetition maximum60; diagnostics maximum3 and hard15-attempt invocation limit; no extra financial grant.\n"
      "No automatic provider/semantic retries; exhausted/corrupt admission stops repeated qualification.\n"
      "--test-loopback allows literal loopback origins only and still requires an isolated valid meter campaign.\n"
      "Legacy profile call/token/money caps describe the old scenario only; they never grant spending authority.\n"
      "Output: one sanitized report per repetition, then bounded empirical control statistics.\n"
      "Native acceptance/omission/duplication does not establish consumption; specific rejection is separate evidence.\n"
      "SPCANARY1 baseline/activation stay immutable; restart renews neither original630/$1 nor optional480/$3 extension.\n"
      "Images/Veo/Decisions are outside this chat grant; catalogue debits are not an invoice guarantee.\n"
      "Exit 0: all executed cases passed; 2: failed/invalid; 3: at least one not_run.\n";
}
} // namespace sp::canary
