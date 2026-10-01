# Proof-of-concept plan

Status: M0, M1 and M1b are done and measured on this branch; M2 to M5 are planned and not started. Everything about M2 and later is a plan, not a result. Evidence labels as in DESIGN.md: `[live observation]` (run here, local loopback, libcurl 8.5.0 with OpenSSL 3.0.13, Linux on WSL2), `[read source]`, `[read docs]`, `[inference]`.

## 1. Purpose and non-purpose

The PoC answers one question before any large investment: **can libcurl driven by a private Asio loop carry the transport contract of DESIGN.md sections 2, 4.2, 7 and the properties of CONFORMANCE.md 5.2, 5.12, 5.15, 5.21, and can one semantic event model and accumulator then be built on top of it for two API families?**

It is not a release, not a public API, and not a replacement of NeoGraph's provider yet. Nothing in `src/` is installed or stable. No live vendor call is made before the owner sets a call and cost budget (open decision C3).

Validation prioritizes **hosted API providers**: OpenAI Chat Completions and Anthropic direct Messages are the first target cells. Ollama and llama.cpp environment setup, model downloads and local inference runs are excluded from this PoC campaign. The localhost HTTP/TLS peers below are model-free, independent transport oracles; passing them is not evidence of live provider compatibility. Live calls remain gated by C3.

## 2. Decisions this plan rests on

| Decision | Value | Record |
|---|---|---|
| HTTP transport | libcurl `multi_socket`, one private stack | [D1](decisions/D1-transport.md) (revised 2026-10-01, FIRM) |
| Event loop | the existing standalone Asio, private to the transport, never in a public header | D1, D3 |
| Public API shape | async core with a blocking facade, no Asio type public | D3 |
| First cells | OpenAI Chat Completions and Anthropic direct Messages over HTTP and SSE; hosted APIs, fixtures first | ROADMAP Stage 1; live budget C3 |
| Out of the PoC | WebSocket and Responses, Gemini, Interactions, Python bindings, non-chat endpoints, Bedrock and Vertex | ROADMAP, D2 |

## 3. Milestones

Each milestone has an exit that is checked by running something, and a stop rule.

| M | Content | Exit | Status |
|---|---|---|---|
| M0 | Align D1, DESIGN, ROADMAP, CONFORMANCE, README with the libcurl decision; this plan | docs consistent; no document still says "no libcurl" | done |
| M1 | Transport spike: libcurl `multi_socket` on Asio with cancel, deadline, bounded threads, backpressure, one attempt | the experiments of section 4 pass under plain, ASan+UBSan and TSan builds | done (section 5) |
| M1b | TLS success and certificate-failure matrix, ALPN HTTP/2 over TLS, HTTP/2 pause/cancel with live sibling streams, GOAWAY/REFUSED_STREAM resend refusal, resolver address refresh; POSIX and WebSocket limits | R1 to R4 exercised; R5 and R6 explicit limits rather than implied support | done (section 5.1) |
| M2 | First vertical slice: SSE framer, `Event`, one accumulator, strict descriptor loader (unknown keys rejected), Chat Completions codec buffered and SSE, fixture runner | `ChunkPartitionInvariant`, `NoTerminalNoSuccess`, `TransportProjectionParity` (buffered vs SSE), `KnownCorruptNeverIgnored` on scripted fixtures | next |
| M3 | Messages codec: thinking capsules with origin, cumulative usage, server tools, stop meanings | `InterleavedToolOwnership`, `SnapshotNotAppend`, `UsageKnowledgeTransitions`, `StopMeaning`, `InvalidToolCallRepresentation`, `ServerToolNotExecuted` | planned |
| M4 | Runtime: `Operation` over the transport, `std::stop_token`, blocking `complete()`, typed `Failure` from `AttemptObservation`, one retry controller (off by default) | `RetrySafetyBudgetDeadline`, `OwnershipAndBounds` at the client level, secret-marker scan | planned |
| M5 | Budgeted live canary on one cell per family, NeoGraph adapter spike, the conformance corpus run against the current NeoGraph implementation, independent review by a different model family | go or no-go record: what stays, what is cut, what the NeoGraph cutover costs | planned |

Stop rule for every milestone: if a property cannot be met, the finding is written into the relevant decision record with its evidence before work continues; a failing property is never relaxed to make a run green.

## 4. M1 experiments (what was built and what each one proves)

