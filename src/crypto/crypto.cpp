#include "crypto/crypto.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#if defined(_MSC_VER)
#pragma comment(lib, "bcrypt.lib")
#endif
#elif defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdlib.h>
#endif

// x86 SHA extensions are used only where the compiler can emit them per function (GCC/Clang) and the
// CPU reports them at run time. Everything else, including ARM, runs the portable reference.
// Defining SP_CRYPTO_NO_ACCEL at build time removes the fast path entirely.
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(SP_CRYPTO_NO_ACCEL)
#define SP_CRYPTO_SHANI 1
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace sp::crypto {
namespace {

// FIPS 180-4 section 4.2.2: the first 32 bits of the fractional parts of the cube roots of the
// first 64 primes.
constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// FIPS 180-4 section 5.3.3: the first 32 bits of the fractional parts of the square roots of the
// first eight primes.
constexpr std::uint32_t kInitial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

// FIPS 180-4 section 5.1.1: a message is at most 2^64 - 1 bits.
constexpr std::uint64_t kMaxMessageBytes = (std::uint64_t{1} << 61) - 1;

constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32 - n)); }

std::uint32_t load_be32(const unsigned char* p) noexcept {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
void store_be32(unsigned char* p, std::uint32_t v) noexcept {
  p[0] = static_cast<unsigned char>(v >> 24); p[1] = static_cast<unsigned char>(v >> 16);
  p[2] = static_cast<unsigned char>(v >> 8); p[3] = static_cast<unsigned char>(v);
}

// Reference compression function, FIPS 180-4 section 6.2.2.
void compress_portable(std::uint32_t state[8], const unsigned char* data, std::size_t blocks) noexcept {
  for (; blocks; --blocks, data += kSha256BlockSize) {
    std::uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) w[i] = load_be32(data + 4 * i);
    for (unsigned i = 16; i < 64; ++i) {
      const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto a = state[0], b = state[1], c = state[2], d = state[3];
    auto e = state[4], f = state[5], g = state[6], h = state[7];
    for (unsigned i = 0; i < 64; ++i) {
      const auto big1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const auto choose = (e & f) ^ (~e & g);
      const auto t1 = h + big1 + choose + kK[i] + w[i];
      const auto big0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto t2 = big0 + majority;
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
  }
}

#if SP_CRYPTO_SHANI
#define SP_SHANI_TARGET __attribute__((target("sha,sse4.1,ssse3")))

bool cpu_has_sha() noexcept {
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (!__get_cpuid(0, &a, &b, &c, &d) || a < 7) return false;
  if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
  const bool ssse3 = (c & (1u << 9)) != 0, sse41 = (c & (1u << 19)) != 0;
  if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
  return ssse3 && sse41 && (b & (1u << 29)) != 0;  // CPUID.(EAX=7,ECX=0):EBX.SHA[bit 29]
}

// Four rounds with the message words in m and the constants kK[4*i .. 4*i+3]. The SHA extension keeps
// the working variables as ABEF (s0) and CDGH (s1).
#define SP_SHANI_ROUNDS(m, i)                                                                   \
  do {                                                                                          \
    __m128i msg = _mm_add_epi32((m), _mm_loadu_si128(reinterpret_cast<const __m128i*>(kK + 4 * (i)))); \
    s1 = _mm_sha256rnds2_epu32(s1, s0, msg);                                                    \
    msg = _mm_shuffle_epi32(msg, 0x0E);                                                         \
    s0 = _mm_sha256rnds2_epu32(s0, s1, msg);                                                    \
  } while (false)
// The next four message words from the previous sixteen: a is the oldest group and is replaced.
#define SP_SHANI_SCHEDULE(a, b, c, d) \
  (a) = _mm_sha256msg2_epu32(_mm_add_epi32(_mm_sha256msg1_epu32((a), (b)), _mm_alignr_epi8((d), (c), 4)), (d))

SP_SHANI_TARGET void compress_shani(std::uint32_t state[8], const unsigned char* data, std::size_t blocks) noexcept {
  const __m128i byte_swap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
  __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state));          // DCBA
  __m128i s1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state + 4));       // HGFE
  tmp = _mm_shuffle_epi32(tmp, 0xB1);                                               // CDAB
  s1 = _mm_shuffle_epi32(s1, 0x1B);                                                 // EFGH
  __m128i s0 = _mm_alignr_epi8(tmp, s1, 8);                                         // ABEF
  s1 = _mm_blend_epi16(s1, tmp, 0xF0);                                              // CDGH
  for (; blocks; --blocks, data += kSha256BlockSize) {
    const __m128i abef = s0, cdgh = s1;
    __m128i m0 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data)), byte_swap);
    __m128i m1 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16)), byte_swap);
    __m128i m2 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 32)), byte_swap);
    __m128i m3 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 48)), byte_swap);
    SP_SHANI_ROUNDS(m0, 0); SP_SHANI_ROUNDS(m1, 1); SP_SHANI_ROUNDS(m2, 2); SP_SHANI_ROUNDS(m3, 3);
    SP_SHANI_SCHEDULE(m0, m1, m2, m3); SP_SHANI_ROUNDS(m0, 4);
    SP_SHANI_SCHEDULE(m1, m2, m3, m0); SP_SHANI_ROUNDS(m1, 5);
    SP_SHANI_SCHEDULE(m2, m3, m0, m1); SP_SHANI_ROUNDS(m2, 6);
    SP_SHANI_SCHEDULE(m3, m0, m1, m2); SP_SHANI_ROUNDS(m3, 7);
    SP_SHANI_SCHEDULE(m0, m1, m2, m3); SP_SHANI_ROUNDS(m0, 8);
    SP_SHANI_SCHEDULE(m1, m2, m3, m0); SP_SHANI_ROUNDS(m1, 9);
    SP_SHANI_SCHEDULE(m2, m3, m0, m1); SP_SHANI_ROUNDS(m2, 10);
    SP_SHANI_SCHEDULE(m3, m0, m1, m2); SP_SHANI_ROUNDS(m3, 11);
    SP_SHANI_SCHEDULE(m0, m1, m2, m3); SP_SHANI_ROUNDS(m0, 12);
    SP_SHANI_SCHEDULE(m1, m2, m3, m0); SP_SHANI_ROUNDS(m1, 13);
    SP_SHANI_SCHEDULE(m2, m3, m0, m1); SP_SHANI_ROUNDS(m2, 14);
    SP_SHANI_SCHEDULE(m3, m0, m1, m2); SP_SHANI_ROUNDS(m3, 15);
    s0 = _mm_add_epi32(s0, abef);
    s1 = _mm_add_epi32(s1, cdgh);
  }
  tmp = _mm_shuffle_epi32(s0, 0x1B);        // FEBA
  s1 = _mm_shuffle_epi32(s1, 0xB1);         // DCHG
  s0 = _mm_blend_epi16(tmp, s1, 0xF0);      // DCBA
  s1 = _mm_alignr_epi8(s1, tmp, 8);         // HGFE
  _mm_storeu_si128(reinterpret_cast<__m128i*>(state), s0);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(state + 4), s1);
}
#undef SP_SHANI_ROUNDS
#undef SP_SHANI_SCHEDULE
#endif  // SP_CRYPTO_SHANI

