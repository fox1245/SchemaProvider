// The typed queries a host asks instead of restating provider facts, and the worst-case cost of a prepared request.
#include "core/enum_names.h"
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace sp;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)

// Every enumerator from zero has a distinct name that reads back, and the one past the last has none. `count` is the
// number of enumerators; a new enumerator makes it differ and fails here until the table and this count follow.
template <class Enum> void names_round_trip(std::size_t count) {
  std::set<std::string_view> seen;
  for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
    const auto value = static_cast<Enum>(ordinal);
    const auto name = to_string(value);
    CHECK(!name.empty() && seen.insert(name).second);
    const auto back = from_string<Enum>(name);
    CHECK(back && *back == value);
  }
  CHECK(to_string(static_cast<Enum>(count)).empty());
  CHECK(!from_string<Enum>("") && !from_string<Enum>("NoSuchEnumerator") && !from_string<Enum>(" "));
}

void enum_names() {
  names_round_trip<Role>(5); names_round_trip<ToolCallKind>(3); names_round_trip<InvalidReason>(6);
  names_round_trip<Evidence>(2); names_round_trip<UsageStage>(3); names_round_trip<UsageQuality>(2);
  names_round_trip<CostSource>(3); names_round_trip<CostStatus>(7); names_round_trip<CostRounding>(3);
  names_round_trip<StopKind>(12); names_round_trip<ErrorKind>(19); names_round_trip<RetryClass>(4);
  names_round_trip<RetrySafety>(4); names_round_trip<ServedRelation>(6); names_round_trip<ImageDetail>(4);
  names_round_trip<MediaKind>(4); names_round_trip<MediaSource>(3);
  names_round_trip<descriptor::StopKind>(11); names_round_trip<descriptor::ModelRelation>(4);
  // The last enumerator of each is the one a host used to bound an ordinal by.
  CHECK(from_string<ErrorKind>("Overloaded") == ErrorKind::Overloaded);
  CHECK(from_string<RetrySafety>("RejectedBeforeOutput") == RetrySafety::RejectedBeforeOutput);
  CHECK(from_string<CostSource>("OpenRouterUsd") == CostSource::OpenRouterUsd);
  CHECK(from_string<StopKind>("Compaction") == StopKind::Compaction);
  // The two StopKinds order their enumerators differently, so the same name is a different ordinal.
  CHECK(from_string<StopKind>("ToolUse") == StopKind::ToolUse && from_string<descriptor::StopKind>("ToolUse") == descriptor::StopKind::ToolUse);
  CHECK(static_cast<int>(StopKind::ToolUse) != static_cast<int>(descriptor::StopKind::ToolUse));
  // Names are case-sensitive.
  CHECK(!from_string<ErrorKind>("overloaded"));
}
} // namespace

int main() {
  try {
    enum_names();
    std::cout << "host query properties passed\n"; return 0;
  }
  catch (const std::exception& error) { std::cerr << "host query test failed: " << error.what() << '\n'; return 1; }
}
