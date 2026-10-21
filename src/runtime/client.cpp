#include "runtime/client.h"
#include "codecs/images.h"
#include "codecs/decisions.h"
#include "codecs/endpoint.h"
#include "runtime/request_facts.h"
#include "runtime/testing.h"
#include "core/native.h"
#include "codecs/messages.h"
#include "codecs/responses.h"
#include "codecs/gemini.h"
#include "codecs/interactions.h"
#include "codecs/reason.h"
#include "crypto/crypto.h"
#include "sp/build_config.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#if SP_BUILD_THREADS
#include <thread>
#endif
#include <type_traits>
#include <unordered_map>
#include <iterator>
#include <utility>

namespace sp::runtime::detail {
namespace {
bool prior_attempt_proves_no_usage(const Failure& failed) noexcept {
  const auto& attempt = failed.error.attempt;
  const auto& usage = failed.partial.usage;
  const auto& cost = usage.provider_cost;
  return failed.error.retry_safety == RetrySafety::NotSent &&
      !attempt.request_may_have_left && attempt.request_body_bytes == 0 &&
      !attempt.response_head_seen && attempt.transport_internal_resends == 0 &&
      failed.partial.messages.empty() && !failed.partial.stop && !failed.partial.wire_envelope &&
      failed.partial.raw_events.empty() && usage.stage == UsageStage::Missing &&
      usage.quality == UsageQuality::Consistent && !usage.input_total && !usage.output_total &&
      !usage.total && !usage.provider_reported_total && !usage.input_uncached &&
      !usage.cache_read && !usage.cache_write && !usage.reasoning && usage.extra.empty() && usage.conflicts.empty() &&
      !cost.total && !cost.upstream_total && !cost.upstream_input && !cost.upstream_output &&
      !cost.is_byok && cost.byok_status == CostStatus::Missing && cost.source == CostSource::None &&
      cost.quality == UsageQuality::Consistent &&
      std::all_of(cost.status.begin(), cost.status.end(), [](CostStatus status) { return status == CostStatus::Missing; });
}
thread_local unsigned runtime_depth = 0;
thread_local const void* pool_thread = nullptr;
struct RuntimeScope {
  RuntimeScope() { ++runtime_depth; }
  ~RuntimeScope() { --runtime_depth; }
};

Error error_for(ErrorKind kind, std::string_view reason = {}) {
  Error error{kind, std::string(safe_message(kind))};
  error.reason = reason;
  return error;
}
// The SSE framer's failure, as the reason the call reports.
std::string_view framer_reason(transport::SseError error) noexcept {
  switch (error) {
    case transport::SseError::ResourceLimit: return "sse.limit.exceeded";
    case transport::SseError::InvalidUtf8: return "sse.utf8.invalid";
    case transport::SseError::Misuse: return "sse.framer.misuse";
    case transport::SseError::None: break;
  }
  return {};
}
Result misuse_result() {
  static const Result result = std::make_shared<const Outcome>(Failure{error_for(ErrorKind::Misuse), {}});
  return result;
}
// The refusal of a blocking call, shared by every caller. Nothing was sent for the call.
Result blocked_result(bool host_pumped) {
  const auto make = [](std::string_view reason) {
    auto error = error_for(ErrorKind::Misuse, reason);
    error.retry_safety = RetrySafety::NotSent;
    return std::make_shared<const Outcome>(Failure{std::move(error), {}});
  };
  static const Result pumped = make(blocking_host_pumped_reason);
  static const Result in_thread = make(blocking_in_thread_reason);
  return host_pumped ? pumped : in_thread;
}

#if SP_BUILD_THREADS
// Workers own only Core, not the executor containing their thread handles. Thus a
// shutdown on a worker can detach those existing handles without a reaper thread.
class PoolExecutor final : public Executor {
  struct Core {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Task> ready;
    using Key = std::pair<SteadyTime, Timer>;
    using Timers = std::map<Key, Task>;
    Timers timers;
    std::unordered_map<Timer, std::map<Key, Task>::iterator> timer_index;
    Timer next_timer = 1;
    std::size_t sleepers = 0;
    bool stopping = false;
  };
  std::shared_ptr<Core> core_ = std::make_shared<Core>();
  std::mutex shutdown_mutex_;
  std::vector<std::thread> threads_;

  static void run(const std::shared_ptr<Core>& core) {
    pool_thread = core.get();
    for (;;) {
      Task task;
      {
        std::unique_lock lock(core->mutex);
        for (;;) {
          if (!core->timers.empty() &&
              core->timers.begin()->first.first <= std::chrono::steady_clock::now()) {
            auto timer = core->timers.begin();
            core->timer_index.erase(timer->first.second);
            task = std::move(timer->second);
            core->timers.erase(timer);
            break;
          }
          if (!core->ready.empty()) {
            task = std::move(core->ready.front());
            core->ready.pop_front();
            break;
          }
          if (core->stopping) { pool_thread = nullptr; return; }
          ++core->sleepers;
          if (core->timers.empty()) core->wake.wait(lock);
          else {
            const auto wake_at = core->timers.begin()->first.first;
            core->wake.wait_until(lock, wake_at);
          }
          --core->sleepers;
        }
      }
      try { task(); } catch (...) { /* An operation boundary owns error delivery. */ }
    }
  }
 public:
  explicit PoolExecutor(std::size_t count) {
    threads_.reserve(count);
    try {
      for (std::size_t i = 0; i < count; ++i)
        threads_.emplace_back([core = core_] { run(core); });
    } catch (...) { shutdown(); throw; }
  }
  ~PoolExecutor() override { shutdown(); }
  void post(Task task) override {
    bool wake;
    {
      std::lock_guard lock(core_->mutex);
      if (core_->stopping) return;
      core_->ready.push_back(std::move(task));
      wake = core_->sleepers != 0;
    }
    // Wait admission and queue publication share the mutex: an active worker
    // cannot miss this task when it next checks the queue before sleeping.
    if (wake) core_->wake.notify_one();
  }
  Timer schedule(SteadyTime at, Task task) override {
    Timer id;
    bool wake = false;
    Core::Timers::node_type rollback;
    {
      std::lock_guard lock(core_->mutex);
      if (core_->stopping) return 0;
      const bool earlier = core_->timers.empty() || at < core_->timers.begin()->first.first;
      id = core_->next_timer++;
      auto entry = core_->timers.emplace(Core::Key{at, id}, std::move(task)).first;
      try { core_->timer_index.emplace(id, entry); }
      catch (...) { rollback = core_->timers.extract(entry); throw; }
      if (earlier) wake = core_->sleepers != 0;
    }
    // Broadcast even when only one sleeper was observed: after unlocking,
    // another worker may enter wait and steal notify_one from an old sleeper
    // whose timeout predates this update (or whose wait was indefinite).
    if (wake) core_->wake.notify_all();
    return id;
  }
  void cancel(Timer id) noexcept override {
    // Destroy cancelled captures outside the queue lock: their destructors may
    // release an operation or executor. No stale timer heap retains requests.
    Task released;
    bool wake = false;
    {
      std::lock_guard lock(core_->mutex);
      auto found = core_->timer_index.find(id);
      if (found == core_->timer_index.end()) return;
      const bool first = found->second == core_->timers.begin();
      const auto at = found->second->first.first;
      released = std::move(found->second->second);
      core_->timers.erase(found->second);
      core_->timer_index.erase(found);
      if (first && (core_->timers.empty() || core_->timers.begin()->first.first != at))
        wake = core_->sleepers != 0;
    }
    // The same unlocked sleeper-count race applies when recomputing the bound
    // after earliest removal. Do not choose a single arbitrary waiter.
    if (wake) core_->wake.notify_all();
  }
  SteadyTime now() const noexcept override { return std::chrono::steady_clock::now(); }
  WallTime wall_now() const noexcept override { return std::chrono::system_clock::now(); }
  bool in_thread() const noexcept override { return pool_thread != nullptr || runtime_depth != 0; }
  void shutdown() noexcept override {
    std::vector<std::thread> threads;
    std::map<Core::Key, Task> released;
    {
      std::lock_guard shutdown_lock(shutdown_mutex_);
      {
        std::lock_guard lock(core_->mutex);
        core_->stopping = true;
        core_->timer_index.clear();
        released.swap(core_->timers);
      }
      threads.swap(threads_);
    }
    core_->wake.notify_all();
    for (auto& thread : threads) {
      if (pool_thread == core_.get()) thread.detach();
      else thread.join();
    }
  }
};

#endif  // SP_BUILD_THREADS

class ScopedTimer {
 public:
  ScopedTimer() = default;
  ~ScopedTimer() { reset(); }
  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;
  // The OperationState's executor owner outlives both scoped timers.
  void arm(Executor& owner, SteadyTime at, Executor::Task task) {
    reset();
    owner_ = &owner;
    id_ = owner.schedule(at, std::move(task));
  }
  void reset() noexcept {
    if (id_) owner_->cancel(std::exchange(id_, 0));
    owner_ = nullptr;
  }
 private:
  Executor* owner_ = nullptr;
  Executor::Timer id_ = 0;
};

bool equal_ascii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    if (lower(static_cast<unsigned char>(a[i])) != lower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}
void validate(const descriptor::ValidatedDescriptor& descriptor, const Options& options) {
  if (!options.policy) throw descriptor::ConfigError{"/policy", "admitted runtime snapshot", 1, "runtime options rejected"};
  const auto& ceiling = options.policy->admission();
  auto reject = [](std::string pointer) {
    throw descriptor::ConfigError{std::move(pointer), "value within admitted runtime policy", 1, "runtime options rejected"};
  };
  if (options.workers == 0 || options.workers > ceiling.workers) reject("/workers");
  if (options.transport.io_threads == 0 || options.transport.io_threads > ceiling.io_threads) reject("/io_threads");
  if (options.transport.resolver_threads == 0 || options.transport.resolver_threads > ceiling.resolver_threads) reject("/resolver_threads");
  if (options.transport.max_host_connections < 0 || static_cast<std::uint64_t>(options.transport.max_host_connections) > ceiling.max_host_connections) reject("/max_host_connections");
  if (options.transport.connection_cache_size > ceiling.max_operations) reject("/connection_cache_size");
  if (options.transport.max_head_bytes == 0 || options.transport.max_head_bytes > ceiling.max_head_bytes) reject("/max_head_bytes");
  if (options.transport.dns_ttl.count() < 0 || static_cast<std::uint64_t>(options.transport.dns_ttl.count()) > ceiling.dns_ttl_seconds) reject("/dns_ttl_seconds");
  // The stall bounds are durations no longer than the longest admissible operation deadline, so
  // they share that ceiling; zero disables a bound.
  if (!detail::valid_stall_bound(options.transport.connect_timeout, ceiling.default_timeout_ms)) reject("/connect_timeout_ms");
  if (!detail::valid_stall_bound(options.transport.first_byte_timeout, ceiling.default_timeout_ms)) reject("/first_byte_timeout_ms");
  if (!detail::valid_stall_bound(options.transport.idle_timeout, ceiling.default_timeout_ms)) reject("/idle_timeout_ms");
  if (options.default_timeout.count() <= 0 || static_cast<std::uint64_t>(options.default_timeout.count()) > ceiling.default_timeout_ms) reject("/default_timeout_ms");
  if (options.slow_callback_threshold.count() < 0 || static_cast<std::uint64_t>(options.slow_callback_threshold.count()) > ceiling.slow_callback_threshold_ms) reject("/slow_callback_threshold_ms");
  if (options.retry_tokens > ceiling.retry_tokens) reject("/retry_tokens");
  if (!std::isfinite(options.retry_tokens_per_second) || options.retry_tokens_per_second < 0 || options.retry_tokens_per_second > ceiling.retry_tokens_per_second) reject("/retry_tokens_per_second");
  if (options.limits.max_operations == 0 || options.limits.max_operations > ceiling.max_operations) reject("/max_operations");
  if (options.limits.queued_body_chunks == 0 || options.limits.queued_body_chunks > ceiling.queued_body_chunks) reject("/queued_body_chunks");
  if (options.limits.queued_body_bytes == 0 || options.limits.queued_body_bytes > ceiling.queued_body_bytes) reject("/queued_body_bytes");
  if (options.limits.max_response_bytes == 0 || options.limits.max_response_bytes > ceiling.max_response_bytes) reject("/max_response_bytes");
  if (options.limits.max_error_bytes == 0 || options.limits.max_error_bytes > ceiling.max_error_bytes) reject("/max_error_bytes");
  if (options.limits.semantic.max_parts == 0 || options.limits.semantic.max_parts > ceiling.semantic_max_parts) reject("/semantic_max_parts");
  if (options.limits.semantic.max_content_bytes == 0 || options.limits.semantic.max_content_bytes > ceiling.semantic_max_content_bytes) reject("/semantic_max_content_bytes");
  if (options.limits.semantic.max_tool_bytes == 0 || options.limits.semantic.max_tool_bytes > ceiling.semantic_max_tool_bytes) reject("/semantic_max_tool_bytes");
  if (options.limits.semantic.max_json_depth == 0 || options.limits.semantic.max_json_depth > ceiling.semantic_max_json_depth) reject("/semantic_max_json_depth");
  if (options.limits.sse.max_line_bytes == 0 || options.limits.sse.max_line_bytes > ceiling.sse_max_line_bytes) reject("/sse_max_line_bytes");
  if (options.limits.sse.max_event_bytes == 0 || options.limits.sse.max_event_bytes > ceiling.sse_max_event_bytes) reject("/sse_max_event_bytes");
  if (options.limits.sse.max_total_bytes == 0 || options.limits.sse.max_total_bytes > ceiling.sse_max_total_bytes) reject("/sse_max_total_bytes");
  if (options.workers > ceiling.total_threads || options.transport.io_threads > ceiling.total_threads - options.workers ||
      options.transport.resolver_threads > ceiling.total_threads - options.workers - options.transport.io_threads) reject("/total_threads");
  if (options.api_key.size() > ceiling.api_key_bytes) reject("/api_key");
  for (unsigned char byte : options.api_key) if (byte < 32 || byte == 127) reject("/api_key");
  if (!options.api_key.empty()) {
    for (const auto& header : descriptor.headers())
      if (equal_ascii(header.first, "authorization") || equal_ascii(header.first, "x-api-key")) reject("/api_key");
  }
}

struct CallbackAbort {};
struct StopObserved {};
}  // namespace

// The Failure a blocking call returns at once instead of waiting, or null when the caller may wait. A wait needs work
// that another thread finishes: a thread of the runtime, or the one the executor runs a task on, would wait for itself,
// and a host-pumped executor runs only when the host's own loop does, which the waiting caller is blocking.
Result blocking_refused(const Executor* executor) {
  if (executor && executor->host_pumped()) return blocked_result(true);
  if (runtime_depth || pool_thread || sp::transport::on_io_thread() || (executor && executor->in_thread())) return blocked_result(false);
  return nullptr;
}

struct ClientState : std::enable_shared_from_this<ClientState> {
  const descriptor::ValidatedDescriptor descriptor;
  const Options options;
  std::shared_ptr<Executor> executor;
  std::shared_ptr<AttemptTransport> transport;
  std::mutex mutex;
  std::condition_variable drained;
  std::unordered_map<OperationState*, std::shared_ptr<OperationState>> active;
  std::size_t prepared_operations = 0;
  TokenBucket bucket;
  std::function<double()> random01;
  bool stopping = false, asynchronous_shutdown = false, closing = false, closed = false;
  std::atomic<std::uint64_t> slow_callbacks{0}, callback_exceptions{0};

