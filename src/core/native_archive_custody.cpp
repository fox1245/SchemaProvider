#include "core/native_archive_custody.h"
#include "core/native_archive_custody_format.h"
#include "core/native_archive_custody_internal.h"
#include "crypto/crypto.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>

namespace sp {
namespace {

constexpr std::size_t kNonceBytes = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr std::size_t kAeadTagBytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;
constexpr std::size_t kWrappedDekBytes = kNonceBytes + DataKeyBuffer::kSize + kAeadTagBytes;

static_assert(DataKeyBuffer::kSize == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
static_assert(DataKeyBuffer::kSize == crypto_kdf_hkdf_sha256_KEYBYTES);
static_assert(SecretBuffer::kMinSecretSize == DataKeyBuffer::kSize);

bool sodium_ready() noexcept {
  return sodium_init() >= 0;
}

#ifdef SP_CUSTODY_TEST_SEAMS
thread_local custody_ops::ReleaseObserver g_release_observer = nullptr;
thread_local void* g_release_observer_context = nullptr;

thread_local std::size_t g_random_call_count = 0;
thread_local std::size_t g_random_fail_on_call = 0;
thread_local std::string g_random_override;
thread_local bool g_random_override_active = false;

thread_local std::size_t g_key_alloc_call_count = 0;
thread_local std::size_t g_key_alloc_fail_on_call = 0;

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
  const std::size_t domain_len = custody_format::epoch_aad_domain.size();
  const std::size_t prefix_len = 8 + domain_len + 8;
  if (epoch_aad.size() < prefix_len + store_id_out.size()) {
    return false;
  }
  char expected_prefix[64];
  if (prefix_len > sizeof(expected_prefix)) {
    return false;
  }
  write_be64(expected_prefix, domain_len);
  std::memcpy(expected_prefix + 8, custody_format::epoch_aad_domain.data(), domain_len);
  write_be64(expected_prefix + 8 + domain_len, custody_format::header_format_version);
  if (std::memcmp(epoch_aad.data(), expected_prefix, prefix_len) != 0) {
    return false;
  }
  std::memcpy(store_id_out.data(), epoch_aad.data() + prefix_len, store_id_out.size());
  return true;
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
    if (auto fault = obtain_kek(key_id, epoch_aad, kek)) {
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
    if (auto fault = obtain_kek(key_id, epoch_aad, kek)) {
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
                                         DataKeyBuffer& kek_out) {
    std::array<unsigned char, 32> store_id{};
    if (!extract_store_id_from_epoch_aad(epoch_aad, store_id) || !kek_out.valid()) {
      return CustodyFault::InvalidKeyMaterial;
    }
    const CacheKey cache_key{store_id, std::string(key_id)};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_scopes_ > 0) {
        if (auto it = cached_keks_.find(cache_key); it != cached_keks_.end()) {
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
      if (cached.valid()) {
        std::memcpy(cached.writable_data(), kek_out.data(), cached.size());
        cached_keks_.emplace(cache_key, std::move(cached));
      }
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
      break;
    case CustodyFault::KeyNotFound:
      out.error.kind = ErrorKind::NotFound;
      out.error.safe_message = "Host native archive key was not found";
      out.error.reason = "archive.custody.key_not_found";
      out.store_id = std::string(store_id);
      out.key_id = std::string(key_id);
      break;
    case CustodyFault::InvalidKeyMaterial:
      out.error.kind = ErrorKind::Permission;
      out.error.safe_message = "Host native archive key material is invalid";
      out.error.reason = "archive.custody.key_invalid";
      out.store_id = std::string(store_id);
      out.key_id = std::string(key_id);
      break;
    case CustodyFault::KeyInUse:
      out.error.kind = ErrorKind::Permission;
      out.error.safe_message = "Host native archive active wrapping key is in use";
      out.error.reason = "archive.custody.key_in_use";
      out.store_id = std::string(store_id);
      out.key_id = std::string(key_id);
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
    case CustodyFault::SourceUnavailable:
      out.error.kind = ErrorKind::Transport;
      out.error.retry_class = RetryClass::Transient;
      out.error.safe_message = "Host native archive key source is unavailable";
      out.error.reason = "archive.custody.source_unavailable";
      out.store_id = std::string(store_id);
      out.key_id = std::string(key_id);
      break;
    case CustodyFault::SourceRateLimited:
      out.error.kind = ErrorKind::RateLimited;
      out.error.retry_class = RetryClass::Transient;
      out.error.safe_message = "Host native archive key source is rate limited";
      out.error.reason = "archive.custody.source_rate_limited";
      out.store_id = std::string(store_id);
      out.key_id = std::string(key_id);
      break;
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

namespace custody_ops {

bool available() noexcept {
  return true;
}

bool random_bytes(void* data, std::size_t size) noexcept {
#ifdef SP_CUSTODY_TEST_SEAMS
  if (g_random_fail_on_call > 0) {
    ++g_random_call_count;
    if (g_random_call_count == g_random_fail_on_call) {
      return false;
    }
  }
  if (g_random_override_active) {
    g_random_override_active = false;
    if (g_random_override.size() != size) {
      crypto::cleanse(g_random_override.data(), g_random_override.size());
      g_random_override.clear();
      return false;
    }
    if (size > 0) {
      std::memcpy(data, g_random_override.data(), size);
    }
    crypto::cleanse(g_random_override.data(), g_random_override.size());
    g_random_override.clear();
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

void fail_random_on_call(std::size_t call_index) noexcept {
  g_random_call_count = 0;
  g_random_fail_on_call = call_index;
}

void override_next_random_bytes(std::string_view bytes) {
  g_random_override.assign(bytes.data(), bytes.size());
  g_random_override_active = true;
}

void reset_random_seam() noexcept {
  g_random_call_count = 0;
  g_random_fail_on_call = 0;
  if (!g_random_override.empty()) {
    crypto::cleanse(g_random_override.data(), g_random_override.size());
    g_random_override.clear();
  }
  g_random_override_active = false;
  g_key_alloc_call_count = 0;
  g_key_alloc_fail_on_call = 0;
}

void fail_key_alloc_on_call(std::size_t call_index) noexcept {
  g_key_alloc_call_count = 0;
  g_key_alloc_fail_on_call = call_index;
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
    const DataKeyBuffer& key,
    const std::array<unsigned char, 24>& nonce,
    std::string_view aad,
    std::string_view plaintext) {
  if (!sodium_ready() || !key.valid()) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
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
          key.data()) != 0 ||
      clen != out.size()) {
    sodium_memzero(out.data(), out.size());
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  return out;
}

CustodyOutcome<CleansedBuffer> aead_xchacha20poly1305_decrypt(
    const DataKeyBuffer& key,
    const std::array<unsigned char, 24>& nonce,
    std::string_view aad,
    std::string_view ciphertext_and_tag) {
  if (!sodium_ready() || !key.valid()) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  if (ciphertext_and_tag.size() < kAeadTagBytes) {
    return custody_error(CustodyFault::Tampered);
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
          key.data()) != 0 ||
      mlen != mlen_expected) {
    plaintext.wipe();
    return custody_error(CustodyFault::Tampered);
  }
  return plaintext;
}

}  // namespace custody_ops
}  // namespace sp
