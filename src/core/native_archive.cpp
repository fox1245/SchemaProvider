#include "core/native_archive.h"
#include "core/native_archive_custody_internal.h"
#include "core/native_archive_fs.h"
#include "core/native.h"
#include "crypto/crypto.h"
#include "json/json.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <mutex>

namespace sp {
namespace {
using Digest = archive_internal::Digest;
using archive_internal::Reader;
using archive_internal::Writer;
using archive_internal::descriptor_binding;
using archive_internal::hex;
using namespace archive_fs;
Error error() { Error e; e.kind = ErrorKind::Permission; e.safe_message = "Trusted local native archive admission failed"; e.retry_safety = RetrySafety::NotSent; return e; }
// This build has no file custody backend: the archive cannot be activated, and nothing is stored.
Error unavailable() { Error e; e.kind = ErrorKind::Unsupported; e.safe_message = "Trusted local native archive custody is unavailable on this platform"; e.retry_safety = RetrySafety::NotSent; return e; }
using Fd = archive_fs::Handle;
Digest mac(const Digest& key, std::string_view bytes) {
  return crypto::hmac_sha256(key.data(), key.size(), bytes.data(), bytes.size());
}
// The activation record is key material: the raw archive key is its first 32 bytes. It is built in fixed
// storage that is wiped on every exit path, never in a growing std::string, because each reallocation of a
// string would copy the key into a new heap block and free the old one unwiped.
class ActivationRecord {
 public:
  static constexpr size_t kSize = 96;
  explicit ActivationRecord(const NativeArchiveLimits& limits) : max_bytes_(limits.max_bytes) {}
  ActivationRecord(const ActivationRecord&) = delete;
  ActivationRecord& operator=(const ActivationRecord&) = delete;
  ~ActivationRecord() { wipe(); }
  void number(uint64_t n) { unsigned char b[8]; for (unsigned i=0;i<8;++i) b[7-i]=static_cast<unsigned char>(n>>(i*8)); raw(b,8); }
  void digest(const Digest& v) { raw(v.data(),v.size()); }
  std::string_view bytes() const { return {reinterpret_cast<const char*>(bytes_.data()),used_}; }
  void wipe() noexcept { crypto::cleanse(bytes_.data(),bytes_.size()); used_=0; }
 private:
  // The same bound the generic Writer applies, so a configured max_bytes below the record size still refuses.
  void raw(const unsigned char* p,size_t n) { require(n <= max_bytes_ - used_ && n <= bytes_.size() - used_); std::memcpy(bytes_.data()+used_,p,n); used_+=n; }
  size_t max_bytes_;
  std::array<unsigned char,kSize> bytes_{};
  size_t used_=0;
};
// Wipes a secret-bearing string when the scope ends, including when a Rejected unwinds through it.
struct ScopedWipe {
  std::string& bytes;
  ScopedWipe(const ScopedWipe&) = delete;
  ScopedWipe& operator=(const ScopedWipe&) = delete;
  explicit ScopedWipe(std::string& value) noexcept : bytes(value) {}
  ~ScopedWipe() { crypto::cleanse(bytes.data(),bytes.size()); }
};
template <class WriterType>
void write_media(WriterType& w, const Media& p) {
  w.number(static_cast<unsigned>(p.kind));w.number(static_cast<unsigned>(p.source));w.text(p.mime);
  w.boolean(bool(p.data));if(p.data)w.text(*p.data);
  w.text(p.reference);w.text(p.name);w.number(static_cast<unsigned>(p.detail));w.text(p.id);w.text(p.transcript);
}
Media read_media(Reader& r) {
  Media p;p.kind=r.enumeration<MediaKind>(3);p.source=r.enumeration<MediaSource>(2);p.mime=r.text();
  if(r.boolean())p.data=std::make_shared<const std::string>(r.text());
  p.reference=r.text();p.name=r.text();p.detail=r.enumeration<ImageDetail>(3);p.id=r.text();p.transcript=r.text();
  return p;
}
template <class WriterType>
void write_result(WriterType& w, const ToolResult& p) {
  w.text(p.tool_use_id);w.text(p.content);w.boolean(p.is_error);w.boolean(p.host.has_value());
  if(p.host){w.text(p.host->name);w.text(p.host->status);w.boolean(p.host->retryable);w.boolean(p.host->effect_uncertain);}
}
ToolResult read_result(Reader& r) {
  ToolResult p;p.tool_use_id=r.text();p.content=r.text();p.is_error=r.boolean();
  if(r.boolean()){ToolResultHostMetadata host;host.name=r.text();host.status=r.text();host.retryable=r.boolean();host.effect_uncertain=r.boolean();p.host=std::move(host);}
  return p;
}
// Media is variant index 10 but is tagged 12: tag 10 stays the original inline-image layout, which
// is still read so archives written before typed media load unchanged.
// Extended tool results use tag 13; absent parts retain tag 7 and its exact layout.
template <class WriterType>
void write_part(WriterType& w, const Part& part, size_t& parts) {
  const auto* result = std::get_if<ToolResult>(&part);
  w.number(std::holds_alternative<Media>(part) ? 12 : result && !result->content_parts.empty() ? 13 : part.index());
  std::visit([&](const auto& p) { using P=std::decay_t<decltype(p)>;
    if constexpr(std::is_same_v<P,Text>) w.text(p.value);
    else if constexpr(std::is_same_v<P,Refusal>) { w.text(p.text);w.text(p.raw_code); }
    else if constexpr(std::is_same_v<P,ToolCall> || std::is_same_v<P,InvalidToolCall>) { w.text(p.id);w.text(p.name);w.number(static_cast<unsigned>(p.kind));w.text(p.wire_type);w.document(p.wire_metadata); if constexpr(std::is_same_v<P,ToolCall>) w.document(p.input); else {w.text(p.raw_fragment);w.number(static_cast<unsigned>(p.reason));} }
    else if constexpr(std::is_same_v<P,Thinking>) {w.text(p.text);w.optional(p.signature);}
    else if constexpr(std::is_same_v<P,RedactedThinking>) w.text(p.data);
    else if constexpr(std::is_same_v<P,ServerToolResult>) {w.text(p.tool_use_id);w.text(p.wire_type);w.document(p.content);}
    else if constexpr(std::is_same_v<P,ToolResult>) {
      write_result(w,p);
      if(!p.content_parts.empty()) {
        require(p.content.empty() && p.content_parts.size() <= w.limits.max_parts-parts);
        parts+=p.content_parts.size();
        w.number(p.content_parts.size());
        for(const auto& nested:p.content_parts) {
          w.number(nested.index());
          if(const auto* text=std::get_if<Text>(&nested))w.text(text->value);
          else if(const auto* block=std::get_if<Opaque>(&nested)){w.text(block->wire_type);w.document(block->wire_metadata);}
          else write_media(w,std::get<Media>(nested));
        }
      }
    }
    else if constexpr(std::is_same_v<P,Reasoning>) {w.text(p.id);w.strings(p.summary);w.optional(p.encrypted_content);w.optional(p.status);w.strings(p.content);}
    else if constexpr(std::is_same_v<P,Opaque>) {w.text(p.wire_type);w.document(p.wire_metadata);}
    else if constexpr(std::is_same_v<P,Media>) write_media(w,p);
    else if constexpr(std::is_same_v<P,Thought>) {w.strings(p.summary);w.optional(p.signature);}
  },part);
}
Part read_part(Reader& r, size_t& parts) {
  switch(r.number()) {
    case 0:return Text{r.text()};
    case 1:{Refusal p;p.text=r.text();p.raw_code=r.text();return p;}
    case 2:{ToolCall p;p.id=r.text();p.name=r.text();p.kind=r.enumeration<ToolCallKind>(2);p.wire_type=r.text();p.wire_metadata=r.document(true);p.input=r.document();return p;}
    case 3:{InvalidToolCall p;p.id=r.text();p.name=r.text();p.kind=r.enumeration<ToolCallKind>(2);p.wire_type=r.text();p.wire_metadata=r.document(true);p.raw_fragment=r.text();p.reason=r.enumeration<InvalidReason>(5);return p;}
    case 4:{Thinking p;p.text=r.text();p.signature=r.optional();return p;}
    case 5:return RedactedThinking{r.text()};
    case 6:{ServerToolResult p;p.tool_use_id=r.text();p.wire_type=r.text();p.content=r.document(true);return p;}
    case 7:return read_result(r);
    case 8:{Reasoning p;p.id=r.text();p.summary=r.strings();p.encrypted_content=r.optional();p.status=r.optional();p.content=r.strings();return p;}
    case 9:{Opaque p;p.wire_type=r.text();p.wire_metadata=r.document(true);return p;}
    case 10:{Media p;p.kind=MediaKind::Image;p.mime=r.text();if(r.boolean())p.data=std::make_shared<const std::string>(r.text());p.detail=r.enumeration<ImageDetail>(3);return p;}
    case 12:return read_media(r);
    case 13:{
      auto p=read_result(r);require(p.content.empty());
      auto count=r.count(r.limits.max_parts-parts);require(count!=0);parts+=count;p.content_parts.reserve(count);
      while(count--) {
        switch(r.number()) {
          case 0:p.content_parts.emplace_back(Text{r.text()});break;
          case 1:p.content_parts.emplace_back(read_media(r));break;
          case 2:{Opaque block;block.wire_type=r.text();block.wire_metadata=r.document(true);p.content_parts.emplace_back(std::move(block));break;}
          default:throw Rejected{};
        }
      }
      return p;
    }
    case 11:{Thought p;p.summary=r.strings();p.signature=r.optional();return p;}
    default:throw Rejected{};
  }
}
} // namespace
struct NativeArchive::State {
  std::string directory, key_file, owner, descriptor_identity, activation;
  descriptor::ValidatedDescriptor descriptor;
  NativeArchiveLimits limits;
  Fd root, key_fd;
  Status root_identity{}, key_identity{};
  mutable std::mutex save_mutex;
  Digest key{};
  bool host_custody = false;
  Digest store_id{};
  std::string store_id_hex;
  Digest descriptor_identity_digest{};
  std::uint64_t current_epoch = 0;
  archive_internal::EpochKeyTable epoch_deks;
  State(std::string d,std::string k,std::string o,descriptor::ValidatedDescriptor descriptor,NativeArchiveLimits l)
      :directory(std::move(d)),key_file(std::move(k)),owner(std::move(o)),descriptor_identity(descriptor_binding(descriptor,l)),descriptor(std::move(descriptor)),limits(l) {}
  State(custody_ops::AuthenticatedHeaderState authenticated, descriptor::ValidatedDescriptor descriptor)
      :owner(std::move(authenticated.header.owner_scope)),
       descriptor_identity(reinterpret_cast<const char*>(authenticated.header.descriptor_identity.data()),
                           authenticated.header.descriptor_identity.size()),
       descriptor(std::move(descriptor)),
       limits(authenticated.resolved_limits.archive),
       host_custody(true),
       store_id(authenticated.header.store_id),
       store_id_hex(hex(authenticated.header.store_id)),
       descriptor_identity_digest(authenticated.header.descriptor_identity),
       current_epoch(authenticated.header.current_epoch),
       epoch_deks(std::move(authenticated.epoch_deks)) {}
  ~State() { crypto::cleanse(key.data(),key.size()); crypto::cleanse(activation.data(),activation.size()); }
  void verify() const {
    require(!host_custody);
    auto d=locate(directory);auto root_now=open_directory(d.parent,d.name);private_directory(root_now);require(same_file(root_identity,status(root_now)));
    auto k=locate(key_file);private_directory(k.parent);independent_key_parent(root,k);auto key_now=open_file(k.parent,k.name);private_file(key_now,true);require(unchanged(key_identity,status(key_now)));auto bytes=read_all(key_now,96);ScopedWipe wipe(bytes);require(bytes.size()==activation.size() && crypto::constant_time_equal(bytes.data(),activation.data(),bytes.size()));
  }
  template <class WriterType>
  void write_messages(WriterType& w, const std::vector<Message>& messages) const {
    require(messages.size()<=limits.max_messages);
    w.number(messages.size());
    size_t parts=0;
    for(const auto& m:messages) {
      require(m.parts.size()<=limits.max_parts-parts);parts+=m.parts.size();w.text(m.id);w.number(static_cast<unsigned>(m.role));w.number(m.parts.size());for(const auto& p:m.parts)write_part(w,p,parts);w.document(m.wire_output);w.boolean(bool(m.native));
      if(m.native) {
        const auto& seal=*m.native;require(seal.archive_valid(m));const auto& c=*seal.context_;require(c.matches_descriptor(descriptor) && c.policy_->identity()==descriptor.policy()->identity());
        w.number(static_cast<unsigned>(c.family_));w.text(c.model_);w.text(c.route_);w.number(c.prefix_count_);w.text(c.policy_->identity());w.digest(c.origin_);w.digest(c.prefix_);w.digest(c.root_);w.boolean(c.valid_);w.boolean(c.history_valid_);w.boolean(c.replay_eligible_);w.number(c.pending_server_tools_.size());for(const auto& [a,b]:c.pending_server_tools_){w.text(a);w.text(b);}w.strings(c.client_tools_);w.number(static_cast<unsigned>(seal.stop_));w.digest(seal.content_);w.boolean(seal.complete_);
      }
    }
  }
  std::vector<Message> read_messages(Reader& r) const {
    auto count=r.count(limits.max_messages);std::vector<Message> messages;messages.reserve(count);size_t parts=0;
    while(count--) {
      Message m;m.id=r.text();m.role=r.enumeration<Role>(4);auto n=r.count(limits.max_parts-parts);parts+=n;m.parts.reserve(n);while(n--)m.parts.push_back(read_part(r,parts));m.wire_output=r.document(true);
      if(r.boolean()) {
        auto family=r.enumeration<NativeContext::Family>(4);auto model=r.text();auto route=r.text();auto prefix=r.number();require(prefix<=limits.max_messages && r.text()==descriptor.policy()->identity());
        auto c=std::shared_ptr<NativeContext>(new NativeContext(family,std::move(model),std::move(route),static_cast<size_t>(prefix),descriptor.policy()));c->origin_=r.digest();c->prefix_=r.digest();c->root_=r.digest();c->valid_=r.boolean();c->history_valid_=r.boolean();c->replay_eligible_=r.boolean();auto pending=r.count(limits.max_parts);c->pending_server_tools_.reserve(pending);while(pending--){auto a=r.text();auto b=r.text();c->pending_server_tools_.emplace_back(std::move(a),std::move(b));}c->client_tools_=r.strings();auto stop=r.enumeration<StopKind>(12);auto digest=r.digest();auto complete=r.boolean();m.native=std::shared_ptr<const NativeReplay>(new NativeReplay(std::move(c),stop,digest,complete));require(m.native->archive_valid(m) && m.native->context_->matches_descriptor(descriptor));
      }
      messages.push_back(std::move(m));
    }
    require(r.data.empty());
    return messages;
  }
};
NativeArchive::NativeArchive(std::unique_ptr<State> state):state_(std::move(state)) {}
NativeArchive::~NativeArchive()=default;
std::string_view NativeArchive::owner_scope() const noexcept { return state_->owner; }
std::uint64_t NativeArchive::current_epoch() const noexcept { return state_->current_epoch; }
bool NativeArchive::matches_descriptor(const descriptor::ValidatedDescriptor& descriptor) const noexcept {
  try {
    if (!descriptor.policy()) return false;
    if (!state_->host_custody) state_->verify();
    const auto binding = descriptor_binding(descriptor, state_->limits);
    return binding.size() == state_->descriptor_identity.size() &&
        crypto::constant_time_equal(binding.data(), state_->descriptor_identity.data(), binding.size());
  } catch (...) { return false; }
}
NativeArchive::CustodyActivation NativeArchive::open_custody(
    std::string_view header_bytes,
    KeySource& source,
    std::string_view owner_scope,
    descriptor::ValidatedDescriptor descriptor,
    HostCustodyLimits limits) {
  if (!custody_ops::available()) {
    return custody_unavailable_error();
  }
  auto auth_res = custody_ops::open_authenticated_header(
      header_bytes, source, owner_scope, descriptor, limits);
  if (const auto* err = std::get_if<CustodyError>(&auth_res)) {
    return *err;
  }
  auto state = std::make_unique<State>(
      std::get<custody_ops::AuthenticatedHeaderState>(std::move(auth_res)),
      std::move(descriptor));
  return std::shared_ptr<NativeArchive>(new NativeArchive(std::move(state)));
}
NativeArchive::Sealed NativeArchive::seal(
    const std::vector<Message>& messages,
    std::string_view binding) const {
  if (!custody_ops::available()) {
    return custody_unavailable_error();
  }
  if (!state_ || !state_->host_custody) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  const unsigned char* dek = state_->epoch_deks.epoch_dek(state_->current_epoch);
  if (dek == nullptr) {
    return custody_error(CustodyFault::InvalidKeyMaterial, state_->store_id_hex);
  }
  archive_internal::CleansedWriter plaintext_writer(state_->limits);
  std::string record_aad;
  try {
    record_aad = archive_internal::build_record_aad(
        state_->store_id,
        state_->current_epoch,
        state_->owner,
        state_->descriptor_identity_digest,
        binding,
        state_->limits);
    state_->write_messages(plaintext_writer, messages);
  } catch (const Rejected&) {
    plaintext_writer.data.wipe();
    return custody_error(CustodyFault::InvalidInput, state_->store_id_hex);
  }
  std::array<unsigned char, 24> nonce{};
  if (!custody_ops::random_bytes(nonce.data(), nonce.size())) {
    crypto::cleanse(nonce.data(), nonce.size());
    plaintext_writer.data.wipe();
    return custody_error(CustodyFault::InvalidKeyMaterial, state_->store_id_hex);
  }
  auto enc_res = custody_ops::aead_xchacha20poly1305_encrypt(
      dek, nonce, record_aad, plaintext_writer.data.view(), state_->store_id_hex);
  plaintext_writer.data.wipe();
  if (const auto* err = std::get_if<CustodyError>(&enc_res)) {
    return *err;
  }
  const std::string& ciphertext_and_tag = std::get<std::string>(enc_res);
  std::string envelope_bytes = archive_internal::serialize_record_envelope(
      state_->current_epoch, nonce, ciphertext_and_tag);
  std::string reference = archive_internal::compute_record_reference(envelope_bytes);
  return SealedRecord{std::move(reference), std::move(envelope_bytes)};
}
NativeArchive::Opened NativeArchive::open(
    std::string_view reference,
    std::string_view bytes,
    std::string_view expected_binding) const {
  if (!custody_ops::available()) {
    return custody_unavailable_error();
  }
  if (!state_ || !state_->host_custody) {
    return custody_error(CustodyFault::InvalidKeyMaterial);
  }
  if (expected_binding.size() > state_->limits.max_bytes) {
    return custody_error(CustodyFault::InvalidInput, state_->store_id_hex);
  }
  if (archive_internal::exceeds_max_record_envelope_bytes(bytes.size(), state_->limits)) {
    return custody_error(CustodyFault::CorruptRecord, state_->store_id_hex);
  }
  if (!archive_internal::verify_record_reference(reference, bytes)) {
    return custody_error(CustodyFault::Tampered, state_->store_id_hex);
  }
  archive_internal::ParsedRecordEnvelopeView env;
  try {
    env = archive_internal::parse_record_envelope_view(bytes, state_->limits);
  } catch (const Rejected&) {
    return custody_error(CustodyFault::CorruptRecord, state_->store_id_hex);
  }
  const unsigned char* dek = state_->epoch_deks.epoch_dek(env.header_epoch);
  if (dek == nullptr) {
    return custody_error(CustodyFault::Tampered, state_->store_id_hex);
  }
  std::string record_aad;
  try {
    record_aad = archive_internal::build_record_aad(
        state_->store_id,
        env.header_epoch,
        state_->owner,
        state_->descriptor_identity_digest,
        expected_binding,
        state_->limits);
  } catch (const Rejected&) {
    return custody_error(CustodyFault::Tampered, state_->store_id_hex);
  }
  auto dec_res = custody_ops::aead_xchacha20poly1305_decrypt(
      dek, env.nonce, record_aad, env.ciphertext_and_tag, state_->store_id_hex);
  if (const auto* err = std::get_if<CustodyError>(&dec_res)) {
    return *err;
  }
  CleansedBuffer plaintext = std::get<CleansedBuffer>(std::move(dec_res));
  try {
    Reader r(plaintext.view(), state_->limits);
    std::vector<Message> messages = state_->read_messages(r);
    plaintext.wipe();
    return messages;
  } catch (const Rejected&) {
    plaintext.wipe();
    return custody_error(CustodyFault::CorruptRecord, state_->store_id_hex);
  }
}
NativeArchive::Activation NativeArchive::provision(std::string d,std::string k,std::string o,descriptor::ValidatedDescriptor v,NativeArchiveLimits l) {return activate(std::move(d),std::move(k),std::move(o),std::move(v),l,true);}
NativeArchive::Activation NativeArchive::open(std::string d,std::string k,std::string o,descriptor::ValidatedDescriptor v,NativeArchiveLimits l) {return activate(std::move(d),std::move(k),std::move(o),std::move(v),l,false);}
NativeArchive::Activation NativeArchive::activate(std::string d,std::string k,std::string o,descriptor::ValidatedDescriptor v,NativeArchiveLimits l,bool create) {
  if(!archive_fs::available())return unavailable();
  try {
    l = archive_internal::resolve_archive_limits(l, v);
    require(!o.empty());
    auto state=std::make_unique<State>(std::move(d),std::move(k),std::move(o),std::move(v),l);
    auto dir=locate(state->directory); auto key=locate(state->key_file); private_directory(key.parent);
    state->root=create ? create_directory(dir.parent,dir.name) : open_directory(dir.parent,dir.name);private_directory(state->root);state->root_identity=status(state->root);
    // The independent activation must remain outside the entire archive tree.
    independent_key_parent(state->root,key);
    if(create) {
      require(crypto::random_bytes(state->key.data(),state->key.size()));
      auto fd=create_file(key.parent,key.name);private_file(fd,false);
      state->key_identity=status(fd);
      Writer identity(l);identity.text(state->owner);identity.text(state->descriptor_identity);identity.number(state->root_identity.device);identity.number(state->root_identity.inode);identity.number(state->key_identity.device);identity.number(state->key_identity.inode);
      ActivationRecord activation(l);activation.digest(state->key);activation.number(state->root_identity.device);activation.number(state->root_identity.inode);activation.number(state->key_identity.device);activation.number(state->key_identity.inode);activation.digest(mac(state->key,identity.data));
      write_all(fd,activation.bytes());activation.wipe();seal_file(fd);sync(fd);sync(key.parent);sync(dir.parent);
    }
    state->key_fd=open_file(key.parent,key.name);private_file(state->key_fd,true);state->key_identity=status(state->key_fd);state->activation=read_all(state->key_fd,96);require(state->activation.size()==96);
    Reader activation(state->activation,l);state->key=activation.digest();require(activation.number()==state->root_identity.device && activation.number()==state->root_identity.inode && activation.number()==state->key_identity.device && activation.number()==state->key_identity.inode);
    Writer identity(l);identity.text(state->owner);identity.text(state->descriptor_identity);identity.number(state->root_identity.device);identity.number(state->root_identity.inode);identity.number(state->key_identity.device);identity.number(state->key_identity.inode);const auto expected=mac(state->key,identity.data);const auto actual=activation.digest();require(crypto::constant_time_equal(expected.data(),actual.data(),expected.size()));
    state->verify();
    return std::shared_ptr<NativeArchive>(new NativeArchive(std::move(state)));
  }catch(const Rejected&){return error();}
}
NativeArchive::Saved NativeArchive::save(const std::vector<Message>& messages,std::string_view binding) const {
  try {
    state_->verify();require(messages.size()<=state_->limits.max_messages);Writer w(state_->limits);w.text("sp.native.local-custody.archive.v3");w.text(state_->owner);w.text(state_->descriptor_identity);w.text(binding);w.number(messages.size());size_t parts=0;
    for(const auto& m:messages) {
      require(m.parts.size()<=state_->limits.max_parts-parts);parts+=m.parts.size();w.text(m.id);w.number(static_cast<unsigned>(m.role));w.number(m.parts.size());for(const auto& p:m.parts)write_part(w,p,parts);w.document(m.wire_output);w.boolean(bool(m.native));
      if(m.native) {
        const auto& seal=*m.native;require(seal.archive_valid(m));const auto& c=*seal.context_;require(c.matches_descriptor(state_->descriptor) && c.policy_->identity()==state_->descriptor.policy()->identity());
        w.number(static_cast<unsigned>(c.family_));w.text(c.model_);w.text(c.route_);w.number(c.prefix_count_);w.text(c.policy_->identity());w.digest(c.origin_);w.digest(c.prefix_);w.digest(c.root_);w.boolean(c.valid_);w.boolean(c.history_valid_);w.boolean(c.replay_eligible_);w.number(c.pending_server_tools_.size());for(const auto& [a,b]:c.pending_server_tools_){w.text(a);w.text(b);}w.strings(c.client_tools_);w.number(static_cast<unsigned>(seal.stop_));w.digest(seal.content_);w.boolean(seal.complete_);
      }
    }
    const auto reference="spna3:"+hex(mac(state_->key,w.data));const auto filename=reference.substr(6);
    std::lock_guard guard(state_->save_mutex);StoreLock store_lock(state_->root,state_->key_fd);state_->verify();
    auto existing=open_file(state_->root,filename,true);
    if(existing) {
      private_file(existing,true);require(read_all(existing,state_->limits.max_bytes)==w.data);
      // Publication follows a successful seal and file flush. A prior save may
      // nevertheless have failed after rename: require the directory's durable
      // publication barrier again before acknowledging this immutable record.
      // Do not reopen the sealed file for write access (including on Windows).
      sync(state_->root);state_->verify();return reference;
    }
    // Capacity is durable, not renewed by reopening the archive. Interrupted
    // staging records also count until the host investigates them.
    check_capacity(state_->root,state_->limits.max_records,state_->limits.max_store_bytes,w.data.size());
    // Stage under an unpredictable private name, then atomically publish without
    // overwrite. An interrupted write can never expose a partial archive record.
    Digest random{};require(crypto::random_bytes(random.data(),random.size()));const auto temporary=".pending-"+hex(random);
    auto fd=create_file(state_->root,temporary);bool published=false;
    try {
      private_file(fd,false);write_all(fd,w.data);seal_file(fd);sync(fd);
      // Each platform publishes with a genuine atomic no-replace rename.
      publish(state_->root,fd,temporary,filename);published=true;
      finish_publication(state_->root,fd);state_->verify();return reference;
    }catch(...) {if(!published)remove_staging(state_->root,fd,temporary);throw;}
  }catch(const Rejected&){return error();}
}
NativeArchive::Loaded NativeArchive::load(std::string_view reference,std::string_view binding) const {
  try {
    state_->verify();require(reference.size()==70 && reference.substr(0,6)=="spna3:");const std::string name(reference.substr(6));require(std::all_of(name.begin(),name.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');}));
    auto fd=open_file(state_->root,name);private_file(fd,true);const auto identity=status(fd);auto bytes=read_all(fd,state_->limits.max_bytes);const auto expected=hex(mac(state_->key,bytes));require(crypto::constant_time_equal(expected.data(),name.data(),64));
    Reader r(bytes,state_->limits);require(r.text()=="sp.native.local-custody.archive.v3" && r.text()==state_->owner && r.text()==state_->descriptor_identity && r.text()==binding);auto count=r.count(state_->limits.max_messages);std::vector<Message> messages;messages.reserve(count);size_t parts=0;
    while(count--) {
      Message m;m.id=r.text();m.role=r.enumeration<Role>(4);auto n=r.count(state_->limits.max_parts-parts);parts+=n;m.parts.reserve(n);while(n--)m.parts.push_back(read_part(r,parts));m.wire_output=r.document(true);
      if(r.boolean()) {
        auto family=r.enumeration<NativeContext::Family>(4);auto model=r.text();auto route=r.text();auto prefix=r.number();require(prefix<=state_->limits.max_messages && r.text()==state_->descriptor.policy()->identity());
        auto c=std::shared_ptr<NativeContext>(new NativeContext(family,std::move(model),std::move(route),static_cast<size_t>(prefix),state_->descriptor.policy()));c->origin_=r.digest();c->prefix_=r.digest();c->root_=r.digest();c->valid_=r.boolean();c->history_valid_=r.boolean();c->replay_eligible_=r.boolean();auto pending=r.count(state_->limits.max_parts);c->pending_server_tools_.reserve(pending);while(pending--){auto a=r.text();auto b=r.text();c->pending_server_tools_.emplace_back(std::move(a),std::move(b));}c->client_tools_=r.strings();auto stop=r.enumeration<StopKind>(12);auto digest=r.digest();auto complete=r.boolean();m.native=std::shared_ptr<const NativeReplay>(new NativeReplay(std::move(c),stop,digest,complete));require(m.native->archive_valid(m) && m.native->context_->matches_descriptor(state_->descriptor));
      }
      messages.push_back(std::move(m));
    }
    require(r.data.empty());require(unchanged(identity,entry_status(state_->root,name)));private_file(fd,true);state_->verify();return messages;
  }catch(const Rejected&){return error();}
}
} // namespace sp
