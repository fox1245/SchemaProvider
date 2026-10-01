# Roadmap: staged release ladder

Design proposal for a two-person team; private transport, Chat/Messages codecs, runtime and M5 canary/consumer experiment now exist (see POC_PLAN, measured M1–M5). M5 supports recording a bounded experiment but explicitly rejects strict lossless NeoGraph cutover. Stage order is fixed; each stage ships only after its own exit criteria. The PoC runs ahead of Stage 0 and de-risks Stage 1; it is not an installed release or completed release stage. Property names are in CONFORMANCE and decisions in decisions/README.

## Stage 0 — Foundations (docs and gates)
- Entry: DESIGN, CONFORMANCE, RESEARCH and decisions accepted by the maintainer.
- Work: fixture format; descriptor schema generator (descriptors embedded at configure time); conformance runner skeleton; include-direction gate (forbidden edges unified, measurable proxies); mutant catalog skeleton; deterministic scheduler/executor seam; sketch compile-check.
- Required properties (runnable as skeleton, real on the fixtures available): `JournalVersionAndCanonicalOrder`, `InstallAndDependencyDAG`, `DescriptorRuleAdmission`.
- Canary cells: none.
- Exit: runner executes a trivial fixture end to end; include gate fails on a seeded violation; rule-kind enumeration == `decisions/rules.json`.
- Cut line: no network code merges before Stage 0 exits.

## Stage 1 — Minimum shipping cell
- Scope: Chat Completions + Messages over HTTP/SSE, two vendors, one transport: libcurl on a private Asio loop (D1), async core (D3). No WebSocket, no Responses.
- Validation starts with hosted APIs: OpenAI Chat Completions and Anthropic direct Messages. Ollama/llama.cpp setup and local model runs are excluded from the current campaign. Model-free HTTP/TLS fixtures are transport oracles, not substitutes for the budgeted live canary.
- Entry: Stage 0 exit.
- Required properties: `ChunkPartitionInvariant`, `NoTerminalNoSuccess`, `KnownCorruptNeverIgnored`, `InterleavedToolOwnership`, `SnapshotNotAppend`, `UsageKnowledgeTransitions`, `StopMeaning`, `TransportProjectionParity` (SSE vs buffered), `RetrySafetyBudgetDeadline`, `IntentOrError`, `OwnershipAndBounds`, `CancelWithoutPeerProgress` (HTTP states), `AdmissionIndependentOfHeldStreams`, `InvalidToolCallRepresentation`, `ServerToolNotExecuted` (Messages), `FailureClassTerminal`, `OriginBindingFacts`, `DescriptorRuleAdmission`.
- Canary minimum: one live cell per vendor (Chat Completions vendor, Anthropic direct Messages) run nightly on the maintainers' own keys, never on fork PRs; `CanaryNegativeControl` run for Anthropic signed thinking (negative control must be rejected, otherwise the cell reports `ReplayAcceptanceUnobservable`); 60-run transport parity per D1 reconsideration condition 5.
- Exit: all listed properties green including sanitizers; both canary cells pass or are reported with cause; README states supported cells only.
- Cut line (drop first if late): second Chat Completions vendor -> cut to one; reasoning replay -> Drop mode only (capsule-less resume with its own identity).

### M5 checkpoint before any engine replacement

One N=1 first-party GPT-4.1 Mini / Haiku 4.5 run passed its text/SSE/tool-loop assertions, with a real signature-negative rejection for that captured Haiku continuation. Full plain/ASan+UBSan/TSan suites pass 14/14. These observations do not satisfy nightly cadence, 60-run parity/equivalence or every release property.

The existing consumer result/request ABI is a strict lossless **NO-GO**: five zero-default integer counters and editable native JSON cannot carry the new owned evidence. Next baseline work is an explicit rich-result/native-capable request contract and pre-dispatch version/capability gate, then reassessment through the same corpus. No lossy success shim, fake zero, engine replacement or journal rewrite is approved by M5.


