#include "core/native_archive_fs.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#if _WIN32_WINNT < 0x0601
#error Native archive custody requires Windows 7 or later filesystem primitives
#endif
#include <windows.h>
#include <aclapi.h>
#include <winternl.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <stdio.h>
#else
#error Native archive custody requires a supported atomic no-replace filesystem backend
#endif
#endif

namespace sp::archive_fs {
namespace {
#ifdef _WIN32
constexpr NativeHandle invalid_handle = nullptr;
void close_handle(NativeHandle value) noexcept { if (value) ::CloseHandle(value); }
using CreateFileFn = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using SetInformationFn = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
using FlushFn = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK);
struct NativeApi {
  CreateFileFn create;
  SetInformationFn set;
  FlushFn flush;
  NativeApi() {
    const auto module = ::GetModuleHandleW(L"ntdll.dll"); require(module != nullptr);
    // These are required primitives, not optional fallback implementations.
    create = reinterpret_cast<CreateFileFn>(::GetProcAddress(module, "NtCreateFile"));
    set = reinterpret_cast<SetInformationFn>(::GetProcAddress(module, "NtSetInformationFile"));
    flush = reinterpret_cast<FlushFn>(::GetProcAddress(module, "NtFlushBuffersFile"));
    require(create && set && flush);
  }
};
const NativeApi& api() { static const NativeApi value; return value; }
constexpr ULONG nt_directory = 0x00000001, nt_write_through = 0x00000002;
constexpr ULONG nt_synchronous = 0x00000020, nt_non_directory = 0x00000040;
constexpr ULONG nt_no_reparse = 0x00200000, object_no_reparse = 0x00001000;
constexpr ULONG nt_open = 1, nt_create = 2;
constexpr DWORD directory_access = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_TRAVERSE;
constexpr DWORD traversal_access = FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
constexpr DWORD sealed_access = FILE_GENERIC_READ;
struct User {
  std::vector<unsigned char> token;
  User() {
    HANDLE raw = nullptr;
    if (!::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &raw)) {
      require(::GetLastError() == ERROR_NO_TOKEN && ::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw));
    }
    Handle handle(raw); DWORD size = 0;
    require(!::GetTokenInformation(handle.get(), TokenUser, nullptr, 0, &size) && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    token.resize(size); require(::GetTokenInformation(handle.get(), TokenUser, token.data(), size, &size));
    require(::IsValidSid(sid()));
  }
  PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(token.data())->User.Sid; }
};
struct PrivateSecurity {
  User user;
  std::vector<unsigned char> acl;
  SECURITY_DESCRIPTOR descriptor{};
  explicit PrivateSecurity(DWORD access) {
    const DWORD size = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + ::GetLengthSid(user.sid()));
    acl.resize(size);
    auto* value = reinterpret_cast<PACL>(acl.data());
    require(::InitializeAcl(value, size, ACL_REVISION) && ::AddAccessAllowedAceEx(value, ACL_REVISION, 0, access, user.sid()));
    require(::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION));
    require(::SetSecurityDescriptorOwner(&descriptor, user.sid(), FALSE));
    require(::SetSecurityDescriptorDacl(&descriptor, TRUE, value, FALSE));
    require(::SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED));
  }
};
struct LocalSecurity {
  PSECURITY_DESCRIPTOR value = nullptr;
  ~LocalSecurity() { if (value) ::LocalFree(value); }
};
void private_acl(const Handle& handle, DWORD access) {
  User user; PSID owner = nullptr; PACL acl = nullptr; LocalSecurity security;
  require(::GetSecurityInfo(handle.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner, nullptr, &acl, nullptr, &security.value) == ERROR_SUCCESS);
  require(owner && ::IsValidSid(owner) && ::EqualSid(owner, user.sid()) && acl && ::IsValidAcl(acl));
  SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
  require(::GetSecurityDescriptorControl(security.value, &control, &revision) && (control & SE_DACL_PROTECTED));
  ACL_SIZE_INFORMATION information{};
  require(::GetAclInformation(acl, &information, sizeof(information), AclSizeInformation) && information.AceCount == 1);
  void* raw = nullptr; require(::GetAce(acl, 0, &raw));
  const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
  require(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceFlags == 0 && ace->Mask == access);
  const auto sid = const_cast<DWORD*>(&ace->SidStart);
  require(::IsValidSid(sid) && ::EqualSid(sid, user.sid()));
}
std::wstring wide(std::string_view text) {
  require(!text.empty() && text.size() <= static_cast<size_t>(std::numeric_limits<int>::max()) && text.find('\0') == text.npos);
  const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  require(size > 0); std::wstring out(static_cast<size_t>(size), L'\0');
  require(::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), size) == size);
  return out;
}
void component(std::wstring_view name) {
  require(!name.empty() && name != L"." && name != L".." && name.back() != L'.' && name.back() != L' ');
  require(std::none_of(name.begin(), name.end(), [](wchar_t c) { return c < 32 || c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|'; }));
}
Handle native_open(NativeHandle parent, std::wstring_view name, DWORD access, bool directory,
                   bool create, PrivateSecurity* security = nullptr, bool allow_missing = false) {
  require(name.size() <= (std::numeric_limits<USHORT>::max() / sizeof(wchar_t)));
  UNICODE_STRING text{}; text.Buffer = const_cast<wchar_t*>(name.data());
  text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t)); text.MaximumLength = text.Length;
  OBJECT_ATTRIBUTES attributes{}; attributes.Length = sizeof(attributes); attributes.RootDirectory = parent;
  attributes.ObjectName = &text; attributes.Attributes = OBJ_CASE_INSENSITIVE | object_no_reparse;
  attributes.SecurityDescriptor = security ? &security->descriptor : nullptr;
  IO_STATUS_BLOCK io{}; HANDLE raw = nullptr;
  const auto result = api().create(&raw, access, &attributes, &io, nullptr,
      directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
      directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ,
      create ? nt_create : nt_open,
      (directory ? nt_directory : nt_non_directory) | nt_synchronous | nt_no_reparse | (create ? nt_write_through : 0), nullptr, 0);
  // Only a genuinely absent last component may be treated as a cache miss.
  if (allow_missing && result == static_cast<NTSTATUS>(0xc0000034UL)) return {};
  require(result == 0 && raw != nullptr); Handle out(raw);
  FILE_ATTRIBUTE_TAG_INFO tag{};
  require(::GetFileInformationByHandleEx(out.get(), FileAttributeTagInfo, &tag, sizeof(tag)));
  require(!(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && bool(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory);
  return out;
}
struct WindowsStatus {
  BY_HANDLE_FILE_INFORMATION file{};
  FILE_BASIC_INFO basic{};
  FILE_STANDARD_INFO standard{};
  explicit WindowsStatus(const Handle& handle) {
    require(::GetFileType(handle.get()) == FILE_TYPE_DISK);
    require(::GetFileInformationByHandle(handle.get(), &file));
    require(::GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic)));
    require(::GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard, sizeof(standard)));
    require(!standard.DeletePending && !(file.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && standard.EndOfFile.QuadPart >= 0);
  }
};
Handle reopen_directory(const Handle& handle, DWORD access) {
  const WindowsStatus before(handle); require(before.standard.Directory);
  FILE_ID_DESCRIPTOR identity{}; identity.dwSize = sizeof(identity); identity.Type = FileIdType;
  identity.FileId.HighPart = static_cast<LONG>(before.file.nFileIndexHigh); identity.FileId.LowPart = before.file.nFileIndexLow;
  const auto raw = ::OpenFileById(handle.get(), &identity, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH);
  require(raw != INVALID_HANDLE_VALUE); Handle out(raw);
  require(same_file(status(handle), status(out))); return out;
}
std::wstring absolute_path(std::string_view input) {
  auto path = wide(input); std::replace(path.begin(), path.end(), L'/', L'\\');
  require(path.back() != L'\\');
  const bool drive = path.size() >= 3 && path[1] == L':' && path[2] == L'\\';
  if (!drive) {
    // Reject drive-relative, rooted, UNC, device and extended namespace paths.
    require(path.front() != L'\\' && path.find(L':') == path.npos);
    const DWORD size = ::GetCurrentDirectoryW(0, nullptr); require(size > 0);
    std::wstring current(size, L'\0'); const DWORD count = ::GetCurrentDirectoryW(size, current.data());
    require(count > 0 && count < size); current.resize(count);
    if (current.back() != L'\\') current += L'\\'; path = current + path;
  }
  require(path.size() >= 4 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && path[2] == L'\\');
  return path;
}
void supported_volume(const Handle& root, std::wstring_view drive) {
  require(::GetDriveTypeW(std::wstring(drive).c_str()) == DRIVE_FIXED);
  DWORD flags = 0; wchar_t filesystem[32]{};
  require(::GetVolumeInformationByHandleW(root.get(), nullptr, 0, nullptr, nullptr, &flags, filesystem, 32));
  // FAT/exFAT have no private DACL; remote and other filesystems are not admitted
  // without equivalent identity, custody and directory-flush qualification.
  require((flags & FILE_PERSISTENT_ACLS) && std::wstring_view(filesystem) == L"NTFS");
}
#else
constexpr NativeHandle invalid_handle = -1;
void close_handle(NativeHandle value) noexcept { if (value >= 0) ::close(value); }
struct stat posix_status(const Handle& handle) { struct stat value{}; require(::fstat(handle.get(), &value) == 0); return value; }
void component(std::string_view name) { require(!name.empty() && name != "." && name != ".." && name.find('/') == name.npos && name.find('\0') == name.npos); }
Status convert(const struct stat& value) {
  require(value.st_size >= 0); Status out;
  out.device = static_cast<uint64_t>(value.st_dev); out.inode = static_cast<uint64_t>(value.st_ino); out.size = static_cast<uint64_t>(value.st_size);
#if defined(__APPLE__)
  out.modified = value.st_mtimespec.tv_sec; out.modified_nanoseconds = value.st_mtimespec.tv_nsec;
  out.changed = value.st_ctimespec.tv_sec; out.changed_nanoseconds = value.st_ctimespec.tv_nsec;
#else
  out.modified = value.st_mtim.tv_sec; out.modified_nanoseconds = value.st_mtim.tv_nsec;
  out.changed = value.st_ctim.tv_sec; out.changed_nanoseconds = value.st_ctim.tv_nsec;
#endif
  return out;
}
#endif
} // namespace

