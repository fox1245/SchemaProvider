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
  const bool vision = vision_provider(report.provider);
  const bool responses = report.provider == Provider::OpenAIResponses || report.provider == Provider::VisionResponses;
  const bool messages = report.provider == Provider::Anthropic || report.provider == Provider::VisionMessages;
  const auto provider = report.provider == Provider::OpenAI || report.provider == Provider::VisionChat || responses ? "openai" :
      messages ? "anthropic" : "gemini";
  const auto family = responses ? "openai.responses" : messages ? "anthropic.messages" :
      report.provider == Provider::VisionGemini ? "google.generate" :
      report.provider == Provider::VisionInteractions ? "google.interactions" : "openai.chat";
  const auto campaign = report.campaign_reserved
      ? "{\"calls\":" + std::to_string(report.campaign_reserved->calls) + ",\"tokens\":" +
        std::to_string(report.campaign_reserved->tokens) + ",\"micro_usd\":" +
        std::to_string(report.campaign_reserved->micro_usd) + '}'
      : std::string("null");
  std::string out = "{\"version\":1,\"provider\":" + json::quote(provider) +
      ",\"api_family\":" + json::quote(family) +
      ",\"verification_scope\":" + json::quote(vision ? "vision_reasoning_function_probe" :
          responses ? "stateless_reasoning_poc" :
          report.provider == Provider::Gemini ? "text_only_compatibility_smoke" : "canary") +
      ",\"test_only\":" + (report.test_only ? "true" : "false") + ",\"replay\":" + json::quote(replay(report.replay)) +
      ",\"positive_retained\":" + (report.positive_retained ? "true" : "false") +
      ",\"signature_mutated\":" + (report.signature_mutated ? "true" : "false") +
      ",\"ciphertext_mutated\":" + (report.ciphertext_mutated ? "true" : "false") +
      ",\"reasoning_removed\":" + (report.reasoning_removed ? "true" : "false") +
      ",\"native_leaves\":" + std::to_string(report.native_leaves) +
      ",\"reserved\":" + reserved + ",\"campaign_reserved\":" + campaign +
      ",\"billing\":\"conditional_conservative_exposure_not_invoice; provider_spending_control_required\","
      "\"usage_estimate\":\"reported_totals_at_profile_max_rates_not_billed_cost\",\"equivalence_admission\":false,\"cases\":[";
  bool comma = false;
  for (const auto& item : report.cases) {
    if (comma) out += ',';
    comma = true;
    // Do not serialize caller-owned identifiers even if a Report was edited.
    std::string_view name = "unknown";
    for (auto allowed : {"text_buffered", "text_sse", "tool_first", "tool_positive", "signature_negative",
                        "reasoning_missing", "ciphertext_negative", "vision_off_buffered", "vision_on_buffered",
                        "vision_on_sse", "vision_changed", "vision_tool_first", "vision_tool_positive",
                        "vision_signature_negative", "vision_reasoning_missing"})
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
  return R"({"version":1,"mode":"plan","state":"not_run","io_performed":false,"scenarios":["text_buffered","text_sse","tool_first","tool_positive","anthropic_signature_negative","responses_reasoning_missing","responses_ciphertext_negative","vision_off_buffered","vision_on_buffered","vision_on_sse","vision_changed","vision_tool_first","vision_tool_positive","vision_signature_negative","vision_reasoning_missing"],"maximum_calls":{"openai":4,"anthropic":5,"gemini_text_compatibility":2,"openai_responses":6,"vision_chat":6,"vision_responses":8,"vision_messages":8,"vision_gemini":8,"vision_interactions":8},"vision_campaign_cap":{"calls":100,"micro_usd":19795635},"vision_additional_allowance":{"calls":60,"micro_usd":12000000,"baseline_calls":40,"baseline_micro_usd":7795635},"credentials":["OPENAI_API_KEY","ANTHROPIC_API_KEY","GEMINI_API_KEY"],"billing":"conditional_conservative_exposure_not_invoice; provider_spending_control_required","equivalence_admission":false})";
}
std::string_view help_text() {
  return "Usage: sp_canary [--profile FILE] [--ledger FILE] [--env-file FILE] [--execute] [--test-loopback]\n"
      "Without --execute: fixed plan only; no files, credentials, ledger or network are read.\n"
      "--execute requires --profile and --ledger. --env-file overrides the environment.\n"
      "Private env file: NAME=value, optional blank/comment lines; owner-only regular file.\n"
      "--test-loopback permits only literal http://127.0.0.1:PORT or http://[::1]:PORT test origins.\n"
      "Live origins are exact first-party HTTPS origins; no gateway or extra endpoints.\n"
      "Legacy Gemini profile is a two-request text-only OpenAI-compatibility smoke.\n"
      "Vision profiles exercise Chat, Responses, Messages, native Gemini and Interactions HTTP/SSE.\n"
      "OpenAI vision profiles require gpt-6-luna; its Chat tool loop uses reasoning_effort:none.\n"
      "Vision cumulative cap is100 attempts/US$19.795635: prior40/US$7.795635 plus additional60/US$12.\n"
      "Responses is a stateless HTTP/SSE reasoning/tool canary; no WebSocket or server continuation.\n"
      "Legacy Responses has an8-attempt/US$1 sublimit; legacy OpenAI lanes share16 attempts/US$10.\n"
      "Exit 0: all cases passed; 2: failed/invalid; 3: at least one not_run.\n"
      "Reservations are durable, shared per provider, never refunded; preserve the ledger across restarts.\n"
      "Provider-side spending controls are required for invoice hard caps. A run is not equivalence admission.\n";
}
} // namespace sp::canary
