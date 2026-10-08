#include "runtime/policy.h"
#include "json/json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace sp::runtime::detail {
namespace {
using Ms = std::chrono::milliseconds;
constexpr auto max_ms = std::numeric_limits<Ms::rep>::max();

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}
bool iequal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    auto c = a[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c != b[i]) return false;
  }
  return true;
}
bool digits(std::string_view s, std::int64_t& out) {
  if (s.empty()) return false;
  out = 0;
  for (char c : s) {
    if (c < '0' || c > '9' || out > (max_ms - (c - '0')) / 10) return false;
    out = out * 10 + c - '0';
  }
  return true;
}
// Exact decimal conversion, rounded upward to a millisecond. No floating-point
// parser, exponent syntax, locale, or overflow can shorten a server minimum.
std::optional<Ms> decimal_ms(std::string_view s, std::int64_t scale) {
  auto dot = s.find('.');
  std::int64_t whole;
  if (!digits(s.substr(0, dot), whole) || whole > max_ms / scale) return {};
  auto value = whole * scale;
  if (dot == std::string_view::npos) return Ms(value);
  auto fraction = s.substr(dot + 1);
  if (fraction.empty()) return {};
  std::int64_t partial = 0;
  bool remainder = false;
  for (char c : fraction) {
    if (c < '0' || c > '9') return {};
    scale /= 10;
    if (scale) partial += (c - '0') * scale;
    else remainder = remainder || c != '0';
  }
  partial += remainder;
  if (value > max_ms - partial) return {};
  return Ms(value + partial);
}
std::optional<Ms> reset_duration(std::string_view s) {
  std::int64_t total = 0, previous = max_ms;
  while (!s.empty()) {
    auto end = s.find_first_not_of("0123456789.");
    if (end == std::string_view::npos || end == 0) return {};
    auto number = s.substr(0, end);
    s.remove_prefix(end);
    std::int64_t scale;
    if (s.starts_with("ms")) { scale = 1; s.remove_prefix(2); }
    else {
      switch (s.front()) {
        case 'd': scale = 86400000; break;
        case 'h': scale = 3600000; break;
        case 'm': scale = 60000; break;
        case 's': scale = 1000; break;
        default: return {};
      }
      s.remove_prefix(1);
    }
    // Fractional seconds/milliseconds only: division of non-power-of-ten unit
    // scales would otherwise require rational parsing. Unknown formats fail closed.
    if (scale >= previous || (scale > 1000 && number.find('.') != std::string_view::npos)) return {};
    previous = scale;
    auto part = decimal_ms(number, scale);
    if (!part || total > max_ms - part->count()) return {};
    total += part->count();
  }
  return Ms(total);
}
SteadyTime add(SteadyTime t, Ms delay) {
  using D = SteadyTime::duration;
  if (delay < Ms::zero() || delay > std::chrono::duration_cast<Ms>(D::max())) return SteadyTime::max();
  const auto d = std::chrono::duration_cast<D>(delay);
  if (t.time_since_epoch().count() > D::max().count() - d.count()) return SteadyTime::max();
  return t + d;
}
std::optional<Ms> http_date(std::string_view s, WallTime wall) {
  // Admit IMF-fixdate only. Obsolete HTTP-date forms are conservatively unknown.
  if (s.size() != 29 || s.substr(3, 2) != ", " || s[7] != ' ' || s[11] != ' ' ||
      s[16] != ' ' || s[19] != ':' || s[22] != ':' || s.substr(25) != " GMT") return {};
  constexpr std::array<std::string_view, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  constexpr std::array<std::string_view, 7> weekdays{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  auto month = std::find(months.begin(), months.end(), s.substr(8, 3));
  auto weekday = std::find(weekdays.begin(), weekdays.end(), s.substr(0, 3));
  std::int64_t y, d, h, m, sec;
  if (month == months.end() || weekday == weekdays.end() || !digits(s.substr(5, 2), d) ||
      !digits(s.substr(12, 4), y) || !digits(s.substr(17, 2), h) ||
      !digits(s.substr(20, 2), m) || !digits(s.substr(23, 2), sec) ||
      y < 1601 || h > 23 || m > 59 || sec > 59) return {};
  const std::chrono::year_month_day date{std::chrono::year(static_cast<int>(y)),
      std::chrono::month(static_cast<unsigned>(month - months.begin() + 1)), std::chrono::day(static_cast<unsigned>(d))};
  if (!date.ok()) return {};
  const auto days = std::chrono::sys_days(date);
  if (std::chrono::weekday(days).c_encoding() != static_cast<unsigned>(weekday - weekdays.begin())) return {};
  const auto target = std::chrono::duration_cast<Ms>(days.time_since_epoch()).count() + ((h * 60 + m) * 60 + sec) * 1000;
  const auto receipt = std::chrono::floor<Ms>(wall.time_since_epoch()).count();
  if (target <= receipt) return Ms::zero();
  if (receipt < 0 && target > max_ms + receipt) return {};
  return Ms(target - receipt);
}
bool never_kind(ErrorKind k) {
  switch (k) {
    case ErrorKind::Transport: case ErrorKind::Truncated: case ErrorKind::RemoteFailure:
    case ErrorKind::RateLimited: case ErrorKind::Overloaded: return false;
    default: return true;
  }
}
}  // namespace

