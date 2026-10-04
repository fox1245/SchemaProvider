#include "core/native_archive.h"
#include "core/native_archive_fs.h"
#include "core/native.h"
#include "json/json.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <mutex>

namespace sp {
namespace {
using Digest = std::array<unsigned char, 32>;
using namespace archive_fs;
Error error() { Error e; e.kind = ErrorKind::Permission; e.safe_message = "Trusted local native archive admission failed"; e.retry_safety = RetrySafety::NotSent; return e; }
using Fd = archive_fs::Handle;
Digest mac(const Digest& key, std::string_view bytes) {
  Digest result{}; unsigned size = 0;
  require(HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), result.data(), &size) && size == result.size()); return result;
}
std::string hex(const Digest& bytes) { static constexpr char digits[] = "0123456789abcdef"; std::string text; text.reserve(64); for (auto b : bytes) { text += digits[b>>4]; text += digits[b&15]; } return text; }
class Writer {
 public:
  explicit Writer(NativeArchiveLimits limits) : limits(limits) {}
  void raw(std::string_view v) { require(v.size() <= limits.max_bytes - data.size()); data.append(v); }
  void number(uint64_t n) { char b[8]; for (unsigned i=0;i<8;++i) b[7-i]=static_cast<char>(n>>(i*8)); raw({b,8}); }
  void boolean(bool v) { number(v ? 1 : 0); }
  void text(std::string_view v) { number(v.size()); raw(v); }
  void optional(const std::optional<std::string>& v) { boolean(v.has_value()); if (v) text(*v); }
  void strings(const std::vector<std::string>& v) { require(v.size() <= limits.max_parts); number(v.size()); for (const auto& s:v) text(s); }
  void document(const std::shared_ptr<const json::Document>& v) {
    boolean(bool(v));if(!v)return;
    const auto length_offset=data.size();number(0);const auto start=data.size();
    json::BoundedWriter writer({limits.max_bytes,limits.max_json_depth},&data);writer.value(v->root());require(writer.ok());
    const uint64_t length=data.size()-start;for(unsigned i=0;i<8;++i)data[length_offset+7-i]=static_cast<char>(length>>(i*8));
  }
  void digest(const Digest& v) { raw({reinterpret_cast<const char*>(v.data()),v.size()}); }
  NativeArchiveLimits limits; std::string data;
};
class Reader {
 public:
  Reader(std::string_view data, NativeArchiveLimits limits) : data(data), limits(limits) {}
  std::string_view raw(size_t n) { require(n <= data.size()); auto out=data.substr(0,n); data.remove_prefix(n); return out; }
  uint64_t number() { uint64_t n=0; for (unsigned char c:raw(8)) n=(n<<8)|c; return n; }
  bool boolean() { auto n=number(); require(n<=1); return n!=0; }
  size_t count(size_t limit) { auto n=number(); require(n<=limit && n<=data.size()/8); return static_cast<size_t>(n); }
  std::string_view blob() { auto n=number();require(n<=data.size());return raw(static_cast<size_t>(n)); }
  std::string text() { return std::string(blob()); }
  std::optional<std::string> optional() { if (!boolean()) return {}; return text(); }
  std::vector<std::string> strings() { std::vector<std::string> v; auto n=count(limits.max_parts); v.reserve(n); while(n--) v.push_back(text()); return v; }
  std::shared_ptr<const json::Document> document(bool diagnostic = false) {
    if (!boolean()) return {};
    auto parsed = json::parse(blob(), {limits.max_bytes, limits.max_json_depth});
    if (auto* accepted = std::get_if<json::Document>(&parsed))
      return std::make_shared<const json::Document>(std::move(*accepted));
    auto& rejected = std::get<json::ParseError>(parsed);
    require(diagnostic && rejected.code == json::ParseCode::DuplicateKey && rejected.context.root().valid());
    return std::make_shared<const json::Document>(std::move(rejected.context));
  }
  Digest digest() { Digest v{}; auto bytes=raw(v.size()); std::memcpy(v.data(),bytes.data(),v.size()); return v; }
  template<class E> E enumeration(unsigned maximum) { auto n=number(); require(n<=maximum); return static_cast<E>(n); }
  std::string_view data; NativeArchiveLimits limits;
};
void write_part(Writer& w, const Part& part) {
  w.number(part.index());
  std::visit([&](const auto& p) { using P=std::decay_t<decltype(p)>;
    if constexpr(std::is_same_v<P,Text>) w.text(p.value);
    else if constexpr(std::is_same_v<P,Refusal>) { w.text(p.text);w.text(p.raw_code); }
    else if constexpr(std::is_same_v<P,ToolCall> || std::is_same_v<P,InvalidToolCall>) { w.text(p.id);w.text(p.name);w.number(static_cast<unsigned>(p.kind));w.text(p.wire_type);w.document(p.wire_metadata); if constexpr(std::is_same_v<P,ToolCall>) w.document(p.input); else {w.text(p.raw_fragment);w.number(static_cast<unsigned>(p.reason));} }
    else if constexpr(std::is_same_v<P,Thinking>) {w.text(p.text);w.optional(p.signature);}
    else if constexpr(std::is_same_v<P,RedactedThinking>) w.text(p.data);
    else if constexpr(std::is_same_v<P,ServerToolResult>) {w.text(p.tool_use_id);w.text(p.wire_type);w.document(p.content);}
    else if constexpr(std::is_same_v<P,ToolResult>) {w.text(p.tool_use_id);w.text(p.content);w.boolean(p.is_error);w.boolean(p.host.has_value());if(p.host){w.text(p.host->name);w.text(p.host->status);w.boolean(p.host->retryable);w.boolean(p.host->effect_uncertain);}}
    else if constexpr(std::is_same_v<P,Reasoning>) {w.text(p.id);w.strings(p.summary);w.optional(p.encrypted_content);w.optional(p.status);w.strings(p.content);}
    else if constexpr(std::is_same_v<P,Opaque>) {w.text(p.wire_type);w.document(p.wire_metadata);}
    else if constexpr(std::is_same_v<P,Image>) {w.text(p.mime);w.boolean(bool(p.data));if(p.data)w.text(*p.data);w.number(static_cast<unsigned>(p.detail));}
    else if constexpr(std::is_same_v<P,Thought>) {w.strings(p.summary);w.optional(p.signature);}
  },part);
}
Part read_part(Reader& r) {
  switch(r.number()) {
    case 0:return Text{r.text()};
    case 1:{Refusal p;p.text=r.text();p.raw_code=r.text();return p;}
    case 2:{ToolCall p;p.id=r.text();p.name=r.text();p.kind=r.enumeration<ToolCallKind>(2);p.wire_type=r.text();p.wire_metadata=r.document(true);p.input=r.document();return p;}
    case 3:{InvalidToolCall p;p.id=r.text();p.name=r.text();p.kind=r.enumeration<ToolCallKind>(2);p.wire_type=r.text();p.wire_metadata=r.document(true);p.raw_fragment=r.text();p.reason=r.enumeration<InvalidReason>(5);return p;}
    case 4:{Thinking p;p.text=r.text();p.signature=r.optional();return p;}
    case 5:return RedactedThinking{r.text()};
    case 6:{ServerToolResult p;p.tool_use_id=r.text();p.wire_type=r.text();p.content=r.document(true);return p;}
    case 7:{ToolResult p;p.tool_use_id=r.text();p.content=r.text();p.is_error=r.boolean();if(r.boolean()){ToolResultHostMetadata host;host.name=r.text();host.status=r.text();host.retryable=r.boolean();host.effect_uncertain=r.boolean();p.host=std::move(host);}return p;}
    case 8:{Reasoning p;p.id=r.text();p.summary=r.strings();p.encrypted_content=r.optional();p.status=r.optional();p.content=r.strings();return p;}
    case 9:{Opaque p;p.wire_type=r.text();p.wire_metadata=r.document(true);return p;}
    case 10:{Image p;p.mime=r.text();if(r.boolean())p.data=std::make_shared<const std::string>(r.text());p.detail=r.enumeration<ImageDetail>(3);return p;}
    case 11:{Thought p;p.summary=r.strings();p.signature=r.optional();return p;}
    default:throw Rejected{};
  }
}
std::string descriptor_binding(const descriptor::ValidatedDescriptor& d, NativeArchiveLimits limits) {
  Writer w(limits);w.text("sp.native.archive.descriptor.v2");w.text(d.id());w.number(d.revision());w.text(d.family());w.text(d.base_url());w.text(d.path(false));w.text(d.path(true));w.text(d.policy()->identity());w.number(d.headers().size());for(const auto& [name,value]:d.headers()){w.text(name);w.text(value);}
  w.text(d.request_model_member());w.text(d.request_messages_member());w.text(d.request_stream_member());w.text(d.max_output_tokens_member());
  w.number(d.usage_path().size());for(const auto& segment:d.usage_path())w.text(segment);
  w.number(d.stop_mappings().size());for(const auto& [raw,kind]:d.stop_mappings()){w.text(raw);w.number(static_cast<unsigned>(kind));}
  Digest digest{};unsigned size=0;require(EVP_Digest(w.data.data(),w.data.size(),digest.data(),&size,EVP_sha256(),nullptr)==1 && size==digest.size());return {reinterpret_cast<const char*>(digest.data()),digest.size()};
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
  State(std::string d,std::string k,std::string o,descriptor::ValidatedDescriptor descriptor,NativeArchiveLimits l)
      :directory(std::move(d)),key_file(std::move(k)),owner(std::move(o)),descriptor_identity(descriptor_binding(descriptor,l)),descriptor(std::move(descriptor)),limits(l) {}
  ~State() { OPENSSL_cleanse(key.data(),key.size()); OPENSSL_cleanse(activation.data(),activation.size()); }
  void verify() const {
    auto d=locate(directory);auto root_now=open_directory(d.parent,d.name);private_directory(root_now);require(same_file(root_identity,status(root_now)));
    auto k=locate(key_file);private_directory(k.parent);independent_key_parent(root,k);auto key_now=open_file(k.parent,k.name);private_file(key_now,true);require(unchanged(key_identity,status(key_now)));auto bytes=read_all(key_now,96);require(bytes.size()==activation.size() && CRYPTO_memcmp(bytes.data(),activation.data(),bytes.size())==0);OPENSSL_cleanse(bytes.data(),bytes.size());
  }
};
NativeArchive::NativeArchive(std::unique_ptr<State> state):state_(std::move(state)) {}
NativeArchive::~NativeArchive()=default;
std::string_view NativeArchive::owner_scope() const noexcept { return state_->owner; }
bool NativeArchive::matches_descriptor(const descriptor::ValidatedDescriptor& descriptor) const noexcept {
  try {
    if (!descriptor.policy()) return false;
    state_->verify();
    const auto binding = descriptor_binding(descriptor, state_->limits);
    return binding.size() == state_->descriptor_identity.size() &&
        CRYPTO_memcmp(binding.data(), state_->descriptor_identity.data(), binding.size()) == 0;
  } catch (...) { return false; }
}
NativeArchive::Activation NativeArchive::provision(std::string d,std::string k,std::string o,descriptor::ValidatedDescriptor v,NativeArchiveLimits l) {return activate(std::move(d),std::move(k),std::move(o),std::move(v),l,true);}
NativeArchive::Activation NativeArchive::open(std::string d,std::string k,std::string o,descriptor::ValidatedDescriptor v,NativeArchiveLimits l) {return activate(std::move(d),std::move(k),std::move(o),std::move(v),l,false);}
NativeArchive::Activation NativeArchive::activate(std::string d,std::string k,std::string o,descriptor::ValidatedDescriptor v,NativeArchiveLimits l,bool create) {
  try {
    require(v.policy()!=nullptr);
    const auto& resources=v.policy()->resources();
    if(!l.max_bytes)l.max_bytes=resources.native_bytes;
    if(!l.max_messages)l.max_messages=resources.request_messages;
    if(!l.max_parts)l.max_parts=resources.request_parts;
    if(!l.max_json_depth)l.max_json_depth=resources.native_depth;
    if(!l.max_records)l.max_records=resources.request_messages;
    if(!l.max_store_bytes){require(l.max_records && l.max_bytes<=UINT64_MAX/l.max_records);l.max_store_bytes=static_cast<uint64_t>(l.max_bytes)*l.max_records;}
    require(resources.request_messages && resources.native_bytes <= UINT64_MAX / resources.request_messages);
    const auto storage_ceiling = static_cast<uint64_t>(resources.native_bytes) * resources.request_messages;
    require(!o.empty() && l.max_bytes > 0 && l.max_bytes <= resources.native_bytes &&
            l.max_messages > 0 && l.max_messages <= resources.request_messages &&
            l.max_parts > 0 && l.max_parts <= resources.request_parts &&
            l.max_json_depth > 0 && l.max_json_depth <= resources.native_depth &&
            l.max_records > 0 && l.max_records <= resources.request_messages &&
            l.max_store_bytes >= l.max_bytes && l.max_store_bytes <= storage_ceiling);
    auto state=std::make_unique<State>(std::move(d),std::move(k),std::move(o),std::move(v),l);
    auto dir=locate(state->directory); auto key=locate(state->key_file); private_directory(key.parent);
    state->root=create ? create_directory(dir.parent,dir.name) : open_directory(dir.parent,dir.name);private_directory(state->root);state->root_identity=status(state->root);
    // The independent activation must remain outside the entire archive tree.
    independent_key_parent(state->root,key);
    if(create) {
      require(RAND_bytes(state->key.data(),static_cast<int>(state->key.size()))==1);
      auto fd=create_file(key.parent,key.name);private_file(fd,false);
      state->key_identity=status(fd);
      Writer identity(l);identity.text(state->owner);identity.text(state->descriptor_identity);identity.number(state->root_identity.device);identity.number(state->root_identity.inode);identity.number(state->key_identity.device);identity.number(state->key_identity.inode);
      Writer activation(l);activation.digest(state->key);activation.number(state->root_identity.device);activation.number(state->root_identity.inode);activation.number(state->key_identity.device);activation.number(state->key_identity.inode);activation.digest(mac(state->key,identity.data));
      write_all(fd,activation.data);OPENSSL_cleanse(activation.data.data(),activation.data.size());seal_file(fd);sync(fd);sync(key.parent);sync(dir.parent);
    }
    state->key_fd=open_file(key.parent,key.name);private_file(state->key_fd,true);state->key_identity=status(state->key_fd);state->activation=read_all(state->key_fd,96);require(state->activation.size()==96);
    Reader activation(state->activation,l);state->key=activation.digest();require(activation.number()==state->root_identity.device && activation.number()==state->root_identity.inode && activation.number()==state->key_identity.device && activation.number()==state->key_identity.inode);
    Writer identity(l);identity.text(state->owner);identity.text(state->descriptor_identity);identity.number(state->root_identity.device);identity.number(state->root_identity.inode);identity.number(state->key_identity.device);identity.number(state->key_identity.inode);const auto expected=mac(state->key,identity.data);const auto actual=activation.digest();require(CRYPTO_memcmp(expected.data(),actual.data(),expected.size())==0);
    state->verify();
    return std::shared_ptr<NativeArchive>(new NativeArchive(std::move(state)));
  }catch(const Rejected&){return error();}
}
NativeArchive::Saved NativeArchive::save(const std::vector<Message>& messages,std::string_view binding) const {
  try {
    state_->verify();require(messages.size()<=state_->limits.max_messages);Writer w(state_->limits);w.text("sp.native.local-custody.archive.v3");w.text(state_->owner);w.text(state_->descriptor_identity);w.text(binding);w.number(messages.size());size_t parts=0;
    for(const auto& m:messages) {
      require(m.parts.size()<=state_->limits.max_parts-parts);parts+=m.parts.size();w.text(m.id);w.number(static_cast<unsigned>(m.role));w.number(m.parts.size());for(const auto& p:m.parts)write_part(w,p);w.document(m.wire_output);w.boolean(bool(m.native));
      if(m.native) {
        const auto& seal=*m.native;require(seal.archive_valid(m));const auto& c=*seal.context_;require(c.matches_descriptor(state_->descriptor) && c.policy_->identity()==state_->descriptor.policy()->identity());
        w.number(static_cast<unsigned>(c.family_));w.text(c.model_);w.text(c.route_);w.number(c.prefix_count_);w.text(c.policy_->identity());w.digest(c.origin_);w.digest(c.prefix_);w.boolean(c.valid_);w.boolean(c.history_valid_);w.boolean(c.replay_eligible_);w.number(c.pending_server_tools_.size());for(const auto& [a,b]:c.pending_server_tools_){w.text(a);w.text(b);}w.strings(c.client_tools_);w.number(static_cast<unsigned>(seal.stop_));w.digest(seal.content_);w.boolean(seal.complete_);
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
    Digest random{};require(RAND_bytes(random.data(),static_cast<int>(random.size()))==1);const auto temporary=".pending-"+hex(random);
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
    auto fd=open_file(state_->root,name);private_file(fd,true);const auto identity=status(fd);auto bytes=read_all(fd,state_->limits.max_bytes);const auto expected=hex(mac(state_->key,bytes));require(CRYPTO_memcmp(expected.data(),name.data(),64)==0);
    Reader r(bytes,state_->limits);require(r.text()=="sp.native.local-custody.archive.v3" && r.text()==state_->owner && r.text()==state_->descriptor_identity && r.text()==binding);auto count=r.count(state_->limits.max_messages);std::vector<Message> messages;messages.reserve(count);size_t parts=0;
    while(count--) {
      Message m;m.id=r.text();m.role=r.enumeration<Role>(4);auto n=r.count(state_->limits.max_parts-parts);parts+=n;m.parts.reserve(n);while(n--)m.parts.push_back(read_part(r));m.wire_output=r.document(true);
      if(r.boolean()) {
        auto family=r.enumeration<NativeContext::Family>(4);auto model=r.text();auto route=r.text();auto prefix=r.number();require(prefix<=state_->limits.max_messages && r.text()==state_->descriptor.policy()->identity());
        auto c=std::shared_ptr<NativeContext>(new NativeContext(family,std::move(model),std::move(route),static_cast<size_t>(prefix),state_->descriptor.policy()));c->origin_=r.digest();c->prefix_=r.digest();c->valid_=r.boolean();c->history_valid_=r.boolean();c->replay_eligible_=r.boolean();auto pending=r.count(state_->limits.max_parts);c->pending_server_tools_.reserve(pending);while(pending--){auto a=r.text();auto b=r.text();c->pending_server_tools_.emplace_back(std::move(a),std::move(b));}c->client_tools_=r.strings();auto stop=r.enumeration<StopKind>(10);auto digest=r.digest();auto complete=r.boolean();m.native=std::shared_ptr<const NativeReplay>(new NativeReplay(std::move(c),stop,digest,complete));require(m.native->archive_valid(m) && m.native->context_->matches_descriptor(state_->descriptor));
      }
      messages.push_back(std::move(m));
    }
    require(r.data.empty());require(unchanged(identity,entry_status(state_->root,name)));private_file(fd,true);state_->verify();return messages;
  }catch(const Rejected&){return error();}
}
} // namespace sp
