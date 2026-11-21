#pragma once
// What a prepared chat request commits to beyond its body, read from the typed request when it is prepared: the facts
// a host prices or routes it by, so that it need not parse the encoded body or restate the request's members.
#include "core/request_controls.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sp {
// Server tools that share one cap on their uses. Messages gives each tool its own `max_uses`; Responses caps all its
// hosted tools with `max_tool_calls`; the Google families cap none.
struct ToolCap {
  // The tool types the cap bounds. Each is a catalogue fee unit, `tool:<type>` (descriptor::tool_fee_unit).
  std::vector<std::string> types;
  // At most this many uses of the group in the request; unset when the request states none.
  std::optional<std::uint64_t> uses;
  // The request member that carries the cap (`max_uses`, `max_tool_calls`); empty when the family has no such member.
  std::string member;
  // The policy admits that member for these tools. False when it does not (Messages' code execution takes no
  // `max_uses`), so a cap cannot be set and the tools' use stays unbounded.
  bool cap_admitted = false;
};
// A request member the policy admits whose value changes the price (descriptor::ExtensionFact::prices, #207): its key,
// and the catalogue tier the value bills under (as DescriptorPolicy::billing_tier maps it), or `unmodeled` when no
// catalogue row states its price.
struct PricedExtension {
  std::string key;
  std::string tier;
  bool unmodeled = false;
};
struct RequestFacts {
  // The service tier the call bills under: the request's, else the policy default for the model; empty when neither
  // names one. As written: DescriptorPolicy::billing_tier maps the standard tier to the catalogue's unlabelled rates.
  std::string service_tier;
  // The models the gateway may serve instead of `model` (Chat's `models`).
  std::vector<std::string> fallback_models;
  // OpenRouter plugins are attached. The catalogue prices none of them.
  bool plugins = false;
  // The OpenRouter routing the request sends, as the request states it, `max_price` included.
  std::optional<OpenRouterRouting> routing;
  // The tools the service runs, grouped by cap. A tool the caller executes (the policy's `client` tools) bills tokens
  // only and is left out.
  std::vector<ToolCap> tools;
  // The request takes an image as input: a message part, a tool result's part, or input the service holds for a
  // continuation (a Responses `previous_response_id`, an Interactions `previous_interaction_id`), whose images are not
  // visible here. A document or a video part counts too, since providers bill its pages and frames as image input. An
  // entry that prices an image beyond its tokens (`image` in a models listing) has no bound for it, and the bound and
  // the estimate read `input_image` for it (#237).
  bool image_input = false;
  // The extension members the request sends whose value changes the price, in the request's order. A value the policy
  // prices at the standard rates is left out.
  std::vector<PricedExtension> priced_extensions;
};
} // namespace sp
