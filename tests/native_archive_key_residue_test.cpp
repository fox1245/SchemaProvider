// Forensic check of the native archive's key hygiene (issue #10).
//
// The archive's activation record starts with the raw 32-byte archive key. Whatever copies of that key are
// left in process memory after provisioning (success or failure) are a secret-hygiene defect: they survive
// in freed heap blocks, core dumps and swap. This test provisions real archives, learns the key from the
// activation file the library wrote, and then scans the process's own writable private memory for it.
//
// A copy of the first half of the key would be clobbered by a free list's bookkeeping words, so the test
// searches for the second half (bytes 16..32): a distinctive 16-byte string that survives a free().
//
// The scanner uses only system calls and one private scratch mapping, which it excludes from the scan, so
// that looking for the key neither allocates on the heap (and so cannot overwrite a stale copy) nor finds
// its own buffers. Linux only: it needs /proc/self/maps and /proc/self/mem.
#include "core/native_archive.h"
#include "descriptor/descriptor.h"
#include "json/json.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

namespace {
using namespace sp;
constexpr std::size_t kKeySize = 32;
constexpr std::size_t kSliceOffset = 16, kSliceSize = 16;

[[noreturn]] void fail(const char* what, int line) {
  std::fprintf(stderr, "key residue check failed at line %d: %s\n", line, what);
  std::exit(1);
}
#define REQUIRE(c) do { if (!(c)) fail(#c, __LINE__); } while (false)

// Everything the scanner reads into or parses lives here, so the scan can skip it.
struct Scratch {
  static constexpr std::size_t kMaps = 256 * 1024, kChunk = 1 << 16;
  unsigned char* base = nullptr;
  std::size_t size = kMaps + kChunk + 4096;
  unsigned char* maps() const { return base; }
  unsigned char* chunk() const { return base + kMaps; }
  unsigned char* slot() const { return base + kMaps + kChunk; }  // 4 KiB for needles and file reads
  Scratch() {
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(p != MAP_FAILED);
    base = static_cast<unsigned char*>(p);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  ~Scratch() { ::munmap(base, size); }
};

std::uintptr_t parse_hex(const unsigned char*& p, const unsigned char* end) {
  std::uintptr_t value = 0;
  while (p < end) {
    const unsigned char c = *p;
    unsigned digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = 10 + c - 'a';
    else break;
    value = (value << 4) | digit;
    ++p;
  }
  return value;
}

// Occurrences of `needle` in writable private anonymous memory (the heap and malloc arenas), outside the scratch.
std::size_t count_in_memory(const Scratch& s, const unsigned char* needle, std::size_t n, bool* scanned) {
  REQUIRE(n > 0 && n <= 4096);
  const int maps_fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  REQUIRE(maps_fd >= 0);
  std::size_t length = 0;
  for (;;) {
    REQUIRE(length < Scratch::kMaps);
    const auto got = ::read(maps_fd, s.maps() + length, Scratch::kMaps - length);
    if (got < 0 && errno == EINTR) continue;
    REQUIRE(got >= 0);
    if (got == 0) break;
    length += static_cast<std::size_t>(got);
  }
  ::close(maps_fd);
  REQUIRE(length < Scratch::kMaps);  // a truncated maps listing would silently shrink the scan
  const int mem_fd = ::open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
  REQUIRE(mem_fd >= 0);

  const auto scratch_begin = reinterpret_cast<std::uintptr_t>(s.base);
  const auto scratch_end = scratch_begin + s.size;
  std::size_t found = 0, regions = 0;
  auto scan = [&](std::uintptr_t begin, std::uintptr_t end) {
    if (end <= begin) return;
    ++regions;
    std::uintptr_t at = begin;
    while (at < end) {
      const std::size_t want = static_cast<std::size_t>(std::min<std::uintptr_t>(end - at, Scratch::kChunk));
      const auto got = ::pread(mem_fd, s.chunk(), want, static_cast<off_t>(at));
      if (got < 0 && errno == EINTR) continue;
      REQUIRE(got > 0);  // An unreadable selected mapping is not evidence of absent key material.
      const unsigned char* hay = s.chunk();
      std::size_t left = static_cast<std::size_t>(got);
      while (left >= n) {
        const void* hit = ::memmem(hay, left, needle, n);
        if (!hit) break;
        ++found;
        const auto skip = static_cast<const unsigned char*>(hit) - hay + 1;
        hay += skip;
        left -= static_cast<std::size_t>(skip);
      }
      // Retain n-1 bytes for a boundary-spanning match, including after a short read.
      const auto received = static_cast<std::size_t>(got);
      if (at + received == end) break;
      REQUIRE(received >= n);
      at += received - (n - 1);
    }
  };

  const unsigned char* p = s.maps();
  const unsigned char* const end = s.maps() + length;
  while (p < end) {
    const unsigned char* line_end = static_cast<const unsigned char*>(std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
    if (!line_end) line_end = end;
    const unsigned char* q = p;
    const std::uintptr_t begin = parse_hex(q, line_end);
    if (q < line_end && *q == '-') ++q;
    const std::uintptr_t stop = parse_hex(q, line_end);
    while (q < line_end && *q == ' ') ++q;
    const bool writable_private = line_end - q >= 4 && q[0] == 'r' && q[1] == 'w' && q[3] == 'p';
    // Skip offset, device and inode, then look at the pathname field, if any.
    const unsigned char* name = q;
    for (int field = 0; field < 4 && name < line_end; ++field) {
      while (name < line_end && *name != ' ') ++name;
      while (name < line_end && *name == ' ') ++name;
    }
    const std::size_t name_size = static_cast<std::size_t>(line_end - name);
    const bool anonymous = name_size == 0 ||
        (name_size >= 6 && std::memcmp(name, "[anon:", 6) == 0);
    const bool heap = name_size == 6 && std::memcmp(name, "[heap]", 6) == 0;
    if (writable_private && (anonymous || heap)) {
      if (begin < scratch_end && stop > scratch_begin) {
        scan(begin, std::min(stop, scratch_begin));
        scan(std::max(begin, scratch_end), stop);
      } else {
        scan(begin, stop);
      }
    }
    p = line_end < end ? line_end + 1 : end;
  }
  ::close(mem_fd);
  if (scanned) *scanned = regions > 0;
  return found;
}

struct Fixture {
  std::string root;
  Fixture() {
    char path[] = "/tmp/sp-key-residue-XXXXXX";
    auto* p = ::mkdtemp(path);
    REQUIRE(p != nullptr);
    root = p;
  }
  ~Fixture() { std::error_code e; std::filesystem::remove_all(root, e); }
  std::string directory() const { return root + "/records"; }
  std::string key() const { return root + "/activation"; }
};

descriptor::ValidatedDescriptor desc() {
  auto loaded = descriptor::load(
      "{\"descriptor_version\":1,\"revision\":1,\"id\":\"archive-fixture\",\"family\":\"anthropic.messages\","
      "\"connection\":{\"base_url\":\"http://127.0.0.1:18080\",\"paths\":{\"buffered\":\"/v1/messages\",\"streaming\":\"/v1/messages\"}}}");
  REQUIRE(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}

// Read the first kKeySize bytes of the activation file the library wrote, using only system calls, and put
// the distinctive slice into the scratch needle slot.
std::size_t learn_slice(const Scratch& s, const std::string& key_file) {
  const int fd = ::open(key_file.c_str(), O_RDONLY | O_CLOEXEC);
  REQUIRE(fd >= 0);
  std::size_t got = 0;
  while (got < kKeySize) {
    const auto n = ::read(fd, s.slot() + 1024 + got, kKeySize - got);
    if (n < 0 && errno == EINTR) continue;
    REQUIRE(n > 0);
    got += static_cast<std::size_t>(n);
  }
  ::close(fd);
  std::memcpy(s.slot(), s.slot() + 1024 + kSliceOffset, kSliceSize);
  std::memset(s.slot() + 1024, 0, kKeySize);
  return kSliceSize;
}

}  // namespace

int main() {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
  // Sanitizer shadow/reservation mappings do not form an ordinary inspectable malloc heap.
  std::fprintf(stderr, "KEY_RESIDUE_SKIPPED: instrumented allocator; run this forensic check in Release\n");
  return 77;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
  std::fprintf(stderr, "KEY_RESIDUE_SKIPPED: instrumented allocator; run this forensic check in Release\n");
  return 77;
#endif
#endif
  ::signal(SIGXFSZ, SIG_IGN);
  Scratch scratch;

  // Control 1: a planted live copy must be found, or the environment cannot be scanned (sanitizer heaps).
  {
    unsigned char planted[kSliceSize];
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    REQUIRE(::read(fd, scratch.slot(), kSliceSize) == static_cast<ssize_t>(kSliceSize));
    ::close(fd);
    std::memcpy(planted, scratch.slot(), kSliceSize);
    auto* heap = new unsigned char[96];
    std::memset(heap, 0xAB, 96);
    std::memcpy(heap + kSliceOffset, planted, kSliceSize);
    bool scanned = false;
    const auto live = count_in_memory(scratch, scratch.slot(), kSliceSize, &scanned);
    if (!scanned || live != 1) {
      delete[] heap;
      std::fprintf(stderr, "KEY_RESIDUE_SKIPPED: a planted heap copy was seen %zu times (scanned=%d); "
                           "this environment's heap cannot be scanned\n", live, scanned);
      return 77;
    }
    // Control 2: a freed, never-wiped copy must still be visible. If the allocator clears freed memory the
    // stale-copy checks below cannot fail, so say so instead of passing.
    delete[] heap;
    const auto stale = count_in_memory(scratch, scratch.slot(), kSliceSize, nullptr);
    if (stale != 1) {
      std::fprintf(stderr, "KEY_RESIDUE_SKIPPED: a freed unwiped copy was seen %zu times; this allocator "
                           "clears or recycles freed blocks, so stale copies cannot be detected\n", stale);
      return 77;
    }
  }

  const auto d = desc();
  int failures = 0;
  auto verdict = [&](const char* name, std::size_t value, std::size_t expected) {
    const bool ok = value == expected;
    std::fprintf(stderr, "[ %s ] %s: %zu copies of the key slice (expected %zu)\n", ok ? " OK " : "FAIL", name, value, expected);
    if (!ok) ++failures;
  };

  // A successful provisioning: while the archive is open its State legitimately holds the key twice (the key
  // itself and the activation record it was read back from). Once destroyed, nothing may remain.
  {
    Fixture f;
    const auto directory = f.directory();
    const auto key_file = f.key();
    auto result = NativeArchive::provision(directory, key_file, "residue-owner", d);
    REQUIRE(std::holds_alternative<std::shared_ptr<NativeArchive>>(result));
    learn_slice(scratch, key_file);
    bool scanned = false;
    const auto live = count_in_memory(scratch, scratch.slot(), kSliceSize, &scanned);
    REQUIRE(scanned);
    verdict("provision succeeded, archive open", live, 2);
    result = Error{};
    verdict("provision succeeded, archive destroyed", count_in_memory(scratch, scratch.slot(), kSliceSize, nullptr), 0);
  }

  // A provisioning whose activation write fails half way: the first 64 bytes (including the whole key) reach
  // the file, so the test can learn the key, and then the write is refused. Nothing may be left in memory.
  {
    Fixture f;
    const auto directory = f.directory();
    const auto key_file = f.key();
    struct rlimit saved{};
    REQUIRE(::getrlimit(RLIMIT_FSIZE, &saved) == 0);
    struct rlimit tight = saved;
    tight.rlim_cur = 64;
    REQUIRE(::setrlimit(RLIMIT_FSIZE, &tight) == 0);
    auto result = NativeArchive::provision(directory, key_file, "residue-owner", d);
    REQUIRE(::setrlimit(RLIMIT_FSIZE, &saved) == 0);
    REQUIRE(std::holds_alternative<Error>(result));
    struct stat st{};
    REQUIRE(::stat(key_file.c_str(), &st) == 0 && st.st_size == 64);
    learn_slice(scratch, key_file);
    verdict("provision failed on the activation write", count_in_memory(scratch, scratch.slot(), kSliceSize, nullptr), 0);
  }

  if (failures) {
    std::fprintf(stderr, "%d key residue check(s) failed\n", failures);
    return 1;
  }
  std::fprintf(stderr, "key residue checks passed\n");
  return 0;
}
