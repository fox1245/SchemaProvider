// Host-native archive custody primitives, store header, and sealed record tests: HKDF-SHA256,
// XChaCha20-Poly1305, derive_kek, make_hkdf_key_source, LockedKeyBuffer, CleansedBuffer,
// StoreHeader, NativeArchive::open_custody/seal/open, and CustodyFault mapping.
#include "codecs/chat.h"
#include "codecs/gemini.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions.h"
#include "codecs/interactions_request.h"
#include "codecs/messages.h"
#include "codecs/messages_request.h"
#include "codecs/responses.h"
#include "codecs/responses_request.h"
#include "core/interface_contract.h"
#include "core/native.h"
#include "core/native_archive.h"
#include "core/native_archive_custody.h"
#include "core/native_archive_custody_format.h"
#include "core/native_archive_custody_internal.h"
#include "crypto/crypto.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include "sp/config_defaults.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#if SP_BUILD_THREADS
#include <latch>
#endif
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#if SP_BUILD_THREADS
#include <thread>
#endif
#include <type_traits>
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

#if SP_BUILD_THREADS
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
#endif

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
      {CustodyFault::InvalidInput, ErrorKind::InvalidRequest, RetryClass::Never, RetrySafety::NotSent,
       "archive.custody.invalid_input", false},
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

  if (!archive_fs::available()) {
    auto unsupported_prov = NativeArchive::provision("records", "activation", "synthetic-owner", valid_desc);
    CHECK(std::holds_alternative<Error>(unsupported_prov));
    CHECK(std::get<Error>(unsupported_prov).kind == ErrorKind::Unsupported);
    auto unsupported_open = NativeArchive::open("records", "activation", "synthetic-owner", valid_desc);
    CHECK(std::holds_alternative<Error>(unsupported_open));
    CHECK(std::get<Error>(unsupported_open).kind == ErrorKind::Unsupported);
    return;
  }

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

  // 3. Change owner_scope -> unwrap_requests, rotate_header, and retire_key_in_header refuse a mismatched
  // owner_scope with Tampered and an empty owner_scope with InvalidInput, populating store_id; if header
  // owner_scope is also rewritten, unwrap_dek refuses because epoch_aad's owner_scope differs.
  {
    {
      auto mismatch_res = unwrap_requests(h2_bytes, "other-owner", valid_desc);
      CHECK(std::holds_alternative<CustodyError>(mismatch_res));
      CHECK(std::get<CustodyError>(mismatch_res).error.reason == "archive.custody.tampered");
      CHECK(std::get<CustodyError>(mismatch_res).store_id == store_id_hex);

      auto rot_res = rotate_header(h2_bytes, *key_source, "k2", "other-owner", valid_desc);
      CHECK(std::holds_alternative<CustodyError>(rot_res));
      CHECK(std::get<CustodyError>(rot_res).error.reason == "archive.custody.tampered");
      CHECK(std::get<CustodyError>(rot_res).store_id == store_id_hex);

      auto ret_res = retire_key_in_header(h2_bytes, "k1", *key_source, "other-owner", valid_desc);
      CHECK(std::holds_alternative<CustodyError>(ret_res));
      CHECK(std::get<CustodyError>(ret_res).error.reason == "archive.custody.tampered");
      CHECK(std::get<CustodyError>(ret_res).store_id == store_id_hex);
    }
    {
      auto empty_unwrap = unwrap_requests(h2_bytes, "", valid_desc);
      CHECK(std::holds_alternative<CustodyError>(empty_unwrap));
      CHECK(std::get<CustodyError>(empty_unwrap).error.kind == ErrorKind::InvalidRequest);
      CHECK(std::get<CustodyError>(empty_unwrap).error.reason == "archive.custody.invalid_input");
      CHECK(std::get<CustodyError>(empty_unwrap).store_id == store_id_hex);

      auto empty_rot = rotate_header(h2_bytes, *key_source, "k2", "", valid_desc);
      CHECK(std::holds_alternative<CustodyError>(empty_rot));
      CHECK(std::get<CustodyError>(empty_rot).error.kind == ErrorKind::InvalidRequest);
      CHECK(std::get<CustodyError>(empty_rot).error.reason == "archive.custody.invalid_input");
      CHECK(std::get<CustodyError>(empty_rot).store_id == store_id_hex);

      auto empty_ret = retire_key_in_header(h2_bytes, "k1", *key_source, "", valid_desc);
      CHECK(std::holds_alternative<CustodyError>(empty_ret));
      CHECK(std::get<CustodyError>(empty_ret).error.kind == ErrorKind::InvalidRequest);
      CHECK(std::get<CustodyError>(empty_ret).error.reason == "archive.custody.invalid_input");
      CHECK(std::get<CustodyError>(empty_ret).store_id == store_id_hex);
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

descriptor::ValidatedDescriptor provider_test_descriptor(
    std::string_view family,
    std::string_view base = "http://127.0.0.1:18080") {
  const auto route = family == "anthropic.messages" ? "/v1/messages"
                     : family == "openai.chat"      ? "/chat/completions"
                                                    : "/v1/responses";
  auto loaded = descriptor::load(
      "{\"descriptor_version\":1,\"revision\":1,\"id\":\"archive-fixture\",\"family\":" +
      json::quote(family) + ",\"connection\":{\"base_url\":" + json::quote(base) +
      ",\"paths\":{\"buffered\":" + json::quote(route) +
      ",\"streaming\":" + json::quote(route) + "}}}");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}

std::shared_ptr<const json::Document> parse_json_doc(std::string_view bytes) {
  auto parsed = json::parse(bytes);
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::get<json::Document>(std::move(parsed)));
}

Message make_user_message(std::string text) {
  return Message{{}, Role::User, {Text{std::move(text)}}};
}

void check_same_document(const std::shared_ptr<const json::Document>& a,
                         const std::shared_ptr<const json::Document>& b) {
  CHECK(bool(a) == bool(b));
  if (a) {
    CHECK(json::equal(a->root(), b->root()));
  }
}

void check_same_media(const Media& p, const Media& q) {
  CHECK(p.kind == q.kind && p.source == q.source && p.mime == q.mime && p.detail == q.detail &&
        p.reference == q.reference && p.name == q.name && p.id == q.id &&
        p.transcript == q.transcript && bool(p.data) == bool(q.data));
  if (p.data) {
    CHECK(*p.data == *q.data);
  }
}

void check_same_part(const Part& a, const Part& b) {
  CHECK(a.index() == b.index());
  std::visit(
      [&](const auto& p) {
        using P = std::decay_t<decltype(p)>;
        const auto& q = std::get<P>(b);
        if constexpr (std::is_same_v<P, Text>) {
          CHECK(p.value == q.value);
        } else if constexpr (std::is_same_v<P, Refusal>) {
          CHECK(p.text == q.text && p.raw_code == q.raw_code);
        } else if constexpr (std::is_same_v<P, ToolCall> || std::is_same_v<P, InvalidToolCall>) {
          CHECK(p.id == q.id && p.name == q.name && p.kind == q.kind && p.wire_type == q.wire_type);
          check_same_document(p.wire_metadata, q.wire_metadata);
          if constexpr (std::is_same_v<P, ToolCall>) {
            check_same_document(p.input, q.input);
          } else {
            CHECK(p.raw_fragment == q.raw_fragment && p.reason == q.reason);
          }
        } else if constexpr (std::is_same_v<P, Thinking>) {
          CHECK(p.text == q.text && p.signature == q.signature);
        } else if constexpr (std::is_same_v<P, RedactedThinking>) {
          CHECK(p.data == q.data);
        } else if constexpr (std::is_same_v<P, ServerToolResult>) {
          CHECK(p.tool_use_id == q.tool_use_id && p.wire_type == q.wire_type);
          check_same_document(p.content, q.content);
        } else if constexpr (std::is_same_v<P, ToolResult>) {
          CHECK(p.tool_use_id == q.tool_use_id && p.content == q.content &&
                p.is_error == q.is_error && p.host.has_value() == q.host.has_value());
          if (p.host) {
            CHECK(p.host->name == q.host->name && p.host->status == q.host->status &&
                  p.host->retryable == q.host->retryable &&
                  p.host->effect_uncertain == q.host->effect_uncertain);
          }
          CHECK(p.content_parts.size() == q.content_parts.size());
          for (std::size_t i = 0; i < p.content_parts.size(); ++i) {
            CHECK(p.content_parts[i].index() == q.content_parts[i].index());
            if (const auto* text = std::get_if<Text>(&p.content_parts[i])) {
              CHECK(text->value == std::get<Text>(q.content_parts[i]).value);
            } else if (const auto* block = std::get_if<Opaque>(&p.content_parts[i])) {
              const auto& other_block = std::get<Opaque>(q.content_parts[i]);
              CHECK(block->wire_type == other_block.wire_type);
              check_same_document(block->wire_metadata, other_block.wire_metadata);
            } else {
              check_same_media(std::get<Media>(p.content_parts[i]),
                               std::get<Media>(q.content_parts[i]));
            }
          }
        } else if constexpr (std::is_same_v<P, Reasoning>) {
          CHECK(p.id == q.id && p.summary == q.summary &&
                p.encrypted_content == q.encrypted_content && p.status == q.status &&
                p.content == q.content);
        } else if constexpr (std::is_same_v<P, Opaque>) {
          CHECK(p.wire_type == q.wire_type);
          check_same_document(p.wire_metadata, q.wire_metadata);
        } else if constexpr (std::is_same_v<P, Media>) {
          check_same_media(p, q);
        } else if constexpr (std::is_same_v<P, Thought>) {
          CHECK(p.summary == q.summary && p.signature == q.signature);
        }
      },
      a);
}

std::string gemini_routed_paths(std::string json_text, bool routed) {
  if (!routed) return json_text;
  for (auto at = json_text.find("/fixture-model:"); at != std::string::npos;
       at = json_text.find("/fixture-model:")) {
    json_text.replace(at + 1, 13, "{model}");
  }
  return json_text;
}

std::shared_ptr<NativeArchive> open_custody_checked(
    std::string_view header_bytes,
    KeySource& source,
    std::string_view owner_scope,
    const descriptor::ValidatedDescriptor& descriptor,
    HostCustodyLimits limits = {}) {
  auto opened = NativeArchive::open_custody(header_bytes, source, owner_scope, descriptor, limits);
  CHECK(std::holds_alternative<std::shared_ptr<NativeArchive>>(opened));
  return std::get<std::shared_ptr<NativeArchive>>(std::move(opened));
}

SealedRecord seal_checked(const NativeArchive& archive,
                          const std::vector<Message>& messages,
                          std::string_view binding = {}) {
  auto sealed = archive.seal(messages, binding);
  CHECK(std::holds_alternative<SealedRecord>(sealed));
  return std::get<SealedRecord>(std::move(sealed));
}

std::vector<Message> open_checked(const NativeArchive& archive,
                                  std::string_view reference,
                                  std::string_view bytes,
                                  std::string_view expected_binding = {}) {
  auto opened = archive.open(reference, bytes, expected_binding);
  CHECK(std::holds_alternative<std::vector<Message>>(opened));
  return std::get<std::vector<Message>>(std::move(opened));
}

void test_custody_roundtrip_all_provider_families() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  auto hkdf_source = make_hkdf_key_source(secrets);
  CountingKeySource counting(hkdf_source);

  // 1. anthropic.messages: portable message with all part kinds + wire_output, genuine native message
  // with thinking signature and redacted thinking, incomplete partial native message, diagnostic
  // duplicate-key document, and extended tool result bounds.
  {
    const auto d = provider_test_descriptor("anthropic.messages");
    auto h_res = create_header(counting, "owner-anthropic", d);
    CHECK(std::holds_alternative<std::string>(h_res));
    const std::string& header_bytes = std::get<std::string>(h_res);
    auto archive = open_custody_checked(header_bytes, counting, "owner-anthropic", d);
    auto reopened = open_custody_checked(header_bytes, *hkdf_source, "owner-anthropic", d);

    counting.reset_counts();
    secrets->reset_counts();
    CHECK(archive->owner_scope() == "owner-anthropic");
    CHECK(archive->current_epoch() == 1);
    CHECK(archive->matches_descriptor(d));
    CHECK(!archive->matches_descriptor(provider_test_descriptor("openai.responses")));

    for (const auto mutation : std::array<std::string_view, 6>{
             R"({"descriptor_version":1,"revision":1,"id":"archive-fixture","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}},"bindings":{"model":"other_model"}})",
             R"({"descriptor_version":1,"revision":1,"id":"archive-fixture","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}},"bindings":{"messages":"other_messages"}})",
             R"({"descriptor_version":1,"revision":1,"id":"archive-fixture","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}},"bindings":{"stream":"other_stream"}})",
             R"({"descriptor_version":1,"revision":1,"id":"archive-fixture","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}},"bindings":{"max_output_tokens":"other_cap"}})",
             R"({"descriptor_version":1,"revision":1,"id":"archive-fixture","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}},"bindings":{"usage":["other_usage"]}})",
             R"({"descriptor_version":1,"revision":1,"id":"archive-fixture","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}},"stop_reasons":{"custom_stop":"EndTurn"}})"}) {
      auto mutated_desc = descriptor::load(mutation);
      CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(mutated_desc));
      CHECK(!archive->matches_descriptor(std::get<descriptor::ValidatedDescriptor>(mutated_desc)));
    }

    auto object = parse_json_doc(
        R"({"second":[1,true,null],"first":{"x":"value","float":1.0,"negative_zero":-0.0,"signed":-4,"large":18446744073709551615}})");
    ToolCall call{"call", "lookup", ToolCallKind::ApprovalRequest, object, "function_call", object};
    InvalidToolCall invalid{"bad", "lookup", ToolCallKind::ClientExecuted, "{",
                            InvalidReason::Truncated, "function_call", object};
    Message portable{
        "portable",
        Role::Developer,
        {Text{std::string("a\0b", 3)},
         Refusal{"no", "policy"},
         call,
         invalid,
         Thinking{"t", std::string{"portable-sig-marker"}},
         RedactedThinking{"portable-redacted-marker"},
         ServerToolResult{"server", "search_result", object},
         ToolResult{"call", "error", true},
         Reasoning{"reason", {"one", "two"}, "portable-encrypted-marker", "completed", {"body"}},
         Opaque{"future", object},
         Media::image("image/png", std::make_shared<const std::string>("AAEC"), ImageDetail::Original),
         Thought{{"thought"}, "portable-thought-sig"}},
        {},
        parse_json_doc(R"([{"raw":"ordered-wire-marker"},{"raw":"group"}])")};
    std::get<ToolResult>(portable.parts[7]).host =
        ToolResultHostMetadata{"lookup", "uncertain", true, true};
    ToolResult extended{"nested", "", true,
                        ToolResultHostMetadata{"lookup", "uncertain", true, true}};
    auto audio =
        Media::audio("audio/l16;rate=24000", std::make_shared<const std::string>("AQID"));
    audio.name = "spoken.raw";
    audio.id = "audio_identity";
    audio.transcript = "spoken text";
    auto image =
        Media::image("image/png", std::make_shared<const std::string>("AAEC"), ImageDetail::High);
    image.name = "image.png";
    extended.content_parts = {
        Text{std::string("nested\0text", 11)},
        audio,
        Media::url(MediaKind::Video, "https://example.test/clip", "video/mp4"),
        image,
        Media::file(MediaKind::Document, "file-owned", "application/pdf"),
        Opaque{"nested_opaque", object},
        Text{"last"}};
    std::get<Media>(extended.content_parts[4]).name = "invoice.pdf";
    portable.parts.emplace_back(extended);

    const SealedRecord sealed_portable = seal_checked(*archive, {portable}, "portable-call");
    CHECK(sealed_portable.reference.substr(0, 6) == "spna4:");
    CHECK(sealed_portable.reference.size() == 70);
    for (std::string_view marker :
         {"portable-sig-marker", "portable-redacted-marker", "portable-encrypted-marker",
          "portable-thought-sig", "ordered-wire-marker", "spoken text", "invoice.pdf"}) {
      CHECK(sealed_portable.bytes.find(marker) == std::string::npos);
    }

    const auto restored_portable =
        open_checked(*reopened, sealed_portable.reference, sealed_portable.bytes, "portable-call");
    CHECK(restored_portable.size() == 1 && !restored_portable[0].native &&
          restored_portable[0].id == portable.id && restored_portable[0].role == portable.role &&
          restored_portable[0].parts.size() == portable.parts.size());
    check_same_document(portable.wire_output, restored_portable[0].wire_output);
    for (std::size_t i = 0; i < portable.parts.size(); ++i) {
      check_same_part(portable.parts[i], restored_portable[0].parts[i]);
    }

    messages::Request r;
    r.model = "fixture-model";
    r.account_scope = "fixture-account";
    r.messages.push_back(make_user_message("hello"));
    auto enc_req = messages::encode(d, r, false);
    CHECK(std::holds_alternative<messages::EncodedRequest>(enc_req));
    Accumulator acc;
    messages::Codec codec(
        d, messages::Mode::Buffered, acc, std::get<messages::EncodedRequest>(enc_req).context);
    codec.buffered(
        R"({"id":"msg_archive","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"thinking","thinking":"reason","signature":"synthetic-sensitive-signature"},{"type":"redacted_thinking","data":"synthetic-encrypted-state"},{"type":"text","text":"answer"}],"stop_reason":"end_turn","stop_sequence":null,"usage":{"input_tokens":2,"output_tokens":3}})",
        {true, ErrorKind::Truncated});
    codec.finish();
    CHECK(acc.outcome() && std::holds_alternative<Completion>(*acc.outcome()));
    Message genuine_msg = std::get<Completion>(*acc.outcome()).messages.at(0);
    CHECK(genuine_msg.native && genuine_msg.native->complete());
    r.messages.push_back(genuine_msg);
    r.messages.push_back(make_user_message("continue"));

    const auto expected_encoded = messages::encode(d, r, false);
    CHECK(std::holds_alternative<messages::EncodedRequest>(expected_encoded));

    const SealedRecord sealed_native = seal_checked(*archive, r.messages, "native-call");
    for (std::string_view marker :
         {"synthetic-sensitive-signature", "synthetic-encrypted-state", "msg_archive", "answer"}) {
      CHECK(sealed_native.bytes.find(marker) == std::string::npos);
    }

    auto restored_native =
        open_checked(*archive, sealed_native.reference, sealed_native.bytes, "native-call");
    auto replay = r;
    replay.messages = restored_native;
    const auto actual_encoded = messages::encode(d, replay, false);
    CHECK(std::holds_alternative<messages::EncodedRequest>(actual_encoded));
    CHECK(std::get<messages::EncodedRequest>(actual_encoded).body ==
          std::get<messages::EncodedRequest>(expected_encoded).body);

    std::get<Thinking>(restored_native[1].parts[0]).signature = "changed";
    auto bad_seal = archive->seal(restored_native, "native-call");
    CHECK(std::holds_alternative<CustodyError>(bad_seal));
    CHECK(std::get<CustodyError>(bad_seal).error.kind == ErrorKind::InvalidRequest);
    CHECK(std::get<CustodyError>(bad_seal).error.reason == "archive.custody.invalid_input");
    CHECK(std::get<CustodyError>(bad_seal).store_id.size() == 64);

    // Incomplete partial native message round-trips with complete() == false and is refused for replay.
    messages::Request incomplete_req;
    incomplete_req.model = "fixture-model";
    incomplete_req.account_scope = "fixture-account";
    incomplete_req.messages.push_back(make_user_message("hello"));
    auto inc_enc = messages::encode(d, incomplete_req, false);
    CHECK(std::holds_alternative<messages::EncodedRequest>(inc_enc));
    Accumulator inc_acc;
    messages::Codec inc_codec(
        d, messages::Mode::Buffered, inc_acc, std::get<messages::EncodedRequest>(inc_enc).context);
    inc_codec.buffered(
        R"({"id":"msg_archive","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"thinking","thinking":"reason","signature":"synthetic-sensitive-signature"},{"type":"redacted_thinking","data":"synthetic-encrypted-state"},{"type":"text","text":"answer"}],"stop_reason":"end_turn","stop_sequence":null,"usage":{"input_tokens":2,"output_tokens":3}})",
        {false, ErrorKind::Truncated});
    inc_codec.finish();
    CHECK(inc_acc.outcome() && std::holds_alternative<Failure>(*inc_acc.outcome()));
    incomplete_req.messages.push_back(
        std::get<Failure>(*inc_acc.outcome()).partial.messages.at(0));
    const SealedRecord sealed_inc = seal_checked(*archive, incomplete_req.messages, "partial");
    incomplete_req.messages =
        open_checked(*archive, sealed_inc.reference, sealed_inc.bytes, "partial");
    CHECK(incomplete_req.messages[1].native && !incomplete_req.messages[1].native->complete());
    CHECK(std::holds_alternative<Error>(messages::encode(d, incomplete_req, false)));

    // Diagnostic duplicate-key document round-trips on non-executable parts, but is refused on executable ToolCall.input.
    auto dup_parsed = json::parse(R"({"x":1,"x":2})");
    CHECK(std::holds_alternative<json::ParseError>(dup_parsed));
    const auto diagnostic = std::make_shared<const json::Document>(
        std::move(std::get<json::ParseError>(dup_parsed).context));
    Message diag_msg{"diagnostic",
                     Role::Assistant,
                     {InvalidToolCall{"bad", "lookup", ToolCallKind::ClientExecuted,
                                      R"({"x":1,"x":2})", InvalidReason::DuplicateKey, "tool_use",
                                      diagnostic},
                      Opaque{"future", diagnostic}},
                     {},
                     diagnostic};
    const SealedRecord sealed_diag = seal_checked(*archive, {diag_msg}, "diagnostic-call");
    const auto restored_diag =
        open_checked(*archive, sealed_diag.reference, sealed_diag.bytes, "diagnostic-call");
    CHECK(restored_diag.size() == 1 &&
          restored_diag[0].wire_output->root().dump() == R"({"x":1,"x":2})");
    Message exec_dup = diag_msg;
    exec_dup.parts = {ToolCall{"bad", "lookup", ToolCallKind::ClientExecuted, diagnostic}};
    const SealedRecord sealed_exec_dup =
        seal_checked(*archive, {exec_dup}, "invalid-executable");
    auto open_exec_dup = archive->open(
        sealed_exec_dup.reference, sealed_exec_dup.bytes, "invalid-executable");
    CHECK(std::holds_alternative<CustodyError>(open_exec_dup));
    CHECK(std::get<CustodyError>(open_exec_dup).error.reason == "archive.custody.corrupt_record");

    // Zero key-source calls across owner_scope, current_epoch, matches_descriptor, seal, and open.
    CHECK(counting.begin_scope_calls == 0);
    CHECK(counting.end_scope_calls == 0);
    CHECK(counting.active_key_id_calls == 0);
    CHECK(counting.wrap_calls == 0);
    CHECK(counting.unwrap_calls == 0);
    CHECK(secrets->load_count("k1") == 0);

    // Extended tool result bounds under custom max_parts = 3.
    HostCustodyLimits part_limits{};
    part_limits.archive.max_parts = 3;
    auto bounded_archive =
        open_custody_checked(header_bytes, *hkdf_source, "owner-anthropic", d, part_limits);
    ToolResult nested_res{"call", ""};
    nested_res.content_parts = {Text{"archive-marker"}, Text{"last"}};
    Message nested_msg{{}, Role::User, {nested_res}};
    const SealedRecord sealed_nested = seal_checked(*bounded_archive, {nested_msg}, "nested");
    const auto restored_nested =
        open_checked(*bounded_archive, sealed_nested.reference, sealed_nested.bytes, "nested");
    check_same_part(nested_msg.parts[0], restored_nested[0].parts[0]);

    Message excessive = nested_msg;
    std::get<ToolResult>(excessive.parts[0]).content_parts.emplace_back(Text{"third"});
    auto excessive_seal = bounded_archive->seal({excessive}, "nested");
    CHECK(std::holds_alternative<CustodyError>(excessive_seal));
    CHECK(std::get<CustodyError>(excessive_seal).error.kind == ErrorKind::InvalidRequest);
    CHECK(std::get<CustodyError>(excessive_seal).error.reason == "archive.custody.invalid_input");

    Message exclusive = nested_msg;
    std::get<ToolResult>(exclusive.parts[0]).content = "legacy";
    auto exclusive_seal = bounded_archive->seal({exclusive}, "nested");
    CHECK(std::holds_alternative<CustodyError>(exclusive_seal));
    CHECK(std::get<CustodyError>(exclusive_seal).error.kind == ErrorKind::InvalidRequest);
    CHECK(std::get<CustodyError>(exclusive_seal).error.reason == "archive.custody.invalid_input");

    for (unsigned mutation = 0; mutation < 3; ++mutation) {
      Message invalid_media_msg = nested_msg;
      auto bad_media =
          Media::image("image/png", std::make_shared<const std::string>("AAEC"));
      if (mutation == 0) bad_media.kind = static_cast<MediaKind>(255);
      if (mutation == 1) bad_media.source = static_cast<MediaSource>(255);
      if (mutation == 2) bad_media.detail = static_cast<ImageDetail>(255);
      std::get<ToolResult>(invalid_media_msg.parts[0]).content_parts = {bad_media};
      const SealedRecord invalid_sealed =
          seal_checked(*bounded_archive, {invalid_media_msg}, "invalid-enum");
      const auto invalid_open = bounded_archive->open(
          invalid_sealed.reference, invalid_sealed.bytes, "invalid-enum");
      CHECK(std::holds_alternative<CustodyError>(invalid_open));
      CHECK(std::get<CustodyError>(invalid_open).error.reason ==
            "archive.custody.corrupt_record");
    }
  }

  // 2. openai.responses: reasoning encrypted_content and wire_output.
  {
    const auto d = provider_test_descriptor("openai.responses");
    auto h_res = create_header(*hkdf_source, "responses-owner", d);
    CHECK(std::holds_alternative<std::string>(h_res));
    auto archive = open_custody_checked(std::get<std::string>(h_res), *hkdf_source, "responses-owner", d);

    responses::Request r;
    r.model = "fixture-model";
    r.account_scope = "fixture-account";
    r.max_output_tokens = 128;
    r.messages.push_back(make_user_message("hello"));
    auto e = responses::encode(d, r, false);
    CHECK(std::holds_alternative<responses::EncodedRequest>(e));
    Accumulator acc;
    responses::Codec codec(
        d, responses::Mode::Buffered, acc, std::get<responses::EncodedRequest>(e).context);
    codec.buffered(
        R"({"id":"resp_archive","object":"response","created_at":1,"model":"fixture-model","status":"completed","output":[{"id":"rs_1","type":"reasoning","status":"completed","summary":[{"type":"summary_text","text":"reason"}],"encrypted_content":"encrypted-native"},{"id":"msg_1","type":"message","status":"completed","role":"assistant","content":[{"type":"output_text","text":"answer","annotations":[]}]}],"usage":{"input_tokens":2,"output_tokens":3,"total_tokens":5},"incomplete_details":null,"error":null})",
        {});
    CHECK(acc.outcome() && std::holds_alternative<Completion>(*acc.outcome()));
    auto m = std::get<Completion>(*acc.outcome()).messages.at(0);
    CHECK(m.native && m.native->complete());
    r.messages.push_back(m);
    r.messages.push_back(make_user_message("next"));

    const auto expected = responses::encode(d, r, false);
    CHECK(std::holds_alternative<responses::EncodedRequest>(expected));
    const SealedRecord sealed = seal_checked(*archive, r.messages, "response-call");
    for (std::string_view marker : {"encrypted-native", "resp_archive", "rs_1", "msg_1", "answer"}) {
      CHECK(sealed.bytes.find(marker) == std::string::npos);
    }

    r.messages = open_checked(*archive, sealed.reference, sealed.bytes, "response-call");
    const auto actual = responses::encode(d, r, false);
    CHECK(std::holds_alternative<responses::EncodedRequest>(actual));
    CHECK(std::get<responses::EncodedRequest>(actual).body ==
          std::get<responses::EncodedRequest>(expected).body);
    check_same_document(m.wire_output, r.messages[1].wire_output);

    r.messages[1].wire_output = parse_json_doc(R"([])");
    CHECK(std::holds_alternative<Error>(responses::encode(d, r, false)));
    auto bad_seal = archive->seal(r.messages, "response-call");
    CHECK(std::holds_alternative<CustodyError>(bad_seal));
    CHECK(std::get<CustodyError>(bad_seal).error.kind == ErrorKind::InvalidRequest);
    CHECK(std::get<CustodyError>(bad_seal).error.reason == "archive.custody.invalid_input");
  }

  // 3. openai.chat: reasoning_details encrypted data and wire_metadata.
  {
    const auto d = provider_test_descriptor("openai.chat", "https://openrouter.ai");
    auto h_res = create_header(*hkdf_source, "chat-owner", d);
    CHECK(std::holds_alternative<std::string>(h_res));
    auto archive = open_custody_checked(std::get<std::string>(h_res), *hkdf_source, "chat-owner", d);

    chat::Request initial;
    initial.model = "fixture-model";
    initial.max_output_tokens = 128;
    initial.messages.push_back({Role::User, "hello"});
    auto e = chat::encode(d, initial, false);
    CHECK(std::holds_alternative<chat::EncodedRequest>(e));
    Accumulator acc;
    chat::Codec codec(
        d, chat::Mode::Buffered, acc, {}, std::get<chat::EncodedRequest>(e).context);
    CHECK(codec.buffered(
        R"({"id":"chat_archive","object":"chat.completion","created":1,"model":"fixture-model","choices":[{"index":0,"message":{"role":"assistant","content":"answer","reasoning":"consider","reasoning_details":[{"type":"reasoning.encrypted","data":"synthetic-encrypted","id":"rd_1","format":"provider-native","index":0}]},"finish_reason":"stop"}],"usage":{"prompt_tokens":2,"completion_tokens":3,"total_tokens":5}})",
        {}));
    CHECK(acc.outcome() && std::holds_alternative<Completion>(*acc.outcome()));
    const auto native = std::get<Completion>(*acc.outcome()).messages.at(0);
    CHECK(native.native && native.native->complete());

    auto follow = initial;
    follow.messages.clear();
    follow.canonical_messages = {make_user_message("hello"), native, make_user_message("continue")};
    const auto expected = chat::encode(d, follow, false);
    CHECK(std::holds_alternative<chat::EncodedRequest>(expected));

    const SealedRecord sealed = seal_checked(*archive, follow.canonical_messages, "chat-call");
    for (std::string_view marker :
         {"synthetic-encrypted", "provider-native", "rd_1", "consider", "answer"}) {
      CHECK(sealed.bytes.find(marker) == std::string::npos);
    }

    follow.canonical_messages = open_checked(*archive, sealed.reference, sealed.bytes, "chat-call");
    const auto actual = chat::encode(d, follow, false);
    CHECK(std::holds_alternative<chat::EncodedRequest>(actual));
    CHECK(std::get<chat::EncodedRequest>(actual).body ==
          std::get<chat::EncodedRequest>(expected).body);

    for (auto& part : follow.canonical_messages[1].parts) {
      if (auto* opaque = std::get_if<Opaque>(&part)) {
        opaque->wire_metadata = parse_json_doc(R"({"type":"reasoning_details","details":[]})");
      }
    }
    CHECK(std::holds_alternative<Error>(chat::encode(d, follow, false)));
    auto bad_seal = archive->seal(follow.canonical_messages, "chat-call");
    CHECK(std::holds_alternative<CustodyError>(bad_seal));
    CHECK(std::get<CustodyError>(bad_seal).error.kind == ErrorKind::InvalidRequest);
    CHECK(std::get<CustodyError>(bad_seal).error.reason == "archive.custody.invalid_input");
  }

  // 4. google.generate (literal and model-routed paths): thoughtSignature and wire_output.
  for (const bool routed : {false, true}) {
    auto admitted_descriptor = descriptor::load(gemini_routed_paths(
        R"({"descriptor_version":1,"revision":1,"id":"archive-gemini","family":"google.generate","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1beta/models/fixture-model:generateContent","streaming":"/v1beta/models/fixture-model:streamGenerateContent?alt=sse"}}})",
        routed));
    CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(admitted_descriptor));
    const auto d = std::get<descriptor::ValidatedDescriptor>(std::move(admitted_descriptor));

    auto h_res = create_header(*hkdf_source, "google-owner", d);
    CHECK(std::holds_alternative<std::string>(h_res));
    auto archive = open_custody_checked(std::get<std::string>(h_res), *hkdf_source, "google-owner", d);

    gemini::Request r;
    r.model = "fixture-model";
    r.account_scope = "fixture-account";
    r.max_output_tokens = 2048;
    r.thinking_budget = 1024;
    r.messages.push_back(make_user_message("question"));
    r.tools.push_back(
        {"lookup", "Find value", parse_json_doc(R"({"type":"object","properties":{"x":{"type":"integer"}}})")});
    auto e = gemini::encode(d, r, false);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(e));
    Accumulator acc;
    gemini::Codec codec(
        d, gemini::Mode::Buffered, acc, std::get<gemini::EncodedRequest>(e).context);
    codec.buffered(
        R"({"modelVersion":"fixture-model","responseId":"generation","candidates":[{"index":0,"content":{"role":"model","parts":[{"text":"Consider","thought":true,"thoughtSignature":"THOUGHT_SIG"},{"functionCall":{"id":"call_owned","name":"lookup","args":{"x":1}},"thoughtSignature":"CALL_SIG"},{"thoughtSignature":"EMPTY_SIG"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":7,"thoughtsTokenCount":3,"totalTokenCount":20}})",
        {});
    CHECK(acc.outcome() && std::holds_alternative<Completion>(*acc.outcome()));
    r.messages.push_back(std::get<Completion>(*acc.outcome()).messages.at(0));
    r.messages.push_back(Message{"", Role::Tool, {ToolResult{"call_owned", R"({"result":1})"}}});

    const auto expected = gemini::encode(d, r, false);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(expected));
    const SealedRecord sealed = seal_checked(*archive, r.messages, "gemini-tool-call");
    for (std::string_view marker : {"THOUGHT_SIG", "CALL_SIG", "EMPTY_SIG", "call_owned", "Consider"}) {
      CHECK(sealed.bytes.find(marker) == std::string::npos);
    }

    r.messages = open_checked(*archive, sealed.reference, sealed.bytes, "gemini-tool-call");
    const auto actual = gemini::encode(d, r, false);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(actual));
    CHECK(std::get<gemini::EncodedRequest>(actual).body ==
          std::get<gemini::EncodedRequest>(expected).body);

    std::get<Thinking>(r.messages[1].parts[0]).signature = "edited";
    CHECK(std::holds_alternative<Error>(gemini::encode(d, r, false)));
    auto bad_seal = archive->seal(r.messages, "gemini-tool-call");
    CHECK(std::holds_alternative<CustodyError>(bad_seal));
    CHECK(std::get<CustodyError>(bad_seal).error.kind == ErrorKind::InvalidRequest);
    CHECK(std::get<CustodyError>(bad_seal).error.reason == "archive.custody.invalid_input");
  }

  // 5. google.interactions: thought signature and step wire_output.
  {
    auto admitted_descriptor = descriptor::load(
        R"({"descriptor_version":1,"revision":1,"id":"archive-interactions","family":"google.interactions","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1beta/interactions","streaming":"/v1beta/interactions"}}})");
    CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(admitted_descriptor));
    const auto d = std::get<descriptor::ValidatedDescriptor>(std::move(admitted_descriptor));

    auto h_res = create_header(*hkdf_source, "google-owner", d);
    CHECK(std::holds_alternative<std::string>(h_res));
    auto archive = open_custody_checked(std::get<std::string>(h_res), *hkdf_source, "google-owner", d);

    interactions::Request r;
    r.model = "fixture-model";
    r.account_scope = "fixture-account";
    r.max_output_tokens = 128;
    r.thinking_level = "low";
    r.messages.push_back(make_user_message("question"));
    r.tools.push_back(
        {"lookup", "Find", parse_json_doc(R"({"type":"object","properties":{"x":{"type":"integer"}}})")});
    auto e = interactions::encode(d, r, false);
    CHECK(std::holds_alternative<interactions::EncodedRequest>(e));
    Accumulator acc;
    interactions::Codec codec(
        d, interactions::Mode::Buffered, acc, std::get<interactions::EncodedRequest>(e).context);
    codec.buffered(
        R"({"id":"i1","model":"fixture-model","status":"requires_action","steps":[{"type":"thought","signature":"FINAL_OPAQUE","summary":[{"type":"text","text":"first"},{"type":"text","text":"second"}],"extra":"retain"},{"type":"function_call","id":"call1","name":"lookup","arguments":{"x":1}}],"usage":{"total_input_tokens":100,"total_output_tokens":25,"total_thought_tokens":22,"total_tokens":147,"total_tool_use_tokens":50}})",
        {});
    CHECK(acc.outcome() && std::holds_alternative<Completion>(*acc.outcome()));
    r.messages.push_back(std::get<Completion>(*acc.outcome()).messages.at(0));
    r.messages.push_back(Message{"", Role::Tool, {ToolResult{"call1", "one"}}});

    const auto expected = interactions::encode(d, r, false);
    CHECK(std::holds_alternative<interactions::EncodedRequest>(expected));
    const SealedRecord sealed = seal_checked(*archive, r.messages, "interactions-tool-call");
    for (std::string_view marker : {"FINAL_OPAQUE", "retain", "call1", "first", "second"}) {
      CHECK(sealed.bytes.find(marker) == std::string::npos);
    }

    r.messages = open_checked(*archive, sealed.reference, sealed.bytes, "interactions-tool-call");
    const auto actual = interactions::encode(d, r, false);
    CHECK(std::holds_alternative<interactions::EncodedRequest>(actual));
    CHECK(std::get<interactions::EncodedRequest>(actual).body ==
          std::get<interactions::EncodedRequest>(expected).body);

    std::get<Thought>(r.messages[1].parts[0]).signature = "edited";
    CHECK(std::holds_alternative<Error>(interactions::encode(d, r, false)));
    auto bad_seal = archive->seal(r.messages, "interactions-tool-call");
    CHECK(std::holds_alternative<CustodyError>(bad_seal));
    CHECK(std::get<CustodyError>(bad_seal).error.kind == ErrorKind::InvalidRequest);
    CHECK(std::get<CustodyError>(bad_seal).error.reason == "archive.custody.invalid_input");
  }
}

