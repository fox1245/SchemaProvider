#pragma once
// What a prepared non-chat endpoint call bills by (runtime::PreparedRequest::endpoint_facts), read from the typed request
// and the policy when the call was prepared, so a host does not restate which operation it reaches or whether the
// provider bills it.
#include "core/request_facts.h"
#include "descriptor/policy.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sp {
// What a quantity of an endpoint call is before the call.
struct EndpointQuantity {
  // The unit a catalogue entry prices it by.
  std::string unit;
  // The most the request lets the call bill; unset when the request does not state it.
  std::optional<std::uint64_t> at_most;
  // Where a bound takes the quantity when `at_most` is unset.
  enum class Source : std::uint8_t { Request, InputLimit, OutputLimit, StatedSeconds } source = Source::Request;
  // InputLimit only: the entry's limit counts per input, so the bound is `per` times it.
  std::uint64_t per = 1;
  // The request fixes the quantity.
  bool exact = false;
};
// A chat request a call carries (an item of a batch submit).
struct CarriedRequest {
  std::string custom_id, model;
  std::optional<std::uint64_t> output_tokens, invocations;
  RequestFacts facts;
};
struct EndpointFacts {
  // The policy fact and the operation the call performs there (`videos` and `poll`), the keys the fact's `paths` and
  // `billing` are read by. PreparedRequest::endpoint() names the codec instead, which a poll shares with its submit.
  std::string endpoint, operation;
  // How the policy says the provider bills the operation (DescriptorPolicy::endpoint_billing).
  descriptor::EndpointBilling billing = descriptor::EndpointBilling::Unstated;
  // The tier the call's carried requests are priced under (DescriptorPolicy::carried_tier); empty unless `billing` is
  // Carried.
  std::string carried_tier;
  // The client's family and origin, and the model the request names: empty for a call that names none (a file, a
  // batch or a generation statistics read), the job's model for a poll and a download.
  std::string family, origin, model;
  // The origin is a routing gateway (descriptor::routed_gateway).
  bool routed_gateway = false;
  // The qualifier dimensions the call offers, sorted by key, and the quantities it bills: a prepared call that bills its
  // model states those of its request (images::endpoint_facts, ...), and any other call states none. Its dimensions
  // hold `operation` with `operation` above and the request's own values, extension members included, so
  // pricing::dearest_item_price and item_price over them give the rates the bound prices at. A member that is set but
  // whose value no dimension can hold (a floating-point number, a signed integer, null, an array, an object, text outside
  // the qualifier grammar) has its key with an empty value: no row matches it, and a row of a priced unit that names
  // the key leaves that unit without a bound. The chat requests it carries, and the uploaded file whose lines are those
  // requests: empty in every prepared call, since the SDK does not fill them yet.
  std::vector<std::pair<std::string, std::string>> dimensions;
  std::vector<EndpointQuantity> quantities;
  std::vector<CarriedRequest> carried;
  std::string input_file;
};
}  // namespace sp
