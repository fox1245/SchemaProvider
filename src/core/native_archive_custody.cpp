#include "core/native_archive_custody.h"
#include "core/native_archive_custody_format.h"
#include "core/native_archive_custody_internal.h"
#include "crypto/crypto.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <new>

namespace sp {
namespace {

constexpr std::size_t kNonceBytes = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr std::size_t kAeadTagBytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;
constexpr std::size_t kWrappedDekBytes = kNonceBytes + DataKeyBuffer::kSize + kAeadTagBytes;
constexpr std::size_t kHeaderTagBytes = crypto_auth_hmacsha256_BYTES;

static_assert(DataKeyBuffer::kSize == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
static_assert(DataKeyBuffer::kSize == crypto_kdf_hkdf_sha256_KEYBYTES);
static_assert(DataKeyBuffer::kSize == crypto_auth_hmacsha256_KEYBYTES);
static_assert(kHeaderTagBytes == 32);
static_assert(SecretBuffer::kMinSecretSize == DataKeyBuffer::kSize);

bool sodium_ready() noexcept {
  return sodium_init() >= 0;
}

#ifdef SP_CUSTODY_TEST_SEAMS
thread_local custody_ops::ReleaseObserver g_release_observer = nullptr;
thread_local void* g_release_observer_context = nullptr;

thread_local custody_ops::AllocationObserver g_allocation_observer = nullptr;
thread_local void* g_allocation_observer_context = nullptr;

thread_local std::size_t g_random_call_count = 0;
thread_local std::size_t g_random_fail_on_call = 0;
thread_local std::deque<std::string> g_random_overrides;
thread_local std::vector<std::size_t> g_random_draw_sizes;

thread_local std::size_t g_key_alloc_call_count = 0;
thread_local std::size_t g_key_alloc_fail_on_call = 0;
thread_local bool g_corrupt_next_cached_kek = false;
thread_local std::map<const void*, std::size_t> g_live_key_allocs;

bool key_alloc_should_fail() noexcept {
  if (g_key_alloc_fail_on_call == 0) {
    return false;
  }
  ++g_key_alloc_call_count;
  return g_key_alloc_call_count == g_key_alloc_fail_on_call;
}
#endif

void write_be64(char out[8], std::uint64_t value) noexcept {
  for (unsigned i = 0; i < 8; ++i) {
    out[7 - i] = static_cast<char>(value >> (i * 8));
  }
}

std::string encode_text_field(std::string_view text) {
  std::string out(8 + text.size(), '\0');
  write_be64(out.data(), text.size());
  if (!text.empty()) {
    std::memcpy(out.data() + 8, text.data(), text.size());
  }
  return out;
}

bool extract_store_id_from_epoch_aad(std::string_view epoch_aad,
                                     std::array<unsigned char, 32>& store_id_out) noexcept {
  try {
    NativeArchiveLimits prefix_limits{};
    prefix_limits.max_bytes = 8 + custody_format::epoch_aad_domain.size() + 8;
    archive_internal::Writer prefix(prefix_limits);
    archive_internal::write_epoch_aad_version_prefix(prefix);
    if (epoch_aad.size() < prefix.data.size() + store_id_out.size() ||
        std::memcmp(epoch_aad.data(), prefix.data.data(), prefix.data.size()) != 0) {
      return false;
    }
    std::memcpy(store_id_out.data(), epoch_aad.data() + prefix.data.size(), store_id_out.size());
    return true;
  } catch (...) {
    return false;
  }
}

class HkdfKeySource final : public KeySource {
 public:
  explicit HkdfKeySource(std::shared_ptr<SecretSource> secret_source)
      : secret_source_(std::move(secret_source)) {}

  ~HkdfKeySource() override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : cached_keks_) {
      entry.second.wipe();
    }
    cached_keks_.clear();
    active_scopes_ = 0;
  }

  CustodyResult<std::string> active_key_id() override {
    if (!secret_source_) {
      return CustodyFault::InvalidKeyMaterial;
    }
    auto res = secret_source_->active_key_id();
    if (const auto* id = std::get_if<std::string>(&res)) {
      if (id->empty()) {
        return CustodyFault::InvalidKeyMaterial;
      }
    }
    return res;
  }

