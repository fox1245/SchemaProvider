#pragma once
#include "core/native_archive_custody.h"
#include "core/native_archive_custody_format.h"
#include "core/native_archive_fs.h"
#include "crypto/crypto.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include "sp/config_defaults.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sp {

namespace custody_ops {
std::array<unsigned char, 32> compute_record_reference_digest(
    std::string_view envelope_bytes) noexcept;
}  // namespace custody_ops

namespace archive_internal {

using Digest = std::array<unsigned char, 32>;

inline std::string hex(const Digest& bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string text;
  text.reserve(64);
  for (auto b : bytes) {
    text += digits[b >> 4];
    text += digits[b & 15];
  }
  return text;
}

inline std::string quote_key_id_for_message(std::string_view key_id) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(key_id.size() + 2);
  out.push_back('"');
  for (unsigned char c : key_id) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c >= 0x20 && c <= 0x7e) {
      out.push_back(static_cast<char>(c));
    } else {
      out.append("\\x");
      out.push_back(digits[c >> 4]);
      out.push_back(digits[c & 15]);
    }
  }
  out.push_back('"');
  return out;
}

template <class Buffer = std::string>
class BasicWriter {
 public:
  explicit BasicWriter(NativeArchiveLimits limits) : limits(limits) {}
  void raw(std::string_view v) {
    archive_fs::require(v.size() <= limits.max_bytes - data.size());
    data.append(v);
  }
  void number(std::uint64_t n) {
    char b[8];
    for (unsigned i = 0; i < 8; ++i) b[7 - i] = static_cast<char>(n >> (i * 8));
    raw({b, 8});
  }
  void boolean(bool v) { number(v ? 1 : 0); }
  void text(std::string_view v) {
    number(v.size());
    raw(v);
  }
  void optional(const std::optional<std::string>& v) {
    boolean(v.has_value());
    if (v) text(*v);
  }
  void strings(const std::vector<std::string>& v) {
    archive_fs::require(v.size() <= limits.max_parts);
    number(v.size());
    for (const auto& s : v) text(s);
  }
  void document(const std::shared_ptr<const json::Document>& v) {
    boolean(bool(v));
    if (!v) return;
    const auto length_offset = data.size();
    number(0);
    const auto start = data.size();
    if constexpr (std::is_same_v<Buffer, std::string>) {
      json::BoundedWriter writer({limits.max_bytes, limits.max_json_depth}, &data);
      writer.value(v->root());
      archive_fs::require(writer.ok());
    } else {
      json::BoundedWriter writer(
          {limits.max_bytes, limits.max_json_depth}, &append_json_chunk, &data, data.size());
      writer.value(v->root());
      archive_fs::require(writer.ok());
    }
    const std::uint64_t length = data.size() - start;
    auto* out = reinterpret_cast<unsigned char*>(data.data()) + length_offset;
    for (unsigned i = 0; i < 8; ++i)
      out[7 - i] = static_cast<unsigned char>(length >> (i * 8));
  }
  void digest(const Digest& v) { raw({reinterpret_cast<const char*>(v.data()), v.size()}); }
  NativeArchiveLimits limits;
  Buffer data;

 private:
  static void append_json_chunk(void* context, std::string_view chunk) {
    static_cast<Buffer*>(context)->append(chunk);
  }
};

using Writer = BasicWriter<std::string>;
using CleansedWriter = BasicWriter<CleansedBuffer>;

class EpochKeyTable final {
 public:
  static constexpr std::size_t kDekBytes = DataKeyBuffer::kSize;

  EpochKeyTable() noexcept = default;
  explicit EpochKeyTable(std::size_t epoch_count) noexcept
      : bytes_(epoch_count > 0 && epoch_count <= std::numeric_limits<std::size_t>::max() / kDekBytes
                   ? custody_detail::sodium_key_alloc(epoch_count * kDekBytes)
                   : nullptr),
        epoch_count_(bytes_ != nullptr ? epoch_count : 0) {}

  EpochKeyTable(const EpochKeyTable&) = delete;
  EpochKeyTable& operator=(const EpochKeyTable&) = delete;

