#include "core/native_archive_custody.h"
#include "core/native_archive_custody_internal.h"
#include "crypto/crypto.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace sp {
namespace {

#ifdef SP_CUSTODY_TEST_SEAMS
thread_local custody_ops::ReleaseObserver g_release_observer = nullptr;
thread_local void* g_release_observer_context = nullptr;

thread_local custody_ops::AllocationObserver g_allocation_observer = nullptr;
thread_local void* g_allocation_observer_context = nullptr;
#endif

class UnavailableHkdfKeySource final : public KeySource {
 public:
  CustodyResult<std::string> active_key_id() override {
    return CustodyFault::InvalidKeyMaterial;
  }

  CustodyResult<std::string> wrap_dek(std::string_view /*key_id*/,
                                       std::string_view /*epoch_aad*/,
                                       const DataKeyBuffer& /*dek*/) override {
    return CustodyFault::InvalidKeyMaterial;
  }

  std::optional<CustodyFault> unwrap_dek(std::string_view /*key_id*/,
                                         std::string_view /*epoch_aad*/,
                                         std::string_view /*wrapped_dek*/,
                                         DataKeyBuffer& dek_out) override {
    dek_out.wipe();
    return CustodyFault::InvalidKeyMaterial;
  }
};

}  // namespace

namespace custody_detail {

unsigned char* sodium_key_alloc(std::size_t /*size*/) noexcept {
  return nullptr;
}

void sodium_key_wipe(void* ptr, std::size_t size) noexcept {
  if (ptr != nullptr && size > 0) {
    crypto::cleanse(ptr, size);
  }
}

void sodium_key_free(void* ptr, std::size_t size) noexcept {
  if (ptr != nullptr && size > 0) {
    crypto::cleanse(ptr, size);
  }
}

}  // namespace custody_detail

CustodyError custody_error(CustodyFault fault,
                           std::string_view store_id,
                           std::string_view key_id) {
  CustodyError out;
  out.error.retry_safety = RetrySafety::NotSent;
  out.error.retry_class = RetryClass::Never;
  bool carries_key_id = false;
  switch (fault) {
    case CustodyFault::Tampered:
      out.error.kind = ErrorKind::Permission;
      out.error.safe_message = "Host native archive custody verification failed";
      out.error.reason = "archive.custody.tampered";
      out.store_id = std::string(store_id);
      break;
    case CustodyFault::Unauthorized:
      out.error.kind = ErrorKind::Permission;
      out.error.safe_message = "Host native archive key source rejected authorization";
      out.error.reason = "archive.custody.unauthorized";
      out.store_id = std::string(store_id);
      out.key_id = std::string(key_id);
      carries_key_id = true;
      break;
    case CustodyFault::KeyNotFound:
      out.error.kind = ErrorKind::NotFound;
      out.error.safe_message = "Host native archive key was not found";
      out.error.reason = "archive.custody.key_not_found";
      out.store_id = std::string(store_id);
      out.key_id = std::string(key_id);
      carries_key_id = true;
      break;
    case CustodyFault::InvalidKeyMaterial:
      out.error.kind = ErrorKind::Permission;
      out.error.safe_message = "Host native archive key material is invalid";
      out.error.reason = "archive.custody.key_invalid";
      out.store_id = std::string(store_id);
      out.key_id = std::string(key_id);
      carries_key_id = true;
      break;
    case CustodyFault::KeyInUse:
      out.error.kind = ErrorKind::Permission;
      out.error.safe_message = "Host native archive active wrapping key is in use";
      out.error.reason = "archive.custody.key_in_use";
      out.store_id = std::string(store_id);
      out.key_id = std::string(key_id);
      carries_key_id = true;
      break;
    case CustodyFault::CorruptHeader:
      out.error.kind = ErrorKind::Permission;
      out.error.safe_message = "Host native archive header is malformed";
      out.error.reason = "archive.custody.corrupt_header";
      out.store_id = std::string(store_id);
      break;
    case CustodyFault::CorruptRecord:
      out.error.kind = ErrorKind::Permission;
      out.error.safe_message = "Host native archive record is malformed";
      out.error.reason = "archive.custody.corrupt_record";
      out.store_id = std::string(store_id);
      break;
    case CustodyFault::InvalidInput:
      out.error.kind = ErrorKind::InvalidRequest;
      out.error.safe_message = "Host native archive custody input is invalid";
      out.error.reason = "archive.custody.invalid_input";
      out.store_id = std::string(store_id);
      break;
    case CustodyFault::EpochLimit:
      out.error.kind = ErrorKind::ResourceLimit;
      out.error.safe_message = "Host native archive epoch limit reached";
      out.error.reason = "archive.custody.epoch_limit";
      out.store_id = std::string(store_id);
      break;
    case CustodyFault::SourceUnavailable:
      out.error.kind = ErrorKind::Transport;
      out.error.retry_class = RetryClass::Transient;
      out.error.safe_message = "Host native archive key source is unavailable";
      out.error.reason = "archive.custody.source_unavailable";
      out.store_id = std::string(store_id);
      out.key_id = std::string(key_id);
      carries_key_id = true;
      break;
    case CustodyFault::SourceRateLimited:
      out.error.kind = ErrorKind::RateLimited;
      out.error.retry_class = RetryClass::Transient;
      out.error.safe_message = "Host native archive key source is rate limited";
      out.error.reason = "archive.custody.source_rate_limited";
      out.store_id = std::string(store_id);
      out.key_id = std::string(key_id);
      carries_key_id = true;
      break;
  }
  if (carries_key_id && !key_id.empty()) {
    out.error.safe_message.append(" (key_id=");
    out.error.safe_message.append(archive_internal::quote_key_id_for_message(key_id));
    if (!store_id.empty()) {
      out.error.safe_message.append(", store_id=");
      out.error.safe_message.append(store_id);
    }
    out.error.safe_message.push_back(')');
  } else if (!store_id.empty()) {
    out.error.safe_message.append(" (store_id=");
    out.error.safe_message.append(store_id);
    out.error.safe_message.push_back(')');
  }
  return out;
}