void test_custody_record_nonce_uniqueness() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  secrets->set_secret("k2", std::string(32, '\x22'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto valid_desc = default_test_descriptor();
  const std::vector<Message> messages = {make_user_message("same-message")};

  auto h1_res = create_header(*key_source, "owner-nonce", valid_desc);
  CHECK(std::holds_alternative<std::string>(h1_res));
  const std::string& h1_bytes = std::get<std::string>(h1_res);

  auto archive_a = open_custody_checked(h1_bytes, *key_source, "owner-nonce", valid_desc);
  auto archive_b = open_custody_checked(h1_bytes, *key_source, "owner-nonce", valid_desc);

  std::set<std::string> nonces;
  std::set<std::string> envelopes;
  std::set<std::string> references;

  auto record_and_check = [&](const SealedRecord& rec) {
    CHECK(rec.bytes.size() >= 48);
    const std::string nonce = rec.bytes.substr(16, 24);
    CHECK(nonces.insert(nonce).second);
    CHECK(envelopes.insert(rec.bytes).second);
    CHECK(references.insert(rec.reference).second);
  };

  // 1,000 seals of the same messages and binding in one epoch.
  for (std::size_t i = 0; i < 1000; ++i) {
    record_and_check(seal_checked(*archive_a, messages, "same-binding"));
  }

  // Seals from a second archive instance opened from the same header.
  for (std::size_t i = 0; i < 32; ++i) {
    record_and_check(seal_checked(*archive_b, messages, "same-binding"));
  }

  // Seals across 4 rotated epochs (epochs 1, 2, 3, 4).
  std::string current_h = h1_bytes;
  for (std::uint64_t target_epoch = 2; target_epoch <= 4; ++target_epoch) {
    auto rot_res = rotate_header(current_h, *key_source, (target_epoch % 2 == 0) ? "k2" : "k1",
                                 "owner-nonce", valid_desc);
    CHECK(std::holds_alternative<std::string>(rot_res));
    current_h = std::get<std::string>(std::move(rot_res));
    auto rotated_archive = open_custody_checked(current_h, *key_source, "owner-nonce", valid_desc);
    CHECK(rotated_archive->current_epoch() == target_epoch);
    for (std::size_t i = 0; i < 16; ++i) {
      record_and_check(seal_checked(*rotated_archive, messages, "same-binding"));
    }
  }

  CHECK(nonces.size() == 1000 + 32 + 3 * 16);
  CHECK(envelopes.size() == nonces.size());
  CHECK(references.size() == nonces.size());
}

// KeySource that wraps and unwraps under a fixed KEK (independent of store_id/owner/descriptor in epoch_aad),
// isolating Record AAD single-field checks so two headers with different store_id, owner_scope, or descriptor
// can hold the exact same 32-byte DEK.
class ConstantKekKeySource final : public KeySource {
 public:
  explicit ConstantKekKeySource(std::string key_id, std::string kek_bytes)
      : key_id_(std::move(key_id)), kek_bytes_(std::move(kek_bytes)) {}

  CustodyResult<std::string> active_key_id() override { return key_id_; }

  CustodyResult<std::string> wrap_dek(std::string_view /*key_id*/,
                                      std::string_view epoch_aad,
                                      const DataKeyBuffer& dek) override {
    DataKeyBuffer kek;
    if (auto fault = kek.assign_exact(kek_bytes_)) {
      return *fault;
    }
    std::array<unsigned char, 24> nonce{};
    if (!custody_ops::random_bytes(nonce.data(), nonce.size())) {
      return CustodyFault::InvalidKeyMaterial;
    }
    const std::string_view pt(reinterpret_cast<const char*>(dek.data()), dek.size());
    auto enc = custody_ops::aead_xchacha20poly1305_encrypt(kek, nonce, epoch_aad, pt);
    if (const auto* err = std::get_if<CustodyError>(&enc)) {
      (void)err;
      return CustodyFault::InvalidKeyMaterial;
    }
    std::string out(reinterpret_cast<const char*>(nonce.data()), nonce.size());
    out.append(std::get<std::string>(enc));
    return out;
  }

  std::optional<CustodyFault> unwrap_dek(std::string_view /*key_id*/,
                                         std::string_view epoch_aad,
                                         std::string_view wrapped_dek,
                                         DataKeyBuffer& dek_out) override {
    if (wrapped_dek.size() != 72) {
      dek_out.wipe();
      return CustodyFault::Tampered;
    }
    DataKeyBuffer kek;
    if (auto fault = kek.assign_exact(kek_bytes_)) {
      dek_out.wipe();
      return *fault;
    }
    std::array<unsigned char, 24> nonce{};
    std::memcpy(nonce.data(), wrapped_dek.data(), nonce.size());
    auto dec = custody_ops::aead_xchacha20poly1305_decrypt(
        kek, nonce, epoch_aad, wrapped_dek.substr(24));
    if (const auto* err = std::get_if<CustodyError>(&dec)) {
      (void)err;
      dek_out.wipe();
      return CustodyFault::Tampered;
    }
    const auto& pt = std::get<CleansedBuffer>(dec);
    return dek_out.assign_exact(pt.view());
  }

 private:
  std::string key_id_;
  std::string kek_bytes_;
};

void test_record_aad_single_field_binding() {
  ConstantKekKeySource fixed_kek_source("k1", std::string(32, '\x42'));
  const auto desc_a = default_test_descriptor();
  const auto desc_b = alternate_test_descriptor();
  const std::vector<Message> messages = {make_user_message("aad-bound-payload")};

  const std::string store_a(32, '\x11');
  const std::string store_b(32, '\x22');
  const std::string shared_dek(32, '\x77');
  const std::string wrap_nonce(24, '\x88');

  auto create_pinned_header = [&](const std::string& store_id_bytes,
                                  const std::string& dek_bytes,
                                  std::string_view owner,
                                  const descriptor::ValidatedDescriptor& desc) {
    custody_ops::reset_random_seam();
    custody_ops::override_next_random_bytes(store_id_bytes);
    custody_ops::override_next_random_bytes(dek_bytes);
    custody_ops::override_next_random_bytes(wrap_nonce);
    auto h = create_header(fixed_kek_source, owner, desc);
    custody_ops::reset_random_seam();
    CHECK(std::holds_alternative<std::string>(h));
    return std::get<std::string>(std::move(h));
  };

  // Base archive: store_a, "owner-a", desc_a, epoch 1, DEK = shared_dek.
  const std::string h_base = create_pinned_header(store_a, shared_dek, "owner-a", desc_a);
  auto archive_base = open_custody_checked(h_base, fixed_kek_source, "owner-a", desc_a);
  const SealedRecord sealed_e1 = seal_checked(*archive_base, messages, "binding-a");
  CHECK(open_checked(*archive_base, sealed_e1.reference, sealed_e1.bytes, "binding-a").size() == 1);

  auto check_tampered = [&](const NativeArchive::Opened& opened, std::string_view expected_store_hex) {
    CHECK(std::holds_alternative<CustodyError>(opened));
    const auto& err = std::get<CustodyError>(opened);
    CHECK(err.error.kind == ErrorKind::Permission);
    CHECK(err.error.reason == "archive.custody.tampered");
    CHECK(err.store_id == expected_store_hex);
  };

  // 1. Different store_id only (same owner_scope, same descriptor, same DEK).
  {
    const std::string h_other_store = create_pinned_header(store_b, shared_dek, "owner-a", desc_a);
    auto archive_other_store = open_custody_checked(h_other_store, fixed_kek_source, "owner-a", desc_a);
    check_tampered(archive_other_store->open(sealed_e1.reference, sealed_e1.bytes, "binding-a"),
                   to_hex(store_b));
  }

  // 2. Different owner_scope only (same store_id, same descriptor, same DEK).
  {
    const std::string h_other_owner = create_pinned_header(store_a, shared_dek, "owner-b", desc_a);
    auto archive_other_owner = open_custody_checked(h_other_owner, fixed_kek_source, "owner-b", desc_a);
    check_tampered(archive_other_owner->open(sealed_e1.reference, sealed_e1.bytes, "binding-a"),
                   to_hex(store_a));
  }

  // 3. Different descriptor_identity only (same store_id, same owner_scope, same DEK).
  {
    const std::string h_other_desc = create_pinned_header(store_a, shared_dek, "owner-a", desc_b);
    auto archive_other_desc = open_custody_checked(h_other_desc, fixed_kek_source, "owner-a", desc_b);
    check_tampered(archive_other_desc->open(sealed_e1.reference, sealed_e1.bytes, "binding-a"),
                   to_hex(store_a));
  }

  // 4. Different binding only (same archive).
  {
    check_tampered(archive_base->open(sealed_e1.reference, sealed_e1.bytes, "binding-b"),
                   to_hex(store_a));
    check_tampered(archive_base->open(sealed_e1.reference, sealed_e1.bytes, ""),
                   to_hex(store_a));
  }

  // 5. Envelope's header_epoch rewritten to another existing epoch (even when both epochs hold the same DEK,
  // and when they hold distinct DEKs), with the reference recomputed so the reference check passes.
  for (const std::string& epoch_2_dek : {shared_dek, std::string(32, '\x99')}) {
    custody_ops::reset_random_seam();
    custody_ops::override_next_random_bytes(epoch_2_dek);
    custody_ops::override_next_random_bytes(wrap_nonce);
    auto h2_res = rotate_header(h_base, fixed_kek_source, {}, "owner-a", desc_a);
    custody_ops::reset_random_seam();
    CHECK(std::holds_alternative<std::string>(h2_res));
    auto archive_e2 = open_custody_checked(std::get<std::string>(h2_res), fixed_kek_source, "owner-a", desc_a);
    CHECK(archive_e2->current_epoch() == 2);

    const SealedRecord sealed_e2 = seal_checked(*archive_e2, messages, "binding-a");
    std::string rewritten_to_e1 = sealed_e2.bytes;
    write_be64(rewritten_to_e1.data() + 8, 1);
    const std::string recomputed_ref_e1 = archive_internal::compute_record_reference(rewritten_to_e1);
    check_tampered(archive_e2->open(recomputed_ref_e1, rewritten_to_e1, "binding-a"),
                   to_hex(store_a));

    std::string rewritten_to_e2 = sealed_e1.bytes;
    write_be64(rewritten_to_e2.data() + 8, 2);
    const std::string recomputed_ref_e2 = archive_internal::compute_record_reference(rewritten_to_e2);
    check_tampered(archive_e2->open(recomputed_ref_e2, rewritten_to_e2, "binding-a"),
                   to_hex(store_a));

    // Rewritten to absent epochs (0 and 3) with reference recomputed -> Tampered at epoch lookup.
    for (std::uint64_t absent_epoch : {std::uint64_t{0}, std::uint64_t{3}}) {
      std::string rewritten_absent = sealed_e2.bytes;
      write_be64(rewritten_absent.data() + 8, absent_epoch);
      const std::string recomputed_absent = archive_internal::compute_record_reference(rewritten_absent);
      check_tampered(archive_e2->open(recomputed_absent, rewritten_absent, "binding-a"),
                     to_hex(store_a));
    }
  }
}

void test_rotated_epochs_and_zero_source_calls_on_seal_and_open() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  secrets->set_secret("k2", std::string(32, '\x22'));
  secrets->set_secret("k3", std::string(32, '\x33'));
  secrets->set_secret("k4", std::string(32, '\x44'));
  auto hkdf_source = make_hkdf_key_source(secrets);
  CountingKeySource counting(hkdf_source);
  const auto valid_desc = default_test_descriptor();

  // Epoch 1: create and seal record 1.
  auto h1_res = create_header(counting, "owner-rotated", valid_desc);
  CHECK(std::holds_alternative<std::string>(h1_res));
  const std::string& h1_bytes = std::get<std::string>(h1_res);
  SealedRecord r1;
  {
    auto a1 = open_custody_checked(h1_bytes, counting, "owner-rotated", valid_desc);
    CHECK(a1->current_epoch() == 1);
    r1 = seal_checked(*a1, {make_user_message("msg-epoch-1")}, "bind-1");
  }

  // Rotation 1 -> Epoch 2: seal record 2.
  auto h2_res = rotate_header(h1_bytes, counting, "k2", "owner-rotated", valid_desc);
  CHECK(std::holds_alternative<std::string>(h2_res));
  const std::string& h2_bytes = std::get<std::string>(h2_res);
  SealedRecord r2;
  {
    auto a2 = open_custody_checked(h2_bytes, counting, "owner-rotated", valid_desc);
    CHECK(a2->current_epoch() == 2);
    r2 = seal_checked(*a2, {make_user_message("msg-epoch-2")}, "bind-2");
  }

  // Rotation 2 -> Epoch 3: seal record 3.
  auto h3_res = rotate_header(h2_bytes, counting, "k3", "owner-rotated", valid_desc);
  CHECK(std::holds_alternative<std::string>(h3_res));
  const std::string& h3_bytes = std::get<std::string>(h3_res);
  SealedRecord r3;
  {
    auto a3 = open_custody_checked(h3_bytes, counting, "owner-rotated", valid_desc);
    CHECK(a3->current_epoch() == 3);
    r3 = seal_checked(*a3, {make_user_message("msg-epoch-3")}, "bind-3");
  }

  // Rotation 3 -> Epoch 4 (three rotations total).
  auto h4_res = rotate_header(h3_bytes, counting, "k4", "owner-rotated", valid_desc);
  CHECK(std::holds_alternative<std::string>(h4_res));
  const std::string& h4_bytes = std::get<std::string>(h4_res);

  // Open the archive once after three rotations.
  counting.reset_counts();
  secrets->reset_counts();
  custody_ops::reset_random_seam();
  CHECK(custody_ops::live_key_allocation_sizes().empty());

  auto archive = open_custody_checked(h4_bytes, counting, "owner-rotated", valid_desc);
  CHECK(archive->current_epoch() == 4);
  CHECK(counting.begin_scope_calls == 1);
  CHECK(counting.end_scope_calls == 1);
  CHECK(counting.active_key_id_calls == 0);
  CHECK(counting.wrap_calls == 0);
  CHECK(counting.unwrap_calls == 4);
  CHECK(secrets->load_count("k1") == 1);
  CHECK(secrets->load_count("k2") == 1);
  CHECK(secrets->load_count("k3") == 1);
  CHECK(secrets->load_count("k4") == 1);

  // All 4 epoch DEKs live in one single sodium_malloc block of 4 * 32 = 128 bytes.
  CHECK(custody_ops::live_key_allocation_sizes() == (std::vector<std::size_t>{4 * 32}));

  // Reset source counters and open records of epochs 3, 1, 2 in that order, plus seal and open in epoch 4.
  counting.reset_counts();
  secrets->reset_counts();

  const auto opened_3 = open_checked(*archive, r3.reference, r3.bytes, "bind-3");
  CHECK(opened_3.size() == 1 && std::get<Text>(opened_3[0].parts[0]).value == "msg-epoch-3");

  const auto opened_1 = open_checked(*archive, r1.reference, r1.bytes, "bind-1");
  CHECK(opened_1.size() == 1 && std::get<Text>(opened_1[0].parts[0]).value == "msg-epoch-1");

  const auto opened_2 = open_checked(*archive, r2.reference, r2.bytes, "bind-2");
  CHECK(opened_2.size() == 1 && std::get<Text>(opened_2[0].parts[0]).value == "msg-epoch-2");

  const SealedRecord r4 = seal_checked(*archive, {make_user_message("msg-epoch-4")}, "bind-4");
  const auto opened_4 = open_checked(*archive, r4.reference, r4.bytes, "bind-4");
  CHECK(opened_4.size() == 1 && std::get<Text>(opened_4[0].parts[0]).value == "msg-epoch-4");

  CHECK(archive->owner_scope() == "owner-rotated");
  CHECK(archive->current_epoch() == 4);
  CHECK(archive->matches_descriptor(valid_desc));

  // Zero key-source calls during every seal, open, and accessor call.
  CHECK(counting.begin_scope_calls == 0);
  CHECK(counting.end_scope_calls == 0);
  CHECK(counting.active_key_id_calls == 0);
  CHECK(counting.wrap_calls == 0);
  CHECK(counting.unwrap_calls == 0);
  CHECK(secrets->load_count("k1") == 0);
  CHECK(secrets->load_count("k2") == 0);
  CHECK(secrets->load_count("k3") == 0);
  CHECK(secrets->load_count("k4") == 0);

  archive.reset();
  CHECK(custody_ops::live_key_allocation_sizes().empty());

  // End-to-end key retirement: retire k1, k2, and k3 into k4, delete k1..k3 from the secret source,
  // open_custody with a fresh source holding only k4, and open records of epochs 3, 1, 2 in that order.
  auto ret1 = retire_key_in_header(h4_bytes, "k1", *hkdf_source, "owner-rotated", valid_desc);
  CHECK(std::holds_alternative<std::string>(ret1));
  auto ret2 = retire_key_in_header(std::get<std::string>(ret1), "k2", *hkdf_source, "owner-rotated", valid_desc);
  CHECK(std::holds_alternative<std::string>(ret2));
  auto ret3 = retire_key_in_header(std::get<std::string>(ret2), "k3", *hkdf_source, "owner-rotated", valid_desc);
  CHECK(std::holds_alternative<std::string>(ret3));

  auto k4_only_secrets = std::make_shared<FixedSecretSource>("k4");
  k4_only_secrets->set_secret("k4", std::string(32, '\x44'));
  auto k4_only_source = make_hkdf_key_source(k4_only_secrets);

  auto retired_archive = open_custody_checked(
      std::get<std::string>(ret3), *k4_only_source, "owner-rotated", valid_desc);
  CHECK(k4_only_secrets->load_count("k4") == 1);
  CHECK(std::get<Text>(open_checked(*retired_archive, r3.reference, r3.bytes, "bind-3")[0].parts[0]).value ==
        "msg-epoch-3");
  CHECK(std::get<Text>(open_checked(*retired_archive, r1.reference, r1.bytes, "bind-1")[0].parts[0]).value ==
        "msg-epoch-1");
  CHECK(std::get<Text>(open_checked(*retired_archive, r2.reference, r2.bytes, "bind-2")[0].parts[0]).value ==
        "msg-epoch-2");
}

