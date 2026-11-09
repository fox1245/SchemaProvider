#pragma once
#include "core/value.h"
#include "descriptor/descriptor.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sp {

// Zero resolves from the admitted descriptor's closed/versioned resource
// policy: native_bytes, request_messages, request_parts, native_depth;
// store records=request_messages, store bytes=native_bytes*records.
struct NativeArchiveLimits {
  std::size_t max_bytes = 0;
  std::size_t max_messages = 0;
  std::size_t max_parts = 0;
  std::size_t max_json_depth = 0;
  std::size_t max_records = 0;
  std::uint64_t max_store_bytes = 0;
};

struct HostCustodyLimits {
  NativeArchiveLimits archive{};
  std::size_t max_epochs = 0;
};

namespace custody_detail {
unsigned char* sodium_key_alloc(std::size_t size) noexcept;
void sodium_key_wipe(void* ptr, std::size_t size) noexcept;
void sodium_key_free(void* ptr, std::size_t size) noexcept;
}  // namespace custody_detail

enum class CustodyFault : std::uint8_t {
  Tampered,            // AEAD tag mismatch, AAD mismatch, or content-address SHA-256 mismatch
  Unauthorized,        // Host key source rejected permission/authentication
  KeyNotFound,         // Requested key_id is absent from KeySource/SecretSource
  InvalidKeyMaterial,  // Secret/DEK length invalid, empty key_id, or CSPRNG/libsodium failure
  KeyInUse,            // Attempted to retire the currently active key_id without rotating first
  CorruptHeader,       // Malformed or unsupported StoreHeader bytes
  CorruptRecord,       // Malformed or unsupported SealedRecord bytes
  InvalidInput,        // Caller input violates archive limits or structural requirements
  EpochLimit,          // StoreHeader epoch table reached configured max_epochs
  SourceUnavailable,   // Transient key source unreachable / timed out
  SourceRateLimited,   // Transient key source rate limit
};

template <class T>
using CustodyResult = std::variant<T, CustodyFault>;

struct CustodyError {
  Error error;
  std::string store_id{};
  std::string key_id{};
};

template <class T>
using CustodyOutcome = std::variant<T, CustodyError>;

CustodyError custody_error(CustodyFault fault,
                           std::string_view store_id = {},
                           std::string_view key_id = {});

// Movable, non-copyable owner of one sodium_malloc(N) allocation, released with sodium_free.
// Page locking is best effort as provided by sodium_malloc. A null allocation is InvalidKeyMaterial.
template <std::size_t N>
class LockedKeyBuffer final {
 public:
  static constexpr std::size_t kSize = N;
  static constexpr std::size_t kMinSecretSize = N / 2;

  LockedKeyBuffer() noexcept : bytes_(custody_detail::sodium_key_alloc(N)) {}
  LockedKeyBuffer(const LockedKeyBuffer&) = delete;
  LockedKeyBuffer& operator=(const LockedKeyBuffer&) = delete;
  LockedKeyBuffer(LockedKeyBuffer&& other) noexcept : bytes_(other.bytes_) {
    other.bytes_ = nullptr;
  }
  LockedKeyBuffer& operator=(LockedKeyBuffer&& other) noexcept {
    if (this != &other) {
      release();
      bytes_ = other.bytes_;
      other.bytes_ = nullptr;
    }
    return *this;
  }
  ~LockedKeyBuffer() noexcept { release(); }

  [[nodiscard]] bool valid() const noexcept { return bytes_ != nullptr; }
  explicit operator bool() const noexcept { return bytes_ != nullptr; }
  [[nodiscard]] unsigned char* writable_data() noexcept { return bytes_; }
  [[nodiscard]] const unsigned char* data() const noexcept { return bytes_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return N; }

  void wipe() noexcept {
    if (bytes_ != nullptr) {
      custody_detail::sodium_key_wipe(bytes_, N);
    }
  }

  void release() noexcept {
    if (bytes_ != nullptr) {
      custody_detail::sodium_key_free(bytes_, N);
      bytes_ = nullptr;
    }
  }

  std::optional<CustodyFault> assign_exact(const void* src, std::size_t len) noexcept {
    if (bytes_ == nullptr || src == nullptr || len != N) {
      wipe();
      return CustodyFault::InvalidKeyMaterial;
    }
    std::memcpy(bytes_, src, N);
    return std::nullopt;
  }

  std::optional<CustodyFault> assign_exact(std::string_view bytes) noexcept {
    return assign_exact(bytes.data(), bytes.size());
  }

  std::optional<CustodyFault> assign_secret(const void* src,
                                            std::size_t len,
                                            std::size_t* written_size) noexcept
    requires(N == 64)
  {
    if (written_size != nullptr) {
      *written_size = 0;
    }
    if (bytes_ == nullptr || src == nullptr || written_size == nullptr || len < kMinSecretSize ||
        len > N) {
      wipe();
      return CustodyFault::InvalidKeyMaterial;
    }
    wipe();
    std::memcpy(bytes_, src, len);
    *written_size = len;
    return std::nullopt;
  }

