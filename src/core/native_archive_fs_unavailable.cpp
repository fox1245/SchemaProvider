// The custody backend of a target without file custody (WebAssembly): there is no atomic no-replace rename and no
// file ownership to check. available() is false, so NativeArchive::provision and open answer with a typed refusal
// and never reach these operations; each one refuses as well, so nothing here can pretend to persist.
#include "core/native_archive_fs.h"

namespace sp::archive_fs {
bool available() noexcept { return false; }

Handle::Handle() noexcept : value_(-1) {}
Handle::Handle(NativeHandle value) noexcept : value_(value) {}
Handle::~Handle() = default;
Handle::Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = -1; }
Handle& Handle::operator=(Handle&& other) noexcept { value_ = other.value_; other.value_ = -1; return *this; }
Handle::operator bool() const noexcept { return value_ >= 0; }

StoreLock::StoreLock(const Handle&, const Handle&) : value_(-1) { throw Rejected{}; }
StoreLock::~StoreLock() = default;

Location locate(std::string_view) { throw Rejected{}; }
Handle open_directory(const Handle&, const std::string&) { throw Rejected{}; }
Handle create_directory(const Handle&, const std::string&) { throw Rejected{}; }
Handle open_file(const Handle&, const std::string&, bool) { throw Rejected{}; }
Handle create_file(const Handle&, const std::string&) { throw Rejected{}; }
Status status(const Handle&) { throw Rejected{}; }
bool same_file(const Status&, const Status&) noexcept { return false; }
bool unchanged(const Status&, const Status&) noexcept { return false; }
void private_directory(const Handle&) { throw Rejected{}; }
void private_file(const Handle&, bool) { throw Rejected{}; }
void independent_key_parent(const Handle&, const Location&) { throw Rejected{}; }
void write_all(const Handle&, std::string_view) { throw Rejected{}; }
std::string read_all(const Handle&, size_t) { throw Rejected{}; }
void seal_file(const Handle&) { throw Rejected{}; }
void sync(const Handle&) { throw Rejected{}; }
void check_capacity(const Handle&, size_t, uint64_t, size_t) { throw Rejected{}; }
void publish(const Handle&, const Handle&, const std::string&, const std::string&) { throw Rejected{}; }
void finish_publication(const Handle&, const Handle&) { throw Rejected{}; }
void remove_staging(const Handle&, const Handle&, const std::string&) noexcept {}
Status entry_status(const Handle&, const std::string&) { throw Rejected{}; }
}  // namespace sp::archive_fs
