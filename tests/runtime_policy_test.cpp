#include "runtime/policy.h"

#include <cstdio>
#include <limits>

using namespace sp;
using namespace sp::runtime;
using namespace sp::runtime::detail;
using namespace std::chrono_literals;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)
const SteadyTime receipt{100s};
const WallTime wall{std::chrono::sys_days(std::chrono::year(2026)/10/1).time_since_epoch()};
ResponseInfo inspect(int status, std::string_view body = {}, std::vector<transport::Header> headers = {},
                     std::string_view family = "openai.chat") {
  return inspect_response(*configuration::builtin_runtime_policy(), family, status, headers, body, receipt, wall);
}
Error transport_error(transport::Stage stage = transport::Stage::Connected) {
  transport::Result result;
  result.failure = transport::FailureKind::Receive;
  result.attempt.reached = stage;
  return classify_failure(result, {}, {}, false, 1, receipt);
}
RetryPolicy enabled() { return {true, false, 3, 100ms, 5s}; }

void classification() {
  auto quota = inspect(429, R"({"error":{"code":"insufficient_quota","type":"rate_limit_exceeded","message":"SECRET"}})", {{"Retry-After", "1"}});
  CHECK(quota.kind == ErrorKind::QuotaExhausted && quota.retry_class == RetryClass::Never);
  auto reverse = inspect(503, R"({"error":{"type":"insufficient_quota","code":"rate_limit_exceeded"}})");
  CHECK(reverse.kind == ErrorKind::QuotaExhausted);
  CHECK(inspect(429).kind == ErrorKind::LimitUnknown);
  CHECK(inspect(429).retry_class == RetryClass::Unknown);
  CHECK(inspect(429, R"({"error":{"code":"SECRET","message":"insufficient_quota"}})").vendor_code.empty());
  CHECK(inspect(429, R"({"error":{"code":"insufficient_quota","code":"rate_limit_exceeded"}})").kind == ErrorKind::LimitUnknown);
  CHECK(inspect(429, R"({"error":{"code":"rate_limit_exceeded"}})").kind == ErrorKind::RateLimited);
  CHECK(inspect(200, R"({"error":{"type":"overloaded_error"}})", {}, "anthropic.messages").kind == ErrorKind::Overloaded);
  CHECK(inspect(429, R"({"error":{"type":"overloaded_error"}})").kind == ErrorKind::LimitUnknown);
  CHECK(inspect(429, R"({"error":{"code":"insufficient_quota"}})", {}, "unknown").vendor_code.empty());
  CHECK(inspect(401).kind == ErrorKind::Authentication);
  CHECK(inspect(403).kind == ErrorKind::Permission);
  CHECK(inspect(404).kind == ErrorKind::NotFound);

  transport::Result result;
  result.status = transport::Status::Cancelled; // Runtime aborted after detecting corruption.
  result.http_status = 503;
  result.attempt = {transport::Stage::ResponseStarted, 271, true, true, 2};
  result.detail = "SECRET transport URL/header/body";
  for (auto cause : {ErrorKind::ProtocolCorrupt, ErrorKind::ResourceLimit, ErrorKind::Misuse,
                     ErrorKind::Cancelled, ErrorKind::DeadlineExceeded}) {
    auto e = classify_failure(result, inspect(503), Error{cause, "SECRET semantic detail"}, false, 4, receipt);
    CHECK(e.kind == cause && e.retry_class == RetryClass::Never);
    CHECK(e.attempt.request_body_bytes == 271 && e.attempt.response_head_seen);
    CHECK(e.attempt.transport_internal_resends == 2 && e.attempt.attempts == 4);
    CHECK(e.safe_message.find("SECRET") == std::string::npos);
  }
  auto e = classify_failure(result, quota, Error{ErrorKind::RemoteFailure, "SECRET"}, false, 4, receipt);
  CHECK(e.kind == ErrorKind::QuotaExhausted && e.retry_safety == RetrySafety::PossiblyAccepted);
  result.status = transport::Status::Failed;
  result.failure = transport::FailureKind::Protocol;
  CHECK(classify_failure(result, inspect(503), {}, false, 1, receipt).kind == ErrorKind::ProtocolCorrupt);
  result.status = transport::Status::Completed;
  result.failure = transport::FailureKind::None;
  result.http_status = 429;
  auto unknown = classify_failure(result, inspect(429), {}, false, 1, receipt);
  CHECK(unknown.kind == ErrorKind::LimitUnknown && unknown.retry_class == RetryClass::Unknown);
}

