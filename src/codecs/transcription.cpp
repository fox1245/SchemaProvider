#include "codecs/transcription.h"
#include "codecs/provider_cost.h"
#include "codecs/reason.h"
#include "codecs/served_model.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <system_error>

namespace sp::transcription {
namespace {
std::optional<Count> counter(json::Value value) {
  if (!value.is_uint()) return std::nullopt;
  return Count{value.as_uint(), Evidence::Reported};
}
// A reported length in seconds as whole seconds, rounded up. The parser keeps no lexeme: the value rounded up is the
// double it parsed, read from the shortest decimal that round-trips to it, as provider costs are, and rounded up with
// integer arithmetic. A provider text the double does not hold exactly is lost before this reads it
// (`2.0000000000000001` is parsed as 2.0, so it gives 2). Nothing for a negative or
// non-finite number, and nothing for one at or above 2^53 that is not an integer: there the spacing of a double is above
// one second, so its value may be below the reported length. An integer is exact at any size up to 64 bits.
std::optional<std::uint64_t> whole_seconds(json::Value value) {
  if (value.is_uint()) return value.as_uint();
  if (!value.is_number()) return std::nullopt;
  const double number = value.as_double();
  // 2^53: from there a double cannot hold every integer.
  constexpr double exact_limit = static_cast<double>(std::uint64_t{1} << std::numeric_limits<double>::digits);
  if (!std::isfinite(number) || number < 0 || number >= exact_limit) return std::nullopt;
  if (number == 0) return 0;
  char text[32];
  const auto written = std::to_chars(text, text + sizeof text, number, std::chars_format::scientific);
  if (written.ec != std::errc{}) return std::nullopt;
  // `number` is `significand` x 10^(exponent - digits + 1).
  std::uint64_t significand = 0;
  int digits = 0, exponent = 0;
  const char* cursor = text;
  for (; cursor != written.ptr && *cursor != 'e'; ++cursor)
    if (*cursor != '.') { significand = significand * 10 + static_cast<std::uint64_t>(*cursor - '0'); ++digits; }
  if (cursor == written.ptr || std::from_chars(cursor + (cursor[1] == '+' ? 2 : 1), written.ptr, exponent).ec != std::errc{}) return std::nullopt;
  const int scale = exponent - digits + 1;
  constexpr auto most = std::numeric_limits<std::uint64_t>::max();
  if (scale >= 0) {
    std::uint64_t seconds = significand;
    for (int i = 0; i < scale; ++i) {
      if (seconds > most / 10) return std::nullopt;
      seconds *= 10;
    }
    return seconds;
  }
  // Divided by 10^-scale, rounded up; a divisor above the significand leaves a positive fraction below one second.
  std::uint64_t divisor = 1;
  for (int i = 0; i < -scale && divisor <= significand; ++i) divisor *= 10;
  if (divisor > significand) return 1;
  return significand / divisor + (significand % divisor ? 1 : 0);
}
}  // namespace

Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator, std::string requested, SemanticLimits limits)
    : descriptor_(descriptor), accumulator_(accumulator), requested_(std::move(requested)), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event); }
bool Codec::fail(ErrorKind kind, std::string text) {
  Error error{kind, std::move(text)};
  error.reason = reason::code(reason::Source::Transcription, error.safe_message);
  emit(Fail{std::move(error)});
  return false;
}
bool Codec::buffered(std::string_view body, Close close) {
  if (!close.normal) return fail(close.error, "abnormal response close");
  auto parsed = json::parse(body, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto* error = std::get_if<json::ParseError>(&parsed))
    return fail(error->code == json::ParseCode::SizeExceeded || error->code == json::ParseCode::DepthExceeded
                    ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, "invalid response JSON");
  auto wire = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = wire->root();
  if (!root.is_object()) return fail(ErrorKind::ProtocolCorrupt, "response must be object");
  if (!emit(RawWire{"transcription.response", wire})) return false;
  if (root.get("error").valid() && !root.get("error").is_null()) return fail(ErrorKind::RemoteFailure, "remote error property");
  if (!root.get("text").is_string()) return fail(ErrorKind::ProtocolCorrupt, "transcript text required");
  for (const char* key : {"duration", "seconds"})
    if (root.get(key).valid() && (!root.get(key).is_number() || !std::isfinite(root.get(key).as_double())))
      return fail(ErrorKind::ProtocolCorrupt, "an audio duration is a number");
  accumulator_.served_model({requested_, {}, ServedRelation::NotReported});
  if (!emit(Begin{}) || !emit(MessageBegin{{0}, std::nullopt, Role::Assistant, nullptr})) return false;
  const LocalId local{1};
  if (!emit(PartBegin{{0}, local, PartKind::Text, {}, 0}) || !emit(PartDelta{local, {PartKind::Text, root.get("text").as_string()}}) ||
      !emit(PartSeal{local, {}})) return false;
  Usage usage;
  if (const auto reported = root.get("usage"); reported.is_object()) {
    usage.input_total = counter(reported.get("input_tokens"));
    usage.output_total = counter(reported.get("output_tokens"));
    usage.total = counter(reported.get("total_tokens"));
    usage.provider_cost = codecs::provider_cost(reported, descriptor_);
    usage.stage = UsageStage::Final;
  }
  // The audio's length in whole seconds rounded up: of `duration` and `usage.seconds`, each that gives a count, the
  // larger, since two reports of one length that disagree must not price the smaller.
  std::optional<std::uint64_t> length;
  for (const auto value : {root.get("duration"), root.get("usage").get("seconds")})
    if (const auto seconds = whole_seconds(value); seconds && (!length || *seconds > *length)) length = seconds;
  if (length) usage.extra[std::string(seconds_counter)] = Count{*length, Evidence::Derived};
  return emit(UsageUpdate{std::move(usage)}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) &&
         emit(MessageSeal{{0}}) && emit(Commit{"transcription.text+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Result result;
  result.text = completion.joined_text();
  for (const auto& raw : completion.raw_events) {
    if (raw.type != "transcription.response" || !raw.payload) continue;
    const auto root = raw.payload->root();
    if (root.get("language").is_string()) result.language = std::string(root.get("language").as_string());
    // OpenAI reports the length as `usage.seconds` (or `duration` in verbose form), OpenRouter as `duration`.
    // A negative length is none; of the reports that give one, the larger.
    for (const auto value : {root.get("duration"), root.get("usage").get("seconds")})
      if (value.is_number() && value.as_double() >= 0 && (!result.seconds || value.as_double() > *result.seconds)) result.seconds = value.as_double();
  }
  result.usage = completion.usage;
  result.model = completion.model;
  result.attempt = completion.attempt;
  result.raw_events = completion.raw_events;
  return result;
}
}  // namespace sp::transcription
