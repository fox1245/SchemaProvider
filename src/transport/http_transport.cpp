// libcurl multi_socket interface driven by a private standalone-Asio event loop.
//
// Threading model: every libcurl multi call (add/remove handle, socket_action, info_read, pause)
// and all per-operation state of an ACTIVE operation run on ONE Asio strand over a bounded pool of
// I/O threads. Public entry points never touch the CURLM handle inline: cancel, resume and the
// teardown post to that strand, so a user callback that calls back into the API cannot re-enter
// the multi handle. The one inline libcurl use is building the private easy handle in
// Transport::start() (and the multi handle in the constructor before any I/O thread exists); an
// easy handle that has not been added yet is owned by the caller alone, so that is not shared state.
//
// POSIX sockets are borrowed stream descriptors. Windows watches Winsock network-event
// HANDLEs instead: no Asio socket takes ownership of, or attaches IOCP to, a libcurl socket.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "transport/http_transport.h"
#include "transport/io_thread.h"
#include "transport/ws_transport.h"
#include "crypto/crypto.h"

#include <asio/bind_executor.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#ifdef _WIN32
#include <asio/windows/object_handle.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

#include <deque>

namespace sp::transport {

namespace {


#ifdef _WIN32
// Resolver threads may outlive the transport. Keep their Winsock reference alive until the
// last lookup returns, and keep the core's reference until its sockets and io_context are gone.
struct WinsockRuntime {
  WinsockRuntime() {
    WSADATA data{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) throw std::runtime_error("WSAStartup failed");
    if (data.wVersion != MAKEWORD(2, 2)) {
      WSACleanup();
      throw std::runtime_error("Winsock 2.2 is required");
    }
  }
  ~WinsockRuntime() { WSACleanup(); }
  WinsockRuntime(const WinsockRuntime&) = delete;
  WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};
#endif

void ensure_curl_global_init() {
  static std::once_flag once;
  std::call_once(once, [] {
#ifdef _WIN32
    // Winsock belongs to WinsockRuntime, not to libcurl's process-lifetime global state.
    const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT & ~CURL_GLOBAL_WIN32);
#else
    const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
    if (rc != CURLE_OK) throw std::runtime_error("curl_global_init failed");
  });
}

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
           return std::tolower(x) == std::tolower(y);
         });
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.remove_suffix(1);
  return s;
}


// Effective stall bound for one attempt: the request's override, else the transport default. A bound
// that is disabled, or would expire no earlier than the deadline, is reported as zero because it
// can never fire first (the idle bound's expiry only ever moves later). Round positive slack upward
// to milliseconds, preserving fractional slack without overflowing the clock's tick type.
std::chrono::milliseconds effective_bound(std::optional<std::chrono::milliseconds> request_value,
                                          std::chrono::milliseconds fallback,
                                          std::chrono::steady_clock::time_point from,
                                          std::chrono::steady_clock::time_point deadline) {
  const std::chrono::milliseconds bound = request_value.value_or(fallback);
  if (bound.count() <= 0 || deadline <= from) return std::chrono::milliseconds{0};
  const auto room = std::chrono::ceil<std::chrono::milliseconds>(deadline - from);
  return bound >= room ? std::chrono::milliseconds{0} : bound;
}
}  // namespace

struct TransportCore;

namespace {

// Splits a URL into host and port through libcurl's URL API (the same parser that will connect).
bool parse_host_port(const std::string& url, std::string& host, std::string& port, bool& https) {
  CURLU* u = curl_url();
  if (!u) return false;
  bool ok = false;
  char* h = nullptr;
  char* p = nullptr;
  char* scheme = nullptr;
  if (curl_url_set(u, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK &&
      curl_url_get(u, CURLUPART_HOST, &h, 0) == CURLUE_OK &&
      curl_url_get(u, CURLUPART_PORT, &p, CURLU_DEFAULT_PORT) == CURLUE_OK &&
      curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK) {
    host = h;
    port = p;
    https = iequals(scheme, "https");
    ok = true;
  }
  curl_free(h);
  curl_free(p);
  curl_free(scheme);
  curl_url_cleanup(u);
  return ok;
}

bool is_ip_literal(const std::string& host) {
  if (!host.empty() && host.front() == '[') return true;  // IPv6 literal
  in_addr v4{};
  return inet_pton(AF_INET, host.c_str(), &v4) == 1;
}

std::vector<std::string> default_resolve(const std::string& host) {
  std::vector<std::string> out;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return out;
  for (addrinfo* p = res; p; p = p->ai_next) {
    char buf[INET6_ADDRSTRLEN] = {};
    std::string text;
    if (p->ai_family == AF_INET) {
      inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr, buf, sizeof buf);
      text = buf;
    } else if (p->ai_family == AF_INET6) {
      inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr, buf, sizeof buf);
      text = std::string("[") + buf + "]";
    } else {
      continue;
    }
    if (std::find(out.begin(), out.end(), text) == out.end()) out.push_back(std::move(text));
  }
  freeaddrinfo(res);
  return out;
}

// A fixed set of lazily started threads running blocking lookups. Threads are detached and share
// ownership of this state, so a lookup stuck in getaddrinfo can never block Transport destruction
// or touch a destroyed transport (results are posted through a weak reference).
struct ResolverPool : std::enable_shared_from_this<ResolverPool> {
  enum class SubmitStatus { Queued, Stopped, ResourceFailure };

  ResolverPool(unsigned thread_count, ResolveFn fn) : n(thread_count), resolve(std::move(fn)) {}

  SubmitStatus submit(std::function<void()> job) {
    std::unique_lock lock(mu);
    if (stopping) return startup_failed ? SubmitStatus::ResourceFailure : SubmitStatus::Stopped;
    if (!started) {
      // Do not detach a partial pool. All workers wait for this lock until the complete
      // configured pool exists, so startup failure can join them without running a lookup.
      std::vector<std::thread> workers;
      try {
        workers.reserve(n);
        for (unsigned i = 0; i < n; ++i)
          workers.emplace_back([self = shared_from_this()] { self->run(); });
        for (auto& worker : workers) worker.detach();
        started = true;
      } catch (...) {
        startup_failed = true;
        stopping = true;
        jobs.clear();
        cv.notify_all();
        lock.unlock();
        for (auto& worker : workers)
          if (worker.joinable()) worker.join();
        return SubmitStatus::ResourceFailure;
      }
    }
    try {
      jobs.push_back(std::move(job));
    } catch (...) {
      return SubmitStatus::ResourceFailure;
    }
    cv.notify_one();
    return SubmitStatus::Queued;
  }

  void stop() {
    std::lock_guard lock(mu);
    stopping = true;
    jobs.clear();
    cv.notify_all();
  }

  void run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock(mu);
        cv.wait(lock, [this] { return stopping || !jobs.empty(); });
        if (stopping) return;
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      job();
    }
  }

#ifdef _WIN32
  std::shared_ptr<WinsockRuntime> winsock_runtime;
#endif
  const unsigned n;
  ResolveFn resolve;
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  bool started = false;
  bool stopping = false;
  bool startup_failed = false;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Operation state
// ---------------------------------------------------------------------------------------------
// A WebSocket session on top of its connect-only operation (see ws_transport.h). Strand-confined
// once the operation is active, except `accepting`, which public sends read from any thread.
struct WsState {
  WsState(WsRequest request, WsCallbacks callbacks)
      : req(std::move(request)), cb(std::move(callbacks)), decoder(req.limits) {}
  enum class Phase : std::uint8_t { Connecting, Handshake, Open, Closing };
  // How the session ends once the server closes TCP or close_timer expires.
  struct Ending {
    Status status;
    FailureKind failure;
    std::string detail;
  };

  WsRequest req;
  WsCallbacks cb;
  WsDecoder decoder;
  Phase phase = Phase::Connecting;
  bool secure = false;  // wss
  std::atomic<bool> accepting{true};  // cleared by close() and at completion
  curl_socket_t fd = CURL_SOCKET_BAD;
  std::string key;
  std::string head;  // handshake response bytes until the blank line
  // Messages sent before the handshake completed: RFC 6455 4.1 forbids writing them earlier.
  std::vector<std::pair<WsOpcode, std::string>> pending;
  std::string out;  // encoded bytes not yet written
  std::size_t out_offset = 0;
  std::random_device entropy;  // masking keys and the handshake nonce (RFC 6455 4.1, 5.3)
  bool sent_close = false;
  bool received_close = false;
  bool closed_by_peer = false;
  bool callback_threw = false;
  std::uint16_t close_code = ws_close::Abnormal;
  std::string close_reason;
  int http_status = 0;
  std::optional<Ending> ending;
  std::optional<asio::steady_timer> close_timer;

  std::array<unsigned char, 4> mask() {
    const std::uint32_t r = entropy();
    return {static_cast<unsigned char>(r), static_cast<unsigned char>(r >> 8), static_cast<unsigned char>(r >> 16),
            static_cast<unsigned char>(r >> 24)};
  }
  void queue(WsOpcode opcode, std::string_view payload) {
    if (out_offset > 65536 && out_offset * 2 > out.size()) {
      out.erase(0, out_offset);
      out_offset = 0;
    }
    out += ws_encode_frame(opcode, payload, mask());
  }
};

struct OperationState {
  OperationState(std::weak_ptr<TransportCore> core, HttpRequest request, Callbacks callbacks,
                 const TransportOptions& options)
      : owner(std::move(core)),
        req(std::move(request)),
        cb(std::move(callbacks)),
        max_head_bytes(options.max_head_bytes),
        started(std::chrono::steady_clock::now()),
        connect_bound(effective_bound(req.connect_timeout, options.connect_timeout, started, req.deadline)),
        first_byte_bound(effective_bound(req.first_byte_timeout, options.first_byte_timeout, started, req.deadline)),
        idle_bound(effective_bound(req.idle_timeout, options.idle_timeout, started, req.deadline)),
        last_activity(started) {}

  ~OperationState() {
    if (easy) curl_easy_cleanup(easy);  // before freeing lists libcurl may still reference
    if (hdrs) curl_slist_free_all(hdrs);
    if (resolve_list) curl_slist_free_all(resolve_list);
    if (upload_file) std::fclose(upload_file);
  }

  // immutable after construction
  std::weak_ptr<TransportCore> owner;
  HttpRequest req;
  Callbacks cb;
  const std::size_t max_head_bytes;
  const std::chrono::steady_clock::time_point started;
  // Set by Transport::start; used only on the strand while the operation is active.
  TransportCore* core = nullptr;

  // libcurl objects: built off-strand by setup(), strand-confined afterwards
  CURL* easy = nullptr;
  curl_slist* hdrs = nullptr;
  std::string setup_error;
  FailureKind setup_failure = FailureKind::Other;
  // A file request body (HttpRequest::file_body): the open file, the body bytes handed to libcurl, the slice's
  // running digest, and its hex once read whole.
  std::FILE* upload_file = nullptr;
  std::uint64_t upload_sent = 0;
  std::optional<crypto::Sha256> upload_hash;
  std::string upload_digest;
  bool upload_failed = false;
  CURLcode setup_code = CURLE_OK;
  std::optional<asio::steady_timer> deadline_timer;
  // Optional stall bounds (zero = disabled, or unable to expire before the deadline). Timers and
  // flags are strand-confined once the operation is active; the *_armed flags (not the timer
  // objects) tell a handler that already ran its wait whether the bound was satisfied meanwhile.
  const std::chrono::milliseconds connect_bound;
  const std::chrono::milliseconds first_byte_bound;
  const std::chrono::milliseconds idle_bound;
  std::optional<asio::steady_timer> connect_timer, first_byte_timer, idle_timer;
  bool connect_armed = false;
  bool first_byte_armed = false;
  std::chrono::steady_clock::time_point last_activity;  // read only when idle_bound > 0
  curl_off_t seen_dl = 0, seen_ul = 0;                  // progress counters at the last activity
  bool in_multi = false;
  bool paused = false;
  bool finished = false;
  // A monotonic cross-thread no-op fence, not outcome/join publication. Publish
  // at finish admission so callback-driven handle teardown cannot queue another
  // cancel after the strand already chose the sole outcome.
  std::atomic<bool> finishing{false};

