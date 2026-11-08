// Host-native archive custody primitives and store header tests: HKDF-SHA256, XChaCha20-Poly1305,
// derive_kek, make_hkdf_key_source, LockedKeyBuffer, CleansedBuffer, StoreHeader, and CustodyFault mapping.
#include "codecs/messages.h"
#include "codecs/messages_request.h"
#include "core/interface_contract.h"
#include "core/native.h"
#include "core/native_archive.h"
#include "core/native_archive_custody.h"
#include "core/native_archive_custody_format.h"
#include "core/native_archive_custody_internal.h"
#include "crypto/crypto.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

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
  std::array<unsigned char, 32> descriptor_id{};
  descriptor_id.fill(0xdd);
  char ver_be[8];
  write_be64(ver_be, custody_format::header_format_version);
  char epoch_be[8];
  write_be64(epoch_be, epoch_id);
  std::string out;
  out.append(encode_text(custody_format::epoch_aad_domain));
  out.append(ver_be, 8);
  out.append(reinterpret_cast<const char*>(store_id.data()), store_id.size());
  out.append(encode_text("test-owner"));
  out.append(reinterpret_cast<const char*>(descriptor_id.data()), descriptor_id.size());
  out.append(epoch_be, 8);
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

  void set_active_key_id(std::string active_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_id_ = std::move(active_id);
  }

  void set_secret(std::string key_id, std::string secret_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    secrets_[std::move(key_id)] = StoredEntry{std::move(secret_bytes), 0, false};
  }

  void remove_secret(std::string_view key_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    secrets_.erase(std::string(key_id));
  }

  void reset_counts() {
    std::lock_guard<std::mutex> lock(mutex_);
    load_counts_.clear();
    pair_load_counts_.clear();
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
      {CustodyFault::EpochLimit, ErrorKind::ResourceLimit, RetryClass::Never, RetrySafety::NotSent,
       "archive.custody.epoch_limit", false},
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

descriptor::ValidatedDescriptor default_test_descriptor() {
  auto loaded = descriptor::load(
      R"({"descriptor_version":1,"revision":1,"id":"archive-fixture","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}

descriptor::ValidatedDescriptor alternate_test_descriptor() {
  auto loaded = descriptor::load(
      R"({"descriptor_version":1,"revision":1,"id":"archive-fixture-other","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}

std::string build_raw_header(std::string_view domain,
                             std::uint64_t version,
                             const std::array<unsigned char, 32>& store_id,
                             std::string_view owner_scope,
                             const std::array<unsigned char, 32>& descriptor_identity,
                             std::uint64_t current_epoch,
                             const std::vector<EpochKeyEntry>& epochs,
                             std::string_view tag_bytes) {
  archive_internal::Writer writer(archive_internal::resolve_default_archive_limits({}));
  writer.text(domain);
  writer.number(version);
  writer.digest(store_id);
  writer.text(owner_scope);
  writer.digest(descriptor_identity);
  writer.number(current_epoch);
  writer.number(epochs.size());
  for (const auto& entry : epochs) {
    writer.number(entry.epoch);
    writer.text(entry.wrapping_key_id);
    writer.text(entry.wrapped_dek);
  }
  writer.raw(tag_bytes);
  return std::move(writer.data);
}

#ifdef _WIN32
struct PrivateSecurity {
  std::vector<unsigned char> token;
  std::vector<unsigned char> acl;
  SECURITY_DESCRIPTOR security_descriptor{};
  explicit PrivateSecurity(DWORD access) {
    HANDLE handle = nullptr;
    CHECK(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &handle));
    DWORD size = 0;
    ::GetTokenInformation(handle, TokenUser, nullptr, 0, &size);
    token.resize(size);
    const BOOL ok = ::GetTokenInformation(handle, TokenUser, token.data(), size, &size);
    ::CloseHandle(handle);
    CHECK(ok);
    size = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) +
                              ::GetLengthSid(sid()));
    acl.resize(size);
    CHECK(::InitializeAcl(dacl(), size, ACL_REVISION));
    CHECK(::AddAccessAllowedAceEx(dacl(), ACL_REVISION, 0, access, sid()));
    CHECK(::InitializeSecurityDescriptor(&security_descriptor, SECURITY_DESCRIPTOR_REVISION));
    CHECK(::SetSecurityDescriptorOwner(&security_descriptor, sid(), FALSE));
    CHECK(::SetSecurityDescriptorDacl(&security_descriptor, TRUE, dacl(), FALSE));
    CHECK(::SetSecurityDescriptorControl(
        &security_descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED));
  }
  PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(token.data())->User.Sid; }
  PACL dacl() { return reinterpret_cast<PACL>(acl.data()); }
};
#endif

void create_private_directory(const std::filesystem::path& path) {
#ifdef _WIN32
  PrivateSecurity security(FILE_ALL_ACCESS);
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &security.security_descriptor, FALSE};
  CHECK(::CreateDirectoryW(path.c_str(), &attributes));
#else
  CHECK(::mkdir(path.c_str(), 0700) == 0);
#endif
}

std::string path_utf8(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

void test_existing_archive_portability_unchanged() {
  CHECK(config_defaults::archive_custody_max_epochs == 4096);
  const auto valid_desc = default_test_descriptor();
  CHECK(valid_desc.policy() != nullptr);
  CHECK(valid_desc.policy()->identity().size() == 32);

  const HostCustodyLimits resolved = archive_internal::resolve_custody_limits({}, valid_desc);
  CHECK(resolved.max_epochs == 4096);
  CHECK(resolved.archive.max_bytes == valid_desc.policy()->resources().native_bytes);
  CHECK(resolved.archive.max_messages == valid_desc.policy()->resources().request_messages);
  CHECK(resolved.archive.max_parts == valid_desc.policy()->resources().request_parts);
  CHECK(resolved.archive.max_json_depth == valid_desc.policy()->resources().native_depth);

  namespace fs = std::filesystem;
  const fs::path parent = fs::canonical(fs::temp_directory_path());
  std::random_device rd;
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string dir_name = "sp-archive-custody-portability-";
  for (int i = 0; i < 32; ++i) {
    dir_name += kHexDigits[rd() & 15];
  }
  const fs::path root = parent / dir_name;
  create_private_directory(root);
  struct ScopedCleanup {
    fs::path path;
    ~ScopedCleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};
  create_private_directory(root / "keys");

  messages::Request req;
  req.model = "fixture-model";
  req.account_scope = "fixture-account";
  req.messages.push_back(Message{"", Role::User, {Text{"hello"}}});
  auto initial_enc = messages::encode(valid_desc, req, false);
  CHECK(std::holds_alternative<messages::EncodedRequest>(initial_enc));
  Accumulator acc;
  messages::Codec codec(
      valid_desc, messages::Mode::Buffered, acc, std::get<messages::EncodedRequest>(initial_enc).context);
  codec.buffered(
      R"({"id":"msg_portable","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"thinking","thinking":"reason","signature":"synthetic-fixture-signature"},{"type":"redacted_thinking","data":"synthetic-fixture-state"},{"type":"text","text":"answer"}],"stop_reason":"end_turn","stop_sequence":null,"usage":{"input_tokens":2,"output_tokens":3}})",
      {});
  codec.finish();
  CHECK(acc.outcome() && std::holds_alternative<Completion>(*acc.outcome()));
  const Completion& comp = std::get<Completion>(*acc.outcome());
  CHECK(comp.messages.size() == 1 && comp.messages[0].native && comp.messages[0].native->complete());
  req.messages.push_back(comp.messages[0]);
  req.messages.push_back(Message{"", Role::User, {Text{"continue"}}});
  auto expected_enc = messages::encode(valid_desc, req, false);
  CHECK(std::holds_alternative<messages::EncodedRequest>(expected_enc));
  const std::string expected_body = std::get<messages::EncodedRequest>(expected_enc).body;

  const std::string records_dir = path_utf8(root / "records");
  const std::string key_file = path_utf8(root / "keys" / "activation");
  auto provisioned = NativeArchive::provision(records_dir, key_file, "synthetic-owner", valid_desc);
  CHECK(std::holds_alternative<std::shared_ptr<NativeArchive>>(provisioned));
  auto archive = std::get<std::shared_ptr<NativeArchive>>(std::move(provisioned));
  auto saved_res = archive->save(req.messages, "native-call");
  CHECK(std::holds_alternative<std::string>(saved_res));
  const std::string reference = std::get<std::string>(std::move(saved_res));
  CHECK(reference.size() == 70 && reference.substr(0, 6) == "spna3:");

  auto reopened_res = NativeArchive::open(records_dir, key_file, "synthetic-owner", valid_desc);
  CHECK(std::holds_alternative<std::shared_ptr<NativeArchive>>(reopened_res));
  auto reopened = std::get<std::shared_ptr<NativeArchive>>(std::move(reopened_res));
  auto loaded_res = reopened->load(reference, "native-call");
  CHECK(std::holds_alternative<std::vector<Message>>(loaded_res));
  messages::Request continued = req;
  continued.messages = std::get<std::vector<Message>>(std::move(loaded_res));
  auto reencoded = messages::encode(valid_desc, continued, false);
  CHECK(std::holds_alternative<messages::EncodedRequest>(reencoded));
  CHECK(std::get<messages::EncodedRequest>(reencoded).body == expected_body);
}