  EpochKeyTable(EpochKeyTable&& other) noexcept
      : bytes_(other.bytes_), epoch_count_(other.epoch_count_) {
    other.bytes_ = nullptr;
    other.epoch_count_ = 0;
  }

  EpochKeyTable& operator=(EpochKeyTable&& other) noexcept {
    if (this != &other) {
      release();
      bytes_ = other.bytes_;
      epoch_count_ = other.epoch_count_;
      other.bytes_ = nullptr;
      other.epoch_count_ = 0;
    }
    return *this;
  }

  ~EpochKeyTable() noexcept { release(); }

  [[nodiscard]] bool valid() const noexcept { return bytes_ != nullptr && epoch_count_ > 0; }
  [[nodiscard]] std::size_t epoch_count() const noexcept { return epoch_count_; }
  [[nodiscard]] std::size_t byte_size() const noexcept { return epoch_count_ * kDekBytes; }

  [[nodiscard]] unsigned char* writable_epoch_dek(std::uint64_t epoch) noexcept {
    if (bytes_ == nullptr || epoch < 1 || epoch > epoch_count_) {
      return nullptr;
    }
    return bytes_ + static_cast<std::size_t>(epoch - 1) * kDekBytes;
  }

  [[nodiscard]] const unsigned char* epoch_dek(std::uint64_t epoch) const noexcept {
    if (bytes_ == nullptr || epoch < 1 || epoch > epoch_count_) {
      return nullptr;
    }
    return bytes_ + static_cast<std::size_t>(epoch - 1) * kDekBytes;
  }

  void wipe() noexcept {
    if (bytes_ != nullptr && epoch_count_ > 0) {
      custody_detail::sodium_key_wipe(bytes_, epoch_count_ * kDekBytes);
    }
  }

  void release() noexcept {
    if (bytes_ != nullptr) {
      custody_detail::sodium_key_free(bytes_, epoch_count_ * kDekBytes);
      bytes_ = nullptr;
      epoch_count_ = 0;
    }
  }

 private:
  unsigned char* bytes_ = nullptr;
  std::size_t epoch_count_ = 0;
};

class Reader {
 public:
  Reader(std::string_view data, NativeArchiveLimits limits) : data(data), limits(limits) {}
  std::string_view raw(std::size_t n) {
    archive_fs::require(n <= data.size());
    auto out = data.substr(0, n);
    data.remove_prefix(n);
    return out;
  }
  std::uint64_t number() {
    std::uint64_t n = 0;
    for (unsigned char c : raw(8)) n = (n << 8) | c;
    return n;
  }
  bool boolean() {
    auto n = number();
    archive_fs::require(n <= 1);
    return n != 0;
  }
  std::size_t count(std::size_t limit) {
    auto n = number();
    archive_fs::require(n <= limit && n <= data.size() / 8);
    return static_cast<std::size_t>(n);
  }
  std::string_view blob() {
    auto n = number();
    archive_fs::require(n <= data.size() && n <= limits.max_bytes);
    return raw(static_cast<std::size_t>(n));
  }
  std::string text() { return std::string(blob()); }
  std::optional<std::string> optional() {
    if (!boolean()) return {};
    return text();
  }
  std::vector<std::string> strings() {
    std::vector<std::string> v;
    auto n = count(limits.max_parts);
    v.reserve(n);
    while (n--) v.push_back(text());
    return v;
  }
  std::shared_ptr<const json::Document> document(bool diagnostic = false) {
    if (!boolean()) return {};
    auto parsed = json::parse(blob(), {limits.max_bytes, limits.max_json_depth});
    if (auto* accepted = std::get_if<json::Document>(&parsed))
      return std::make_shared<const json::Document>(std::move(*accepted));
    auto& rejected = std::get<json::ParseError>(parsed);
    archive_fs::require(diagnostic && rejected.code == json::ParseCode::DuplicateKey &&
                        rejected.context.root().valid());
    return std::make_shared<const json::Document>(std::move(rejected.context));
  }
  Digest digest() {
    Digest v{};
    auto bytes = raw(v.size());
    std::memcpy(v.data(), bytes.data(), v.size());
    return v;
  }
  template <class E>
  E enumeration(unsigned maximum) {
    auto n = number();
    archive_fs::require(n <= maximum);
    return static_cast<E>(n);
  }
  std::string_view data;
  NativeArchiveLimits limits;
};

