# D1 — Transport

- Status: FIRM, revised 2026-10-01. Supersedes the earlier GATED default "A: one private Asio + OpenSSL stack, no libcurl".
- Decider: the owner (decision: SchemaProvider adopts libcurl; the event loop stays the existing standalone Asio), after the A/B measurement below and the M1 transport spike of [../POC_PLAN.md](../POC_PLAN.md).
- Evidence labels: `[read source]`, `[read docs]`, `[live observation]`, `[inference]`. Live observations are local loopback runs.

## Context
The library needs HTTP/1.1 and HTTP/2, SSE for streamed responses, cancellation and deadlines that complete without peer progress, and a WebSocket lane for OpenAI Responses (a later stage). The previous default kept NeoGraph's hand-written Asio + OpenSSL HTTP/1.1 client and argued against libcurl partly on a recorded claim that libcurl HTTP/2 is about 25% slower at p50.

## Decision
1. **One HTTP stack: libcurl, `multi_socket` interface**, private to the library. No httplib, no hand-written HTTP/1.1 client, no runtime backend flag, no `prefer_libcurl`.
2. **The event loop is the existing standalone Asio**, private to the transport and never in a public header. libcurl's socket and timer callbacks are bridged to Asio; all libcurl calls run on one strand.
3. **What the library still owns above libcurl** (each is a tested property of the PoC, see POC_PLAN section 5):
   - exactly one outcome per operation, cancel and deadline that finish without peer progress;
   - the one-attempt rule: libcurl's own resend on a fresh connection is refused through the prerequisite callback and reported as a failure;
   - backpressure: read readiness is not reported for the socket of a paused HTTP/1.x transfer, because libcurl 8.5.0 otherwise reads and buffers while paused;
   - name resolution: single-flight, TTL-cached, on a bounded resolver pool, passed to libcurl with `CURLOPT_RESOLVE`, because libcurl's threaded resolver starts a thread per concurrent lookup;
   - normal-close classification: a body delimited only by connection close is a failure.
4. **HTTP/2 comes from libcurl.** The opt-in HTTP/2 capability of the current NeoGraph implementation is therefore not retired; the former owner question O1 is closed.
5. **WebSocket (OpenAI Responses lane) is not decided here (D1b).** libcurl's WebSocket API is official from 8.11 and the frame-masking fix is in 8.16 (`[read docs]`), while the libcurl on this machine and on Ubuntu 24.04 is 8.5.0 and Debian 12 ships 7.88.1 (`[inference]` from distribution package versions). D1b must be decided before ROADMAP Stage 2 and not before Stage 1. A permanent hybrid of two socket and cancel families remains rejected unless D1b shows no alternative and the cost is stated.

## Evidence
- `[live observation]` A/B benchmark in a separate worktree, baseline commit 60b795aa (NeoGraph master at the time), local TLS HTTP/1.1 and HTTP/2 server, randomized order, 220 process runs and 47,740 measured requests with zero failures. The artifacts are local files outside this repository and are not reproducible from it.
  - The existing `CurlH2Pool` calls `curl_multi_poll(..., 500)` before draining `CURLMSG_DONE`, which delays completion delivery by about 500 ms when the last request has already finished. A control that changes only that order moved p50 from 501.8 ms to 0.56 ms on HTTP/1.1 and from 502.0 ms to 0.72 ms on HTTP/2 (server delay 0, one request). The delay is a wrapper defect common to both protocols, not an HTTP/2 property.
  - POST, 1 KiB response, 20 ms server delay, per-run p50 then median of 5 runs: at 5 concurrent, Asio HTTP/1.1 21.07 ms, libcurl HTTP/1.1 21.12 ms, libcurl HTTP/2 21.33 ms; at 128 concurrent, 25.40, 26.31 and 24.28 ms. At 128 concurrent POSTs the server saw 128 connections for Asio and for libcurl HTTP/1.1 and 1 connection for libcurl HTTP/2.
  - SSE, 32 concurrent, 8 events: completion p50 51.38 ms (Asio, new connection per request), 47.42 ms (libcurl HTTP/1.1, new connections), 24.77 ms (libcurl HTTP/1.1, reused), 22.25 ms (libcurl HTTP/2); first event 31.70, 30.41, 6.63, 6.45 ms. Connection reuse, not the protocol version, explains most of the difference.
  - Not covered: real vendors, proxies, WebSocket, the full NeoGraph call path. That benchmark polled in a worker thread; it did not use the Asio integration.