  CustodyResult<std::string> wrap_dek(std::string_view key_id,
                                      std::string_view epoch_aad,
                                      const DataKeyBuffer& dek) override {
    if (!secret_source_ || key_id.empty() || !dek.valid() || !sodium_ready()) {
      return CustodyFault::InvalidKeyMaterial;
    }
    DataKeyBuffer kek;
    if (auto fault = obtain_kek(key_id, epoch_aad, kek, true)) {
      return *fault;
    }
    std::string wrapped(kWrappedDekBytes, '\0');
    auto* nonce_ptr = reinterpret_cast<unsigned char*>(wrapped.data());
    if (!custody_ops::random_bytes(nonce_ptr, kNonceBytes)) {
      sodium_memzero(wrapped.data(), wrapped.size());
      return CustodyFault::InvalidKeyMaterial;
    }
    unsigned long long clen = 0;
    const auto* ad_ptr =
        epoch_aad.empty() ? nullptr : reinterpret_cast<const unsigned char*>(epoch_aad.data());
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char*>(wrapped.data() + kNonceBytes),
            &clen,
            dek.data(),
            dek.size(),
            ad_ptr,
            epoch_aad.size(),
            nullptr,
            nonce_ptr,
            kek.data()) != 0 ||
        clen != DataKeyBuffer::kSize + kAeadTagBytes) {
      sodium_memzero(wrapped.data(), wrapped.size());
      return CustodyFault::InvalidKeyMaterial;
    }
    return wrapped;
  }

  std::optional<CustodyFault> unwrap_dek(std::string_view key_id,
                                         std::string_view epoch_aad,
                                         std::string_view wrapped_dek,
                                         DataKeyBuffer& dek_out) override {
    if (!secret_source_ || key_id.empty() || !dek_out.valid() || !sodium_ready()) {
      dek_out.wipe();
      return CustodyFault::InvalidKeyMaterial;
    }
    if (wrapped_dek.size() != kWrappedDekBytes) {
      dek_out.wipe();
      return CustodyFault::Tampered;
    }
    DataKeyBuffer kek;
    if (auto fault = obtain_kek(key_id, epoch_aad, kek, false)) {
      dek_out.wipe();
      return *fault;
    }
    unsigned long long mlen = 0;
    const auto* c_ptr = reinterpret_cast<const unsigned char*>(wrapped_dek.data() + kNonceBytes);
    const std::size_t clen = wrapped_dek.size() - kNonceBytes;
    const auto* ad_ptr = epoch_aad.empty() ? nullptr : reinterpret_cast<const unsigned char*>(epoch_aad.data());
    const auto* npub_ptr = reinterpret_cast<const unsigned char*>(wrapped_dek.data());
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            dek_out.writable_data(),
            &mlen,
            nullptr,
            c_ptr,
            clen,
            ad_ptr,
            epoch_aad.size(),
            npub_ptr,
            kek.data()) != 0 ||
        mlen != DataKeyBuffer::kSize) {
      dek_out.wipe();
      return CustodyFault::Tampered;
    }
    return std::nullopt;
  }

 protected:
  void begin_operation_scope() noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++active_scopes_;
  }

  void end_operation_scope() noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_scopes_ > 0 && --active_scopes_ == 0) {
      for (auto& entry : cached_keks_) {
        entry.second.wipe();
      }
      cached_keks_.clear();
    }
  }

  std::optional<CustodyFault> verify_unwrap_dek(std::string_view key_id,
                                                std::string_view epoch_aad,
                                                std::string_view wrapped_dek,
                                                DataKeyBuffer& dek_out) override {
    if (!secret_source_ || key_id.empty() || !dek_out.valid() || !sodium_ready()) {
      dek_out.wipe();
      return CustodyFault::InvalidKeyMaterial;
    }
    if (wrapped_dek.size() != kWrappedDekBytes) {
      dek_out.wipe();
      return CustodyFault::Tampered;
    }
    std::array<unsigned char, 32> store_id{};
    if (!extract_store_id_from_epoch_aad(epoch_aad, store_id)) {
      dek_out.wipe();
      return CustodyFault::InvalidKeyMaterial;
    }
    DataKeyBuffer kek;
    if (!kek.valid()) {
      dek_out.wipe();
      return CustodyFault::InvalidKeyMaterial;
    }
    if (auto fault = load_and_derive_kek(key_id, store_id, kek)) {
      kek.wipe();
      dek_out.wipe();
      return *fault;
    }
    unsigned long long mlen = 0;
    const auto* c_ptr = reinterpret_cast<const unsigned char*>(wrapped_dek.data() + kNonceBytes);
    const std::size_t clen = wrapped_dek.size() - kNonceBytes;
    const auto* ad_ptr =
        epoch_aad.empty() ? nullptr : reinterpret_cast<const unsigned char*>(epoch_aad.data());
    const auto* npub_ptr = reinterpret_cast<const unsigned char*>(wrapped_dek.data());
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        dek_out.writable_data(),
        &mlen,
        nullptr,
        c_ptr,
        clen,
        ad_ptr,
        epoch_aad.size(),
        npub_ptr,
        kek.data());
    kek.wipe();
    if (rc != 0 || mlen != DataKeyBuffer::kSize) {
      dek_out.wipe();
      return CustodyFault::Tampered;
    }
    return std::nullopt;
  }

 private:
  struct CacheKey {
    std::array<unsigned char, 32> store_id{};
    std::string key_id{};

    bool operator<(const CacheKey& other) const noexcept {
      if (store_id != other.store_id) {
        return store_id < other.store_id;
      }
      return key_id < other.key_id;
    }
  };

  std::optional<CustodyFault> load_and_derive_kek(std::string_view key_id,
                                                  const std::array<unsigned char, 32>& store_id,
                                                  DataKeyBuffer& kek_out) {
    SecretBuffer secret;
    if (!secret.valid()) {
      return CustodyFault::InvalidKeyMaterial;
    }
    std::size_t written_size = 0;
    if (auto fault = secret_source_->load_secret(key_id, secret, &written_size)) {
      secret.wipe();
      return *fault;
    }
    if (auto fault = validate_secret_material(secret, written_size)) {
      return *fault;
    }
    auto derived = custody_ops::derive_kek(secret, written_size, store_id);
    secret.wipe();
    if (std::holds_alternative<CustodyError>(derived)) {
      return CustodyFault::InvalidKeyMaterial;
    }
    kek_out = std::get<DataKeyBuffer>(std::move(derived));
    return std::nullopt;
  }

  // Writes the KEK for (store_id from epoch_aad, key_id) into kek_out. While at least one operation scope on this
  // source is open, a derived KEK is cached and reused. The cache is read and written only under mutex_ and every
  // caller gets its own copy, so no pointer into the cache outlives the lock and the last scope may clear it at any
  // time. The secret source is called without the lock held, so a source that re-enters this one does not deadlock.
  std::optional<CustodyFault> obtain_kek(std::string_view key_id,
                                         std::string_view epoch_aad,
                                         DataKeyBuffer& kek_out,
                                         bool for_wrap) {
    std::array<unsigned char, 32> store_id{};
    if (!extract_store_id_from_epoch_aad(epoch_aad, store_id) || !kek_out.valid()) {
      return CustodyFault::InvalidKeyMaterial;
    }
    const CacheKey cache_key{store_id, std::string(key_id)};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_scopes_ > 0) {
        if (auto it = cached_keks_.find(cache_key); it != cached_keks_.end()) {
#ifdef SP_CUSTODY_TEST_SEAMS
          if (for_wrap && g_corrupt_next_cached_kek) {
            it->second.writable_data()[0] ^= 0xff;
            g_corrupt_next_cached_kek = false;
          }
#endif
          std::memcpy(kek_out.writable_data(), it->second.data(), kek_out.size());
          return std::nullopt;
        }
      }
    }
    if (auto fault = load_and_derive_kek(key_id, store_id, kek_out)) {
      return *fault;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_scopes_ > 0 && cached_keks_.find(cache_key) == cached_keks_.end()) {
      DataKeyBuffer cached;
      if (!cached.valid()) {
        kek_out.wipe();
        return CustodyFault::InvalidKeyMaterial;
      }
      std::memcpy(cached.writable_data(), kek_out.data(), cached.size());
#ifdef SP_CUSTODY_TEST_SEAMS
      if (for_wrap && g_corrupt_next_cached_kek) {
        cached.writable_data()[0] ^= 0xff;
        std::memcpy(kek_out.writable_data(), cached.data(), kek_out.size());
        g_corrupt_next_cached_kek = false;
      }
#endif
      cached_keks_.emplace(cache_key, std::move(cached));
    }
    return std::nullopt;
  }

  std::shared_ptr<SecretSource> secret_source_;
  std::mutex mutex_;
  std::size_t active_scopes_ = 0;
  std::map<CacheKey, DataKeyBuffer> cached_keks_;
};

}  // namespace

