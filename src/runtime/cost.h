#pragma once
// The worst-case cost of a prepared chat request (core/pricing.h worst_case), from the request's own caps and the
// catalogue's prices.
#include "core/pricing.h"
#include "runtime/client.h"

namespace sp::runtime {
struct WorstCaseOptions {
  // The input tokens of the request as the host counted them; it must not understate the prompt (pricing::WorstCaseInput).
  // Unset: the model's catalogue input limit bounds the input.
  std::optional<std::uint64_t> measured_input_tokens;
};
// Bounds what `prepared` can bill, with the prices of `catalogue` (a host's overlay, merged over the descriptor
// policy's catalogue with merge_catalogues, for instance). The request supplies the rest: its output cap, the model
// invocations and attempts it may make, its service tier, its server tools with their caps, its fallback models and
// its routing. A request that is not prepared, a non-chat endpoint call and a request with OpenRouter plugins have no
// bound, and say so. Free of side effects, and nothing is reserved.
pricing::WorstCase worst_case_cost(const PreparedRequest& prepared, const descriptor::ModelCatalogue& catalogue,
                                   const WorstCaseOptions& options = {});
// With the catalogue of the descriptor's policy.
pricing::WorstCase worst_case_cost(const PreparedRequest& prepared, const WorstCaseOptions& options = {});
} // namespace sp::runtime
