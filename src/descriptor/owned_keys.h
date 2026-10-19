#pragma once
// The top-level request members each family's encoder writes or reserves: typed controls, their aliases, and
// provider-held state the SDK does not model. A policy extension (#32) may never name one of them, in any letter case.
// tests/extension_test.cpp checks the lists against what the encoders write.
#include <algorithm>
#include <initializer_list>
#include <string_view>

namespace sp::descriptor {
inline bool request_key_owned(std::string_view family, std::string_view key) noexcept {
  auto lower_equal = [](std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
      return (x >= 'A' && x <= 'Z' ? static_cast<char>(x + 32) : x) == (y >= 'A' && y <= 'Z' ? static_cast<char>(y + 32) : y);
    });
  };
  auto any = [&](std::initializer_list<std::string_view> keys) {
    return std::any_of(keys.begin(), keys.end(), [&](std::string_view owned) { return lower_equal(owned, key); });
  };
  // Every family: the members a descriptor binding may name, and the extension slot itself.
  if (any({"model", "messages", "input", "contents", "stream", "stream_options", "max_tokens", "max_output_tokens",
           "max_completion_tokens", "temperature", "top_p", "tools", "tool_choice", "provider", "extensions"})) return true;
  if (family == "openai.chat")
    return any({"reasoning_effort", "service_tier", "response_format", "modalities", "audio", "image_config", "reasoning",
                "include_reasoning", "usage", "models", "plugins", "functions", "function_call"});
  if (family == "anthropic.messages")
    return any({"system", "thinking", "output_config", "cache_control", "container", "mcp_servers", "metadata"});
  if (family == "openai.responses")
    return any({"store", "previous_response_id", "include", "instructions", "max_tool_calls", "reasoning", "service_tier",
                "text", "parallel_tool_calls", "truncation", "modalities", "image_config", "plugins", "conversation",
                "background", "prompt"});
  if (family == "google.generate")
    return any({"generationConfig", "generation_config", "safetySettings", "safety_settings", "systemInstruction",
                "system_instruction", "toolConfig", "tool_config", "cachedContent", "cached_content"});
  if (family == "google.interactions")
    return any({"service_tier", "agent", "store", "background", "previous_interaction_id", "generation_config", "system_instruction",
                "response_format", "response_modalities", "agent_config"});
  return true;
}
} // namespace sp::descriptor
