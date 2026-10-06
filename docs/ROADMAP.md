# Roadmap and release gates

Current package0.1.0 alpha uses typed interface/shared-library generation4 with five HTTP/SSE families and the NeoGraph adapter. [Usage](USAGE.md) defines new preserved controls, Responses provider-held cursors and explicit Generate portable imports. [Recorded provenance](POC_PLAN.md#current-typed-c-cutover-provenance) keeps previous interface3 runs unchanged; implementation, fresh wire proof and release/support admission are separate claims.

## Owner-requested next work

The heading retains links from the original work sequence. These C++ items are implemented, not a pending plan:

| Item | Current state and evidence |
|---|---|
| Caller output cap | Typed caller/default selection; incompatible model/resource facts reject rather than silently clamp. Historical CLI proof carried 16384 unchanged in 38 model-free peer requests. [Policy/control contract](USAGE.md#request-families-and-controls). |
| Closed external JSON | Immutable runtime/error and descriptor/codec snapshots with explicit bounded loaders. JSON cannot issue financial or native authority. [Inventory](USAGE.md#descriptors-credentials-and-defaults). |
| HTTP/1.1, HTTP/2, optional HTTP/3 | One libcurl/Asio transport. Capable Linux HTTP/3 scenarios and incapable-build rejection have recorded evidence; exact backends/instrumentation/platform limits remain in [D1](decisions/D1-transport.md#evidence). |
| SDK benchmark | Model-free 58-configuration/174-fresh-process cohort after full raw retention. [Records](../benchmarks/current-sdk-benchmark-results.json), [summary](../benchmarks/current-sdk-summary.json), [recipe](#model-free-benchmark-recipe). No H3 performance or model-inference claim. |
| Typed NeoGraph cutover | Owned native-capable requests/outcomes, prepare-before-effect, linked-interface gate and host receipt/custody boundary; obsolete provider/interpreter paths removed. [Migration](MIGRATION.md) and [integrated evidence](POC_PLAN.md#current-typed-c-cutover-provenance). |
| Post-cutover graph benchmark | Separate final 16-configuration/48-process ReleaseGraph cohort with zero recorded failures. NeoGraph owns its result/summary artifacts; SDK-only numbers are not substituted for graph runs. |

NeoGraph owns its Python binding migration and package verification. This SDK does not ship an independent Python package. Do not use the old “Python after C++ stabilization” proposal as a current NeoGraph implementation restriction.

The preservation cutover adds Chat reasoning/usage/alternative-model controls, Messages mode/output/cache/tool controls, Generate thinking/safety/tool/sampling controls and explicit portable foreign history, Responses cursor/new-input ownership and detail controls, case-insensitive model temperature facts, and host environment-header preprocessing. Parent owns interface4 qualification; earlier interface3 passes/benchmarks are not new passes. `spna3` storage format remains3 while policy/control identity changes can reject old carry.

## Historical release ladder

The original stages explain the order of design/experiments, not today's inventory or a claim that all stage gates passed. M0–M5 and later family/native/control cohorts remain in [POC_PLAN](POC_PLAN.md), including M5's strict lossless **old consumer ABI NO-GO**. The typed cutover replaces that ABI; it does not rewrite the finding.

| Original stage | What exists now | What it did not establish |
|---|---|---|
| 0: foundations | Strict JSON/descriptor admission, fixture runner, shared accumulator, runtime scheduling seam and explicit decision records. | A generic descriptor-constraints engine, generated full-target schema or every proposed mutant/dependency gate. |
| 1: Chat and Messages | Typed buffered/SSE encoders/codecs/runtime; recorded semantic, ownership and bounded live cohorts. | Nightly canary cadence, every model/operator or statistical equivalence. |
| 2: Responses/OpenRouter | Responses HTTP/SSE, typed controls/native carry, and interface4 provider-held cursor state with new input and private tool ownership. | WebSocket, automatic full transcript retrieval, unrestricted gateway/model qualification or native authority from cursor strings. |
| 3: Gemini/Interactions | Native Generate and model-only stateless Interactions HTTP/SSE; local and bounded paid observations. | Agents/environments, arbitrary server tools, native-signature consumption or universal vision correctness. |
| 4: NeoGraph cutover | Typed adapter, full outcome preservation and explicit archive/host custody integration; no legacy interpreter shim. | Automatic migration of old native-ineligible records, mid-tool-loop origin switching or cross-platform stable ABI. |

## Remaining release and support gates

A release manifest must state the actual package version, interface revision, toolchain/dependency set and exercised platform/cells. A shared-library generation is not a stable cross-compiler ABI, and “all five families implemented” is not “every hosted model passed.” Use [CONFORMANCE](CONFORMANCE.md) for behavioral acceptance and explicit unsupported/error boundaries.

Current executed runtime/custody evidence of this SDK's own suite is Linux/POSIX scoped; that suite uses `fork` and `unistd.h` and does not build on Windows. The provider-construction gate `NativeSocketRuntime` is granted wherever the transport has a native socket implementation: POSIX descriptors (Linux, macOS) and Winsock event objects (Windows). Windows has been executed only through NeoGraph's native-MSVC integration suite, and macOS/ARM64 has not been re-run for this revision; source changes alone do not qualify those platforms. Neither platform gets silently advertised as qualified or removed from another product's package metadata on the strength of this SDK note.

[Current interface4 local execution](CONFORMANCE.md#interface-4-execution-record) records all27 cases covered by25 initial and2 corrected focused passes, plus unchanged installed Stop and exact README consumer observations. It is not a second full-suite run, hosted/platform qualification or a replacement for earlier interface3 benchmarks.

HTTP/3 remains optional. Baseline builds must work without QUIC dependencies; capable claims require actual negotiated QUIC, one-request connection-stage fallback, unchanged deadline/cancel/retry safety, reset/truncation and shared-stream bounds. [D1](decisions/D1-transport.md) separates capable proof from uninstrumented release-backend coverage and hosted-provider support.

The paid baseline recorded 293/300 pairs: Chat/Responses/Messages 60/60 each, Generate 56/60, Interactions 57/60. Google mutation/omission acceptance and generic aggregate-omission errors do not prove cryptographic validation or native consumption. [Qualification summary](../config/qualification-final-summary.json) and [native-axis report](../config/qualification-native-axis-results.json) retain failures/unavailable/inconclusive rows. The fixed campaign has no remaining authorized requests; a release gate is not permission to run it again.

Remote mixed-gRPC TSan limits belong to the NeoGraph integrated cohort and are not a race-freedom claim. Earlier SDK, sanitizer and installed-consumer observations retain their original dates/counts; future verification must record a new cohort rather than relabel old outputs current.

## Explicitly outside the current SDK

- Responses WebSocket, automatic server-state transcript retrieval/backfill and transparent cursor-failure resend; implemented previous_response_id state is a distinct bounded path.
- Native Bedrock/Vertex authentication: SigV4, OAuth refresh and binary event framing.
- Cross-origin native equivalence and general Drop/Demote treatments; Generate's explicit unsealed PortableForeign/sentinel path is implemented separately and never repairs native carry.
- Generic descriptor constraints/model-selector/options programs and full target schema generation.
- Standalone image, Veo and Decisions endpoints; NeoGraph retains separate typed clients.
- An independent SDK Python package, graph runtime, tool executor or financial budget authority.
- Qualified enterprise proxy and broader platform/TLS-backend matrices.

## Model-free benchmark recipe

These commands are a recipe, not a fresh result. Run from the SDK root, with Node.js and OpenSSL CLI available and `YYJSON_ROOT` pointing to an existing dependency:

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DSP_BUILD_TESTS=OFF -DSP_BUILD_CANARY=OFF -DSP_BUILD_BENCHMARKS=ON \
  -DYYJSON_ROOT="$YYJSON_ROOT"
cmake --build build-bench --target sp_provider_benchmark
python3 -c 'import json; d=json.load(open("benchmarks/matrix.json")); print(*(d["same_workload_baseline_configs"]+d["extended_runtime_configs"]+d["local_encode_configs"]), sep="\n")' |
while IFS= read -r config; do
  build-bench/benchmarks/sp_provider_benchmark "$config" || exit "$?"
done
```

The [matrix](../benchmarks/matrix.json) is the workload authority. Compare equivalent Release workloads/settings and record actual protocol counts, owned-outcome checks, latency units, RSS and threads. Synthetic local signatures prove retention only; local transport throughput is not model generation speed.
