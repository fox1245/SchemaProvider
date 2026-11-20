#include "runtime/cost.h"
#include "codecs/batches_internal.h"
#include "core/pricing_internal.h"
#include "crypto/crypto.h"
#include "descriptor/policy.h"
#include "runtime/request_facts.h"
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
// An endpoint call's bound as a chat request's: its token lines, one invocation and the attempts. A call that bills the
// requests it carries states no tokens or invocations of its own: they are its requests', each bounded on its own
// (the bound's lines).
pricing::WorstCase projected(pricing::EndpointBound bound, bool carried = false) {
  pricing::WorstCase out;
  out.total = bound.total;
  out.unbounded = std::move(bound.unbounded);
  out.entry = std::move(bound.entry);
  out.model = std::move(bound.model);
  out.attempts = bound.attempts;
  if (out.total && !carried) {
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
        return projected(endpoint_worst_case_cost(prepared, catalogue), true);
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

namespace {
// The bound of the prepared endpoint call `prepared` over `facts`: its own, or those of an OpenAI batch whose carried
// requests were read from the items of its input file.
pricing::EndpointBound bound_of(const PreparedRequest& prepared, const descriptor::ValidatedDescriptor& descriptor, const EndpointFacts& facts,
                                const descriptor::ModelCatalogue& catalogue, const EndpointCostOptions& options) {
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
  const auto* fact = descriptor.policy()->endpoint(facts.endpoint, facts.origin);
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
// Lower-case hex of the SHA-256 of `bytes`, the spelling of files::File::uploaded_sha256.
std::string sha256_hex(std::string_view bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string hex;
  for (const auto byte : crypto::sha256(bytes)) {
    hex += digits[byte >> 4];
    hex += digits[byte & 15];
  }
  return hex;
}
} // namespace

pricing::EndpointBound endpoint_worst_case_cost(const PreparedRequest& prepared, const descriptor::ModelCatalogue& catalogue,
                                                const EndpointCostOptions& options) {
  const auto* descriptor = prepared.descriptor();
  if (!prepared.valid() || prepared.error() || !descriptor) return unmodeled_call("request", "the request did not prepare");
  if (prepared.endpoint().empty()) return unmodeled_call("request", "a chat request is bounded by worst_case_cost");
  return bound_of(prepared, *descriptor, prepared.endpoint_facts(), catalogue, options);
}
pricing::EndpointBound endpoint_worst_case_cost(const PreparedRequest& prepared, const EndpointCostOptions& options) {
  const auto* descriptor = prepared.descriptor();
  if (!prepared.valid() || prepared.error() || !descriptor) return unmodeled_call("request", "the request did not prepare");
  return endpoint_worst_case_cost(prepared, *descriptor->policy()->catalogue(), options);
}
pricing::EndpointBound batch_worst_case_cost(const PreparedRequest& submit, const descriptor::ValidatedDescriptor& family,
                                             const std::vector<batches::Item>& items, const files::File& input_file,
                                             const descriptor::ModelCatalogue& catalogue, const EndpointCostOptions& options) {
  const auto* descriptor = submit.descriptor();
  if (!submit.valid() || submit.error() || !descriptor) return unmodeled_call("request", "the request did not prepare");
  if (submit.endpoint().empty()) return unmodeled_call("request", "a chat request is bounded by worst_case_cost");
  const auto& facts = submit.endpoint_facts();
  const std::string call = facts.endpoint + "/" + facts.operation;
  if (facts.billing != descriptor::EndpointBilling::Carried || facts.input_file.empty() || !facts.carried.empty())
    return unmodeled_call(call, "the call reads no uploaded input file");
  if (family.base_url() != facts.origin) return unmodeled_call(call, "the items' descriptor is not at the batch's origin");
  // The items are the requests the batch runs only when they are the bytes the submit's file was uploaded with.
  constexpr std::string_view not_the_file = "the items are not the uploaded input file";
  if (input_file.id != facts.input_file || input_file.uploaded_sha256.empty()) return unmodeled_call(call, std::string(not_the_file));
  auto lines = batches::detail::encode_lines(family, items);
  if (std::holds_alternative<Error>(lines)) return unmodeled_call(call, std::string(not_the_file));
  auto& encoded = std::get<batches::detail::EncodedLines>(lines);
  if (sha256_hex(encoded.text) != input_file.uploaded_sha256 || encoded.items.size() != items.size()) return unmodeled_call(call, std::string(not_the_file));
  auto bounded = facts;
  bounded.family = std::string(family.family());
  for (std::size_t i = 0; i < items.size(); ++i) bounded.carried.push_back(detail::carried_request(family, items[i].request, encoded.items[i]));
  return bound_of(submit, *descriptor, bounded, catalogue, options);
}
pricing::EndpointBound batch_worst_case_cost(const PreparedRequest& submit, const descriptor::ValidatedDescriptor& family,
                                             const std::vector<batches::Item>& items, const files::File& input_file, const EndpointCostOptions& options) {
  const auto* descriptor = submit.descriptor();
  if (!submit.valid() || submit.error() || !descriptor) return unmodeled_call("request", "the request did not prepare");
  return batch_worst_case_cost(submit, family, items, input_file, *descriptor->policy()->catalogue(), options);
}

namespace {
// The rate of a batch line: one nano-USD per unit, in nano-USD per million units, so that the line's quantity is the
// request's estimate in nano-USD, as the bound's lines are.
constexpr std::uint64_t nano_usd_rate = 1'000'000;
} // namespace

pricing::EndpointEstimate batch_estimate(const EndpointFacts& submit, const descriptor::ValidatedDescriptor& family,
                                         const std::vector<batches::Line>& lines, const descriptor::ModelCatalogue& catalogue) {
  pricing::EndpointEstimate out;
  const auto refuse = [&](std::string name) {
    if (std::find(out.unpriced.begin(), out.unpriced.end(), name) == out.unpriced.end()) out.unpriced.push_back(std::move(name));
  };
  const auto assume = [&](std::string rule) {
    if (std::find(out.assumptions.begin(), out.assumptions.end(), rule) == out.assumptions.end()) out.assumptions.push_back(std::move(rule));
  };
  if (submit.billing == descriptor::EndpointBilling::None) {
    out.total = UsdAmount{0, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
    return out;
  }
  if (submit.billing != descriptor::EndpointBilling::Carried) refuse("billing");
  // The requests are priced under the carried tier only, as the bound prices them.
  if (submit.carried_tier.empty()) refuse("tier");
  if (family.base_url() != submit.origin) refuse("origin");
  // Inline requests are of the batch client's family, as the bound prices them.
  if (!submit.carried.empty() && family.family() != submit.family) refuse("family");
  if (!out.unpriced.empty()) return out;
  // The bound at a routing gateway is at the route ceiling; the estimate takes the entries' listed rates.
  if (submit.routed_gateway) assume("listed rates");

  std::uint64_t sum = 0;
  bool overflow = false;
  std::optional<std::string> entry;
  bool one_entry = true;
  for (const auto& line : lines) {
    const auto& id = line.custom_id;
    if (!submit.carried.empty() && std::none_of(submit.carried.begin(), submit.carried.end(), [&](const CarriedRequest& r) { return r.custom_id == id; })) {
      refuse(id + ":custom_id");
      continue;
    }
    const Usage* usage = nullptr;
    const ServedModel* model = nullptr;
    if (const auto* completion = std::get_if<Completion>(&line.outcome)) {
      usage = &completion->usage;
      model = &completion->model;
    } else {
      // Whether the provider bills a request it failed, cancelled or let expire is not stated: what its line reports is
      // priced, and the provider may bill more.
      const auto& failure = std::get<Failure>(line.outcome);
      out.lower_bound = true;
      const auto& reported = failure.partial.usage;
      if (!reported.input_total && !reported.input_uncached && !reported.cache_read && !reported.cache_write && !reported.output_total &&
          !reported.reasoning)
        continue;
      usage = &reported;
      model = &failure.partial.model;
    }
    const auto priced = pricing::estimate(catalogue, family.family(), submit.origin, *model, *usage, submit.carried_tier);
    if (!priced.total) {
      for (const auto& name : priced.unpriced) refuse(id + ":" + name);
      continue;
    }
    for (const auto& rule : priced.assumptions) assume(id + ":" + rule);
    out.lower_bound = out.lower_bound || priced.lower_bound;
    out.requested_entry = out.requested_entry || priced.requested_entry;
    out.lines.push_back({id + ":nano_usd", priced.entry, priced.total->nano_usd, nano_usd_rate});
    if (!pricing::detail::checked_add(sum, priced.total->nano_usd, sum)) overflow = true;
    if (!entry) entry = priced.entry;
    else if (*entry != priced.entry) one_entry = false;
  }
  if (overflow) refuse("overflow");
  if (!out.unpriced.empty()) {
    out.lines.clear();
    return out;
  }
  out.total = UsdAmount{sum, Evidence::Derived, CostRounding::CeilingCatalogueProduct};
  if (one_entry && entry) out.entry = *entry;
  return out;
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