void test_golden_header_bytes() {
  std::array<unsigned char, 32> store_id{};
  for (std::size_t i = 0; i < store_id.size(); ++i) {
    store_id[i] = static_cast<unsigned char>(i);
  }
  const std::string store_id_bytes(reinterpret_cast<const char*>(store_id.data()), store_id.size());

  std::string secret_32_bytes(32, '\0');
  for (std::size_t i = 0; i < 32; ++i) {
    secret_32_bytes[i] = static_cast<char>(0x20 + i);
  }
  std::string dek_1_bytes(32, '\0');
  for (std::size_t i = 0; i < 32; ++i) {
    dek_1_bytes[i] = static_cast<char>(0x80 + i);
  }
  std::string nonce_1_bytes(24, '\0');
  for (std::size_t i = 0; i < 24; ++i) {
    nonce_1_bytes[i] = static_cast<char>(0xa0 + i);
  }
  std::array<unsigned char, 24> fixed_nonce{};
  std::memcpy(fixed_nonce.data(), nonce_1_bytes.data(), fixed_nonce.size());

  auto secrets = std::make_shared<FixedSecretSource>("key-fixed");
  secrets->set_secret("key-fixed", secret_32_bytes);
  auto key_source = make_hkdf_key_source(secrets);

  const auto valid_desc = default_test_descriptor();

  custody_ops::reset_random_seam();
  custody_ops::override_next_random_bytes(store_id_bytes);
  custody_ops::override_next_random_bytes(dek_1_bytes);
  custody_ops::override_next_random_bytes(nonce_1_bytes);
  auto created_res = create_header(*key_source, "owner-golden", valid_desc);
  custody_ops::reset_random_seam();

  CHECK(std::holds_alternative<std::string>(created_res));
  const std::string& serialized = std::get<std::string>(created_res);
  CHECK(serialized.size() == 288);

  const std::string_view body(serialized.data(), 256);
  const std::string_view tag(serialized.data() + 256, 32);

  // Checked-in structural byte layout from `tests/custody_vectors.py`.
  constexpr std::string_view kExpectedHeaderBodyPrefixHex =
      "000000000000002373702e6e61746976652e617263686976652e637573746f64792e6865616465722e7631"
      "0000000000000001"
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
      "000000000000000c6f776e65722d676f6c64656e";
  constexpr std::string_view kExpectedHeaderBodyMidAndNonceHex =
      "0000000000000001"
      "0000000000000001"
      "0000000000000001"
      "00000000000000096b65792d6669786564"
      "0000000000000048"
      "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7";
  constexpr std::string_view kExpectedEpochAadPrefixHex =
      "000000000000002673702e6e61746976652e617263686976652e637573746f64792e65706f63682d6161642e7631"
      "0000000000000001"
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
      "000000000000000c6f776e65722d676f6c64656e";
  constexpr std::string_view kExpectedEpochAadSuffixHex =
      "0000000000000001"
      "00000000000000096b65792d6669786564";
  constexpr std::string_view kExpectedHeaderTagExpandInputHex =
      "000000000000002773702e6e61746976652e617263686976652e637573746f64792e6865616465722d7461672e763101";

  const HostCustodyLimits resolved_limits = archive_internal::resolve_custody_limits({}, valid_desc);
  const auto expected_desc_digest =
      archive_internal::descriptor_binding_digest(valid_desc, resolved_limits.archive);
  const std::string expected_desc_bytes(reinterpret_cast<const char*>(expected_desc_digest.data()),
                                        expected_desc_digest.size());

  CHECK(body.substr(0, 103) == from_hex(kExpectedHeaderBodyPrefixHex));
  CHECK(body.substr(103, 32) == expected_desc_bytes);
  CHECK(body.substr(135, 73) == from_hex(kExpectedHeaderBodyMidAndNonceHex));

  const std::string expected_epoch_aad_1 =
      from_hex(kExpectedEpochAadPrefixHex) + expected_desc_bytes + from_hex(kExpectedEpochAadSuffixHex);
  const std::string actual_epoch_aad_1 = archive_internal::build_epoch_aad(
      store_id, "owner-golden", expected_desc_digest, 1, "key-fixed", resolved_limits.archive);
  CHECK(actual_epoch_aad_1 == expected_epoch_aad_1);

  SecretBuffer secret_buf;
  std::size_t written = 0;
  CHECK(!secret_buf.assign_secret(secret_32_bytes, &written));
  auto kek_res = custody_ops::derive_kek(secret_buf, written, store_id);
  CHECK(std::holds_alternative<DataKeyBuffer>(kek_res));
  const auto& kek_32 = std::get<DataKeyBuffer>(kek_res);

  auto expected_aead =
      custody_ops::aead_xchacha20poly1305_encrypt(kek_32, fixed_nonce, expected_epoch_aad_1, dek_1_bytes);
  CHECK(std::holds_alternative<std::string>(expected_aead));
  CHECK(body.substr(208, 48) == std::get<std::string>(expected_aead));

  // Tag recomputation from the body using independent in-tree HMAC-SHA256 (`sp::crypto::hmac_sha256`).
  const std::string header_tag_expand_input = from_hex(kExpectedHeaderTagExpandInputHex);
  const auto expected_mac_key = crypto::hmac_sha256(dek_1_bytes, header_tag_expand_input);
  const std::string expected_mac_key_view(reinterpret_cast<const char*>(expected_mac_key.data()),
                                          expected_mac_key.size());
  const auto expected_tag = crypto::hmac_sha256(expected_mac_key_view, body);
  const std::string expected_tag_view(reinterpret_cast<const char*>(expected_tag.data()), expected_tag.size());
  CHECK(tag == expected_tag_view);

  // Parse and re-serialize round-trip.
  auto parsed_res = parse_header(serialized);
  CHECK(std::holds_alternative<StoreHeader>(parsed_res));
  const StoreHeader& parsed = std::get<StoreHeader>(parsed_res);
  CHECK(parsed.format_version == 1);
  CHECK(parsed.store_id == store_id);
  CHECK(parsed.owner_scope == "owner-golden");
  CHECK(parsed.descriptor_identity == expected_desc_digest);
  CHECK(parsed.current_epoch == 1);
  CHECK(parsed.epochs.size() == 1);
  CHECK(parsed.epochs[0].epoch == 1);
  CHECK(parsed.epochs[0].wrapping_key_id == "key-fixed");
  CHECK(parsed.header_tag == expected_tag);

  auto reserialized_res = serialize_header(parsed);
  CHECK(std::holds_alternative<std::string>(reserialized_res));
  CHECK(std::get<std::string>(reserialized_res) == serialized);

  // Fresh-source unwrap check on the wrapped DEK.
  auto reqs_res = unwrap_requests(serialized, "owner-golden", valid_desc);
  CHECK(std::holds_alternative<std::vector<UnwrapRequest>>(reqs_res));
  const auto& reqs = std::get<std::vector<UnwrapRequest>>(reqs_res);
  CHECK(reqs.size() == 1);
  CHECK(reqs[0].key_id == "key-fixed");
  CHECK(reqs[0].epoch_aad == expected_epoch_aad_1);

  auto fresh_secrets = std::make_shared<FixedSecretSource>("key-fixed");
  fresh_secrets->set_secret("key-fixed", secret_32_bytes);
  auto fresh_source = make_hkdf_key_source(fresh_secrets);
  DataKeyBuffer unwrapped_dek;
  CHECK(!fresh_source->unwrap_dek(reqs[0].key_id, reqs[0].epoch_aad, reqs[0].wrapped_dek, unwrapped_dek));
  CHECK(std::memcmp(unwrapped_dek.data(), dek_1_bytes.data(), 32) == 0);
}

