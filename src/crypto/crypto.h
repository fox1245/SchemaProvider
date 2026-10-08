#pragma once
// Private in-tree primitives for local custody and identity: SHA-256 and HMAC-SHA256 (FIPS 180-4,
// RFC 2104), constant-time equality, secret wiping and OS entropy. The header is standard-library
// only and never installed; callers keep digests as plain byte arrays. The primitives authenticate
// local binding/custody; they are not a general cryptography API.
//
// SHA-256 has a portable reference implementation. On x86-64 with GCC/Clang the SHA extensions are
// selected at run time by CPUID when present; both paths produce identical digests and the portable
// path can always be requested explicitly.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sp::crypto {

inline constexpr std::size_t kSha256DigestSize = 32;
inline constexpr std::size_t kSha256BlockSize = 64;
using Sha256Digest = std::array<unsigned char, kSha256DigestSize>;

// Which implementation computes the compression function. Auto picks the fastest one this process
// can run; Portable always runs the reference code. Digests are identical either way.
enum class Backend { Auto, Portable };

// True when Backend::Auto resolves to a hardware-accelerated SHA-256 on this machine.
bool sha256_accelerated() noexcept;

// Streaming SHA-256. update() accepts any chunking; finish() pads and returns the digest.
// After finish(): finish() is idempotent and returns the same digest again, update() throws
// std::logic_error (a hash is never silently extended), and reset() starts a new message.
// Messages are limited to 2^61 - 1 bytes (the FIPS 180-4 length field); update() throws
// std::length_error beyond that.
// Each context belongs to its invocation: independent contexts may be used concurrently; sharing
// one mutable context requires caller synchronization. Nonempty pointer inputs must be readable
// for the supplied size; nullptr is permitted only for zero bytes.
// Teardown wipes chaining state and the buffered message tail, which may contain native secrets.
class Sha256 {
 public:
  explicit Sha256(Backend backend = Backend::Auto) noexcept;
  ~Sha256();
  void update(const void* data, std::size_t size);
  void update(std::string_view bytes) { update(bytes.data(), bytes.size()); }
  Sha256Digest finish() noexcept;
  void reset() noexcept;
  bool finished() const noexcept { return finished_; }

 private:
  using Compress = void (*)(std::uint32_t state[8], const unsigned char* blocks, std::size_t count) noexcept;
  Compress compress_;
  std::uint32_t state_[8];
  std::uint64_t length_ = 0;  // message bytes consumed so far
  unsigned char buffer_[kSha256BlockSize];
  std::size_t buffered_ = 0;
  bool finished_ = false;
};

// One-shot SHA-256 of a contiguous message.
Sha256Digest sha256(const void* data, std::size_t size, Backend backend = Backend::Auto);
inline Sha256Digest sha256(std::string_view bytes, Backend backend = Backend::Auto) {
  return sha256(bytes.data(), bytes.size(), backend);
}

// HMAC-SHA256 (RFC 2104) with a key of any length.
// Key and message pointers have the same readable-range precondition as Sha256::update.
Sha256Digest hmac_sha256(const void* key, std::size_t key_size, const void* data, std::size_t size,
                         Backend backend = Backend::Auto);
inline Sha256Digest hmac_sha256(std::string_view key, std::string_view bytes, Backend backend = Backend::Auto) {
  return hmac_sha256(key.data(), key.size(), bytes.data(), bytes.size(), backend);
}

// Reads every byte and accumulates differences without content-dependent branches or early exits.
// This is a source-level constant-time construction, not a timing or microarchitectural guarantee.
// Different lengths compare unequal at once: lengths are not secret in any caller. Both nonempty
// inputs must be readable for size bytes.
bool constant_time_equal(const void* a, const void* b, std::size_t size) noexcept;
inline bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
  return a.size() == b.size() && constant_time_equal(a.data(), b.data(), a.size());
}

// Zero memory in a way the compiler may not remove as a dead store.
void cleanse(void* data, std::size_t size) noexcept;

// Fill the buffer from the operating system's cryptographic random source (getrandom, arc4random_buf,
// BCryptGenRandom). Returns false, with the buffer contents unspecified, when the source fails; there
// is no fallback to any other generator.
// Nonempty output must be writable for size bytes; a zero-byte request always succeeds.
[[nodiscard]] bool random_bytes(void* data, std::size_t size) noexcept;

}  // namespace sp::crypto
