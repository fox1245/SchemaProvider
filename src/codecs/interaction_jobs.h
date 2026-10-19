#pragma once
// A background interaction as a job (D6, D7). The Interactions lifecycle keeps its chat form: a background create
// answers with a `StopKind::Pending` result, and a Get request returns the interaction's state with native replay.
// These functions read that lifecycle in the job vocabulary, so a host journals and polls every provider job the
// same way.
#include "codecs/interactions_request.h"
#include "codecs/jobs.h"
#include <optional>

namespace sp::jobs {
// The job a Pending (or any) Interactions result belongs to: endpoint `interactions`, the descriptor's origin, the
// interaction id and the requested model. Empty when the descriptor is not an Interactions one or the result names
// no interaction.
std::optional<Handle> from_interaction(const descriptor::ValidatedDescriptor&, const Completion&);
// The Interactions request that polls the job: lifecycle Get, by its id. Run it buffered
// (`RunOptions::streaming = false`), as every lifecycle request.
interactions::Request interaction_poll(const Handle&);
// An Interactions outcome as a job state: Pending while queued, Running while in progress, Completed once it
// stopped for any other reason, Cancelled or Failed when the interaction ended that way. Other failures (the
// transport, the deadline) stay failures. The state's raw events are the outcome's.
Outcome interaction_state(const descriptor::ValidatedDescriptor&, const sp::Outcome&);
}  // namespace sp::jobs