void test_header_splices() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  secrets->set_secret("k2", std::string(32, '\x22'));
  secrets->set_secret("k3", std::string(32, '\x33'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto valid_desc = default_test_descriptor();

  auto h1_res = create_header(*key_source, "owner-splice", valid_desc);
  CHECK(std::holds_alternative<std::string>(h1_res));
  auto h2_res = rotate_header(std::get<std::string>(h1_res), *key_source, "k2", "owner-splice", valid_desc);
  CHECK(std::holds_alternative<std::string>(h2_res));
  auto h3_res = rotate_header(std::get<std::string>(h2_res), *key_source, "k3", "owner-splice", valid_desc);
  CHECK(std::holds_alternative<std::string>(h3_res));
  const std::string& h3_bytes = std::get<std::string>(h3_res);

  auto parsed_res = parse_header(h3_bytes);
  CHECK(std::holds_alternative<StoreHeader>(parsed_res));
  const StoreHeader h3 = std::get<StoreHeader>(parsed_res);
  const std::string store_id_hex = to_hex(h3.store_id.data(), h3.store_id.size());
  const std::string tag_bytes = h3_bytes.substr(h3_bytes.size() - 32);

  auto check_structural_splice = [&](const std::string& spliced, const StoreHeader* bad_struct = nullptr) {
    auto p = parse_header(spliced);
    CHECK(std::holds_alternative<CustodyError>(p));
    CHECK(std::get<CustodyError>(p).error.reason == "archive.custody.corrupt_header");

    auto rot = rotate_header(spliced, *key_source, {}, "owner-splice", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(rot));
    CHECK(std::get<CustodyError>(rot).error.reason == "archive.custody.corrupt_header");

    auto ret = retire_key_in_header(spliced, "k1", *key_source, "owner-splice", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(ret));
    CHECK(std::get<CustodyError>(ret).error.reason == "archive.custody.corrupt_header");

    if (bad_struct != nullptr) {
      auto ser = serialize_header(*bad_struct);
      CHECK(std::holds_alternative<CustodyError>(ser));
      CHECK(std::get<CustodyError>(ser).error.reason == "archive.custody.corrupt_header");
    }
  };

  // 1a. Rewound current_epoch (structural mismatch: current_epoch = 2 while 3 epochs are present).
  {
    StoreHeader bad = h3;
    bad.current_epoch = 2;
    const std::string spliced =
        build_raw_header(custody_format::header_body_domain, 1, bad.store_id, bad.owner_scope,
                         bad.descriptor_identity, bad.current_epoch, bad.epochs, tag_bytes);
    check_structural_splice(spliced, &bad);
  }

  // 1b. Rewound current_epoch with truncated epochs (epochs 1..2 with epoch 3's tag -> parses structurally,
  // but rotate_header and retire_key_in_header refuse with Tampered when verifying the tag under dek_2).
  {
    std::vector<EpochKeyEntry> two_epochs = {h3.epochs[0], h3.epochs[1]};
    const std::string rewound =
        build_raw_header(custody_format::header_body_domain, 1, h3.store_id, h3.owner_scope,
                         h3.descriptor_identity, 2, two_epochs, tag_bytes);
    CHECK(std::holds_alternative<StoreHeader>(parse_header(rewound)));

    auto rot = rotate_header(rewound, *key_source, {}, "owner-splice", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(rot));
    CHECK(std::get<CustodyError>(rot).error.reason == "archive.custody.tampered");
    CHECK(std::get<CustodyError>(rot).store_id == store_id_hex);

    auto ret = retire_key_in_header(rewound, "k1", *key_source, "owner-splice", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(ret));
    CHECK(std::get<CustodyError>(ret).error.reason == "archive.custody.tampered");
    CHECK(std::get<CustodyError>(ret).store_id == store_id_hex);
  }

  // 2. Dropped middle epoch ([1, 3], current_epoch = 3).
  {
    StoreHeader bad = h3;
    bad.epochs = {h3.epochs[0], h3.epochs[2]};
    const std::string spliced =
        build_raw_header(custody_format::header_body_domain, 1, bad.store_id, bad.owner_scope,
                         bad.descriptor_identity, bad.current_epoch, bad.epochs, tag_bytes);
    check_structural_splice(spliced, &bad);
  }

  // 3. Duplicated epoch ([1, 2, 2], current_epoch = 3; [1, 1, 3], current_epoch = 3; and [1, 1], current_epoch = 2).
  {
    StoreHeader bad_122 = h3;
    bad_122.epochs[2].epoch = 2;
    check_structural_splice(
        build_raw_header(custody_format::header_body_domain, 1, bad_122.store_id, bad_122.owner_scope,
                         bad_122.descriptor_identity, bad_122.current_epoch, bad_122.epochs, tag_bytes),
        &bad_122);

    StoreHeader bad_113 = h3;
    bad_113.epochs[1].epoch = 1;
    check_structural_splice(
        build_raw_header(custody_format::header_body_domain, 1, bad_113.store_id, bad_113.owner_scope,
                         bad_113.descriptor_identity, bad_113.current_epoch, bad_113.epochs, tag_bytes),
        &bad_113);

    StoreHeader bad_11 = h3;
    bad_11.current_epoch = 2;
    bad_11.epochs = {h3.epochs[0], h3.epochs[0]};
    check_structural_splice(
        build_raw_header(custody_format::header_body_domain, 1, bad_11.store_id, bad_11.owner_scope,
                         bad_11.descriptor_identity, bad_11.current_epoch, bad_11.epochs, tag_bytes),
        &bad_11);
  }

  // 4. Reordered epochs ([1, 3, 2], current_epoch = 3 and [2, 1, 3], current_epoch = 3).
  {
    StoreHeader bad_132 = h3;
    std::swap(bad_132.epochs[1], bad_132.epochs[2]);
    check_structural_splice(
        build_raw_header(custody_format::header_body_domain, 1, bad_132.store_id, bad_132.owner_scope,
                         bad_132.descriptor_identity, bad_132.current_epoch, bad_132.epochs, tag_bytes),
        &bad_132);

    StoreHeader bad_213 = h3;
    std::swap(bad_213.epochs[0], bad_213.epochs[1]);
    check_structural_splice(
        build_raw_header(custody_format::header_body_domain, 1, bad_213.store_id, bad_213.owner_scope,
                         bad_213.descriptor_identity, bad_213.current_epoch, bad_213.epochs, tag_bytes),
        &bad_213);
  }

  // 5. Flipped body byte in an epoch entry that no unwrap on the tested path reads (epoch 2's wrapped_dek and
  // epoch 2's wrapping_key_id when rotating or retiring "k1", where only epoch 3 is unwrapped before verify_header_tag).
  {
    StoreHeader dek_mut = h3;
    dek_mut.epochs[1].wrapped_dek[0] ^= 0x01;
    const std::string flipped_dek_body =
        build_raw_header(custody_format::header_body_domain, 1, dek_mut.store_id, dek_mut.owner_scope,
                         dek_mut.descriptor_identity, dek_mut.current_epoch, dek_mut.epochs, tag_bytes);
    CHECK(std::holds_alternative<StoreHeader>(parse_header(flipped_dek_body)));

    auto rot_dek = rotate_header(flipped_dek_body, *key_source, {}, "owner-splice", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(rot_dek));
    CHECK(std::get<CustodyError>(rot_dek).error.reason == "archive.custody.tampered");
    CHECK(std::get<CustodyError>(rot_dek).store_id == store_id_hex);

    auto ret_dek = retire_key_in_header(flipped_dek_body, "k1", *key_source, "owner-splice", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(ret_dek));
    CHECK(std::get<CustodyError>(ret_dek).error.reason == "archive.custody.tampered");
    CHECK(std::get<CustodyError>(ret_dek).store_id == store_id_hex);

    for (std::string_view mutated_kid : {std::string_view{"k9"}, std::string_view{"k\x7f", 2}}) {
      StoreHeader kid_mut = h3;
      kid_mut.epochs[1].wrapping_key_id = std::string(mutated_kid);
      const std::string flipped_kid_body =
          build_raw_header(custody_format::header_body_domain, 1, kid_mut.store_id, kid_mut.owner_scope,
                           kid_mut.descriptor_identity, kid_mut.current_epoch, kid_mut.epochs, tag_bytes);
      CHECK(std::holds_alternative<StoreHeader>(parse_header(flipped_kid_body)));

      auto rot_kid = rotate_header(flipped_kid_body, *key_source, {}, "owner-splice", valid_desc);
      CHECK(std::holds_alternative<CustodyError>(rot_kid));
      CHECK(std::get<CustodyError>(rot_kid).error.reason == "archive.custody.tampered");
      CHECK(std::get<CustodyError>(rot_kid).store_id == store_id_hex);

      auto ret_kid = retire_key_in_header(flipped_kid_body, "k1", *key_source, "owner-splice", valid_desc);
      CHECK(std::holds_alternative<CustodyError>(ret_kid));
      CHECK(std::get<CustodyError>(ret_kid).error.reason == "archive.custody.tampered");
      CHECK(std::get<CustodyError>(ret_kid).store_id == store_id_hex);
    }
  }

  // 6. One flipped tag byte.
  {
    std::string flipped_tag = h3_bytes;
    flipped_tag.back() ^= 0x01;
    CHECK(std::holds_alternative<StoreHeader>(parse_header(flipped_tag)));

    auto rot = rotate_header(flipped_tag, *key_source, {}, "owner-splice", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(rot));
    CHECK(std::get<CustodyError>(rot).error.reason == "archive.custody.tampered");
    CHECK(std::get<CustodyError>(rot).store_id == store_id_hex);

    auto ret = retire_key_in_header(flipped_tag, "k1", *key_source, "owner-splice", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(ret));
    CHECK(std::get<CustodyError>(ret).error.reason == "archive.custody.tampered");
    CHECK(std::get<CustodyError>(ret).store_id == store_id_hex);
  }

  // 7. Additional structural boundary checks on parse_header / serialize_header.
  {
    // current_epoch == 0
    StoreHeader zero_epoch = h3;
    zero_epoch.current_epoch = 0;
    zero_epoch.epochs.clear();
    check_structural_splice(
        build_raw_header(custody_format::header_body_domain, 1, zero_epoch.store_id, zero_epoch.owner_scope,
                         zero_epoch.descriptor_identity, 0, zero_epoch.epochs, tag_bytes),
        &zero_epoch);

    // Empty key id
    StoreHeader bad_kid = h3;
    bad_kid.epochs[0].wrapping_key_id.clear();
    check_structural_splice(
        build_raw_header(custody_format::header_body_domain, 1, bad_kid.store_id, bad_kid.owner_scope,
                         bad_kid.descriptor_identity, bad_kid.current_epoch, bad_kid.epochs, tag_bytes),
        &bad_kid);

    // Empty wrapped_dek
    StoreHeader empty_dek = h3;
    empty_dek.epochs[0].wrapped_dek.clear();
    check_structural_splice(
        build_raw_header(custody_format::header_body_domain, 1, empty_dek.store_id, empty_dek.owner_scope,
                         empty_dek.descriptor_identity, empty_dek.current_epoch, empty_dek.epochs, tag_bytes),
        &empty_dek);

    // Trailing byte
    check_structural_splice(h3_bytes + std::string(1, '\0'));
  }
}

class UniformSecretSource final : public SecretSource {
 public:
  UniformSecretSource(std::string active_id, std::string secret_bytes)
      : active_id_(std::move(active_id)), secret_bytes_(std::move(secret_bytes)) {}

  CustodyResult<std::string> active_key_id() override { return active_id_; }

  std::optional<CustodyFault> load_secret(std::string_view key_id,
                                          SecretBuffer& out,
                                          std::size_t* written_size) override {
    if (key_id.empty()) {
      return CustodyFault::InvalidKeyMaterial;
    }
    return out.assign_secret(secret_bytes_, written_size);
  }

 private:
  std::string active_id_;
  std::string secret_bytes_;
};

void test_epoch_aad_single_field_binding() {
  // Every key_id resolves to the exact same secret, so KEK is identical across all key_ids for a given store_id;
  // only the AAD differs when epoch, wrapping_key_id, owner_scope, or descriptor_identity is altered.
  auto uniform_secrets = std::make_shared<UniformSecretSource>("k1", std::string(32, '\x5c'));
  auto key_source = make_hkdf_key_source(uniform_secrets);
  const auto valid_desc = default_test_descriptor();
  const auto alt_desc = alternate_test_descriptor();

  auto h1_res = create_header(*key_source, "owner-aad-bind", valid_desc);
  CHECK(std::holds_alternative<std::string>(h1_res));
  auto h2_res = rotate_header(std::get<std::string>(h1_res), *key_source, "k2", "owner-aad-bind", valid_desc);
  CHECK(std::holds_alternative<std::string>(h2_res));
  const std::string& h2_bytes = std::get<std::string>(h2_res);

  auto parsed_res = parse_header(h2_bytes);
  CHECK(std::holds_alternative<StoreHeader>(parsed_res));
  const StoreHeader h2 = std::get<StoreHeader>(parsed_res);
  const std::string store_id_hex = to_hex(h2.store_id.data(), h2.store_id.size());
  const std::string tag_bytes = h2_bytes.substr(h2_bytes.size() - 32);

  // Baseline: both epochs unwrap cleanly.
  {
    auto reqs_res = unwrap_requests(h2_bytes, "owner-aad-bind", valid_desc);
    CHECK(std::holds_alternative<std::vector<UnwrapRequest>>(reqs_res));
    const auto& reqs = std::get<std::vector<UnwrapRequest>>(reqs_res);
    CHECK(reqs.size() == 2);
    for (const auto& req : reqs) {
      DataKeyBuffer out;
      CHECK(!key_source->unwrap_dek(req.key_id, req.epoch_aad, req.wrapped_dek, out));
    }
  }

  // 1a. Swap wrapped_dek between epochs 1 and 2 when both epochs use the same wrapping_key_id ("k1"),
  // so only the epoch number (1 vs 2) in epoch_aad differs -> unwrap refuses with Tampered.
  {
    auto same_key_h2_res =
        rotate_header(std::get<std::string>(h1_res), *key_source, "k1", "owner-aad-bind", valid_desc);
    CHECK(std::holds_alternative<std::string>(same_key_h2_res));
    const std::string& same_key_h2_bytes = std::get<std::string>(same_key_h2_res);
    auto same_key_parsed = parse_header(same_key_h2_bytes);
    CHECK(std::holds_alternative<StoreHeader>(same_key_parsed));
    StoreHeader same_key_swapped = std::get<StoreHeader>(same_key_parsed);
    CHECK(same_key_swapped.epochs[0].wrapping_key_id == "k1");
    CHECK(same_key_swapped.epochs[1].wrapping_key_id == "k1");
    std::swap(same_key_swapped.epochs[0].wrapped_dek, same_key_swapped.epochs[1].wrapped_dek);
    const std::string same_key_tag = same_key_h2_bytes.substr(same_key_h2_bytes.size() - 32);
    const std::string same_key_swapped_bytes = build_raw_header(
        custody_format::header_body_domain, 1, same_key_swapped.store_id, same_key_swapped.owner_scope,
        same_key_swapped.descriptor_identity, same_key_swapped.current_epoch, same_key_swapped.epochs, same_key_tag);
    auto reqs_res = unwrap_requests(same_key_swapped_bytes, "owner-aad-bind", valid_desc);
    CHECK(std::holds_alternative<std::vector<UnwrapRequest>>(reqs_res));
    for (const auto& req : std::get<std::vector<UnwrapRequest>>(reqs_res)) {
      DataKeyBuffer out;
      CHECK(key_source->unwrap_dek(req.key_id, req.epoch_aad, req.wrapped_dek, out) ==
            CustodyFault::Tampered);
    }
    auto rot = rotate_header(same_key_swapped_bytes, *key_source, {}, "owner-aad-bind", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(rot));
    CHECK(std::get<CustodyError>(rot).error.reason == "archive.custody.tampered");
  }

  // 1b. Swap wrapped_dek between epochs 1 and 2 across distinct key ids -> unwrap refuses with Tampered.
  {
    StoreHeader swapped = h2;
    std::swap(swapped.epochs[0].wrapped_dek, swapped.epochs[1].wrapped_dek);
    const std::string swapped_bytes =
        build_raw_header(custody_format::header_body_domain, 1, swapped.store_id, swapped.owner_scope,
                         swapped.descriptor_identity, swapped.current_epoch, swapped.epochs, tag_bytes);
    auto reqs_res = unwrap_requests(swapped_bytes, "owner-aad-bind", valid_desc);
    CHECK(std::holds_alternative<std::vector<UnwrapRequest>>(reqs_res));
    const auto& reqs = std::get<std::vector<UnwrapRequest>>(reqs_res);
    for (const auto& req : reqs) {
      DataKeyBuffer out;
      CHECK(key_source->unwrap_dek(req.key_id, req.epoch_aad, req.wrapped_dek, out) ==
            CustodyFault::Tampered);
    }
    auto rot = rotate_header(swapped_bytes, *key_source, {}, "owner-aad-bind", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(rot));
    CHECK(std::get<CustodyError>(rot).error.reason == "archive.custody.tampered");
  }

  // 2. Change wrapping_key_id (while source returns the same secret) -> unwrap refuses with Tampered.
  {
    StoreHeader changed_kid = h2;
    changed_kid.epochs[0].wrapping_key_id = "k1-other";
    changed_kid.epochs[1].wrapping_key_id = "k2-other";
    const std::string changed_bytes =
        build_raw_header(custody_format::header_body_domain, 1, changed_kid.store_id, changed_kid.owner_scope,
                         changed_kid.descriptor_identity, changed_kid.current_epoch, changed_kid.epochs, tag_bytes);
    auto reqs_res = unwrap_requests(changed_bytes, "owner-aad-bind", valid_desc);
    CHECK(std::holds_alternative<std::vector<UnwrapRequest>>(reqs_res));
    const auto& reqs = std::get<std::vector<UnwrapRequest>>(reqs_res);
    for (const auto& req : reqs) {
      DataKeyBuffer out;
      CHECK(key_source->unwrap_dek(req.key_id, req.epoch_aad, req.wrapped_dek, out) ==
            CustodyFault::Tampered);
    }
    auto rot = rotate_header(changed_bytes, *key_source, {}, "owner-aad-bind", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(rot));
    CHECK(std::get<CustodyError>(rot).error.reason == "archive.custody.tampered");
  }

  // 3. Change owner_scope (including empty owner_scope) -> unwrap_requests, rotate_header, and retire_key_in_header
  // refuse with Tampered and populate store_id; if header owner_scope is also rewritten, unwrap_dek refuses
  // because epoch_aad's owner_scope differs.
  {
    for (std::string_view bad_owner : {std::string_view{"other-owner"}, std::string_view{""}}) {
      auto mismatch_res = unwrap_requests(h2_bytes, bad_owner, valid_desc);
      CHECK(std::holds_alternative<CustodyError>(mismatch_res));
      CHECK(std::get<CustodyError>(mismatch_res).error.reason == "archive.custody.tampered");
      CHECK(std::get<CustodyError>(mismatch_res).store_id == store_id_hex);

      auto rot_res = rotate_header(h2_bytes, *key_source, "k2", bad_owner, valid_desc);
      CHECK(std::holds_alternative<CustodyError>(rot_res));
      CHECK(std::get<CustodyError>(rot_res).error.reason == "archive.custody.tampered");
      CHECK(std::get<CustodyError>(rot_res).store_id == store_id_hex);

      auto ret_res = retire_key_in_header(h2_bytes, "k1", *key_source, bad_owner, valid_desc);
      CHECK(std::holds_alternative<CustodyError>(ret_res));
      CHECK(std::get<CustodyError>(ret_res).error.reason == "archive.custody.tampered");
      CHECK(std::get<CustodyError>(ret_res).store_id == store_id_hex);
    }

    StoreHeader changed_owner = h2;
    changed_owner.owner_scope = "other-owner";
    const std::string changed_bytes = build_raw_header(
        custody_format::header_body_domain, 1, changed_owner.store_id, changed_owner.owner_scope,
        changed_owner.descriptor_identity, changed_owner.current_epoch, changed_owner.epochs, tag_bytes);
    auto reqs_res = unwrap_requests(changed_bytes, "other-owner", valid_desc);
    CHECK(std::holds_alternative<std::vector<UnwrapRequest>>(reqs_res));
    for (const auto& req : std::get<std::vector<UnwrapRequest>>(reqs_res)) {
      DataKeyBuffer out;
      CHECK(key_source->unwrap_dek(req.key_id, req.epoch_aad, req.wrapped_dek, out) ==
            CustodyFault::Tampered);
    }
  }

  // 4. Change descriptor -> unwrap_requests refuses mismatch, and if header descriptor_identity is also rewritten,
  // unwrap_dek refuses because epoch_aad's descriptor_identity differs.
  {
    auto mismatch_res = unwrap_requests(h2_bytes, "owner-aad-bind", alt_desc);
    CHECK(std::holds_alternative<CustodyError>(mismatch_res));
    CHECK(std::get<CustodyError>(mismatch_res).error.reason == "archive.custody.tampered");
    CHECK(std::get<CustodyError>(mismatch_res).store_id == store_id_hex);

    StoreHeader changed_desc = h2;
    changed_desc.descriptor_identity = archive_internal::descriptor_binding_digest(
        alt_desc, archive_internal::resolve_default_archive_limits({}));
    const std::string changed_bytes = build_raw_header(
        custody_format::header_body_domain, 1, changed_desc.store_id, changed_desc.owner_scope,
        changed_desc.descriptor_identity, changed_desc.current_epoch, changed_desc.epochs, tag_bytes);
    auto reqs_res = unwrap_requests(changed_bytes, "owner-aad-bind", alt_desc);
    CHECK(std::holds_alternative<std::vector<UnwrapRequest>>(reqs_res));
    for (const auto& req : std::get<std::vector<UnwrapRequest>>(reqs_res)) {
      DataKeyBuffer out;
      CHECK(key_source->unwrap_dek(req.key_id, req.epoch_aad, req.wrapped_dek, out) ==
            CustodyFault::Tampered);
    }
  }
}

void test_retire_key_in_header() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  secrets->set_secret("k2", std::string(32, '\x22'));
  secrets->set_secret("k3", std::string(32, '\x33'));
  secrets->set_secret("k4", std::string(32, '\x44'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto valid_desc = default_test_descriptor();

  // Create under k1, rotate with explicit k2, rotate with explicit k3, then change active_key_id() to k4.
  auto h1_res = create_header(*key_source, "owner-retire", valid_desc);
  CHECK(std::holds_alternative<std::string>(h1_res));
  auto h2_res = rotate_header(std::get<std::string>(h1_res), *key_source, "k2", "owner-retire", valid_desc);
  CHECK(std::holds_alternative<std::string>(h2_res));
  auto h3_res = rotate_header(std::get<std::string>(h2_res), *key_source, "k3", "owner-retire", valid_desc);
  CHECK(std::holds_alternative<std::string>(h3_res));
  const std::string h3_bytes = std::get<std::string>(h3_res);

  // Record original DEKs for epochs 1, 2, 3 before retirement.
  std::vector<std::string> original_deks;
  {
    auto reqs_res = unwrap_requests(h3_bytes, "owner-retire", valid_desc);
    CHECK(std::holds_alternative<std::vector<UnwrapRequest>>(reqs_res));
    const auto& reqs = std::get<std::vector<UnwrapRequest>>(reqs_res);
    CHECK(reqs.size() == 3);
    for (const auto& req : reqs) {
      DataKeyBuffer dek;
      CHECK(!key_source->unwrap_dek(req.key_id, req.epoch_aad, req.wrapped_dek, dek));
      original_deks.emplace_back(reinterpret_cast<const char*>(dek.data()), dek.size());
    }
  }

  secrets->set_active_key_id("k4");

  // Retiring k3 (the current epoch's wrapping_key_id) is refused with KeyInUse even though active_key_id() is k4.
  {
    auto retire_k3 = retire_key_in_header(h3_bytes, "k3", *key_source, "owner-retire", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(retire_k3));
    const auto& err = std::get<CustodyError>(retire_k3);
    CHECK(err.error.kind == ErrorKind::Permission);
    CHECK(err.error.reason == "archive.custody.key_in_use");
    CHECK(err.key_id == "k3");
    CHECK(err.store_id.size() == 64);
  }

  // Retiring k1 re-wraps epoch 1 under epochs[current].wrapping_key_id ("k3"), leaves epoch 2 wrapped under "k2"
  // with its wrapped_dek bytes unchanged, and produces a valid header tag under dek_3.
  auto h3_parsed_res = parse_header(h3_bytes);
  CHECK(std::holds_alternative<StoreHeader>(h3_parsed_res));
  const StoreHeader h3_parsed = std::get<StoreHeader>(h3_parsed_res);

  auto retired_k1_res = retire_key_in_header(h3_bytes, "k1", *key_source, "owner-retire", valid_desc);
  CHECK(std::holds_alternative<std::string>(retired_k1_res));
  const std::string& retired_k1_bytes = std::get<std::string>(retired_k1_res);
  {
    auto parsed_k1_res = parse_header(retired_k1_bytes);
    CHECK(std::holds_alternative<StoreHeader>(parsed_k1_res));
    const StoreHeader& parsed_k1 = std::get<StoreHeader>(parsed_k1_res);
    CHECK(parsed_k1.epochs.size() == 3);
    CHECK(parsed_k1.epochs[0].wrapping_key_id == "k3");
    CHECK(parsed_k1.epochs[1].wrapping_key_id == "k2");
    CHECK(parsed_k1.epochs[1].wrapped_dek == h3_parsed.epochs[1].wrapped_dek);
    CHECK(parsed_k1.epochs[2].wrapping_key_id == "k3");
    CHECK(parsed_k1.epochs[2].wrapped_dek == h3_parsed.epochs[2].wrapped_dek);

    const std::string_view k1_body(retired_k1_bytes.data(), retired_k1_bytes.size() - 32);
    const std::string header_tag_expand_input =
        encode_text(custody_format::header_tag_domain) + std::string(1, '\x01');
    const auto mac_key = crypto::hmac_sha256(original_deks[2], header_tag_expand_input);
    const std::string mac_key_view(reinterpret_cast<const char*>(mac_key.data()), mac_key.size());
    const auto expected_tag = crypto::hmac_sha256(mac_key_view, k1_body);
    CHECK(parsed_k1.header_tag == expected_tag);

    auto rotated_from_retired =
        rotate_header(retired_k1_bytes, *key_source, "k4", "owner-retire", valid_desc);
    CHECK(std::holds_alternative<std::string>(rotated_from_retired));
  }

  auto retired_k1_k2_res =
      retire_key_in_header(retired_k1_bytes, "k2", *key_source, "owner-retire", valid_desc);
  CHECK(std::holds_alternative<std::string>(retired_k1_k2_res));
  const std::string& retired_bytes = std::get<std::string>(retired_k1_k2_res);

  // Delete k1 and k2 from the source; unwrap_requests + a fresh source unwraps every epoch with k3 only.
  secrets->remove_secret("k1");
  secrets->remove_secret("k2");

  auto fresh_secrets = std::make_shared<FixedSecretSource>("k3");
  fresh_secrets->set_secret("k3", std::string(32, '\x33'));
  auto fresh_source = make_hkdf_key_source(fresh_secrets);

  auto reqs_res = unwrap_requests(retired_bytes, "owner-retire", valid_desc);
  CHECK(std::holds_alternative<std::vector<UnwrapRequest>>(reqs_res));
  const auto& reqs = std::get<std::vector<UnwrapRequest>>(reqs_res);
  CHECK(reqs.size() == 3);
  for (std::size_t i = 0; i < reqs.size(); ++i) {
    CHECK(reqs[i].key_id == "k3");
    DataKeyBuffer dek;
    CHECK(!fresh_source->unwrap_dek(reqs[i].key_id, reqs[i].epoch_aad, reqs[i].wrapped_dek, dek));
    CHECK(std::memcmp(dek.data(), original_deks[i].data(), 32) == 0);
  }

  // A second retirement of k1 returns identical bytes.
  auto second_retire_res = retire_key_in_header(retired_bytes, "k1", *key_source, "owner-retire", valid_desc);
  CHECK(std::holds_alternative<std::string>(second_retire_res));
  CHECK(std::get<std::string>(second_retire_res) == retired_bytes);
}

void test_random_failures_header() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  secrets->set_secret("k2", std::string(32, '\x22'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto valid_desc = default_test_descriptor();

  // Create random failures: call 1 = store_id (32), call 2 = DEK (32), call 3 = wrap_dek nonce (24).
  constexpr std::size_t kCreateDraws[] = {32, 32, 24};
  for (std::size_t call_idx = 1; call_idx <= std::size(kCreateDraws); ++call_idx) {
    custody_ops::reset_random_seam();
    custody_ops::fail_random_on_call(call_idx);
    auto res = create_header(*key_source, "owner-rng", valid_desc);
    const auto draws = custody_ops::recorded_random_draws();
    custody_ops::reset_random_seam();
    CHECK(draws == std::vector<std::size_t>(kCreateDraws, kCreateDraws + call_idx));
    CHECK(std::holds_alternative<CustodyError>(res));
    CHECK(std::get<CustodyError>(res).error.reason == "archive.custody.key_invalid");
  }

  std::string create_store_id(32, '\x31');
  std::string create_dek(32, '\x41');
  std::string create_nonce(24, '\x51');
  custody_ops::reset_random_seam();
  custody_ops::override_next_random_bytes(create_store_id);
  custody_ops::override_next_random_bytes(create_dek);
  custody_ops::override_next_random_bytes(create_nonce);
  auto h1_res = create_header(*key_source, "owner-rng", valid_desc);
  const auto create_draws = custody_ops::recorded_random_draws();
  custody_ops::reset_random_seam();
  CHECK(create_draws == (std::vector<std::size_t>{32, 32, 24}));
  CHECK(std::holds_alternative<std::string>(h1_res));
  const std::string& h1_bytes = std::get<std::string>(h1_res);
  {
    auto h1_parsed = parse_header(h1_bytes);
    CHECK(std::holds_alternative<StoreHeader>(h1_parsed));
    CHECK( std::get<StoreHeader>(h1_parsed).epochs[0].wrapped_dek.substr(0, 24) == create_nonce);
  }

  // Rotate random failures: call 1 = new DEK (32), call 2 = wrap_dek nonce (24).
  constexpr std::size_t kRotateDraws[] = {32, 24};
  for (std::size_t call_idx = 1; call_idx <= std::size(kRotateDraws); ++call_idx) {
    custody_ops::reset_random_seam();
    custody_ops::fail_random_on_call(call_idx);
    auto res = rotate_header(h1_bytes, *key_source, "k2", "owner-rng", valid_desc);
    const auto draws = custody_ops::recorded_random_draws();
    custody_ops::reset_random_seam();
    CHECK(draws == std::vector<std::size_t>(kRotateDraws, kRotateDraws + call_idx));
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.reason == "archive.custody.key_invalid");
    CHECK(err.store_id.size() == 64);
    CHECK(err.key_id == "k2");
  }

  // Key buffer allocation failures across rotate_header (including mac_key allocation inside verify_header_tag).
  for (std::size_t alloc_idx = 1; alloc_idx <= 7; ++alloc_idx) {
    custody_ops::fail_key_alloc_on_call(alloc_idx);
    auto res = rotate_header(h1_bytes, *key_source, "k2", "owner-rng", valid_desc);
    custody_ops::reset_random_seam();
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.reason == "archive.custody.key_invalid");
    CHECK(err.store_id.size() == 64);
    CHECK(err.key_id == "k1");
  }

  std::string rotate_dek(32, '\x61');
  std::string rotate_nonce(24, '\x71');
  custody_ops::reset_random_seam();
  custody_ops::override_next_random_bytes(rotate_dek);
  custody_ops::override_next_random_bytes(rotate_nonce);
  auto h2_res = rotate_header(h1_bytes, *key_source, "k2", "owner-rng", valid_desc);
  const auto rotate_draws = custody_ops::recorded_random_draws();
  custody_ops::reset_random_seam();
  CHECK(rotate_draws == (std::vector<std::size_t>{32, 24}));
  CHECK(std::holds_alternative<std::string>(h2_res));
  const std::string& h2_bytes = std::get<std::string>(h2_res);
  {
    auto h2_parsed = parse_header(h2_bytes);
    CHECK(std::holds_alternative<StoreHeader>(h2_parsed));
    CHECK(std::get<StoreHeader>(h2_parsed).epochs[1].wrapped_dek.substr(0, 24) == rotate_nonce);
  }

  // Retire random failure on 1-rewrap header: call 1 = re-wrap nonce (24).
  custody_ops::reset_random_seam();
  custody_ops::fail_random_on_call(1);
  auto retire_res = retire_key_in_header(h2_bytes, "k1", *key_source, "owner-rng", valid_desc);
  const auto retire_fail_draws = custody_ops::recorded_random_draws();
  custody_ops::reset_random_seam();
  CHECK(retire_fail_draws == (std::vector<std::size_t>{24}));
  CHECK(std::holds_alternative<CustodyError>(retire_res));
  const auto& retire_err = std::get<CustodyError>(retire_res);
  CHECK(retire_err.error.reason == "archive.custody.key_invalid");
  CHECK(retire_err.store_id.size() == 64);
  CHECK(retire_err.key_id == "k2");

  // Retire random failures and injected nonces when retiring 2 epochs wrapped under "k1": one 24-byte draw per re-wrap.
  auto h1b_res = rotate_header(h1_bytes, *key_source, "k1", "owner-rng", valid_desc);
  CHECK(std::holds_alternative<std::string>(h1b_res));
  auto h3_two_k1_res =
      rotate_header(std::get<std::string>(h1b_res), *key_source, "k2", "owner-rng", valid_desc);
  CHECK(std::holds_alternative<std::string>(h3_two_k1_res));
  const std::string& h3_two_k1 = std::get<std::string>(h3_two_k1_res);

  for (std::size_t call_idx : {std::size_t{1}, std::size_t{2}}) {
    custody_ops::reset_random_seam();
    custody_ops::fail_random_on_call(call_idx);
    auto r = retire_key_in_header(h3_two_k1, "k1", *key_source, "owner-rng", valid_desc);
    const auto draws = custody_ops::recorded_random_draws();
    custody_ops::reset_random_seam();
    CHECK(draws == std::vector<std::size_t>(call_idx, 24));
    CHECK(std::holds_alternative<CustodyError>(r));
    CHECK(std::get<CustodyError>(r).error.reason == "archive.custody.key_invalid");
  }

  std::string retire_nonce_1(24, '\x81');
  std::string retire_nonce_2(24, '\x91');
  custody_ops::reset_random_seam();
  custody_ops::override_next_random_bytes(retire_nonce_1);
  custody_ops::override_next_random_bytes(retire_nonce_2);
  auto retired_two_res = retire_key_in_header(h3_two_k1, "k1", *key_source, "owner-rng", valid_desc);
  const auto retire_two_draws = custody_ops::recorded_random_draws();
  custody_ops::reset_random_seam();
  CHECK(retire_two_draws == (std::vector<std::size_t>{24, 24}));
  CHECK(std::holds_alternative<std::string>(retired_two_res));
  {
    auto retired_parsed = parse_header(std::get<std::string>(retired_two_res));
    CHECK(std::holds_alternative<StoreHeader>(retired_parsed));
    const auto& rh = std::get<StoreHeader>(retired_parsed);
    CHECK(rh.epochs[0].wrapped_dek.substr(0, 24) == retire_nonce_1);
    CHECK(rh.epochs[1].wrapped_dek.substr(0, 24) == retire_nonce_2);
  }

  // Key buffer allocation failures across retire_key_in_header (including mac_key allocation inside verify_header_tag).
  for (std::size_t alloc_idx = 1; alloc_idx <= 7; ++alloc_idx) {
    custody_ops::fail_key_alloc_on_call(alloc_idx);
    auto res = retire_key_in_header(h2_bytes, "k1", *key_source, "owner-rng", valid_desc);
    custody_ops::reset_random_seam();
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.reason == "archive.custody.key_invalid");
    CHECK(err.store_id.size() == 64);
    CHECK(err.key_id == "k2");
  }
}

