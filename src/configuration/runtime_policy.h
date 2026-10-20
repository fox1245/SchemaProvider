#pragma once
#include "core/value.h"
#include "descriptor/descriptor.h"
#include "sp/config_defaults.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sp::configuration {
struct RuntimeValues {
  std::uint64_t workers = config_defaults::defaults_workers;
  std::uint64_t io_threads = config_defaults::defaults_io_threads;
  std::uint64_t resolver_threads = config_defaults::defaults_resolver_threads;
  std::uint64_t max_host_connections = config_defaults::defaults_max_host_connections;
  std::uint64_t max_head_bytes = config_defaults::defaults_max_head_bytes;
  std::uint64_t dns_ttl_seconds = config_defaults::defaults_dns_ttl_seconds;
  std::uint64_t max_operations = config_defaults::defaults_max_operations;
  std::uint64_t queued_body_chunks = config_defaults::defaults_queued_body_chunks;
  std::uint64_t queued_body_bytes = config_defaults::defaults_queued_body_bytes;
  std::uint64_t max_response_bytes = config_defaults::defaults_max_response_bytes;
  std::uint64_t max_error_bytes = config_defaults::defaults_max_error_bytes;
  std::uint64_t semantic_max_parts = config_defaults::defaults_semantic_max_parts;
  std::uint64_t semantic_max_content_bytes = config_defaults::defaults_semantic_max_content_bytes;
  std::uint64_t semantic_max_tool_bytes = config_defaults::defaults_semantic_max_tool_bytes;
  std::uint64_t semantic_max_json_depth = config_defaults::defaults_semantic_max_json_depth;
  std::uint64_t sse_max_line_bytes = config_defaults::defaults_sse_max_line_bytes;
  std::uint64_t sse_max_event_bytes = config_defaults::defaults_sse_max_event_bytes;
  std::uint64_t sse_max_total_bytes = config_defaults::defaults_sse_max_total_bytes;
  std::uint64_t default_timeout_ms = config_defaults::defaults_default_timeout_ms;
  std::uint64_t slow_callback_threshold_ms = config_defaults::defaults_slow_callback_threshold_ms;
  std::uint64_t retry_tokens = config_defaults::defaults_retry_tokens;
  double retry_tokens_per_second = config_defaults::defaults_retry_tokens_per_second;
  bool retry_enabled = config_defaults::defaults_retry_enabled;
  bool retry_allow_duplicate_billing_risk = config_defaults::defaults_retry_allow_duplicate_billing_risk;
  std::uint64_t retry_max_attempts = config_defaults::defaults_retry_max_attempts;
  std::uint64_t retry_base_delay_ms = config_defaults::defaults_retry_base_delay_ms;
  std::uint64_t retry_max_delay_ms = config_defaults::defaults_retry_max_delay_ms;
};
struct AdmissionValues {
  std::uint64_t workers = config_defaults::admission_workers;
  std::uint64_t io_threads = config_defaults::admission_io_threads;
  std::uint64_t resolver_threads = config_defaults::admission_resolver_threads;
  std::uint64_t max_host_connections = config_defaults::admission_max_host_connections;
  std::uint64_t max_head_bytes = config_defaults::admission_max_head_bytes;
  std::uint64_t dns_ttl_seconds = config_defaults::admission_dns_ttl_seconds;
  std::uint64_t max_operations = config_defaults::admission_max_operations;
  std::uint64_t queued_body_chunks = config_defaults::admission_queued_body_chunks;
  std::uint64_t queued_body_bytes = config_defaults::admission_queued_body_bytes;
  std::uint64_t max_response_bytes = config_defaults::admission_max_response_bytes;
  std::uint64_t max_error_bytes = config_defaults::admission_max_error_bytes;
  std::uint64_t semantic_max_parts = config_defaults::admission_semantic_max_parts;
  std::uint64_t semantic_max_content_bytes = config_defaults::admission_semantic_max_content_bytes;
  std::uint64_t semantic_max_tool_bytes = config_defaults::admission_semantic_max_tool_bytes;
  std::uint64_t semantic_max_json_depth = config_defaults::admission_semantic_max_json_depth;
  std::uint64_t sse_max_line_bytes = config_defaults::admission_sse_max_line_bytes;
  std::uint64_t sse_max_event_bytes = config_defaults::admission_sse_max_event_bytes;
  std::uint64_t sse_max_total_bytes = config_defaults::admission_sse_max_total_bytes;
  std::uint64_t default_timeout_ms = config_defaults::admission_default_timeout_ms;
  std::uint64_t slow_callback_threshold_ms = config_defaults::admission_slow_callback_threshold_ms;
  std::uint64_t retry_tokens = config_defaults::admission_retry_tokens;
  double retry_tokens_per_second = config_defaults::admission_retry_tokens_per_second;
  std::uint64_t retry_max_attempts = config_defaults::admission_retry_max_attempts;
  std::uint64_t retry_base_delay_ms = config_defaults::admission_retry_base_delay_ms;
  std::uint64_t retry_max_delay_ms = config_defaults::admission_retry_max_delay_ms;
  std::uint64_t total_threads = config_defaults::admission_total_threads;
  std::uint64_t api_key_bytes = config_defaults::admission_api_key_bytes;
  std::uint64_t configuration_bytes = config_defaults::admission_configuration_bytes;
  std::uint64_t configuration_depth = config_defaults::admission_configuration_depth;
  std::uint64_t error_json_depth = config_defaults::admission_error_json_depth;
};
struct StatusRule { int status; ErrorKind kind; RetryClass retry; };
struct CodeRule { std::string value; ErrorKind kind; RetryClass retry; };
struct FamilyRules {
  std::string family, root_error_type;
  std::vector<std::string> error_paths, code_fields;
  std::vector<CodeRule> codes;
};
enum class HeaderFormat { SecondsOrHttpDate, Milliseconds, Duration };
struct HeaderRule { std::string family, name; HeaderFormat format; };
struct ErrorPolicy {
  std::vector<StatusRule> statuses;
  std::vector<FamilyRules> families;
  std::vector<HeaderRule> headers;
};
class RuntimePolicy;
using PolicySnapshot = std::shared_ptr<const RuntimePolicy>;
using PolicyResult = std::variant<PolicySnapshot, descriptor::ConfigError>;
// An admission ceiling the document states above the largest std::size_t of the target (the shipped
// "no bound" ceilings are INT64_MAX; std::size_t is 32 bits on wasm32), admitted as that largest value.
struct CeilingClamp {
  std::string pointer;         // JSON pointer of the ceiling, e.g. "/admission/max_response_bytes"
  std::uint64_t configured;    // the value the document states
  std::uint64_t admitted;      // the largest std::size_t of the target
};
namespace detail {
// load_runtime_policy with the largest size spelled out, so a test can load for a 32-bit target on a 64-bit host.
PolicyResult load_runtime_policy_for_size(std::string_view runtime_json, std::string_view error_json,
                                          std::uint64_t size_max);
}
class RuntimePolicy final {
 public:
  const RuntimeValues& defaults() const noexcept { return defaults_; }
  const AdmissionValues& admission() const noexcept { return admission_; }
  const ErrorPolicy& errors() const noexcept { return errors_; }
  // The ceilings the loader clamped to the target's std::size_t; empty on a 64-bit target and for any document
  // whose ceilings fit. A clamped ceiling is as unbounded in practice as the document intended, so the policy is
  // admitted; a host that wants to see or refuse the clamp reads it here.
  const std::vector<CeilingClamp>& clamps() const noexcept { return clamps_; }
 private:
  RuntimePolicy() = default;
  RuntimeValues defaults_;
  AdmissionValues admission_;
  ErrorPolicy errors_;
  std::vector<CeilingClamp> clamps_;
  friend PolicyResult detail::load_runtime_policy_for_size(std::string_view, std::string_view, std::uint64_t);
};
// Both documents are complete, closed version-1 objects. Reload creates a new
// snapshot; existing clients retain their admitted values and classification. An admission ceiling above the
// target's largest std::size_t is clamped to it and listed in RuntimePolicy::clamps(); a default above its
// (clamped) ceiling is rejected.
PolicyResult load_runtime_policy(std::string_view runtime_json, std::string_view error_json);
PolicyResult load_runtime_policy_files(const std::string& runtime_path, const std::string& error_path);
PolicySnapshot builtin_runtime_policy();
} // namespace sp::configuration