  // response parsing (strand-confined; invoked from inside libcurl calls)
  ResponseHead head;
  bool head_started = false;   // inside a header block (reset after a 1xx block)
  bool response_seen = false;  // monotonic: some response status line arrived
  bool head_delivered = false;
  bool head_overflow = false;
  std::size_t head_bytes = 0;
  std::uint32_t prereq_count = 0;
  bool resend_refused = false;
  bool first_write_reused = false;
  bool callback_threw = false;
  // Socket bookkeeping for pause handling (see TransportCore::read_suppressed).
  curl_socket_t sock = CURL_SOCKET_BAD;
  bool counted_pause = false;

  // Set for a WebSocket session: a connect-only attempt whose connection the session then drives.
  std::unique_ptr<WsState> ws;

  // Name resolution (see TransportOptions::resolver_threads).
  std::string host;
  std::string port;
  bool needs_resolve = false;
  curl_slist* resolve_list = nullptr;

  // outcome, shared with join()
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  Result result;

  void setup();

  // Stall-bound bookkeeping, called from libcurl callbacks on the strand. None of them completes
  // the operation: that is left to the timers' handlers.
  void note_activity() noexcept {
    if (idle_bound.count() > 0) last_activity = std::chrono::steady_clock::now();
  }
  void note_connected() {  // the connection (TCP and TLS) is established, or was reused
    connect_armed = false;
    if (connect_timer) {
      connect_timer->cancel();
      connect_timer.reset();
    }
  }
  void note_response_started() {  // the first response header line arrived
    first_byte_armed = false;
    if (first_byte_timer) {
      first_byte_timer->cancel();
      first_byte_timer.reset();
    }
  }

  bool open_upload();
  bool rewind_upload();
  std::size_t abort_upload() noexcept { upload_failed = true; return CURL_READFUNC_ABORT; }
  bool finish_upload_digest();
  static std::size_t read_cb(char* buf, std::size_t size, std::size_t n, void* ud);
  static int seek_cb(void* ud, curl_off_t offset, int origin);
  static std::size_t header_cb(char* buf, std::size_t size, std::size_t n, void* ud);
  static std::size_t write_cb(char* ptr, std::size_t size, std::size_t n, void* ud);
  static int prereq_cb(void* ud, char* primary_ip, char* local_ip, int primary_port, int local_port);
  static int xferinfo_cb(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
  static int debug_cb(CURL*, curl_infotype type, char*, std::size_t size, void* ud);
};

// ---------------------------------------------------------------------------------------------
// Core: owns the io_context, the strand, the CURLM and the I/O threads
// ---------------------------------------------------------------------------------------------
#ifdef _WIN32
constexpr std::chrono::milliseconds kWritePollInitial{2};
constexpr std::chrono::milliseconds kWritePollMax{16};
#endif

struct TransportCore : std::enable_shared_from_this<TransportCore> {
  using Strand = asio::strand<asio::io_context::executor_type>;

  struct SockWatch {
#ifdef _WIN32
    SockWatch(asio::io_context& ctx, curl_socket_t s) : event(ctx), write_poll(ctx), fd(s) {
      // A private manual-reset event is interoperable with WSAEventSelect. Asio owns only
      // this HANDLE, so cancellation/destruction cannot close libcurl's SOCKET.
      HANDLE native = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!native)
        throw asio::system_error(asio::error_code(GetLastError(), asio::error::get_system_category()),
                                 "CreateEventW");
      asio::error_code ec;
      event.assign(native, ec);
      if (ec) {
        CloseHandle(native);
        throw asio::system_error(ec, "event handle assignment");
      }
    }
    ~SockWatch() { release_quiet(); }
    void release_quiet() noexcept {
      if (selected_events != 0) {
        // REMOVE may arrive after libcurl closed the socket. Ignore that error, and never
        // repeat this socket operation from a delayed handler or destructor after reuse.
        WSAEventSelect(fd, nullptr, 0);
        selected_events = 0;
      }
      try {
        write_poll.cancel();
      } catch (...) {
      }
      asio::error_code ignored;
      event.close(ignored);  // unregisters the native wait and closes only the event HANDLE
    }
    asio::windows::object_handle event;
    // Bounded fallback while a write wait is pending; see TransportCore::poll_write_readiness.
    asio::steady_timer write_poll;
    std::chrono::milliseconds write_poll_delay{0};
    long selected_events = 0;
    std::uint64_t generation = 0;
    bool armed = false;
    bool failed = false;
#else
    SockWatch(asio::io_context& ctx, curl_socket_t s) : desc(ctx, s), fd(s) {}
    ~SockWatch() { release_quiet(); }
    void release_quiet() noexcept {
      try {
        if (desc.is_open()) (void)desc.release();  // never close: libcurl owns the descriptor
      } catch (...) {
      }
    }
    asio::posix::stream_descriptor desc;
    bool armed_read = false;
    bool armed_write = false;
#endif
    curl_socket_t fd;
    bool want_read = false;
    bool want_write = false;
    bool removed = false;
    // The WebSocket session that drives this socket after its connect-only transfer; libcurl's
    // own interest in it is then ignored.
    OperationState* ws_owner = nullptr;
  };

  explicit TransportCore(TransportOptions options)
      : opts(std::move(options)),
        strand(asio::make_strand(ctx)),
        guard(asio::make_work_guard(ctx)),
        timer(ctx) {}

#ifdef _WIN32
  // Declared before ctx so cleanup occurs after every Asio service and borrowed socket watch.
  std::shared_ptr<WinsockRuntime> winsock_runtime = std::make_shared<WinsockRuntime>();
#endif
  TransportOptions opts;
  asio::io_context ctx;
  Strand strand;
  asio::executor_work_guard<asio::io_context::executor_type> guard;
  std::vector<std::thread> threads;

  // strand-confined
  CURLM* multi = nullptr;
  bool ipv6_resolve_hosts = false;  // CURLOPT_RESOLVE accepts an IPv6 HOST starting in curl 8.13.
  asio::steady_timer timer;
  enum class TimerMode { Inactive, Immediate, Delayed };
  TimerMode timer_mode = TimerMode::Inactive;
  std::uint64_t timer_generation = 0;
  bool timeout_posted = false;
  bool defer_start_kick = false;
  bool stopped = false;
  std::unordered_map<curl_socket_t, std::shared_ptr<SockWatch>> socks;
  std::unordered_map<OperationState*, std::shared_ptr<OperationState>> active;
  // libcurl 8.5 keeps asking for read readiness on a connected socket even when its transfer is
  // paused, and it reads and buffers (up to DYN_PAUSE_BUFFER) whenever the application reports
  // readability on it. TCP backpressure therefore needs the application to stop reporting
  // readability for the socket of a paused transfer. That is only safe when the socket carries
  // exactly one transfer, i.e. HTTP/1.x. HTTP/2 and HTTP/3 paused streams share their
  // connection with live siblings, so their read readiness must remain enabled.
  // libcurl's stream flow-control windows bound retained data (curl_easy_pause).
  std::unordered_set<curl_socket_t> paused_h1_socks;
  // Name resolution: single-flight per host, TTL cache, bounded resolver threads.
  struct DnsEntry {
    std::vector<std::string> addrs;
    std::chrono::steady_clock::time_point expires;
  };
  std::shared_ptr<ResolverPool> resolver;
  std::unordered_map<std::string, DnsEntry> dns_cache;
  struct PendingLookup {
    bool in_flight = false;  // a resolver job for this host is running
    std::vector<std::shared_ptr<OperationState>> waiters;
  };
  std::unordered_map<std::string, PendingLookup> dns_pending;
  // The operation whose socket callback last named a descriptor; cleared on REMOVE and in finish().
  std::unordered_map<curl_socket_t, OperationState*> sock_owner;

  void init();
  void add_to_multi(const std::shared_ptr<OperationState>& op);
  void lookup(const std::shared_ptr<OperationState>& op);
  void on_resolved(const std::string& host, std::vector<std::string> addrs);
  void apply_addrs_and_add(const std::shared_ptr<OperationState>& op, std::span<const std::string> addrs);
  void begin(const std::shared_ptr<OperationState>& op);
  void finish(std::shared_ptr<OperationState> op, Status status, FailureKind failure, CURLcode code,
              const char* detail);
  void resume(const std::shared_ptr<OperationState>& op);
  void complete_from_curl(const std::shared_ptr<OperationState>& op, CURLcode code);
  void check_multi_info();
  void on_timeout();
  void arm_stall_timers(const std::shared_ptr<OperationState>& op);
  void on_idle_expired(const std::shared_ptr<OperationState>& op);
  void set_timer(long ms);
  void on_socket(CURL* easy, curl_socket_t s, int what);
  void arm(const std::shared_ptr<SockWatch>& w);
  void on_socket_event(const std::shared_ptr<SockWatch>& w, int flags);
#ifdef _WIN32
  bool watch_failure_pending = false;
  void fail_watches_later();
  void poll_write_readiness(const std::shared_ptr<SockWatch>& w, std::uint64_t generation);
#endif
  void note_pause(OperationState& op);
  void note_resume(OperationState& op);
  bool read_suppressed(curl_socket_t s) const;
  void rearm_socket(curl_socket_t s);
  void shutdown();

  // WebSocket sessions (ws_transport.h), all on the strand.
  void ws_connected(const std::shared_ptr<OperationState>& op);
  void ws_ready(const std::shared_ptr<SockWatch>& w, int flags);
  void ws_interest(const std::shared_ptr<OperationState>& op);
  void ws_flush(const std::shared_ptr<OperationState>& op);
  void ws_read(const std::shared_ptr<OperationState>& op);
  bool ws_consume(const std::shared_ptr<OperationState>& op, std::string_view bytes);
  bool ws_handshake(const std::shared_ptr<OperationState>& op, std::string_view& bytes);
  void ws_send(const std::shared_ptr<OperationState>& op, WsOpcode opcode, std::string payload);
  void ws_close(const std::shared_ptr<OperationState>& op, std::uint16_t code, std::string reason);
  void ws_send_close(const std::shared_ptr<OperationState>& op, std::uint16_t code, std::string_view reason);
  void ws_end(const std::shared_ptr<OperationState>& op);
  void ws_release(OperationState& op);

  static int socket_cb(CURL* easy, curl_socket_t s, int what, void* userp, void*) {
#ifdef _WIN32
    auto* self = static_cast<TransportCore*>(userp);
    try {
      self->on_socket(easy, s, what);
    } catch (...) {
      // Native event allocation/registration failures must not escape a C callback or
      // leave an unmonitored transfer waiting for peer progress.
      self->fail_watches_later();
    }
#else
    static_cast<TransportCore*>(userp)->on_socket(easy, s, what);
#endif
    return 0;
  }
  static int timer_cb(CURLM*, long ms, void* userp) {
    static_cast<TransportCore*>(userp)->set_timer(ms);
    return 0;
  }
};