void record_allocation(std::size_t capacity, void* context) noexcept {
  auto* capacities = static_cast<std::vector<std::size_t>*>(context);
  capacities->push_back(capacity);
}

void test_record_tampering_and_truncation_bounds() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto valid_desc = default_test_descriptor();

  // The byte limit also bounds the descriptor binding and the header, so it must leave room for them.
  HostCustodyLimits custom_limits{};
  custom_limits.archive.max_bytes = 4096;

  auto h_res = create_header(*key_source, "owner-tamper", valid_desc, custom_limits);
  CHECK(std::holds_alternative<std::string>(h_res));
  const std::string& header_bytes = std::get<std::string>(h_res);
  const StoreHeader parsed_h = std::get<StoreHeader>(parse_header(header_bytes, custom_limits));
  const std::string store_id_hex = to_hex(parsed_h.store_id.data(), parsed_h.store_id.size());

  auto archive = open_custody_checked(header_bytes, *key_source, "owner-tamper", valid_desc, custom_limits);
  const SealedRecord sealed =
      seal_checked(*archive, {make_user_message("tamper-target-message")}, "tamper-bind");
  CHECK(sealed.bytes.size() > 48 + 16);

  auto check_fault = [&](const NativeArchive::Opened& res, std::string_view expected_reason) {
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.kind == ErrorKind::Permission);
    CHECK(err.error.reason == expected_reason);
    CHECK(err.store_id == store_id_hex);
    CHECK(err.key_id.empty());
  };

  // 1. Flip one bit in the ciphertext body (both with original reference and with recomputed reference).
  {
    std::string mutated = sealed.bytes;
    mutated[48] ^= 0x01;
    check_fault(archive->open(sealed.reference, mutated, "tamper-bind"), "archive.custody.tampered");
    const std::string recomputed_ref = archive_internal::compute_record_reference(mutated);
    check_fault(archive->open(recomputed_ref, mutated, "tamper-bind"), "archive.custody.tampered");
  }

  // 2. Flip one bit in the 16-byte AEAD tag (both with original reference and with recomputed reference).
  {
    std::string mutated = sealed.bytes;
    mutated.back() ^= 0x01;
    check_fault(archive->open(sealed.reference, mutated, "tamper-bind"), "archive.custody.tampered");
    const std::string recomputed_ref = archive_internal::compute_record_reference(mutated);
    check_fault(archive->open(recomputed_ref, mutated, "tamper-bind"), "archive.custody.tampered");
  }

  // 3. Flip one bit in the 24-byte nonce (both with original reference and with recomputed reference).
  {
    std::string mutated = sealed.bytes;
    mutated[16] ^= 0x01;
    check_fault(archive->open(sealed.reference, mutated, "tamper-bind"), "archive.custody.tampered");
    const std::string recomputed_ref = archive_internal::compute_record_reference(mutated);
    check_fault(archive->open(recomputed_ref, mutated, "tamper-bind"), "archive.custody.tampered");
  }

  // 4. Flip one nibble in the reference.
  {
    std::string bad_ref = sealed.reference;
    bad_ref.back() = (bad_ref.back() == '0') ? '1' : '0';
    check_fault(archive->open(bad_ref, sealed.bytes, "tamper-bind"), "archive.custody.tampered");
  }

  // 5. Truncations of the ciphertext to 0, 1, 15, 16, 23 bytes, and over-limit lengths, each with
  // the reference recomputed over the truncated/mutated bytes so the length check refuses it with
  // CorruptRecord and the allocation observer sees zero buffer allocations.
  std::vector<std::size_t> observed_allocations;
  custody_ops::set_allocation_observer(&record_allocation, &observed_allocations);
  try {
    for (std::size_t trunc_len : {std::size_t{0}, std::size_t{1}, std::size_t{15}, std::size_t{16},
                                  std::size_t{23}}) {
      std::string truncated = sealed.bytes.substr(0, 48 + trunc_len);
      write_be64(truncated.data() + 40, trunc_len);
      const std::string recomputed_ref = archive_internal::compute_record_reference(truncated);
      observed_allocations.clear();
      check_fault(archive->open(recomputed_ref, truncated, "tamper-bind"),
                  "archive.custody.corrupt_record");
      CHECK(observed_allocations.empty());
    }

    // Over-limit length: max_bytes + ABYTES (16) + 1 bytes (both with full payload and header-only claim).
    const std::size_t over_limit_len = custom_limits.archive.max_bytes + 16 + 1;
    {
      std::string over_full = sealed.bytes.substr(0, 48) + std::string(over_limit_len, '\xaa');
      write_be64(over_full.data() + 40, over_limit_len);
      const std::string recomputed_ref = archive_internal::compute_record_reference(over_full);
      observed_allocations.clear();
      check_fault(archive->open(recomputed_ref, over_full, "tamper-bind"),
                  "archive.custody.corrupt_record");
      CHECK(observed_allocations.empty());
    }
    for (std::uint64_t claimed_huge :
         {static_cast<std::uint64_t>(over_limit_len), std::uint64_t{1024 * 1024},
          std::uint64_t{0xffffffffffffffffULL}}) {
      std::string over_claim = sealed.bytes;
      write_be64(over_claim.data() + 40, claimed_huge);
      const std::string recomputed_ref = archive_internal::compute_record_reference(over_claim);
      observed_allocations.clear();
      check_fault(archive->open(recomputed_ref, over_claim, "tamper-bind"),
                  "archive.custody.corrupt_record");
      CHECK(observed_allocations.empty());
    }

    // Wrong record format version and trailing bytes after ciphertext (with reference recomputed).
    {
      std::string bad_ver = sealed.bytes;
      write_be64(bad_ver.data(), 2);
      const std::string recomputed_ref = archive_internal::compute_record_reference(bad_ver);
      observed_allocations.clear();
      check_fault(archive->open(recomputed_ref, bad_ver, "tamper-bind"),
                  "archive.custody.corrupt_record");
      CHECK(observed_allocations.empty());
    }
    {
      std::string trailing = sealed.bytes + std::string(1, '\x00');
      const std::string recomputed_ref = archive_internal::compute_record_reference(trailing);
      observed_allocations.clear();
      check_fault(archive->open(recomputed_ref, trailing, "tamper-bind"),
                  "archive.custody.corrupt_record");
      CHECK(observed_allocations.empty());
    }

    // A 1 MiB input under a 4096-byte limit is refused with CorruptRecord before reference hashing,
    // without the allocation observer seeing any buffer allocation.
    {
      const std::string one_mib_input(1024 * 1024, '\xaa');
      observed_allocations.clear();
      check_fault(archive->open(sealed.reference, one_mib_input, "tamper-bind"),
                  "archive.custody.corrupt_record");
      CHECK(observed_allocations.empty());
    }
  } catch (...) {
    custody_ops::set_allocation_observer(nullptr, nullptr);
    throw;
  }
  custody_ops::set_allocation_observer(nullptr, nullptr);
}

