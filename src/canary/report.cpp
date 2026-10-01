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
std::string nullable(std::optional<std::uint64_t> value) { return value ? std::to_string(*value) : "null"; }
} // namespace
std::string report_json(const Report& report) {
  const auto reserved = report.reserved_known
      ? "{\"calls\":" + std::to_string(report.reserved.calls) + ",\"tokens\":" + std::to_string(report.reserved.tokens) +
        ",\"micro_usd\":" + std::to_string(report.reserved.micro_usd) + '}'
      : std::string("null");
  const bool responses = report.provider == Provider::OpenAIResponses;
  const auto provider = report.provider == Provider::OpenAI || responses ? "openai" :
      report.provider == Provider::Anthropic ? "anthropic" : "gemini";
  std::string out = "{\"version\":1,\"provider\":" + json::quote(provider) +
      ",\"api_family\":" + json::quote(responses ? "openai.responses" :
          report.provider == Provider::Anthropic ? "anthropic.messages" : "openai.chat") +
      ",\"verification_scope\":" + json::quote(responses ? "stateless_reasoning_poc" :
          report.provider == Provider::Gemini ? "text_only_compatibility_smoke" : "canary") +
      ",\"test_only\":" + (report.test_only ? "true" : "false") + ",\"replay\":" + json::quote(replay(report.replay)) +
      ",\"positive_retained\":" + (report.positive_retained ? "true" : "false") +
      ",\"signature_mutated\":" + (report.signature_mutated ? "true" : "false") +
      ",\"ciphertext_mutated\":" + (report.ciphertext_mutated ? "true" : "false") +
      ",\"reasoning_removed\":" + (report.reasoning_removed ? "true" : "false") +
      ",\"native_leaves\":" + std::to_string(report.native_leaves) +
      ",\"reserved\":" + reserved +
      ",\"billing\":\"conditional_conservative_exposure_not_invoice; provider_spending_control_required\","
      "\"usage_estimate\":\"reported_totals_at_profile_max_rates_not_billed_cost\",\"equivalence_admission\":false,\"cases\":[";
  bool comma = false;
  for (const auto& item : report.cases) {
    if (comma) out += ',';
    comma = true;
    // Do not serialize caller-owned identifiers even if a Report was edited.
    std::string_view name = "unknown";
    for (auto allowed : {"text_buffered", "text_sse", "tool_first", "tool_positive", "signature_negative",
                        "reasoning_missing", "ciphertext_negative"})
      if (item.name == allowed) name = allowed;
    out += "{\"name\":" + json::quote(name) + ",\"state\":" + json::quote(state(item.state)) +
        ",\"reason\":" + json::quote(reason(item.reason)) + ",\"dispatched\":" + (item.dispatched ? "true" : "false") +
        ",\"attempts\":" + std::to_string(item.attempts) + ",\"input_tokens\":" + nullable(item.input_tokens) +
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
        ",\"native_complete\":" + (item.native_complete ? "true" : "false") + '}';
  }
  return out + "]}";
}
std::string_view plan_json() {
  return R"({"version":1,"mode":"plan","state":"not_run","io_performed":false,"scenarios":["text_buffered","text_sse","tool_first","tool_positive","anthropic_signature_negative","responses_reasoning_missing","responses_ciphertext_negative"],"maximum_calls":{"openai":4,"anthropic":5,"gemini_text_compatibility":2,"openai_responses":6},"credentials":["OPENAI_API_KEY","ANTHROPIC_API_KEY","GEMINI_API_KEY"],"billing":"conditional_conservative_exposure_not_invoice; provider_spending_control_required","equivalence_admission":false})";
}
std::string_view help_text() {
  return "Usage: sp_canary [--profile FILE] [--ledger FILE] [--env-file FILE] [--execute] [--test-loopback]\n"
      "Without --execute: fixed plan only; no files, credentials, ledger or network are read.\n"
      "--execute requires --profile and --ledger. --env-file overrides the environment.\n"
      "Private env file: NAME=value, optional blank/comment lines; owner-only regular file.\n"
      "--test-loopback permits only literal http://127.0.0.1:PORT or http://[::1]:PORT test origins.\n"
      "Live origins are exact first-party HTTPS origins; no gateway or extra endpoints.\n"
      "Gemini is a two-request text-only OpenAI-compatibility smoke, not native Gemini support.\n"
      "Responses is a stateless HTTP/SSE reasoning/tool canary; no WebSocket or server continuation.\n"
      "Responses has an8-attempt/US$1 sublimit; all OpenAI lanes also share16 attempts/US$10.\n"
      "Exit 0: all cases passed; 2: failed/invalid; 3: at least one not_run.\n"
      "Reservations are durable, shared per provider, never refunded; preserve the ledger across restarts.\n"
      "Provider-side spending controls are required for invoice hard caps. A run is not equivalence admission.\n";
}
} // namespace sp::canary
