# Using the interface 6 SDK

Start with the complete [README program](../README.md#first-request). This guide explains what that program owns and how to extend it. Installed declarations, not historical sketches in DESIGN, define the API.

Source references: [runtime request/handle API](../src/runtime/client.h), [owned values/outcomes](../src/core/value.h), [descriptor admission](../src/descriptor/descriptor.h), [immutable policy](../src/descriptor/policy.h) and [native archive](../src/core/native_archive.h). Request-family headers named below define the controls; historical names such as `JsonValue`, `Client::option` and `ForeignReasoning` are not current declarations.

## Provider-reported costs

`Completion::usage.provider_cost` and `Failure::partial.usage.provider_cost` separate monetary metadata from integer token counts. The fixed-size object has optional `total`, `upstream_total`, `upstream_input` and `upstream_output` amounts, four matching `CostStatus` entries in that order, optional `is_byok`/`byok_status`, `CostSource` and independent `UsageQuality`. Amounts carry `Evidence::Reported` and `CostRounding::CeilingParsedBinary64`. No monetary leaves enter `Usage::extra` or token totals.

One `UsdAmount::nano_usd` is 1e-9 USD. Conversion computes the upward integer bound of the already-parsed binary64 USD number with stack-only integer arithmetic; it does not reconstruct an exact original JSON decimal. For example parsed `0.1` projects to `100000001` nanoUSD. Positive subnano amounts round up to one; reported zero stays present zero. Amounts above 2^53 nanoUSD lack the admitted precision, and values reaching the uint64 range overflow. `Missing`, `Malformed`, `PrecisionExceeded`, `Overflow`, `UnknownCurrency` and `Conflict` remain explicit unavailable statuses, not invented zero. Only policy-admitted OpenRouter origins have known USD monetary semantics; arbitrary gateways retain unknown-currency status and raw metadata.

Each reported usage object replaces its monetary snapshot, clearing stale omitted values. The enclosing `Usage::stage` is partial for updates/failures and final on successful commit. Read status/source/stage before using any amount; malformed monetary metadata does not discard otherwise-valid text or relax token-counter validation. Raw envelopes/events remain available. The JSON parser remains strict, so invalid nonfinite JSON and overflow-to-infinity can fail before cost projection; numeric lexical precision already lost during binary64 parsing cannot be recovered.

OpenRouter Chat reports upstream input/output using `upstream_inference_prompt_cost` / `upstream_inference_completions_cost`; Responses uses `upstream_inference_input_cost` / `upstream_inference_output_cost`. They project into the same typed `upstream_input` / `upstream_output` fields. When both spellings are supplied they must agree as parsed binary64 values before quantization; different values become `Conflict`, unavailable amount and inconsistent monetary quality even if both would round into the same nanoUSD bucket. A null/missing primary can use a known alternate. Both raw values remain retained; no arbitrary winner or arithmetic guess is introduced.

Provider-reported costs are not invoices, catalogue estimates or budget authority. This projection does not change a grant, refund an unknown hold, estimate a missing amount, or settle cumulative spending automatically. The qualification catalogue/meter keeps its existing integer microUSD arithmetic, conservative unknown holds and nonrenewable authorization lineage.

The monetary snapshot describes the currently observed wire attempt, not an automatically accumulated bill for every retry of one logical operation. Keep `AttemptEvidence::prior_usage_unknown` alongside it: an earlier attempt can incur unreported usage, even when the final attempt reports zero. Cost-only metadata (including reported zero or BYOK false) prevents the runtime from proving that a prior attempt had no usage. Do not infer a refund, free retry or complete logical-call invoice from one reported amount.

## Run the first request without a hosted API

Create a temporary consumer directory containing the README's `CMakeLists.txt` and `main.cpp`. With `SDK_PREFIX` set to your installation prefix:

The README install recipe uses `SP_BUILD_TESTS=OFF` and `SP_BUILD_CANARY=OFF` to build only the consumer product. The complete SDK property suite currently requires `SP_BUILD_CANARY=ON`; those diagnostic tools are not part of the installed consumer API. Enabling/building a tool never authorizes a hosted request.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build
```

In another terminal, run this peer. It binds only loopback, handles exactly one request and exits. It checks the actual request body before sending a synthetic response. Do not supply real credentials.

```sh
python3 - <<'PY'
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

class Peer(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        valid = (
            self.path == '/v1/chat/completions'
            and request['model'] == 'example-model'
            and request['messages'] == [{'role': 'user', 'content': 'Say hello.'}]
            and request.get('max_tokens', request.get('max_completion_tokens')) == 64
            and request['stream'] is False
            and self.headers.get('Authorization') is None
        )
        if not valid:
            self.send_error(400, 'request did not match the example')
            return
        body = json.dumps({
            'id': 'local-1', 'model': 'example-model',
            'object': 'chat.completion', 'created': 1,
            'choices': [{'index': 0, 'message': {'role': 'assistant', 'content': 'Hello.'},
                         'finish_reason': 'stop'}],
            'usage': {'prompt_tokens': 3, 'completion_tokens': 0, 'total_tokens': 3}
        }).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        print('matched one credential-free request', flush=True)

with HTTPServer(('127.0.0.1', 8765), Peer) as server:
    print('ready on 127.0.0.1:8765', flush=True)
    server.handle_request()
PY
```

If port 8765 is occupied, choose another localhost port and change both the
descriptor's `base_url` in `main.cpp` and the peer's `HTTPServer` address. Do not
send the example request to an unrelated service already listening on that port.

Run `./build/first_request` in the consumer terminal. Expected output, to verify against your build:

```text
Hello.
output tokens: 0
```

The zero is intentionally synthetic: the SDK must preserve a reported zero even though this peer returns text. Remove the entire `usage` member, restart the peer and rerun to check the separate missing-count path; expected output ends with `output tokens: unknown`. These scenarios exercise protocol mapping and ownership, not a real model's token counting.

For a failure scenario, replace the response object with `{'error': {'code': 'invalid_request_error', 'message': 'synthetic failure'}}` and send status 400. The executable must take its `Failure` branch, return 2 and not print successful text. To inspect raw non-2xx retention, use the result example below; do not log production bodies.

## Descriptors, credentials and defaults

A `ValidatedDescriptor` is a value returned by `sp::descriptor::load`. The result is `std::variant<ValidatedDescriptor, ConfigError>`; inspect the error arm before constructing a client. The constructor can also throw `sp::descriptor::ConfigError` for invalid runtime options. `ConfigError` is a structured value with `pointer`, `expected`, `revision` and `message`, not a `std::exception` subclass.

The closed descriptor-version-1 root accepts only these keys:

| Key | Meaning |
|---|---|
| `descriptor_version` | Exactly `1`; future versions are rejected, not guessed. |
| `revision`, `id` | Positive unsigned fact revision and stable ASCII identifier. |
| `family` | `openai.chat`, `openai.responses`, `anthropic.messages`, `google.generate` or `google.interactions`. |
| `connection` | Origin-only `base_url`, literal `paths.buffered`/`paths.streaming`, optional non-reserved literal `headers`. |
| `bindings` | Optional admitted model/message/stream/output-cap member names and usage path segments. Defaults come from the family's policy. |
| `stop_reasons` | Optional mappings to declared stop kinds; compiled failure/standard-stop safety cannot be overridden. |
| `evidence` | Optional HTTPS documentation URLs and verification date; metadata is not runtime qualification. |

The loader rejects duplicate/unknown keys, wrong types, size/depth overflow, path traversal, URL userinfo and invalid bindings. Non-loopback origins require HTTPS. HTTP is allowed only for canonical loopback hosts. Google generate paths are literal model routes: the codec checks their model against `Request::model` and permits the fixed streaming `?alt=sse` suffix. Descriptors cannot script authentication, expand environment variables, interpolate prompts or merge arbitrary JSON bodies. Host endpoint allow-lists are a host responsibility; descriptor validation alone does not decide who should receive a credential.

Pass credentials separately:

```cpp
#include <cstdlib>
#include <runtime/client.h>

sp::runtime::Options options;
if (const char* key = std::getenv("PROVIDER_API_KEY"))
    options.api_key = key;  // Never print this value.
// Construct Client with an already admitted HTTPS descriptor and these options.
```

The runtime sends Bearer authorization for Chat/Responses, `x-api-key` for Messages and `x-goog-api-key` for Generate/Interactions. It does not fetch or refresh credentials. An empty key is valid for credential-free loopback use; it is not a promise that a hosted service accepts unauthenticated requests. Messages API-version headers come from admitted descriptor/family facts. Do not use literal headers to smuggle credentials into configuration.

For optional Anthropic deployment headers, use explicit host preprocessing before descriptor admission:

```cpp
#include <descriptor/descriptor.h>
#include <descriptor/policy.h>

// descriptor_json is your complete Messages descriptor, not a request body.
sp::descriptor::DeploymentHeaderEnvironment environment;
environment.anthropic_beta = "your-approved-beta";
auto loaded = sp::descriptor::load_with_deployment_headers(
    descriptor_json, {}, environment, sp::descriptor::builtin_policy());
// Inspect ConfigError or consume the returned ValidatedDescriptor as above.
```

`load_with_environment_headers(descriptor_json, overrides)` reads `ANTHROPIC_WORKSPACE_ID` and `ANTHROPIC_BETA` as a host convenience. Only Messages gets these optional values; absent or empty values are omitted. Literal descriptor headers outrank environment values, and explicit `overrides` outrank both, case-insensitively. Duplicate override names, reserved headers, control characters and invalid values reject before admission. The deterministic helper avoids process-environment dependence. Neither helper reads environment in the encoder, mutates an admitted descriptor or executes a JSON template. Keep API keys in runtime options, not these headers.

There are two independent immutable policy snapshots:

| Inputs | Loader | Consumer |
|---|---|---|
| `config/runtime-defaults.json`, `config/error-policy.json` | `configuration::load_runtime_policy(runtime_json, error_json)` or `load_runtime_policy_files(runtime_path, error_path)` | Construct `runtime::Options(snapshot)` and then a new `Client`. |
| `config/descriptor-policy.json`, `config/codec-defaults.json` | `descriptor::load_policy(family_json, resource_json)` | Call `descriptor::load(descriptor_json, snapshot)`. |

Both loaders return a variant of snapshot or `descriptor::ConfigError`. Embedded snapshots are the default; they are generated during SDK configuration. External loads are explicit and bounded. Files are source inputs, not files that every installed process must discover. Existing clients/descriptors retain their admitted policy; loading a replacement affects only newly constructed values.

Family/model defaults are resolved before encoding. An unset request optional uses its admitted default, which may also be unset. An explicit value replaces that default; false and allowed zero values remain distinct from absence. Output caps must be positive, so explicit zero is rejected there. Caller caps are not silently clamped: an admitted model limit or representation/resource bound rejects an incompatible value. No policy snapshot or canary profile grants financial authority.

## Request families and controls

`sp::runtime::Request` is a variant of the five structs below. A request must match the client's descriptor family; a mismatch fails before network dispatch. Assign declared fields rather than using a generic `extra_fields` map.

| Request type / header | Family-specific fields besides model, messages and tools |
|---|---|
| `sp::chat::Request`, `<codecs/chat.h>` | `temperature`, `top_p`, `max_output_tokens`, scalar `reasoning_effort`, `service_tier`, `provider`, `response_format`, OpenRouter `reasoning`, `include_reasoning`, `usage_include`, alternative `models`; choose either simple `messages` or ordered `canonical_messages`, never both. |
| `sp::messages::Request`, `<codecs/messages_request.h>` | `system`, `account_scope`, **`max_tokens`**, `thinking_budget`, `thinking_mode`, `output_effort`, `cache_control`, `tool_choice`, `temperature`, `top_p`, `provider`. |
| `sp::responses::Request`, `<codecs/responses_request.h>` | `instructions`, `account_scope`, `max_output_tokens`, `reasoning` (`effort`, `summary`), `required_tool`, `service_tier`, `temperature`, `top_p`, `store`, `response_format`, `provider`, `hosted_tools`, `max_tool_calls`, `previous_response_id`, `previous_response_history`, `parallel_tool_calls`, `verbosity`, `truncation`, `include`. |
| `sp::gemini::Request`, `<codecs/gemini_request.h>` | `system`, `account_scope`, `max_output_tokens`, `thinking_budget`, typed `thinking_level`, `include_thoughts`, `temperature`, `safety_settings`, `tool_choice`, `required_tool`, `history_mode`. |
| `sp::interactions::Request`, `<codecs/interactions_request.h>` | `system`, `account_scope`, `max_output_tokens`, `thinking_level`, `thinking_summaries`, `service_tier`, `required_tool`. |

This lists the C++ surface, not every field's availability on every model. Admitted enum/range/model facts and compiled family constraints still apply. Generate supports temperature; Interactions does not expose temperature/top-p. OpenRouter controls are admitted only at declared origins for the corresponding family, not arbitrary compatible URLs. Explicit model-prohibited temperature rejects before I/O. Messages enabled thinking has a separately documented omission rule, described below.

All generic-family messages use ordered `sp::Message::parts`:

```cpp
#include <runtime/client.h>

sp::Message user;
user.role = sp::Role::User;
user.parts.emplace_back(sp::Text{"Explain why missing usage differs from zero."});

sp::messages::Request request;
request.model = "your-selected-model";
request.system = "Answer in one sentence.";
request.messages.push_back(user);
request.max_tokens = 128;  // This family uses max_tokens in C++.
```

`sp::chat::InputMessage` is the shorter text/tool/image path. Its image vector is emitted in vector order before nonempty text. Use `canonical_messages` for caller-controlled Text/Image part order or retained native history.

Structured output uses `sp::ResponseFormat`, not a descriptor program. Tool schemas are owned immutable `sp::json::Document` values. This fragment creates a Responses request with a JSON-object response format:

```cpp
sp::responses::Request request;
request.model = "your-selected-model";
request.messages.push_back(user);  // user is the Message above.
request.max_output_tokens = 256;
request.store = false;
request.response_format = sp::ResponseFormat{};  // Kind::JsonObject
```

For `Kind::JsonSchema`, provide `name`, `schema` and optional `strict`; the encoder validates the supported schema keyword types and bounds. It does not validate a model's returned text against that schema. Refusal remains a `Refusal` part, not repaired JSON. Responses hosted tool variants include `WebSearchTool`, `ImageGenerationTool`, `FileSearchTool`, `ToolSearchTool` and `ShellTool`. Tool admission and bounded invocation facts are family-specific; hosted tools do not become host-executable `ToolCall`s merely because they have a name.

## Reasoning, sampling and tool controls

Chat's OpenRouter reasoning object is `sp::chat::ReasoningOptions` with optional `effort`, `max_tokens`, `exclude` and `enabled`. It is distinct from scalar `Request::reasoning_effort`. `include_reasoning`, `usage_include` (wire `usage.include`) and alternative `models` also require a declared OpenRouter origin. For example, given an admitted OpenRouter Chat descriptor:

```cpp
sp::chat::Request request;
request.model = "your-primary-model";
request.messages.push_back({sp::Role::User, "Explain the result briefly."});
request.max_output_tokens = 256;
sp::chat::ReasoningOptions reasoning;
reasoning.effort = "high";
reasoning.exclude = false;
request.reasoning = reasoning;
request.include_reasoning = true;
request.usage_include = true;
request.models = {"your-alternative-model"};
```

These fields choose wire controls; they do not authorize a hidden client retry or guarantee a gateway's backend selection. Retained OpenRouter reasoning fragments and cache/reasoning counts stay in the owned outcome. Counts still obey reported/derived and unknown-versus-zero rules.

A declared OpenRouter origin may answer Responses and Messages requests with a different concrete `model` (a routing alias such as `~vendor/model-latest` resolves to a dated or provider-specific slug). The codec accepts any nonempty served model there, rejects a served model that changes within one response, and records it only in the raw wire envelope; replay keeps the requested model. Other origins require the response to echo the requested model.

Messages has three `ThinkingMode` values. `Manual` needs a positive effective budget at least the admitted minimum and below `max_tokens`; an effective budget without a mode selects manual thinking. `Adaptive` and `Disabled` forbid an explicit budget and clear an inherited budget. The SDK's Messages encoding rule omits temperature when manual/adaptive thinking is enabled, after rejecting explicit prohibited-model temperature or an invalid range. Thinking `top_p` bounds still apply. Vendor model-specific rules can differ; this names the current SDK contract rather than a universal guarantee about every model.

```cpp
sp::messages::Request request;
request.model = "your-selected-model";
request.messages.push_back(user);  // Ordered Message from the earlier example.
request.max_tokens = 256;
request.thinking_mode = sp::messages::ThinkingMode::Adaptive;
request.output_effort = sp::messages::OutputEffort::High;
request.cache_control = sp::messages::CacheControl{sp::messages::CacheTtl::FiveMinutes};
request.tool_choice = sp::messages::ToolChoice{sp::messages::ToolChoiceMode::Auto};
```

`OutputEffort` is Low/Medium/High/Max. `CacheControl` requests ephemeral cache control with optional `CacheTtl::FiveMinutes`/`OneHour`; unset TTL emits no duration. This does not promise a cache hit or known cache usage. Tool choice is Auto/Any/None/Tool with an optional parallel-use flag. Named Tool requires a declared client function. The SDK currently rejects forced Any/Tool with enabled thinking, and None cannot carry the parallel-use flag. `provider` routing requires an admitted OpenRouter Messages origin.

Generate's `ThinkingLevel` is Minimal/Low/Medium/High, mutually exclusive with an explicit `thinking_budget`. `ToolChoice` uses Auto/Any/None/Validated and optional `allowed_function_names`; names must be declared and unique, and allow-lists apply only to Any/Validated. Do not combine it with `required_tool`.

```cpp
sp::gemini::Request request;
request.model = "your-literal-model";
request.messages.push_back(user);
request.max_output_tokens = 256;
request.thinking_level = sp::gemini::ThinkingLevel::Low;
request.temperature = 0.5;
request.safety_settings = {{
    sp::gemini::SafetyCategory::Harassment,
    sp::gemini::SafetyThreshold::BlockMediumAndAbove
}};
request.tool_choice = sp::gemini::ToolChoice{sp::gemini::ToolChoiceMode::None};
```

Safety categories are Harassment/HateSpeech/SexuallyExplicit/DangerousContent/CivicIntegrity; thresholds are BlockNone/BlockOnlyHigh/BlockMediumAndAbove/BlockLowAndAbove/Off. Duplicate categories and invalid values reject. These are explicit service controls, not a safety guarantee about returned content.

The immutable family policy's `temperature_forbidden_model_prefixes` supplies model facts. Matching is ASCII case-insensitive against the full model or the suffix after its final `/`, so a gateway namespace does not bypass a prohibition. The embedded OpenAI facts cover gpt-5/gpt-6/o1/o3/o4 prefixes; Messages facts cover the admitted newer Claude families. Loading a different closed policy creates a new identity, not a live edit to an existing client's behavior. Read the admitted facts for the selected deployment; do not guess temperature support from a model name alone.

## Responses provider-held continuation

Responses supports both full client-held native replay and the distinct `previous_response_id` cursor path. A cursor asks the provider to retrieve its own prior conversation state. With a cursor, `messages` contains **new input only**; including captured assistant messages/native output there rejects before dispatch. `previous_response_history` is local validation evidence and is never emitted. A cursor string or raw response JSON is not `NativeReplay` or archive authority.

`parallel_tool_calls` is optional bool. `verbosity` uses `Verbosity::{Low,Medium,High}` and writes `text.verbosity`; `truncation` uses `Truncation::{Disabled,Auto}`. `include` is an optional vector of `Include` values: ReasoningEncryptedContent/WebSearchSources/FileSearchResults/MessageOutputTextLogprobs/ComputerCallOutputImageUrl/CodeInterpreterCallOutputs. Absence preserves the encrypted-reasoning include default; an explicit vector, including an empty vector, is honored. Duplicate/invalid selections reject. Selecting diagnostics does not make an unsupported response item executable or grant native replay when required encrypted state was not returned.

For a text cursor continuation, keep the original request and successful terminal result. Choose `store`, `parallel_tool_calls`, `verbosity`, `truncation` and `include` on that original request, before capture; changing bound controls during continuation rejects. A returned id alone is no service-state availability promise:

```cpp
// original is the sp::responses::Request used for the first completed call.
// first is its successful owned result; terminal is the completed response group.
const auto& terminal = std::get<sp::Completion>(*first).messages.back();
auto next = original;  // Keeps model, instructions, tools and effective controls.
next.previous_response_id = terminal.id;
next.previous_response_history = original.messages;
next.previous_response_history.push_back(terminal);  // Full original prefix, not assistant-only.
sp::Message followup;
followup.role = sp::Role::User;
followup.parts.emplace_back(sp::Text{"Continue with one concrete example."});
next.messages = {followup};  // Only this new message is emitted as input.
auto second = client.complete(std::move(next), run);
```

Plain server-held text state may use no `previous_response_history`. Client tool results require genuine completed ownership evidence: for an initial captured completion, provide its complete original request prefix plus terminal assistant group with `id == previous_response_id`. For a later cursor response, only that genuine in-process completed terminal group supplies its private cursor ownership evidence. Match origin/model/configuration and unedited content; failures, fabricated history, wrong ids or mismatched seals reject. Tool results still name exactly one declared pending client call, and all pending calls need results before sending.

Cursor-generated messages retain a separate in-process terminal-ownership scope, not complete client-held native history: `NativeReplay::complete()` is false and archive/full-native replay is ineligible. No automatic retrieval, transcript backfill, cursor-not-found resend or fallback to full-input replay occurs. If the provider no longer has the referenced response, inspect the actual Failure and decide a new host operation explicitly; do not infer no billing from a missing cursor. Buffered/SSE terminal evidence, failure partials and deadline/cancel rules remain unchanged.

[OpenAI's conversation-state guide](https://developers.openai.com/api/docs/guides/conversation-state) illustrates `previous_response_id` with new input and notes that earlier inputs in a chain still contribute to billing. Sending less history on the wire is not a zero-cost continuation claim.

## Explicit portable Gemini history

Generate defaults to `HistoryMode::NativeOnly`, which requires genuine eligible native assistant history. Set `HistoryMode::PortableForeign` only when deliberately importing portable history. It admits caller-created assistant Text and client ToolCall parts with no native sidecar, message wire output, call wire type/metadata, signatures or other native parts. For example:

```cpp
sp::gemini::Request request;
request.model = "your-literal-model";
request.max_output_tokens = 256;
request.history_mode = sp::gemini::HistoryMode::PortableForeign;
sp::Message imported;
imported.role = sp::Role::Assistant;
imported.parts.emplace_back(sp::Text{"A portable earlier explanation."});
request.messages.push_back(imported);
request.messages.push_back(user);
```

Imported text receives no signature. An imported function call must have a unique id, declared name and immutable object arguments, followed by its matching ToolResult. The encoder adds Google's `skip_thought_signature_validator` sentinel only to the first foreign function call in an imported assistant message, never to text-only or genuine native groups. This explicitly requests the service's imported-history bypass; it does not prove the imported call came from that service or mint a seal for it.

[Google's thought-signature guidance](https://ai.google.dev/gemini-api/docs/generate-content/thought-signatures.md.txt) describes the imported/client-generated call exception and discourages its use in place of genuine signatures. The SDK requires the explicit typed mode and never uses that exception to repair authentic carry.

An authentic same-route native group still follows exact native validation in either mode. A failed, edited or mismatched native seal is never stripped, demoted or repaired by switching modes. Imported content enters the ordinary prefix binding for newly generated output; any new response's authority comes from its actual completed runtime generation, not from an imported message. This Generate-only path is distinct from historical cross-origin equivalence and general Drop/Demote treatments.

## Prepare once, then dispatch

The SDK names its send operations `start` and `complete`. NeoGraph calls the corresponding adapter operation `dispatch` (see [Migration](MIGRATION.md#the-neograph-adapter-boundary)). Preparation is useful when a host needs pre-effect admission:

```cpp
// client and request are constructed as in README; run carries the original deadline.
auto prepared = client.prepare(std::move(request), run);
if (const sp::Error* error = prepared.error()) {
    // A semantic/native/configuration preflight error: no request has been sent.
    std::cerr << error->safe_message << '\n';
    // Consume it to obtain the same owned Failure representation.
    auto rejected = client.complete(std::move(prepared));
} else {
    // Inspect family(), model(), max_output_tokens(), deadline(), limits() here.
    // If your host requires a durable receipt/budget claim, persist it here.
    // Do not log encoded_body(): it can contain sensitive native state.
    auto result = client.complete(std::move(prepared));
}
```

`valid()` says the handle has state; `error()` says whether that state contains an initial rejection. A valid handle can therefore carry a preflight error. The handle owns borrowed accessor views; do not retain those views after moving or destroying it. Successful preparation does not guarantee later dispatch: the deadline can expire, cancellation can arrive or the client can shut down before `start`.

`prepare()` reserves one of `Options::limits.max_operations` slots shared by prepared and active operations. Capacity/shutdown/moved-client rejection throws `AdmissionError`; no operation or callback is accepted. `start(PreparedRequest)` requires a live preparation belonging to that same client. A moved-from/default/foreign handle is misuse. Destroying an unconsumed preparation releases its slot without sending, timers or callbacks.

The direct paths call preparation internally:

```cpp
auto result = client.complete(std::move(request), run);  // blocking caller
// Or: auto operation = client.start(std::move(request), run, callbacks);
```

`complete()` translates `AdmissionError` to its owned `Failure` outcome. `prepare`/`start` callers catch `AdmissionError` and can retain `error.outcome()`. Linked-interface mismatch remains `InterfaceContractError`, not a successful provider result. `Client` construction and dispatch perform the out-of-line revision/capability gate; explicitly calling it at application startup makes dependency mismatch visible before doing host work. It checks loaded libraries, not only header constants, and is not a general ABI-safety guarantee.

## Streaming, deadlines, cancellation and backpressure

`RunOptions::streaming` defaults to true. A callback's presence does not choose HTTP/SSE mode. You may stream internally and collect only the final outcome, or choose buffered mode explicitly as in README.

```cpp
#include <atomic>
#include <chrono>
#include <memory>
#include <stop_token>
#include <runtime/client.h>

// client and typed request are already constructed.
std::stop_source stop;
sp::runtime::RunOptions run;
run.streaming = true;
run.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
run.stop_token = stop.get_token();
auto text_bytes = std::make_shared<std::atomic<std::size_t>>(0);
sp::runtime::Callbacks callbacks;
callbacks.on_event = [text_bytes](const sp::Event& event) {
    if (const auto* delta = std::get_if<sp::PartDelta>(&event)) {
        if (delta->payload.kind == sp::PartKind::Text)
            text_bytes->fetch_add(delta->payload.bytes.size(), std::memory_order_relaxed);
    }
};
auto operation = client.start(std::move(request), run, std::move(callbacks));
// Another thread may call stop.request_stop(); or call operation.cancel().
auto result = operation.join();  // Use a caller thread, never a library callback.
```

`on_event` receives nonterminal borrowed views. Copy only the data you need into your own bounded queue before returning. Callbacks are serialized per operation on shared runtime workers, not the caller's event loop. They must not block. `on_outcome`, if supplied, receives the retainable `shared_ptr<const Outcome>` exactly once after events. `join()` fences callback return, callback storage destruction and slot release; calling a blocking operation from a library worker yields a `Misuse` result rather than deadlocking that worker.

An absolute `steady_clock` deadline includes preparation time, retained-handle waiting, DNS/connect/TLS, request/response I/O and any retry delay. Omitting it computes `now + Options::default_timeout` before encoding in `prepare`; moving the handle does not renew it. A stop requested before preparation or an already expired deadline is inspectable as an initial error; expiry during preparation is still checked before sending. Later cancellation cancels local work without waiting for peer progress. It cannot promise the remote service stops generating or billing. If the outcome was decided before cancellation was observed, that terminal outcome remains.

### Stall bounds for long generations

The deadline is the only time bound by default, so a stream that keeps its connection open and trickles bytes, or goes silent after its headers, runs until the whole deadline expires. That is acceptable at the 30 s default and costly once you raise the deadline for a long generation. Three optional bounds detect a stalled peer without touching the deadline. All are `std::chrono::milliseconds`, **off by default** (zero), and each is measured from the start of every attempt:

| Bound | Fires when | Outcome |
|---|---|---|
| `connect_timeout` | the connection (TCP and TLS) is not established in time | `Failure(Transport)`, `retry_safety == NotSent` |
| `first_byte_timeout` | no response header line arrives in time (includes connect, upload and the server's think time) | `Failure(Transport)`, `NotSent` before possible dispatch, otherwise `PossiblyAccepted` |
| `idle_timeout` | no byte moves in either direction for this long: request headers/body sent, response headers, body chunks and SSE comments or keep-alives all refresh activity | `Failure(Truncated)` once the response head was seen, else `Failure(Transport)` |

Set a client-wide default in `Options::transport`, and override it for one call in `RunOptions` (unset inherits, zero disables for that call). These bounds overlap: idle detection starts with the attempt, not after the first byte. A run with a long overall generation budget but bounded connection, initial response and stream silence:

```cpp
#include <chrono>
#include <runtime/client.h>

sp::runtime::Options options;                          // client-wide defaults
options.transport.connect_timeout = std::chrono::seconds(5);
// options.transport.idle_timeout = std::chrono::seconds(60);   // or for every call
sp::runtime::Client client(std::move(descriptor), options);

sp::runtime::RunOptions run;
run.streaming = true;
run.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);  // long generation
run.first_byte_timeout = std::chrono::seconds(20);     // response headers must arrive within 20 s
run.idle_timeout = std::chrono::seconds(30);           // includes pre-head silence; later SSE heartbeats count
auto result = client.complete(std::move(request), run);
```

Pick an `idle_timeout` comfortably above the longest silence a healthy stream can have, including a reasoning model's pause between events; SSE comments and pings from the server count as activity. The bounds never extend the deadline and are never later than it: when the deadline is earlier the outcome stays `DeadlineExceeded`. A stall is not `DeadlineExceeded`, because you still have time budget (should the deadline pass before the runtime has consumed a stall result that is already queued, the deadline wins); it is a retryable `Transport` or `Truncated` failure whose `retry_safety` and `attempt` evidence are exactly those of any other failure at that point, including a stream cut inside a multi-byte UTF-8 character. After the request may have been accepted, an automatic retry still needs the duplicate-billing opt-in, and once semantic output was observed there is no retry and the partial message stays on the `Failure`. Values must be non-negative and not exceed the `default_timeout_ms` admission ceiling: bad client-wide values throw `ConfigError` from the `Client` constructor, bad per-run values give an `InvalidRequest` failure before anything is sent. Pausing delivery through backpressure (a slow consumer) is not counted as peer silence.

`Operation` is move-only. Destruction requests nonblocking cancellation; it does not wait for an already running callback. `detach()` relinquishes the handle without cancellation, so use it only when another owner will observe completion. Destroying a client requests shutdown and drains off-worker; retain outcomes independently of the client.

Wire queues have chunk and byte bounds (`queued_body_chunks`, `queued_body_bytes`), separate response/error limits and semantic/SSE bounds. The runtime pauses transport delivery while its actor drains the bounded queue. This does not make blocking callbacks safe or bound a consumer's own unbounded queue. Limit exhaustion produces a typed failure. Slow-callback and callback-exception counters are available through `Client::diagnostics()`; they report misuse, not preemption.

A throwing nonterminal callback cancels with `Failure(Misuse)`. A throwing terminal callback increments diagnostics but cannot replace the already decided outcome or create a second result. Keep callback errors distinct from provider failures; never redispatch merely because downstream delivery failed.

Retries are off by default. `RunOptions::retry` can replace the client's admitted default policy. If enabled, the runtime is the single retry owner with an attempt cap, shared token bucket and unchanged deadline. Retry class and safety are separate; no observed semantic output is retried, and possible acceptance requires explicit duplicate-billing-risk approval in the current policy. Retry-After cannot shorten a server wait to fit the deadline. `AttemptEvidence::prior_usage_unknown` keeps unknown earlier cost visible. If NeoGraph or another host owns retry, leave SDK retry off.

## Outcomes, raw data and nullable usage

Retain the result owner, then inspect its variant. A `Failure` retains `PartialCompletion`: already observed messages, nullable usage, optional stop, wire envelope and raw observations. Partial text is not a successful completion and partial tool fragments are not executable calls.

```cpp
#include <iostream>
#include <json/json.h>
#include <runtime/client.h>

void inspect(const sp::runtime::Result& result) {
    if (const auto* failure = std::get_if<sp::Failure>(result.get())) {
        const auto& error = failure->error;
        // Log only safe_message and approved scalar classification, not raw bodies.
        std::cerr << error.safe_message << " (HTTP " << error.http_status << ")\n";
        const auto& partial = failure->partial;
        // Retain result if partial.messages/raw_events must outlive this call.
        (void)partial;
        return;
    }
    const auto& completion = std::get<sp::Completion>(*result);
    const auto& usage = completion.usage;
    if (usage.input_total) {
        auto tokens = usage.input_total->value;  // uint64_t, including known zero
        auto evidence = usage.input_total->evidence;  // Reported or Derived
        (void)tokens;
        (void)evidence;
    } // Absence remains unknown: do not value_or(Count{}) for billing.
    for (const auto& raw : completion.raw_events) {
        auto owner = raw.payload;  // Shared Document owner can be retained.
        if (owner) {
            auto view = owner->root();  // Borrowed only while owner lives.
            (void)view;
        }
    }
}
```

`Usage` distinguishes input/output/total, provider-reported total, uncached input, cache reads/writes and reasoning counts. `stage` is Missing/Partial/Final; `quality` is Consistent/Inconsistent with conflict details. Do not sum cache/reasoning bands into totals again without applying that family's inclusion rules. Do not substitute reported totals with derived ones or turn an absent count into zero. Error `attempt` and Completion `attempt` carry dispatched-attempt evidence, independent of token usage.

`Completion::raw_events` and `wire_envelope` retain owned JSON observations; failures keep their partial equivalents. A bounded non-2xx body may appear as `RawWire` type `http.error`, a diagnostic label rather than vendor message type. Retention is bounded by configured resources, not an unlimited wire dump. `Document` owns JSON; `Value`, string views and iterators borrow it. Raw JSON is not automatically safe to log, nor does it grant native replay authority. Text extraction is a presentation projection: keep the original outcome for storage/accounting/native continuation.

Inspect `StopReason::kind` and `raw` rather than assuming all completions mean natural end. Length, refusal, filter, tool use and server pause retain different meanings. Execute only a sealed `sp::ToolCall` with `kind == ClientExecuted` under your host's tool policy. `InvalidToolCall`, `ServerExecuted` and `ApprovalRequest` do not authorize tool execution.

At the semantic event boundary, an otherwise natural `EndTurn` with non-server tool-call intent becomes `ToolUse`, including calls whose arguments later remain `InvalidToolCall`. The Stop event and final outcome use the same normalized kind. Specific MaxTokens/filter/unknown stops remain specific, and server-executed tools alone do not create client execution intent. A ToolUse stop never repairs malformed arguments into an executable call.

## Native continuation and persistence

A completed `sp::Message` can own both typed parts and an immutable `NativeReplay` sidecar. Native state includes signed thinking, encrypted reasoning and thought signatures that cannot be reconstructed safely from portable text or editable JSON. Keep the whole ordered message, unchanged, with its sidecar and wire output.

For a Responses tool continuation, retain the original request prefix, tool declarations and controls, then append the real completed messages and a tool result:

```cpp
// first is a successful result of client.complete(request, run).
// request is the original sp::responses::Request with the declared client tool.
const auto& completion = std::get<sp::Completion>(*first);
request.messages.insert(request.messages.end(),
                        completion.messages.begin(), completion.messages.end());
sp::Message tool_result;
tool_result.role = sp::Role::Tool;
tool_result.parts.emplace_back(sp::ToolResult{call_id, "42", false});
request.messages.push_back(std::move(tool_result));
auto second = client.complete(std::move(request), run);
```

Here `call_id` must be the genuine completed client `ToolCall::id`, and `"42"` is an illustrative authorized host tool result. The host must select/check the call before execution. Keep the complete original request prefix before the first consuming call, then append returned messages; assistant-only history, including archive-restored assistant history without its original prefix, is insufficient. Reuse admitted descriptor/model/scope and bound reasoning/tool/content controls. Output generation cap is per-call admission and may change without changing replay configuration; its new effective value still needs resource and host budget admission. Preserve the original deadline for one host budget; a new deadline belongs only to a separately admitted operation.

The native gate checks descriptor/origin, model, original prefix, bound control/scope and completed content. Edited/reordered carry, incomplete/invalid calls and foreign native origins reject before sending. Output cap and documented per-turn tool selection/cursor behavior are not permission to edit bound reasoning or content. Local retention/remote acceptance does not prove vendor cryptographic validation. The explicit Generate portable-history path above imports no native authority; general cross-origin equivalence/Drop/Demote remain unavailable.

In-memory continuation requires no archive. Durable genuine native history uses `<core/native_archive.h>`:

- `NativeArchive::provision(directory, independent_key_file, owner_scope, descriptor)` creates protected owner-private custody explicitly.
- `NativeArchive::open(...)` opens existing custody without renewing/resetting it.
- `save(messages, binding)` returns a variant of opaque reference or `Error`; `load(reference, binding)` returns messages or `Error`. Check every arm.

Archive v3 records and `spna3` references authenticate **local custody**, using an independent key and descriptor/owner/binding facts. They are not encryption, export bundles, vendor-issuer authentication or financial grants. Old v2 references reject rather than upgrading. Protect directory and key separately; never publish native leaves. JSON imports and portable message exports cannot mint the sidecar. NeoGraph owns durable checkpoint/budget integration; the archive does not create graph or dispatch authority.

Interface 4 does not rename this archive format: it remains v3/`spna3`. The new admitted policy/control identity can make older policy-bound archive records honestly `ReplayIneligible` even though their storage format is readable. Format equality is not replay compatibility; do not rebind, relabel or repair old native records to make them pass.
