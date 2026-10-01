# SchemaProvider

A multi-vendor LLM client library for C++20.

> **Status: private M5 proof of concept. No installed/stable library or lossless NeoGraph cutover is approved.**
> This repository is the future home of `SchemaProvider`, which currently lives inside [NeoGraph](https://github.com/fox1245/NeoGraph). The PoC has one libcurl-on-Asio transport, strict descriptors, buffered/SSE Chat and Messages codecs, one accumulator, an async-native runtime, a budgeted canary CLI and an optional isolated NeoGraph experiment. See the [measured M5 result](docs/POC_PLAN.md#55-m5-results-and-go-no-go-live-observation), not the full design, for execution evidence. The direction remains under discussion in [fox1245/NeoGraph#321](https://github.com/fox1245/NeoGraph/issues/321).
> M1/M1b transport verification: 32 tests pass under plain, ASan+UBSan and TSan on Linux, including TLS certificate rejection and HTTP/2 multiplexing. Validation targets hosted APIs first; Ollama/llama.cpp setup is excluded. No live provider compatibility is claimed by these model-free loopback tests.
> M3 verification: 62 independently reviewed Messages fixtures, 30,430 partition variants and ten buffered/SSE parity pairs; both client tool-result and server-pause continuation traverse real loopback HTTP. Twenty-two replay mutations are rejected before dispatch. Existing Chat coverage remains 35 fixtures, 9,409 partitions and five parity pairs. These synthetic capsules prove local retention and rejection, not vendor signature validation.
> M4 verification: move-only operations, immutable owned results, stop/deadline handling, bounded queues and default-off retry. RAII covers normal and exceptional ownership. Evidence includes 298 deterministic schedules, 88/176 held-stream admission and exact 8 MiB delivery through a 32 KiB queue.
> M5 verification: **14/14 CTest groups pass under plain, ASan+UBSan and TSan**. One live run passed four OpenAI GPT-4.1 Mini assertions and five Anthropic Haiku 4.5 assertions, including a retained positive thinking replay and a dispatched one-byte-signature negative rejected by the provider. This is N=1, not release/equivalence admission. The 97-fixture legacy comparison leaves 57 expected-fail candidates and 40 unsupported cases: **strict lossless cutover is NO-GO**.
> Gemini compatibility smoke: **2/2 bounded text assertions pass** on `gemini-2.5-flash-lite` through Google's OpenAI-compatible Chat Completions endpoint, reusing the existing runtime/Chat codec. This confirms the selected key/route for that buffered/SSE pair, **not native Gemini, thinking-signature, tool-replay or broad vendor support**. Scope and accounting: [POC_PLAN section 5.6](docs/POC_PLAN.md#56-cheap-gemini-compatibility-smoke-live-observation).
> **HTTP/3: accepted direction, not implemented.** The [D1 policy](docs/decisions/D1-transport.md#optional-http3-policy) adds optional HTTP/3 preference with safe HTTP/2/1.1 connection fallback, while keeping non-QUIC builds usable. Provider semantics stay shared; generation POST 0-RTT is off by default. The current linked libcurl has no HTTP/3 support.

## What it is meant to be
One C++20 client over the chat API families the major vendors expose (Chat Completions, Responses, Messages, Gemini generate; Interactions later), usable on its own and as the LLM layer of NeoGraph. The first release is chat completion plus artifacts that arrive inside chat responses; image endpoints, long-running video operations and the OpenRouter decisions endpoint are not part of it.

## Goals under consideration
- Typed errors carrying status, retry class, retry safety (not sent, possibly accepted, rejected before output, output observed), vendor code and request id, so a provider failure is never returned as a successful completion. A stream that ends without terminal evidence is a failure, never a success.
- Token usage including cached and reasoning tokens, with each vendor's counting rules kept as data and "unknown" kept distinct from zero.
- Per-call request knobs declared per vendor, with a clear error before any network call for a key that is not declared.
- Structure in typed code per API family; the values that change often (endpoints, headers, knobs, closed validation rules, retryable codes, model lists) in versioned JSON descriptors that cannot contain hooks or stateful rules. Adding a strictly compatible vendor needs no C++ source edit, but the descriptor is embedded at build time, so it still means a rebuild and release.
- One semantic event model and one accumulator shared by buffered, SSE and WebSocket responses.
- Automatic retry off by default; if enabled, in exactly one layer, with a deadline, a shared budget and respect for `Retry-After`.
- A conformance suite built from recorded real vendor request/response pairs and streams, so behaviour learned from the live APIs is a spec that both the current and the new implementation must pass.
- A portable conversation history plus an opaque, origin-bound sidecar for vendor reasoning state (signed or encrypted reasoning cannot be translated between vendors, and is rejected by default when the origin differs). Replay is also checked against binding facts (model, context fingerprint, account scope); origin is a replay boundary, not proof of who issued a block.

## Documentation
All of these describe a proposal except where a document says it reports a measurement.
- [docs/POC_PLAN.md](docs/POC_PLAN.md): the proof-of-concept plan, its milestones, the transport spike results and the risk register.
- [docs/DESIGN.md](docs/DESIGN.md): proposed architecture, vocabulary, streaming model, threat model, extension walkthroughs, decisions and the open questions.
- [docs/ROADMAP.md](docs/ROADMAP.md): the staged release plan (what ships first and the extension order).
- [docs/CONFORMANCE.md](docs/CONFORMANCE.md): the properties a release must pass and how they are tested.
- [docs/RESEARCH.md](docs/RESEARCH.md): the measurements and surveys the design is based on.
- [docs/decisions/](docs/decisions/): the decision records D1-D5 (all FIRM; D1 revised to libcurl on 2026-10-01) and the descriptor rule ledger.

## Not goals for now
- Image, video-operation and routing-decision endpoints in the first release: NeoGraph keeps ownership of them and of the cutover.
- Native Amazon Bedrock or Google Vertex access (SigV4, OAuth refresh, binary event-stream framing).
- Python bindings (deferred until the C++ API settles).
- A graph or agent runtime: that stays in NeoGraph.

## Relationship to NeoGraph
NeoGraph's engine already depends only on the `Provider` interface, not on `SchemaProvider`. Splitting the two is mostly making that boundary real. The staging (contract cleanup inside NeoGraph, then the conformance suite, then the new implementation, then the switch; NeoGraph's cutover also removes its old descriptor interpreter) is written up in [NeoGraph#321](https://github.com/fox1245/NeoGraph/issues/321).

## License
MIT. See [LICENSE](LICENSE).
