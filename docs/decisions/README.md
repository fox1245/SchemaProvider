# Decision records

Decision records with historical design and current implementation guidance. Current installed SDK is unstable0.0.0/interface3/SOV3, with actual owner-approved typed C++ integrated cutover proof recorded, not stable/released or fully qualified. [Current provenance](../POC_PLAN.md#current-typed-c-cutover-provenance) and [public installation/configuration/custody contracts](../../README.md#install-and-use-the-unstable-c-sdk) supersede private/not-installed, legacy importer/adapter and WS sketches. D1's exact HTTP3 proof and all historical cohorts remain intact; no broad platform/vendor qualification is inferred.

| ID | Title | Status | Date | Decision | Open item |
|---|---|---|---|---|---|
| [D1](D1-transport.md) | Transport | FIRM (revised) | 2026-10-01 | One libcurl stack; optional HTTP3 implemented and locally exercised plain/ASan+UBSan/TSan with release backend libraries; safe one-POST fallback and incapable-build rejection | Hosted-provider/platform matrix; WebSocket deferred/excluded |
| [D2](D2-non-chat-scope.md) | Non-chat scope | FIRM | 2026-10-01 | SDK chat/artifacts only; NeoGraph retains separate typed Images/Veo/Decisions clients and authority. Approved single-shot media observations are recorded, not universal endpoint qualification | Future SDK Endpoints scope/qualification; no additional paid authority |
| [D3](D3-async-native.md) | Async-native core | FIRM | 2026-10-01 | Bounded I/O workers, blocking facade over the same operation; gated by cancel and admission properties | NeoGraph target concurrency for the scheduled benchmark |
| [D4](D4-origin-and-binding.md) | Origin and binding facts | FIRM | 2026-10-01 | Exact-origin default gate plus recorded binding facts; documented equivalence class only after negative-control canary | Bedrock/Vertex direct credentials; OpenRouter invalid-signature cause |
| [D5](D5-rule-admission.md) | Descriptor rule admission | FIRM | 2026-10-01 | Admission policy:3 independent cases + truth table + tests; original target v1 only omit and require_greater, not current installed constraint execution | Future rule implementation/admission, vendor-doc review and real fixture paths |

Machine-readable rule ledger: [rules.json](rules.json). Staged plan: [../ROADMAP.md](../ROADMAP.md).

## Status legend
- **FIRM**: decided; changed only via the listed reconsideration conditions.
- **GATED**: provisional default with a measurable flip condition; the default holds until a condition is measured as met.
- **Open item**: something still awaiting an owner input or measurement; never implied resolved by a FIRM/GATED status.

Deciders for all records: maintainer + two-family review panel. Evidence labels: `[read source]`, `[read docs]`, `[live observation]`, `[inference]`.
