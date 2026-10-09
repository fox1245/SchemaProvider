#pragma once
#include <string_view>

namespace sp::responses {
inline bool snapshot_date(std::string_view date) {
  if (date.size() != 10 || date[4] != '-' || date[7] != '-') return false;
  for (size_t i = 0; i < date.size(); ++i)
    if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) return false;
  const unsigned year = static_cast<unsigned>((date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 + date[3] - '0');
  const unsigned month = static_cast<unsigned>((date[5] - '0') * 10 + date[6] - '0');
  const unsigned day = static_cast<unsigned>((date[8] - '0') * 10 + date[9] - '0');
  if (!year || month < 1 || month > 12 || day < 1) return false;
  constexpr unsigned days[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return day <= days[month - 1] + (month == 2 && leap ? 1U : 0U);
}
inline bool requested_model_matches(std::string_view requested, std::string_view served, std::string_view origin) {
  if (requested == served) return true;
  // Snapshot alias admission is restricted to the direct documented OpenAI
  // origin. Gateway namespaces and explicit snapshots retain exact equality.
  if (origin != "https://api.openai.com" || requested.empty()) return false;
  for (const char c : requested)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
  if (requested.size() >= 11 && requested[requested.size() - 11] == '-' && snapshot_date(requested.substr(requested.size() - 10))) return false;
  return served.size() == requested.size() + 11 && served.starts_with(requested) &&
      served[requested.size()] == '-' && snapshot_date(served.substr(requested.size() + 1));
}
} // namespace sp::responses