void safety_and_budget() {
  auto p = enabled();
  const auto deadline = receipt + 10s;
  auto e = transport_error();
  CHECK(e.retry_safety == RetrySafety::NotSent);
  CHECK(retry_at(e, p, 1, receipt, deadline, 0) == receipt);
  auto disabled = p; disabled.enabled = false;
  CHECK(!retry_at(e, disabled, 1, receipt, deadline, 0));
  e = transport_error(transport::Stage::RequestStarted);
  CHECK(e.attempt.request_body_bytes == 0 && e.attempt.request_may_have_left);
  CHECK(e.retry_safety == RetrySafety::PossiblyAccepted);
  CHECK(!retry_at(e, p, 1, receipt, deadline, 0));
  p.allow_duplicate_billing_risk = true;
  CHECK(retry_at(e, p, 1, receipt, deadline, 0).has_value());
  e.retry_safety = RetrySafety::OutputObserved;
  CHECK(!retry_at(e, p, 1, receipt, deadline, 0));
  transport::Result result;
  result.failure = transport::FailureKind::Receive;
  CHECK(classify_failure(result, {}, {}, true, 1, receipt).retry_safety == RetrySafety::OutputObserved);
  CHECK(classify_failure(result, {}, e, false, 1, receipt).retry_safety == RetrySafety::OutputObserved);
  for (auto stage : {transport::Stage::Queued, transport::Stage::Connected, transport::Stage::RequestStarted, transport::Stage::ResponseStarted}) {
    result.attempt.reached = stage;
    result.http_status = 429;
    auto limited = classify_failure(result, inspect(429, R"({"error":{"code":"rate_limit_exceeded"}})"), {}, false, 1, receipt);
    CHECK(limited.retry_safety == RetrySafety::PossiblyAccepted);
  }
  e = transport_error();
  CHECK(!retry_at(e, p, 3, receipt, deadline, 0));
  e.attempt.attempts = 3; // Caller already counted internal resend attempts.
  CHECK(!retry_at(e, p, 1, receipt, deadline, 0));
  e.attempt.attempts = 1;
  for (auto kind : {ErrorKind::QuotaExhausted, ErrorKind::Cancelled, ErrorKind::DeadlineExceeded,
                    ErrorKind::ResourceLimit, ErrorKind::Misuse, ErrorKind::ProtocolCorrupt, ErrorKind::LimitUnknown}) {
    e.kind = kind; e.retry_class = RetryClass::Transient;
    CHECK(!retry_at(e, p, 1, receipt, deadline, 0));
  }
}

void hints() {
  auto p = enabled(); p.allow_duplicate_billing_risk = true;
  auto hinted = [&](std::vector<transport::Header> headers, SteadyTime now = receipt) {
    transport::Result r; r.status = transport::Status::Completed; r.http_status = 503;
    return classify_failure(r, inspect(503, {}, std::move(headers)), {}, false, 1, now);
  };
  auto e = hinted({{"Retry-After", "2"}}, receipt + 500ms);
  CHECK(e.retry_after == 1500ms);
  CHECK(retry_at(e, p, 1, receipt + 500ms, receipt + 3s, 0) == receipt + 2s);
  CHECK(!retry_at(e, p, 1, receipt + 500ms, receipt + 2s, 0));
  e = hinted({{"Retry-After", "Thu, 01 Oct 2026 00:00:02 GMT"}});
  CHECK(e.retry_after == 2s);
  auto fractional_receipt = inspect_response(*configuration::builtin_runtime_policy(), "openai.chat", 503, {{"Retry-After", "Thu, 01 Oct 2026 00:00:02 GMT"}}, {}, receipt, wall + 500us);
  CHECK(fractional_receipt.retry_not_before == receipt + 2s); // Never round a minimum down.
  CHECK(inspect(503, {}, {{"Retry-After", "Wed, 30 Sep 2026 23:59:59 GMT"}}).retry_not_before == receipt);
  e = hinted({{"retry-after-ms", "0.001"}});
  CHECK(e.retry_after == 1ms);
  e = hinted({{"retry-after-ms", "1250.25"}});
  CHECK(e.retry_after == 1251ms);
  e = hinted({{"x-ratelimit-reset-requests", "1m2.003s"}, {"x-ratelimit-reset-tokens", "3s"}, {"Retry-After", "2"}});
  CHECK(e.retry_after == 62003ms);
  e = hinted({{"x-ratelimit-reset-tokens", "1h2m3s4ms"}});
  CHECK(e.retry_after == 3723004ms);
  e = hinted({{"Retry-After", "1"}, {"retry-after", "3"}});
  CHECK(e.retry_after == 3s);
  for (auto value : {"", "-1", "+2", "1.5", "NaN", "1e3", "18446744073709551616", "Thu, 31 Feb 2026 00:00:00 GMT", "Wed, 01 Oct 2026 00:00:00 GMT"}) {
    e = hinted({{"Retry-After", value}, {"Retry-After", "1"}});
    CHECK(e.retry_after == std::chrono::milliseconds::max());
    CHECK(!retry_at(e, p, 1, receipt, SteadyTime::max(), 0));
  }
  for (auto value : {"", "1ms1s", "1s1s", ".2s", "1.2m", "1q", "999999999999999999999s"}) {
    e = hinted({{"x-ratelimit-reset-tokens", value}});
    CHECK(!retry_at(e, p, 1, receipt, SteadyTime::max(), 0));
  }
  CHECK(!inspect(503, {}, {{"retry-after-ms", "SECRET"}}, "anthropic.messages").retry_not_before);
  auto extreme = inspect_response(*configuration::builtin_runtime_policy(), "openai.chat", 503, {{"Retry-After", "1"}}, {}, SteadyTime::max() - 1ms, wall);
  CHECK(extreme.retry_not_before == SteadyTime::max());
}

