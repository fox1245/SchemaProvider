// Host-native archive custody Slice 1 primitives: HKDF-SHA256, XChaCha20-Poly1305, derive_kek,
// make_hkdf_key_source, LockedKeyBuffer, CleansedBuffer release observer, and CustodyFault mapping.
#include "core/interface_contract.h"
#include "core/native_archive.h"
#include "core/native_archive_custody.h"
#include "core/native_archive_custody_format.h"
#include "core/native_archive_custody_internal.h"
#include "crypto/crypto.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace sp;

#define CHECK(c)                                                                                \
  do {                                                                                          \
    if (!(c)) {                                                                                 \
      throw std::runtime_error("custody check failed at line " + std::to_string(__LINE__) +     \
                               ": " #c);                                                        \
    }                                                                                           \
  } while (false)

unsigned char hex_nibble(char c) {
  if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(10 + (c - 'a'));
  if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(10 + (c - 'A'));
  throw std::runtime_error("invalid hex digit");
}

std::string from_hex(std::string_view hex) {
  CHECK(hex.size() % 2 == 0);
  std::string out(hex.size() / 2, '\0');
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<char>((hex_nibble(hex[2 * i]) << 4) | hex_nibble(hex[2 * i + 1]));
  }
  return out;
}

std::string to_hex(const void* data, std::size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 0x0f]);
  }
  return out;
}

std::string to_hex(std::string_view bytes) {
  return to_hex(bytes.data(), bytes.size());
}

void write_be64(char out[8], std::uint64_t value) noexcept {
  for (unsigned i = 0; i < 8; ++i) {
    out[7 - i] = static_cast<char>(value >> (i * 8));
  }
}

std::string encode_text(std::string_view text) {
  std::string out(8 + text.size(), '\0');
  write_be64(out.data(), text.size());
  if (!text.empty()) {
    std::memcpy(out.data() + 8, text.data(), text.size());
  }
  return out;
}

std::string build_epoch_aad(const std::array<unsigned char, 32>& store_id,
                            std::uint64_t epoch_id,
                            std::string_view key_id) {
  std::string out = encode_text(custody_format::epoch_aad_domain);
  char ver[8];
  write_be64(ver, custody_format::header_format_version);
  out.append(ver, 8);
  out.append(reinterpret_cast<const char*>(store_id.data()), store_id.size());
  char ep[8];
  write_be64(ep, epoch_id);
  out.append(ep, 8);
  out.append(encode_text(key_id));
  return out;
}

bool all_bytes_zero(const unsigned char* bytes, std::size_t size) noexcept {
  for (std::size_t i = 0; i < size; ++i) {
    if (bytes[i] != 0) {
      return false;
    }
  }
  return true;
}

void test_rfc5869_vectors() {
  struct Vector {
    std::string_view ikm_hex;
    std::string_view salt_hex;
    std::string_view info_hex;
    std::size_t l;
    std::string_view prk_hex;
    std::string_view okm_hex;
  };
  // RFC 5869 Appendix A.1, A.2, A.3 (SHA-256 test cases 1-3).
  constexpr Vector kVectors[] = {
      {
          "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
          "000102030405060708090a0b0c",
          "f0f1f2f3f4f5f6f7f8f9",
          42,
          "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
          "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
      },
      {
          "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
          "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
          "404142434445464748494a4b4c4d4e4f",
          "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
          "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
          "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
          "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
          "d0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
          "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
          82,
          "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244",
          "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
          "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
          "cc30c58179ec3e87c14c01d5c1f3434f1d87",
      },
      {
          "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
          "",
          "",
          42,
          "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04",
          "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8",
      },
  };

  for (const auto& v : kVectors) {
    const std::string ikm = from_hex(v.ikm_hex);
    const std::string salt = from_hex(v.salt_hex);
    const std::string info = from_hex(v.info_hex);

    auto prk_res = custody_ops::hkdf_sha256_extract(salt, ikm);
    CHECK(std::holds_alternative<DataKeyBuffer>(prk_res));
    const auto& prk = std::get<DataKeyBuffer>(prk_res);
    CHECK(to_hex(prk.data(), prk.size()) == v.prk_hex);

    auto okm_res = custody_ops::hkdf_sha256_expand(prk, info, v.l);
    CHECK(std::holds_alternative<std::string>(okm_res));
    const auto& okm = std::get<std::string>(okm_res);
    CHECK(to_hex(okm) == v.okm_hex);

    auto key_res = custody_ops::hkdf_sha256_expand_key(prk, info);
    CHECK(std::holds_alternative<DataKeyBuffer>(key_res));
    const auto& key32 = std::get<DataKeyBuffer>(key_res);
    CHECK(to_hex(key32.data(), key32.size()) == v.okm_hex.substr(0, 64));
  }
}

