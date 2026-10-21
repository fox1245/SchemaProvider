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
};
} // namespace sp
