#include "codecs/interaction_jobs.h"
#include "codecs/reason.h"

namespace sp::jobs {
namespace {
std::optional<Handle> handle_of(const descriptor::ValidatedDescriptor& descriptor, const std::vector<Message>& messages, const ServedModel& model) {
  if (descriptor.family() != "google.interactions") return std::nullopt;
  for (const auto& message : messages)
    if (!message.id.empty()) return Handle{"interactions", std::string(descriptor.base_url()), message.id, model.requested};
  return std::nullopt;
}
}  // namespace
std::optional<Handle> from_interaction(const descriptor::ValidatedDescriptor& descriptor, const Completion& completion) {
  return handle_of(descriptor, completion.messages, completion.model);
}
interactions::Request interaction_poll(const Handle& handle) {
  interactions::Request request;
  request.model = handle.model;
  request.lifecycle = interactions::Lifecycle::Get;
  request.interaction_id = handle.id;
  return request;
}
Outcome interaction_state(const descriptor::ValidatedDescriptor& descriptor, const sp::Outcome& outcome) {
  State state;
  if (const auto* failure = std::get_if<Failure>(&outcome)) {
    // An interaction that ended failed or cancelled is a finished job; anything else did not reach its state.
    const bool cancelled = failure->error.kind == ErrorKind::Cancelled && failure->error.reason == reason::code(reason::Source::Interactions, "interaction cancelled");
    const bool failed = failure->error.reason == reason::code(reason::Source::Interactions, "interaction failed");
    if (!cancelled && !failed) return *failure;
    state.status = cancelled ? Status::Cancelled : Status::Failed;
    state.raw_status = cancelled ? "cancelled" : "failed";
    if (auto handle = handle_of(descriptor, failure->partial.messages, failure->partial.model)) state.handle = std::move(*handle);
    state.attempt = failure->error.attempt;
    state.raw_events = failure->partial.raw_events;
    return state;
  }
  const auto& completion = std::get<Completion>(outcome);
  if (auto handle = from_interaction(descriptor, completion)) state.handle = std::move(*handle);
  state.raw_status = completion.stop.raw;
  state.status = completion.stop.kind == StopKind::Pending ? (completion.stop.raw == "queued" ? Status::Pending : Status::Running)
               : completion.stop.raw == "cancelled" ? Status::Cancelled : Status::Completed;
  state.usage = completion.usage;
  state.model = completion.model;
  state.attempt = completion.attempt;
  state.raw_events = completion.raw_events;
  return state;
}
}  // namespace sp::jobs
