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
// sp::Completion carries `secret` (#132). A library built before the member has a smaller Completion, which a
// consumer built with this header would read past, so it lacks this bit and a require_interface_contract call
// with RequiredProvider refuses it.
inline constexpr std::uint64_t CompletionSecret = 1ULL << 12;
// The runtime has the public host seam (runtime/host.h): Executor with `host_pumped`, HostTransport and
// Client(descriptor, options, executor, transport). A library built before it has no such constructor and an
// Executor without the virtual, so a host that supplies its own executor requires this bit itself; the bit is not
// part of RequiredProvider, which a consumer that never uses the seam keeps meeting.
inline constexpr std::uint64_t HostSeam = 1ULL << 13;
// Text, Media and ToolResult carry `cache`, Messages tool definitions carry `cache` and a Messages request carries
// `system_blocks`: the typed cache marks. A library built before them has smaller part and request layouts, which a
// consumer built with this header would read past, so it lacks this bit and a require_interface_contract call with
// RequiredProvider refuses it.
inline constexpr std::uint64_t PartCacheMarks = 1ULL << 14;
// ServedModel (Completion::model, PartialCompletion::model) carries `served_by`, the upstream a routing gateway named. A
// library built before the member has a smaller ServedModel, which a consumer built with this header would read past, so
// it lacks this bit and a require_interface_contract call with RequiredProvider refuses it.
inline constexpr std::uint64_t ServedByUpstream = 1ULL << 15;
// transport::TransportOptions carries `connection_cache_size`, the size of libcurl's connection cache. A library built
// before the member has a smaller TransportOptions (and so a smaller runtime::Options), which a consumer built with this
// header would read past, and it keeps idle connections only up to libcurl's own default; it lacks this bit and a
// require_interface_contract call with RequiredProvider refuses it.
inline constexpr std::uint64_t TransportConnectionCache = 1ULL << 16;
inline constexpr std::uint64_t RequiredProvider = TypedRuntime | PreparedAdmission |
    NullableUsage | All5Native | TrustedArchive | OrderedWireEvents | NativeSocketRuntime |
    RetainedJsonSize | CompleteAttemptEvidence | OpenRouterRoutingMaxPrice | ErrorReason | PolicyAdmittedExtensions |
    CompletionSecret | PartCacheMarks | ServedByUpstream | TransportConnectionCache;
}
InterfaceContract core_interface_contract() noexcept;
std::uint32_t codec_interface_revision() noexcept;
} // namespace sp