// ---------------------------------------------------------------------------------------------
// OperationState: libcurl setup and callbacks
// ---------------------------------------------------------------------------------------------
void OperationState::setup() {
  easy = curl_easy_init();
  if (!easy) {
    setup_error = "curl_easy_init failed";
    return;
  }
  auto set = [&](CURLoption option, auto value) {
    if (!setup_error.empty()) return;
    const CURLcode rc = curl_easy_setopt(easy, option, value);
    if (rc != CURLE_OK) setup_error = std::string("curl_easy_setopt failed: ") + curl_easy_strerror(rc);
  };

  set(CURLOPT_URL, req.url.c_str());
  bool https = false;
  if (setup_error.empty()) {
    if (!parse_host_port(req.url, host, port, https)) setup_error = "invalid URL";
    else needs_resolve = !is_ip_literal(host);
  }
  set(CURLOPT_PROTOCOLS_STR, "http,https");
  // Never negotiate anything older than TLS 1.2, whatever the libcurl build or TLS backend
  // (OpenSSL, Schannel, Secure Transport) would allow by default.
  set(CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
  set(CURLOPT_FOLLOWLOCATION, 0L);
  set(CURLOPT_NOSIGNAL, 1L);
  set(CURLOPT_NOPROGRESS, idle_bound.count() > 0 ? 0L : 1L);
  if (idle_bound.count() > 0) {
    // Upload progress tracks body bytes handed to the socket. Outbound headers do not change that
    // counter, so consume HEADER_OUT notifications too, without retaining or logging their bytes.
    set(CURLOPT_XFERINFOFUNCTION, &OperationState::xferinfo_cb);
    set(CURLOPT_XFERINFODATA, static_cast<void*>(this));
    set(CURLOPT_VERBOSE, 1L);
    set(CURLOPT_DEBUGFUNCTION, &OperationState::debug_cb);
    set(CURLOPT_DEBUGDATA, static_cast<void*>(this));
  }
  set(CURLOPT_PROXY, "");  // proxies are not supported: an empty value also disables environment proxies
  set(CURLOPT_PRIVATE, static_cast<void*>(this));
  set(CURLOPT_HEADERFUNCTION, &OperationState::header_cb);
  set(CURLOPT_HEADERDATA, static_cast<void*>(this));
  set(CURLOPT_WRITEFUNCTION, &OperationState::write_cb);
  set(CURLOPT_WRITEDATA, static_cast<void*>(this));
  set(CURLOPT_PREREQFUNCTION, &OperationState::prereq_cb);
  set(CURLOPT_PREREQDATA, static_cast<void*>(this));

  if (ws) {
    // libcurl only connects (TCP and TLS with ALPN http/1.1); the session writes the request. A
    // fresh connection, never a pooled one: it leaves HTTP once the server switches protocols.
    // libcurl never pools a connect-only connection; FORBID_REUSE would close it at once.
    set(CURLOPT_CONNECT_ONLY, 1L);
    set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    set(CURLOPT_FRESH_CONNECT, 1L);
    if (!req.ca_file.empty()) set(CURLOPT_CAINFO, req.ca_file.c_str());
    return;
  }

  const auto features = curl_version_info(CURLVERSION_NOW)->features;
  const long normal_http = (features & CURL_VERSION_HTTP2) ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_1_1;
  switch (req.http_version) {
    case HttpVersion::Auto:
      set(CURLOPT_HTTP_VERSION, normal_http);
      break;
    case HttpVersion::Http1_1: set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1)); break;
    case HttpVersion::Http2PriorKnowledge:
      set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE));
      break;
    case HttpVersion::Http3Preferred:
      set(CURLOPT_HTTP_VERSION, https && (features & CURL_VERSION_HTTP3)
                                   ? static_cast<long>(CURL_HTTP_VERSION_3) : normal_http);
      break;
    case HttpVersion::Http3Only:
      if (!https || !(features & CURL_VERSION_HTTP3)) {
        setup_error = !https ? "HTTP/3-only requires HTTPS" : "linked libcurl lacks HTTP/3";
        setup_failure = FailureKind::Protocol;
        setup_code = CURLE_UNSUPPORTED_PROTOCOL;
        return;
      }
      set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_3ONLY));
      // A shared multi cache may contain H2/H1 for this origin. Never let its
      // existing connection satisfy the verification lane, nor retain this lane's
      // connection for another policy. Ordinary preferred H3 remains multiplexable.
      set(CURLOPT_FRESH_CONNECT, 1L);
      set(CURLOPT_FORBID_REUSE, 1L);
      break;
  }

  const bool is_get = iequals(req.method, "GET");
  const bool is_head = iequals(req.method, "HEAD");
  const bool is_post = iequals(req.method, "POST");
  const bool sends_body_fields = !is_get && !is_head;
  if (!sends_body_fields && (!req.body.empty() || req.file_body)) {
    setup_error = "GET and HEAD requests cannot carry a body";
    return;
  }
  if (req.file_body && !req.body.empty()) {
    setup_error = "a file body replaces the in-memory body";
    return;
  }
  if (is_head) {
    set(CURLOPT_NOBODY, 1L);
  } else if (sends_body_fields) {
    // POSTFIELDS installs the owned body and its length for every body-carrying method; for
    // methods other than POST, CUSTOMREQUEST then only replaces the method token. A file body is
    // read by the read callback instead, with its length declared up front.
    set(CURLOPT_POST, 1L);
    if (req.file_body) {
      if (!open_upload()) return;
      const auto& file = *req.file_body;
      set(CURLOPT_READFUNCTION, &OperationState::read_cb);
      set(CURLOPT_READDATA, this);
      set(CURLOPT_SEEKFUNCTION, &OperationState::seek_cb);
      set(CURLOPT_SEEKDATA, this);
      set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(file.prefix.size() + file.size + file.suffix.size()));
    } else {
      set(CURLOPT_POSTFIELDS, req.body.data());
      set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
    }
    if (!is_post) set(CURLOPT_CUSTOMREQUEST, req.method.c_str());
  }

  if (!req.ca_file.empty()) set(CURLOPT_CAINFO, req.ca_file.c_str());

  // Caller headers verbatim; suppress libcurl's own defaults unless the caller set them, so the
  // wire request is exactly what the layer above asked for.
  auto has_header = [&](std::string_view name) {
    return std::any_of(req.headers.begin(), req.headers.end(),
                       [&](const Header& h) { return iequals(h.name, name); });
  };
  for (const Header& h : req.headers) {
    // A colon with no value suppresses a header in libcurl; a semicolon sends it empty.
    const std::string line = h.value.empty() ? h.name + ";" : h.name + ": " + h.value;
    curl_slist* next = curl_slist_append(hdrs, line.c_str());
    if (!next) {
      setup_error = "curl_slist_append failed";
      return;
    }
    hdrs = next;
  }
  for (const char* suppressed : {"Accept", "User-Agent", "Expect", "Content-Type"}) {
    if (has_header(suppressed)) continue;
    if (iequals(suppressed, "Content-Type") && !sends_body_fields) continue;
    const std::string line = std::string(suppressed) + ":";
    curl_slist* next = curl_slist_append(hdrs, line.c_str());
    if (!next) {
      setup_error = "curl_slist_append failed";
      return;
    }
    hdrs = next;
  }
  set(CURLOPT_HTTPHEADER, hdrs);
}