CustodyError custody_unavailable_error() {
  CustodyError out;
  out.error.kind = ErrorKind::Unsupported;
  out.error.retry_class = RetryClass::Never;
  out.error.retry_safety = RetrySafety::NotSent;
  out.error.safe_message = "Host native archive custody is unavailable in this build";
  out.error.reason = "archive.custody.unavailable";
  return out;
}

CleansedBuffer::CleansedBuffer(std::size_t size) {
  if (size > 0) {
#ifdef SP_CUSTODY_TEST_SEAMS
    if (g_allocation_observer != nullptr) {
      g_allocation_observer(size, g_allocation_observer_context);
    }
#endif
    data_ = static_cast<unsigned char*>(::operator new(size));
    crypto::cleanse(data_, size);
    size_ = size;
    capacity_ = size;
  }
}

CleansedBuffer::CleansedBuffer(CleansedBuffer&& other) noexcept
    : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
  other.data_ = nullptr;
  other.size_ = 0;
  other.capacity_ = 0;
}

CleansedBuffer& CleansedBuffer::operator=(CleansedBuffer&& other) noexcept {
  if (this != &other) {
    wipe();
    data_ = other.data_;
    size_ = other.size_;
    capacity_ = other.capacity_;
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
  }
  return *this;
}

CleansedBuffer::~CleansedBuffer() noexcept {
  wipe();
}

void CleansedBuffer::release_block(unsigned char* block, std::size_t capacity) noexcept {
  if (block == nullptr) {
    return;
  }
  if (capacity > 0) {
    crypto::cleanse(block, capacity);
  }
#ifdef SP_CUSTODY_TEST_SEAMS
  if (g_release_observer != nullptr) {
    bool all_zero = true;
    const volatile unsigned char* p = block;
    for (std::size_t i = 0; i < capacity; ++i) {
      if (p[i] != 0) {
        all_zero = false;
        break;
      }
    }
    g_release_observer(capacity, all_zero, g_release_observer_context);
  }
#endif
  ::operator delete(block);
}

