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
  // A batch submit: the input tokens of each chat request it carries as the host counted them, by custom id, each once.
  // They must not understate the input; they are not checked, and each is capped at its model's input limit. A request
  // without one is bounded at that limit.
  std::vector<std::pair<std::string, std::uint64_t>> measured_carried_input_tokens;
};
// Bounds what `prepared` can bill, with the prices of `catalogue` (a host's overlay, merged over the descriptor
// policy's catalogue with merge_catalogues, for instance). The request supplies the rest: its output cap, the model
// invocations and attempts it may make, its service tier, its server tools with their caps, its fallback models and
// its routing. A request that is not prepared and a request with OpenRouter plugins have no bound, and say so. A non-chat
// endpoint call is bounded by its operation's billing (PreparedRequest::endpoint_facts): exactly zero when the policy
// states the provider does not bill it, with no catalogue lookup; no bound (`UnboundedBy::Billing`) when the policy
// does not state it; endpoint_worst_case_cost, without the audio's length and with
// WorstCaseOptions::measured_input_tokens, when the operation bills its model (its token counts, one invocation, the
// attempts that bound, and at a routing gateway `route_ceiling`, `pin` and `pinned` as the endpoint bound states them);
// and endpoint_worst_case_cost, without measured input, when it bills the requests it
// carries (a batch submit): its total, reasons, entry, model and attempts, with no tokens and no invocations of its own,
// since each request it carries is bounded on its own. Free of side effects, and nothing is reserved.
pricing::WorstCase worst_case_cost(const PreparedRequest& prepared, const descriptor::ModelCatalogue& catalogue,
                                   const WorstCaseOptions& options = {});
// With the catalogue of the descriptor's policy.
pricing::WorstCase worst_case_cost(const PreparedRequest& prepared, const WorstCaseOptions& options = {});
// Bounds what the prepared non-chat endpoint call `prepared` can bill: pricing::endpoint_worst_case over the call's
// facts (PreparedRequest::endpoint_facts), at the rates of `catalogue` in the facts' service tier (the standard rates,
// since no endpoint request names a tier), with the host's `options`. The call's
// retry policy gives the attempts: its `max_attempts`, one when retry is off, none (`Invocations`) without a policy. A
// billed operation of an endpoint fact that charges once it accepts a call (descriptor::EndpointFact::charge_on_accept)
// is bounded at one attempt, since the runtime sends such a call again only when the provider did not accept it; any
// other billed operation at the attempts. An operation that bills its model and states no quantities has no bound
// (`Unmodeled`, subject `<endpoint>/<operation>`). A batch submit with inline items (Anthropic, Google) is bounded from
// the requests it carries (pricing::endpoint_worst_case), with `measured_carried_input_tokens`; an OpenAI submit, whose
// requests are in an uploaded file, has no bound here (`Unmodeled`, subject `batches/submit`): batch_worst_case_cost
// bounds it from the items. A chat request, and a request that did not prepare, have no bound (`Unmodeled`, subject
// `request`). An embeddings call at a routing gateway whose request sets `provider.max_price.prompt` is bounded at the
// lower of that cap and the route ceiling, with the pin the request must carry (EndpointBound::pin); every other call at
// a routing gateway has no bound (`RouteCeiling`). Free of side effects, and nothing is reserved.
pricing::EndpointBound endpoint_worst_case_cost(const PreparedRequest& prepared, const descriptor::ModelCatalogue& catalogue,
                                                const EndpointCostOptions& options = {});
// With the catalogue of the descriptor's policy.
pricing::EndpointBound endpoint_worst_case_cost(const PreparedRequest& prepared, const EndpointCostOptions& options = {});
// Bounds an OpenAI batch submit, whose requests are in an uploaded file, from the items the host wrote that file from
// with batches::jsonl(family, items), as endpoint_worst_case_cost bounds an inline batch. Only when the items are that
// file: `input_file` is the file the submit names (its id, at the submit's origin) and its `uploaded_sha256`, the digest
// of the bytes the upload sent (the files::File an upload returns), is the SHA-256 of jsonl(family, items). Otherwise no bound (`Unmodeled`,
// subject `<endpoint>/<operation>`): a call that reads no uploaded input file, a `family` at another origin than the
// submit's, and items that are not the uploaded file, or that do not encode. The requests are priced by `family`'s family
// at the submit's origin. Free of side effects, and nothing is reserved.
pricing::EndpointBound batch_worst_case_cost(const PreparedRequest& submit, const descriptor::ValidatedDescriptor& family,
                                             const std::vector<batches::Item>& items, const files::File& input_file,
                                             const descriptor::ModelCatalogue& catalogue, const EndpointCostOptions& options = {});
// With the catalogue of the submit's descriptor policy.
pricing::EndpointBound batch_worst_case_cost(const PreparedRequest& submit, const descriptor::ValidatedDescriptor& family,
                                             const std::vector<batches::Item>& items, const files::File& input_file,
                                             const EndpointCostOptions& options = {});

// What a finished endpoint call cost by the catalogue, in the facts' service tier: pricing::endpoint_estimate over the facts
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
// What a batch cost by the catalogue: the facts of its submit (PreparedRequest::endpoint_facts, journaled with it) and
// its result lines, each decoded with batches::decode through `family`, the descriptor whose family encoded the requests
// (the batch client's, or at OpenAI the Chat or Responses one that wrote the file). Each line is priced as
// pricing::estimate prices a chat call's usage, by `family`'s family at the submit's origin under the submit's carried
// tier, and rounded on its own, as each request's bound is; the lines are summed exactly. Each line is one EndpointLine:
// unit `<custom_id>:nano_usd`, the entry that priced it, its estimate in nano-USD as the quantity and a rate of one
// nano-USD per unit; `entry` is the one entry every line was priced by, empty when they were priced by several, and
// `requested_entry` is set when any line was priced by its requested model's entry. A request the provider failed,
// cancelled or let expire adds what its line reports (nothing, when it reports no usage), and sets `lower_bound`, since
// whether the provider bills it is not stated. A custom id with more than one line is `<custom_id>:duplicate`, and none
// of its later lines is priced. By the submit's billing:
// `None` is exactly zero; any other value than `Carried` is "billing". Also unpriced: "tier" when the submit names no
// carried tier, "origin" when `family` is at another origin, "family" when the submit carries inline requests of
// another family than `family`'s, `<custom_id>:custom_id` for a line whose request the submit does not carry (inline
// submits only), each name a line's estimate leaves unpriced as `<custom_id>:<name>`, and "overflow". Any unpriced
// name leaves no total and no lines. A line's assumptions are named `<custom_id>:<rule>`, and "listed
// rates" at a routing gateway. For lines within their requests' caps, priced by the entries the bound used, the
// estimate is at most endpoint_worst_case_cost's (or batch_worst_case_cost's) total for one attempt.
pricing::EndpointEstimate batch_estimate(const EndpointFacts& submit, const descriptor::ValidatedDescriptor& family,
                                         const std::vector<batches::Line>& lines, const descriptor::ModelCatalogue& catalogue);
// The same for a batch whose requests are `items` (an OpenAI batch, whose submit carries none inline): a line whose
// custom id names no item is `<custom_id>:custom_id`, and no items is "items".
pricing::EndpointEstimate batch_estimate(const EndpointFacts& submit, const descriptor::ValidatedDescriptor& family,
                                         const std::vector<batches::Item>& items, const std::vector<batches::Line>& lines,
                                         const descriptor::ModelCatalogue& catalogue);
} // namespace sp::runtime