Code: `src/transport/http_transport.{h,cpp}` (private port, no Asio or libcurl type in the header), `tests/support/loopback_server.cpp` (scripted HTTP/1.1 server run as a separate process, so its counters are an oracle that shares no code with the client and the client is the only source of threads in the test process), `tests/support/h2c_flood_server.mjs` (cleartext HTTP/2), `tests/transport_properties_test.cpp`.

| Experiment | Property or risk | Oracle |
|---|---|---|
| exactly one outcome under 2000 randomized cancel, deadline, handle-drop and completion races | `NoTerminalNoSuccess` (ExactlyOneOutcome), `OwnershipAndBounds` | per-operation outcome counter and a late-callback counter |
| cancel and deadline in each waiting state: DNS (injected stuck resolver), TCP connect (listener with a full accept queue), TLS handshake (silent peer), response header wait, partial body then stall | `CancelWithoutPeerProgress` | the scripted peer sends nothing after the state is reached; the operation must still end; server-side counters confirm the state |
| abnormal body ends: truncated chunked, short Content-Length, close-delimited, RST mid-body, oversize headers | `NoTerminalNoSuccess` at the transport | status and failure kind; HTTP 500 is a `Completed` transport result |
| dead reused connection | one-attempt rule (DESIGN section 7) | server-observed request count |
| pause and resume with a refusing consumer, and with a slow bounded-queue consumer | backpressure, no lost wakeup | server bytes written plateau, client RSS, completeness after resume |
| K = 4T + 64 and 2K held streams plus a short call | `AdmissionIndependentOfHeldStreams` | `/proc/self/status` thread count and the short call latency |
| 72 concurrent lookups, same host and distinct hosts, with a counting resolver and with real `getaddrinfo` | risk R2 | resolver call count, resolver concurrency, process thread count |
| callback exception, `join()` on an I/O thread, `Transport` destruction with live operations and with a stuck lookup, handle destroyed after the transport | `OwnershipAndBounds` | outcome counts; ASan |
| the review regressions: framing from effective message boundaries (transfer coding, overflow Content-Length, obs-fold), PUT/PATCH/DELETE bodies, ambient proxy variables, a 1xx block then a stall, an error thrown while `resume()` redelivers, pause on a reused connection, `Transport` destroyed from its own callback, a stuck lookup behind many cancelled requests | `NoTerminalNoSuccess`, `OwnershipAndBounds`, `CancelWithoutPeerProgress`, backpressure | status and framing; server-seen body length; RSS; outcome counts |

## 5. M1 results `[live observation]`

All 27 tests pass in the plain, ASan+UBSan and TSan builds (`ctest` in `build/`, `build-asan/`, `build-tsan/`; each run takes about 17 s; TSan needs `setarch -R` on this kernel, wired into CMake). One RSS assertion is not made under ASan because its quarantine keeps freed memory resident; the plain and TSan builds carry it.

Findings that changed the design or the code, in the order they appeared:

