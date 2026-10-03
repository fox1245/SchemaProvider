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
// Linux/POSIX only in the current implementation: sockets owned by libcurl are watched through
// asio::posix::stream_descriptor and released (never closed) when libcurl removes them.
#include "transport/http_transport.h"

#include <asio/bind_executor.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/posix/stream_descriptor.hpp>
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

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <deque>

namespace sp::transport {

namespace {

thread_local bool tl_io_thread = false;

void ensure_curl_global_init() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
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
  ResolverPool(unsigned thread_count, ResolveFn fn) : n(thread_count), resolve(std::move(fn)) {}

  void submit(std::function<void()> job) {
    std::lock_guard lock(mu);
    if (stopping) return;
    if (!started) {
      started = true;
      for (unsigned i = 0; i < n; ++i) std::thread([self = shared_from_this()] { self->run(); }).detach();
    }
    jobs.push_back(std::move(job));
    cv.notify_one();
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

  const unsigned n;
  ResolveFn resolve;
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  bool started = false;
  bool stopping = false;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Operation state
// ---------------------------------------------------------------------------------------------
struct OperationState {
  OperationState(std::weak_ptr<TransportCore> core, HttpRequest request, Callbacks callbacks,
                 std::size_t head_limit)
      : owner(std::move(core)),
        req(std::move(request)),
        cb(std::move(callbacks)),
        max_head_bytes(head_limit),
        started(std::chrono::steady_clock::now()) {}

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

  static std::size_t header_cb(char* buf, std::size_t size, std::size_t n, void* ud);
  static std::size_t write_cb(char* ptr, std::size_t size, std::size_t n, void* ud);
  static int prereq_cb(void* ud, char* primary_ip, char* local_ip, int primary_port, int local_port);
};

// ---------------------------------------------------------------------------------------------
// Core: owns the io_context, the strand, the CURLM and the I/O threads
// ---------------------------------------------------------------------------------------------
struct TransportCore : std::enable_shared_from_this<TransportCore> {
  using Strand = asio::strand<asio::io_context::executor_type>;

  struct SockWatch {
    SockWatch(asio::io_context& ctx, curl_socket_t s) : desc(ctx, s), fd(s) {}
    ~SockWatch() { release_quiet(); }
    void release_quiet() noexcept {
      try {
        if (desc.is_open()) (void)desc.release();  // never close: libcurl owns the descriptor
      } catch (...) {
      }
    }
    asio::posix::stream_descriptor desc;
    curl_socket_t fd;
    bool want_read = false;
    bool want_write = false;
    bool armed_read = false;
    bool armed_write = false;
    bool removed = false;
  };

  explicit TransportCore(TransportOptions options)
      : opts(std::move(options)),
        strand(asio::make_strand(ctx)),
        guard(asio::make_work_guard(ctx)),
        timer(ctx) {}

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
  void set_timer(long ms);
  void on_socket(CURL* easy, curl_socket_t s, int what);
  void arm(const std::shared_ptr<SockWatch>& w);
  void on_socket_event(const std::shared_ptr<SockWatch>& w, int flags);
  void note_pause(OperationState& op);
  void note_resume(OperationState& op);
  bool read_suppressed(curl_socket_t s) const;
  void rearm_socket(curl_socket_t s);
  void shutdown();

  static int socket_cb(CURL* easy, curl_socket_t s, int what, void* userp, void*) {
    static_cast<TransportCore*>(userp)->on_socket(easy, s, what);
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
  set(CURLOPT_NOPROGRESS, 1L);
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

int OperationState::prereq_cb(void* ud, char*, char*, int, int) {
  auto* op = static_cast<OperationState*>(ud);
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
  for (unsigned i = 0; i < n; ++i) {
    threads.emplace_back([this] {
      tl_io_thread = true;
      ctx.run();
    });
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
  const std::string key = lower(op->host);
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
  if (pending.in_flight) return;  // single-flight: a lookup for this host is already running
  pending.in_flight = true;
  std::weak_ptr<TransportCore> weak = shared_from_this();
  auto pool = resolver;
  pool->submit([weak, pool, key] {
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
  Callbacks released = std::move(op->cb);
  op->cb = Callbacks{};
  {
    std::lock_guard lock(op->mu);
    op->done = true;
  }
  op->cv.notify_all();
}

void TransportCore::resume(const std::shared_ptr<OperationState>& op) {
  if (op->finished || !op->paused || !multi) return;
  op->paused = false;
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
}

void TransportCore::on_socket_event(const std::shared_ptr<SockWatch>& w, int flags) {
  if (!multi) return;
  int running = 0;
  curl_multi_socket_action(multi, w->fd, flags, &running);
  check_multi_info();
  if (!w->removed) arm(w);  // one-shot waits: re-arm whatever libcurl still wants
}

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
  if (tl_io_thread) {
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
  if (tl_io_thread) {
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
  auto op = std::make_shared<OperationState>(core_, std::move(request), std::move(callbacks),
                                             core_->opts.max_head_bytes);
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

bool on_io_thread() noexcept { return tl_io_thread; }

}  // namespace sp::transport
