#pragma once
#include "core/native_archive_custody.h"
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace sp {

CustodyError custody_unavailable_error();

namespace custody_ops {

bool available() noexcept;
[[nodiscard]] bool random_bytes(void* data, std::size_t size) noexcept;

CustodyOutcome<DataKeyBuffer> hkdf_sha256_extract(std::string_view salt, std::string_view ikm);
CustodyOutcome<DataKeyBuffer> hkdf_sha256_expand_key(const DataKeyBuffer& prk,
                                                     std::string_view info);
CustodyOutcome<DataKeyBuffer> derive_kek(const SecretBuffer& secret,
                                         std::size_t written_size,
                                         const std::array<unsigned char, 32>& store_id);

CustodyOutcome<std::string> aead_xchacha20poly1305_encrypt(
    const DataKeyBuffer& key,
    const std::array<unsigned char, 24>& nonce,
    std::string_view aad,
    std::string_view plaintext);
CustodyOutcome<CleansedBuffer> aead_xchacha20poly1305_decrypt(
    const DataKeyBuffer& key,
    const std::array<unsigned char, 24>& nonce,
    std::string_view aad,
    std::string_view ciphertext_and_tag);

#ifdef SP_CUSTODY_TEST_SEAMS
using ReleaseObserver = void (*)(std::size_t capacity, bool all_zero, void* context) noexcept;
void set_release_observer(ReleaseObserver observer, void* context = nullptr) noexcept;

void fail_random_on_call(std::size_t call_index) noexcept;
void override_next_random_bytes(std::string_view bytes);
void reset_random_seam() noexcept;
void fail_key_alloc_on_call(std::size_t call_index) noexcept;

CustodyOutcome<std::string> hkdf_sha256_expand(const DataKeyBuffer& prk,
                                               std::string_view info,
                                               std::size_t out_len);
#endif

}  // namespace custody_ops

}  // namespace sp