void test_record_plaintext_residue_zeroed_on_release() {
  std::array<unsigned char, 32> needle_bytes{};
  CHECK(crypto::random_bytes(needle_bytes.data(), needle_bytes.size()));
  const std::string needle_hex = to_hex(needle_bytes.data(), needle_bytes.size());
  const std::string needle_raw(reinterpret_cast<const char*>(needle_bytes.data()),
                               needle_bytes.size());

  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto valid_desc = default_test_descriptor();

  auto h_res = create_header(*key_source, "owner-residue", valid_desc);
  CHECK(std::holds_alternative<std::string>(h_res));
  auto archive =
      open_custody_checked(std::get<std::string>(h_res), *key_source, "owner-residue", valid_desc);

  Message msg{"id-residue",
              Role::Assistant,
              {Text{needle_raw + ":" + needle_hex},
               Thinking{"thought-" + needle_hex, needle_hex}},
              {},
              parse_json_doc("{\"needle\":" + json::quote(needle_hex) + "}")};

  ObserverStats stats;
  custody_ops::set_release_observer(&record_release, &stats);
  try {
    // 1. seal: releases plaintext buffer with all bytes zeroed.
    stats = {};
    const SealedRecord sealed = seal_checked(*archive, {msg}, "residue-bind");
    CHECK(stats.releases >= 1);
    CHECK(stats.all_zero_every_time);
    CHECK(sealed.bytes.find(needle_raw) == std::string::npos);
    CHECK(sealed.bytes.find(needle_hex) == std::string::npos);

    // 2. open (success): releases decrypted plaintext buffer with all bytes zeroed.
    stats = {};
    const auto opened = open_checked(*archive, sealed.reference, sealed.bytes, "residue-bind");
    CHECK(opened.size() == 1);
    CHECK(stats.releases >= 1);
    CHECK(stats.all_zero_every_time);

    // 3. open (AEAD decrypt failure): releases plaintext buffer with all bytes zeroed.
    stats = {};
    std::string tampered = sealed.bytes;
    tampered[48] ^= 0x01;
    const std::string tampered_ref = archive_internal::compute_record_reference(tampered);
    const auto failed_open = archive->open(tampered_ref, tampered, "residue-bind");
    CHECK(std::holds_alternative<CustodyError>(failed_open));
    CHECK(std::get<CustodyError>(failed_open).error.reason == "archive.custody.tampered");
    CHECK(stats.releases >= 1);
    CHECK(stats.all_zero_every_time);
  } catch (...) {
    custody_ops::set_release_observer(nullptr, nullptr);
    throw;
  }
  custody_ops::set_release_observer(nullptr, nullptr);
}

