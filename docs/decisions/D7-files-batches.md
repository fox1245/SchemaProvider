# D7 — Files, batches and the remaining provider resources

- Status: FIRM, 2026-10-19. Extends [D6](D6-endpoints.md).
- Deciders: maintainer + review panel (eight seats from four model families, two rounds)

## Context
D6 put non-chat endpoints on `runtime::Client`. Image generation, decisions, embeddings, rerank, moderation, speech, transcription and OpenRouter video jobs shipped under it. What remains of SchemaProvider#59 is different in kind:
- **Files.** OpenAI, Anthropic and Google take uploads of hundreds of megabytes to gigabytes. OpenAI and Anthropic take one multipart POST of up to 512 and 500 MB. OpenAI Uploads (up to 8 GB in 64 MB parts) and Google's resumable upload (up to 2 GB in chunks) split a file over several calls.
- **Batches.** OpenAI, Anthropic and Google run 1-to-N jobs whose results are JSONL.
- **Stores and lookups.** Token counting, models, OpenRouter generation statistics, Google caches and file search stores, OpenAI vector stores and conversations, voices, translations.
- **Webhooks.** OpenAI sends signed webhooks. An OpenRouter job takes a `callback_url`.

The transport sent only an in-memory body, bounded by a policy limit (16 MiB by default).

## Decision
- **Upload bodies.** Large files go through each provider's chunked protocol (OpenAI Uploads parts, Google resumable chunks) as ordinary calls, each bounded. The single-shot multipart endpoints take a file slice: a path, an offset, a size and an optional SHA-256, never a read callback.
  - The transport checks the file's length before any byte leaves.
  - It hashes the slice as it sends it. With a digest, it hands over the slice's last bytes only once the digest matches, so a file changed after `prepare` never reaches the provider whole.
  - A body that must be sent again starts over from its first byte, and only when the attempt never reached the provider, as for any call.
- **Files.** One `files::File` across providers: the origin and id, Google's URI, name, MIME type, size, creation and expiry times, a state, OpenAI's purpose, Anthropic's `downloadable`, Google's SHA-256, the digest of the bytes this call uploaded, and the provider's object. `files::media` turns it into the `Media::file` reference a chat request names. It refuses a file of another origin, one not yet active, and one that has expired.
- **Multi-step operations.** Each step is an ordinary call. A journalable handle carries upload URLs, offsets, part ids and job ids between steps, as `jobs::Handle` does. The upload URL is a credential, so it stays in the `Session` and in the call's URL: a prepared call's model and the raw events name it by its SHA-256.
- **Batches.** A submit is charge-on-accept. Results come both as a JSONL download to a file and as a pure decoder of one line through the family codecs, into `{custom_id, Outcome}`.
- **Resources.**
  - Typed where the SDK gives the result meaning: token counting, models, generation statistics, translations.
  - The provider-side stores go through one call resolved from closed policy facts by an operation id: vector stores, file search stores, conversations, caches, voices. A caller never supplies a path or a header.
- **Webhooks.** A pure verifier and parser for OpenAI's signed scheme: the secret, the headers, the body and a clock in, a verified event out. An unsigned OpenRouter callback is a hint to poll, not an event.
- **Endpoint facts** may name literal headers every call sends, such as Anthropic's `anthropic-version`. A header the descriptor sets wins.

## Evidence
- [live observation] On 2026-10-19, all three providers accepted uploads, reads, listings and deletions, and served the content of files they made. Uploaded content was refused at every provider. OpenAI and Anthropic answered `/v1/files` without a beta header; Anthropic's listing pages with `page` and `next_page`.
- [live observation] Google accepted a single-request multipart upload (`X-Goog-Upload-Protocol: multipart`) and reported the SHA-256 of the content.
- [read docs] OpenAI Uploads and Google's resumable protocol split a file over calls; OpenAI and Anthropic `/v1/files` do not.

## Options considered
- In-memory bodies only, with a larger limit for uploads: rejected by 7 of 8 seats. 500 MB per admission slot is a heap hazard.
- A general byte source with a read callback: rejected. A callback cannot be journaled, and its bytes cannot be shown to be the ones the caller described.
- Types per provider for files: rejected. A host names a file in a chat request the same way at every provider.
- One generic call for every resource: rejected. D6's typed results catch shape errors in the SDK.
- Typed envelopes for every store: the minority (3 of 8). Adopted only where the SDK adds meaning.

## Consequences
- `transport::HttpRequest` gains `file_body`, and `transport::Result` gains `upload_sha256`. `FailureKind::UploadSource` reports a file that could not be read or no longer matched; the runtime reports it as `InvalidRequest` with the reason `transport.upload.source`.
- `endpoints::Encoded` gains request headers and a file slice.
- The panel recommends that models, generation statistics, caches, file search stores, vector stores, conversations, voices and translations may follow 0.4.0. The owner decided that every provider surface ships in interface 7, so this is a question for the owner, not part of the decision.

## Reconsideration conditions
- A provider whose single-shot upload exceeds what a file slice can describe, such as a streamed body without a known length.
- A host that needs uploads from a source other than a local file.

## Enforcement
- Tests:
  - a file slice is sent exactly, with its digest reported;
  - a changed file never reaches the server whole, and a missing file fails before any byte leaves;
  - every provider shape's upload, read, listing, deletion and download request;
  - `files::media` refuses another origin, a file not yet active and an expired one.
- The live e2e report carries one upload of each kind at each provider, and a chat request that reads an uploaded file.
