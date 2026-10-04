#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Private filesystem custody; no serialization or exported SDK interface.
namespace sp::archive_fs {
struct Rejected {};
inline void require(bool value) { if (!value) throw Rejected{}; }
#ifdef _WIN32
using NativeHandle = void*;
#else
using NativeHandle = int;
#endif
class Handle {
 public:
  Handle() noexcept;
  explicit Handle(NativeHandle value) noexcept;
  ~Handle();
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept;
  Handle& operator=(Handle&& other) noexcept;
  NativeHandle get() const noexcept { return value_; }
  explicit operator bool() const noexcept;
 private:
  NativeHandle value_;
};
struct Status {
  uint64_t device = 0, inode = 0, size = 0;
  int64_t modified = 0, changed = 0;
  long modified_nanoseconds = 0, changed_nanoseconds = 0;
};
struct Location {
  // Windows retains all ancestors without delete sharing until the operation
  // ends. POSIX openat traversal uses the parent descriptor directly.
  std::vector<Handle> ancestors;
  Handle parent;
  std::string name;
};
class StoreLock {
 public:
  StoreLock(const Handle& root, const Handle& key);
  ~StoreLock();
  StoreLock(const StoreLock&) = delete;
  StoreLock& operator=(const StoreLock&) = delete;
 private:
  NativeHandle value_;
};
Location locate(std::string_view path);
Handle open_directory(const Handle& parent, const std::string& name);
Handle create_directory(const Handle& parent, const std::string& name);
Handle open_file(const Handle& parent, const std::string& name, bool allow_missing = false);
Handle create_file(const Handle& parent, const std::string& name);
Status status(const Handle& handle);
bool same_file(const Status& a, const Status& b) noexcept;
bool unchanged(const Status& a, const Status& b) noexcept;
void private_directory(const Handle& handle);
void private_file(const Handle& handle, bool read_only);
void independent_key_parent(const Handle& root, const Location& key);
void write_all(const Handle& handle, std::string_view bytes);
std::string read_all(const Handle& handle, size_t limit);
void seal_file(const Handle& handle);
void sync(const Handle& handle);
void check_capacity(const Handle& root, size_t max_records, uint64_t max_bytes, size_t additional_bytes);
void publish(const Handle& root, const Handle& staged, const std::string& temporary, const std::string& name);
void finish_publication(const Handle& root, const Handle& staged);
void remove_staging(const Handle& root, const Handle& staged, const std::string& name) noexcept;
Status entry_status(const Handle& root, const std::string& name);
} // namespace sp::archive_fs