namespace custody_detail {

unsigned char* sodium_key_alloc(std::size_t size) noexcept {
#ifdef SP_CUSTODY_TEST_SEAMS
  if (key_alloc_should_fail()) {
    return nullptr;
  }
#endif
  if (size == 0 || !sodium_ready()) {
    return nullptr;
  }
  void* ptr = sodium_malloc(size);
  if (ptr == nullptr) {
    return nullptr;
  }
  sodium_memzero(ptr, size);
#ifdef SP_CUSTODY_TEST_SEAMS
  try {
    g_live_key_allocs[ptr] = size;
  } catch (...) {
    sodium_free(ptr);
    return nullptr;
  }
#endif
  return static_cast<unsigned char*>(ptr);
}

void sodium_key_wipe(void* ptr, std::size_t size) noexcept {
  if (ptr != nullptr && size > 0) {
    sodium_memzero(ptr, size);
  }
}

void sodium_key_free(void* ptr, std::size_t size) noexcept {
  if (ptr != nullptr) {
    if (size > 0) {
      sodium_memzero(ptr, size);
    }
#ifdef SP_CUSTODY_TEST_SEAMS
    g_live_key_allocs.erase(ptr);
#endif
    sodium_free(ptr);
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
    sodium_memzero(data_, size);
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
    sodium_memzero(block, capacity);
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
  sodium_memzero(next, new_capacity);
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
    sodium_memzero(data_ + new_size, size_ - new_size);
    size_ = new_size;
  } else if (new_size > capacity_) {
    const std::size_t doubled =
        capacity_ > std::numeric_limits<std::size_t>::max() / 2
            ? std::numeric_limits<std::size_t>::max()
            : capacity_ * 2;
    const std::size_t target = capacity_ == 0 ? new_size : std::max(new_size, doubled);
    reallocate(target);
    size_ = new_size;
  } else if (new_size > size_) {
    sodium_memzero(data_ + size_, new_size - size_);
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
        capacity_ > std::numeric_limits<std::size_t>::max() / 2
            ? std::numeric_limits<std::size_t>::max()
            : capacity_ * 2;
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

std::shared_ptr<KeySource> make_hkdf_key_source(std::shared_ptr<SecretSource> secret_source) {
  return std::make_shared<HkdfKeySource>(std::move(secret_source));
}

namespace {

bool is_valid_key_id(std::string_view key_id, std::size_t max_bytes) noexcept {
  return !key_id.empty() && key_id.size() <= max_bytes;
}

bool constant_time_string_equal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  if (a.empty()) {
    return true;
  }
  return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

void validate_header_structure(const StoreHeader& header, const HostCustodyLimits& resolved) {
  archive_fs::require(header.format_version == custody_format::header_format_version);
  archive_fs::require(!header.owner_scope.empty() &&
                      header.owner_scope.size() <= resolved.archive.max_bytes);
  archive_fs::require(header.current_epoch >= 1 && header.current_epoch <= resolved.max_epochs);
  archive_fs::require(header.epochs.size() == header.current_epoch &&
                      header.epochs.size() <= resolved.max_epochs);
  for (std::size_t i = 0; i < header.epochs.size(); ++i) {
    const auto& entry = header.epochs[i];
    archive_fs::require(entry.epoch == i + 1);
    archive_fs::require(is_valid_key_id(entry.wrapping_key_id, resolved.archive.max_bytes));
    archive_fs::require(!entry.wrapped_dek.empty() &&
                        entry.wrapped_dek.size() <= resolved.archive.max_bytes);
  }
}

std::string serialize_header_body(const StoreHeader& header, const HostCustodyLimits& resolved) {
  validate_header_structure(header, resolved);
  archive_internal::Writer w(resolved.archive);
  w.text(custody_format::header_body_domain);
  w.number(header.format_version);
  w.digest(header.store_id);
  w.text(header.owner_scope);
  w.digest(header.descriptor_identity);
  w.number(header.current_epoch);
  w.number(header.epochs.size());
  for (const auto& entry : header.epochs) {
    w.number(entry.epoch);
    w.text(entry.wrapping_key_id);
    w.text(entry.wrapped_dek);
  }
  archive_fs::require(w.data.size() <= resolved.archive.max_bytes - kHeaderTagBytes);
  return std::move(w.data);
}

std::optional<CustodyFault> compute_header_tag(const DataKeyBuffer& dek,
                                               std::string_view body,
                                               std::array<unsigned char, 32>& tag_out) {
  if (!sodium_ready() || !dek.valid()) {
    return CustodyFault::InvalidKeyMaterial;
  }
  const std::string info = encode_text_field(custody_format::header_tag_domain);
  auto mac_key_res = custody_ops::hkdf_sha256_expand_key(dek, info);
  if (std::holds_alternative<CustodyError>(mac_key_res)) {
    return CustodyFault::InvalidKeyMaterial;
  }
  auto& mac_key = std::get<DataKeyBuffer>(mac_key_res);
  const auto* body_ptr =
      body.empty() ? nullptr : reinterpret_cast<const unsigned char*>(body.data());
  const int rc = crypto_auth_hmacsha256(tag_out.data(), body_ptr, body.size(), mac_key.data());
  mac_key.wipe();
  if (rc != 0) {
    sodium_memzero(tag_out.data(), tag_out.size());
    return CustodyFault::InvalidKeyMaterial;
  }
  return std::nullopt;
}

std::optional<CustodyFault> verify_header_tag(
    const DataKeyBuffer& dek,
    std::string_view body,
    const std::array<unsigned char, 32>& expected_tag) {
  std::array<unsigned char, 32> computed_tag{};
  if (auto fault = compute_header_tag(dek, body, computed_tag)) {
    return *fault;
  }
  const bool equal =
      sodium_memcmp(computed_tag.data(), expected_tag.data(), computed_tag.size()) == 0;
  sodium_memzero(computed_tag.data(), computed_tag.size());
  if (!equal) {
    return CustodyFault::Tampered;
  }
  return std::nullopt;
}

CustodyOutcome<StoreHeader> parse_header_with_limits(std::string_view header_bytes,
                                                     const HostCustodyLimits& resolved) {
  std::string store_id_hex;
  try {
    archive_fs::require(header_bytes.size() <= resolved.archive.max_bytes);
    archive_internal::Reader r(header_bytes, resolved.archive);
    archive_fs::require(r.blob() == custody_format::header_body_domain);
    StoreHeader header;
    header.format_version = r.number();
    archive_fs::require(header.format_version == custody_format::header_format_version);
    header.store_id = r.digest();
    store_id_hex = archive_internal::hex(header.store_id);
    header.owner_scope = r.text();
    archive_fs::require(!header.owner_scope.empty() &&
                        header.owner_scope.size() <= resolved.archive.max_bytes);
    header.descriptor_identity = r.digest();
    header.current_epoch = r.number();
    archive_fs::require(header.current_epoch >= 1 && header.current_epoch <= resolved.max_epochs);
    const std::uint64_t epoch_count = r.number();
    archive_fs::require(epoch_count == header.current_epoch &&
                        epoch_count <= resolved.max_epochs &&
                        epoch_count <= r.data.size() / 24);
    header.epochs.reserve(static_cast<std::size_t>(epoch_count));
    for (std::uint64_t i = 0; i < epoch_count; ++i) {
      EpochKeyEntry entry;
      entry.epoch = r.number();
      archive_fs::require(entry.epoch == i + 1);
      entry.wrapping_key_id = r.text();
      archive_fs::require(is_valid_key_id(entry.wrapping_key_id, resolved.archive.max_bytes));
      entry.wrapped_dek = r.text();
      archive_fs::require(!entry.wrapped_dek.empty() &&
                          entry.wrapped_dek.size() <= resolved.archive.max_bytes);
      header.epochs.push_back(std::move(entry));
    }
    header.header_tag = r.digest();
    archive_fs::require(r.data.empty());
    return header;
  } catch (const archive_fs::Rejected&) {
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }
}

CustodyOutcome<std::string> serialize_header_with_limits(const StoreHeader& header,
                                                         const HostCustodyLimits& resolved) {
  const std::string store_id_hex = archive_internal::hex(header.store_id);
  try {
    std::string body = serialize_header_body(header, resolved);
    body.append(reinterpret_cast<const char*>(header.header_tag.data()), header.header_tag.size());
    auto reparsed = parse_header_with_limits(body, resolved);
    if (const auto* err = std::get_if<CustodyError>(&reparsed)) {
      return *err;
    }
    return body;
  } catch (const archive_fs::Rejected&) {
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }
}

}  // namespace

std::optional<CustodyFault> verify_wrapped_dek(KeySource& source,
                                               std::string_view key_id,
                                               std::string_view epoch_aad,
                                               std::string_view wrapped_dek,
                                               const DataKeyBuffer& expected_dek) {
  DataKeyBuffer verified_dek;
  if (!verified_dek.valid() || !expected_dek.valid()) {
    return CustodyFault::InvalidKeyMaterial;
  }
  if (auto fault = source.verify_unwrap_dek(key_id, epoch_aad, wrapped_dek, verified_dek)) {
    verified_dek.wipe();
    if (*fault == CustodyFault::Tampered) {
      return CustodyFault::InvalidKeyMaterial;
    }
    return *fault;
  }
  const bool matches =
      sodium_memcmp(expected_dek.data(), verified_dek.data(), expected_dek.size()) == 0;
  verified_dek.wipe();
  if (!matches) {
    return CustodyFault::InvalidKeyMaterial;
  }
  return std::nullopt;
}

CustodyOutcome<StoreHeader> parse_header(std::string_view header_bytes,
                                         HostCustodyLimits limits) {
  try {
    const HostCustodyLimits resolved = archive_internal::resolve_header_limits(limits);
    return parse_header_with_limits(header_bytes, resolved);
  } catch (const archive_fs::Rejected&) {
    return custody_error(CustodyFault::CorruptHeader);
  }
}

CustodyOutcome<std::string> serialize_header(const StoreHeader& header,
                                             HostCustodyLimits limits) {
  const std::string store_id_hex = archive_internal::hex(header.store_id);
  try {
    const HostCustodyLimits resolved = archive_internal::resolve_header_limits(limits);
    return serialize_header_with_limits(header, resolved);
  } catch (const archive_fs::Rejected&) {
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }
}

CustodyOutcome<std::string> create_header(KeySource& source,
                                          std::string_view owner_scope,
                                          const descriptor::ValidatedDescriptor& descriptor,
                                          HostCustodyLimits limits) {
  if (!sodium_ready()) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  CustodyOperationScope scope(source);
  HostCustodyLimits resolved{};
  archive_internal::Digest descriptor_identity{};
  try {
    resolved = archive_internal::resolve_custody_limits(limits, descriptor);
    descriptor_identity = archive_internal::descriptor_binding_digest(descriptor, resolved.archive);
  } catch (const archive_fs::Rejected&) {
    return custody_error(CustodyFault::CorruptHeader);
  }
  if (owner_scope.empty() || owner_scope.size() > resolved.archive.max_bytes) {
    return custody_error(CustodyFault::InvalidInput);
  }

  StoreHeader header;
  header.format_version = custody_format::header_format_version;
  if (!custody_ops::random_bytes(header.store_id.data(), header.store_id.size())) {
    sodium_memzero(header.store_id.data(), header.store_id.size());
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  const std::string store_id_hex = archive_internal::hex(header.store_id);
  header.owner_scope = std::string(owner_scope);
  header.descriptor_identity = descriptor_identity;
  header.current_epoch = 1;

  DataKeyBuffer dek;
  if (!dek.valid() || !custody_ops::random_bytes(dek.writable_data(), dek.size())) {
    dek.wipe();
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex);
  }

  auto key_id_res = source.active_key_id();
  if (const auto* fault = std::get_if<CustodyFault>(&key_id_res)) {
    dek.wipe();
    return custody_error(*fault, store_id_hex);
  }
  std::string key_id = std::get<std::string>(std::move(key_id_res));
  if (!is_valid_key_id(key_id, resolved.archive.max_bytes)) {
    dek.wipe();
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex, key_id);
  }

  std::string epoch_aad_1;
  try {
    epoch_aad_1 = archive_internal::build_epoch_aad(
        header.store_id, header.owner_scope, header.descriptor_identity, 1, key_id, resolved.archive);
  } catch (const archive_fs::Rejected&) {
    dek.wipe();
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }

  auto wrapped_res = source.wrap_dek(key_id, epoch_aad_1, dek);
  if (const auto* fault = std::get_if<CustodyFault>(&wrapped_res)) {
    dek.wipe();
    return custody_error(*fault, store_id_hex, key_id);
  }
  std::string wrapped_dek = std::get<std::string>(std::move(wrapped_res));
  if (wrapped_dek.empty() || wrapped_dek.size() > resolved.archive.max_bytes) {
    dek.wipe();
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex, key_id);
  }

  if (auto fault = verify_wrapped_dek(source, key_id, epoch_aad_1, wrapped_dek, dek)) {
    dek.wipe();
    return custody_error(*fault, store_id_hex, key_id);
  }

  header.epochs.push_back(EpochKeyEntry{1, std::move(key_id), std::move(wrapped_dek)});

  std::string body;
  try {
    body = serialize_header_body(header, resolved);
  } catch (const archive_fs::Rejected&) {
    dek.wipe();
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }

  if (auto fault = compute_header_tag(dek, body, header.header_tag)) {
    dek.wipe();
    return custody_error(*fault, store_id_hex, header.epochs[0].wrapping_key_id);
  }
  dek.wipe();

  body.append(reinterpret_cast<const char*>(header.header_tag.data()), header.header_tag.size());
  return body;
}

CustodyOutcome<std::string> rotate_header(std::string_view current_header_bytes,
                                          KeySource& source,
                                          std::string_view target_key_id,
                                          std::string_view owner_scope,
                                          const descriptor::ValidatedDescriptor& descriptor,
                                          HostCustodyLimits limits) {
  if (!sodium_ready()) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  CustodyOperationScope scope(source);
  HostCustodyLimits resolved{};
  archive_internal::Digest expected_descriptor_identity{};
  try {
    resolved = archive_internal::resolve_custody_limits(limits, descriptor);
    expected_descriptor_identity =
        archive_internal::descriptor_binding_digest(descriptor, resolved.archive);
  } catch (const archive_fs::Rejected&) {
    return custody_error(CustodyFault::CorruptHeader);
  }

  auto parsed_res = parse_header_with_limits(current_header_bytes, resolved);
  if (const auto* err = std::get_if<CustodyError>(&parsed_res)) {
    return *err;
  }
  StoreHeader header = std::get<StoreHeader>(std::move(parsed_res));
  const std::string store_id_hex = archive_internal::hex(header.store_id);

  if (owner_scope.empty() || owner_scope.size() > resolved.archive.max_bytes) {
    return custody_error(CustodyFault::InvalidInput, store_id_hex);
  }
  if (!constant_time_string_equal(header.owner_scope, owner_scope) ||
      sodium_memcmp(header.descriptor_identity.data(),
                    expected_descriptor_identity.data(),
                    expected_descriptor_identity.size()) != 0) {
    return custody_error(CustodyFault::Tampered, store_id_hex);
  }

  // Only the current epoch's entry is used before the header tag check.
  const EpochKeyEntry& current_entry =
      header.epochs[static_cast<std::size_t>(header.current_epoch - 1)];
  std::string current_aad;
  try {
    current_aad = archive_internal::build_epoch_aad(header.store_id,
                                                    header.owner_scope,
                                                    header.descriptor_identity,
                                                    current_entry.epoch,
                                                    current_entry.wrapping_key_id,
                                                    resolved.archive);
  } catch (const archive_fs::Rejected&) {
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }

  DataKeyBuffer current_dek;
  if (!current_dek.valid()) {
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex, current_entry.wrapping_key_id);
  }
  if (auto fault = source.unwrap_dek(
          current_entry.wrapping_key_id, current_aad, current_entry.wrapped_dek, current_dek)) {
    current_dek.wipe();
    return custody_error(*fault, store_id_hex, current_entry.wrapping_key_id);
  }