namespace {

// Message framing from the headers as libcurl actually applies them (RFC 9112 6.3): a transfer
// coding overrides Content-Length, only a final "chunked" coding gives chunk framing, and an unusable
// Content-Length gives no framing. Anything ambiguous is treated as close-delimited, which is never
// a normal end, so EOF cannot turn it into a success.
BodyFraming derive_framing(const ResponseHead& head, bool head_request) {
  if (head.version == ResponseVersion::Http2 || head.version == ResponseVersion::Http3)
    return BodyFraming::Stream;
  if (head_request || head.status == 204 || head.status == 304) return BodyFraming::None;

  bool te_present = false;
  std::string te;  // every Transfer-Encoding value in order, comma separated
  bool cl_seen = false;
  bool cl_valid = true;
  std::string cl_first;
  for (const Header& h : head.headers) {
    if (h.name == "transfer-encoding") {
      te_present = true;
      if (!te.empty()) te += ',';
      te += lower(h.value);
    } else if (h.name == "content-length") {
      cl_seen = true;
      std::size_t pos = 0;
      while (pos <= h.value.size()) {  // a comma list of identical decimal values is allowed
        const std::size_t comma = h.value.find(',', pos);
        const std::string_view item =
            trim(std::string_view(h.value).substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        const bool digits = !item.empty() && item.size() <= 18 &&
                            std::all_of(item.begin(), item.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (!digits) cl_valid = false;
        else if (cl_first.empty()) cl_first = std::string(item);
        else if (cl_first != item) cl_valid = false;
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    }
  }
  if (te_present) {
    const std::size_t comma = te.rfind(',');
    const std::string_view last = trim(std::string_view(te).substr(comma == std::string::npos ? 0 : comma + 1));
    return last == "chunked" ? BodyFraming::Chunked : BodyFraming::CloseDelimited;
  }
  return (cl_seen && cl_valid) ? BodyFraming::ContentLength : BodyFraming::CloseDelimited;
}

}  // namespace

bool OperationState::open_upload() {
  const auto& file = *req.file_body;
  std::error_code error;
  const auto length = std::filesystem::file_size(file.path, error);
  constexpr std::uint64_t limit = std::uint64_t{1} << 62;
  if (!error && file.offset <= length && file.size <= length - file.offset && file.size < limit &&
      file.prefix.size() + file.suffix.size() < limit) upload_file = std::fopen(file.path.c_str(), "rb");
  if (!upload_file || !rewind_upload()) {
    setup_error = "upload source unreadable";
    setup_failure = FailureKind::UploadSource;
    return false;
  }
  return true;
}
bool OperationState::rewind_upload() {
  upload_sent = 0;
  upload_digest.clear();
  upload_hash.emplace();
  const auto offset = req.file_body->offset;
#ifdef _WIN32
  return _fseeki64(upload_file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
  return fseeko(upload_file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
// The slice was read whole: its digest must match before its last bytes leave.
bool OperationState::finish_upload_digest() {
  static constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  for (const auto byte : upload_hash->finish()) { hex += digits[byte >> 4]; hex += digits[byte & 15]; }
  if (!req.file_body->expected_sha256.empty() && hex != req.file_body->expected_sha256) return false;
  upload_digest = std::move(hex);
  return true;
}
std::size_t OperationState::read_cb(char* buf, std::size_t size, std::size_t n, void* ud) {
  auto* op = static_cast<OperationState*>(ud);
  const auto& file = *op->req.file_body;
  const std::size_t capacity = size * n;
  const std::uint64_t slice_end = file.prefix.size() + file.size;
  std::size_t out = 0;
  while (out < capacity) {
    const auto at = op->upload_sent;
    std::size_t take = 0;
    if (at < file.prefix.size()) {
      take = static_cast<std::size_t>(std::min<std::uint64_t>(capacity - out, file.prefix.size() - at));
      std::memcpy(buf + out, file.prefix.data() + at, take);
    } else if (at < slice_end) {
      const auto remaining = slice_end - at;
      take = static_cast<std::size_t>(std::min<std::uint64_t>(capacity - out, remaining));
      if (std::fread(buf + out, 1, take, op->upload_file) != take) return op->abort_upload();
      op->upload_hash->update(buf + out, take);
      if (take == remaining && !op->finish_upload_digest()) return op->abort_upload();
    } else {
      // An empty slice has no last bytes to hold back; its digest is checked before the suffix.
      if (op->upload_digest.empty() && !op->finish_upload_digest()) return op->abort_upload();
      const auto suffix_at = at - slice_end;
      if (suffix_at >= file.suffix.size()) break;
      take = static_cast<std::size_t>(std::min<std::uint64_t>(capacity - out, file.suffix.size() - suffix_at));
      std::memcpy(buf + out, file.suffix.data() + suffix_at, take);
    }
    out += take;
    op->upload_sent += take;
  }
  return out;
}
// libcurl rewinds a body it must send again; only a restart from the first byte is possible.
int OperationState::seek_cb(void* ud, curl_off_t offset, int origin) {
  auto* op = static_cast<OperationState*>(ud);
  if (origin != SEEK_SET || offset != 0) return CURL_SEEKFUNC_CANTSEEK;
  return op->rewind_upload() ? CURL_SEEKFUNC_OK : CURL_SEEKFUNC_FAIL;
}

std::size_t OperationState::header_cb(char* buf, std::size_t size, std::size_t n, void* ud) {
  auto* op = static_cast<OperationState*>(ud);
  const std::size_t len = size * n;
  op->note_activity();
  op->note_response_started();  // any response header line means the peer answered
  if (op->head_delivered) return len;  // trailers: not interpreted
  op->head_bytes += len;
  if (op->head_bytes > op->max_head_bytes) {
    op->head_overflow = true;
    return 0;
  }
  const bool continuation = len > 0 && (buf[0] == ' ' || buf[0] == '\t');  // obs-fold (RFC 9112 5.2)
  std::string_view line = trim(std::string_view(buf, len));

  if (!continuation && line.substr(0, 5) == "HTTP/") {
    op->head = ResponseHead{};
    op->head_started = true;
    op->response_seen = true;
    const auto space = line.find(' ');
    const std::string_view proto = line.substr(0, space);
    if (proto == "HTTP/1.0") op->head.version = ResponseVersion::Http1_0;
    else if (proto == "HTTP/1.1") op->head.version = ResponseVersion::Http1_1;
    else if (proto == "HTTP/2") op->head.version = ResponseVersion::Http2;
    else if (proto == "HTTP/3") op->head.version = ResponseVersion::Http3;
    if (space != std::string_view::npos) {
      int code = 0;
      for (char c : trim(line.substr(space + 1)).substr(0, 3)) {
        if (c < '0' || c > '9') break;
        code = code * 10 + (c - '0');
      }
      op->head.status = code;
    }
    return len;
  }

  if (line.empty()) {  // end of a header block
    if (!op->head_started || op->head.status < 200) {  // 1xx: wait for the final block
      op->head_started = false;
      return len;
    }
    op->head.framing = derive_framing(op->head, iequals(op->req.method, "HEAD"));
    op->head_delivered = true;
    if (op->cb.on_head) {
      try {
        op->cb.on_head(op->head);
      } catch (...) {
        op->callback_threw = true;
        return 0;
      }
    }
    return len;
  }

  if (!op->head_started) return len;
  if (continuation) {  // unfold into the previous field; never read it as a new field
    if (!op->head.headers.empty()) {
      std::string& value = op->head.headers.back().value;
      if (!value.empty()) value += ' ';
      value.append(line);
    }
    return len;
  }
  const auto colon = line.find(':');
  if (colon == std::string_view::npos) return len;
  op->head.headers.push_back(Header{lower(trim(line.substr(0, colon))), std::string(trim(line.substr(colon + 1)))});
  return len;
}

std::size_t OperationState::write_cb(char* ptr, std::size_t size, std::size_t n, void* ud) {
  auto* op = static_cast<OperationState*>(ud);
  const std::size_t len = size * n;
  op->note_activity();
  if (!op->cb.on_body) return len;
  try {
    if (op->cb.on_body(std::string_view(ptr, len))) return len;
  } catch (...) {
    op->callback_threw = true;
    return CURL_WRITEFUNC_ERROR;
  }
  op->paused = true;  // not consumed: libcurl keeps the bytes and stops reading this transfer
  if (op->core) op->core->note_pause(*op);
  return CURL_WRITEFUNC_PAUSE;
}

int OperationState::xferinfo_cb(void* ud, curl_off_t, curl_off_t dlnow, curl_off_t, curl_off_t ulnow) {
  auto* op = static_cast<OperationState*>(ud);
  if (dlnow != op->seen_dl || ulnow != op->seen_ul) {
    op->seen_dl = dlnow;
    op->seen_ul = ulnow;
    op->note_activity();
  }
  return 0;
}

int OperationState::debug_cb(CURL*, curl_infotype type, char*, std::size_t size, void* ud) {
  if (type == CURLINFO_HEADER_OUT && size > 0)
    static_cast<OperationState*>(ud)->note_activity();
  return 0;  // Suppress every debug record, including credentials and payloads.
}

int OperationState::prereq_cb(void* ud, char*, char*, int, int) {
  auto* op = static_cast<OperationState*>(ud);
  op->note_connected();  // also reached for a reused connection; a second visit is harmless
  // The prerequisite point is reached once per request write. A second arrival means libcurl is
  // about to resend on a fresh connection after a reused one died. One attempt is one write.
  if (++op->prereq_count > 1) {
    op->resend_refused = true;
    return CURL_PREREQFUNC_ABORT;
  }
  long connects = 1;
  curl_easy_getinfo(op->easy, CURLINFO_NUM_CONNECTS, &connects);  // 0: the connection already existed
  op->first_write_reused = (connects == 0);
  return CURL_PREREQFUNC_OK;
}

// ---------------------------------------------------------------------------------------------
// Core
// ---------------------------------------------------------------------------------------------
void TransportCore::init() {
  ensure_curl_global_init();
  resolver = std::make_shared<ResolverPool>(std::max(1u, opts.resolver_threads),
                                            opts.resolve ? opts.resolve : ResolveFn(default_resolve));
#ifdef _WIN32
  resolver->winsock_runtime = winsock_runtime;
#endif
  const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
  if (info->version_num < 0x075800) {  // 7.88.0: PREREQFUNCTION, PROTOCOLS_STR, *_TIME_T infos
    throw std::runtime_error("libcurl 7.88.0 or newer is required");
  }
  ipv6_resolve_hosts = info->version_num >= 0x080D00;
  multi = curl_multi_init();
  if (!multi) throw std::runtime_error("curl_multi_init failed");
  curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, &TransportCore::socket_cb);
  curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, this);
  curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, &TransportCore::timer_cb);
  curl_multi_setopt(multi, CURLMOPT_TIMERDATA, this);
  curl_multi_setopt(multi, CURLMOPT_PIPELINING, static_cast<long>(CURLPIPE_MULTIPLEX));
  if (opts.max_host_connections > 0)
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, opts.max_host_connections);

  const unsigned n = std::max(1u, opts.io_threads);
  try {
    threads.reserve(n);  // never allocate a vector entry after starting a joinable worker
    for (unsigned i = 0; i < n; ++i) {
      threads.emplace_back([this] {
        detail::mark_io_thread();
        ctx.run();
      });
    }
  } catch (...) {
    // Transport construction has not published this core or queued any operation. Stop
    // directly instead of posting normal shutdown to a pool that may be incomplete.
    stopped = true;
    guard.reset();
    ctx.stop();
    for (auto& worker : threads)
      if (worker.joinable()) worker.join();
    threads.clear();
    curl_multi_cleanup(multi);
    multi = nullptr;
    throw;
  }
}

void TransportCore::begin(const std::shared_ptr<OperationState>& op) {
  if (op->finished) return;
  if (stopped || !multi) {
    finish(op, Status::Cancelled, FailureKind::None, CURLE_OK, "transport stopped");
    return;
  }
  if (!op->setup_error.empty()) {
    finish(op, Status::Failed, op->setup_failure, op->setup_code, op->setup_error.c_str());
    return;
  }
  if (op->req.deadline <= std::chrono::steady_clock::now()) {
    finish(op, Status::DeadlineExceeded, FailureKind::None, CURLE_OK, "deadline exceeded before start");
    return;
  }
  active.emplace(op.get(), op);

  if (op->req.deadline != std::chrono::steady_clock::time_point::max()) {
    op->deadline_timer.emplace(ctx);
    op->deadline_timer->expires_at(op->req.deadline);
    op->deadline_timer->async_wait(asio::bind_executor(
        strand, [self = shared_from_this(), op](const asio::error_code& ec) {
          if (!ec) self->finish(op, Status::DeadlineExceeded, FailureKind::None, CURLE_OK, "deadline exceeded");
        }));
  }

  arm_stall_timers(op);

  if (op->needs_resolve) {
    lookup(op);
  } else if (op->host.front() != '[' || ipv6_resolve_hosts) {
    // A literal is already an exact resolved address. Install it just like an SDK resolver
    // answer: Darwin libcurl otherwise sends even numeric hosts to its own growing DNS pool.
    // Exact host+port entries also keep concurrent literal origins isolated.
    apply_addrs_and_add(op, std::span<const std::string>(&op->host, 1));
  } else {
    // Older curl cannot parse IPv6 HOST rules. Preserve working IPv6 connections on those
    // libraries; their native numeric resolver cannot be bypassed with this cache API.
    add_to_multi(op);
  }
}

void TransportCore::add_to_multi(const std::shared_ptr<OperationState>& op) {
  if (op->finished || stopped || !multi) return;  // shutdown finishes every active operation
  CURLMcode added;
  {
    // With no other active operation to batch, a zero-time kick can run after
    // add_handle returns instead of taking another turn through the strand.
    // Never call socket_action inside libcurl's timer callback itself.
    defer_start_kick = active.size() == 1 && !timeout_posted;
    struct ResetKick {
      bool& flag;
      ~ResetKick() { flag = false; }
    } reset{defer_start_kick};
    added = curl_multi_add_handle(multi, op->easy);
  }
  if (added != CURLM_OK)
    finish(op, Status::Failed, FailureKind::Other, CURLE_OK, "curl_multi_add_handle failed");
  else
    op->in_multi = true;  // publish before a kick can complete/remove this handle
  // DNS completion or handle setup may have consumed the remaining slack.
  // An eager kick must not overtake an already-due operation deadline.
  if (timer_mode == TimerMode::Immediate && !timeout_posted && !op->finished &&
      op->req.deadline <= std::chrono::steady_clock::now())
    finish(op, Status::DeadlineExceeded, FailureKind::None, CURLE_OK, "deadline exceeded before dispatch");
  if (timer_mode == TimerMode::Immediate && !timeout_posted) {
    timer_mode = TimerMode::Inactive;
    on_timeout();
  }
}

void TransportCore::lookup(const std::shared_ptr<OperationState>& op) {
  std::string key;
  bool owns_submission = false;
  ResolverPool::SubmitStatus status = ResolverPool::SubmitStatus::ResourceFailure;
  try {
    key = lower(op->host);
    const auto cached = dns_cache.find(key);
    if (cached != dns_cache.end()) {
      if (cached->second.expires > std::chrono::steady_clock::now()) {
        apply_addrs_and_add(op, cached->second.addrs);
        return;
      }
      dns_cache.erase(cached);
    }
    auto& pending = dns_pending[key];
    pending.waiters.push_back(op);
    if (pending.in_flight) return;  // single-flight: a resolver job for this host is running
    owns_submission = true;
    pending.in_flight = true;
    std::weak_ptr<TransportCore> weak = shared_from_this();
    auto pool = resolver;
    status = pool->submit([weak, pool, key] {
      std::vector<std::string> addrs;
      try {
        addrs = pool->resolve(key);
      } catch (...) {
      }
      if (auto core = weak.lock()) {
        asio::post(core->strand, [weak, key, addrs = std::move(addrs)]() mutable {
          if (auto self = weak.lock()) self->on_resolved(key, std::move(addrs));
        });
      }
    });
    if (status == ResolverPool::SubmitStatus::Queued) return;
  } catch (...) {
    // Covers pending-waiter/callable allocation as well as submit's locking failures.
    // An existing in-flight lookup still belongs to its other waiters.
  }
  const auto pending = dns_pending.find(key);
  if (pending != dns_pending.end() &&
      (owns_submission || (!pending->second.in_flight && pending->second.waiters.empty())))
    dns_pending.erase(pending);
  if (status == ResolverPool::SubmitStatus::Stopped)
    finish(op, Status::Cancelled, FailureKind::None, CURLE_OK, "transport shutdown");
  else
    finish(op, Status::Failed, FailureKind::Other, CURLE_FAILED_INIT, "resolver scheduling failed");
}

void TransportCore::on_resolved(const std::string& host, std::vector<std::string> addrs) {
  const auto it = dns_pending.find(host);
  if (it == dns_pending.end()) return;
  std::vector<std::shared_ptr<OperationState>> waiters = std::move(it->second.waiters);
  dns_pending.erase(it);
  if (!addrs.empty()) {
    dns_cache[host] = DnsEntry{addrs, std::chrono::steady_clock::now() + opts.dns_ttl};
  }
  for (auto& op : waiters) {
    if (op->finished) continue;  // cancelled or past its deadline while the lookup ran
    if (addrs.empty()) finish(op, Status::Failed, FailureKind::Resolve, CURLE_COULDNT_RESOLVE_HOST, "name resolution failed");
    else apply_addrs_and_add(op, addrs);
  }
}

void TransportCore::apply_addrs_and_add(const std::shared_ptr<OperationState>& op,
                                        std::span<const std::string> addrs) {
  std::string joined;
  for (const std::string& a : addrs) {
    if (!joined.empty()) joined += ',';
    joined += (a.find(':') != std::string::npos && a.front() != '[') ? "[" + a + "]" : a;
  }
  const std::string entry = op->host + ":" + op->port + ":" + joined;
  curl_slist* list = curl_slist_append(nullptr, entry.c_str());
  if (!list || curl_easy_setopt(op->easy, CURLOPT_RESOLVE, list) != CURLE_OK) {
    if (list) curl_slist_free_all(list);
    finish(op, Status::Failed, FailureKind::Other, CURLE_OK, "could not install resolved addresses");
    return;
  }
  op->resolve_list = list;
  add_to_multi(op);
}

void TransportCore::finish(std::shared_ptr<OperationState> op, Status status, FailureKind failure,
                           CURLcode code, const char* detail) {
  if (op->finished) return;  // exactly one outcome: cancel, deadline and completion race here
  op->finished = true;
  op->finishing.store(true, std::memory_order_relaxed);
  if (op->deadline_timer) {
    // The timer references the io_context's services, which die with the transport while an
    // Operation handle may outlive it: destroy it here, on the strand, never in ~OperationState.
    op->deadline_timer->cancel();
    op->deadline_timer.reset();
  }
  // Same rule for the stall timers: destroyed here, on the strand. Clearing the armed flags makes
  // a handler whose wait already completed (and is queued) a no-op.
  op->connect_armed = op->first_byte_armed = false;
  for (auto* timer : {&op->connect_timer, &op->first_byte_timer, &op->idle_timer}) {
    if (*timer) {
      (*timer)->cancel();
      timer->reset();
    }
  }

  Result r;
  r.status = status;
  r.failure = failure;
  r.curl_code = static_cast<int>(code);
  r.detail = detail ? detail : "";
  r.elapsed = std::chrono::steady_clock::now() - op->started;
  r.attempt.version = op->head.version;
  r.attempt.response_head_seen = op->head_delivered;
  r.attempt.transport_internal_resends = op->prereq_count > 1 ? op->prereq_count - 1 : 0;
  r.upload_sha256 = op->upload_digest;

  if (op->easy) {
    long http_status = 0;
    curl_easy_getinfo(op->easy, CURLINFO_RESPONSE_CODE, &http_status);
    r.http_status = static_cast<int>(http_status);
    curl_off_t t_name = 0, t_conn = 0, uploaded = 0;
    curl_easy_getinfo(op->easy, CURLINFO_NAMELOOKUP_TIME_T, &t_name);
    curl_easy_getinfo(op->easy, CURLINFO_CONNECT_TIME_T, &t_conn);
    curl_easy_getinfo(op->easy, CURLINFO_SIZE_UPLOAD_T, &uploaded);
    r.attempt.request_body_bytes = uploaded;
    r.attempt.connection_reused = op->first_write_reused;
    // The prerequisite callback runs before any request write, and is the conservative latch for
    // "request bytes may have left". PRETRANSFER_TIME is not a send witness: newer libcurl also
    // populates it while completing failures that never reached the request phase (connect/TLS).
    // Response and upload observations remain independent positive evidence, never error-text rules.
    if (op->response_seen) r.attempt.reached = Stage::ResponseStarted;
    else if (op->prereq_count > 0 || uploaded > 0) r.attempt.reached = Stage::RequestStarted;
    else if (t_conn > 0) r.attempt.reached = Stage::Connected;
    else if (t_name > 0) r.attempt.reached = Stage::Resolved;
    if (r.attempt.version == ResponseVersion::Unknown) {
      long v = 0;
      curl_easy_getinfo(op->easy, CURLINFO_HTTP_VERSION, &v);
      if (v == CURL_HTTP_VERSION_1_0) r.attempt.version = ResponseVersion::Http1_0;
      else if (v == CURL_HTTP_VERSION_1_1) r.attempt.version = ResponseVersion::Http1_1;
      else if (v == CURL_HTTP_VERSION_2_0) r.attempt.version = ResponseVersion::Http2;
      else if (v == CURL_HTTP_VERSION_3) r.attempt.version = ResponseVersion::Http3;
    }
  }

  if (op->ws) {
    ws_release(*op);
    r.http_status = op->ws->http_status;
  }
  if (op->in_multi && multi) {
    curl_multi_remove_handle(multi, op->easy);
    op->in_multi = false;
  }
  if (op->easy) {
    curl_easy_cleanup(op->easy);
    op->easy = nullptr;
  }
  if (op->hdrs) {
    curl_slist_free_all(op->hdrs);
    op->hdrs = nullptr;
  }
  if (op->resolve_list) {
    curl_slist_free_all(op->resolve_list);
    op->resolve_list = nullptr;
  }
  if (op->needs_resolve) {  // a cancelled waiter must not stay referenced by a stuck lookup
    const auto pending = dns_pending.find(lower(op->host));
    if (pending != dns_pending.end()) {
      auto& w = pending->second.waiters;
      w.erase(std::remove(w.begin(), w.end(), op), w.end());
    }
  }
  if (op->sock != CURL_SOCKET_BAD) {
    const auto owner = sock_owner.find(op->sock);
    if (owner != sock_owner.end() && owner->second == op.get()) sock_owner.erase(owner);
  }
  note_resume(*op);
  active.erase(op.get());

  {
    std::lock_guard lock(op->mu);
    op->result = r;
  }
  if (op->cb.on_done) {
    try {
      op->cb.on_done(r);
    } catch (...) {
      // on_done must not throw; there is nowhere left to report it.
    }
  }
  // Break callback-owned cycles now that no callback can run again, then release join().
  {
    Callbacks released = std::move(op->cb);
    op->cb = Callbacks{};
    if (op->ws) {
      WsCallbacks session = std::move(op->ws->cb);
      op->ws->cb = WsCallbacks{};
    }
  }  // Destroy callback storage before join() can return.
  {
    std::lock_guard lock(op->mu);
    op->done = true;
  }
  op->cv.notify_all();
}

void TransportCore::resume(const std::shared_ptr<OperationState>& op) {
  if (op->finished || !op->paused || !multi) return;
  op->paused = false;
  if (op->idle_bound.count() > 0) op->last_activity = std::chrono::steady_clock::now();
  note_resume(*op);
  // Unpausing redelivers the retained bytes synchronously; an error from that delivery (for
  // example on_body throwing) comes back here and produces no completion message of its own.
  const CURLcode rc = curl_easy_pause(op->easy, CURLPAUSE_CONT);
  if (rc != CURLE_OK) {
    complete_from_curl(op, rc);
    return;
  }
  on_timeout();  // documented kick after unpausing
}

void TransportCore::note_pause(OperationState& op) {
  if ((op.head.version != ResponseVersion::Http1_0 && op.head.version != ResponseVersion::Http1_1) ||
      op.sock == CURL_SOCKET_BAD || op.counted_pause) return;
  op.counted_pause = true;
  paused_h1_socks.insert(op.sock);
#ifdef _WIN32
  rearm_socket(op.sock);  // remove FD_READ/FD_CLOSE interest before another event can drain data
#endif
}

void TransportCore::note_resume(OperationState& op) {
  if (!op.counted_pause) return;
  op.counted_pause = false;
  paused_h1_socks.erase(op.sock);
  rearm_socket(op.sock);
}

bool TransportCore::read_suppressed(curl_socket_t s) const { return paused_h1_socks.count(s) != 0; }

void TransportCore::rearm_socket(curl_socket_t s) {
  const auto it = socks.find(s);
  if (it != socks.end()) arm(it->second);
}

void TransportCore::complete_from_curl(const std::shared_ptr<OperationState>& op, CURLcode code) {
  if (op->ws && code == CURLE_OK) {
    ws_connected(op);
    return;
  }
  if (code == CURLE_OK) {
    if (!op->head_delivered) {
      finish(op, Status::Failed, FailureKind::Protocol, code, "response ended without a head");
    } else if (op->req.http_version == HttpVersion::Http3Only && op->head.version != ResponseVersion::Http3) {
      finish(op, Status::Failed, FailureKind::Protocol, CURLE_UNSUPPORTED_PROTOCOL,
             "HTTP/3-only exchange did not negotiate HTTP/3");
    } else if (op->head.framing == BodyFraming::CloseDelimited) {
      finish(op, Status::Failed, FailureKind::Truncated, code,
             "body delimited only by connection close is not a normal end");
    } else {
      finish(op, Status::Completed, FailureKind::None, code, "");
    }
    return;
  }
  FailureKind kind = FailureKind::Other;
  switch (code) {
    case CURLE_ABORTED_BY_CALLBACK:
      if (op->resend_refused) kind = FailureKind::ResendRefused;
      else if (op->upload_failed) kind = FailureKind::UploadSource;
      break;
    case CURLE_WRITE_ERROR:
      if (op->callback_threw) kind = FailureKind::CallbackError;
      else if (op->head_overflow) kind = FailureKind::ResponseTooLarge;
      else kind = FailureKind::Receive;
      break;
    // Older libcurl reports its header-line bound as OOM; 8.6+ uses TOO_LARGE.
#if LIBCURL_VERSION_NUM >= 0x080600
    case CURLE_TOO_LARGE:
#endif
    case CURLE_OUT_OF_MEMORY:
      kind = (op->head_started && !op->head_delivered) ? FailureKind::ResponseTooLarge : FailureKind::Other;
      break;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY: kind = FailureKind::Resolve; break;
    case CURLE_COULDNT_CONNECT: kind = FailureKind::Connect; break;
    case CURLE_QUIC_CONNECT_ERROR:
      kind = op->head_delivered ? FailureKind::Truncated : FailureKind::Connect;
      break;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_INVALIDCERTSTATUS:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH: kind = FailureKind::Tls; break;
    case CURLE_SEND_ERROR: kind = FailureKind::Send; break;
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_HTTP2_STREAM:
      kind = op->head_delivered ? FailureKind::Truncated : FailureKind::Receive;
      break;
    case CURLE_PARTIAL_FILE: kind = FailureKind::Truncated; break;
    case CURLE_HTTP3:
      kind = op->head_delivered ? FailureKind::Truncated : FailureKind::Protocol;
      break;
    case CURLE_WEIRD_SERVER_REPLY:
    case CURLE_BAD_CONTENT_ENCODING:
    case CURLE_HTTP2:
    case CURLE_UNSUPPORTED_PROTOCOL: kind = FailureKind::Protocol; break;
    default: break;
  }
  finish(op, Status::Failed, kind, code, kind == FailureKind::UploadSource ? "upload source changed" : curl_easy_strerror(code));
}

void TransportCore::check_multi_info() {
  if (!multi) return;
  int left = 0;
  while (CURLMsg* msg = curl_multi_info_read(multi, &left)) {
    if (msg->msg != CURLMSG_DONE) continue;
    OperationState* raw = nullptr;
    curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &raw);
    auto it = active.find(raw);
    if (it == active.end()) continue;
    std::shared_ptr<OperationState> op = it->second;
    complete_from_curl(op, msg->data.result);
  }
}

void TransportCore::on_timeout() {
  if (!multi) return;
  int running = 0;
  curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &running);
  check_multi_info();
}

