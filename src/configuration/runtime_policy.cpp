#include "configuration/runtime_policy.h"
#include "json/json.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace sp::configuration {
namespace {
using json::Value;
[[noreturn]] void fail(std::string pointer, std::string expected) {
  throw descriptor::ConfigError{std::move(pointer), std::move(expected), 1, "configuration value rejected"};
}
void closed(Value value, std::initializer_list<std::string_view> keys, const std::string& pointer) {
  if (!value.is_object() || value.size() != keys.size()) fail(pointer, "closed object with required fields");
  for (const auto& member : value.members())
    if (std::find(keys.begin(), keys.end(), member.key) == keys.end()) fail(pointer, "closed object with required fields");
  for (auto key : keys) if (!value.get(key).valid()) fail(pointer, "closed object with required fields");
}
std::uint64_t integer(Value value, const std::string& pointer) {
  if (value.is_uint() && value.as_uint() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return value.as_uint();
  if (value.is_int() && value.as_int() >= 0) return static_cast<std::uint64_t>(value.as_int());
  fail(pointer, "nonnegative representable integer");
}
double rate(Value value, const std::string& pointer) {
  if (!value.is_number() || !std::isfinite(value.as_double()) || value.as_double() < 0) fail(pointer, "finite nonnegative number");
  return value.as_double();
}
bool boolean(Value value, const std::string& pointer) {
  if (!value.is_bool()) fail(pointer, "boolean");
  return value.as_bool();
}
std::string text(Value value, const std::string& pointer) {
  if (!value.is_string() || value.as_string().empty()) fail(pointer, "nonempty string");
  return std::string(value.as_string());
}
void array(Value value, const std::string& pointer) {
  if (!value.is_array()) fail(pointer, "array");
}
ErrorKind kind(Value value) {
  const auto name = text(value, "/errors/kind");
  if (name == "InvalidRequest") return ErrorKind::InvalidRequest;
  if (name == "Authentication") return ErrorKind::Authentication;
  if (name == "Permission") return ErrorKind::Permission;
  if (name == "NotFound") return ErrorKind::NotFound;
  if (name == "RateLimited") return ErrorKind::RateLimited;
  if (name == "QuotaExhausted") return ErrorKind::QuotaExhausted;
  if (name == "LimitUnknown") return ErrorKind::LimitUnknown;
  if (name == "Overloaded") return ErrorKind::Overloaded;
  if (name == "RemoteFailure") return ErrorKind::RemoteFailure;
  fail("/errors/kind", "remote failure classification");
}
RetryClass retry(Value value) {
  const auto name = text(value, "/errors/retry");
  if (name == "Never") return RetryClass::Never;
  if (name == "Transient") return RetryClass::Transient;
  if (name == "AfterReset") return RetryClass::AfterReset;
  if (name == "Unknown") return RetryClass::Unknown;
  fail("/errors/retry", "retry classification");
}
json::Document document(std::string_view source) {
  auto result = json::parse(source, {config_defaults::admission_configuration_bytes, config_defaults::admission_configuration_depth});
  if (!std::holds_alternative<json::Document>(result)) fail("", "strict bounded JSON without duplicate keys");
  return std::move(std::get<json::Document>(result));
}
void version(Value root) {
  if (integer(root.get("version"), "/version") != 1) fail("/version", "schema version 1");
}
std::vector<std::string> selections(Value value, std::initializer_list<std::string_view> allowed, const std::string& pointer) {
  array(value, pointer);
  if (!value.size()) fail(pointer, "nonempty selection array");
  std::vector<std::string> result;
  for (auto element : value.elements()) {
    auto name = text(element, pointer);
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end() || std::find(result.begin(), result.end(), name) != result.end()) fail(pointer, "unique admitted selections");
    result.push_back(std::move(name));
  }
  return result;
}
ErrorPolicy error_policy(Value root) {
  closed(root, {"version", "statuses", "families", "headers"}, "/errors");
  version(root);
  ErrorPolicy result;
  array(root.get("statuses"), "/errors/statuses");
  std::unordered_set<int> statuses;
  for (auto value : root.get("statuses").elements()) {
    closed(value, {"status", "kind", "retry"}, "/errors/statuses");
    auto status = integer(value.get("status"), "/errors/statuses/status");
    if (status < 300 || status > 599 || !statuses.insert(static_cast<int>(status)).second) fail("/errors/statuses/status", "unique failure HTTP status");
    result.statuses.push_back({static_cast<int>(status), kind(value.get("kind")), retry(value.get("retry"))});
  }
  array(root.get("families"), "/errors/families");
  std::unordered_set<std::string> families;
  for (auto value : root.get("families").elements()) {
    closed(value, {"family", "root_error_type", "error_paths", "code_fields", "codes"}, "/errors/families");
    auto family = text(value.get("family"), "/errors/families/family");
    if (!families.insert(family).second) fail("/errors/families/family", "unique family");
    FamilyRules rules{std::move(family), text(value.get("root_error_type"), "/errors/families/root_error_type"),
        selections(value.get("error_paths"), {"error", "response.error", "root_error"}, "/errors/families/error_paths"),
        selections(value.get("code_fields"), {"type", "code", "details.reason"}, "/errors/families/code_fields"), {}};
    array(value.get("codes"), "/errors/families/codes");
    std::unordered_set<std::string> codes;
    for (auto code : value.get("codes").elements()) {
      closed(code, {"value", "kind", "retry"}, "/errors/families/codes");
      auto name = text(code.get("value"), "/errors/families/codes/value");
      if (!codes.insert(name).second) fail("/errors/families/codes/value", "unique machine code");
      rules.codes.push_back({std::move(name), kind(code.get("kind")), retry(code.get("retry"))});
    }
    result.families.push_back(std::move(rules));
  }
  array(root.get("headers"), "/errors/headers");
  std::unordered_set<std::string> header_keys;
  for (auto value : root.get("headers").elements()) {
    closed(value, {"family", "name", "format"}, "/errors/headers");
    auto family = text(value.get("family"), "/errors/headers/family");
    auto name = text(value.get("name"), "/errors/headers/name");
    if ((family != "*" && !families.contains(family)) || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789!#$%&'*+-.^_`|~") != std::string::npos ||
        !header_keys.insert(family + "\n" + name).second) fail("/errors/headers", "unique admitted family and lowercase HTTP token");
    auto format = text(value.get("format"), "/errors/headers/format");
    HeaderFormat admitted;
    if (format == "seconds_or_http_date") admitted = HeaderFormat::SecondsOrHttpDate;
    else if (format == "milliseconds") admitted = HeaderFormat::Milliseconds;
    else if (format == "duration") admitted = HeaderFormat::Duration;
    else fail("/errors/headers/format", "admitted retry time format");
    result.headers.push_back({std::move(family), std::move(name), admitted});
  }
  return result;
}
void admit_depth(Value value, std::uint64_t limit, std::uint64_t enclosing = 0) {
  if (!value.is_object() && !value.is_array()) return;
  if (enclosing >= limit) fail("", "configuration within admitted nesting depth");
  if (value.is_object()) {
    for (auto member : value.members()) admit_depth(member.value, limit, enclosing + 1);
  } else {
    for (auto element : value.elements()) admit_depth(element, limit, enclosing + 1);
  }
}
std::string file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) fail("", "readable configuration file");
  std::string result;
  char buffer[4096];
  while (input) {
    input.read(buffer, sizeof buffer);
    const auto count = static_cast<std::size_t>(input.gcount());
    if (count > config_defaults::admission_configuration_bytes - result.size()) fail("", "bounded configuration file");
    result.append(buffer, count);
  }
  if (!input.eof()) fail("", "readable configuration file");
  return result;
}
} // namespace
PolicyResult load_runtime_policy(std::string_view runtime_json, std::string_view error_json) {
  try {
    auto runtime = document(runtime_json);
    auto errors = document(error_json);
    const auto root = runtime.root();
    closed(root, {"version", "defaults", "admission"}, "");
    version(root);
    auto policy = std::shared_ptr<RuntimePolicy>(new RuntimePolicy);
    auto d = root.get("defaults");
    auto a = root.get("admission");
    closed(d, {"workers", "io_threads", "resolver_threads", "max_host_connections", "max_head_bytes", "dns_ttl_seconds", "max_operations", "queued_body_chunks", "queued_body_bytes", "max_response_bytes", "max_error_bytes", "semantic_max_parts", "semantic_max_content_bytes", "semantic_max_tool_bytes", "semantic_max_json_depth", "sse_max_line_bytes", "sse_max_event_bytes", "sse_max_total_bytes", "default_timeout_ms", "slow_callback_threshold_ms", "retry_tokens", "retry_tokens_per_second", "retry_enabled", "retry_allow_duplicate_billing_risk", "retry_max_attempts", "retry_base_delay_ms", "retry_max_delay_ms"}, "/defaults");
    closed(a, {"workers", "io_threads", "resolver_threads", "max_host_connections", "max_head_bytes", "dns_ttl_seconds", "max_operations", "queued_body_chunks", "queued_body_bytes", "max_response_bytes", "max_error_bytes", "semantic_max_parts", "semantic_max_content_bytes", "semantic_max_tool_bytes", "semantic_max_json_depth", "sse_max_line_bytes", "sse_max_event_bytes", "sse_max_total_bytes", "default_timeout_ms", "slow_callback_threshold_ms", "retry_tokens", "retry_tokens_per_second", "retry_max_attempts", "retry_base_delay_ms", "retry_max_delay_ms", "total_threads", "api_key_bytes", "configuration_bytes", "configuration_depth", "error_json_depth"}, "/admission");
    policy->defaults_.workers = integer(d.get("workers"), "/defaults/workers");
    policy->defaults_.io_threads = integer(d.get("io_threads"), "/defaults/io_threads");
    policy->defaults_.resolver_threads = integer(d.get("resolver_threads"), "/defaults/resolver_threads");
    policy->defaults_.max_host_connections = integer(d.get("max_host_connections"), "/defaults/max_host_connections");
    policy->defaults_.max_head_bytes = integer(d.get("max_head_bytes"), "/defaults/max_head_bytes");
    policy->defaults_.dns_ttl_seconds = integer(d.get("dns_ttl_seconds"), "/defaults/dns_ttl_seconds");
    policy->defaults_.max_operations = integer(d.get("max_operations"), "/defaults/max_operations");
    policy->defaults_.queued_body_chunks = integer(d.get("queued_body_chunks"), "/defaults/queued_body_chunks");
    policy->defaults_.queued_body_bytes = integer(d.get("queued_body_bytes"), "/defaults/queued_body_bytes");
    policy->defaults_.max_response_bytes = integer(d.get("max_response_bytes"), "/defaults/max_response_bytes");
    policy->defaults_.max_error_bytes = integer(d.get("max_error_bytes"), "/defaults/max_error_bytes");
    policy->defaults_.semantic_max_parts = integer(d.get("semantic_max_parts"), "/defaults/semantic_max_parts");
    policy->defaults_.semantic_max_content_bytes = integer(d.get("semantic_max_content_bytes"), "/defaults/semantic_max_content_bytes");
    policy->defaults_.semantic_max_tool_bytes = integer(d.get("semantic_max_tool_bytes"), "/defaults/semantic_max_tool_bytes");
    policy->defaults_.semantic_max_json_depth = integer(d.get("semantic_max_json_depth"), "/defaults/semantic_max_json_depth");
    policy->defaults_.sse_max_line_bytes = integer(d.get("sse_max_line_bytes"), "/defaults/sse_max_line_bytes");
    policy->defaults_.sse_max_event_bytes = integer(d.get("sse_max_event_bytes"), "/defaults/sse_max_event_bytes");
    policy->defaults_.sse_max_total_bytes = integer(d.get("sse_max_total_bytes"), "/defaults/sse_max_total_bytes");
    policy->defaults_.default_timeout_ms = integer(d.get("default_timeout_ms"), "/defaults/default_timeout_ms");
    policy->defaults_.slow_callback_threshold_ms = integer(d.get("slow_callback_threshold_ms"), "/defaults/slow_callback_threshold_ms");
    policy->defaults_.retry_tokens = integer(d.get("retry_tokens"), "/defaults/retry_tokens");
    policy->defaults_.retry_tokens_per_second = rate(d.get("retry_tokens_per_second"), "/defaults/retry_tokens_per_second");
    policy->defaults_.retry_enabled = boolean(d.get("retry_enabled"), "/defaults/retry_enabled");
    policy->defaults_.retry_allow_duplicate_billing_risk = boolean(d.get("retry_allow_duplicate_billing_risk"), "/defaults/retry_allow_duplicate_billing_risk");
    policy->defaults_.retry_max_attempts = integer(d.get("retry_max_attempts"), "/defaults/retry_max_attempts");
    policy->defaults_.retry_base_delay_ms = integer(d.get("retry_base_delay_ms"), "/defaults/retry_base_delay_ms");
    policy->defaults_.retry_max_delay_ms = integer(d.get("retry_max_delay_ms"), "/defaults/retry_max_delay_ms");
    policy->admission_.workers = integer(a.get("workers"), "/admission/workers");
    policy->admission_.io_threads = integer(a.get("io_threads"), "/admission/io_threads");
    policy->admission_.resolver_threads = integer(a.get("resolver_threads"), "/admission/resolver_threads");
    policy->admission_.max_host_connections = integer(a.get("max_host_connections"), "/admission/max_host_connections");
    policy->admission_.max_head_bytes = integer(a.get("max_head_bytes"), "/admission/max_head_bytes");
    policy->admission_.dns_ttl_seconds = integer(a.get("dns_ttl_seconds"), "/admission/dns_ttl_seconds");
    policy->admission_.max_operations = integer(a.get("max_operations"), "/admission/max_operations");
    policy->admission_.queued_body_chunks = integer(a.get("queued_body_chunks"), "/admission/queued_body_chunks");
    policy->admission_.queued_body_bytes = integer(a.get("queued_body_bytes"), "/admission/queued_body_bytes");
    policy->admission_.max_response_bytes = integer(a.get("max_response_bytes"), "/admission/max_response_bytes");
    policy->admission_.max_error_bytes = integer(a.get("max_error_bytes"), "/admission/max_error_bytes");
    policy->admission_.semantic_max_parts = integer(a.get("semantic_max_parts"), "/admission/semantic_max_parts");
    policy->admission_.semantic_max_content_bytes = integer(a.get("semantic_max_content_bytes"), "/admission/semantic_max_content_bytes");
    policy->admission_.semantic_max_tool_bytes = integer(a.get("semantic_max_tool_bytes"), "/admission/semantic_max_tool_bytes");
    policy->admission_.semantic_max_json_depth = integer(a.get("semantic_max_json_depth"), "/admission/semantic_max_json_depth");
    policy->admission_.sse_max_line_bytes = integer(a.get("sse_max_line_bytes"), "/admission/sse_max_line_bytes");
    policy->admission_.sse_max_event_bytes = integer(a.get("sse_max_event_bytes"), "/admission/sse_max_event_bytes");
    policy->admission_.sse_max_total_bytes = integer(a.get("sse_max_total_bytes"), "/admission/sse_max_total_bytes");
    policy->admission_.default_timeout_ms = integer(a.get("default_timeout_ms"), "/admission/default_timeout_ms");
    policy->admission_.slow_callback_threshold_ms = integer(a.get("slow_callback_threshold_ms"), "/admission/slow_callback_threshold_ms");
    policy->admission_.retry_tokens = integer(a.get("retry_tokens"), "/admission/retry_tokens");
    policy->admission_.retry_tokens_per_second = rate(a.get("retry_tokens_per_second"), "/admission/retry_tokens_per_second");
    policy->admission_.retry_max_attempts = integer(a.get("retry_max_attempts"), "/admission/retry_max_attempts");
    policy->admission_.retry_base_delay_ms = integer(a.get("retry_base_delay_ms"), "/admission/retry_base_delay_ms");
    policy->admission_.retry_max_delay_ms = integer(a.get("retry_max_delay_ms"), "/admission/retry_max_delay_ms");
    policy->admission_.total_threads = integer(a.get("total_threads"), "/admission/total_threads");
    policy->admission_.api_key_bytes = integer(a.get("api_key_bytes"), "/admission/api_key_bytes");
    policy->admission_.configuration_bytes = integer(a.get("configuration_bytes"), "/admission/configuration_bytes");
    policy->admission_.configuration_depth = integer(a.get("configuration_depth"), "/admission/configuration_depth");
    policy->admission_.error_json_depth = integer(a.get("error_json_depth"), "/admission/error_json_depth");
    const auto& values = policy->defaults_;
    const auto& limits = policy->admission_;
    if (limits.workers <= 0) fail("/admission/workers", "positive admission ceiling");
    if (limits.io_threads <= 0) fail("/admission/io_threads", "positive admission ceiling");
    if (limits.resolver_threads <= 0) fail("/admission/resolver_threads", "positive admission ceiling");
    if (limits.max_head_bytes <= 0) fail("/admission/max_head_bytes", "positive admission ceiling");
    if (limits.max_operations <= 0) fail("/admission/max_operations", "positive admission ceiling");
    if (limits.queued_body_chunks <= 0) fail("/admission/queued_body_chunks", "positive admission ceiling");
    if (limits.queued_body_bytes <= 0) fail("/admission/queued_body_bytes", "positive admission ceiling");
    if (limits.max_response_bytes <= 0) fail("/admission/max_response_bytes", "positive admission ceiling");
    if (limits.max_error_bytes <= 0) fail("/admission/max_error_bytes", "positive admission ceiling");
    if (limits.semantic_max_parts <= 0) fail("/admission/semantic_max_parts", "positive admission ceiling");
    if (limits.semantic_max_content_bytes <= 0) fail("/admission/semantic_max_content_bytes", "positive admission ceiling");
    if (limits.semantic_max_tool_bytes <= 0) fail("/admission/semantic_max_tool_bytes", "positive admission ceiling");
    if (limits.semantic_max_json_depth <= 0) fail("/admission/semantic_max_json_depth", "positive admission ceiling");
    if (limits.sse_max_line_bytes <= 0) fail("/admission/sse_max_line_bytes", "positive admission ceiling");
    if (limits.sse_max_event_bytes <= 0) fail("/admission/sse_max_event_bytes", "positive admission ceiling");
    if (limits.sse_max_total_bytes <= 0) fail("/admission/sse_max_total_bytes", "positive admission ceiling");
    if (limits.default_timeout_ms <= 0) fail("/admission/default_timeout_ms", "positive admission ceiling");
    if (limits.retry_max_attempts <= 0) fail("/admission/retry_max_attempts", "positive admission ceiling");
    if (limits.total_threads <= 0) fail("/admission/total_threads", "positive admission ceiling");
    if (limits.api_key_bytes <= 0) fail("/admission/api_key_bytes", "positive admission ceiling");
    if (limits.configuration_bytes <= 0) fail("/admission/configuration_bytes", "positive admission ceiling");
    if (limits.configuration_depth <= 0) fail("/admission/configuration_depth", "positive admission ceiling");
    if (limits.error_json_depth <= 0) fail("/admission/error_json_depth", "positive admission ceiling");
    if (values.workers > limits.workers) fail("/defaults/workers", "value within admission ceiling");
    if (values.workers == 0) fail("/defaults/workers", "positive operational value");
    if (values.io_threads > limits.io_threads) fail("/defaults/io_threads", "value within admission ceiling");
    if (values.io_threads == 0) fail("/defaults/io_threads", "positive operational value");
    if (values.resolver_threads > limits.resolver_threads) fail("/defaults/resolver_threads", "value within admission ceiling");
    if (values.resolver_threads == 0) fail("/defaults/resolver_threads", "positive operational value");
    if (values.max_host_connections > limits.max_host_connections) fail("/defaults/max_host_connections", "value within admission ceiling");
    if (values.max_head_bytes > limits.max_head_bytes) fail("/defaults/max_head_bytes", "value within admission ceiling");
    if (values.max_head_bytes == 0) fail("/defaults/max_head_bytes", "positive operational value");
    if (values.dns_ttl_seconds > limits.dns_ttl_seconds) fail("/defaults/dns_ttl_seconds", "value within admission ceiling");
    if (values.max_operations > limits.max_operations) fail("/defaults/max_operations", "value within admission ceiling");
    if (values.max_operations == 0) fail("/defaults/max_operations", "positive operational value");
    if (values.queued_body_chunks > limits.queued_body_chunks) fail("/defaults/queued_body_chunks", "value within admission ceiling");
    if (values.queued_body_chunks == 0) fail("/defaults/queued_body_chunks", "positive operational value");
    if (values.queued_body_bytes > limits.queued_body_bytes) fail("/defaults/queued_body_bytes", "value within admission ceiling");
    if (values.queued_body_bytes == 0) fail("/defaults/queued_body_bytes", "positive operational value");
    if (values.max_response_bytes > limits.max_response_bytes) fail("/defaults/max_response_bytes", "value within admission ceiling");
    if (values.max_response_bytes == 0) fail("/defaults/max_response_bytes", "positive operational value");
    if (values.max_error_bytes > limits.max_error_bytes) fail("/defaults/max_error_bytes", "value within admission ceiling");
    if (values.max_error_bytes == 0) fail("/defaults/max_error_bytes", "positive operational value");
    if (values.semantic_max_parts > limits.semantic_max_parts) fail("/defaults/semantic_max_parts", "value within admission ceiling");
    if (values.semantic_max_parts == 0) fail("/defaults/semantic_max_parts", "positive operational value");
    if (values.semantic_max_content_bytes > limits.semantic_max_content_bytes) fail("/defaults/semantic_max_content_bytes", "value within admission ceiling");
    if (values.semantic_max_content_bytes == 0) fail("/defaults/semantic_max_content_bytes", "positive operational value");
    if (values.semantic_max_tool_bytes > limits.semantic_max_tool_bytes) fail("/defaults/semantic_max_tool_bytes", "value within admission ceiling");
    if (values.semantic_max_tool_bytes == 0) fail("/defaults/semantic_max_tool_bytes", "positive operational value");
    if (values.semantic_max_json_depth > limits.semantic_max_json_depth) fail("/defaults/semantic_max_json_depth", "value within admission ceiling");
    if (values.semantic_max_json_depth == 0) fail("/defaults/semantic_max_json_depth", "positive operational value");
    if (values.sse_max_line_bytes > limits.sse_max_line_bytes) fail("/defaults/sse_max_line_bytes", "value within admission ceiling");
    if (values.sse_max_line_bytes == 0) fail("/defaults/sse_max_line_bytes", "positive operational value");
    if (values.sse_max_event_bytes > limits.sse_max_event_bytes) fail("/defaults/sse_max_event_bytes", "value within admission ceiling");
    if (values.sse_max_event_bytes == 0) fail("/defaults/sse_max_event_bytes", "positive operational value");
    if (values.sse_max_total_bytes > limits.sse_max_total_bytes) fail("/defaults/sse_max_total_bytes", "value within admission ceiling");
    if (values.sse_max_total_bytes == 0) fail("/defaults/sse_max_total_bytes", "positive operational value");
    if (values.default_timeout_ms > limits.default_timeout_ms) fail("/defaults/default_timeout_ms", "value within admission ceiling");
    if (values.default_timeout_ms == 0) fail("/defaults/default_timeout_ms", "positive operational value");
    if (values.slow_callback_threshold_ms > limits.slow_callback_threshold_ms) fail("/defaults/slow_callback_threshold_ms", "value within admission ceiling");
    if (values.retry_tokens > limits.retry_tokens) fail("/defaults/retry_tokens", "value within admission ceiling");
    if (values.retry_tokens_per_second > limits.retry_tokens_per_second) fail("/defaults/retry_tokens_per_second", "value within admission ceiling");
    if (values.retry_max_attempts > limits.retry_max_attempts) fail("/defaults/retry_max_attempts", "value within admission ceiling");
    if (values.retry_max_attempts == 0) fail("/defaults/retry_max_attempts", "positive operational value");
    if (values.retry_base_delay_ms > limits.retry_base_delay_ms) fail("/defaults/retry_base_delay_ms", "value within admission ceiling");
    if (values.retry_max_delay_ms > limits.retry_max_delay_ms) fail("/defaults/retry_max_delay_ms", "value within admission ceiling");
    if (limits.io_threads > std::numeric_limits<unsigned>::max() || limits.resolver_threads > std::numeric_limits<unsigned>::max() ||
        limits.retry_max_attempts > std::numeric_limits<std::uint32_t>::max()) fail("/admission", "representable typed ceiling");
    if (limits.workers > std::numeric_limits<std::size_t>::max()) fail("/admission/workers", "representable size ceiling");
    if (limits.io_threads > std::numeric_limits<std::size_t>::max()) fail("/admission/io_threads", "representable size ceiling");
    if (limits.resolver_threads > std::numeric_limits<std::size_t>::max()) fail("/admission/resolver_threads", "representable size ceiling");
    if (limits.max_host_connections > std::numeric_limits<std::size_t>::max()) fail("/admission/max_host_connections", "representable size ceiling");
    if (limits.max_head_bytes > std::numeric_limits<std::size_t>::max()) fail("/admission/max_head_bytes", "representable size ceiling");
    if (limits.dns_ttl_seconds > std::numeric_limits<std::size_t>::max()) fail("/admission/dns_ttl_seconds", "representable size ceiling");
    if (limits.max_operations > std::numeric_limits<std::size_t>::max()) fail("/admission/max_operations", "representable size ceiling");
    if (limits.queued_body_chunks > std::numeric_limits<std::size_t>::max()) fail("/admission/queued_body_chunks", "representable size ceiling");
    if (limits.queued_body_bytes > std::numeric_limits<std::size_t>::max()) fail("/admission/queued_body_bytes", "representable size ceiling");
    if (limits.max_response_bytes > std::numeric_limits<std::size_t>::max()) fail("/admission/max_response_bytes", "representable size ceiling");
    if (limits.max_error_bytes > std::numeric_limits<std::size_t>::max()) fail("/admission/max_error_bytes", "representable size ceiling");
    if (limits.semantic_max_parts > std::numeric_limits<std::size_t>::max()) fail("/admission/semantic_max_parts", "representable size ceiling");
    if (limits.semantic_max_content_bytes > std::numeric_limits<std::size_t>::max()) fail("/admission/semantic_max_content_bytes", "representable size ceiling");
    if (limits.semantic_max_tool_bytes > std::numeric_limits<std::size_t>::max()) fail("/admission/semantic_max_tool_bytes", "representable size ceiling");
    if (limits.semantic_max_json_depth > std::numeric_limits<std::size_t>::max()) fail("/admission/semantic_max_json_depth", "representable size ceiling");
    if (limits.sse_max_line_bytes > std::numeric_limits<std::size_t>::max()) fail("/admission/sse_max_line_bytes", "representable size ceiling");
    if (limits.sse_max_event_bytes > std::numeric_limits<std::size_t>::max()) fail("/admission/sse_max_event_bytes", "representable size ceiling");
    if (limits.sse_max_total_bytes > std::numeric_limits<std::size_t>::max()) fail("/admission/sse_max_total_bytes", "representable size ceiling");
    if (limits.default_timeout_ms > std::numeric_limits<std::size_t>::max()) fail("/admission/default_timeout_ms", "representable size ceiling");
    if (limits.slow_callback_threshold_ms > std::numeric_limits<std::size_t>::max()) fail("/admission/slow_callback_threshold_ms", "representable size ceiling");
    if (limits.retry_tokens > std::numeric_limits<std::size_t>::max()) fail("/admission/retry_tokens", "representable size ceiling");
    if (limits.retry_max_attempts > std::numeric_limits<std::size_t>::max()) fail("/admission/retry_max_attempts", "representable size ceiling");
    if (limits.retry_base_delay_ms > std::numeric_limits<std::size_t>::max()) fail("/admission/retry_base_delay_ms", "representable size ceiling");
    if (limits.retry_max_delay_ms > std::numeric_limits<std::size_t>::max()) fail("/admission/retry_max_delay_ms", "representable size ceiling");
    if (limits.total_threads > std::numeric_limits<std::size_t>::max()) fail("/admission/total_threads", "representable size ceiling");
    if (limits.api_key_bytes > std::numeric_limits<std::size_t>::max()) fail("/admission/api_key_bytes", "representable size ceiling");
    if (limits.configuration_bytes > std::numeric_limits<std::size_t>::max()) fail("/admission/configuration_bytes", "representable size ceiling");
    if (limits.configuration_depth > std::numeric_limits<std::size_t>::max()) fail("/admission/configuration_depth", "representable size ceiling");
    if (limits.error_json_depth > std::numeric_limits<std::size_t>::max()) fail("/admission/error_json_depth", "representable size ceiling");
    if (values.workers > limits.total_threads || values.io_threads > limits.total_threads - values.workers ||
        values.resolver_threads > limits.total_threads - values.workers - values.io_threads) fail("/defaults", "thread total within admission");
    if (values.retry_base_delay_ms > values.retry_max_delay_ms) fail("/defaults", "ordered retry delays");
    if (limits.max_host_connections > static_cast<std::uint64_t>(std::numeric_limits<long>::max()) ||
        limits.dns_ttl_seconds > static_cast<std::uint64_t>(std::chrono::seconds::max().count()) ||
        limits.default_timeout_ms > static_cast<std::uint64_t>(std::chrono::milliseconds::max().count()) ||
        limits.slow_callback_threshold_ms > static_cast<std::uint64_t>(std::chrono::milliseconds::max().count()) ||
        limits.retry_base_delay_ms > static_cast<std::uint64_t>(std::chrono::milliseconds::max().count()) ||
        limits.retry_max_delay_ms > static_cast<std::uint64_t>(std::chrono::milliseconds::max().count()))
      fail("/admission", "representable operational duration or connection ceiling");
    if (runtime_json.size() > limits.configuration_bytes || error_json.size() > limits.configuration_bytes)
      fail("", "configuration within admitted byte limit");
    admit_depth(root, limits.configuration_depth);
    admit_depth(errors.root(), limits.configuration_depth);
    policy->errors_ = error_policy(errors.root());
    return PolicySnapshot(std::move(policy));
  } catch (descriptor::ConfigError& error) { return std::move(error); }
}
PolicyResult load_runtime_policy_files(const std::string& runtime_path, const std::string& error_path) {
  try { return load_runtime_policy(file(runtime_path), file(error_path)); }
  catch (descriptor::ConfigError& error) { return std::move(error); }
}
PolicySnapshot builtin_runtime_policy() {
  static const auto snapshot = [] {
    auto loaded = load_runtime_policy(config_defaults::runtime_defaults_json, config_defaults::error_policy_json);
    if (auto* value = std::get_if<PolicySnapshot>(&loaded)) return *value;
    throw std::logic_error("embedded runtime policy rejected");
  }();
  return snapshot;
}
} // namespace sp::configuration