  const std::string_view current_body =
      current_header_bytes.substr(0, current_header_bytes.size() - kHeaderTagBytes);
  if (auto fault = verify_header_tag(current_dek, current_body, header.header_tag)) {
    current_dek.wipe();
    return custody_error(*fault, store_id_hex, current_entry.wrapping_key_id);
  }
  current_dek.wipe();

  if (header.current_epoch >= resolved.max_epochs) {
    return custody_error(CustodyFault::EpochLimit, store_id_hex);
  }

  std::string new_key_id;
  if (target_key_id.empty()) {
    auto active_res = source.active_key_id();
    if (const auto* fault = std::get_if<CustodyFault>(&active_res)) {
      return custody_error(*fault, store_id_hex);
    }
    new_key_id = std::get<std::string>(std::move(active_res));
  } else {
    new_key_id = std::string(target_key_id);
  }
  if (!is_valid_key_id(new_key_id, resolved.archive.max_bytes)) {
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex, new_key_id);
  }

  DataKeyBuffer new_dek;
  if (!new_dek.valid() || !custody_ops::random_bytes(new_dek.writable_data(), new_dek.size())) {
    new_dek.wipe();
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex, new_key_id);
  }

  const std::uint64_t new_epoch = header.current_epoch + 1;
  std::string new_aad;
  try {
    new_aad = archive_internal::build_epoch_aad(header.store_id,
                                                header.owner_scope,
                                                header.descriptor_identity,
                                                new_epoch,
                                                new_key_id,
                                                resolved.archive);
  } catch (const archive_fs::Rejected&) {
    new_dek.wipe();
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }

  auto wrapped_res = source.wrap_dek(new_key_id, new_aad, new_dek);
  if (const auto* fault = std::get_if<CustodyFault>(&wrapped_res)) {
    new_dek.wipe();
    return custody_error(*fault, store_id_hex, new_key_id);
  }
  std::string new_wrapped = std::get<std::string>(std::move(wrapped_res));
  if (new_wrapped.empty() || new_wrapped.size() > resolved.archive.max_bytes) {
    new_dek.wipe();
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex, new_key_id);
  }

  if (auto fault = verify_wrapped_dek(source, new_key_id, new_aad, new_wrapped, new_dek)) {
    new_dek.wipe();
    return custody_error(*fault, store_id_hex, new_key_id);
  }

  header.current_epoch = new_epoch;
  header.epochs.push_back(EpochKeyEntry{new_epoch, std::move(new_key_id), std::move(new_wrapped)});

  std::string new_body;
  try {
    new_body = serialize_header_body(header, resolved);
  } catch (const archive_fs::Rejected&) {
    new_dek.wipe();
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }

  if (auto fault = compute_header_tag(new_dek, new_body, header.header_tag)) {
    new_dek.wipe();
    return custody_error(*fault, store_id_hex, header.epochs.back().wrapping_key_id);
  }
  new_dek.wipe();

  new_body.append(reinterpret_cast<const char*>(header.header_tag.data()), header.header_tag.size());
  return new_body;
}