void test_xchacha20_poly1305_vector() {
  // Known-answer vector from draft-irtf-cfrg-xchacha Section A.3.1.
  const std::string key_bytes =
      from_hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
  const std::string nonce_bytes =
      from_hex("404142434445464748494a4b4c4d4e4f5051525354555657");
  const std::string aad = from_hex("50515253c0c1c2c3c4c5c6c7");
  constexpr std::string_view plaintext =
      "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
      "sunscreen would be it.";
  constexpr std::string_view expected_ciphertext_hex =
      "bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb"
      "731c7f1b0b4aa6440bf3a82f4eda7e39ae64c6708c54c216cb96b72e1213b452"
      "2f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff9"
      "21f9664c97637da9768812f615c68b13b52e";
  constexpr std::string_view expected_tag_hex = "c0875924c1c7987947deafd8780acf49";

  DataKeyBuffer key;
  CHECK(!key.assign_exact(key_bytes));
  std::array<unsigned char, 24> nonce{};
  std::memcpy(nonce.data(), nonce_bytes.data(), nonce.size());

  auto enc_res = custody_ops::aead_xchacha20poly1305_encrypt(key, nonce, aad, plaintext);
  CHECK(std::holds_alternative<std::string>(enc_res));
  const auto& ciphertext_and_tag = std::get<std::string>(enc_res);
  CHECK(ciphertext_and_tag.size() == plaintext.size() + 16);
  CHECK(to_hex(ciphertext_and_tag.substr(0, plaintext.size())) == expected_ciphertext_hex);
  CHECK(to_hex(ciphertext_and_tag.substr(plaintext.size())) == expected_tag_hex);

  auto dec_res = custody_ops::aead_xchacha20poly1305_decrypt(key, nonce, aad, ciphertext_and_tag);
  CHECK(std::holds_alternative<CleansedBuffer>(dec_res));
  const auto& recovered = std::get<CleansedBuffer>(dec_res);
  CHECK(recovered.view() == plaintext);
}

thread_local const std::array<unsigned char, 32>* g_current_test_store_id = nullptr;

class FixedSecretSource final : public SecretSource {
 public:
  explicit FixedSecretSource(std::string active_id) : active_id_(std::move(active_id)) {}

  void set_secret(std::string key_id, std::string secret_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    secrets_[std::move(key_id)] = StoredEntry{std::move(secret_bytes), 0, false};
  }

  void set_malformed_secret(std::string key_id, std::string raw_bytes, std::size_t reported_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    secrets_[std::move(key_id)] = StoredEntry{std::move(raw_bytes), reported_size, true};
  }

  CustodyResult<std::string> active_key_id() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_id_;
  }

  std::optional<CustodyFault> load_secret(std::string_view key_id,
                                          SecretBuffer& out,
                                          std::size_t* written_size) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++load_counts_[std::string(key_id)];
    if (g_current_test_store_id != nullptr) {
      ++pair_load_counts_[{*g_current_test_store_id, std::string(key_id)}];
    }
    auto it = secrets_.find(std::string(key_id));
    if (it == secrets_.end()) {
      out.wipe();
      if (written_size != nullptr) {
        *written_size = 0;
      }
      return CustodyFault::KeyNotFound;
    }
    if (it->second.use_custom_secret_write(out, written_size)) {
      return std::nullopt;
    }
    return out.assign_secret(it->second.bytes, written_size);
  }

  std::size_t load_count(std::string_view key_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = load_counts_.find(std::string(key_id));
    return it == load_counts_.end() ? 0 : it->second;
  }

  std::size_t pair_load_count(const std::array<unsigned char, 32>& store_id,
                              std::string_view key_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pair_load_counts_.find({store_id, std::string(key_id)});
    return it == pair_load_counts_.end() ? 0 : it->second;
  }

 private:
  struct StoredEntry {
    std::string bytes;
    std::size_t reported_written_size = 0;
    bool use_custom_written_size = false;

    bool use_custom_secret_write(SecretBuffer& out, std::size_t* written_size) const {
      if (!use_custom_written_size) {
        return false;
      }
      out.wipe();
      if (!bytes.empty() && out.valid()) {
        std::memcpy(out.writable_data(), bytes.data(), std::min(bytes.size(), out.size()));
      }
      if (written_size != nullptr) {
        *written_size = reported_written_size;
      }
      return true;
    }
  };
  mutable std::mutex mutex_;
  std::string active_id_;
  std::map<std::string, StoredEntry> secrets_;
  std::map<std::string, std::size_t> load_counts_;
  std::map<std::pair<std::array<unsigned char, 32>, std::string>, std::size_t> pair_load_counts_;
};