void test_open_custody_refusals() {
  const std::string special_kid("k2\x00\n\"\\id", 8);
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  secrets->set_secret(special_kid, std::string(32, '\x22'));
  secrets->set_secret("k3", std::string(32, '\x33'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto valid_desc = default_test_descriptor();
  const auto other_desc = alternate_test_descriptor();

  auto h1_res = create_header(*key_source, "owner-open", valid_desc);
  CHECK(std::holds_alternative<std::string>(h1_res));
  auto h2_res = rotate_header(std::get<std::string>(h1_res), *key_source, special_kid, "owner-open", valid_desc);
  CHECK(std::holds_alternative<std::string>(h2_res));
  auto h3_res = rotate_header(std::get<std::string>(h2_res), *key_source, "k3", "owner-open", valid_desc);
  CHECK(std::holds_alternative<std::string>(h3_res));
  const std::string& h3_bytes = std::get<std::string>(h3_res);

  const StoreHeader h3 = std::get<StoreHeader>(parse_header(h3_bytes));
  const std::string store_id_hex = to_hex(h3.store_id.data(), h3.store_id.size());
  const std::string tag_bytes = h3_bytes.substr(h3_bytes.size() - 32);

  auto check_refusal = [&](const NativeArchive::CustodyActivation& res,
                           ErrorKind expected_kind,
                           std::string_view expected_reason,
                           std::string_view expected_key_id = {}) {
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.kind == expected_kind);
    CHECK(err.error.reason == expected_reason);
    CHECK(err.store_id == store_id_hex);
    CHECK(err.key_id == expected_key_id);
    CHECK(custody_ops::live_key_allocation_sizes().empty());
  };

  // 1. Header with a zeroed tag -> Tampered.
  {
    std::string zeroed_tag = h3_bytes.substr(0, h3_bytes.size() - 32) + std::string(32, '\0');
    check_refusal(NativeArchive::open_custody(zeroed_tag, *key_source, "owner-open", valid_desc),
                  ErrorKind::Permission, "archive.custody.tampered");
  }

  // 2. Header rewound to an earlier epoch with a stale tag -> Tampered.
  {
    std::vector<EpochKeyEntry> two_epochs = {h3.epochs[0], h3.epochs[1]};
    const std::string rewound =
        build_raw_header(custody_format::header_body_domain, 1, h3.store_id, h3.owner_scope,
                         h3.descriptor_identity, 2, two_epochs, tag_bytes);
    check_refusal(NativeArchive::open_custody(rewound, *key_source, "owner-open", valid_desc),
                  ErrorKind::Permission, "archive.custody.tampered");
  }

  // 3. Header with a flipped tag byte -> Tampered.
  {
    std::string flipped_tag = h3_bytes;
    flipped_tag.back() ^= 0x01;
    check_refusal(NativeArchive::open_custody(flipped_tag, *key_source, "owner-open", valid_desc),
                  ErrorKind::Permission, "archive.custody.tampered");
  }

  // 4. Header with a mutated earlier epoch key_id ("missing-key") or flipped wrapped_dek byte is rejected by
  // verify_header_tag with Tampered BEFORE attempting to unwrap epoch 1's key; structural splices (rewound
  // current_epoch with all 3 epochs, dropped middle epoch, duplicated epoch, reordered epochs) are refused
  // with CorruptHeader.
  {
    StoreHeader mutated = h3;
    mutated.epochs[0].wrapping_key_id = "missing-key";
    const std::string mutated_bytes =
        build_raw_header(custody_format::header_body_domain, 1, mutated.store_id, mutated.owner_scope,
                         mutated.descriptor_identity, mutated.current_epoch, mutated.epochs, tag_bytes);
    check_refusal(NativeArchive::open_custody(mutated_bytes, *key_source, "owner-open", valid_desc),
                  ErrorKind::Permission, "archive.custody.tampered");

    StoreHeader dek_mut = h3;
    dek_mut.epochs[0].wrapped_dek[0] ^= 0x01;
    const std::string dek_mut_bytes =
        build_raw_header(custody_format::header_body_domain, 1, dek_mut.store_id, dek_mut.owner_scope,
                         dek_mut.descriptor_identity, dek_mut.current_epoch, dek_mut.epochs, tag_bytes);
    check_refusal(NativeArchive::open_custody(dek_mut_bytes, *key_source, "owner-open", valid_desc),
                  ErrorKind::Permission, "archive.custody.tampered");

    StoreHeader bad_rewound = h3;
    bad_rewound.current_epoch = 2;
    check_refusal(NativeArchive::open_custody(
                      build_raw_header(custody_format::header_body_domain, 1, bad_rewound.store_id,
                                       bad_rewound.owner_scope, bad_rewound.descriptor_identity,
                                       bad_rewound.current_epoch, bad_rewound.epochs, tag_bytes),
                      *key_source, "owner-open", valid_desc),
                  ErrorKind::Permission, "archive.custody.corrupt_header");

    StoreHeader bad_dropped = h3;
    bad_dropped.epochs = {h3.epochs[0], h3.epochs[2]};
    check_refusal(NativeArchive::open_custody(
                      build_raw_header(custody_format::header_body_domain, 1, bad_dropped.store_id,
                                       bad_dropped.owner_scope, bad_dropped.descriptor_identity,
                                       bad_dropped.current_epoch, bad_dropped.epochs, tag_bytes),
                      *key_source, "owner-open", valid_desc),
                  ErrorKind::Permission, "archive.custody.corrupt_header");

    StoreHeader bad_dup = h3;
    bad_dup.epochs[2].epoch = 2;
    check_refusal(NativeArchive::open_custody(
                      build_raw_header(custody_format::header_body_domain, 1, bad_dup.store_id,
                                       bad_dup.owner_scope, bad_dup.descriptor_identity,
                                       bad_dup.current_epoch, bad_dup.epochs, tag_bytes),
                      *key_source, "owner-open", valid_desc),
                  ErrorKind::Permission, "archive.custody.corrupt_header");

    StoreHeader bad_reordered = h3;
    std::swap(bad_reordered.epochs[0], bad_reordered.epochs[1]);
    check_refusal(NativeArchive::open_custody(
                      build_raw_header(custody_format::header_body_domain, 1, bad_reordered.store_id,
                                       bad_reordered.owner_scope, bad_reordered.descriptor_identity,
                                       bad_reordered.current_epoch, bad_reordered.epochs, tag_bytes),
                      *key_source, "owner-open", valid_desc),
                  ErrorKind::Permission, "archive.custody.corrupt_header");
  }

  // 5. Another owner scope or another descriptor -> Tampered.
  {
    check_refusal(NativeArchive::open_custody(h3_bytes, *key_source, "other-owner", valid_desc),
                  ErrorKind::Permission, "archive.custody.tampered");
    check_refusal(NativeArchive::open_custody(h3_bytes, *key_source, "owner-open", other_desc),
                  ErrorKind::Permission, "archive.custody.tampered");
  }

  // 6. Source missing one epoch's key -> KeyNotFound naming that key id (escaped in safe_message) and store_id,
  // wiping every DEK unwrapped so far.
  {
    secrets->remove_secret(special_kid);
    auto res = NativeArchive::open_custody(h3_bytes, *key_source, "owner-open", valid_desc);
    check_refusal(res, ErrorKind::NotFound, "archive.custody.key_not_found", special_kid);
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.safe_message.find("\"k2\\x00\\x0a\\\"\\\\id\"") != std::string::npos);
    CHECK(err.error.safe_message.find(store_id_hex) != std::string::npos);
    for (unsigned char c : err.error.safe_message) {
      CHECK(c >= 0x20 && c <= 0x7e);
    }
  }
}