// Stall bounds. Each bound owns one timer, started at the attempt's start (OperationState::started).
// A timer handler runs on the strand, so it observes a consistent view of the operation; the
// handler of a wait that already completed when its bound was satisfied sees the cleared armed flag
// (connect, first byte) or an expiry that moved later (idle) and does nothing. Completion goes
// through finish(), which makes the first outcome win against the deadline, cancel and completion.
void TransportCore::arm_stall_timers(const std::shared_ptr<OperationState>& op) {
  if (op->connect_bound.count() > 0) {
    op->connect_armed = true;
    op->connect_timer.emplace(ctx);
    op->connect_timer->expires_at(op->started + op->connect_bound);
    op->connect_timer->async_wait(asio::bind_executor(
        strand, [self = shared_from_this(), op](const asio::error_code& ec) {
          if (ec || op->finished || !op->connect_armed) return;
          self->finish(op, Status::Failed, FailureKind::ConnectTimeout, CURLE_OPERATION_TIMEDOUT,
                       "connect timeout");
        }));
  }
  if (op->first_byte_bound.count() > 0) {
    op->first_byte_armed = true;
    op->first_byte_timer.emplace(ctx);
    op->first_byte_timer->expires_at(op->started + op->first_byte_bound);
    op->first_byte_timer->async_wait(asio::bind_executor(
        strand, [self = shared_from_this(), op](const asio::error_code& ec) {
          if (ec || op->finished || !op->first_byte_armed) return;
          self->finish(op, Status::Failed, FailureKind::FirstByteTimeout, CURLE_OPERATION_TIMEDOUT,
                       "first byte timeout");
        }));
  }
  if (op->idle_bound.count() > 0) {
    op->last_activity = op->started;
    op->idle_timer.emplace(ctx);
    op->idle_timer->expires_at(op->started + op->idle_bound);
    op->idle_timer->async_wait(asio::bind_executor(
        strand, [self = shared_from_this(), op](const asio::error_code& ec) {
          if (!ec) self->on_idle_expired(op);
        }));
  }
}

