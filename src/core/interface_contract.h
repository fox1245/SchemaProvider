#pragma once
#include <cstdint>

namespace sp {
// This scalar ABI handshake is intentionally independent of request/value layout.
// The implementation is out-of-line, so a consumer checks the library it loaded,
// not just the headers it compiled. The package remains pre-stable.
struct InterfaceContract {
  std::uint32_t revision;
  std::uint64_t capabilities;
};
inline constexpr std::uint32_t EXPECTED_INTERFACE_REVISION = 7;
namespace capability {
inline constexpr std::uint64_t TypedRuntime = 1ULL << 0;
inline constexpr std::uint64_t PreparedAdmission = 1ULL << 1;
inline constexpr std::uint64_t NullableUsage = 1ULL << 2;
inline constexpr std::uint64_t All5Native = 1ULL << 3;
inline constexpr std::uint64_t TrustedArchive = 1ULL << 4;
inline constexpr std::uint64_t OrderedWireEvents = 1ULL << 5;
// The transport has a native socket implementation for this platform: POSIX descriptors on
// Linux and macOS, Winsock events on Windows. A platform without one is not granted this bit,
// so provider construction fails closed there.
inline constexpr std::uint64_t NativeSocketRuntime = 1ULL << 6;
inline constexpr std::uint64_t RetainedJsonSize = 1ULL << 7;
inline constexpr std::uint64_t CompleteAttemptEvidence = 1ULL << 8;
// The library sends OpenRouterRouting::max_price and binds it into native replay. A library
// built before the field has smaller request layouts and would not send the cap. It lacks this
// bit, so a consumer's require_interface_contract call with RequiredProvider refuses it; its own
// Client construction checks only the constants it was built with.
inline constexpr std::uint64_t OpenRouterRoutingMaxPrice = 1ULL << 9;
// sp::Error carries `reason`. A library built before the member has a smaller Error, which a consumer
// built with this header would read past, so it lacks this bit and a require_interface_contract call
// with RequiredProvider refuses it.
inline constexpr std::uint64_t ErrorReason = 1ULL << 10;
// Every request has `extensions`, the top-level members the policy admits (#32). A library built before the member has
// smaller requests and would not send them, so it lacks this bit and a require_interface_contract call with
// RequiredProvider refuses it.
inline constexpr std::uint64_t PolicyAdmittedExtensions = 1ULL << 11;
inline constexpr std::uint64_t RequiredProvider = TypedRuntime | PreparedAdmission |
    NullableUsage | All5Native | TrustedArchive | OrderedWireEvents | NativeSocketRuntime |
    RetainedJsonSize | CompleteAttemptEvidence | OpenRouterRoutingMaxPrice | ErrorReason | PolicyAdmittedExtensions;
}
InterfaceContract core_interface_contract() noexcept;
std::uint32_t codec_interface_revision() noexcept;
} // namespace sp