void test_exact_length_rules() {
  // Secret length rules: 32..64 bytes accepted; 31, 65, or written_size > 64 rejected with InvalidKeyMaterial and wiped.
  {
    SecretBuffer secret;
    CHECK(secret.valid());
    std::memset(secret.writable_data(), 0x5a, secret.size());
    std::size_t written = 99;
    CHECK(secret.assign_secret(std::string(31, 'a'), &written) == CustodyFault::InvalidKeyMaterial);
    CHECK(written == 0);
    CHECK(all_bytes_zero(secret.data(), secret.size()));

    std::memset(secret.writable_data(), 0x5a, secret.size());
    CHECK(secret.assign_secret(std::string(65, 'b'), &written) == CustodyFault::InvalidKeyMaterial);
    CHECK(written == 0);
    CHECK(all_bytes_zero(secret.data(), secret.size()));

    std::memset(secret.writable_data(), 0x5a, secret.size());
    CHECK(validate_secret_material(secret, 65) == CustodyFault::InvalidKeyMaterial);
    CHECK(all_bytes_zero(secret.data(), secret.size()));

    std::memset(secret.writable_data(), 0x5a, secret.size());
    CHECK(validate_secret_material(secret, 31) == CustodyFault::InvalidKeyMaterial);
    CHECK(all_bytes_zero(secret.data(), secret.size()));
  }

  // DEK length rules: exactly 32 bytes; 31 and 33 bytes rejected with InvalidKeyMaterial and wiped.
  {
    DataKeyBuffer dek;
    CHECK(dek.valid());
    std::memset(dek.writable_data(), 0x77, dek.size());
    CHECK(dek.assign_exact(std::string(31, 'd')) == CustodyFault::InvalidKeyMaterial);
    CHECK(all_bytes_zero(dek.data(), dek.size()));

    std::memset(dek.writable_data(), 0x77, dek.size());
    CHECK(dek.assign_exact(std::string(33, 'd')) == CustodyFault::InvalidKeyMaterial);
    CHECK(all_bytes_zero(dek.data(), dek.size()));

    std::memset(dek.writable_data(), 0x77, dek.size());
    CHECK(validate_dek_material(dek, 31) == CustodyFault::InvalidKeyMaterial);
    CHECK(all_bytes_zero(dek.data(), dek.size()));

    std::memset(dek.writable_data(), 0x77, dek.size());
    CHECK(validate_dek_material(dek, 33) == CustodyFault::InvalidKeyMaterial);
    CHECK(all_bytes_zero(dek.data(), dek.size()));
  }

  // Adapter also rejects a SecretSource reporting written_size > 64 or < 32.
  {
    auto secrets = std::make_shared<FixedSecretSource>("k-bad");
    secrets->set_malformed_secret("k-31", std::string(31, 'x'), 31);
    secrets->set_malformed_secret("k-65", std::string(64, 'y'), 65);
    auto key_source = make_hkdf_key_source(secrets);

    std::array<unsigned char, 32> store_id{};
    store_id.fill(0x11);
    DataKeyBuffer dek;
    CHECK(!dek.assign_exact(std::string(32, 'z')));

    for (std::string_view kid : {"k-31", "k-65"}) {
      const std::string aad = build_epoch_aad(store_id, 1, kid);
      auto res = key_source->wrap_dek(kid, aad, dek);
      CHECK(std::holds_alternative<CustodyFault>(res));
      CHECK(std::get<CustodyFault>(res) == CustodyFault::InvalidKeyMaterial);
    }
  }

  // Failed sodium_malloc allocation maps to InvalidKeyMaterial.
  {
    custody_ops::fail_key_alloc_on_call(1);
    DataKeyBuffer failed_dek;
    custody_ops::reset_random_seam();
    CHECK(!failed_dek.valid());
    CHECK(failed_dek.assign_exact(std::string(32, 'k')) == CustodyFault::InvalidKeyMaterial);
  }
}

void test_tampered_wrapped_dek() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x42'));
  auto key_source = make_hkdf_key_source(secrets);

  std::array<unsigned char, 32> store_id{};
  for (std::size_t i = 0; i < store_id.size(); ++i) {
    store_id[i] = static_cast<unsigned char>(i + 1);
  }
  const std::string aad = build_epoch_aad(store_id, 1, "k1");

  DataKeyBuffer dek;
  CHECK(!dek.assign_exact(std::string(32, '\x99')));
  auto wrapped_res = key_source->wrap_dek("k1", aad, dek);
  CHECK(std::holds_alternative<std::string>(wrapped_res));
  const std::string wrapped = std::get<std::string>(wrapped_res);
  CHECK(wrapped.size() == 72);

  // 71 and 73 bytes must fail with Tampered and wipe dek_out.
  {
    DataKeyBuffer out;
    std::memset(out.writable_data(), 0xaa, out.size());
    CHECK(key_source->unwrap_dek("k1", aad, wrapped.substr(0, 71), out) == CustodyFault::Tampered);
    CHECK(all_bytes_zero(out.data(), out.size()));

    std::memset(out.writable_data(), 0xaa, out.size());
    CHECK(key_source->unwrap_dek("k1", aad, wrapped + std::string(1, '\0'), out) == CustodyFault::Tampered);
    CHECK(all_bytes_zero(out.data(), out.size()));
  }

  // One flipped bit in nonce, ciphertext, or tag -> Tampered and dek_out wiped.
  for (std::size_t pos : {std::size_t{0}, std::size_t{24}, std::size_t{71}}) {
    std::string mutated = wrapped;
    mutated[pos] ^= 0x01;
    DataKeyBuffer out;
    std::memset(out.writable_data(), 0xbb, out.size());
    CHECK(key_source->unwrap_dek("k1", aad, mutated, out) == CustodyFault::Tampered);
    CHECK(all_bytes_zero(out.data(), out.size()));
  }

  // Wrong epoch_aad -> Tampered and dek_out wiped.
  {
    const std::string wrong_aad = build_epoch_aad(store_id, 2, "k1");
    DataKeyBuffer out;
    std::memset(out.writable_data(), 0xcc, out.size());
    CHECK(key_source->unwrap_dek("k1", wrong_aad, wrapped, out) == CustodyFault::Tampered);
    CHECK(all_bytes_zero(out.data(), out.size()));
  }
}

