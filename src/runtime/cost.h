#pragma once
// The worst-case cost of a prepared request, from the catalogue's prices: a chat request (core/pricing.h worst_case)
// from the request's own caps, and a non-chat endpoint call (core/endpoint_pricing.h endpoint_worst_case) from the
// quantities and dimensions its prepared facts state.
#include "core/endpoint_pricing.h"
#include "core/pricing.h"
#include "runtime/client.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sp::runtime {
struct WorstCaseOptions {
  // The input tokens of the request as the host counted them; it must not understate the prompt (pricing::WorstCaseInput).
  // Unset: the model's catalogue input limit bounds the input.
  std::optional<std::uint64_t> measured_input_tokens;
};
// What a host knows of an endpoint call beyond its request.
struct EndpointCostOptions {
  // The input tokens of the call as the host counted them. They must not understate the input; they are not checked,
  // and they are capped at the model's input limit (times the inputs, where the limit counts per input). Unset: that
  // limit bounds the input.
  std::optional<std::uint64_t> measured_input_tokens;
  // A transcription: the audio's length in whole seconds, rounded up. It must not understate the audio; it is not
  // checked. Unset: a model that prices `item:second` above zero gives the call no bound.
  std::optional<std::uint64_t> audio_seconds;
  // Measured input tokens of the chat requests a call carries, by custom id. No bound reads them yet.
  std::vector<std::pair<std::string, std::uint64_t>> measured_carried_input_tokens;
};
// Bounds what `prepared` can bill, with the prices of `catalogue` (a host's overlay, merged over the descriptor
// policy's catalogue with merge_catalogues, for instance). The request supplies the rest: its output cap, the model
// invocations and attempts it may make, its service tier, its server tools with their caps, its fallback models and
// its routing. A request that is not prepared and a request with OpenRouter plugins have no bound, and say so. A non-chat
// endpoint call is bounded by its operation's billing (PreparedRequest::endpoint_facts): exactly zero when the policy
// states the provider does not bill it, with no catalogue lookup; no bound (`UnboundedBy::Billing`) when the policy
// does not state it; endpoint_worst_case_cost, without the audio's length and with
// WorstCaseOptions::measured_input_tokens, when the operation bills its model (its token counts, one invocation, and
// the attempts that bound); and no bound (`Unmodeled`) when it bills the requests it carries (a batch submit). Free of
// side effects, and nothing is reserved.
pricing::WorstCase worst_case_cost(const PreparedRequest& prepared, const descriptor::ModelCatalogue& catalogue,
                                   const WorstCaseOptions& options = {});
// With the catalogue of the descriptor's policy.
pricing::WorstCase worst_case_cost(const PreparedRequest& prepared, const WorstCaseOptions& options = {});
// Bounds what the prepared non-chat endpoint call `prepared` can bill: pricing::endpoint_worst_case over the call's
// facts (PreparedRequest::endpoint_facts), at the standard rates of `catalogue`, with the host's `options`. The call's
// retry policy gives the attempts: its `max_attempts`, one when retry is off, none (`Invocations`) without a policy. A
// billed operation of an endpoint fact that charges once it accepts a call (descriptor::EndpointFact::charge_on_accept)
// is bounded at one attempt, since the runtime sends such a call again only when the provider did not accept it; any
// other billed operation at the attempts. An operation that bills its model and states no quantities has no bound
// (`Unmodeled`, subject `<endpoint>/<operation>`). A chat request, and a request that did not prepare, have no bound
// (`Unmodeled`, subject `request`). Free of side effects, and nothing is reserved.
pricing::EndpointBound endpoint_worst_case_cost(const PreparedRequest& prepared, const descriptor::ModelCatalogue& catalogue,
                                                const EndpointCostOptions& options = {});
// With the catalogue of the descriptor's policy.
pricing::EndpointBound endpoint_worst_case_cost(const PreparedRequest& prepared, const EndpointCostOptions& options = {});

// What a finished endpoint call cost by the catalogue, at the standard rates: pricing::endpoint_estimate over the facts
// of the prepared call (PreparedRequest::endpoint_facts, which a host journals with the call) and what its result
// counted. Where the provider reports the cost (Usage::provider_cost, at OpenRouter) that figure is the cost and this
// is a cross-check. Each result type gives:
// - images: its usage, the image tokens of the input (`input_tokens_details.image_tokens` of the usage), and
//   `item:image` the images it returned;
// - decisions and embeddings: its usage;
// - rerank: its usage, and `item:search_unit` the search units it reports (`search_units`);
// - speech: its usage, which counts nothing;
// - transcription: its usage, and `item:second` the audio's length it reports in whole seconds (`seconds` of the usage);
// - a job's state (a video's poll, priced with its submit's facts): its usage, which counts no tokens; a state other
//   than Completed has no total ("job"), since whether a job that did not complete bills is not stated.
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const images::Result& result, const descriptor::ModelCatalogue& catalogue);
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const decisions::Result& result, const descriptor::ModelCatalogue& catalogue);
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const embeddings::Result& result, const descriptor::ModelCatalogue& catalogue);
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const rerank::Result& result, const descriptor::ModelCatalogue& catalogue);
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const speech::Result& result, const descriptor::ModelCatalogue& catalogue);
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const transcription::Result& result, const descriptor::ModelCatalogue& catalogue);
pricing::EndpointEstimate endpoint_estimate(const EndpointFacts& facts, const jobs::State& result, const descriptor::ModelCatalogue& catalogue);
} // namespace sp::runtime