// One timer serves the whole transfer: activity only stores a timestamp, and the timer is re-armed
// here to last_activity + idle instead of once per chunk.
void TransportCore::on_idle_expired(const std::shared_ptr<OperationState>& op) {
  if (op->finished || !op->idle_timer) return;
  const auto now = std::chrono::steady_clock::now();
  // A paused transfer is the application applying backpressure, not the peer going silent.
  if (op->paused) op->last_activity = now;
  // Round positive slack upward before adding: preserve sub-millisecond deadline slack while
  // rejecting enormous bounds that would overflow the clock's representable range.
  if (op->idle_bound >= std::chrono::ceil<std::chrono::milliseconds>(op->req.deadline - op->last_activity))
    return;  // The immutable deadline is earlier (also avoids overflowing last_activity + bound).
  const auto due = op->last_activity + op->idle_bound;
  if (due <= now) {
    finish(op, Status::Failed, FailureKind::IdleTimeout, CURLE_OPERATION_TIMEDOUT, "idle timeout");
    return;
  }
  if (due >= op->req.deadline) return;  // the deadline is earlier and activity only moves `due` later
  op->idle_timer->expires_at(due);
  op->idle_timer->async_wait(asio::bind_executor(
      strand, [self = shared_from_this(), op](const asio::error_code& ec) {
        if (!ec) self->on_idle_expired(op);
      }));
}

void TransportCore::set_timer(long ms) {
  const auto generation = ++timer_generation;
  if (ms <= 0) {
    if (timer_mode == TimerMode::Delayed) timer.cancel();
    timer_mode = ms < 0 ? TimerMode::Inactive : TimerMode::Immediate;
    if (ms < 0 || timeout_posted || defer_start_kick) return;
    // libcurl forbids re-entering socket_action inside its timer callback.
    // Several zero-time updates before this strand turn need only one kick;
    // a subsequent negative/positive update supersedes that queued kick.
    timeout_posted = true;
    asio::post(strand, [self = shared_from_this()] {
      self->timeout_posted = false;
      if (self->timer_mode != TimerMode::Immediate) return;
      self->timer_mode = TimerMode::Inactive;
      self->on_timeout();
    });
    return;
  }
  timer_mode = TimerMode::Delayed;
  // expires_after already cancels the preceding wait; do not cancel it twice.
  timer.expires_after(std::chrono::milliseconds(ms));
  timer.async_wait(asio::bind_executor(strand, [self = shared_from_this(), generation](const asio::error_code& ec) {
    // Cancellation cannot retract a successful handler already on the strand.
    if (ec || generation != self->timer_generation || self->timer_mode != TimerMode::Delayed) return;
    self->timer_mode = TimerMode::Inactive;
    self->on_timeout();
  }));
}

void TransportCore::on_socket(CURL* easy, curl_socket_t s, int what) {
  if (const auto session = socks.find(s); session != socks.end() && session->second->ws_owner)
    return;  // a WebSocket session watches this socket now; ws_release ends that
  if (what == CURL_POLL_REMOVE) {
    auto it = socks.find(s);
    if (it != socks.end()) {
      it->second->removed = true;
      it->second->release_quiet();  // aborts pending waits; libcurl closes the descriptor
      socks.erase(it);
    }
    paused_h1_socks.erase(s);
    const auto owner = sock_owner.find(s);
    if (owner != sock_owner.end()) {
      OperationState* gone = owner->second;
      if (gone->sock == s) {  // the descriptor is gone: forget the association and any pause counted on it
        gone->sock = CURL_SOCKET_BAD;
        gone->counted_pause = false;
      }
      sock_owner.erase(owner);
    }
    return;
  }
  OperationState* op = nullptr;
  if ((what & CURL_POLL_IN) && easy && curl_easy_getinfo(easy, CURLINFO_PRIVATE, &op) == CURLE_OK && op) {
    // On HTTP/1.x the socket that carries the response is the one libcurl wants to read from;
    // candidate sockets of a connect race (Happy Eyeballs) only ever want OUT. The association is
    // therefore updated whenever read interest is announced, not frozen at the first socket seen.
    if (op->sock != s) {
      if (op->counted_pause) {
        paused_h1_socks.erase(op->sock);
        op->counted_pause = false;
      }
      op->sock = s;
    }
    sock_owner[s] = op;
    if (op->paused) note_pause(*op);  // the pause may have been requested before the socket was known
  }
  auto& slot = socks[s];
  if (!slot) slot = std::make_shared<SockWatch>(ctx, s);
  slot->want_read = (what & CURL_POLL_IN) != 0;
  slot->want_write = (what & CURL_POLL_OUT) != 0;
  arm(slot);
}

void TransportCore::arm(const std::shared_ptr<SockWatch>& w) {
  if (w->removed) return;
#ifdef _WIN32
  if (w->failed) return;
  const bool read = w->want_read && !read_suppressed(w->fd);
  const long desired = (read ? FD_READ | FD_CLOSE : 0L) |
                       (w->want_write ? FD_WRITE | FD_CONNECT | FD_CLOSE : 0L);
  if (desired != w->selected_events) {
    // Interest changes can race a completion already queued on the strand. Cancel only the
    // event wait; the generation fence prevents its handler from touching a reused socket
    // or clearing the armed flag of the replacement wait.
    ++w->generation;
    asio::error_code ignored;
    w->event.cancel(ignored);
    w->armed = false;
    if (WSAEventSelect(w->fd, w->event.native_handle(), desired) == SOCKET_ERROR) {
      w->failed = true;
      fail_watches_later();
      return;
    }
    w->selected_events = desired;
  }
  if (desired == 0 || w->armed) return;

  // FD_WRITE is edge-triggered, unlike the level-triggered POSIX wait. Probe current
  // readiness once before each one-shot wait, and post genuinely ready sockets to the
  // strand. Otherwise the Winsock event wakes Asio with no timer or polling thread.
  // Unlike resetting FD_WRITE with send(..., 0), this also works for QUIC/UDP sockets
  // and cannot emit an empty datagram.
  fd_set reads, writes, errors;
  FD_ZERO(&reads);
  FD_ZERO(&writes);
  FD_ZERO(&errors);
  if (read) FD_SET(w->fd, &reads);
  if (w->want_write) FD_SET(w->fd, &writes);
  FD_SET(w->fd, &errors);
  timeval immediate{};
  const int ready = select(0, read ? &reads : nullptr, w->want_write ? &writes : nullptr,
                           &errors, &immediate);
  if (ready == SOCKET_ERROR) {
    w->failed = true;
    fail_watches_later();
    return;
  }
  int flags = 0;
  if (FD_ISSET(w->fd, &reads)) flags |= CURL_CSELECT_IN;
  if (FD_ISSET(w->fd, &writes)) flags |= CURL_CSELECT_OUT;
  if (FD_ISSET(w->fd, &errors)) flags |= CURL_CSELECT_ERR;
  w->armed = true;
  const std::uint64_t generation = w->generation;
  if (flags != 0) {
    // Never drive curl inline from its socket callback, even for immediate readiness.
    asio::post(strand, [self = shared_from_this(), w, generation, flags]() mutable {
      if (w->removed || w->generation != generation) return;
      w->armed = false;
      if (!w->want_read || self->read_suppressed(w->fd)) flags &= ~CURL_CSELECT_IN;
      if (!w->want_write) flags &= ~CURL_CSELECT_OUT;
      if (flags != 0) self->on_socket_event(w, flags);
      else self->arm(w);
    });
    return;
  }
  // The probe above found the socket not writable, so wait for FD_WRITE. Winsock records FD_WRITE
  // only after a send() has failed with WSAEWOULDBLOCK and buffer space has returned. libcurl sends
  // one chunk per socket action: when that send succeeded and left the socket not writable, no
  // send has failed, FD_WRITE is never recorded, and the event wait alone would last until the
  // operation deadline although the peer has drained the buffer. Poll select() as a bound.
  if (w->want_write) {
    w->write_poll_delay = kWritePollInitial;
    poll_write_readiness(w, generation);
  }
  w->event.async_wait(asio::bind_executor(
      strand, [self = shared_from_this(), w, generation](const asio::error_code& ec) {
        if (w->removed || w->generation != generation) return;
        w->armed = false;
        if (ec == asio::error::operation_aborted) return;
        WSANETWORKEVENTS events{};
        if (ec || WSAEnumNetworkEvents(w->fd, w->event.native_handle(), &events) == SOCKET_ERROR) {
          w->failed = true;
          self->fail_watches_later();
          return;
        }
        // EnumNetworkEvents atomically clears both the record and the manual-reset event.
        // Recheck current interest: a paused HTTP/1.x response must stay in the kernel.
        int observed = 0;
        if (w->want_read && !self->read_suppressed(w->fd) &&
            (events.lNetworkEvents & (FD_READ | FD_CLOSE)))
          observed |= CURL_CSELECT_IN;
        if (w->want_write && (events.lNetworkEvents & (FD_WRITE | FD_CONNECT | FD_CLOSE)))
          observed |= CURL_CSELECT_OUT;
        for (const int bit : {FD_READ_BIT, FD_WRITE_BIT, FD_CONNECT_BIT, FD_CLOSE_BIT}) {
          if ((events.lNetworkEvents & (1L << bit)) && events.iErrorCode[bit] != 0)
            observed |= CURL_CSELECT_ERR;
        }
        if (observed != 0) self->on_socket_event(w, observed);
        else self->arm(w);
      }));
#else
  if (w->want_read && !w->armed_read && !read_suppressed(w->fd)) {
    w->armed_read = true;
    w->desc.async_wait(asio::posix::stream_descriptor::wait_read,
                       asio::bind_executor(strand, [self = shared_from_this(), w](const asio::error_code& ec) {
                         w->armed_read = false;
                         if (w->removed || ec == asio::error::operation_aborted) return;
                         if (!w->want_read || self->read_suppressed(w->fd)) return;  // keep paused data in the kernel
                         self->on_socket_event(w, ec ? CURL_CSELECT_ERR : CURL_CSELECT_IN);
                       }));
  }
  if (w->want_write && !w->armed_write) {
    w->armed_write = true;
    w->desc.async_wait(asio::posix::stream_descriptor::wait_write,
                       asio::bind_executor(strand, [self = shared_from_this(), w](const asio::error_code& ec) {
                         w->armed_write = false;
                         if (w->removed || ec == asio::error::operation_aborted) return;
                         if (!w->want_write) return;  // a queued wait may outlive libcurl's write interest
                         self->on_socket_event(w, ec ? CURL_CSELECT_ERR : CURL_CSELECT_OUT);
                       }));
  }
#endif
}

void TransportCore::on_socket_event(const std::shared_ptr<SockWatch>& w, int flags) {
  if (!multi) return;
  if (w->ws_owner) {
    ws_ready(w, flags);
    return;
  }
  int running = 0;
  curl_multi_socket_action(multi, w->fd, flags, &running);
  check_multi_info();
  if (!w->removed) arm(w);  // one-shot waits: re-arm whatever libcurl still wants
}

#ifdef _WIN32
// Probes a pending write wait with select() on a short, growing interval. The interval stays small
// because a missed FD_WRITE otherwise costs the whole interval per filled buffer, and bounded
// because thousands of blocked uploads must not turn into a polling storm. Once the event fires,
// the wait is replaced or the interest drops, the chain ends: a single timer per watch means a
// newer wait replaces a pending poll instead of adding a second chain.
void TransportCore::poll_write_readiness(const std::shared_ptr<SockWatch>& w, std::uint64_t generation) {
  w->write_poll.expires_after(w->write_poll_delay);
  w->write_poll_delay = (std::min)(w->write_poll_delay * 2, kWritePollMax);
  w->write_poll.async_wait(asio::bind_executor(
      strand, [self = shared_from_this(), w, generation](const asio::error_code& ec) {
        if (ec || w->removed || w->failed || w->generation != generation || !w->armed || !w->want_write) return;
        fd_set writes, errors;
        FD_ZERO(&writes);
        FD_ZERO(&errors);
        FD_SET(w->fd, &writes);
        FD_SET(w->fd, &errors);
        timeval immediate{};
        if (select(0, nullptr, &writes, &errors, &immediate) == SOCKET_ERROR) {
          w->failed = true;
          self->fail_watches_later();
          return;
        }
        int flags = 0;
        if (FD_ISSET(w->fd, &writes)) flags |= CURL_CSELECT_OUT;
        if (FD_ISSET(w->fd, &errors)) flags |= CURL_CSELECT_ERR;
        if (flags == 0) {
          self->poll_write_readiness(w, generation);
          return;
        }
        // The event wait is still pending. Retire it, then drive libcurl as the event would have;
        // on_socket_event re-arms whatever libcurl still wants.
        ++w->generation;
        asio::error_code ignored;
        w->event.cancel(ignored);
        w->armed = false;
        self->on_socket_event(w, flags);
      }));
}

