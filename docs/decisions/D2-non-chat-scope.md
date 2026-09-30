# D2 — Non-chat scope

- Status: FIRM, 2026-10-01
- Deciders: maintainer + two-family review panel

## Context
NeoGraph's current provider descriptors also drive non-chat features: long-running `operation` (Veo), `artifacts`, `prompt_template`/`prompt_field` (standalone image endpoints), `request_json`, and the OpenRouter Decisions endpoint. The proposed library must decide whether these belong in its Event/Completion model.

## Decision
The first release covers **chat completion APIs only**: Chat Completions, Responses, Messages, Gemini generate, Interactions (subject to the staged ladder in ../ROADMAP.md), including artifacts that arrive inside chat responses (`Completion.artifacts`).
Not in the first release and not covered by the Event set: Veo long-running operations, standalone image endpoints, the OpenRouter Decisions endpoint. The property `OperationLifecycle` is dropped.

Clean-cutover rule (owner principle: no shims, no half migration): NeoGraph's cutover deletes the old descriptor-grammar interpreter for operation/artifacts/prompt_template/prompt_field, `request_json` and `SchemaPrimitiveRegistry`. Any of the three features the owner keeps is rewritten by NeoGraph as a **typed client on NeoGraph's own async HTTP layer** (never interpreting the old grammar) in the same cutover. No feature is deleted without explicit owner approval.

Decisions endpoint: typed per the CURRENT official OpenRouter schema (the repo fixture is stale: criteria array vs an object in the official schema; response model/answers/usage), alpha endpoint, never surfaced as a chat Completion and never a fake EndTurn.

## Evidence
- [read source] A repo grep found no application callers of these features in NeoGraph `src/`, `examples/` or skills; only tests, Python-binding tests and opt-in live snippets.
- [read docs] The OpenRouter Decisions schema differs from the repo fixture (see above).
- [inference] Folding long-running operations into chat Events would force an operation lifecycle and polling semantics into every codec.

## Options considered
- Endpoints target now: rejected (scope growth before any chat cell ships; two-person team).
- Keep the old grammar interpreter in NeoGraph: rejected (half migration; two interpreters).
- Fold operations into Events: rejected (see inference).

## Consequences
- The library's first release is not blocked by this ADR; NeoGraph's **cutover completion is gated** on it.
- Descriptor grammar v1 has no operation/artifact-request/prompt keys.

## Cutover checklist (NeoGraph side, all required before the cutover is called complete)
1. **Delete-list**: old interpreter for operation/artifacts/prompt_template/prompt_field, `request_json`, `SchemaPrimitiveRegistry`, and their tests and bindings.
2. **Owner inventory input**: the owner lists live users of Images/Veo/Decisions; the list decides keep (typed client) versus delete.
3. **Approval before any deletion**: no item on the delete-list is removed without explicit owner approval recorded in the cutover change.
4. **Half-migration test**: after cutover NeoGraph source contains zero code interpreting the old descriptor grammar (automated search gate).
5. Every kept feature has a typed client on NeoGraph's async HTTP layer, landing in the same cutover.

## Reconsideration conditions
Owner inventory confirms live users AND a second consumer appears, OR NeoGraph would otherwise hold two polling clients. **Resume path (minor release)**: an optional `Endpoints` target for Images/Veo inside this repo; chat Event and Completion stay untouched.

## What would falsify it
A live user of Images/Veo/Decisions surfaced by the inventory that cannot be served by a typed client.

## Enforcement
CI/cutover gate: half-migration search; `DescriptorRuleAdmission` (no grammar keys for non-chat features); ROADMAP Stage 4 exit criteria.

## Open items
- Owner usage inventory for Images/Veo/Decisions.
