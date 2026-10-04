#include "core/native_archive.h"
#include "core/native.h"
#include "codecs/messages.h"
#include "codecs/messages_request.h"
#include "json/json.h"
#include <array>
#include <barrier>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <type_traits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using namespace sp;
namespace fs = std::filesystem;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("portable archive check failed at " + std::to_string(__LINE__)); } while (false)

std::string utf8(const fs::path& path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}
#ifdef _WIN32
struct PrivateSecurity {
  std::vector<unsigned char> token, acl;
  SECURITY_DESCRIPTOR descriptor{};
  explicit PrivateSecurity(DWORD access) {
    HANDLE handle = nullptr;
    CHECK(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &handle));
    DWORD size = 0;
    ::GetTokenInformation(handle, TokenUser, nullptr, 0, &size);
    token.resize(size);
    const BOOL ok = ::GetTokenInformation(handle, TokenUser, token.data(), size, &size);
    ::CloseHandle(handle);
    CHECK(ok);
    size = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + ::GetLengthSid(sid()));
    acl.resize(size);
    CHECK(::InitializeAcl(dacl(), size, ACL_REVISION));
    CHECK(::AddAccessAllowedAceEx(dacl(), ACL_REVISION, 0, access, sid()));
    CHECK(::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION));
    CHECK(::SetSecurityDescriptorOwner(&descriptor, sid(), FALSE));
    CHECK(::SetSecurityDescriptorDacl(&descriptor, TRUE, dacl(), FALSE));
    CHECK(::SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED));
  }
  PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(token.data())->User.Sid; }
  PACL dacl() { return reinterpret_cast<PACL>(acl.data()); }
};
#endif
void private_directory(const fs::path& path) {
#ifdef _WIN32
  PrivateSecurity security(FILE_ALL_ACCESS);
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &security.descriptor, FALSE};
  CHECK(::CreateDirectoryW(path.c_str(), &attributes));
#else
  CHECK(::mkdir(path.c_str(), 0700) == 0);
#endif
}
void permissions(const fs::path& path, bool directory, bool sealed) {
#ifdef _WIN32
  PrivateSecurity security(sealed ? FILE_GENERIC_READ : FILE_ALL_ACCESS);
  CHECK(::SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      nullptr, nullptr, security.dacl(), nullptr) == ERROR_SUCCESS);
  (void)directory;
#else
  CHECK(::chmod(path.c_str(), directory ? 0700 : sealed ? 0400 : 0600) == 0);
#endif
}
void assert_private(const fs::path& path, bool directory) {
#ifdef _WIN32
  PrivateSecurity current(directory ? FILE_ALL_ACCESS : FILE_GENERIC_READ);
  PSID owner = nullptr;
  PACL acl = nullptr;
  PSECURITY_DESCRIPTOR security = nullptr;
  CHECK(::GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &security) == ERROR_SUCCESS);
  SECURITY_DESCRIPTOR_CONTROL control{};
  DWORD revision = 0;
  ACL_SIZE_INFORMATION information{};
  void* raw = nullptr;
  const bool valid = owner && ::EqualSid(owner, current.sid()) && acl &&
      ::GetSecurityDescriptorControl(security, &control, &revision) && (control & SE_DACL_PROTECTED) &&
      ::GetAclInformation(acl, &information, sizeof(information), AclSizeInformation) && information.AceCount == 1 &&
      ::GetAce(acl, 0, &raw);
  bool exact = false;
  if (valid) {
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
    exact = ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceFlags == 0 &&
        ace->Mask == (directory ? FILE_ALL_ACCESS : FILE_GENERIC_READ) &&
        ::EqualSid(const_cast<DWORD*>(&ace->SidStart), current.sid());
  }
  ::LocalFree(security);
  CHECK(valid && exact);
#else
  struct stat status{};
  CHECK(::lstat(path.c_str(), &status) == 0);
  CHECK(status.st_uid == ::geteuid());
  CHECK((status.st_mode & 07777) == (directory ? 0700 : 0400));
  CHECK(directory ? S_ISDIR(status.st_mode) : S_ISREG(status.st_mode) && status.st_nlink == 1);