void TransportCore::fail_watches_later() {
  if (stopped || watch_failure_pending) return;
  watch_failure_pending = true;
  // A socket callback is inside a libcurl call. Finish outside that call, on the same
  // strand, rather than re-entering curl or waiting indefinitely on an unmonitored socket.
  asio::post(strand, [self = shared_from_this()] {
    self->watch_failure_pending = false;
    if (self->stopped) return;
    std::vector<std::shared_ptr<OperationState>> remaining;
    remaining.reserve(self->active.size());
    for (const auto& entry : self->active) remaining.push_back(entry.second);
    for (const auto& op : remaining)
      self->finish(op, Status::Failed, FailureKind::Other, CURLE_FAILED_INIT,
                   "socket readiness monitoring failed");
    for (auto& entry : self->socks) {
      entry.second->removed = true;
      entry.second->release_quiet();
    }
    self->socks.clear();
  });
}
#endif

void TransportCore::shutdown() {
  if (resolver) resolver->stop();
  std::promise<void> cleaned;
  std::future<void> cleaned_future = cleaned.get_future();
  asio::post(strand, [this, &cleaned] {
    stopped = true;
    std::vector<std::shared_ptr<OperationState>> remaining;
    remaining.reserve(active.size());
    for (auto& entry : active) remaining.push_back(entry.second);
    for (auto& op : remaining)
      finish(op, Status::Cancelled, FailureKind::None, CURLE_OK, "transport shutdown");
    dns_pending.clear();  // waiters were finished above; drop their references
    dns_cache.clear();
    set_timer(-1);
    for (auto& entry : socks) {
      entry.second->removed = true;
      entry.second->release_quiet();
    }
    socks.clear();
    if (multi) {
      curl_multi_cleanup(multi);  // closes cached idle connections
      multi = nullptr;
    }
    cleaned.set_value();
  });
  cleaned_future.wait();
  guard.reset();
  for (auto& t : threads) t.join();
}

