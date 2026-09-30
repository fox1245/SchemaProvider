# D5 — Descriptor rule admission

- Status: FIRM, 2026-10-01
- Deciders: maintainer + two-family review panel

## Context
Descriptors are data; every stateless rule kind added to the grammar adds an interpreter path, a bump to the descriptor major version and review burden. Prior drafts carried `RequireEqualWhen` without a real case.

## Decision
A new stateless rule kind is admitted only with:
1. **3 independent real cases**: different vendor, source (doc URL/date or reproduction log) and model family. Two models from one vendor doc count as one case. Each case has a fixture that fails when the rule is removed.
2. A full truth table over semantic equivalence classes: enum values + unknown, absent/null/wrong type, explicit vs library-default provenance, numeric <, =, > and boundaries.
3. Permutation/idempotence tests: output identical under any rule order; contradictions rejected at load.
4. A mutant check.
5. A prototype showing no increase in production C++ files touched and >= 30% less duplicated semantic code (a policy choice, not a natural constant).

Forbidden forever, regardless of case count: state, ordering/chaining, dynamic paths, callbacks/hooks, I/O, vendor-name conditions, message reordering, event-meaning changes.

Every new rule bumps the descriptor major version. **v1 ships only rules with a real case in current NeoGraph**: `omit` (with `when.in`) and `require_greater`; the bootstrap waiver is recorded in `rules.json`.
`RequireEqualWhen` has NO real case (verified: the current NeoGraph interpreter parses only omit and require_greater) and is **removed from v1** until a real case is submitted.
`require_greater` needs a selector/exemption: Anthropic documents that `budget_tokens` may exceed `max_tokens` with interleaved thinking, so it is modelled as conditional on a selector slot.

Machine-readable ledger: `rules.json`. CI gate: loader rule-kind enumeration == ledger.

## Evidence
- [read source] Current NeoGraph interpreter handles only `omit` and `require_greater`.
- [read docs] Anthropic documents the interleaved-thinking exemption for `budget_tokens` vs `max_tokens`; another documented request shape (block binding with a between-tools option returning 400) is a mutual-exclusion rule, i.e. a fourth shape, so the rule set must be checked against current vendor docs before freezing.
- [inference] Three-case threshold is a policy to resist single-vendor special-casing.

## Options considered
- Admit on one case: rejected (vendor-specific logic disguised as data).
- Turing-complete expressions/hooks: rejected (forbidden list).
- Keep RequireEqualWhen "for symmetry": rejected (no case).

## Consequences
Anything not admissible lives in family C++ tables. Descriptor grammar stays small and closed; the ledger is the audit record.

## Reconsideration conditions
Threshold (3 cases, 30%) may be revisited only with a recorded rationale and data from at least two rule submissions.

## What would falsify it
A rule admitted that fails the permutation test; a ledger/loader mismatch; a real vendor case that needs a forbidden construct (then it goes to C++).

## Enforcement
Property `DescriptorRuleAdmission`; `JournalVersionAndCanonicalOrder` for descriptor versioning; CI: loader enumeration == `rules.json`, permutation and mutant jobs.

## Open items
- Pre-validate the v1 rule set against current vendor docs before freezing.
- Replace placeholder fixture paths in `rules.json` with real fixtures.
