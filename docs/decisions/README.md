# Decision records

These records preserve decisions and historical evidence alongside current interface4 scope in SDK0.1.0 alpha. [Usage](../USAGE.md), [Migration](../MIGRATION.md) and [recorded interface3 provenance](../POC_PLAN.md#current-typed-c-cutover-provenance) separate installed controls/cursor/portable-history paths from old layouts/importers and future equivalence/rules. A FIRM decision is not proof every gate ran.

| ID | Title | Status | Date | Decision | Open item |
|---|---|---|---|---|---|
| [D1](D1-transport.md) | Transport | FIRM (revised) | 2026-10-01 | One libcurl stack; optional HTTP3 implemented and locally exercised plain/ASan+UBSan/TSan with release backend libraries; safe one-POST fallback and incapable-build rejection | Hosted-provider/platform matrix; Responses WebSocket excluded (the WebSocket client serves realtime sessions, D8) |
| [D2](D2-non-chat-scope.md) | Non-chat scope | SUPERSEDED by D6 | 2026-10-01 | SDK chat/artifacts only; NeoGraph retains separate typed Images/Veo/Decisions clients and authority. Approved single-shot media observations are recorded, not universal endpoint qualification | Future SDK Endpoints scope/qualification; no additional paid authority |
| [D3](D3-async-native.md) | Async-native core | FIRM | 2026-10-01 | Bounded I/O workers, blocking facade over the same operation; gated by cancel and admission properties | NeoGraph target concurrency for the scheduled benchmark |
| [D4](D4-origin-and-binding.md) | Origin and binding facts | FIRM | 2026-10-01 | Implemented exact-origin/content/binding rejection and explicit archive custody; equivalence/Drop/Demote remain historical proposals | Separate cross-origin implementation/admission; direct credentials and gateway cause evidence |
| [D5](D5-rule-admission.md) | Descriptor rule admission | FIRM | 2026-10-01 | Admission policy:3 independent cases + truth table + tests; original target v1 only omit and require_greater, not current installed constraint execution | Future rule implementation/admission, vendor-doc review and real fixture paths |
| [D6](D6-endpoints.md) | Non-chat endpoints | FIRM | 2026-10-19 | Endpoint request and result types of their own on the chat client's lifecycle; closed policy facts per endpoint; typed results with the raw document; a charge-on-accept call is never retried once it may have reached the provider; images and decisions first | The other endpoints in 0.4.x; jobs and file sinks with the first job endpoint |
| [D7](D7-files-batches.md) | Files, batches and resources | FIRM | 2026-10-19 | Chunked provider protocols, and a hashed file slice for single-shot uploads; one `files::File`; multi-step calls joined by journalable handles; charge-on-accept batch submit with JSONL results; typed lookups and one policy-resolved call for the stores; a pure webhook verifier | Whether models, statistics and the stores may follow 0.4.0 |
| [D8](D8-realtime-sessions.md) | Realtime sessions | FIRM | 2026-10-20 | One provider-neutral session with typed commands and events and a per-turn `Completion` projection; pull inbound; two outbound lanes; the host reconnects; a per-turn lease; closed realtime facts; conformance against the official SDKs | Timing of translation, Lyria RealTime and GPT-Live |

Machine-readable rule ledger: [rules.json](rules.json). Staged plan: [../ROADMAP.md](../ROADMAP.md).

## Status legend
- **FIRM**: decided; changed only via the listed reconsideration conditions.
- **GATED**: provisional default with a measurable flip condition; the default holds until a condition is measured as met.
- **Open item**: something still awaiting an input or measurement; never implied resolved by a FIRM/GATED status.

Evidence labels: `[read source]`, `[read docs]`, `[live observation]`, `[inference]`.
