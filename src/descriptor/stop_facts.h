#pragma once
// The stop facts the policy loader and the descriptor loader share. Not installed: it is not part of the public
// interface.
#include "descriptor/descriptor.h"
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sp::descriptor {
// A stop table: raw stop values with their kinds, in the order a policy or a descriptor states them.
using StopRows = std::vector<std::pair<std::string, StopKind>>;
// The kind a stop table names (`EndTurn`, `MaxTokens`), or nullopt for any other text.
std::optional<StopKind> stop_kind_from_name(std::string_view name) noexcept;
// True for `error`, `failed`, `cancelled` and `incomplete`: raw values whose meaning the codecs decide in code (a
// failure, or a lifecycle status), so no stop table may list one.
bool failure_stop(std::string_view raw) noexcept;
// The rows the embedded policy gives `family`, empty for a family it does not list. They are read from the embedded
// policy text itself, never through builtin_policy(), which may be the policy that is loading.
const StopRows& embedded_stops(std::string_view family);
// True when `rows` list `raw` with a kind other than `kind`.
bool contradicts(const StopRows& rows, std::string_view raw, StopKind kind) noexcept;
} // namespace sp::descriptor
