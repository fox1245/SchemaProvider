# Conformance plan

This is a behavioral acceptance catalogue, not a blanket pass report. The current typed SDK uses interface/shared-library generation 5 and remains pre-stable. [Usage](USAGE.md) defines current controls and ownership. The [interface 5](#interface-5-execution-record) and [interface 4](#interface-4-execution-record) execution records below are separate from [earlier interface 3 provenance](POC_PLAN.md#current-typed-c-cutover-provenance); no historical count is relabeled.

Earlier interface 3 Linux cohorts exercised optional HTTP/3 under plain, ASan+UBSan and TSan; [D1's historical evidence](decisions/D1-transport.md#evidence) records those backends and instrumentation limits. The standalone interface 4 SDK cohort below did not exercise HTTP/3, ASan/TSan or Windows/macOS/ARM64. Integrated NeoGraph diagnostics and release-matrix checks have separate scopes. WebSocket properties below remain deferred future specifications, outside current SDK/release gates and never counted as passes.

Scope (decision D2): chat completion APIs and the artifacts that arrive inside chat responses. Long-running operations, standalone image endpoints and the OpenRouter decisions endpoint have no cell, no property and no fixture here, and none may be advertised as supported. The earlier operation-lifecycle property is dropped for that reason.

Current source scope is five typed buffered/SSE families, bounded prepare/start/complete lifecycle, owned nullable/raw outcomes, exact native binding and archive v3 custody, plus interface 4 controls, Responses cursor ownership and explicit Generate portable history. General sidecar fixture schema, constraints engine, generated support tables, SDK journal projection/kit, cross-origin equivalence and general Drop/Demote remain historical proposals. Generate's explicit imported-call sentinel is a narrow current path, not those historical treatments. WebSocket has no current cell; NeoGraph owns its outcome/checkpoint serializers.

## Interface 4 execution record

The SDK0.1.0 Release build and installed consumer were exercised on Linux x86_64/WSL2 with GNU13.3.0. The initial full run passed25 of27 registered cases; the two remaining cases used obsolete cap-binding and partial native-pointer assumptions. After replacing those assumptions with archive-restored changed-cap wire/content checks and the actual Truncated/replay-ineligible boundary, both corrected cases passed in a focused run. All27 cases therefore have passing execution coverage from **25 initial passes plus2 corrected passes**, not a claimed second full27/27 run.

Real local peers exercised Responses cursor text/client-tool state, Generate foreign/native/control paths, the signed Messages2048/4096/8192/16384 cap ladder, model-specific admission, deployment headers, nullable usage and runtime ownership. An unchanged installed Stop consumer failed all four valid/invalid buffered/SSE cases before the fix and then observed ToolUse consistently in both semantic events and final outcomes in all four. The exact README CMake/C++ consumer compiled against installed interface4 and passed reported-zero, missing-count and HTTP400 Failure scenarios, with one credential-free request per variant to an owned ephemeral loopback port.

All14 C++ fragments in USAGE were syntax-compiled with `g++ -std=c++20 -fsyntax-only` against installed interface4, using the documented surrounding context. This verifies type/API spelling and contextual compilation, not runtime behavior of every fragment.

The exact documented Responses cursor fragment also ran in a standalone installed-interface4 consumer against an independent owned HTTP peer. The peer observed two requests: initial input, then `previous_response_id` with only the new follow-up input. Inherited store/parallel/verbosity/truncation/include controls matched, the response reflected the peer's held conversation state, and the cursor outcome remained ineligible for full native replay. No hosted endpoint or credential was used.

These observations establish no new hosted-vendor, signature-consumption, Windows/macOS/ARM64, HTTP/3 or sanitizer qualification. Archive format remains3. Native NeoGraph/wheel and publication were separate pending integration work at this checkpoint; no paid calls were made.

Historically, the runtime target declared a private `OpenSSL::Crypto` dependency for `client.cpp`; after that build correction, the local shared SDK was rebuilt and installed, and the exact README consumer passed reported-zero, missing-count and HTTP400 Failure scenarios. Those observations do not qualify Windows or macOS wheels. The SDK's direct OpenSSL dependency has since been removed: SHA-256, HMAC-SHA256, constant-time comparison and operating-system entropy are implemented in-tree (`src/crypto`). Production targets and package exports no longer request libcrypto; libcurl's TLS backend remains unchanged and may itself use OpenSSL. Persisted digest/archive/ledger formats are unchanged, and `crypto_runtime_links_no_libcrypto` guards the runtime-only process maps.

## Interface 5 execution record

The SDK 0.2.0 tree (interface revision 5, shared-library generation 5) was built and exercised on Linux x86_64/WSL2 (kernel 6.18, AMD Ryzen 7 5800X) with GNU 13.3.0, CMake 3.28.3, Ninja 1.11.1, libcurl 8.5.0 and Node.js 22.14.0 for the local peers. All 47 registered ctest entries passed in a Release build (`-g1 -fno-omit-frame-pointer`), in five further full Release runs, in an AddressSanitizer+UBSan build and in a ThreadSanitizer build (both Debug `-O1`; the latter started through `setarch x86_64 -R`), with no sanitizer report. The four `stall_*` entries also passed 20 consecutive repetitions. Every peer was a local process on an owned ephemeral loopback port (HTTP/1.1, cleartext HTTP/2, and TLS with an ephemeral private CA); no hosted endpoint, credential or paid call was used.

The entries cover the optional connect, first-byte and idle bounds (`stall_*`), five API families across real transport lifecycle states (`matrix_*`), and the in-tree SHA-256, HMAC-SHA256 and entropy primitives with their differential check against OpenSSL (`crypto_*`; `crypto_runtime_links_no_libcrypto` guards the runtime's process maps). They do not qualify HTTP/3, Windows, macOS, ARM64 or any hosted vendor, and the 47 ctest entries are not the 27 registered cases counted in the interface 4 record above.

A separate scratch-tree campaign (not a registered test) applied seven single-line production mutants, one for each test added after an independent review, and every mutant was caught by its test: validating a dangling UTF-8 sequence when the stream was cut instead of when it ended; losing the idle watchdog after a pause; waiting for the complete head before clearing the first-byte bound; ignoring the client-wide first-byte default; ignoring a per-run connect override; flooring a Retry-After date to whole seconds; and accepting group-writable meter directories.

### Native platform follow-up

The SDK's own suite now runs on native Windows, not only through NeoGraph integration. On Windows 11 x64, MSVC 19.44.35228, Node.js 24.18.0, libcurl 8.21.0 with HTTP/2/OpenSSL 3.6.3 and nghttp2 1.70.0, all 41 registered Release tests passed. This includes all five 141-cell lifecycle matrices (705 cells), four stall suites, runtime scheduling/ownership, real TLS/ALPN and multiplexing/backpressure/resend checks, crypto vectors and OpenSSL differential checks, and the native archive portability consumer. Peers used only local loopback ports and synthetic credentials.

The same platform-adapted source passed all 49 registered Release tests on Linux x86_64 with GCC 13.3, Node.js 24.21.0 and libcurl 8.5.0, including the additional native archive portability consumer and the real key-residue check. Transport assertions require the typed failure, correct framing, exactly one outcome and no late callbacks rather than pinning incidental backend return codes.

The Windows canary/qualification and POSIX archive custody/restart tests remain excluded because their file-custody/fork contracts are POSIX-specific; the Linux-only process-maps probes and forensic heap scan are not Windows coverage. Windows `crypto_golden` checks policy identities, not the POSIX canary digests. MSVC selects the portable SHA-256 backend; Windows randomness uses `BCryptGenRandom`.

Native execution exposed libcurl 8.21's synthesized completion timings on pre-dispatch failures. `PRETRANSFER_TIME` is no longer used as send evidence: the actual prereq callback/upload/response observations distinguish `NotSent` from `PossiblyAccepted`. The constructor-resource test initializes the exact native directory-rejection path before its ownership baseline: a direct `CreateProcessW` control, with no SDK helper pipes/job, reproduced the same one-time OS handle growth; subsequent direct and helper failures added none. Strict helper constructor/unwind resource assertions remain intact.

The `SDK macOS` workflow executes the complete SDK suite on a standard native macOS runner. It does not relabel Windows/Linux execution, Darling compatibility-layer behavior or cross-compilation as macOS runtime proof; its actual result and platform versions must accompany the tested commit.


## 1. Purpose

The suite answers one question: does the library hand its caller the same messages, tool calls, usage, stop reason and failure that the vendor's protocol actually means? It does not measure code coverage or test count.

Implemented path: typed owned request, validated move-only prepared handle, one attempt, family decoder, semantic events, one accumulator and immutable owned Completion/Failure. Buffered/SSE share this path. Durable host claim/receipt precedes dispatch of that same handle; consumer/observer/persistence failures retain original outcomes/partials.

### 1.1 Oracle philosophy

No single kind of evidence is trusted as truth. A property passes only with an oracle that does not share code with the thing tested.

| Oracle | What it is | What it catches | Limit |
|---|---|---|---|
| Reviewed golden | A hand-reviewed expected result for a recorded or synthetic exchange, written or approved from the vendor documentation and the wire bytes. | Wrong mapping of a real vendor response | Only as good as the review. Never generated by the production decoder. |
| Reference state machine | A tiny independent implementation of the event lifecycle in test code with its own tables. | Illegal orderings, double outcomes, events after terminal, wrong tool ownership across generated sequences | Must not share helpers or tables with production. |
| Metamorphic relation | A transformation of the input that provably preserves meaning for that fixture (byte re-partition, comment lines, line endings, legal interleaving). Output must be equal. | Framing bugs, state kept per read instead of per stream | Only relations proven safe for the fixture are applied. |
| Real loopback transport | The real client against a scripted local HTTP, SSE or WebSocket server that counts the requests it received, resets connections and delays reads. | Socket-path bugs, cancellation, early EOF, lifetime races, request shape on the wire | Not a vendor. It proves the client, not the vendor. |
| Same-origin live canary | A small real two-turn tool loop against the real vendor, with a tampered-signature negative control (section 9). | Wire drift, replay acceptance, signature binding, real usage arithmetic | Non-deterministic, costs money, structural assertions only, and proves nothing without its negative control. |

Differential comparison of two live models is not an oracle. Comparing the old and new implementation on the same recorded input is an investigation tool for the migration gate (section 10), not an oracle.

Rules that keep the oracles honest:

- Expected values are never produced by running the production decoder and approving the output.
- Review rule (realistic for two maintainers): the approver of a pull request is never its author. A pull request that changes both a captured wire fixture and its expected projection must be approved by someone other than the author, and the approval comment must list the changed expectations. While only one maintainer exists, the author may self-approve only after a recorded delay of at least 24 hours and a checklist of the changed expectations; the release manifest then marks those fixtures `single-reviewer`, and such fixtures do not count toward a stage gate that requires independent review (`ROADMAP.md`).
- A failing characterization of an old bug is never frozen as golden.
- Normalization must not erase differences that matter (absent versus zero, empty versus missing, which block owns a signature).
- Agreement of several independent implementations is not proof, because they can share one misreading of the spec.

### 1.2 Why server-side Open Responses compliance is not enough

The Open Responses suite tests a server. A client library decodes what servers send, so the suite cannot test our code. Further facts from the survey: it needs a live server, a real model and a key, with no deterministic mode; it has no reasoning assertions, no stream-assembly check, no usage-value check, no truncation handling, and its tool case is non-stream; its own SSE parser has reported defects (an event split across reads being dropped is an inference from code and was not run); its schema is reported as tied to one vendor's shape. Reused: its published schema may validate fixtures, and the suite may capture traffic that we then review. Passing it is never release evidence.

### 1.3 Record and replay pitfall

Common recorders match method, host, path and query and leave body and headers out, so two different requests to the same path match one recording and a payload regression passes green. Countermeasures, all mandatory in the fixture runner:

- The matcher compares method, authority and path, the semantic headers named in the fixture, and the request body (structural JSON equality after scrubbing, opaque strings exact).
- A recorded interaction never consumed fails the test. An unrecorded request fails the test. Repeat-playback is off unless a fixture declares `request_count`.
- Automatic re-recording and TTL overwrite may not change a golden. New captures go to a review artifact.

Consumer-driven contract testing (Pact) is not adopted as a vendor guarantee, because vendors do not run our contract.

## 2. Fixture format

One JSON file per scenario. Large or non-UTF-8 bodies go to a sidecar referenced by SHA-256. No YAML, no runtime recorder.

```json
{
  "fixture_version": 2,
  "case_id": "messages-tool-loop-signed-thinking",
  "origin": {"family": "anthropic.messages", "vendor": "anthropic", "authority": "direct"},
  "binding_facts": {"model": "...", "context_fingerprint": "sha256:...", "account_scope": "profile-A"},
  "descriptor_digest": "sha256:...",
  "provenance": {
    "kind": "synthetic | captured",
    "api_version": "...", "model": "...", "captured_at": "...",
    "doc_reference": "vendor documentation page title and section",
    "reviewer": "...", "review_date": "...", "review_mode": "independent | single-reviewer"
  },
  "request": {"method": "POST", "path": "/v1/messages", "semantic_headers": {"anthropic-version": "..."}, "body": {}},
  "transport": {
    "scheme": "https | http", "tls": {"test_ca": "relative/path.pem"},
    "mode": "buffered | sse | ws",
    "status": 200, "headers": {},
    "schedule": [
      {"kind": "bytes", "utf8": "event: message_start\r\n"},
      {"kind": "bytes", "sidecar": "case.001.bin", "sha256": "..."},
      {"kind": "delay_ms", "value": 25},
      {"kind": "ws_message", "text": "...", "fragmented": false},
      {"kind": "close", "how": "chunked_terminator | content_length_met | h2_end_stream | eof_unmarked | short_body | rst | ws_close_code"},
      {"kind": "reset"}
    ]
  },
  "expect": {
    "request_count": 1,
    "events": [{"type": "Begin"}],
    "outcome": "completion | failure",
    "terminal_evidence": "family terminal name | none",
    "completion": {"messages": [], "usage": {}, "stop": "tool_use", "tool_calls": []},
    "failure": {"class": "...", "retry_safety": "...", "retry_advice": "...", "attempt_evidence": "..."}
  },
  "scrub": {"version": 1, "id_replacements": {}, "removed_fields": [], "replay_eligible": false, "log": []}
}
```

Field rules:

- `provenance`: `synthetic` fixtures cite the documentation they were written from (page title and section). `captured` fixtures record API version, model and date. Reviewer, date and review mode are required. Digests are integrity checks, never proof of meaning.
- `origin` and `binding_facts` are the capsule identity the fixture asserts (`OriginBindingFacts`); the model and account scope are non-secret labels.
- `request`: the complete outbound request after scrubbing. `semantic_headers` lists only headers that change vendor behaviour. Credentials are never part of a fixture.
- `transport`: model-free HTTP peers use canonical loopback origins, which the installed descriptor/transport path admits without a public test-only configuration flag. Hosted origins require HTTPS. `tls.test_ca` in this proposed fixture schema records the private peer's trust anchor; the actual runtime option is `Options::ca_file`, not a descriptor credential/trust field.
- `transport.schedule` is separate from the semantic expectation and fixes chunk boundaries, delays, fragmentation, close shape and reset. Metamorphic variants rewrite the schedule only.
- `expect.events`: the original example used the historical ten-event sketch. Actual `sp::Event` in `<core/value.h>` also includes `MessageSeal`, `RawWire` and `ResponseEnvelope`; current fixtures/visitors must use installed declarations, not the old sketch count.
- `expect.outcome`: exactly one of completion or failure. A scrub that removes the terminal event to "tidy" a fixture is forbidden.
- `scrub.replay_eligible: false` marks fixtures whose signatures or encrypted reasoning were replaced: they verify parsing and assembly, not vendor acceptance.

Directory discovery loads all fixtures and runs them across the properties that apply.

**M2/M3 runner subset and extensions.** `tests/fixtures/{chat,messages}/*.json` uses format version 2 with an `input` object for the typed caller request, kept separate from the expected outbound `request`; optional `parity_group` names a buffered/SSE pair. Optional `descriptor_source` supplies exact source bytes for a variant, bound by `descriptor_digest`. The CLI requires an explicit descriptor path for either family. Messages `replay_from` names an earlier captured interaction: the socket path uses its actual completion, never a capsule reconstructed from golden JSON. The independent `tests/support/fixture_server.mjs` validates the digest and matches method, loopback authority, path, semantic headers and structural JSON body; unused and repeated interactions fail. Wrong-method/path/header/body controls exercise the matcher. Client and server replay chains additionally reject 12 and 10 mutations with zero dispatches.

The current C++ runner supports inline UTF-8 schedules, delays and the HTTP close/reset variants used by this corpus, over model-free loopback HTTP. It is not the general sidecar/TLS/WebSocket runner illustrated above; M1/M1b exercise TLS and HTTP/2 separately. M3 extends projections with thinking/signature/redacted leaves, full server result blocks, stop sequence/details, cache counters and extras; the versioned journal remains a later gate. Provenance records primary-source expectations and actual independent review; no production decoder generates goldens. Unsigned thinking, split signatures, reverse/interleaved blocks, iteration usage, synthetic refusal categories/server IDs and explicit zero empty-output counters are local boundary/compatibility tests, not claims of stable API emission. Synthetic capsules establish local retention and rejection, never live signature validity or provider acceptance.

**Responses private PoC subset.** Separate semantic/runtime peers validate stateless text, client functions and reasoning on buffered/SSE production paths. Each generation retains one sealed ordered native output group, not an editable JSON authority. The final streamed replay group comes from completed item.done snapshots; only top-level opaque reasoning ciphertext may differ in the terminal envelope, whose shape is still validated. All other item metadata must reconcile in both directions, and terminal plus normal close still gates success. This does not add a general Responses capture corpus or WebSocket qualification to the Chat/Messages fixture runner.

**Five-family private vision subset.** Typed inline images and native Gemini/Interactions use separate semantic/runtime peers plus a real CLI oracle (POC_PLAN5.8). The peer checks PNG bytes/hash and image-dependent numeric answers, not encoder-generated goldens or nonempty text. The corpus covers canonical image bounds/order/ownership, prefix mutation refusal, native parts/steps, generation-unique synthesized call IDs, invalid-call ineligibility, missing terminal usage and stateless omitted/empty IDs. A role-only Gemini empty terminal requires genuine finish evidence and normal close. The CLI compares semantic numeric JSON answers even when presentation is fenced or integral decimal notation; wrong or contradictory answers remain failures. This is not general all-model part-parity or release qualification.

## 3. Scrubbing rules

| Material | Rule |
|---|---|
| API keys, bearer tokens, cookies, signed URLs, query secrets, userinfo in URLs | Removed. A scanner fails the build on any key-shaped string, authorization header, `key=` query parameter or URL userinfo in fixtures, logs and test output. |
| Account, organisation, user, workspace identifiers; request ids | Replaced by stable placeholders. The same real id always maps to the same placeholder so correlation survives. Vendor error bodies embedding identifiers are scrubbed before storage. |
| Prompt text and tool values | Replaced by public synthetic values. |
| Signed or encrypted reasoning, redacted blocks, opaque vendor state | Never published as captured. A public fixture carries a synthetic capsule of the same structure and is `replay_eligible: false`. Real opaque bytes for live replay are held only for the test run and deleted. |
| Generated response ids | Consistent substitution recorded in the scrub log. |
| Timing | Replaced by scripted delays. |
| Terminal and error events | Never removed. |

Secret marker test (`OwnershipAndBounds`, section 5.12): the harness plants distinct markers (a) as the API key in the header, (b) as a key in the URL query and in URL userinfo, (c) as a value inside a synthetic reasoning capsule, (d) inside vendor error bodies, and (e) inside the text of errors raised by the transport and TLS stack (failed handshake, refused connection, redirect, proxy or DNS failure against a scripted server). It asserts that no marker appears in logs, exception and `Failure` text, `what()` strings, debug dumps or public fixture output, and that marker (c) appears only in an explicitly allowed sensitive export. The transport-error case (e) is the one that catches third-party library messages embedding the request URL.

## 4. Support matrix rule

The support matrix is a table of cells: family, vendor descriptor, transport (buffered, SSE, WebSocket), and feature (text, tool loop, reasoning replay, cache, structured output, optional knobs).

- A cell is `Supported` only if every property that applies to it passed with its fault injection observed and its mutants failing (section 5). Otherwise it is `Unsupported` (explicit request error) or `Unverified` (documented as such). There is no fourth state.
- Skipped or not-applicable tests are never counted as passes. The report prints the three totals and lists every skip with its reason.
- A property with zero applicable cells needs a reviewed not-applicable reason. WebSocket is a historical future Responses lane, excluded from current advertised cells and release gates.
- Reasoning replay acceptance is a separate evidence column with its own values (section 9.1): `ReplayVerified`, `ReplayAcceptanceUnobservable`, `Unverified`. A cell whose live canary has not run, whose negative control was not rejected, or which is a gateway cell without route pinning (section 9.1) is never `ReplayVerified`, whatever the recorded fixtures say.
- The support table in the documentation is generated from descriptors plus evidence dates. CI fails if the documentation claims a cell that has no evidence.

## 5. The 22 MUST properties

General rules for every property:

- Each property declares the fault it injects. The harness counts how often the fault fired and fails the test if the count is zero, so a property cannot pass because the trigger never ran.
- Each property has a fixed mutant set in the catalog (5.23). Every mutant must fail the property. A property with a surviving mutant is vacuous and blocks the stage.
- Every property runs over each currently advertised buffered/SSE cell to which it applies. Deferred WebSocket, equivalence and generic rule-engine scenarios do not become implied current gates.
- "Defect class prevented" cites the NeoGraph audit (issue and PR numbers of that repository) or surveyed projects (numbers belong to those projects). Items marked `[INFERENCE]` come from design analysis or from review, not from an observed defect.
- Race properties run on the deterministic executor seam (5.24), not on sampled thread timing.

### 5.1 ChunkPartitionInvariant

- Statement: for any partition of the same byte stream, including splits inside a UTF-8 sequence, between CR and LF, inside a BOM, inside a multi-line `data` value and at event delimiters, framing yields identical frames and the final completion or failure projection is identical. An event without its terminating blank line at EOF is not dispatched by the framer; what a family then does with such an EOF is decided by the terminal evidence of section 5.2 and the fixture named below.
- Exercised by: metamorphic relation over every fixture. Exhaustive single-split sweep up to a size cap, seeded random multi-way partitions above it. Expected frames for the framing corpus are written from the SSE specification, not from our framer.
- Fault injected: partitions hitting each boundary class (multibyte character, CR/LF pair, BOM, between `data:` lines, blank-line delimiter, before the final blank line). A coverage assertion checks each class was hit.
- Fixture (Gemini EOF decision): a Gemini SSE exchange whose last chunk carries `finishReason` and valid JSON but is not followed by a blank line before a normal HTTP close. Decided expectation: `Failure(Truncated)` carrying the partial content, because the framer discards the pending event and Gemini has no `[DONE]`. Whether any vendor or proxy really ends streams this way is unverified; the fixture is synthetic until a capture exists, and a capture changes the policy only through a recorded decision, never by editing the expectation.
- Prevents: events dropped when a delimiter straddles reads, loss of the last event, overwrite of multi-line data (another project's SSE parser, issue 56).

### 5.2 NoTerminalNoSuccess (includes ExactlyOneOutcome)

- Statement: no EOF, clean close, reset, in-band error event, HTTP 200 with an error body, failed Responses status or early WebSocket close produces a successful completion. Terminal evidence is what the family defines for that transport (`DESIGN.md` section 4.2). Empty output without terminal evidence is a failure. When terminal, error and cancel race, the caller gets exactly one outcome and no callback fires after it.
- Normal close, defined once for all cells: HTTP/1.1 response complete when Content-Length bytes arrived, or the chunked zero-length terminator arrived; HTTP/2 when END_STREAM arrived with no earlier RST_STREAM; WebSocket when a close frame with a normal code arrived after the terminal message. EOF on a close-delimited body, a short Content-Length body, a missing chunk terminator, RST_STREAM and an abnormal close code are abnormal close and yield `Failure(Truncated)` even if the received bytes parse as complete JSON. The scripted server expresses each through `close.how` (section 2). Whether the `Failure` keeps the partial content for the caller is a `DESIGN.md` rule; the oracle checks that it is never a `Completion`.
  - For optional HTTP/3, require complete HTTP message framing and a clean end of the response direction of the QUIC stream. A stream reset or connection failure before completion is abnormal; QUIC FIN or GOAWAY alone never substitutes for family terminal evidence. Actual capable Linux completion/reset/truncation scenarios passed plain, ASan+UBSan and TSan; [D1 evidence](decisions/D1-transport.md#evidence) preserves the exact scope and release-backend instrumentation limits.
- Oracle matrix: the terminal evidence each cell must show for `Commit` (rows restate fixture expectations and follow `DESIGN.md` section 4.2). Every cell also requires normal close; a cell's fixture set is {complete, cut before terminal, cut after terminal but before a usage trailer, clean close with no terminal, reset, in-band error, terminal plus error}.

| Family | Buffered | SSE | WebSocket |
|---|---|---|---|
| Chat Completions | normal close; parseable body; a choice with non-null finish reason; no top-level `error` | a finish-reason chunk, then the `[DONE]` marker (a usage chunk may sit between them) | n/a |
| Responses | normal close; `status` completed, or incomplete (non-success stop per design); `failed` and `cancelled` are failures | one of the three response terminal events (completed, incomplete, failed) or an error event | same events per lane; a close before the terminal event for any in-flight lane is that lane's failure |
| Messages | normal close; non-null `stop_reason` | `message_stop`, after the final `message_delta`; in-stream `error` event is a failure | n/a |
| Gemini generate | normal close; a finish reason on the candidate; a prompt block reason is a failure class | no `[DONE]`: a finish-reason chunk plus normal close. A close that is abnormal after the finish-reason chunk is `Truncated` because a usage trailer may be missing | n/a |
| Interactions | normal close; terminal `status` | the interaction-completed event is terminal even when its status is failed, cancelled, incomplete or requires-action; status maps per design | n/a |

- Exercised by: reviewed golden for each cell and each truncation shape, reference-machine runs over generated sequences, real loopback server that closes early, and the deterministic race harness (5.24).
- Fault injected: cut the stream before each possible terminal event and after it; every `close.how` variant; `200` plus error body; error event after text; WebSocket closed early; cancel and terminal raced in every order the seam can produce.
- Race rule under test: if terminal evidence was already committed when cancellation is processed, the outcome is the `Completion` and the cancel is a no-op; otherwise exactly one `Cancelled`. There is no order in which both fire (`DESIGN.md` section 7).
- Prevents: stream cut without terminal reported as `end_turn` (NeoGraph audit, issue 307); error events ignored by the parser; non-stream `failed` status or Gemini block reasons becoming quiet `end_turn`; OpenRouter upstream failures as HTTP 200 with an error body. Other projects: EOF synthesized as a stop (LiteLLM), truncated stream as 200 with no signal (Portkey, issue 1797), stream error swallowed into `stop` (LiteLLM, issue 23707). The buffered and Gemini close distinction closes review finding W09.

### 5.3 KnownCorruptNeverIgnored

- Statement: frames are Known, Unknown or Corrupt. A known tag with a wrongly typed or missing required field is Corrupt and fails the operation. An unknown event type that cannot be shown harmless is not silently skipped and never produces a success on its own. An unrecognised property that the family does not mark as optional falls in a third class, unknown-property: it is ignorable by default, with one exception: an in-band top-level `error` property is always critical and fails the operation, including when it appears on an otherwise known event. Ignorable examples that the corpus must contain: OpenAI `obfuscation`, `service_tier`, `system_fingerprint`; Gemini `modelVersion`, `responseId`, `avgLogprobs`, `groundingMetadata`; OpenRouter `provider`, `native_finish_reason`. A known-but-unmapped delta kind (annotation, citation, refusal) is never dropped silently: it yields a `ConversionLoss` diagnostic or a mapped part (`DESIGN.md` section 3).
- Exercised by: discriminator matrix per family with reviewed outcomes over five classes (known-valid, known-malformed, unknown-harmless, unknown-event, unknown-property), both directions.
- Fault injected: break one required field per known tag; add an unknown property to every event (expect pass, including each listed example); add an unknown event type (expect not silently skipped); add top-level `error` to a known event and to an unknown event (expect failure); send each dropped-delta kind (expect loss diagnostic).
- Mutants: skip every unknown event; reject every unknown property; ignore top-level `error` on known events; treat a wrongly typed known field as ignorable; drop annotation, citation or refusal deltas without a diagnostic.
- Prevents: the "undeclared event is skipped" default that hid error events (NeoGraph audit, issue 307). The opposite defect: a strict typed reader rejecting a valid response because a vendor added a field or sent two alias fields (BAML alias defect; closed-enum failures in async-openai and rig). Review finding W10 (unknown-property class) and W23 (silent delta loss).

### 5.4 InterleavedToolOwnership

- Statement: with legally interleaved tool calls, each call keeps its own id, name, arguments and signature. Partial argument text is never an executable call. A call becomes executable only when sealed, and only when it is a client-executed call (see `InvalidToolCallRepresentation` and `ServerToolNotExecuted`). An explicit empty object `{}` is distinguished from missing arguments. A freeform (custom) tool input is carried as raw text and never parsed as JSON. Text after a close or stray argument deltas after the response ended cannot corrupt a sealed call.
- Exercised by: reviewed golden per id, reference state machine over generated legal interleavings, generator with a shrinker that keeps id dependencies, and the family fixtures below.
- Fault injected: interleave A and B fragments in every legal order; fragments each invalid JSON but valid together; close with incomplete JSON; stray delta after close; empty-arguments call buffered and streamed. Family fixtures:
  - Chat Completions: the streaming key is `tool_calls[].index` (id and name only in the first fragment). A compatible server that omits `index`, or sends index 0 for every call, with distinct ids: a changed id starts a new call. An id arriving mid-call with the same index is a corruption fixture.
  - Gemini: function-call ids are sent by the vendor and must be returned unchanged in the tool result; an id is synthesized only when absent. Gemini part-boundary fixtures: a part carrying a signature is not merged with one that does not; two signature-bearing parts are never combined; a signature may sit on an empty text part; a network chunk boundary inside a part and a part boundary inside a chunk both leave the part list identical (`TransportProjectionParity`).
  - Messages: a no-argument tool call is normally `input: {}` at block start and an empty or absent `partial_json` accumulation; golden for that wire in streamed and buffered form, expected arguments `{}`.
  - Responses: custom tool input as freeform text; function call with both item id and `call_id`.
- Mutants: attach fragments to the last call; associate by position only; overwrite a vendor id with a synthesized one; merge a signature part with its neighbour; turn empty `partial_json` into missing arguments; parse freeform input as JSON.
- Prevents: fragments attached to the last call (llm-rosetta PR 628); signatures attached to the wrong block (openai-agents, issue 4154); crossed parallel streams (Pydantic AI, issue 6759); stray arguments after completion (Pydantic AI, issue 5757); streamed `""` versus buffered `"{}"` (NeoGraph audit, issue 311 item). Findings W03, W13, W14, W18.

### 5.5 SnapshotNotAppend

- Statement: a final whole-item snapshot or a repeated usage snapshot is never added on top of already accumulated deltas. A snapshot contradicting accumulated content is a failure. Cumulative sources are applied as set, delta sources as add.
- Exercised by: reviewed golden with hand-computed arithmetic, reference model, repeat-and-duplicate mutations of snapshot events.
- Fault injected: duplicate the final snapshot; snapshot then deltas and deltas then snapshot; contradicting snapshot.
- Prevents: doubled text or arguments; cache and input tokens double counted when a start and a delta event both carry usage (genai issue 272, Bifrost issues 5354 and 3451, Envoy issues 2279 and 2290); overwrite instead of combine (llm-rosetta PR 771); stream builder mixing chunks (LiteLLM issue 25869).

### 5.6 UsageKnowledgeTransitions

- Statement: explicit zero is not absent. The public usage event is a full absolute snapshot, so a later snapshot can move a field from known to unknown and from known to zero. Vendor Set, Add and Invalidate are applied privately. The provider's own total is kept beside the derived one. Input totals include cache reads and writes and output totals include reasoning, under the family's algebra. Partial, final, consistent and inconsistent are independent states. Overflow, wrong types and subset conflicts are reported and not hidden. Cache-write time-to-live splits and extra vendor counters survive in the usage extra map rather than being summed away.
- Family rules exercised:
  - Messages: `message_start` already carries an output token count; `message_delta` usage is cumulative per counter (input and cache counters can reappear and differ, for example when server tools ran), and the last `message_delta` is final. A gateway that sends an explicit zero for a counter after an earlier positive value must not erase the earlier value; the result keeps the positive value and is flagged inconsistent. Thinking tokens reported in an output-token detail feed reasoning.
  - Gemini: thought tokens are added to candidate tokens for the output total.
  - Responses and Chat: cached and reasoning detail fields are subsets of their totals; a subset larger than its total is an inconsistency.
- Exercised by: hand-calculated fixtures (Anthropic uncached plus cache-read plus cache-write equals input; Gemini thoughts added to candidates; the Messages cumulative sequence start, delta, delta with differing input counters), generator over update sequences, reference model for transitions.
- Fault injected: usage missing entirely; zero snapshot after a positive one on a cumulative family (expect flag, not erasure) and on a non-cumulative family (expect zero); conflicting subset and total; late invalidation; usage after the terminal event; overflow-size values; a repeated final `message_delta`.
- Mutants: zero overwrites earlier positive on cumulative counters; `message_delta` added to `message_start`; absent treated as zero; output total without reasoning; cache write dropped from input total.
- Prevents: Anthropic input excluding cache (3 versus 9,818 observed in NeoGraph audit, issue 308); Gemini candidates excluding thoughts (3 versus about 269 billed); stream with no total reported as zero; inconsistent meaning across routes (Portkey issue 1564); negative uncached count (Vercel AI SDK, issue 13902); contradictory cached-exceeds-prompt values (llm-rosetta). Findings W12, W20.

### 5.7 NativeRetentionForeignGate

Current eligibility/content/prefix rejection and archive custody retain genuine native authority. Cross-origin equivalence and general Drop/Demote remain historical; Generate's explicit `HistoryMode::PortableForeign` admits only unsealed portable assistant Text/client ToolCall and applies the documented first-foreign-call sentinel. It never repairs an edited or mismatched authentic seal. Responses cursor ownership is separate, terminal in-process evidence, not complete NativeReplay/archive authority. New controls are bound except generation cap and documented per-turn selection/cursor scope.

- Statement: within one origin, signed, unsigned, empty and redacted reasoning blocks are retained whole and in order, and replayed without editing. Exact bytes are required for the leaves that the vendor validates (signature, encrypted content, thinking text); the surrounding structure is the library's canonical replay projection for that family (section 5.13), not vendor bytes, because streamed blocks are assembled by the library. Blocks with the same index but different type stay separate. A capsule is bound to its origin `(family, vendor, effective authority, route scope)` and replay to a different origin is refused by default, with Drop or Demote only when explicitly requested (`DESIGN.md` section 6). Same-origin unsigned blocks are never demoted. A vendor switch in the middle of a tool loop is refused. No foreign native value appears in an outbound native wire field. Demoted reasoning is placed as a quoted block in an assistant-role position or behind an explicit untrusted delimiter, never in a user-role instruction position.
- Family retention oracles:
  - Responses with `store: false`: a reasoning item (`rs_` id) is retained with its required following item as an ordered group. Removing or reordering any member invalidates the whole group and the gate refuses the group rather than sending part of it. A function call carries both its item id and its `call_id`. The phase is per item.
  - Gemini: signature position semantics of 5.4 hold on replay.
  - Messages: reasoning blocks and any server-side fallback or stop-detail block follow the codec's replay projection including its drop rules (`DESIGN.md` section 6).
- Exercised by: byte and ordered-value oracle for retention, target-origin matrix for the gate, outbound-body capture at the scripted server, group-removal cases, plus the live canary for actual acceptance.
- Fault injected: each block kind buffered and streamed; same-index different-type blocks; replay to every other origin including the same vendor through a different host; vendor switch mid tool loop; Drop and Demote requests; remove one member of a Responses group; Demote output inspected for role and delimiter.
- Mutants: demote an unsigned same-origin block; allow replay across authority; forward a partial group; reorder blocks; put demoted text in a user-role message; merge same-index blocks of different types.
- Prevents: dropped Gemini signatures giving a 400 on the second tool turn; reasoning not captured (NeoGraph audit, issues 305 and 306); Anthropic blocks leaking to OpenAI-compatible backends (LiteLLM, issues 31279 and 32188); forged foreign fields (llm-rosetta); demoting an unsigned same-provider block causing 400 (Pydantic AI, issue 7908); empty reasoning strings deleted (Vercel AI SDK, issue 14071). Findings W19, F06.
- Limit: synthetic capsules do not prove a vendor will accept a replay. That evidence comes only from the canary (section 9), and only with its negative control.

### 5.8 StopMeaning

- Statement: a tool call alongside a natural stop is `ToolUse`. Length, content filter, pause, context-window, refusal and similar stops keep their meaning and are not collapsed. A raw vendor value is kept next to the normalized kind. An unknown vendor stop value is kept raw and never mapped to success (failure-class terminals are `FailureClassTerminal`, section 5.18). A `pause_turn` stop is surfaced as such so that the caller can continue; its replay follows the family projection.
- Exercised by: reviewed golden for mixed text plus tool turns, tool plus length, parallel tools, interrupted streams, across all assembly paths.
- Fault injected: Gemini `STOP` with a function call; OpenAI-compatible `stop` with `tool_calls`; `max_tokens` with a call (the call is carried as an invalid call, section 5.16); pause and context-window values; an unknown value.
- Mutants: natural stop wins over a tool call; unknown value maps to `EndTurn`; raw value dropped; pause mapped to end of turn.
- Prevents: tool call reported as `end_turn` (NeoGraph audit, issue 307 and PR 323; LiteLLM issue 21041; Bifrost issues 3638 and 6123); pause and context-window stops becoming unknown.

### 5.9 TransportProjectionParity

- Statement: one scripted logical response, expressed as buffered, SSE and (where the family supports it) WebSocket, yields identical messages, usage, stop, tool calls, and the identical canonical replay projection of its native carry (the same projection used by the journal, section 5.13). Parity is unconditional at that level. Raw bytes of assembled native blocks are not compared, because buffered and streamed assembly can differ in whitespace and key order while the projection is equal. For Gemini, parity is defined on the part list (parts, their order and signature positions), never on coalesced text. Callback timing is a separate contract.
  - In an advertised HTTP/3 lane, also compare the same buffered/SSE response over HTTP/2 and HTTP/3. HTTP version must not change the semantic or replay projection, and this does not imply WebSocket-over-HTTP/3 support.
- Exercised by: every recorded and synthetic reply decoded through all supported transports and compared, generalizing rig's buffered-versus-stream parity test to the final projection.
- Fault injected: the same reply with different chunking per transport; native blocks whose whitespace differs between buffered and assembled forms; a Gemini reply whose chunk boundary differs from its part boundary.
- Mutants: a transport-specific finalizer that normalizes differently; Gemini text coalesced across a signature part; projection that includes raw assembled bytes.
- Prevents: three-parser drift; WebSocket early close surfacing as a generic EOF error while HTTP was typed (NeoGraph audit); streamed versus buffered empty arguments; duplicated carry code in SSE and WS paths; error handling fixed in one transport only. Findings F11, W14.

### 5.10 RetrySafetyBudgetDeadline

- Statement: default is one attempt. With retry opted in, in exactly one layer, a retry class (transient, after reset) and a retry safety are separate and both must allow the retry. Retry safety has four values: `NotSent`, `RejectedBeforeOutput`, `PossiblyAccepted`, `OutputObserved`.
  - `NotSent`: the request body never left the client (connect, DNS, TLS failure before the first body byte).
  - `RejectedBeforeOutput` is granted only by a family-specific C++ table that maps a status plus machine-readable error code (for example a rate-limit or overloaded error with an error body of the expected shape) to "the server rejected it and produced no output". It is never granted from descriptor data or from a bare status. Because 429 and 503 arrive after the body was sent, they are `PossiblyAccepted` unless that table grants the stronger value. Quota-exhausted 429 is never retried, whatever the safety.
  - In-band errors: an error delivered as HTTP 200 or a stream error is not success. Before semantic output it is `PossiblyAccepted` unless an admitted family table grants `RejectedBeforeOutput`; after output it is `OutputObserved`. M4 grants no `RejectedBeforeOutput` case.
  - `OutputObserved` is judged by the accumulator on nonterminal semantic events after `Begin`, not by the transport or the presence of a consumer callback. Heartbeats, SSE comments, ping, and terminal `Fail`/`Commit` do not count as semantic output.
  - A request body already written followed by a timeout or reset is `PossiblyAccepted` and is not retried by default.
  - Stall bounds (`connect_timeout`, `first_byte_timeout`, `idle_timeout`; optional, off by default) end an attempt whose peer stops progressing while the deadline is far away. Default behaviour is pinned: with all three zero, a valid-event trickle, an SSE-comment trickle, a silent stall after the head and a never-responding peer all end exactly at the deadline as `DeadlineExceeded` with the partial message preserved. With a bound set: the deadline wins when it is earlier; connect and first-byte stalls are `Transport` (`NotSent` before the request left, `PossiblyAccepted` after); an idle stall is `Truncated` after the head and `Transport` before it, `OutputObserved` (never retried, partial preserved) once semantic output arrived; valid events, SSE comments and a slow upload keep an idle bound from firing; HTTP/2 activity is per transfer. Exercised by CTest `stall_transport`, `stall_tls_h2`, `stall_connect`, `stall_runtime` (all five families, request counts from the peer as oracle).
  - Transport-internal resend counts as an attempt: a reused connection that resets before a response byte may not be silently resent below the retry layer. The oracle is the scripted server's application request count, which must be at most one per logical request unless the retry layer dispatched more.
  - HTTP/3 connection-stage fallback may try QUIC and TCP candidates before request dispatch, but only one may send the HTTP request. Changing protocols after request bytes may have left is a retry, not fallback. Count received HTTP requests across all candidate peers; share one deadline and retry budget. Generation POSTs keep TLS 0-RTT early data disabled by default; an HTTP/3 preference never grants replay permission.
  - After `OutputObserved`, no reconnect or retry, and a retry result is never appended to the same accumulator. No dispatch before the server's minimum delay or after the absolute deadline; total attempts stay within the shared budget; cancellation ends a read or a backoff wait.
  - Delay hints are read from every source the family uses: `Retry-After` (seconds or HTTP date), OpenAI `retry-after-ms` and `x-ratelimit-reset-*` durations, and the Gemini error body `RetryInfo` retry delay. The Gemini body hint is `[INFERENCE]`: the vendor page consulted shows other detail types in its example, so the fixture is synthetic until a capture exists.
- Exercised by: scripted server with a virtual clock and injected RNG, attempt-evidence oracle, server-side dispatch counters.
- Fault injected: each failure shape at each point (before connect, after body write, stale reused connection reset before any response byte, mid-stream after output, quota body, rate-limit with each hint source, HTTP-200 in-band error before and after output, heartbeat then failure), exhausted budget, deadline inside a wait, cancel during backoff, fault removal then recovery check.
- Mutants: 429 treated as `RejectedBeforeOutput` from status alone; heartbeat counts as output; transport resends on reset; hint ignored or read only from the header; retry after output; retry multiplied across layers; quota retried.
- Prevents: non-429 transient errors retried with a 30-second default, and retry after chunks reached the caller (NeoGraph audit); quota exhaustion retried (LiteLLM issue 32785); captured Retry-After never reaching the scheduler (Bifrost issue 6971); multiplied retries (LiteLLM, Pydantic AI documentation). A current NeoGraph test treating a cut stream without a callback as retry-safe is not migrated. Findings W04, F10, W21.
- M4 measured subset: `runtime_policy` exercises typed classification, internal resend accounting, header minima/malformed hints, jitter and bucket boundaries. `runtime_properties` uses actual HTTP/SSE request/fault counters for default-off behavior, refused-connect `NotSent`, explicit duplicate-billing opt-in, quota, partial output with/without callbacks, ping, shared budget, attempt cap, cancellation and one deadline through retries. A complete quota JSON body followed by a short Content-Length close must remain nonretryable; this regression first observed three requests and now requires one. Gemini hints, provider idempotency grants and general descriptor error tables remain unimplemented.
- Optional-bound regression targets are `stall_transport`, `stall_tls_h2`, `stall_connect` and `stall_runtime`. They cover unchanged default deadlines, outbound request headers with an empty upload, independently arriving response header lines, upload/event/comment activity, total-deadline ties, cancellation/timeout terminal races, callback-storage release before `join`, operation/client teardown, TLS handshake and loopback TCP stalls, same-connection HTTP/2 stream isolation, five-family typed safety and partial preservation, and a total deadline that does not renew across retries. Activity is measured at libcurl's per-transfer application-byte boundary, not at a shared encrypted socket: see DESIGN section 7 for the framing/handshake visibility limitation. The retained initial runs include all four suites and activity/first-byte-clear mutation failures; final repeated-under-load and sanitizer runs remain qualification gates, not claims inferred from that initial evidence.

### 5.11 IntentOrError

- Statement: every declared control reaches the wire with its documented family meaning or rejects before I/O. Unknown/reserved/unsupported values cause zero requests. Caller caps are not clamped and explicit false/empty selections remain explicit. Messages enabled thinking has a named temperature-omission rule after prohibited-model/range rejection; test that family behavior instead of treating it as arbitrary silent rewriting. Do not infer unsupported model capability from acceptance alone.
- Exercised by: body-sensitive capture at the scripted server, a per-option probe diffing the wire body against a baseline with the declared-options list, and a server-side request counter (not a mock echo).
- Fault injected: each declared option toggled; a typo per option name; each reserved path; each unsupported option per descriptor and per model capability.
- Mutants: silently drop unknown options; bypass model-specific sampling rejection or the declared thinking omission; send before validation; replace explicit false/empty with defaults.
- Prevents: undeclared fields silently dropped (NeoGraph audit, issue 309); reasoning effort dropped by a built-in schema (PR 304); temperature forced or rejected for new models; silent drops in Portkey (issue 1469) and Bifrost (issue 5764). The probe follows a Pydantic AI test.


Interface 4 peer scenarios must count actual requests and inspect encoded controls, semantic Stop/outcome agreement and private state ownership. Cover Chat wrong-origin reasoning/usage/model alternatives; Messages mode/budget/sampling/tool/cache combinations and header precedence; Generate duplicate safety/tool settings and explicit foreign versus genuine/edited carry; Responses new-input cursor chains, mismatched original-prefix/tool ownership, absent versus explicit include and incomplete full-native/archive eligibility. Never promote compiled fields or mock echoes to wire proof.

### 5.12 OwnershipAndBounds

- Statement: accepted operations own their callback state. Every ownership situation in DESIGN section 7 must deliver exactly one outcome, never more, with no later callback. Handle destruction requests nonblocking cancellation; explicit `detach()` is required to keep running. `join()` fences callback return, closure destruction and admission-slot release. Same/runtime/raw-I/O-thread blocking calls return `Misuse`; an indirect cross-thread join cycle is unsupported and must be observed in an exec-isolated, guarded subprocess, not allowed to hang the suite. Terminal already decided wins over a later stop. Nonterminal callback exceptions produce safe `Failure(Misuse)`; an outcome-callback exception preserves that terminal result and increments diagnostics. Callbacks must not block: slow-callback detection is not preemption or a guarantee against starvation under caller misuse. Every resource has a RAII owner through normal and exceptional paths. Requests that cannot reserve/publish a bounded slot are rejected before callback acceptance; accepted preflight failures still run on the executor. Frame, queue, response, part, tool and native-content caps must fail rather than grow without bound. Secrets must be absent from public diagnostics (section 3). Immutable returned values have positive read-only and negative mutation compile controls; replay fingerprints are checked before dispatch.
- Exercised by: real loopback HTTP, SSE and WebSocket with slow readers and slow callbacks; the deterministic executor seam (5.24) enumerating interleavings; ASan, UBSan and TSan jobs as crash oracles only; boundary-generated oversize corpus; marker scan.
- Fault injected: each row of the ownership table; frames one byte over each limit; throwing and blocking callbacks; planted markers.
- Mutants: outcome delivered twice on cancel-versus-terminal; callback after join; destructor blocks; limit off by one; handle drop leaves the operation running; secret included in an exception message.
- Prevents: reader task leaked when the consumer dropped the stream (async-openai issue 576); error bodies leaking identifiers into exceptions (NeoGraph audit, OpenRouter error body). The lifetime and limit class is derived from design and is `[INFERENCE]`. Findings F08, F09, F23.
- M4 measured subset: all four Chat/Messages buffered/SSE paths, handle/client drop from callbacks, stop/cancel/deadline, detach/move, callback fences and exceptions, raw transport worker misuse, bounded admission and exceptional startup rollback. A real libcurl producer delivers an exact indexed 8 MiB response after pausing at a two-chunk/32 KiB queue limit; this is not a mock-only backpressure claim. Failed peer exec and diagnostic-capture unwinding release child/FD owners. Marker scans cover synthetic credentials, rejected userinfo/query, reasoning, vendor bodies and injected transport exception/detail text. Four `Result` mutation probes fail compilation while the read-only control compiles. WebSocket and arbitrary third-party diagnostic formats remain outside M4.

### 5.13 JournalVersionAndCanonicalOrder

Historical SDK journal-projection proposal, not an installed serializer/consumer kit or active SDK release gate. NeoGraph owns current typed provider-outcome/call/checkpoint serializers. The retained statement records the original versioning rationale; it does not claim `projection_version`, continuation or artifact fields exist in `sp::Completion`.

- Statement: the consumer journal identity is `provider-completion/v2`. A version mismatch is rejected before dispatch. The identity is computed from the canonical replay projection of the completion (`DESIGN.md` section 8): ordered messages and parts with vendor ids and phases, stop (raw and normalized), nullable usage with provenance, native carry, origin and binding facts, continuation (with its lifetime) and artifact identity. The native carry enters as a projection, not as vendor bytes: stream-assembled blocks are synthesized by the library, so the projection fixes which leaves are exact (signature, encrypted content, thinking text, vendor ids) and which structure is canonicalized (key order, whitespace). The identity changes when usage moves between unknown and zero, when native leaves or origin change, or when continuation changes. It does not change with chunk partition, transport, local ids, timestamps, request id or loss diagnostics. Local ids are remapped canonically and ordering is by semantic order key, not arrival. A resume in the capsule-less mode (Drop) has a different identity from a native resume; a v1 paused run is not resumed natively; a run in the middle of a tool loop is not migrated across identity versions.
- Library-side gate: the library repository contains pinned bytes-and-hash goldens of the canonical replay projection (including its `projection_version`) for every fixture family, written from the decision and approved by someone other than the author. Its release gate never runs consumer code. A separate consumer conformance kit is shipped with the library (projection goldens plus a driver) so that a consumer can run the same check on its own digest; the kit's results are consumer evidence and not a library gate. The separate `completion-envelope` serialization is dropped in `DESIGN.md`; the projection and a field inventory test replace it: a test lists every `Completion` field and fails when a field is neither in the projection nor explicitly excluded, and a projection change without a `projection_version` bump fails the pinned goldens.
- Exercised by: pinned goldens, transitions over each included field, perturbation of each excluded field, buffered versus streamed projection equality (5.9).
- Fault injected: change each included field singly and expect a different identity; change each excluded field and expect the same; a v1 journal presented to a v2 reader; synthesized-bytes difference between buffered and streamed assembly (expect equal identity).
- Mutants: hash raw assembled bytes; include local ids or timestamps; omit cached tokens, reasoning tokens or artifacts; order by arrival.
- Prevents: the v1 digest covers message, stop and three usage counters only and omits cached tokens, reasoning tokens and artifacts (measured in the current implementation); stored-history versioning without a plan was costly in another framework (LangChain output-version migration). Findings F04, F11, F12.

### 5.14 InstallAndDependencyDAG

- Statement: a clean consumer compiles, links and sends a loopback request using only installed headers/targets. Public signatures expose no yyjson, Asio, OpenSSL or curl implementation types. The current package exports seven component targets; network applications link `SchemaProvider::transport`, while runtime-only consumers can link `SchemaProvider::runtime` without libcurl or libcrypto. Private target direction follows DESIGN section 2.4.
- Exercised by: install-tree consumer build in CI, a standalone compile of each public header, include-direction checks from the compiler's dependency output and the CMake target graph (PRIVATE linking alone does not stop a stray include). The C++ toolchain floor (standard library support for `std::stop_token`) is checked by a configure-time test on each supported compiler.
- Fault injected: a planted forbidden include in a public header, a planted reverse include between private targets, a planted install of a private header.
- Mutants: the same three plants, each must fail the gate.
- Prevents: third-party types in the public surface (the current implementation exposes its JSON library); installed internals becoming de facto API. No vendor semantics.

### 5.15 CancelWithoutPeerProgress

- Statement: cancellation completes without any further action from the server. In each waiting state (DNS or connect, TLS handshake, response header wait, partial SSE body then stall, WebSocket partial frame then stall, backoff wait) the scripted peer sends nothing more after the state is reached, and the operation ends with exactly one `Cancelled` outcome without the peer releasing it. A one-second timeout is only an anti-hang guard, not an acceptance latency.
- Exercised by: real loopback server with a script that stalls in each state, the deterministic seam (5.24) for the waiting-state entry point.
- Fault injected: a peer that accepts then never reads or writes (per state), counted by a fired-state assertion; a peer that would answer only after the test waits (proves the cancel did not wait for it).
- Mutants: cancel implemented as a flag checked only on the next read; backoff wait not cancellable; TLS handshake not cancellable; cancel waits for peer close.
- Prevents: blocked shutdown and stuck operations (D3 rationale). The class is design-derived and `[INFERENCE]`.

### 5.16 InvalidToolCallRepresentation

- Statement: a tool call whose arguments are truncated, invalid JSON, or contain duplicate keys is never sealed as an executable call and never discarded. The `Completion` carries it as an invalid call (kind, raw fragment, reason) next to the text, usage and stop that survived, so that the caller can return an error to the model. Model-invalid JSON is classified separately from wire corruption (`KnownCorruptNeverIgnored`): the former ends as a `Completion`, the latter as a `Failure`. A `MaxTokens` stop that cuts a call mid-arguments is the required fixture; the expected outcome is a `Completion` whose stop is `MaxTokens` and whose call list holds one invalid call with the raw fragment.
- Exercised by: reviewed goldens per family for truncated arguments, invalid JSON with balanced braces, duplicate keys, and a call cut by the token limit, buffered and streamed.
- Fault injected: stop at `max_tokens` inside `partial_json`; duplicate key in arguments; invalid escape; wire corruption of the envelope (expect `Failure`).
- Mutants: seal the fragment as valid; drop the call and keep the stop; fail the whole response for model-invalid JSON; accept last duplicate key silently.
- Prevents: text, usage and stop lost on a cut call and an agent that cannot feed back an error. Findings W01, F13.

### 5.17 ServerToolNotExecuted

- Statement: a server-executed tool record (Messages server tool use and its result, Responses web search, code interpreter and MCP call items, Gemini executable code) and an approval request are never surfaced as a client-executed call that the caller would run. They keep their own kind in the `Completion`, are retained for replay per the family projection, and an approval request is distinct from a call. A `pause_turn` continuation replays the server-side content unchanged.
- Exercised by: reviewed goldens per family with a server tool, a client tool and an approval request in one response, buffered and streamed; outbound capture on the continuation.
- Fault injected: each server-tool record type alongside a client call; a response containing only server-tool records.
- Mutants: promote a server record to a client call; drop server records on replay; map an approval request to a call.
- Prevents: an agent executing a call the vendor already ran, and stop misreading when only server tools appear. Findings W02, W24.

### 5.18 FailureClassTerminal

- Statement: a vendor terminal whose meaning is failure is a `Failure` or a distinct non-success stop, never `EndTurn`. The family C++ tables cover at least: Gemini malformed function call, malformed response, missing thought signature, too many tool calls, other; OpenRouter finish reason `error`; an Anthropic refusal arriving mid-stream; a Responses failed status. An unknown finish value follows the unknown-stop rule of 5.8. Each table row has a fixture and the table is checked against the vendor finish-reason list in `RESEARCH.md` by the review step of a descriptor or codec change.
- Exercised by: reviewed golden per row in buffered and streamed form.
- Fault injected: each row as the only terminal, and after partial text.
- Mutants: map each row to `EndTurn`; map unknown to success; drop partial text on failure.
- Prevents: malformed-call finish reasons becoming quiet `end_turn` (NeoGraph audit); error finish reasons committed as success. Findings W05, F24, W17.

### 5.19 OriginBindingFacts

Actual native contexts bind admitted descriptor/model/prefix/control/scope and reject mutations. Equivalence-class activation/manifest clauses below are future proposals, not current codec branches.

- Statement: besides origin equality (the necessary gate of 5.7), each capsule records its binding facts (producing model id, a fingerprint of the replay context as sent, non-secret account scope) and the codec checks them before dispatch (`DESIGN.md` section 6). A changed model, a changed context fingerprint (system prompt, tool list or an earlier message) or a different account scope is refused before any request is sent, with a specific error, unless the family rule says that a change is tolerated; an allow-listed equivalence class (for example Messages across direct, Bedrock and Vertex) is status `Documented-Unverified` and inactive until the canary admits the exact cell (section 9.1). The OpenRouter authority is never in an equivalence class.
- Exercised by: outbound capture with zero request count on refusal; a matrix of binding-fact changes; equivalence class activation tests with and without canary evidence in the manifest.
- Fault injected: change each binding fact singly; toggle thinking mid-turn; change tool order; activate a class without evidence.
- Mutants: skip the fingerprint check; compare origin only; activate a class without manifest evidence; include the OpenRouter authority in a class.
- Prevents: vendor 400 on changed prefix (enforced by default for accounts created on or after 2026-08-31, opt-in earlier; vendor documentation), silent thinking disablement on mid-turn toggle. Findings F01, W15, W16.

### 5.20 CanaryNegativeControl

- Statement: the canary runner can never report a replay as verified without a rejected negative control and all three evidence fields (section 9.1). This is a deterministic property of the runner, tested against scripted servers.
- Exercised by: the runner against (a) a strict server that rejects a tampered signature, (b) a lenient server that accepts it (OpenRouter-like), (c) a gracefully degrading server that answers 200 after stripping the thinking block, (d) a server that never receives the control because the run was skipped.
- Fault injected: each of (a) to (d). Expected cell states: (a) `ReplayVerified` if the other evidence holds; (b), (c) and (d) `ReplayAcceptanceUnobservable`.
- Mutants: count acceptance alone as verified; ignore `negative_control: not_run`; omit the thinking-presence or usage check.
- Prevents: the vacuous canary that passes because the vendor ignores or tolerates the tampered block. Finding F15 and the owner's live observation (RESEARCH section 9).

### 5.21 AdmissionIndependentOfHeldStreams

- Statement: with T I/O threads and K held streams open, K being four times T plus 64, a short call still completes within the anti-hang guard, and the process thread count stays at most T plus a constant, independent of K. Library-caused extra threads are at most 8.
- Exercised by: loopback server holding K streams (mix of SSE and WebSocket) while a short HTTPS-loopback call runs; thread count from the operating system.
- Fault injected: the held streams are fired (counter must equal K) before the short call starts.
- Mutants: thread per request; a blocking wait inside an I/O worker; a shared queue where held streams occupy worker slots.
- Prevents: long-lived streams delaying short calls (scenario argued from design, `[INFERENCE]`). Decision D3. The capacity benchmark of section 11 is separate and non-gating.
- M4 measured subset: T = 6 configured runtime/I/O/resolver workers, K = 88 and 2K = 176 held SSE requests. Peer counters prove each held state; the short unrelated call succeeds at both loads, OS thread growth remains at most eight and does not rise from K to 2K. These are HTTP-loopback admission checks, not a throughput benchmark or the future SSE/WSS/HTTPS mixed-load gate.

### 5.22 DescriptorRuleAdmission

Historical future constraints-engine admission proposal. The installed loader validates closed descriptors/policies and rejects unknown keys; it does not enumerate/execute the ledger's proposed `omit`/`require_greater` rules. D5 remains policy for a separately approved implementation, not evidence the grammar shipped.

- Statement: the loader's set of rule kinds equals the ledger `decisions/rules.json`; every admitted rule kind has its admission evidence (decision D5): at least three independent real cases (different vendor, source document with date or reproduction log, model family; two models from one vendor document count once), each with a fixture that fails when the rule is removed; a full truth table over equivalence classes (enum values and unknown, absent, null, wrong type, explicit versus library default, numeric below, equal, above and boundaries); a permutation test showing identical output under every rule order; and contradictory rules rejected at load. Stateful, ordered, chained, dynamic-path and callback rules are refused at load. A rule kind added to the loader without a ledger entry fails CI.
- Exercised by: enumeration check, truth-table runner over the ledger, permutation and idempotence test, and the mutant check below.
- Fault injected: add a rule kind without a ledger row; remove a case's fixture expectation; permute rules; add contradicting rules.
- Mutants: a rule whose result depends on declaration order; a truth-table cell dropped; a rule kind in the loader but not in the ledger.
- Prevents: declarative-layer growth into a language (cautionary cases in `RESEARCH.md`). Decision D5; the ledger records the bootstrap waiver for the two v1 rules.

### 5.23 Mutant catalog

Mutants are reviewed patch files applied to a scratch checkout, one mutant per build (`tests/mutants/<id>.patch`). Production sources contain no mutation flag. Each property lists its mutants in its own section above; the catalog below fixes the minimum identity set the gate enforces (a count per property and a check that every patch still applies). Mutants and their fixtures are added under the rule that each post-release defect adds its regression mutant. The catalog is a floor, not a proof of sufficiency; a scheduled run of a general mutation tool may propose additional mutants and is not a gate (suitability for this code base is unevaluated).

| Property | Minimum mutants | Kill evidence |
|---|---|---|
| ChunkPartitionInvariant | state reset per read; CR/LF split mishandled; BOM split kept | partition sweep |
| NoTerminalNoSuccess | EOF becomes stop; abnormal close treated normal; both outcomes on race | terminal matrix |
| KnownCorruptNeverIgnored | 5 (section 5.3) | discriminator matrix |
| InterleavedToolOwnership | 6 (section 5.4) | interleaving reference |
| SnapshotNotAppend | snapshot appended; contradiction ignored; repeated usage added | duplicate snapshot |
| UsageKnowledgeTransitions | 5 (section 5.6) | arithmetic goldens |
| NativeRetentionForeignGate | 6 (section 5.7) | outbound capture |
| StopMeaning | 4 (section 5.8) | stop goldens |
| TransportProjectionParity | 3 (section 5.9) | triple-transport compare |
| RetrySafetyBudgetDeadline | 7 (section 5.10) | dispatch counters |
| IntentOrError | 3 (section 5.11) | wire diff probe |
| OwnershipAndBounds | 6 (section 5.12) | seam enumeration |
| JournalVersionAndCanonicalOrder | 4 (section 5.13) | pinned goldens |
| InstallAndDependencyDAG | 3 plants | gate failure |
| CancelWithoutPeerProgress | 4 (section 5.15) | per-state stall |
| AdmissionIndependentOfHeldStreams | 3 (section 5.21) | thread count |
| InvalidToolCallRepresentation | 4 (section 5.16) | cut-call golden |
| ServerToolNotExecuted | 3 (section 5.17) | mixed response golden |
| FailureClassTerminal | 3 (section 5.18) | per-row golden |
| OriginBindingFacts | 4 (section 5.19) | zero-request refusal |
| CanaryNegativeControl | 3 (section 5.20) | scripted servers |
| DescriptorRuleAdmission | 3 (section 5.22) | ledger equality |

### 5.24 Deterministic scheduler and executor seam

Requirement on the private core: runtime-visible I/O notifications, timer actions and user callbacks pass through an injectable executor and clock boundary. The manual executor holds ready handlers and runs them in an order chosen by the test. Race properties (`NoTerminalNoSuccess`, `OwnershipAndBounds`, `CancelWithoutPeerProgress`) enumerate every order of up to four simultaneously ready stimuli and resulting actor/timer tasks in a bounded scenario, and sample larger scenarios with seeded schedules; actual fault firings and outcomes are counted. Sanitizers complement this oracle because they see only interleavings executed in that run. The boundary is private and non-installed, as required by DESIGN section 7. Clock/RNG injection without controlled task scheduling is insufficient.

M4 implementation: `src/runtime/testing.h` is a private, non-installed executor/clock/attempt seam. The bounded race model starts four simultaneously ready stimuli (cancel, stop token, deadline advance, response) and explores every ready-task choice, including resulting actor turns and promoted timers: **298 schedules**, with **6 completion / 260 cancellation / 32 deadline** outcomes. It observes **216 wire completions, 82 wire cancellations and 256 timer callbacks**, rather than counting attempted no-op injections as fired faults. The independent decision model samples control state at actor entry and checks exactly one terminal and no later events. Separately, 64 seeded schedules of eight concurrent operations with three retry tokens produce 704 total dispatches and 512 outcomes, with three recoveries per seed. These finite cases do not enumerate all OS/libcurl interleavings or every ownership scenario; the actual transport and sanitizer suites complement them.


## 6. Reference state machine

A small test-only implementation, separate from production tables and helpers.

- Global states: `Receiving`, `TerminalObserved`, `Failed`, `Cancelled`. Terminal observation is not the end: the family may allow a usage trailer. Framing end, semantic completion and delivery end are separate facts.
- Per tool call: `NotSeen`, `Open`, `Closed`. Only `Closed` client-executed calls can reach dispatch.
- Checks generated sequences including legal interleavings and near-legal ones that break exactly one rule (missing start, duplicate id, wrong block index, delta after close, early terminal, EOF, error after text).
- A timeout alone never proves that the server is quiet forever. Local error contracts use a fake clock with a scripted EOF.

## 7. Generators and fuzzing

Three layers: byte layer (incremental UTF-8 and SSE framing), family frame layer (known, unknown, corrupt), event sequence layer (legal and one-rule-broken sequences into the accumulator). Sanitizers and time and size limits are crash oracles; the properties are the semantic oracles. Not crashing is never counted as correct. The corpus unit is a sequence with assembler-state coverage collected. Failing seeds only are added. Shrinkers keep tool ids and sequence dependencies.

## 8. Metamorphic relations allowed

Allowed, because meaning is provably preserved for the fixture: re-partitioning bytes, inserting SSE comment lines, changing line endings among CR, LF and CRLF, adding a leading BOM at stream start, and interleaving calls while preserving each call's internal order. Not allowed: splitting inside a JSON string by newline, reordering reasoning blocks, reformatting an opaque signed value, canonicalizing opaque bytes.

## 9. Live canary

Purpose: catch what recorded fixtures cannot, real vendor drift and real replay acceptance. A canary proves nothing about replay unless its negative control ran (section 9.1).

### 9.1 Design

- One cell per family, vendor host, supported transport and model.
- Scenario: a same-origin two-turn tool loop. Turn 1 asks for a tool call with reasoning enabled where the family returns signed or encrypted reasoning; the test captures what was returned and replays it unchanged in turn 2 with a tool result. Variants where available: unsigned, redacted and empty reasoning blocks.
- Negative control: a second run of turn 2 replaces one byte of the signature (or encrypted content) in a copy of the capsule. A vendor that validates should reject it. The run records `negative_control: rejected | accepted | not_run`.
- Three evidence fields per run: `client_retention_verified` (the outbound turn-2 body contains exactly the captured leaves in order), `request_accepted` (turn 2 returned success), and `native_validation_evidenced` (the tampered control was rejected, or independent evidence shows the vendor consumed the thinking, see the observation below).
- Graceful-degradation observation: acceptance alone is vacuous because a vendor may answer normally after ignoring or stripping a block (the owner observed a success after stripping thinking in a same-turn loop, single sample). The runner therefore also records thinking presence in turn 2 when thinking was enabled, reasoning usage, and cache read and input counts against the expected growth. The usage-growth comparison is an `[INFERENCE]` heuristic with unknown sensitivity and is calibrated before it can gate anything; thinking presence and the negative control are the evidence that can.
- Cell states: `ReplayVerified` requires negative control rejected and all three fields true. A cell whose negative control is `accepted` or `not_run` is `ReplayAcceptanceUnobservable`: its acceptance is recorded but never called verified (an OpenRouter run accepted a tampered signature in the owner's test, so OpenRouter acceptances are not evidence). Gateway cells (OpenRouter and similar multi-backend routers) without route pinning to one backend stay `Unverified`; with pinning, the pinned backend is recorded in the cell.
- Result form: each run is repeated N times per (cell, model, direction). The record holds successes out of N and a one-sided lower confidence bound on the acceptance rate (exact binomial); for example 60 of 60 gives a lower bound of about 95.1 percent at 95 percent confidence. Equivalence-class admission (decision D4) needs vendor documentation, 60 runs with no failure per (host, model, direction) and 100 percent negative-control rejection. A cell with any failure is reported as a rate, never as a boolean. Bedrock and Vertex direct access is untested (no credentials) and stays `Documented-Unverified`.
- Manifest entries per run: model id, account age class (created before or on or after 2026-08-31, because vendor prefix enforcement differs), negative control result, N, successes, lower bound, date, pinned route. A pass on one account does not establish behaviour for another; the manifest says so.
- Structural assertions also include: terminal present; stop `ToolUse` for turn 1; call ids correlate; usage arithmetic holds under the family algebra; raw vendor usage recorded.
- Foreign-origin refusal, Drop and Demote are asserted client-side on the outbound body, no live call.
- A restricted store holds real opaque bytes for one run. The public report contains only the result contract.

### 9.2 Cadence, budget and secrets

- Daily per family: short text, stream and tool-return canary on a representative cell. Weekly rotation: reasoning replay with negative control and N-run rates, cache behaviour, Responses WebSocket, Interactions continuation, optional knobs.
- Call and token caps are explicit. A run without credentials is `not run`, never green. Vendor outages do not fail deterministic CI, but a cell whose canary failed is not released as verified.
- Canary keys belong only in scheduled/protected execution, never fork pull requests. Read and comply with applicable API terms before enabling a cell. M5 read the current first-party API agreements; this is not an account-specific legal approval or provider-side spending-control verification. The preservation/reservation boundary below is not an invoice hard cap.
- Each cell's last success date and descriptor verification date go into the release manifest.
- M5's opt-in `sp_canary` CLI has a strict versioned profile and a preserved, nonsecret reservation ledger. No `--execute` means a fixed plan with no file/credential/network I/O. Every attempted backend start is durably charged before dispatch; failures, cancellation and negative controls are never refunded. Call, token and checked integer micro-USD caps are shared per provider within that preserved ledger. A reopened empty or malformed/torn ledger fails closed. This is not tamper-proof accounting across arbitrary replacement files or a new ledger path.
- Live profiles are restricted to reviewed first-party origins and exact snapshots. Reservation uses the full model-context input bound and the configured output cap at maximum applicable rates, not a character/token guess; OpenAI requests explicitly select standard `service_tier: "default"`. Premium modes, paid server tools, caches and automatic retries are excluded from this campaign. Reserved exposure is not an invoice: provider-side spending controls, applicable pricing and taxes remain outside the client's enforcement boundary.
- Credentials are read only from the selected environment variable or explicit file input; no shell expansion or key in command-line arguments. The file path and values never enter the public report. A POSIX owner/mode gate does not establish Windows/DrvFS ACL confidentiality. Native leaves and response prose stay in process memory; report fields are fixed structural states and nullable usage counters. A signature-specific negative control changes one byte only after ordinary SDK replay admission, and an unrelated/unknown rejection remains inconclusive rather than `ReplayVerified`.
- Post-M5 Gemini text smoke uses the existing `openai.chat` codec on Google's exact compatibility endpoint, not native Gemini API semantics. ID2 accounting preserves prior ID0/1 counters and has a separate4-attempt/1,000,000-microUSD/output128 ceiling. Its report exposes the family/scope explicitly and cannot label native replay verified. The peer's streamed usage depends on the actual `include_usage` option; legacy call/token/cost totals are asserted through restart/exhaustion. Actual N=1 buffered/SSE results and exclusions are in POC_PLAN5.6, not a new Supported cell or an all-family gate.
- Responses uses separate ID3 initially6/US$1, then explicitly raised to8 cumulative with no reset/refund; combined OpenAI IDs0+3 also obey16/US$10. The full input-window/output-including-reasoning bound reserves23,277 microUSD per attempt; final8/US$0.186216 and unchanged earlier provider balances are in POC_PLAN5.7. A buffer-preserved positive plus an explicit `invalid_encrypted_content` one-byte rejection established N=1 replay; an omitted-reasoning control was accepted. The original live5/6 and two diagnostic/confirmation calls are separate evidence, not a new6/6 campaign. Synthetic controls never grant vendor ReplayVerified.
- The five vision lanes append IDs4..8 under one additional **40-attempt/US$8** ceiling, without resetting IDs0..3 or borrowing an old allowance. Final40/US$7.795635 reservations and failure/confirmation history are in POC_PLAN5.8. Actual GPT-6 Luna Responses SSE native replay with an explicit ciphertext-negative is N=1 ReplayVerified; new vision positives for other providers do not get that label. Interactions live tool replay and their new signature negatives remain unverified at the exhausted cap. Both invoice limits and audit coverage limitations stay explicit.
- After an explicit new60/US$12 grant, cumulative vision guard is100 calls/19,795,635 microUSD: previous **actual spent**, not previous unused cap, plus the new allowance. Final new59/US$11.184045 and preserved cumulative99/US$18.979680 are in5.9. Consumer tests cover preserved40-call history, only60 additional admissions, exact12M extra-cost contention/restart and rejection of renewed US$20.
- Duplicate signature values are distinct associated carriers, never deduplicated or mistaken for absent state. A control changes one byte of the pending call's carrier while preserving all other fields; all-state omission also removes remaining associated signatures. Independent wire inspection checks exact change/owner and unchanged image/prefix/call identity. A still-valid duplicate prevents inferring universal validation behavior from accepted mutation.
- Bounded JSON or a complete single Interactions SSE ErrorEvent may carry reviewed native rejection evidence; incomplete/multiple/wrong-schema/status/generic errors never do. A signature diagnostic cannot establish omission proof. Real5.9 runs establish N=1 Messages/Responses rejection pairs and Google unobservable accepted controls, not new Supported release cells; fresh low/medium Responses cohorts and Interactions7/8 remain separate explicit outcomes.

### 9.3 Drift detection by behavioural fingerprint

A fingerprint is a structural summary, not sampled prose: event type tags seen, field types per tag, stop and usage relations, replay rejection, capability rejections, keyed by vendor, family, API version, model and date.

- A new tag, a changed known-field type, a changed stop or usage relation, or a replay rejection opens a review artifact.
- Recorded canary traffic is kept apart from goldens, used only to review drift, never the contract, and promoted to a fixture only through review and scrub.
- Changes in generated wording are recorded but are not a codec regression. A silent vendor rule change (for example the 2026-08-31 default enforcement) is caught only as a replay rejection on a cell that ran after it; the account-age field in the manifest is the control for it.
- Statistical monitoring of model behaviour is out of the library. The library never sends hidden billable probes, and opaque reasoning is never probed.

## 10. Historical comparison against pre-cutover NeoGraph

The M5 experiment ran the fixture corpus against unchanged old NeoGraph sources through an isolated adapter. That adapter/driver/build flag has been removed. The comparison and requirements below preserve that cohort's methodology and NO-GO, not a current command or requirement to keep two implementations. Current installed consumers exercise the typed boundary directly.

- Outcomes are `pass`, `expected-fail`, `unsupported` or `not-applicable`. An expected failure is a tracked entry naming the audited defect class and flips to pass when fixed. An expected-fail that unexpectedly passes, or a previously passing cell that fails, fails the gate.
- Properties depending on the new design (origin-bound capsules, binding facts, invalid-call representation, server-tool kinds, journal v2, installed headers, the seam-dependent race properties) are `unsupported` or `not-applicable` for the old implementation and are never reported as pass.
- Non-chat features (operations, standalone images, decisions endpoint) are excluded from the gate by D2; their removal or rewrite belongs to NeoGraph's cutover and is not measured here.
- The old-versus-new comparison is a defect-finding aid. The expected value is the reviewed golden. Differences need an accepted-difference record with a named approver other than the author.
- Existing tests are reused only when their expectation matches consumer-visible meaning. Expectations that encode old defects (a stream cut without a terminal event treated as success; a cut stream without a callback treated as retry-safe) are not migrated.
- The old journal identity `provider-completion/v1` keeps its verification. New dispatches pass the v2 gate; mismatches start a new run or fork.
- Output: per property and family the counts of pass, expected-fail and unsupported, plus the accepted differences.
- Historical M5 compiled unchanged pre-cutover NeoGraph sources in an isolated experiment and retained raw body/native/tool/block differences rather than normalizing away loss. That obsolete adapter/legacy driver/build path is removed. Its fixed corpus and past NO-GO observations remain historical, not a current CTest alias or ABI support claim. Current direct consumers must qualify the typed SDK/NeoGraph boundary before dispatch.
- The actual request handler is awaited through its full fault schedule before reporting. A client that completes and closes before a delayed reset is recorded as `early-terminal-before-close` / `client_closed_before_fault`, not as proof that a reset was delivered and ignored. Comparable semantics and unsupported ABI dimensions remain separate. Named independent defect acceptance is recorded in the M5 result, never inferred from every author-labelled expected-failure row.

## 11. CI gates

Pull request (deterministic, no credentials):

1. Current descriptor/policy strict load: unknown/duplicate keys, invalid origins and reserved bindings reject. The full target-schema generation and proposed rule-ledger equality remain future gates only if that grammar is separately implemented.
2. Fixture replay: fixtures of changed families first, then the small corpus, with body-sensitive matching and unused-interaction checks.
3. Metamorphic and property suites with fixed seeds and fault-fired assertions. Mutant runs for the properties whose sources changed; the catalog check (every patch applies) always.
4. Loopback smoke per advertised transport: fragmentation, reset, slow reader, cancel per waiting state, held streams.
5. Sanitizer jobs (ASan, UBSan, TSan) over the ownership properties on the deterministic seam.
6. Public header standalone compile, include-direction and target-graph checks, install-tree consumer build.
7. Secret scan of fixtures, logs and test output, and the marker test including transport-stack errors.
8. Review rules of section 1.1; generated support table checked against evidence.
9. Deterministic D3 gates: library-caused extra threads at most 8 and independent of concurrency, exactly one outcome, no late callbacks, sanitizer runs of `OwnershipAndBounds`.

Scheduled:
- Live canary cadence is a future operations proposal, never hidden probing or permission to renew the completed fixed campaign. New hosted calls require separate explicit owner authority.
- Full mutant catalog, byte-framer and event-sequence fuzz (failing seeds added).
- The historical migration comparison ran while the old implementation existed; the approved cutover removes that path rather than retaining a compatibility test driver.
- The historical D3 C/2C mixed SSE/WebSocket benchmark and proposed later 25% regression gate are future work, not current capacity or supported WS claims. The completed HTTP/SSE SDK matrix measures a different workload; it must not be substituted for that historical proposal.

Semantic release notes are required for any change to stop, usage, retry or replay meaning, even if labelled a bug fix, with fixture and journal-identity impact reviewed.

### 11.1 Lifecycle matrix

`matrix_chat`, `matrix_responses`, `matrix_messages`, `matrix_gemini` and `matrix_interactions` run the same real-transport cell table in `tests/matrix_lifecycle_test.cpp`. The local scripted peer (`tests/support/lifecycle_server.mjs`) verifies each family's endpoint, authentication, request mode, request/fault counts and actual HTTP/TLS protocol. No provider credentials or hosted calls are used. TLS trust is supplied through `ca_file`; the OpenSSL command-line program generates an ephemeral self-signed loopback certificate and is a test prerequisite, not a library crypto linkage.

Inventory per family (141 cells; 705 across the five families):

| Wire / lifecycle group | Cells per family | Pinned cases |
|---|---:|---|
| HTTP/1.1 normal | 2 | Buffered and SSE completion, semantic text and EndTurn |
| HTTP/1.1 HTTP failures | 38 | 400/401/403/404/429/500/503/529, each with family-shaped and empty bodies in buffered/SSE modes; 429/500 array envelopes and 503 HTML |
| HTTP/1.1 Retry-After metadata | 10 | 429 seconds/date, 503 seconds, 529 date and malformed hints, each buffered/SSE |
| HTTP/1.1 retry scheduling | 3 | Honour seconds and HTTP-date minima; reject a retry whose delay exceeds the immutable total deadline |
| HTTP/1.1 explicit retry policy | 5 | Duplicate-billing risk not authorized, family-specific 429 policy, attempt cap, original deadline carried into a subsequent attempt, no retry after semantic output |
| HTTP/1.1 total deadline | 9 | Before head, after head, mid-body in both modes; valid SSE trickle; incomplete SSE frame after valid output; raw TCP peer that never responds |
| HTTP/1.1 cancellation | 11 | Before head, after head and mid-body via handle/stop token for SSE and handle for buffered; both mechanisms during retry backoff |
| HTTP/1.1 peer close/reset | 6 | Reset before head, reset mid-body and close after head, each buffered/SSE |
| HTTP/1.1 connect refusal | 2 | Default single attempt and explicitly enabled three-attempt retry |
| TLS certificate rejection | 1 | Untrusted self-signed peer fails before sending a provider request |
| Each of TLS HTTP/1.1, h2c and TLS ALPN HTTP/2 | 14 each | Both normal modes; 503 seconds, 429 date and bare 401; deadline before head/mid-body/trickle; cancellation in three states; reset before head/mid-body; close after head |
| Reuse on all four wires | 12 | Two sequential requests on one connection; idle connection reset between requests; reset while serving the second request on the reused connection |

Accepted operations are fenced with `join()` and must produce exactly one outcome callback, the same owned result from callback/join, no terminal semantic event and no semantic callback after the outcome. Failure cells assert kind, retry class/safety, status, vendor code, Retry-After presence/range, attempt/send/head evidence and transport resend accounting; reset/stall cells also retain the delivered semantic prefix. Peer counters independently prove expected request counts (one unless the cell explicitly permits retries), injected faults, correct wire protocol and valid request semantics. Waits and overall test execution are bounded; a filtered invocation matching no cells fails rather than reporting a vacuous pass. `--list` exposes the complete named inventory and `--only <substring>` selects targeted cells.

Policy differences are intentional, not normalized away: `config/error-policy.json` leaves Google 429 as `LimitUnknown/Unknown`, so enabling retry still does not resend it; Chat/Responses/Messages classify their recognized rate-limit bodies as `RateLimited/AfterReset`. Messages' `api_error` 500 is `RemoteFailure/Transient`; the other families' supplied 500 envelopes use `Overloaded/Transient`. Shared scheduled-retry cells use 503 so they exercise a real retry in every family. HTTP/1.1 short-body FIN is abnormal (`Truncated/Transient`), whereas HTTP/2 END_STREAM after HEADERS is normally framed but lacks a semantic terminal (`Truncated/Never`).

Connection-reset accounting distinguishes a **refused resend proposal** from an actual duplicate dispatch. `OperationState::prereq_cb` in `src/transport/http_transport.cpp` aborts libcurl's second write proposal before it writes; `TransportCore::finish` nevertheless reports `prereq_count - 1` as `transport_internal_resends`, and runtime `OperationState::done` includes that count in `attempts`. The HTTP/1.1 reused-connection reset therefore pins counter 1/attempts 2 together with exactly two total peer requests, not a false counter-0 claim. Successful reuse and ordinary cells pin counter 0. An idle-reset race may instead select a fresh connection before writing (success, counter 0, two peer requests) or hit the refusal guard (failure, counter 1, only the first request reached the peer); no branch permits a duplicate request.

For HTTP/2 cancellation/deadline cells, the terminal callback and `join()` must finish promptly **while the Client is still alive**. Only after proving this does the cell destroy the Client to fence eventual peer stream cleanup. The current libcurl transport can leave that stream open until connection destruction rather than sending RST_STREAM immediately; peer cleanup is not substituted for operation cancellation proof.

Backoff-cancellation cells fence the runtime actor's actual retry-wait transition through the private test seam before cancelling; a peer's completed-response counter is not treated as that state fence. Idle HTTP/2 connection-reset cells retain the underlying accepted TCP socket and inject RST directly, rather than substituting orderly GOAWAY teardown.

After-head and buffered mid-body cancellation cells fence on the peer having flushed the head (or the body prefix), not on the client having consumed it, so the client may still be before head delivery when the cancellation lands. They assert the outcome and its retry safety and do not assert `response_head_seen`; a streaming mid-body cell fences on the first delivered semantic delta instead.

This inventory describes registered regression checks, not proof that a verification run succeeded. Qualification requires the named cells to execute, the applicable mutation checks to fail, and the full-suite/sanitizer gates to be recorded separately.

## 12. Staged ladder and release gates

Release criteria are stage gates, defined with their dates, cells and property subsets in `ROADMAP.md`; this file does not repeat the stage content. The rules that hold at every stage:

1. A stage ships only for the cells it advertises. Every property that applies to an advertised cell passes with fault-fired assertions green and mutants failing. No skipped cell counts as supported, and a property needed by a later stage only is listed as not yet in force, never as passed.
2. The loopback smoke passes for every advertised transport.
3. Replay claims: a cell has `ReplayVerified` only with the evidence of section 9.1 in a manifest whose age limit is fixed in `ROADMAP.md` before that stage; otherwise it is listed `Unverified` or `ReplayAcceptanceUnobservable` and excluded from support claims.
4. The original library-side journal projection golden/consumer kit is a historical proposal, not an installed feature or current SDK release gate. Actual adapter serializers and native archive/custody must preserve their advertised host contracts and version boundaries.
5. The migration gate report has no unexplained difference, no unexpected pass of an expected-fail, and every accepted difference has an approver who is not its author.
6. The install-tree consumer builds and no forbidden include exists in installed headers.
7. The support table, release manifest (descriptor verification dates, canary rates, account age classes) and semantic release notes are generated and reviewed under the rules of section 1.1.
8. Known gaps are in the release notes; nothing untested is described as supported.

### 12.1 Optional HTTP/3 gate

Implemented optional transport; capable Linux scenarios have run under plain, ASan+UBSan and TSan, with actual HTTP/3 preference/only negotiation, one-POST HTTP/2 fallback, cancel/deadline, reset/truncation and same-connection paused-stream isolation. The non-QUIC system build rejects HTTP/3-only before dispatch. [D1 evidence](decisions/D1-transport.md#evidence) records exact linked versions and uninstrumented release-backend limits. The table remains the acceptance checklist for any advertised release/build matrix, not a claim that every peer/fault combination or five-family semantic parity cell has been qualified. HTTP/3 is not required for the HTTP/2/HTTP/1.1 baseline; existing property names are reused rather than introducing a second semantic suite.

| Gate | Required evidence |
|---|---|
| Capability and install isolation (`InstallAndDependencyDAG`, `IntentOrError`) | Test builds with and without HTTP/3 in the linked libcurl, not just a `curl` executable. The non-capable build uses its normal HTTP/2/1.1 path. QUIC backend headers/types remain private. Missing capability is explicit in an HTTP/3-only verification run. |
| Actual protocol, not successful fallback | A real QUIC peer and an isolated HTTP/3-only client connection prove HTTP/3 negotiation from both ends. A response served by HTTP/2/1.1 is fallback evidence only, never an HTTP/3 pass. Record the linked libcurl/TLS/QUIC versions and negotiated protocol with the result. |
| Safe fallback and replay (`RetrySafetyBudgetDeadline`) | Unsupported peer, UDP refusal/blackhole, QUIC handshake failure, raced connection candidates and failures after possible send. At most one HTTP request reaches the peers per permitted application attempt; no timeout/budget reset, silent resend or retry after semantic output. Verify generation POST early data is disabled. |
| Completion and parity (`NoTerminalNoSuccess`, `TransportProjectionParity`) | Normal HTTP/3 stream end, truncated body, stream reset and connection failure; one outcome, with no EOF-created success. HTTP/2 and HTTP/3 must yield the same buffered/SSE semantic and replay projections for each advertised family cell; transport delivery proof alone is not full semantic parity qualification. |
| Liveness and bounds (`CancelWithoutPeerProgress`, `OwnershipAndBounds`, `AdmissionIndependentOfHeldStreams`) | Cancel/deadline while QUIC handshake, fallback race or response delivery makes no progress; pause/resume one stream beside active siblings, cancel it without aborting siblings, and verify byte completeness and bounded memory. QUIC timers must still run; do not suppress readability for the entire shared connection as for HTTP/1.x. Run the applicable scenarios under plain, ASan+UBSan and TSan. |

TLS verification and origin/credential boundaries apply in both the HTTP/3 and fallback lanes. Capability or loopback success does not establish a provider's HTTP/3 support; provider claims need the relevant budgeted live evidence. Performance comparisons are measurements, not correctness gates or promised speedups.

## 13. Open decisions that touch conformance

Decided (see `decisions/`): D1 transport (libcurl `multi_socket` on a private Asio loop, FIRM, revised 2026-10-01; the transport-level properties 5.2, 5.12, 5.15 and 5.21 already run in the M1 spike, see `POC_PLAN.md` section 5), D2 scope (chat only), D3 async core (gates 5.15 and 5.21), D4 exact origin plus recorded binding facts and a documented equivalence class, D5 rule admission. Still open:

- Any future WebSocket lane requires separate design and protocol/cancel proof; it does not block current Responses HTTP/SSE.
- Any cross-origin equivalence activation requires separate implementation/admission and evidence. Direct Bedrock/Vertex access is not current SDK scope.
- NeoGraph's target concurrency for the historical scheduled D3 benchmark remains separate from the completed model-free SDK matrix.
- Non-chat SDK scope remains excluded; NeoGraph's retained Images/Veo/Decisions are separate typed clients. [Recorded sanitized media observations](../config/media-minimal-validation.json) do not establish broad endpoint qualification or authorize more requests.

## 14. Limits of this plan

- Fixtures prove assembly and mapping against reviewed expectations, not that a vendor behaves as recorded today.
- The canary proves acceptance for the tested cell, model and account on the tested days, and only with a rejected negative control.
- Synthetic signatures prove structure and retention, never vendor acceptance.
- Mutant sets are a floor chosen by the authors; they do not prove the properties are strong enough.
- The deterministic seam enumerates interleavings among handlers it controls; it does not model kernel or vendor timing.
- The property catalogue remains the release acceptance plan, not a blanket pass report. Measurements explicitly identified above or in POC_PLAN/D1 are observations with their recorded cohorts and limits; unexercised properties, proposed thresholds and future cells remain gates, never retroactive successes.

## Appendix A. Review disposition

Review findings from two independent reviews (wire-level W, architecture F) that touch conformance, and where they are addressed. Design-only findings are omitted.

| Finding | Addressed in |
|---|---|
| W01, F13 truncated or invalid tool arguments | `InvalidToolCallRepresentation` (5.16), 5.8 |
| W02, W24 server tools, fallback blocks | `ServerToolNotExecuted` (5.17), 5.7 |
| W03 freeform tool input | `InterleavedToolOwnership` (5.4) |
| W04, F10, W21 retry safety, in-band errors, hints | `RetrySafetyBudgetDeadline` (5.10) |
| W05, F24, W17 failure-class terminals | `FailureClassTerminal` (5.18), 5.8 |
| W09, F15 terminal evidence, buffered close | `NoTerminalNoSuccess` (5.2), `CanaryNegativeControl` (5.20) |
| W10, W23 unknown property class, silent delta loss | `KnownCorruptNeverIgnored` (5.3) |
| W12, W20 Anthropic cumulative usage, extra map | `UsageKnowledgeTransitions` (5.6) |
| W13, W14, W18 tool keys, part boundaries, no-arg | `InterleavedToolOwnership` (5.4), `TransportProjectionParity` (5.9) |
| W19 Responses reasoning groups | `NativeRetentionForeignGate` (5.7) |
| W22 Gemini EOF | `ChunkPartitionInvariant` (5.1), 5.2 |
| W15, W16, F01 origin, binding, canary oracle | `OriginBindingFacts` (5.19), `CanaryNegativeControl` (5.20), 9.1 |
| F04 storage contradiction, resume identity | `JournalVersionAndCanonicalOrder` (5.13) |
| F06 demotion position | `NativeRetentionForeignGate` (5.7) |
| F07, F08, F09, F23 immutability, handle contract, stop token | `OwnershipAndBounds` (5.12), 5.2 |
| F11, F12 digest projection, library-side gate | 5.9, 5.13 |
| F16 mutant catalog, scheduler seam | 5.23, 5.24 |
| F17 staged ladder, reviewer rule | 1.1, 12 |
| F18 canary secrets, fork PRs, cost, account age | 9.2, 9.1 |
| F02 loopback http and test CA | 2 (`transport`) |
| D2 non-chat scope | scope paragraph, 10 |
| D3 async gates | 5.15, 5.21, 11 |
| D5 rule admission | `DescriptorRuleAdmission` (5.22) |
| Secrets in URLs and transport errors | 3, 5.12 |
