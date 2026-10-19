# D6 — Non-chat endpoints

- Status: FIRM, 2026-10-19. Supersedes [D2](D2-non-chat-scope.md).
- Deciders: maintainer + review panel (eight seats from four model families, two rounds)

## Context
D2 kept the SDK chat-only: NeoGraph held its own typed clients for OpenAI image generation, Veo and OpenRouter Decisions, on its own HTTP layer. On 2026-10-18 the owner reversed that. Every provider surface moves into interface 7 (0.4.0), and NeoGraph 0.16.0 deletes its typed clients (SchemaProvider#58, #59, #61).

The endpoints differ from the chat families in shape, not in what a host needs from a call: a durable receipt before dispatch, an absolute deadline, cancellation, stall bounds, a shared retry budget, a stable error reason and the retained wire. They also differ among themselves. Images are base64 or URLs, speech is binary audio, embeddings are vectors, decisions are per-question answers, and video is an asynchronous job.

## Decision
- **Placement.** An endpoint has request and result types of its own, dispatched by the existing `runtime::Client` through separate entry points. It shares the client's admission slots, retry budget, stall bounds, cancellation, deadline handling and error classification. The chat `Request`, `Outcome` and `Completion` stay as they are, and a test holds them to that.
- **One client per origin.** An endpoint call uses the client's origin, credential and descriptor headers. Its policy fact says which origins serve it.
- **Results.** Typed per endpoint, keeping the raw response document. A downloaded body can be written to a file as it arrives; the result then keeps its content type, size and SHA-256 instead of the bytes.
- **Admission.** Closed policy facts per endpoint name the origins, the paths and the schema of the members a request may add beside the typed ones. Codecs are compiled per shape. Models come from the request and the host's catalogue; as for the chat families, the catalogue never refuses a model, and the provider judges one it does not know.
- **Jobs.** An asynchronous job is a journalable `jobs::Handle` value, advanced by ordinary submit, poll, cancel, download and delete calls. The Interactions background lifecycle keeps its chat form; a handle can be projected from a pending interaction, so a host has one job vocabulary.
- **Money.** An endpoint call that costs money when the provider accepts it is never retried once it may have reached the provider. It is retried only when it provably never did, or when the provider refused it before accepting it (429, 503). Polls, gets and downloads keep the ordinary budget. A dispatched call with no outcome is never resubmitted automatically.
- **Streaming.** Buffered first. Results are shaped so a later stream (partial images, speech and transcript deltas) extends them without a new result type.
- **Scope of 0.4.0.** Image generation and edits (OpenAI and OpenRouter) and decisions (OpenRouter and OpenAI), which is what NeoGraph's deleted clients covered. Veo is not ported: its preview models end on 2026-10-22, and Gemini Omni through Interactions replaces it. The other endpoints follow in 0.4.x.

## Evidence
- [live observation] Every endpoint in SchemaProvider#59 answered a live call on 2026-10-18.
- [read source] NeoGraph's ImagesClient calls OpenAI `/v1/images/generations` (and Gemini through generateContent, which the SDK already serves), and its DecisionsClient calls OpenRouter `/api/alpha/decisions`.
- [read docs] OpenAI image edits take a JSON body whose images are URLs, data URLs or file ids, so the first slice needs no multipart upload.

## Options considered
- A separate endpoint client per origin: rejected. It would split the pool, the retry budget and the admission slots that the chat client owns.
- New alternatives in the chat `Request` and `Outcome`: rejected. Every chat consumer would have to handle results it never asks for.
- One generic endpoint result (media parts plus a JSON document): rejected. Typed results catch shape errors in the SDK, not in the host.
- Refusing a model a stale catalogue does not list: rejected. The catalogue informs; it never refuses (SchemaProvider#69).
- SSE from the start: deferred. The buffered forms answer every endpoint of the first slice.

## Consequences
- `runtime::Client` gains endpoint entry points; `PreparedEndpoint<T>` is a prepared endpoint call with a typed result, journaled like a `PreparedRequest`.
- The descriptor policy gains `endpoints` facts. The policy identity does not change with them, since they bind no sealed history.
- NeoGraph 0.16.0 deletes ImagesClient, VeoClient and DecisionsClient once the first slice ships.

## Reconsideration conditions
A provider whose endpoint needs state the client cannot share (a separate credential scheme for the same origin), or a consumer that needs endpoint results in the chat event stream.

## Enforcement
- Tests:
  - the chat request and outcome layouts are unchanged by the endpoint types;
  - an endpoint call is refused at prepare, with no I/O, for an origin, path or member its fact does not admit;
  - a charge-on-accept call that may have reached the provider is attempted once.
- The live e2e report carries one call per endpoint of the slice.
