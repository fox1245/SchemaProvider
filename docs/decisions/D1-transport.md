# D1 — Transport

- Status: FIRM, revised 2026-10-01. Supersedes the earlier GATED default "A: one private Asio + OpenSSL stack, no libcurl".
- Decider: the owner accepts one libcurl/Asio stack and optional HTTP/3. Both capable and incapable Linux transport scenarios have been exercised, including capable plain/ASan+UBSan/TSan runs; hosted-provider interoperability, broad platform coverage and release-backend instrumentation limits remain distinct.
- Evidence labels: `[read source]`, `[read docs]`, `[live observation]`, `[inference]`. Live observations are local loopback runs.

## Context
The library needs HTTP/1.1, HTTP/2 and optional HTTP/3, SSE, and cancellation/deadlines without peer progress. The approved NeoGraph provider cutover uses typed HTTP/SSE and removes its old Responses WebSocket lane. Generic non-provider WebSocket transport is unaffected.

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
5. **No Responses WebSocket lane in this cutover.** The owner approved typed HTTP/SSE replacement rather than retaining the old interpreter or a permanent hybrid provider stack.
6. **HTTP/3 is optional within the same libcurl stack.** HTTP/3-capable builds can prefer it with safe connection-stage fallback; builds without it retain HTTP/2 and HTTP/1.1. No application-wide QUIC dependency requirement and no second HTTP stack. Policy and admission conditions follow below.

## Optional HTTP/3 policy

**Status: implemented; capable Linux scenarios pass under plain, ASan+UBSan and TSan.** The prior 32 transport cases and incapable build remain separate HTTP/1.x/HTTP/2 evidence. Actual QUIC exchanges are verified with an isolated capable build; this does not establish hosted API compatibility or a performance advantage.

| Build / policy | Implemented behavior |
|---|---|
| HTTP/3-capable libcurl, HTTP/3 preference enabled | Prefer HTTP/3 for eligible HTTPS calls; allow HTTP/2 or HTTP/1.1 connection-stage fallback to the same HTTPS origin if QUIC is unavailable, unsupported by the peer or blocked by the network. |
| libcurl without HTTP/3, or HTTP/3 preference disabled | Keep the normal HTTP/2/HTTP/1.1 path usable; do not require QUIC packages for this build. |
| HTTP/3-only verification mode | No downgrade. Missing runtime capability fails explicitly; a successful exchange counts only when the actual negotiated version is HTTP/3. Use an isolated connection cache so a reused HTTP/2 connection cannot masquerade as an HTTP/3 test. |

The operational rules are:

