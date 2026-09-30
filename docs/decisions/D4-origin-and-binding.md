# D4 — Origin, binding facts and replay of native reasoning

- Status: FIRM (exact-origin default + documented equivalence class), 2026-10-01
- Deciders: maintainer + two-family review panel

## Context
Signed/encrypted reasoning blocks must be replayed to the producing model to keep multi-step tool loops valid. Replaying them to the wrong route can fail (400) or be silently dropped.

## Correction note
Earlier drafts claimed that cross-host signature interoperability is "unverified" as a premise. That premise was **wrong**: Anthropic documents cross-platform compatibility of `signature` values together with model, prefix and account binding (see Evidence). What remains unverified is our own observation for Bedrock/Vertex direct access, not the vendor statement.

## Decision
1. `Origin = (family, vendor, authority, route_scope)`; exact equality stays the DEFAULT replay gate (necessary, not sufficient).
2. Each capsule ALSO records binding facts the codec checks before dispatch: producing model id; a context fingerprint of the replay prefix (system, tools, preceding messages as sent); account/credential-profile scope (non-secret). Property `OriginBindingFacts`.
3. A documented equivalence class (`anthropic.messages` across direct / Bedrock / Vertex) is a C++ allow-list in the Messages codec with status `Documented-Unverified`, activated per canary cell only after a negative-control canary passes. OpenRouter is NEVER in an equivalence class.
4. The live canary carries a NEGATIVE CONTROL (tampered signature must be rejected) and records `negative_control: rejected|accepted|not_run` plus `client_retention_verified`, `request_accepted`, `native_validation_evidenced`. A cell whose negative control is accepted/not_run is `ReplayAcceptanceUnobservable` and never "replay verified". Acceptance alone is vacuous because Anthropic degrades gracefully (a 200 was seen after stripping thinking in a same-turn loop), so the oracle also observes thinking presence / usage / cache evidence. Results are N-run acceptance rates with a lower confidence bound, with model and account age in the manifest. Property `CanaryNegativeControl`.
5. Equivalence-class admission: vendor documentation + 60-run no-failure canary per (host, model, direction) + 100% negative-control rejection. Bedrock/Vertex direct access is currently UNTESTED (no credentials on the owner's machine).
6. Reject-by-default is a policy with an availability cost: per-model capability (`required|optional|ignored` replay of thinking, with evidence) is recorded in descriptor `models`, so Reject applies where replay is required and Drop is the explicit choice elsewhere.

Origin is an observed/configured replay boundary, NOT cryptographic proof of issuer. `sealed` means C++ immutability + provenance, not authentication. Importer input is untrusted; persistent capsules need a host-held AEAD/HMAC envelope or are not replayed natively (see DESIGN threat model).

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
Capsule carries binding facts; codec verifies before dispatch; availability cost of Reject is explicit per model; OpenRouter reasoning is demoted rather than replayed to other hosts (demotion uses an assistant-role quoted block or explicit untrusted delimiter, never a user-role instruction).

## Reconsideration conditions
Vendors change binding rules (example: 2026-08-31 default enforcement) — canary must detect; equivalence class admission evidence obtained for a cell; OpenRouter failure cause identified and fixed with negative control rejecting.

## What would falsify it
A negative control accepted on a supposedly validating cell; a bound-by-account block replayed successfully across accounts where docs say it cannot; a 60-run canary failure in an admitted class.

## Enforcement
Properties `OriginBindingFacts`, `CanaryNegativeControl`, `NativeRetentionForeignGate`, `KnownCorruptNeverIgnored`; canary manifest schema check.

## Open items
- Bedrock/Vertex direct credentials for the equivalence canary.
- OpenRouter invalid-signature cause.
