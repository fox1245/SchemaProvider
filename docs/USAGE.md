# Using the interface 7 SDK

Start with the complete [README program](../README.md#first-request). This guide explains what that program owns and how to extend it. Installed declarations, not historical sketches in DESIGN, define the API.

Source references: [runtime request/handle API](../src/runtime/client.h), [owned values/outcomes](../src/core/value.h), [descriptor admission](../src/descriptor/descriptor.h), [immutable policy](../src/descriptor/policy.h) and [native archive](../src/core/native_archive.h). Request-family headers named below define the controls; historical names such as `JsonValue`, `Client::option` and `ForeignReasoning` are not current declarations.

## Provider-reported costs

`Completion::usage.provider_cost` and `Failure::partial.usage.provider_cost` separate monetary metadata from integer token counts. The fixed-size object has optional `total`, `upstream_total`, `upstream_input` and `upstream_output` amounts, four matching `CostStatus` entries in that order, optional `is_byok`/`byok_status`, `CostSource` and independent `UsageQuality`. Amounts carry `Evidence::Reported` and `CostRounding::CeilingShortestDecimal`. No monetary leaves enter `Usage::extra` or token totals.

One `UsdAmount::nano_usd` is 1e-9 USD. The parser keeps no lexeme, so conversion recovers the shortest decimal that round-trips to the parsed binary64 value (the provider's own decimal whenever it had at most 17 significant digits) and converts that decimal exactly with integer arithmetic. It rounds upward only when that decimal has a nonzero digit beyond the ninth fractional place: `0.1` is `100000000`, `1.5e-06` is `1500` and `0.00029` is `290000` nanoUSD, so reported upstream input plus output equals the reported total, while `1.0000000001` is `1000000001` and positive subnano amounts round up to one. Reported zero stays present zero. Amounts above 2^53 nanoUSD lack the admitted precision, and values reaching the uint64 range overflow. `Missing`, `Malformed`, `PrecisionExceeded`, `Overflow`, `UnknownCurrency` and `Conflict` remain explicit unavailable statuses, not invented zero. Only policy-admitted OpenRouter origins have known USD monetary semantics; arbitrary gateways retain unknown-currency status and raw metadata.

Each reported usage object replaces its monetary snapshot, clearing stale omitted values. The enclosing `Usage::stage` is partial for updates/failures and final on successful commit. Read status/source/stage before using any amount; malformed monetary metadata does not discard otherwise-valid text or relax token-counter validation. Raw envelopes/events remain available. The JSON parser remains strict, so invalid nonfinite JSON and overflow-to-infinity can fail before cost projection; a decimal with more than 17 significant digits was already rounded by binary64 parsing and is converted from its shortest round-trip form.

OpenRouter Chat and Messages report upstream input/output using `upstream_inference_prompt_cost` / `upstream_inference_completions_cost`; Responses uses `upstream_inference_input_cost` / `upstream_inference_output_cost`. They project into the same typed `upstream_input` / `upstream_output` fields. When both spellings are supplied they must agree as parsed binary64 values before quantization; different values become `Conflict`, unavailable amount and inconsistent monetary quality even if both would round into the same nanoUSD bucket. A null/missing primary can use a known alternate. Both raw values remain retained; no arbitrary winner or arithmetic guess is introduced.

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
| `bindings` | Optional admitted model/message/stream/output-cap member names and usage path segments. Defaults come from the family's policy. The Chat output cap defaults to `max_completion_tokens` on direct OpenAI (`https://api.openai.com`), whose current models reject `max_tokens`, and to `max_tokens` on every other origin, OpenRouter and compatible endpoints included. An explicit `max_output_tokens` binding wins on any origin. |
| `stop_reasons` | Optional mappings to declared stop kinds; compiled failure/standard-stop safety cannot be overridden. Chat and Messages decode stops through this table. Responses, Generate and Interactions decode with compiled mappings and do not consult it; entries added for them change the native binding identity, not decoded stops (Generate's built-in table mirrors its compiled mapping). |
| `evidence` | Optional HTTPS documentation URLs and verification date; metadata is not runtime qualification. |

The loader rejects duplicate/unknown keys, wrong types, size/depth overflow, path traversal, URL userinfo and invalid bindings. Non-loopback origins require HTTPS. HTTP is allowed only for canonical loopback hosts. Google generate paths name their model: the loader requires `/v1beta/models/<model>:generateContent` and `/v1beta/models/<model>:streamGenerateContent?alt=sse` for the same `<model>` (a ConfigError at `/connection/paths/buffered` or `/streaming` otherwise). `<model>` is either `{model}` in both paths, so that one descriptor serves every model of the origin and each request goes to its own `Request::model`, or one literal model made of letters, digits, `-`, `.` and `_`, which serves only requests for that model. On a `{model}` descriptor native history binds the model it was sealed for: replaying it in a request for another model is `ReplayIneligible`. Descriptors cannot script authentication, expand environment variables, interpolate prompts or merge arbitrary JSON bodies. Host endpoint allow-lists are a host responsibility; descriptor validation alone does not decide who should receive a credential.

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

`Client(descriptor, std::string api_key)` and `Options::with_api_key(key, runtime_policy)` are shorthand for the lines above: they set `Options::api_key` (and nothing else) and take the key by value, so move it in. The rules above are unchanged: keep the key out of descriptors, logs and encoded bodies.

`sp::descriptor::presets` (`<descriptor/presets.h>`, included by `<runtime/client.h>`) builds the descriptor for a well-known origin without writing JSON: `openai_chat`, `openai_responses`, `anthropic_messages`, `google_generate()` or `google_generate(model)`, `google_interactions`, `openrouter_chat`, `openrouter_responses`, `openrouter_messages` and `loopback_chat(base_url, path)`. Each has a `try_` form that returns the `LoadResult` variant and a form that throws `std::invalid_argument` carrying the `ConfigError` message. A preset only writes the closed descriptor-version-1 JSON for you and passes it through `descriptor::load` (or `load_with_deployment_headers` when `PresetOptions::headers` is set), so origin, HTTPS/loopback, path, header and policy admission are exactly those of a hand-written descriptor. `PresetOptions` may replace the `id`, `revision`, `base_url`, the deployment headers and the policy snapshot. A replaced `base_url` is still only a descriptor origin: which hosts may receive a credential stays your decision. `google_generate()` builds the `{model}` descriptor that serves every Gemini model through one `Client`. `google_generate(model)` builds a literal route for one model; a model that is empty, longer than 128 bytes or contains a control character, `/`, `\`, `?`, `#` or `:` is refused. A preset has a fixed default `id` (for example `openai-chat`), and the descriptor `id` is part of the native origin identity, so history sealed under a preset continues only under a descriptor with the same id and facts.

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
| `config/descriptor-policy.json`, `config/codec-defaults.json`, and the model catalogue `config/models.json` | `descriptor::load_policy(family_json, resource_json)`, or `load_policy(family_json, resource_json, catalogue)` | Call `descriptor::load(descriptor_json, snapshot)`. |

Both loaders return a variant of snapshot or `descriptor::ConfigError`. Embedded snapshots are the default; they are generated during SDK configuration. External loads are explicit and bounded. Files are source inputs, not files that every installed process must discover. Existing clients/descriptors retain their admitted policy; loading a replacement affects only newly constructed values.

A descriptor/codec snapshot can also be loaded from files, from one bundle, and against an expected content digest, so a vendor fact (a model rule, a header version) can be updated without rebuilding the SDK:

| Function | Reads |
|---|---|
| `descriptor::load_policy_files(family_path, resource_path, expected_sha256_hex = {})` | The two files named in the table above, each bounded by the codec admission byte limit. |
| `descriptor::load_policy_bundle(bundle_json, expected_sha256_hex = {})` and `load_policy_bundle_file(path, expected_sha256_hex = {})` | One closed JSON object `{"version":1,"descriptor_policy":{...},"codec_defaults":{...}}`. An unknown key or a version other than 1 is a `ConfigError`. The file form is bounded to twice the single-file limit. |
| `descriptor::load_verified_policy(family_json, resource_json, expected_sha256_hex = {})` | The two documents as text; the common core of the loaders above. |
| `descriptor::builtin_policy_bundle_json()` | The embedded pair in the bundle shape, a starting point for an edited bundle. |

A policy has two digests. Both are lowercase 64-digit SHA-256 values computed from the parsed documents, not the raw file bytes:
- `DescriptorPolicy::identity_hex()` is the replay identity. It covers the facts that decide how sealed history is written to and read from the wire: each family's bindings, stop reasons and Messages header version. Every native origin and archive binds it, so a policy that changes one of these facts makes history sealed under the previous snapshot `ReplayIneligible`.
- `DescriptorPolicy::content_digest_hex()` covers the whole pair, including the admission data: model defaults and limits, media allowlists, sampling rules, server-tool facts, gateway origins and codec resources. Editing that data changes the content digest only, so sealed history stays replayable.

A nonempty `expected_sha256_hex` must be 64 hexadecimal digits (either case) equal to the content digest, otherwise the load returns a `ConfigError` at `/content_digest`, so a malformed or wrong pin is never ignored. An empty pin means no check. The pin compares against a value you obtained out of band; it is an integrity check, not proof of who produced the file. The bundle covers the descriptor/codec pair only: runtime and error policy still load with `configuration::load_runtime_policy`.

Family/model defaults are resolved before encoding. An unset request optional uses its admitted default, which may also be unset. An explicit value replaces that default; false and allowed zero values remain distinct from absence. Output caps must be positive, so explicit zero is rejected there. Caller caps are not silently clamped: an admitted model limit or representation/resource bound rejects an incompatible value. No policy snapshot or canary profile grants financial authority.

### Model catalogue

What the SDK knows about models is data in a model catalogue (`<descriptor/catalogue.h>`), not code. A policy snapshot carries one. The embedded catalogue, `config/models.json`, holds only facts confirmed by live calls; it is not a provider listing.

A catalogue never refuses a model. A model it does not know gets the family defaults and no limits, and a known model gets its entry's facts:
- **Defaults.** The entry's `defaults` for the family, over the family's own defaults. An absent member keeps the family default, and `null` leaves the control unset.
- **Limits.** `output_limit` refuses a larger output cap before dispatch, as an admitted model limit always did. `input_limit` and `context_window` are reported, not enforced.
- **Temperature.** `temperature` (`true` or `false`) replaces the family's sampling rules for that model.

Ask how a model matched with `descriptor::model_match(descriptor, model)` or `DescriptorPolicy::model(family, origin, model)`. The result names the entry and the relation:
- `Exact`, the entry's `id`;
- `Alias`, one of its `aliases`;
- `Prefix`, the longest profile (`"match": "prefix"`) whose `id` starts the model;
- `Unknown`, no entry.

Among entries of the same rank, one that lists the family wins over one that lists other families, and one scoped to the descriptor's origin wins over an unscoped one. An entry whose `origins` does not include the descriptor's `base_url` never matches. `family_listed` is false when the entry lists other families only; the request is still admitted.

The file is closed JSON, `{"version": 1, "models": [entry, ...]}`, at most 8 MiB and 16384 entries. Only `id` is required:

| Member | Meaning |
|---|---|
| `id`, `match` | The model name (printable ASCII without spaces, at most 256 bytes), and `"exact"` (the default) or `"prefix"` for a profile. One entry per id and match. |
| `families`, `origins` | The families that serve the model, and the descriptor origins the entry applies to. Empty or absent: not stated, and every origin. |
| `aliases`, `served_as`, `alias_of` | Other names that request this model, names a response may report for it, and the model a provider-resolved alias points to. A profile has no aliases. |
| `name`, `lifecycle`, `retires_at`, `source` | Display name, lifecycle token, retirement date, and where the entry came from (`verified` in the embedded file). |
| `context_window`, `input_limit`, `output_limit` | Positive token counts. |
| `input_modalities`, `output_modalities`, `methods`, `parameters` | Tokens as a provider lists them. |
| `temperature` | Whether the model accepts `temperature`. |
| `pricing` | USD per unit as decimal text, by unit name (`{"prompt": "0.00000015"}`). |
| `defaults` | Request defaults by family, with the members of the family `defaults`, each optional. |

To use your own catalogue:
- **Load it.** `load_catalogue(json)` or `load_catalogue_file(path)`, or build one from entries in memory with `make_catalogue(entries)`.
- **Layer it.** `merge_catalogues(layers)` applies layers in order. An entry with the same id and match is merged member by member, so a later layer replaces only what it sets (one price unit, one default) and adds new entries. Put the provider facts first, verified corrections next and your own file last.
- **Attach it.** `policy->with_catalogue(catalogue)` returns a new snapshot, and descriptors built from it use the catalogue. A bundle may carry it as `model_catalogue`; without one, the embedded catalogue applies. Entry defaults are checked against the family rules, and a refusal points into the catalogue (`/model_catalogue/models/<id>/defaults/<family>`, with the entry id in place of an index).
- **Compare it.** `diff_catalogues(before, after)` lists the added, removed and changed entries. `to_json()` is the canonical text, and `digest()` its SHA-256.

The catalogue is admission data: it changes the policy's content digest, not its replay identity, so a new catalogue keeps sealed history replayable.

### Refreshing the catalogue from provider listings

`transport::refresh_catalogue(transport, request)` (`<transport/catalogue_refresh.h>`) builds a catalogue from the providers' model listings. The SDK never calls it on its own and never polls. Call it when your harness decides: at start-up, from a scheduled job or by hand. Then attach the result to a new policy for the clients you build next; existing clients keep their policy.

```cpp
#include <descriptor/policy.h>
#include <transport/catalogue_refresh.h>

sp::transport::Transport transport;
sp::transport::RefreshRequest request;
request.sources = {{sp::descriptor::Listing::OpenRouter},                       // public, no key
                   {sp::descriptor::Listing::Anthropic, {}, anthropic_key},
                   {sp::descriptor::Listing::Gemini, {}, gemini_key},
                   {sp::descriptor::Listing::OpenAI, {}, openai_key}};
request.previous = last_generated;                                  // optional
request.overlays = {sp::descriptor::builtin_catalogue(), my_catalogue};  // verified facts, then yours
auto refreshed = sp::transport::refresh_catalogue(transport, request);
if (auto* result = std::get_if<sp::transport::RefreshResult>(&refreshed)) {
    auto policy = sp::descriptor::builtin_policy()->with_catalogue(result->catalogue);
    // Build descriptors from `policy`; keep result->generated as the next `previous`.
}
```

The four listings:

| `Listing` | Request | Credential | Entries |
|---|---|---|---|
| `OpenRouter` | `GET /api/v1/models?output_modalities=all` | none needed (`authorization: Bearer` if given) | Name, context window, output limit, modalities, supported parameters, string prices, canonical slug as `served_as`, alias target and expiration date. Text models list the Chat, Responses and Messages families. |
| `Gemini` | `GET /v1beta/models?pageSize=1000`, then `pageToken` | `x-goog-api-key` | Id without `models/`, display name, input and output limits and methods. Models with `generateContent` list the Generate and Interactions families. |
| `Anthropic` | `GET /v1/models?limit=1000`, then `after_id` | `x-api-key`, `anthropic-version: 2023-06-01` | Display name, input and output limits, lifecycle, retirement date and image or PDF input. |
| `OpenAI` | `GET /v1/models` | `authorization: Bearer` | Ids and shutdown dates only. |

The rules of a refresh:
- **Source URL and origin.** A source's `url` is where the listing is served (https, or http on a loopback host). Its `origin` is the descriptor origin the entries are scoped to. Both default to the provider's API.
- **What a source owns.** Each source owns the entries whose `source` is the listing's name (`openrouter`, `gemini`, `anthropic`, `openai`) and whose origins are its origin.
- **Failures keep old entries.** A source that fails keeps its entries from `previous`, so an outage never drops models. `RefreshResult::sources` reports the failure as fixed text: never a URL, header, credential or body.
- **Listings are provider data.** Unknown members are ignored. A value the catalogue cannot hold is left out, and an id that is not a model name is counted in `skipped`. A body that is not a listing fails that source.
- **Bounds.** Each page waits at most `page_timeout` and may be at most 32 MiB. A source stops after 64 pages, or when it repeats its page token.
- **Result.** `generated` is the provider facts alone, `catalogue` is `generated` with the overlays merged over it, and `diff` runs from `previous` to `generated`.
- **Blocking.** The call waits for each page, so it must not run on one of the transport's I/O threads.

Listings are sometimes wrong. On 2026-10-19 the Gemini listing named `generateContent` for `gemini-omni-1.1-flash`, which answers only on Interactions. The embedded catalogue corrects that, so pass it as the first overlay.

The `sp_catalogue_refresh` tool, built with the tests, runs a refresh from the command line. It reads `OPENROUTER_API_KEY`, `GEMINI_API_KEY`, `ANTHROPIC_API_KEY` and `OPENAI_API_KEY` and never prints them:

```bash
sp_catalogue_refresh --out models.json --generated generated.json --builtin-overlay [--previous generated.json] [--overlay mine.json]
```

### Served model identity and routing gateways

A response may report another model name than the one requested. Providers serve aliases as dated snapshots, Google answers `lyria-3-pro-preview` as `lyria-3.5` and auto-routes `gemini-3.5-flash` to `gemini-3.6-flash`, and a gateway answers with the model it routed to. No name fails a completed response, because the provider generated and billed the answer. `Completion::model` (and `PartialCompletion::model`) records the requested name, the reported one and how they relate:

| `ServedRelation` | Reported name | Native state |
|---|---|---|
| `NotReported` | none | sealed |
| `Exact` | the requested name | sealed |
| `Snapshot` | the requested name, `-` and a suffix of digits and dashes (`claude-haiku-4-5-20251001`, `gpt-4.1-nano-2025-04-14`, `gemini-2.5-flash-001`) | sealed |
| `Listed` | a name the policy's model catalogue relates to the request: the requested entry's `aliases`, `served_as` or `alias_of`, or an entry that lists the request among its own names | sealed |
| `Routed` | another name at a declared routing gateway | sealed |
| `Unexplained` | any other name | delivered without a native seal |

Native history stays bound to the requested model. An unexplained name keeps the answer and withholds replay authority: the message has no native seal, so a continuation that has to replay its thinking or encrypted state is `ReplayIneligible`. To make a known pair replayable, list it in the catalogue (`served_as`); the embedded catalogue does this for the Google pairs above.

The reported name must stay the same for the whole response. A change is `ProtocolCorrupt` (for example `gemini.model.identity_changed` or `interactions.model.changed`). Chat records the name but never binds replay to it, because compatible servers report names of their own.

A gateway is declared by listing its origin in `openrouter_origins` of the family's entry (`openai.responses`, `anthropic.messages` or `openai.chat`) in the descriptor policy, and building the descriptor against that policy. The listed string is compared byte for byte with the descriptor's `base_url` as stored: scheme and host, plus the port if there is one, with no path and no trailing slash. `https://GW.example.com` does not declare `https://gw.example.com`, and a second origin needs its own entry. HTTP is admitted for loopback hosts only.

```cpp
#include <descriptor/descriptor.h>
#include <descriptor/policy.h>

// bundle is sp::descriptor::builtin_policy_bundle_json() with "https://llm.example.com" added to the
// "openrouter_origins" array of the "openai.responses" entry of descriptor_policy.families.
auto policy = sp::descriptor::load_policy_bundle(bundle, expected_content_digest_hex);
if (auto* error = std::get_if<sp::descriptor::ConfigError>(&policy)) { /* error->pointer names the entry */ }
// descriptor_json has "base_url": "https://llm.example.com" and the Responses paths of the gateway.
auto loaded = sp::descriptor::load(descriptor_json, std::get<sp::descriptor::PolicySnapshot>(policy));
```

A preset takes the same snapshot in `PresetOptions::policy`, together with the replaced `base_url`.

The declaration is not a model-only switch. It makes the origin an OpenRouter-profile origin for that family, so besides classifying another name as `Routed` it admits the `provider` routing object (`OpenRouterRouting`), Chat's gateway controls (`reasoning`, `include_reasoning`, `usage_include`, `models`) and image output, the policy's declared-origin media admission (`openrouter_media`) in place of `media`, assistant content arrays, `reasoning_details` and file annotations in Chat responses, one `[DONE]` after a Messages `message_stop`, an absent Chat cache-write counter as unknown instead of zero, and `usage.cost` read as USD (`CostSource::OpenRouterUsd`). Declare only an origin that behaves like OpenRouter in those respects; otherwise the SDK sends controls the service ignores or reads its cost in the wrong unit.

What the declaration keeps: the served identity must still stay the same for the whole response, the reported model is in `Completion::model` (as `Routed`) and the raw wire envelope, and native history stays bound to the requested model. What it does not cost: the origin list is admission data, so it changes `DescriptorPolicy::content_digest_hex()` and leaves the replay identity as it was, and history sealed under the embedded policy stays replayable. Use the same snapshot for every client of the deployment, and pin it with `expected_content_digest_hex`.

## Request families and controls

`sp::runtime::Request` is a variant of the five structs below. A request must match the client's descriptor family; a mismatch fails before network dispatch. Assign declared fields rather than using a generic `extra_fields` map.

| Request type / header | Family-specific fields besides model, messages and tools |
|---|---|
| `sp::chat::Request`, `<codecs/chat.h>` | `temperature`, `top_p`, `max_output_tokens`, scalar `reasoning_effort`, `service_tier`, `provider`, `response_format`, `modalities`, `audio`, OpenRouter `image_config`, `reasoning`, `include_reasoning`, `usage_include`, alternative `models`; choose either simple `messages` or ordered `canonical_messages`, never both. |
| `sp::messages::Request`, `<codecs/messages_request.h>` | `system`, `account_scope`, **`max_tokens`**, `thinking_budget`, `thinking_mode`, `output_effort`, `cache_control`, `tool_choice`, `temperature`, `top_p`, `provider`. |
| `sp::responses::Request`, `<codecs/responses_request.h>` | `instructions`, `account_scope`, `max_output_tokens`, `reasoning` (`effort`, `summary`), `required_tool`, `service_tier`, `temperature`, `top_p`, `store`, `response_format`, `provider`, `hosted_tools`, `max_tool_calls`, `previous_response_id`, `previous_response_history`, `parallel_tool_calls`, `verbosity`, `truncation`, `include`. |
| `sp::gemini::Request`, `<codecs/gemini_request.h>` | `system`, `account_scope`, `max_output_tokens`, `thinking_budget`, typed `thinking_level`, `include_thoughts`, `temperature`, `safety_settings`, `tool_choice`, `required_tool`, `history_mode`, `response_modalities`, `speech_config`, `image_config`, `audio_transcription_config`. |
| `sp::interactions::Request`, `<codecs/interactions_request.h>` | `system`, `account_scope`, `max_output_tokens`, `thinking_level`, `thinking_summaries`, `service_tier`, `required_tool`, typed `response_format`, `speech_config`, `transcription_config`, `builtin_tools`, `previous_interaction_id`, `store`, `video_task`. |

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

`sp::chat::InputMessage` is the shorter text/tool/media path. Its `media` vector is emitted in vector order before nonempty text. Use `canonical_messages` for caller-controlled Text/Media part order or retained native history.

A `Role::Tool` message may carry the results of several parallel tool calls in every family, and holds nothing else. Chat Completions takes one wire message per result, so the Chat encoder writes one `tool` message for each `ToolResult`, in the order given; one message of two results and two messages of one result each encode to the same bytes. Each result must still resolve exactly one pending call of the preceding assistant message. A Tool message that is empty or mixes in other parts is refused before dispatch as `InvalidRequest`, naming this rule.

Messages has no tool role on the wire. It now accepts a `Role::Tool` message of `ToolResult` parts only (previously `Unsupported`) and writes it as the `user` message that carries `tool_result` blocks, the same bytes as a `Role::User` message with those parts. The role still joins the message's content digest, so a later turn that continues sealed native history must resend each result message with the role it had when the history was sealed; rewriting it from `Tool` to `User` makes the history `ReplayIneligible`. The other families already accepted both roles.

Short constructors cover the common messages and add no behavior of their own. `Message::user_text`, `system_text`, `developer_text` and `assistant_text` build one `Text` part with an optional id. `Message::tool_result(tool_use_id, content, is_error = false)` builds a `Role::Tool` message with one legacy-text `ToolResult`; `content` is passed through unchanged. Generate needs that text to be one JSON object (a plain `"5"` is refused as `InvalidRequest`), the other families take any text, and `is_error` follows each family's wire: Messages and Interactions send the flag, Generate wraps the object as `error`, Responses refuses `true` as `Unsupported`, and Chat Completions has no field for it. Set `ToolResult::content_parts` yourself for typed text or media results.

Each family's request type has `Request::simple(model, prompt, max_tokens = {}, system = {})` and an overload taking a `std::vector<sp::Message>` history in place of the prompt. They set the model, the output cap and the family's own system member (a leading System message for Chat, `system` for Messages and Generate, `instructions` for Responses, `system_instruction` for Interactions) and leave every other control unset, so the family and model defaults apply as before. Chat's history overload fills `canonical_messages`, so sealed native history continues to work; that path writes message content as typed parts (`[{"type":"text",...}]`) where the prompt overload writes plain strings, which the hosted API treats alike, so keep the prompt overload for a compatible server that accepts only string content. In the other families both overloads write the same bytes. Assign further typed fields (`tools`, `reasoning`, ...) on the returned request.

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

A Responses function tool (`sp::responses::ToolDefinition`) has an optional `strict`. When it is unset, the policy's `strict_tools` default applies only to a strict-compatible `parameters` schema: the root is an object schema, and every object schema reached through `properties`, `items`, `prefixItems`, `anyOf`, `$defs` or `definitions` sets `"additionalProperties": false` and lists each of its properties in `required`. Any other schema is sent with `"strict": false`. An explicit `false` is sent as given. An explicit `true` on an incompatible schema returns `InvalidRequest` before dispatch, naming the tool and the first failing JSON pointer. The check is structural: it does not decide which other keywords strict mode supports, so the service still judges those.

## External runtimes and dotenv

SchemaProvider does not require NeoGraph. The [worked examples](../examples/README.md) build a standalone C++ tool loop using `cppdotenv::dotenv_values` and an example C ABI shared library loaded through Python `ctypes` by a LangGraph node. Both use the installed `SchemaProvider::transport` target and retain assistant history as typed SDK messages. The example is Chat text/client-tool-only; it is not a product Python binding or a typed-media FFI. The README records GIL, lifetime, async and credential-address-space limits and the pending Windows/macOS execution gates.

## Media inputs and generated outputs

`<core/media.h>` defines `MediaKind::{Image,Audio,Video,Document}` and `MediaSource::{Inline,Url,File}`. Inline input owns canonical padded base64 through `shared_ptr<const string>`; URL input carries an HTTPS location, and File carries a provider-held ID or Files-API URI. The SDK never reads, uploads or downloads files/URLs or decodes pixels/samples. The application supplies those bytes/references. `name` supplies a title/filename where representable; `detail` applies where the image wire supports it.

| Family | Input wire | Generated output |
|---|---|---|
| Chat | `image_url`, WAV/MP3 `input_audio`, PDF `file`; declared OpenRouter origins add documented audio/video/PDF forms and plain-text `file` data (verified live). | `audio` identity/transcript with request-derived format; OpenRouter `images[]`. |
| Responses | `input_image` and documented `input_file` document formats; declared OpenRouter origins add audio/video forms and plain-text/CSV `input_file` data (verified live). | Completed `image_generation_call`; other hosted tool metadata remains typed/opaque. |
| Messages | `image` and PDF/text `document`, using supported base64/URL/provider-file sources. Inline text documents decode to UTF-8. | No direct image/audio/video generation schema; hosted-tool file references remain in `ServerToolResult`. |
| Generate | `inlineData`/`fileData` image (HEIC/HEIF/AVIF included), audio, video and documented documents. | Typed `inlineData`/`fileData` preserves reported MIME parameters, spelled as reported (streamed TTS says `audio/l16; rate=24000; channels=1`); a part whose only data is `audioTranscription` (a transcription model's answer) is `Text`; consecutive inline audio frames of one concatenable MIME (raw PCM `audio/l16` or `audio/pcm`, MP3) are one `Media` part, streamed as `PartDelta`s; an empty text part without a signature is not a typed part; thought images remain opaque native parts. |
| Interactions | Image/audio/video/document `data` or `uri`; HEIC/HEIF included. Its enum `mime_type` receives the lowercase essence, not arbitrary MIME parameters. | Typed model-output media; thought-summary images stay in native JSON, not final images. |

Gemini endpoints reject `image/tiff` ("Unsupported MIME type"), so it is not admitted. Exact MIME/source lists are in [descriptor-policy.json](../config/descriptor-policy.json), not a promise that every model supports every type. MIME matching compares ASCII-case-insensitive essences. Arbitrary compatible hosts do not receive declared OpenRouter-origin privileges. Unsupported family/source forms return `Unsupported`, malformed input returns `InvalidRequest`, and media resource exhaustion returns `ResourceLimit`. Registered image magic/brand checks are structural; other media containers are not guessed from short prefixes.

`ToolResult::content_parts` is an ordered `std::vector<std::variant<Text, Media>>`; leave legacy `content` empty when supplying typed parts. Supplying both is `InvalidRequest` in families that support typed results. Existing tool-call ownership, role, part-count and media resource checks still apply. Tool output is not a general-purpose copy of the user-input schema:

| Family | Typed tool-result boundary |
|---|---|
| Chat | Nonempty `content_parts` returns `Unsupported`; use legacy text `content`. |
| Responses | Text plus admitted image/document forms in `function_call_output.output`; audio/video are `Unsupported`. |
| Messages | Text plus admitted image/PDF/text-document forms in user-role `tool_result.content`. |
| Generate | Text plus admitted inline image/audio/video, PDF and `text/plain` bytes; other document types, URL/file references, names, non-default image detail and generated identity/transcript metadata are `Unsupported`. |
| Interactions | Text plus admitted images in `function_result.result`; other media kinds, names, non-default image detail and generated identity/transcript metadata are `Unsupported`. |

Generate's function-response media forms were verified live on Gemini 3 (`gemini-3.1-flash-lite`, 2026-10-11): inline `image/png`, `image/jpeg`, `image/webp`, `image/gif`, `image/heic`, `application/pdf`, `text/plain`, `audio/wav` and `video/mp4` parts were accepted and read correctly. Multimodal function responses are a Gemini 3 feature; earlier models may reject them, and that remains the vendor's decision rather than an SDK admission rule. Interactions `function_result` was not part of that check and still rejects non-image media.

Generate's legacy `ToolResult::content` must parse as one JSON object, not arbitrary text or a JSON array. Typed text strings go to `functionResponse.response.output` (or `response.error` when `is_error`); inline media goes to the separate `functionResponse.parts` lane. Generate accepts alternating typed text/media and groups them into these separate ordered lanes. Each lane retains its order, but cross-lane positions are not represented; callers must not rely on cross-lane interleaving. Named blobs return `Unsupported`; no synthetic references are invented. Responses and Messages also reject tool-media controls/metadata their output wire cannot represent rather than silently dropping them.

Generate preserves `Media::name` as `displayName` on ordinary `inlineData`/`fileData` input and generated output. Its documented `video/audio/s16le` and `video/audio/wav` aliases classify as Audio, and `video/text/timestamp` as Document. Interactions ordinary input permits `name` only for Video (`Unsupported` otherwise), and emits the lowercase MIME essence.

Default decoded bounds are 5 MiB/image, 10 MiB/audio, 12 MiB/video and 10 MiB/document under one aggregate 16 MiB encoded-request budget. Base64/JSON overhead counts, so an individual bound does not promise that a request of exactly that size fits. External snapshots can select admitted resource bounds, not model capability or financial authority.

Messages decodes inline text documents to UTF-8 once and reuses the owned bytes during encoding. The retained decoded-text cache is bounded by the remaining aggregate request-byte budget as well as each document's decoded-byte bound; nested tool-result documents share that budget.

This function builds a request from an application-owned real HEIC file:

```cpp
#include <codecs/gemini_request.h>
#include <core/media.h>

sp::gemini::Request request_with_heic(std::string model, std::string_view file_bytes) {
    sp::gemini::Request request;
    request.model = std::move(model);
    auto bytes = std::make_shared<const std::string>(sp::base64_encode(file_bytes));
    request.messages.push_back(sp::Message{
        {}, sp::Role::User, {sp::Media::image("image/heic", std::move(bytes)),
                            sp::Text{"Describe this image."}}});
    return request;
}
```

Generation controls are explicit: Chat `OutputModality` with `AudioOutput{voice,format}` or declared-origin `ImageOutput`; Generate `ResponseModality`, `SpeechConfig`, `ImageConfig`, `AudioTranscriptionConfig`; Interactions an ordered `response_format` vector of `AudioResponseFormat`, `ImageResponseFormat`, `VideoResponseFormat` or `TextResponseFormat` (one entry emits an object, multiple entries an array), `SpeechConfiguration` and `TranscriptionConfig`. Interactions transcription uses the modern nested Verbatim/Smart mode; incompatible vocabulary/diarization/timestamp combinations reject before I/O. Responses requests images through `hosted_tools` with `ImageGenerationTool::output_format` (png/jpeg/webp). Select a model that supports the requested output.

Generated `Media` lives in `Completion::messages[].parts`: inline `data` is owned canonical base64; URL/file sources use `reference`; audio may carry `id`/`transcript`. Missing format evidence stays an empty MIME, not an assumed PNG/WAV/sample rate. Unsealed failure partials are description-only; sealed parts remain owned. Media descriptions/payloads and controls join native bindings, so copied bytes/provider IDs do not create replay authority.

OpenAI Chat audio SSE has an observed provider variation: metadata heartbeats may omit `choices`, and a late expiry-only audio delta after final usage may omit `finish_reason`. When validated, complete audio, final usage, `audio.expires_at`, `[DONE]` and normal transport close all agree, the SDK derives `StopKind::EndTurn` with raw stop evidence `derived:audio_usage+audio.expires_at+DONE`; it does not invent a provider finish reason. A declared OpenRouter origin orders the tail differently: the expiry-only delta comes first, then the final usage on a choice whose delta holds only the assistant role and empty text and whose `finish_reason` is null, then `[DONE]`. That order derives the same stop there and nowhere else. Heartbeats, usage alone, incomplete audio prefixes, missing terminal evidence or abnormal close cannot produce success or complete native replay authority. Ordinary Chat streams still require their chosen choice's finish reason and `[DONE]`.

Retain the owned messages/native sidecars, not just a text projection, when extending history. Chat accepts assistant content arrays and preserves generated-image native history; declared-origin parsed-PDF `file` annotations and URL citations remain owned raw-wire observations, not synthesized semantic parts or file/replay authority. Interactions `text_annotation_delta` transcription annotations merge into the current native text run; they do not reach backward across media or create a missing text part.

Dedicated image/video jobs, uploads/downloads, operation polling and Gemini Live WebSocket remain separate endpoints. Primary references: [OpenAI Chat](https://developers.openai.com/api/reference/resources/chat), [Responses](https://developers.openai.com/api/reference/resources/responses/methods/create), [Anthropic vision](https://platform.claude.com/docs/en/build-with-claude/vision)/[PDFs](https://platform.claude.com/docs/en/build-with-claude/pdf-support), [Gemini media](https://ai.google.dev/gemini-api/docs/image-understanding)/[Interactions](https://ai.google.dev/api/interactions-api), [OpenRouter audio](https://openrouter.ai/docs/guides/overview/multimodal/audio)/[images](https://openrouter.ai/docs/guides/overview/multimodal/image-generation). Schema research is not live API qualification.

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

In Chat, Responses and Messages requests, `provider` is `sp::OpenRouterRouting`, sent as OpenRouter's [`provider` object](https://openrouter.ai/docs/guides/routing/provider-selection) and admitted only at a declared OpenRouter origin. It holds optional `zdr`, `allow_fallbacks`, `require_parameters`, `data_collection` (`allow` or `deny`), the `only`/`order`/`ignore` provider lists and `max_price`. OpenRouter routes one model to endpoints with different prices, so `max_price` (`sp::OpenRouterMaxPrice`) caps the price of the endpoint it may choose; when no endpoint is priced at or below the cap, the request fails instead of reaching a dearer one. The optional amounts are integer nanoUSD (1e-9 USD, the `UsdAmount` unit): `prompt_nano_usd` and `completion_nano_usd` per million tokens, `image_nano_usd` per image, `audio_nano_usd` per audio unit and `request_nano_usd` per request. Each is sent as the exact decimal USD number, with no exponent or binary rounding: `75'000'000` is `0.075`, `15'000'000'000` is `15` and `100'000` is `0.0001`. Zero admits only free endpoints. An empty `max_price` or an amount above 2^53 nanoUSD rejects before I/O.

```cpp
sp::OpenRouterMaxPrice ceiling;
ceiling.prompt_nano_usd = 75'000'000;      // $0.075 per million prompt tokens
ceiling.completion_nano_usd = 300'000'000; // $0.30 per million completion tokens
sp::OpenRouterRouting routing;
routing.max_price = ceiling;
request.provider = routing;  // "provider":{"max_price":{"prompt":0.075,"completion":0.3}}
```

Routing joins the native configuration binding, `max_price` included: changing or removing the cap between turns makes captured native history `ReplayIneligible`. Routing without a cap keeps the binding it had before `max_price` existed. The cap filters endpoints by OpenRouter's listed prices; it bounds neither tokens nor total spending and grants no budget. Reported spending stays in `usage.provider_cost`. The linked library advertises this support as `sp::capability::OpenRouterRoutingMaxPrice`, which `RequiredProvider` includes. A consumer's own `require_interface_contract(sp::EXPECTED_INTERFACE_REVISION, sp::capability::RequiredProvider)` call, as in the README program, therefore throws `InterfaceContractError` against a library built without it instead of letting the cap be dropped. `Client` construction checks a library against its own constants and cannot detect this, so make that call at startup.

A declared OpenRouter origin may answer Responses and Messages requests with a different concrete `model` (a routing alias such as `~vendor/model-latest` resolves to a dated or provider-specific slug). That name is classified `Routed`, and a served model that changes within one response is still refused. Its Messages streams end with one extra `[DONE]` frame after `message_stop`, which is accepted only there and only once. [Served model identity and routing gateways](#served-model-identity-and-routing-gateways) says how names are classified elsewhere, how to declare an origin of your own and what the declaration turns on besides the model.

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

Generate `include_thoughts` and Interactions `thinking_summaries` have nullable family defaults: unset means no family-wide thought/summary control is sent, not an implicit `true` or `false`. Exact admitted model policies may still supply defaults; the embedded `gemini-2.5-flash-lite` policies retain `include_thoughts=true` for Generate and `thinking_summaries=true` for Interactions. Explicit caller values, including `false`, override inherited defaults and remain subject to model/family constraints. Nullable family defaults do not disable thinking or silently change known thinking-model defaults; media-only models do not inherit thought controls merely from their family.

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

The immutable family policy's `temperature_forbidden_model_prefixes` supplies model facts. Matching is ASCII case-insensitive against the full model or the suffix after its final `/`, so a gateway namespace does not bypass a prohibition. The embedded OpenAI facts cover gpt-5/gpt-6/o1/o3/o4 prefixes. Messages facts list the known Claude models and add the optional `temperature_forbidden_generations`, a rule by generation: every model of the `claude-` series from generation 4.7 on refuses `temperature`, whatever its family word (`opus`, `sonnet`, `haiku`, `fable` or one not yet released), dated suffix, gateway namespace or dotted gateway version (`anthropic/claude-opus-4.7`). The generation is the first one or two numbers of one or two digits after the series, so `claude-haiku-5-5` is 5.5 and `claude-opus-4-20250514` is 4.0 (a longer number is a date). A Claude id whose generation cannot be read is treated as the newest one and refused, which costs an omitted optional control instead of an HTTP 400. A snapshot without the key keeps only its listed prefixes. Loading a different closed policy creates a new identity, not a live edit to an existing client's behavior. Read the admitted facts for the selected deployment; do not guess temperature support from a model name alone.

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

## Interactions continuation and service tools

**Service tools.** `interactions::Request::builtin_tools` adds tools the service runs itself: `GoogleSearchTool` (with optional `search_types` such as `web_search` or `image_search`), `UrlContextTool` and `CodeExecutionTool`. Their steps decode as parts:
- **Call.** A `<tool>_call` step is a `ToolCall` of kind `ServerExecuted`, named after the tool (`code_execution`), with `wire_type` the step type and the step's `arguments` as `input`.
- **Result.** A `<tool>_result` step is a `ServerToolResult` whose `tool_use_id` is the step's `call_id` and whose `content` is the whole step.
- **Unknown tools.** Any such pair is admitted, so a tool the service adds decodes without a change.
- **Replay.** The steps stay in `wire_output` with their signatures and replay as received.

**Continuation by id.** The service keeps an interaction only when it is stored: set `store = true` (unset sends `false`). A stored completion's `messages[0].id` is the interaction id.
- **The next turn.** Set `previous_interaction_id` to that id and put only the new input in `messages`. A captured assistant or native message there is refused before dispatch.
- **No client replay.** The service holds the history, so the result has no native seal.
- **Chaining.** Store each turn that a later one continues.

**Video task.** `video_task` writes `generation_config.video_config.task` (`text_to_video`, `image_to_video`, `reference_to_video`, `edit`, `extend`; any lower-case token is sent). The service admits only user input with a video task. To edit a generated video in a second turn, continue it by `previous_interaction_id` without a task. On 2026-10-19, `gemini-omni-1.1-flash` returned a `video/mp4` both for the first turn and for the continued edit.

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

A `PartDelta` is already part of the partial output when `on_event` receives it. A call that then fails, is cancelled or times out reports every delta its callback was shown in `Failure::partial`, and `RetrySafety::OutputObserved`. A cancel, stop request or deadline is acted on when the running callback returns, including a cancel made from inside that callback, and no later event is delivered after that. A cancel made from another thread between two events lets at most one more event through, and that event is retained as well. Wire content the runtime had received but not yet delivered when the call was cut is not reported. A text `PartDelta` with empty text is never delivered. Wires that open a text part with an empty string (Chat's first chunk, a Messages text block, a Gemini text part) show `PartBegin` and then the first delta that carries text, so the first text delta can be counted as the first token. The empty delta is still applied: a text part that only ever received empty text is sealed as an empty `Text`, as a buffered response returns it, and `on_event` sees its `PartBegin` and `PartSeal` with no delta between. Thinking, refusal and tool-call deltas are delivered as the wire gives them.

An absolute `steady_clock` deadline includes preparation time, retained-handle waiting, DNS/connect/TLS, request/response I/O and any retry delay. Omitting it computes `now + Options::default_timeout` before encoding in `prepare`; moving the handle does not renew it. A stop requested before preparation or an already expired deadline is inspectable as an initial error; expiry during preparation is still checked before sending. Later cancellation cancels local work without waiting for peer progress. It cannot promise the remote service stops generating or billing. If the outcome was decided before cancellation was observed, that terminal outcome remains.

A Chat stream is bound to one `id` and `model`. Some OpenAI-compatible local servers stamp each chunk with the current time, so `created` may repeat or advance between chunks; it must still be an unsigned count on every chunk, and a decrease, like a changed `id` or `model`, fails with `ProtocolCorrupt`. Google's OpenAI-compatible endpoint leaves `finish_reason` out of every chunk before the last; on an open stream an absent `finish_reason` reads as `null`. The stream still needs a string `finish_reason` (or the audio evidence) before `[DONE]`, and a buffered choice still needs the member.

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

## WebSocket sessions

Realtime APIs (Gemini Live, Lyria RealTime, OpenAI Realtime and the like) speak WebSocket. `transport/ws_transport.h` opens an RFC 6455 client session on a `sp::transport::Transport`. The session shares that transport's I/O threads, resolver and TLS settings and costs no thread of its own. The provider protocols themselves (setup messages, audio chunks, turn events) are not decoded here yet: a session moves whole text and binary messages.

```cpp
#include <chrono>
#include <transport/ws_transport.h>

sp::transport::Transport transport;
sp::transport::WsRequest request;
request.url = "wss://api.openai.com/v1/realtime?model=gpt-realtime-2.1-mini";
request.headers.push_back({"Authorization", "Bearer " + api_key});
request.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(30);
request.idle_timeout = std::chrono::seconds(60);
sp::transport::WsCallbacks callbacks;
callbacks.on_message = [](sp::transport::WsMessageKind kind, std::string_view message) {
    // Copy what you need; the view ends with the call. Runs on an I/O thread: never block.
};
callbacks.on_done = [](const sp::transport::WsResult& result) { /* exactly once */ };
auto session = sp::transport::open_websocket(transport, std::move(request), std::move(callbacks));
session.send_text(R"({"type":"session.update","session":{"type":"realtime","output_modalities":["text"]}})");
// ... later, from any thread:
session.close();
auto result = session.join();  // result.clean: both Close frames were exchanged
```

- **Connection.** libcurl opens the connection (TCP, TLS with ALPN `http/1.1`, the private CA in `ca_file`), and the session writes the opening handshake and every frame itself. A server that does not answer `101` with the right `Sec-WebSocket-Accept`, or that selects an extension or subprotocol, fails the session with `FailureKind::Protocol`. `WsResult::http_status` keeps its status (for example 401).
- **Sending.** `send_text` and `send_binary` are thread-safe and queue the message. Messages sent before the handshake completes are written, in order, once it has. Text must be valid UTF-8. After `close()` or the end of the session, a send returns false.
- **Receiving.** Fragmented messages arrive whole. A Ping is answered with a Pong of the same payload. A server message larger than `limits.max_message_bytes` (64 MiB by default), a masked server frame, a reserved bit or opcode, or invalid UTF-8 fails the session after a Close frame with 1009, 1002 or 1007.
- **Closing.**
  - `close()` starts the closing handshake. `close_timeout` (5 s) bounds the wait for the server's Close.
  - A server's Close is answered with its code and reported through `on_close`.
  - The session is `Completed` only after a closing handshake. A connection lost without a Close frame is `Failed` with `close_code` 1006.
- **Bounds.** The absolute `deadline` covers the whole session. The optional `connect_timeout`, `handshake_timeout` (until the `101` head) and `idle_timeout` (no byte in either direction) behave as the HTTP stall bounds do. `cancel()`, destroying the handle and destroying the transport end the session at once, without a closing handshake.

The [Autobahn|Testsuite](../tests/autobahn/README.md) fuzzing server checks the framing and closing behaviour: every case 1 to 10 ends `OK` or `INFORMATIONAL`.

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

`Error::safe_message` is fixed per `ErrorKind` (every `ProtocolCorrupt` reads `invalid response protocol`), so it never carries vendor text or payload bytes, and it says nothing about which check failed. `Error::reason` does: a stable code of lower-case dotted segments, `<source>.<subject>.<problem>`, for the check of the SDK that failed. Examples: `responses.event.unsupported` (an event type the codec does not know), `chat.finish_reason.invalid`, `messages.delta.type_required`, `gemini.part.text_invalid`, `interactions.identity.changed`, `accumulator.part.delta_invalid` (a decoder's events broke the part lifecycle), `sse.utf8.invalid` (the stream is not valid UTF-8) and `transport.protocol` (HTTP-level protocol failure). Every decoder failure carries one, whatever its kind (`ProtocolCorrupt`, `Unsupported`, `ResourceLimit`, `Truncated`), and so does an in-band vendor error, which names the path it arrived by (`chat.remote.error_event`) next to its `vendor_code`. So does a request the SDK refuses before dispatch. `Client::prepare` replaces the encoder's text with the generic text of its kind (`invalid request`, `unsupported operation`), and the code names the check: `request.<area>.<check>`, where the area is the family encoder (`request.messages.temperature_is_prohibited_for_this_model`) or a check the encoders share (`request.choice.output_cap_exceeds_admitted_model_limit`, `request.media.media_payload_exceeds_the_decoded_size_limit`). A codec's `encode()` called directly returns the encoder's own text and no code. The code comes from a fixed table in the SDK, keyed by the decoder's own failure string, and is never built from the response, so it holds no vendor text, payload bytes or model output; it is not appended to `safe_message`. It is empty when no check of the SDK failed: transport and HTTP-status failures (including a close the transport reports while a decoder was still reading) keep using `kind`, `http_status` and `vendor_code`. `vendor_code` is the provider's machine-readable code for its own refusal. It is the value the error policy (`config/error-policy.json`) lists, which also sets `kind` and `retry_class`. Otherwise it is any value with the shape of a machine code, kept without changing the classification: Google's `details[].reason`, then its `status` (UPPER_SNAKE, such as `INVALID_ARGUMENT` or `NOT_FOUND`); Anthropic's `error.type`; OpenAI's `error.code`, then `error.type` (lower_snake). Each is at most 64 characters. Free text never has that shape, so a vendor message never reaches the error. A blocked Gemini answer carries its `finishReason` or `blockReason` (`SAFETY`, `PROHIBITED_CONTENT`), which `partial.stop->raw` also holds. Log it next to `safe_message` and put it in a field report. A code is the identity of a check: the decoder's wording may change, the code does not, and a new check adds a code. The linked library advertises the member as `sp::capability::ErrorReason`, which `RequiredProvider` includes, so a consumer's `require_interface_contract(sp::EXPECTED_INTERFACE_REVISION, sp::capability::RequiredProvider)` call refuses a library whose `Error` is smaller.

`Usage` distinguishes input/output/total, provider-reported total, uncached input, cache reads/writes and reasoning counts. `stage` is Missing/Partial/Final; `quality` is Consistent/Inconsistent with conflict details. Do not sum cache/reasoning bands into totals again without applying that family's inclusion rules. Do not substitute reported totals with derived ones or turn an absent count into zero. Error `attempt` and Completion `attempt` carry dispatched-attempt evidence, independent of token usage. A call that is cut off (cancelled, past its deadline, reset) keeps the usage the provider had reported before the cut, and nothing more: stage `Missing` with every counter absent when it had reported nothing, which is *no usage evidence* and not zero, and stage `Partial` when it had, whose counters are only what was seen so far. Neither is ever `Final`, so neither settles a call; a host decides what to hold or charge for it. A streamed Messages call reports its output count only through `message_delta`, because the output count in `message_start` is a placeholder (the first token, often none): a stream cut before that has the input counters, labelled partial, and no output counter.

Read-only projections shorten the common reads and do not replace the variants. `Message`, `Completion` and `PartialCompletion` have `joined_text()` (the `Text` parts in order; refusal, thinking, tool and media parts are not text), `joined_thinking()`, `first_refusal()`, `executable_tool_calls()` (only `ToolCallKind::ClientExecuted`; `ServerExecuted` and `ApprovalRequest` calls and `InvalidToolCall` never appear), `invalid_tool_calls()` and `has_invalid_tool_calls()`. `sp::is_completion`, `is_failure`, `as_completion`, `as_failure`, `as_error` and `joined_text` take an `Outcome` or a `Result` (a null `Result` answers false, null or empty). A `Failure`'s partial has the same projections, and partial text or tool fragments stay non-executable as above. `first_refusal()`, `executable_tool_calls()`, `invalid_tool_calls()` and the `as_*` pointer accessors borrow from the owner and are deleted on a temporary: bind the `Completion` or the `Result` to a variable first, and keep it alive while you use what they return.

`Usage::input_total_value()`, `output_total_value()`, `reasoning_value()`, `total_tokens_value()` and `cost_nano_usd_value()` return the bare number of one counter, and `nullopt` for an unknown one, never zero. They drop `Count::evidence` (Reported or Derived) and say nothing about `Usage::stage`, `quality` or `conflicts`, so a partial or inconsistent usage looks like a final one; read those fields, or the `Count` itself, before you settle or bill on a value. `total_tokens_value()` prefers `total` and falls back to `provider_reported_total`. `cost_nano_usd_value()` is the provider-reported USD total, present only when its status is `Available`, and is not an invoice.

`input_uncached` follows one rule in every family: it is `input_total − cache_read − cache_write`, marked `Evidence::Derived`, once `input_total` and `cache_read` are reported. A reported `cache_write` is always subtracted. An absent one counts as zero only where the wire has no cache-write counter that it always reports: Chat on origins other than a declared OpenRouter origin, Responses (direct OpenAI reports `input_tokens_details.cache_write_tokens`, OpenRouter omits it), Generate and Interactions. A declared OpenRouter Chat origin always reports `prompt_tokens_details.cache_write_tokens`, so when that counter is missing there `input_uncached` stays unknown. Messages reports the exclusive `input_tokens` itself (`Evidence::Reported`) and derives `input_total` from it plus both cache bands, so the identity holds by construction. Generate omits `cachedContentTokenCount` when nothing was read from cache; the SDK does not infer a zero read from that omission, so `input_uncached` is then unknown. Cache bands that exceed `input_total` are a conflict and leave `input_uncached` unknown.

`Usage::extra` holds unmapped vendor counters: integer counters that have no typed slot. It never repeats a typed counter or carries money, and its entries are not to be summed with the typed slots, because many are subsets, breakdowns or alternative views of them. Chat and Responses keep every other nonnegative integer usage leaf under its dotted wire path (for example `prompt_tokens_details.video_tokens`), while Chat's earlier names `input_audio`, `audio_tokens`, `accepted_prediction_tokens` and `rejected_prediction_tokens` are unchanged. Interactions omits `total_input_tokens`, `total_cached_tokens`, `total_thought_tokens` and `total_tokens` (they are `input_total`, `cache_read`, `reasoning` and `provider_reported_total`), keeps `total_output_tokens` (the response-only count; `output_total` is it plus `total_thought_tokens`, so the two differ whenever thoughts are reported) and `total_tool_use_tokens` (no typed slot), and keys modality breakdowns by name, as Generate does: `input_tokens_by_modality.text`. Generate derives `reasoning` zero (`Evidence::Derived`), `output_total` and `total` when `thoughtsTokenCount` is omitted but `totalTokenCount` equals `promptTokenCount` plus `candidatesTokenCount` with no nonzero `toolUsePromptTokenCount`; a larger total or a nonzero tool-use prompt count leaves them unknown. When the output budget is spent while thinking, `usageMetadata` has `promptTokenCount`, `thoughtsTokenCount` and `totalTokenCount` but no `candidatesTokenCount`; Generate then derives `output_total` as the total minus the prompt count and `total` from it, both `Evidence::Derived`, so such a call can still be settled from usage (a total below the prompt count, or thoughts beyond what the total leaves, is a conflict instead, and a nonzero tool-use prompt count leaves them unknown).

`Completion::raw_events` and `wire_envelope` retain owned JSON observations; failures keep their partial equivalents. A vendor 400 on Chat or Responses that names the member it refused (OpenAI's `error.param` with the code `unsupported_parameter` or `unsupported_value`) repeats that name in `Error::safe_message`, for example `invalid request: unsupported parameter 'max_tokens'`; only a plain lowercase member name is kept, never the vendor's message. A bounded non-2xx body may appear as `RawWire` type `http.error`, a diagnostic label rather than vendor message type. Retention is bounded by configured resources, not an unlimited wire dump. `Document` owns JSON; `Value`, string views and iterators borrow it. Raw JSON is not automatically safe to log, nor does it grant native replay authority. Text extraction is a presentation projection: keep the original outcome for storage/accounting/native continuation.

`Options::limits.max_error_bytes` bounds only the error bytes the runtime buffers and parses itself to classify a failure: a non-2xx body, or an in-band error the codec did not admit as a document. An in-band failure the codec already admitted under the response bounds is classified from that document whatever its size. A Responses `response.failed` repeats the whole output, so after a hosted image it is megabytes long; it still fails as `RemoteFailure`, or with the kind, retry class and `vendor_code` of an admitted error code, not as `ResourceLimit`.

Inspect `StopReason::kind` and `raw` rather than assuming all completions mean natural end. Length, refusal, filter, tool use and server pause retain different meanings. Execute only a sealed `sp::ToolCall` with `kind == ClientExecuted` under your host's tool policy. `InvalidToolCall`, `ServerExecuted` and `ApprovalRequest` do not authorize tool execution.

At the semantic event boundary, an otherwise natural `EndTurn` with non-server tool-call intent becomes `ToolUse`, including calls whose arguments later remain `InvalidToolCall`. The Stop event and final outcome use the same normalized kind. Specific MaxTokens/filter/unknown stops remain specific, and server-executed tools alone do not create client execution intent. A ToolUse stop never repairs malformed arguments into an executable call.

**Messages response metadata.** A code-execution `container` and cache `diagnostics` stay in `raw_events`; read `container.id` and `container.expires_at` there. Server-side `context_management` or `input_transformations` that changed the answer still fail the call as `Unsupported`.

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