void test_random_source_failure() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x33'));
  auto key_source = make_hkdf_key_source(secrets);

  std::array<unsigned char, 32> store_id{};
  store_id.fill(0x22);
  const std::string aad = build_epoch_aad(store_id, 1, "k1");

  DataKeyBuffer dek;
  CHECK(!dek.assign_exact(std::string(32, '\x44')));

  custody_ops::fail_random_on_call(1);
  auto res = key_source->wrap_dek("k1", aad, dek);
  custody_ops::reset_random_seam();

  CHECK(std::holds_alternative<CustodyFault>(res));
  CHECK(std::get<CustodyFault>(res) == CustodyFault::InvalidKeyMaterial);
}

class ReentrantSecretSource final : public SecretSource {
 public:
  ReentrantSecretSource(std::shared_ptr<SecretSource> inner,
                        const std::array<unsigned char, 32>& store_id)
      : inner_(std::move(inner)), store_id_(store_id) {}

  void set_key_source(std::weak_ptr<KeySource> ks) { key_source_ = std::move(ks); }

  CustodyResult<std::string> active_key_id() override { return inner_->active_key_id(); }

  std::optional<CustodyFault> load_secret(std::string_view key_id,
                                          SecretBuffer& out,
                                          std::size_t* written_size) override {
    if (key_id == "k-reentrant") {
      if (auto ks = key_source_.lock()) {
        CustodyOperationScope nested_scope(*ks);
        DataKeyBuffer nested_dek;
        if (auto f = nested_dek.assign_exact(std::string(32, '\x66'))) {
          return f;
        }
        const std::string nested_aad = build_epoch_aad(store_id_, 99, "k1");
        auto wrapped = ks->wrap_dek("k1", nested_aad, nested_dek);
        if (std::holds_alternative<CustodyFault>(wrapped)) {
          return std::get<CustodyFault>(wrapped);
        }
        DataKeyBuffer unwrapped;
        if (auto f = ks->unwrap_dek("k1", nested_aad, std::get<std::string>(wrapped), unwrapped)) {
          return f;
        }
      }
    }
    return inner_->load_secret(key_id, out, written_size);
  }

 private:
  std::shared_ptr<SecretSource> inner_;
  std::array<unsigned char, 32> store_id_;
  std::weak_ptr<KeySource> key_source_;
};

void test_operation_scope_call_counting() {
  std::array<unsigned char, 32> store_id{};
  store_id.fill(0x55);

  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x10'));
  secrets->set_secret("k2", std::string(64, '\x20'));
  secrets->set_secret("k-reentrant", std::string(32, '\x30'));
  auto reentrant = std::make_shared<ReentrantSecretSource>(secrets, store_id);
  auto key_source = make_hkdf_key_source(reentrant);
  reentrant->set_key_source(key_source);

  const std::string aad1 = build_epoch_aad(store_id, 1, "k1");
  const std::string aad2 = build_epoch_aad(store_id, 2, "k2");
  const std::string aad_re = build_epoch_aad(store_id, 3, "k-reentrant");

  DataKeyBuffer dek;
  CHECK(!dek.assign_exact(std::string(32, '\x7f')));

  {
    CustodyOperationScope scope(*key_source);
    for (int i = 0; i < 4; ++i) {
      auto w1 = key_source->wrap_dek("k1", aad1, dek);
      CHECK(std::holds_alternative<std::string>(w1));
      DataKeyBuffer u1;
      CHECK(!key_source->unwrap_dek("k1", aad1, std::get<std::string>(w1), u1));
      CHECK(std::memcmp(u1.data(), dek.data(), dek.size()) == 0);

      auto w2 = key_source->wrap_dek("k2", aad2, dek);
      CHECK(std::holds_alternative<std::string>(w2));
      DataKeyBuffer u2;
      CHECK(!key_source->unwrap_dek("k2", aad2, std::get<std::string>(w2), u2));
      CHECK(std::memcmp(u2.data(), dek.data(), dek.size()) == 0);
    }
    CHECK(secrets->load_count("k1") == 1);
    CHECK(secrets->load_count("k2") == 1);

    auto w_re = key_source->wrap_dek("k-reentrant", aad_re, dek);
    CHECK(std::holds_alternative<std::string>(w_re));
    CHECK(secrets->load_count("k-reentrant") == 1);
    CHECK(secrets->load_count("k1") == 1);
  }

  // After the operation scope ends, cached KEKs were wiped and removed, so a new scope loads once more.
  {
    CustodyOperationScope scope(*key_source);
    auto w1 = key_source->wrap_dek("k1", aad1, dek);
    CHECK(std::holds_alternative<std::string>(w1));
    CHECK(secrets->load_count("k1") == 2);
  }
}

