#include "core/enum_names.h"

namespace sp {
// A switch with no default, so a compiler that warns on a missing enumerator (-Wswitch) flags a new one; a value
// outside the enumerators has no name.
#define SP_NAME(E, X) case E::X: return #X

std::string_view to_string(Role value) noexcept {
  switch (value) {
    SP_NAME(Role, System); SP_NAME(Role, Developer); SP_NAME(Role, User); SP_NAME(Role, Assistant); SP_NAME(Role, Tool);
  }
  return {};
}
std::string_view to_string(ToolCallKind value) noexcept {
  switch (value) {
    SP_NAME(ToolCallKind, ClientExecuted); SP_NAME(ToolCallKind, ServerExecuted); SP_NAME(ToolCallKind, ApprovalRequest);
  }
  return {};
}
std::string_view to_string(InvalidReason value) noexcept {
  switch (value) {
    SP_NAME(InvalidReason, Truncated); SP_NAME(InvalidReason, NotJson); SP_NAME(InvalidReason, DuplicateKey);
    SP_NAME(InvalidReason, DepthExceeded); SP_NAME(InvalidReason, Empty); SP_NAME(InvalidReason, Other);
  }
  return {};
}
std::string_view to_string(Evidence value) noexcept {
  switch (value) { SP_NAME(Evidence, Reported); SP_NAME(Evidence, Derived); }
  return {};
}
std::string_view to_string(UsageStage value) noexcept {
  switch (value) { SP_NAME(UsageStage, Missing); SP_NAME(UsageStage, Partial); SP_NAME(UsageStage, Final); }
  return {};
}
std::string_view to_string(UsageQuality value) noexcept {
  switch (value) { SP_NAME(UsageQuality, Consistent); SP_NAME(UsageQuality, Inconsistent); }
  return {};
}
std::string_view to_string(CostSource value) noexcept {
  switch (value) { SP_NAME(CostSource, None); SP_NAME(CostSource, OpenRouterUsd); SP_NAME(CostSource, UnknownCurrency); }
  return {};
}
std::string_view to_string(CostStatus value) noexcept {
  switch (value) {
    SP_NAME(CostStatus, Missing); SP_NAME(CostStatus, Available); SP_NAME(CostStatus, Malformed);
    SP_NAME(CostStatus, PrecisionExceeded); SP_NAME(CostStatus, Overflow); SP_NAME(CostStatus, UnknownCurrency);
    SP_NAME(CostStatus, Conflict);
  }
  return {};
}
std::string_view to_string(CostRounding value) noexcept {
  switch (value) {
    SP_NAME(CostRounding, CeilingParsedBinary64); SP_NAME(CostRounding, CeilingShortestDecimal);
    SP_NAME(CostRounding, CeilingCatalogueProduct);
  }
  return {};
}
std::string_view to_string(StopKind value) noexcept {
  switch (value) {
    SP_NAME(StopKind, EndTurn); SP_NAME(StopKind, ToolUse); SP_NAME(StopKind, MaxTokens); SP_NAME(StopKind, StopSequence);
    SP_NAME(StopKind, ContentFilter); SP_NAME(StopKind, Refusal); SP_NAME(StopKind, PauseTurn);
    SP_NAME(StopKind, ContextLimit); SP_NAME(StopKind, MalformedCall); SP_NAME(StopKind, Unknown);
    SP_NAME(StopKind, Pending); SP_NAME(StopKind, Compaction);
  }
  return {};
}
std::string_view to_string(ErrorKind value) noexcept {
  switch (value) {
    SP_NAME(ErrorKind, InvalidConfig); SP_NAME(ErrorKind, InvalidRequest); SP_NAME(ErrorKind, Unsupported);
    SP_NAME(ErrorKind, Transport); SP_NAME(ErrorKind, ProtocolCorrupt); SP_NAME(ErrorKind, Truncated);
    SP_NAME(ErrorKind, RemoteFailure); SP_NAME(ErrorKind, Cancelled); SP_NAME(ErrorKind, DeadlineExceeded);
    SP_NAME(ErrorKind, ResourceLimit); SP_NAME(ErrorKind, Misuse); SP_NAME(ErrorKind, ReplayIneligible);
    SP_NAME(ErrorKind, Authentication); SP_NAME(ErrorKind, Permission); SP_NAME(ErrorKind, NotFound);
    SP_NAME(ErrorKind, RateLimited); SP_NAME(ErrorKind, QuotaExhausted); SP_NAME(ErrorKind, LimitUnknown);
    SP_NAME(ErrorKind, Overloaded);
  }
  return {};
}
std::string_view to_string(RetryClass value) noexcept {
  switch (value) {
    SP_NAME(RetryClass, Never); SP_NAME(RetryClass, Transient); SP_NAME(RetryClass, AfterReset);
    SP_NAME(RetryClass, Unknown);
  }
  return {};
}
std::string_view to_string(RetrySafety value) noexcept {
  switch (value) {
    SP_NAME(RetrySafety, NotSent); SP_NAME(RetrySafety, PossiblyAccepted); SP_NAME(RetrySafety, RejectedBeforeOutput);
    SP_NAME(RetrySafety, OutputObserved);
  }
  return {};
}
std::string_view to_string(ServedRelation value) noexcept {
  switch (value) {
    SP_NAME(ServedRelation, NotReported); SP_NAME(ServedRelation, Exact); SP_NAME(ServedRelation, Snapshot);
    SP_NAME(ServedRelation, Listed); SP_NAME(ServedRelation, Routed); SP_NAME(ServedRelation, Unexplained);
  }
  return {};
}
std::string_view to_string(ImageDetail value) noexcept {
  switch (value) {
    SP_NAME(ImageDetail, Auto); SP_NAME(ImageDetail, Low); SP_NAME(ImageDetail, High); SP_NAME(ImageDetail, Original);
  }
  return {};
}
std::string_view to_string(MediaKind value) noexcept {
  switch (value) {
    SP_NAME(MediaKind, Image); SP_NAME(MediaKind, Audio); SP_NAME(MediaKind, Video); SP_NAME(MediaKind, Document);
  }
  return {};
}
std::string_view to_string(MediaSource value) noexcept {
  switch (value) { SP_NAME(MediaSource, Inline); SP_NAME(MediaSource, Url); SP_NAME(MediaSource, File); }
  return {};
}
namespace descriptor {
std::string_view to_string(StopKind value) noexcept {
  switch (value) {
    SP_NAME(StopKind, Unknown); SP_NAME(StopKind, EndTurn); SP_NAME(StopKind, MaxTokens); SP_NAME(StopKind, ToolUse);
    SP_NAME(StopKind, ContentFilter); SP_NAME(StopKind, Refusal); SP_NAME(StopKind, PauseTurn);
    SP_NAME(StopKind, ContextLimit); SP_NAME(StopKind, StopSequence); SP_NAME(StopKind, MalformedCall);
    SP_NAME(StopKind, Compaction);
  }
  return {};
}
std::string_view to_string(ModelRelation value) noexcept {
  switch (value) {
    SP_NAME(ModelRelation, Unknown); SP_NAME(ModelRelation, Exact); SP_NAME(ModelRelation, Alias);
    SP_NAME(ModelRelation, Prefix);
  }
  return {};
}
} // namespace descriptor
#undef SP_NAME

// Names are looked up by walking the ordinals until one has no name: the enumerators are contiguous from zero.
template <class Enum> std::optional<Enum> from_string(std::string_view name) noexcept {
  if (name.empty()) return std::nullopt;
  for (std::size_t ordinal = 0;; ++ordinal) {
    const auto value = static_cast<Enum>(ordinal);
    const auto spelled = to_string(value);
    if (spelled.empty()) return std::nullopt;
    if (spelled == name) return value;
  }
}
#define SP_FROM_STRING(E) template std::optional<E> from_string<E>(std::string_view) noexcept
SP_FROM_STRING(Role); SP_FROM_STRING(ToolCallKind); SP_FROM_STRING(InvalidReason); SP_FROM_STRING(Evidence);
SP_FROM_STRING(UsageStage); SP_FROM_STRING(UsageQuality); SP_FROM_STRING(CostSource); SP_FROM_STRING(CostStatus);
SP_FROM_STRING(CostRounding); SP_FROM_STRING(StopKind); SP_FROM_STRING(ErrorKind); SP_FROM_STRING(RetryClass);
SP_FROM_STRING(RetrySafety); SP_FROM_STRING(ServedRelation); SP_FROM_STRING(ImageDetail); SP_FROM_STRING(MediaKind);
SP_FROM_STRING(MediaSource); SP_FROM_STRING(descriptor::StopKind); SP_FROM_STRING(descriptor::ModelRelation);
#undef SP_FROM_STRING
} // namespace sp