CustodyOutcome<std::string> retire_key_in_header(std::string_view current_header_bytes,
                                                 std::string_view retired_key_id,
                                                 KeySource& source,
                                                 std::string_view owner_scope,
                                                 const descriptor::ValidatedDescriptor& descriptor,
                                                 HostCustodyLimits limits) {
  if (!sodium_ready()) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  CustodyOperationScope scope(source);
  HostCustodyLimits resolved{};
  archive_internal::Digest expected_descriptor_identity{};
  try {
    resolved = archive_internal::resolve_custody_limits(limits, descriptor);
    expected_descriptor_identity =
        archive_internal::descriptor_binding_digest(descriptor, resolved.archive);
  } catch (const archive_fs::Rejected&) {
    return custody_error(CustodyFault::CorruptHeader);
  }

  auto parsed_res = parse_header_with_limits(current_header_bytes, resolved);
  if (const auto* err = std::get_if<CustodyError>(&parsed_res)) {
    return *err;
  }
  StoreHeader header = std::get<StoreHeader>(std::move(parsed_res));
  const std::string store_id_hex = archive_internal::hex(header.store_id);

  if (owner_scope.empty() || owner_scope.size() > resolved.archive.max_bytes) {
    return custody_error(CustodyFault::InvalidInput, store_id_hex);
  }
  if (!constant_time_string_equal(header.owner_scope, owner_scope) ||
      sodium_memcmp(header.descriptor_identity.data(),
                    expected_descriptor_identity.data(),
                    expected_descriptor_identity.size()) != 0) {
    return custody_error(CustodyFault::Tampered, store_id_hex);
  }

  // Only the current epoch's entry is used before the header tag check.
  const std::string current_key_id =
      header.epochs[static_cast<std::size_t>(header.current_epoch - 1)].wrapping_key_id;
  const std::string current_wrapped_dek =
      header.epochs[static_cast<std::size_t>(header.current_epoch - 1)].wrapped_dek;
  std::string current_aad;
  try {
    current_aad = archive_internal::build_epoch_aad(header.store_id,
                                                    header.owner_scope,
                                                    header.descriptor_identity,
                                                    header.current_epoch,
                                                    current_key_id,
                                                    resolved.archive);
  } catch (const archive_fs::Rejected&) {
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }

  DataKeyBuffer current_dek;
  if (!current_dek.valid()) {
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex, current_key_id);
  }
  if (auto fault = source.unwrap_dek(current_key_id, current_aad, current_wrapped_dek, current_dek)) {
    current_dek.wipe();
    return custody_error(*fault, store_id_hex, current_key_id);
  }

  const std::string_view current_body =
      current_header_bytes.substr(0, current_header_bytes.size() - kHeaderTagBytes);
  if (auto fault = verify_header_tag(current_dek, current_body, header.header_tag)) {
    current_dek.wipe();
    return custody_error(*fault, store_id_hex, current_key_id);
  }

  if (!is_valid_key_id(retired_key_id, resolved.archive.max_bytes)) {
    current_dek.wipe();
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex, retired_key_id);
  }

  if (retired_key_id == current_key_id) {
    current_dek.wipe();
    return custody_error(CustodyFault::KeyInUse, store_id_hex, retired_key_id);
  }

  bool any_retired = false;
  for (auto& entry : header.epochs) {
    if (entry.wrapping_key_id != retired_key_id) {
      continue;
    }
    any_retired = true;
    std::string old_aad;
    std::string new_aad;
    try {
      old_aad = archive_internal::build_epoch_aad(header.store_id,
                                                  header.owner_scope,
                                                  header.descriptor_identity,
                                                  entry.epoch,
                                                  retired_key_id,
                                                  resolved.archive);
      new_aad = archive_internal::build_epoch_aad(header.store_id,
                                                  header.owner_scope,
                                                  header.descriptor_identity,
                                                  entry.epoch,
                                                  current_key_id,
                                                  resolved.archive);
    } catch (const archive_fs::Rejected&) {
      current_dek.wipe();
      return custody_error(CustodyFault::CorruptHeader, store_id_hex);
    }

    DataKeyBuffer epoch_dek;
    if (!epoch_dek.valid()) {
      current_dek.wipe();
      return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex, retired_key_id);
    }
    if (auto fault = source.unwrap_dek(retired_key_id, old_aad, entry.wrapped_dek, epoch_dek)) {
      epoch_dek.wipe();
      current_dek.wipe();
      return custody_error(*fault, store_id_hex, retired_key_id);
    }

    auto rewrapped_res = source.wrap_dek(current_key_id, new_aad, epoch_dek);
    if (const auto* fault = std::get_if<CustodyFault>(&rewrapped_res)) {
      epoch_dek.wipe();
      current_dek.wipe();
      return custody_error(*fault, store_id_hex, current_key_id);
    }
    std::string rewrapped = std::get<std::string>(std::move(rewrapped_res));
    if (rewrapped.empty() || rewrapped.size() > resolved.archive.max_bytes) {
      epoch_dek.wipe();
      current_dek.wipe();
      return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex, current_key_id);
    }

    if (auto fault = verify_wrapped_dek(source, current_key_id, new_aad, rewrapped, epoch_dek)) {
      epoch_dek.wipe();
      current_dek.wipe();
      return custody_error(*fault, store_id_hex, current_key_id);
    }
    epoch_dek.wipe();

    entry.wrapping_key_id = current_key_id;
    entry.wrapped_dek = std::move(rewrapped);
  }

  if (!any_retired) {
    current_dek.wipe();
    return std::string(current_header_bytes);
  }

  std::string new_body;
  try {
    new_body = serialize_header_body(header, resolved);
  } catch (const archive_fs::Rejected&) {
    current_dek.wipe();
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }

  if (auto fault = compute_header_tag(current_dek, new_body, header.header_tag)) {
    current_dek.wipe();
    return custody_error(*fault, store_id_hex, current_key_id);
  }
  current_dek.wipe();

  new_body.append(reinterpret_cast<const char*>(header.header_tag.data()), header.header_tag.size());
  return new_body;
}