// One scope, two stores, one key id, in a fixed order: every DEK must open through a fresh source with no cache.
// A cache that ignored the store would hand store B the KEK derived for store A; the wrap and an unwrap inside the
// same scope would both read that KEK and agree, so only a fresh derivation shows the loss.
void test_one_scope_two_stores_same_key_id() {
  auto secrets = std::make_shared<FixedSecretSource>("k-shared");
  secrets->set_secret("k-shared", std::string(32, '\x5a'));
  auto key_source = make_hkdf_key_source(secrets);
  std::array<unsigned char, 32> store_a{};
  std::array<unsigned char, 32> store_b{};
  store_a.fill(0x01);
  store_b.fill(0x02);
  struct WrappedDek {
    std::string epoch_aad;
    std::string wrapped;
    std::string dek;
  };
  std::vector<WrappedDek> all_wrapped;
  {
    CustodyOperationScope scope(*key_source);
    for (const auto* store : {&store_a, &store_b, &store_a, &store_b}) {
      const std::string aad = build_epoch_aad(*store, all_wrapped.size() + 1, "k-shared");
      std::string dek_bytes(32, static_cast<char>(0x30 + all_wrapped.size()));
      DataKeyBuffer dek;
      CHECK(!dek.assign_exact(dek_bytes));
      auto wrapped = key_source->wrap_dek("k-shared", aad, dek);
      CHECK(std::holds_alternative<std::string>(wrapped));
      all_wrapped.push_back({aad, std::get<std::string>(wrapped), dek_bytes});
    }
  }
  auto fresh_secrets = std::make_shared<FixedSecretSource>("k-shared");
  fresh_secrets->set_secret("k-shared", std::string(32, '\x5a'));
  auto fresh_source = make_hkdf_key_source(fresh_secrets);
  for (const auto& w : all_wrapped) {
    DataKeyBuffer opened;
    CHECK(!fresh_source->unwrap_dek("k-shared", w.epoch_aad, w.wrapped, opened));
    CHECK(std::memcmp(opened.data(), w.dek.data(), opened.size()) == 0);
  }
}