void CleansedBuffer::reallocate(std::size_t new_capacity) {
  if (new_capacity <= capacity_) {
    return;
  }
#ifdef SP_CUSTODY_TEST_SEAMS
  if (g_allocation_observer != nullptr) {
    g_allocation_observer(new_capacity, g_allocation_observer_context);
  }
#endif
  auto* next = static_cast<unsigned char*>(::operator new(new_capacity));
  crypto::cleanse(next, new_capacity);
  if (data_ != nullptr && size_ > 0) {
    std::memcpy(next, data_, size_);
  }
  if (data_ != nullptr) {
    release_block(data_, capacity_);
  }
  data_ = next;
  capacity_ = new_capacity;
}

void CleansedBuffer::reserve(std::size_t new_capacity) {
  if (new_capacity > capacity_) {
    reallocate(new_capacity);
  }
}

void CleansedBuffer::resize(std::size_t new_size) {
  if (new_size < size_) {
    crypto::cleanse(data_ + new_size, size_ - new_size);
    size_ = new_size;
  } else if (new_size > capacity_) {
    const std::size_t doubled =
        capacity_ > std::numeric_limits<std::size_t>::max() / 2 ? new_size : capacity_ * 2;
    const std::size_t target = capacity_ == 0 ? new_size : std::max(new_size, doubled);
    reallocate(target);
    size_ = new_size;
  } else if (new_size > size_) {
    crypto::cleanse(data_ + size_, new_size - size_);
    size_ = new_size;
  }
}

void CleansedBuffer::append(const void* data, std::size_t size) {
  if (size == 0) {
    return;
  }
  if (size > std::numeric_limits<std::size_t>::max() - size_) {
    throw std::bad_alloc();
  }
  if (size > capacity_ - size_) {
    const std::size_t needed = size_ + size;
    const std::size_t doubled =
        capacity_ > std::numeric_limits<std::size_t>::max() / 2 ? needed : capacity_ * 2;
    const std::size_t target =
        capacity_ == 0 ? std::max<std::size_t>(needed, 64) : std::max(needed, doubled);
    reallocate(target);
  }
  std::memcpy(data_ + size_, data, size);
  size_ += size;
}

void CleansedBuffer::append(std::string_view bytes) {
  append(bytes.data(), bytes.size());
}

void CleansedBuffer::wipe() noexcept {
  if (data_ != nullptr) {
    release_block(data_, capacity_);
    data_ = nullptr;
    size_ = 0;
    capacity_ = 0;
  }
}

std::string_view CleansedBuffer::view() const noexcept {
  return {reinterpret_cast<const char*>(data_), size_};
}

std::shared_ptr<KeySource> make_hkdf_key_source(std::shared_ptr<SecretSource> /*secret_source*/) {
  return std::make_shared<UnavailableHkdfKeySource>();
}

CustodyOutcome<StoreHeader> parse_header(std::string_view /*header_bytes*/,
                                         HostCustodyLimits /*limits*/) {
  return custody_unavailable_error();
}

CustodyOutcome<std::string> serialize_header(const StoreHeader& /*header*/,
                                             HostCustodyLimits /*limits*/) {
  return custody_unavailable_error();
}

CustodyOutcome<std::string> create_header(KeySource& /*source*/,
                                          std::string_view /*owner_scope*/,
                                          const descriptor::ValidatedDescriptor& /*descriptor*/,
                                          HostCustodyLimits /*limits*/) {
  return custody_unavailable_error();
}

CustodyOutcome<std::string> rotate_header(std::string_view /*current_header_bytes*/,
                                          KeySource& /*source*/,
                                          std::string_view /*target_key_id*/,
                                          std::string_view /*owner_scope*/,
                                          const descriptor::ValidatedDescriptor& /*descriptor*/,
                                          HostCustodyLimits /*limits*/) {
  return custody_unavailable_error();
}

CustodyOutcome<std::string> retire_key_in_header(
    std::string_view /*current_header_bytes*/,
    std::string_view /*retired_key_id*/,
    KeySource& /*source*/,
    std::string_view /*owner_scope*/,
    const descriptor::ValidatedDescriptor& /*descriptor*/,
    HostCustodyLimits /*limits*/) {
  return custody_unavailable_error();
}

CustodyOutcome<std::vector<UnwrapRequest>> unwrap_requests(
    std::string_view /*header_bytes*/,
    std::string_view /*owner_scope*/,
    const descriptor::ValidatedDescriptor& /*descriptor*/,
    HostCustodyLimits /*limits*/) {
  return custody_unavailable_error();
}

