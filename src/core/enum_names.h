#pragma once
// Stable names for the public enums a host stores or mirrors, so that a journal holds a name and not an ordinal
// that moves when an enumerator is inserted. A name is the enumerator's identifier ("RejectedBeforeOutput"); it
// does not change once released.
#include "core/media.h"
#include "core/value.h"
#include "descriptor/catalogue.h"
#include "descriptor/descriptor.h"
#include <optional>
#include <string_view>

namespace sp {
std::string_view to_string(Role) noexcept;
std::string_view to_string(ToolCallKind) noexcept;
std::string_view to_string(InvalidReason) noexcept;
std::string_view to_string(Evidence) noexcept;
std::string_view to_string(UsageStage) noexcept;
std::string_view to_string(UsageQuality) noexcept;
std::string_view to_string(CostSource) noexcept;
std::string_view to_string(CostStatus) noexcept;
std::string_view to_string(CostRounding) noexcept;
std::string_view to_string(StopKind) noexcept;
std::string_view to_string(ErrorKind) noexcept;
std::string_view to_string(RetryClass) noexcept;
std::string_view to_string(RetrySafety) noexcept;
std::string_view to_string(ServedRelation) noexcept;
std::string_view to_string(ImageDetail) noexcept;
std::string_view to_string(MediaKind) noexcept;
std::string_view to_string(MediaSource) noexcept;
// The descriptor layer's own stop kind (the policy's `stop_reasons` values) and a catalogue lookup's relation.
namespace descriptor {
std::string_view to_string(StopKind) noexcept;
std::string_view to_string(ModelRelation) noexcept;
} // namespace descriptor

// The enumerator that `to_string` spells `name`, or nullopt for any other text. Defined for every enum above:
//   sp::from_string<sp::ErrorKind>("Overloaded"), sp::from_string<sp::descriptor::StopKind>("Compaction").
template <class Enum> std::optional<Enum> from_string(std::string_view name) noexcept;
} // namespace sp