1. **Capability is not a version number or a CLI claim.** Check the linked library's `CURL_VERSION_HTTP3` capability and record the actual response protocol. HTTP/3 preference is not a promise that every request uses it: libcurl may reuse an existing connection. The intended mapping is `CURL_HTTP_VERSION_3` for preference and `CURL_HTTP_VERSION_3ONLY` for the isolated verification lane ([libcurl option semantics](https://curl.se/libcurl/c/CURLOPT_HTTP_VERSION.html)).
2. **Protocol fallback is not an application retry.** Connection candidates may race, but only one may send the HTTP request. Fallback stays within the current attempt and its absolute deadline, without resetting budgets. Once request bytes may have been sent, a new HTTP request is a retry governed solely by the existing retry controller; absence of a response is not permission to resend. After semantic output there is no reconnect/retry. TLS verification and the original origin/credential boundary remain unchanged.
3. **Shared semantics stay shared.** Provider codecs, SSE framing, the event model, the accumulator and NeoGraph call sites do not split by HTTP version. HTTP/3 response-version reporting, stream completion, QUIC error classification and pause/cancel handling belong in the private transport. HTTP/1.x socket-wide read suppression must not be applied to a shared QUIC connection.
4. **Generation POSTs do not use TLS 0-RTT early data by default.** Enabling it would need a separately reviewed replay/idempotency contract and tests; enabling HTTP/3 alone never enables early data. Packet retransmission inside QUIC is not a second application request, but replaying an HTTP request is ([RFC 9001, section 2.1](https://www.rfc-editor.org/rfc/rfc9001.html#section-2.1)).
5. **Dependencies remain private and optional.** The initial build candidate is libcurl with ngtcp2, nghttp3 and a compatible TLS backend. Pin and validate that combination in an isolated build/install prefix, without replacing the system libcurl; the baseline non-HTTP/3 build remains supported. QUIC backend types do not enter installed headers or provider codecs. Exact package versions and distribution strategy remain C1/C5 decisions. The current [curl build guide](https://curl.se/docs/http3.html) marks ngtcp2 non-experimental and quiche experimental; its OpenSSL route requires OpenSSL 3.5+ and ngtcp2 1.12+, not the tested baseline's OpenSSL 3.0.13.

**Rationale.** `[read docs]` QUIC provides reliable per-stream delivery, avoids TCP's cross-stream head-of-line blocking, and reduces connection setup latency ([RFC 9114, sections 1-2](https://www.rfc-editor.org/rfc/rfc9114.html#section-1), [RFC 9001, section 2](https://www.rfc-editor.org/rfc/rfc9001.html#section-2)). `[inference]` For concurrent LLM streams sharing one connection, especially on lossy or high-RTT paths, this can reduce correlated output stalls and tail latency. It does not accelerate model inference, remove shared congestion/bandwidth limits, or guarantee an improvement over a warm HTTP/2 connection. Deployment and regression-testing costs remain real; no speedup or connection-migration support is claimed without measurement.

**Admission.** Run the [optional HTTP/3 workstream](../POC_PLAN.md#31-optional-http3-workstream) and [conformance gate](../CONFORMANCE.md#121-optional-http3-gate). A passing fallback exchange is never HTTP/3 evidence. Test both capable and incapable builds, request counts across protocol candidates, cancellation/deadline without peer progress, stream completion/reset, paused-stream bounds and sibling isolation. Preserve the existing one-attempt rule and run plain/ASan+UBSan/TSan checks before advertising support.

## Evidence
- `[live observation]` A/B benchmark in a separate worktree, baseline commit 60b795aa (NeoGraph master at the time), local TLS HTTP/1.1 and HTTP/2 server, randomized order, 220 process runs and 47,740 measured requests with zero failures. The artifacts are local files outside this repository and are not reproducible from it.
  - The existing `CurlH2Pool` calls `curl_multi_poll(..., 500)` before draining `CURLMSG_DONE`, which delays completion delivery by about 500 ms when the last request has already finished. A control that changes only that order moved p50 from 501.8 ms to 0.56 ms on HTTP/1.1 and from 502.0 ms to 0.72 ms on HTTP/2 (server delay 0, one request). The delay is a wrapper defect common to both protocols, not an HTTP/2 property.
  - POST, 1 KiB response, 20 ms server delay, per-run p50 then median of 5 runs: at 5 concurrent, Asio HTTP/1.1 21.07 ms, libcurl HTTP/1.1 21.12 ms, libcurl HTTP/2 21.33 ms; at 128 concurrent, 25.40, 26.31 and 24.28 ms. At 128 concurrent POSTs the server saw 128 connections for Asio and for libcurl HTTP/1.1 and 1 connection for libcurl HTTP/2.
  - SSE, 32 concurrent, 8 events: completion p50 51.38 ms (Asio, new connection per request), 47.42 ms (libcurl HTTP/1.1, new connections), 24.77 ms (libcurl HTTP/1.1, reused), 22.25 ms (libcurl HTTP/2); first event 31.70, 30.41, 6.63, 6.45 ms. Connection reuse, not the protocol version, explains most of the difference.
  - Not covered: real vendors, proxies, WebSocket, the full NeoGraph call path. That benchmark polled in a worker thread; it did not use the Asio integration.
- `[read source]` At NeoGraph commit 60b795aa the hand-written HTTP/1.1 exchange code is 2,287 lines (`src/async/conn_pool.cpp` 482, `src/async/http_client.cpp` 526, `src/async/http_exchange_detail.h` 1,279, counted with `wc -l`); the existing libcurl HTTP/2 wrapper `src/async/curl_h2_pool.cpp` is 686 lines.
- `[live observation]` M1 transport spike, 27 tests green under plain, ASan+UBSan and TSan builds, plus a read-only review by a model of another family whose confirmed findings were fixed with regression tests (POC_PLAN section 5): the libcurl 8.5.0 pause behaviour under `multi_socket`, the resend refusal, the per-lookup resolver threads, framing that EOF could turn into success, a destructor deadlock on an I/O thread, and an AddressSanitizer use-after-free in the spike's own code.
- `[live observation]` M1b extends this to 32 tests passing under plain, ASan+UBSan and TSan without a production transport change. Custom-CA TLS succeeds with HTTP/1.1 and ALPN HTTP/2; wrong-host, expired and self-signed certificates fail before any HTTP request. One paused h2c stream remains bounded while three siblings complete on the same session; cancellation preserves the sibling. GOAWAY and REFUSED_STREAM both trigger a refused resend with exactly one request seen by the peer. Resolver refresh affects fresh connections after idle retirement, not the lifetime of healthy pooled connections. Exact measurements and oracle corrections: POC_PLAN section 5.1.
- `[live observation]` M2 adds the Chat fixture path above the same transport: 35 independently reviewed synthetic fixtures, 9,409 partition variants and five buffered/SSE parity pairs pass, with five CTest groups green under plain, ASan+UBSan and TSan. An observed empty-literal-header omission required one transport correction (libcurl's empty-header form); the existing 32 transport cases still pass. These are loopback results, not live-provider or HTTP/3 evidence. Details: POC_PLAN section 5.2.
- `[live observation]` M3 adds 62 independently reviewed Messages fixtures, 30,430 partition variants, ten parity pairs and actual two-request tool/server continuation over the same transport. No production transport change was needed. All nine CTest groups pass under plain, ASan+UBSan and TSan. Initial RSS/settle-time test failures and the precise-RSS/bounded-plateau oracle corrections are retained in POC_PLAN section 5.3; thresholds were not relaxed. These remain model-free loopback results, not a live-provider or HTTP/3 claim.
- `[live observation]` Historical baseline HTTP/3 capability probe against shared libcurl8.5.0/OpenSSL3.0.13: `HTTP2=1`, `HTTP3=0`. Setting either `CURL_HTTP_VERSION_3` or `CURL_HTTP_VERSION_3ONLY` returned `CURLE_UNSUPPORTED_PROTOCOL`; no HTTP transfer occurred. This proves that baseline library lacked HTTP/3, not that the current optional adapter/capable build is unimplemented. The implemented non-capable policy keeps H2/H1 usable and rejects HTTP/3-only before dispatch.
- `[live observation]` Isolated curl 8.16.0 (`8.16.0-DEV` build string), OpenSSL 3.5.3, ngtcp2 1.16.0, nghttp3 1.12.0 and nghttp2 1.65.0 report `HTTP2=1`, `HTTP3=1`. `http3_transport_scenarios ... capable` passes preferred/only exchanges over actual HTTP/3, Auto over HTTP/2, and connection-stage fallback with one HTTP/2 POST and zero internal resends. The only lane never downgrades.
- `[live observation]` The same capable run exercises buffered/SSE delivery, QUIC reset/truncated/short-body failures, cancel/deadline without peer progress, and same-connection pause/cancel isolation. A paused 64 MiB stream plateaus at 557,056 peer-produced bytes with 356,352 bytes of observed RSS growth, then delivers all 67,108,864 bytes. Its sibling completes; cancelling another paused stream leaves a surviving 64 MiB stream intact. These are one-run bounded local observations, not universal memory bounds or throughput claims.
- `[live observation]` The capable scenario also passes with the transport/scenario C++ units compiled under ASan+UBSan and TSan. TSan uses the existing `setarch x86_64 -R` launch convention; GCC reports its known `atomic_thread_fence` instrumentation warning in Asio. No sanitizer runtime finding occurs. The isolated curl/OpenSSL/ngtcp2/nghttp3 dependencies themselves are release builds, not sanitizer-instrumented libraries.
- Reproduce with an isolated HTTP/3-capable libcurl prefix, the existing `tests/support/http3_peer.py` and an isolated Python containing aioquic/h2. Select `SP_HTTP3_PYTHON`, configure the SDK against that prefix, then run the registered `http3_scenarios`; use `capable` or `incapable` when invoking the executable directly. Neither source acquisition nor prefix installation replaces system libraries or trust stores.

## Options considered
- **A, one private Asio + OpenSSL stack (previous default):** rejected by the owner after the A/B result removed the performance argument, and because it keeps the maintenance of an HTTP/1.1 client, a connection pool and (later) an HTTP/2 path in this library.
- **C, Asio for WebSocket and libcurl for HTTP as a permanent state:** still rejected as a permanent state (two TLS, socket and cancel families); see item 5 for D1b.
- **httplib:** rejected: blocking I/O, HTTP/1.1 only, no cancellation fit for the async core (D3).

## Consequences
- libcurl is a required dependency; the current installed SDK uses a7.88.0 baseline floor and was exercised with system8.5.0 and the isolated capable8.16.0-DEV combination. Broader version/backend/platform qualification and distribution strategy remain open, not an increase to the baseline floor.
- TLS trust and verification follow libcurl's TLS backend. The custom-CA and certificate-failure matrix passes on the tested OpenSSL backend (M1b, risk R4); public OS trust stores and other platforms/backends remain unmeasured.
- The spike is POSIX-only (`asio::posix::stream_descriptor`); a Windows socket path is a later task (risk R5).
- libcurl 8.5.0 specifics found by the PoC (pause under `multi_socket`, resend, resolver threads) are workarounds in the transport and must be re-verified when the supported libcurl range changes.
- Proxies are not supported: the transport sets an empty `CURLOPT_PROXY` so that environment proxy variables cannot redirect traffic or start libcurl resolver threads outside the bounded pool.
- Optional HTTP/3 adds a separately validated dependency/build matrix and the admission gate above. Failure of that lane must not disable the baseline HTTP/2/HTTP/1.1 build; HTTP/3 preference is a protocol policy within libcurl, not the rejected runtime backend selector.

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
- C1 libcurl floor and feature requirements; C5 deployment (system libcurl baseline versus an isolated HTTP/3-capable dependency build, exact backend versions and packaging).
- Responses WebSocket is excluded by the owner's typed HTTP/SSE cutover decision; any future new lane needs separate admission.
- R3's broader HTTP/2 stream-count/TLS/peer/version matrix, R4's public trust stores and other TLS backends, and R5's non-Linux socket/platform support (POC_PLAN section 6). M1b closes only the measured scenarios, not universal compatibility.
