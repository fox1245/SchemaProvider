# SchemaProvider

A multi-vendor LLM client library for C++20.

> **Status: exercised installed C++20 SDK, unstable version0.0.0/interface3/SOV3; actual integrated typed lossless C++ proof recorded with explicit limits below. Not stable or released.**
> Five typed HTTP/SSE families are implemented. Latest SDK26/26 passed with zero failures in74.07seconds. Core2242-test full run had zero failures and16 documented skips; genuine shared-bank/recorded-control, installed Program/C ABI/dualQuickJS and typed native/raw/lifetime consumers were exercised. Local TSan seven scopes passed, but full mixed gRPC/system-dependency TSan exited66 with402 race warnings; remote TSan/race-freedom is not claimed. Paid baseline293/300 pairs is not universal qualification; detailed observations, meter and native-axis limits appear below.
> M1/M1b transport verification: 32 tests pass under plain, ASan+UBSan and TSan on Linux, including TLS certificate rejection and HTTP/2 multiplexing. Validation targets hosted APIs first; Ollama/llama.cpp setup is excluded. No live provider compatibility is claimed by these model-free loopback tests.
> M3 verification: 62 independently reviewed Messages fixtures, 30,430 partition variants and ten buffered/SSE parity pairs; both client tool-result and server-pause continuation traverse real loopback HTTP. Twenty-two replay mutations are rejected before dispatch. Existing Chat coverage remains 35 fixtures, 9,409 partitions and five parity pairs. These synthetic capsules prove local retention and rejection, not vendor signature validation.
> M4 verification: move-only operations, immutable owned results, stop/deadline handling, bounded queues and default-off retry. RAII covers normal and exceptional ownership. Evidence includes 298 deterministic schedules, 88/176 held-stream admission and exact 8 MiB delivery through a 32 KiB queue.
> Historical M5 verification: **14/14 CTest groups passed under plain, ASan+UBSan and TSan**. One live run passed four OpenAI GPT-4.1 Mini assertions and five Anthropic Haiku 4.5 assertions, including a retained positive thinking replay and a dispatched one-byte-signature negative rejected by the provider. This was N=1, not release/equivalence admission. The 97-fixture legacy comparison left 57 expected-fail candidates and 40 unsupported cases: the **old consumer ABI was strict lossless NO-GO**. The approved typed contract replaces that ABI; it does not retroactively change M5's result.
> Gemini compatibility smoke: **2/2 bounded text assertions pass** on `gemini-2.5-flash-lite` through Google's OpenAI-compatible Chat Completions endpoint, reusing the existing runtime/Chat codec. This confirms the selected key/route for that buffered/SSE pair, **not native Gemini, thinking-signature, tool-replay or broad vendor support**. Scope and accounting: [POC_PLAN section 5.6](docs/POC_PLAN.md#56-cheap-gemini-compatibility-smoke-live-observation).
> Earlier Responses reasoning PoC: **16/16 groups passed** under plain, ASan+UBSan and TSan. `gpt-5-nano-2025-08-07` buffered tool continuation retained encrypted reasoning and rejected a one-byte ciphertext control (**N=1 ReplayVerified**); omission was accepted. The original **5/6** and two separately authorized SSE diagnostic/confirmation calls remain separate evidence: **8 calls / US$0.186216 reserved**, not an invoice. That campaign did not verify live SSE tool replay or visible summaries; the newer GPT-6 Luna experiment below does. [POC_PLAN5.7](docs/POC_PLAN.md#57-responses-reasoning-poc-live-observation).
> Initial five-API vision/reasoning probe:23/23 model-free groups passed and40/40 calls reserved US$7.795635. Actual vision/thinking ran on all five families; GPT-6 Luna Responses passed8/8 with N=1 ciphertext proof. Interactions tool replay and further vision-signature controls were not yet observed at that cap. This remains the historical [POC_PLAN5.8](docs/POC_PLAN.md#58-five-api-vision-and-reasoning-live-observation) record, not a new all-family pass.
> Remaining native controls and fresh execution: **24/24 groups pass** under plain, ASan+UBSan and TSan. Actual Interactions SSE tool replay now passes with exact native retention; Messages and Responses have N=1 `ReplayVerified`. Gemini/Interactions accepted single-carrier signature mutation and remain `ReplayAcceptanceUnobservable`. Fresh results: Chat6/6, Messages8/8, Gemini8/8, Interactions7/8 (generic omission rejection), Responses low6 passed/2 unavailable, then separate medium8/8. Additional **59/60 calls / US$11.184045 reserved**, cumulative99/100 / US$18.979680. [Exact separate cohorts, fixes and limits](docs/POC_PLAN.md#59-remaining-native-controls-and-fresh-campaign-live-observation).
> **Optional HTTP/3 policy is implemented and capable Linux scenarios pass under plain, ASan+UBSan and TSan.** An isolated curl 8.16/OpenSSL 3.5.3/ngtcp2/nghttp3 build negotiates real HTTP/3 for preference/only, HTTP/2 for single-POST fallback, and preserves cancel/deadline, reset/truncation and same-connection paused-stream isolation. The system curl 8.5 remains a supported non-QUIC build; its only mode rejects before dispatch. Backend dependency libraries are release builds; hosted API evidence is separate. [Exact local observations and limits](docs/decisions/D1-transport.md#evidence).
> Owner-priority work: caller-controlled MAX_TOKEN, bounded HTTP/3 transport, installed shared SDK and the 58-configuration model-free SDK benchmark matrix are exercised. Full external JSON admission, lossless NeoGraph consumer/example/cookbook cutover and before/after graph measurements remain in integrated verification. [Current sequence](docs/ROADMAP.md#owner-requested-next-work).

## What it is meant to be
One C++20 client over the chat API families the major vendors expose (Chat Completions, Responses, Messages, Gemini generate and Interactions), usable on its own and as the LLM layer of NeoGraph. The first release is chat completion plus artifacts that arrive inside chat responses; image-generation endpoints, long-running video operations and the OpenRouter decisions endpoint are not part of it.

## Goals under consideration
- Typed errors carrying status, retry class, retry safety (not sent, possibly accepted, rejected before output, output observed), vendor code and request id, so a provider failure is never returned as a successful completion. A stream that ends without terminal evidence is a failure, never a success.
- Token usage including cached and reasoning tokens, with each vendor's counting rules kept as data and "unknown" kept distinct from zero.
- Per-call request knobs declared per vendor, with a clear error before any network call for a key that is not declared.
- Structure and state transitions live in typed family code. Closed/versioned JSON supplies admitted immutable values, never hooks or a stateful configuration interpreter. External snapshots can be loaded explicitly; embedded defaults remain available.
- One semantic event model and accumulator for supported buffered/SSE paths. Responses WebSocket is excluded from the current SDK and release gate; old WebSocket sketches are future design only.
- Automatic retry off by default; if enabled, in exactly one layer, with a deadline, a shared budget and respect for `Retry-After`.
- A conformance suite built from recorded real vendor request/response pairs and streams, so behaviour learned from the live APIs is a spec that both the current and the new implementation must pass.
- A portable conversation history plus an opaque, origin-bound sidecar for vendor reasoning state (signed or encrypted reasoning cannot be translated between vendors, and is rejected by default when the origin differs). Replay is also checked against binding facts (model, context fingerprint, account scope); origin is a replay boundary, not proof of who issued a block.

## Caller-controlled output tokens

Typed request fields and each canary profile's `max_output_tokens` remain caller choices. `sp_canary --max-output-tokens N` overrides the profile unchanged. The historical8192/128 PoC ceilings and output-versus-input shortcut are removed; the CLI does not clamp the chosen value or raise call/token/money allowances. Genuine provider/model constraints and representation limits remain separate.

An actual five-family model-free CLI smoke supplied16384 over a profile's8192 and input bound10000. All38 peer requests carried16384, all first six positive assertions per family passed, and reservations used10000+16384 tokens per attempt. Native negative/omission fixture outcomes remain separate from this control proof; it is not a live all-canary pass or a financial grant.

## Workload-based qualification cost

`tools/qualification_cost.py` reads the versioned model catalog, workload plan and scalar-only historical usage in `config/`. It does not dispatch requests, authorize spending, refund reservations or reset the canary ledger.

```sh
python3 tools/qualification_cost.py \
  --catalog config/model-catalog.json \
  --plan config/qualification-plan.json \
  --observations config/qualification-observations.json
```

The exercised 2026-10-02 forecast for 60 buffered/SSE pairs per family (600 generation requests) is **US$0.157890**, with **US$0.009414** for 30 Google diagnostic requests: **US$0.167304** combined. It uses 227,460 expected input tokens and 67,560 expected output tokens, including billed thinking—not whole model context windows or output caps. This is a small-sample standard-price forecast, not a bill or statistical qualification result; prices and observations are reproducible in the JSON inputs and `config/qualification-cost-result.json`.

The short baseline assumes no cache discount. GPT-6 Luna and Haiku inputs are below their documented 1,024/4,096 cache minima. Known Interactions tool-control rows report 28.005% input-token reuse (2/10 requests); that does not establish the same reuse for the short baseline. Gemini's missing cache fields remain unknown, not reported zero. Two Gemini off-output counts are explicitly estimated from same-family reported visible output; their original usage remains null. Thinking is already included in output and is never charged twice. Historical `SPCANARY1` monetary records remain conservative exposure reservations, **not invoice costs**.

## Install and use the unstable C++ SDK

Current runtime/archive proof is Linux/POSIX only; no Windows, macOS or WASM runtime qualification is claimed. Build prerequisites are CMake 3.20+, a C++20 compiler, Python 3 for configuration generation, standalone Asio headers, libcurl 7.88+, OpenSSL Crypto and yyjson (installed library or explicitly supplied source tree). Configuration does not download dependencies.

```sh
cmake -S . -B build-sdk -DSP_BUILD_TESTS=OFF \
  -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="$PWD/install-sdk" \
  -DYYJSON_ROOT="$YYJSON_ROOT"
cmake --build build-sdk
cmake --install build-sdk
```

Use a separate build directory and `-DBUILD_SHARED_LIBS=ON` for shared libraries. Shared product libraries have SOVERSION 3 and installed self-relative dependency resolution. Version 0.0.0 is unstable, not a compatibility promise.

Fresh isolated static and shared `find_package` consumers passed against the current ABI3 product after archive v3 and HTTP-error retention corrections. Both made two real loopback HTTP requests, replayed genuinely sealed tool history and retained refusal/known-zero/original raw outcomes after Client destruction; a mismatched linked-interface revision was refused. Current Linux plain and ASan+UBSan suites passed26/26. TSan passed25groups plus the corrected HTTP/3 launcher group; the launcher failure was shadow-address-space initialization, not a reported race. These are Linux product/ownership observations, not stable ABI, other-platform or vendor-consumption qualification.

```cmake
find_package(SchemaProvider 0.0.0 EXACT CONFIG REQUIRED)
target_link_libraries(my_client PRIVATE SchemaProvider::runtime)
target_compile_features(my_client PRIVATE cxx_std_20)
```

Set `CMAKE_PREFIX_PATH` to the installation prefix. The target exports `include/SchemaProvider`; include public headers as `<runtime/client.h>`, `<core/native_archive.h>` and `<configuration/runtime_policy.h>`. Call `sp::runtime::require_interface_contract` with the revision/capabilities in `<core/interface_contract.h>` before dispatch: the out-of-line check validates the actually linked runtime/core, not just header constants. NeoGraph may instead explicitly set `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`; neither route restores a legacy adapter.

## Owned requests, results and configuration

`sp::runtime::Request` is the five-family typed variant. `Client::prepare(Request, RunOptions)` validates/encodes exactly once and returns a move-only `PreparedRequest`; `start`/`complete` consumes that same handle. Hosts with durable budget authority must claim and persist their dispatch receipt after successful preparation and before dispatch, retaining the original deadline/cancellation. Dropping a prepared handle releases its bounded slot without sending.

Results are immutable `std::shared_ptr<const sp::Outcome>` with full `Completion` or `Failure`, ordered parts, native carry, wire/raw observations, nullable `uint64_t` usage with evidence/stage/quality and genuine attempt metadata. Unknown is not zero. Portable text/tool projections are not native authority. Observer or persistence exceptions must retain the original result/partial failure rather than invent a successful projection. The bounded NeoGraph bridge owns request/callback state and preserves these results; legacy completion/interpreter paths are removed, not shims.

| Closed JSON inventory | Admission/authority |
|---|---|
| `config/runtime-defaults.json`, `config/error-policy.json` | `configuration::load_runtime_policy` or `load_runtime_policy_files` creates an immutable snapshot; pass it to `runtime::Options`. Reload affects new clients only. |
| `config/descriptor-policy.json`, `config/codec-defaults.json` | `descriptor::load_policy(family_json, resource_json)` creates an immutable snapshot; `descriptor::load(source, policy)` retains it. No event actions, loops or stateful interpreter. |
| `config/model-catalog.json` | Genuine selected-model pricing/limit facts for qualification; missing bounded-call facts are `LimitUnknown`, never guessed. |
| `tools/canary_profiles/` | Endpoint, selected model and caller controls; profiles do not issue or renew grants. |
| `config/qualification-plan.json`, `config/qualification-observations.json` | Workload/forecast inputs and historical scalar observations, not spending authority. |
| `config/qualification-authorization.json` | Separate fixed owner-approved campaign authority enforced by the persistent Meter; a request/profile cannot widen it. |
| `config/qualification-authorization-extension.json` | Separately approved fixed480-call/3000000-microUSD extension, admitted once as hash-chained `A` in the original Meter; never a replacement grant or profile authority. |

Unset optional controls remain unset; explicit zero remains distinguishable where the typed field permits it. Caller-selected optional controls/output caps are not silently clamped. Model limits, representation/resource bounds and host authority are distinct. Reservations are exposure bounds, not observed usage, forecasts or invoices. Retry has one owner, defaults off, stays inside its bounded window and retains unknown prior-attempt holds; there is no hidden resend.

`NativeArchive::provision(directory, independent_key_file, owner_scope, descriptor)` explicitly creates owner-private protected custody; `open` activates existing custody without resetting it. Closed v3 records and `spna3` references authenticate local custody with an independent key; v2 custody is rejected, not reinterpreted. Binding includes request members, usage paths and stop mappings as well as origin, policy and representation. This is not encryption or vendor-issuer authentication. `save`/`load` bind genuine native generations; editable JSON import cannot mint replay authority. Permitted duplicate-key diagnostic documents round-trip without admitting duplicates as executable tool input. Genuine in-memory C++ checkpoint sidecars require no archive; durable native/bank references require the actual archive. Never export raw native leaves through telemetry. Negative controls mutate already admitted genuine requests through the bounded diagnostic path; they do not issue native authority or authorize spending.

**Genuine InMemory shared-bank fork retained and exercised.** The original genuine C++ fork uses ONE original financial journal and trusted current branch heads, not cloned grants. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` (and `_async`) requires the authentic current source/full commitment and actual same-bank native C++ pointer; durable standalone forks remain explicitly unsupported. `OwnedManagedBudgetLease::scope()` and original owner/thread/graph, ceiling, deadline/clock and generation remain immutable. Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` select the execution branch separately; `GraphState::budget_original_thread_id()` identifies the original financial bank. Exact selected-branch head CAS and global actor/revision serialize all branches against canonical current counters, pending effects and burned identities. Original and fork branches remain usable without replenishment; stale snapshots, copied checkpoints and imported JSON cannot mint aliases or rewind heads. The original root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank proof PASSED in the unchanged test_graph_engine.cpp:810–913; saved original ceiling30 is separate from effective fork ceiling20; widening31 and JSON-only restore must reject. Unbounded reported observations are factual data, not finite grants. Only a proven zero-effect lease can release an unchanged head; unknown/pending effects keep their obligations.

**Recorded-control causal fix exercised in the full suite.** Captured command replay durably reserves only new CPU wall-time/Core work before execution, then publishes measured work and any newly produced Core checkpoint through the result CAS. It consumes no new model, money or Program-operation allowance and does not redispatch captured external effects. An unreconciled reservation remains debited. The reservation selects the authenticated settlement transition rather than an ordinary Running→Running transition that rejected the first new Core checkpoint. Await channel receive, timer wait/cancel and handoff wait initiation/release are serialized on their owning executors/strands; the existing Recorded CPU/Memory await/handoff scenarios passed in the full suite; remote TSan coverage limits remain explicit below.

Original JSON observations survive typed semantic projection, partial failure and admitted retries, including bounded non-2xx `http.error` bodies. That label is diagnostic, not vendor type, model output or native authority. Named SSE errors remain authoritative through actual close even after terminal-looking data; malformed payloads and raw-retention overflow cannot turn them into success. Durable reconstruction no longer applies the unrelated1MiB request-parser default to already owned larger outcomes.

## Reproducible model-free benchmarks

The Linux x64/WSL2 static Release run freshly qualified all **58 configurations / 174 fresh-process records after full five-family raw JSON retention**: three common H1 workloads, five families × H1/H2 × buffered text/tool/native and SSE tool/native, plus five native-carry encode cases. System libcurl8.5.0/OpenSSL3.0.13 negotiated the requested H1/H2 protocols; HTTP/3 correctness evidence is separate, not an H3 performance result. No compiler ran during measurements and no hosted or paid request was made.

The independent ephemeral-CA TLS peer observed **31,380 HTTP requests**, **83,520 original raw JSON observations**, **6,615 exact native continuations**, zero invalid workload/replay checks and zero transport-internal resends. All31,380 owned results were revalidated after Client destruction. Synthetic native signatures prove exact local replay only, not vendor acceptance or consumption. Missing Messages cache bands and Google thought usage remain unknown; the benchmark verifies nullable counts and reported-versus-derived evidence rather than fabricating zero.

[Raw records](benchmarks/current-sdk-benchmark-results.json) and [summary](benchmarks/current-sdk-summary.json) retain protocol counts, effective configuration, p50/p95/p99, first semantic event, RSS, threads and ownership/replay checks. Values below are medians of three process repetitions; latency and throughput units are **per HTTP request**, not per graph run or model token.

| Common H1 workload | p50 ms | p95 ms | p99 ms | HTTP requests/s | Peak RSS MiB | Sampled peak threads |
|---|---:|---:|---:|---:|---:|---:|
| Buffered text | 0.682 | 0.893 | 1.052 | 1,392.53 | 11.441 | 5 |
| Buffered tool + continuation | 7.588 | 76.346 | 83.203 | 1,656.50 | 14.719 | 36 |
| SSE tool + continuation | 7.156 | 73.948 | 81.084 | 1,666.23 | 14.770 | 36 |

Text uses concurrency1/no peer delay; tools use concurrency32/5ms per-request peer delay. Each runtime cohort warms up10 and measures100 logical iterations; tool/native iterations dispatch two requests. Threads include caller threads and the one sampling thread. These are local protocol/retention costs, not model-inference speedups or equivalence with the legacy GraphEngine.

| Native encode family | Initial p50 µs | Retained native/tool carry p50 µs |
|---|---:|---:|
| Chat | 13.245 | 21.210 |
| Messages | 11.421 | 17.031 |
| Responses | 12.553 | 25.497 |
| Generate | 13.835 | 26.559 |
| Interactions | 11.932 | 22.873 |

Each encode cohort measures1,000 initial and1,000 carried encodes per repetition after real two-turn capture. These concurrent per-encode latencies exclude TLS and peer delay.

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DSP_BUILD_TESTS=OFF -DSP_BUILD_BENCHMARKS=ON \
  -DYYJSON_ROOT="$YYJSON_ROOT"
cmake --build build-bench --target sp_provider_benchmark
python3 -c 'import json; d=json.load(open("benchmarks/matrix.json")); print(*(d["same_workload_baseline_configs"]+d["extended_runtime_configs"]+d["local_encode_configs"]), sep="\n")' |
while IFS= read -r config; do
  build-bench/benchmarks/sp_provider_benchmark "$config" || exit "$?"
done
```

Run from the SDK root with Node.js and OpenSSL CLI available. The closed [matrix](benchmarks/matrix.json) is the workload authority. Private model rows omit inherited output defaults and every request carries the explicit configured cap; no caller cap is clamped. Use equivalent Release settings for graph before/after comparisons.

## Persistent qualification campaign (completed observations; explicit limits)

The original fixed `SPQUAL1` authority is **630 requests / US$1** under one persistent project-root Meter. The separately approved **additional480 requests / US$3** is admitted once in that same ledger, for aggregate **1110 requests / US$4**. The original immutable `SPCANARY1` baseline remains **99 calls / US$18.979680 exposure**; reopening/restarting never renews budgets. `--ledger` is not a current CLI argument.

**Completed paid observations; not universal qualification.** Original `SPQUAL1` base630/1000000 microUSD is unchanged; ONE hash-chained `A` admits approved extension480/3000000 in the same original ledger, aggregate1110/4000000, with cumulative calls/spent/holds/settlements and no new grant ID/header/reset. Exact declaration bytes/file identity and original authorization/baseline/catalog/activation/ledger-prefix hashes/totals remain pinned; removal/replacement/change fails closed. The final canonical ledger is calls1110/spent437958/held1287828 microUSD, eventA1, limits1110/4000000; spent+held is US$1.725786 LOCAL catalogue meter, not an invoice. The documented five-family60-pair baseline completed600 requests: Chat60/60, Responses60/60, Messages60/60, Generate56/60 (four incorrect-vision SSE), Interactions57/60 (one buffered and two SSE incorrect-vision); aggregate293/300 pairs, not300/300. Other old600 financial records remain preserved, not full behavioral proof. Earlier M5/media one-shot cohorts are unchanged. The earlier three-round Google prerequisites retain two invalid-tool and one unreadable-positive failures. No further paid calls are authorized. Final SDK evidence and native-axis limits are separate from baseline success. Earlier activation/reopen smoke remains recorded at calls610/spent219159/held751233 after two reopens, with SDK meter/canary/vision four tests passed19.38seconds; these are scoped prior checkpoints, not final ledger totals. The earlier verified Chat60-pair cohort retains120 actual attempts,120 UpperBound charges and no UnknownHold.

**Native-axis observations, not cryptographic verification or native consumption/equivalence.** Generate accepted mutation, omission and duplication. Interactions accepted the isolated genuine source/positive control, one-owner signature mutation, thought-carrier omission, call-carrier omission and duplication. Removing all thoughts/signatures returned generic400; removing all signature fields while keeping THOUGHT items also returned generic400. The last capture had a local encoded-original retention control, not a same-capture server positive; the earlier positive cohort remains genuine. These observations establish an aggregate-carrier-absence boundary only, not issuer/signature validation or vendor consumption. Actual reports: SDK `config/qualification-extension-results.json`, `qualification-final-summary.json`, `qualification-native-axis-results.json`, `qualification-combined-omission-results.json`, `qualification-signature-presence-results.json`; prerequisite-failed/not-run/negative-inconclusive states remain factual. Thought-only/carrier-only omissions were accepted while another carrier remained; this does not strengthen issuer-validation or native-consumption claims.

Reports: [qualification-extension-results.json](config/qualification-extension-results.json), [qualification-final-summary.json](config/qualification-final-summary.json), [qualification-native-axis-results.json](config/qualification-native-axis-results.json), [qualification-combined-omission-results.json](config/qualification-combined-omission-results.json), [qualification-signature-presence-results.json](config/qualification-signature-presence-results.json).

**Actual integrated proof and remaining limits.** Latest Core full run:2242 tests, zero failures,16 skips (14 RAM process-loss cases not applicable; two live-credential gates),130.17seconds. `PgNestedJsonRoundTrips` preserved exact duplicate keys/order/null metadata, blob and residual in0.18seconds. The unchanged original shared-bank fork and existing Recorded CPU/Memory await/handoff scenarios passed. Real wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probes passed plain and ASan+UBSan. LOCAL Memory/SQLite/PostgreSQL TSan scopes:seven passed,zero warnings. Full mixed gRPC plus system Abseil/Protobuf TSan exited66 with402 race warnings in dependency/generated-RPC stacks: an instrumentation/coverage limit, not a proven false positive; remote TSan/race-freedom is NOT claimed and no warning is suppressed. Installed find_package Program C++/C ABI/dualQuickJS three consumers passed. Fresh installed NeoGraph/SchemaProvider typed consumer passed two real HTTP requests, provider destruction before coroutine start, native/tool replay, refusal,known-zero/raw retention and actual LinkedMismatch rejection. Browser Alice/Bob isolation and generation2 replacement were visually verified; PostgreSQL Program Chat six black-box tests passed18.989seconds. Latest SDK26/26 passed,zero failures,74.07seconds. Final ReleaseGraph16 configurations ×3 fresh process repetitions/48 records completed38.29seconds,zero failures,all actual protocol/owned-outcome checks passed. NeoGraph `benchmarks/provider-cutover-final-results.json` and `benchmarks/provider-cutover-final-summary.json` retain this separate final cohort. No compiler or paid model ran during measurement; historical cohorts stay unchanged and semantic/resource equivalence is not claimed. Unstable SDK/ABI3 is not a stable release or broader-platform qualification.

Plan-only execution performs no Meter/extension I/O and reports no spending authority. Reported `grant.original` stays630/1000000; `grant.extension` is null before activation or480/3000000 with one authorization event afterward. Aggregate report limits must come from actual `Meter::Totals`, not approval text; absent Totals leave them null.

```sh
sp_canary --execute --profile tools/canary_profiles/vision-chat.json \
  --project-root "$PROJECT_ROOT" --mode baseline-pair --repetitions 60
```

Select `vision-chat.json`, `vision-messages.json`, `vision-responses.json`, `vision-gemini.json` or `vision-interactions.json`. `--mode full` runs one complete selected-family campaign by default, not the 600-request baseline. `--mode baseline-pair --repetitions 60` selects 60 buffered/SSE pairs per family: five families total 600 generation requests. `--mode google-diagnostics --repetitions 3` selects Google native controls for Gemini or Interactions, with an actual **15-request cap per invocation**, totaling at most 30 across both families. Use the same persistent project root for every invocation. Missing credentials/denied admission stop execution, not reset the Meter.

Reports separate `Exact`, `UpperBound` and `UnknownHold`; unknown usage retains its hold. Empirical pair/control frequencies do not prove native consumption or equivalence. **The paid baseline completed600 requests and293/300 pairs; this is not an all-family pass or native-consumption proof.** Separately approved NeoGraph media validation completed one request each: NanoBanana2Lite returned one1024×1024 JPEG (360,685bytes,input19/output1408); Veo3.1Lite returned one720p4second MP4 (437,737bytes,one generation/three status queries), decoded and visually inspected in Chromium; JeV1.13 returned probability0.93 (input283/output21,API-reported US$0.000011886). Missing totals/bands remain unknown. Image catalogue base US$0.0336 plus text/thinking and video catalogue expectation US$0.20 are not invoices. These requests consumed no chat grant and authorize no further media generation. The selected settings and sanitized observations are in [media-minimal-validation.json](config/media-minimal-validation.json); private artifacts/one-shot reservation markers are not published.


## Documentation
Current installation/configuration guidance below is normative for the unstable SDK. Historical measurements retain their original cohort and limits; remaining design sketches and release properties are not claims of implementation.
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
