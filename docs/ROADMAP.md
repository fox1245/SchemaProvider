# Roadmap: staged release ladder

Design proposal for a two-person team; nothing is implemented. Stage order is fixed; each stage ships only if its exit criteria pass. Property names are defined in CONFORMANCE.md section 5; decisions in decisions/README.md. A reviewer requirement is one independent reviewer per stage (may be a different model family), not a standing panel.

## Stage 0 — Foundations (docs and gates)
- Entry: DESIGN, CONFORMANCE, RESEARCH and decisions accepted by the maintainer.
- Work: fixture format; descriptor schema generator (descriptors embedded at configure time); conformance runner skeleton; include-direction gate (forbidden edges unified, measurable proxies); mutant catalog skeleton; deterministic scheduler/executor seam; sketch compile-check.
- Required properties (runnable as skeleton, real on the fixtures available): `JournalVersionAndCanonicalOrder`, `InstallAndDependencyDAG`, `DescriptorRuleAdmission`.
- Canary cells: none.
- Exit: runner executes a trivial fixture end to end; include gate fails on a seeded violation; rule-kind enumeration == `decisions/rules.json`.
- Cut line: no network code merges before Stage 0 exits.

## Stage 1 — Minimum shipping cell
- Scope: Chat Completions + Messages over HTTP/SSE, two vendors, single transport A (D1), async core (D3). No WebSocket, no Responses.
- Entry: Stage 0 exit.
- Required properties: `ChunkPartitionInvariant`, `NoTerminalNoSuccess`, `KnownCorruptNeverIgnored`, `InterleavedToolOwnership`, `SnapshotNotAppend`, `UsageKnowledgeTransitions`, `StopMeaning`, `TransportProjectionParity` (SSE vs buffered), `RetrySafetyBudgetDeadline`, `IntentOrError`, `OwnershipAndBounds`, `CancelWithoutPeerProgress` (HTTP states), `AdmissionIndependentOfHeldStreams`, `InvalidToolCallRepresentation`, `ServerToolNotExecuted` (Messages), `FailureClassTerminal`, `OriginBindingFacts`, `DescriptorRuleAdmission`.
- Canary minimum: one live cell per vendor (Chat Completions vendor, Anthropic direct Messages) run nightly on the maintainers' own keys, never on fork PRs; `CanaryNegativeControl` run for Anthropic signed thinking (negative control must be rejected, otherwise the cell reports `ReplayAcceptanceUnobservable`); 60-run transport parity per D1 flip (1).
- Exit: all listed properties green including sanitizers; both canary cells pass or are reported with cause; README states supported cells only.
- Cut line (drop first if late): second Chat Completions vendor -> cut to one; reasoning replay -> Drop mode only (capsule-less resume with its own identity).

## Stage 2 — Responses and OpenRouter reasoning
- Scope: OpenAI Responses over HTTP/SSE and WebSocket, single lane only (the protocol supports several stream_id lanes; the library must not misstate that); OpenRouter `reasoning_details`.
- Entry: Stage 1 exit; WS facts re-verified against the current vendor documentation.
- Required properties: Stage 1 set plus `NativeRetentionForeignGate`, `TransportProjectionParity` (WS), `CancelWithoutPeerProgress` (WS partial frame), `ServerToolNotExecuted` (Responses), `FailureClassTerminal` (OpenRouter in-band errors delivered as HTTP 200).
- Canary cells: OpenAI Responses SSE and WSS (60 runs each for D1 flip (1)); OpenRouter cell with `negative_control` recorded; OpenRouter never in an equivalence class.
- Exit: Continuation has an explicit lifetime (Persisted | ConnectionBound) with full-input fallback or no-retry rule; previous_response_not_found path tested.
- Cut line: WS lane may slip to a later release while Responses over SSE ships.

## Stage 3 — Gemini
- Scope: Gemini generate (buffered + SSE), part-structure-preserving codec. Interactions ONLY if a complete spec (event order, terminal semantics, resume) is written and reviewed first; otherwise not in this ladder.
- Entry: Stage 2 exit (or Stage 1 exit if the maintainer reorders).
- Required properties: Stage 1 set with `TransportProjectionParity` defined on parts, `FailureClassTerminal` (Gemini failure finish reasons).
- Canary cells: Gemini generate live cell; Interactions cell only if specified.
- Exit: terminal-evidence table covers Gemini x {buffered, SSE}; EOF-without-trailing-blank-line policy has a captured fixture.
- Cut line: Interactions.

## Stage 4 — NeoGraph adapter, journal v2, cutover
- Scope: NeoGraph adapter; journal v2 with a library-side projection golden and a consumer conformance kit; cutover including D2 typed clients and deletion gate.
- Entry: Stage 1 exit minimum (cutover of supported cells only); owner inventory for Images/Veo/Decisions available.
- Required properties: `JournalVersionAndCanonicalOrder`, `SnapshotNotAppend`, `OwnershipAndBounds`, plus all supported-cell properties via the adapter.
- Exit: D2 cutover checklist complete (delete-list approved by owner, half-migration search finds zero old-grammar interpreter code, kept features have typed clients); paused v1 runs are documented as not natively resumable and mid-tool-loop runs as not migratable; dual-implementation period ends.
- Canary cells: adapter smoke on each shipped cell.
- Cut line: anything unsupported stays on the old path only if the old path is fully removed from shared code; no partial migration.

## Explicitly deferred
- Python bindings.
- Bedrock and Vertex native access (SigV4, OAuth, binary event stream, credential refresh): out of scope unless designed; equivalence canary for them needs direct credentials (open item D4).
- Proxy (HTTP CONNECT) support.
- HTTP/2, unless the owner confirms it as a requirement (D1 flip (4); retirement of the opt-in capability needs owner approval).
- Windows/macOS trust stores (D1 flip (2)).
- Non-chat endpoints (D2): Veo, image endpoints, Decisions.
- Multi-lane Responses WebSocket.

## Risks tracked across stages
Vendor silently changes binding rules; gateway multi-backend nondeterminism; canary keys/cost/ToS and no secrets on fork PRs; bus factor of two; dual-implementation migration period.