using Compress = void (*)(std::uint32_t[8], const unsigned char*, std::size_t) noexcept;

Compress fastest() noexcept {
#if SP_CRYPTO_SHANI
  static const bool accelerated = cpu_has_sha();
  if (accelerated) return compress_shani;
#endif
  return compress_portable;
}

// Wipes a local when scope exits, including by exception.
struct Wipe {
  void* data; std::size_t size;
  ~Wipe() { cleanse(data, size); }
};

}  // namespace

bool sha256_accelerated() noexcept { return fastest() != compress_portable; }

Sha256::Sha256(Backend backend) noexcept
    : compress_(backend == Backend::Portable ? compress_portable : fastest()) {
  reset();
}

Sha256::~Sha256() {
  cleanse(state_, sizeof state_);
  cleanse(buffer_, sizeof buffer_);
}

void Sha256::reset() noexcept {
  std::copy(std::begin(kInitial), std::end(kInitial), state_);
  length_ = 0;
  buffered_ = 0;
  finished_ = false;
}

void Sha256::update(const void* data, std::size_t size) {
  if (finished_) throw std::logic_error("Sha256::update after finish");
  if (size == 0) return;
  if (size > kMaxMessageBytes - length_) throw std::length_error("Sha256 message too long");
  auto* p = static_cast<const unsigned char*>(data);
  length_ += size;
  if (buffered_) {
    const auto take = std::min(kSha256BlockSize - buffered_, size);
    std::memcpy(buffer_ + buffered_, p, take);
    buffered_ += take; p += take; size -= take;
    if (buffered_ < kSha256BlockSize) return;
    compress_(state_, buffer_, 1);
    buffered_ = 0;
  }
  if (const auto blocks = size / kSha256BlockSize) {
    compress_(state_, p, blocks);
    p += blocks * kSha256BlockSize; size -= blocks * kSha256BlockSize;
  }
  if (size) { std::memcpy(buffer_, p, size); buffered_ = size; }
}