ResponseInfo inspect_document_response(const configuration::RuntimePolicy& policy, std::string_view family, int status,
    const std::vector<transport::Header>& headers, json::Value document,
    SteadyTime received, WallTime wall_received) {
  ResponseInfo info;
  const auto& facts = policy.errors();
  for (const auto& rule : facts.statuses) {
    if (status != rule.status) continue;
    info.kind = rule.kind;
    info.retry_class = rule.retry;
    break;
  }
  auto family_rules = std::find_if(facts.families.begin(), facts.families.end(),
      [&](const auto& rule) { return rule.family == family; });
  // Some vendors wrap the error envelope in a one-element array
  // (`[{"error": {...}}]`, seen on Google Interactions). Read the envelope itself.
  if (document.is_array() && document.size() == 1 && document.at(0).is_object()) document = document.at(0);
  if (document.valid() && family_rules != facts.families.end()) {
    json::Value error;
    for (const auto& path : family_rules->error_paths) {
      if (path == "error") error = document.get("error");
      else if (path == "response.error") error = document.get("response").get("error");
      else if (path == "root_error" && document.get("type").as_string() == family_rules->root_error_type) error = document;
      if (error.is_object()) break;
    }
    if (error.is_object()) {
      auto apply_code = [&](json::Value value) {
        if (!value.is_string()) return;
        for (const auto& rule : family_rules->codes) {
          if (value.as_string() != rule.value || info.kind == ErrorKind::QuotaExhausted) continue;
          info.kind = rule.kind;
          info.retry_class = rule.retry;
          info.vendor_code = rule.value;
          break;
        }
      };
      for (const auto& field : family_rules->code_fields) {
        // `details.reason` selects `reason` of every object in the `details` array
        // (Google's google.rpc.ErrorInfo); every other field is read from the error itself.
        if (field == "details.reason") {
          const auto details = error.get("details");
          if (!details.is_array()) continue;
          for (auto detail : details.elements()) if (detail.is_object()) apply_code(detail.get("reason"));
        } else apply_code(error.get(field));
      }
    }
  }
  for (const auto& header : headers) {
    auto rule = std::find_if(facts.headers.begin(), facts.headers.end(), [&](const auto& candidate) {
      return (candidate.family == "*" || candidate.family == family) && iequal(header.name, candidate.name);
    });
    if (rule == facts.headers.end()) continue;
    const auto value = trim(header.value);
    std::optional<Ms> delay;
    switch (rule->format) {
      case configuration::HeaderFormat::SecondsOrHttpDate: {
        std::int64_t seconds;
        if (digits(value, seconds) && seconds <= max_ms / 1000) delay = Ms(seconds * 1000);
        else delay = http_date(value, wall_received);
        break;
      }
      case configuration::HeaderFormat::Milliseconds: delay = decimal_ms(value, 1); break;
      case configuration::HeaderFormat::Duration: if (!value.empty()) delay = reset_duration(value); break;
    }
    const auto minimum = delay ? add(received, *delay) : SteadyTime::max();
    info.retry_not_before = std::max(info.retry_not_before.value_or(minimum), minimum);
  }
  return info;
}

ResponseInfo inspect_response(const configuration::RuntimePolicy& policy, std::string_view family, int status,
    const std::vector<transport::Header>& headers, std::string_view body,
    SteadyTime received, WallTime wall_received) {
  if (body.empty()) return inspect_document_response(policy, family, status, headers, {}, received, wall_received);
  auto parsed = json::parse(body, {body.size(), static_cast<std::size_t>(policy.admission().error_json_depth)});
  const auto* document = std::get_if<json::Document>(&parsed);
  return inspect_document_response(policy, family, status, headers,
      document ? document->root() : json::Value{}, received, wall_received);
}

