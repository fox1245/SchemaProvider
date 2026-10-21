// Cache marks: the policy's capabilities per family and origin, the Messages and Chat encoders' marks and limits, the
// families that drop them, replay identity, usage decoding and the report of a prepared request.
#include "codecs/chat.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "codecs/messages.h"
#include "codecs/messages_request.h"
#include "codecs/reason.h"
#include "codecs/responses_request.h"
#include "core/cache_marks.h"
#include "core/native.h"
#include "descriptor/policy.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include "runtime/client.h"
#include "sp/config_defaults.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)

constexpr auto five = CacheTtl::FiveMinutes;
constexpr auto hour = CacheTtl::OneHour;
const CacheControl mark{};
const CacheControl mark_5m{five};
const CacheControl mark_1h{hour};

std::shared_ptr<const json::Document> document(std::string_view bytes) {
  auto parsed = json::parse(bytes); CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::get<json::Document>(std::move(parsed)));
}
Text text(std::string value, std::optional<CacheControl> cache = std::nullopt) {
  Text part{std::move(value)}; part.cache = cache; return part;
}
Message user(std::vector<Part> parts) { return Message{{}, Role::User, std::move(parts)}; }
Message user(std::string value, std::optional<CacheControl> cache = std::nullopt) { return user({text(std::move(value), cache)}); }
Media image(std::optional<CacheControl> cache = std::nullopt) {
  auto media = Media::image("image/png", std::make_shared<const std::string>("iVBORw0KGgo=")); media.cache = cache; return media;
}

// The builtin policy with the cache facts of one family replaced. The embedded text is compact with sorted keys, so a
// family's `cache` member is the last one before its `family` member.
std::string policy_text(std::string_view family, std::string_view facts) {
  std::string text(config_defaults::descriptor_policy_json);
  const auto at = text.find("\"family\":\"" + std::string(family) + "\"");
  CHECK(at != std::string::npos);
  const auto start = text.rfind("\"cache\":[", at);
  CHECK(start != std::string::npos);
  int depth = 0; bool quoted = false; std::size_t end = start + 8;
  for (; end < text.size(); ++end) {
    const char c = text[end];
    if (quoted) { if (c == '\\') ++end; else if (c == '"') quoted = false; continue; }
    if (c == '"') quoted = true;
    else if (c == '[' || c == '{') ++depth;
    else if (c == ']' || c == '}') { if (--depth == 0) break; }
  }
  CHECK(end < text.size());
  text.replace(start, end + 1 - start, facts.empty() ? std::string("\"cache\":[]") : "\"cache\":[" + std::string(facts) + "]");
  return text;
}
std::variant<descriptor::PolicySnapshot, descriptor::ConfigError> load_policy(std::string_view family, std::string_view facts) {
  return descriptor::load_policy(policy_text(family, facts), config_defaults::codec_defaults_json);
}
descriptor::PolicySnapshot policy_with(std::string_view family, std::string_view facts) {
  auto loaded = load_policy(family, facts);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("policy refused at " + error->pointer + ": " + error->expected);
  return std::get<descriptor::PolicySnapshot>(std::move(loaded));
}
descriptor::ValidatedDescriptor messages_at(std::string_view origin, descriptor::PolicySnapshot policy = {}) {
  return descriptor::presets::anthropic_messages({.base_url = std::string(origin), .policy = std::move(policy)});
}
descriptor::ValidatedDescriptor chat_at(std::string_view origin, descriptor::PolicySnapshot policy = {}) {
  return descriptor::presets::openai_chat({.base_url = std::string(origin), .policy = std::move(policy)});
}
constexpr std::string_view loopback = "http://127.0.0.1:18080";
constexpr std::string_view anthropic_origin = "https://api.anthropic.com";
constexpr std::string_view router = "https://openrouter.ai";

messages::Request messages_request(std::vector<Message> history = {}) {
  messages::Request request; request.model = "fixture-model"; request.account_scope = "fixture-account";
  request.messages = history.empty() ? std::vector<Message>{user("Hello")} : std::move(history);
  return request;
}
messages::EncodedRequest encoded(const descriptor::ValidatedDescriptor& descriptor, const messages::Request& request) {
  auto result = messages::encode(descriptor, request, false);
  if (const auto* error = std::get_if<Error>(&result)) throw std::runtime_error("encode refused: " + error->safe_message);
  return std::get<messages::EncodedRequest>(std::move(result));
}
Error refused(const descriptor::ValidatedDescriptor& descriptor, const messages::Request& request, ErrorKind kind, std::string_view code = {}) {
  const auto result = messages::encode(descriptor, request, false);
  CHECK(std::holds_alternative<Error>(result));
  const auto& error = std::get<Error>(result);
  CHECK(error.kind == kind);
  if (!code.empty()) CHECK(reason::request_code(reason::Source::MessagesRequest, error.safe_message) == code);
  return error;
}
chat::Request chat_request(std::string model, std::vector<Message> history) {
  return chat::Request::simple(std::move(model), std::move(history), 64);
}
chat::EncodedRequest encoded(const descriptor::ValidatedDescriptor& descriptor, const chat::Request& request) {
  auto result = chat::encode(descriptor, request, false);
  if (const auto* error = std::get_if<Error>(&result)) throw std::runtime_error("encode refused: " + error->safe_message);
  return std::get<chat::EncodedRequest>(std::move(result));
}
Error refused(const descriptor::ValidatedDescriptor& descriptor, const chat::Request& request, ErrorKind kind, std::string_view code = {}) {
  const auto result = chat::encode(descriptor, request, false);
  CHECK(std::holds_alternative<Error>(result));
  const auto& error = std::get<Error>(result);
  CHECK(error.kind == kind);
  if (!code.empty()) CHECK(reason::request_code(reason::Source::ChatRequest, error.safe_message) == code);
  return error;
}
std::string replaced(std::string body, std::string_view from, std::string_view to) {
  const auto at = body.find(from); CHECK(at != std::string::npos);
  body.replace(at, from.size(), to); return body;
}
std::size_t occurrences(std::string_view body, std::string_view needle) {
  std::size_t count = 0;
  for (auto at = body.find(needle); at != std::string_view::npos; at = body.find(needle, at + needle.size())) ++count;
  return count;
}

