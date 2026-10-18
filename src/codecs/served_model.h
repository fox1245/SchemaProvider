#pragma once
// How a decoder classifies the model a response reported (Completion::model).
#include "codecs/accumulator.h"
#include "core/native.h"
#include "core/value.h"
#include "descriptor/descriptor.h"
#include <memory>
#include <string_view>

namespace sp::served {
// Exact, then the requested name with a version or date suffix, then a name the policy's model
// catalogue relates to the request (in either direction), then any name at a declared routing
// gateway; anything else is Unexplained. An empty name is NotReported.
ServedRelation relation(const descriptor::ValidatedDescriptor& descriptor, std::string_view requested,
                        std::string_view served) noexcept;
// Records `served` (empty: none reported yet) against the context's requested model in the
// outcome. For an Unexplained name with `bind_replay`, `context` becomes decoding-only and the
// messages already begun lose replay authority too: they are delivered without a native seal, so
// a continuation that has to replay their native state is ReplayIneligible.
void observe(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator,
             std::shared_ptr<const NativeContext>& context, std::string_view served, bool bind_replay = true);
}  // namespace sp::served
