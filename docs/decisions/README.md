# Decision records

Design proposals only; nothing is implemented yet. Each record lists flip/reconsideration conditions, falsifiers and enforcement names.

| ID | Title | Status | Date | Decision | Open item |
|---|---|---|---|---|---|
| [D1](D1-transport.md) | Transport | GATED | 2026-10-01 | One private Asio + OpenSSL transport (HTTP/1.1, SSE, WS); no httplib/libcurl; flip to libcurl only on measured conditions | Owner confirmation that retiring opt-in HTTP/2 is acceptable; OS trust store |
| [D2](D2-non-chat-scope.md) | Non-chat scope | FIRM | 2026-10-01 | First release is chat + chat artifacts only; cutover gated on typed clients or approved deletion for Images/Veo/Decisions | Owner usage inventory |
| [D3](D3-async-native.md) | Async-native core | FIRM | 2026-10-01 | Bounded I/O workers, blocking facade over the same operation; gated by cancel and admission properties | NeoGraph target concurrency for the scheduled benchmark |
| [D4](D4-origin-and-binding.md) | Origin and binding facts | FIRM | 2026-10-01 | Exact-origin default gate plus recorded binding facts; documented equivalence class only after negative-control canary | Bedrock/Vertex direct credentials; OpenRouter invalid-signature cause |
| [D5](D5-rule-admission.md) | Descriptor rule admission | FIRM | 2026-10-01 | 3 real independent cases + truth table + tests; v1 has only omit and require_greater | Pre-validate against vendor docs; real fixture paths |

Machine-readable rule ledger: [rules.json](rules.json). Staged plan: [../ROADMAP.md](../ROADMAP.md).

## Status legend
- **FIRM**: decided; changed only via the listed reconsideration conditions.
- **GATED**: provisional default with a measurable flip condition; the default holds until a condition is measured as met.
- **Open item**: something still awaiting an owner input or measurement; never implied resolved by a FIRM/GATED status.

Deciders for all records: maintainer + two-family review panel. Evidence labels: `[read source]`, `[read docs]`, `[live observation]`, `[inference]`.