void capabilities_of_the_shipped_policy() {
  const auto policy = descriptor::builtin_policy();
  // Messages: marks everywhere, with the prices only where the policy states them.
  auto direct = policy->cache_capabilities("anthropic.messages", anthropic_origin, "claude-haiku-4-5");
  CHECK(direct && direct->supports_explicit && direct->max_breakpoints == 4 && direct->longest_first);
  CHECK(direct->five_minutes.offered && direct->one_hour.offered);
  CHECK(cache_marks::write_multiplier(*direct, std::nullopt) == 1.25 && cache_marks::write_multiplier(*direct, five) == 1.25 && cache_marks::write_multiplier(*direct, hour) == 2.0);
  CHECK(!direct->min_cacheable_tokens);
  const auto elsewhere = policy->cache_capabilities("anthropic.messages", loopback, "any-model");
  CHECK(elsewhere && elsewhere->supports_explicit && elsewhere->max_breakpoints == 4 && !cache_marks::write_multiplier(*elsewhere, hour));
  // Chat through the gateway: by the route of the model.
  const auto anthropic_route = policy->cache_capabilities("openai.chat", router, "anthropic/claude-haiku-4.5");
  CHECK(anthropic_route && anthropic_route->supports_explicit && anthropic_route->max_breakpoints == 4 && anthropic_route->one_hour.offered);
  CHECK(cache_marks::write_multiplier(*anthropic_route, hour) == 2.0);
  const auto google_route = policy->cache_capabilities("openai.chat", router, "google/gemini-2.5-flash");
  CHECK(google_route && google_route->supports_explicit && google_route->max_breakpoints == 1 && google_route->five_minutes.offered && !google_route->one_hour.offered);
  CHECK(!cache_marks::write_multiplier(*google_route, five));
  for (const auto* model : {"openai/gpt-5-nano", "deepseek/deepseek-v4-flash", ""}) {
    const auto other = policy->cache_capabilities("openai.chat", router, model);
    CHECK(other && !other->supports_explicit && other->ignore_unsupported && other->max_breakpoints == 0);
  }
  // A route is a gateway's namespace; the same model id at the vendor's own origin is not routed.
  const auto direct_chat = policy->cache_capabilities("openai.chat", "https://api.openai.com", "anthropic/claude-haiku-4.5");
  CHECK(direct_chat && !direct_chat->supports_explicit);
  // The families whose cache is implicit state nothing explicit.
  for (const char* family : {"openai.responses", "google.generate", "google.interactions"}) {
    const auto facts = policy->cache_capabilities(family, "https://example.test", "m");
    CHECK(facts && !facts->supports_explicit && facts->ignore_unsupported);
  }
  CHECK(!policy->cache_capabilities("no.such.family", router, "m"));
  // The descriptor form asks for the descriptor's own family and origin.
  const auto route = descriptor::cache_capabilities(chat_at(router), "anthropic/claude-haiku-4.5");
  CHECK(route && route->supports_explicit);
  CHECK(!descriptor::cache_capabilities(chat_at("https://api.openai.com"), "gpt-5-nano")->supports_explicit);
}