- `[read source]` At NeoGraph commit 60b795aa the hand-written HTTP/1.1 exchange code is 2,287 lines (`src/async/conn_pool.cpp` 482, `src/async/http_client.cpp` 526, `src/async/http_exchange_detail.h` 1,279, counted with `wc -l`); the existing libcurl HTTP/2 wrapper `src/async/curl_h2_pool.cpp` is 686 lines.
- `[live observation]` M1 transport spike, 27 tests green under plain, ASan+UBSan and TSan builds, plus a read-only review by a model of another family whose confirmed findings were fixed with regression tests (POC_PLAN section 5): the libcurl 8.5.0 pause behaviour under `multi_socket`, the resend refusal, the per-lookup resolver threads, framing that EOF could turn into success, a destructor deadlock on an I/O thread, and an AddressSanitizer use-after-free in the spike's own code.

## Options considered
- **A, one private Asio + OpenSSL stack (previous default):** rejected by the owner after the A/B result removed the performance argument, and because it keeps the maintenance of an HTTP/1.1 client, a connection pool and (later) an HTTP/2 path in this library.
- **C, Asio for WebSocket and libcurl for HTTP as a permanent state:** still rejected as a permanent state (two TLS, socket and cancel families); see item 5 for D1b.
- **httplib:** rejected: blocking I/O, HTTP/1.1 only, no cancellation fit for the async core (D3).

## Consequences
- libcurl becomes a required dependency with a version floor (open item C1: proposed 7.88.0, tested 8.5.0) and packaging questions per platform.
- TLS trust and verification follow libcurl's TLS backend; the certificate-failure matrix is an M1b test (risk R4).
- The spike is POSIX-only (`asio::posix::stream_descriptor`); a Windows socket path is a later task (risk R5).
- libcurl 8.5.0 specifics found by the PoC (pause under `multi_socket`, resend, resolver threads) are workarounds in the transport and must be re-verified when the supported libcurl range changes.
- Proxies are not supported: the transport sets an empty `CURLOPT_PROXY` so that environment proxy variables cannot redirect traffic or start libcurl resolver threads outside the bounded pool.

## Reconsideration conditions
Reopen D1 if any of these is measured:
1. HTTP/2 pause with live sibling streams buffers without a usable bound (risk R3) and neither a stream cap nor a documented limit is acceptable.
2. The TLS verification matrix (expired, wrong host, self-signed rejected) cannot be met through libcurl on a supported platform.
3. A property that a stable release must meet (POC_PLAN section 4) cannot be met with `multi_socket` plus the workarounds above.
4. D1b can only be met by a permanent second socket stack whose cost outweighs the libcurl benefit.
5. Live transport parity fails persistently for a required direct-vendor cell: 60 runs per cell, transport-caused failures only.

## What would falsify it
An unbounded buffer or an unfinishable operation in an M1b scenario; a transport-caused persistent failure in a live cell; a libcurl version in the supported range that removes a workaround's premise without a replacement.

## Enforcement
Properties `OwnershipAndBounds`, `CancelWithoutPeerProgress`, `AdmissionIndependentOfHeldStreams`, `NoTerminalNoSuccess`, `RetrySafetyBudgetDeadline`, `InstallAndDependencyDAG`. Gate: no libcurl or Asio header reachable from an installed header. The transport tests of POC_PLAN section 4 run in the plain, ASan+UBSan and TSan configurations.

## Open items
- C1 libcurl floor and feature requirements; C5 deployment (system libcurl versus vendoring).
- D1b WebSocket transport.
- Risks R3 (HTTP/2 pause), R4 (TLS and trust), R5 (Windows) of POC_PLAN section 6.
