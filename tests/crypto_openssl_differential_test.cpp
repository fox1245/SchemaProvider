// Differential check of the in-tree SHA-256/HMAC-SHA256/constant-time compare against OpenSSL's
// libcrypto over many random inputs. OpenSSL is linked into this test only; no SDK library uses it.
#include "crypto/crypto.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
using namespace sp::crypto;
#define CHECK(cond)                                                                       \
  do {                                                                                    \
    if (!(cond)) {                                                                        \
      std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);       \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

struct Rng {
  std::uint64_t s;
  std::uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
  std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
};

Sha256Digest openssl_sha256(const std::string& m) {
  Sha256Digest d{};
  unsigned n = 0;
  CHECK(EVP_Digest(m.data(), m.size(), d.data(), &n, EVP_sha256(), nullptr) == 1 && n == d.size());
  return d;
}
Sha256Digest openssl_hmac(const std::string& k, const std::string& m) {
  Sha256Digest d{};
  unsigned n = 0;
  CHECK(HMAC(EVP_sha256(), k.data(), static_cast<int>(k.size()), reinterpret_cast<const unsigned char*>(m.data()), m.size(), d.data(), &n) != nullptr && n == d.size());
  return d;
}
}  // namespace

int main() {
  Rng rng{0x853c49e6748fea9bULL};
  std::size_t messages = 0, bytes = 0;
  for (int round = 0; round < 30000; ++round) {
    // Dense around the block boundaries, with occasional large messages.
    std::size_t n = round % 11 == 0 ? rng.below(300000) : round % 3 == 0 ? rng.below(300) : rng.below(5000);
    std::string m(n, '\0');
    for (auto& c : m) c = static_cast<char>(rng.next());
    const auto expected = openssl_sha256(m);
    CHECK(sha256(m, Backend::Auto) == expected);
    CHECK(sha256(m, Backend::Portable) == expected);
    Sha256 streamed;  // random chunking against the same OpenSSL digest
    for (std::size_t pos = 0; pos < n;) {
      const auto take = std::min(n - pos, rng.below(300) + 1);
      streamed.update(m.data() + pos, take);
      pos += take;
    }
    CHECK(streamed.finish() == expected);
    std::string key(rng.below(round % 5 == 0 ? 400 : 100), '\0');
    for (auto& c : key) c = static_cast<char>(rng.next());
    const auto mac = openssl_hmac(key, m);
    CHECK(hmac_sha256(key, m, Backend::Auto) == mac);
    CHECK(hmac_sha256(key, m, Backend::Portable) == mac);
    // The compare agrees with CRYPTO_memcmp on equal and on single-bit-different inputs.
    std::string other = m;
    CHECK(constant_time_equal(m, other) == (CRYPTO_memcmp(m.data(), other.data(), n) == 0));
    if (n) {
      other[rng.below(n)] ^= static_cast<char>(1u << rng.below(8));
      CHECK(!constant_time_equal(m, other));
      CHECK(CRYPTO_memcmp(m.data(), other.data(), n) != 0);
    }
    ++messages; bytes += n;
  }
  std::printf("crypto_openssl_differential ok: %zu messages, %zu bytes, OpenSSL %s\n", messages, bytes,
              OpenSSL_version(OPENSSL_VERSION));
  return 0;
}
