#pragma once

#include "descriptor/descriptor.h"
#include "json/json.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sp::descriptor::presets {

struct PresetOptions {
  std::string id{};
  std::uint64_t revision = 1;
  std::string base_url{};
  std::vector<std::pair<std::string, std::string>> headers{};
  PolicySnapshot policy{};
};

namespace detail {
inline LoadResult build_preset(std::string_view default_id,
                               std::string_view family,
                               std::string_view default_base_url,
                               std::string_view buffered_path,
                               std::string_view streaming_path,
                               PresetOptions options) {
  const auto id_value = options.id.empty() ? default_id : std::string_view(options.id);
  const auto origin_value = options.base_url.empty() ? default_base_url : std::string_view(options.base_url);
  const auto quoted_id = json::quote(id_value);
  const auto quoted_family = json::quote(family);
  const auto quoted_origin = json::quote(origin_value);
  const auto quoted_buffered = json::quote(buffered_path);
  const auto quoted_streaming = json::quote(streaming_path);
  if (quoted_id.empty() || quoted_family.empty() || quoted_origin.empty() ||
      quoted_buffered.empty() || quoted_streaming.empty()) {
    return ConfigError{"", "valid UTF-8 preset configuration", options.revision,
                       "descriptor preset requires valid UTF-8 strings"};
  }
  const std::string source =
      "{\"descriptor_version\":1,\"revision\":" + std::to_string(options.revision) +
      ",\"id\":" + quoted_id +
      ",\"family\":" + quoted_family +
      ",\"connection\":{\"base_url\":" + quoted_origin +
      ",\"paths\":{\"buffered\":" + quoted_buffered +
      ",\"streaming\":" + quoted_streaming + "}}}";
  if (options.headers.empty()) {
    return options.policy ? load(source, std::move(options.policy)) : load(source);
  }
  return load_with_deployment_headers(source, options.headers, {}, std::move(options.policy));
}

inline bool valid_model_segment(std::string_view model) noexcept {
  if (model.empty() || model.size() > 128) return false;
  for (const unsigned char c : model) {
    if (c <= 32 || c == 127 || c == '/' || c == '\\' || c == '?' || c == '#' || c == ':')
      return false;
  }
  return true;
}
}  // namespace detail

inline ValidatedDescriptor unwrap(LoadResult result) {
  if (auto* error = std::get_if<ConfigError>(&result)) {
    throw std::invalid_argument(error->message);
  }
  return std::get<ValidatedDescriptor>(std::move(result));
}

inline LoadResult try_openai_chat(PresetOptions options = {}) {
  return detail::build_preset("openai-chat", "openai.chat", "https://api.openai.com",
                              "/v1/chat/completions", "/v1/chat/completions", std::move(options));
}
inline ValidatedDescriptor openai_chat(PresetOptions options = {}) {
  return unwrap(try_openai_chat(std::move(options)));
}

inline LoadResult try_openai_responses(PresetOptions options = {}) {
  return detail::build_preset("openai-responses", "openai.responses", "https://api.openai.com",
                              "/v1/responses", "/v1/responses", std::move(options));
}
inline ValidatedDescriptor openai_responses(PresetOptions options = {}) {
  return unwrap(try_openai_responses(std::move(options)));
}

inline LoadResult try_anthropic_messages(PresetOptions options = {}) {
  return detail::build_preset("anthropic-messages", "anthropic.messages", "https://api.anthropic.com",
                              "/v1/messages", "/v1/messages", std::move(options));
}
inline ValidatedDescriptor anthropic_messages(PresetOptions options = {}) {
  return unwrap(try_anthropic_messages(std::move(options)));
}

inline LoadResult try_google_generate(std::string_view model, PresetOptions options = {}) {
  if (!detail::valid_model_segment(model)) {
    return ConfigError{"/connection/paths/buffered", "concrete model path segment", options.revision,
                       "google.generate preset requires a bounded single-segment model identifier"};
  }
  const std::string prefix = "/v1beta/models/" + std::string(model);
  return detail::build_preset("google-generate", "google.generate",
                              "https://generativelanguage.googleapis.com",
                              prefix + ":generateContent",
                              prefix + ":streamGenerateContent?alt=sse",
                              std::move(options));
}
inline ValidatedDescriptor google_generate(std::string_view model, PresetOptions options = {}) {
  return unwrap(try_google_generate(model, std::move(options)));
}
// One Generate descriptor for every model of the origin: the request's model fills `{model}`.
inline LoadResult try_google_generate(PresetOptions options = {}) {
  return detail::build_preset("google-generate", "google.generate",
                              "https://generativelanguage.googleapis.com",
                              "/v1beta/models/{model}:generateContent",
                              "/v1beta/models/{model}:streamGenerateContent?alt=sse",
                              std::move(options));
}
inline ValidatedDescriptor google_generate(PresetOptions options = {}) {
  return unwrap(try_google_generate(std::move(options)));
}

inline LoadResult try_google_interactions(PresetOptions options = {}) {
  return detail::build_preset("google-interactions", "google.interactions",
                              "https://generativelanguage.googleapis.com",
                              "/v1beta/interactions", "/v1beta/interactions", std::move(options));
}
inline ValidatedDescriptor google_interactions(PresetOptions options = {}) {
  return unwrap(try_google_interactions(std::move(options)));
}

inline LoadResult try_openrouter_chat(PresetOptions options = {}) {
  return detail::build_preset("openrouter-chat", "openai.chat", "https://openrouter.ai",
                              "/api/v1/chat/completions", "/api/v1/chat/completions", std::move(options));
}
inline ValidatedDescriptor openrouter_chat(PresetOptions options = {}) {
  return unwrap(try_openrouter_chat(std::move(options)));
}

inline LoadResult try_openrouter_responses(PresetOptions options = {}) {
  return detail::build_preset("openrouter-responses", "openai.responses", "https://openrouter.ai",
                              "/api/v1/responses", "/api/v1/responses", std::move(options));
}
inline ValidatedDescriptor openrouter_responses(PresetOptions options = {}) {
  return unwrap(try_openrouter_responses(std::move(options)));
}

inline LoadResult try_openrouter_messages(PresetOptions options = {}) {
  return detail::build_preset("openrouter-messages", "anthropic.messages", "https://openrouter.ai",
                              "/api/v1/messages", "/api/v1/messages", std::move(options));
}
inline ValidatedDescriptor openrouter_messages(PresetOptions options = {}) {
  return unwrap(try_openrouter_messages(std::move(options)));
}

inline LoadResult try_loopback_chat(std::string_view base_url = "http://127.0.0.1:8080",
                                    std::string_view path = "/v1/chat/completions",
                                    PresetOptions options = {}) {
  if (options.base_url.empty()) options.base_url = std::string(base_url);
  return detail::build_preset("loopback-chat", "openai.chat", base_url,
                              path, path, std::move(options));
}
inline ValidatedDescriptor loopback_chat(std::string_view base_url = "http://127.0.0.1:8080",
                                         std::string_view path = "/v1/chat/completions",
                                         PresetOptions options = {}) {
  return unwrap(try_loopback_chat(base_url, path, std::move(options)));
}

}  // namespace sp::descriptor::presets