// ---------------------------------------------------------------------------------------------
// WebSocket sessions (ws_transport.h). A session is a connect-only operation: libcurl resolves,
// connects and completes TLS, then complete_from_curl() hands the connection to ws_connected().
// The easy handle stays in the multi handle until finish(), because removing it would close the
// connection. From then on the session watches the socket itself (SockWatch::ws_owner) and moves
// bytes with curl_easy_send/curl_easy_recv, all on the strand.
// ---------------------------------------------------------------------------------------------
namespace {

// The request target (path and query) of a ws/wss URL; a fragment is never sent.
std::string request_target(std::string_view url) {
  const auto scheme_end = url.find("://");
  const auto start = url.find_first_of("/?#", scheme_end == std::string_view::npos ? 0 : scheme_end + 3);
  if (start == std::string_view::npos) return "/";
  std::string target(url.substr(start));
  if (const auto hash = target.find('#'); hash != std::string::npos) target.erase(hash);
  if (target.empty() || target.front() != '/') target.insert(0, "/");
  return target;
}

WsResult ws_result(const WsState& ws, const Result& r) {
  WsResult out;
  out.transport = r;
  out.http_status = ws.http_status;
  if (ws.sent_close || ws.received_close) {
    out.close_code = ws.close_code;
    out.close_reason = ws.close_reason;
  }
  out.closed_by_peer = ws.closed_by_peer;
  out.clean = ws.sent_close && ws.received_close && r.status == Status::Completed;
  return out;
}

bool header_line_safe(std::string_view s) {
  return std::none_of(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
}

}  // namespace

void TransportCore::ws_connected(const std::shared_ptr<OperationState>& op) {
  auto& ws = *op->ws;
  curl_socket_t fd = CURL_SOCKET_BAD;
  if (curl_easy_getinfo(op->easy, CURLINFO_ACTIVESOCKET, &fd) != CURLE_OK || fd == CURL_SOCKET_BAD) {
    finish(op, Status::Failed, FailureKind::Connect, CURLE_COULDNT_CONNECT, "no connected socket");
    return;
  }
  op->note_connected();
  try {
    auto& slot = socks[fd];
    if (!slot) slot = std::make_shared<SockWatch>(ctx, fd);
    slot->ws_owner = op.get();
  } catch (...) {
    socks.erase(fd);
    finish(op, Status::Failed, FailureKind::Other, CURLE_FAILED_INIT, "socket readiness monitoring failed");
    return;
  }
  ws.fd = fd;
  std::array<unsigned char, 16> nonce{};
  for (std::size_t i = 0; i < nonce.size(); i += 4) {
    const auto random = ws.mask();
    std::copy(random.begin(), random.end(), nonce.begin() + static_cast<std::ptrdiff_t>(i));
  }
  ws.key = ws_key_from(nonce);
  std::string authority = op->host;
  if (op->port != (ws.secure ? "443" : "80")) authority += ":" + op->port;
  std::vector<std::pair<std::string, std::string>> headers;
  headers.reserve(ws.req.headers.size());
  for (const Header& h : ws.req.headers) headers.emplace_back(h.name, h.value);
  ws.out = ws_handshake_request(authority, request_target(ws.req.url), ws.key, headers);
  if (ws.out.empty()) {
    finish(op, Status::Failed, FailureKind::Other, CURLE_OK, "invalid handshake request");
    return;
  }
  ws.phase = WsState::Phase::Handshake;
  op->prereq_count = 1;  // request bytes may leave from here on (AttemptObservation::reached)
  ws_flush(op);
  if (!op->finished) ws_interest(op);
}

void TransportCore::ws_ready(const std::shared_ptr<SockWatch>& w, int flags) {
  const auto it = active.find(w->ws_owner);
  if (it == active.end()) return;
  const std::shared_ptr<OperationState> op = it->second;
  if (flags & CURL_CSELECT_OUT) ws_flush(op);
  if (!op->finished && (flags & (CURL_CSELECT_IN | CURL_CSELECT_ERR))) ws_read(op);
  if (!op->finished) ws_interest(op);
}

void TransportCore::ws_interest(const std::shared_ptr<OperationState>& op) {
  auto& ws = *op->ws;
  const auto it = socks.find(ws.fd);
  if (it == socks.end()) return;
  it->second->want_read = true;  // until the end: a Close, the server's TCP close or an error
  it->second->want_write = ws.out_offset < ws.out.size();
  arm(it->second);
}

void TransportCore::ws_flush(const std::shared_ptr<OperationState>& op) {
  auto& ws = *op->ws;
  while (ws.out_offset < ws.out.size()) {
    std::size_t sent = 0;
    const CURLcode rc = curl_easy_send(op->easy, ws.out.data() + ws.out_offset, ws.out.size() - ws.out_offset, &sent);
    if (rc == CURLE_AGAIN || (rc == CURLE_OK && sent == 0)) return;  // wait for write readiness
    if (rc != CURLE_OK) {
      finish(op, Status::Failed, FailureKind::Send, rc, "send failed");
      return;
    }
    ws.out_offset += sent;
    op->note_activity();
  }
  ws.out.clear();
  ws.out_offset = 0;
}

void TransportCore::ws_read(const std::shared_ptr<OperationState>& op) {
  auto& ws = *op->ws;
  char buffer[16384];
  for (int round = 0; round < 64; ++round) {
    std::size_t got = 0;
    const CURLcode rc = curl_easy_recv(op->easy, buffer, sizeof buffer, &got);
    if (rc == CURLE_AGAIN) return;
    if (rc != CURLE_OK || got == 0) {
      // The server closed the connection (got == 0) or reset it. After both Close frames, or after
      // the client failed the session, that is the expected end (RFC 6455 7.1.1, 7.1.7); otherwise
      // the session was lost.
      if ((ws.sent_close && ws.received_close) || ws.ending) ws_end(op);
      else if (rc != CURLE_OK) finish(op, Status::Failed, FailureKind::Receive, rc, "receive failed");
      else
        finish(op, Status::Failed, FailureKind::Receive, CURLE_RECV_ERROR,
               ws.phase == WsState::Phase::Handshake ? "connection closed during the handshake"
                                                     : "connection closed without a Close frame");
      return;
    }
    op->note_activity();
    if (!ws_consume(op, std::string_view(buffer, got))) return;
  }
  // TLS may hold more decrypted bytes than socket readiness can show: continue on a later turn
  // instead of starving the other operations on the strand.
  asio::post(strand, [self = shared_from_this(), op] {
    if (op->finished) return;
    self->ws_read(op);
    if (!op->finished) self->ws_interest(op);
  });
}

bool TransportCore::ws_handshake(const std::shared_ptr<OperationState>& op, std::string_view& bytes) {
  auto& ws = *op->ws;
  const std::size_t before = ws.head.size();
  ws.head.append(bytes);
  op->response_seen = true;
  const auto end = ws.head.find("\r\n\r\n", before >= 3 ? before - 3 : 0);
  if (end == std::string::npos || end > op->max_head_bytes) {
    if (ws.head.size() > op->max_head_bytes) {
      finish(op, Status::Failed, FailureKind::ResponseTooLarge, CURLE_OK, "handshake response head too large");
      return false;
    }
    bytes = {};
    return true;
  }
  bytes.remove_prefix(end + 4 - before);
  ws.head.resize(end);
  op->note_response_started();
  const WsHandshakeResponse checked = ws_check_handshake(ws.head, ws.key);
  ws.http_status = checked.status;
  if (!checked.error.empty()) {
    finish(op, Status::Failed, FailureKind::Protocol, CURLE_OK, checked.error.c_str());
    return false;
  }
  op->head.status = checked.status;
  op->head.version = ResponseVersion::Http1_1;
  op->head.framing = BodyFraming::None;
  for (const auto& [name, value] : checked.headers) op->head.headers.push_back(Header{lower(name), value});
  op->head_delivered = true;
  std::string().swap(ws.head);
  ws.phase = WsState::Phase::Open;
  try {
    if (ws.cb.on_open) ws.cb.on_open(op->head);
  } catch (...) {
    finish(op, Status::Failed, FailureKind::CallbackError, CURLE_OK, "a session callback threw");
    return false;
  }
  for (auto& [opcode, payload] : ws.pending) ws.queue(opcode, payload);
  ws.pending.clear();
  return true;
}

bool TransportCore::ws_consume(const std::shared_ptr<OperationState>& op, std::string_view bytes) {
  auto& ws = *op->ws;
  if (ws.phase == WsState::Phase::Handshake && !ws_handshake(op, bytes)) return false;
  // After the server's Close, or a protocol failure, bytes are ignored until the server closes TCP.
  if (!bytes.empty() && !ws.received_close && !ws.decoder.stopped()) {
    ws.decoder.feed(bytes, [&](const WsEvent& event) {
      try {
        switch (event.kind) {
          case WsEvent::Kind::Text:
          case WsEvent::Kind::Binary:
            if (ws.cb.on_message)
              ws.cb.on_message(event.kind == WsEvent::Kind::Text ? WsMessageKind::Text : WsMessageKind::Binary,
                               event.payload);
            break;
          case WsEvent::Kind::Ping:
            if (!ws.sent_close) ws.queue(WsOpcode::Pong, event.payload);  // RFC 6455 5.5.2
            break;
          case WsEvent::Kind::Pong:
            break;
          case WsEvent::Kind::Close:
            ws.received_close = true;
            ws.close_code = event.close_code;
            ws.close_reason = std::string(event.payload);
            if (!ws.sent_close) {
              ws.closed_by_peer = true;
              if (ws.cb.on_close) ws.cb.on_close(event.close_code, event.payload);
            }
            break;
        }
      } catch (...) {
        ws.callback_threw = true;
        return false;
      }
      return true;
    });
    if (op->finished) return false;
    if (ws.callback_threw) {
      finish(op, Status::Failed, FailureKind::CallbackError, CURLE_OK, "a session callback threw");
      return false;
    }
    std::optional<std::uint16_t> close;
    if (ws.received_close) {
      // Answer the server's Close with its code, or without one when it sent none (RFC 6455 5.5.1).
      if (!ws.ending) ws.ending = WsState::Ending{Status::Completed, FailureKind::None, ""};
      close = ws.close_code == ws_close::NoStatus ? std::uint16_t{0} : ws.close_code;
    } else if (const WsError error = ws.decoder.error(); error != WsError::None) {
      ws.ending = WsState::Ending{Status::Failed,
                                  error == WsError::TooBig ? FailureKind::ResponseTooLarge : FailureKind::Protocol,
                                  error == WsError::InvalidUtf8 ? "the server sent invalid UTF-8"
                                  : error == WsError::TooBig    ? "a server message exceeds the limit"
                                                                : "the server violated the WebSocket protocol"};
      close = ws.decoder.close_code_for_error();
    }
    if (close) {
      // Through the strand, so that what the callbacks above sent is written before the Close.
      asio::post(strand, [self = shared_from_this(), op, code = *close] {
        if (op->finished) return;
        self->ws_send_close(op, code, {});
        self->ws_flush(op);
        if (!op->finished) self->ws_interest(op);
      });
    }
  }
  ws_flush(op);
  return !op->finished;
}

void TransportCore::ws_send(const std::shared_ptr<OperationState>& op, WsOpcode opcode, std::string payload) {
  if (op->finished) return;
  auto& ws = *op->ws;
  if (ws.sent_close) return;
  if (ws.phase != WsState::Phase::Open) {
    ws.pending.emplace_back(opcode, std::move(payload));
    return;
  }
  ws.queue(opcode, payload);
  ws_flush(op);
  if (!op->finished) ws_interest(op);
}

void TransportCore::ws_close(const std::shared_ptr<OperationState>& op, std::uint16_t code, std::string reason) {
  if (op->finished) return;
  auto& ws = *op->ws;
  if (ws.phase != WsState::Phase::Open && ws.phase != WsState::Phase::Closing) {
    finish(op, Status::Cancelled, FailureKind::None, CURLE_OK, "closed before the handshake completed");
    return;
  }
  ws_send_close(op, code, reason);  // nothing when a Close was already sent
  ws_flush(op);
  if (!op->finished) ws_interest(op);
}

// Queues the client's Close frame (code 0: an empty payload) and bounds the wait for the server's
// Close and, after it, for the server's TCP close.
void TransportCore::ws_send_close(const std::shared_ptr<OperationState>& op, std::uint16_t code,
                                  std::string_view reason) {
  auto& ws = *op->ws;
  if (ws.sent_close) return;
  ws.sent_close = true;
  ws.accepting.store(false, std::memory_order_relaxed);
  ws.phase = WsState::Phase::Closing;
  if (!ws.received_close) {
    ws.close_code = code;
    ws.close_reason = std::string(reason);
  }
  ws.queue(WsOpcode::Close, code == 0 ? std::string() : ws_close_payload(code, reason));
  ws.close_timer.emplace(ctx);
  ws.close_timer->expires_after(ws.req.close_timeout);
  ws.close_timer->async_wait(asio::bind_executor(strand, [self = shared_from_this(), op](const asio::error_code& ec) {
    if (ec || op->finished) return;
    const auto& state = *op->ws;
    if (state.received_close || state.ending) self->ws_end(op);
    else
      self->finish(op, Status::Failed, FailureKind::Receive, CURLE_OPERATION_TIMEDOUT,
                   "the server did not answer the Close frame");
  }));
}

void TransportCore::ws_end(const std::shared_ptr<OperationState>& op) {
  const auto& ws = *op->ws;
  if (ws.ending) finish(op, ws.ending->status, ws.ending->failure, CURLE_OK, ws.ending->detail.c_str());
  else finish(op, Status::Completed, FailureKind::None, CURLE_OK, "");
}

// Called by finish() before the easy handle, and with it the connection, is released.
void TransportCore::ws_release(OperationState& op) {
  auto& ws = *op.ws;
  ws.accepting.store(false, std::memory_order_relaxed);
  if (ws.close_timer) {
    ws.close_timer->cancel();
    ws.close_timer.reset();
  }
  if (ws.fd == CURL_SOCKET_BAD) return;
  const auto it = socks.find(ws.fd);
  if (it != socks.end() && it->second->ws_owner == &op) {
    it->second->ws_owner = nullptr;
    it->second->removed = true;
    it->second->release_quiet();
    socks.erase(it);
  }
  ws.fd = CURL_SOCKET_BAD;
}

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------
Operation::Operation(Operation&& other) noexcept : state_(std::move(other.state_)) {}

Operation& Operation::operator=(Operation&& other) noexcept {
  if (this != &other) {
    if (state_) cancel();
    state_ = std::move(other.state_);
  }
  return *this;
}

Operation::~Operation() {
  if (state_) cancel();
}

void Operation::cancel() noexcept {
  if (!state_) return;
  if (state_->finishing.load(std::memory_order_relaxed)) return;
  auto core = state_->owner.lock();
  if (!core) return;  // transport gone: shutdown already delivered the outcome
  try {
    // Weak capture: a handler parked in the io_context after shutdown must not own the core that
    // owns the io_context.
    asio::post(core->strand, [weak = state_->owner, st = state_] {
      if (auto self = weak.lock()) self->finish(st, Status::Cancelled, FailureKind::None, CURLE_OK, "cancelled");
    });
  } catch (...) {
  }
}

void Operation::resume() noexcept {
  if (!state_) return;
  if (state_->finishing.load(std::memory_order_relaxed)) return;
  auto core = state_->owner.lock();
  if (!core) return;
  try {
    asio::post(core->strand, [weak = state_->owner, st = state_] {
      if (auto self = weak.lock()) self->resume(st);
    });
  } catch (...) {
  }
}

void Operation::detach() noexcept { state_.reset(); }

Result Operation::join() {
  const std::shared_ptr<OperationState> state = state_;  // never read state_ again: it may be reset concurrently
  if (!state) {
    Result r;
    r.failure = FailureKind::Misuse;
    r.detail = "join() on an empty handle";
    return r;
  }
  if (on_io_thread()) {
    Result r;
    r.failure = FailureKind::Misuse;
    r.detail = "join() called from an I/O thread";
    return r;
  }
  std::unique_lock lock(state->mu);
  state->cv.wait(lock, [&state] { return state->done; });
  return state->result;
}

Transport::Transport(TransportOptions options) : core_(std::make_shared<TransportCore>(std::move(options))) {
  core_->init();
}

Transport::~Transport() {
  if (!core_) return;
  if (on_io_thread()) {
    // Destroyed from inside an operation callback (or a callback's captured state): joining an I/O
    // thread from an I/O thread would deadlock. Hand the teardown to a detached reaper that owns the
    // core until the threads are joined; the destructor returns at once, and the strand runs the
    // cleanup after the running callback has returned.
    std::thread([core = std::move(core_)] { core->shutdown(); }).detach();
    return;
  }
  core_->shutdown();
}

Operation Transport::start(HttpRequest request, Callbacks callbacks) {
  auto op = std::make_shared<OperationState>(core_, std::move(request), std::move(callbacks), core_->opts);
  op->core = core_.get();
  op->setup();
  asio::post(core_->strand, [core = core_, op] { core->begin(op); });
  return Operation(std::move(op));
}

RuntimeInfo Transport::runtime_info() const {
  const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
  RuntimeInfo out;
  out.curl_version = info->version ? info->version : "";
  out.ssl_backend = info->ssl_version ? info->ssl_version : "";
  out.http2 = (info->features & CURL_VERSION_HTTP2) != 0;
  out.http3 = (info->features & CURL_VERSION_HTTP3) != 0;
  out.async_dns = (info->features & CURL_VERSION_ASYNCHDNS) != 0;
  return out;
}

unsigned Transport::io_threads() const noexcept { return static_cast<unsigned>(core_->threads.size()); }

struct WsSessionState {
  std::shared_ptr<OperationState> op;
};

WsSession::WsSession(WsSession&& other) noexcept : state_(std::move(other.state_)) {}

WsSession& WsSession::operator=(WsSession&& other) noexcept {
  if (this != &other) {
    if (state_) cancel();
    state_ = std::move(other.state_);
  }
  return *this;
}

WsSession::~WsSession() {
  if (state_) cancel();
}

bool WsSession::send(WsOpcode opcode, std::string message) {
  if (!state_) return false;
  const auto& op = state_->op;
  if (op->finishing.load(std::memory_order_relaxed) || !op->ws->accepting.load(std::memory_order_relaxed))
    return false;
  auto core = op->owner.lock();
  if (!core) return false;
  try {
    asio::post(core->strand, [weak = op->owner, op, opcode, message = std::move(message)]() mutable {
      if (auto self = weak.lock()) self->ws_send(op, opcode, std::move(message));
    });
  } catch (...) {
    return false;
  }
  return true;
}

bool WsSession::send_text(std::string message) {
  return ws_utf8_valid(message) && send(WsOpcode::Text, std::move(message));
}

bool WsSession::send_binary(std::string message) { return send(WsOpcode::Binary, std::move(message)); }

void WsSession::close(std::uint16_t code, std::string reason) {
  if (!state_) return;
  const auto& op = state_->op;
  if (op->finishing.load(std::memory_order_relaxed)) return;
  op->ws->accepting.store(false, std::memory_order_relaxed);
  auto core = op->owner.lock();
  if (!core) return;
  if (!ws_close_code_valid(code)) code = ws_close::Normal;
  if (reason.size() > 123 || !ws_utf8_valid(reason)) reason.clear();
  try {
    asio::post(core->strand, [weak = op->owner, op, code, reason = std::move(reason)]() mutable {
      if (auto self = weak.lock()) self->ws_close(op, code, std::move(reason));
    });
  } catch (...) {
  }
}

void WsSession::cancel() noexcept {
  if (!state_) return;
  const auto& op = state_->op;
  if (op->finishing.load(std::memory_order_relaxed)) return;
  auto core = op->owner.lock();
  if (!core) return;  // transport gone: shutdown already delivered the outcome
  try {
    asio::post(core->strand, [weak = op->owner, op] {
      if (auto self = weak.lock()) self->finish(op, Status::Cancelled, FailureKind::None, CURLE_OK, "cancelled");
    });
  } catch (...) {
  }
}

void WsSession::detach() noexcept { state_.reset(); }

WsResult WsSession::join() {
  const std::shared_ptr<WsSessionState> state = state_;
  WsResult misuse;
  misuse.transport.failure = FailureKind::Misuse;
  if (!state) {
    misuse.transport.detail = "join() on an empty handle";
    return misuse;
  }
  if (on_io_thread()) {
    misuse.transport.detail = "join() called from an I/O thread";
    return misuse;
  }
  OperationState& op = *state->op;
  std::unique_lock lock(op.mu);
  op.cv.wait(lock, [&op] { return op.done; });
  return ws_result(*op.ws, op.result);
}

WsSession open_websocket(Transport& transport, WsRequest request, WsCallbacks callbacks) {
  const std::shared_ptr<TransportCore>& core = transport.core_;
  HttpRequest http;
  http.method = "GET";
  http.deadline = request.deadline;
  http.http_version = HttpVersion::Http1_1;
  http.ca_file = request.ca_file;
  http.connect_timeout = request.connect_timeout;
  http.first_byte_timeout = request.handshake_timeout;
  http.idle_timeout = request.idle_timeout;
  std::string error;
  bool secure = false;
  const std::string_view url = request.url;
  if (url.size() > 6 && iequals(url.substr(0, 6), "wss://")) {
    http.url = "https://" + std::string(url.substr(6));
    secure = true;
  } else if (url.size() > 5 && iequals(url.substr(0, 5), "ws://")) {
    http.url = "http://" + std::string(url.substr(5));
  } else {
    error = "a WebSocket URL must use ws or wss";
  }
  for (const Header& h : request.headers)
    if (h.name.empty() || h.name.find(':') != std::string::npos || !header_line_safe(h.name) ||
        !header_line_safe(h.value))
      error = "invalid handshake header";

  auto op = std::make_shared<OperationState>(core, std::move(http), Callbacks{}, core->opts);
  op->ws = std::make_unique<WsState>(std::move(request), std::move(callbacks));
  op->ws->secure = secure;
  // The operation's single outcome becomes the session's; the raw pointer cannot dangle because
  // finish() holds the operation while it calls on_done.
  op->cb.on_done = [raw = op.get()](const Result& r) {
    const WsState& ws = *raw->ws;
    if (ws.cb.on_done) ws.cb.on_done(ws_result(ws, r));
  };
  op->core = core.get();
  op->setup();
  if (!error.empty()) {
    op->setup_error = error;
    op->setup_failure = FailureKind::Misuse;
    op->setup_code = CURLE_URL_MALFORMAT;
  }
  asio::post(core->strand, [core, op] { core->begin(op); });
  return WsSession(std::make_shared<WsSessionState>(WsSessionState{op}));
}


}  // namespace sp::transport