  ClientState(descriptor::ValidatedDescriptor d, Options o, std::shared_ptr<Executor> e,
              std::shared_ptr<AttemptTransport> t, std::function<double()> random)
      : descriptor(std::move(d)), options(std::move(o)), executor(std::move(e)), transport(std::move(t)),
        bucket(options.retry_tokens, options.retry_tokens_per_second, executor->now()), random01(std::move(random)) {
    if (!random01) {
      random01 = [engine = std::mt19937_64(std::random_device{}())]() mutable {
        return std::generate_canonical<double, 53>(engine);
      };
    }
  }
  bool in_thread() const noexcept {
    return runtime_depth || pool_thread || sp::transport::on_io_thread() || executor->in_thread();
  }
  void shutdown() noexcept;
  void release(OperationState*);
  void close_resources() noexcept;
};

struct OperationState : std::enable_shared_from_this<OperationState> {
  enum class Stage { Initial, InFlight, Backoff, Terminal };
  struct Head {
    transport::ResponseHead value;
    SteadyTime received;
    WallTime wall_received;
  };
  std::shared_ptr<ClientState> client;
  // Retained after terminal delivery solely for worker-misuse detection.
  std::shared_ptr<Executor> executor;
  RunOptions options;
  Callbacks callbacks;
  SteadyTime deadline;
  transport::HttpRequest request;
  std::shared_ptr<const NativeContext> native_context;
  enum class Family { Chat, Messages, Responses, Gemini, Interactions, Endpoint };
  Family family = Family::Chat;
  // A non-chat endpoint call (D6): which one, and whether the provider charges once it accepts it.
  std::string endpoint;
  bool charge_on_accept = false, read_only = false;
  // The endpoint fact's bound for a buffered response, used where it is above the client's; zero for none.
  std::uint64_t endpoint_response_bytes = 0;
  // What the decoder checks the answer against, and a value no journal may store; the prepared call shows neither.
  endpoints::Context codec_context;
  // A download written to a file as it arrives: the file, its byte limit, and the attempt's partial copy, which
  // is renamed to the file once the body is whole and the decoder accepted it, and removed when the call fails.
  // The file is never touched before then, so a failed download leaves what was there.
  std::string sink_path;
  std::uint64_t sink_limit = 0;
  std::FILE* sink_file = nullptr;
  std::uint64_t sink_bytes = 0;
  std::optional<crypto::Sha256> sink_hash;
  bool sink_whole = false;
  bool admitted = false;
  bool prepared = false;
  std::string prepared_model;
  std::optional<std::uint64_t> prepared_max_output_tokens;
  std::optional<std::uint64_t> prepared_model_invocation_limit;
  RequestFacts facts;
  CacheMarkReport prepared_cache_marks;

  mutable std::mutex mutex;
  std::condition_variable joined;
  bool ready = false, scheduled = false, finished = false, accepting = false;
  bool cancellation = false, deadline_fired = false, retry_fired = false, control_pending = false;
  bool head_received = false, done_received = false, paused = false;
  std::size_t paused_bytes = 0;
  std::uint64_t generation = 0;
  std::optional<Head> head_input;
  std::deque<std::string> body_input;
  std::optional<transport::Result> done_input;
  std::optional<ErrorKind> input_error;
  OperationStats statistics;
  Result result;
  struct StopCallback {
    std::weak_ptr<OperationState> operation;
    void operator()() const noexcept { if (auto state = operation.lock()) state->cancel(); }
  };
  std::optional<std::stop_callback<StopCallback>> stop_callback;
  // Fixed before activation; consumed by the first actor turn.
  std::optional<Error> initial_error;

  // Everything below is confined to the serialized actor.
  Stage stage = Stage::Initial;
  ScopedTimer deadline_timer, retry_timer;
  std::uint32_t attempts = 0;
  std::unique_ptr<Attempt> attempt;
  std::unique_ptr<Accumulator> accumulator;
  std::unique_ptr<chat::Codec> chat_codec;
  std::unique_ptr<messages::Codec> messages_codec;
  std::unique_ptr<responses::Codec> responses_codec;
  std::unique_ptr<gemini::Codec> gemini_codec;
  std::unique_ptr<interactions::Codec> interactions_codec;
  std::unique_ptr<endpoints::Codec> endpoint_codec;
  std::unique_ptr<transport::SseFramer> framer;
  std::optional<Head> response;
  ResponseInfo response_info;
  std::string buffered;
  std::shared_ptr<const json::Document> observed_document;
  std::size_t response_bytes = 0;
  bool semantic_output = false;
  std::optional<Error> stopping_error;
  std::optional<transport::Result> terminal_wire;
  std::optional<Outcome> pending_failure;
  bool prior_usage_unknown = false;
  std::vector<RawWire> prior_raw_events;

  OperationState(std::shared_ptr<ClientState> c, RunOptions o, Callbacks cb, SteadyTime end)
      : client(std::move(c)), executor(client ? client->executor : nullptr), options(std::move(o)),
        callbacks(std::move(cb)), deadline(end) {}
  ~OperationState() { if (sink_file) discard_sink(); }

  // What a response may total: the client's bound, or the endpoint fact's where that is higher (never above the
  // runtime policy's admission ceiling).
  std::size_t response_limit() const {
    const auto base = client->options.limits.max_response_bytes;
    if (endpoint_response_bytes <= base) return base;
    return static_cast<std::size_t>(std::min<std::uint64_t>(endpoint_response_bytes, client->options.policy->admission().max_response_bytes));
  }
  std::string sink_part() const { return sink_path + ".part"; }
  void discard_sink() {
    if (sink_file) { std::fclose(sink_file); sink_file = nullptr; }
    std::error_code ignored;
    if (!sink_path.empty()) std::filesystem::remove(sink_part(), ignored);
    sink_whole = false;
    sink_bytes = 0;
    sink_hash.reset();
  }
  void sink(std::string_view bytes) {
    if (bytes.size() > sink_limit - sink_bytes) { fail_semantic(ErrorKind::ResourceLimit, "runtime.sink.limit"); return; }
    if (!sink_file) {
      sink_file = std::fopen(sink_part().c_str(), "wb");
      if (!sink_file) { fail_semantic(ErrorKind::InvalidRequest, "runtime.sink.open_failed"); return; }
      sink_hash.emplace();
    }
    if (std::fwrite(bytes.data(), 1, bytes.size(), sink_file) != bytes.size()) { fail_semantic(ErrorKind::ResourceLimit, "runtime.sink.write_failed"); return; }
    sink_hash->update(bytes);
    sink_bytes += bytes.size();
  }
  // Closes the attempt's partial copy for the decoder to check; false when it could not be written whole.
  bool close_sink(endpoints::Sink& out) {
    const bool written = std::fflush(sink_file) == 0 && !std::ferror(sink_file);
    const bool closed = std::fclose(sink_file) == 0;
    sink_file = nullptr;
    if (!written || !closed) { discard_sink(); return false; }
    sink_whole = true;
    static constexpr char digits[] = "0123456789abcdef";
    out = {sink_path, sink_bytes, {}};
    for (const auto byte : sink_hash->finish()) { out.sha256 += digits[byte >> 4]; out.sha256 += digits[byte & 15]; }
    return true;
  }
  // Moves the checked copy to the file, replacing what was there; false when it could not.
  bool commit_sink() {
    std::error_code error;
    std::filesystem::rename(sink_part(), sink_path, error);
    if (error) { discard_sink(); return false; }
    sink_whole = false;
    return true;
  }

