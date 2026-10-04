#include "core/native_archive.h"
#include "core/native.h"
#include "codecs/chat.h"
#include "codecs/messages.h"
#include "codecs/messages_request.h"
#include "codecs/responses.h"
#include "codecs/gemini.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions.h"
#include "codecs/interactions_request.h"
#include "codecs/responses_request.h"
#include "json/json.h"
#include <openssl/evp.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace sp;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("archive check failed at " + std::to_string(__LINE__)); } while(false)
struct Fixture {
  std::string root;
  Fixture() { char path[]="/tmp/sp-native-archive-XXXXXX";auto* p=::mkdtemp(path);CHECK(p);root=p; }
  ~Fixture() { std::error_code e;std::filesystem::remove_all(root,e); }
  std::string directory() const { return root+"/records"; }
  std::string key() const { return root+"/activation"; }
};
descriptor::ValidatedDescriptor desc(std::string_view family="anthropic.messages",std::string_view base="http://127.0.0.1:18080") {
  const auto route=family=="anthropic.messages"?"/v1/messages":family=="openai.chat"?"/chat/completions":"/v1/responses";
  auto loaded=descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"archive-fixture\",\"family\":"+json::quote(family)+",\"connection\":{\"base_url\":"+json::quote(base)+",\"paths\":{\"buffered\":"+json::quote(route)+",\"streaming\":"+json::quote(route)+"}}}");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::shared_ptr<const json::Document> document(std::string_view bytes) {
  auto parsed=json::parse(bytes);CHECK(std::holds_alternative<json::Document>(parsed));return std::make_shared<const json::Document>(std::get<json::Document>(std::move(parsed)));
}
std::shared_ptr<NativeArchive> admitted(NativeArchive::Activation a) { CHECK(std::holds_alternative<std::shared_ptr<NativeArchive>>(a));return std::get<std::shared_ptr<NativeArchive>>(std::move(a)); }
std::string saved(NativeArchive::Saved s) {CHECK(std::holds_alternative<std::string>(s));return std::get<std::string>(std::move(s));}
std::vector<Message> loaded(NativeArchive::Loaded l) {CHECK(std::holds_alternative<std::vector<Message>>(l));return std::get<std::vector<Message>>(std::move(l));}
Message user(std::string s) {return Message{{},Role::User,{Text{std::move(s)}}};}
messages::Request request() {messages::Request r;r.model="fixture-model";r.account_scope="fixture-account";r.messages.push_back(user("hello"));return r;}
messages::EncodedRequest encoded(const descriptor::ValidatedDescriptor& d,const messages::Request& r) {auto e=messages::encode(d,r,false);CHECK(std::holds_alternative<messages::EncodedRequest>(e));return std::get<messages::EncodedRequest>(std::move(e));}
Message genuine(const descriptor::ValidatedDescriptor& d,const messages::Request& r,bool complete=true) {
  auto e=encoded(d,r);Accumulator a;messages::Codec c(d,messages::Mode::Buffered,a,e.context);
  c.buffered(R"({"id":"msg_archive","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"thinking","thinking":"reason","signature":"synthetic-sensitive-signature"},{"type":"redacted_thinking","data":"synthetic-encrypted-state"},{"type":"text","text":"answer"}],"stop_reason":"end_turn","stop_sequence":null,"usage":{"input_tokens":2,"output_tokens":3}})",{complete,ErrorKind::Truncated});c.finish();CHECK(a.outcome());
  if(complete){CHECK(std::holds_alternative<Completion>(*a.outcome()));auto m=std::get<Completion>(*a.outcome()).messages.at(0);CHECK(m.native && m.native->complete());return m;}
  CHECK(std::holds_alternative<Failure>(*a.outcome()));auto m=std::get<Failure>(*a.outcome()).partial.messages.at(0);CHECK(m.native && !m.native->complete());return m;
}
std::string digest(std::string_view bytes) {
  std::array<unsigned char,32> h{};unsigned n=0;CHECK(EVP_Digest(bytes.data(),bytes.size(),h.data(),&n,EVP_sha256(),nullptr)==1 && n==h.size());std::string s;constexpr char digits[]="0123456789abcdef";for(auto c:h){s+=digits[c>>4];s+=digits[c&15];}return s;
}
void same_document(const std::shared_ptr<const json::Document>& a,const std::shared_ptr<const json::Document>& b) {CHECK(bool(a)==bool(b));if(a)CHECK(json::equal(a->root(),b->root()));}
void same_part(const Part& a,const Part& b) {
  CHECK(a.index()==b.index());std::visit([&](const auto& p){using P=std::decay_t<decltype(p)>;const auto& q=std::get<P>(b);
    if constexpr(std::is_same_v<P,Text>) CHECK(p.value==q.value);
    else if constexpr(std::is_same_v<P,Refusal>) CHECK(p.text==q.text && p.raw_code==q.raw_code);
    else if constexpr(std::is_same_v<P,ToolCall> || std::is_same_v<P,InvalidToolCall>){CHECK(p.id==q.id && p.name==q.name && p.kind==q.kind && p.wire_type==q.wire_type);same_document(p.wire_metadata,q.wire_metadata);if constexpr(std::is_same_v<P,ToolCall>)same_document(p.input,q.input);else CHECK(p.raw_fragment==q.raw_fragment && p.reason==q.reason);}
    else if constexpr(std::is_same_v<P,Thinking>)CHECK(p.text==q.text && p.signature==q.signature);
    else if constexpr(std::is_same_v<P,RedactedThinking>)CHECK(p.data==q.data);
    else if constexpr(std::is_same_v<P,ServerToolResult>){CHECK(p.tool_use_id==q.tool_use_id && p.wire_type==q.wire_type);same_document(p.content,q.content);}
    else if constexpr(std::is_same_v<P,ToolResult>){CHECK(p.tool_use_id==q.tool_use_id && p.content==q.content && p.is_error==q.is_error && p.host.has_value()==q.host.has_value());if(p.host)CHECK(p.host->name==q.host->name && p.host->status==q.host->status && p.host->retryable==q.host->retryable && p.host->effect_uncertain==q.host->effect_uncertain);}
    else if constexpr(std::is_same_v<P,Reasoning>)CHECK(p.id==q.id && p.summary==q.summary && p.encrypted_content==q.encrypted_content && p.status==q.status && p.content==q.content);
    else if constexpr(std::is_same_v<P,Opaque>){CHECK(p.wire_type==q.wire_type);same_document(p.wire_metadata,q.wire_metadata);}
    else if constexpr(std::is_same_v<P,Image>){CHECK(p.mime==q.mime && p.detail==q.detail && bool(p.data)==bool(q.data));if(p.data)CHECK(*p.data==*q.data);}
    else if constexpr(std::is_same_v<P,Thought>)CHECK(p.summary==q.summary && p.signature==q.signature);
  },a);
}
void full_portable_and_native_roundtrip() {
  Fixture f;auto d=desc();auto a=admitted(NativeArchive::provision(f.directory(),f.key(),"owner-a",d));
  auto object=document(R"({"second":[1,true,null],"first":{"x":"value","float":1.0,"negative_zero":-0.0,"signed":-4,"large":18446744073709551615}})");
  ToolCall call{"call","lookup",ToolCallKind::ApprovalRequest,object,"function_call",object};
  InvalidToolCall invalid{"bad","lookup",ToolCallKind::ClientExecuted,"{",InvalidReason::Truncated,"function_call",object};
  Message portable{"portable",Role::Developer,{Text{std::string("a\0b",3)},Refusal{"no","policy"},call,invalid,Thinking{"t",std::string{}},RedactedThinking{"redacted"},ServerToolResult{"server","search_result",object},ToolResult{"call","error",true},Reasoning{"reason",{"one","two"},"encrypted","completed",{"body"}},Opaque{"future",object},Image{"image/png",std::make_shared<const std::string>("AAEC"),ImageDetail::Original},Thought{{"thought"},"sig"}}, {},document(R"([{"raw":"ordered"},{"raw":"group"}])")};
  std::get<ToolResult>(portable.parts[7]).host=ToolResultHostMetadata{"lookup","uncertain",true,true};
  const auto ref=saved(a->save({portable},"portable-call"));a.reset();a=admitted(NativeArchive::open(f.directory(),f.key(),"owner-a",d));auto h=loaded(a->load(ref,"portable-call"));CHECK(h.size()==1 && !h[0].native && h[0].id==portable.id && h[0].role==portable.role && h[0].parts.size()==portable.parts.size());same_document(portable.wire_output,h[0].wire_output);for(size_t i=0;i<portable.parts.size();++i)same_part(portable.parts[i],h[0].parts[i]);
  std::get<Text>(h[0].parts[0]).value="edited portable";const auto edited=saved(a->save(h,"portable-call"));CHECK(edited!=ref);CHECK(!loaded(a->load(edited,"portable-call"))[0].native);
  auto r=request();r.messages.push_back(genuine(d,r));r.messages.push_back(user("continue"));auto native_ref=saved(a->save(r.messages,"native-call"));auto restored=loaded(a->load(native_ref,"native-call"));auto replay=r;replay.messages=restored;CHECK(encoded(d,replay).body==encoded(d,r).body);
  std::get<Thinking>(restored[1].parts[0]).signature="changed";CHECK(std::holds_alternative<Error>(a->save(restored,"native-call")));replay.messages=restored;CHECK(std::holds_alternative<Error>(messages::encode(d,replay,false)));
  replay.messages=loaded(a->load(native_ref,"native-call"));replay.messages[1].native.reset();CHECK(std::holds_alternative<Error>(messages::encode(d,replay,false)));
  replay=r;replay.account_scope="other-account";CHECK(std::holds_alternative<Error>(messages::encode(d,replay,false)));replay=r;replay.model="other-model";CHECK(std::holds_alternative<Error>(messages::encode(d,replay,false)));replay=r;replay.messages.insert(replay.messages.begin(),user("stale prefix"));CHECK(std::holds_alternative<Error>(messages::encode(d,replay,false)));
  replay=r;replay.messages=loaded(a->load(native_ref,"native-call"));replay.max_tokens=1234;
  const auto resized_messages=document(encoded(d,replay).body);
  CHECK(resized_messages->root().get("max_tokens").as_uint()==1234);
  CHECK(resized_messages->root().get("messages").at(1).get("content").at(0).get("signature").as_string()=="synthetic-sensitive-signature");
  auto incomplete=request();incomplete.messages.push_back(genuine(d,incomplete,false));auto incomplete_ref=saved(a->save(incomplete.messages,"partial"));incomplete.messages=loaded(a->load(incomplete_ref,"partial"));CHECK(incomplete.messages[1].native && !incomplete.messages[1].native->complete());CHECK(std::holds_alternative<Error>(messages::encode(d,incomplete,false)));
}
void responses_ordered_restart() {
  Fixture f;auto d=desc("openai.responses");auto a=admitted(NativeArchive::provision(f.directory(),f.key(),"responses-owner",d));responses::Request r;r.model="fixture-model";r.account_scope="fixture-account";r.max_output_tokens=128;r.messages.push_back(user("hello"));auto e=responses::encode(d,r,false);CHECK(std::holds_alternative<responses::EncodedRequest>(e));
  Accumulator acc;responses::Codec codec(d,responses::Mode::Buffered,acc,std::get<responses::EncodedRequest>(e).context);
  codec.buffered(R"({"id":"resp_archive","object":"response","created_at":1,"model":"fixture-model","status":"completed","output":[{"id":"rs_1","type":"reasoning","status":"completed","summary":[{"type":"summary_text","text":"reason"}],"encrypted_content":"encrypted-native"},{"id":"msg_1","type":"message","status":"completed","role":"assistant","content":[{"type":"output_text","text":"answer","annotations":[]}]}],"usage":{"input_tokens":2,"output_tokens":3,"total_tokens":5},"incomplete_details":null,"error":null})", {});
  CHECK(acc.outcome() && std::holds_alternative<Completion>(*acc.outcome()));
  auto m = std::get<Completion>(*acc.outcome()).messages.at(0);
  CHECK(m.native && m.native->complete());
  r.messages.push_back(m);
  r.messages.push_back(user("next"));
  auto expected = responses::encode(d, r, false);
  CHECK(std::holds_alternative<responses::EncodedRequest>(expected));
  auto ref = saved(a->save(r.messages, "response-call"));
  a.reset();
  a = admitted(NativeArchive::open(f.directory(), f.key(), "responses-owner", d));
  r.messages = loaded(a->load(ref, "response-call"));
  auto actual = responses::encode(d, r, false);
  CHECK(std::holds_alternative<responses::EncodedRequest>(actual));
  CHECK(std::get<responses::EncodedRequest>(actual).body == std::get<responses::EncodedRequest>(expected).body);
  same_document(m.wire_output, r.messages[1].wire_output);
  r.messages[1].wire_output=document(R"([])");CHECK(std::holds_alternative<Error>(responses::encode(d,r,false)));CHECK(std::holds_alternative<Error>(a->save(r.messages,"response-call")));
}
void chat_encrypted_legacy_to_canonical_restart() {
  Fixture f;auto d=desc("openai.chat","https://openrouter.ai");
  chat::Request initial;initial.model="fixture-model";initial.max_output_tokens=128;initial.messages.push_back({Role::User,"hello"});
  auto e=chat::encode(d,initial,false);CHECK(std::holds_alternative<chat::EncodedRequest>(e));
  Accumulator acc;chat::Codec codec(d,chat::Mode::Buffered,acc,{},std::get<chat::EncodedRequest>(e).context);
  CHECK(codec.buffered(R"({"id":"chat_archive","object":"chat.completion","created":1,"model":"fixture-model","choices":[{"index":0,"message":{"role":"assistant","content":"answer","reasoning":"consider","reasoning_details":[{"type":"reasoning.encrypted","data":"synthetic-encrypted","id":"rd_1","format":"provider-native","index":0}]},"finish_reason":"stop"}],"usage":{"prompt_tokens":2,"completion_tokens":3,"total_tokens":5}})",{}));
  CHECK(acc.outcome() && std::holds_alternative<Completion>(*acc.outcome()));const auto native=std::get<Completion>(*acc.outcome()).messages.at(0);CHECK(native.native && native.native->complete());
  auto follow=initial;follow.messages.clear();follow.canonical_messages={user("hello"),native,user("continue")};
  auto expected=chat::encode(d,follow,false);CHECK(std::holds_alternative<chat::EncodedRequest>(expected));
  auto a=admitted(NativeArchive::provision(f.directory(),f.key(),"chat-owner",d));auto ref=saved(a->save(follow.canonical_messages,"chat-call"));a.reset();
  a=admitted(NativeArchive::open(f.directory(),f.key(),"chat-owner",d));follow.canonical_messages=loaded(a->load(ref,"chat-call"));
  auto actual=chat::encode(d,follow,false);CHECK(std::holds_alternative<chat::EncodedRequest>(actual));CHECK(std::get<chat::EncodedRequest>(actual).body==std::get<chat::EncodedRequest>(expected).body);
  follow.canonical_messages[1].native.reset();CHECK(std::holds_alternative<Error>(chat::encode(d,follow,false)));
  follow.canonical_messages=loaded(a->load(ref,"chat-call"));follow.max_output_tokens=256;
  const auto resized_chat=chat::encode(d,follow,false);CHECK(std::holds_alternative<chat::EncodedRequest>(resized_chat));
  const auto resized_body=document(std::get<chat::EncodedRequest>(resized_chat).body);
  CHECK(resized_body->root().get("max_tokens").as_uint()==256);
  CHECK(resized_body->root().get("messages").at(1).get("reasoning_details").at(0).get("data").as_string()=="synthetic-encrypted");
  follow.max_output_tokens=128;
  for(auto& part:follow.canonical_messages[1].parts)if(auto* opaque=std::get_if<Opaque>(&part))opaque->wire_metadata=document(R"({"type":"reasoning_details","details":[]})");
  CHECK(std::holds_alternative<Error>(chat::encode(d,follow,false)));CHECK(std::holds_alternative<Error>(a->save(follow.canonical_messages,"chat-call")));
}
void google_tool_loop_restart() {
  {
    Fixture f;
    auto admitted_descriptor=descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"archive-gemini","family":"google.generate","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1beta/models/fixture-model:generateContent","streaming":"/v1beta/models/fixture-model:streamGenerateContent?alt=sse"}}})");
    CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(admitted_descriptor));auto d=std::get<descriptor::ValidatedDescriptor>(std::move(admitted_descriptor));
    gemini::Request r;r.model="fixture-model";r.account_scope="fixture-account";r.max_output_tokens=2048;r.thinking_budget=1024;r.messages.push_back(user("question"));r.tools.push_back({"lookup","Find value",document(R"({"type":"object","properties":{"x":{"type":"integer"}}})")});
    auto e=gemini::encode(d,r,false);CHECK(std::holds_alternative<gemini::EncodedRequest>(e));Accumulator acc;gemini::Codec codec(d,gemini::Mode::Buffered,acc,std::get<gemini::EncodedRequest>(e).context);
    codec.buffered(R"({"modelVersion":"fixture-model","responseId":"generation","candidates":[{"index":0,"content":{"role":"model","parts":[{"text":"Consider","thought":true,"thoughtSignature":"THOUGHT_SIG"},{"functionCall":{"id":"call_owned","name":"lookup","args":{"x":1}},"thoughtSignature":"CALL_SIG"},{"thoughtSignature":"EMPTY_SIG"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":7,"thoughtsTokenCount":3,"totalTokenCount":20}})",{});
    CHECK(acc.outcome() && std::holds_alternative<Completion>(*acc.outcome()));r.messages.push_back(std::get<Completion>(*acc.outcome()).messages.at(0));r.messages.push_back(Message{"",Role::Tool,{ToolResult{"call_owned",R"({"result":1})"}}});
    auto expected=gemini::encode(d,r,false);CHECK(std::holds_alternative<gemini::EncodedRequest>(expected));auto a=admitted(NativeArchive::provision(f.directory(),f.key(),"google-owner",d));auto ref=saved(a->save(r.messages,"gemini-tool-call"));a.reset();a=admitted(NativeArchive::open(f.directory(),f.key(),"google-owner",d));r.messages=loaded(a->load(ref,"gemini-tool-call"));auto actual=gemini::encode(d,r,false);CHECK(std::holds_alternative<gemini::EncodedRequest>(actual));CHECK(std::get<gemini::EncodedRequest>(actual).body==std::get<gemini::EncodedRequest>(expected).body);
    std::get<Thinking>(r.messages[1].parts[0]).signature="edited";CHECK(std::holds_alternative<Error>(gemini::encode(d,r,false)));CHECK(std::holds_alternative<Error>(a->save(r.messages,"gemini-tool-call")));
  }
  {
    Fixture f;
    auto admitted_descriptor=descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"archive-interactions","family":"google.interactions","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1beta/interactions","streaming":"/v1beta/interactions"}}})");
    CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(admitted_descriptor));auto d=std::get<descriptor::ValidatedDescriptor>(std::move(admitted_descriptor));
    interactions::Request r;r.model="fixture-model";r.account_scope="fixture-account";r.max_output_tokens=128;r.thinking_level="low";r.messages.push_back(user("question"));r.tools.push_back({"lookup","Find",document(R"({"type":"object","properties":{"x":{"type":"integer"}}})")});
    auto e=interactions::encode(d,r,false);CHECK(std::holds_alternative<interactions::EncodedRequest>(e));Accumulator acc;interactions::Codec codec(d,interactions::Mode::Buffered,acc,std::get<interactions::EncodedRequest>(e).context);
    codec.buffered(R"({"id":"i1","model":"fixture-model","status":"requires_action","steps":[{"type":"thought","signature":"FINAL_OPAQUE","summary":[{"type":"text","text":"first"},{"type":"text","text":"second"}],"extra":"retain"},{"type":"function_call","id":"call1","name":"lookup","arguments":{"x":1}}],"usage":{"total_input_tokens":100,"total_output_tokens":25,"total_thought_tokens":22,"total_tokens":147,"total_tool_use_tokens":50}})",{});
    CHECK(acc.outcome() && std::holds_alternative<Completion>(*acc.outcome()));r.messages.push_back(std::get<Completion>(*acc.outcome()).messages.at(0));r.messages.push_back(Message{"",Role::Tool,{ToolResult{"call1","one"}}});
    auto expected=interactions::encode(d,r,false);CHECK(std::holds_alternative<interactions::EncodedRequest>(expected));auto a=admitted(NativeArchive::provision(f.directory(),f.key(),"google-owner",d));auto ref=saved(a->save(r.messages,"interactions-tool-call"));a.reset();a=admitted(NativeArchive::open(f.directory(),f.key(),"google-owner",d));r.messages=loaded(a->load(ref,"interactions-tool-call"));auto actual=interactions::encode(d,r,false);CHECK(std::holds_alternative<interactions::EncodedRequest>(actual));CHECK(std::get<interactions::EncodedRequest>(actual).body==std::get<interactions::EncodedRequest>(expected).body);
    std::get<Thought>(r.messages[1].parts[0]).signature="edited";CHECK(std::holds_alternative<Error>(interactions::encode(d,r,false)));CHECK(std::holds_alternative<Error>(a->save(r.messages,"interactions-tool-call")));
  }
}
void deprivileged_decode_preserves_result_without_issuing_seals() {
  auto admitted_descriptor=descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"decode-only-gemini","family":"google.generate","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1beta/models/fixture-model:generateContent","streaming":"/v1beta/models/fixture-model:streamGenerateContent?alt=sse"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(admitted_descriptor));auto d=std::get<descriptor::ValidatedDescriptor>(std::move(admitted_descriptor));
  gemini::Request request;request.model="fixture-model";request.account_scope="fixture-account";request.max_output_tokens=2048;request.thinking_budget=1024;request.messages.push_back(user("question"));request.tools.push_back({"lookup","Find",document(R"({"type":"object","properties":{"x":{"type":"integer"}}})")});
  auto encoded=gemini::encode(d,request,false);CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));const auto original=std::get<gemini::EncodedRequest>(encoded).context;
  auto scope=original->decoding_only();CHECK(original->replay_eligible() && !scope->replay_eligible());CHECK(scope->matches_descriptor(d) && scope->model()=="fixture-model" && scope->client_tool_declared("lookup"));CHECK(!scope->decoding_only()->replay_eligible());
  const auto parts=document(R"([{"text":"Consider","thought":true,"thoughtSignature":"DECODE_ONLY_THOUGHT"},{"functionCall":{"id":"call_control","name":"lookup","args":{"x":1}},"thoughtSignature":"DECODE_ONLY_CALL"}])");
  Accumulator accumulator;gemini::Codec codec(d,gemini::Mode::Buffered,accumulator,scope);
  codec.buffered("{\"modelVersion\":\"fixture-model\",\"responseId\":\"control\",\"candidates\":[{\"index\":0,\"content\":{\"role\":\"model\",\"parts\":"+parts->root().dump()+"},\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"promptTokenCount\":10,\"candidatesTokenCount\":7,\"thoughtsTokenCount\":3,\"totalTokenCount\":20}}",{});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));const auto& result=std::get<Completion>(*accumulator.outcome());const auto& message=result.messages.at(0);
  CHECK(!message.native && message.wire_output && json::equal(message.wire_output->root(),parts->root()));CHECK(std::get<Thinking>(message.parts.at(0)).signature=="DECODE_ONLY_THOUGHT");CHECK(std::get<ToolCall>(message.parts.at(1)).name=="lookup");
  CHECK(result.usage.input_total && result.usage.input_total->value==10 && result.usage.output_total && result.usage.output_total->value==10 && result.stop.kind==StopKind::ToolUse);
  Fixture fixture;auto archive=admitted(NativeArchive::provision(fixture.directory(),fixture.key(),"decode-control",d));auto reference=saved(archive->save({message},"nonissuing-result"));auto restored=loaded(archive->load(reference,"nonissuing-result"));CHECK(!restored.at(0).native);same_document(message.wire_output,restored.at(0).wire_output);
  request.messages.push_back(restored.at(0));request.messages.push_back(Message{"",Role::Tool,{ToolResult{"call_control","one"}}});CHECK(std::holds_alternative<Error>(gemini::encode(d,request,false)));
}
void diagnostic_documents_roundtrip_without_executable_upgrade() {
  Fixture fixture;auto d=desc();auto archive=admitted(NativeArchive::provision(fixture.directory(),fixture.key(),"diagnostic-owner",d));
  auto parsed=json::parse(R"({"x":1,"x":2})");CHECK(std::holds_alternative<json::ParseError>(parsed));
  const auto diagnostic=std::make_shared<const json::Document>(std::move(std::get<json::ParseError>(parsed).context));
  Message message{"diagnostic",Role::Assistant,{InvalidToolCall{"bad","lookup",ToolCallKind::ClientExecuted,R"({"x":1,"x":2})",InvalidReason::DuplicateKey,"tool_use",diagnostic},Opaque{"future",diagnostic}}, {},diagnostic};
  auto reference=saved(archive->save({message},"diagnostic-call"));archive.reset();
  archive=admitted(NativeArchive::open(fixture.directory(),fixture.key(),"diagnostic-owner",d));
  auto restored=loaded(archive->load(reference,"diagnostic-call"));CHECK(restored.size()==1 && !restored[0].native);
  CHECK(restored[0].wire_output->root().dump()==R"({"x":1,"x":2})");
  const auto& invalid=std::get<InvalidToolCall>(restored[0].parts[0]);
  CHECK(invalid.id=="bad" && invalid.name=="lookup" && invalid.kind==ToolCallKind::ClientExecuted && invalid.reason==InvalidReason::DuplicateKey);
  CHECK(invalid.raw_fragment==R"({"x":1,"x":2})" && invalid.wire_metadata->root().dump()==R"({"x":1,"x":2})");
  const auto& opaque=std::get<Opaque>(restored[0].parts[1]);
  CHECK(opaque.wire_type=="future" && opaque.wire_metadata->root().dump()==R"({"x":1,"x":2})");
  auto executable=message;executable.parts={ToolCall{"bad","lookup",ToolCallKind::ClientExecuted,diagnostic}};
  const auto invalid_reference=saved(archive->save({executable},"invalid-executable"));
  CHECK(std::holds_alternative<Error>(archive->load(invalid_reference,"invalid-executable")));
  CHECK(std::holds_alternative<Error>(archive->load("spna2:"+reference.substr(6),"diagnostic-call")));
}
void descriptor_semantics_are_archive_authority() {
  Fixture fixture;auto original=desc();auto archive=admitted(NativeArchive::provision(fixture.directory(),fixture.key(),"semantic-owner",original));
  const std::string prefix=R"({"descriptor_version":1,"revision":1,"id":"archive-fixture","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}})";
  for(const auto mutation:std::array<std::string_view,6>{
      R"(,"bindings":{"model":"other_model"}})",R"(,"bindings":{"messages":"other_messages"}})",
      R"(,"bindings":{"stream":"other_stream"}})",R"(,"bindings":{"max_output_tokens":"other_cap"}})",
      R"(,"bindings":{"usage":["other_usage"]}})",R"(,"stop_reasons":{"custom_stop":"EndTurn"}})"}) {
    auto loaded_descriptor=descriptor::load(prefix+std::string(mutation));CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded_descriptor));
    auto changed=std::get<descriptor::ValidatedDescriptor>(std::move(loaded_descriptor));
    CHECK(!archive->matches_descriptor(changed));
    CHECK(std::holds_alternative<Error>(NativeArchive::open(fixture.directory(),fixture.key(),"semantic-owner",changed)));
  }
  CHECK(archive->matches_descriptor(original));
}
void process_restart(const char* executable) {
  Fixture f;auto d=desc();auto r=request();r.messages.push_back(genuine(d,r));r.messages.push_back(user("next"));auto a=admitted(NativeArchive::provision(f.directory(),f.key(),"restart-owner",d));const auto ref=saved(a->save(r.messages,"restart-call"));const auto expected=digest(encoded(d,r).body);a.reset();
  const auto child=::fork();CHECK(child>=0);if(child==0){::execl(executable,executable,"--restart",f.root.c_str(),ref.c_str(),expected.c_str(),static_cast<char*>(nullptr));::_exit(127);}int status=0;CHECK(::waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
}
void identity_tamper_permissions_and_bounds() {
  Fixture f;auto d=desc();auto a=admitted(NativeArchive::provision(f.directory(),f.key(),"owner-a",d));const auto ref=saved(a->save({user("first")},"call-one"));const auto path=f.directory()+"/"+ref.substr(6);
  CHECK(a->matches_descriptor(d));CHECK(!a->matches_descriptor(desc("anthropic.messages","http://127.0.0.1:18081")));
  CHECK(std::holds_alternative<Error>(a->load(ref,"other-call")));CHECK(std::holds_alternative<Error>(a->load("spna3:"+std::string(64,'0'),"call-one")));CHECK(std::holds_alternative<Error>(NativeArchive::open(f.directory(),f.key(),"owner-b",d)));CHECK(std::holds_alternative<Error>(NativeArchive::open(f.directory(),f.key(),"owner-a",desc("anthropic.messages","http://127.0.0.1:18081"))));
  CHECK(::chmod(path.c_str(),0644)==0);CHECK(std::holds_alternative<Error>(a->load(ref,"call-one")));CHECK(::chmod(path.c_str(),0400)==0);
  const auto original=path+".original";CHECK(::rename(path.c_str(),original.c_str())==0);CHECK(::symlink(original.c_str(),path.c_str())==0);CHECK(std::holds_alternative<Error>(a->load(ref,"call-one")));CHECK(::unlink(path.c_str())==0 && ::rename(original.c_str(),path.c_str())==0);
  CHECK(::chmod(path.c_str(),0600)==0);int fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);CHECK(fd>=0);char byte=0;CHECK(::pread(fd,&byte,1,16)==1);byte^=1;CHECK(::pwrite(fd,&byte,1,16)==1);CHECK(::fchmod(fd,0400)==0);::close(fd);CHECK(std::holds_alternative<Error>(a->load(ref,"call-one")));
  CHECK(::chmod(path.c_str(),0600)==0);CHECK(::truncate(path.c_str(),7)==0 && ::chmod(path.c_str(),0400)==0);CHECK(std::holds_alternative<Error>(a->load(ref,"call-one")));
  const auto old=f.directory()+"-old";CHECK(::rename(f.directory().c_str(),old.c_str())==0 && ::mkdir(f.directory().c_str(),0700)==0);CHECK(std::holds_alternative<Error>(a->load(ref,"call-one")));CHECK(std::holds_alternative<Error>(NativeArchive::open(f.directory(),f.key(),"owner-a",d)));
  CHECK(!a->matches_descriptor(d));
  Fixture limited;NativeArchiveLimits limits;limits.max_records=1;auto bounded=admitted(NativeArchive::provision(limited.directory(),limited.key(),"limited",d,limits));auto first=saved(bounded->save({user("one")}));CHECK(saved(bounded->save({user("one")}))==first);CHECK(std::holds_alternative<Error>(bounded->save({user("two")})));bounded.reset();bounded=admitted(NativeArchive::open(limited.directory(),limited.key(),"limited",d,limits));CHECK(std::holds_alternative<Error>(bounded->save({user("two")})));CHECK(std::get<Text>(loaded(bounded->load(first))[0].parts[0]).value=="one");
  CHECK(::chmod(limited.key().c_str(),0600)==0);CHECK(std::holds_alternative<Error>(bounded->load(first)));CHECK(::chmod(limited.key().c_str(),0400)==0);const auto moved_key=limited.key()+".old";CHECK(::rename(limited.key().c_str(),moved_key.c_str())==0);std::filesystem::copy_file(moved_key,limited.key());CHECK(::chmod(limited.key().c_str(),0400)==0);CHECK(std::holds_alternative<Error>(bounded->load(first)));
  CHECK(!bounded->matches_descriptor(d));
  CHECK(std::holds_alternative<Error>(NativeArchive::open(limited.directory(),limited.key(),"limited",d,limits)));
  Fixture weak;CHECK(::chmod(weak.root.c_str(),0755)==0);CHECK(std::holds_alternative<Error>(NativeArchive::provision(weak.directory(),weak.key(),"owner",d)));
}
}
int main(int argc,char** argv) {
  try {
    if(argc==5 && std::string_view(argv[1])=="--restart") {const std::string root=argv[2];auto d=desc();auto a=admitted(NativeArchive::open(root+"/records",root+"/activation","restart-owner",d));auto r=request();r.messages=loaded(a->load(argv[3],"restart-call"));CHECK(digest(encoded(d,r).body)==argv[4]);return 0;}
    full_portable_and_native_roundtrip();responses_ordered_restart();chat_encrypted_legacy_to_canonical_restart();google_tool_loop_restart();deprivileged_decode_preserves_result_without_issuing_seals();diagnostic_documents_roundtrip_without_executable_upgrade();descriptor_semantics_are_archive_authority();process_restart(argv[0]);identity_tamper_permissions_and_bounds();std::cout<<"native archive behavioral tests passed\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