void policy_data_is_closed() {
  const std::string one_fact = R"({"explicit":true,"max_breakpoints":2,"ttls":["5m"]})";
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(load_policy("anthropic.messages", one_fact)));
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(load_policy("anthropic.messages", "")));
  for (const auto* bad : {
      R"({"explicit":true})",                                                                              // breakpoints and TTLs are required
      R"({"explicit":true,"max_breakpoints":0,"ttls":["5m"]})",
      R"({"explicit":true,"max_breakpoints":17,"ttls":["5m"]})",
      R"({"explicit":true,"max_breakpoints":2,"ttls":[]})",
      R"({"explicit":true,"max_breakpoints":2,"ttls":["1d"]})",
      R"({"explicit":true,"max_breakpoints":2,"ttls":["5m"],"write_multiplier":{"1h":2}})",                // a multiplier for a TTL not offered
      R"({"explicit":true,"max_breakpoints":2,"ttls":["5m"],"write_multiplier":{"5m":0.5}})",
      R"({"explicit":true,"max_breakpoints":2,"ttls":["5m"],"unsupported":"ignore"})",
      R"({"explicit":false,"max_breakpoints":2})",
      R"({"explicit":false,"unsupported":"drop"})",
      R"({"explicit":false,"min_cacheable_tokens":0})",
      R"({"explicit":true,"max_breakpoints":2,"ttls":["5m"],"routes":["x/"]})",                             // routes need a gateway origin
      R"({"explicit":true,"max_breakpoints":2,"ttls":["5m"],"origins":["https://x.example/"]})",
      R"({"explicit":true,"max_breakpoints":2,"ttls":["5m"],"surprise":1})",
      R"({"explicit":true,"max_breakpoints":2,"ttls":["5m"]},{"explicit":false})"})                          // unreachable after a fact for every origin
    CHECK(std::holds_alternative<descriptor::ConfigError>(load_policy("anthropic.messages", bad)));
  CHECK(std::holds_alternative<descriptor::ConfigError>(load_policy("openai.chat", R"({"origins":["https://not-a-gateway.example"],"routes":["x/"],"explicit":false})")));
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(load_policy("openai.chat", R"({"origins":["https://openrouter.ai"],"routes":["x/"],"explicit":false})")));
  // Cache facts are admission data: the replay identity stays, the content digest changes.
  const auto shipped = descriptor::builtin_policy();
  const auto narrowed = policy_with("anthropic.messages", one_fact);
  CHECK(narrowed->identity() == shipped->identity() && narrowed->content_digest() != shipped->content_digest());
}

void messages_goldens() {
  const auto descriptor = messages_at(loopback);
  const std::string unmarked = encoded(descriptor, messages_request()).body;
  CHECK(unmarked == R"({"model":"fixture-model","max_tokens":1024,"stream":false,"messages":[{"role":"user","content":[{"type":"text","text":"Hello"}]}]})");
  auto request = messages_request({user("Hello", mark)});
  auto wire = encoded(descriptor, request);
  CHECK(wire.body == R"({"model":"fixture-model","max_tokens":1024,"stream":false,"messages":[{"role":"user","content":[{"type":"text","text":"Hello","cache_control":{"type":"ephemeral"}}]}]})");
  CHECK(wire.cache_marks.requested == 1 && wire.cache_marks.emitted == 1 && !wire.cache_marks.tail_suppressed);
  request.messages[0].parts[0] = text("Hello", mark_1h);
  CHECK(encoded(descriptor, request).body == replaced(unmarked, R"("text":"Hello")", R"("text":"Hello","cache_control":{"type":"ephemeral","ttl":"1h"})"));
  request.messages[0].parts[0] = text("Hello", mark_5m);
  CHECK(encoded(descriptor, request).body == replaced(unmarked, R"("text":"Hello")", R"("text":"Hello","cache_control":{"type":"ephemeral","ttl":"5m"})"));
  // The same request at the vendor's origin.
  CHECK(encoded(messages_at(anthropic_origin), request).cache_marks.emitted == 1);
  // A request without marks is as before, whatever the policy says about marks.
  const auto none = policy_with("anthropic.messages", "");
  wire = encoded(messages_at(loopback, none), messages_request());
  CHECK(wire.body == unmarked && wire.cache_marks.requested == 0 && wire.cache_marks.emitted == 0);
}

void messages_every_carrier() {
  const auto descriptor = messages_at(loopback);
  // A system prompt in blocks, a client tool, a typed tool, text and an image: four marks.
  auto request = messages_request({user({text("Look at this"), Part{image(mark_5m)}, text("and answer", mark_5m)})});
  request.system_blocks = {{"Stable rules", mark_1h}, {"Slow summary", std::nullopt}};
  request.tools.push_back({"lookup", "Look a name up", document(R"({"type":"object"})"), {}, {}});
  request.tools.back().cache = mark_1h;
  request.tools.push_back({"web_search", {}, {}, "web_search_20250305", 2});
  auto wire = encoded(descriptor, request);
  const auto body = document(wire.body);
  CHECK(wire.cache_marks.requested == 4 && wire.cache_marks.emitted == 4);
  const auto root = body->root();
  CHECK(root.get("system").is_array() && root.get("system").size() == 2);
  CHECK(root.get("system").at(0).get("text").as_string() == "Stable rules" && root.get("system").at(0).get("cache_control").get("ttl").as_string() == "1h");
  CHECK(!root.get("system").at(1).get("cache_control").valid());
  CHECK(root.get("tools").at(0).get("cache_control").get("ttl").as_string() == "1h" && !root.get("tools").at(1).get("cache_control").valid());
  const auto content = root.get("messages").at(0).get("content");
  CHECK(!content.at(0).get("cache_control").valid());
  CHECK(content.at(1).get("type").as_string() == "image" && content.at(1).get("cache_control").get("ttl").as_string() == "5m");
  CHECK(content.at(2).get("cache_control").get("type").as_string() == "ephemeral");
  // The mark is the last member of its block.
  CHECK(wire.body.find(R"({"type":"text","text":"Stable rules","cache_control":{"type":"ephemeral","ttl":"1h"}})") != std::string::npos);
  // A typed server tool takes one as well.
  auto server = messages_request();
  server.tools.push_back({"web_search", {}, {}, "web_search_20250305", 2});
  server.tools.back().cache = mark_5m;
  CHECK(document(encoded(descriptor, server).body)->root().get("tools").at(0).get("cache_control").get("ttl").as_string() == "5m");
  // A document, inline text or PDF, takes one like an image.
  auto pdf = Media::inline_data(MediaKind::Document, "application/pdf", std::make_shared<const std::string>("JVBERi0xLjQK")); pdf.cache = mark_5m;
  auto with_pdf = messages_request({user({Part{pdf}})});
  CHECK(document(encoded(descriptor, with_pdf).body)->root().get("messages").at(0).get("content").at(0).get("cache_control").get("ttl").as_string() == "5m");

  // Tool results take the mark on the result, which covers its parts; the service refuses one inside `tool_result.content`.
  auto tools = messages_request();
  tools.tools.push_back({"f", "first", document(R"({"type":"object"})"), {}, {}});
  Accumulator accumulator;
  const auto first = encoded(descriptor, tools);
  messages::Codec codec(descriptor, messages::Mode::Buffered, accumulator, first.context);
  codec.buffered(R"({"id":"msg_1","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"tool_use","id":"call_a","name":"f","input":{}}],"stop_reason":"tool_use","stop_sequence":null,"usage":{"input_tokens":2,"output_tokens":3,"cache_read_input_tokens":0,"cache_creation_input_tokens":0}})", {true, ErrorKind::Truncated});
  codec.finish();
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  tools.messages.push_back(std::get<Completion>(*accumulator.outcome()).messages.at(0));
  ToolResult result{"call_a", "", false};
  result.content_parts.emplace_back(text("table of results"));
  result.content_parts.emplace_back(image());
  result.cache = mark_5m;
  tools.messages.push_back(Message{{}, Role::Tool, {result}});
  wire = encoded(descriptor, tools);
  CHECK(wire.cache_marks.requested == 1 && wire.cache_marks.emitted == 1);
  const auto sent = document(wire.body);
  const auto block = sent->root().get("messages").at(2).get("content").at(0);
  CHECK(block.get("type").as_string() == "tool_result" && block.get("cache_control").get("ttl").as_string() == "5m");
  CHECK(!block.get("content").at(0).get("cache_control").valid() && !block.get("content").at(1).get("cache_control").valid());
  CHECK(wire.body.find(R"("is_error":false,"cache_control":{"type":"ephemeral","ttl":"5m"}}]})") != std::string::npos);
  // A mark on a part inside the result is refused, not moved.
  auto inside = tools;
  std::get<Text>(std::get<ToolResult>(inside.messages.back().parts[0]).content_parts[0]).cache = mark;
  refused(descriptor, inside, ErrorKind::Unsupported, "request.messages.cache_mark_in_tool_result");
  std::get<Text>(std::get<ToolResult>(inside.messages.back().parts[0]).content_parts[0]).cache.reset();
  std::get<Media>(std::get<ToolResult>(inside.messages.back().parts[0]).content_parts[1]).cache = mark;
  refused(descriptor, inside, ErrorKind::Unsupported, "request.messages.cache_mark_in_tool_result");
  // Where the policy ignores marks, the same request is sent without any.
  const auto ignoring = policy_with("anthropic.messages", R"({"explicit":false,"unsupported":"ignore"})");
  wire = encoded(messages_at(loopback, ignoring), inside);
  CHECK(occurrences(wire.body, "cache_control") == 0 && wire.cache_marks.requested == 2 && wire.cache_marks.emitted == 0);
  // Without marks the same result is plain.
  tools.messages.back() = Message{{}, Role::Tool, {ToolResult{"call_a", "plain", false}}};
  CHECK(encoded(descriptor, tools).cache_marks.requested == 0);
}

void messages_limits_order_and_refusals() {
  const auto descriptor = messages_at(loopback);
  const auto spread = [](int marks, std::optional<CacheControl> lifetime = std::nullopt) {
    std::vector<Part> parts;
    for (int i = 0; i < marks; ++i) parts.push_back(text("part " + std::to_string(i), lifetime ? lifetime : std::optional<CacheControl>(mark)));
    return messages_request({user(std::move(parts))});
  };
  CHECK(encoded(descriptor, spread(4)).cache_marks.emitted == 4);
  // A fifth is refused, not dropped, wherever the marks are.
  refused(descriptor, spread(5), ErrorKind::InvalidRequest, "request.cache.breakpoint_limit");
  auto mixed = spread(3);
  mixed.system_blocks = {{"rules", mark}, {"more rules", mark}};
  refused(descriptor, mixed, ErrorKind::InvalidRequest, "request.cache.breakpoint_limit");
  mixed.system_blocks.pop_back();
  CHECK(encoded(descriptor, mixed).cache_marks.emitted == 4);
  mixed.tools.push_back({"lookup", "d", document(R"({"type":"object"})"), {}, {}});
  mixed.tools.back().cache = mark;
  refused(descriptor, mixed, ErrorKind::InvalidRequest, "request.cache.breakpoint_limit");
  // A mark written inside a block counts too once typed marks ask.
  auto raw = spread(4);
  raw.messages.push_back(user({Part{Opaque{"search_result", document(R"({"type":"search_result","source":"kb://a","title":"A","content":[{"type":"text","text":"x"}],"cache_control":{"type":"ephemeral"}})")}}}));
  refused(descriptor, raw, ErrorKind::InvalidRequest, "request.cache.breakpoint_limit");
  // A toolset may carry its mark in its options. It counts and takes its place in the order, and a typed mark beside it
  // would be a second one.
  const auto vendor = messages_at(anthropic_origin);
  auto toolset = messages_request({user("Hello", mark_1h)});
  toolset.mcp_servers = {messages::McpServer{"ctx", "https://mcp.example.test/mcp", std::nullopt}};
  toolset.tools.emplace_back();
  toolset.tools.back().type = "mcp_toolset";
  toolset.tools.back().options = document(R"({"mcp_server_name":"ctx","cache_control":{"type":"ephemeral"}})");
  refused(vendor, toolset, ErrorKind::InvalidRequest, "request.cache.lifetime_order");  // 1h in a message after 5m in a tool
  toolset.messages[0].parts[0] = text("Hello", mark_5m);
  CHECK(encoded(vendor, toolset).cache_marks.requested == 1);
  toolset.tools.back().cache = mark;
  refused(vendor, toolset, ErrorKind::InvalidRequest, "request.messages.cache_control_duplicate");

  // A longer lifetime may not follow a shorter one: the service refuses the request.
  auto ordered = messages_request({user({text("a", mark_1h), text("b", mark_5m), text("c", mark)})});
  CHECK(encoded(descriptor, ordered).cache_marks.emitted == 3);
  auto backwards = messages_request({user({text("a", mark_5m), text("b", mark_1h)})});
  refused(descriptor, backwards, ErrorKind::InvalidRequest, "request.cache.lifetime_order");
  backwards = messages_request({user({text("a", mark), text("b", mark_1h)})});
  refused(descriptor, backwards, ErrorKind::InvalidRequest, "request.cache.lifetime_order");
  // Prompt order is tools, system, then messages.
  auto across = messages_request({user({text("a", mark_1h)})});
  across.system_blocks = {{"rules", mark_5m}};
  refused(descriptor, across, ErrorKind::InvalidRequest, "request.cache.lifetime_order");
  across.system_blocks[0].cache = mark_1h;
  CHECK(encoded(descriptor, across).cache_marks.emitted == 2);
  // An invalid lifetime is refused.
  auto invalid = messages_request({user({text("a", CacheControl{static_cast<CacheTtl>(9)})})});
  refused(descriptor, invalid, ErrorKind::InvalidRequest, "request.cache.ttl_invalid");
  // The services refuse a mark on an empty text block.
  refused(descriptor, messages_request({user({text("", mark)})}), ErrorKind::InvalidRequest, "request.cache.empty_text");
  CHECK(encoded(descriptor, messages_request({user({text("")})})).cache_marks.requested == 0);
  // A system prompt is a string or blocks.
  auto both = messages_request(); both.system = "x"; both.system_blocks = {{"y", std::nullopt}};
  refused(descriptor, both, ErrorKind::InvalidRequest, "request.messages.system_blocks_conflict");
  auto empty = messages_request(); empty.system_blocks = {{"", std::nullopt}};
  refused(descriptor, empty, ErrorKind::InvalidRequest, "request.messages.system_block_empty");
  auto plain = messages_request(); plain.system_blocks = {{"y", std::nullopt}};
  CHECK(document(encoded(descriptor, plain).body)->root().get("system").at(0).get("text").as_string() == "y");
}

void request_level_mark_yields() {
  const auto descriptor = messages_at(loopback);
  auto request = messages_request();
  request.cache_control = mark_1h;
  // On its own it is sent as before.
  auto wire = encoded(descriptor, request);
  CHECK(document(wire.body)->root().get("cache_control").get("ttl").as_string() == "1h");
  CHECK(wire.cache_marks.requested == 0 && !wire.cache_marks.tail_suppressed);
  // With a mark on a part it is not sent, and the report says so.
  request.messages[0].parts[0] = text("Hello", mark_1h);
  wire = encoded(descriptor, request);
  CHECK(!document(wire.body)->root().get("cache_control").valid() && occurrences(wire.body, "cache_control") == 1);
  CHECK(wire.cache_marks.emitted == 1 && wire.cache_marks.tail_suppressed);
  // The same holds for a mark on a system block or a tool.
  auto system = messages_request(); system.cache_control = mark; system.system_blocks = {{"rules", mark}};
  CHECK(occurrences(encoded(descriptor, system).body, "cache_control") == 1);
  // Marks the policy says to ignore do not take the request-level mark with them.
  const auto ignoring = policy_with("anthropic.messages", R"({"explicit":false,"unsupported":"ignore"})");
  wire = encoded(messages_at(loopback, ignoring), request);
  CHECK(document(wire.body)->root().get("cache_control").get("ttl").as_string() == "1h" && occurrences(wire.body, "cache_control") == 1);
  CHECK(wire.cache_marks.requested == 1 && wire.cache_marks.emitted == 0 && !wire.cache_marks.tail_suppressed);
  // A request without the member is unchanged by marks being ignored.
  request.cache_control.reset();
  CHECK(encoded(messages_at(loopback, ignoring), request).body == encoded(descriptor, messages_request()).body);
}

void chat_marks_by_policy() {
  const auto gateway = chat_at(router);
  const auto marked = [](Text part) { return std::vector<Message>{Message::system_text("rules"), user({part})}; };
  // Through the gateway to a model whose route takes marks.
  const auto unmarked = encoded(gateway, chat_request("anthropic/claude-haiku-4.5", marked(text("Hello")))).body;
  CHECK(occurrences(unmarked, "cache_control") == 0);
  auto wire = encoded(gateway, chat_request("anthropic/claude-haiku-4.5", marked(text("Hello", mark_1h))));
  CHECK(wire.body == replaced(unmarked, R"("text":"Hello")", R"("text":"Hello","cache_control":{"type":"ephemeral","ttl":"1h"})"));
  CHECK(wire.cache_marks.requested == 1 && wire.cache_marks.emitted == 1);
  // The system message is an array of parts as well.
  auto system = marked(text("Hello"));
  system[0] = Message::system_text("rules"); std::get<Text>(system[0].parts[0]).cache = mark;
  wire = encoded(gateway, chat_request("anthropic/claude-haiku-4.5", system));
  CHECK(wire.body == replaced(unmarked, R"("text":"rules")", R"("text":"rules","cache_control":{"type":"ephemeral"})"));
  // The route of Google's models takes one mark with the default lifetime.
  const auto google = encoded(gateway, chat_request("google/gemini-2.5-flash", marked(text("Hello", mark_5m))));
  CHECK(google.cache_marks.emitted == 1 && occurrences(google.body, R"("cache_control":{"type":"ephemeral","ttl":"5m"})") == 1);
  refused(gateway, chat_request("google/gemini-2.5-flash", marked(text("Hello", mark_1h))), ErrorKind::Unsupported, "request.cache.lifetime_not_offered");
  auto two = marked(text("Hello", mark)); std::get<Text>(two[0].parts[0]).cache = mark;
  refused(gateway, chat_request("google/gemini-2.5-flash", two), ErrorKind::InvalidRequest, "request.cache.breakpoint_limit");
  CHECK(encoded(gateway, chat_request("anthropic/claude-haiku-4.5", two)).cache_marks.emitted == 2);
  // Routes with implicit caching and the vendor's own origin send nothing, and say so.
  for (const auto& [origin, model] : std::vector<std::pair<std::string_view, std::string_view>>{
           {router, "openai/gpt-5-nano"}, {router, "deepseek/deepseek-v4-flash"}, {"https://api.openai.com", "gpt-5-nano"},
           {"https://api.openai.com", "anthropic/claude-haiku-4.5"}, {loopback, "any-model"}}) {
    const auto descriptor = chat_at(origin);
    const auto plain = encoded(descriptor, chat_request(std::string(model), marked(text("Hello"))));
    const auto ignored = encoded(descriptor, chat_request(std::string(model), marked(text("Hello", mark_1h))));
    CHECK(ignored.body == plain.body && ignored.cache_marks.requested == 1 && ignored.cache_marks.emitted == 0);
  }
  // Only a text part carries a mark here.
  auto media = marked(text("Hello")); media.push_back(user({Part{image(mark)}}));
  refused(gateway, chat_request("anthropic/claude-haiku-4.5", media), ErrorKind::Unsupported, "request.chat.cache_marks_text_only");
  CHECK(encoded(chat_at("https://api.openai.com"), chat_request("gpt-5-nano", media)).cache_marks.emitted == 0);
  // A tool result is a string on the wire, with no part to mark.
  ToolResult result{"call_a", "42", false};
  result.cache = mark;
  refused(gateway, chat_request("anthropic/claude-haiku-4.5", {user("Hello"), Message{{}, Role::Tool, {result}}}), ErrorKind::Unsupported, "request.chat.cache_marks_text_only");
  refused(gateway, chat_request("anthropic/claude-haiku-4.5", marked(text("", mark))), ErrorKind::InvalidRequest, "request.cache.empty_text");
  // The legacy single-string message form has no part to mark.
  CHECK(encoded(gateway, chat::Request::simple("anthropic/claude-haiku-4.5", "Hello", 64)).cache_marks.requested == 0);
}

void synthetic_policies() {
  // The gateway's routes are data: another namespace takes marks when the policy says so.
  const std::string facts =
      R"({"origins":["https://openrouter.ai"],"routes":["acme/"],"explicit":true,"max_breakpoints":2,"ttls":["1h"],"write_multiplier":{"1h":3},"min_cacheable_tokens":2048},)"
      R"({"origins":["https://openrouter.ai"],"routes":["anthropic/"],"explicit":false,"unsupported":"reject"},)"
      R"({"explicit":false,"unsupported":"ignore"})";
  const auto policy = policy_with("openai.chat", facts);
  const auto gateway = chat_at(router, policy);
  const auto query = policy->cache_capabilities("openai.chat", router, "acme/model-1");
  CHECK(query && query->supports_explicit && query->max_breakpoints == 2 && !query->five_minutes.offered && query->one_hour.offered);
  CHECK(cache_marks::write_multiplier(*query, hour) == 3.0 && query->min_cacheable_tokens == 2048 && !query->longest_first);
  const auto marked = [] { return std::vector<Message>{user("Hello", mark_1h)}; };
  auto wire = encoded(gateway, chat_request("acme/model-1", marked()));
  CHECK(wire.cache_marks.emitted == 1 && occurrences(wire.body, R"("ttl":"1h")") == 1);
  refused(gateway, chat_request("acme/model-1", {user("Hello", mark_5m)}), ErrorKind::Unsupported, "request.cache.lifetime_not_offered");
  // The route that took marks before now refuses them, and the rest ignores.
  refused(gateway, chat_request("anthropic/claude-haiku-4.5", marked()), ErrorKind::Unsupported, "request.cache.unsupported");
  CHECK(encoded(gateway, chat_request("other/model", marked())).cache_marks.emitted == 0);
  // The same fact for a family: the Messages limit and lifetimes come from the policy, not from the encoder.
  const auto narrow = policy_with("anthropic.messages", R"({"explicit":true,"max_breakpoints":2,"ttls":["5m"]})");
  const auto messages = messages_at(loopback, narrow);
  auto request = messages_request({user({text("a", mark), text("b", mark)})});
  CHECK(encoded(messages, request).cache_marks.emitted == 2);
  request.messages[0].parts.push_back(text("c", mark));
  refused(messages, request, ErrorKind::InvalidRequest, "request.cache.breakpoint_limit");
  refused(messages, messages_request({user({text("a", mark_1h)})}), ErrorKind::Unsupported, "request.cache.lifetime_not_offered");
  // Without the order rule, the order is free.
  const auto free_order = policy_with("anthropic.messages", R"({"explicit":true,"max_breakpoints":4,"ttls":["5m","1h"]})");
  CHECK(encoded(messages_at(loopback, free_order), messages_request({user({text("a", mark_5m), text("b", mark_1h)})})).cache_marks.emitted == 2);
  // A policy that states nothing, or refuses, admits no marks; one that ignores them drops them. None of this touches
  // a request that carries no mark.
  const auto silent = policy_with("anthropic.messages", "");
  refused(messages_at(loopback, silent), messages_request({user("Hello", mark)}), ErrorKind::Unsupported, "request.cache.not_admitted");
  CHECK(encoded(messages_at(loopback, silent), messages_request()).cache_marks.requested == 0);
  const auto refusing = policy_with("anthropic.messages", R"({"explicit":false})");
  refused(messages_at(loopback, refusing), messages_request({user("Hello", mark)}), ErrorKind::Unsupported, "request.cache.unsupported");
  // A mark inside a block still passes when no typed mark asks, as it did before typed marks.
  auto raw = messages_request();
  raw.messages.push_back(user({Part{Opaque{"search_result", document(R"({"type":"search_result","source":"kb://a","title":"A","content":[{"type":"text","text":"x"}],"cache_control":{"type":"ephemeral"}})")}}}));
  CHECK(occurrences(encoded(messages_at(loopback, silent), raw).body, "cache_control") == 1);
}

void families_with_implicit_caches() {
  // Responses, Generate and Interactions send nothing for a mark, and say so; the body is the one without marks.
  const auto history = [](std::optional<CacheControl> cache) { return std::vector<Message>{user("Hello", cache)}; };
  {
    const auto descriptor = descriptor::presets::openai_responses();
    auto marked = responses::Request::simple("gpt-5-nano", history(mark_1h), 64);
    auto plain = responses::Request::simple("gpt-5-nano", history(std::nullopt), 64);
    const auto a = std::get<responses::EncodedRequest>(responses::encode(descriptor, marked, false));
    const auto b = std::get<responses::EncodedRequest>(responses::encode(descriptor, plain, false));
    CHECK(a.body == b.body && a.cache_marks.requested == 1 && a.cache_marks.emitted == 0 && b.cache_marks.requested == 0);
    const auto refusing = policy_with("openai.responses", R"({"explicit":false})");
    const auto strict = descriptor::presets::openai_responses({.policy = refusing});
    const auto result = responses::encode(strict, marked, false);
    CHECK(std::holds_alternative<Error>(result) && std::get<Error>(result).kind == ErrorKind::Unsupported);
    CHECK(std::holds_alternative<responses::EncodedRequest>(responses::encode(strict, plain, false)));
  }
  {
    const auto descriptor = descriptor::presets::google_generate("gemini-2.5-flash-lite");
    auto marked = gemini::Request::simple("gemini-2.5-flash-lite", history(mark), 64);
    auto plain = gemini::Request::simple("gemini-2.5-flash-lite", history(std::nullopt), 64);
    const auto a = std::get<gemini::EncodedRequest>(gemini::encode(descriptor, marked, false));
    const auto b = std::get<gemini::EncodedRequest>(gemini::encode(descriptor, plain, false));
    CHECK(a.body == b.body && a.cache_marks.requested == 1 && a.cache_marks.emitted == 0);
  }
  {
    const auto descriptor = descriptor::presets::google_interactions();
    auto marked = interactions::Request::simple("gemini-2.5-flash-lite", history(mark), 64);
    auto plain = interactions::Request::simple("gemini-2.5-flash-lite", history(std::nullopt), 64);
    const auto a = std::get<interactions::EncodedRequest>(interactions::encode(descriptor, marked, false));
    const auto b = std::get<interactions::EncodedRequest>(interactions::encode(descriptor, plain, false));
    CHECK(a.body == b.body && a.cache_marks.requested == 1 && a.cache_marks.emitted == 0);
  }
}

// A captured answer, as the codec rebuilds it from the wire.
Message captured(const descriptor::ValidatedDescriptor& descriptor, const messages::Request& request, std::string_view answer) {
  const auto wire = encoded(descriptor, request);
  Accumulator accumulator;
  messages::Codec codec(descriptor, messages::Mode::Buffered, accumulator, wire.context);
  codec.buffered(R"({"id":"msg_1","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"text","text":")" + std::string(answer) +
                 R"("}],"stop_reason":"end_turn","stop_sequence":null,"usage":{"input_tokens":2,"output_tokens":3,"cache_read_input_tokens":0,"cache_creation_input_tokens":0}})",
                 {true, ErrorKind::Truncated});
  codec.finish();
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  auto message = std::get<Completion>(*accumulator.outcome()).messages.at(0);
  CHECK(message.native && message.native->complete());
  return message;
}

void marks_do_not_bind_replay() {
  const auto descriptor = messages_at(loopback);
  auto first = messages_request({user("Hello", mark)});
  first.system_blocks = {{"rules", mark_1h}};
  const auto answer = captured(descriptor, first, "Hi");
  auto next = first;
  next.messages.push_back(answer);
  next.messages.push_back(user("Again"));
  CHECK(encoded(descriptor, next).cache_marks.emitted == 2);
  // The next turns move, add and drop marks; the captured history is still the history it was.
  next.messages[0] = user("Hello");
  next.messages[2] = user("Again", mark);
  next.system_blocks[0].cache = std::nullopt;
  CHECK(encoded(descriptor, next).cache_marks.emitted == 1);
  next.system_blocks[0].cache = mark_5m;
  next.messages[2] = user("Again");
  CHECK(encoded(descriptor, next).cache_marks.emitted == 1);
  next.system_blocks[0].cache = std::nullopt;
  CHECK(encoded(descriptor, next).cache_marks.emitted == 0);
  // The captured answer's own text may carry one too: a mark is not part of what its seal binds.
  auto marked_answer = answer;
  std::get<Text>(marked_answer.parts[0]).cache = mark_5m;
  next.messages[1] = marked_answer;
  CHECK(encoded(descriptor, next).cache_marks.emitted == 1);
  // What a system prompt in blocks says is bound, as `system` is: another text is another conversation.
  auto edited = next;
  edited.system_blocks[0].text = "other rules";
  const auto result = messages::encode(descriptor, edited, false);
  CHECK(std::holds_alternative<Error>(result) && std::get<Error>(result).kind == ErrorKind::ReplayIneligible);
  // The request-level mark keeps the binding it had: adding it to a conversation answered without it breaks the history.
  auto tail = next; tail.cache_control = mark;
  const auto bound = messages::encode(descriptor, tail, false);
  CHECK(std::holds_alternative<Error>(bound) && std::get<Error>(bound).kind == ErrorKind::ReplayIneligible);
}

void usage_round_trip() {
  const auto descriptor = messages_at(loopback);
  auto request = messages_request({user("Hello", mark_1h)});
  const auto wire = encoded(descriptor, request);
  Accumulator accumulator;
  messages::Codec codec(descriptor, messages::Mode::Buffered, accumulator, wire.context);
  codec.buffered(R"({"id":"msg_1","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"text","text":"Hi"}],"stop_reason":"end_turn","stop_sequence":null,)"
                 R"("usage":{"input_tokens":12,"output_tokens":3,"cache_read_input_tokens":4096,"cache_creation_input_tokens":512,"cache_creation":{"ephemeral_5m_input_tokens":128,"ephemeral_1h_input_tokens":384}}})",
                 {true, ErrorKind::Truncated});
  codec.finish();
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  const auto& usage = std::get<Completion>(*accumulator.outcome()).usage;
  CHECK(usage.cache_read && usage.cache_read->value == 4096 && usage.cache_write && usage.cache_write->value == 512);
  CHECK(usage.input_uncached && usage.input_uncached->value == 12 && usage.input_total && usage.input_total->value == 12 + 4096 + 512);
  CHECK(usage.extra.at("cache_creation.ephemeral_5m_input_tokens").value == 128 && usage.extra.at("cache_creation.ephemeral_1h_input_tokens").value == 384);
  CHECK(usage.conflicts.empty());

  const auto gateway = chat_at(router);
  const auto sent = encoded(gateway, chat_request("anthropic/claude-haiku-4.5", {user("Hello", mark)}));
  Accumulator chat_accumulator;
  chat::Codec chat_codec(gateway, chat::Mode::Buffered, chat_accumulator, {}, sent.context);
  chat_codec.buffered(R"({"id":"gen-1","model":"anthropic/claude-haiku-4.5","created":7,"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"Hi"},"finish_reason":"stop"}],)"
                      R"("usage":{"prompt_tokens":5000,"completion_tokens":3,"total_tokens":5003,"prompt_tokens_details":{"cached_tokens":4096,"cache_write_tokens":512}}})",
                      {true, ErrorKind::Truncated});
  chat_codec.finish();
  CHECK(chat_accumulator.outcome() && std::holds_alternative<Completion>(*chat_accumulator.outcome()));
  const auto& chat_usage = std::get<Completion>(*chat_accumulator.outcome()).usage;
  CHECK(chat_usage.cache_read && chat_usage.cache_read->value == 4096 && chat_usage.cache_write && chat_usage.cache_write->value == 512);
}

void prepared_request_reports_the_marks() {
  sp::runtime::Client client(descriptor::presets::anthropic_messages(), std::string("unused-key"));
  sp::runtime::RunOptions run;
  run.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  auto request = messages::Request::simple("claude-haiku-4-5-20251001", "hi", 64);
  request.cache_control = mark;
  auto prepared = client.prepare(request, run);
  CHECK(prepared.valid() && !prepared.error());
  CHECK(prepared.cache_marks().requested == 0 && !prepared.cache_marks().tail_suppressed);
  CHECK(prepared.encoded_body().find("\"cache_control\"") != std::string_view::npos);
  request.messages[0].parts[0] = text("hi", mark_1h);
  prepared = client.prepare(request, run);
  CHECK(!prepared.error() && prepared.cache_marks().requested == 1 && prepared.cache_marks().emitted == 1 && prepared.cache_marks().tail_suppressed);
  CHECK(prepared.encoded_body().find("\"ttl\":\"1h\"") != std::string_view::npos);
  // The fifth is refused when the request is prepared, with the check's code.
  request.messages[0].parts = {text("a", mark), text("b", mark), text("c", mark), text("d", mark), text("e", mark)};
  prepared = client.prepare(request, run);
  CHECK(prepared.error() && prepared.error()->kind == ErrorKind::InvalidRequest && prepared.error()->reason == "request.cache.breakpoint_limit");
  CHECK(prepared.cache_marks().emitted == 0);
  // A default-constructed handle reports none.
  CHECK(sp::runtime::PreparedRequest{}.cache_marks().requested == 0);
}
} // namespace

int main() {
  try {
    capabilities_of_the_shipped_policy(); policy_data_is_closed();
    messages_goldens(); messages_every_carrier(); messages_limits_order_and_refusals(); request_level_mark_yields();
    chat_marks_by_policy(); synthetic_policies(); families_with_implicit_caches();
    marks_do_not_bind_replay(); usage_round_trip(); prepared_request_reports_the_marks();
    std::cout << "Cache mark properties passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
