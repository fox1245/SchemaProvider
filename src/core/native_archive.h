#pragma once
#include "core/value.h"
#include "descriptor/descriptor.h"

namespace sp {
// Sensitive native bytes stay in local host custody. References are not export
// bundles. Provision/open is host activation, never a model-input operation.
struct NativeArchiveLimits {
  // Zero resolves from the admitted descriptor's closed/versioned resource
  // policy: native_bytes, request_messages, request_parts, native_depth;
  // store records=request_messages, store bytes=native_bytes*records.
  size_t max_bytes = 0;
  size_t max_messages = 0;
  size_t max_parts = 0;
  size_t max_json_depth = 0;
  size_t max_records = 0;
  uint64_t max_store_bytes = 0;
};
class NativeArchive final {
 public:
  using Activation = std::variant<std::shared_ptr<NativeArchive>, Error>;
  using Saved = std::variant<std::string, Error>;
  using Loaded = std::variant<std::vector<Message>, Error>;
  static Activation provision(std::string directory, std::string independent_key_file,
      std::string owner_scope, descriptor::ValidatedDescriptor, NativeArchiveLimits = {});
  static Activation open(std::string directory, std::string independent_key_file,
      std::string owner_scope, descriptor::ValidatedDescriptor, NativeArchiveLimits = {});
  ~NativeArchive();
  NativeArchive(const NativeArchive&) = delete;
  NativeArchive& operator=(const NativeArchive&) = delete;
  Saved save(const std::vector<Message>&, std::string_view binding = {}) const;
  Loaded load(std::string_view reference, std::string_view binding = {}) const;
  std::string_view owner_scope() const noexcept;
  bool matches_descriptor(const descriptor::ValidatedDescriptor&) const noexcept;
 private:
  struct State;
  explicit NativeArchive(std::unique_ptr<State>);
  static Activation activate(std::string, std::string, std::string,
      descriptor::ValidatedDescriptor, NativeArchiveLimits, bool create);
  std::unique_ptr<State> state_;
};
} // namespace sp
