// Custody primitives on a build without libsodium: every CustodyOutcome-returning custody function returns
// ErrorKind::Unsupported with reason "archive.custody.unavailable", KeySource methods return
// CustodyFault::InvalidKeyMaterial, and core_interface_contract() omits capability::HostArchiveCustody.
#include "core/interface_contract.h"
#include "core/native_archive.h"
#include "core/native_archive_custody.h"
#include "core/native_archive_custody_internal.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace sp;

#define CHECK(c)                                                                                      \
  do {                                                                                                \
    if (!(c)) {                                                                                       \
      throw std::runtime_error("custody unavailable check failed at line " + std::to_string(__LINE__)); \
    }                                                                                                 \
  } while (false)

template <class T>
void check_unavailable(const CustodyOutcome<T>& outcome) {
  const auto* err = std::get_if<CustodyError>(&outcome);
  CHECK(err != nullptr);
  CHECK(err->error.kind == ErrorKind::Unsupported);
  CHECK(err->error.retry_class == RetryClass::Never);
  CHECK(err->error.retry_safety == RetrySafety::NotSent);
  CHECK(err->error.reason == "archive.custody.unavailable");
  CHECK(err->store_id.empty());
  CHECK(err->key_id.empty());
}

}  // namespace

int main() {
  try {
    CHECK(!custody_ops::available());
    const auto contract = core_interface_contract();
    CHECK(contract.revision == EXPECTED_INTERFACE_REVISION);
    CHECK((contract.capabilities & capability::HostArchiveCustody) == 0);
    CHECK((capability::RequiredProvider & capability::HostArchiveCustody) == 0);

    DataKeyBuffer dek;
    SecretBuffer secret;
    CHECK(!dek.valid());
    CHECK(!secret.valid());
    CHECK(dek.assign_exact(std::string(32, 'k')) == CustodyFault::InvalidKeyMaterial);
    std::size_t written = 99;
    CHECK(secret.assign_secret(std::string(32, 's'), &written) == CustodyFault::InvalidKeyMaterial);
    CHECK(written == 0);

    const std::array<unsigned char, 32> store_id{};
    const std::array<unsigned char, 24> nonce{};
    check_unavailable(custody_ops::hkdf_sha256_extract("salt", "ikm"));
    check_unavailable(custody_ops::hkdf_sha256_expand_key(dek, "info"));
    check_unavailable(custody_ops::derive_kek(secret, 32, store_id));
    check_unavailable(custody_ops::aead_xchacha20poly1305_encrypt(dek, nonce, "aad", "plaintext"));
    check_unavailable(custody_ops::aead_xchacha20poly1305_decrypt(dek, nonce, "aad", std::string(32, '\0')));

    auto source = make_hkdf_key_source(nullptr);
    CHECK(source != nullptr);
    const auto active = source->active_key_id();
    CHECK(std::holds_alternative<CustodyFault>(active) &&
          std::get<CustodyFault>(active) == CustodyFault::InvalidKeyMaterial);
    const auto wrapped = source->wrap_dek("k1", "aad", dek);
    CHECK(std::holds_alternative<CustodyFault>(wrapped) &&
          std::get<CustodyFault>(wrapped) == CustodyFault::InvalidKeyMaterial);
    CHECK(source->unwrap_dek("k1", "aad", std::string(72, '\0'), dek) == CustodyFault::InvalidKeyMaterial);

    std::cout << "native archive custody unavailable test passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