class MismatchedWrapKeySource final : public KeySource {
 public:
  MismatchedWrapKeySource(std::shared_ptr<KeySource> wrap_source,
                          std::shared_ptr<KeySource> unwrap_source)
      : wrap_source_(std::move(wrap_source)), unwrap_source_(std::move(unwrap_source)) {}

  CustodyResult<std::string> active_key_id() override { return unwrap_source_->active_key_id(); }

  CustodyResult<std::string> wrap_dek(std::string_view key_id,
                                      std::string_view epoch_aad,
                                      const DataKeyBuffer& dek) override {
    return wrap_source_->wrap_dek(key_id, epoch_aad, dek);
  }

  std::optional<CustodyFault> unwrap_dek(std::string_view key_id,
                                         std::string_view epoch_aad,
                                         std::string_view wrapped_dek,
                                         DataKeyBuffer& dek_out) override {
    return unwrap_source_->unwrap_dek(key_id, epoch_aad, wrapped_dek, dek_out);
  }

 private:
  std::shared_ptr<KeySource> wrap_source_;
  std::shared_ptr<KeySource> unwrap_source_;
};

void test_wrap_verification() {
  auto wrap_secrets = std::make_shared<FixedSecretSource>("k1");
  wrap_secrets->set_secret("k1", std::string(32, '\xaa'));
  wrap_secrets->set_secret("k2", std::string(32, '\xbb'));
  auto wrap_source = make_hkdf_key_source(wrap_secrets);

  auto unwrap_secrets = std::make_shared<FixedSecretSource>("k1");
  unwrap_secrets->set_secret("k1", std::string(32, '\x11'));
  unwrap_secrets->set_secret("k2", std::string(32, '\x22'));
  auto unwrap_source = make_hkdf_key_source(unwrap_secrets);

  MismatchedWrapKeySource mismatched(wrap_source, unwrap_source);
  const auto valid_desc = default_test_descriptor();

  // create_header fails post-wrap verification and returns no bytes.
  {
    auto res = create_header(mismatched, "owner-verify", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.reason == "archive.custody.key_invalid");
    CHECK(err.key_id == "k1");
    CHECK(err.store_id.size() == 64);
  }

  // Build valid 2-epoch header with unwrap_source so existing epochs unwrap cleanly, then test rotate and retire
  // with `mismatched` so the failure occurs specifically on the verification unwrap of the newly wrapped DEK.
  auto h1_res = create_header(*unwrap_source, "owner-verify", valid_desc);
  CHECK(std::holds_alternative<std::string>(h1_res));
  auto h2_res = rotate_header(std::get<std::string>(h1_res), *unwrap_source, "k2", "owner-verify", valid_desc);
  CHECK(std::holds_alternative<std::string>(h2_res));
  const std::string& h2_bytes = std::get<std::string>(h2_res);

  // rotate_header fails post-wrap verification and returns no bytes.
  {
    auto res = rotate_header(h2_bytes, mismatched, "k2", "owner-verify", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.reason == "archive.custody.key_invalid");
    CHECK(err.key_id == "k2");
    CHECK(err.store_id.size() == 64);
  }

  // retire_key_in_header fails post-wrap verification and returns no bytes.
  {
    auto res = retire_key_in_header(h2_bytes, "k1", mismatched, "owner-verify", valid_desc);
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.reason == "archive.custody.key_invalid");
    CHECK(err.key_id == "k2");
    CHECK(err.store_id.size() == 64);
  }

  // Post-wrap verification on HkdfKeySource derives the KEK fresh and never reuses cached_keks_.
  // Corrupting the cached KEK causes wrap_dek to encrypt under the corrupted cached KEK while
  // verify_unwrap_dek derives the uncorrupted KEK fresh and rejects with InvalidKeyMaterial.
  const std::string& h1_bytes = std::get<std::string>(h1_res);
  {
    custody_ops::corrupt_next_cached_kek();
    auto res = create_header(*unwrap_source, "owner-verify", valid_desc);
    custody_ops::reset_random_seam();
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.reason == "archive.custody.key_invalid");
    CHECK(err.key_id == "k1");
    CHECK(err.store_id.size() == 64);
  }
  {
    custody_ops::corrupt_next_cached_kek();
    auto res = rotate_header(h1_bytes, *unwrap_source, {}, "owner-verify", valid_desc);
    custody_ops::reset_random_seam();
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.reason == "archive.custody.key_invalid");
    CHECK(err.key_id == "k1");
    CHECK(err.store_id.size() == 64);
  }
  {
    custody_ops::corrupt_next_cached_kek();
    auto res = rotate_header(h1_bytes, *unwrap_source, "k2", "owner-verify", valid_desc);
    custody_ops::reset_random_seam();
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.reason == "archive.custody.key_invalid");
    CHECK(err.key_id == "k2");
    CHECK(err.store_id.size() == 64);
  }
  {
    custody_ops::corrupt_next_cached_kek();
    auto res = retire_key_in_header(h2_bytes, "k1", *unwrap_source, "owner-verify", valid_desc);
    custody_ops::reset_random_seam();
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.reason == "archive.custody.key_invalid");
    CHECK(err.key_id == "k2");
    CHECK(err.store_id.size() == 64);
  }
  {
    // Warm the KEK cache in an outer scope before arming corrupt_next_cached_kek(), then rotate under the same key.
    CustodyOperationScope outer_scope(*unwrap_source);
    auto reqs_res = unwrap_requests(h1_bytes, "owner-verify", valid_desc);
    CHECK(std::holds_alternative<std::vector<UnwrapRequest>>(reqs_res));
    const auto& req = std::get<std::vector<UnwrapRequest>>(reqs_res).at(0);
    DataKeyBuffer warm_dek;
    CHECK(!unwrap_source->unwrap_dek(req.key_id, req.epoch_aad, req.wrapped_dek, warm_dek));
    custody_ops::corrupt_next_cached_kek();
    auto res = rotate_header(h1_bytes, *unwrap_source, "k1", "owner-verify", valid_desc);
    custody_ops::reset_random_seam();
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.reason == "archive.custody.key_invalid");
    CHECK(err.key_id == "k1");
    CHECK(err.store_id.size() == 64);
  }
}

