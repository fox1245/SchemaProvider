// Provider files (D7): the request each provider shape takes for an upload, a read, a listing, a deletion and a
// download; the one `files::File` every response becomes; its `Media::file` reference; and the runtime's file
// slice upload.
#include "codecs/files.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include "support/endpoint_fakes.h"
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
                           google = "https://generativelanguage.googleapis.com";
descriptor::ValidatedDescriptor at(std::string_view origin) {
  const std::string family = origin == google ? "google.generate" : origin == anthropic ? "anthropic.messages" : "openai.chat";
  const std::string path = origin == google ? "/v1beta/models/{model}:generateContent" : origin == anthropic ? "/v1/messages" : "/v1/chat/completions";
  const std::string stream = origin == google ? "/v1beta/models/{model}:streamGenerateContent?alt=sse" : path;
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"files-fixture","family":)" + json::quote(family) +
                                 R"(,"connection":{"base_url":)" + json::quote(origin) + R"(,"paths":{"buffered":)" + json::quote(path) +
                                 R"(,"streaming":)" + json::quote(stream) + "}}}");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
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
  CHECK(refused(at("https://openrouter.ai"), plain) == ErrorKind::Unsupported);
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
  CHECK(encoded(at(openai), files::List{20, "file-abc", "batch"}).path == "/v1/files?limit=20&after=file-abc&purpose=batch");
  CHECK(encoded(at(anthropic), files::List{5, "page_eyJh+/="}).path == "/v1/files?limit=5&page=page_eyJh%2B%2F%3D");
  CHECK(encoded(at(google), files::List{10, "tok"}).path == "/v1beta/files?pageSize=10&pageToken=tok");
  CHECK(refused(at(google), files::List{std::nullopt, {}, "batch"}) == ErrorKind::Unsupported);
  const auto download = encoded(at(google), files::Download{"files/4jn7p8olfxjg", "/tmp/clip.mp4"});
  CHECK(download.path == "/v1beta/files/4jn7p8olfxjg:download?alt=media" && download.accept == "*/*" && download.sink == "/tmp/clip.mp4");
  CHECK(encoded(at(openai), files::Download{"file-JAmK8oxEAZEPYrjL4VHYcv"}).path == "/v1/files/file-JAmK8oxEAZEPYrjL4VHYcv/content");
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
  files::SessionCodec codec(d, accumulator, step, files::session_document(session, part));
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
  session = std::get<files::Progress>(progress_of(openai, Step::Part, R"({"id":"part_1","object":"upload.part","upload_id":"upload_6ad61989ee2c8191879c23751f41c77f"})",
                                                 {}, session, mib8)).session;
  CHECK(session.sent == mib8 && session.parts == std::vector<std::string>{"part_1"});
  CHECK(refused(at(openai), files::Complete{session}) == ErrorKind::InvalidRequest);          // a part is missing
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
  upload = std::get<files::Progress>(progress_of(google, Step::Part, "", {{"x-goog-upload-status", "active"}}, upload, mib8)).session;
  CHECK(upload.sent == mib8);
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
}
}  // namespace

int main() {
  try { uploads(); reads(); decoded_files(); references(); chunked(); runtime_upload(); std::cout << "file properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "file test failed: " << error.what() << '\n'; return 1; }
}