void test_concurrent_operation_scopes() {
  auto secrets = std::make_shared<FixedSecretSource>("k-shared-1");
  secrets->set_secret("k-shared-1", std::string(32, '\x11'));
  secrets->set_secret("k-shared-2", std::string(64, '\x22'));
  secrets->set_secret("k-only-a", std::string(32, '\x33'));
  secrets->set_secret("k-only-b", std::string(48, '\x44'));
  auto key_source = make_hkdf_key_source(secrets);

  std::array<unsigned char, 32> store_a{};
  std::array<unsigned char, 32> store_b{};
  for (std::size_t i = 0; i < 32; ++i) {
    store_a[i] = static_cast<unsigned char>(0x10 + i);
    store_b[i] = static_cast<unsigned char>(0x90 + i);
  }

  auto run_period = [&]() {
    std::latch start_latch(2);
    std::latch finish_latch(2);
    std::string err_a;
    std::string err_b;
    struct WrappedDek {
      std::string epoch_aad;
      std::string key_id;
      std::string wrapped;
      std::string dek;
    };
    std::mutex wrapped_mutex;
    std::vector<WrappedDek> all_wrapped;

    auto worker = [&](const std::array<unsigned char, 32>& own_store,
                      std::string_view own_key,
                      unsigned char dek_seed,
                      std::string* err_out) {
      g_current_test_store_id = &own_store;
      try {
        CustodyOperationScope scope(*key_source);
        start_latch.arrive_and_wait();

        const std::array<std::string_view, 3> keys = {"k-shared-1", "k-shared-2", own_key};
        for (std::size_t round = 0; round < 8; ++round) {
          for (std::size_t k_idx = 0; k_idx < keys.size(); ++k_idx) {
            const std::string_view kid = keys[k_idx];
            const std::uint64_t epoch_id = round * keys.size() + k_idx + 1;
            const std::string own_aad = build_epoch_aad(own_store, epoch_id, kid);

            std::string dek_bytes(32, '\0');
            for (std::size_t b = 0; b < 32; ++b) {
              dek_bytes[b] = static_cast<char>(dek_seed + round * 17 + k_idx * 5 + b);
            }
            DataKeyBuffer dek;
            CHECK(!dek.assign_exact(dek_bytes));

            auto wrapped = key_source->wrap_dek(kid, own_aad, dek);
            CHECK(std::holds_alternative<std::string>(wrapped));
            {
              std::lock_guard<std::mutex> lock(wrapped_mutex);
              all_wrapped.push_back({own_aad, std::string(kid), std::get<std::string>(wrapped), dek_bytes});
            }

            DataKeyBuffer unwrapped;
            CHECK(!key_source->unwrap_dek(kid, own_aad, std::get<std::string>(wrapped), unwrapped));
            CHECK(std::memcmp(unwrapped.data(), dek.data(), dek.size()) == 0);
          }
        }
        finish_latch.arrive_and_wait();
      } catch (const std::exception& e) {
        *err_out = e.what();
        finish_latch.count_down();
      } catch (...) {
        *err_out = "unknown worker exception";
        finish_latch.count_down();
      }
      g_current_test_store_id = nullptr;
    };

    std::thread thread_a(worker, std::cref(store_a), "k-only-a", 0x20, &err_a);
    std::thread thread_b(worker, std::cref(store_b), "k-only-b", 0xa0, &err_b);
    thread_a.join();
    thread_b.join();
    if (!err_a.empty()) {
      throw std::runtime_error("thread_a failed: " + err_a);
    }
    if (!err_b.empty()) {
      throw std::runtime_error("thread_b failed: " + err_b);
    }
    // A DEK wrapped under another store's KEK still round-trips inside the period, because wrap and unwrap read the
    // same cached KEK; it fails only when the KEK is derived again. So every DEK wrapped in the period must open
    // through a fresh source with no cache (its own secret source, so the load counts above stay untouched).
    CHECK(all_wrapped.size() == 2 * 8 * 3);
    auto fresh_secrets = std::make_shared<FixedSecretSource>("k-shared-1");
    fresh_secrets->set_secret("k-shared-1", std::string(32, '\x11'));
    fresh_secrets->set_secret("k-shared-2", std::string(64, '\x22'));
    fresh_secrets->set_secret("k-only-a", std::string(32, '\x33'));
    fresh_secrets->set_secret("k-only-b", std::string(48, '\x44'));
    auto fresh_source = make_hkdf_key_source(fresh_secrets);
    for (const auto& w : all_wrapped) {
      DataKeyBuffer opened;
      CHECK(!fresh_source->unwrap_dek(w.key_id, w.epoch_aad, w.wrapped, opened));
      CHECK(std::memcmp(opened.data(), w.dek.data(), opened.size()) == 0);
    }
  };

  // Period 1: both scopes open concurrently; each (store_id, key_id) pair loads its secret once.
  run_period();
  CHECK(secrets->pair_load_count(store_a, "k-shared-1") == 1);
  CHECK(secrets->pair_load_count(store_b, "k-shared-1") == 1);
  CHECK(secrets->pair_load_count(store_a, "k-shared-2") == 1);
  CHECK(secrets->pair_load_count(store_b, "k-shared-2") == 1);
  CHECK(secrets->pair_load_count(store_a, "k-only-a") == 1);
  CHECK(secrets->pair_load_count(store_b, "k-only-b") == 1);
  CHECK(secrets->pair_load_count(store_a, "k-only-b") == 0);
  CHECK(secrets->pair_load_count(store_b, "k-only-a") == 0);
  CHECK(secrets->load_count("k-shared-1") == 2);
  CHECK(secrets->load_count("k-shared-2") == 2);
  CHECK(secrets->load_count("k-only-a") == 1);
  CHECK(secrets->load_count("k-only-b") == 1);

  // Period 2: after Period 1's scopes closed, cached KEKs were wiped and removed, so each pair loads once more.
  run_period();
  CHECK(secrets->pair_load_count(store_a, "k-shared-1") == 2);
  CHECK(secrets->pair_load_count(store_b, "k-shared-1") == 2);
  CHECK(secrets->pair_load_count(store_a, "k-shared-2") == 2);
  CHECK(secrets->pair_load_count(store_b, "k-shared-2") == 2);
  CHECK(secrets->pair_load_count(store_a, "k-only-a") == 2);
  CHECK(secrets->pair_load_count(store_b, "k-only-b") == 2);
  CHECK(secrets->load_count("k-shared-1") == 4);
  CHECK(secrets->load_count("k-shared-2") == 4);
  CHECK(secrets->load_count("k-only-a") == 2);
  CHECK(secrets->load_count("k-only-b") == 2);
}

struct ObserverStats {
  std::size_t releases = 0;
  bool all_zero_every_time = true;
  std::vector<std::size_t> capacities;
};

void record_release(std::size_t capacity, bool all_zero, void* context) noexcept {
  auto* stats = static_cast<ObserverStats*>(context);
  ++stats->releases;
  if (!all_zero) {
    stats->all_zero_every_time = false;
  }
  stats->capacities.push_back(capacity);
}

void test_wiping_buffer() {
  std::array<unsigned char, 32> needle{};
  CHECK(crypto::random_bytes(needle.data(), needle.size()));

  ObserverStats stats;
  custody_ops::set_release_observer(&record_release, &stats);
  try {
    {
      CleansedBuffer buf;
      // Initial allocation (no prior block to release yet).
      buf.reserve(32);
      buf.append(needle.data(), needle.size());
      CHECK(stats.releases == 0);

      // Grow 4 times explicitly, filling new space with the per-run random needle.
      constexpr std::size_t kCapacities[] = {64, 160, 384, 1024};
      std::size_t expected_releases = 0;
      for (std::size_t cap : kCapacities) {
        buf.reserve(cap);
        ++expected_releases;
        CHECK(stats.releases == expected_releases);
        while (buf.size() + needle.size() <= cap) {
          buf.append(needle.data(), needle.size());
        }
      }
      CHECK(buf.capacity() == 1024);
    }
    // Final release on scope exit: 4 growth releases + 1 final release = 5.
    CHECK(stats.releases == 5);
    CHECK(stats.all_zero_every_time);
    CHECK(stats.capacities == (std::vector<std::size_t>{32, 64, 160, 384, 1024}));
  } catch (...) {
    custody_ops::set_release_observer(nullptr, nullptr);
    throw;
  }
  custody_ops::set_release_observer(nullptr, nullptr);
}

