# D4 — Origin, binding facts and replay of native reasoning

- Status: FIRM exact-origin/binding decision; equivalence/treatment proposals remain unimplemented, 2026-10-01
- Deciders: maintainer + two-family review panel

## Context
Signed/encrypted reasoning blocks must be replayed to the producing model to keep multi-step tool loops valid. Replaying them to the wrong route can fail (400) or be silently dropped.

## Correction note
Earlier drafts claimed that cross-host signature interoperability is "unverified" as a premise. That premise was **wrong**: Anthropic documents cross-platform compatibility of `signature` values together with model, prefix and account binding (see Evidence). What remains unverified is our own observation for Bedrock/Vertex direct access, not the vendor statement.

## Decision and implementation scope
1. The installed `NativeContext`/`NativeReplay` gate checks descriptor/origin, producing model, actual request prefix, tool/control scope, non-secret account scope and complete sealed content. Historical `Origin`/`BindingFacts` structs were sketches, not the current C++ surface.
2. Exact native eligibility or rejection is implemented; no caller-provided binding hash can mint a seal.
3. A documented direct/Bedrock/Vertex equivalence allow-list was proposed, requiring separate implementation and negative-control admission. No such active class exists in the installed SDK. OpenRouter was excluded from that proposal.
4. The live canary carries a NEGATIVE CONTROL (tampered signature must be rejected) and records `negative_control: rejected|accepted|not_run` plus `client_retention_verified`, `request_accepted`, `native_validation_evidenced`. A cell whose negative control is accepted/not_run is `ReplayAcceptanceUnobservable` and never "replay verified". Acceptance alone is vacuous because Anthropic degrades gracefully (a 200 was seen after stripping thinking in a same-turn loop), so the oracle also observes thinking presence / usage / cache evidence. Results are N-run acceptance rates with a lower confidence bound, with model and account age in the manifest. Property `CanaryNegativeControl`.
5. Equivalence-class admission: vendor documentation + 60-run no-failure canary per (host, model, direction) + 100% negative-control rejection. Bedrock/Vertex direct access is currently UNTESTED (no credentials on the owner's machine).
6. Per-model `required|optional|ignored` replay capabilities and explicit Drop/Demote were historical target grammar/treatments. The installed descriptor root has no `models` program, and the SDK exposes no Drop/Demote policy; ineligible native replay rejects.

Origin is a configured replay boundary, not proof of vendor issuer. In-process sealing provides provenance/content integrity; explicit independent-key archive custody authenticates local restoration, not encryption or vendor identity. Editable JSON cannot mint native authority.

**Current custody boundary.** The installed unstable SDK now admits persisted genuine native generations through explicitly provisioned/opened independent-key owner-private `NativeArchive` v3/`spna3` custody. This authenticates local custody/binding, not encryption or vendor issuer identity; editable JSON cannot mint replay authority, and v2 is not upgraded. [Current public contract and proof](../../README.md#owned-requests-results-and-configuration) supersede the M3 no-persistence checkpoint below. Documented-equivalence activation, cross-vendor native consumption and statistical model equivalence remain unclaimed; Google accepted single-carrier controls, and generic aggregate-omission400 is not cryptographic validation.

Interface 4 adds two distinct boundaries without changing archive format3. Generate `HistoryMode::PortableForeign` imports only unsealed portable assistant Text/client ToolCall, using the documented first-imported-function-call sentinel; genuine native groups still require exact seals. Responses cursors carry provider-held state and separate in-process completed terminal ownership; they are incomplete for full NativeReplay/archive use. The new policy/control identity can reject old policy-bound records. Generation cap is per-dispatch admission, not a native replay configuration field; all remaining origin/model/prefix/content/reasoning/tool-scope checks still apply. See [current usage](../USAGE.md#explicit-portable-gemini-history).

## Measured M3 implementation

`[live observation]` The private Messages path performs actual two-request loopback conversations for client tool results and server pause continuation. The second request uses the first HTTP response's captured completion. Thinking, signature and redacted leaves retain their bytes and order; server calls, caller metadata and complete result objects survive without becoming executable client calls. Twelve client-chain and ten server-chain mutations fail before dispatch, with zero requests observed.

`[read source]` `messages_request.cpp` derives context from the admitted descriptor and actual typed request. The accumulator seals complete content; the gate checks exact origin/route, model, declared non-secret account scope, system/tools/thinking configuration, preceding prefix and sealed message contents. Fingerprints use typed, length-prefixed SHA-256 and key-order-independent JSON object hashing. Callers supply no trusted fingerprint; a changed visible value cannot be legitimized merely by retaining its old immutable capsule.

At the M3 checkpoint these were trusted private in-process helpers: raw codec input or synthetic accumulator events were outside the boundary, and persisted/imported capsule admission did not yet exist. The account label was not authenticated credentials and SHA-256 was not issuer authentication. Only exact-origin Native-or-Reject was implemented, not documented-equivalence activation, Drop or Demote. Those historical limits are not changed by later protected archive admission; an invalid tool call is still retained for inspection, never repaired into replayable input.

M3 signatures, encrypted leaves and server IDs were synthetic local retention/rejection evidence, not a live canary or signature validity proof. Later M5/5.7–5.9 and [typed-cutover/qualification cohorts](../POC_PLAN.md#current-typed-c-cutover-provenance) are separate observations and do not establish universal native consumption.

## Evidence
- [read docs] Anthropic extended-thinking documentation: `signature` values are compatible across the Claude API, Amazon Bedrock and Google Cloud; a thinking block is readable only by the producing model and certain other models (unreadable blocks are silently ignored/dropped); newer models bind blocks to the preceding system/tools/messages prefix (400 when it changes; enforced by default for accounts created on/after 2026-08-31, opt-in through `thinking.block_binding.prefix_mismatch_behavior` for older accounts); Sonnet 5.5 blocks are account-bound; toggling thinking mid-turn silently disables thinking.
- [live observation] Experiment 2026-09-30, maintainer's keys, claude-haiku-4-5 / anthropic/claude-haiku-4.5; thinking enabled (budget 1024) + one tool; turn 1 captured thinking+tool_use, turn 2 replayed with tool_result. Routes: Anthropic-direct `/v1/messages`; OpenRouter `/v1/chat/completions` with `provider.order=[X]`, `allow_fallbacks=false`, X in {Anthropic, Amazon Bedrock, Google Vertex, Azure}.

| Replay path | Result | Reading |
|---|---|---|
| direct -> direct | 200, 8/8 | reliable |
| direct, tampered signature | 400 `Invalid signature in thinking block` | API validates |
| OpenRouter X -> same X; Anthropic<->Bedrock cross-backend | 200, 8/8 each | NOT evidence (see next row) |
| OpenRouter, tampered signature | 200 for every backend | OpenRouter path does not show validation |
| direct-issued -> OpenRouter (all backends) | 200 | same caveat |
| OpenRouter-issued (reasoning_details converted to `thinking` blocks, text unmodified) -> direct | mixed: 400 `Invalid signature` in 8/20 and 10/20 runs (~40-50%), every backend incl. "Anthropic" | not reliably replayable; cause unidentified |
| thinking stripped in same-turn tool loop, direct | 200 (one sample) | API did not enforce replay there |

Caveats: small samples; one model; one account; no direct Bedrock/Vertex credentials, so those only appear through OpenRouter; failure cause unknown (uncorrelated with quotes, whitespace, text length or backend); unknown whether OpenRouter forwards replayed thinking at all; scripts were scratch and are not in any repository.

## Options considered
- Exact origin only: kept as default; too strict alone, ignores model/prefix/account binding.
- Global "same family = interoperable": rejected (OpenRouter data; account/prefix binding).
- Always Drop: rejected as default (breaks tool loops where replay is required).

## Consequences
Native sidecars retain binding facts and codecs check them before dispatch. The SDK rejects foreign/edited carry rather than automatically demoting OpenRouter reasoning or stripping it and retrying. Historical demotion proposals do not change that current behavior.

## Reconsideration conditions
Vendors change binding rules (example: 2026-08-31 default enforcement) — canary must detect; equivalence class admission evidence obtained for a cell; OpenRouter failure cause identified and fixed with negative control rejecting.

## What would falsify it
A negative control accepted on a supposedly validating cell; a bound-by-account block replayed successfully across accounts where docs say it cannot; a 60-run canary failure in an admitted class.

## Enforcement
Properties `OriginBindingFacts`, `CanaryNegativeControl`, `NativeRetentionForeignGate`, `KnownCorruptNeverIgnored`; canary manifest schema check.

## Open items
- Bedrock/Vertex direct credentials for the equivalence canary.
- OpenRouter invalid-signature cause.
