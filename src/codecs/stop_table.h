#pragma once
// How a codec reads a provider's stop value. The descriptor's stop table (the family policy's `stop_reasons` with the
// descriptor's additions) gives the kind; a value the table does not list is Unknown, and the codec keeps the value as
// StopReason::raw. Not installed: it is not part of the public interface.
#include "core/value.h"
#include "descriptor/descriptor.h"
#include <string_view>

namespace sp {
// The outcome's stop kind for a table's kind. The two enums name the same kinds in different orders; neither order
// changes, since both are hashed into what sealed history binds.
constexpr StopKind stop_kind_of(descriptor::StopKind kind) noexcept {
  switch (kind) {
    case descriptor::StopKind::EndTurn: return StopKind::EndTurn;
    case descriptor::StopKind::MaxTokens: return StopKind::MaxTokens;
    case descriptor::StopKind::ToolUse: return StopKind::ToolUse;
    case descriptor::StopKind::ContentFilter: return StopKind::ContentFilter;
    case descriptor::StopKind::Refusal: return StopKind::Refusal;
    case descriptor::StopKind::PauseTurn: return StopKind::PauseTurn;
    case descriptor::StopKind::ContextLimit: return StopKind::ContextLimit;
    case descriptor::StopKind::StopSequence: return StopKind::StopSequence;
    case descriptor::StopKind::MalformedCall: return StopKind::MalformedCall;
    case descriptor::StopKind::Compaction: return StopKind::Compaction;
    case descriptor::StopKind::Unknown: return StopKind::Unknown;
  }
  return StopKind::Unknown;
}
// The kind the descriptor's stop table gives `raw`: Unknown for a value the table does not list.
inline StopKind table_stop(const descriptor::ValidatedDescriptor& descriptor, std::string_view raw) {
  return stop_kind_of(descriptor.stop_kind(raw));
}
} // namespace sp