  std::optional<CustodyFault> assign_secret(std::string_view bytes,
                                            std::size_t* written_size) noexcept
    requires(N == 64)
  {
    return assign_secret(bytes.data(), bytes.size(), written_size);
  }

 private:
  unsigned char* bytes_ = nullptr;
};

using DataKeyBuffer = LockedKeyBuffer<32>;
using SecretBuffer = LockedKeyBuffer<64>;

inline std::optional<CustodyFault> validate_dek_material(DataKeyBuffer& dek,
                                                         std::size_t written_size) noexcept {
  if (!dek.valid() || written_size != DataKeyBuffer::kSize) {
    dek.wipe();
    return CustodyFault::InvalidKeyMaterial;
  }
  return std::nullopt;
}

inline std::optional<CustodyFault> validate_secret_material(SecretBuffer& secret,
                                                            std::size_t written_size) noexcept {
  if (!secret.valid() || written_size < SecretBuffer::kMinSecretSize ||
      written_size > SecretBuffer::kSize) {
    secret.wipe();
    return CustodyFault::InvalidKeyMaterial;
  }
  return std::nullopt;
}

// Wiping heap buffer for record plaintexts. Growth allocates a new block, copies existing bytes,
// zeroes the old block across its full capacity, and frees it without std::vector/std::string reallocation.
class CleansedBuffer final {
 public:
  CleansedBuffer() noexcept = default;
  explicit CleansedBuffer(std::size_t size);
  CleansedBuffer(const CleansedBuffer&) = delete;
  CleansedBuffer& operator=(const CleansedBuffer&) = delete;
  CleansedBuffer(CleansedBuffer&& other) noexcept;
  CleansedBuffer& operator=(CleansedBuffer&& other) noexcept;
  ~CleansedBuffer() noexcept;

  void reserve(std::size_t new_capacity);
  void resize(std::size_t new_size);
  void append(const void* data, std::size_t size);
  void append(std::string_view bytes);
  void wipe() noexcept;

  [[nodiscard]] unsigned char* data() noexcept { return data_; }
  [[nodiscard]] const unsigned char* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::string_view view() const noexcept;

 private:
  void reallocate(std::size_t new_capacity);
  static void release_block(unsigned char* block, std::size_t capacity) noexcept;

  unsigned char* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t capacity_ = 0;
};

class SecretSource {
 public:
  virtual ~SecretSource() = default;
  virtual CustodyResult<std::string> active_key_id() = 0;
  // Writes 32..64 secret bytes for `key_id` into `out` and sets `*written_size`.
  // Must call `out.wipe()` on failure.
  virtual std::optional<CustodyFault> load_secret(std::string_view key_id,
                                                  SecretBuffer& out,
                                                  std::size_t* written_size) = 0;
};

class CustodyOperationScope;
class KeySource;
std::optional<CustodyFault> verify_wrapped_dek(KeySource& source,
                                               std::string_view key_id,
                                               std::string_view epoch_aad,
                                               std::string_view wrapped_dek,
                                               const DataKeyBuffer& expected_dek);

class KeySource {
 public:
  virtual ~KeySource() = default;
  virtual CustodyResult<std::string> active_key_id() = 0;

  virtual CustodyResult<std::string> wrap_dek(std::string_view key_id,
                                              std::string_view epoch_aad,
                                              const DataKeyBuffer& dek) = 0;

  virtual std::optional<CustodyFault> unwrap_dek(std::string_view key_id,
                                                 std::string_view epoch_aad,
                                                 std::string_view wrapped_dek,
                                                 DataKeyBuffer& dek_out) = 0;

 protected:
  friend class CustodyOperationScope;
  friend std::optional<CustodyFault> verify_wrapped_dek(KeySource& source,
                                                        std::string_view key_id,
                                                        std::string_view epoch_aad,
                                                        std::string_view wrapped_dek,
                                                        const DataKeyBuffer& expected_dek);
  virtual void begin_operation_scope() noexcept {}
  virtual void end_operation_scope() noexcept {}
  virtual std::optional<CustodyFault> verify_unwrap_dek(std::string_view key_id,
                                                        std::string_view epoch_aad,
                                                        std::string_view wrapped_dek,
                                                        DataKeyBuffer& dek_out) {
    return unwrap_dek(key_id, epoch_aad, wrapped_dek, dek_out);
  }
  static std::optional<CustodyFault> forward_verify_unwrap_dek(KeySource& target,
                                                               std::string_view key_id,
                                                               std::string_view epoch_aad,
                                                               std::string_view wrapped_dek,
                                                               DataKeyBuffer& dek_out) {
    return target.verify_unwrap_dek(key_id, epoch_aad, wrapped_dek, dek_out);
  }
};

