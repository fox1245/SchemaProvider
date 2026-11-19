#include "runtime/cost.h"
#include "descriptor/policy.h"
#include <algorithm>

namespace sp::runtime {
namespace {
pricing::WorstCase unmodeled(std::string subject, std::string detail) {
  pricing::WorstCase out;
  out.unbounded.push_back({pricing::UnboundedBy::Unmodeled, std::move(subject), std::move(detail)});
  return out;
}
pricing::EndpointBound unmodeled_call(std::string subject, std::string detail) {
  pricing::EndpointBound out;
  out.unbounded.push_back({pricing::UnboundedBy::Unmodeled, std::move(subject), std::move(detail)});
  return out;
}
// The attempts the retry policy allows a prepared call: one when retry is off; a call prepared without a policy states
// none.
std::uint64_t allowed_attempts(const PreparedRequest& prepared) {
  const auto* retry = prepared.retry_policy();
  if (!retry) return 0;
  return retry->enabled ? retry->max_attempts : 1;
}
// An endpoint call's bound as a chat request's: its token lines, one invocation and the attempts.
pricing::WorstCase projected(pricing::EndpointBound bound) {
  pricing::WorstCase out;
  out.total = bound.total;
  out.unbounded = std::move(bound.unbounded);
  out.entry = std::move(bound.entry);
  out.model = std::move(bound.model);
  out.attempts = bound.attempts;
  if (out.total) {
    for (const auto& line : bound.lines) {
      if (line.unit == "input") out.input_tokens = line.quantity;
      else if (line.unit == "output") out.output_tokens = line.quantity;
    }
    out.invocation_input_tokens = out.input_tokens;
    out.invocations = 1;
  }
  return out;
}
} // namespace

pricing::WorstCase worst_case_cost(const PreparedRequest& prepared, const descriptor::ModelCatalogue& catalogue,
                                   const WorstCaseOptions& options) {
  if (!prepared.valid() || prepared.error()) return unmodeled("request", "the request did not prepare");
  const auto* descriptor = prepared.descriptor();
  const auto* retry = prepared.retry_policy();
  // An endpoint call is bounded by what the policy states about its operation. One the provider does not bill is
  // bounded at exactly zero, without a catalogue lookup: its model needs no entry, and the usage its answer reports (a
  // generation statistics read, a finished job's poll) is another call's. One that bills its model is bounded by the
  // quantities its request states. The requests a batch submit carries are not priced yet, and an operation whose
  // billing the policy does not state has no bound at all.
  if (!prepared.endpoint().empty()) {
    const auto& facts = prepared.endpoint_facts();
    switch (facts.billing) {
      case descriptor::EndpointBilling::None: {
        pricing::WorstCase out;
        out.total = UsdAmount{0, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
        out.model = facts.model;
        out.attempts = retry ? (retry->enabled ? retry->max_attempts : 1) : 0;
        return out;
      }
      case descriptor::EndpointBilling::Model: {
        EndpointCostOptions endpoint_options;
        endpoint_options.measured_input_tokens = options.measured_input_tokens;
        return projected(endpoint_worst_case_cost(prepared, catalogue, endpoint_options));
      }
      case descriptor::EndpointBilling::Carried:
        return unmodeled(std::string(prepared.endpoint()), "an endpoint call has no catalogue bound");
      case descriptor::EndpointBilling::Unstated:
        break;
    }
    pricing::WorstCase out;
    out.unbounded.push_back({pricing::UnboundedBy::Billing, facts.endpoint + "/" + facts.operation,
                             "the policy does not state whether the provider bills the operation"});
    return out;
  }
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

pricing::EndpointBound endpoint_worst_case_cost(const PreparedRequest& prepared, const descriptor::ModelCatalogue& catalogue,
                                                const EndpointCostOptions& options) {
  const auto* descriptor = prepared.descriptor();
  if (!prepared.valid() || prepared.error() || !descriptor) return unmodeled_call("request", "the request did not prepare");
  if (prepared.endpoint().empty()) return unmodeled_call("request", "a chat request is bounded by worst_case_cost");
  const auto& facts = prepared.endpoint_facts();
  // Only the quantities a request states are priced: an operation that bills its model and states none (one the SDK
  // has no quantities for, such as a policy that bills moderation) would otherwise be bounded at its request fee.
  if (facts.billing == descriptor::EndpointBilling::Model && facts.quantities.empty())
    return unmodeled_call(facts.endpoint + "/" + facts.operation, "the call states no quantities for the operation");
  pricing::EndpointWorstCaseInput input;
  input.facts = &facts;
  input.measured_input_tokens = options.measured_input_tokens;
  input.stated_seconds = options.audio_seconds;
  input.measured_carried_input_tokens = options.measured_carried_input_tokens;
  input.attempts = allowed_attempts(prepared);
  // The provider charges a billed operation of a charge-on-accept fact once it accepts the call, and the runtime sends
  // such a call again only when it never reached the provider or was refused before it was accepted: one attempt bounds
  // it. An operation the policy states is not billed charges nothing, and the runtime retries it as a read.
  const bool billed = facts.billing == descriptor::EndpointBilling::Model || facts.billing == descriptor::EndpointBilling::Carried;
  const auto* fact = descriptor->policy()->endpoint(facts.endpoint, facts.origin);
  input.charge_on_accept = billed && fact && fact->charge_on_accept;
  auto bound = pricing::endpoint_worst_case(input, catalogue);
  // The audio's length is the host's to state: name where.
  if (!options.audio_seconds)
    for (auto& reason : bound.unbounded)
      if (reason.by == pricing::UnboundedBy::Quantity &&
          std::any_of(facts.quantities.begin(), facts.quantities.end(), [&](const EndpointQuantity& q) {
            return q.source == EndpointQuantity::Source::StatedSeconds && q.unit == reason.subject;
          }))
        reason.detail = "the audio's length is not stated (EndpointCostOptions::audio_seconds)";
  return bound;
}
pricing::EndpointBound endpoint_worst_case_cost(const PreparedRequest& prepared, const EndpointCostOptions& options) {
  const auto* descriptor = prepared.descriptor();
  if (!prepared.valid() || prepared.error() || !descriptor) return unmodeled_call("request", "the request did not prepare");
  return endpoint_worst_case_cost(prepared, *descriptor->policy()->catalogue(), options);
}
namespace {
// The count a usage's `extra` holds under `counter`, when it holds one.
std::optional<std::uint64_t> counted(const Usage& usage, std::string_view counter) {
  const auto found = usage.extra.find(std::string(counter));
  if (found == usage.extra.end()) return std::nullopt;
  return found->second.value;
}
// What a result counted: its usage and model, and the item unit `unit` at the count `count` when it has one.
pricing::EndpointCounted counted_by(const Usage& usage, const ServedModel& model, std::string_view unit = {},
                                    std::optional<std::uint64_t> count = std::nullopt) {
  pricing::EndpointCounted out;
  out.usage = &usage;
  out.model = model;
  if (!unit.empty() && count) out.items.emplace_back(std::string(unit), *count);
  return out;
}
} // namespace

pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const images::Result& result, const descriptor::ModelCatalogue& catalogue) {
  auto counted_result = counted_by(result.usage, result.model, "item:image", result.images.size());
  counted_result.input_image_tokens = counted(result.usage, "input_tokens_details.image_tokens");
  return pricing::endpoint_estimate(facts, counted_result, catalogue);
}
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const decisions::Result& result, const descriptor::ModelCatalogue& catalogue) {
  return pricing::endpoint_estimate(facts, counted_by(result.usage, result.model), catalogue);
}
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const embeddings::Result& result, const descriptor::ModelCatalogue& catalogue) {
  return pricing::endpoint_estimate(facts, counted_by(result.usage, result.model), catalogue);
}
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const rerank::Result& result, const descriptor::ModelCatalogue& catalogue) {
  return pricing::endpoint_estimate(facts, counted_by(result.usage, result.model, "item:search_unit", counted(result.usage, "search_units")), catalogue);
}
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const speech::Result& result, const descriptor::ModelCatalogue& catalogue) {
  return pricing::endpoint_estimate(facts, counted_by(result.usage, result.model), catalogue);
}
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const transcription::Result& result, const descriptor::ModelCatalogue& catalogue) {
  return pricing::endpoint_estimate(facts, counted_by(result.usage, result.model, "item:second", counted(result.usage, transcription::seconds_counter)), catalogue);
}
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const jobs::State& result, const descriptor::ModelCatalogue& catalogue) {
  // Whether a job that did not complete bills is not stated.
  if (facts.billing == descriptor::EndpointBilling::Model && result.status != jobs::Status::Completed) {
    pricing::EndpointEstimate out;
    out.unpriced.emplace_back("job");
    return out;
  }
  return pricing::endpoint_estimate(facts, counted_by(result.usage, result.model), catalogue);
}
} // namespace sp::runtime