  bool mark_scheduled_locked() {
    if (!ready || scheduled || finished) return false;
    scheduled = true;
    return true;
  }
  void post_actor() {
    executor->post([self = shared_from_this()] { self->run(); });
  }
  void cancel() noexcept {
    bool post;
    {
      std::lock_guard lock(mutex);
      if (finished || cancellation) return;
      cancellation = true;
      control_pending = true;
      post = mark_scheduled_locked();
    }
    if (post) post_actor();
  }
  void timer(bool retry) {
    bool post;
    {
      std::lock_guard lock(mutex);
      if (finished) return;
      if (retry) retry_fired = true;
      else { deadline_fired = true; control_pending = true; }
      post = mark_scheduled_locked();
    }
    if (post) post_actor();
  }
  void activate() {
    if (!initial_error && options.stop_token.stop_possible()) {
      stop_callback.emplace(options.stop_token, StopCallback{weak_from_this()});
    }
    bool post;
    {
      std::lock_guard lock(mutex);
      ready = true;
      post = mark_scheduled_locked();
    }
    if (post) post_actor();
  }
  void receive_head(std::uint64_t epoch, const transport::ResponseHead& value) {
    RuntimeScope scope;
    const auto received = executor->now();
    const auto wall_received = executor->wall_now();
    bool post;
    {
      std::lock_guard lock(mutex);
      if (finished || !accepting || epoch != generation) return;
      std::size_t bytes = 0;
      bool oversized = false;
      const auto limit = client->options.transport.max_head_bytes;
      for (const auto& header : value.headers) {
        if (header.name.size() > limit - bytes) { oversized = true; break; }
        bytes += header.name.size();
        if (header.value.size() > limit - bytes) { oversized = true; break; }
        bytes += header.value.size();
        if (limit - bytes < 4) { oversized = true; break; }
        bytes += 4;
      }
      if (oversized) input_error = ErrorKind::ResourceLimit;
      else if (head_received) input_error = ErrorKind::ProtocolCorrupt;
      else {
        head_received = true;
        head_input = Head{value, received, wall_received};
      }
      post = mark_scheduled_locked();
    }
    if (post) post_actor();
  }
  bool receive_body(std::uint64_t epoch, std::string_view value) {
    RuntimeScope scope;
    bool post = false, consumed = true;
    {
      std::lock_guard lock(mutex);
      if (finished || !accepting || epoch != generation || input_error) return true;
      const auto& limits = client->options.limits;
      if (value.size() > limits.queued_body_bytes) {
        input_error = ErrorKind::ResourceLimit;
        post = mark_scheduled_locked();
      } else if (!value.empty()) {
        if (body_input.size() >= limits.queued_body_chunks ||
            value.size() > limits.queued_body_bytes - statistics.queued_bytes) {
          paused = true;
          paused_bytes = value.size();
          ++statistics.pauses;
          consumed = false;
        } else {
          body_input.emplace_back(value);
          statistics.queued_bytes += value.size();
          statistics.queued_chunks = body_input.size();
          statistics.peak_queued_bytes = std::max(statistics.peak_queued_bytes, statistics.queued_bytes);
        }
        post = mark_scheduled_locked();
      }
    }
    if (post) post_actor();
    return consumed;
  }
  void receive_done(std::uint64_t epoch, const transport::Result& value) {
    RuntimeScope scope;
    bool post;
    {
      std::lock_guard lock(mutex);
      if (finished || epoch != generation || done_received) return;
      done_received = true;
      // No unbounded diagnostic text crosses the wire-event boundary.
      transport::Result bounded;
      bounded.status = value.status;
      bounded.failure = value.failure;
      bounded.http_status = value.http_status;
      bounded.curl_code = value.curl_code;
      bounded.attempt = value.attempt;
      bounded.elapsed = value.elapsed;
      bounded.upload_sha256 = value.upload_sha256;
      done_input = std::move(bounded);
      post = mark_scheduled_locked();
    }
    if (post) post_actor();
  }
  void note_callback(SteadyTime before, bool threw) {
    if (threw) ++client->callback_exceptions;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(executor->now() - before) >=
        client->options.slow_callback_threshold) ++client->slow_callbacks;
  }
  void semantic(const Event& event) {
    if (std::holds_alternative<Commit>(event) || std::holds_alternative<Fail>(event)) return;
    if (const auto* raw = std::get_if<RawWire>(&event)) observed_document = raw->payload;
    if (!std::holds_alternative<Begin>(event) && !std::holds_alternative<RawWire>(event) &&
        !std::holds_alternative<ResponseEnvelope>(event)) semantic_output = true;
    if (stopping_error) return;
    if (callbacks.on_event) {
      const auto before = executor->now();
      try { callbacks.on_event(event); }
      catch (...) { note_callback(before, true); throw CallbackAbort{}; }
      note_callback(before, false);
    }
    {
      std::lock_guard lock(mutex);
      if (cancellation || deadline_fired || executor->now() >= deadline) throw StopObserved{};
    }
  }
  void fresh_codec() {
    chat_codec.reset();
    messages_codec.reset();
    responses_codec.reset();
    gemini_codec.reset();
    interactions_codec.reset();
    endpoint_codec.reset();
    accumulator = std::make_unique<Accumulator>(client->options.limits.semantic,
        [this](const Event& event) { semantic(event); }, response_limit());
    if (family == Family::Endpoint) endpoint_codec = endpoints::make_codec(endpoint, client->descriptor, *accumulator,
        prepared_model, codec_context, client->options.limits.semantic);
    else if (family == Family::Chat) chat_codec = std::make_unique<chat::Codec>(client->descriptor,
        options.streaming ? chat::Mode::Sse : chat::Mode::Buffered, *accumulator,
        client->options.limits.semantic, native_context);
    else if (family == Family::Messages) messages_codec = std::make_unique<messages::Codec>(client->descriptor,
        options.streaming ? messages::Mode::Sse : messages::Mode::Buffered, *accumulator,
        native_context, client->options.limits.semantic);
    else if (family == Family::Responses) responses_codec = std::make_unique<responses::Codec>(client->descriptor,
        options.streaming ? responses::Mode::Sse : responses::Mode::Buffered, *accumulator,
        native_context, client->options.limits.semantic);
    else if (family == Family::Gemini) gemini_codec = std::make_unique<gemini::Codec>(client->descriptor,
        options.streaming ? gemini::Mode::Sse : gemini::Mode::Buffered, *accumulator,
        native_context, client->options.limits.semantic);
    else interactions_codec = std::make_unique<interactions::Codec>(client->descriptor,
        options.streaming ? interactions::Mode::Sse : interactions::Mode::Buffered, *accumulator,
        native_context, client->options.limits.semantic);
    framer = options.streaming ? std::make_unique<transport::SseFramer>(client->options.limits.sse) : nullptr;
    response.reset();
    response_info = {};
    buffered.clear();
    observed_document.reset();
    response_bytes = 0;
    semantic_output = false;
    stopping_error.reset();
  }
  void dispatch() {
    const bool retry = stage == Stage::Backoff;
    fresh_codec();
    const auto epoch = generation + 1;
    transport::Callbacks wire;
    auto weak = weak_from_this();
    wire.on_head = [weak, epoch](const auto& head) { if (auto self = weak.lock()) self->receive_head(epoch, head); };
    wire.on_body = [weak, epoch](std::string_view bytes) {
      if (auto self = weak.lock()) return self->receive_body(epoch, bytes);
      return true;
    };
    wire.on_done = [weak, epoch](const auto& done) { if (auto self = weak.lock()) self->receive_done(epoch, done); };
    auto outbound = options.retry->enabled && attempts + 1 < options.retry->max_attempts ? request : std::move(request);
    outbound.deadline = deadline;
    {
      std::unique_lock lock(mutex);
      // Dispatch is linearized here, after allocation/encoding and immediately
      // before calling the transport. No token is spent on a cancelled wait.
      if (cancellation || deadline_fired || executor->now() >= deadline) {
        const auto kind = cancellation ? ErrorKind::Cancelled : ErrorKind::DeadlineExceeded;
        lock.unlock();
        stop(kind);
        return;
      }
      if (retry) {
        std::lock_guard budget_lock(client->mutex);
        if (!client->bucket.consume(executor->now())) return;
        if (pending_failure) {
          auto& observations = std::get<Failure>(*pending_failure).partial.raw_events;
          if (observations.size() > prior_raw_events.max_size() - prior_raw_events.size())
            throw std::length_error("runtime raw observation extent exceeded");
          prior_raw_events.reserve(prior_raw_events.size() + observations.size());
          prior_raw_events.insert(prior_raw_events.end(), std::make_move_iterator(observations.begin()),
              std::make_move_iterator(observations.end()));
        }
        pending_failure.reset();
      }
      stage = Stage::InFlight;
      statistics.backing_off = false;
      generation = epoch;
      accepting = true;
      head_received = done_received = paused = false;
      head_input.reset(); done_input.reset(); input_error.reset();
      body_input.clear();
      statistics.queued_bytes = statistics.queued_chunks = 0;
      ++attempts;
      statistics.attempts = attempts;
    }
    try {
      attempt = client->transport->start(std::move(outbound), std::move(wire));
      if (!attempt) throw std::runtime_error("missing attempt");
    } catch (...) {
      transport::Result failed;
      failed.status = transport::Status::Failed;
      failed.failure = transport::FailureKind::Other;
      receive_done(epoch, failed);
    }
  }
  void stop(ErrorKind kind) {
    if (stage == Stage::Terminal) return;
    if (accumulator && accumulator->terminal()) {
      if (attempt) attempt->cancel();
      return;
    }
    if (!stopping_error) stopping_error = error_for(kind);
    if (stage == Stage::Initial || stage == Stage::Backoff) {
      Failure failure{*stopping_error, {}};
      if (pending_failure) {
        auto& previous = std::get<Failure>(*pending_failure);
        failure.error.attempt = previous.error.attempt;
        failure.error.retry_safety = previous.error.retry_safety;
        failure.partial = std::move(previous.partial);
      } else if (attempts == 0) {
        failure.error.retry_safety = RetrySafety::NotSent;
      }
      failure.error.attempt.attempts = attempts;
      deliver(Outcome{std::move(failure)});
      return;
    }
    {
      std::lock_guard lock(mutex);
      accepting = false;
      body_input.clear();
      statistics.queued_bytes = statistics.queued_chunks = 0;
      paused = false;
    }
    if (attempt) attempt->cancel();
  }
  void fail_semantic(ErrorKind kind, std::string_view reason = {}) {
    if (!accumulator->terminal()) accumulator->accept(Fail{error_for(kind, reason)});
    {
      std::lock_guard lock(mutex);
      accepting = false;
      body_input.clear();
      statistics.queued_bytes = statistics.queued_chunks = 0;
      paused = false;
    }
    if (attempt) attempt->cancel();
  }
  void inspect(std::string_view body, bool retain_body = false) {
    static const std::vector<transport::Header> no_headers;
    auto classify = [&](json::Value document) {
      response_info = inspect_document_response(*client->options.policy, client->descriptor.family(),
          response ? response->value.status : 0, response ? response->value.headers : no_headers, document,
          response ? response->received : executor->now(), response ? response->wall_received : executor->wall_now());
    };
    // The codec already admitted this document under the response bounds, so
    // max_error_bytes, which bounds only bytes parsed here, does not apply. A
    // hosted image makes response.failed megabytes long; that is no local limit.
    if (observed_document) { classify(observed_document->root()); return; }
    if (body.size() > client->options.limits.max_error_bytes) {
      response_info = {};
      response_info.kind = ErrorKind::ResourceLimit;
      response_info.retry_class = RetryClass::Never;
      return;
    }
    auto parsed = json::parse(body, {client->options.limits.max_error_bytes,
        static_cast<std::size_t>(client->options.policy->admission().error_json_depth)});
    if (auto* document = std::get_if<json::Document>(&parsed)) {
      if (retain_body) {
        observed_document = std::make_shared<const json::Document>(std::move(*document));
        classify(observed_document->root());
      } else classify(document->root());
    } else {
      auto& rejected = std::get<json::ParseError>(parsed);
      if (retain_body && rejected.code == json::ParseCode::DuplicateKey && rejected.context.root().valid())
        observed_document = std::make_shared<const json::Document>(std::move(rejected.context));
      // Ambiguous duplicate fields remain diagnostic, never policy authority.
      classify({});
    }
  }
  void body(std::string_view bytes) {
    const auto& limits = client->options.limits;
    if (!response) { fail_semantic(ErrorKind::ProtocolCorrupt, "transport.body.before_head"); return; }
    const bool http_error = response->value.status < 200 || response->value.status >= 300;
    // A download bound for a file is held to its own limit, not the response limit.
    if (!http_error && !sink_path.empty()) { sink(bytes); return; }
    const auto response_bound = response_limit();
    if (bytes.size() > response_bound - response_bytes) { fail_semantic(ErrorKind::ResourceLimit, "runtime.response.limit"); return; }
    response_bytes += bytes.size();
    if (http_error || !options.streaming) {
      const auto limit = http_error ? limits.max_error_bytes : response_bound;
      if (bytes.size() > limit - buffered.size()) {
        fail_semantic(ErrorKind::ResourceLimit, http_error ? "runtime.error_body.limit" : "runtime.response.limit");
        return;
      }
      buffered.append(bytes);
      return;
    }
    framer->feed(bytes, [this](const transport::SseFrame& frame) {
      if (options.stop_token.stop_requested()) {
        fail_semantic(ErrorKind::Cancelled);
        return false;
      }
      observed_document.reset();
      const bool accepted = chat_codec ? chat_codec->frame(frame.event, frame.data) :
          messages_codec ? messages_codec->frame(frame.event, frame.data) :
          responses_codec ? responses_codec->frame(frame.event, frame.data) :
          gemini_codec ? gemini_codec->frame(frame.event, frame.data) :
          interactions_codec->frame(frame.event, frame.data);
      if (accumulator->terminal()) {
        const auto* failed = std::get_if<Failure>(&*accumulator->outcome());
        if (failed && failed->error.kind == ErrorKind::RemoteFailure) inspect(frame.data);
      }
      return accepted;
    });
    if (framer->error() != transport::SseError::None) {
      fail_semantic(framer->error() == transport::SseError::ResourceLimit ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt,
                    framer_reason(framer->error()));
    } else if (accumulator->terminal()) {
      // Semantic failure is already decided; cancellation only harvests wire evidence.
      if (attempt) attempt->cancel();
      std::lock_guard lock(mutex);
      accepting = false;
      body_input.clear();
      statistics.queued_bytes = statistics.queued_chunks = 0;
      paused = false;
    }
  }
  void done(transport::Result wire) {
    {
      std::lock_guard lock(mutex);
      accepting = false;
      const auto extra = wire.attempt.transport_internal_resends;
      attempts = extra > std::numeric_limits<std::uint32_t>::max() - attempts
          ? std::numeric_limits<std::uint32_t>::max() : attempts + extra;
      statistics.attempts = attempts;
    }
    attempt.reset();
    terminal_wire = wire;
    const bool semantic_failure_decided = accumulator->terminal();
    const bool http_error = (wire.http_status > 0 && (wire.http_status < 200 || wire.http_status >= 300)) ||
        (response && (response->value.status < 200 || response->value.status >= 300));
    if (http_error) inspect(buffered, true);
    const auto close_error = stopping_error ? *stopping_error :
        classify_failure(wire, response_info, {}, semantic_output, attempts, executor->now());
    const bool normal = wire.status == transport::Status::Completed && !stopping_error && !http_error;
    if (!accumulator->terminal()) {
      if (http_error) {
        if (observed_document) accumulator->accept(RawWire{std::string(http_error_event), observed_document}, &close_error);
        if (!accumulator->terminal()) accumulator->accept(Fail{close_error});
      }
      else if (options.streaming) {
        // Validate a dangling multi-byte sequence only when the stream really ended. A stall bound, reset or
        // truncation cuts it wherever it was, and that is the transport's error, not malformed server output.
        if (normal) framer->finish();
        if (framer->error() != transport::SseError::None)
          fail_semantic(framer->error() == transport::SseError::ResourceLimit ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt,
                        framer_reason(framer->error()));
        else if (chat_codec) chat_codec->finish({normal, close_error.kind});
        else if (messages_codec) messages_codec->finish({normal, close_error.kind});
        else if (responses_codec) responses_codec->finish({normal, close_error.kind});
        else if (gemini_codec) gemini_codec->finish({normal, close_error.kind});
        else interactions_codec->finish({normal, close_error.kind});
      } else {
        if (endpoint_codec) {
          endpoints::Close ended{normal, close_error.kind, {}};
          ended.upload_sha256 = wire.upload_sha256;
          ended.status = response ? response->value.status : 0;
          if (response) for (const auto& header : response->value.headers) ended.headers.emplace_back(header.name, header.value);
          if (!sink_path.empty() && normal) {
            ended.sink.emplace();
            if (!sink_file) ended.sink->path = sink_path;  // nothing arrived: the codec refuses an empty download
            else if (!close_sink(*ended.sink)) fail_semantic(ErrorKind::ResourceLimit, "runtime.sink.write_failed");
          }
          if (!accumulator->terminal()) endpoint_codec->buffered(buffered, std::move(ended));
        }
        else if (chat_codec) chat_codec->buffered(buffered, {normal, close_error.kind});
        else if (messages_codec) messages_codec->buffered(buffered, {normal, close_error.kind});
        else if (responses_codec) responses_codec->buffered(buffered, {normal, close_error.kind});
        else if (gemini_codec) gemini_codec->buffered(buffered, {normal, close_error.kind});
        else interactions_codec->buffered(buffered, {normal, close_error.kind});
        if (accumulator->terminal()) {
          const auto* failure = std::get_if<Failure>(&*accumulator->outcome());
          if (failure && (failure->error.kind == ErrorKind::RemoteFailure ||
              (!normal && !semantic_output && (failure->error.kind == ErrorKind::Transport ||
                                               failure->error.kind == ErrorKind::Truncated))))
            inspect(buffered);
        }
      }
    }
    if (!accumulator->terminal()) accumulator->accept(Fail{error_for(ErrorKind::ProtocolCorrupt, "runtime.outcome.undecided")});
    auto outcome = accumulator->take_outcome();
    // The decoder accepted the download: only now does it replace the file the caller named.
    if (sink_whole && std::holds_alternative<Completion>(*outcome) && !commit_sink())
      outcome = Outcome{Failure{error_for(ErrorKind::ResourceLimit, "runtime.sink.write_failed"), {}}};
    if (auto* failure = std::get_if<Failure>(&*outcome)) {
      if (!sink_path.empty()) discard_sink();
      if (!semantic_failure_decided && !normal && failure->error.kind == close_error.kind)
        failure->error = close_error;
      failure->error = classify_failure(wire, response_info, failure->error, semantic_output, attempts, executor->now());
      bool available;
      double random;
      {
        std::lock_guard lock(client->mutex);
        available = client->bucket.available(executor->now());
        random = available && options.retry->enabled ? client->random01() : 0;
      }
      auto retry = *options.retry;
      // A read (a job's poll or download) has no charge to duplicate.
      if (read_only) retry.allow_duplicate_billing_risk = true;
      // An endpoint call the provider charges for once it accepts it is attempted again only when it never
      // reached the provider, or when the provider refused it before accepting it: a complete 429 or 503 answer
      // (D6). The refusal needs no duplicate-billing opt-in, and the opt-in lets no other failure through.
      const bool refused = charge_on_accept && wire.status == transport::Status::Completed &&
          (failure->error.http_status == 429 || failure->error.http_status == 503);
      if (refused && failure->error.retry_safety == RetrySafety::PossiblyAccepted)
        failure->error.retry_safety = RetrySafety::RejectedBeforeOutput;
      const bool repeatable = !charge_on_accept || refused || failure->error.retry_safety == RetrySafety::NotSent;
      auto at = available && repeatable ? retry_at(failure->error, retry, attempts, executor->now(), deadline, random) : std::nullopt;
      if (at) {
        prior_usage_unknown = prior_usage_unknown || !prior_attempt_proves_no_usage(*failure);
        pending_failure = std::move(outcome);
        stage = Stage::Backoff;
        chat_codec.reset(); messages_codec.reset(); responses_codec.reset(); gemini_codec.reset(); interactions_codec.reset(); endpoint_codec.reset(); accumulator.reset(); framer.reset();
        terminal_wire.reset();
        buffered.clear(); response.reset();
        retry_timer.arm(*executor, *at, [weak = weak_from_this()] { if (auto self = weak.lock()) self->timer(true); });
        {
          std::lock_guard lock(mutex);
          statistics.backing_off = true;
        }
        joined.notify_all();
        return;
      }
    }
    deliver(std::move(*outcome));
  }
  void deliver(Outcome outcome) {
    if (!prior_raw_events.empty()) {
      auto& observations = std::visit([](auto& value) -> std::vector<RawWire>& {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Completion>) return value.raw_events;
        else return value.partial.raw_events;
      }, outcome);
      if (observations.size() > prior_raw_events.max_size() - prior_raw_events.size())
        throw std::length_error("runtime raw observation extent exceeded");
      prior_raw_events.reserve(prior_raw_events.size() + observations.size());
      prior_raw_events.insert(prior_raw_events.end(), std::make_move_iterator(observations.begin()),
          std::make_move_iterator(observations.end()));
      observations = std::move(prior_raw_events);
    }
    if (auto* completion = std::get_if<Completion>(&outcome)) {
      completion->attempt.attempts = attempts;
      completion->attempt.prior_usage_unknown = prior_usage_unknown;
      if (terminal_wire) {
        const auto& observed = terminal_wire->attempt;
        completion->attempt.request_may_have_left = observed.reached >= transport::Stage::RequestStarted;
        completion->attempt.request_body_bytes = observed.request_body_bytes;
        completion->attempt.response_head_seen = observed.response_head_seen;
        completion->attempt.transport_internal_resends = observed.transport_internal_resends;
      }
    } else std::get<Failure>(outcome).error.attempt.prior_usage_unknown = prior_usage_unknown;
    {
      std::lock_guard lock(mutex);
      accepting = false;
      result = std::make_shared<const Outcome>(std::move(outcome));
      stage = Stage::Terminal;
      statistics.backing_off = false;
      body_input.clear(); head_input.reset(); done_input.reset();
      statistics.queued_bytes = statistics.queued_chunks = 0;
    }
    deadline_timer.reset();
    retry_timer.reset();
    if (attempt) { attempt->cancel(); attempt.reset(); }
    // A stop callback may be executing cancel(). Never destroy its registration
    // while holding mutex, which that callback needs to leave.
    stop_callback.reset();
    RuntimeScope scope;
    if (callbacks.on_outcome) {
      const auto before = executor ? executor->now() : SteadyTime{};
      bool threw = false;
      try { callbacks.on_outcome(result); } catch (...) { threw = true; }
      if (client) note_callback(before, threw);
    }
    callbacks = {};
    chat_codec.reset(); messages_codec.reset(); responses_codec.reset(); gemini_codec.reset(); interactions_codec.reset(); accumulator.reset(); framer.reset();
    pending_failure.reset(); response.reset(); terminal_wire.reset();
    std::string{}.swap(buffered);
    request = {}; native_context.reset();
    auto owner = std::move(client);
    if (owner && admitted) owner->release(this);
    {
      std::lock_guard lock(mutex);
      finished = true;
    }
    joined.notify_all();
  }
  void run() noexcept {
    RuntimeScope scope;
    // Drain a small bounded batch without returning through the executor for
    // every body chunk and terminal wire event. Each step still rechecks stop
    // controls; the bound preserves scheduling opportunities for other actors.
    constexpr unsigned max_steps = 4;
    for (unsigned steps = 0; ; ++steps) {
      try { step(); }
      catch (const CallbackAbort&) {
        stopping_error = error_for(ErrorKind::Misuse);
        fail_semantic(ErrorKind::Misuse);
        if (!attempt) finish_interrupted();
      } catch (const StopObserved&) {
        bool cancelled;
        { std::lock_guard lock(mutex); cancelled = cancellation; }
        stop(cancelled ? ErrorKind::Cancelled : ErrorKind::DeadlineExceeded);
        // A callback can interrupt decoding the already-final wire event.
        if (stage == Stage::InFlight && !attempt) finish_interrupted();
      } catch (...) {
        if (stage != Stage::Terminal) {
          stopping_error = error_for(ErrorKind::ResourceLimit);
          if (attempt) attempt->cancel();
          deliver(Outcome{Failure{*stopping_error, {}}});
        }
      }
      bool again;
      {
        std::lock_guard lock(mutex);
        again = !finished && (head_input || !body_input.empty() || done_input || input_error ||
            control_pending || retry_fired);
        if (!again) scheduled = false;
      }
      if (!again) return;
      // Keep scheduled set throughout the batch and the queued continuation,
      // so producers cannot start a second concurrent actor.
      if (steps + 1 == max_steps) { post_actor(); return; }
    }
  }
  void finish_interrupted() {
    if (!accumulator->terminal()) accumulator->accept(Fail{*stopping_error});
    auto outcome = accumulator->take_outcome();
    if (auto* failure = std::get_if<Failure>(&*outcome)) {
      if (terminal_wire)
        failure->error = classify_failure(*terminal_wire, response_info, failure->error,
            semantic_output, attempts, executor->now());
      else {
        failure->error.attempt.attempts = attempts;
        failure->error.retry_safety = semantic_output ? RetrySafety::OutputObserved : RetrySafety::PossiblyAccepted;
      }
    }
    deliver(std::move(*outcome));
  }
  void step() {
    if (stage == Stage::Terminal) return;
    if (initial_error) {
      deliver(Outcome{Failure{std::move(*initial_error), {}}});
      return;
    }
    bool cancelled, expired, retry;
    std::optional<ErrorKind> failure;
    std::optional<Head> head;
    {
      std::lock_guard lock(mutex);
      cancelled = cancellation;
      expired = deadline_fired || executor->now() >= deadline;
      retry = std::exchange(retry_fired, false);
      failure = std::exchange(input_error, {});
      control_pending = false;
      head = std::move(head_input); head_input.reset();
    }
    if (head) { response = std::move(head); inspect({}); }
    if (failure && accumulator)
      fail_semantic(*failure, *failure == ErrorKind::ProtocolCorrupt ? "transport.head.duplicate" : std::string_view{});
    if (cancelled || expired) {
      stop(cancelled ? ErrorKind::Cancelled : ErrorKind::DeadlineExceeded);
      if (stage == Stage::Terminal) return;
    }
    if (stage == Stage::Initial) {
      deadline_timer.arm(*executor, deadline, [weak = weak_from_this()] { if (auto self = weak.lock()) self->timer(false); });
      dispatch();
      return;
    }
    if (stage == Stage::Backoff) {
      if (retry) {
        retry_timer.reset();
        dispatch();
        if (pending_failure) deliver(std::move(*pending_failure));
      }
      return;
    }
    std::string bytes;
    std::optional<transport::Result> completed;
    bool resume = false;
    {
      std::lock_guard lock(mutex);
      if (!body_input.empty()) {
        bytes = std::move(body_input.front()); body_input.pop_front();
        statistics.queued_bytes -= bytes.size();
        statistics.queued_chunks = body_input.size();
      } else if (done_input) { completed = std::move(done_input); done_input.reset(); }
      if (paused && accepting && body_input.size() < client->options.limits.queued_body_chunks &&
          paused_bytes <= client->options.limits.queued_body_bytes - statistics.queued_bytes) {
        paused = false;
        resume = true;
      }
    }
    if (!bytes.empty() && !stopping_error && !accumulator->terminal()) body(bytes);
    if (resume && attempt && !stopping_error && !accumulator->terminal()) attempt->resume();
    if (completed) done(std::move(*completed));
  }
};

