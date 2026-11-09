// Custody primitives on a build without libsodium: every CustodyOutcome-returning custody function returns
// ErrorKind::Unsupported with reason "archive.custody.unavailable", KeySource methods return
// CustodyFault::InvalidKeyMaterial, and core_interface_contract() omits capability::HostArchiveCustody.
#include "core/interface_contract.h"
#include "core/native_archive.h"
#include "core/native_archive_custody.h"
#include "core/native_archive_custody_internal.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>
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

    auto loaded = descriptor::load(
        R"({"descriptor_version":1,"revision":1,"id":"archive-fixture","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}}})");
    CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
    const auto& desc = std::get<descriptor::ValidatedDescriptor>(loaded);
    const StoreHeader empty_header{};
    check_unavailable(parse_header("header"));
    check_unavailable(serialize_header(empty_header));
    check_unavailable(create_header(*source, "owner", desc));
    check_unavailable(rotate_header("header", *source, "k2", "owner", desc));
    check_unavailable(retire_key_in_header("header", "k1", *source, "owner", desc));
    check_unavailable(unwrap_requests("header", "owner", desc));
    check_unavailable(NativeArchive::open_custody("header", *source, "owner", desc));

    PreResolvedSecretSource pre_resolved("k1");
    CHECK(pre_resolved.put("k1", std::string(32, 's')) == CustodyFault::InvalidKeyMaterial);
    CHECK(pre_resolved.load_secret("k1", secret, &written) == CustodyFault::KeyNotFound);
    CHECK(written == 0);

    sp::PreResolvedSecretSource* c_source = sp_custody_secret_source_create();
    CHECK(c_source != nullptr);
    CHECK(sp_custody_secret_alloc(32) == nullptr);
    const unsigned char raw_kid[] = {'k', '1'};
    CHECK(sp_custody_secret_commit(c_source, raw_kid, sizeof(raw_kid), nullptr, 32) ==
          static_cast<int>(CustodyFault::InvalidKeyMaterial));
    CHECK(std::string_view(sp_custody_fault_reason(static_cast<int>(CustodyFault::InvalidKeyMaterial))) ==
          "archive.custody.key_invalid");
    sp_custody_secret_source_destroy(c_source);

    if (archive_fs::available()) {
      const auto base = std::filesystem::temp_directory_path() /
                        ("sp-custody-unavail-" +
#ifdef _WIN32
                         std::to_string(::GetCurrentProcessId()));
#else
                         std::to_string(::getpid()));
#endif
      std::filesystem::remove_all(base);
      std::filesystem::create_directories(base);
#ifndef _WIN32
      ::chmod(base.c_str(), 0700);
#endif
      const auto store_dir = (base / "store").string();
      const auto key_file = (base / "activation.key").string();
      auto provisioned = NativeArchive::provision(store_dir, key_file, "owner", desc);
      if (const auto* local_archive = std::get_if<std::shared_ptr<NativeArchive>>(&provisioned)) {
        check_unavailable((*local_archive)->seal({}, "binding"));
        check_unavailable((*local_archive)->open("spna4:" + std::string(64, '0'), "bytes", "binding"));
      }
      std::filesystem::remove_all(base);
    } else {
      auto provisioned = NativeArchive::provision("store", "activation.key", "owner", desc);
      CHECK(std::holds_alternative<Error>(provisioned));
      CHECK(std::get<Error>(provisioned).kind == ErrorKind::Unsupported);
    }

    const CustodyError epoch_err = custody_error(CustodyFault::EpochLimit, "store-hex", "ignored");
    CHECK(epoch_err.error.kind == ErrorKind::ResourceLimit);
    CHECK(epoch_err.error.retry_class == RetryClass::Never);
    CHECK(epoch_err.error.retry_safety == RetrySafety::NotSent);
    CHECK(epoch_err.error.reason == "archive.custody.epoch_limit");
    CHECK(epoch_err.store_id == "store-hex");
    CHECK(epoch_err.key_id.empty());

    std::cout << "native archive custody unavailable test passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