void test_record_nonce_random_failure() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto valid_desc = default_test_descriptor();

  auto h_res = create_header(*key_source, "owner-nonce-fail", valid_desc);
  CHECK(std::holds_alternative<std::string>(h_res));
  const std::string& header_bytes = std::get<std::string>(h_res);
  const StoreHeader parsed_h = std::get<StoreHeader>(parse_header(header_bytes));
  const std::string store_id_hex = to_hex(parsed_h.store_id.data(), parsed_h.store_id.size());

  auto archive = open_custody_checked(header_bytes, *key_source, "owner-nonce-fail", valid_desc);

  ObserverStats stats;
  custody_ops::set_release_observer(&record_release, &stats);
  try {
    custody_ops::reset_random_seam();
    custody_ops::fail_random_on_call(1);
    auto res = archive->seal({make_user_message("secret-on-nonce-fail")}, "binding-rng");
    const auto draws = custody_ops::recorded_random_draws();
    custody_ops::reset_random_seam();

    CHECK(draws == (std::vector<std::size_t>{24}));
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.kind == ErrorKind::Permission);
    CHECK(err.error.reason == "archive.custody.key_invalid");
    CHECK(err.store_id == store_id_hex);
    CHECK(stats.releases >= 1);
    CHECK(stats.all_zero_every_time);
  } catch (...) {
    custody_ops::reset_random_seam();
    custody_ops::set_release_observer(nullptr, nullptr);
    throw;
  }
  custody_ops::set_release_observer(nullptr, nullptr);
}