void test_error_mapping() {
  struct Row {
    CustodyFault fault;
    ErrorKind kind;
    RetryClass retry_class;
    RetrySafety retry_safety;
    std::string_view reason;
    bool carries_key_id;
  };
  constexpr Row kRows[] = {
      {CustodyFault::Tampered, ErrorKind::Permission, RetryClass::Never, RetrySafety::NotSent,
       "archive.custody.tampered", false},
      {CustodyFault::Unauthorized, ErrorKind::Permission, RetryClass::Never, RetrySafety::NotSent,
       "archive.custody.unauthorized", true},
      {CustodyFault::KeyNotFound, ErrorKind::NotFound, RetryClass::Never, RetrySafety::NotSent,
       "archive.custody.key_not_found", true},
      {CustodyFault::InvalidKeyMaterial, ErrorKind::Permission, RetryClass::Never, RetrySafety::NotSent,
       "archive.custody.key_invalid", true},
      {CustodyFault::KeyInUse, ErrorKind::Permission, RetryClass::Never, RetrySafety::NotSent,
       "archive.custody.key_in_use", true},
      {CustodyFault::CorruptHeader, ErrorKind::Permission, RetryClass::Never, RetrySafety::NotSent,
       "archive.custody.corrupt_header", false},
      {CustodyFault::CorruptRecord, ErrorKind::Permission, RetryClass::Never, RetrySafety::NotSent,
       "archive.custody.corrupt_record", false},
      {CustodyFault::SourceUnavailable, ErrorKind::Transport, RetryClass::Transient, RetrySafety::NotSent,
       "archive.custody.source_unavailable", true},
      {CustodyFault::SourceRateLimited, ErrorKind::RateLimited, RetryClass::Transient, RetrySafety::NotSent,
       "archive.custody.source_rate_limited", true},
  };

  for (const auto& row : kRows) {
    const CustodyError mapped = custody_error(row.fault, "store-hex", "key-1");
    CHECK(mapped.error.kind == row.kind);
    CHECK(mapped.error.retry_class == row.retry_class);
    CHECK(mapped.error.retry_safety == row.retry_safety);
    CHECK(mapped.error.reason == row.reason);
    CHECK(!mapped.error.safe_message.empty());
    CHECK(mapped.store_id == "store-hex");
    if (row.carries_key_id) {
      CHECK(mapped.key_id == "key-1");
    } else {
      CHECK(mapped.key_id.empty());
    }
  }

  const CustodyError unavail = custody_unavailable_error();
  CHECK(unavail.error.kind == ErrorKind::Unsupported);
  CHECK(unavail.error.retry_class == RetryClass::Never);
  CHECK(unavail.error.retry_safety == RetrySafety::NotSent);
  CHECK(unavail.error.reason == "archive.custody.unavailable");
}

void test_capability_and_format_constants() {
  CHECK(custody_ops::available());
  const auto contract = core_interface_contract();
  CHECK(contract.revision == EXPECTED_INTERFACE_REVISION);
  CHECK(EXPECTED_INTERFACE_REVISION == 7);
  CHECK((contract.capabilities & capability::HostArchiveCustody) != 0);
  CHECK((capability::RequiredProvider & capability::HostArchiveCustody) == 0);

  CHECK(custody_format::header_format_version == 1);
  CHECK(custody_format::record_format_version == 1);
  CHECK(custody_format::header_body_domain == "sp.native.archive.custody.header.v1");
  CHECK(custody_format::header_tag_domain == "sp.native.archive.custody.header-tag.v1");
  CHECK(custody_format::epoch_aad_domain == "sp.native.archive.custody.epoch-aad.v1");
  CHECK(custody_format::kek_domain == "sp.native.archive.custody.kek.v1");
  CHECK(custody_format::record_aad_domain == "sp.native.archive.custody.record-aad.v1");
  CHECK(custody_format::ref_domain == "sp.native.archive.custody.record-ref.v1");
  CHECK(custody_format::ref_prefix == "spna4:");
}

