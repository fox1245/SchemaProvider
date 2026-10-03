#pragma once
#include <cstdint>

namespace sp {
// This scalar ABI handshake is intentionally independent of request/value layout.
// The implementation is out-of-line, so a consumer checks the library it loaded,
// not just the headers it compiled. The package remains unstable (0.0.0).
struct InterfaceContract {
  std::uint32_t revision;
  std::uint64_t capabilities;
};
inline constexpr std::uint32_t EXPECTED_INTERFACE_REVISION = 3;
namespace capability {
inline constexpr std::uint64_t TypedRuntime = 1ULL << 0;
inline constexpr std::uint64_t PreparedAdmission = 1ULL << 1;
inline constexpr std::uint64_t NullableUsage = 1ULL << 2;
inline constexpr std::uint64_t All5Native = 1ULL << 3;
inline constexpr std::uint64_t TrustedArchive = 1ULL << 4;
inline constexpr std::uint64_t OrderedWireEvents = 1ULL << 5;
inline constexpr std::uint64_t PosixRuntime = 1ULL << 6;
inline constexpr std::uint64_t RetainedJsonSize = 1ULL << 7;
inline constexpr std::uint64_t CompleteAttemptEvidence = 1ULL << 8;
inline constexpr std::uint64_t RequiredProvider = TypedRuntime | PreparedAdmission |
    NullableUsage | All5Native | TrustedArchive | OrderedWireEvents | PosixRuntime |
    RetainedJsonSize | CompleteAttemptEvidence;
}
InterfaceContract core_interface_contract() noexcept;
std::uint32_t codec_interface_revision() noexcept;
} // namespace sp
