#include "codecs/webhooks.h"
#include "codecs/reason.h"
#include "core/media.h"
#include "crypto/crypto.h"
#include <algorithm>

namespace sp::webhooks {
namespace {
Error reject(ErrorKind kind, std::string text) {
  Error error{kind, std::move(text)};
  error.reason = std::string(reason::code(reason::Source::Webhooks, error.safe_message));
  return error;
}
std::string_view header(const std::vector<std::pair<std::string, std::string>>& headers, std::string_view name) {
  for (const auto& [key, value] : headers)
    if (key.size() == name.size() && std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; }))
      return value;
  return {};
}
std::string text_of(json::Value value) { return value.is_string() ? std::string(value.as_string()) : std::string(); }
}  // namespace

std::variant<Event, Error> verify(std::string_view secret, const std::vector<std::pair<std::string, std::string>>& headers,
                                  std::string_view body, std::uint64_t now, std::uint64_t tolerance) {
  // The key is the base64 after `whsec_`.
  if (secret.starts_with("whsec_")) secret.remove_prefix(6);
  std::string key;
  if (secret.empty() || !base64_decode(secret, key) || key.empty()) return reject(ErrorKind::InvalidRequest, "a webhook secret is whsec_ and base64");
  const auto id = header(headers, "webhook-id"), timestamp = header(headers, "webhook-timestamp"), signatures = header(headers, "webhook-signature");
  if (id.empty() || timestamp.empty() || signatures.empty()) return reject(ErrorKind::InvalidRequest, "a webhook carries its id, timestamp and signature headers");
  if (timestamp.size() > 19 || !std::all_of(timestamp.begin(), timestamp.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return reject(ErrorKind::InvalidRequest, "a webhook timestamp is Unix seconds");
  const auto sent = std::stoull(std::string(timestamp));
  if ((sent > now && sent - now > tolerance) || (now > sent && now - sent > tolerance)) return reject(ErrorKind::InvalidRequest, "the webhook timestamp is outside the tolerance");
  std::string signed_content;
  signed_content.append(id).append(".").append(timestamp).append(".").append(body);
  const auto digest = crypto::hmac_sha256(key, signed_content);
  const auto expected = base64_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
  crypto::cleanse(key.data(), key.size());
  // The header lists `v1,<signature>` entries separated by spaces; one match is enough.
  bool matched = false;
  for (std::size_t at = 0; at < signatures.size();) {
    const auto end = std::min(signatures.find(' ', at), signatures.size());
    const auto entry = signatures.substr(at, end - at);
    if (entry.starts_with("v1,") && crypto::constant_time_equal(entry.substr(3), expected)) matched = true;
    at = end + 1;
  }
  if (!matched) return reject(ErrorKind::InvalidRequest, "the webhook signature does not match");
  auto parsed = json::parse(body);
  if (!std::holds_alternative<json::Document>(parsed) || !std::get<json::Document>(parsed).root().is_object())
    return reject(ErrorKind::InvalidRequest, "a webhook body is a JSON object");
  Event event;
  event.raw = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = event.raw->root();
  event.id = std::string(id);
  event.event_id = text_of(root.get("id"));
  event.type = text_of(root.get("type"));
  if (root.get("created_at").is_uint()) event.created_at = root.get("created_at").as_uint();
  event.subject = text_of(root.get("data").get("id"));
  return event;
}
}  // namespace sp::webhooks