void test_golden_record_bytes() {
  // Checked-in reference vectors from `tests/custody_vectors.py`.
  const std::string store_id_bytes =
      from_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  const std::string secret_32_bytes =
      from_hex("202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f");
  const std::string dek_1_bytes =
      from_hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
  const std::string wrap_nonce_bytes =
      from_hex("a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7");
  const std::string record_nonce_bytes =
      from_hex("b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecf");

  constexpr std::string_view kExpectedRecordAadPrefixHex =
      "000000000000002773702e6e61746976652e617263686976652e637573746f64792e7265636f72642d6161642e7631"
      "0000000000000001"
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
      "0000000000000001"
      "000000000000000c6f776e65722d676f6c64656e";
  constexpr std::string_view kExpectedRecordAadSuffixHex =
      "000000000000000e62696e64696e672d676f6c64656e";
  constexpr std::string_view kExpectedRecordPlaintextHex =
      "0000000000000001"
      "00000000000000056d73672d31"
      "0000000000000002"
      "0000000000000001"
      "0000000000000000"
      "000000000000000568656c6c6f"
      "0000000000000000"
      "0000000000000000";
  constexpr std::string_view kExpectedRecordEnvelopePrefixHex =
      "0000000000000001"
      "0000000000000001"
      "b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
      "000000000000005a";
  constexpr std::string_view kExpectedRecordRefDomainPrefixHex =
      "000000000000002773702e6e61746976652e617263686976652e637573746f64792e7265636f72642d7265662e7631";

  std::array<unsigned char, 32> store_id{};
  std::memcpy(store_id.data(), store_id_bytes.data(), store_id.size());
  std::array<unsigned char, 24> record_nonce{};
  std::memcpy(record_nonce.data(), record_nonce_bytes.data(), record_nonce.size());

  auto secrets = std::make_shared<FixedSecretSource>("key-fixed");
  secrets->set_secret("key-fixed", secret_32_bytes);
  auto key_source = make_hkdf_key_source(secrets);
  const auto valid_desc = default_test_descriptor();

  custody_ops::reset_random_seam();
  custody_ops::override_next_random_bytes(store_id_bytes);
  custody_ops::override_next_random_bytes(dek_1_bytes);
  custody_ops::override_next_random_bytes(wrap_nonce_bytes);
  auto created_res = create_header(*key_source, "owner-golden", valid_desc);
  custody_ops::reset_random_seam();
  CHECK(std::holds_alternative<std::string>(created_res));
  const std::string& header_bytes = std::get<std::string>(created_res);

  auto archive = open_custody_checked(header_bytes, *key_source, "owner-golden", valid_desc);

  const HostCustodyLimits resolved_limits = archive_internal::resolve_custody_limits({}, valid_desc);
  const auto expected_desc_digest =
      archive_internal::descriptor_binding_digest(valid_desc, resolved_limits.archive);
  const std::string expected_desc_bytes(reinterpret_cast<const char*>(expected_desc_digest.data()),
                                        expected_desc_digest.size());

  // 1. Record AAD matches `tests/custody_vectors.py` byte for byte.
  const std::string expected_record_aad =
      from_hex(kExpectedRecordAadPrefixHex) + expected_desc_bytes + from_hex(kExpectedRecordAadSuffixHex);
  const std::string actual_record_aad = archive_internal::build_record_aad(
      store_id, 1, "owner-golden", expected_desc_digest, "binding-golden", resolved_limits.archive);
  CHECK(actual_record_aad == expected_record_aad);

  // 2. Seal with fixed record nonce and verify envelope framing, ciphertext, and SHA-256 reference.
  Message msg{"msg-1", Role::User, {Text{"hello"}}};
  custody_ops::reset_random_seam();
  custody_ops::override_next_random_bytes(record_nonce_bytes);
  const SealedRecord sealed = seal_checked(*archive, {msg}, "binding-golden");
  const auto draws = custody_ops::recorded_random_draws();
  custody_ops::reset_random_seam();
  CHECK(draws == (std::vector<std::size_t>{24}));

  const std::string expected_envelope_prefix = from_hex(kExpectedRecordEnvelopePrefixHex);
  const std::string expected_plaintext = from_hex(kExpectedRecordPlaintextHex);
  CHECK(expected_plaintext.size() == 74);
  CHECK(sealed.bytes.size() == 48 + 90);
  CHECK(sealed.bytes.substr(0, 48) == expected_envelope_prefix);

  DataKeyBuffer dek_1;
  CHECK(!dek_1.assign_exact(dek_1_bytes));
  auto expected_aead = custody_ops::aead_xchacha20poly1305_encrypt(
      dek_1, record_nonce, expected_record_aad, expected_plaintext);
  CHECK(std::holds_alternative<std::string>(expected_aead));
  CHECK(sealed.bytes.substr(48) == std::get<std::string>(expected_aead));

  const std::string expected_ref_input = from_hex(kExpectedRecordRefDomainPrefixHex) + sealed.bytes;
  const auto expected_ref_digest = crypto::sha256(expected_ref_input);
  const std::string expected_reference =
      "spna4:" + to_hex(expected_ref_digest.data(), expected_ref_digest.size());
  CHECK(sealed.reference == expected_reference);

  // Also verify the script's sample_reference over synthetic 0..89 ciphertext bytes.
  std::string sample_ciphertext(90, '\0');
  for (std::size_t i = 0; i < sample_ciphertext.size(); ++i) {
    sample_ciphertext[i] = static_cast<char>(i);
  }
  const std::string sample_envelope = expected_envelope_prefix + sample_ciphertext;
  const auto sample_digest =
      crypto::sha256(from_hex(kExpectedRecordRefDomainPrefixHex) + sample_envelope);
  CHECK(archive_internal::compute_record_reference(sample_envelope) ==
        "spna4:" + to_hex(sample_digest.data(), sample_digest.size()));

  // 3. Round-trip open recovers the exact message.
  const auto opened = open_checked(*archive, sealed.reference, sealed.bytes, "binding-golden");
  CHECK(opened.size() == 1);
  CHECK(opened[0].id == "msg-1");
  CHECK(opened[0].role == Role::User);
  CHECK(opened[0].parts.size() == 1);
  CHECK(std::get<Text>(opened[0].parts[0]).value == "hello");
}

void append_test_sink(void* context, std::string_view chunk) {
  static_cast<std::string*>(context)->append(chunk);
}

void test_json_sink_string_escaping_matches_string_writer() {
  std::string all_controls;
  all_controls.reserve(32);
  for (unsigned c = 0; c < 0x20; ++c) {
    all_controls.push_back(static_cast<char>(c));
  }

  // Multi-chunk UTF-8 string (> 8192 bytes) with 2/3/4-byte code points, quotes, backslashes, and controls
  // crossing 4096-byte chunk boundaries.
  std::string multi_chunk_utf8;
  while (multi_chunk_utf8.size() < 9000) {
    multi_chunk_utf8.append("ascii-\"-\\-\n-\t-\x01-\xc3\xa9-\xe2\x82\xac-\xed\x9f\xbf-\xf0\x9f\x98\x80-");
  }

  std::vector<std::string> corpus = {
      "",
      "plain ascii 0123456789 !#$%&'()*+,-./:;<=>?@[]^_`{|}~",
      "\"",
      "\\",
      "\"\\\"",
      "\\\\\"\\b\\f\\n\\r\\t",
      std::string("\x7f", 1),
      all_controls,
      // 2-byte, 3-byte (including U+0800 and U+D7FF), and 4-byte (including U+10000 and U+10FFFF) UTF-8.
      "\xc2\x80\xdf\xbf\xe0\xa0\x80\xe2\x82\xac\xed\x9f\xbf\xee\x80\x80\xef\xbf\xbd\xf0\x90\x80\x80\xf0\x9f\x98\x80\xf4\x8f\xbf\xbf",
      multi_chunk_utf8,
      // Malformed UTF-8 sequence:
      std::string("\xc0\xaf", 2),
  };
  for (unsigned c = 0; c < 0x20; ++c) {
    corpus.emplace_back(1, static_cast<char>(c));
  }

  const json::Limits limits{65536, 32};
  for (const std::string& input : corpus) {
    std::string string_bytes;
    json::BoundedWriter string_writer(limits, &string_bytes);
    string_writer.quoted(input);

    std::string sink_bytes;
    json::BoundedWriter sink_writer(limits, &append_test_sink, &sink_bytes);
    sink_writer.quoted(input);

    CHECK(sink_writer.ok() == string_writer.ok());
    CHECK(sink_writer.size() == string_writer.size());
    CHECK(sink_bytes == string_bytes);
  }
}

void test_caller_invalid_input_faults() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  secrets->set_secret("k2", std::string(32, '\x22'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto valid_desc = default_test_descriptor();

  HostCustodyLimits limits{};
  limits.archive.max_bytes = 4096;
  limits.archive.max_messages = 2;
  limits.archive.max_parts = 2;

  const std::string oversized_text(limits.archive.max_bytes + 1, 'x');

  // 1. Header functions with empty or oversized owner_scope -> InvalidInput.
  for (std::string_view bad_owner : {std::string_view{""}, std::string_view{oversized_text}}) {
    auto create_res = create_header(*key_source, bad_owner, valid_desc, limits);
    CHECK(std::holds_alternative<CustodyError>(create_res));
    const auto& create_err = std::get<CustodyError>(create_res);
    CHECK(create_err.error.kind == ErrorKind::InvalidRequest);
    CHECK(create_err.error.retry_class == RetryClass::Never);
    CHECK(create_err.error.retry_safety == RetrySafety::NotSent);
    CHECK(create_err.error.reason == "archive.custody.invalid_input");
    CHECK(create_err.store_id.empty());
    CHECK(create_err.key_id.empty());
  }

  auto h1_res = create_header(*key_source, "owner-valid", valid_desc, limits);
  CHECK(std::holds_alternative<std::string>(h1_res));
  const std::string& h1_bytes = std::get<std::string>(h1_res);
  const StoreHeader parsed_h = std::get<StoreHeader>(parse_header(h1_bytes, limits));
  const std::string store_id_hex = to_hex(parsed_h.store_id.data(), parsed_h.store_id.size());

  for (std::string_view bad_owner : {std::string_view{""}, std::string_view{oversized_text}}) {
    auto rot_res = rotate_header(h1_bytes, *key_source, "k2", bad_owner, valid_desc, limits);
    CHECK(std::holds_alternative<CustodyError>(rot_res));
    CHECK(std::get<CustodyError>(rot_res).error.kind == ErrorKind::InvalidRequest);
    CHECK(std::get<CustodyError>(rot_res).error.reason == "archive.custody.invalid_input");
    CHECK(std::get<CustodyError>(rot_res).store_id == store_id_hex);

    auto ret_res = retire_key_in_header(h1_bytes, "k1", *key_source, bad_owner, valid_desc, limits);
    CHECK(std::holds_alternative<CustodyError>(ret_res));
    CHECK(std::get<CustodyError>(ret_res).error.kind == ErrorKind::InvalidRequest);
    CHECK(std::get<CustodyError>(ret_res).error.reason == "archive.custody.invalid_input");
    CHECK(std::get<CustodyError>(ret_res).store_id == store_id_hex);

    auto unwrap_res = unwrap_requests(h1_bytes, bad_owner, valid_desc, limits);
    CHECK(std::holds_alternative<CustodyError>(unwrap_res));
    CHECK(std::get<CustodyError>(unwrap_res).error.kind == ErrorKind::InvalidRequest);
    CHECK(std::get<CustodyError>(unwrap_res).error.reason == "archive.custody.invalid_input");
    CHECK(std::get<CustodyError>(unwrap_res).store_id == store_id_hex);

    auto open_res = NativeArchive::open_custody(h1_bytes, *key_source, bad_owner, valid_desc, limits);
    CHECK(std::holds_alternative<CustodyError>(open_res));
    CHECK(std::get<CustodyError>(open_res).error.kind == ErrorKind::InvalidRequest);
    CHECK(std::get<CustodyError>(open_res).error.reason == "archive.custody.invalid_input");
    CHECK(std::get<CustodyError>(open_res).store_id == store_id_hex);
  }

  // Structural parse still runs before caller owner_scope validation on existing headers.
  {
    auto malformed_open = NativeArchive::open_custody("not-a-header", *key_source, "", valid_desc, limits);
    CHECK(std::holds_alternative<CustodyError>(malformed_open));
    CHECK(std::get<CustodyError>(malformed_open).error.reason == "archive.custody.corrupt_header");
  }

  auto archive = open_custody_checked(h1_bytes, *key_source, "owner-valid", valid_desc, limits);
  const SealedRecord valid_sealed =
      seal_checked(*archive, {make_user_message("ok")}, "bind-ok");

  auto check_invalid_input = [&](const auto& res) {
    CHECK(std::holds_alternative<CustodyError>(res));
    const auto& err = std::get<CustodyError>(res);
    CHECK(err.error.kind == ErrorKind::InvalidRequest);
    CHECK(err.error.retry_class == RetryClass::Never);
    CHECK(err.error.retry_safety == RetrySafety::NotSent);
    CHECK(err.error.reason == "archive.custody.invalid_input");
    CHECK(err.store_id == store_id_hex);
    CHECK(err.key_id.empty());
  };

  // 2. seal with messages over max_messages, parts over max_parts, messages over max_bytes, or oversized binding.
  check_invalid_input(archive->seal(
      {make_user_message("1"), make_user_message("2"), make_user_message("3")}, "bind-ok"));
  check_invalid_input(archive->seal(
      {Message{"", Role::User, {Text{"p1"}, Text{"p2"}, Text{"p3"}}}}, "bind-ok"));
  check_invalid_input(archive->seal({make_user_message(oversized_text)}, "bind-ok"));
  check_invalid_input(archive->seal({make_user_message("ok")}, oversized_text));

  // 3. open with oversized expected_binding.
  check_invalid_input(archive->open(valid_sealed.reference, valid_sealed.bytes, oversized_text));
}

void test_record_plaintext_exact_limit_edge() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto valid_desc = default_test_descriptor();

  HostCustodyLimits limits{};
  limits.archive.max_bytes = 4096;

  auto h_res = create_header(*key_source, "owner-edge", valid_desc, limits);
  CHECK(std::holds_alternative<std::string>(h_res));
  auto archive = open_custody_checked(
      std::get<std::string>(h_res), *key_source, "owner-edge", valid_desc, limits);

  // One message with empty id, one Text part, no wire_output, and no native seal has 64 bytes of framing:
  // count(8) + id_len(8) + role(8) + parts_count(8) + part_tag(8) + text_len(8) + wire_output(8) + native(8) = 64.
  constexpr std::size_t kSingleTextMessageOverhead = 64;
  const std::string exact_text(limits.archive.max_bytes - kSingleTextMessageOverhead, 'e');
  const Message exact_msg = make_user_message(exact_text);

  const SealedRecord sealed = seal_checked(*archive, {exact_msg}, "edge-binding");
  // Envelope = 48-byte framing + max_bytes ciphertext + 16-byte AEAD tag.
  CHECK(sealed.bytes.size() ==
        archive_internal::kRecordEnvelopeFramingBytes + limits.archive.max_bytes +
            archive_internal::kRecordAeadTagBytes);

  const auto opened = open_checked(*archive, sealed.reference, sealed.bytes, "edge-binding");
  CHECK(opened.size() == 1);
  CHECK(opened[0].parts.size() == 1);
  CHECK(std::get<Text>(opened[0].parts[0]).value == exact_text);

  // One byte more than max_bytes is refused by seal with InvalidInput.
  const Message over_by_one_msg = make_user_message(exact_text + "!");
  const auto refused = archive->seal({over_by_one_msg}, "edge-binding");
  CHECK(std::holds_alternative<CustodyError>(refused));
  const auto& err = std::get<CustodyError>(refused);
  CHECK(err.error.kind == ErrorKind::InvalidRequest);
  CHECK(err.error.retry_class == RetryClass::Never);
  CHECK(err.error.retry_safety == RetrySafety::NotSent);
  CHECK(err.error.reason == "archive.custody.invalid_input");
}