std::string_view safe_message(ErrorKind kind) noexcept {
  switch (kind) {
    case ErrorKind::InvalidConfig: return "invalid runtime configuration";
    case ErrorKind::InvalidRequest: return "invalid request";
    case ErrorKind::Unsupported: return "unsupported operation";
    case ErrorKind::Authentication: return "authentication failed";
    case ErrorKind::Permission: return "permission denied";
    case ErrorKind::NotFound: return "requested resource not found";
    case ErrorKind::RateLimited: return "request rate limited";
    case ErrorKind::QuotaExhausted: return "quota exhausted";
    case ErrorKind::LimitUnknown: return "unclassified request limit";
    case ErrorKind::Overloaded: return "remote service unavailable";
    case ErrorKind::Transport: return "transport failed";
    case ErrorKind::ProtocolCorrupt: return "invalid response protocol";
    case ErrorKind::Truncated: return "response ended abnormally";
    case ErrorKind::RemoteFailure: return "remote request failed";
    case ErrorKind::Cancelled: return "operation cancelled";
    case ErrorKind::DeadlineExceeded: return "operation deadline exceeded";
    case ErrorKind::ResourceLimit: return "runtime resource limit exceeded";
    case ErrorKind::Misuse: return "runtime API misuse";
    case ErrorKind::ReplayIneligible: return "native replay ineligible";
  }
  return "operation failed";
}

Error classify_failure(const transport::Result& result, const ResponseInfo& info,
    const std::optional<Error>& semantic, bool output, std::uint32_t attempts, SteadyTime now) {
  Error error;
  if (semantic) {
    error.kind = semantic->kind;
    error.retry_class = semantic->retry_class;
    if (error.kind == ErrorKind::RemoteFailure) { error.kind = info.kind; error.retry_class = info.retry_class; }
  } else if (result.status == transport::Status::Cancelled) error.kind = ErrorKind::Cancelled;
  else if (result.status == transport::Status::DeadlineExceeded) error.kind = ErrorKind::DeadlineExceeded;
  else if (result.failure == transport::FailureKind::Protocol) error.kind = ErrorKind::ProtocolCorrupt;
  else if (result.failure == transport::FailureKind::ResponseTooLarge) error.kind = ErrorKind::ResourceLimit;
  else if (result.failure == transport::FailureKind::CallbackError || result.failure == transport::FailureKind::Misuse) error.kind = ErrorKind::Misuse;
  else if (result.http_status >= 400 || result.status == transport::Status::Completed) {
    error.kind = info.kind; error.retry_class = info.retry_class;
  } else {
    using F = transport::FailureKind;
    switch (result.failure) {
      case F::Protocol: error.kind = ErrorKind::ProtocolCorrupt; break;
      case F::ResponseTooLarge: error.kind = ErrorKind::ResourceLimit; break;
      case F::CallbackError: case F::Misuse: error.kind = ErrorKind::Misuse; break;
      case F::Truncated: error.kind = ErrorKind::Truncated; error.retry_class = RetryClass::Transient; break;
      case F::Resolve: case F::Connect: case F::Tls: case F::Send: case F::Receive: case F::ResendRefused:
        error.kind = ErrorKind::Transport; error.retry_class = RetryClass::Transient; break;
      // Stall bounds are transport failures of the attempt, not the operation deadline: the caller
      // may still hold time budget. Connect and first-byte stalls share Receive/Connect's
      // classification. An idle stall after the response head is an abnormal end of the response
      // (Truncated, like an EOF mid-body); one before it, a transport failure. Retry safety comes from
      // the attempt evidence below, exactly as for those failures.
      case F::ConnectTimeout: case F::FirstByteTimeout:
        error.kind = ErrorKind::Transport; error.retry_class = RetryClass::Transient; break;
      case F::IdleTimeout:
        error.kind = result.attempt.response_head_seen ? ErrorKind::Truncated : ErrorKind::Transport;
        error.retry_class = RetryClass::Transient; break;
      default: error.kind = ErrorKind::Transport; error.retry_class = RetryClass::Unknown; break;
    }
  }
  if (error.kind == ErrorKind::LimitUnknown) error.retry_class = RetryClass::Unknown;
  else if (never_kind(error.kind)) error.retry_class = RetryClass::Never;
  // Permanent response evidence remains a no-retry condition even when a wire
  // close error has higher diagnostic precedence (for example quota + short EOF).
  if (info.retry_class == RetryClass::Never) error.retry_class = RetryClass::Never;
  error.safe_message = safe_message(error.kind);
  error.http_status = result.http_status;
  error.vendor_code = info.vendor_code;
  const auto& a = result.attempt;
  const bool sent = a.reached >= transport::Stage::RequestStarted || a.request_body_bytes > 0 ||
                    a.response_head_seen || a.transport_internal_resends > 0 || result.http_status != 0;
  error.attempt = {sent, a.request_body_bytes, a.response_head_seen, a.transport_internal_resends, attempts};
  error.retry_safety = (output || (semantic && semantic->retry_safety == RetrySafety::OutputObserved)) ?
      RetrySafety::OutputObserved : sent ? RetrySafety::PossiblyAccepted : RetrySafety::NotSent;
  if (info.retry_not_before) {
    if (*info.retry_not_before == SteadyTime::max()) error.retry_after = Ms::max();
    else if (*info.retry_not_before <= now) error.retry_after = Ms::zero();
    else {
      // Subtraction is only performed once known to fit the signed duration.
      const auto target = info.retry_not_before->time_since_epoch().count();
      const auto start = now.time_since_epoch().count();
      if (start < 0 && target > SteadyTime::duration::max().count() + start) error.retry_after = Ms::max();
      else error.retry_after = std::chrono::ceil<Ms>(*info.retry_not_before - now);
    }
  }
  return error;
}

