#pragma once
// Questions about a failed call that a host asks of its outcome and would otherwise answer from the wire's details.
#include "core/value.h"

namespace sp {
// True when the usage holds nothing at all: no stage, no counter, no conflict and no cost. The provider reported
// nothing, which is not the same as reporting zero.
bool usage_reported_nothing(const Usage&) noexcept;
// True when the provider answered with a complete refusal of the request and accepted nothing: the failure is
// labelled RetrySafety::RejectedBeforeOutput, the request left and a response head arrived, no earlier attempt could
// have been billed (no transport resend, no unknown prior usage), and the failure holds no output, stop reason or
// usage, and no raw event but the diagnostic error body (http_error_event). A host may release the hold it took for
// the call. Missing usage or a status code alone does not prove it.
bool request_declined(const Failure&) noexcept;
} // namespace sp