void test_open_custody_matches_descriptor() {
  auto secrets = std::make_shared<FixedSecretSource>("k1");
  secrets->set_secret("k1", std::string(32, '\x11'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto desc_d = default_test_descriptor();
  const auto desc_other = alternate_test_descriptor();

  auto h_res = create_header(*key_source, "owner-desc", desc_d);
  CHECK(std::holds_alternative<std::string>(h_res));
  auto archive =
      open_custody_checked(std::get<std::string>(h_res), *key_source, "owner-desc", desc_d);

  CHECK(archive->matches_descriptor(desc_d));
  CHECK(!archive->matches_descriptor(desc_other));
  CHECK(!archive->matches_descriptor(provider_test_descriptor("openai.responses")));
}

void test_randomness_entropy_and_header_uniqueness() {
  custody_ops::reset_random_seam();
  CHECK(sodium_init() >= 0);

  std::array<unsigned char, 32> sp_draw_a{};
  std::array<unsigned char, 32> sp_draw_b{};
  std::array<unsigned char, 32> all_zeros{};
  CHECK(crypto::random_bytes(sp_draw_a.data(), sp_draw_a.size()));
  CHECK(crypto::random_bytes(sp_draw_b.data(), sp_draw_b.size()));
  CHECK(sp_draw_a != all_zeros);
  CHECK(sp_draw_b != all_zeros);
  CHECK(sp_draw_a != sp_draw_b);

  std::array<unsigned char, 32> sodium_draw_a{};
  std::array<unsigned char, 32> sodium_draw_b{};
  randombytes_buf(sodium_draw_a.data(), sodium_draw_a.size());
  randombytes_buf(sodium_draw_b.data(), sodium_draw_b.size());
  CHECK(sodium_draw_a != all_zeros);
  CHECK(sodium_draw_b != all_zeros);
  CHECK(sodium_draw_a != sodium_draw_b);

  auto secrets = std::make_shared<FixedSecretSource>("k-rng");
  secrets->set_secret("k-rng", std::string(32, '\x42'));
  auto key_source = make_hkdf_key_source(secrets);
  const auto desc = default_test_descriptor();

  auto h1_res = create_header(*key_source, "owner-entropy", desc);
  auto h2_res = create_header(*key_source, "owner-entropy", desc);
  CHECK(std::holds_alternative<std::string>(h1_res));
  CHECK(std::holds_alternative<std::string>(h2_res));
  const std::string& h1_bytes = std::get<std::string>(h1_res);
  const std::string& h2_bytes = std::get<std::string>(h2_res);
  CHECK(h1_bytes != h2_bytes);

  auto parsed_h1_res = parse_header(h1_bytes);
  auto parsed_h2_res = parse_header(h2_bytes);
  CHECK(std::holds_alternative<StoreHeader>(parsed_h1_res));
  CHECK(std::holds_alternative<StoreHeader>(parsed_h2_res));
  const StoreHeader& parsed_h1 = std::get<StoreHeader>(parsed_h1_res);
  const StoreHeader& parsed_h2 = std::get<StoreHeader>(parsed_h2_res);
  CHECK(parsed_h1.store_id != all_zeros);
  CHECK(parsed_h2.store_id != all_zeros);
  CHECK(parsed_h1.store_id != parsed_h2.store_id);
  CHECK(parsed_h1.epochs.size() == 1);
  CHECK(parsed_h2.epochs.size() == 1);
  CHECK(parsed_h1.epochs[0].wrapped_dek != parsed_h2.epochs[0].wrapped_dek);

  auto reqs_1_res = unwrap_requests(h1_bytes, "owner-entropy", desc);
  auto reqs_2_res = unwrap_requests(h2_bytes, "owner-entropy", desc);
  CHECK(std::holds_alternative<std::vector<UnwrapRequest>>(reqs_1_res));
  CHECK(std::holds_alternative<std::vector<UnwrapRequest>>(reqs_2_res));
  const auto& reqs_1 = std::get<std::vector<UnwrapRequest>>(reqs_1_res);
  const auto& reqs_2 = std::get<std::vector<UnwrapRequest>>(reqs_2_res);
  CHECK(reqs_1.size() == 1 && reqs_2.size() == 1);

  DataKeyBuffer dek_1;
  DataKeyBuffer dek_2;
  CHECK(!key_source->unwrap_dek(reqs_1[0].key_id, reqs_1[0].epoch_aad, reqs_1[0].wrapped_dek, dek_1));
  CHECK(!key_source->unwrap_dek(reqs_2[0].key_id, reqs_2[0].epoch_aad, reqs_2[0].wrapped_dek, dek_2));
  CHECK(std::memcmp(dek_1.data(), all_zeros.data(), 32) != 0);
  CHECK(std::memcmp(dek_2.data(), all_zeros.data(), 32) != 0);
  CHECK(std::memcmp(dek_1.data(), dek_2.data(), 32) != 0);
}

void test_pre_resolved_secret_source() {
  custody_ops::fail_key_alloc_on_call(0);
  const auto baseline_allocs = custody_ops::live_key_allocation_sizes();

  {
    PreResolvedSecretSource source;
    CHECK(source.empty());
    CHECK(source.size() == 0);
    auto empty_active = source.active_key_id();
    CHECK(std::holds_alternative<CustodyFault>(empty_active));
    CHECK(std::get<CustodyFault>(empty_active) == CustodyFault::InvalidKeyMaterial);

    source.set_active_key_id("k1");
    auto active_k1 = source.active_key_id();
    CHECK(std::holds_alternative<std::string>(active_k1));
    CHECK(std::get<std::string>(active_k1) == "k1");

    const std::string secret_32(32, '\x21');
    const std::string secret_48(48, '\x43');
    const std::string secret_64(64, '\x65');
    const std::string binary_key_id("k\0bin", 5);

    // Invalid lengths and invalid key_ids are refused with InvalidKeyMaterial and allocate nothing.
    CHECK(source.put("k1", std::string_view{}) == CustodyFault::InvalidKeyMaterial);
    CHECK(source.put("k1", std::string(31, '\x11')) == CustodyFault::InvalidKeyMaterial);
    CHECK(source.put("k1", std::string(65, '\x11')) == CustodyFault::InvalidKeyMaterial);
    CHECK(source.put("k1", nullptr, 32) == CustodyFault::InvalidKeyMaterial);
    CHECK(source.put("", secret_32) == CustodyFault::InvalidKeyMaterial);
    CHECK(custody_ops::live_key_allocation_sizes() == baseline_allocs);

    // Valid 32..64 byte secrets allocate sodium_malloc blocks of exact length.
    CHECK(!source.put("k1", secret_32));
    CHECK(!source.put(binary_key_id, secret_48));
    CHECK(!source.put("k3", secret_64));
    CHECK(source.size() == 3);
    CHECK(custody_ops::live_key_allocation_sizes().size() == baseline_allocs.size() + 3);

    // Failed sodium_malloc on put returns InvalidKeyMaterial without disturbing existing entries.
    custody_ops::fail_key_alloc_on_call(1);
    CHECK(source.put("k-fail", secret_32) == CustodyFault::InvalidKeyMaterial);
    custody_ops::fail_key_alloc_on_call(0);
    CHECK(source.size() == 3);

    // Overwriting an existing key_id replaces and frees the old block.
    const std::string secret_32_replaced(40, '\x77');
    CHECK(!source.put("k1", secret_32_replaced));
    CHECK(source.size() == 3);
    CHECK(custody_ops::live_key_allocation_sizes().size() == baseline_allocs.size() + 3);

    SecretBuffer out;
    std::size_t written = 0;
    CHECK(!source.load_secret("k1", out, &written));
    CHECK(written == 40);
    CHECK(std::memcmp(out.data(), secret_32_replaced.data(), 40) == 0);

    CHECK(!source.load_secret(binary_key_id, out, &written));
    CHECK(written == 48);
    CHECK(std::memcmp(out.data(), secret_48.data(), 48) == 0);

    CHECK(!source.load_secret("k3", out, &written));
    CHECK(written == 64);
    CHECK(std::memcmp(out.data(), secret_64.data(), 64) == 0);

    // Missing key_id wipes out and returns KeyNotFound.
    CHECK(source.load_secret("absent", out, &written) == CustodyFault::KeyNotFound);
    CHECK(written == 0);
    std::array<unsigned char, 64> zero64{};
    CHECK(std::memcmp(out.data(), zero64.data(), 64) == 0);

    // Removing a key wipes and frees its block (counted against the live blocks just before, which include `out`).
    const std::size_t live_before_remove = custody_ops::live_key_allocation_sizes().size();
    CHECK(source.remove("k3"));
    CHECK(!source.remove("k3"));
    CHECK(source.size() == 2);
    CHECK(custody_ops::live_key_allocation_sizes().size() == live_before_remove - 1);
    CHECK(source.load_secret("k3", out, &written) == CustodyFault::KeyNotFound);

    // Move construction transfers blocks without copying; destruction of the final owner frees all.
    PreResolvedSecretSource moved(std::move(source));
    CHECK(source.empty());
    CHECK(moved.size() == 2);
    CHECK(std::get<std::string>(moved.active_key_id()) == "k1");

    const auto desc = default_test_descriptor();
    auto key_source = make_hkdf_key_source(std::move(moved));
    auto h1_res = create_header(*key_source, "owner-pre-resolved", desc);
    CHECK(std::holds_alternative<std::string>(h1_res));
    auto h2_res =
        rotate_header(std::get<std::string>(h1_res), *key_source, binary_key_id, "owner-pre-resolved", desc);
    CHECK(std::holds_alternative<std::string>(h2_res));
    auto archive = open_custody_checked(
        std::get<std::string>(h2_res), *key_source, "owner-pre-resolved", desc);
    const std::vector<Message> messages = {make_user_message("pre-resolved secret roundtrip")};
    const SealedRecord sealed = seal_checked(*archive, messages, "ctx-pre");
    const auto opened = open_checked(*archive, sealed.reference, sealed.bytes, "ctx-pre");
    CHECK(opened.size() == 1 && opened[0].id == messages[0].id &&
          opened[0].role == messages[0].role && opened[0].parts.size() == messages[0].parts.size());
    check_same_part(messages[0].parts[0], opened[0].parts[0]);
  }
  CHECK(custody_ops::live_key_allocation_sizes() == baseline_allocs);

  // C handoff functions allocate, commit under a binary key_id, wipe and free the transfer block, and refuse invalid sizes.
  sp::PreResolvedSecretSource* c_source = sp_custody_secret_source_create();
  CHECK(c_source != nullptr);
  CHECK(sp_custody_secret_alloc(0) == nullptr);
  CHECK(sp_custody_secret_alloc(31) == nullptr);
  CHECK(sp_custody_secret_alloc(65) == nullptr);
  CHECK(std::string_view(sp_custody_fault_reason(static_cast<int>(CustodyFault::InvalidKeyMaterial))) ==
        "archive.custody.key_invalid");

  unsigned char* transfer = sp_custody_secret_alloc(32);
  CHECK(transfer != nullptr);
  std::memset(transfer, 0x5c, 32);
  const unsigned char raw_kid[] = {'k', '\0', 'c'};
  CHECK(sp_custody_secret_commit(c_source, raw_kid, sizeof(raw_kid), transfer, 32) == 0);
  CHECK(custody_ops::live_key_allocation_sizes().size() == baseline_allocs.size() + 1);

  SecretBuffer loaded;
  std::size_t loaded_len = 0;
  CHECK(!c_source->load_secret(std::string_view(reinterpret_cast<const char*>(raw_kid), sizeof(raw_kid)),
                               loaded,
                               &loaded_len));
  CHECK(loaded_len == 32);
  CHECK(std::memcmp(loaded.data(), std::string(32, '\x5c').data(), 32) == 0);
  loaded.release();

  sp_custody_secret_source_destroy(c_source);
  CHECK(custody_ops::live_key_allocation_sizes() == baseline_allocs);
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
#if SP_BUILD_THREADS
    test_concurrent_operation_scopes();
#endif
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
    test_custody_roundtrip_all_provider_families();
    test_custody_record_nonce_uniqueness();
    test_record_aad_single_field_binding();
    test_rotated_epochs_and_zero_source_calls_on_seal_and_open();
    test_record_tampering_and_truncation_bounds();
    test_record_plaintext_residue_zeroed_on_release();
    test_open_custody_refusals();
    test_record_nonce_random_failure();
    test_golden_record_bytes();
    test_json_sink_string_escaping_matches_string_writer();
    test_caller_invalid_input_faults();
    test_record_plaintext_exact_limit_edge();
    test_open_custody_matches_descriptor();
    test_randomness_entropy_and_header_uniqueness();
    test_pre_resolved_secret_source();
    std::cout << "native archive custody tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