void arithmetic_and_bucket() {
  auto e = transport_error(); auto p = enabled();
  CHECK(retry_at(e, p, 1, receipt, receipt + 1s, 1) == receipt + 100ms);
  CHECK(retry_at(e, p, 2, receipt, receipt + 1s, .5) == receipt + 100ms);
  p.max_attempts = std::numeric_limits<std::uint32_t>::max();
  CHECK(retry_at(e, p, p.max_attempts - 1, receipt, receipt + 6s, 1) == receipt + 5s);
  CHECK(!retry_at(e, p, 1, receipt, receipt, 0));
  CHECK(!retry_at(e, p, 1, receipt, receipt + 100ms, 1));
  CHECK(!retry_at(e, p, 1, SteadyTime::max() - 1ms, SteadyTime::max(), 1));
  CHECK(!retry_at(e, p, 1, receipt, SteadyTime::max(), std::numeric_limits<double>::quiet_NaN()));
  CHECK(!retry_at(e, p, 1, receipt, SteadyTime::max(), -0.1));
  CHECK(!retry_at(e, p, 1, receipt, SteadyTime::max(), 1.1));
  p.base_delay = std::chrono::milliseconds::max(); p.max_delay = p.base_delay;
  CHECK(!retry_at(e, p, 2, receipt, SteadyTime::max(), 1));
  p.base_delay = 0ms; p.max_delay = 0ms;
  CHECK(retry_at(e, p, p.max_attempts - 1, receipt, receipt + 1ms, 1) == receipt);
  p.base_delay = -1ms; CHECK(!valid_retry_policy(p));

  TokenBucket bucket(2, 2, receipt);
  CHECK(bucket.available(receipt) && bucket.available(receipt));
  CHECK(bucket.consume(receipt) && bucket.consume(receipt));
  CHECK(!bucket.consume(receipt));
  CHECK(!bucket.available(receipt + 499ms));
  CHECK(bucket.consume(receipt + 500ms));
  CHECK(!bucket.consume(receipt + 500ms));
  CHECK(!bucket.consume(receipt - 1s)); // Backwards reads cannot mint credit.
  CHECK(bucket.consume(receipt + 1s));
  CHECK(bucket.consume(receipt + 100s) && bucket.consume(receipt + 100s));
  CHECK(!bucket.consume(receipt + 100s)); // Refill is capped at capacity.
  TokenBucket empty(0, 1000, receipt);
  CHECK(!empty.consume(SteadyTime::max()));
  TokenBucket fixed(1, 0, receipt);
  CHECK(fixed.consume(receipt)); CHECK(!fixed.consume(SteadyTime::max()));
  TokenBucket extremes(1, 1, SteadyTime::min());
  CHECK(extremes.consume(SteadyTime::min())); CHECK(extremes.consume(SteadyTime::max()));
}
}  // namespace
int main() {
  classification(); safety_and_budget(); hints(); arithmetic_and_bucket();
  if (failures) return 1;
  std::puts("runtime policy: classification, safety, hints, arithmetic and bucket passed");
}