void test_derive_kek_and_fixed_nonce_wrap_vectors() {
  std::array<unsigned char, 32> store_id{};
  for (std::size_t i = 0; i < store_id.size(); ++i) {
    store_id[i] = static_cast<unsigned char>(i);
  }
  std::string secret_32_bytes(32, '\0');
  for (std::size_t i = 0; i < 32; ++i) {
    secret_32_bytes[i] = static_cast<char>(0x20 + i);
  }
  std::string secret_64_bytes(64, '\0');
  for (std::size_t i = 0; i < 64; ++i) {
    secret_64_bytes[i] = static_cast<char>(0x40 + i);
  }

  SecretBuffer secret_32;
  std::size_t written_32 = 0;
  CHECK(!secret_32.assign_secret(secret_32_bytes, &written_32));
  CHECK(written_32 == 32);

  SecretBuffer secret_64;
  std::size_t written_64 = 0;
  CHECK(!secret_64.assign_secret(secret_64_bytes, &written_64));
  CHECK(written_64 == 64);

  auto kek_32_res = custody_ops::derive_kek(secret_32, written_32, store_id);
  CHECK(std::holds_alternative<DataKeyBuffer>(kek_32_res));
  const auto& kek_32 = std::get<DataKeyBuffer>(kek_32_res);

  auto kek_64_res = custody_ops::derive_kek(secret_64, written_64, store_id);
  CHECK(std::holds_alternative<DataKeyBuffer>(kek_64_res));
  const auto& kek_64 = std::get<DataKeyBuffer>(kek_64_res);

  // Cross-check against independent in-tree HMAC-SHA256 implementation (sp::crypto::hmac_sha256).
  const std::string salt_view(reinterpret_cast<const char*>(store_id.data()), store_id.size());
  const std::string expand_input = encode_text(custody_format::kek_domain) + std::string(1, '\x01');

  const auto ref_prk_32 = crypto::hmac_sha256(salt_view, secret_32_bytes);
  const std::string ref_prk_32_view(reinterpret_cast<const char*>(ref_prk_32.data()), ref_prk_32.size());
  const auto ref_kek_32 = crypto::hmac_sha256(ref_prk_32_view, expand_input);
  CHECK(std::memcmp(kek_32.data(), ref_kek_32.data(), 32) == 0);

  const auto ref_prk_64 = crypto::hmac_sha256(salt_view, secret_64_bytes);
  const std::string ref_prk_64_view(reinterpret_cast<const char*>(ref_prk_64.data()), ref_prk_64.size());
  const auto ref_kek_64 = crypto::hmac_sha256(ref_prk_64_view, expand_input);
  CHECK(std::memcmp(kek_64.data(), ref_kek_64.data(), 32) == 0);

  // End-to-end wrap_dek/unwrap_dek vector with a fixed 24-byte nonce injected through the seam.
  auto secrets = std::make_shared<FixedSecretSource>("key-fixed");
  secrets->set_secret("key-fixed", secret_32_bytes);
  auto key_source = make_hkdf_key_source(secrets);

  const std::string epoch_aad = build_epoch_aad(store_id, 1, "key-fixed");
  std::string dek_bytes(32, '\0');
  for (std::size_t i = 0; i < 32; ++i) {
    dek_bytes[i] = static_cast<char>(0x80 + i);
  }
  DataKeyBuffer dek;
  CHECK(!dek.assign_exact(dek_bytes));

  std::string fixed_nonce_bytes(24, '\0');
  for (std::size_t i = 0; i < 24; ++i) {
    fixed_nonce_bytes[i] = static_cast<char>(0xa0 + i);
  }
  std::array<unsigned char, 24> fixed_nonce{};
  std::memcpy(fixed_nonce.data(), fixed_nonce_bytes.data(), fixed_nonce.size());

  custody_ops::override_next_random_bytes(fixed_nonce_bytes);
  auto wrapped_res = key_source->wrap_dek("key-fixed", epoch_aad, dek);
  custody_ops::reset_random_seam();
  CHECK(std::holds_alternative<std::string>(wrapped_res));
  const std::string& wrapped = std::get<std::string>(wrapped_res);
  CHECK(wrapped.size() == 72);
  CHECK(wrapped.substr(0, 24) == fixed_nonce_bytes);

  auto direct_enc = custody_ops::aead_xchacha20poly1305_encrypt(kek_32, fixed_nonce, epoch_aad, dek_bytes);
  CHECK(std::holds_alternative<std::string>(direct_enc));
  CHECK(wrapped.substr(24) == std::get<std::string>(direct_enc));

  DataKeyBuffer unwrapped_dek;
  CHECK(!key_source->unwrap_dek("key-fixed", epoch_aad, wrapped, unwrapped_dek));
  CHECK(std::memcmp(unwrapped_dek.data(), dek.data(), dek.size()) == 0);

  // Expected values: the output of `python3 tests/custody_vectors.py` (Python standard library hmac and
  // hashlib only), checked in as the fixture.
  constexpr std::string_view kExpectedKek32Hex = "f6d446b5190800ddd17cfd21def332f53d7be8ed3ed7d6091ac8c35965d0749e";
  constexpr std::string_view kExpectedKek64Hex = "767b5ffd9f40723e27417ba668c19c587b285253e78aa65154debacfc45101ee";
  const std::string actual_kek_32_hex = to_hex(kek_32.data(), kek_32.size());
  const std::string actual_kek_64_hex = to_hex(kek_64.data(), kek_64.size());
  CHECK(actual_kek_32_hex == kExpectedKek32Hex);
  CHECK(actual_kek_64_hex == kExpectedKek64Hex);
}

}  // namespace

int main() {
  try {
    test_capability_and_format_constants();
    test_rfc5869_vectors();
    test_xchacha20_poly1305_vector();
    test_exact_length_rules();
    test_tampered_wrapped_dek();
    test_random_source_failure();
    test_operation_scope_call_counting();
    test_one_scope_two_stores_same_key_id();
    test_concurrent_operation_scopes();
    test_wiping_buffer();
    test_error_mapping();
    test_derive_kek_and_fixed_nonce_wrap_vectors();
    std::cout << "native archive custody tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