CustodyOutcome<std::vector<UnwrapRequest>> unwrap_requests(
    std::string_view header_bytes,
    std::string_view owner_scope,
    const descriptor::ValidatedDescriptor& descriptor,
    HostCustodyLimits limits) {
  if (!sodium_ready()) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  HostCustodyLimits resolved{};
  archive_internal::Digest expected_descriptor_identity{};
  try {
    resolved = archive_internal::resolve_custody_limits(limits, descriptor);
    expected_descriptor_identity =
        archive_internal::descriptor_binding_digest(descriptor, resolved.archive);
  } catch (const archive_fs::Rejected&) {
    return custody_error(CustodyFault::CorruptHeader);
  }

  auto parsed_res = parse_header_with_limits(header_bytes, resolved);
  if (const auto* err = std::get_if<CustodyError>(&parsed_res)) {
    return *err;
  }
  const StoreHeader& header = std::get<StoreHeader>(parsed_res);
  const std::string store_id_hex = archive_internal::hex(header.store_id);

  if (owner_scope.empty() || owner_scope.size() > resolved.archive.max_bytes) {
    return custody_error(CustodyFault::InvalidInput, store_id_hex);
  }
  if (!constant_time_string_equal(header.owner_scope, owner_scope) ||
      sodium_memcmp(header.descriptor_identity.data(),
                    expected_descriptor_identity.data(),
                    expected_descriptor_identity.size()) != 0) {
    return custody_error(CustodyFault::Tampered, store_id_hex);
  }

  std::vector<UnwrapRequest> requests;
  requests.reserve(header.epochs.size());
  try {
    for (const auto& entry : header.epochs) {
      std::string aad = archive_internal::build_epoch_aad(header.store_id,
                                                          header.owner_scope,
                                                          header.descriptor_identity,
                                                          entry.epoch,
                                                          entry.wrapping_key_id,
                                                          resolved.archive);
      requests.push_back(UnwrapRequest{entry.wrapping_key_id, std::move(aad), entry.wrapped_dek});
    }
  } catch (const archive_fs::Rejected&) {
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }
  return requests;
}