void test_epoch_cap() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto valid_desc = default_test_descriptor();

  HostCustodyLimits limits_3{};
  limits_3.max_epochs = 3;

  auto h1_res = create_header(*key_source, "owner-cap", valid_desc, limits_3);
  CHECK(std::holds_alternative<std::string>(h1_res));
  auto h2_res = rotate_header(std::get<std::string>(h1_res), *key_source, {}, "owner-cap", valid_desc, limits_3);
  CHECK(std::holds_alternative<std::string>(h2_res));
  auto h3_res = rotate_header(std::get<std::string>(h2_res), *key_source, {}, "owner-cap", valid_desc, limits_3);
  CHECK(std::holds_alternative<std::string>(h3_res));
  const std::string& h3_bytes = std::get<std::string>(h3_res);

  // Fourth epoch rotation (would exceed max_epochs == 3) is refused with CustodyFault::EpochLimit.
  auto h4_cap_res = rotate_header(h3_bytes, *key_source, {}, "owner-cap", valid_desc, limits_3);
  CHECK(std::holds_alternative<CustodyError>(h4_cap_res));
  const auto& cap_err = std::get<CustodyError>(h4_cap_res);
  CHECK(cap_err.error.kind == ErrorKind::ResourceLimit);
  CHECK(cap_err.error.retry_class == RetryClass::Never);
  CHECK(cap_err.error.retry_safety == RetrySafety::NotSent);
  CHECK(cap_err.error.reason == "archive.custody.epoch_limit");
  CHECK(cap_err.store_id.size() == 64);
  CHECK(cap_err.key_id.empty());

  // Build a 4-epoch header under default limits, then verify parse_header accepts the 3-epoch header
  // and refuses the 4-epoch header when max_epochs == 3, and accepts the 4-epoch header when max_epochs == 4.
  CHECK(std::holds_alternative<StoreHeader>(parse_header(h3_bytes, limits_3)));

  auto h4_default_res = rotate_header(h3_bytes, *key_source, {}, "owner-cap", valid_desc);
  CHECK(std::holds_alternative<std::string>(h4_default_res));
  const std::string& h4_bytes = std::get<std::string>(h4_default_res);
  CHECK(std::holds_alternative<StoreHeader>(parse_header(h4_bytes)));

  auto parsed_cap_res = parse_header(h4_bytes, limits_3);
  CHECK(std::holds_alternative<CustodyError>(parsed_cap_res));
  CHECK(std::get<CustodyError>(parsed_cap_res).error.reason == "archive.custody.corrupt_header");

  HostCustodyLimits limits_4{};
  limits_4.max_epochs = 4;
  CHECK(std::holds_alternative<StoreHeader>(parse_header(h4_bytes, limits_4)));
}