Sha256Digest Sha256::finish() noexcept {
  if (!finished_) {
    // Padding, FIPS 180-4 section 5.1.1: a one bit, zeros to 56 mod 64, the 64-bit message length in bits.
    buffer_[buffered_++] = 0x80;
    if (buffered_ > kSha256BlockSize - 8) {
      std::memset(buffer_ + buffered_, 0, kSha256BlockSize - buffered_);
      compress_(state_, buffer_, 1);
      buffered_ = 0;
    }
    std::memset(buffer_ + buffered_, 0, kSha256BlockSize - 8 - buffered_);
    const std::uint64_t bits = length_ * 8;
    store_be32(buffer_ + 56, static_cast<std::uint32_t>(bits >> 32));
    store_be32(buffer_ + 60, static_cast<std::uint32_t>(bits));
    compress_(state_, buffer_, 1);
    buffered_ = 0;
    finished_ = true;
  }
  Sha256Digest digest;
  for (unsigned i = 0; i < 8; ++i) store_be32(digest.data() + 4 * i, state_[i]);
  return digest;
}

Sha256Digest sha256(const void* data, std::size_t size, Backend backend) {
  Sha256 hash(backend);
  hash.update(data, size);
  return hash.finish();
}

Sha256Digest hmac_sha256(const void* key, std::size_t key_size, const void* data, std::size_t size, Backend backend) {
  // RFC 2104 with B = 64: a longer key is replaced by its digest, a shorter one is zero padded.
  unsigned char block[kSha256BlockSize] = {};
  unsigned char pad[kSha256BlockSize];
  Wipe wipe_block{block, sizeof block}, wipe_pad{pad, sizeof pad};
  Sha256 inner(backend);
  if (key_size > kSha256BlockSize) {
    inner.update(key, key_size);
    auto reduced = inner.finish();
    Wipe wipe_reduced{reduced.data(), reduced.size()};
    std::memcpy(block, reduced.data(), reduced.size());
    inner.reset();
  } else if (key_size) {
    std::memcpy(block, key, key_size);
  }
  for (std::size_t i = 0; i < sizeof pad; ++i) pad[i] = block[i] ^ 0x36;
  inner.update(pad, sizeof pad);
  inner.update(data, size);
  auto inner_digest = inner.finish();
  Wipe wipe_digest{inner_digest.data(), inner_digest.size()};
  Sha256 outer(backend);
  for (std::size_t i = 0; i < sizeof pad; ++i) pad[i] = block[i] ^ 0x5c;
  outer.update(pad, sizeof pad);
  outer.update(inner_digest.data(), inner_digest.size());
  return outer.finish();
}

bool constant_time_equal(const void* a, const void* b, std::size_t size) noexcept {
  // Volatile reads keep the compiler from stopping at the first difference.
  const volatile unsigned char* x = static_cast<const volatile unsigned char*>(a);
  const volatile unsigned char* y = static_cast<const volatile unsigned char*>(b);
  unsigned char difference = 0;
  for (std::size_t i = 0; i < size; ++i) difference = static_cast<unsigned char>(difference | (x[i] ^ y[i]));
  return difference == 0;
}

void cleanse(void* data, std::size_t size) noexcept {
  volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
  while (size--) *p++ = 0;
}

bool random_bytes(void* data, std::size_t size) noexcept {
  auto* p = static_cast<unsigned char*>(data);
#if defined(_WIN32)
  while (size) {
    const auto chunk = static_cast<ULONG>(std::min<std::size_t>(size, 1u << 20));
    if (BCryptGenRandom(nullptr, p, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    p += chunk; size -= chunk;
  }
  return true;
#elif defined(__linux__)
  while (size) {
    const auto got = ::getrandom(p, size, 0);
    if (got < 0) { if (errno == EINTR) continue; return false; }
    if (got == 0) return false;
    p += got; size -= static_cast<std::size_t>(got);
  }
  return true;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  if (size) ::arc4random_buf(p, size);
  return true;
#else
  (void)p;
  return size == 0;  // no supported OS entropy source: fail closed
#endif
}

}  // namespace sp::crypto