namespace custody_ops {

bool available() noexcept {
  return true;
}

bool random_bytes(void* data, std::size_t size) noexcept {
#ifdef SP_CUSTODY_TEST_SEAMS
  try {
    g_random_draw_sizes.push_back(size);
  } catch (...) {
    return false;
  }
  if (g_random_fail_on_call > 0) {
    ++g_random_call_count;
    if (g_random_call_count == g_random_fail_on_call) {
      return false;
    }
  }
  if (!g_random_overrides.empty()) {
    std::string next = std::move(g_random_overrides.front());
    g_random_overrides.pop_front();
    if (next.size() != size) {
      crypto::cleanse(next.data(), next.size());
      return false;
    }
    if (size > 0) {
      std::memcpy(data, next.data(), size);
    }
    crypto::cleanse(next.data(), next.size());
    return true;
  }
#endif
  return crypto::random_bytes(data, size);
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

void fail_random_on_call(std::size_t call_index) noexcept {
  g_random_call_count = 0;
  g_random_fail_on_call = call_index;
}

void override_next_random_bytes(std::string_view bytes) {
  g_random_overrides.emplace_back(bytes.data(), bytes.size());
}

std::vector<std::size_t> recorded_random_draws() {
  return g_random_draw_sizes;
}

void reset_random_seam() noexcept {
  g_random_call_count = 0;
  g_random_fail_on_call = 0;
  for (auto& entry : g_random_overrides) {
    crypto::cleanse(entry.data(), entry.size());
  }
  g_random_overrides.clear();
  g_random_draw_sizes.clear();
  g_key_alloc_call_count = 0;
  g_key_alloc_fail_on_call = 0;
  g_corrupt_next_cached_kek = false;
}

void fail_key_alloc_on_call(std::size_t call_index) noexcept {
  g_key_alloc_call_count = 0;
  g_key_alloc_fail_on_call = call_index;
}

void corrupt_next_cached_kek() noexcept {
  g_corrupt_next_cached_kek = true;
}

std::vector<std::size_t> live_key_allocation_sizes() {
  std::vector<std::size_t> out;
  out.reserve(g_live_key_allocs.size());
  for (const auto& [ptr, size] : g_live_key_allocs) {
    (void)ptr;
    out.push_back(size);
  }
  return out;
}

CustodyOutcome<std::string> hkdf_sha256_expand(const DataKeyBuffer& prk,
                                               std::string_view info,
                                               std::size_t out_len) {
  if (!sodium_ready() || !prk.valid() || out_len > crypto_kdf_hkdf_sha256_BYTES_MAX) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  std::string out(out_len, '\0');
  const char* info_ptr = info.empty() ? nullptr : info.data();
  if (crypto_kdf_hkdf_sha256_expand(reinterpret_cast<unsigned char*>(out.data()),
                                    out.size(),
                                    info_ptr,
                                    info.size(),
                                    prk.data()) != 0) {
    sodium_memzero(out.data(), out.size());
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  return out;
}
#endif

CustodyOutcome<AuthenticatedHeaderState> open_authenticated_header(
    std::string_view header_bytes,
    KeySource& source,
    std::string_view owner_scope,
    const descriptor::ValidatedDescriptor& descriptor,
    HostCustodyLimits limits) {
  if (!sodium_ready()) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  CustodyOperationScope scope(source);
  HostCustodyLimits resolved{};
  archive_internal::Digest expected_descriptor_identity{};
  try {
    resolved = archive_internal::resolve_custody_limits(limits, descriptor);
    expected_descriptor_identity =
        archive_internal::descriptor_binding_digest(descriptor, resolved.archive);
  } catch (const archive_fs::Rejected&) {
    return custody_error(CustodyFault::CorruptHeader);
  }

  auto parsed_res = parse_header_with_limits(header_bytes, resolved);
  if (const auto* err = std::get_if<CustodyError>(&parsed_res)) {
    return *err;
  }
  StoreHeader header = std::get<StoreHeader>(std::move(parsed_res));
  const std::string store_id_hex = archive_internal::hex(header.store_id);

  if (owner_scope.empty() || owner_scope.size() > resolved.archive.max_bytes) {
    return custody_error(CustodyFault::InvalidInput, store_id_hex);
  }
  if (!constant_time_string_equal(header.owner_scope, owner_scope) ||
      sodium_memcmp(header.descriptor_identity.data(),
                    expected_descriptor_identity.data(),
                    expected_descriptor_identity.size()) != 0) {
    return custody_error(CustodyFault::Tampered, store_id_hex);
  }

  archive_internal::EpochKeyTable epoch_deks(header.epochs.size());
  if (!epoch_deks.valid()) {
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex);
  }

  DataKeyBuffer scratch_dek;
  if (!scratch_dek.valid()) {
    epoch_deks.release();
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex);
  }

  // Only the current epoch's entry is used before the header tag check.
  const EpochKeyEntry& current_entry =
      header.epochs[static_cast<std::size_t>(header.current_epoch - 1)];
  std::string current_aad;
  try {
    current_aad = archive_internal::build_epoch_aad(header.store_id,
                                                    header.owner_scope,
                                                    header.descriptor_identity,
                                                    current_entry.epoch,
                                                    current_entry.wrapping_key_id,
                                                    resolved.archive);
  } catch (const archive_fs::Rejected&) {
    scratch_dek.wipe();
    epoch_deks.release();
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }

  if (auto fault = source.unwrap_dek(
          current_entry.wrapping_key_id, current_aad, current_entry.wrapped_dek, scratch_dek)) {
    scratch_dek.wipe();
    epoch_deks.release();
    return custody_error(*fault, store_id_hex, current_entry.wrapping_key_id);
  }

  const std::string_view header_body =
      header_bytes.substr(0, header_bytes.size() - kHeaderTagBytes);
  if (auto fault = verify_header_tag(scratch_dek, header_body, header.header_tag)) {
    scratch_dek.wipe();
    epoch_deks.release();
    return custody_error(*fault, store_id_hex, current_entry.wrapping_key_id);
  }

  unsigned char* current_slot = epoch_deks.writable_epoch_dek(header.current_epoch);
  if (current_slot == nullptr) {
    scratch_dek.wipe();
    epoch_deks.release();
    return custody_error(CustodyFault::CorruptHeader, store_id_hex);
  }
  std::memcpy(current_slot, scratch_dek.data(), DataKeyBuffer::kSize);
  scratch_dek.wipe();

  for (std::uint64_t epoch = 1; epoch < header.current_epoch; ++epoch) {
    const EpochKeyEntry& entry = header.epochs[static_cast<std::size_t>(epoch - 1)];
    std::string epoch_aad;
    try {
      epoch_aad = archive_internal::build_epoch_aad(header.store_id,
                                                    header.owner_scope,
                                                    header.descriptor_identity,
                                                    entry.epoch,
                                                    entry.wrapping_key_id,
                                                    resolved.archive);
    } catch (const archive_fs::Rejected&) {
      scratch_dek.wipe();
      epoch_deks.release();
      return custody_error(CustodyFault::CorruptHeader, store_id_hex);
    }

    if (auto fault = source.unwrap_dek(
            entry.wrapping_key_id, epoch_aad, entry.wrapped_dek, scratch_dek)) {
      scratch_dek.wipe();
      epoch_deks.release();
      return custody_error(*fault, store_id_hex, entry.wrapping_key_id);
    }

    unsigned char* slot = epoch_deks.writable_epoch_dek(entry.epoch);
    if (slot == nullptr) {
      scratch_dek.wipe();
      epoch_deks.release();
      return custody_error(CustodyFault::CorruptHeader, store_id_hex);
    }
    std::memcpy(slot, scratch_dek.data(), DataKeyBuffer::kSize);
    scratch_dek.wipe();
  }

  scratch_dek.release();
  return AuthenticatedHeaderState{std::move(header), resolved, std::move(epoch_deks)};
}

std::array<unsigned char, 32> compute_record_reference_digest(
    std::string_view envelope_bytes) noexcept {
  std::array<unsigned char, 32> digest{};
  crypto_hash_sha256_state state{};
  crypto_hash_sha256_init(&state);
  unsigned char domain_len_be[8];
  const std::uint64_t domain_len = custody_format::ref_domain.size();
  for (unsigned i = 0; i < 8; ++i) {
    domain_len_be[7 - i] = static_cast<unsigned char>(domain_len >> (i * 8));
  }
  crypto_hash_sha256_update(&state, domain_len_be, sizeof(domain_len_be));
  crypto_hash_sha256_update(
      &state,
      reinterpret_cast<const unsigned char*>(custody_format::ref_domain.data()),
      custody_format::ref_domain.size());
  if (!envelope_bytes.empty()) {
    crypto_hash_sha256_update(
        &state,
        reinterpret_cast<const unsigned char*>(envelope_bytes.data()),
        envelope_bytes.size());
  }
  crypto_hash_sha256_final(&state, digest.data());
  sodium_memzero(&state, sizeof(state));
  return digest;
}

CustodyOutcome<DataKeyBuffer> hkdf_sha256_extract(std::string_view salt, std::string_view ikm) {
  if (!sodium_ready()) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  DataKeyBuffer prk;
  if (!prk.valid()) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  const auto* salt_ptr = salt.empty() ? nullptr : reinterpret_cast<const unsigned char*>(salt.data());
  const auto* ikm_ptr = ikm.empty() ? nullptr : reinterpret_cast<const unsigned char*>(ikm.data());
  if (crypto_kdf_hkdf_sha256_extract(prk.writable_data(), salt_ptr, salt.size(), ikm_ptr, ikm.size()) != 0) {
    prk.wipe();
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  return prk;
}

CustodyOutcome<DataKeyBuffer> hkdf_sha256_expand_key(const DataKeyBuffer& prk,
                                                     std::string_view info) {
  if (!sodium_ready() || !prk.valid()) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  DataKeyBuffer out;
  if (!out.valid()) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  const char* info_ptr = info.empty() ? nullptr : info.data();
  if (crypto_kdf_hkdf_sha256_expand(out.writable_data(),
                                    out.size(),
                                    info_ptr,
                                    info.size(),
                                    prk.data()) != 0) {
    out.wipe();
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  return out;
}

CustodyOutcome<DataKeyBuffer> derive_kek(const SecretBuffer& secret,
                                         std::size_t written_size,
                                         const std::array<unsigned char, 32>& store_id) {
  if (!secret.valid() || written_size < SecretBuffer::kMinSecretSize ||
      written_size > SecretBuffer::kSize) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  const std::string_view salt(reinterpret_cast<const char*>(store_id.data()), store_id.size());
  const std::string_view ikm(reinterpret_cast<const char*>(secret.data()), written_size);
  auto prk_res = hkdf_sha256_extract(salt, ikm);
  if (const auto* err = std::get_if<CustodyError>(&prk_res)) {
    return *err;
  }
  auto& prk = std::get<DataKeyBuffer>(prk_res);
  const std::string info = encode_text_field(custody_format::kek_domain);
  auto kek_res = hkdf_sha256_expand_key(prk, info);
  prk.wipe();
  return kek_res;
}

CustodyOutcome<std::string> aead_xchacha20poly1305_encrypt(
    const unsigned char* key_32,
    const std::array<unsigned char, 24>& nonce,
    std::string_view aad,
    std::string_view plaintext,
    std::string_view store_id_hex) {
  if (!sodium_ready() || key_32 == nullptr) {
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex);
  }
  std::string out(plaintext.size() + kAeadTagBytes, '\0');
  unsigned long long clen = 0;
  const auto* m_ptr = plaintext.empty() ? nullptr : reinterpret_cast<const unsigned char*>(plaintext.data());
  const auto* ad_ptr = aad.empty() ? nullptr : reinterpret_cast<const unsigned char*>(aad.data());
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(
          reinterpret_cast<unsigned char*>(out.data()),
          &clen,
          m_ptr,
          plaintext.size(),
          ad_ptr,
          aad.size(),
          nullptr,
          nonce.data(),
          key_32) != 0 ||
      clen != out.size()) {
    sodium_memzero(out.data(), out.size());
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex);
  }
  return out;
}