class CountingKeySource final : public KeySource {
 public:
  explicit CountingKeySource(std::shared_ptr<KeySource> inner) : inner_(std::move(inner)) {}

  void reset_counts() noexcept {
    active_key_id_calls = 0;
    wrap_calls = 0;
    unwrap_calls = 0;
    begin_scope_calls = 0;
    end_scope_calls = 0;
  }

  CustodyResult<std::string> active_key_id() override {
    ++active_key_id_calls;
    return inner_->active_key_id();
  }

  CustodyResult<std::string> wrap_dek(std::string_view key_id,
                                      std::string_view epoch_aad,
                                      const DataKeyBuffer& dek) override {
    ++wrap_calls;
    return inner_->wrap_dek(key_id, epoch_aad, dek);
  }

  std::optional<CustodyFault> unwrap_dek(std::string_view key_id,
                                         std::string_view epoch_aad,
                                         std::string_view wrapped_dek,
                                         DataKeyBuffer& dek_out) override {
    ++unwrap_calls;
    return inner_->unwrap_dek(key_id, epoch_aad, wrapped_dek, dek_out);
  }

  std::optional<CustodyFault> verify_unwrap_dek(std::string_view key_id,
                                                std::string_view epoch_aad,
                                                std::string_view wrapped_dek,
                                                DataKeyBuffer& dek_out) override {
    ++unwrap_calls;
    return KeySource::forward_verify_unwrap_dek(*inner_, key_id, epoch_aad, wrapped_dek, dek_out);
  }

