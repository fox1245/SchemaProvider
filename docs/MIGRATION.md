# Migrating to interface 6

Use the [current first-request example](../README.md#first-request) before adapting an older caller. Interface 6 keeps the typed outcome/prepare and stall-bound contracts and appends provider-reported monetary usage. Rebuild with matching headers and loaded generation 6 libraries. Earlier interface 3–5 migrations remain below as historical steps; migrate removed callers rather than adding a legacy adapter or shim.

The SDK declarations are [runtime/client.h](../src/runtime/client.h) and [core/value.h](../src/core/value.h). The adapter declarations live in NeoGraph's [`provider.h`](https://github.com/fox1245/NeoGraph/blob/feat/schemaprovider-cutover/include/neograph/provider.h) and [`llm/schema_provider.h`](https://github.com/fox1245/NeoGraph/blob/feat/schemaprovider-cutover/include/neograph/llm/schema_provider.h); deployment must pin the released repository/package combination rather than treating a branch URL as an immutable version.

## Why requests, codecs and outcomes are typed

The older NeoGraph provider path interpreted JSON to build requests and mapped a response into `ChatCompletion`. That contract could not represent several ordered messages, unknown usage, owned partial failures or authenticated native carry. A successful text projection could lose information after the request had already incurred an external effect. The [historical M5 comparison](POC_PLAN.md#55-m5-results-and-gono-go-live-observation) recorded a strict lossless NO-GO for that consumer ABI. It did not establish that the protocol architecture should be discarded.

The current boundary changes both request and result ownership:

```text
closed descriptor + immutable policies + typed family request
    -> prepare: validate, encode, reserve a slot (no send)
    -> consume that preparation: start/complete
    -> immutable owned Completion | Failure
```

Family C++ code now owns request structure, tool/native checks, stream correlation, usage arithmetic and terminal decisions. Closed JSON supplies admitted values. Adding an endpoint speaking an existing family can change routes or allowed bindings, but JSON cannot invent a new event lifecycle. New wire semantics require a codec change and corresponding evidence.

`sp::runtime::Request` is the five-family typed variant. `Result` is `std::shared_ptr<const sp::Outcome>`, not a flat successful completion object. This lets callers retain ordered parts, native sidecars, raw observations and failure partials without borrowing a client or callback.

## Package 0.3.3 (interface 6)

Package 0.3.3 is a maintenance release of this line. The interface revision and shared-library generation stay 6, no installed struct or enum changes layout or gains an enumerator, and the embedded descriptor policy and its identity are unchanged. The fixes below change behaviour.

`on_event` no longer receives a text `PartDelta` with empty text. OpenAI Chat opens a stream with `"content":""`, a Messages text block starts with `"text":""`, a Gemini answer usually ends with a part whose text is `""` and Responses repeats the whole text at `output_text.done`; each of those used to arrive as a delta without text, so a caller that counted chunks, or took the first text delta for the first token, was off by one. The accumulator still applies the event: the part exists, keeps its index and order and seals as before, a text part that only ever received empty text is the empty `Text` a buffered response returns, and `PartBegin` and `PartSeal` are delivered either way. Code that waited for text rather than for the first delta (the advice that came with the old behaviour) keeps working. Thinking, refusal, tool-call and signature deltas are delivered as the wire gives them. See [streaming](USAGE.md#streaming-deadlines-cancellation-and-backpressure).

`descriptor::load` now holds a `google.generate` descriptor to the route shape the encoder accepts: the buffered path `/v1beta/models/<model>:generateContent` and the streaming path `/v1beta/models/<model>:streamGenerateContent?alt=sse` for the same `<model>`, made of letters, digits, `-`, `.` and `_`. Any other pair used to load and then failed every request, streamed or buffered, with `InvalidConfig` at dispatch (a streaming path copied from the buffered one is the usual cause); it is now a `ConfigError` at `/connection/paths/buffered` or `/connection/paths/streaming`. A descriptor that could serve a request is unaffected, and so are the other families and the embedded policy and its identity. See [descriptors](USAGE.md#descriptors-credentials-and-defaults).

## Package 0.3.2 (interface 6)

Package 0.3.2 is a maintenance release of this line. The interface revision and shared-library generation stay 6, no installed struct or enum changes layout or gains an enumerator, and the embedded descriptor policy and its identity are unchanged. The fix below changes behaviour.

A streamed Chat Completions choice without `finish_reason` now reads as `"finish_reason": null` while the stream is open. Google's OpenAI-compatible endpoint (`https://generativelanguage.googleapis.com/v1beta/openai/chat/completions`) leaves the member out of every chunk before the last, so every stream from it longer than one chunk failed with `ProtocolCorrupt`; OpenRouter, which sends `null`, was not affected. The stream must still carry a string `finish_reason` before `[DONE]`, a buffered choice without the member and a chunk after the stop reason without it are still `ProtocolCorrupt`, and no other member is read this way. See [frame classes](DESIGN.md#41-frame-classes).

## Package 0.3.1 (interface 6)

Package 0.3.1 is a maintenance release of this line. The interface revision and shared-library generation stay 6, and no struct or enum of the public API ([runtime/client.h](../src/runtime/client.h), [core/value.h](../src/core/value.h)) changes layout or gains an enumerator, so a consumer rebuilds against the 0.3.1 headers only to select `SchemaProvider 0.3.1 EXACT`. The embedded descriptor policy and its identity, a persisted value on this interface, are unchanged. Two installed helper types that only the SDK's own libraries construct grow (`messages::Codec` and `runtime::detail::ResponseInfo`). The fixes below change behaviour.

Chat Completions now accepts one `Role::Tool` message that carries several `ToolResult` parts, like the other families, and writes one wire `tool` message for each in order. A Tool message that is empty or holds anything else is still refused, and the refusal names the rule. See [request families](USAGE.md#request-families-and-controls).

Temperature facts are now a rule by generation as well as a list. Every model of the `claude-` series from generation 4.7 on refuses `temperature` locally, so a Claude release such as `claude-haiku-5-5` no longer needs a new list entry (it used to be sent and answered with HTTP 400 "`temperature` is deprecated for this model"). Gateway spellings with a namespace or dots (`anthropic/claude-opus-4.7`) match the same rule, and a Claude id whose generation cannot be read is refused like the newest one. The rule is compiled, not policy data: the embedded policy and its identity, a persisted value on this interface, are unchanged, so no sealed history is affected, and a policy snapshot needs no new key. No public struct or enum changes; the interface revision stays 6. See [request controls](USAGE.md#reasoning-sampling-and-tool-controls).

The Chat output cap is written to `max_completion_tokens` by default on direct OpenAI (`https://api.openai.com`). Current OpenAI models (gpt-5.x and gpt-6.x, the reasoning models) answered `max_tokens` with HTTP 400 "Use 'max_completion_tokens'". Every other origin, OpenRouter and compatible endpoints included, keeps `max_tokens`, and an explicit `bindings.max_output_tokens` wins on any origin. The default is compiled, not policy data, so the embedded policy and its identity are unchanged. The binding member is part of the origin identity of native history, so Chat history sealed against direct OpenAI under the old default becomes `ReplayIneligible` when it is continued with the new one; binding `max_output_tokens` to `max_tokens` explicitly keeps the old identity for a conversation that must continue. A vendor 400 on Chat or Responses that names the refused member in `error.param` (`unsupported_parameter` or `unsupported_value`) now says so in `Error::safe_message`, for example `invalid request: unsupported parameter 'max_tokens'`, without the vendor's text. No public struct or enum changes; the interface revision stays 6. See [descriptors](USAGE.md#descriptors-credentials-and-defaults) and [outcomes](USAGE.md#outcomes-raw-data-and-nullable-usage).

A completed HTTP 4xx answer other than 408, 425 and 429 (401, 400, a 404 for an unsatisfiable OpenRouter `max_price` pin) is now `RetrySafety::RejectedBeforeOutput`: the server declined the request, so nothing ran and nothing is billed. It used to be `PossiblyAccepted`, which made a host keep its whole reservation for the call. `PossiblyAccepted` remains for 408, 425, 429, 5xx, answers that were cut off and transport failures after the request left, and `OutputObserved` and `NotSent` are unchanged. `RejectedBeforeOutput` is an existing enumerator, so no public struct or enum changes; the interface revision stays 6. A host that releases a reservation only for `NotSent` must also accept `RejectedBeforeOutput` with an HTTP status to benefit; retry policy treats it like `NotSent`, which changes nothing in practice because the embedded error policy gives no 4xx status a retryable class.

Generate now derives `Usage::output_total` and `total` (both `Evidence::Derived`) when `candidatesTokenCount` is omitted but `promptTokenCount` and `totalTokenCount` are present, which is what the API sends when the output budget is spent while thinking: `output_total` is the total minus the prompt count. Both used to stay unknown, so the call could not be settled from usage. A nonzero tool-use prompt count still leaves them unknown, and impossible counters are a conflict. No public struct or enum changes; the interface revision stays 6. See [nullable usage](USAGE.md#outcomes-raw-data-and-nullable-usage).

The usage of a call that ends early now follows one rule across families: `Usage::stage` is `Missing` with every counter absent when the provider had reported nothing (no evidence, not zero) and `Partial` with the counters seen so far otherwise, never `Final`. Chat and Responses send usage only with their final event, so a cut stream there already had none; a cut Messages stream no longer reports the placeholder output count of `message_start` (`output_total` was `0` or `1` while text had streamed), so it has the input counters, labelled partial, and no output counter. Complete calls are unchanged: `message_delta` supplies the cumulative output count, and a cumulative count below the one in `message_start` is still a conflict. No public struct or enum changes; the interface revision stays 6. See [nullable usage](USAGE.md#outcomes-raw-data-and-nullable-usage).

## From interface 5 to 6

Package 0.3.0 alpha adds `Usage::provider_cost` and the fixed-size `ProviderReportedCost`, `UsdAmount`, `CostSource`, `CostStatus` and `CostRounding` types. Existing field access remains source-compatible, but `Usage`, `UsageUpdate`, `Completion`, `PartialCompletion` and the containing `Event` layouts grow. Rebuild every consumer against the same headers and generation 6 libraries (`libsp_*.so.6`); interface 5 binaries must not load interface 6 libraries. CMake consumers select `SchemaProvider 0.3.0 EXACT`.

Money is not a token counter. Do not read `cost` from `Usage::extra` or add it to token totals. The amount unit is nanoUSD (1e-9 USD), converted exactly from the shortest decimal that round-trips to the parsed binary64 number and rounded upward only below one nanoUSD (`CostRounding::CeilingShortestDecimal`; the earlier binary64 ceiling added one nanoUSD to values such as `1.5e-06`). Read each field's status and source, preserve missing versus reported zero, and use the enclosing usage stage to distinguish partial from final observations. A provider report is not an invoice, a catalogue estimate, or authority to settle/renew a spending grant; existing qualification ledgers and nonrenewable holds are unchanged. See [reported costs](USAGE.md#provider-reported-costs).

The Responses codec admits a direct OpenAI base-model alias resolving to the same model with a valid dated snapshot suffix, while retaining requested context and stable served-stream identity. Explicit snapshots and unrelated origins stay exact. The Chat codec accepts a semantically empty idempotent repeated terminal choice carrying usage, without another Stop/content/outcome; contradictory/new post-terminal data remains invalid. These repairs do not admit foreign native replay authority or extend operation deadlines.

Responses and Messages also admit a different nonempty served `model` when the descriptor origin is declared in its family's `openrouter_origins` policy. Such a routing gateway resolves aliases (for example `~vendor/model-latest`) and fallbacks itself, so it echoes the concrete model it served rather than the request. The served identity must still stay stable for the whole response, the raw wire envelope keeps it, and native replay stays bound to the requested model. Undeclared origins keep exact equality and the direct OpenAI and Anthropic dated-snapshot rules. See [served model identity and routing gateways](USAGE.md#served-model-identity-and-routing-gateways) for how a deployment declares its origin, what the declaration turns on besides the model, and why Gemini cannot declare one.

Usage fidelity repairs keep layouts and the interface revision. `CostRounding` gains `CeilingShortestDecimal`, now the produced and default value; `CeilingParsedBinary64` remains declared but is no longer produced, so update comparisons and exhaustive switches. Exact decimal costs such as `1.5e-06` USD are `1500` nanoUSD (previously `1501`). OpenRouter Messages now projects `provider_cost` and accepts the one `[DONE]` frame that gateway sends after `message_stop`; direct origins and any other post-terminal data stay corrupt. Direct Anthropic admits an undated alias served as its dated snapshot (`claude-haiku-4-5` as `claude-haiku-4-5-20251001`) with requested-model replay binding, like the direct OpenAI rule. `Usage::extra` gains Chat's unlisted integer leaves; Interactions drops typed aggregates and renames positional modality keys (`input_tokens_by_modality.0.tokens` becomes `input_tokens_by_modality.text`). Generate now derives zero thoughts when the reported total proves them. See [nullable usage](USAGE.md#outcomes-raw-data-and-nullable-usage).

Responses function tools apply the policy `strict_tools` default only to strict-compatible parameter schemas and send `"strict": false` for other schemas, which OpenAI rejected with HTTP 400 under the default. An explicit `strict = true` on an incompatible schema now returns `InvalidRequest` before dispatch, naming the tool and the first failing JSON pointer; an explicit `false` is unchanged. The descriptor policy, its identity and native configuration digests are unchanged. See [request controls](USAGE.md#request-families-and-controls).

The Chat codec binds stream identity to `id` and `model`. Some OpenAI-compatible local servers stamp each chunk with the current time, so `created` may now repeat or increase between chunks instead of failing every stream as `ProtocolCorrupt`; it must still be an unsigned count on every chunk, and a decrease or a changed `id` or `model` remains `ProtocolCorrupt`.

An in-band failure larger than `max_error_bytes` (64 KiB by default) that the codec had already admitted under the response bounds was reported as `ResourceLimit` with `RetryClass::Never`. A Responses `response.failed` after a hosted image, which repeats the multi-megabyte image, hit this. It is now classified from the admitted document like a small one: `RemoteFailure`, or the kind, retry class and `vendor_code` of an admitted error code. `max_error_bytes` still bounds non-2xx bodies and in-band error bytes the runtime parses itself; runtime and error policies are unchanged. See [outcomes](USAGE.md#outcomes-raw-data-and-nullable-usage).

`sp::OpenRouterRouting` gains optional `max_price` (`sp::OpenRouterMaxPrice`, integer nanoUSD amounts sent as exact decimal USD), OpenRouter's cap on the endpoint price it may route to. Unlike the earlier interface 6 repairs, this change grows public layouts: `OpenRouterRouting` grows from 120 to 208 bytes, and with it `chat::Request` (568 to 656), `messages::Request` (424 to 512), `responses::Request` (760 to 848) and the `runtime::Request` variant (768 to 856), measured with GCC 13 and libstdc++ on Linux x86_64. Rebuild every consumer against these headers and libraries; a binary built against earlier interface 6 headers must not load them. By maintainer decision the interface revision and shared generation stay 6: this line promises no binary layout compatibility between builds, and consumers rebuild per release. Because the generation does not change, the new capability bit `sp::capability::OpenRouterRoutingMaxPrice`, part of `RequiredProvider`, marks the change: a consumer built with these headers that calls `require_interface_contract` with it is refused by an earlier interface 6 library instead of losing the cap. A binary built before the change cannot ask for the bit, so it must be rebuilt. Routing without a cap encodes the same body and native configuration digest as before, so histories sealed under it still continue; a cap joins the routing binding. Code that copies routing field by field, such as a defaults merge, must copy `max_price` too. The descriptor policy and its persisted identity are unchanged. See [request controls](USAGE.md#reasoning-sampling-and-tool-controls).

## From interface 4 to 5

Package 0.2.0 advances the interface revision and shared-library generation from 4 to 5. Source compatibility is additive: `transport::TransportOptions` and `transport::HttpRequest` gain optional `connect_timeout`, `first_byte_timeout` and `idle_timeout` fields (disabled by default), `RunOptions` gains the matching per-run overrides, and `transport::FailureKind` gains `ConnectTimeout`, `FirstByteTimeout` and `IdleTimeout` ahead of `Other`. Existing callers compile unchanged and behave unchanged until they set a bound; a `switch` over `FailureKind` without a default needs cases for the three new kinds. Stall-bound outcomes are classified by existing runtime kinds, so `ErrorKind` has no new value; see [stall bounds](USAGE.md#stall-bounds-for-long-generations).

The public layouts and the `FailureKind` values changed, so rebuild every consumer with the matching headers and load generation 5 libraries (`libsp_*.so.5`); never mix an interface 4 binary with interface 5 libraries. The runtime also no longer links libcrypto: SHA-256, HMAC-SHA256, constant-time comparison and operating-system entropy are implemented in-tree and produce the same digests, archive bytes and ledger formats. The libcurl TLS backend is unchanged.

## From interface 3 to 4

Package 0.1.0 remains alpha; loaded interface/shared-library generation advance from 3 to 4 independently of package, descriptor, native-archive and portable JSON versions. Historical interface 3 suite/consumer/benchmark observations stay archived and do not establish interface 4 support.

| Preserved capability | Typed interface 4 surface |
|---|---|
| Chat reasoning object/returned reasoning/usage/alternative models | `chat::ReasoningOptions`, `Request::{reasoning,include_reasoning,usage_include,models}` at declared OpenRouter origins; scalar `reasoning_effort` remains distinct. |
| Responses server-held conversation | `previous_response_id` with **new input only**; local `previous_response_history` checks authentic tool ownership but is never emitted. It is distinct from full native replay. |
| Responses parallel/format/detail controls | `parallel_tool_calls`, typed `Verbosity`, `Truncation`, optional vector of `Include`. Explicit false/empty selection remains explicit. |
| Messages thinking/output/cache/tool controls | typed Manual/Adaptive/Disabled thinking, OutputEffort, CacheControl/CacheTtl, ToolChoice and declared-origin routing. Disabled differs from unset; thinking/sampling validity is model-specific. |
| Generate portable foreign assistant history | explicit `HistoryMode::PortableForeign` for unsealed portable Text/ToolCall, with the documented first-function-call sentinel; no imported native grant or seal repair. |
| Generate thinking/sampling/safety/tool controls | typed ThinkingLevel/SafetySetting/ToolChoice plus temperature; admitted mutual exclusions and model/resource bounds reject before I/O. |
| Model temperature facts | closed family `temperature_forbidden_model_prefixes`, ASCII case-insensitive full/gateway-suffix matching; explicit forbidden values reject. |
| Optional Anthropic environment headers | host `load_with_environment_headers` or deterministic `load_with_deployment_headers` **before** closed descriptor admission; literal/override precedence is case-insensitive. |

See [Usage](USAGE.md#reasoning-sampling-and-tool-controls) for actual enum members, constraints and examples. JSON interpretation is not restored: these capabilities have typed fields, encoded wire shapes and ownership/error boundaries.

Generation cap is now per-dispatch resource admission rather than a consumable native-history configuration field. Changing a cap does not repair content, prefix, model, reasoning, routing or policy mismatch; its actual new value still needs resource and host-bank admission with the original effect/deadline rules. Changing a deadline or catching a failure does not renew a budget.

The archive format remains v3/`spna3`, and portable JSON version domains stay independent. A new admitted policy/control identity can reject an older policy-bound archive as `ReplayIneligible`; do not forge a new identity or silently migrate it. Cursor-produced terminal ownership remains in-process and incomplete for full NativeReplay/archive use. Plain cursor strings and imported Gemini history are not substitutes for genuine original-prefix native authority.

## Replacement map

| Old caller assumption | Current action |
|---|---|
| NeoGraph `CompletionParams`, `ChatCompletion`, `Provider::complete` or `complete_async` | Build `ProviderRequest`; use `Provider::invoke`/`invoke_async`, or prepare then `dispatch`/`dispatch_async`; inspect the full SDK outcome. |
| Separate `OpenAIProvider` and descriptor-driven provider path | Use `neograph::llm::SchemaProvider` with a `ValidatedDescriptor`, `sp::runtime::Options` and its typed `Defaults`. |
| `RateLimitedProvider` as the old completion decorator | Move caller policy to the actual host's current admission/control surface; do not retain the removed wrapper or add a lossy alias. SDK retry is a separate opt-in policy, not rate-limit authority. |
| `SchemaPrimitiveRegistry`, descriptor actions, prompt templates or arbitrary `request_json`/`extra_fields` | Select a declared family request and populate its fields. Unknown configuration is rejected; implement new wire behavior in family C++, not a JSON program. |
| “No observer” means buffered mode | Set `RunOptions::streaming` in the SDK, or `ProviderMode` in NeoGraph. Mode is explicit and independent of callback presence. |
| Usage is zero when absent | Preserve `optional<Count>`/unknown and count evidence, stage and quality. Narrowing unsigned counts into old `int` counters can overflow. |
| A partial streamed tool fragment is executable | Execute only a sealed `ToolCall` of kind `ClientExecuted`, under host policy. Preserve invalid/server/approval calls without executing them. |
| Saved text/JSON can reconstruct native reasoning | Retain genuine `Message::native` and ordered carry in memory; use protected `NativeArchive` for durable custody. Portable export does not confer authority. |
| Failed stream after some text is successful partial output | Keep `Failure` and its `PartialCompletion`; use partial text for display only if your application labels it incomplete. |
| Error/observer exception permits another request | Retain attempt evidence and any exact outcome. A possibly accepted request can still have generated/billed remotely. |
| SDK `Client::complete` was removed with NeoGraph `Provider::complete` | SDK `Client::complete` remains the blocking facade over the same operation as `start`. The two APIs are distinct. |

## The NeoGraph adapter boundary

The adapter lives in NeoGraph, not in the SDK's `integration/` directory. Its current constructor is:

```cpp
neograph::llm::SchemaProvider(
    sp::descriptor::ValidatedDescriptor descriptor,
    sp::runtime::Options options = {},
    neograph::llm::SchemaProvider::Defaults defaults = {});
```

`Defaults` contains `provider` (optional `sp::OpenRouterRouting`) and `responses_store` (optional bool), not a generic field map. The descriptor and runtime policy are immutable; creating a new provider is the reload boundary. Each request can still carry supported typed controls.

This callable example performs one buffered Chat request against the same local peer as README. Compile it in an installed NeoGraph consumer linked to `neograph::llm`:

```cpp
#include <neograph/llm/schema_provider.h>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <variant>

sp::runtime::Result local_provider_call() {
    auto loaded = sp::descriptor::load(R"json({
      "descriptor_version":1,"revision":1,"id":"local-chat","family":"openai.chat",
      "connection":{"base_url":"http://127.0.0.1:8765",
        "paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}
    })json");
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded))
        throw std::runtime_error(error->expected);
    neograph::llm::SchemaProvider provider(
        std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded)));
    sp::Message user;
    user.role = sp::Role::User;
    user.parts.emplace_back(sp::Text{"Say hello."});
    neograph::ProviderControls controls;
    controls.max_output_tokens = 64;
    auto request = neograph::make_provider_request(
        provider, "example-model", {user}, {}, controls,
        neograph::ProviderMode::Collect);
    request.options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    return provider.invoke(std::move(request));
}
```

The SDK remains sufficient for standalone use. NeoGraph adds `ProviderRequest` with typed `payload`, explicit `ProviderMode`, SDK `RunOptions`, host cancellation and bounded event delivery. `make_provider_request` maps portable messages/tools and declared controls to the provider's actual family; it cannot make unsupported controls meaningful.

For a pre-dispatch host claim, replace the final invocation with:

```cpp
auto prepared = provider.prepare(std::move(request));
if (const auto* error = prepared.error()) {
    // Inspect the pre-effect rejection; do not reserve money for a rejected request.
    (void)error;
    return provider.dispatch(std::move(prepared));  // Owned preflight Failure, no send.
}
// On successful admission, persist the host's required receipt/claim here.
return provider.dispatch(std::move(prepared));
```

`PreparedProviderRequest` is move-only. Its coroutine dispatch owns runtime/request state; a returned awaitable does not borrow the `Provider` or original request through later scheduling. `invoke` combines preparation and dispatch, while the two-phase path permits host validation and durable write-ahead admission. Graph/Program budgets, claims, checkpoint authority and tool execution remain host responsibilities. Preparation does not create those authorities.

`ProviderOutcomeError` and its observer/budget-settlement subclasses retain the exact drained `sp::runtime::Result`. Handle delivery/persistence errors without discarding that result or reporting it as a successful projection. The SDK alone has no graph journal or financial settlement policy.

## Results and conversations

Migrate stored and in-memory consumers together. Preserve the full `Outcome`, ordered messages/parts, nullable counts, raw envelope/events, stop details and actual attempt evidence. Update exhaustive visitors when `Part`/`Event`/error enums change; a fallback that silently drops a typed member reintroduces loss.

Use generic `sp::Message` history for native-capable continuations. Keep the complete original request prefix plus returned sealed messages; assistant-only history, including restored assistant groups without their prefix, rejects. Chat's simple `InputMessage` is not a whole-native-history replacement. Responses cursor input instead carries only new messages, with separate local ownership evidence. Generate's explicit portable mode admits only the declared unsealed imported shape; edited/foreign native carry is never repaired by stripping it or switching modes.

Keep durable version domains separate:

- Package version identifies an SDK build. Interface revision/capabilities identify the loaded typed runtime contract. Shared-library version is not a cross-compiler compatibility promise.
- Descriptor grammar version and fact revision describe configuration, not a provider-completion serialization.
- NativeArchive v3/`spna3` authenticates local custody independently of interface 4. Older policy/control identities can still be replay-ineligible even when storage format matches; v2 custody is not upgraded or relabeled.
- NeoGraph owns its provider-outcome/call/checkpoint formats and migration rules. Do not reinterpret old v1 successful text as a native-capable current outcome or bulk-rehash old identities.

A historical run lacking native authority cannot be resumed as if it carried genuine current sidecars. An unfinished tool loop cannot be moved to a new origin by inventing provenance. Any application-level turn-boundary migration must be explicit about the new run and lost continuity; the SDK supplies no automatic old-record importer.

## Verify a migrated caller

Use local peers and installed consumers before any hosted request:

1. Compile the README program against installed static and shared SDK trees, not source-tree include shortcuts. Run the loopback recipe and check the peer's single matched request plus the exact text/count result.
2. Repeat with usage missing and reported zero. Both must stay distinct in every application serializer and budget calculation.
3. Exercise a non-2xx error, a truncated SSE body and a stop requested before dispatch. Assert `Failure` and inspect partial/raw/attempt evidence; no false success or consumer-triggered resend.
4. Exercise successful preparation followed by abandonment, capacity rejection and an expired retained deadline. Count requests at the peer; preparation must send none.
5. For native-capable consumers, capture a real local two-turn completion from the runtime, retain unchanged carry and test prefix/model/origin mutation refusal. Synthetic signatures prove local replay retention only.
6. Retain the result past provider/client destruction. Validate typed messages, zero/unknown usage and raw `Document` ownership after destruction.
7. For new controls, use independent local body/state peers: reject wrong-origin Chat/Message routing, prohibited model temperature, invalid thinking/tool/safety combinations and duplicate include/header selections before I/O; distinguish absent/false/empty controls. Exercise Responses new-input cursor state and failed/mismatched ownership, and Generate explicit foreign text/tool history versus unchanged/edited genuine carry. Parent qualification must observe the wire/state/error result, not merely compile request fields.

These are verification requirements, not a claim this guide has run them. [CONFORMANCE](CONFORMANCE.md) and [recorded provenance](POC_PLAN.md#current-typed-c-cutover-provenance) distinguish previous executed cohorts from current release gates. No paid calls are authorized by an example, policy snapshot or this document.