CustodyOutcome<std::string> aead_xchacha20poly1305_encrypt(
    const DataKeyBuffer& key,
    const std::array<unsigned char, 24>& nonce,
    std::string_view aad,
    std::string_view plaintext) {
  return aead_xchacha20poly1305_encrypt(key.data(), nonce, aad, plaintext);
}

CustodyOutcome<CleansedBuffer> aead_xchacha20poly1305_decrypt(
    const unsigned char* key_32,
    const std::array<unsigned char, 24>& nonce,
    std::string_view aad,
    std::string_view ciphertext_and_tag,
    std::string_view store_id_hex) {
  if (!sodium_ready() || key_32 == nullptr) {
    return custody_error(CustodyFault::InvalidKeyMaterial, store_id_hex);
  }
  if (ciphertext_and_tag.size() < kAeadTagBytes) {
    return custody_error(CustodyFault::Tampered, store_id_hex);
  }
  const std::size_t mlen_expected = ciphertext_and_tag.size() - kAeadTagBytes;
  CleansedBuffer plaintext(mlen_expected);
  unsigned long long mlen = 0;
  const auto* ad_ptr = aad.empty() ? nullptr : reinterpret_cast<const unsigned char*>(aad.data());
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(
          plaintext.data(),
          &mlen,
          nullptr,
          reinterpret_cast<const unsigned char*>(ciphertext_and_tag.data()),
          ciphertext_and_tag.size(),
          ad_ptr,
          aad.size(),
          nonce.data(),
          key_32) != 0 ||
      mlen != mlen_expected) {
    plaintext.wipe();
    return custody_error(CustodyFault::Tampered, store_id_hex);
  }
  return plaintext;
}

CustodyOutcome<CleansedBuffer> aead_xchacha20poly1305_decrypt(
    const DataKeyBuffer& key,
    const std::array<unsigned char, 24>& nonce,
    std::string_view aad,
    std::string_view ciphertext_and_tag) {
  return aead_xchacha20poly1305_decrypt(key.data(), nonce, aad, ciphertext_and_tag);
}

}  // namespace custody_ops
}  // namespace sp
