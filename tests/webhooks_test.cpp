// The webhook verifier (D7): the Standard Webhooks scheme OpenAI signs with, checked against the scheme's published
// example, an OpenAI event, and every way a delivery is refused.
#include "codecs/webhooks.h"
#include "core/media.h"
#include "crypto/crypto.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
using Headers = std::vector<std::pair<std::string, std::string>>;
std::string refused(std::string_view secret, const Headers& headers, std::string_view body, std::uint64_t now) {
  auto result = webhooks::verify(secret, headers, body, now);
  CHECK(std::holds_alternative<Error>(result));
  CHECK(std::get<Error>(result).kind == ErrorKind::InvalidRequest);
  return std::get<Error>(result).reason;
}
// The example of the Standard Webhooks specification.
constexpr std::string_view secret = "whsec_MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw";
constexpr std::string_view body = R"({"test": 2432232314})";
constexpr std::uint64_t sent = 1614265330;
const Headers headers{{"webhook-id", "msg_p5jXN8AQM9LWM0D4loKWxJek"}, {"webhook-timestamp", "1614265330"},
                      {"webhook-signature", "v1,g0hM9SsE+OTPJTGt/tmIKtSyZlE3uFJELVlNIOLJ1OE="}};

void published_example() {
  const auto event = std::get<webhooks::Event>(webhooks::verify(secret, headers, body, sent));
  CHECK(event.id == "msg_p5jXN8AQM9LWM0D4loKWxJek" && event.raw && event.raw->root().get("test").as_uint() == 2432232314u);
  // Header names in any case, several signatures (a rotated secret), and a clock within the tolerance.
  Headers rotated{{"Webhook-Id", headers[0].second}, {"WEBHOOK-TIMESTAMP", headers[1].second},
                  {"webhook-signature", "v1,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA= " + headers[2].second}};
  CHECK(std::holds_alternative<webhooks::Event>(webhooks::verify(secret, rotated, body, sent + 299)));
  CHECK(std::holds_alternative<webhooks::Event>(webhooks::verify(secret, headers, body, sent - 299)));
}

void refusals() {
  CHECK(refused(secret, headers, R"({"test": 2432232315})", sent) == "webhooks.signature.mismatch");
  CHECK(refused("whsec_" + std::string(sp::base64_encode("another key, same length")), headers, body, sent) == "webhooks.signature.mismatch");
  auto other_id = headers; other_id[0].second = "msg_other";
  CHECK(refused(secret, other_id, body, sent) == "webhooks.signature.mismatch");
  auto unversioned = headers; unversioned[2].second = headers[2].second.substr(3);
  CHECK(refused(secret, unversioned, body, sent) == "webhooks.signature.mismatch");
  CHECK(refused(secret, headers, body, sent + 301) == "webhooks.timestamp.stale");
  CHECK(refused(secret, headers, body, sent - 301) == "webhooks.timestamp.stale");
  auto letters = headers; letters[1].second = "16142653x0";
  CHECK(refused(secret, letters, body, sent) == "webhooks.timestamp.invalid");
  CHECK(refused(secret, Headers{headers[0], headers[1]}, body, sent) == "webhooks.headers.missing");
  CHECK(refused("whsec_not base64!", headers, body, sent) == "webhooks.secret.invalid");
  CHECK(refused("", headers, body, sent) == "webhooks.secret.invalid");
}

// An OpenAI event, signed as OpenAI signs it.
void openai_event() {
  const std::string key = "an endpoint secret of thirty-two";
  const std::string event_body = R"({"object":"event","id":"evt_685343a1381c819085d44c354e1b330e","type":"batch.completed","created_at":1792418975,"data":{"id":"batch_6ad6245d717c81908adf45db42ffb5b1"}})";
  const std::string id = "wh_685342e6c53c8190a1be43f081506c52", timestamp = "1792418976";
  const auto digest = crypto::hmac_sha256(key, id + "." + timestamp + "." + event_body);
  const Headers signed_headers{{"webhook-id", id}, {"webhook-timestamp", timestamp},
                               {"webhook-signature", "v1," + base64_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()))}};
  const auto event = std::get<webhooks::Event>(webhooks::verify("whsec_" + base64_encode(key), signed_headers, event_body, 1792418980));
  CHECK(event.id == id && event.event_id == "evt_685343a1381c819085d44c354e1b330e" && event.type == "batch.completed" &&
        event.created_at == 1792418975 && event.subject == "batch_6ad6245d717c81908adf45db42ffb5b1");
  // A correctly signed body that is not JSON.
  const std::string text = "not json";
  const auto text_digest = crypto::hmac_sha256(key, id + "." + timestamp + "." + text);
  Headers text_headers = signed_headers;
  text_headers[2].second = "v1," + base64_encode(std::string_view(reinterpret_cast<const char*>(text_digest.data()), text_digest.size()));
  CHECK(refused("whsec_" + base64_encode(key), text_headers, text, 1792418980) == "webhooks.body.invalid");
}
}  // namespace

int main() {
  try { published_example(); refusals(); openai_event(); std::cout << "webhook properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "webhook test failed: " << error.what() << '\n'; return 1; }
}