inline NativeArchiveLimits resolve_archive_limits(
    NativeArchiveLimits l,
    const descriptor::CodecResources& resources) {
  if (!l.max_bytes) l.max_bytes = resources.native_bytes;
  if (!l.max_messages) l.max_messages = resources.request_messages;
  if (!l.max_parts) l.max_parts = resources.request_parts;
  if (!l.max_json_depth) l.max_json_depth = resources.native_depth;
  if (!l.max_records) l.max_records = resources.request_messages;
  if (!l.max_store_bytes) {
    archive_fs::require(l.max_records && l.max_bytes <= UINT64_MAX / l.max_records);
    l.max_store_bytes = static_cast<std::uint64_t>(l.max_bytes) * l.max_records;
  }
  archive_fs::require(resources.request_messages &&
                      resources.native_bytes <= UINT64_MAX / resources.request_messages);
  const auto storage_ceiling =
      static_cast<std::uint64_t>(resources.native_bytes) * resources.request_messages;
  archive_fs::require(l.max_bytes > 0 && l.max_bytes <= resources.native_bytes &&
                      l.max_messages > 0 && l.max_messages <= resources.request_messages &&
                      l.max_parts > 0 && l.max_parts <= resources.request_parts &&
                      l.max_json_depth > 0 && l.max_json_depth <= resources.native_depth &&
                      l.max_records > 0 && l.max_records <= resources.request_messages &&
                      l.max_store_bytes >= l.max_bytes && l.max_store_bytes <= storage_ceiling);
  return l;
}

inline NativeArchiveLimits resolve_archive_limits(NativeArchiveLimits l,
                                                  const descriptor::ValidatedDescriptor& v) {
  archive_fs::require(v.policy() != nullptr);
  return resolve_archive_limits(l, v.policy()->resources());
}

inline NativeArchiveLimits resolve_default_archive_limits(NativeArchiveLimits l) {
  const auto& policy = descriptor::builtin_policy();
  archive_fs::require(policy != nullptr);
  return resolve_archive_limits(l, policy->resources());
}

inline std::size_t resolve_max_epochs(std::size_t max_epochs) {
  archive_fs::require(config_defaults::archive_custody_max_epochs <=
                      std::numeric_limits<std::size_t>::max());
  const auto default_max_epochs =
      static_cast<std::size_t>(config_defaults::archive_custody_max_epochs);
  if (!max_epochs) {
    max_epochs = default_max_epochs;
  }
  archive_fs::require(max_epochs > 0 && max_epochs <= default_max_epochs);
  return max_epochs;
}

inline HostCustodyLimits resolve_custody_limits(HostCustodyLimits l,
                                                const descriptor::ValidatedDescriptor& v) {
  l.archive = resolve_archive_limits(l.archive, v);
  l.max_epochs = resolve_max_epochs(l.max_epochs);
  return l;
}

inline HostCustodyLimits resolve_header_limits(HostCustodyLimits l) {
  l.archive = resolve_default_archive_limits(l.archive);
  l.max_epochs = resolve_max_epochs(l.max_epochs);
  return l;
}

inline Digest descriptor_binding_digest(const descriptor::ValidatedDescriptor& d,
                                        NativeArchiveLimits limits) {
  Writer w(limits);
  w.text("sp.native.archive.descriptor.v2");
  w.text(d.id());
  w.number(d.revision());
  w.text(d.family());
  w.text(d.base_url());
  w.text(d.path(false));
  w.text(d.path(true));
  w.text(d.policy()->identity());
  w.number(d.headers().size());
  for (const auto& [name, value] : d.headers()) {
    w.text(name);
    w.text(value);
  }
  w.text(d.request_model_member());
  w.text(d.request_messages_member());
  w.text(d.request_stream_member());
  w.text(d.max_output_tokens_member());
  w.number(d.usage_path().size());
  for (const auto& segment : d.usage_path()) w.text(segment);
  w.number(d.stop_mappings().size());
  for (const auto& [raw, kind] : d.stop_mappings()) {
    w.text(raw);
    w.number(static_cast<unsigned>(kind));
  }
  return crypto::sha256(w.data);
}