void ClientState::close_resources() noexcept {
  {
    std::lock_guard lock(mutex);
    if (closing) return;
    closing = true;
  }
  transport->shutdown();
  executor->shutdown();
  {
    std::lock_guard lock(mutex);
    closed = true;
  }
  drained.notify_all();
}
void ClientState::release(OperationState* operation) {
  bool close;
  {
    std::lock_guard lock(mutex);
    active.erase(operation);
    close = stopping && asynchronous_shutdown && active.empty();
  }
  drained.notify_all();
  if (close) close_resources();
}
void ClientState::shutdown() noexcept {
  const bool asynchronous = in_thread();
  const bool pumped = executor->host_pumped();
  std::vector<std::shared_ptr<OperationState>> operations;
  bool empty;
  {
    std::lock_guard lock(mutex);
    stopping = true;
    // The cancelled operations of a host-pumped executor finish when the host runs it again, so the destructor cannot
    // wait for them: the last one to finish closes the resources.
    asynchronous_shutdown = asynchronous_shutdown || asynchronous || (pumped && !active.empty());
    operations.reserve(active.size());
    for (const auto& entry : active) operations.push_back(entry.second);
    empty = active.empty();
  }
  for (const auto& operation : operations) operation->cancel();
  if (asynchronous) {
    if (empty) executor->post([self = shared_from_this()] { self->close_resources(); });
    return;
  }
  if (pumped && !empty) return;
  {
    std::unique_lock lock(mutex);
    drained.wait(lock, [this] { return active.empty(); });
  }
  close_resources();
  std::unique_lock lock(mutex);
  drained.wait(lock, [this] { return closed; });
}

