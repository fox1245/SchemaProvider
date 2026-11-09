#pragma once
#include "core/native_archive_custody.h"
#include "core/value.h"
#include "descriptor/descriptor.h"

namespace sp {
// Sensitive native bytes stay in local host custody. References are not export
// bundles. Provision/open is host activation, never a model-input operation.
class NativeArchive final {
 public:
  using Activation = std::variant<std::shared_ptr<NativeArchive>, Error>;
  using Saved = std::variant<std::string, Error>;
  using Loaded = std::variant<std::vector<Message>, Error>;
  using CustodyActivation = CustodyOutcome<std::shared_ptr<NativeArchive>>;
  using Sealed = CustodyOutcome<SealedRecord>;
  using Opened = CustodyOutcome<std::vector<Message>>;
  static Activation provision(std::string directory, std::string independent_key_file,
      std::string owner_scope, descriptor::ValidatedDescriptor, NativeArchiveLimits = {});
  static Activation open(std::string directory, std::string independent_key_file,
      std::string owner_scope, descriptor::ValidatedDescriptor, NativeArchiveLimits = {});
  static CustodyActivation open_custody(std::string_view header_bytes, KeySource& source,
      std::string_view owner_scope, descriptor::ValidatedDescriptor, HostCustodyLimits = {});
  ~NativeArchive();
  NativeArchive(const NativeArchive&) = delete;
  NativeArchive& operator=(const NativeArchive&) = delete;
  Saved save(const std::vector<Message>&, std::string_view binding = {}) const;
  Loaded load(std::string_view reference, std::string_view binding = {}) const;
  // Serializes and encrypts `messages` into a wiping plaintext buffer before AEAD sealing.
  // Strings inside JSON values (`wire_output`, tool inputs, `wire_metadata`) are escaped
  // directly into the wiping buffer without intermediate string allocations, and scalar JSON
  // spellings are wiped after emission.
  Sealed seal(const std::vector<Message>&, std::string_view binding = {}) const;
  // Verifies and decrypts a sealed record into a wiping plaintext buffer, then decodes the
  // record and wipes the decrypted bytes before returning. The returned `Message` values
  // (including their strings and parsed `json::Document` heap allocations) are ordinary heap
  // objects owned by the caller; if decoding fails part way through a decrypted record, any
  // partly decoded `Message` fields or `json::Document` nodes already constructed are freed
  // by their ordinary allocators without wiping.
  Opened open(std::string_view reference, std::string_view bytes,
      std::string_view expected_binding = {}) const;
  std::string_view owner_scope() const noexcept;
  std::uint64_t current_epoch() const noexcept;
  bool matches_descriptor(const descriptor::ValidatedDescriptor&) const noexcept;
 private:
  struct State;
  explicit NativeArchive(std::unique_ptr<State>);
  static Activation activate(std::string, std::string, std::string,
      descriptor::ValidatedDescriptor, NativeArchiveLimits, bool create);
  std::unique_ptr<State> state_;
};
} // namespace sp
