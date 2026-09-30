# D1 — Transport

- Status: GATED (default A), 2026-10-01
- Deciders: maintainer + two-family review panel
- Nothing here is implemented; this is a design decision for a proposed library.

## Context
The library needs HTTP/1.1 + SSE for buffered and streaming calls and a WebSocket lane for OpenAI Responses. Candidates: (A) one private transport on standalone Asio + OpenSSL, (B) libcurl as the single stack, (C) hybrid: Asio for WebSocket and libcurl for HTTP.

## Decision
Default **A**: one private transport built on the standalone Asio + OpenSSL code that NeoGraph already has (ConnPool over HTTP/1.1, SSE through the async streaming path `async_post_stream` plus `SseEventParser`, the native `WsClient`), extracted into the library's private `transport/`.
- The library does not use httplib or libcurl.
- No runtime backend flag. No `prefer_libcurl` option, no stub translation unit legacy.
- **C is rejected as a permanent state**: it would keep two TLS/socket/cancel families alive.
- The one-attempt contract counts any transport-internal automatic resend as an attempt. Oracle: server-observed application request count <= 1 by default.

## Evidence
- [read source] NeoGraph's MCP and A2A clients already use the Asio HTTP layer (`src/mcp/client.cpp`, `src/a2a/client.cpp` include `neograph/async/http_client.h`).
- [read docs] libcurl WebSocket support needs a recent libcurl (8.11 official; CVE-2025-10148 fixed in 8.16). [read source/inference] Ubuntu 24.04 ships 8.5.0 and Debian 12 ships 7.88.1, so a libcurl stack implies vendoring (packaging gap).
- [read source] NeoGraph's own documentation (its HTTP/2 transport example in the Python bindings) records libcurl HTTP/2 as **about 25% slower at p50** than the HTTP/1.1 pool. `prefer_libcurl` defaults to false.
- [live observation] Existing PASS logs cover ConnPool/WS cancel, chunked-SSE loopback and deadline behaviour. Transport parity for vendor cells is NOT yet measured for the new library.
- Caveats of A: the current code uses OpenSSL default verify paths only (an OS trust-store story is needed); no proxy support; no HTTP/2.

## Options considered
- B libcurl single stack: rejected as default because of the WS version gap, the measured HTTP/2 slowdown and the CURL-specific cancel semantics; remains the flip target.
- C Asio WS + curl HTTP: rejected (two TLS/socket/cancel families, double the cancel/ownership proof).
- httplib: rejected (no streaming/cancel fit for the async core, see D3).

## Consequences
- Library owns connection pooling, SSE framing and WS framing; must prove cancel and deadline behaviour itself (D3 properties).
- No enterprise proxy and no HTTP/2 in the first release.
- Windows/macOS trust stores are out of the first-release matrix unless the flip (2) fires.

## Flip conditions (GATED) — flip to B (libcurl single stack, never the hybrid C) if ANY
1. Live transport parity fails persistently for a required direct-vendor HTTP/1.1 SSE cell or the OpenAI WSS cell: 60 runs per cell (rule of three), counting transport-caused failures only (WAF/ALPN rejection, connection limits, TLS verification).
2. Windows/macOS become declared first-release cells and adding OS trust to the OpenSSL store exceeds about 150 LOC or fails the trust matrix (expired, wrong host, self-signed must be rejected).
3. Enterprise HTTP CONNECT proxy becomes a first-release requirement.
4. The owner confirms HTTP/2 (multiplexing, session count, WAF fingerprint) as a first-release requirement.

## What would falsify it
A persistent transport-caused failure in the 60-run cells; a trust-matrix failure; evidence that the libcurl HTTP/2 slowdown does not hold for vendor workloads (then the performance argument disappears, the packaging and cancel arguments remain).

## Enforcement
Properties `RetrySafetyBudgetDeadline`, `CancelWithoutPeerProgress`, `OwnershipAndBounds`, `TransportProjectionParity`, `InstallAndDependencyDAG`; CI gate: include-direction check (no httplib/libcurl header reachable from public headers).

## Open items
- **OWNER CONFIRMATION REQUIRED**: choosing A retires NeoGraph's existing opt-in HTTP/2 capability (`CurlH2Pool`). That is a capability removal the owner must approve explicitly; until then it is an open owner question attached to flip (4). Separately, whether NeoGraph removes `NEOGRAPH_USE_LIBCURL` afterwards is a NeoGraph decision independent of this library.
- OS trust-store design for A.