#if SP_BUILD_THREADS
std::shared_ptr<Executor> make_pool_executor(std::size_t workers) {
  return std::make_shared<PoolExecutor>(workers);
}
#else
// A single-thread build has no worker threads to make: the host supplies the executor.
std::shared_ptr<Executor> make_pool_executor(std::size_t) {
  throw descriptor::ConfigError{"/executor", "a host-supplied executor (this build has no threads)", 1, "runtime options rejected"};
}
#endif

Client ClientAccess::make(descriptor::ValidatedDescriptor descriptor, Options options,
                          std::shared_ptr<Executor> executor, std::shared_ptr<AttemptTransport> transport,
                          std::function<double()> random01, RealTransportFactory real_transport) {
  validate(descriptor, options);
  if (!transport && !real_transport)
    throw descriptor::ConfigError{"/transport", "a transport backend", 1, "runtime options rejected"};
  if (!executor) executor = make_pool_executor(options.workers);
  if (!transport) {
    auto wire = options.transport;
    // libcurl keeps idle connections only up to this many; it must cover every operation the client may
    // run at once, or a burst of that size reconnects (a TLS handshake each) after the previous one drained.
    if (wire.connection_cache_size == 0) wire.connection_cache_size = options.limits.max_operations;
    if (wire.resolve) {
      auto resolve = std::move(wire.resolve);
      wire.resolve = [resolve = std::move(resolve)](const std::string& host) {
        RuntimeScope scope;
        return resolve(host);
      };
    }
    transport = real_transport(std::move(wire));
  }
  return Client(std::make_shared<ClientState>(std::move(descriptor), std::move(options),
      std::move(executor), std::move(transport), std::move(random01)));
}
PreparedRequest ClientAccess::prepare_control(Client& client, Request request, RunOptions options,
                                             std::function<bool(std::string&)> transform) {
  auto preparation = client.prepare(std::move(request), std::move(options));
  if (preparation.error() || !transform) return preparation;
  auto& state = *preparation.state_;
  const auto digest = [](std::string_view bytes) { return crypto::sha256(bytes); };
  const auto before = digest(state.request.body);
  std::optional<ErrorKind> error;
  try {
    if (!transform(state.request.body)) error = ErrorKind::InvalidRequest;
    const auto& resources = state.client->descriptor.policy()->resources();
    const auto ceiling = state.family == OperationState::Family::Chat
        ? resources.chat_text_request_bytes : resources.request_bytes;
    if (state.request.body.size() > ceiling) error = ErrorKind::ResourceLimit;
  } catch (...) { error = ErrorKind::InvalidRequest; }
  if (before != digest(state.request.body)) {
    if (state.native_context) state.native_context = state.native_context->decoding_only();
    // A private qualification control changed the actual body: original typed
    // cap/invocation proofs cannot authorize a bounded ordinary dispatch.
    state.prepared_max_output_tokens.reset();
    state.prepared_model_invocation_limit.reset();
    state.facts = {};
    state.prepared_cache_marks = {};
  }
  if (error) {
    state.initial_error = error_for(*error);
    state.initial_error->retry_safety = RetrySafety::NotSent;
  }
  return preparation;
}
OperationStats ClientAccess::stats(const Operation& operation) {
  if (!operation.state_) return {};
  std::lock_guard lock(operation.state_->mutex);
  return operation.state_->statistics;
}
bool ClientAccess::wait_for_backoff(const Operation& operation, std::chrono::milliseconds timeout) {
  auto state = operation.state_;
  if (!state || runtime_depth || pool_thread || transport::on_io_thread() ||
      (state->executor && state->executor->in_thread()))
    return false;
  std::unique_lock lock(state->mutex);
  state->joined.wait_for(lock, timeout, [&] { return state->statistics.backing_off || state->finished; });
  return state->statistics.backing_off;
}
}  // namespace sp::runtime::detail

