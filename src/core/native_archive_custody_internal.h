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
#include <utility>
#include <vector>

namespace sp {

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

class Writer {
 public:
  explicit Writer(NativeArchiveLimits limits) : limits(limits) {}
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
    json::BoundedWriter writer({limits.max_bytes, limits.max_json_depth}, &data);
    writer.value(v->root());
    archive_fs::require(writer.ok());
    const std::uint64_t length = data.size() - start;
    for (unsigned i = 0; i < 8; ++i)
      data[length_offset + 7 - i] = static_cast<char>(length >> (i * 8));
  }
  void digest(const Digest& v) { raw({reinterpret_cast<const char*>(v.data()), v.size()}); }
  NativeArchiveLimits limits;
  std::string data;
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

}  // namespace archive_internal

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
std::vector<std::size_t> recorded_random_draws();
void reset_random_seam() noexcept;
void fail_key_alloc_on_call(std::size_t call_index) noexcept;
void corrupt_next_cached_kek() noexcept;

CustodyOutcome<std::string> hkdf_sha256_expand(const DataKeyBuffer& prk,
                                               std::string_view info,
                                               std::size_t out_len);
#endif

}  // namespace custody_ops

}  // namespace sp