Handle::Handle() noexcept : value_(invalid_handle) {}
Handle::Handle(NativeHandle value) noexcept : value_(value) {}
Handle::~Handle() { close_handle(value_); }
Handle::Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, invalid_handle)) {}
Handle& Handle::operator=(Handle&& other) noexcept { if (this != &other) { close_handle(value_); value_ = std::exchange(other.value_, invalid_handle); } return *this; }
Handle::operator bool() const noexcept { return value_ != invalid_handle; }
bool same_file(const Status& a, const Status& b) noexcept { return a.device == b.device && a.inode == b.inode; }
bool unchanged(const Status& a, const Status& b) noexcept {
  return same_file(a, b) && a.size == b.size && a.modified == b.modified && a.changed == b.changed &&
      a.modified_nanoseconds == b.modified_nanoseconds && a.changed_nanoseconds == b.changed_nanoseconds;
}
StoreLock::StoreLock(const Handle& root, const Handle& key)
#ifdef _WIN32
    : value_(key.get()) {
  (void)root; OVERLAPPED offset{}; offset.Offset = 96;
  // A lock past the immutable activation's EOF serializes saves without blocking
  // activation reads or introducing an uncounted lock file into the archive.
  require(::LockFileEx(value_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &offset));
#else
    : value_(root.get()) {
  (void)key; require(::flock(value_, LOCK_EX) == 0);
#endif
}
StoreLock::~StoreLock() {
#ifdef _WIN32
  OVERLAPPED offset{}; offset.Offset = 96; ::UnlockFileEx(value_, 0, 1, 0, &offset);
#else
  ::flock(value_, LOCK_UN);
#endif
}
Location locate(std::string_view input) {
#ifdef _WIN32
  const auto path = absolute_path(input); const auto drive = path.substr(0, 3);
  wchar_t mapping[4096]{}; const auto device_name = drive.substr(0, 2);
  require(::QueryDosDeviceW(device_name.c_str(), mapping, 4096) != 0);
  const std::wstring_view device(mapping); constexpr std::wstring_view prefix = L"\\Device\\HarddiskVolume";
  require(device.size() > prefix.size() && device.substr(0, prefix.size()) == prefix &&
      std::all_of(device.begin() + static_cast<std::ptrdiff_t>(prefix.size()), device.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }));
  // Open the resolved volume, not a mutable DOS mapping or SUBST ancestor.
  Handle current = native_open(nullptr, std::wstring(device) + L"\\", traversal_access, true, false);
  supported_volume(current, drive); Location out; size_t position = 3;
  while (true) {
    const auto end = path.find(L'\\', position);
    const auto part = std::wstring_view(path).substr(position, end == path.npos ? path.size() - position : end - position); component(part);
    if (end == path.npos) {
      // Keep UTF-8 names in the private cross-platform interface.
      const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, part.data(), static_cast<int>(part.size()), nullptr, 0, nullptr, nullptr);
      require(size > 0); out.name.resize(static_cast<size_t>(size));
      require(::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, part.data(), static_cast<int>(part.size()), out.name.data(), size, nullptr, nullptr) == size);
      out.parent = std::move(current); return out;
    }
    auto next = native_open(current.get(), part, traversal_access, true, false);
    out.ancestors.push_back(std::move(current)); current = std::move(next); position = end + 1;
  }
