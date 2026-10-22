# Replay of recorded provider responses

In the default SDK comparison (`../README.md`), the mock TLS peer answers immediately with a synthetic buffered JSON body or a 50-chunk SSE stream. That isolates pure client CPU and throughput against a fast local peer, but in streaming mode all 50 SSE events can coalesce into one or two socket reads, and at concurrency 8 and above the loopback peer itself saturates its two CPU cores.

Replay mode (`sdkbench.py run --workload workload-replay.json --replay replay/recordings/<dir>`) keeps the same client programs, TLS setup, request validation, and connection-reuse gates, while replacing the synthetic response generator in `tools/benchmark_peer.mjs` with a byte-for-byte, pace-for-pace replay of real provider responses recorded ahead of time.

## Recording format (`record.mjs`)

`node replay/record.mjs --api openai|anthropic --mode buffered|stream --model ID --text FILE --out FILE [--count 30] [--max-attempts 45] [--pause-ms 1000] [--reasoning-effort none]` sends real requests over HTTPS (`http/1.1`, `accept-encoding: identity`) to OpenAI Chat Completions (`/v1/chat/completions`) or Anthropic Messages (`/v1/messages`), buffered or streamed, and writes one JSON recording file per `(api, mode)` containing the raw response bytes and microsecond timestamps from the moment the request was handed to the TLS socket:

- `head_us`: elapsed microseconds when the response status and headers arrived.
- `chunks`: array of `[offset_us, base64_bytes]` for each socket `data` event of the HTTP response body (Node's HTTP parser strips transfer-encoding framing before emitting `data`; the peer re-applies `Transfer-Encoding: chunked` or `Content-Length` when serving).
- `deltas`: number of non-empty text deltas in a streamed response (`0` for buffered responses).
- `body_bytes`: total unchunked response body bytes.

Only an allowlist of response headers (`content-type`, `content-length`, `cache-control`, `request-id`, `x-request-id`, `openai-processing-ms`, `openai-version`) is kept; organization headers are dropped, and `request-id` / `x-request-id` headers as well as `chatcmpl-*`, `msg_*`, and `req_*` identifiers in the body are replaced with same-length synthetic placeholders (`chatcmpl-bench0...`, `msg_bench0...`, `req_bench0...`) so piece byte lengths stay unchanged. Each request prompts the model to copy `reply.txt` (a fixed 1,161-byte English prose passage) verbatim; any response whose decoded text differs from `reply.txt` in any byte is rejected at recording time so every client verifies the exact text after the request clock stops.

## Replay pacing in `benchmark_peer.mjs`

When `sdkbench.py` runs with `--replay <dir>`, it passes the cell's recording file (`openai-buffered.json`, `openai-stream.json`, `anthropic-buffered.json`, or `anthropic-stream.json`) to `tools/benchmark_peer.mjs` as `replay_file` in `--config file.json`. Each valid request claims the next recording in round-robin order (`seq % N`):

1. The peer schedules the response header at `t0 + head_us` and each body piece `chunks[k]` at `t0 + chunks[k][0]`, where `t0` is the microsecond timestamp when the peer finished reading the client's HTTP request.
2. One timer per in-flight response wakes at the next scheduled offset and writes all pieces whose scheduled time has arrived, avoiding a queue of dozens of timers per stream.
3. For each completed response inside the measured window, the peer accumulates:
   - `scheduled_us`: `chunks[-1][0]` (the recorded time from request completion to the last body piece).
   - `late_us`: `max(0, (actual_finish_us - t0) - chunks[-1][0])` (how far behind schedule the peer finished sending the last piece).

Because the streamed recordings have varying delta counts (`deltas` differs from recording to recording depending on how the provider chunked its SSE stream), `sdkbench.py` passes `--chunks 0` in replay mode so clients still verify that `deltas.join("") == expected_text` without requiring a fixed 50-chunk count.

## Definitions in `tables.md` (`render_replay.py`)

`python3 render.py <session_dir>` delegates to `replay/render_replay.py` when `session.json` contains a `replay` provenance block:

- **CPU per request (µs):** `(cpu_user_us + cpu_sys_us) / ok` over the measured window, median across valid repetitions.
- **Residual (ms):** `mean(client_latency_ms) - (scheduled_mean_us + late_mean_us) / 1000`. Because the peer and the client observe the exact same set of requests in the measured window, the difference of means is exact: it is the average time the client (and loopback socket exchange) added on top of the time the peer actually took to send the responses.
- **p50 latency (ms):** median client request latency across all samples of the valid repetitions (dominated by the 2–3 s recorded provider duration).
- **Peer lag (µs):** `late_mean_us`, the mean timer lag of the peer at the end of each response. Cells exceeding `2,000 µs` are marked `⚠`.