1. **Cancel and deadline work in every scripted waiting state without peer progress.** Cancel completed in 0.1 to 2.6 ms in the plain and ASan builds (44.5 ms once in the TSan build for the TLS stall); deadlines fired 0 to 4 ms late. The DNS state became testable only because the resolver is injectable.
2. **libcurl does try to resend on a fresh connection** when a reused connection dies before any response byte: the prerequisite callback fired twice for one operation (`transport_internal_resends=1`). Refusing the second prerequisite point (`CURLOPT_PREREQFUNCTION`, abort) leaves the server with exactly one request and reports `FailureKind::ResendRefused`. The first write's connection reuse is recorded because that is what retry safety needs.
3. **Backpressure through `curl_easy_pause` does not work with `multi_socket` unless the application helps.** In libcurl 8.5.0 a connected socket always keeps read interest (`cf_socket_adjust_pollset` adds `POLL_IN` unconditionally, `[read source]` lib/cf-socket.c), and `curl_multi_socket_action(fd, CSELECT_IN)` makes libcurl read and buffer into its pause buffer even while the transfer is paused (`[read source]` lib/transfer.c, the `conn->cselect_bits` path skips the paused check). Measured: a paused client received all 64 MiB (server wrote 67,108,864 B, client RSS +66 MB). The same pause under `curl_multi_perform` and `curl_multi_poll` does stop reading, which is why a polling loop hides the problem. The transport therefore stops reporting readability for the socket of a paused HTTP/1.x transfer. After that: the server's writes plateau at 2.2 to 3.2 MB (kernel buffers), client RSS grows by 0.25 to 3.7 MB, and `resume()` drains all 64 MiB. A slow consumer with a 128 KiB bounded queue passed 5 of 5 repetitions with bounded occupancy. This is HTTP/1.x only, because on HTTP/2 a paused stream shares its socket with live ones (risk R3).
4. **HTTP/2, single stream, cleartext:** a paused stream grew client RSS by 0.3 to 0.5 MB and the server could hand only 3.3 to 6.7 MB to its socket before blocking. This is one stream against a Node server; a paused stream next to live streams, TLS, and a real vendor server are not measured.
5. **libcurl's threaded resolver starts one thread per concurrent lookup:** 8 lookups gave +8 threads and 72 distinct names gave +72 (measured with the stock resolver before the change). That breaks the "library threads independent of concurrency" gate for a cold start against several hosts. The transport now resolves names itself (single-flight per host, TTL cache, 2 resolver threads, injectable function) and hands the addresses to libcurl with `CURLOPT_RESOLVE`. Measured after: 72 operations on one host cause 1 lookup; 72 distinct names peak at 4 library threads (2 I/O + 2 resolver); a cached name is not looked up again and is refreshed after the TTL; 72 real `getaddrinfo` failures stay bounded. A stuck lookup delays only other uncached hosts, never cancel or deadline, and never blocks `Transport` destruction.
6. **libcurl reports the connect time only after TLS completes**, so a stall in the TCP connect and a stall in the TLS handshake are indistinguishable from timing marks (both `Stage::Resolved`). `Stage::RequestStarted` is exact for the question that matters: below it, nothing was written (retry safety `NotSent`).
7. **`STARTTRANSFER_TIME` is set even when the peer closes without any byte**, and the header parser's state resets after a 1xx block, so the response start is a separate monotonic flag set by the first status line. The request-may-have-left mark is latched by the prerequisite callback, which fires before any request byte is written; the pretransfer timing is only a second witness.
8. **AddressSanitizer found a real use-after-free** (an `asio::steady_timer` inside the operation state destroyed after the transport's `io_context`, reachable by dropping an `Operation` handle after the `Transport`). Fixed by destroying the timer on the strand when the operation finishes; `shutdown_cancels_active_operations` is its regression test.
9. **Admission:** with 2 I/O threads, 72 and then 144 held streams left the process thread count unchanged at baseline + 2 (the I/O threads), and a short call completed in 0.8 to 1.2 ms.

Independent review. A read-only review by a model of a different family (Codex, `TransportReview`) read the transport and libcurl 8.5.0 sources and returned 11 confirmed and 3 suspected findings. It was verified, not trusted: every fix below has a regression test except the `join()` snapshot and the two association paths of finding 16, and for the one-line behaviour changes the test was run against a mutant of the fix on a scratch copy (a `sed` edit of the fix, rebuilt, the single test run; the fix of finding 16 is the one that survived).

10. **Framing was derived from header presence.** A transfer coding that is not a final `chunked`, an overflowing Content-Length, and a line folded under another field (`X-Note: a` then ` Content-Length: 5`) all made the transport report `ContentLength` for a response libcurl treats as close-delimited. Mutated back, the tests show `/cl-overflow -> Completed/None` and `/folded-cl -> Completed/None`, a truncated body reported as success. Framing is now derived at the end of the header block from the unfolded fields (`derive_framing`), and anything ambiguous is close-delimited, which is never a normal end.
11. **Ambient proxy variables bypassed the bounded resolver.** libcurl resolves a proxy host itself, with its own resolver threads, and removing a handle waits for a pending threaded lookup. The transport now sets an empty `CURLOPT_PROXY`, which also disables environment proxies. Mutated back, a request with `http_proxy` set fails with `Failed/Connect`. Proxy support is explicitly not provided.
12. **An error thrown while `resume()` redelivers retained bytes was dropped.** libcurl returns it only from `curl_easy_pause()`. Mutated to ignore the return value, the operation completes as `Completed/None` after `on_body` threw. `resume()` now routes a non-OK result to the completion path.
13. **Destroying the `Transport` from its own callback deadlocked** (the destructor waited on the strand it was running on). The destructor now detects an I/O thread and hands the teardown to a detached reaper that owns the core until the threads are joined. Mutated back, the test times out at the 3 s guard.
14. **Cancelled operations stayed referenced by a stuck lookup.** `finish()` now removes the operation from the pending list while an in-flight marker keeps lookups single-flight. Mutated back, 200 cancelled 1 MiB requests behind one stuck lookup grew RSS by 212 MB; fixed, 2.4 MB.
15. **Smaller confirmed defects:** PUT, PATCH and DELETE bodies were silently dropped (now sent; GET and HEAD with a body are rejected; the mutant sends no body and the server sees length 0); a complete `100 Continue` block made progress move backwards to `RequestStarted` (the mutant reports `RequestStarted`); `request_header_bytes` included an inline POST body, so the field was removed and only the uploaded body bytes are reported (a 1 MiB upload reports 1,048,576); `join()` read the handle's state after blocking (it now snapshots the state, and the header states that one `Operation` object is not safe for concurrent mutation, like `std::thread`; no test).
16. **The pause association now follows the connected socket.** It was frozen at the first socket libcurl announced and never cleared on REMOVE, which can misattribute a pause when a connect race leaves a different socket carrying the response, or when a descriptor number is reused. It is now updated whenever read interest is announced and cleared on REMOVE. **Not killed by any test:** on loopback a refused connect fails inside `connect()`, so libcurl never announces a socket for the dead candidate and the race cannot be reproduced; the sharpened tests still pass with the fix mutated away. The same holds for the reconcile line that enrolls a pause requested before the socket was known (that ordering did not occur in the tests). Both are fixed by construction and remain untested.
17. **Not changed, decided:** libcurl calls in `Transport::start()` (building the private easy handle) stay inline and the multi handle is still created before the I/O threads start; the file header now says so instead of claiming "never inline", because an easy handle that has not been added is not shared state.

Not covered by M1 (each is a named item below, never implied done): TLS success and certificate verification, HTTP/2 over TLS, real vendor endpoints, Windows and macOS, WebSocket, proxy support (it is disabled, not tested), throughput sharding of the single curl multi handle, the deterministic executor seam of CONFORMANCE 5.24, exhaustive enumeration of interleavings (the race test is randomized, not exhaustive), a send failure at the very start of the request (the `NotSent` latch is not exercised), and the two untested association paths of finding 16.

### 5.1 M1b results `[live observation]`

Five added transport property tests bring the total to **32, all passing under plain, ASan+UBSan and TSan**. Full-suite runs took 19.32 s, 19.96 s and 21.45 s respectively. The M1 ASan quarantine exception remains unchanged; no M1b assertion is skipped. No production transport change was needed for these scenarios.

The new independent peers are `tests/support/tls_test_server.mjs` and `tests/support/h2_adversarial_server.mjs`. They use Node built-ins, loopback sockets and an OpenSSL CLI; no npm packages, model runtime, credentials or vendor call. TLS certificates and private keys are generated in a private temporary directory and removed when the fixture exits. Tested fixture tools: Node 22.14.0 and OpenSSL 3.0.13.

| Scenario | Observed result |
|---|---|
| Trusted CA, forced HTTP/1.1 and automatic HTTP/2 ALPN | Two successful requests per protocol, second connection reused, binary body intact; peer counters prove 2 HTTP/1.1 and 2 HTTP/2 requests. |
| Wrong host, expired certificate, self-signed certificate | `Failed/Tls`, curl code 60, no request body sent, no HTTP request reached an invalid peer. |
| Missing CA file; system trust after warming a custom-CA connection | `Failed/Tls` (missing file: code 77); no HTTP request sent. Connection reuse did not leak custom-CA trust into a system-trust-only request. |
| One paused 64 MiB stream plus three active 4 MiB siblings | One peer session for all streams; all siblings completed while the first stayed paused. Server writes plateaued at 10,551,296 B; plain client RSS grew 4,444,160 B. Resume delivered all 64 MiB exactly once. Both write-buffer and RSS assertions stay below 32 MiB in this scenario. |
| Cancel one paused stream while a sibling is paused | Cancellation completed without peer progress; resuming the sibling delivered all its 4 MiB on the same connection, with one outcome each. |
| GOAWAY below the current stream ID; REFUSED_STREAM | Each prompted one libcurl internal resend attempt, refused at the prerequisite callback: `Failed/ResendRefused`, code 42. The independent peer saw exactly one request for each operation. |
| Expired resolver cache, changed address, idle connection retirement | With TTL zero, three calls caused three lookups. The second reused the healthy connection to peer A despite the new answer; after the peer closed that idle connection, a fresh connection reached peer B at the new address with no resend. DNS TTL is not a connection lifetime limit. |

The GOAWAY oracle initially passed zero to Node's API, which actually emitted the current stream ID (3), not wire zero; that correctly left the accepted request waiting until its deadline. A separate Node client observed the frame. The corrected oracle warms a connection and sends last-stream ID 1 for request stream 3; the same observer confirmed that frame before the final runs. Idle-session retirement explicitly closes the socket: a connection cache need not watch an idle socket or immediately reciprocate a graceful GOAWAY.

Limits remain explicit: HTTP/2 multiplexing/backpressure was measured with cleartext HTTP/2, while TLS/ALPN was exercised separately. One paused stream plus three siblings is not a universal memory bound for arbitrary stream counts, peers or libcurl versions. Real provider behavior, public OS trust stores, Windows/macOS, proxies and WebSocket are not verified here. Linux/POSIX is the PoC platform (R5); D1b stays deferred until before ROADMAP Stage 2 (R6), not implemented or silently replaced with a second HTTP stack.

## 6. Risk register

| Id | Risk | State | Next step |
|---|---|---|---|
| R1 | libcurl resends silently on a fresh connection | closed on tested libcurl for HTTP/1.x and HTTP/2 GOAWAY/REFUSED_STREAM | re-run the one-request oracle for every supported libcurl version |
| R2 | threaded resolver threads scale with lookups; stale addresses in curl's cache | bounded resolver tested; HTTP/2 fresh connection uses refreshed addresses after idle retirement | healthy pooled connections can outlive DNS TTL; changing that policy requires a separate decision |
| R3 | HTTP/2 pause buffers inside libcurl while the socket is shared | bounded in the one-paused/three-active h2c scenario; resume and cancellation isolation pass | remeasure TLS multiplexing, other peers, stream counts and libcurl versions before claiming a general bound |
| R4 | TLS trust and verification | custom-CA success, ALPN and certificate rejection matrix pass on OpenSSL 3.0.13/libcurl 8.5.0 | public OS trust stores and other TLS backends/platforms remain unmeasured |
| R5 | POSIX descriptor model (`asio::posix::stream_descriptor`) | explicit PoC limit: Linux only | Windows socket path and macOS verification are later work, not M1b blockers |
| R6 | WebSocket for the Responses lane | undecided (D1b): libcurl WebSocket is official from 8.11 and the CVE fix is in 8.16, while the distribution libcurl here is 8.5.0 | decide before ROADMAP Stage 2, not before Stage 1 |
| R7 | single `CURLM` on one strand limits CPU throughput | unmeasured at scale; the earlier A/B (one worker thread polling, not this integration) had libcurl within about 4% of the Asio pool at 128 concurrent POSTs | measure at M4 with the real codec cost; shard the multi handle only if the number says so |
| R8 | resolver pool threads stuck in `getaddrinfo` delay uncached hosts | accepted and bounded by pool size | document; the deadline still ends every operation |

## 7. Open decisions and defaults

| Id | Question | Default used by the PoC | Who decides |
|---|---|---|---|
| C1 | libcurl floor and required features | 7.88.0 (Debian 12) as the floor, 8.5.0 as the tested version; TLS and HTTP/2 required, a threaded or async resolver is irrelevant now that the transport resolves names | owner, after M1b |
| C2 | first platforms | Linux; Windows only after R5 | owner |
| C3 | live call budget | none authorized; no live call until the owner sets a cap on calls and cost per cell | owner, before M5 |
| C4 | target concurrency | scenarios C = 1, 32, 128 and held-stream counts of 4T + 64 and twice that, reported as measurements and never as promised capacity | owner |
| C5 | libcurl deployment | system libcurl first; vendoring only if R6 forces a newer version | owner |

## 8. How to build and run

```
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
cmake -S . -B build-asan -G Ninja -DSP_SANITIZE=address && cmake --build build-asan && ctest --test-dir build-asan
cmake -S . -B build-tsan -G Ninja -DSP_SANITIZE=thread  && cmake --build build-tsan && ctest --test-dir build-tsan
```

Requirements: Linux, C++20 compiler, libcurl 7.88 or newer with TLS and HTTP/2, standalone Asio headers (`libasio-dev` or `-DASIO_ROOT=`), `node` and the `openssl` CLI. CMake requires both fixture executables when tests are enabled; M1b peer startup failure fails the test. A single scenario: `build/sp_transport_tests build/sp_loopback_server tests/support <name-substring>`; `http2_` exercises the HTTP/2 scenarios, including TLS/ALPN. No local inference server or API key is required.

## 9. Working rules

One branch (`poc/curl-asio-transport`), no push, no merge. Every finding is written into this plan or a decision record with the run that produced it; a result that was not run is labelled `[INFERENCE]`. A property test that fails is a finding to record, never a threshold to adjust. Secrets never appear in test output or in `Result::detail`.