#else
  require(!input.empty() && input.back() != '/' && input.find('\0') == input.npos);
  Handle current(::open(input.front() == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)); require(bool(current));
  size_t position = input.front() == '/' ? 1 : 0;
  while (true) {
    const auto end = input.find('/', position); std::string part(input.substr(position, end == input.npos ? input.size() - position : end - position)); component(part);
    if (end == input.npos) return {{}, std::move(current), std::move(part)};
    Handle next(::openat(current.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)); require(bool(next)); current = std::move(next); position = end + 1;
  }
#endif
}
Handle open_directory(const Handle& parent, const std::string& name) {
#ifdef _WIN32
  const auto part = wide(name); component(part); return native_open(parent.get(), part, traversal_access, true, false);
#else
  component(name); Handle out(::openat(parent.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)); require(bool(out)); return out;
#endif
}
Handle create_directory(const Handle& parent, const std::string& name) {
#ifdef _WIN32
  const auto part = wide(name); component(part); PrivateSecurity security(FILE_ALL_ACCESS);
  return native_open(parent.get(), part, traversal_access, true, true, &security);
#else
  component(name); require(::mkdirat(parent.get(), name.c_str(), 0700) == 0); return open_directory(parent, name);
#endif
}
Handle open_file(const Handle& parent, const std::string& name, bool allow_missing) {
#ifdef _WIN32
  const auto part = wide(name); component(part); return native_open(parent.get(), part, FILE_GENERIC_READ, false, false, nullptr, allow_missing);
#else
  component(name); Handle out(::openat(parent.get(), name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (!out) require(allow_missing && errno == ENOENT);
  return out;
#endif
}
Handle create_file(const Handle& parent, const std::string& name) {
#ifdef _WIN32
  const auto part = wide(name); component(part); PrivateSecurity security(FILE_ALL_ACCESS);
  return native_open(parent.get(), part, FILE_GENERIC_READ | FILE_GENERIC_WRITE | WRITE_DAC | DELETE, false, true, &security);
#else
  component(name); Handle out(::openat(parent.get(), name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)); require(bool(out)); return out;
#endif
}
Status status(const Handle& handle) {
#ifdef _WIN32
  const WindowsStatus value(handle); Status out;
  out.device = value.file.dwVolumeSerialNumber; out.inode = (static_cast<uint64_t>(value.file.nFileIndexHigh) << 32) | value.file.nFileIndexLow;
  out.size = static_cast<uint64_t>(value.standard.EndOfFile.QuadPart); out.modified = value.basic.LastWriteTime.QuadPart; out.changed = value.basic.ChangeTime.QuadPart;
  return out;
#else
  return convert(posix_status(handle));
#endif
}
void private_directory(const Handle& handle) {
#ifdef _WIN32
  const WindowsStatus value(handle); require(value.standard.Directory); private_acl(handle, FILE_ALL_ACCESS);
#else
  const auto value = posix_status(handle); require(S_ISDIR(value.st_mode) && value.st_uid == ::geteuid() && (value.st_mode & 07777) == 0700);
#endif
}
void private_file(const Handle& handle, bool read_only) {
#ifdef _WIN32
  const WindowsStatus value(handle); require(!value.standard.Directory && value.standard.NumberOfLinks == 1 && value.file.nNumberOfLinks == 1);
  private_acl(handle, read_only ? sealed_access : FILE_ALL_ACCESS);
#else
  const auto value = posix_status(handle); require(S_ISREG(value.st_mode) && value.st_uid == ::geteuid() && (value.st_mode & 07777) == (read_only ? 0400 : 0600) && value.st_nlink == 1);
#endif
}
void independent_key_parent(const Handle& root, const Location& key) {
  const auto archive = status(root);
#ifdef _WIN32
  require(!same_file(archive, status(key.parent)));
  for (const auto& ancestor : key.ancestors) require(!same_file(archive, status(ancestor)));
#else
  Handle current(::openat(key.parent.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)); require(bool(current));
  while (true) {
    const auto here = status(current); require(!same_file(archive, here));
    Handle next(::openat(current.get(), "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)); require(bool(next));
    if (same_file(here, status(next))) return;
    current = std::move(next);
  }
#endif
}
void write_all(const Handle& handle, std::string_view bytes) {
  while (!bytes.empty()) {
#ifdef _WIN32
    DWORD written = 0; const DWORD count = static_cast<DWORD>(std::min<size_t>(bytes.size(), std::numeric_limits<DWORD>::max()));
    require(::WriteFile(handle.get(), bytes.data(), count, &written, nullptr) && written > 0); bytes.remove_prefix(written);
#else
    const auto count = ::write(handle.get(), bytes.data(), bytes.size()); if (count < 0 && errno == EINTR) continue;
    require(count > 0); bytes.remove_prefix(static_cast<size_t>(count));
#endif
  }
}
std::string read_all(const Handle& handle, size_t limit) {
  const auto before = status(handle); require(before.size <= limit); std::string bytes(static_cast<size_t>(before.size), '\0'); size_t done = 0;
#ifdef _WIN32
  LARGE_INTEGER offset{}; require(::SetFilePointerEx(handle.get(), offset, nullptr, FILE_BEGIN));
#endif
  while (done < bytes.size()) {
#ifdef _WIN32
    DWORD count = 0; const DWORD wanted = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, std::numeric_limits<DWORD>::max()));
    require(::ReadFile(handle.get(), bytes.data() + done, wanted, &count, nullptr) && count > 0); done += count;
#else
    const auto count = ::pread(handle.get(), bytes.data() + done, bytes.size() - done, static_cast<off_t>(done)); if (count < 0 && errno == EINTR) continue;
    require(count > 0); done += static_cast<size_t>(count);
#endif
  }
  require(unchanged(before, status(handle))); return bytes;
}
void seal_file(const Handle& handle) {
#ifdef _WIN32
  PrivateSecurity security(sealed_access);
  require(::SetSecurityInfo(handle.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      nullptr, nullptr, reinterpret_cast<PACL>(security.acl.data()), nullptr) == ERROR_SUCCESS);
#else
  require(::fchmod(handle.get(), 0400) == 0);
#endif
  private_file(handle, true);
}
void sync(const Handle& handle) {
#ifdef _WIN32
  // Keep long-lived directory handles at traverse/read-attribute access, as
  // required to avoid sharing conflicts with the native rename target open.
  // Flush through a checked reopen by the pinned NTFS identity, never a path.
  Handle directory;
  if (WindowsStatus(handle).standard.Directory) directory = reopen_directory(handle, directory_access);
  IO_STATUS_BLOCK io{};
  // Issue the normal synchronous IRP_MJ_FLUSH_BUFFERS request, not a
  // data-only/no-sync variant. Unsupported directory flushes fail closed.
  require(api().flush(directory ? directory.get() : handle.get(), &io) == 0);
#else
  require(::fsync(handle.get()) == 0);
#if defined(__APPLE__)
  // fsync alone does not order drive-cache writes on Darwin.
  require(::fcntl(handle.get(), F_FULLFSYNC) == 0);
#endif
#endif
}
void check_capacity(const Handle& root, size_t max_records, uint64_t max_bytes, size_t additional_bytes) {
  uint64_t bytes = 0; size_t records = 0;
  const auto account = [&](uint64_t size) { require(++records < max_records && size <= max_bytes - bytes); bytes += size; };
#ifdef _WIN32
  auto scan = reopen_directory(root, FILE_GENERIC_READ | FILE_TRAVERSE);
  alignas(FILE_ID_BOTH_DIR_INFO) std::array<unsigned char, 65536> buffer{}; bool restart = true;
  while (true) {
    if (!::GetFileInformationByHandleEx(scan.get(), restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
        buffer.data(), static_cast<DWORD>(buffer.size()))) { require(::GetLastError() == ERROR_NO_MORE_FILES); break; }
    restart = false; size_t offset = 0;
    while (true) {
      require(offset <= buffer.size() - offsetof(FILE_ID_BOTH_DIR_INFO, FileName));
      const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
      require(entry->FileNameLength % sizeof(wchar_t) == 0 && entry->FileNameLength <= buffer.size() - offset - offsetof(FILE_ID_BOTH_DIR_INFO, FileName));
      const std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
      if (name != L"." && name != L"..") {
        component(name); auto file = native_open(root.get(), name, FILE_GENERIC_READ, false, false);
        const WindowsStatus value(file); require(!value.standard.Directory && value.standard.NumberOfLinks == 1 && value.file.nNumberOfLinks == 1);
        // Sealed records and interrupted private staging records consume capacity.
        LocalSecurity security; PACL acl = nullptr;
        require(::GetSecurityInfo(file.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &security.value) == ERROR_SUCCESS && acl);
        void* raw = nullptr; require(::GetAce(acl, 0, &raw));
        const auto access = static_cast<const ACCESS_ALLOWED_ACE*>(raw)->Mask;
        require(access == sealed_access || access == FILE_ALL_ACCESS); private_acl(file, access);
        account(status(file).size);
      }
      if (!entry->NextEntryOffset) break;
      require(entry->NextEntryOffset >= offsetof(FILE_ID_BOTH_DIR_INFO, FileName) + entry->FileNameLength && entry->NextEntryOffset <= buffer.size() - offset);
      offset += entry->NextEntryOffset;
    }
  }
#else
  const int fd = ::openat(root.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW); require(fd >= 0);
  std::unique_ptr<DIR, decltype(&::closedir)> entries(::fdopendir(fd), ::closedir); if (!entries) { ::close(fd); throw Rejected{}; }
  errno = 0;
  while (auto* entry = ::readdir(entries.get())) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
    struct stat value{};
    require(::fstatat(root.get(), entry->d_name, &value, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(value.st_mode) && value.st_uid == ::geteuid() && (value.st_mode & 0077) == 0 && value.st_nlink == 1 && value.st_size >= 0);
    account(static_cast<uint64_t>(value.st_size)); errno = 0;
  }
  require(errno == 0);
#endif
  require(additional_bytes <= max_bytes - bytes);
}
void publish(const Handle& root, const Handle& staged, const std::string& temporary, const std::string& name) {
#ifdef _WIN32
  (void)temporary; const auto part = wide(name); component(part);
  // FILE_RENAME_INFORMATION's ABI; user-mode SDK headers do not expose it.
  struct Rename { BOOLEAN replace; HANDLE root; ULONG length; WCHAR name[1]; };
  const auto size = sizeof(Rename) + part.size() * sizeof(wchar_t); require(size <= std::numeric_limits<ULONG>::max());
  std::vector<unsigned char> buffer(size, 0); auto* rename = reinterpret_cast<Rename*>(buffer.data());
  rename->replace = FALSE; rename->root = root.get(); rename->length = static_cast<ULONG>(part.size() * sizeof(wchar_t));
  std::memcpy(rename->name, part.data(), rename->length); IO_STATUS_BLOCK io{};
  require(api().set(staged.get(), &io, rename, static_cast<ULONG>(buffer.size()), static_cast<FILE_INFORMATION_CLASS>(10)) == 0);
#elif defined(__APPLE__)
  (void)staged; require(::renameatx_np(root.get(), temporary.c_str(), root.get(), name.c_str(), RENAME_EXCL) == 0);
#else
  (void)staged; require(::syscall(SYS_renameat2, root.get(), temporary.c_str(), root.get(), name.c_str(), RENAME_NOREPLACE) == 0);
#endif
}
void finish_publication(const Handle& root, const Handle& staged) {
#ifdef _WIN32
  sync(staged);
#else
  (void)staged;
#endif
  sync(root);
}
void remove_staging(const Handle& root, const Handle& staged, const std::string& name) noexcept {
#ifdef _WIN32
  (void)root; (void)name; BOOLEAN remove = TRUE; IO_STATUS_BLOCK io{};
  api().set(staged.get(), &io, &remove, sizeof(remove), static_cast<FILE_INFORMATION_CLASS>(13));
#else
  (void)staged; ::unlinkat(root.get(), name.c_str(), 0);
#endif
}
Status entry_status(const Handle& root, const std::string& name) {
#ifdef _WIN32
  auto file = open_file(root, name); private_file(file, true); return status(file);
#else
  component(name); struct stat value{}; require(::fstatat(root.get(), name.c_str(), &value, AT_SYMLINK_NOFOLLOW) == 0); return convert(value);
#endif
}
} // namespace sp::archive_fs