namespace custody_ops {

bool available() noexcept {
  return false;
}

bool random_bytes(void* /*data*/, std::size_t /*size*/) noexcept {
  return false;
}

#ifdef SP_CUSTODY_TEST_SEAMS
void set_release_observer(ReleaseObserver observer, void* context) noexcept {
  g_release_observer = observer;
  g_release_observer_context = context;
}

void set_allocation_observer(AllocationObserver observer, void* context) noexcept {
  g_allocation_observer = observer;
  g_allocation_observer_context = context;
}

void fail_random_on_call(std::size_t /*call_index*/) noexcept {}

void override_next_random_bytes(std::string_view /*bytes*/) {}

std::vector<std::size_t> recorded_random_draws() { return {}; }

void reset_random_seam() noexcept {}

void fail_key_alloc_on_call(std::size_t /*call_index*/) noexcept {}

void corrupt_next_cached_kek() noexcept {}

std::vector<std::size_t> live_key_allocation_sizes() { return {}; }

CustodyOutcome<std::string> hkdf_sha256_expand(const DataKeyBuffer& /*prk*/,
                                               std::string_view /*info*/,
                                               std::size_t /*out_len*/) {
  return custody_unavailable_error();
}
#endif

std::array<unsigned char, 32> compute_record_reference_digest(
    std::string_view envelope_bytes) noexcept {
  crypto::Sha256 hash;
  unsigned char domain_len_be[8];
  const std::uint64_t domain_len = custody_format::ref_domain.size();
  for (unsigned i = 0; i < 8; ++i) {
    domain_len_be[7 - i] = static_cast<unsigned char>(domain_len >> (i * 8));
  }
  hash.update(domain_len_be, sizeof(domain_len_be));
  hash.update(custody_format::ref_domain);
  hash.update(envelope_bytes);
  return hash.finish();
}

CustodyOutcome<AuthenticatedHeaderState> open_authenticated_header(
    std::string_view /*header_bytes*/,
    KeySource& /*source*/,
    std::string_view /*owner_scope*/,
    const descriptor::ValidatedDescriptor& /*descriptor*/,
    HostCustodyLimits /*limits*/) {
  return custody_unavailable_error();
}

CustodyOutcome<DataKeyBuffer> hkdf_sha256_extract(std::string_view /*salt*/,
                                                  std::string_view /*ikm*/) {
  return custody_unavailable_error();
}

CustodyOutcome<DataKeyBuffer> hkdf_sha256_expand_key(const DataKeyBuffer& /*prk*/,
                                                     std::string_view /*info*/) {
  return custody_unavailable_error();
}

CustodyOutcome<DataKeyBuffer> derive_kek(const SecretBuffer& /*secret*/,
                                         std::size_t /*written_size*/,
                                         const std::array<unsigned char, 32>& /*store_id*/) {
  return custody_unavailable_error();
}

CustodyOutcome<std::string> aead_xchacha20poly1305_encrypt(
    const unsigned char* /*key_32*/,
    const std::array<unsigned char, 24>& /*nonce*/,
    std::string_view /*aad*/,
    std::string_view /*plaintext*/,
    std::string_view /*store_id_hex*/) {
  return custody_unavailable_error();
}

CustodyOutcome<std::string> aead_xchacha20poly1305_encrypt(
    const DataKeyBuffer& /*key*/,
    const std::array<unsigned char, 24>& /*nonce*/,
    std::string_view /*aad*/,
    std::string_view /*plaintext*/) {
  return custody_unavailable_error();
}

CustodyOutcome<CleansedBuffer> aead_xchacha20poly1305_decrypt(
    const unsigned char* /*key_32*/,
    const std::array<unsigned char, 24>& /*nonce*/,
    std::string_view /*aad*/,
    std::string_view /*ciphertext_and_tag*/,
    std::string_view /*store_id_hex*/) {
  return custody_unavailable_error();
}

CustodyOutcome<CleansedBuffer> aead_xchacha20poly1305_decrypt(
    const DataKeyBuffer& /*key*/,
    const std::array<unsigned char, 24>& /*nonce*/,
    std::string_view /*aad*/,
    std::string_view /*ciphertext_and_tag*/) {
  return custody_unavailable_error();
}

}  // namespace custody_ops
}  // namespace sp
