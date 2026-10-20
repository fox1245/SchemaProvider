# Migrating to interface 7

Use the [current first-request example](../README.md#first-request) before adapting an older caller. Interface 7 keeps the typed outcome/prepare, monetary-usage and stall-bound contracts and replaces image-only input with typed media. Rebuild with matching headers and loaded generation 7 libraries. Earlier migrations remain historical steps; migrate removed callers rather than adding an adapter or shim.

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

## From interface 6 to 7

Package 0.4.0 alpha carries interface revision 7 and shared-library generation 7 (`libsp_*.so.7`); CMake consumers select `SchemaProvider 0.4.0 EXACT`. Replace `<core/image.h>` with `<core/media.h>`, `sp::Image` with `sp::Media`, and `chat::InputMessage::images` with `media`. Construct an image with `Media::image(mime, shared_base64, detail)` rather than the old aggregate layout. Generic `Message::parts` still preserves caller order; its variant now holds `Media` at the former image index. No image-header/type/field alias is retained.

`Media` distinguishes Image/Audio/Video/Document and Inline/Url/File, with MIME, shared immutable base64 or a provider reference, optional name, image detail, generated identity and audio transcript. Do not treat every media part as an image or dereference `data` for URL/file sources. Generated media may have an unknown (empty) MIME when the provider and request do not identify it; do not fabricate a format from the kind. An unsealed partial media part has no payload.

`ToolResult` adds ordered `content_parts` (`std::vector<std::variant<Text, Media>>`). Keep legacy `content` empty when this vector is nonempty; do not flatten typed output back to a string. Responses and Messages support representable text/image/document results, Interactions text/images, and Generate text plus inline image/audio/video, PDF and plain-text bytes (verified live on Gemini 3; earlier models may reject multimodal function responses). Chat rejects nonempty typed tool results with `Unsupported`. Generate still requires one JSON object for legacy `content`; typed text uses `functionResponse.response.output` (or `error`) and media uses the separate ordered `parts` lane. Alternating typed text/media is accepted and grouped into these lanes; cross-lane positions are not represented, so callers must not rely on interleaving. No synthetic references are invented, and named blobs are unsupported. Family/source/metadata restrictions and ordinary tool ownership remain enforced; see [media usage](USAGE.md#media-inputs-and-generated-outputs).

Chat adds `modalities`, `audio` and OpenRouter `image_config`; Generate adds `response_modalities`, `speech_config`, `image_config` and `audio_transcription_config`; Interactions adds typed `response_format`, `speech_config` and `transcription_config`. Responses `ImageGenerationTool` adds `output_format`. These controls join native configuration bindings: changing one does not grant replay authority under the old configuration. Use the modern nested Interactions transcription mode, not deprecated top-level diarization/timestamp fields.

Generate's family `include_thoughts` default and Interactions' family `thinking_summaries` default are now null. An unset request emits no such control unless its exact admitted model policy supplies one; known thinking-model defaults remain intact (the embedded `gemini-2.5-flash-lite` policies retain their respective `true` defaults). Explicit `true` and `false` still override inherited values. Do not turn this family-default correction into a blanket disabling of thinking.

For the observed OpenAI Chat audio SSE variation, final usage plus validated complete audio, `audio.expires_at`, `[DONE]` and normal close can derive `EndTurn` without a provider `finish_reason`. The raw stop is `derived:audio_usage+audio.expires_at+DONE`, not a fabricated vendor reason. Metadata heartbeats may omit `choices`; the permitted late omission of `finish_reason` is limited to the expiry-only audio tail after final usage. Ordinary streams retain strict finish-reason requirements. Incomplete or abnormally closed prefixes remain failures and cannot gain complete native authority.

Closed descriptor-policy data now includes per-kind `media` MIME/source lists and optional declared-origin `openrouter_media`; codec resources add audio/video/document decoded-byte bounds. Update external snapshots together with their matching SDK instead of passing unknown keys to an older loader. Resource exhaustion on media requests returns `ResourceLimit`; malformed payloads remain `InvalidRequest`, and unrepresentable family/source forms remain `Unsupported`.

Archive v3 writes the extended media layout with tag 12 and typed `ToolResult` content with tag 13; legacy tag 10 reads inline images, and legacy tag 7 reads string-only tool results. These storage readers are not a native-policy migration: changed policy identity, configuration or content can still make a restored generation replay-ineligible. Retain authenticated native history for generated media and annotations; copying payloads or reading an old archive does not grant replay authority. Never rehash old records to manufacture compatibility. See [media usage](USAGE.md#media-inputs-and-generated-outputs) for current family boundaries.

Usage fidelity repairs keep layouts and the interface revision. `CostRounding` gains `CeilingShortestDecimal`, now the produced and default value; `CeilingParsedBinary64` remains declared but is no longer produced, so update comparisons and exhaustive switches. Exact decimal costs such as `1.5e-06` USD are `1500` nanoUSD (previously `1501`). OpenRouter Messages now projects `provider_cost` and accepts the one `[DONE]` frame that gateway sends after `message_stop`; direct origins and any other post-terminal data stay corrupt. Direct Anthropic admits an undated alias served as its dated snapshot (`claude-haiku-4-5` as `claude-haiku-4-5-20251001`) with requested-model replay binding, like the direct OpenAI rule. `Usage::extra` gains Chat's unlisted integer leaves; Interactions drops typed aggregates and renames positional modality keys (`input_tokens_by_modality.0.tokens` becomes `input_tokens_by_modality.text`). Generate now derives zero thoughts when the reported total proves them. See [nullable usage](USAGE.md#outcomes-raw-data-and-nullable-usage).

Responses function tools apply the policy `strict_tools` default only to strict-compatible parameter schemas and send `"strict": false` for other schemas, which OpenAI rejected with HTTP 400 under the default. An explicit `strict = true` on an incompatible schema now returns `InvalidRequest` before dispatch, naming the tool and the first failing JSON pointer; an explicit `false` is unchanged. The descriptor policy, its identity and native configuration digests are unchanged. See [request controls](USAGE.md#request-families-and-controls).

The Chat codec binds stream identity to `id` and `model`. Some OpenAI-compatible local servers stamp each chunk with the current time, so `created` may now repeat or increase between chunks instead of failing every stream as `ProtocolCorrupt`; it must still be an unsigned count on every chunk, and a decrease or a changed `id` or `model` remains `ProtocolCorrupt`.

An in-band failure larger than `max_error_bytes` (64 KiB by default) that the codec had already admitted under the response bounds was reported as `ResourceLimit` with `RetryClass::Never`. A Responses `response.failed` after a hosted image, which repeats the multi-megabyte image, hit this. It is now classified from the admitted document like a small one: `RemoteFailure`, or the kind, retry class and `vendor_code` of an admitted error code. `max_error_bytes` still bounds non-2xx bodies and in-band error bytes the runtime parses itself; runtime and error policies are unchanged. See [outcomes](USAGE.md#outcomes-raw-data-and-nullable-usage).

`sp::OpenRouterRouting` gains optional `max_price` (`sp::OpenRouterMaxPrice`, integer nanoUSD amounts sent as exact decimal USD), OpenRouter's cap on the endpoint price it may route to. The Chat, Messages and Responses request layouts grow within interface 7, so rebuild consumers against matching headers and libraries. Routing without a cap encodes the same body and native configuration digest as before, so histories sealed under it still continue; a cap joins the routing binding. Code that copies routing field by field, such as a defaults merge, must copy `max_price` too. The new capability bit `sp::capability::OpenRouterRoutingMaxPrice` is part of `RequiredProvider`, so a consumer built with these headers that calls `require_interface_contract` with it is refused by a library without the field instead of losing the cap. The descriptor policy and its identity are unchanged. See [request controls](USAGE.md#reasoning-sampling-and-tool-controls).

Chat Completions now accepts one `Role::Tool` message that carries several `ToolResult` parts, like the other families, and writes one wire `tool` message for each in order. A Tool message that is empty or holds anything else is still refused, and the refusal names the rule. See [request families](USAGE.md#request-families-and-controls).

Temperature facts are now a rule by generation as well as a list. The family policy gains the optional key `temperature_forbidden_generations` (`[{"series": "claude-", "from": "4.7"}]`): every model of the series from that generation on refuses `temperature`, so a Claude release such as `claude-haiku-5-5` no longer needs a new list entry (it used to be sent and answered with HTTP 400 "`temperature` is deprecated for this model"). The embedded Messages facts also list `claude-haiku-5`. Gateway spellings with a namespace or dots (`anthropic/claude-opus-4.7`) match the same rule, and a Claude id whose generation cannot be read is refused like the newest one. A policy snapshot written before the key still loads and keeps only its listed prefixes. These facts are admission data, so they leave the replay identity unchanged. No public struct or enum changes; the interface revision stays 7. See [request controls](USAGE.md#reasoning-sampling-and-tool-controls).

The Chat output cap is written to `max_completion_tokens` by default on direct OpenAI (`https://api.openai.com`). Current OpenAI models (gpt-5.x and gpt-6.x, the reasoning models) answered `max_tokens` with HTTP 400 "Use 'max_completion_tokens'". Every other origin, OpenRouter and compatible endpoints included, keeps `max_tokens`, and an explicit `bindings.max_output_tokens` wins on any origin. The family policy gains the optional key `max_completion_tokens_origins`, which only `openai.chat` may list; a snapshot written before it keeps `max_tokens` everywhere. The binding member is part of the origin identity of native history, so Chat history sealed against direct OpenAI under the old default becomes `ReplayIneligible` when it is continued with the new one; binding `max_output_tokens` to `max_tokens` explicitly keeps the old origin identity for a conversation that must continue. A vendor 400 on Chat or Responses that names the refused member in `error.param` (`unsupported_parameter` or `unsupported_value`) now says so in `Error::safe_message`, for example `invalid request: unsupported parameter 'max_tokens'`, without the vendor's text. No public struct or enum changes; the interface revision stays 7. See [descriptors](USAGE.md#descriptors-credentials-and-defaults) and [outcomes](USAGE.md#outcomes-raw-data-and-nullable-usage).

A completed HTTP 4xx answer other than 408, 425 and 429 (401, 400, a 404 for an unsatisfiable OpenRouter `max_price` pin) is now `RetrySafety::RejectedBeforeOutput`: the server declined the request, so nothing ran and nothing is billed. It used to be `PossiblyAccepted`, which made a host keep its whole reservation for the call. `PossiblyAccepted` remains for 408, 425, 429, 5xx, answers that were cut off and transport failures after the request left, and `OutputObserved` and `NotSent` are unchanged. `RejectedBeforeOutput` is an existing enumerator, so no public struct or enum changes; the interface revision stays 7. A host that releases a reservation only for `NotSent` must also accept `RejectedBeforeOutput` with an HTTP status to benefit; retry policy treats it like `NotSent`, which changes nothing in practice because the embedded error policy gives no 4xx status a retryable class.

Generate now derives `Usage::output_total` and `total` (both `Evidence::Derived`) when `candidatesTokenCount` is omitted but `promptTokenCount` and `totalTokenCount` are present, which is what the API sends when the output budget is spent while thinking: `output_total` is the total minus the prompt count. Both used to stay unknown, so the call could not be settled from usage. A nonzero tool-use prompt count still leaves them unknown, and impossible counters are a conflict. No public struct or enum changes; the interface revision stays 7. See [nullable usage](USAGE.md#outcomes-raw-data-and-nullable-usage).

The usage of a call that ends early now follows one rule across families: `Usage::stage` is `Missing` with every counter absent when the provider had reported nothing (no evidence, not zero) and `Partial` with the counters seen so far otherwise, never `Final`. Chat and Responses send usage only with their final event, so a cut stream there already had none; a cut Messages stream no longer reports the placeholder output count of `message_start` (`output_total` was `0` or `1` while text had streamed), so it has the input counters, labelled partial, and no output counter. Complete calls are unchanged: `message_delta` supplies the cumulative output count, and a cumulative count below the one in `message_start` is still a conflict. No public struct or enum changes; the interface revision stays 7. See [nullable usage](USAGE.md#outcomes-raw-data-and-nullable-usage).

A thin convenience layer joins the public headers; nothing in it changes a request layout, a wire byte or the interface revision, and the handshake bit set is unchanged, so a consumer only needs headers and libraries from the same build. `sp::descriptor::presets` (`<descriptor/presets.h>`, also included by `<runtime/client.h>`) builds validated descriptors for the OpenAI, Anthropic, Google, OpenRouter and loopback origins through the unchanged `descriptor::load`. `Client(descriptor, api_key)` and `Options::with_api_key` shorten the credential path. `Message::user_text`, `system_text`, `developer_text`, `assistant_text` and `tool_result` build common messages; every family's request gains `Request::simple(model, prompt_or_history, max_tokens, system)`. `Message`, `Completion` and `PartialCompletion` gain read-only projections (`joined_text()`, `joined_thinking()`, `first_refusal()`, `executable_tool_calls()`, `invalid_tool_calls()`), `Usage` gains `*_value()` readers that drop evidence, stage and quality, and `sp::as_completion`, `as_failure`, `as_error`, `is_completion`, `is_failure` and `joined_text` read an `Outcome` or a `Result`. Projections that borrow from their owner are deleted on a temporary, so code that calls them directly on the result of a call must keep that result in a variable first. Existing code that unwraps the variants keeps compiling and behaving as before. See [request families](USAGE.md#request-families-and-controls) and [outcomes](USAGE.md#outcomes-raw-data-and-nullable-usage).

Policy snapshots can be loaded as files, as one bundle and against an expected content digest: `descriptor::load_policy_files`, `load_policy_bundle`, `load_policy_bundle_file` and `load_verified_policy`, with `DescriptorPolicy::content_digest_hex()` and `builtin_policy_bundle_json()`. A nonempty expected SHA-256 that is malformed or differs from the content digest is a `ConfigError` at `/content_digest`. The bundle is the closed object `{"version":1,"descriptor_policy":{...},"codec_defaults":{...}}` and holds the descriptor/codec pair only; runtime and error policy load as before. Updating vendor facts this way does not touch the interface revision. See [policy snapshots](USAGE.md#descriptors-credentials-and-defaults).

Messages now accepts one `Role::Tool` message that carries only `ToolResult` parts, like the other families; it used to answer `Unsupported`. It is written as the same wire `user` message as a `Role::User` message with those parts. The role is part of the message's content digest, so keep the role a result message had when the history was sealed. An empty Tool message, or one that mixes in other parts, is refused as `InvalidRequest`. `Message::tool_result` passes its text through: Generate requires one JSON object there, and `is_error` is encoded per family (Responses refuses `true`; Chat Completions has no field for it). See [request families](USAGE.md#request-families-and-controls).

A streamed Chat Completions choice without `finish_reason` now reads as `"finish_reason": null` while the stream is open. Google's OpenAI-compatible endpoint (`https://generativelanguage.googleapis.com/v1beta/openai/chat/completions`) leaves the member out of every chunk before the last, so every stream from it longer than one chunk failed with `ProtocolCorrupt`; OpenRouter, which sends `null`, was not affected. The stream must still carry a string `finish_reason` (or the audio evidence) before `[DONE]`, a buffered choice without the member and a chunk after the stop reason without it are still `ProtocolCorrupt` (the audio expiry delta keeps its own exception), and no other member is read this way. No layout or interface revision changes. See [frame classes](DESIGN.md#41-frame-classes).

`on_event` no longer receives a text `PartDelta` with empty text. OpenAI Chat opens a stream with `"content":""`, a Messages text block starts with `"text":""`, a Gemini answer usually ends with a part whose text is `""` and Responses repeats the whole text at `output_text.done`; each of those used to arrive as a delta without text, so a caller that counted chunks, or took the first text delta for the first token, was off by one. The accumulator still applies the event: the part exists, keeps its index and order and seals as before, a text part that only ever received empty text is the empty `Text` a buffered response returns, and `PartBegin` and `PartSeal` are delivered either way. Code that waited for text rather than for the first delta (the advice that came with the old behaviour) keeps working. Thinking, refusal, tool-call and signature deltas are delivered as the wire gives them. No layout or interface revision changes. See [streaming](USAGE.md#streaming-deadlines-cancellation-and-backpressure).

`descriptor::load` now holds a `google.generate` descriptor to the route shape the encoder accepts: the buffered path `/v1beta/models/<model>:generateContent` and the streaming path `/v1beta/models/<model>:streamGenerateContent?alt=sse` for the same `<model>`, made of letters, digits, `-`, `.` and `_`. Any other pair used to load and then failed every request, streamed or buffered, with `InvalidConfig` at dispatch (a streaming path copied from the buffered one is the usual cause); it is now a `ConfigError` at `/connection/paths/buffered` or `/connection/paths/streaming`. A descriptor that could serve a request is unaffected, and so are the presets, the other families and the embedded policy and its identity. No layout or interface revision changes. See [descriptors](USAGE.md#descriptors-credentials-and-defaults).

`sp::Error` gains `reason`, a stable machine-readable code for the check of the SDK that failed (`responses.event.unsupported`, `chat.finish_reason.invalid`, `sse.utf8.invalid`). Every `ProtocolCorrupt` used to reach the caller as `invalid response protocol` with nothing to tell the checks apart, so a field report could not say what had failed. The code is lower-case dotted segments from a fixed table keyed by the decoder's failure string and is never built from the response, so it carries no vendor text, payload bytes or model output; `safe_message` is unchanged and does not include it. It is empty when no check of the SDK failed (transport and HTTP-status failures keep `kind`, `http_status` and `vendor_code`). `Error` grows within interface 7, so rebuild consumers against matching headers and libraries, and copy `reason` wherever an `Error` is copied field by field. The new capability bit `sp::capability::ErrorReason` is part of `RequiredProvider`, so a consumer built with these headers that calls `require_interface_contract` with it is refused by a library whose `Error` is smaller instead of reading past it. The descriptor policy and its identity are unchanged. See [outcomes](USAGE.md#outcomes-raw-data-and-nullable-usage).

A Messages response whose top-level `container` or `diagnostics` is an object is now accepted. Both used to make the whole response `Unsupported`, which threw away a billed answer:
- `container` is the code-execution container. The dynamic filtering of `web_search_20260318` and `web_fetch_20260318` opens one even without a code-execution tool.
- `diagnostics` is the cache diagnostics object.

Both describe the response and leave its content as it is. They stay in `raw_events`. A non-empty `context_management` or `input_transformations` still means the server changed the answer, and is still `Unsupported`. No public struct or enum changes, and the interface revision stays 7. See [outcomes](USAGE.md#outcomes-raw-data-and-nullable-usage).

Editing admission data no longer makes sealed history `ReplayIneligible`. `DescriptorPolicy::identity()` used to be SHA-256 over the whole descriptor/codec pair, and every native origin and archive binds it. So adding a model, a MIME type, a gateway origin or a server-tool version, or changing a default or a codec limit, made all history sealed under the previous snapshot unusable, and a host that kept its policy current paid for each update with its replay history. The identity now covers only the facts that decide how sealed history is written to and read from the wire: each family's bindings, stop reasons and Messages header version. The new `DescriptorPolicy::content_digest()` and `content_digest_hex()` cover the whole pair, and pins check them:
- **Pins.** `expected_sha256_hex` is compared with the content digest, and a mismatch is a `ConfigError` at `/content_digest` (it was `/identity`). Pin the value of `content_digest_hex()`.
- **Caches and journals.** A key that has to change whenever the policy content changes, such as a cache of descriptors or a digest of a prepared request, should use the content digest. Two policies that differ only in admission data share a replay identity.
- **History.** Every identity changes with this computation, so history sealed under interface 6 or an earlier snapshot of interface 7 is `ReplayIneligible`. After that, admission edits keep it replayable. A changed binding, stop reason or Messages header version still makes it ineligible.

No struct of the outcome or request changes. `DescriptorPolicy` gains a private member, and the interface revision stays 7.

Model facts moved from the descriptor policy to a model catalogue (`<descriptor/catalogue.h>`, embedded from `config/models.json`). The policy's `models[]` held five exact `(family, model)` entries: any other id, a dated snapshot or a gateway spelling matched nothing, and adding one changed the policy file. A catalogue entry can now match by id, alias or prefix profile, be scoped to origins, and carry limits, modalities, prices and defaults. A model the catalogue does not know is still admitted with the family defaults.
- **Policy file.** `descriptor-policy.json` no longer has `models`, and `load_policy` refuses a snapshot that still lists it. Move each entry to a catalogue entry (`model` becomes `id`, and `defaults` becomes `defaults` keyed by the family).
- **Defaults.** Entry defaults now apply over the family's, member by member: an absent member keeps the family default, and `null` leaves the control unset. `models[]` replaced the whole object.
- **Lookups.** `DescriptorPolicy::defaults`, `output_limit` and `input_limit` take the origin as well (`family, origin, model`). `descriptor::model_defaults(descriptor, model)` and `model_match(descriptor, model)` are the descriptor-level forms. `ModelDefaults` is removed.
- **Loading.** `load_policy(family_json, resource_json, catalogue)` and `DescriptorPolicy::with_catalogue(catalogue)` attach a catalogue; the two-argument `load_policy` uses the embedded one. A policy bundle may carry `model_catalogue`, and `builtin_policy_bundle_json()` includes it.
- **Digests.** The content digest, which pins check, covers the catalogue. The replay identity does not.
- **Refresh.** `transport::refresh_catalogue` builds a catalogue from the OpenRouter, Gemini, Anthropic and OpenAI model listings when the host calls it. Nothing refreshes on its own. The `sp_catalogue_refresh` tool does the same from the command line.

A response that reports another model name than the one requested is no longer refused. Every family failed such a response with `ProtocolCorrupt` (`messages.model.mismatch`, `responses.model.mismatch`, `gemini.model.mismatch`, `interactions.identity.changed`). The exceptions were dated snapshots at the direct OpenAI and Anthropic origins and any name at a declared gateway. So answers the provider had billed were thrown away: `lyria-3-pro-preview` answers as `lyria-3.5`, Google auto-routes `gemini-3.5-flash` to `gemini-3.6-flash`, and an OpenRouter free model can answer as `private/openrouter/...`.
- **Outcome.** `Completion::model` and `PartialCompletion::model` (`ServedModel`) hold the requested name, the reported one and a `ServedRelation`: `NotReported`, `Exact`, `Snapshot`, `Listed`, `Routed` or `Unexplained`. Both layouts grow at the end.
- **Replay.** History stays bound to the requested model. An `Unexplained` name is delivered without a native seal, so a continuation that replays its native state is `ReplayIneligible`. List a known pair in the model catalogue (`served_as`) to make it replayable; the embedded catalogue lists the Google pairs above.
- **Snapshots.** The requested name with `-` and a suffix of digits and dashes is a snapshot at every origin, not only at the direct OpenAI and Anthropic origins.
- **Stability.** A name that changes within one response is still `ProtocolCorrupt`. Interactions now tells a changed model (`interactions.model.changed`) and an empty one (`interactions.model.invalid`) apart from a changed id (`interactions.identity.changed`).
- **Reason codes.** `messages.model.mismatch`, `responses.model.mismatch` and `gemini.model.mismatch` are removed.

A request the SDK refuses before dispatch now names the check that refused it. `Client::prepare` replaced the encoder's text with the generic text of its kind, so "temperature is prohibited for this model", "unsupported audio output MIME type" and "output cap exceeds admitted model limit" all reached the caller as `invalid request` or `unsupported operation`, with an empty `Error::reason`. The reason is now the code of the check, `request.<area>.<check>`: the family encoder's (`request.chat`, `request.messages`, `request.responses`, `request.generate`, `request.interactions`) or one the encoders share (`request.choice`, `request.controls`, `request.media`, `request.native`). `safe_message` does not change. No struct changes.

One Generate descriptor can now serve every model of its origin. A `google.generate` descriptor was bound to one model through its paths, so a host that called a second Gemini model needed a second descriptor and a second `Client`. Paths that name `{model}` (`/v1beta/models/{model}:generateContent` and `/v1beta/models/{model}:streamGenerateContent?alt=sse`) take the model from each request, and `presets::google_generate()` builds that descriptor. `ValidatedDescriptor::model_routed()` and `resolved_path(streaming, model)` report it. Native history sealed on such a descriptor binds its model, so replaying it in a request for another model is `ReplayIneligible`. Literal routes behave as before. `ValidatedDescriptor` gains a private member; no request or outcome struct changes.

Two Gemini media answers that failed or hid their content now decode:
- **MIME parameters.** A MIME type may have optional whitespace around the `;` that separates its parameters (RFC 9110), as in `audio/l16; rate=24000; channels=1`, which Gemini's streamed TTS sends with every audio part. Every streamed TTS response failed with `gemini.part.media_invalid`. The spelling is kept as reported, `mime_essence` drops the whitespace, and whitespace anywhere else (inside a name or value, or around `=`) is still refused. The same rule applies to MIME types in requests.
- **Transcripts.** A Generate part whose only data is `audioTranscription` (the answer of `gemini-3.5-transcribe`) is a `Text` part with the transcript. It used to be `Opaque`, so the transcript was not part of the text. Next to inline audio, `audioTranscription` stays that part's metadata. A malformed one is `gemini.part.transcription_invalid`.

A streamed Chat audio answer through a declared OpenRouter origin now completes. OpenRouter never sends `finish_reason` for it: after the audio it sends the expiry-only delta, an empty delta, and the final usage on a choice with an empty delta and a null `finish_reason`, then `[DONE]`. The SDK only admitted the direct OpenAI order (a usage-only frame, then the expiry), so every such stream failed with `chat.done.without_terminal`. On a declared OpenRouter origin the OpenRouter order derives the same `EndTurn` (`derived:audio_usage+audio.expires_at+DONE`). Every other omission of `finish_reason`, and the same order at another origin, is still refused.

`Error::vendor_code` now names the provider's code even when the error policy does not list it. It used to be set only for listed values (such as `API_KEY_INVALID` or `rate_limit_exceeded`), so every other Gemini 400 or 404, and most OpenAI and Anthropic refusals, reached the caller with an empty code. A value with the shape of a machine code is now kept, and it does not change `kind` or `retry_class`:
- Google `details[].reason`, then `status` (UPPER_SNAKE, such as `INVALID_ARGUMENT`);
- Anthropic `error.type`;
- OpenAI `error.code`, then `error.type` (lower_snake).
Each value is at most 64 characters, and free text never passes. A blocked Gemini answer also carries its `finishReason` or `blockReason` here. Code that treated an empty `vendor_code` as "not a listed error" should compare against the listed values instead.

Streamed Gemini audio is one part, and Gemini's empty closing text part is gone from the typed message:
- **Audio frames.** Streamed TTS sends one `inlineData` part per frame, 26 or more a second, and each became its own `Media` part, while the buffered call returned one. Consecutive inline audio frames of the same MIME are now one `Media` part when the format concatenates: raw PCM (`audio/l16`, `audio/pcm`) or MP3. Each frame is a `PartDelta` of that part, re-encoded as an append-only canonical base64 stream, so the deltas a sink sees join to the sealed payload, and audio can still play as it arrives. A different MIME, a container format such as WAV, an image or video, or any other part in between starts a new part.
- **Empty text.** Gemini ends most answers with a part whose text is `""`. Without a `thoughtSignature` it is no longer a typed part. The part opened and sealed as an empty `Text` before, which #37 already kept from `on_event`. With a signature it stays.
- **Replay.** `wire_output` holds the run of frames as one `inlineData` part with all the audio, and native replay sends that part. An empty closing text part with nothing to join stays a wire part of its own.

A streamed reply has the parts a buffered one has, however many chunks it takes. The part limit (`semantic_max_parts`, 1024) failed a long stream with `ResourceLimit` after the provider had billed it: OpenRouter Chat made an `Opaque` part of every chunk's `reasoning_details`, and Generate made a `Text` or `Thinking` part of every text chunk, so about a thousand chunks ended the call. Now a chunk that continues the part before it extends that part:
- **Chat.** `reasoning_details` fragments merge by index into the entries a buffered reply carries, in one `Opaque` part of type `reasoning_details` that is emitted with the finish reason. The `reasoning_details.frame` parts and the `frames` member of that part are gone, and so is the `chat.reasoning_details.part_limit` code. Native history captured before this change still replays. Text elements of a streamed content array continue one `Text` part.
- **Generate.** Consecutive text, thought and transcript chunks of the same shape are one part, and so are the audio frames of the previous paragraph. A chunk with a `thoughtSignature`, a call, another part member or another value of one starts a part of its own. `wire_output` holds the merged parts, so replay sends what a buffered reply would have carried. A buffered body keeps its wire parts as they are.

Interactions can continue a stored interaction and use the service's own tools:
- **Continuation.** `interactions::Request` gains `previous_interaction_id`, `store` (the request used to send `"store":false` always) and `video_task`.
- **Service tools.** It also gains `builtin_tools` (`GoogleSearchTool`, `UrlContextTool`, `CodeExecutionTool`).
- **Decoding.** Steps of the service's tools (`<tool>_call`, `<tool>_result`) decode as `ServerExecuted` `ToolCall` and `ServerToolResult` parts. They used to fail the response as `Unsupported`.
- **Results.** A continued interaction's result has no native seal.
- **Configuration digest.** It binds the new fields only when they are set, so existing digests do not change.
- **Error codes.** An Interactions HTTP error's lower_snake `code` (`invalid_request`) is now its `vendor_code`.

Interactions runs in the background and acts on stored interactions:
- **`StopKind::Pending`.** A new value, added last: a buffered answer whose status is `in_progress` or `queued`, such as a `background` request. Its usage is `Partial`, and `messages[0].id` names the interaction. Exhaustive switches over `StopKind` need the new case. The native archive reads it.
- **New request fields.** `interactions::Request` gains `background` (which needs `store`), `agent` (which replaces `model`), and `lifecycle` with `interaction_id`:
  - `Get`, `Cancel` and `Delete` act on a stored interaction;
  - they send no input and are buffered only;
  - their results have no client-held replay authority.
- **Echoed input.** A stored interaction read back lists the request's input as a `user_input` step first. That step is not output, and it is no longer refused.
- **Agents.** An `agent` member in the resource is admitted. An `environment` still is not.

Messages server tools take their options, and cited answers decode:
- **Tool options.** `messages::ToolDefinition` gains `allowed_callers`, `allowed_domains`, `blocked_domains`, `user_location` (`UserLocation`), `citations` and `max_content_tokens`. They are sent only when set. Without `allowed_callers: ["direct"]`, `claude-haiku-4-5` refused the `_20260318` web tools with HTTP 400.
- **Container.** `messages::Request` gains `container`.
- **Citations.** A text block with `citations` failed as `Unsupported` (`messages.block.citations_unsupported`, now removed), and a streamed `citations_delta` failed as `messages.delta.unsupported`. That threw away every web-search answer that cited its sources. Citations are now kept as raw-wire observations; a malformed one is `messages.block.citation_invalid`.
- **Configuration digest.** It binds the options only when they are set.

Generate gains the service's tools and an audio output format:
- **New request fields.** `gemini::Request` gains `audio_format`, `builtin_tools` (`GoogleSearchTool`, `UrlContextTool`, `CodeExecutionTool`, `GoogleMapsTool`, `ComputerUseTool`, `FileSearchTool`), `mcp_servers`, `server_tool_invocations` and `retrieval_config`. The configuration digest binds the tools only when they are set, so existing digests do not change.
- **Typed service parts.** `executableCode` and `toolCall` parts were `Opaque`; they are now `ServerExecuted` `ToolCall` parts. `codeExecutionResult` and `toolResponse` are `ServerToolResult` parts. Code that read these parts from `Opaque::wire_metadata` reads the typed parts instead. `wire_output` and native replay are unchanged.
- **Computer use calls.** With `ComputerUseTool`, a call to a function the service declared is a `ClientExecuted` `ToolCall`. Without it, an undeclared function still fails the response as `Unsupported`.
- **Tool results.** A JSON object `content` beside media-only `content_parts` used to be `InvalidRequest`. It is now the `functionResponse.response` object, with the media as its `parts`.
- **G.711 audio.** Streamed `audio/mulaw`, `audio/alaw` and `audio/basic` frames merge into one `Media` part, like raw PCM and MP3.

Generate refuses history the service would refuse:
- **The policy key.** The family policy gains the optional key `unreplayable_parts`, for `google.generate` only, and the built-in policy lists the MCP `toolCall` without `toolType`.
- **The refusal.** Native history with such a part is now `Unsupported` before dispatch (`request.generate.captured_part_refused_by_the_service`). Before, the service answered the next turn with HTTP 400.
- **Projection.** `gemini::project_portable` turns such an answer into portable history.
- **Policy pin.** The policy's content digest changes, so a host pinning it updates the pin.

Responses gains code interpreter, MCP, computer use and OpenRouter's tools and image output:
- **New tools.** `HostedTool` gains `CodeInterpreterTool`, `McpTool`, `ComputerTool`, `ComputerUsePreviewTool` and `OpenRouterServerTool`, added last. `WebSearchTool`, `ImageGenerationTool` and `FileSearchTool` gain options. The policy admits the new OpenAI tool types.
- **New request fields.** `responses::Request` gains `modalities`, `image_config` and `mcp_approvals`.
- **Configuration digest.** Unset options leave existing digests unchanged.
- **Image generation values.** `size`, `quality` and `output_format` were limited to fixed lists. The SDK now checks their shape only, so `xhigh` or `2048x1152` is sent. The reason codes `request.responses.unsupported_image_generation_*` are replaced by shape codes.
- **Computer calls.** A `computer_call` item was an `Opaque` part; it is now a `ClientExecuted` `ToolCall` named `computer`, and the stop is `ToolUse`.
- **Approval requests.** An `mcp_approval_request` item was an `Opaque` part with an `EndTurn` stop. It is now a `ToolCall` of kind `ApprovalRequest`, as the design describes, and the stop is `ToolUse`.
- **Progress events.** Events named after a service item (`response.code_interpreter_call_code.delta`, `response.mcp_call_arguments.delta`) failed the stream as `Unsupported`; they are now that item's progress.
- **OpenRouter images.** An image result given as a data URL was refused as an invalid payload. It is now decoded, with its MIME.
- **Chat.** `chat::ImageOutput` is now an alias of `sp::OpenRouterImageConfig`. Its two reason codes moved to `request.image_config.values_must_be_short_tokens` and `request.image_config.output_compression_is_0_to_100`.

Chat and Responses take OpenRouter plugins, and the cited sources of an answer are typed:
- **Plugins.** `chat::Request` and `responses::Request` gain `plugins` (`sp::OpenRouterPlugin`), for declared OpenRouter origins.
- **Citations.** `Text` gains `citations`, filled from Chat URL citations, Responses annotations, Messages citations and Interactions citation annotations. These were only raw observations before. `Text{value}` still compiles, and the native seal is unchanged.
- **Events.** `PartSeal` gains `citations`, filled for text parts.

The catalogue prices a call:
- **New module.** `<core/pricing.h>` adds `sp::pricing::price_of` and `sp::pricing::estimate`.
- **Fallback.** An estimate prices the served model's entry, else the requested model's, and says so.
- **New enum value.** `CostRounding` gains `CeilingCatalogueProduct`, added last, for these estimates.
- **Reservation bounds (#121).** `ModelPrice` gains `cache_write_1h`, `output_image`, the route ceiling (`ceiling_input`, `ceiling_output`, `ceiling_request`) and `tool_fees` (`tool:<type>` units).
  - `price_of` and `estimate` take an optional service tier, priced by `<unit>/<tier>` units.
  - `estimate` bills Messages' 1-hour cache writes and Gemini's image output tokens at their own rates, and with `route_ceiling` prices a routing gateway's call at the route ceiling.
  - Input bands (`<unit>+<tokens>`) price long-context calls. `price_of` takes the call's input length, `estimate` reads it from the usage, and `dearest_price_of` gives the prices to reserve with.
  - `upper_bound(price, BoundRequest)` bounds a call's cost before dispatch with the same arithmetic as `estimate`.

Values the providers extend are checked by shape, not by compiled lists:
- **Generate.** Speech language codes, image aspect ratios and image sizes are checked as a language tag, `W:H` and a short token. Function responses take any inline document the media policy admits.
- **Interactions.** Output MIME types must be of the right kind, ratios are `W:H`, and image sizes and video resolutions are short tokens. A bit rate is no longer refused next to an uncompressed format.
- **Chat.** Audio formats are lower-case tokens, and the reply's MIME is empty for a format the SDK cannot name.
- **Response schemas.** Unknown schema keywords are sent; known ones keep their checks.
- **Messages.** `OutputEffort` gains `XHigh` (`xhigh`), added last.
  - A server tool's result block decodes for any `<tool>_tool_result` that answers a call to `<tool>` or `<tool>_<variant>`.
  - A server caller is any server tool type the policy admits.
  - A result shape the SDK does not know is kept and counted in the diagnostics. The fixed list and the refusal codes `messages.server_result.*_unknown` are gone.
- **Responses.** `Request::include_names` adds include selections by name.
- **Policy data.** The Chat reasoning efforts gain `minimal` and the Responses ones `max`. The policy's content digest changes.
- **Reason codes.** The refusal reason codes of these checks are renamed after their shape rules.

Untyped values are admitted by the policy (#32):
- **Extensions.** Every request struct gains `extensions`, top-level members sent only when the policy's new `extensions` facts admit them for the descriptor's origin, with a declared value schema. The built-in policy admits none, and a request without them keeps its body and digest.
- **OpenRouter plugins and server tools.** These are admitted by the policy's new `openrouter_plugins` and `openrouter_server_tools` facts, with the schemas of their options. The built-in policy lists the documented ones.
  - An id or type it does not list, which #89 and #90 sent as given, is now `Unsupported`.
  - Options outside the schema are `InvalidRequest`.
- **Generic tools.** `responses::GenericHostedTool` sends a tool type the policy lists in `server_tools` with `"generic": true` and an options schema.
- **Capability.** The new capability bit `sp::capability::PolicyAdmittedExtensions` is part of `RequiredProvider`.
- **Policy digest.** The policy's content digest changes.

Messages typed tools and beta headers (#53):
- **Typed tools.** `messages::ToolDefinition` gains `options`, a typed tool's other members, checked against the schema of the tool's policy fact. A custom tool with options is `InvalidRequest`.
- **Policy facts.** A Messages `server_tools` fact may omit `name` (a toolset), set `"client": true` (the caller runs the tool; no `max_uses`), and name a `beta`. The new `betas` facts admit `anthropic-beta` values per origin. The built-in policy lists memory, bash, the text editors, the computer and browser toolsets, and the advisor, and admits `speed`, `inference_geo` and `diagnostics` as extensions at the Anthropic origin. The policy's content digest changes.
- **Beta headers.** `messages::Request::betas` adds admitted `anthropic-beta` values. The encoder now writes one `anthropic-beta` header that merges the descriptor's values, the request's and the declared tools'. A request with betas or tool options binds them in its configuration digest; one without keeps its digest.
- **Named tool choice** may name a typed tool the caller runs.
- **Toolsets.** A tool result answering a toolset member's call is sent with that call's `toolset_name`.
- **Invocation limit.** Typed tools the caller runs no longer clear `model_invocation_limit`.
- **MCP connector.** `messages::Request::mcp_servers` (`McpServer`) and the `mcp_toolset` fact. `mcp_tool_use` decodes as a server `ToolCall` with that `wire_type` and `mcp_tool_result` as a `ServerToolResult`; both replay as captured. An MCP call pairs as a call of `mcp`, also in a native context's pending server calls.
- **Kept blocks.** The new `response_blocks` facts make a listed response block an `Opaque` part of the captured message, replayed in place. A Messages history may now hold `Opaque` parts in sealed assistant messages. The built-in policy lists `fallback`, `container_upload` and `mcp_tool_listing`; an unlisted block is still `Unsupported`.
- **Value schemas** gain `map`.
- **Unbound extensions (#96).** An extension fact may set `"bound": false` (replay-eligible only). Its value never joins the configuration digest, so it may change between turns. The built-in Messages facts for `speed`, `inference_geo` and `diagnostics` are unbound. A request that sets only these keeps the digest it would have without extensions.
- **Container (#98).** `messages::Request::container` no longer joins the configuration digest, so a conversation can reuse the container a response reported. A request that sets it now has the digest it would have without one.
- **Code interpreter container (#101).** The Responses configuration digest no longer binds the container of a `CodeInterpreterTool`: its `container_id`, `file_ids`, `memory_limit` or `network_policy`. A conversation can move to the container a response reported. A request with a code interpreter tool now has another digest, so history sealed with one before this change is `ReplayIneligible`.

Messages compaction (#53):
- **`StopKind::Compaction`.** A new value, added last. The answer is a summary in place of a reply, or the turn paused after writing one. Exhaustive switches over `StopKind` need a case. The Messages policy maps `compaction` to it. Stop reasons are part of the policy identity, so history sealed under an earlier snapshot is `ReplayIneligible`.
- **Policy facts.**
  - `response_blocks` entries gain `signed_beta` and `opens_history`.
  - Extension facts gain `beta` (Messages only): the `anthropic-beta` value the encoder adds when the request sets that extension.
  - The built-in policy admits `compaction` and `context_management` (compaction edits only) at the Anthropic origin, the betas `compact-2026-01-12` and `compact-2026-09-04`, and the `compaction` block.
- **History.** A Messages request sends and binds its history from the last captured message that carries a `compaction` block. A context's `prefix_count` counts from it.
- **Decoding.** A kept block takes its `<type>_delta` events. `context_management` metadata that reports no applied edit is accepted, also beside a streamed `message_delta`, where it used to be ignored.
- **Usage and pricing.** `Usage::extra` gains the `compaction.*` sums. `pricing::estimate` charges them for Messages. After a compaction iteration, a streamed answer's input counters replace those of `message_start` instead of recording a conflict.
- **Archive.** A v3 record of a native message also keeps its context's configuration digest, so the layout grows by one digest. Records written before this change are refused at load, as the policy identity change already makes them. Stop kinds are read up to `Compaction`.

Realtime sessions (D8, #60):
- **Sessions.** `<codecs/realtime.h>` adds `realtime::Setup`, the commands, the events, `realtime::Codec` and `realtime::connect`; `<transport/realtime_session.h>` adds `realtime::Session`, `realtime::Options`, `realtime::Outcome` and `realtime::open`. The built-in policy admits Google Live and OpenAI Realtime.
- **Reason codes.** New sources `realtime` and `request.realtime`; the session adds `runtime.realtime.inbound_overflow`, `runtime.realtime.lease_refused`, `runtime.realtime.audio_too_large`, `runtime.realtime.closed`, `runtime.realtime.cancelled` and `runtime.realtime.refused`.

Non-chat endpoints (D6, #61):
- **Decision.** [D6](decisions/D6-endpoints.md) supersedes D2: non-chat endpoints run on `runtime::Client` with request and result types of their own.
- **Client.** `runtime::Client` gains `prepare(images::Request)`, returning `PreparedEndpoint<images::Outcome>`; `complete` for a request or a prepared call; and `start`, returning `EndpointCall<images::Outcome>`. `PreparedRequest` gains `endpoint()`. The chat `Request`, `Outcome` and `Completion` are unchanged, and a test checks that.
- **Images.** `<codecs/images.h>` adds `images::Request`, `images::Result` and `images::Outcome` (a Result or a `Failure`).
- **Decisions.** `<codecs/decisions.h>` adds `decisions::Request`, `Question`, `Option`, `Answer`, `Result` and `Outcome`, with `prepare`, `complete` and `start` on `runtime::Client`. The built-in policy admits OpenAI `/v1/decisions` and OpenRouter `/api/alpha/decisions`.
- **Policy facts.** The descriptor policy gains a top-level `endpoints` list. The built-in policy admits images at the OpenAI and OpenRouter origins. The content digest changes; the replay identity does not.
- **Embeddings, rerank, moderation.** `<codecs/embeddings.h>`, `<codecs/rerank.h>` and `<codecs/moderation.h>` add their requests, results and outcomes, with `prepare`, `complete` and `start` on `runtime::Client`. `<codecs/endpoint.h>` declares the decoder interface they share; `images::Close` and `decisions::Close` are now `endpoints::Close`.
- **Speech and transcription.** `<codecs/speech.h>` and `<codecs/transcription.h>` add their requests, results and outcomes. `endpoints::Encoded` replaces each endpoint's own `Encoded` and carries the body's media type and what the call accepts. `endpoints::Close` carries the response headers.
- **Jobs and video.** `<codecs/jobs.h>` adds `jobs::Handle`, `State`, `Poll`, `Download`, `Content` and their outcomes, and `<codecs/videos.h>` adds `videos::Request`, with `prepare`, `complete` and `start` on `runtime::Client`. The built-in policy admits video jobs at OpenRouter. `endpoints::Encoded` gains `method`, `sink` and `sink_limit`, and `endpoints::Close` gains `sink`.
- **Files by URI.** At Google, `files::Get`, `files::Delete` and `files::Download` also take the file's URI at the origin, as an Interactions result delivered by URI names it.
- **Files (D7).** `<codecs/files.h>` adds `files::File`, `Upload`, `Get`, `List`, `Delete`, `Download`, their outcomes and `files::media`, with `prepare`, `complete` and `start` on `runtime::Client`. The built-in policy admits files at OpenAI, Anthropic and Google. Endpoint facts may name literal `headers`. `endpoints::Encoded` gains `headers`, `file` and `body_suffix`; `endpoints::Close` gains `upload_sha256`. Chunked uploads add `files::Start`, `Part`, `Complete`, `Query`, `Cancel`, `Session` and `Progress`.
- **Batches (D7).** `<codecs/batches.h>` adds `batches::Item`, `Submit`, `Batch`, `Get`, `Cancel`, `Delete`, `Results`, `jsonl`, `inline_lines`, `custom_id` and `decode`, with `prepare`, `complete` and `start` on `runtime::Client`. The built-in policy admits batches at OpenAI, Anthropic and Google. `jobs::ContentCodec` takes a flag that accepts a JSON or text body as content.
- **Token counting (D7).** `<codecs/tokens.h>` adds `tokens::Count`, `Result` and `Outcome`, with `prepare`, `complete` and `start` on `runtime::Client`. Endpoint facts gain `keep`. `<codecs/family_request.h>` names the chat requests a batch item or a count carries (`sp::FamilyRequest`); `batches::Request` is that type.
- **Background interactions (D6, D7).** `<codecs/interaction_jobs.h>` adds `jobs::from_interaction`, `jobs::interaction_poll` and `jobs::interaction_state`. The Interactions lifecycle itself is unchanged.
- **Generation statistics and translations (D7).** `<codecs/generations.h>` adds `generations::Get` and `generations::Result`, with `prepare`, `complete` and `start` on `runtime::Client`. `transcription::Request` gains `translate`. The built-in policy admits OpenRouter `/api/v1/generation` and OpenAI `/v1/audio/translations`.
- **Webhooks (D7).** `<codecs/webhooks.h>` adds `webhooks::verify` and `webhooks::Event`. Its refusals have reason codes under `webhooks.`.
- **Transport.** `transport::HttpRequest` gains `file_body`, a body read from a file slice while it is sent, and `transport::Result` gains `upload_sha256`. The new `FailureKind::UploadSource` is reported as `InvalidRequest` with the reason `transport.upload.source`. A `switch` over `FailureKind` needs the case.
- **Retries of reads.** An endpoint call sent as a GET (a poll or a download) is retried on a transient failure even when the retry policy accepts no duplicate billing risk. It still spends the retry budget.
- **Checks of calls and answers (#132).** `endpoints::Encoded` gains `context` (`endpoints::Context`): the number of inputs and of results the call asked for, which the embeddings and rerank decoders check the answer against, and a value no journal may store, which `Completion` carries as `secret`. `endpoints::Close` gains `status`, and `endpoints::make_codec` takes the context. A chunked upload's `model()` and its `files.session` raw event name the session by `url_sha256`; the URL is in `Session::url`, `PreparedRequest::url()` and `Completion::secret`. `rerank::Result::ranked` is sorted by relevance. `files::Download` takes any content type, and the file at its `path` changes only when the download is accepted. `Completion` grows by `secret`, so rebuild consumers against matching headers; the new capability bit `sp::capability::CompletionSecret` is part of `RequiredProvider`. The endpoint calls that left out their fact's `headers` now send them.
- **Reason codes.** New sources: `images`, `decisions`, `embeddings`, `rerank`, `moderation`, `speech`, `transcription`, `jobs`, `files`, `batches`, `tokens` and `generations`, each with its `request.` form; `webhooks`; and `request.videos`. A download to a file adds the runtime reasons `runtime.sink.limit`, `runtime.sink.open_failed` and `runtime.sink.write_failed`.

Messages input blocks and skills (#53):
- **`ToolResult::content_parts`** is now `std::vector<std::variant<Text, Media, Opaque>>`. `Opaque` moved before `ToolResult` in `<core/value.h>`. Visitors and `std::get<Media>` on a part that is not text need a case for it. The archive stores such a part under a new nested tag, and the native digest hashes it.
- **Input blocks.** The new `input_blocks` policy facts (Messages only) admit content blocks the caller writes; the built-in policy lists `search_result`. A Messages user message may hold `Opaque` parts of a listed type. The other families refuse an `Opaque` part in a tool result as `Unsupported`.
- **Skills.** `messages::Request::skills` (`ContainerSkill`) sends `container` as an object with `skills`, and its `id` when `container` is set. The configuration digest binds the skills; a request without skills keeps its digest.
- **Value schemas.** A string may now hold carriage returns, like tabs and line feeds.

## From interface 5 to 6

Package 0.3.0 alpha adds `Usage::provider_cost` and the fixed-size `ProviderReportedCost`, `UsdAmount`, `CostSource`, `CostStatus` and `CostRounding` types. Existing field access remains source-compatible, but `Usage`, `UsageUpdate`, `Completion`, `PartialCompletion` and the containing `Event` layouts grow. Rebuild every consumer against the same headers and generation 6 libraries (`libsp_*.so.6`); interface 5 binaries must not load interface 6 libraries. CMake consumers select `SchemaProvider 0.3.0 EXACT`.

Money is not a token counter. Do not read `cost` from `Usage::extra` or add it to token totals. The amount unit is nanoUSD (1e-9 USD), converted exactly from the shortest decimal that round-trips to the parsed binary64 number and rounded upward only below one nanoUSD (`CostRounding::CeilingShortestDecimal`; the earlier binary64 ceiling added one nanoUSD to values such as `1.5e-06`). Read each field's status and source, preserve missing versus reported zero, and use the enclosing usage stage to distinguish partial from final observations. A provider report is not an invoice, a catalogue estimate, or authority to settle/renew a spending grant; existing qualification ledgers and nonrenewable holds are unchanged. See [reported costs](USAGE.md#provider-reported-costs).

The Responses codec admits a direct OpenAI base-model alias resolving to the same model with a valid dated snapshot suffix, while retaining requested context and stable served-stream identity. Explicit snapshots and unrelated origins stay exact. The Chat codec accepts a semantically empty idempotent repeated terminal choice carrying usage, without another Stop/content/outcome; contradictory/new post-terminal data remains invalid. These repairs do not admit foreign native replay authority or extend operation deadlines.

Responses and Messages also admit a different nonempty served `model` when the descriptor origin is declared in its family's `openrouter_origins` policy. Such a routing gateway resolves aliases (for example `~vendor/model-latest`) and fallbacks itself, so it echoes the concrete model it served rather than the request. The served identity must still stay stable for the whole response, the raw wire envelope keeps it, and native replay stays bound to the requested model. Undeclared origins keep exact equality and the direct OpenAI and Anthropic dated-snapshot rules. See [served model identity and routing gateways](USAGE.md#served-model-identity-and-routing-gateways) for how a deployment declares its origin, what the declaration turns on besides the model, and why Gemini cannot declare one.

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