// Scopes one header operation on a KeySource so make_hkdf_key_source loads each
// (store_id, key_id) secret at most once while any scope is open and wipes derived KEKs
// when the last open scope exits.
class CustodyOperationScope final {
 public:
  explicit CustodyOperationScope(KeySource& source) noexcept : source_(source) {
    source_.begin_operation_scope();
  }
  CustodyOperationScope(const CustodyOperationScope&) = delete;
  CustodyOperationScope& operator=(const CustodyOperationScope&) = delete;
  ~CustodyOperationScope() noexcept { source_.end_operation_scope(); }

 private:
  KeySource& source_;
};

std::shared_ptr<KeySource> make_hkdf_key_source(std::shared_ptr<SecretSource> secret_source);

struct EpochKeyEntry {
  std::uint64_t epoch = 0;
  std::string wrapping_key_id;
  std::string wrapped_dek;
};

struct StoreHeader {
  std::uint64_t format_version = 0;
  std::array<unsigned char, 32> store_id{};
  std::string owner_scope;
  std::array<unsigned char, 32> descriptor_identity{};
  std::uint64_t current_epoch = 0;
  std::vector<EpochKeyEntry> epochs;
  std::array<unsigned char, 32> header_tag{};
};

struct UnwrapRequest {
  std::string key_id;
  std::string epoch_aad;
  std::string wrapped_dek;
};

struct SealedRecord {
  std::string reference;
  std::string bytes;
};

// Parses header bytes structurally without a key. Returns unauthenticated data; nothing
// returned here may be treated as custody state before open_custody succeeds.
// Malformed header bytes or invalid limits return CustodyFault::CorruptHeader.
CustodyOutcome<StoreHeader> parse_header(std::string_view header_bytes,
                                         HostCustodyLimits limits = {});

// Serializes a StoreHeader structurally and verifies round-trip parsing under limits.
// Structural or limit violations return CustodyFault::CorruptHeader.
CustodyOutcome<std::string> serialize_header(const StoreHeader& header,
                                             HostCustodyLimits limits = {});

// Creates a new authenticated StoreHeader at epoch 1.
// Invalid limits return CustodyFault::CorruptHeader; an empty or oversized owner_scope
// returns CustodyFault::InvalidInput; an invalid active key_id from source returns
// CustodyFault::InvalidKeyMaterial.
CustodyOutcome<std::string> create_header(KeySource& source,
                                          std::string_view owner_scope,
                                          const descriptor::ValidatedDescriptor& descriptor,
                                          HostCustodyLimits limits = {});

// Verifies the current header tag and appends epoch current_epoch + 1.
// Only the current epoch's entry is used before the header tag check.
// Invalid limits or malformed header bytes return CustodyFault::CorruptHeader; an empty
// or oversized owner_scope returns CustodyFault::InvalidInput; caller owner_scope or
// descriptor mismatch against the parsed header returns CustodyFault::Tampered; an invalid
// non-empty target_key_id returns CustodyFault::InvalidKeyMaterial.
CustodyOutcome<std::string> rotate_header(std::string_view current_header_bytes,
                                          KeySource& source,
                                          std::string_view target_key_id,
                                          std::string_view owner_scope,
                                          const descriptor::ValidatedDescriptor& descriptor,
                                          HostCustodyLimits limits = {});

// Verifies the current header tag and re-wraps any prior epoch entries wrapped under
// retired_key_id to the current epoch's wrapping_key_id.
// Only the current epoch's entry is used before the header tag check.
// Invalid limits or malformed header bytes return CustodyFault::CorruptHeader; an empty
// or oversized owner_scope returns CustodyFault::InvalidInput; caller owner_scope or
// descriptor mismatch against the parsed header returns CustodyFault::Tampered; an empty or
// overlong retired_key_id returns CustodyFault::InvalidKeyMaterial; retiring the
// current epoch's wrapping_key_id returns CustodyFault::KeyInUse.
CustodyOutcome<std::string> retire_key_in_header(std::string_view current_header_bytes,
                                                 std::string_view retired_key_id,
                                                 KeySource& source,
                                                 std::string_view owner_scope,
                                                 const descriptor::ValidatedDescriptor& descriptor,
                                                 HostCustodyLimits limits = {});

// Parses header bytes without a key, checks owner_scope and descriptor_identity against
// the caller's arguments, and builds one UnwrapRequest per epoch entry. Returns
// unauthenticated data; nothing returned here may be treated as custody state before
// open_custody succeeds.
// Invalid limits or malformed header bytes return CustodyFault::CorruptHeader; an empty
// or oversized owner_scope returns CustodyFault::InvalidInput; caller owner_scope or
// descriptor mismatch against the parsed header returns CustodyFault::Tampered.
CustodyOutcome<std::vector<UnwrapRequest>> unwrap_requests(
    std::string_view header_bytes,
    std::string_view owner_scope,
    const descriptor::ValidatedDescriptor& descriptor,
    HostCustodyLimits limits = {});

}  // namespace sp
