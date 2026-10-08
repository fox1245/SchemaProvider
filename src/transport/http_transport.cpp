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
#include <cctype>
#include <condition_variable>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
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
// can never fire first (the idle bound's expiry only ever moves later). The comparison is done in
// milliseconds so an absurdly large bound cannot overflow the clock's tick type.
std::chrono::milliseconds effective_bound(std::optional<std::chrono::milliseconds> request_value,
                                          std::chrono::milliseconds fallback,
                                          std::chrono::steady_clock::time_point from,
                                          std::chrono::steady_clock::time_point deadline) {
  const std::chrono::milliseconds bound = request_value.value_or(fallback);
  if (bound.count() <= 0 || deadline <= from) return std::chrono::milliseconds{0};
  const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - from);
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
  asio::steady_timer timer;
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
  void apply_addrs_and_add(const std::shared_ptr<OperationState>& op, const std::vector<std::string>& addrs);
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
  if (!sends_body_fields && !req.body.empty()) {
    setup_error = "GET and HEAD requests cannot carry a body";
    return;
  }
  if (is_head) {
    set(CURLOPT_NOBODY, 1L);
  } else if (sends_body_fields) {
    // POSTFIELDS installs the owned body and its length for every body-carrying method; for
    // methods other than POST, CUSTOMREQUEST then only replaces the method token.
    set(CURLOPT_POST, 1L);
    set(CURLOPT_POSTFIELDS, req.body.data());
    set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
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

  if (op->needs_resolve) lookup(op);
  else add_to_multi(op);
}

void TransportCore::add_to_multi(const std::shared_ptr<OperationState>& op) {
  if (op->finished || stopped || !multi) return;  // shutdown finishes every active operation
  if (curl_multi_add_handle(multi, op->easy) != CURLM_OK) {
    finish(op, Status::Failed, FailureKind::Other, CURLE_OK, "curl_multi_add_handle failed");
    return;
  }
  op->in_multi = true;
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
                                        const std::vector<std::string>& addrs) {
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

  if (op->easy) {
    long http_status = 0;
    curl_easy_getinfo(op->easy, CURLINFO_RESPONSE_CODE, &http_status);
    r.http_status = static_cast<int>(http_status);
    curl_off_t t_name = 0, t_conn = 0, t_pre = 0, uploaded = 0;
    curl_easy_getinfo(op->easy, CURLINFO_NAMELOOKUP_TIME_T, &t_name);
    curl_easy_getinfo(op->easy, CURLINFO_CONNECT_TIME_T, &t_conn);
    curl_easy_getinfo(op->easy, CURLINFO_PRETRANSFER_TIME_T, &t_pre);
    curl_easy_getinfo(op->easy, CURLINFO_SIZE_UPLOAD_T, &uploaded);
    r.attempt.request_body_bytes = uploaded;
    r.attempt.connection_reused = op->first_write_reused;
    // Stage evidence. STARTTRANSFER_TIME is set even when the peer closes without a single byte and
    // the header parser's state resets after a 1xx block, so the response start is the monotonic
    // `response_seen`. The prerequisite callback fires before any request byte is written, so it is
    // a latch for "the request may have left"; the pretransfer timing mark is only a second witness.
    if (op->response_seen) r.attempt.reached = Stage::ResponseStarted;
    else if (op->prereq_count > 0 || t_pre > 0) r.attempt.reached = Stage::RequestStarted;
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
      break;
    case CURLE_WRITE_ERROR:
      if (op->callback_threw) kind = FailureKind::CallbackError;
      else if (op->head_overflow) kind = FailureKind::ResponseTooLarge;
      else kind = FailureKind::Receive;
      break;
    case CURLE_OUT_OF_MEMORY:  // libcurl's own 100 KB per-header-line limit reports this code
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
    case CURLE_HTTP2:
    case CURLE_UNSUPPORTED_PROTOCOL: kind = FailureKind::Protocol; break;
    default: break;
  }
  finish(op, Status::Failed, kind, code, curl_easy_strerror(code));
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
  // Compare milliseconds before adding: an admitted bound may be enormous, and activity can move
  // its expiry beyond the clock's representable range even when started + bound was safe.
  if (op->idle_bound >= std::chrono::duration_cast<std::chrono::milliseconds>(op->req.deadline - op->last_activity))
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
  timer.cancel();
  if (ms < 0) return;
  if (ms == 0) {  // libcurl forbids calling socket_action from inside this callback
    asio::post(strand, [self = shared_from_this()] { self->on_timeout(); });
    return;
  }
  timer.expires_after(std::chrono::milliseconds(ms));
  timer.async_wait(asio::bind_executor(strand, [self = shared_from_this()](const asio::error_code& ec) {
    if (!ec) self->on_timeout();
  }));
}

void TransportCore::on_socket(CURL* easy, curl_socket_t s, int what) {
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
                         if (self->read_suppressed(w->fd)) return;  // paused: leave the data in the kernel
                         self->on_socket_event(w, ec ? CURL_CSELECT_ERR : CURL_CSELECT_IN);
                       }));
  }
  if (w->want_write && !w->armed_write) {
    w->armed_write = true;
    w->desc.async_wait(asio::posix::stream_descriptor::wait_write,
                       asio::bind_executor(strand, [self = shared_from_this(), w](const asio::error_code& ec) {
                         w->armed_write = false;
                         if (w->removed || ec == asio::error::operation_aborted) return;
                         self->on_socket_event(w, ec ? CURL_CSELECT_ERR : CURL_CSELECT_OUT);
                       }));
  }
#endif
}

void TransportCore::on_socket_event(const std::shared_ptr<SockWatch>& w, int flags) {
  if (!multi) return;
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
    timer.cancel();
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


}  // namespace sp::transport