inline std::string descriptor_binding(const descriptor::ValidatedDescriptor& d,
                                      NativeArchiveLimits limits) {
  const Digest digest = descriptor_binding_digest(d, limits);
  return {reinterpret_cast<const char*>(digest.data()), digest.size()};
}

inline void write_epoch_aad_version_prefix(Writer& w) {
  w.text(custody_format::epoch_aad_domain);
  w.number(custody_format::header_format_version);
}

inline std::string build_epoch_aad(const Digest& store_id,
                                   std::string_view owner_scope,
                                   const Digest& descriptor_identity,
                                   std::uint64_t epoch,
                                   std::string_view wrapping_key_id,
                                   NativeArchiveLimits limits) {
  Writer w(limits);
  write_epoch_aad_version_prefix(w);
  w.digest(store_id);
  w.text(owner_scope);
  w.digest(descriptor_identity);
  w.number(epoch);
  w.text(wrapping_key_id);
  return std::move(w.data);
}

inline std::string build_record_aad(const Digest& store_id,
                                    std::uint64_t header_epoch,
                                    std::string_view owner_scope,
                                    const Digest& descriptor_identity,
                                    std::string_view binding,
                                    NativeArchiveLimits limits) {
  archive_fs::require(!owner_scope.empty() && owner_scope.size() <= limits.max_bytes);
  archive_fs::require(binding.size() <= limits.max_bytes);
  NativeArchiveLimits aad_limits = limits;
  const std::size_t fixed_overhead =
      8 + custody_format::record_aad_domain.size() + 8 + store_id.size() + 8 + 8 +
      descriptor_identity.size() + 8;
  aad_limits.max_bytes = fixed_overhead + owner_scope.size() + binding.size();
  Writer w(aad_limits);
  w.text(custody_format::record_aad_domain);
  w.number(custody_format::record_format_version);
  w.digest(store_id);
  w.number(header_epoch);
  w.text(owner_scope);
  w.digest(descriptor_identity);
  w.text(binding);
  return std::move(w.data);
}

inline std::string serialize_record_envelope(std::uint64_t header_epoch,
                                             const std::array<unsigned char, 24>& nonce,
                                             std::string_view ciphertext_and_tag) {
  NativeArchiveLimits envelope_limits{};
  envelope_limits.max_bytes = 8 + 8 + nonce.size() + 8 + ciphertext_and_tag.size();
  Writer w(envelope_limits);
  w.number(custody_format::record_format_version);
  w.number(header_epoch);
  w.raw({reinterpret_cast<const char*>(nonce.data()), nonce.size()});
  w.text(ciphertext_and_tag);
  return std::move(w.data);
}

inline constexpr std::size_t kRecordEnvelopeFramingBytes = 8 + 8 + 24 + 8;
inline constexpr std::size_t kRecordAeadTagBytes = 16;

inline bool exceeds_max_record_envelope_bytes(std::size_t envelope_size,
                                              const NativeArchiveLimits& limits) noexcept {
  constexpr std::size_t kFixedBytes = kRecordEnvelopeFramingBytes + kRecordAeadTagBytes;
  return envelope_size > kFixedBytes && envelope_size - kFixedBytes > limits.max_bytes;
}

struct ParsedRecordEnvelopeView {
  std::uint64_t format_version = 0;
  std::uint64_t header_epoch = 0;
  std::array<unsigned char, 24> nonce{};
  std::string_view ciphertext_and_tag;
};