#endif
}
struct Fixture {
  fs::path root;
  Fixture() {
    // Canonicalize the OS temp directory: macOS /var and /tmp may be symlinks.
    const auto parent = fs::canonical(fs::temp_directory_path());
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    std::string name = "sp-archive-portable-";
    for (int i = 0; i < 32; ++i) name += digits[random() & 15];
    root = parent / name;
    private_directory(root);
    try {
      private_directory(root / "keys");
      assert_private(root, true);
      assert_private(root / "keys", true);
    } catch (...) {
      std::error_code ignored;
      fs::remove_all(root, ignored);
      throw;
    }
  }
  ~Fixture() {
    // Only this randomly named, exclusively created fixture is removed.
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  fs::path directory() const { return root / "records"; }
  fs::path key() const { return root / "keys" / "activation"; }
};
std::string bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  CHECK(input.good());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write_bytes(const fs::path& path, std::string_view value) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  CHECK(output.good());
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
  output.close();
  CHECK(output.good());
}
descriptor::ValidatedDescriptor descriptor() {
  auto value = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"portable-archive-fixture","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(value));
  return std::get<descriptor::ValidatedDescriptor>(std::move(value));
}
std::shared_ptr<NativeArchive> admitted(NativeArchive::Activation value) {
  CHECK(std::holds_alternative<std::shared_ptr<NativeArchive>>(value));
  return std::get<std::shared_ptr<NativeArchive>>(std::move(value));
}
std::shared_ptr<NativeArchive> provision(const Fixture& f, const descriptor::ValidatedDescriptor& d) {
  return admitted(NativeArchive::provision(utf8(f.directory()), utf8(f.key()), "synthetic-owner", d));
}
std::shared_ptr<NativeArchive> reopen(const Fixture& f, const descriptor::ValidatedDescriptor& d) {
  return admitted(NativeArchive::open(utf8(f.directory()), utf8(f.key()), "synthetic-owner", d));
}
std::string saved(NativeArchive::Saved value) {
  CHECK(std::holds_alternative<std::string>(value));
  return std::get<std::string>(std::move(value));
}
std::vector<Message> loaded(NativeArchive::Loaded value) {
  CHECK(std::holds_alternative<std::vector<Message>>(value));
  return std::get<std::vector<Message>>(std::move(value));
}
std::shared_ptr<const json::Document> document(std::string_view text) {
  auto value = json::parse(text);
  CHECK(std::holds_alternative<json::Document>(value));
  return std::make_shared<const json::Document>(std::get<json::Document>(std::move(value)));
}
Message user(std::string text) { return Message{"", Role::User, {Text{std::move(text)}}}; }
messages::Request request() {
  messages::Request result;
  result.model = "fixture-model";
  result.account_scope = "fixture-account";
  result.messages.push_back(user("hello"));
  return result;
}
messages::EncodedRequest encoded(const descriptor::ValidatedDescriptor& d, const messages::Request& r) {
  auto value = messages::encode(d, r, false);
  CHECK(std::holds_alternative<messages::EncodedRequest>(value));
  return std::get<messages::EncodedRequest>(std::move(value));
}
Completion decode(const descriptor::ValidatedDescriptor& d, const messages::Request& r, std::string_view wire) {
  auto initial = encoded(d, r);
  Accumulator accumulator;
  messages::Codec codec(d, messages::Mode::Buffered, accumulator, initial.context);
  codec.buffered(wire, {});
  codec.finish();
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  return std::get<Completion>(*accumulator.outcome());
}
std::string wire(std::string_view usage) {
  return R"({"id":"msg_portable","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"thinking","thinking":"reason","signature":"synthetic-fixture-signature"},{"type":"redacted_thinking","data":"synthetic-fixture-state"},{"type":"text","text":"answer"}],"stop_reason":"end_turn","stop_sequence":null,"usage":)" + std::string(usage) + "}";
}
void same_document(const std::shared_ptr<const json::Document>& a, const std::shared_ptr<const json::Document>& b) {
  CHECK(bool(a) == bool(b));
  if (a) CHECK(json::equal(a->root(), b->root()));
}
void same_part(const Part& a, const Part& b) {
  CHECK(a.index() == b.index());
  std::visit([&](const auto& p) {
    using P = std::decay_t<decltype(p)>;
    const auto& q = std::get<P>(b);
    if constexpr (std::is_same_v<P, Text>) CHECK(p.value == q.value);
    else if constexpr (std::is_same_v<P, Refusal>) CHECK(p.text == q.text && p.raw_code == q.raw_code);
    else if constexpr (std::is_same_v<P, ToolCall> || std::is_same_v<P, InvalidToolCall>) {
      CHECK(p.id == q.id && p.name == q.name && p.kind == q.kind && p.wire_type == q.wire_type);
      same_document(p.wire_metadata, q.wire_metadata);
      if constexpr (std::is_same_v<P, ToolCall>) same_document(p.input, q.input);
      else CHECK(p.raw_fragment == q.raw_fragment && p.reason == q.reason);
    } else if constexpr (std::is_same_v<P, Thinking>) CHECK(p.text == q.text && p.signature == q.signature);
    else if constexpr (std::is_same_v<P, RedactedThinking>) CHECK(p.data == q.data);
    else if constexpr (std::is_same_v<P, ServerToolResult>) { CHECK(p.tool_use_id == q.tool_use_id && p.wire_type == q.wire_type); same_document(p.content, q.content); }
    else if constexpr (std::is_same_v<P, ToolResult>) {
      CHECK(p.tool_use_id == q.tool_use_id && p.content == q.content && p.is_error == q.is_error && p.host.has_value() == q.host.has_value());
      if (p.host) CHECK(p.host->name == q.host->name && p.host->status == q.host->status && p.host->retryable == q.host->retryable && p.host->effect_uncertain == q.host->effect_uncertain);
    } else if constexpr (std::is_same_v<P, Reasoning>) CHECK(p.id == q.id && p.summary == q.summary && p.encrypted_content == q.encrypted_content && p.status == q.status && p.content == q.content);
    else if constexpr (std::is_same_v<P, Opaque>) { CHECK(p.wire_type == q.wire_type); same_document(p.wire_metadata, q.wire_metadata); }
    else if constexpr (std::is_same_v<P, Image>) { CHECK(p.mime == q.mime && p.detail == q.detail && bool(p.data) == bool(q.data)); if (p.data) CHECK(*p.data == *q.data); }
    else if constexpr (std::is_same_v<P, Thought>) CHECK(p.summary == q.summary && p.signature == q.signature);
  }, a);
}
void same_message(const Message& a, const Message& b) {
  CHECK(a.id == b.id && a.role == b.role && a.parts.size() == b.parts.size());
  CHECK(bool(a.native) == bool(b.native));
  if (a.native) CHECK(a.native->complete() == b.native->complete());
  same_document(a.wire_output, b.wire_output);
  for (size_t i = 0; i < a.parts.size(); ++i) same_part(a.parts[i], b.parts[i]);
}
void roundtrip_and_continuation() {
  Fixture f;
  const auto d = descriptor();
  auto r = request();
  const auto completion = decode(d, r, wire(R"({"input_tokens":2,"output_tokens":3})"));
  CHECK(completion.messages.size() == 1 && completion.messages[0].native && completion.messages[0].native->complete());
  CHECK(!completion.usage.input_total && !completion.usage.cache_read && !completion.usage.cache_write);
  CHECK(completion.usage.input_uncached && completion.usage.input_uncached->value == 2);
  r.messages.push_back(completion.messages[0]);
  r.messages.push_back(user("continue"));
  const auto expected = encoded(d, r).body;
  auto archive = provision(f, d);
  const auto reference = saved(archive->save(r.messages, "native-call"));
  CHECK(reference.size() == 70 && reference.substr(0, 6) == "spna3:");
  assert_private(f.directory(), true);
  assert_private(f.key(), false);
  assert_private(f.directory() / reference.substr(6), false);
  archive.reset();
  archive = reopen(f, d);
  const auto restored = loaded(archive->load(reference, "native-call"));
  CHECK(restored.size() == r.messages.size());
  for (size_t i = 0; i < restored.size(); ++i) same_message(r.messages[i], restored[i]);
  r.messages = restored;
  CHECK(encoded(d, r).body == expected);
  std::get<Thinking>(r.messages[1].parts[0]).signature = "changed";
  CHECK(std::holds_alternative<Error>(archive->save(r.messages, "native-call")));
  CHECK(std::holds_alternative<Error>(messages::encode(d, r, false)));
  r.messages = restored;
  r.messages[1].native.reset();
  CHECK(std::holds_alternative<Error>(messages::encode(d, r, false)));

  const auto object = document(R"({"missing":null,"zero":0,"large":18446744073709551615,"ordered":["second","first"]})");
  Message portable{"portable", Role::Developer, {
      Text{std::string("a\0b", 3)}, Refusal{"no", "policy"},
      ToolCall{"call", "lookup", ToolCallKind::ApprovalRequest, object, "function_call", object},
      InvalidToolCall{"bad", "lookup", ToolCallKind::ClientExecuted, "{", InvalidReason::Truncated, "function_call", object},
      Thinking{"unknown", std::nullopt}, Thinking{"empty", std::string{}}, RedactedThinking{"redacted"},
      ServerToolResult{"server", "search_result", object}, ToolResult{"call", "error", true, ToolResultHostMetadata{"lookup", "uncertain", true, true}},
      Reasoning{"reason", {"one", "two"}, "encrypted", "completed", {"body"}}, Opaque{"future", object},
      Image{"image/png", std::make_shared<const std::string>("AAEC"), ImageDetail::Original}, Thought{{"thought"}, "sig"}}, {}, object};
  // Archive stores Message, not Outcome. Persist authentic diagnostic envelopes
  // without native authority, then decode them to check usage knowledge survives.
  auto initial = request();
  const auto zero = decode(d, initial, wire(R"({"input_tokens":0,"output_tokens":0,"cache_read_input_tokens":0,"cache_creation_input_tokens":0})"));
  CHECK(zero.usage.total && zero.usage.total->value == 0 && zero.usage.cache_read && zero.usage.cache_read->value == 0);
  CHECK(completion.raw_events.size() == 1 && completion.raw_events[0].payload);
  CHECK(zero.raw_events.size() == 1 && zero.raw_events[0].payload);
  Message unknown_diagnostic{"unknown", Role::Assistant, {}, {}, completion.raw_events[0].payload};
  Message zero_diagnostic{"zero", Role::Assistant, {}, {}, zero.raw_events[0].payload};
  const std::vector<Message> diagnostics{portable, unknown_diagnostic, zero_diagnostic};
  const auto diagnostic_ref = saved(archive->save(diagnostics, "portable-call"));
  archive.reset();
  archive = reopen(f, d);
  const auto actual = loaded(archive->load(diagnostic_ref, "portable-call"));
  CHECK(actual.size() == diagnostics.size());
  for (size_t i = 0; i < actual.size(); ++i) same_message(diagnostics[i], actual[i]);
  const auto unknown_again = decode(d, initial, actual[1].wire_output->root().dump());
  const auto zero_again = decode(d, initial, actual[2].wire_output->root().dump());
  CHECK(!unknown_again.usage.total && !unknown_again.usage.cache_read && !unknown_again.usage.cache_write);
  CHECK(zero_again.usage.total && zero_again.usage.total->value == 0);
  CHECK(zero_again.usage.cache_read && zero_again.usage.cache_read->value == 0);
}
void admission_and_publication() {
  Fixture f, other;
  const auto d = descriptor();
  auto archive = provision(f, d);
  const std::vector<Message> history{user("first")};
  const auto reference = saved(archive->save(history, "bound-call"));
  const auto record = f.directory() / reference.substr(6);
  const auto original = bytes(record);
  CHECK(saved(archive->save(history, "bound-call")) == reference);
  CHECK(bytes(record) == original);
  CHECK(std::holds_alternative<Error>(archive->load(reference, "wrong-call")));
  CHECK(std::holds_alternative<Error>(archive->load("spna3:" + std::string(64, '0'), "bound-call")));
  CHECK(std::holds_alternative<Error>(archive->load("spna3:../activation", "bound-call")));
  CHECK(std::holds_alternative<Error>(NativeArchive::open(utf8(f.directory()), utf8(f.key()), "other-owner", d)));
  archive.reset();
  auto other_archive = provision(other, d);
  other_archive.reset();
  CHECK(std::holds_alternative<Error>(NativeArchive::open(utf8(f.directory()), utf8(other.key()), "synthetic-owner", d)));
  CHECK(std::holds_alternative<Error>(NativeArchive::open(utf8(other.directory()), utf8(f.key()), "synthetic-owner", d)));
  // Provision must not overwrite an already sealed independent activation.
  const auto key_before = bytes(f.key());
  CHECK(std::holds_alternative<Error>(NativeArchive::provision(utf8(f.root / "another-store"), utf8(f.key()), "synthetic-owner", d)));
  CHECK(bytes(f.key()) == key_before);
  assert_private(f.key(), false);

  permissions(record, false, false);
  archive = reopen(f, d);
  CHECK(std::holds_alternative<Error>(archive->load(reference, "bound-call")));
  archive.reset();
  auto corrupt = original;
  CHECK(corrupt.size() > 16);
  corrupt[16] ^= 1;
  write_bytes(record, corrupt);
  permissions(record, false, true);
  archive = reopen(f, d);
  CHECK(std::holds_alternative<Error>(archive->load(reference, "bound-call")));
  // An occupied content-addressed name cannot be repaired by replacing it.
  CHECK(std::holds_alternative<Error>(archive->save(history, "bound-call")));
  CHECK(bytes(record) == corrupt);
  for (const auto& entry : fs::directory_iterator(f.directory())) CHECK(entry.path().filename() == record.filename());
  archive.reset();
  permissions(f.key(), false, false);
  CHECK(std::holds_alternative<Error>(NativeArchive::open(utf8(f.directory()), utf8(f.key()), "synthetic-owner", d)));
  permissions(f.key(), false, true);
  const auto nested = f.directory() / "nested-keys";
  private_directory(nested);
  fs::rename(f.key(), nested / "activation");
  // Same genuine activation inode, but now inside the archive tree.
  CHECK(std::holds_alternative<Error>(NativeArchive::open(utf8(f.directory()), utf8(nested / "activation"), "synthetic-owner", d)));
}
void concurrent_publication() {
  Fixture f;
  const auto d = descriptor();
  auto first = provision(f, d);
  auto second = reopen(f, d);
  const std::vector<Message> history{user("concurrent content")};
  std::barrier start(3);
  auto save = [&](const std::shared_ptr<NativeArchive>& archive) {
    start.arrive_and_wait();
    return saved(archive->save(history, "concurrent-call"));
  };
  auto one = std::async(std::launch::async, save, first);
  auto two = std::async(std::launch::async, save, second);
  start.arrive_and_wait();
  const auto reference = one.get();
  CHECK(two.get() == reference);
  first.reset();
  second.reset();
  auto archive = reopen(f, d);
  const auto restored = loaded(archive->load(reference, "concurrent-call"));
  CHECK(restored.size() == 1);
  same_message(history[0], restored[0]);
  assert_private(f.directory() / reference.substr(6), false);
  for (const auto& entry : fs::directory_iterator(f.directory()))
    CHECK(entry.path().filename() == reference.substr(6));
}
void directory_link(const fs::path& link, const fs::path& target) {
#ifdef _WIN32
  // NTFS junctions exercise reparse rejection without symlink privilege or
  // Developer Mode. The target is also owned by this test fixture.
  private_directory(link);
  struct Junction {
    DWORD tag;
    WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
    wchar_t paths[4096];
  } data{};
  const auto print = target.native();
  const auto substitute = std::wstring(L"\\??\\") + print;
  CHECK(substitute.size() + print.size() + 2 < 4096);
  data.tag = IO_REPARSE_TAG_MOUNT_POINT;
  data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
  data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
  data.print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
  std::memcpy(data.paths, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
  std::memcpy(reinterpret_cast<unsigned char*>(data.paths) + data.print_offset, print.c_str(), (print.size() + 1) * sizeof(wchar_t));
  data.length = static_cast<WORD>(8 + (substitute.size() + print.size() + 2) * sizeof(wchar_t));
  const HANDLE handle = ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  CHECK(handle != INVALID_HANDLE_VALUE);
  DWORD returned = 0;
  const BOOL ok = ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
      static_cast<DWORD>(data.length + 8), nullptr, 0, &returned, nullptr);
  ::CloseHandle(handle);
  CHECK(ok);
  CHECK((::GetFileAttributesW(link.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
#else
  CHECK(::symlink(target.c_str(), link.c_str()) == 0);
#endif
}
void permissions_and_reparse() {
  Fixture f;
  const auto d = descriptor();
  auto archive = provision(f, d);
  const auto reference = saved(archive->save({user("retained")}, "link-call"));
  archive.reset();
  const auto link = f.root / "linked-records";
  directory_link(link, f.directory());
  CHECK(std::holds_alternative<Error>(NativeArchive::open(utf8(link), utf8(f.key()), "synthetic-owner", d)));
  const auto linked_keys = f.root / "linked-keys";
  directory_link(linked_keys, f.root / "keys");
  CHECK(std::holds_alternative<Error>(NativeArchive::open(utf8(f.directory()), utf8(linked_keys / "activation"), "synthetic-owner", d)));
#ifdef _WIN32
  CHECK(::RemoveDirectoryW(link.c_str()));
  CHECK(::RemoveDirectoryW(linked_keys.c_str()));
  // A current-owner writable but unprotected DACL is not private custody.
  PrivateSecurity security(FILE_ALL_ACCESS);
  CHECK(::SetNamedSecurityInfoW(const_cast<wchar_t*>(f.directory().c_str()), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, security.dacl(), nullptr) == ERROR_SUCCESS);
#else
  CHECK(::chmod(f.directory().c_str(), 0755) == 0);
#endif
  CHECK(std::holds_alternative<Error>(NativeArchive::open(utf8(f.directory()), utf8(f.key()), "synthetic-owner", d)));
  permissions(f.directory(), true, false);
  archive = reopen(f, d);
  CHECK(std::get<Text>(loaded(archive->load(reference, "link-call"))[0].parts[0]).value == "retained");
}
}
int main() {
  try {
    roundtrip_and_continuation();
    admission_and_publication();
    concurrent_publication();
    permissions_and_reparse();
    std::cout << "native archive portable consumer behavior passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
