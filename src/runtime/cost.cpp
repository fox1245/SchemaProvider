#include "runtime/cost.h"
#include "descriptor/policy.h"

namespace sp::runtime {
namespace {
pricing::WorstCase unmodeled(std::string subject, std::string detail) {
  pricing::WorstCase out;
  out.unbounded.push_back({pricing::UnboundedBy::Unmodeled, std::move(subject), std::move(detail)});
  return out;
}
} // namespace

pricing::WorstCase worst_case_cost(const PreparedRequest& prepared, const descriptor::ModelCatalogue& catalogue,
                                   const WorstCaseOptions& options) {
  if (!prepared.valid() || prepared.error()) return unmodeled("request", "the request did not prepare");
  // The catalogue prices chat tokens and the tools of a chat request, not an endpoint call.
  if (!prepared.endpoint().empty()) return unmodeled(std::string(prepared.endpoint()), "an endpoint call has no catalogue bound");
  const auto* descriptor = prepared.descriptor();
  const auto* retry = prepared.retry_policy();
  if (!descriptor) return unmodeled("request", "the request has no admitted descriptor");
  pricing::WorstCaseInput input;
  input.family = prepared.family();
  input.origin = descriptor->base_url();
  input.model = prepared.model();
  const auto& facts = prepared.facts();
  input.facts = &facts;
  input.service_tier = descriptor->policy()->billing_tier(input.family, facts.service_tier);
  input.routed_gateway = descriptor::routed_gateway(*descriptor);
  input.output_tokens = prepared.max_output_tokens();
  input.measured_input_tokens = options.measured_input_tokens;
  input.invocations = prepared.model_invocation_limit();
  // A retry policy that is off makes one attempt. A request prepared without one states no attempts: unbounded.
  if (retry) input.attempts = retry->enabled ? retry->max_attempts : 1;
  else input.attempts = 0;
  return pricing::worst_case(input, catalogue);
}
pricing::WorstCase worst_case_cost(const PreparedRequest& prepared, const WorstCaseOptions& options) {
  const auto* descriptor = prepared.descriptor();
  if (!prepared.valid() || prepared.error() || !descriptor)
    return unmodeled("request", "the request did not prepare");
  return worst_case_cost(prepared, *descriptor->policy()->catalogue(), options);
}
} // namespace sp::runtime
