# D2 — Non-chat scope

- Status: FIRM, 2026-10-01
- Deciders: maintainer + two-family review panel

**Current cutover status.** The approved typed C++ cutover removed the old interpreter and retained separate typed Images/Veo/Decisions clients in NeoGraph, not in the SDK. [Current integrated proof and single-shot media observations](../../README.md#persistent-qualification-campaign-completed-observations-explicit-limits) record the actual retained paths and separate authority. Image/video/Decisions observations are bounded one-shot evidence, not broad endpoint qualification, invoices or permission for more generation. The original design/checklist below remains the acceptance rationale, not today's unfinished cutover list.

## Context
At the original design checkpoint, NeoGraph's provider descriptors also drove non-chat features: long-running `operation` (Veo), `artifacts`, `prompt_template`/`prompt_field` (standalone image endpoints), `request_json` and OpenRouter Decisions. The library design had to decide whether these belonged in its Event/Completion model; those old descriptor-interpreter paths are now removed.

## Decision
The SDK covers **chat completion APIs only**: Chat Completions, Responses, Messages, Gemini generate and Interactions, including artifacts that arrive inside chat responses. The staged release gates remain in ../ROADMAP.md; implemented families do not imply every release cell is qualified.
Not in the first release and not covered by the Event set: Veo long-running operations, standalone image endpoints, the OpenRouter Decisions endpoint. The property `OperationLifecycle` is dropped.

Clean-cutover rule (owner principle: no shims, no half migration): NeoGraph's cutover deletes the old descriptor-grammar interpreter for operation/artifacts/prompt_template/prompt_field, `request_json` and `SchemaPrimitiveRegistry`. Any of the three features the owner keeps is rewritten by NeoGraph as a **typed client on NeoGraph's own async HTTP layer** (never interpreting the old grammar) in the same cutover. No feature is deleted without explicit owner approval.

Decisions remains a separate typed NeoGraph endpoint matching the reviewed official OpenRouter request/response schema, not the old criteria-array fixture. It is an alpha endpoint, never surfaced as a chat Completion or fake EndTurn; one JeV1.13 observation does not establish broad endpoint support.

## Evidence
- [read source] A repo grep found no application callers of these features in NeoGraph `src/`, `examples/` or skills; only tests, Python-binding tests and opt-in live snippets.
- [read docs] The OpenRouter Decisions schema differs from the repo fixture (see above).
- [inference] Folding long-running operations into chat Events would force an operation lifecycle and polling semantics into every codec.

## Options considered
- Endpoints target now: rejected (scope growth before any chat cell ships; two-person team).
- Keep the old grammar interpreter in NeoGraph: rejected (half migration; two interpreters).
- Fold operations into Events: rejected (see inference).

## Consequences
- The library's first release is not blocked by this ADR. The original NeoGraph cutover gate required explicit retained/deleted feature ownership; the approved cutover now retains typed clients and removes the old interpreter. Future SDK Endpoints support still requires a separate scope/qualification decision.
- Descriptor grammar v1 has no operation/artifact-request/prompt keys.

## Original cutover checklist (NeoGraph side; historical acceptance requirements)
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
- Future SDK Images/Veo Endpoints scope and qualification remain separate decisions; retained NeoGraph typed clients and their one-shot observations do not admit them into the chat SDK.
