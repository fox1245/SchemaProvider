// Provider files (D7): the request each provider shape takes for an upload, a read, a listing, a deletion and a
// download; the one `files::File` every response becomes; its `Media::file` reference; and the runtime's file
// slice upload.
#include "codecs/files.h"
#include "crypto/crypto.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include "sp/config_defaults.h"
#include "support/endpoint_fakes.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace sp;
using namespace sp::test;
using namespace std::chrono_literals;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
constexpr std::string_view openai = "https://api.openai.com", anthropic = "https://api.anthropic.com",
                           google = "https://generativelanguage.googleapis.com", router = "https://openrouter.ai";
descriptor::ValidatedDescriptor at(std::string_view origin, descriptor::PolicySnapshot policy = descriptor::builtin_policy()) {
  const std::string family = origin == google ? "google.generate" : origin == anthropic ? "anthropic.messages" : "openai.chat";
  const std::string path = origin == google ? "/v1beta/models/{model}:generateContent" : origin == anthropic ? "/v1/messages" : "/v1/chat/completions";
  const std::string stream = origin == google ? "/v1beta/models/{model}:streamGenerateContent?alt=sse" : path;
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"files-fixture","family":)" + json::quote(family) +
                                 R"(,"connection":{"base_url":)" + json::quote(origin) + R"(,"paths":{"buffered":)" + json::quote(path) +
                                 R"(,"streaming":)" + json::quote(stream) + "}}}", std::move(policy));
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::shared_ptr<const json::Document> document(std::string_view text) {
  auto parsed = json::parse(text);
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
template <class Request> endpoints::Encoded encoded(const descriptor::ValidatedDescriptor& d, const Request& r) {
  auto result = files::encode(d, r);
  if (const auto* error = std::get_if<Error>(&result)) throw std::runtime_error("encode refused: " + error->safe_message);
  return std::get<endpoints::Encoded>(std::move(result));
}
template <class Request> ErrorKind refused(const descriptor::ValidatedDescriptor& d, const Request& r) {
  auto result = files::encode(d, r);
  CHECK(std::holds_alternative<Error>(result));
  return std::get<Error>(result).kind;
}
std::string boundary_of(const endpoints::Encoded& wire) { return wire.content_type.substr(wire.content_type.find("boundary=") + 9); }
std::string_view extra_header(const endpoints::Encoded& wire, std::string_view name) {
  for (const auto& [key, value] : wire.headers) if (key == name) return value;
  return {};
}

void uploads() {
  const auto note = std::make_shared<const std::string>("hello from a multipart upload\n");
  files::Upload upload{{note}, "note.txt", "text/plain", "user_data", 3600};
  const auto direct = encoded(at(openai), upload);
  const auto b = boundary_of(direct);
  CHECK(direct.path == "/v1/files" && direct.method == "POST" && direct.content_type.starts_with("multipart/form-data; boundary=sp-boundary-") && !direct.charge_on_accept);
  CHECK(direct.body == "--" + b + "\r\nContent-Disposition: form-data; name=\"purpose\"\r\n\r\nuser_data\r\n--" + b +
                           "\r\nContent-Disposition: form-data; name=\"expires_after[anchor]\"\r\n\r\ncreated_at\r\n--" + b +
                           "\r\nContent-Disposition: form-data; name=\"expires_after[seconds]\"\r\n\r\n3600\r\n--" + b +
                           "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"note.txt\"\r\nContent-Type: text/plain\r\n\r\n" + *note + "\r\n--" + b + "--\r\n");
  // Anthropic takes the file alone; Google its metadata, then the content, as one multipart/related body.
  auto plain = upload; plain.purpose.clear(); plain.expires_after.reset();
  const auto claude = encoded(at(anthropic), plain);
  CHECK(claude.path == "/v1/files" && claude.auth == "x-api-key" && extra_header(claude, "anthropic-version") == "2023-06-01" &&
        claude.body.find("purpose") == std::string::npos &&
        claude.body.ends_with(*note + "\r\n--" + boundary_of(claude) + "--\r\n"));
  const auto gemini = encoded(at(google), plain);
  const auto g = boundary_of(gemini);
  CHECK(gemini.path == "/upload/v1beta/files" && gemini.content_type == "multipart/related; boundary=" + g && extra_header(gemini, "X-Goog-Upload-Protocol") == "multipart" &&
        gemini.body == "--" + g + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n{\"file\":{\"display_name\":\"note.txt\"}}\r\n--" + g +
                           "\r\nContent-Type: text/plain\r\n\r\n" + *note + "\r\n--" + g + "--\r\n");
  // A file slice: the multipart framing in memory, the content read as it is sent.
  auto sliced = plain;
  sliced.source = {nullptr, {"/data/clip.mp4", 4096, 1u << 30, std::string(64, 'a')}};
  sliced.mime = "video/mp4";
  const auto streamed = encoded(at(google), sliced);
  CHECK(streamed.file && streamed.file->path == "/data/clip.mp4" && streamed.file->offset == 4096 && streamed.file->size == (1u << 30) &&
        streamed.body.ends_with("Content-Type: video/mp4\r\n\r\n") && streamed.body_suffix == "\r\n--" + boundary_of(streamed) + "--\r\n");
  // Refusals before any I/O.
  auto both = sliced; both.source.bytes = note;
  CHECK(refused(at(google), both) == ErrorKind::InvalidRequest);
  auto empty = plain; empty.source.bytes = std::make_shared<const std::string>();
  CHECK(refused(at(anthropic), empty) == ErrorKind::InvalidRequest);
  auto quoted = plain; quoted.filename = "a\"b.txt";
  CHECK(refused(at(anthropic), quoted) == ErrorKind::InvalidRequest);
  auto typeless = plain; typeless.mime = "text";
  CHECK(refused(at(anthropic), typeless) == ErrorKind::InvalidRequest);
  auto unhashed = sliced; unhashed.source.file.sha256 = "ABC";
  CHECK(refused(at(google), unhashed) == ErrorKind::InvalidRequest);
  CHECK(refused(at(openai), plain) == ErrorKind::InvalidRequest);          // OpenAI needs a purpose
  CHECK(refused(at(anthropic), upload) == ErrorKind::Unsupported);         // the others take none
  auto huge = plain; huge.source.bytes = std::make_shared<const std::string>(std::size_t{17} << 20, 'x');
  CHECK(refused(at(anthropic), huge) == ErrorKind::ResourceLimit);
}

void reads() {
  const auto get = encoded(at(openai), files::Get{"file-NR7M3NLRVMCb89QpK7DBD3"});
  CHECK(get.path == "/v1/files/file-NR7M3NLRVMCb89QpK7DBD3" && get.method == "GET" && get.content_type.empty() && get.model == "file-NR7M3NLRVMCb89QpK7DBD3");
  CHECK(encoded(at(google), files::Get{"files/ji5y1o0uxvgc"}).path == "/v1beta/files/ji5y1o0uxvgc");
  const auto del = encoded(at(anthropic), files::Delete{"file_01FtPPJTTcSwESaZdfVzgCK1"});
  CHECK(del.method == "DELETE" && del.path == "/v1/files/file_01FtPPJTTcSwESaZdfVzgCK1" && del.body.empty());
  CHECK(refused(at(google), files::Get{"ji5y1o0uxvgc"}) == ErrorKind::InvalidRequest);
  CHECK(refused(at(openai), files::Delete{"../v1/batches"}) == ErrorKind::InvalidRequest);
  CHECK(refused(at(google), files::Get{"files/../models"}) == ErrorKind::InvalidRequest);
  // `.` and `..` are made of token characters but are not names: they move the request to another path.
  for (const auto origin : {openai, anthropic})
    for (const char* id : {".", ".."}) {
      CHECK(refused(at(origin), files::Get{id}) == ErrorKind::InvalidRequest);
      CHECK(refused(at(origin), files::Delete{id}) == ErrorKind::InvalidRequest);
      CHECK(refused(at(origin), files::Download{id, "/tmp/out"}) == ErrorKind::InvalidRequest);
    }
  for (const char* id : {"files/.", "files/.."}) {
    CHECK(refused(at(google), files::Get{id}) == ErrorKind::InvalidRequest);
    CHECK(refused(at(google), files::Delete{id}) == ErrorKind::InvalidRequest);
    CHECK(refused(at(google), files::Download{id, "/tmp/out"}) == ErrorKind::InvalidRequest);
  }
  CHECK(encoded(at(openai), files::Get{"file-a..b"}).path == "/v1/files/file-a..b");
  CHECK(encoded(at(openai), files::List{20, "file-abc", "batch"}).path == "/v1/files?limit=20&after=file-abc&purpose=batch");
  CHECK(encoded(at(anthropic), files::List{5, "page_eyJh+/="}).path == "/v1/files?limit=5&page=page_eyJh%2B%2F%3D");
  CHECK(encoded(at(google), files::List{10, "tok"}).path == "/v1beta/files?pageSize=10&pageToken=tok");
  CHECK(refused(at(google), files::List{std::nullopt, {}, "batch"}) == ErrorKind::Unsupported);
  const auto download = encoded(at(google), files::Download{"files/4jn7p8olfxjg", "/tmp/clip.mp4"});
  CHECK(download.path == "/v1beta/files/4jn7p8olfxjg:download?alt=media" && download.accept == "*/*" && download.sink == "/tmp/clip.mp4");
  CHECK(encoded(at(openai), files::Download{"file-JAmK8oxEAZEPYrjL4VHYcv"}).path == "/v1/files/file-JAmK8oxEAZEPYrjL4VHYcv/content");
  // A result delivered by URI names its file by that URI, at the same origin only.
  const auto by_uri = encoded(at(google), files::Download{"https://generativelanguage.googleapis.com/v1beta/files/hrn9uffqzcaj:download?alt=media"});
  CHECK(by_uri.path == "/v1beta/files/hrn9uffqzcaj:download?alt=media" && by_uri.model == "files/hrn9uffqzcaj");
  CHECK(encoded(at(google), files::Get{"https://generativelanguage.googleapis.com/v1beta/files/hrn9uffqzcaj"}).path == "/v1beta/files/hrn9uffqzcaj");
  CHECK(refused(at(google), files::Download{"https://example.com/v1beta/files/hrn9uffqzcaj:download?alt=media"}) == ErrorKind::InvalidRequest);
  CHECK(refused(at(google), files::Get{"https://generativelanguage.googleapis.com/v1beta/files/a/../b"}) == ErrorKind::InvalidRequest);
}

sp::Outcome decoded(std::string_view origin, files::Codec::Kind kind, std::string_view body, std::string subject = {}, std::string uploaded = {}) {
  const auto d = at(origin);
  Accumulator accumulator;
  files::Codec codec(d, accumulator, kind, std::move(subject));
  endpoints::Close close;
  close.upload_sha256 = std::move(uploaded);
  codec.buffered(body, std::move(close));
  CHECK(accumulator.outcome());
  return *accumulator.outcome();
}
void decoded_files() {
  using Kind = files::Codec::Kind;
  const auto o = std::get<files::File>(files::typed(decoded(openai, Kind::File,
      R"({"object":"file","id":"file-NR7M3NLRVMCb89QpK7DBD3","purpose":"user_data","filename":"note.txt","bytes":30,"created_at":1792414945,"expires_at":1792418545,"status":"processed","status_details":null})",
      {}, std::string(64, 'b'))));
  CHECK(o.id == "file-NR7M3NLRVMCb89QpK7DBD3" && o.origin == openai && o.bytes == 30u && o.created_at == 1792414945u && o.expires_at == 1792418545u &&
        o.purpose == "user_data" && o.state == files::State::Active && o.uploaded_sha256 == std::string(64, 'b') && o.raw && o.name == "note.txt");
  const auto a = std::get<files::File>(files::typed(decoded(anthropic, Kind::File,
      R"({"type":"file","id":"file_01FtPPJTTcSwESaZdfVzgCK1","size_bytes":30,"created_at":"2026-10-19T13:02:22.150902Z","expires_at":null,"filename":"note.txt","mime_type":"text/plain","downloadable":false})")));
  CHECK(a.id == "file_01FtPPJTTcSwESaZdfVzgCK1" && a.bytes == 30u && a.created_at == 1792414942u && !a.expires_at && a.mime == "text/plain" &&
        a.downloadable == false && a.state == files::State::Active);
  const auto g = std::get<files::File>(files::typed(decoded(google, Kind::File,
      R"({"file":{"name":"files/ji5y1o0uxvgc","displayName":"note.txt","mimeType":"text/plain","sizeBytes":"30","createTime":"2026-10-19T13:02:07.137020Z","expirationTime":"2026-10-21T13:02:06.132779308Z","sha256Hash":"YmZjMTc2YWZiYmU1ZmJmZWExMWQ4MDYxMjZjYmJkM2M0NzE3NzZlZDFjMWQ1MTJjMmI1NGVlNTRkNTA3NjhhOQ==","uri":"https://generativelanguage.googleapis.com/v1beta/files/ji5y1o0uxvgc","state":"ACTIVE","source":"UPLOADED"}})")));
  CHECK(g.id == "files/ji5y1o0uxvgc" && g.uri.ends_with("/v1beta/files/ji5y1o0uxvgc") && g.bytes == 30u && g.created_at == 1792414927u &&
        g.expires_at == 1792587726u && g.state == files::State::Active && g.sha256 == "bfc176afbbe5fbfea11d806126cbbd3c471776ed1c1d512c2b54ee54d50768a9");
  CHECK(std::get<files::File>(files::typed(decoded(google, Kind::File, R"({"name":"files/x","state":"PROCESSING","createTime":"2026-10-19T22:02:07+09:00"})"))).created_at == 1792414927u);
  // Listings: OpenAI says whether more follow its last id, Anthropic and Google name the next page.
  const auto op = std::get<files::Page>(files::typed_page(decoded(openai, Kind::Page,
      R"({"object":"list","data":[{"id":"file-a","bytes":1},{"id":"file-b","bytes":2}],"has_more":true,"first_id":"file-a","last_id":"file-b"})")));
  CHECK(op.files.size() == 2 && op.files[1].id == "file-b" && op.next == "file-b" && op.files[0].raw);
  CHECK(std::get<files::Page>(files::typed_page(decoded(openai, Kind::Page, R"({"data":[],"has_more":false,"last_id":null})"))).next.empty());
  // More pages with no last id would read as the last page: refused.
  for (const char* body : {R"({"data":[],"has_more":true})", R"({"data":[],"has_more":true,"last_id":null})", R"({"data":[],"has_more":true,"last_id":""})"})
    CHECK(std::get<Failure>(decoded(openai, Kind::Page, body)).error.reason == "files.page.cursor_missing");
  const auto ap = std::get<files::Page>(files::typed_page(decoded(anthropic, Kind::Page, R"({"data":[{"type":"file","id":"file_1","downloadable":true}],"next_page":"page_x"})")));
  CHECK(ap.files.size() == 1 && ap.files[0].downloadable == true && ap.next == "page_x");
  CHECK(std::get<files::Page>(files::typed_page(decoded(google, Kind::Page, "{}"))).files.empty());
  CHECK(std::get<files::Page>(files::typed_page(decoded(google, Kind::Page, R"({"files":[{"name":"files/a"}],"nextPageToken":"t2"})"))).next == "t2");
  // Deletions.
  CHECK(std::get<files::Deleted>(files::typed_deleted(decoded(google, Kind::Deleted, "{}", "files/a"))).id == "files/a");
  CHECK(std::get<files::Deleted>(files::typed_deleted(decoded(google, Kind::Deleted, "", "files/a"))).id == "files/a");
  CHECK(std::holds_alternative<files::Deleted>(files::typed_deleted(decoded(anthropic, Kind::Deleted, R"({"id":"file_1","type":"file_deleted"})", "file_1"))));
  CHECK(std::holds_alternative<Failure>(files::typed_deleted(decoded(openai, Kind::Deleted, R"({"id":"file-a","deleted":false})", "file-a"))));
  // Malformed documents.
  for (const auto& [origin, kind, body] : std::initializer_list<std::tuple<std::string_view, Kind, std::string_view>>{
           {openai, Kind::File, R"({"bytes":3})"}, {openai, Kind::File, R"({"id":"file-a","bytes":"3"})"},
           {anthropic, Kind::File, R"({"id":"file_1","created_at":"yesterday"})"}, {google, Kind::File, R"({"name":"models/x"})"},
           {google, Kind::File, R"({"name":"files/x","sizeBytes":30})"}, {openai, Kind::Page, R"({"data":{}})"},
           {anthropic, Kind::Page, R"({"data":[],"next_page":7})"}, {openai, Kind::File, "[]"}}) {
    const auto failed = decoded(origin, kind, body);
    CHECK(std::holds_alternative<Failure>(failed) && std::get<Failure>(failed).error.kind == ErrorKind::ProtocolCorrupt &&
          std::get<Failure>(failed).error.reason.starts_with("files."));
  }
}

void references() {
  files::File file{std::string(google), "files/ji5y1o0uxvgc", "https://generativelanguage.googleapis.com/v1beta/files/ji5y1o0uxvgc", "clip.mp4", "video/mp4"};
  file.state = files::State::Active;
  file.expires_at = 2000;
  const auto media = std::get<Media>(files::media(file, google, 1000));
  CHECK(media.source == MediaSource::File && media.kind == MediaKind::Video && media.reference == file.uri && media.mime == "video/mp4");
  CHECK(std::get<Error>(files::media(file, openai, 1000)).kind == ErrorKind::InvalidRequest);
  CHECK(std::holds_alternative<Error>(files::media(file, google, 2000)));
  auto processing = file; processing.state = files::State::Processing;
  CHECK(std::holds_alternative<Error>(files::media(processing, google, 1000)));
  files::File doc{std::string(anthropic), "file_1", {}, "a.pdf", "application/pdf"};
  doc.state = files::State::Active;
  const auto named = std::get<Media>(files::media(doc, anthropic, 0));
  CHECK(named.kind == MediaKind::Document && named.reference == "file_1");
}

// A chunked upload: each step's request, and the session it answers with.
files::ProgressOutcome progress_of(std::string_view origin, files::SessionCodec::Step step, std::string_view body,
                                   std::vector<std::pair<std::string, std::string>> headers, const files::Session& session, std::uint64_t part = 0) {
  const auto d = at(origin);
  Accumulator accumulator;
  files::SessionCodec codec(d, accumulator, step, files::session_document(session, part), {}, session.url);
  endpoints::Close close;
  close.headers = std::move(headers);
  codec.buffered(body, std::move(close));
  CHECK(accumulator.outcome());
  return files::typed_progress(*accumulator.outcome());
}
void chunked() {
  using Step = files::SessionCodec::Step;
  constexpr std::uint64_t mib8 = 8388608, total = 9437184;
  // OpenAI Uploads.
  const auto start = encoded(at(openai), files::Start{"chunked.txt", "text/plain", total, "user_data", 3600});
  CHECK(start.path == "/v1/uploads" && start.method == "POST" &&
        start.body == R"({"filename":"chunked.txt","purpose":"user_data","bytes":9437184,"mime_type":"text/plain","expires_after":{"anchor":"created_at","seconds":3600}})");
  const auto begun = std::get<files::Progress>(progress_of(openai, Step::Start,
      R"({"id":"upload_6ad61989ee2c8191879c23751f41c77f","object":"upload","bytes":9437184,"created_at":1792416137,"filename":"chunked.txt","purpose":"user_data","status":"pending","expires_at":1792419737})",
      {}, files::Session{std::string(openai), "openai", {}, {}, total}));
  auto session = begun.session;
  CHECK(session.id == "upload_6ad61989ee2c8191879c23751f41c77f" && session.size == total && session.sent == 0 && session.expires_at == 1792419737u &&
        session.status == "pending" && !begun.file);
  const auto part = encoded(at(openai), files::Part{session, {nullptr, {"/data/big.bin", 0, mib8}}});
  CHECK(part.path == "/v1/uploads/upload_6ad61989ee2c8191879c23751f41c77f/parts" && part.file && part.file->size == mib8 &&
        part.body.find("name=\"data\"") != std::string::npos && part.content_type.starts_with("multipart/form-data"));
  // The first part starts at the file's start. A slice that starts elsewhere would put other bytes at the upload's offset.
  CHECK(refused(at(openai), files::Part{session, {nullptr, {"/data/big.bin", 4096, mib8}}}) == ErrorKind::InvalidRequest);
  // The part cap is the fact's (64 MiB at OpenAI): a part of the cap is sent, one byte more is refused.
  {
    constexpr std::uint64_t mib64 = std::uint64_t{64} << 20;
    auto large = session; large.size = std::uint64_t{200} << 20;
    CHECK(encoded(at(openai), files::Part{large, {nullptr, {"/data/big.bin", 0, mib64}}}).file->size == mib64);
    CHECK(refused(at(openai), files::Part{large, {nullptr, {"/data/big.bin", 0, mib64 + 1}}}) == ErrorKind::InvalidRequest);
    CHECK(refused(at(openai), files::Part{large, {std::make_shared<const std::string>(mib64 + 1, 'x')}}) == ErrorKind::InvalidRequest);
  }
  session = std::get<files::Progress>(progress_of(openai, Step::Part, R"({"id":"part_1","object":"upload.part","upload_id":"upload_6ad61989ee2c8191879c23751f41c77f"})",
                                                 {}, session, mib8)).session;
  CHECK(session.sent == mib8 && session.parts == std::vector<std::string>{"part_1"});
  CHECK(refused(at(openai), files::Complete{session}) == ErrorKind::InvalidRequest);          // a part is missing
  CHECK(refused(at(openai), files::Part{session, {nullptr, {"/data/big.bin", 0, total - mib8}}}) == ErrorKind::InvalidRequest);   // the first slice again
  CHECK(encoded(at(openai), files::Part{session, {nullptr, {"/data/big.bin", mib8, total - mib8}}}).file->offset == mib8);
  CHECK(refused(at(openai), files::Part{session, {std::make_shared<const std::string>(std::size_t{2} << 20, 'x')}}) == ErrorKind::InvalidRequest);
  session = std::get<files::Progress>(progress_of(openai, Step::Part, R"({"id":"part_2","object":"upload.part"})", {}, session, total - mib8)).session;
  const auto complete = encoded(at(openai), files::Complete{session});
  CHECK(complete.path == "/v1/uploads/upload_6ad61989ee2c8191879c23751f41c77f/complete" && complete.body == R"({"part_ids":["part_1","part_2"]})");
  const auto done = std::get<files::Progress>(progress_of(openai, Step::Complete,
      R"({"id":"upload_6ad61989ee2c8191879c23751f41c77f","object":"upload","bytes":9437184,"status":"completed","file":{"object":"file","id":"file-WabU99AgMEADp826Ga8M5U","purpose":"user_data","filename":"chunked.txt","bytes":9437184,"created_at":1792416137,"status":"processed"}})",
      {}, session));
  CHECK(done.file && done.file->id == "file-WabU99AgMEADp826Ga8M5U" && done.file->bytes == total && done.session.status == "completed");
  CHECK(std::holds_alternative<Failure>(progress_of(openai, Step::Complete, R"({"id":"upload_x","status":"completed"})", {}, session)));
  CHECK(encoded(at(openai), files::Cancel{session}).path == "/v1/uploads/upload_6ad61989ee2c8191879c23751f41c77f/cancel");
  CHECK(std::get<files::Progress>(progress_of(openai, Step::Cancel, R"({"id":"upload_x","status":"cancelled"})", {}, session)).session.status == "cancelled");
  CHECK(refused(at(openai), files::Query{session}) == ErrorKind::Unsupported);
  CHECK(refused(at(anthropic), files::Start{"a.txt", "text/plain", 10}) == ErrorKind::Unsupported);

  // Google's resumable upload.
  const auto google_start = encoded(at(google), files::Start{"chunked.txt", "text/plain", total});
  CHECK(google_start.path == "/upload/v1beta/files" && extra_header(google_start, "X-Goog-Upload-Protocol") == "resumable" &&
        extra_header(google_start, "X-Goog-Upload-Command") == "start" && extra_header(google_start, "X-Goog-Upload-Header-Content-Length") == "9437184" &&
        extra_header(google_start, "X-Goog-Upload-Header-Content-Type") == "text/plain" && google_start.body == R"({"file":{"display_name":"chunked.txt"}})");
  const std::string url = "https://generativelanguage.googleapis.com/upload/v1beta/files?upload_id=AB-cd_12&upload_protocol=resumable";
  auto upload = std::get<files::Progress>(progress_of(google, Step::Start, "",
      {{"x-goog-upload-status", "active"}, {"x-goog-upload-url", url}, {"x-goog-upload-chunk-granularity", "8388608"}},
      files::Session{std::string(google), "google", {}, {}, total})).session;
  CHECK(upload.url == url && upload.granularity == mib8 && upload.status == "active");
  CHECK(std::holds_alternative<Failure>(progress_of(google, Step::Start, "", {{"x-goog-upload-status", "active"}, {"x-goog-upload-url", "https://evil.example/upload"}},
                                                    files::Session{std::string(google), "google", {}, {}, total})));
  const auto chunk = encoded(at(google), files::Part{upload, {nullptr, {"/data/big.bin", 0, mib8}}});
  CHECK(chunk.path == "/upload/v1beta/files?upload_id=AB-cd_12&upload_protocol=resumable" && extra_header(chunk, "X-Goog-Upload-Command") == "upload" &&
        extra_header(chunk, "X-Goog-Upload-Offset") == "0" && chunk.body.empty() && chunk.file && chunk.content_type == "application/octet-stream");
  CHECK(refused(at(google), files::Part{upload, {nullptr, {"/data/big.bin", 0, 1048576}}}) == ErrorKind::InvalidRequest);   // not a multiple, not last
  // Google states no part cap: a chunk of any multiple of the granularity goes.
  {
    auto large = upload; large.size = std::uint64_t{512} << 20;
    CHECK(encoded(at(google), files::Part{large, {nullptr, {"/data/big.bin", 0, std::uint64_t{128} << 20}}}).file->size == std::uint64_t{128} << 20);
  }
  upload = std::get<files::Progress>(progress_of(google, Step::Part, "", {{"x-goog-upload-status", "active"}}, upload, mib8)).session;
  CHECK(upload.sent == mib8);
  CHECK(refused(at(google), files::Part{upload, {nullptr, {"/data/big.bin", 0, total - mib8}}}) == ErrorKind::InvalidRequest);   // not where the upload stands
  const auto last = encoded(at(google), files::Part{upload, {nullptr, {"/data/big.bin", mib8, total - mib8}}});
  CHECK(extra_header(last, "X-Goog-Upload-Command") == "upload, finalize" && extra_header(last, "X-Goog-Upload-Offset") == "8388608");
  const auto finished = std::get<files::Progress>(progress_of(google, Step::Part,
      R"({"file":{"name":"files/abc","mimeType":"text/plain","sizeBytes":"9437184","state":"ACTIVE","uri":"https://generativelanguage.googleapis.com/v1beta/files/abc"}})",
      {{"x-goog-upload-status", "final"}}, upload, total - mib8));
  CHECK(finished.file && finished.file->id == "files/abc" && finished.session.sent == total && finished.session.status == "final");
  CHECK(std::holds_alternative<Failure>(progress_of(google, Step::Part, "", {{"x-goog-upload-status", "final"}}, upload, total - mib8)));
  CHECK(std::holds_alternative<Failure>(progress_of(google, Step::Part, "", {}, upload, mib8)));
  // A lost answer: the query says how much arrived.
  const auto query = encoded(at(google), files::Query{upload});
  CHECK(extra_header(query, "X-Goog-Upload-Command") == "query" && query.body.empty() && query.content_type.empty());
  auto lost = upload; lost.sent = 0;
  CHECK(std::get<files::Progress>(progress_of(google, Step::Query, "", {{"x-goog-upload-status", "active"}, {"x-goog-upload-size-received", "8388608"}}, lost)).session.sent == mib8);
  CHECK(extra_header(encoded(at(google), files::Cancel{upload}), "X-Goog-Upload-Command") == "cancel");
  CHECK(std::holds_alternative<Failure>(progress_of(google, Step::Cancel, "", {{"x-goog-upload-status", "active"}}, upload)));
  CHECK(refused(at(google), files::Complete{upload}) == ErrorKind::Unsupported);
  // A session reaches only the origin that began it, and Google's only under the upload path.
  auto elsewhere = upload; elsewhere.origin = std::string(openai);
  CHECK(refused(at(google), files::Part{elsewhere, {std::make_shared<const std::string>("x")}}) == ErrorKind::InvalidRequest);
  auto redirected = upload; redirected.url = "https://generativelanguage.googleapis.com/v1beta/models";
  CHECK(refused(at(google), files::Query{redirected}) == ErrorKind::InvalidRequest);
  auto spliced = upload; spliced.url += "\r\nx-goog-api-key: y";
  CHECK(refused(at(google), files::Query{spliced}) == ErrorKind::InvalidRequest);
}

// A session reaches only the upload path of its origin, by names: no dot, empty or encoded segment.
void upload_urls() {
  using Step = files::SessionCodec::Step;
  const std::string base = "https://generativelanguage.googleapis.com";
  CHECK(files::upload_url(base, base + "/upload/v1beta/files?upload_id=AB-cd_12&upload_protocol=resumable"));
  CHECK(files::upload_url(base, base + "/upload/v1beta/files"));
  for (const char* tail : {"/upload/../v1beta/models/x:generateContent", "/upload/./v1beta/files", "/upload/v1beta/../files", "/upload//v1beta/files",
                           "/upload/%2e%2e/v1beta/files", "/upload/%2E/v1beta/files", "/upload/", "/upload/v1beta/files/", "/upload/v1beta/files/..", "/upload/v1beta;x/files",
                           "/upload", "/upload/v1beta/files#x", "/v1beta/files", "/uploads/v1beta/files"})
    CHECK(!files::upload_url(base, base + tail));
  CHECK(!files::upload_url(base, "https://generativelanguage.googleapis.com.example/upload/v1beta/files"));
  CHECK(!files::upload_url(base, "https://example.com/upload/v1beta/files"));
  CHECK(!files::upload_url(base, base + "/upload/v1beta/files?x=\"y"));
  for (const char* tail : {"/upload/../v1beta/models/x:generateContent", "/upload/%2e%2e/v1beta/files", "/upload/v1beta/files/.."}) {
    files::Session session{std::string(google), "google", {}, base + tail, 100, 0, 0};
    CHECK(refused(at(google), files::Query{session}) == ErrorKind::InvalidRequest);
    CHECK(refused(at(google), files::Cancel{session}) == ErrorKind::InvalidRequest);
    CHECK(refused(at(google), files::Part{session, {std::make_shared<const std::string>("x")}}) == ErrorKind::InvalidRequest);
    // The URL a Start answers with passes the same check.
    const auto begun = progress_of(google, Step::Start, "", {{"x-goog-upload-status", "active"}, {"x-goog-upload-url", base + tail}}, files::Session{std::string(google), "google", {}, {}, 100});
    CHECK(std::holds_alternative<Failure>(begun) && std::get<Failure>(begun).error.reason == "files.session.url_required");
  }
  for (const char* id : {".", ".."}) {
    files::Session session{std::string(openai), "openai", id, {}, 100};
    CHECK(refused(at(openai), files::Cancel{session}) == ErrorKind::InvalidRequest);
  }
}

// Digits a server sends are parsed within 64 bits: a longer number fails the call with a reason, and no parse throws
// out of `join()` or keeps `on_outcome` from running.
void digits_fit() {
  using Kind = files::Codec::Kind;
  using Step = files::SessionCodec::Step;
  for (const char* size : {"99999999999999999999", "18446744073709551616", "000000000000000000001"}) {
    const auto body = std::string(R"({"name":"files/x","sizeBytes":")") + size + R"("})";
    const auto outcome = decoded(google, Kind::File, body);
    if (std::string_view(size) == "000000000000000000001") CHECK(std::holds_alternative<Completion>(outcome));
    else CHECK(std::holds_alternative<Failure>(outcome) && std::get<Failure>(outcome).error.kind == ErrorKind::ProtocolCorrupt &&
               std::get<Failure>(outcome).error.reason == "files.file.id_required");
  }
  CHECK(std::get<files::File>(files::typed(decoded(google, Kind::File, R"({"name":"files/x","sizeBytes":"18446744073709551615"})"))).bytes == 18446744073709551615ull);
  const std::string url = "https://generativelanguage.googleapis.com/upload/v1beta/files?upload_id=AB-cd_12&upload_protocol=resumable";
  const files::Session fresh{std::string(google), "google", {}, {}, 100};
  files::Session session{std::string(google), "google", {}, url, 100};
  const auto granularity = progress_of(google, Step::Start, "", {{"x-goog-upload-status", "active"}, {"x-goog-upload-url", url}, {"x-goog-upload-chunk-granularity", "99999999999999999999"}}, fresh);
  CHECK(std::holds_alternative<Failure>(granularity) && std::get<Failure>(granularity).error.reason == "files.session.url_required");
  const auto received = progress_of(google, Step::Query, "", {{"x-goog-upload-status", "active"}, {"x-goog-upload-size-received", "99999999999999999999"}}, session);
  CHECK(std::holds_alternative<Failure>(received) && std::get<Failure>(received).error.reason == "files.session.size_invalid");
  // A result a journal restored is typed without a throw, whatever its records say.
  auto context = document(R"({"step":"query","session":{"origin":"https://generativelanguage.googleapis.com","shape":"google","size":100,"sent":0,"granularity":0,"parts":[],"status":"","part":0},"status":"active","granularity":"99999999999999999999","received":"99999999999999999999"})");
  Completion forged;
  forged.raw_events.push_back(RawWire{"files.session", context});
  const auto typed = std::get<files::Progress>(files::typed_progress(sp::Outcome{forged}));
  CHECK(typed.session.sent == 0 && typed.session.granularity == 0);

  // Through the runtime, the failure is the call's outcome and the callback runs.
  auto executor = std::make_shared<QueueExecutor>();
  auto wire = std::make_shared<ScriptTransport>();
  runtime::Options options;
  options.api_key = "synthetic-key";
  auto client = runtime::detail::ClientAccess::make(at(google), options, executor, wire);
  runtime::RunOptions run;
  run.retry = runtime::RetryPolicy{true, false, 3, 0ms, 0ms};
  run.deadline = executor->now() + 10s;
  wire->script = [](Exchange& e) { e.head(200, {{"x-goog-upload-status", "active"}, {"x-goog-upload-size-received", "99999999999999999999"}}); e.ending = true; e.flush(); };
  bool called = false;
  auto call = client.start(client.prepare(files::Query{session}, run), [&](files::ProgressOutcome) { called = true; });
  executor->drain();
  const auto outcome = call.join();
  CHECK(called && std::holds_alternative<Failure>(outcome) && std::get<Failure>(outcome).error.reason == "files.session.size_invalid");
  wire->script = [](Exchange& e) { e.reply(200, "application/json", R"({"file":{"name":"files/x","sizeBytes":"99999999999999999999"}})"); };
  called = false;
  auto listed = client.start(client.prepare(files::Get{"files/x"}, run), [&](files::Outcome) { called = true; });
  executor->drain();
  const auto got = listed.join();
  CHECK(called && std::holds_alternative<Failure>(got) && std::get<Failure>(got).error.reason == "files.file.id_required");
}

// Google's upload URL lets anyone write the file: it is in the session and in the call's URL, and nowhere a host
// journals a prepared call (its model) or a result (its raw events).
void upload_url_stays_private() {
  using Step = files::SessionCodec::Step;
  constexpr std::uint64_t total = 100, mib8 = 8388608;
  const std::string url = "https://generativelanguage.googleapis.com/upload/v1beta/files?upload_id=SECRET-session_1&upload_protocol=resumable";
  const std::string other_url = "https://generativelanguage.googleapis.com/upload/v1beta/files?upload_id=SECRET-session_2&upload_protocol=resumable";
  const auto hex = [](std::string_view text) {
    std::string out;
    for (const auto byte : crypto::sha256(text)) { out += "0123456789abcdef"[byte >> 4]; out += "0123456789abcdef"[byte & 15]; }
    return out;
  };
  files::Session session{std::string(google), "google", {}, url, total, 0, mib8};
  session.status = "active";
  const auto leaks = [&](std::string_view text) { return text.find("SECRET") != std::string_view::npos || text.find("upload_id") != std::string_view::npos; };
  // The model of every step names the session by the digest of its URL: steady for a session, different between two.
  const auto part = encoded(at(google), files::Part{session, {std::make_shared<const std::string>(std::string(total, 'x'))}});
  CHECK(!leaks(part.model) && part.context.secret == url && part.path.find("SECRET") != std::string::npos);
  const auto model = document(part.model);
  CHECK(model->root().get("url_sha256").as_string() == hex(url) && model->root().get("size").as_uint() == total && model->root().get("part").as_uint() == total);
  CHECK(encoded(at(google), files::Part{session, {std::make_shared<const std::string>(std::string(total, 'y'))}}).model == part.model);
  auto other = session; other.url = other_url;
  CHECK(encoded(at(google), files::Part{other, {std::make_shared<const std::string>(std::string(total, 'x'))}}).model != part.model);
  for (const auto& step : {encoded(at(google), files::Query{session}), encoded(at(google), files::Cancel{session})})
    CHECK(!leaks(step.model) && step.context.secret == url);
  CHECK(!leaks(files::session_document(session)) && files::session_document(files::Session{std::string(google), "google", {}, {}, total}).find("\"url_sha256\":\"\"") != std::string::npos);

  // Every step's answer carries the URL in the typed session, in no raw event.
  const auto clean = [&](const files::Progress& progress) {
    for (const auto& raw : progress.raw_events) CHECK(!raw.payload || !leaks(raw.payload->root().dump()));
  };
  const auto begun = std::get<files::Progress>(progress_of(google, Step::Start, "",
      {{"x-goog-upload-status", "active"}, {"x-goog-upload-url", url}, {"x-goog-upload-chunk-granularity", "8388608"}}, files::Session{std::string(google), "google", {}, {}, total}));
  CHECK(begun.session.url == url && begun.session.granularity == mib8);
  clean(begun);
  bool recorded = false;
  for (const auto& raw : begun.raw_events) if (raw.type == "files.session") recorded = raw.payload->root().get("url_sha256").as_string() == hex(url);
  CHECK(recorded);
  const auto chunk = std::get<files::Progress>(progress_of(google, Step::Part, "", {{"x-goog-upload-status", "active"}}, begun.session, total));
  CHECK(chunk.session.url == url && chunk.session.sent == total);
  clean(chunk);
  const auto asked = std::get<files::Progress>(progress_of(google, Step::Query, "", {{"x-goog-upload-status", "active"}, {"x-goog-upload-size-received", "50"}}, begun.session));
  CHECK(asked.session.url == url && asked.session.sent == 50);
  clean(asked);
  const auto cancelled = std::get<files::Progress>(progress_of(google, Step::Cancel, "", {{"x-goog-upload-status", "cancelled"}}, begun.session));
  CHECK(cancelled.session.url == url);
  clean(cancelled);
  // A failed step has no raw event of the session at all.
  const auto failed = progress_of(google, Step::Part, "", {}, begun.session, total);
  CHECK(std::holds_alternative<Failure>(failed));
  for (const auto& raw : std::get<Failure>(failed).partial.raw_events) CHECK(!raw.payload || !leaks(raw.payload->root().dump()));

  // Through the runtime: the prepared call shows the URL only as its URL, and a result restored without the
  // completion's secret (a journal keeps the raw events) still types, with no URL.
  auto executor = std::make_shared<QueueExecutor>();
  auto wire = std::make_shared<ScriptTransport>();
  runtime::Options options;
  options.api_key = "synthetic-key";
  auto client = runtime::detail::ClientAccess::make(at(google), options, executor, wire);
  runtime::RunOptions run;
  run.retry = runtime::RetryPolicy{true, false, 3, 0ms, 0ms};
  run.deadline = executor->now() + 10s;
  wire->script = [&](Exchange& e) { e.head(200, {{"x-goog-upload-status", "active"}, {"x-goog-upload-url", url}, {"x-goog-upload-chunk-granularity", "8388608"}}); e.ending = true; e.flush(); };
  auto started = client.prepare(files::Start{"chunked.txt", "text/plain", total}, run);
  CHECK(!leaks(started.prepared().model()) && !leaks(started.prepared().url()));
  auto first = client.start(std::move(started));
  executor->drain();
  const auto opened = std::get<files::Progress>(first.join());
  CHECK(opened.session.url == url);
  clean(opened);
  wire->script = [](Exchange& e) { e.head(200, {{"x-goog-upload-status", "active"}}); e.ending = true; e.flush(); };
  auto prepared = client.prepare(files::Part{opened.session, {std::make_shared<const std::string>(std::string(total, 'x'))}}, run);
  CHECK(!leaks(prepared.prepared().model()) && prepared.prepared().url() == url && prepared.prepared().endpoint() == "files.part");
  std::optional<files::ProgressOutcome> delivered;
  auto second = client.start(std::move(prepared), [&](files::ProgressOutcome outcome) { delivered = std::move(outcome); });
  executor->drain();
  const auto advanced = std::get<files::Progress>(second.join());
  CHECK(delivered && std::get<files::Progress>(*delivered).session.url == url && advanced.session.url == url && advanced.session.sent == total);
  clean(advanced);
  // The untyped path: the result a host journals holds no URL in its wire records.
  auto untyped = client.prepare(files::Query{opened.session}, run);
  wire->script = [](Exchange& e) { e.head(200, {{"x-goog-upload-status", "active"}, {"x-goog-upload-size-received", "50"}}); e.ending = true; e.flush(); };
  std::optional<runtime::Result> result;
  runtime::Callbacks callbacks;
  callbacks.on_outcome = [&](runtime::Result value) { result = std::move(value); };
  auto third = client.start(std::move(untyped).release(), std::move(callbacks));
  executor->drain();
  third.join();
  CHECK(result);
  const auto& completion = std::get<Completion>(**result);
  for (const auto& raw : completion.raw_events) CHECK(!raw.payload || !leaks(raw.payload->root().dump()));
  Completion restored = completion;
  restored.secret.clear();
  const auto retyped = std::get<files::Progress>(files::typed_progress(sp::Outcome{restored}));
  CHECK(retyped.session.sent == 50 && retyped.session.url.empty());
}

// The runtime sends a file slice as the wire's file body and reports the transport's digest on the file.
void runtime_upload() {
  auto executor = std::make_shared<QueueExecutor>();
  auto wire = std::make_shared<ScriptTransport>();
  runtime::Options options;
  options.api_key = "synthetic-key";
  auto client = runtime::detail::ClientAccess::make(at(anthropic), options, executor, wire);
  runtime::RunOptions run;
  run.retry = runtime::RetryPolicy{true, false, 3, 0ms, 0ms};
  const auto run_for = [&] { auto value = run; value.deadline = executor->now() + 10s; return value; };
  const auto complete = [&](auto prepared) { auto call = client.start(std::move(prepared)); executor->drain(); return call.join(); };
  wire->script = [](Exchange& e) {
    e.result.upload_sha256 = std::string(64, 'c');
    e.reply(200, "application/json", R"({"type":"file","id":"file_9","size_bytes":4096,"filename":"clip.bin","mime_type":"application/octet-stream","downloadable":false})");
  };
  const auto uploaded = complete(client.prepare(files::Upload{{nullptr, {"/data/clip.bin", 0, 4096, std::string(64, 'c')}}, "clip.bin", "application/octet-stream"}, run_for()));
  const auto& request = wire->exchanges.back()->request;
  CHECK(request.method == "POST" && request.url == "https://api.anthropic.com/v1/files" && request.body.empty() && request.file_body &&
        request.file_body->path == "/data/clip.bin" && request.file_body->size == 4096 && request.file_body->expected_sha256 == std::string(64, 'c') &&
        request.file_body->prefix.ends_with("Content-Type: application/octet-stream\r\n\r\n") && request.file_body->suffix.ends_with("--\r\n"));
  CHECK(header(request, "x-api-key") == "synthetic-key");
  const auto& file = std::get<files::File>(uploaded);
  CHECK(file.id == "file_9" && file.uploaded_sha256 == std::string(64, 'c') && file.attempt.attempts == 1);

  // A failed upload that may have reached the provider is not sent again; a read is.
  std::size_t calls = 0;
  wire->script = [&](Exchange& e) {
    if (++calls == 1) { e.head(200, {{"content-type", "application/json"}}); e.finish(transport::Status::Failed, transport::FailureKind::Receive); }
    else e.reply(200, "application/json", R"({"type":"file","id":"file_9"})");
  };
  CHECK(std::holds_alternative<Failure>(complete(client.prepare(files::Upload{{std::make_shared<const std::string>("x")}, "x.txt", "text/plain"}, run_for()))) && calls == 1);
  calls = 0;
  CHECK(std::holds_alternative<files::File>(complete(client.prepare(files::Get{"file_9"}, run_for()))) && calls == 2);
  // A file body the transport could not read is the caller's request, not the provider's failure.
  wire->script = [](Exchange& e) { e.finish(transport::Status::Failed, transport::FailureKind::UploadSource); };
  const auto unreadable = complete(client.prepare(files::Upload{{nullptr, {"/data/gone.bin", 0, 10}}, "gone.bin", "application/octet-stream"}, run_for()));
  CHECK(std::get<Failure>(unreadable).error.kind == ErrorKind::InvalidRequest && std::get<Failure>(unreadable).error.reason == "transport.upload.source");

  // Bytes in memory go whole in the body, and the file reports their digest, whatever the transport reports: here an
  // OpenAI batch input file, whose digest a batch bound compares with the items.
  auto openai_client = runtime::detail::ClientAccess::make(at(openai), options, executor, wire);
  const auto jsonl = std::make_shared<const std::string>("{\"custom_id\":\"o-1\",\"method\":\"POST\",\"url\":\"/v1/chat/completions\",\"body\":{}}\n");
  std::string expected;
  for (const auto byte : crypto::sha256(*jsonl)) { expected += "0123456789abcdef"[byte >> 4]; expected += "0123456789abcdef"[byte & 15]; }
  wire->script = [](Exchange& e) { e.reply(200, "application/json", R"({"id":"file-abc","object":"file","bytes":64,"filename":"batch.jsonl","purpose":"batch"})"); };
  files::Upload batch_input{{jsonl}, "batch.jsonl", "application/jsonl"};
  batch_input.purpose = "batch";
  const auto start_openai = [&](auto prepared) { auto call = openai_client.start(std::move(prepared)); executor->drain(); return call.join(); };
  const auto in_memory = std::get<files::File>(start_openai(openai_client.prepare(batch_input, run_for())));
  CHECK(wire->exchanges.back()->request.body.find(*jsonl) != std::string::npos && !wire->exchanges.back()->request.file_body);
  CHECK(in_memory.id == "file-abc" && in_memory.uploaded_sha256 == expected && in_memory.uploaded_sha256.size() == 64);
  // A read of the file names no upload, so no digest.
  wire->script = [](Exchange& e) { e.reply(200, "application/json", R"({"id":"file-abc","object":"file","purpose":"batch"})"); };
  CHECK(std::get<files::File>(start_openai(openai_client.prepare(files::Get{"file-abc"}, run_for()))).uploaded_sha256.empty());
}
// A file's content is a document whatever its type; the file the caller named changes only when the download was
// accepted, and a download that was not leaves what was there.
void runtime_download() {
  auto executor = std::make_shared<QueueExecutor>();
  auto wire = std::make_shared<ScriptTransport>();
  runtime::Options options;
  options.api_key = "synthetic-key";
  auto client = runtime::detail::ClientAccess::make(at(anthropic), options, executor, wire);
  runtime::RunOptions run;
  run.retry = runtime::RetryPolicy{true, false, 3, 0ms, 0ms};
  const auto run_for = [&] { auto value = run; value.deadline = executor->now() + 10s; return value; };
  const auto complete = [&](auto prepared) { auto call = client.start(std::move(prepared)); executor->drain(); return call.join(); };
  const auto directory = std::filesystem::temp_directory_path() / ("sp-files-test-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  const auto target = directory / "out.bin";
  const auto part = target.string() + ".part";
  const auto read = [](const std::filesystem::path& path) { std::ifstream in(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(in), {}); };
  const auto write = [](const std::filesystem::path& path, std::string_view text) { std::ofstream(path, std::ios::binary) << text; };
  const auto hex = [](std::string_view text) {
    std::string out;
    for (const auto byte : crypto::sha256(text)) { out += "0123456789abcdef"[byte >> 4]; out += "0123456789abcdef"[byte & 15]; }
    return out;
  };
  const auto fetch = [&] { return complete(client.prepare(files::Download{"file_1", target.string()}, run_for())); };

  // Documents of every type are the content.
  for (const auto& [type, body] : std::initializer_list<std::pair<const char*, const char*>>{
           {"text/csv", "a,b\n1,2\n"}, {"application/json", R"({"a":1})"}, {"text/markdown; charset=utf-8", "# title\n"}, {"text/plain", "hello\n"},
           {"text/html", "<p>report</p>"}, {"application/pdf", "%PDF-1.4"}, {"application/octet-stream", "{\"custom_id\":\"1\"}\n"}}) {
    std::filesystem::remove(target);
    wire->script = [&, type = type, body = body](Exchange& e) { e.reply(200, type, body); };
    const auto fetched = fetch();
    CHECK(std::holds_alternative<jobs::Content>(fetched));
    const auto& content = std::get<jobs::Content>(fetched);
    CHECK(content.path == target.string() && content.bytes == std::string_view(body).size() && content.sha256 == hex(body) && read(target) == body &&
          !std::filesystem::exists(part));
    CHECK(content.content_type == std::string(type).substr(0, std::string(type).find(';')));
  }
  CHECK(wire->exchanges.back()->request.url == "https://api.anthropic.com/v1/files/file_1/content");

  // A download that is accepted replaces the file.
  write(target, "previous content");
  wire->script = [](Exchange& e) { e.reply(200, "text/csv", "a,b\n"); };
  CHECK(std::holds_alternative<jobs::Content>(fetch()) && read(target) == "a,b\n" && !std::filesystem::exists(part));

  // One that is refused leaves it, and its partial copy is gone: an empty body, a status that is not the content,
  // a body past the limit, an error status, a connection lost for good.
  const auto untouched = [&] { CHECK(read(target) == "previous content" && !std::filesystem::exists(part)); };
  write(target, "previous content");
  wire->script = [](Exchange& e) { e.reply(200, "application/pdf", ""); };
  const auto empty = fetch();
  CHECK(std::holds_alternative<Failure>(empty) && std::get<Failure>(empty).error.reason == "jobs.download.empty");
  untouched();
  wire->script = [](Exchange& e) { e.reply(202, "application/json", R"({"status":"pending"})"); };
  const auto pending = fetch();
  CHECK(std::holds_alternative<Failure>(pending) && std::get<Failure>(pending).error.kind == ErrorKind::ProtocolCorrupt &&
        std::get<Failure>(pending).error.reason == "jobs.download.status");
  untouched();
  wire->script = [](Exchange& e) { e.reply(206, "text/csv", "a,b\n"); };
  const auto partial = fetch();
  CHECK(std::holds_alternative<Failure>(partial) && std::get<Failure>(partial).error.reason == "jobs.download.status");
  untouched();
  wire->script = [](Exchange& e) { e.reply(404, "application/json", R"({"type":"error","error":{"type":"not_found_error","message":"missing"}})"); };
  const auto missing = fetch();
  CHECK(std::holds_alternative<Failure>(missing) && std::get<Failure>(missing).error.http_status == 404);
  untouched();
  wire->script = [](Exchange& e) {
    e.head(200, {{"content-type", "text/csv"}});
    e.callbacks.on_body("a,b\n");
    e.finish(transport::Status::Failed, transport::FailureKind::Receive);
  };
  CHECK(std::holds_alternative<Failure>(fetch()));
  untouched();
  wire->script = [](Exchange& e) { e.reply(200, "text/csv", "a,b\n1,2\n"); };
  const auto bounded = complete(client.prepare(files::Download{"file_1", target.string(), 3}, run_for()));
  CHECK(std::holds_alternative<Failure>(bounded) && std::get<Failure>(bounded).error.reason == "runtime.sink.limit");
  untouched();
  std::filesystem::remove_all(directory);
}
// OpenRouter's Files API (https://openrouter.ai/docs/guides/features/files-api and the API reference pages it links):
// the same calls as the other providers, with the documented object, `cursor` paging and name rules, and no
// chunked upload.
descriptor::PolicySnapshot edited_policy(std::string_view from, std::string_view to) {
  std::string text(config_defaults::descriptor_policy_json);
  const auto found = text.find(from);
  if (found == std::string::npos) throw std::runtime_error("no " + std::string(from) + " in the embedded policy");
  text.replace(found, from.size(), to);
  auto loaded = descriptor::load_policy(text, config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(loaded));
  return std::get<descriptor::PolicySnapshot>(std::move(loaded));
}
std::string reason_of(const descriptor::ValidatedDescriptor& d, const files::Upload& upload) {
  auto result = files::encode(d, upload);
  CHECK(std::holds_alternative<Error>(result));
  return std::get<Error>(result).reason;
}
void openrouter_files() {
  const auto note = std::make_shared<const std::string>("a,b\n1,2\n");
  files::Upload upload{{note}, "data/report.csv", "text/csv"};
  const auto sent = encoded(at(router), upload);
  const auto b = boundary_of(sent);
  CHECK(sent.path == "/api/v1/files" && sent.method == "POST" && sent.auth == "bearer" && !sent.charge_on_accept &&
        sent.content_type.starts_with("multipart/form-data; boundary=sp-boundary-"));
  // The file alone, in the field `file`: no `purpose`, no expiry.
  CHECK(sent.body == "--" + b + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"data/report.csv\"\r\nContent-Type: text/csv\r\n\r\n" + *note +
                         "\r\n--" + b + "--\r\n");
  auto purposed = upload; purposed.purpose = "user_data";
  CHECK(refused(at(router), purposed) == ErrorKind::Unsupported && reason_of(at(router), purposed) == "request.files.purpose_unsupported");
  auto expiring = upload; expiring.expires_after = 3600;
  CHECK(refused(at(router), expiring) == ErrorKind::Unsupported);
  auto empty = upload; empty.source.bytes = std::make_shared<const std::string>();
  CHECK(reason_of(at(router), empty) == "request.files.source_empty");
  auto typeless = upload; typeless.mime = "text";
  CHECK(refused(at(router), typeless) == ErrorKind::InvalidRequest);

  // The file name rules: 1 to 255 bytes, `/` for folders, no empty, `.` or `..` segment, none of `< > : " | ? * \`.
  for (const std::string name : {std::string("report.pdf"), std::string("data/report.csv"), std::string("a/b/c/d.txt"), std::string("..a/b.."),
                                 std::string("a.b"), std::string(255, 'n'), std::string("sp ace.txt")}) {
    auto named = upload; named.filename = name;
    CHECK(std::holds_alternative<endpoints::Encoded>(files::encode(at(router), named)));
  }
  // The encoder's own checks (length, control characters, a quote or a backslash) refuse with `name_invalid`; the policy
  // fact's rules (folders, forbidden characters) with `name_rejected`.
  for (const std::string name : {std::string(), std::string(256, 'n'), std::string("a\"b"), std::string("a\\b"), std::string("a\rb"),
                                 std::string("a\nb"), std::string("a\tb"), std::string("a\x01" "b"), std::string("a\x7f" "b")}) {
    auto named = upload; named.filename = name;
    CHECK(refused(at(router), named) == ErrorKind::InvalidRequest && reason_of(at(router), named) == "request.files.name_invalid");
  }
  for (const std::string name : {std::string("/a"), std::string("a/"), std::string("a//b"), std::string("."), std::string(".."), std::string("./a"),
                                 std::string("a/./b"), std::string("a/../b"), std::string("../a"), std::string("a/.."), std::string("a<b"),
                                 std::string("a>b"), std::string("a:b"), std::string("a|b"), std::string("a?b"), std::string("a*b")}) {
    auto named = upload; named.filename = name;
    CHECK(refused(at(router), named) == ErrorKind::InvalidRequest && reason_of(at(router), named) == "request.files.name_rejected");
  }
  {
    // The length is the fact's: a lower `max_bytes` refuses what the encoder's own 255 would admit.
    const auto short_names = edited_policy(R"("max_bytes":255)", R"("max_bytes":10)");
    CHECK(short_names->endpoint("files", router)->file_names->max_bytes == 10u);
    auto ten = upload; ten.filename = std::string(10, 'n');
    CHECK(std::holds_alternative<endpoints::Encoded>(files::encode(at(router, short_names), ten)));
    auto eleven = upload; eleven.filename = std::string(11, 'n');
    CHECK(reason_of(at(router, short_names), eleven) == "request.files.name_rejected");
    CHECK(std::holds_alternative<endpoints::Encoded>(files::encode(at(router), eleven)));
  }
  // The characters the provider refuses are the policy fact's.
  for (const char* name : {"a<b", "a>b", "a:b", "a|b", "a?b", "a*b"}) {
    auto named = upload; named.filename = name;
    CHECK(reason_of(at(router), named) == "request.files.name_rejected");
  }
  {
    const auto custom = edited_policy(R"("forbidden":")", R"("forbidden":"#)");
    auto hashed = upload; hashed.filename = "a#b";
    CHECK(std::holds_alternative<endpoints::Encoded>(files::encode(at(router), hashed)));
    CHECK(reason_of(at(router, custom), hashed) == "request.files.name_rejected");
    const auto flat = edited_policy(R"("folders":true)", R"("folders":false)");
    CHECK(reason_of(at(router, flat), upload) == "request.files.name_rejected");
    auto plain = upload; plain.filename = "report.csv";
    CHECK(std::holds_alternative<endpoints::Encoded>(files::encode(at(router, flat), plain)));
  }

  // The size limit is the fact's: 100 MiB, and a limit the policy states instead.
  const auto slice = [&](std::uint64_t size) { auto sliced = upload; sliced.source = {nullptr, {"/data/big.bin", 0, size}}; return sliced; };
  constexpr std::uint64_t limit = 104857600;
  CHECK(descriptor::builtin_policy()->endpoint("files", router)->max_file_bytes == limit);
  CHECK(encoded(at(router), slice(limit)).file->size == limit);
  CHECK(reason_of(at(router), slice(limit + 1)) == "request.files.file_size_limit" && refused(at(router), slice(limit + 1)) == ErrorKind::InvalidRequest);
  {
    const auto small = edited_policy(R"("max_file_bytes":104857600)", R"("max_file_bytes":1000)");
    CHECK(small->endpoint("files", router)->max_file_bytes == 1000u);
    CHECK(encoded(at(router, small), slice(1000)).file->size == 1000u);
    CHECK(reason_of(at(router, small), slice(1001)) == "request.files.file_size_limit");
    auto thousand = upload; thousand.source.bytes = std::make_shared<const std::string>(1000, 'x');
    CHECK(std::holds_alternative<endpoints::Encoded>(files::encode(at(router, small), thousand)));
    auto over = upload; over.source.bytes = std::make_shared<const std::string>(1001, 'x');
    CHECK(reason_of(at(router, small), over) == "request.files.file_size_limit");
    // The other providers state no file limit and no name rules.
    CHECK(descriptor::builtin_policy()->endpoint("files", openai)->max_file_bytes == 0 && !descriptor::builtin_policy()->endpoint("files", openai)->file_names);
  }

  // Reads, a listing by cursor, a deletion and a download.
  constexpr const char* id = "or_file_011CNha8iCJcU1wXNR6q4V8w";
  const auto get = encoded(at(router), files::Get{id});
  CHECK(get.path == std::string("/api/v1/files/") + id && get.method == "GET" && get.content_type.empty() && get.model == id);
  const auto del = encoded(at(router), files::Delete{id});
  CHECK(del.method == "DELETE" && del.path == std::string("/api/v1/files/") + id && del.body.empty());
  const auto content = encoded(at(router), files::Download{id, "/tmp/out"});
  CHECK(content.path == std::string("/api/v1/files/") + id + "/content" && content.sink == "/tmp/out");
  CHECK(refused(at(router), files::Get{"../keys"}) == ErrorKind::InvalidRequest && refused(at(router), files::Delete{".."}) == ErrorKind::InvalidRequest);
  constexpr const char* cursor = "eyJjdXJzb3IiOiJvcl9maWxlXzAxMUNOaGE4aUNKY1Uxd1hOUjZxNFY4dyJ9";
  CHECK(encoded(at(router), files::List{}).path == "/api/v1/files");
  CHECK(encoded(at(router), files::List{100, cursor}).path == std::string("/api/v1/files?limit=100&cursor=") + cursor);
  CHECK(encoded(at(router), files::List{std::nullopt, "a+b/c="}).path == "/api/v1/files?cursor=a%2Bb%2Fc%3D");
  CHECK(refused(at(router), files::List{0}) == ErrorKind::InvalidRequest);
  CHECK(refused(at(router), files::List{std::nullopt, {}, "batch"}) == ErrorKind::Unsupported);

  // No chunked upload: every step is refused before it is sent.
  CHECK(refused(at(router), files::Start{"a.txt", "text/plain", 10}) == ErrorKind::Unsupported);
  const files::Session session{std::string(router), "openrouter", "upload_1", {}, 10};
  CHECK(refused(at(router), files::Part{session, {std::make_shared<const std::string>("x")}}) == ErrorKind::Unsupported);
  CHECK(refused(at(router), files::Complete{session}) == ErrorKind::Unsupported);
  CHECK(refused(at(router), files::Query{session}) == ErrorKind::Unsupported);
  CHECK(refused(at(router), files::Cancel{session}) == ErrorKind::Unsupported);
}

void openrouter_responses() {
  using Kind = files::Codec::Kind;
  // The documented file object (the guide's metadata example, and the API reference's `_shape` of the OpenRouter shape).
  const auto file = std::get<files::File>(files::typed(decoded(router, Kind::File,
      R"({"id":"or_file_011CNha8iCJcU1wXNR6q4V8w","type":"file","filename":"data/report.csv","mime_type":"text/csv","size_bytes":24576,"created_at":"2026-08-25T00:00:00Z","downloadable":false})")));
  CHECK(file.id == "or_file_011CNha8iCJcU1wXNR6q4V8w" && file.origin == router && file.name == "data/report.csv" && file.mime == "text/csv" && file.bytes == 24576u &&
        file.created_at == 1787616000u && !file.expires_at && file.downloadable == false && file.state == files::State::Active && file.shape == "openrouter" &&
        file.raw && file.raw->root().get("type").as_string() == "file" && file.purpose.empty() && file.uri.empty());
  const auto shaped = std::get<files::File>(files::typed(decoded(router, Kind::File,
      R"({"_shape":"openrouter","id":"or_file_x","type":"file","filename":"document.pdf","mime_type":"application/pdf","size_bytes":1024000,"created_at":"2025-01-01T00:00:00Z","downloadable":true})")));
  CHECK(shaped.downloadable == true && shaped.created_at == 1735689600u && shaped.bytes == 1024000u);
  // An object the service answered with in a live check: `created_at` is ISO 8601 with milliseconds, read as Unix seconds.
  const auto live = std::get<files::File>(files::typed(decoded(router, Kind::File,
      R"({"_shape":"openrouter","id":"or_file_0VbWvsZpHy4eppdP2e5wlI0y","type":"file","filename":"sp-live-227/a.txt","mime_type":"text/plain","size_bytes":23,"created_at":"2026-11-20T16:07:35.794Z","downloadable":false})")));
  CHECK(live.id == "or_file_0VbWvsZpHy4eppdP2e5wlI0y" && live.name == "sp-live-227/a.txt" && live.bytes == 23u && live.created_at == 1795190855u &&
        live.downloadable == false && live.shape == "openrouter");
  // The other shapes the service answers with are not read as this one.
  for (const char* body : {R"({"_shape":"openai","id":"or_file_x","object":"file","bytes":1,"created_at":1,"filename":"a","purpose":"user_data","status":"processed"})",
                           R"({"_shape":"anthropic","id":"or_file_x","type":"file"})"}) {
    const auto failed = decoded(router, Kind::File, body);
    CHECK(std::holds_alternative<Failure>(failed) && std::get<Failure>(failed).error.kind == ErrorKind::ProtocolCorrupt &&
          std::get<Failure>(failed).error.reason == "files.response.shape_unexpected");
  }
  // The other providers' files name the shape that held them.
  CHECK(std::get<files::File>(files::typed(decoded(openai, Kind::File, R"({"id":"file-a"})"))).shape == "openai");
  CHECK(std::get<files::File>(files::typed(decoded(anthropic, Kind::File, R"({"id":"file_a"})"))).shape == "anthropic");
  CHECK(std::get<files::File>(files::typed(decoded(google, Kind::File, R"({"name":"files/a"})"))).shape == "google");
  for (const char* body : {R"({"type":"file","filename":"a"})", R"({"id":"","type":"file"})", R"({"id":"or_file_x","size_bytes":"3"})",
                           R"({"id":"or_file_x","created_at":"yesterday"})", R"({"id":"or_file_x","downloadable":"no"})", "[]"}) {
    const auto failed = decoded(router, Kind::File, body);
    CHECK(std::holds_alternative<Failure>(failed) && std::get<Failure>(failed).error.kind == ErrorKind::ProtocolCorrupt &&
          std::get<Failure>(failed).error.reason.starts_with("files."));
  }

  // A listing pages by `cursor`; every page also carries `last_id`, which is not the cursor.
  const std::string items = R"([{"id":"or_file_a","type":"file","filename":"a.txt","mime_type":"text/plain","size_bytes":1,"created_at":"2025-01-01T00:00:00Z","downloadable":false},)"
                            R"({"id":"or_file_b","type":"file","filename":"b.txt","mime_type":"text/plain","size_bytes":2,"created_at":"2025-01-01T00:00:00Z","downloadable":true}])";
  const auto page = std::get<files::Page>(files::typed_page(decoded(router, Kind::Page,
      R"({"_shape":"openrouter","cursor":"opaque-next","data":)" + items + R"(,"first_id":"or_file_a","has_more":true,"last_id":"or_file_b"})")));
  CHECK(page.files.size() == 2 && page.files[1].id == "or_file_b" && page.files[1].downloadable == true && page.files[1].shape == "openrouter" && page.files[0].raw &&
        page.next == "opaque-next");
  const auto last = std::get<files::Page>(files::typed_page(decoded(router, Kind::Page,
      R"({"_shape":"openrouter","cursor":null,"data":)" + items + R"(,"first_id":"or_file_a","has_more":false,"last_id":"or_file_b"})")));
  CHECK(last.files.size() == 2 && last.next.empty());
  // A cursor beside `has_more: false` is not followed.
  CHECK(std::get<files::Page>(files::typed_page(decoded(router, Kind::Page,
      R"({"_shape":"openrouter","cursor":"opaque-next","data":[],"first_id":null,"has_more":false,"last_id":null})"))).next.empty());
  CHECK(std::get<files::Page>(files::typed_page(decoded(router, Kind::Page, R"({"cursor":null,"data":[],"first_id":null,"has_more":false,"last_id":null})"))).files.empty());
  // A page that says more follow and names no cursor would read as the last page, whatever `last_id` holds.
  for (const char* tail : {R"("cursor":null,"has_more":true,"last_id":"or_file_b")", R"("has_more":true,"last_id":"or_file_b")", R"("cursor":"","has_more":true,"last_id":"or_file_b")"}) {
    const auto failed = decoded(router, Kind::Page, std::string(R"({"data":[],)") + tail + "}");
    CHECK(std::holds_alternative<Failure>(failed) && std::get<Failure>(failed).error.reason == "files.page.cursor_missing");
  }
  for (const auto& [body, reason] : std::initializer_list<std::pair<const char*, const char*>>{
           {R"({"data":[],"cursor":7,"has_more":false})", "files.page.cursor_invalid"},
           {R"({"data":[],"cursor":null})", "files.page.more_required"},
           {R"({"data":[],"cursor":null,"has_more":"no"})", "files.page.more_required"},
           {R"({"data":{},"has_more":false})", "files.page.list_required"},
           {R"({"_shape":"openai","object":"list","data":[],"has_more":false,"first_id":null,"last_id":null})", "files.response.shape_unexpected"},
           {R"({"data":[{"type":"file"}],"has_more":false})", "files.file.id_required"}}) {
    const auto failed = decoded(router, Kind::Page, body);
    CHECK(std::holds_alternative<Failure>(failed) && std::get<Failure>(failed).error.kind == ErrorKind::ProtocolCorrupt && std::get<Failure>(failed).error.reason == reason);
  }

  // A deletion is confirmed by the object the service answers with.
  CHECK(std::get<files::Deleted>(files::typed_deleted(decoded(router, Kind::Deleted, R"({"_shape":"openrouter","id":"or_file_a","type":"file_deleted"})", "or_file_a"))).id == "or_file_a");
  CHECK(std::get<files::Deleted>(files::typed_deleted(decoded(router, Kind::Deleted, R"({"id":"or_file_a","type":"file_deleted"})", "or_file_a"))).id == "or_file_a");
  for (const char* body : {"{}", R"({"id":"or_file_a"})", R"({"id":"or_file_a","type":"file"})", R"({"id":"or_file_a","deleted":true,"object":"file"})"}) {
    const auto failed = decoded(router, Kind::Deleted, body, "or_file_a");
    CHECK(std::holds_alternative<Failure>(failed) && std::get<Failure>(failed).error.reason == "files.delete.unconfirmed");
  }

  // No form in which a chat or Responses request names the file is documented, so no reference is made from it.
  const auto reference = files::media(file, router, 0);
  CHECK(std::holds_alternative<Error>(reference) && std::get<Error>(reference).kind == ErrorKind::InvalidRequest &&
        std::get<Error>(reference).reason == "files.media.reference_unsupported");
  CHECK(std::get<Error>(files::media(file, openai, 0)).reason == "files.media.origin_mismatch");
}

// Through the runtime, the calls reach the origin's paths and the listing's next page comes back as `next`.
void openrouter_runtime() {
  auto executor = std::make_shared<QueueExecutor>();
  auto wire = std::make_shared<ScriptTransport>();
  runtime::Options options;
  options.api_key = "synthetic-key";
  auto client = runtime::detail::ClientAccess::make(at(router), options, executor, wire);
  runtime::RunOptions run;
  run.retry = runtime::RetryPolicy{true, false, 3, 0ms, 0ms};
  const auto run_for = [&] { auto value = run; value.deadline = executor->now() + 10s; return value; };
  const auto complete = [&](auto prepared) { auto call = client.start(std::move(prepared)); executor->drain(); return call.join(); };
  wire->script = [](Exchange& e) {
    e.reply(200, "application/json", R"({"_shape":"openrouter","cursor":"c2","data":[{"id":"or_file_a","type":"file","filename":"a.txt","mime_type":"text/plain","size_bytes":3,"created_at":"2025-01-01T00:00:00Z","downloadable":false}],"first_id":"or_file_a","has_more":true,"last_id":"or_file_a"})");
  };
  const auto listed = complete(client.prepare(files::List{1, "c1"}, run_for()));
  const auto& request = wire->exchanges.back()->request;
  CHECK(request.method == "GET" && request.url == "https://openrouter.ai/api/v1/files?limit=1&cursor=c1" && header(request, "Authorization") == "Bearer synthetic-key");
  const auto& page = std::get<files::Page>(listed);
  CHECK(page.files.size() == 1 && page.next == "c2" && page.attempt.attempts == 1);
  wire->script = [](Exchange& e) { e.reply(200, "application/json", R"({"id":"or_file_a","type":"file_deleted"})"); };
  CHECK(std::get<files::Deleted>(complete(client.prepare(files::Delete{"or_file_a"}, run_for()))).id == "or_file_a");
  CHECK(wire->exchanges.back()->request.method == "DELETE" && wire->exchanges.back()->request.url == "https://openrouter.ai/api/v1/files/or_file_a");
  // A provider refusal (an upload cannot be downloaded) is the call's error, as for any other.
  wire->script = [](Exchange& e) { e.reply(400, "application/json", R"({"error":{"code":400,"message":"Invalid request parameters"}})"); };
  const auto refused_content = complete(client.prepare(files::Download{"or_file_a"}, run_for()));
  CHECK(std::holds_alternative<Failure>(refused_content) && std::get<Failure>(refused_content).error.http_status == 400);
}
}  // namespace

int main() {
  try { uploads(); reads(); decoded_files(); references(); chunked(); upload_urls(); digits_fit(); upload_url_stays_private(); runtime_upload(); runtime_download(); openrouter_files(); openrouter_responses(); openrouter_runtime(); std::cout << "file properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "file test failed: " << error.what() << '\n'; return 1; }
}