## Stage 2 — Responses and OpenRouter reasoning
- Scope: OpenAI Responses over HTTP/SSE and WebSocket, single lane only (the protocol supports several stream_id lanes; the library must not misstate that); OpenRouter `reasoning_details`.
- Entry: Stage 1 exit; WS facts re-verified against the current vendor documentation.
- Required properties: Stage 1 set plus `NativeRetentionForeignGate`, `TransportProjectionParity` (WS), `CancelWithoutPeerProgress` (WS partial frame), `ServerToolNotExecuted` (Responses), `FailureClassTerminal` (OpenRouter in-band errors delivered as HTTP 200).
- Canary cells: OpenAI Responses SSE and WSS (60 runs each as the transport-parity canary of D1's reconsideration condition 5); OpenRouter cell with `negative_control` recorded; OpenRouter never in an equivalence class. Entry also needs the WebSocket transport decision D1b.
- Exit: Continuation has an explicit lifetime (Persisted | ConnectionBound) with full-input fallback or no-retry rule; previous_response_not_found path tested.
- Cut line: WS lane may slip to a later release while Responses over SSE ships.
- Private post-M5 HTTP/SSE Responses reasoning PoC now exists (POC_PLAN5.7). Final16/16 plain/ASan+UBSan/TSan groups pass; one buffered encrypted tool continuation is N=1 ReplayVerified, while omission was accepted. Initial live5/6 included a real SSE ciphertext-authority failure; two explicitly additive calls diagnosed it and confirmed corrected SSE, retaining8/US$0.186216 reservations. This is not a fresh6/6 run, live SSE tool replay or Stage2 exit. WebSocket, OpenRouter, server-state continuation and60-run qualification remain absent.

## Stage 3 — Gemini
- Scope: Gemini generate (buffered + SSE), part-structure-preserving codec. Interactions ONLY if a complete spec (event order, terminal semantics, resume) is written and reviewed first; otherwise not in this ladder.
- Entry: Stage 2 exit (or Stage 1 exit if the maintainer reorders).
- Required properties: Stage 1 set with `TransportProjectionParity` defined on parts, `FailureClassTerminal` (Gemini failure finish reasons).
- Canary cells: Gemini generate live cell; Interactions cell only if specified.
- Exit: terminal-evidence table covers Gemini x {buffered, SSE}; EOF-without-trailing-blank-line policy has a captured fixture.
- A separate cheap `gemini-2.5-flash-lite` OpenAI-compatibility text smoke now passed one buffered/SSE pair under a4-attempt/US$1 ceiling (POC_PLAN5.6). It is **not** this native Gemini stage's exit, tool/signature/replay support or broader model qualification. Future Gemini runs still prefer a low-cost model after checking current prices/features and explicit remaining/new budget.
- Cut line: Interactions.

## Stage 4 — NeoGraph adapter, journal v2, cutover
- Scope: NeoGraph adapter; journal v2 with a library-side projection golden and a consumer conformance kit; cutover including D2 typed clients and deletion gate.
- Entry: Stage 1 exit minimum (cutover of supported cells only); owner inventory for Images/Veo/Decisions available.
- Required properties: `JournalVersionAndCanonicalOrder`, `SnapshotNotAppend`, `OwnershipAndBounds`, plus all supported-cell properties via the adapter.
- Exit: D2 cutover checklist complete (delete-list approved by owner, half-migration search finds zero old-grammar interpreter code, kept features have typed clients); paused v1 runs are documented as not natively resumable and mid-tool-loop runs as not migratable; dual-implementation period ends.
- Canary cells: adapter smoke on each shipped cell.
- Cut line: anything unsupported stays on the old path only if the old path is fully removed from shared code; no partial migration.

## Optional HTTP/3 transport lane

- Accepted direction, not implemented or verified. [D1](decisions/D1-transport.md#optional-http3-policy) defines the policy; [POC_PLAN section 3.1](POC_PLAN.md#31-optional-http3-workstream) defines the workstream. M5's live/cutover findings do not change baseline stage order or make QUIC the next task.
- Scope: HTTP/3 preference in capable builds, safe same-origin HTTPS connection-stage fallback to HTTP/2/1.1, and a working non-HTTP/3 build. One libcurl stack, shared provider codecs/events/accumulator, no protocol-specific NeoGraph call path.
- Safety: only one connection candidate may send a request; possible acceptance is never retried under the name of fallback. One deadline/budget, generation POST 0-RTT disabled by default, and unchanged TLS verification.
- Exit before advertising HTTP/3: [CONFORMANCE section 12.1](CONFORMANCE.md#121-optional-http3-gate), including actual QUIC negotiation, fallback request counters, cancellation/deadline, reset/truncation, shared-stream bounds/isolation and applicable sanitizer runs. Baseline HTTP/2/1.1 must remain usable without QUIC dependencies.
- Benefit targets are lower cold-connection and cross-stream tail latency on suitable networks, not faster model inference; measure rather than promise. This lane does not settle D1b or claim WebSocket-over-HTTP/3 support.

## Explicitly deferred
- Python bindings.
- Bedrock and Vertex native access (SigV4, OAuth, binary event stream, credential refresh): out of scope unless designed; equivalence canary for them needs direct credentials (open item D4).
- Enterprise HTTP CONNECT proxy configuration and testing (libcurl can proxy; the library does not promise it before it is tested).
- HTTP/2 beyond the measured M1b case: stream caps, many paused streams, TLS multiplexing and other peer/libcurl combinations. One paused h2c stream with three live siblings, cancellation isolation, GOAWAY/REFUSED_STREAM and resolver refresh now pass (POC_PLAN section 5.1); this is not an arbitrary-concurrency memory guarantee.
- Windows and macOS: POSIX descriptor model and trust stores (POC_PLAN risks R4 and R5).
- Non-chat endpoints (D2): Veo, image endpoints, Decisions.
- Multi-lane Responses WebSocket.

## Risks tracked across stages
Vendor silently changes binding rules; gateway multi-backend nondeterminism; canary keys/cost/ToS and no secrets on fork PRs; bus factor of two; dual-implementation migration period.