inline ParsedRecordEnvelopeView parse_record_envelope_view(std::string_view envelope_bytes,
                                                           NativeArchiveLimits limits) {
  constexpr std::uint64_t kMinPlaintextBytes = 8;
  Reader r(envelope_bytes, limits);
  ParsedRecordEnvelopeView out;
  out.format_version = r.number();
  archive_fs::require(out.format_version == custody_format::record_format_version);
  out.header_epoch = r.number();
  const std::string_view nonce_view = r.raw(out.nonce.size());
  std::memcpy(out.nonce.data(), nonce_view.data(), out.nonce.size());
  const std::uint64_t ciphertext_len = r.number();
  archive_fs::require(ciphertext_len >= kRecordAeadTagBytes + kMinPlaintextBytes);
  archive_fs::require(ciphertext_len - kRecordAeadTagBytes <= limits.max_bytes);
  archive_fs::require(ciphertext_len == r.data.size());
  out.ciphertext_and_tag = r.raw(static_cast<std::size_t>(ciphertext_len));
  archive_fs::require(r.data.empty());
  return out;
}

inline Digest compute_record_reference_digest(std::string_view envelope_bytes) noexcept {
  return custody_ops::compute_record_reference_digest(envelope_bytes);
}

inline std::string compute_record_reference(std::string_view envelope_bytes) {
  return std::string(custody_format::ref_prefix) +
         hex(compute_record_reference_digest(envelope_bytes));
}

inline bool verify_record_reference(std::string_view reference, std::string_view envelope_bytes) {
  if (reference.size() != custody_format::ref_prefix.size() + 64 ||
      reference.substr(0, custody_format::ref_prefix.size()) != custody_format::ref_prefix) {
    return false;
  }
  const std::string_view hex_part = reference.substr(custody_format::ref_prefix.size());
  for (char c : hex_part) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return reference == compute_record_reference(envelope_bytes);
}

}  // namespace archive_internal

class NativeArchive;

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
    const unsigned char* key_32,
    const std::array<unsigned char, 24>& nonce,
    std::string_view aad,
    std::string_view plaintext,
    std::string_view store_id_hex = {});
CustodyOutcome<std::string> aead_xchacha20poly1305_encrypt(
    const DataKeyBuffer& key,
    const std::array<unsigned char, 24>& nonce,
    std::string_view aad,
    std::string_view plaintext);
CustodyOutcome<CleansedBuffer> aead_xchacha20poly1305_decrypt(
    const unsigned char* key_32,
    const std::array<unsigned char, 24>& nonce,
    std::string_view aad,
    std::string_view ciphertext_and_tag,
    std::string_view store_id_hex = {});
CustodyOutcome<CleansedBuffer> aead_xchacha20poly1305_decrypt(
    const DataKeyBuffer& key,
    const std::array<unsigned char, 24>& nonce,
    std::string_view aad,
    std::string_view ciphertext_and_tag);

struct AuthenticatedHeaderState {
  StoreHeader header;
  HostCustodyLimits resolved_limits{};
  archive_internal::EpochKeyTable epoch_deks;
};

CustodyOutcome<AuthenticatedHeaderState> open_authenticated_header(
    std::string_view header_bytes,
    KeySource& source,
    std::string_view owner_scope,
    const descriptor::ValidatedDescriptor& descriptor,
    HostCustodyLimits limits = {});

#ifdef SP_CUSTODY_TEST_SEAMS
using ReleaseObserver = void (*)(std::size_t capacity, bool all_zero, void* context) noexcept;
void set_release_observer(ReleaseObserver observer, void* context = nullptr) noexcept;

using AllocationObserver = void (*)(std::size_t capacity, void* context) noexcept;
void set_allocation_observer(AllocationObserver observer, void* context = nullptr) noexcept;

void fail_random_on_call(std::size_t call_index) noexcept;
void override_next_random_bytes(std::string_view bytes);
std::vector<std::size_t> recorded_random_draws();
void reset_random_seam() noexcept;
void fail_key_alloc_on_call(std::size_t call_index) noexcept;
void corrupt_next_cached_kek() noexcept;
std::vector<std::size_t> live_key_allocation_sizes();

CustodyOutcome<std::string> hkdf_sha256_expand(const DataKeyBuffer& prk,
                                               std::string_view info,
                                               std::size_t out_len);
#endif

}  // namespace custody_ops

}  // namespace sp