bool valid_retry_policy(const RetryPolicy& p) noexcept {
  return p.max_attempts > 0 && p.base_delay >= Ms::zero() && p.max_delay >= p.base_delay;
}
bool valid_stall_bound(Ms value, std::uint64_t ceiling_ms) noexcept {
  return value.count() >= 0 && static_cast<std::uint64_t>(value.count()) <= ceiling_ms;
}
std::optional<SteadyTime> retry_at(const Error& e, const RetryPolicy& p,
    std::uint32_t attempts, SteadyTime now, SteadyTime deadline, double uniform01) {
  if (!valid_retry_policy(p) || !p.enabled || attempts == 0 || attempts >= p.max_attempts ||
      e.attempt.attempts >= p.max_attempts || now >= deadline || never_kind(e.kind) ||
      (e.retry_class != RetryClass::Transient && e.retry_class != RetryClass::AfterReset) ||
      e.retry_safety == RetrySafety::OutputObserved ||
      (e.retry_safety == RetrySafety::PossiblyAccepted && !p.allow_duplicate_billing_risk) ||
      !std::isfinite(uniform01) || uniform01 < 0 || uniform01 > 1) return {};
  auto bound = p.base_delay.count();
  for (std::uint32_t n = 1; n < attempts && bound < p.max_delay.count(); ++n) {
    if (bound == 0) break;
    bound = bound > p.max_delay.count() / 2 ? p.max_delay.count() : bound * 2;
  }
  const long double jitter = static_cast<long double>(bound) * uniform01;
  // Rounding up avoids dispatch before the sampled delay; preserve endpoints.
  const auto delay = jitter >= static_cast<long double>(max_ms) ? Ms::max() : Ms(static_cast<Ms::rep>(std::ceil(jitter)));
  if (e.retry_after && *e.retry_after < Ms::zero()) return {};
  const auto when = add(now, std::max(delay, e.retry_after.value_or(Ms::zero())));
  if (when == SteadyTime::max() || when >= deadline) return {};
  return when;
}

TokenBucket::TokenBucket(std::size_t capacity, double rate, SteadyTime now)
    : capacity_(static_cast<double>(capacity)), tokens_(capacity_),
      refill_per_second_(std::isfinite(rate) && rate > 0 ? rate : 0), last_(now) {}
void TokenBucket::refill(SteadyTime now) {
  if (now <= last_) return;
  const long double ticks = static_cast<long double>(now.time_since_epoch().count()) -
                            static_cast<long double>(last_.time_since_epoch().count());
  const long double seconds = ticks * SteadyTime::duration::period::num / SteadyTime::duration::period::den;
  tokens_ = static_cast<double>(std::min(static_cast<long double>(capacity_),
      static_cast<long double>(tokens_) + seconds * refill_per_second_));
  last_ = now;
}
bool TokenBucket::available(SteadyTime now) { refill(now); return tokens_ >= 1; }
bool TokenBucket::consume(SteadyTime now) {
  refill(now);
  if (tokens_ < 1) return false;
  tokens_ -= 1;
  return true;
}
}  // namespace sp::runtime::detail

namespace sp::runtime {
RetryPolicy default_retry_policy(const configuration::RuntimePolicy& policy) noexcept {
  const auto& values = policy.defaults();
  return {values.retry_enabled, values.retry_allow_duplicate_billing_risk,
      static_cast<std::uint32_t>(values.retry_max_attempts),
      std::chrono::milliseconds(values.retry_base_delay_ms),
      std::chrono::milliseconds(values.retry_max_delay_ms)};
}
}
