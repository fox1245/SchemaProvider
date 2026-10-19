#pragma once
// Webhooks (D7): a pure verifier and parser for the signed events a provider posts to a host's server. OpenAI signs
// them with the Standard Webhooks scheme: `webhook-id`, `webhook-timestamp` and `webhook-signature` headers, and an
// HMAC-SHA256 of `id.timestamp.body` under the endpoint's `whsec_` secret. The verifier does no I/O and keeps no
// state: the host passes its clock, and remembers the ids it has handled to refuse a replay. An OpenRouter
// `callback_url` is not signed; treat it as a hint to poll the job, never as an event.
#include "core/value.h"
#include "json/json.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sp::webhooks {
struct Event {
  // The delivery id (`webhook-id`): the same for every retry of one event, so a host keys its replay check on it.
  std::string id;
  // The event's own id, type (`batch.completed`, `response.completed`) and creation time in Unix seconds.
  std::string event_id, type;
  std::uint64_t created_at = 0;
  // What the event is about: `data.id`, a batch or response id.
  std::string subject;
  std::shared_ptr<const json::Document> raw;
};
// Header names are matched without regard to case. `tolerance` bounds how far the signed timestamp may be from `now`
// (Unix seconds), in either direction.
std::variant<Event, Error> verify(std::string_view secret, const std::vector<std::pair<std::string, std::string>>& headers,
                                  std::string_view body, std::uint64_t now, std::uint64_t tolerance = 300);
}  // namespace sp::webhooks
