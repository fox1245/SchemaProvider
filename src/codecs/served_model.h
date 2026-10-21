#pragma once
// How a decoder classifies the model a response reported (Completion::model).
#include "codecs/accumulator.h"
#include "core/native.h"
#include "core/value.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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

// The longest upstream name kept, in bytes.
inline constexpr std::size_t max_upstream_bytes = 256;
// What a response object said about the upstream that served it: nothing contradictory (including no member), a
// member that is not a usable name, or a name that differs from the one already recorded for the response.
enum class UpstreamSeen : std::uint8_t { Fine, Unusable, Conflict };
// Reads the `provider` member of a response object at a declared routing gateway (OpenRouter's top-level member) and
// records the first usable name in the outcome (ServedModel::served_by). `held` is the name recorded so far for this
// response, empty before one is. A usable name is a nonempty string of at most max_upstream_bytes bytes without
// control characters. An origin that is not a declared gateway does not report one: the member is left unread.
UpstreamSeen observe_upstream(const descriptor::ValidatedDescriptor& descriptor, json::Value object, std::string& held,
                              Accumulator& accumulator);
// The member's name for a decoder's known-property list: empty, a name no response uses for a member, where the origin does not report it.
std::string_view upstream_key(const descriptor::ValidatedDescriptor& descriptor) noexcept;
}  // namespace sp::served