  void begin_operation_scope() noexcept override {
    ++begin_scope_calls;
    scopes_.push_back(std::make_unique<CustodyOperationScope>(*inner_));
  }

  void end_operation_scope() noexcept override {
    ++end_scope_calls;
    if (!scopes_.empty()) {
      scopes_.pop_back();
    }
  }

  std::size_t active_key_id_calls = 0;
  std::size_t wrap_calls = 0;
  std::size_t unwrap_calls = 0;
  std::size_t begin_scope_calls = 0;
  std::size_t end_scope_calls = 0;

 private:
  std::shared_ptr<KeySource> inner_;
  std::vector<std::unique_ptr<CustodyOperationScope>> scopes_;
};

void test_header_call_counts() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  secrets->set_secret("k2", std::string(32, '\x22'));
  secrets->set_secret("k3", std::string(32, '\x33'));
  auto hkdf_source = make_hkdf_key_source(secrets);
  CountingKeySource counting(hkdf_source);
  const auto valid_desc = default_test_descriptor();

  // 1. create_header: 1 scope, 1 active_key_id, 1 wrap, 1 verification unwrap;
  // k1 loaded twice (1 for wrap_dek, 1 fresh derivation in verify_unwrap_dek).
  counting.reset_counts();
  secrets->reset_counts();
  auto h1_res = create_header(counting, "owner-counts", valid_desc);
  CHECK(std::holds_alternative<std::string>(h1_res));
  CHECK(counting.begin_scope_calls == 1);
  CHECK(counting.end_scope_calls == 1);
  CHECK(counting.active_key_id_calls == 1);
  CHECK(counting.wrap_calls == 1);
  CHECK(counting.unwrap_calls == 1);
  CHECK(secrets->load_count("k1") == 2);

  // 2. rotate_header with empty target_key_id (active = k1): 1 scope, 1 active_key_id, 1 current unwrap +
  // 1 wrap + 1 verification unwrap = 2 unwraps; k1 loaded twice (1 cached across current unwrap + wrap,
  // 1 fresh derivation in verify_unwrap_dek).
  counting.reset_counts();
  secrets->reset_counts();
  auto h2_res = rotate_header(std::get<std::string>(h1_res), counting, {}, "owner-counts", valid_desc);
  CHECK(std::holds_alternative<std::string>(h2_res));
  CHECK(counting.begin_scope_calls == 1);
  CHECK(counting.end_scope_calls == 1);
  CHECK(counting.active_key_id_calls == 1);
  CHECK(counting.wrap_calls == 1);
  CHECK(counting.unwrap_calls == 2);
  CHECK(secrets->load_count("k1") == 2);

  // 3. rotate_header with explicit target_key_id = "k3": 1 scope, 0 active_key_id, 1 current unwrap (k1) +
  // 1 wrap (k3) + 1 verification unwrap (k3); k1 loaded once and k3 loaded twice (wrap + fresh verify).
  counting.reset_counts();
  secrets->reset_counts();
  auto h3_res = rotate_header(std::get<std::string>(h2_res), counting, "k3", "owner-counts", valid_desc);
  CHECK(std::holds_alternative<std::string>(h3_res));
  CHECK(counting.begin_scope_calls == 1);
  CHECK(counting.end_scope_calls == 1);
  CHECK(counting.active_key_id_calls == 0);
  CHECK(counting.wrap_calls == 1);
  CHECK(counting.unwrap_calls == 2);
  CHECK(secrets->load_count("k1") == 1);
  CHECK(secrets->load_count("k3") == 2);

  // 4. retire_key_in_header retiring "k1" (used by epochs 1 and 2, current epoch 3 uses "k3"):
  // 1 scope, 0 active_key_id, 1 current unwrap (k3) + 2 retiring unwraps (k1) + 2 re-wraps (k3) +
  // 2 verification unwraps (k3) = 2 wraps and 5 unwraps; k1 loaded once (cached across both retiring unwraps)
  // and k3 loaded 3 times (1 cached across current unwrap + 2 re-wraps, plus 2 fresh derivations in verify_unwrap_dek).
  counting.reset_counts();
  secrets->reset_counts();
  auto retired_res = retire_key_in_header(std::get<std::string>(h3_res), "k1", counting, "owner-counts", valid_desc);
  CHECK(std::holds_alternative<std::string>(retired_res));
  CHECK(counting.begin_scope_calls == 1);
  CHECK(counting.end_scope_calls == 1);
  CHECK(counting.active_key_id_calls == 0);
  CHECK(counting.wrap_calls == 2);
  CHECK(counting.unwrap_calls == 5);
  CHECK(secrets->load_count("k1") == 1);
  CHECK(secrets->load_count("k3") == 3);
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
    test_existing_archive_portability_unchanged();
    test_golden_header_bytes();
    test_header_splices();
    test_epoch_aad_single_field_binding();
    test_retire_key_in_header();
    test_random_failures_header();
    test_wrap_verification();
    test_epoch_cap();
    test_header_call_counts();
    std::cout << "native archive custody tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