namespace sp::runtime {
Options::Options(configuration::PolicySnapshot snapshot) : policy(std::move(snapshot)) {
  if (!policy) throw descriptor::ConfigError{"/policy", "admitted runtime snapshot", 1, "runtime options rejected"};
  const auto& values = policy->defaults();
  workers = values.workers;
  transport.io_threads = static_cast<unsigned>(values.io_threads);
  transport.resolver_threads = static_cast<unsigned>(values.resolver_threads);
  transport.max_host_connections = static_cast<long>(values.max_host_connections);
  transport.max_head_bytes = values.max_head_bytes;
  transport.dns_ttl = std::chrono::seconds(values.dns_ttl_seconds);
  default_timeout = std::chrono::milliseconds(values.default_timeout_ms);
  slow_callback_threshold = std::chrono::milliseconds(values.slow_callback_threshold_ms);
  retry_tokens = values.retry_tokens;
  retry_tokens_per_second = values.retry_tokens_per_second;
  limits.max_operations = values.max_operations;
  limits.queued_body_chunks = values.queued_body_chunks;
  limits.queued_body_bytes = values.queued_body_bytes;
  limits.max_response_bytes = values.max_response_bytes;
  limits.max_error_bytes = values.max_error_bytes;
  limits.semantic.max_parts = values.semantic_max_parts;
  limits.semantic.max_content_bytes = values.semantic_max_content_bytes;
  limits.semantic.max_tool_bytes = values.semantic_max_tool_bytes;
  limits.semantic.max_json_depth = values.semantic_max_json_depth;
  limits.sse.max_line_bytes = values.sse_max_line_bytes;
  limits.sse.max_event_bytes = values.sse_max_event_bytes;
  limits.sse.max_total_bytes = values.sse_max_total_bytes;
}
AdmissionError::AdmissionError(ErrorKind kind) {
  auto error = detail::error_for(kind);
  error.retry_safety = RetrySafety::NotSent;
  outcome_ = std::make_shared<const Outcome>(Failure{std::move(error), {}});
}
const char* AdmissionError::what() const noexcept { return "runtime admission rejected"; }

Operation::Operation(std::shared_ptr<detail::OperationState> state) : state_(std::move(state)) {}
Operation::~Operation() { cancel(); }
Operation::Operation(Operation&& other) noexcept : state_(std::move(other.state_)) {}
Operation& Operation::operator=(Operation&& other) noexcept {
  if (this != &other) { cancel(); state_ = std::move(other.state_); }
  return *this;
}
void Operation::cancel() noexcept { if (state_) state_->cancel(); }
void Operation::detach() noexcept { state_.reset(); }
bool Operation::valid() const noexcept { return bool(state_); }
Result Operation::join() const {
  auto state = state_;
  if (!state) return detail::misuse_result();
  if (auto refused = detail::blocking_refused(state->executor.get())) return refused;
  std::unique_lock lock(state->mutex);
  state->joined.wait(lock, [&] { return state->finished; });
  return state->result;
}

InterfaceContract interface_contract() noexcept {
  const auto core = ::sp::core_interface_contract();
  // Literal implementation revision is independent of the consumer's headers.
  if (core.revision != 7 || codec_interface_revision() != 7 || descriptor::interface_revision() != 7) return {0, 0};
  return {7, core.capabilities | capability::TypedRuntime | capability::PreparedAdmission |
      capability::CompleteAttemptEvidence | capability::HostSeam};
}
const char* InterfaceContractError::what() const noexcept {
  return "SchemaProvider loaded interface contract is unsupported";
}
void require_interface_contract(std::uint32_t expected_revision, std::uint64_t required_capabilities) {
  const auto actual = interface_contract();
  if (actual.revision != expected_revision ||
      (actual.capabilities & required_capabilities) != required_capabilities)
    throw InterfaceContractError();
}
Client::Client(descriptor::ValidatedDescriptor descriptor, Options options, std::shared_ptr<Executor> executor,
               std::shared_ptr<HostTransport> transport)
    : Client((require_interface_contract(EXPECTED_INTERFACE_REVISION, capability::RequiredProvider),
              detail::ClientAccess::make(std::move(descriptor), std::move(options), std::move(executor), std::move(transport)))) {}
Client::Client(std::shared_ptr<detail::ClientState> state) : state_(std::move(state)) {}
Client::~Client() { if (state_) state_->shutdown(); }
Client::Client(Client&& other) noexcept : state_(std::move(other.state_)) {}
Client& Client::operator=(Client&& other) noexcept {
  if (this != &other) {
    if (state_) state_->shutdown();
    state_ = std::move(other.state_);
  }
  return *this;
}
PreparedRequest::PreparedRequest(std::shared_ptr<detail::OperationState> state)
    : state_(std::move(state)) {}
PreparedRequest::~PreparedRequest() { release(); }
PreparedRequest::PreparedRequest(PreparedRequest&& other) noexcept
    : state_(std::move(other.state_)) {}
PreparedRequest& PreparedRequest::operator=(PreparedRequest&& other) noexcept {
  if (this != &other) {
    release();
    state_ = std::move(other.state_);
  }
  return *this;
}
void PreparedRequest::release() noexcept {
  if (!state_) return;
  auto client = state_->client;
  if (client) {
    std::lock_guard lock(client->mutex);
    if (state_->prepared) {
      --client->prepared_operations;
      state_->prepared = false;
    }
  }
  state_.reset();
}
bool PreparedRequest::valid() const noexcept { return state_ && state_->prepared; }
const Error* PreparedRequest::error() const noexcept {
  return state_ && state_->initial_error ? &*state_->initial_error : nullptr;
}
std::string_view PreparedRequest::family() const noexcept {
  return state_ && state_->client ? state_->client->descriptor.family() : std::string_view{};
}
std::string_view PreparedRequest::model() const noexcept {
  return state_ ? state_->prepared_model : std::string_view{};
}
std::string_view PreparedRequest::encoded_body() const noexcept {
  return state_ ? state_->request.body : std::string_view{};
}
SteadyTime PreparedRequest::deadline() const noexcept {
  return state_ ? state_->deadline : SteadyTime{};
}
const descriptor::ValidatedDescriptor* PreparedRequest::descriptor() const noexcept {
  return state_ && state_->client ? &state_->client->descriptor : nullptr;
}
const RetryPolicy* PreparedRequest::retry_policy() const noexcept {
  return state_ && state_->options.retry ? &*state_->options.retry : nullptr;
}
std::optional<std::uint64_t> PreparedRequest::max_output_tokens() const noexcept {
  return state_ ? state_->prepared_max_output_tokens : std::nullopt;
}
const Limits* PreparedRequest::limits() const noexcept {
  return state_ && state_->client ? &state_->client->options.limits : nullptr;
}
const NativeContext* PreparedRequest::native_context() const noexcept {
  return state_ ? state_->native_context.get() : nullptr;
}
std::optional<std::uint64_t> PreparedRequest::model_invocation_limit() const noexcept {
  return state_ ? state_->prepared_model_invocation_limit : std::nullopt;
}
const RequestFacts& PreparedRequest::facts() const noexcept {
  static const RequestFacts none;
  return state_ ? state_->facts : none;
}
const CacheMarkReport& PreparedRequest::cache_marks() const noexcept {
  static const CacheMarkReport none;
  return state_ ? state_->prepared_cache_marks : none;
}
std::string_view PreparedRequest::endpoint() const noexcept {
  return state_ ? std::string_view(state_->endpoint) : std::string_view{};
}
std::string_view PreparedRequest::method() const noexcept {
  return state_ ? std::string_view(state_->request.method) : std::string_view{};
}
std::string_view PreparedRequest::url() const noexcept {
  return state_ ? std::string_view(state_->request.url) : std::string_view{};
}
Operation Client::start(Request request, RunOptions options, Callbacks callbacks) {
  return start(prepare(std::move(request), std::move(options)), std::move(callbacks));
}
Operation Client::start(PreparedRequest preparation, Callbacks callbacks) {
  require_interface_contract(EXPECTED_INTERFACE_REVISION, capability::RequiredProvider);
  auto client = state_;
  auto& operation = preparation.state_;
  if (!client || !operation || operation->client != client || !operation->prepared)
    throw AdmissionError(ErrorKind::Misuse);
  {
    std::lock_guard lock(client->mutex);
    if (client->stopping) throw AdmissionError(ErrorKind::Cancelled);
    client->active.emplace(operation.get(), operation);
    --client->prepared_operations;
    operation->prepared = false;
    operation->admitted = true;
  }
  struct Publication {
    detail::ClientState& owner;
    detail::OperationState& operation;
    bool published = false;
    ~Publication() {
      if (published) return;
      {
        std::lock_guard lock(operation.mutex);
        operation.ready = false;
      }
      operation.stop_callback.reset();
      owner.release(&operation);
    }
  } publication{*client, *operation};
  operation->callbacks = std::move(callbacks);
  try { operation->activate(); }
  catch (...) { throw AdmissionError(ErrorKind::ResourceLimit); }
  publication.published = true;
  return Operation(std::move(operation));
}
PreparedRequest Client::prepare_call(RunOptions options, const std::function<void(detail::OperationState&, std::optional<Error>&)>& encode) {
  require_interface_contract(EXPECTED_INTERFACE_REVISION, capability::RequiredProvider);
  auto client = state_;
  if (!client) throw AdmissionError(ErrorKind::Misuse);
  if (!options.retry) options.retry = default_retry_policy(*client->options.policy);
  const auto now = client->executor->now();
  const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyTime::max() - now);
  const auto deadline = options.deadline.value_or(client->options.default_timeout >= room
      ? SteadyTime::max() - SteadyTime::duration{1} : now + client->options.default_timeout);
  PreparedRequest preparation(std::make_shared<detail::OperationState>(client, std::move(options), Callbacks{}, deadline));
  auto& operation = preparation.state_;
  std::optional<ErrorKind> admission;
  {
    std::lock_guard lock(client->mutex);
    if (client->stopping) admission = ErrorKind::Cancelled;
    else if (client->active.size() >= client->options.limits.max_operations ||
             client->prepared_operations >= client->options.limits.max_operations - client->active.size())
      admission = ErrorKind::ResourceLimit;
    else {
      ++client->prepared_operations;
      operation->prepared = true;
    }
  }
  if (admission) throw AdmissionError(*admission);
  std::optional<Error> error;
  const auto& retry_ceiling = client->options.policy->admission();
  const auto& stall_ceiling = retry_ceiling.default_timeout_ms;
  if (!detail::valid_retry_policy(*operation->options.retry) || deadline == SteadyTime::max() ||
      (operation->options.connect_timeout && !detail::valid_stall_bound(*operation->options.connect_timeout, stall_ceiling)) ||
      (operation->options.first_byte_timeout && !detail::valid_stall_bound(*operation->options.first_byte_timeout, stall_ceiling)) ||
      (operation->options.idle_timeout && !detail::valid_stall_bound(*operation->options.idle_timeout, stall_ceiling)) ||
      operation->options.retry->max_attempts > retry_ceiling.retry_max_attempts ||
      static_cast<std::uint64_t>(operation->options.retry->base_delay.count()) > retry_ceiling.retry_base_delay_ms ||
      static_cast<std::uint64_t>(operation->options.retry->max_delay.count()) > retry_ceiling.retry_max_delay_ms)
    error = detail::error_for(ErrorKind::InvalidRequest);
  else if (operation->options.stop_token.stop_requested()) error = detail::error_for(ErrorKind::Cancelled);
  else if (client->executor->now() >= deadline) error = detail::error_for(ErrorKind::DeadlineExceeded);
  try {
    if (!error) encode(*operation, error);
    if (!error) {
      auto& wire = operation->request;
      wire.deadline = deadline;
      // Resolved here, once, so every attempt (and any backend) sees the same effective bounds:
      // the per-run override, else the client-wide default. Zero means disabled.
      wire.connect_timeout = operation->options.connect_timeout.value_or(client->options.transport.connect_timeout);
      wire.first_byte_timeout = operation->options.first_byte_timeout.value_or(client->options.transport.first_byte_timeout);
      wire.idle_timeout = operation->options.idle_timeout.value_or(client->options.transport.idle_timeout);
      wire.http_version = client->options.http_version;
      wire.ca_file = client->options.ca_file;
    }
  } catch (...) { error = detail::error_for(ErrorKind::ResourceLimit); }
  if (error) {
    error->safe_message = detail::safe_message(error->kind);
    error->retry_safety = RetrySafety::NotSent;
    operation->initial_error = std::move(error);
  }
  return preparation;
}
PreparedRequest Client::prepare(Request request, RunOptions options) {
  auto client = state_;
  return prepare_call(std::move(options), [&](detail::OperationState& state, std::optional<Error>& error) {
    auto* operation = &state;
    if (!error) std::visit([&](const auto& typed) {
      using T = std::decay_t<decltype(typed)>;
      operation->prepared_model = typed.model;
      operation->family = std::is_same_v<T, chat::Request> ? detail::OperationState::Family::Chat :
          std::is_same_v<T, messages::Request> ? detail::OperationState::Family::Messages :
          std::is_same_v<T, responses::Request> ? detail::OperationState::Family::Responses :
          std::is_same_v<T, gemini::Request> ? detail::OperationState::Family::Gemini :
          detail::OperationState::Family::Interactions;
      auto encoded = [&] {
        if constexpr (std::is_same_v<T, chat::Request>) return chat::encode(client->descriptor, typed, operation->options.streaming);
        else if constexpr (std::is_same_v<T, messages::Request>) return messages::encode(client->descriptor, typed, operation->options.streaming);
        else if constexpr (std::is_same_v<T, responses::Request>) return responses::encode(client->descriptor, typed, operation->options.streaming);
        else if constexpr (std::is_same_v<T, gemini::Request>) return gemini::encode(client->descriptor, typed, operation->options.streaming);
        else return interactions::encode(client->descriptor, typed, operation->options.streaming);
      }();
      if (auto* failure = std::get_if<Error>(&encoded)) {
        error = std::move(*failure);
        // The encoder's text is replaced by the generic text of its kind below; the code keeps the check.
        constexpr auto encoder = std::is_same_v<T, chat::Request> ? reason::Source::ChatRequest :
            std::is_same_v<T, messages::Request> ? reason::Source::MessagesRequest :
            std::is_same_v<T, responses::Request> ? reason::Source::ResponsesRequest :
            std::is_same_v<T, gemini::Request> ? reason::Source::GeminiRequest : reason::Source::InteractionsRequest;
        if (error->reason.empty()) error->reason = std::string(reason::request_code(encoder, error->safe_message));
        return;
      }
      auto& value = std::get<0>(encoded);
      auto& wire = operation->request;
      wire.method = std::move(value.method);
      wire.url = std::string(client->descriptor.base_url()) + value.path;
      wire.body = std::move(value.body);
      operation->prepared_max_output_tokens = value.max_output_tokens;
      operation->prepared_model_invocation_limit = value.model_invocation_limit;
      operation->facts = detail::request_facts(client->descriptor, typed);
      operation->prepared_cache_marks = value.cache_marks;
      wire.headers.reserve(value.headers.size() + (client->options.api_key.empty() ? 0 : 1));
      for (auto& header : value.headers) wire.headers.push_back({std::move(header.first), std::move(header.second)});
      if (!client->options.api_key.empty()) {
        if (operation->family == detail::OperationState::Family::Messages)
          wire.headers.push_back({"x-api-key", client->options.api_key});
        else if (operation->family == detail::OperationState::Family::Gemini ||
                 operation->family == detail::OperationState::Family::Interactions)
          wire.headers.push_back({"x-goog-api-key", client->options.api_key});
        else wire.headers.push_back({"Authorization", "Bearer " + client->options.api_key});
      }
      operation->native_context = std::move(value.context);
    }, request);
  });
}
Result Client::complete(Request request, RunOptions options) {
  if (!state_) return detail::misuse_result();
  if (auto refused = detail::blocking_refused(state_->executor.get())) return refused;
  try {
    auto operation = start(std::move(request), std::move(options));
    return operation.join();
  } catch (const AdmissionError& error) { return error.outcome(); }
}
Result Client::complete(PreparedRequest preparation) {
  if (!state_) return detail::misuse_result();
  if (auto refused = detail::blocking_refused(state_->executor.get())) return refused;
  try {
    auto operation = start(std::move(preparation));
    return operation.join();
  } catch (const AdmissionError& error) { return error.outcome(); }
}
template <class Encoded>
PreparedRequest Client::prepare_endpoint(RunOptions options, std::string_view endpoint, int reason_source,
                                         const std::function<std::variant<Encoded, Error>()>& encode) {
  auto client = state_;
  options.streaming = false;  // endpoint calls are buffered (D6)
  return prepare_call(std::move(options), [&](detail::OperationState& operation, std::optional<Error>& error) {
    auto encoded = encode();
    if (auto* failure = std::get_if<Error>(&encoded)) {
      error = std::move(*failure);
      if (error->reason.empty()) error->reason = std::string(reason::request_code(static_cast<reason::Source>(reason_source), error->safe_message));
      return;
    }
    auto& value = std::get<Encoded>(encoded);
    operation.family = detail::OperationState::Family::Endpoint;
    operation.endpoint = std::string(endpoint);
    operation.charge_on_accept = value.charge_on_accept;
    operation.endpoint_response_bytes = value.max_response_bytes;
    operation.prepared_model = std::move(value.model);
    operation.codec_context = std::move(value.context);
    operation.sink_path = std::move(value.sink);
    operation.sink_limit = value.sink_limit;
    operation.read_only = value.method == "GET";
    auto& wire = operation.request;
    wire.method = std::move(value.method);
    wire.url = std::string(client->descriptor.base_url()) + value.path;
    if (value.file) {
      wire.file_body = transport::FileBody{std::move(value.body), std::move(value.file->path), value.file->offset, value.file->size,
                                           std::move(value.body_suffix), std::move(value.file->sha256)};
    } else wire.body = std::move(value.body);
    // The descriptor's own headers go with every call to its origin; the codec owns the content headers.
    for (const auto& header : client->descriptor.headers())
      if (!detail::equal_ascii(header.first, "content-type") && !detail::equal_ascii(header.first, "accept"))
        wire.headers.push_back({header.first, header.second});
    if (!value.content_type.empty()) wire.headers.push_back({"Content-Type", value.content_type});
    wire.headers.push_back({"Accept", value.accept});
    // A header the descriptor sets wins over the same header of the endpoint's fact.
    for (auto& [name, text] : value.headers) {
      const bool overridden = std::any_of(client->descriptor.headers().begin(), client->descriptor.headers().end(),
                                          [&](const auto& header) { return detail::equal_ascii(header.first, name); });
      if (!overridden) wire.headers.push_back({std::move(name), std::move(text)});
    }
    if (!client->options.api_key.empty()) {
      if (value.auth == "x-api-key") wire.headers.push_back({"x-api-key", client->options.api_key});
      else if (value.auth == "x-goog-api-key") wire.headers.push_back({"x-goog-api-key", client->options.api_key});
      else wire.headers.push_back({"Authorization", "Bearer " + client->options.api_key});
    }
  });
}
PreparedEndpoint<images::Outcome> Client::prepare(images::Request request, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<images::Outcome>(prepare_endpoint<images::Encoded>(std::move(options), "images",
      static_cast<int>(reason::Source::ImagesRequest), [&] { return images::encode(client->descriptor, request); }));
}
PreparedEndpoint<decisions::Outcome> Client::prepare(decisions::Request request, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<decisions::Outcome>(prepare_endpoint<decisions::Encoded>(std::move(options), "decisions",
      static_cast<int>(reason::Source::DecisionsRequest), [&] { return decisions::encode(client->descriptor, request); }));
}
PreparedEndpoint<embeddings::Outcome> Client::prepare(embeddings::Request request, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<embeddings::Outcome>(prepare_endpoint<embeddings::Encoded>(std::move(options), "embeddings",
      static_cast<int>(reason::Source::EmbeddingsRequest), [&] { return embeddings::encode(client->descriptor, request); }));
}
PreparedEndpoint<rerank::Outcome> Client::prepare(rerank::Request request, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<rerank::Outcome>(prepare_endpoint<rerank::Encoded>(std::move(options), "rerank",
      static_cast<int>(reason::Source::RerankRequest), [&] { return rerank::encode(client->descriptor, request); }));
}
PreparedEndpoint<moderation::Outcome> Client::prepare(moderation::Request request, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<moderation::Outcome>(prepare_endpoint<moderation::Encoded>(std::move(options), "moderation",
      static_cast<int>(reason::Source::ModerationRequest), [&] { return moderation::encode(client->descriptor, request); }));
}
PreparedEndpoint<speech::Outcome> Client::prepare(speech::Request request, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<speech::Outcome>(prepare_endpoint<speech::Encoded>(std::move(options), "speech",
      static_cast<int>(reason::Source::SpeechRequest), [&] { return speech::encode(client->descriptor, request); }));
}
PreparedEndpoint<transcription::Outcome> Client::prepare(transcription::Request request, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<transcription::Outcome>(prepare_endpoint<transcription::Encoded>(std::move(options), "transcription",
      static_cast<int>(reason::Source::TranscriptionRequest), [&] { return transcription::encode(client->descriptor, request); }));
}
images::Outcome Client::complete(images::Request request, RunOptions options) {
  return complete(prepare(std::move(request), std::move(options)));
}
images::Outcome Client::complete(PreparedEndpoint<images::Outcome> call) {
  if (!state_) return images::typed(*detail::misuse_result());
  if (auto refused = detail::blocking_refused(state_->executor.get())) return images::typed(*refused);
  try { return start(std::move(call)).join(); }
  catch (const AdmissionError& error) { return images::typed(*error.outcome()); }
}
EndpointCall<images::Outcome> Client::start(PreparedEndpoint<images::Outcome> call, std::function<void(images::Outcome)> on_outcome) {
  Callbacks callbacks;
  if (on_outcome) callbacks.on_outcome = [deliver = std::move(on_outcome)](Result result) { deliver(images::typed(*result)); };
  return EndpointCall<images::Outcome>(start(std::move(call.prepared_), std::move(callbacks)));
}
decisions::Outcome Client::complete(decisions::Request request, RunOptions options) {
  return complete(prepare(std::move(request), std::move(options)));
}
decisions::Outcome Client::complete(PreparedEndpoint<decisions::Outcome> call) {
  if (!state_) return decisions::typed(*detail::misuse_result());
  if (auto refused = detail::blocking_refused(state_->executor.get())) return decisions::typed(*refused);
  try { return start(std::move(call)).join(); }
  catch (const AdmissionError& error) { return decisions::typed(*error.outcome()); }
}
EndpointCall<decisions::Outcome> Client::start(PreparedEndpoint<decisions::Outcome> call, std::function<void(decisions::Outcome)> on_outcome) {
  Callbacks callbacks;
  if (on_outcome) callbacks.on_outcome = [deliver = std::move(on_outcome)](Result result) { deliver(decisions::typed(*result)); };
  return EndpointCall<decisions::Outcome>(start(std::move(call.prepared_), std::move(callbacks)));
}
embeddings::Outcome Client::complete(embeddings::Request request, RunOptions options) {
  return complete(prepare(std::move(request), std::move(options)));
}
embeddings::Outcome Client::complete(PreparedEndpoint<embeddings::Outcome> call) {
  if (!state_) return embeddings::typed(*detail::misuse_result());
  if (auto refused = detail::blocking_refused(state_->executor.get())) return embeddings::typed(*refused);
  try { return start(std::move(call)).join(); }
  catch (const AdmissionError& error) { return embeddings::typed(*error.outcome()); }
}
EndpointCall<embeddings::Outcome> Client::start(PreparedEndpoint<embeddings::Outcome> call, std::function<void(embeddings::Outcome)> on_outcome) {
  Callbacks callbacks;
  if (on_outcome) callbacks.on_outcome = [deliver = std::move(on_outcome)](Result result) { deliver(embeddings::typed(*result)); };
  return EndpointCall<embeddings::Outcome>(start(std::move(call.prepared_), std::move(callbacks)));
}
rerank::Outcome Client::complete(rerank::Request request, RunOptions options) {
  return complete(prepare(std::move(request), std::move(options)));
}
rerank::Outcome Client::complete(PreparedEndpoint<rerank::Outcome> call) {
  if (!state_) return rerank::typed(*detail::misuse_result());
  if (auto refused = detail::blocking_refused(state_->executor.get())) return rerank::typed(*refused);
  try { return start(std::move(call)).join(); }
  catch (const AdmissionError& error) { return rerank::typed(*error.outcome()); }
}
EndpointCall<rerank::Outcome> Client::start(PreparedEndpoint<rerank::Outcome> call, std::function<void(rerank::Outcome)> on_outcome) {
  Callbacks callbacks;
  if (on_outcome) callbacks.on_outcome = [deliver = std::move(on_outcome)](Result result) { deliver(rerank::typed(*result)); };
  return EndpointCall<rerank::Outcome>(start(std::move(call.prepared_), std::move(callbacks)));
}
moderation::Outcome Client::complete(moderation::Request request, RunOptions options) {
  return complete(prepare(std::move(request), std::move(options)));
}
moderation::Outcome Client::complete(PreparedEndpoint<moderation::Outcome> call) {
  if (!state_) return moderation::typed(*detail::misuse_result());
  if (auto refused = detail::blocking_refused(state_->executor.get())) return moderation::typed(*refused);
  try { return start(std::move(call)).join(); }
  catch (const AdmissionError& error) { return moderation::typed(*error.outcome()); }
}
EndpointCall<moderation::Outcome> Client::start(PreparedEndpoint<moderation::Outcome> call, std::function<void(moderation::Outcome)> on_outcome) {
  Callbacks callbacks;
  if (on_outcome) callbacks.on_outcome = [deliver = std::move(on_outcome)](Result result) { deliver(moderation::typed(*result)); };
  return EndpointCall<moderation::Outcome>(start(std::move(call.prepared_), std::move(callbacks)));
}
speech::Outcome Client::complete(speech::Request request, RunOptions options) {
  return complete(prepare(std::move(request), std::move(options)));
}
speech::Outcome Client::complete(PreparedEndpoint<speech::Outcome> call) {
  if (!state_) return speech::typed(*detail::misuse_result());
  if (auto refused = detail::blocking_refused(state_->executor.get())) return speech::typed(*refused);
  try { return start(std::move(call)).join(); }
  catch (const AdmissionError& error) { return speech::typed(*error.outcome()); }
}
EndpointCall<speech::Outcome> Client::start(PreparedEndpoint<speech::Outcome> call, std::function<void(speech::Outcome)> on_outcome) {
  Callbacks callbacks;
  if (on_outcome) callbacks.on_outcome = [deliver = std::move(on_outcome)](Result result) { deliver(speech::typed(*result)); };
  return EndpointCall<speech::Outcome>(start(std::move(call.prepared_), std::move(callbacks)));
}
transcription::Outcome Client::complete(transcription::Request request, RunOptions options) {
  return complete(prepare(std::move(request), std::move(options)));
}
transcription::Outcome Client::complete(PreparedEndpoint<transcription::Outcome> call) {
  if (!state_) return transcription::typed(*detail::misuse_result());
  if (auto refused = detail::blocking_refused(state_->executor.get())) return transcription::typed(*refused);
  try { return start(std::move(call)).join(); }
  catch (const AdmissionError& error) { return transcription::typed(*error.outcome()); }
}
EndpointCall<transcription::Outcome> Client::start(PreparedEndpoint<transcription::Outcome> call, std::function<void(transcription::Outcome)> on_outcome) {
  Callbacks callbacks;
  if (on_outcome) callbacks.on_outcome = [deliver = std::move(on_outcome)](Result result) { deliver(transcription::typed(*result)); };
  return EndpointCall<transcription::Outcome>(start(std::move(call.prepared_), std::move(callbacks)));
}
PreparedEndpoint<jobs::Outcome> Client::prepare(videos::Request request, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<jobs::Outcome>(prepare_endpoint<videos::Encoded>(std::move(options), "videos",
      static_cast<int>(reason::Source::VideosRequest), [&] { return videos::encode(client->descriptor, request); }));
}
PreparedEndpoint<jobs::Outcome> Client::prepare(jobs::Poll poll, RunOptions options) {
  auto client = state_;
  // The poll answers with the job's state, which the codec of the endpoint that started it reads.
  return PreparedEndpoint<jobs::Outcome>(prepare_endpoint<jobs::Encoded>(std::move(options), poll.handle.endpoint,
      static_cast<int>(reason::Source::JobsRequest), [&] { return jobs::encode(client->descriptor, poll); }));
}
PreparedEndpoint<jobs::DownloadOutcome> Client::prepare(jobs::Download download, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<jobs::DownloadOutcome>(prepare_endpoint<jobs::Encoded>(std::move(options), "content",
      static_cast<int>(reason::Source::JobsRequest), [&] { return jobs::encode(client->descriptor, download); }));
}
jobs::Outcome Client::complete(videos::Request request, RunOptions options) {
  return complete(prepare(std::move(request), std::move(options)));
}
jobs::Outcome Client::complete(jobs::Poll poll, RunOptions options) {
  return complete(prepare(std::move(poll), std::move(options)));
}
jobs::DownloadOutcome Client::complete(jobs::Download download, RunOptions options) {
  return complete(prepare(std::move(download), std::move(options)));
}
jobs::Outcome Client::complete(PreparedEndpoint<jobs::Outcome> call) {
  if (!state_) return jobs::typed(*detail::misuse_result());
  if (auto refused = detail::blocking_refused(state_->executor.get())) return jobs::typed(*refused);
  try { return start(std::move(call)).join(); }
  catch (const AdmissionError& error) { return jobs::typed(*error.outcome()); }
}
jobs::DownloadOutcome Client::complete(PreparedEndpoint<jobs::DownloadOutcome> call) {
  if (!state_) return jobs::typed_content(*detail::misuse_result());
  if (auto refused = detail::blocking_refused(state_->executor.get())) return jobs::typed_content(*refused);
  try { return start(std::move(call)).join(); }
  catch (const AdmissionError& error) { return jobs::typed_content(*error.outcome()); }
}
EndpointCall<jobs::Outcome> Client::start(PreparedEndpoint<jobs::Outcome> call, std::function<void(jobs::Outcome)> on_outcome) {
  Callbacks callbacks;
  if (on_outcome) callbacks.on_outcome = [deliver = std::move(on_outcome)](Result result) { deliver(jobs::typed(*result)); };
  return EndpointCall<jobs::Outcome>(start(std::move(call.prepared_), std::move(callbacks)));
}
EndpointCall<jobs::DownloadOutcome> Client::start(PreparedEndpoint<jobs::DownloadOutcome> call, std::function<void(jobs::DownloadOutcome)> on_outcome) {
  Callbacks callbacks;
  if (on_outcome) callbacks.on_outcome = [deliver = std::move(on_outcome)](Result result) { deliver(jobs::typed_content(*result)); };
  return EndpointCall<jobs::DownloadOutcome>(start(std::move(call.prepared_), std::move(callbacks)));
}
template <class Outcome> Outcome Client::complete_endpoint(PreparedEndpoint<Outcome> call) {
  constexpr const Outcome* typed = nullptr;
  if (!state_) return detail::typed_outcome(*detail::misuse_result(), typed);
  if (auto refused = detail::blocking_refused(state_->executor.get())) return detail::typed_outcome(*refused, typed);
  try { return start_endpoint(std::move(call), {}).join(); }
  catch (const AdmissionError& error) { return detail::typed_outcome(*error.outcome(), typed); }
}
template <class Outcome> EndpointCall<Outcome> Client::start_endpoint(PreparedEndpoint<Outcome> call, std::function<void(Outcome)> on_outcome) {
  Callbacks callbacks;
  if (on_outcome)
    callbacks.on_outcome = [deliver = std::move(on_outcome)](Result result) { deliver(detail::typed_outcome(*result, static_cast<const Outcome*>(nullptr))); };
  return EndpointCall<Outcome>(start(std::move(call.prepared_), std::move(callbacks)));
}
PreparedEndpoint<files::Outcome> Client::prepare(files::Upload upload, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<files::Outcome>(prepare_endpoint<files::Encoded>(std::move(options), "files",
      static_cast<int>(reason::Source::FilesRequest), [&] { return files::encode(client->descriptor, upload); }));
}
PreparedEndpoint<files::Outcome> Client::prepare(files::Get get, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<files::Outcome>(prepare_endpoint<files::Encoded>(std::move(options), "files",
      static_cast<int>(reason::Source::FilesRequest), [&] { return files::encode(client->descriptor, get); }));
}
PreparedEndpoint<files::PageOutcome> Client::prepare(files::List list, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<files::PageOutcome>(prepare_endpoint<files::Encoded>(std::move(options), "files.list",
      static_cast<int>(reason::Source::FilesRequest), [&] { return files::encode(client->descriptor, list); }));
}
PreparedEndpoint<files::DeleteOutcome> Client::prepare(files::Delete del, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<files::DeleteOutcome>(prepare_endpoint<files::Encoded>(std::move(options), "files.delete",
      static_cast<int>(reason::Source::FilesRequest), [&] { return files::encode(client->descriptor, del); }));
}
PreparedEndpoint<jobs::DownloadOutcome> Client::prepare(files::Download download, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<jobs::DownloadOutcome>(prepare_endpoint<files::Encoded>(std::move(options), "content.document",
      static_cast<int>(reason::Source::FilesRequest), [&] { return files::encode(client->descriptor, download); }));
}
files::Outcome Client::complete(files::Upload upload, RunOptions options) { return complete(prepare(std::move(upload), std::move(options))); }
files::Outcome Client::complete(files::Get get, RunOptions options) { return complete(prepare(std::move(get), std::move(options))); }
files::PageOutcome Client::complete(files::List list, RunOptions options) { return complete(prepare(std::move(list), std::move(options))); }
files::DeleteOutcome Client::complete(files::Delete del, RunOptions options) { return complete(prepare(std::move(del), std::move(options))); }
jobs::DownloadOutcome Client::complete(files::Download download, RunOptions options) { return complete(prepare(std::move(download), std::move(options))); }
files::Outcome Client::complete(PreparedEndpoint<files::Outcome> call) { return complete_endpoint(std::move(call)); }
files::PageOutcome Client::complete(PreparedEndpoint<files::PageOutcome> call) { return complete_endpoint(std::move(call)); }
files::DeleteOutcome Client::complete(PreparedEndpoint<files::DeleteOutcome> call) { return complete_endpoint(std::move(call)); }
EndpointCall<files::Outcome> Client::start(PreparedEndpoint<files::Outcome> call, std::function<void(files::Outcome)> on_outcome) {
  return start_endpoint(std::move(call), std::move(on_outcome));
}
EndpointCall<files::PageOutcome> Client::start(PreparedEndpoint<files::PageOutcome> call, std::function<void(files::PageOutcome)> on_outcome) {
  return start_endpoint(std::move(call), std::move(on_outcome));
}
EndpointCall<files::DeleteOutcome> Client::start(PreparedEndpoint<files::DeleteOutcome> call, std::function<void(files::DeleteOutcome)> on_outcome) {
  return start_endpoint(std::move(call), std::move(on_outcome));
}
PreparedEndpoint<files::ProgressOutcome> Client::prepare(files::Start step, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<files::ProgressOutcome>(prepare_endpoint<files::Encoded>(std::move(options), "files.start",
      static_cast<int>(reason::Source::FilesRequest), [&] { return files::encode(client->descriptor, step); }));
}
files::ProgressOutcome Client::complete(files::Start step, RunOptions options) { return complete(prepare(std::move(step), std::move(options))); }
PreparedEndpoint<files::ProgressOutcome> Client::prepare(files::Part step, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<files::ProgressOutcome>(prepare_endpoint<files::Encoded>(std::move(options), "files.part",
      static_cast<int>(reason::Source::FilesRequest), [&] { return files::encode(client->descriptor, step); }));
}
files::ProgressOutcome Client::complete(files::Part step, RunOptions options) { return complete(prepare(std::move(step), std::move(options))); }
PreparedEndpoint<files::ProgressOutcome> Client::prepare(files::Complete step, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<files::ProgressOutcome>(prepare_endpoint<files::Encoded>(std::move(options), "files.complete",
      static_cast<int>(reason::Source::FilesRequest), [&] { return files::encode(client->descriptor, step); }));
}
files::ProgressOutcome Client::complete(files::Complete step, RunOptions options) { return complete(prepare(std::move(step), std::move(options))); }
PreparedEndpoint<files::ProgressOutcome> Client::prepare(files::Query step, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<files::ProgressOutcome>(prepare_endpoint<files::Encoded>(std::move(options), "files.query",
      static_cast<int>(reason::Source::FilesRequest), [&] { return files::encode(client->descriptor, step); }));
}
files::ProgressOutcome Client::complete(files::Query step, RunOptions options) { return complete(prepare(std::move(step), std::move(options))); }
PreparedEndpoint<files::ProgressOutcome> Client::prepare(files::Cancel step, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<files::ProgressOutcome>(prepare_endpoint<files::Encoded>(std::move(options), "files.cancel",
      static_cast<int>(reason::Source::FilesRequest), [&] { return files::encode(client->descriptor, step); }));
}
files::ProgressOutcome Client::complete(files::Cancel step, RunOptions options) { return complete(prepare(std::move(step), std::move(options))); }
files::ProgressOutcome Client::complete(PreparedEndpoint<files::ProgressOutcome> call) { return complete_endpoint(std::move(call)); }
EndpointCall<files::ProgressOutcome> Client::start(PreparedEndpoint<files::ProgressOutcome> call, std::function<void(files::ProgressOutcome)> on_outcome) {
  return start_endpoint(std::move(call), std::move(on_outcome));
}
PreparedEndpoint<batches::Outcome> Client::prepare(batches::Submit call, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<batches::Outcome>(prepare_endpoint<batches::Encoded>(std::move(options), "batches",
      static_cast<int>(reason::Source::BatchesRequest), [&] { return batches::encode(client->descriptor, call); }));
}
batches::Outcome Client::complete(batches::Submit call, RunOptions options) { return complete(prepare(std::move(call), std::move(options))); }
PreparedEndpoint<batches::Outcome> Client::prepare(batches::Get call, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<batches::Outcome>(prepare_endpoint<batches::Encoded>(std::move(options), "batches",
      static_cast<int>(reason::Source::BatchesRequest), [&] { return batches::encode(client->descriptor, call); }));
}
batches::Outcome Client::complete(batches::Get call, RunOptions options) { return complete(prepare(std::move(call), std::move(options))); }
PreparedEndpoint<batches::Outcome> Client::prepare(batches::Cancel call, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<batches::Outcome>(prepare_endpoint<batches::Encoded>(std::move(options), "batches",
      static_cast<int>(reason::Source::BatchesRequest), [&] { return batches::encode(client->descriptor, call); }));
}
batches::Outcome Client::complete(batches::Cancel call, RunOptions options) { return complete(prepare(std::move(call), std::move(options))); }
PreparedEndpoint<batches::DeleteOutcome> Client::prepare(batches::Delete call, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<batches::DeleteOutcome>(prepare_endpoint<batches::Encoded>(std::move(options), "batches.delete",
      static_cast<int>(reason::Source::BatchesRequest), [&] { return batches::encode(client->descriptor, call); }));
}
batches::DeleteOutcome Client::complete(batches::Delete call, RunOptions options) { return complete(prepare(std::move(call), std::move(options))); }
PreparedEndpoint<jobs::DownloadOutcome> Client::prepare(batches::Results call, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<jobs::DownloadOutcome>(prepare_endpoint<batches::Encoded>(std::move(options), "content.document",
      static_cast<int>(reason::Source::BatchesRequest), [&] { return batches::encode(client->descriptor, call); }));
}
jobs::DownloadOutcome Client::complete(batches::Results call, RunOptions options) { return complete(prepare(std::move(call), std::move(options))); }
batches::Outcome Client::complete(PreparedEndpoint<batches::Outcome> call) { return complete_endpoint(std::move(call)); }
batches::DeleteOutcome Client::complete(PreparedEndpoint<batches::DeleteOutcome> call) { return complete_endpoint(std::move(call)); }
EndpointCall<batches::Outcome> Client::start(PreparedEndpoint<batches::Outcome> call, std::function<void(batches::Outcome)> on_outcome) {
  return start_endpoint(std::move(call), std::move(on_outcome));
}
EndpointCall<batches::DeleteOutcome> Client::start(PreparedEndpoint<batches::DeleteOutcome> call, std::function<void(batches::DeleteOutcome)> on_outcome) {
  return start_endpoint(std::move(call), std::move(on_outcome));
}
PreparedEndpoint<tokens::Outcome> Client::prepare(tokens::Count count, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<tokens::Outcome>(prepare_endpoint<tokens::Encoded>(std::move(options), "tokens",
      static_cast<int>(reason::Source::TokensRequest), [&] { return tokens::encode(client->descriptor, count); }));
}
tokens::Outcome Client::complete(tokens::Count count, RunOptions options) { return complete(prepare(std::move(count), std::move(options))); }
tokens::Outcome Client::complete(PreparedEndpoint<tokens::Outcome> call) { return complete_endpoint(std::move(call)); }
EndpointCall<tokens::Outcome> Client::start(PreparedEndpoint<tokens::Outcome> call, std::function<void(tokens::Outcome)> on_outcome) {
  return start_endpoint(std::move(call), std::move(on_outcome));
}
PreparedEndpoint<generations::Outcome> Client::prepare(generations::Get get, RunOptions options) {
  auto client = state_;
  return PreparedEndpoint<generations::Outcome>(prepare_endpoint<generations::Encoded>(std::move(options), "generations",
      static_cast<int>(reason::Source::GenerationsRequest), [&] { return generations::encode(client->descriptor, get); }));
}
generations::Outcome Client::complete(generations::Get get, RunOptions options) { return complete(prepare(std::move(get), std::move(options))); }
generations::Outcome Client::complete(PreparedEndpoint<generations::Outcome> call) { return complete_endpoint(std::move(call)); }
EndpointCall<generations::Outcome> Client::start(PreparedEndpoint<generations::Outcome> call, std::function<void(generations::Outcome)> on_outcome) {
  return start_endpoint(std::move(call), std::move(on_outcome));
}
Diagnostics Client::diagnostics() const noexcept {
  if (!state_) return {};
  return {state_->slow_callbacks.load(), state_->callback_exceptions.load()};
}
}  // namespace sp::runtime
