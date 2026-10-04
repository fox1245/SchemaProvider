# D3 — Async-native core

- Status: FIRM, 2026-10-01
- Deciders: maintainer + two-family review panel

Current runtime supports HTTP/SSE, not WebSocket. WS waits and the 45% WSS scheduled benchmark below preserve historical future acceptance proposals; they are excluded from current SDK release gates. See [Usage](../USAGE.md#streaming-deadlines-cancellation-and-backpressure) for actual callback/deadline/handle semantics.

## Context
Agent workloads hold many long streams (SSE, WebSocket) concurrently. A thread-per-request design ties capacity to thread count; a blocking-first design makes cancellation of waiting states hard.

## Decision
Async-native private core (bounded I/O workers, no thread per request) plus a blocking `complete()` facade that waits on the same operation. No switching thresholds between modes (they would need a blocking prototype to measure).

Gating properties:
- `CancelWithoutPeerProgress`: the server sends nothing more; cancel is exercised in each waiting state: DNS/connect, TLS handshake, header wait, partial SSE body then stall, WS partial frame, backoff wait. The operation ends with exactly one Cancelled outcome, without the server releasing it. A 1 s timeout is only an anti-hang guard, not the oracle.
- `AdmissionIndependentOfHeldStreams`: with T I/O threads and K = 4T+64 held streams open, a short call still completes and the process thread count stays <= T + constant, independent of K.

Deterministic release gates: library-caused extra threads <= 8 and independent of concurrency; exactly-one-outcome; no late callbacks; ASan/UBSan/TSan runs of `OwnershipAndBounds`.

Non-gating scheduled benchmark for the first 3 releases: C=1024 / 2C=2048 held-stream scenario (45% SSE, 45% WSS, 10% short HTTPS; p99 admission delay, RSS, cancel latency). After 3 baseline releases it is promoted to a +25% regression check. Capacity numbers are proposals, not measurements; agent count != in-flight count.

## Evidence
- [read source] NeoGraph already runs an async streaming path and native `WsClient` (see D1).
- [inference] Bounded workers make thread count independent of held streams; blocking facade keeps simple callers simple.
- [live observation] M4's private Chat/Messages runtime exercises 88 and 176 held SSE streams with six configured workers, a short unrelated request, an OS thread-count plateau and at most eight extra threads. This is a bounded-admission observation, not a throughput or supported-capacity claim; the scheduled benchmark above remains unrun.

## Options considered
- Blocking-first core with thread pool: rejected (admission depends on held streams, cancel of blocked reads is OS-specific).
- Thread-per-request: rejected (thread count scales with K).
- Threshold-based mode switching: rejected (needs unmeasurable prototype).

## Consequences
Callback/handle contract (exactly one outcome, ownership) is a first-class API concern; a deterministic scheduler/executor seam is required for race tests. Callbacks must not block the shared workers. Slow-callback diagnostics detect misuse, not preemption or a starvation-prevention guarantee (DESIGN section 7).

## Reconsideration conditions
Property gates cannot be met with the chosen executor on the supported toolchain floor; or the scheduled benchmark regresses >25% after baseline promotion with no fix.

## What would falsify it
Thread count growing with K, a waiting state where cancel needs peer progress, or a duplicate/missing outcome under TSan.

## Enforcement
`CancelWithoutPeerProgress`, `AdmissionIndependentOfHeldStreams`, `OwnershipAndBounds`; TSan/ASan/UBSan CI jobs; scheduled benchmark job.

### M4 implementation and corrections

At M4, `src/runtime/client.h` was private; the current typed runtime is installed with interface4 in a pre-stable package. `Operation` is move-only; destruction requests nonblocking cancellation and `detach()` relinquishes without cancel. SDK `Client::complete()` remains the blocking facade; results outlive Client and `join()` fences callback return/storage/slot release. Admitted preflight failures run on the executor; capacity/publication rejection throws `AdmissionError` without callbacks and `complete()` returns its owned Failure. `prepare()` exposes semantic/native rejection before dispatch. Historical M4/interface3 observations below are not new interface4 verification.

RAII applies to workers, timers, stop registrations, attempt handles and test processes/FDs. A timer wait originally borrowed a map key across an unlock; the actual API smoke reproduced ASan heap-use-after-free, so the wait now takes a deadline value. Injected initial enqueue allocation failure also reproduced a shutdown hang caused by an unreleased admission slot; an RAII publication reservation now rolls back that ownership. A bounded manual-executor test enumerates 298 ready-task schedules and records actual completion/cancel/timer firings; 64 seeded eight-operation schedules exercise the shared retry budget. These are local runtime tests, not exhaustive OS/libcurl scheduling or the future WebSocket gate.

### M5 consumer proof

**Historical M5 experiment, not the current installed boundary.** That opt-in bridge ran the actual runtime through the old NeoGraph `CompletionProvider` interface and an owned-result entry. It preserved explicit STREAM without an observer, caller-executor observation, cancellation/deadlines and RAII abandonment without per-request threads; plain/ASan+UBSan/TSan gates passed. Production callbacks retained only owned bounded mailboxes, never an abandoned caller executor. A negative temperature omission sentinel remained omitted on the actual wire. The obsolete adapter/build path is removed; M5's strict lossless old-ABI NO-GO remains historical.

At M5 the old result projection was deliberately refused with owned `BoundaryError` evidence even for numerically representable text answers: usage provenance and unknown-vs-zero could not be represented. It could reject only after a real request and therefore was never a valid production capability/billing probe. Full historical evidence is in POC_PLAN5.5. The later approved cutover now uses native-capable owned typed requests/results, exact-once `prepare`, durable host receipt before dispatch and a linked-interface capability gate; actual installed Program/C ABI/dualQuickJS and two-turn HTTP consumer proof are recorded in [current provenance](../POC_PLAN.md#current-typed-c-cutover-provenance), with Linux, remote TSan and release/qualification limits intact.



## Open items
- NeoGraph target concurrency C for the scheduled benchmark (owner input).
