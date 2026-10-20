# D8 — Realtime sessions

- Status: FIRM, 2026-10-20. Builds on [D6](D6-endpoints.md) and the WebSocket transport of SchemaProvider#73.
- Deciders: maintainer + review panel (eight seats from four model families, two rounds)

## Context
Google Live (`BidiGenerateContent`, also serving live transcription and translation models), Google Lyria RealTime, OpenAI Realtime and OpenAI GPT-Live are full-duplex WebSocket protocols. The SDK has a client-only RFC 6455 transport (Autobahn cases 1 to 10 pass) that carries whole text and binary messages and decodes no provider event. A chat call is one request and one outcome; a realtime session is many turns, interleaved audio both ways, barge-in, tool calls, resumption handles and server drains, billed per turn while it is open. NeoGraph journals every provider call and enforces money budgets per run.

## Decision
- **Shape.** One provider-neutral `realtime::Session`: typed client commands, one typed server event stream, a codec per provider behind it. Each finished or interrupted conversation turn is also projected into the existing `Completion`, so a chat consumer reuses its code. Session notices (resumption, drain) never project; a turnless stream (music) stays on the event stream.
- **Inbound.** Pull is the contract: `next_event()` over a bounded queue, in its blocking form. A coroutine form and a push adapter over the same queue are not shipped; a host that wants either wraps `next_event`. Media back-pressure never holds back control events (tool calls, drain notices, errors). The WebSocket session cannot pause its reads yet: past a hard cap of queued model audio, or of the other queued events, the session ends with a stable reason, and a read pause is added to the transport if a live run shows the cap is reached.
- **Outbound.** Two lanes. Control preempts: cancel, tool results, close. A bounded media ring (about 2 s of PCM by default) drops the oldest frames or suspends the sender, by session kind, and counts what it dropped. Turn markers (commit, activity end) are fenced behind the media they cover; a drop never crosses a turn marker, so audio ahead of a pending marker stays and, when only that audio is left, the new audio is refused.
- **Lifecycle.** The host reconnects. Resumption handles, drain notices and expiry are typed events the host journals before it acts. The SDK builds the setup that resumes from a handle; it never opens a socket on its own. A provider that ends a failed session with a close code of its own (a wrong key) is reported as the error the runtime error policy gives the status that the fact's `close_codes` entry names, not as a clean close.
- **Money.** After each settled turn the session calls the host's `renew` with the provider's usage for that turn: the totals and the counts by modality. A refusal ends the session with a stable reason. The wall-time bound is the session's `max_duration`, or its `deadline`. Counting audio seconds, and an in-turn watermark that bounds spend within a turn, are the host's: the SDK keeps the events that carry the audio and does not count it. The SDK prices nothing; the host's budget decides.
- **Admission.** Closed per-endpoint policy facts: the origin, the WebSocket path, where the model goes (a query parameter or the setup message), the authentication header, literal headers (never the ones the handshake sets), an allow-list of setup members with value domains, the audio formats the endpoint takes and sends (`audio`: media type, rate and bytes per sample, the first the default), and the close codes with which the provider ends a failed session (`close_codes`). A setup is validated when the session is prepared, with no I/O. Session duration and queue sizes are `realtime::Options` of the host, not facts. As for chat, the catalogue never refuses a model; the facts say where the model goes, not which models.
- **Scope of 0.4.0.** Google Live conversation, OpenAI Realtime conversation, and the transcription sessions of both: a setup with no output modalities, verified live. Realtime translation, Lyria RealTime and GPT-Live follow in 0.4.x, behind the same session ABI (the release scope of NeoGraph#512).
- **Conformance.** One scripted scenario runs through the official SDKs (google-genai Live, the OpenAI realtime client) and the SDK against a recording WebSocket server. The outbound frames are canonicalized (sorted keys, redacted credentials, normalized ids and times) and diffed; the recorded server frames are replayed into typed events and per-turn completions. Live runs of both sides are archived.

## Evidence
- [live observation] Each realtime API answered a live session on 2026-10-18 (SchemaProvider#60), and Google Live and OpenAI Realtime ran over the shipped transport to `turnComplete` and `response.done` with clean closes.
- [read source] `WsSession` delivers whole messages by callback and has no read pause.

## Options considered
- Per-provider session types: rejected. A host would write every graph node twice.
- Push callbacks as the contract: rejected. Callbacks run on I/O threads, must not block, and give a host no place to apply back-pressure.
- One outbound queue with host pacing: rejected. A tool result would wait behind seconds of audio.
- SDK reconnects: rejected. A reconnect opens a billed session the host has not journaled.
- Allowed model sets in the facts: rejected (SchemaProvider#69).

## Consequences
- `realtime::Session`, its commands and events, and per-provider codecs join the SDK; the policy gains realtime endpoint facts.
- The live e2e report carries a Google Live and an OpenAI Realtime session, and the conformance diffs.

## Reconsideration conditions
A provider protocol whose turns cannot be delimited (no turn markers in either direction), or a host that needs the SDK to reconnect within one journaled receipt.

## Enforcement
- Tests:
  - canonical outbound frames equal the official SDKs' for the shared script, per provider;
  - recorded server frames decode to the expected events and per-turn completions, interruption included;
  - control preempts a full media ring, and a turn marker waits for the media before it (`control_preempts_full_ring`, `lanes_and_fences`);
  - a full ring never drops audio ahead of a pending turn marker;
  - a cancel ends the session as `Cancelled`, and a refused handshake as the kind the runtime error policy gives its status, with that status, not as a transport failure;
  - a setup outside the facts is refused at prepare with no I/O;
  - a failed lease renewal ends the session with its reason;
  - queued events other than audio are bounded, and a session's duration counts from `open`;
  - a close code the fact lists ends the session as the error its status classifies to, and one it does not list stays a clean close;
  - a turn's usage keeps the counts by modality, and the audio formats and their pacing come from the fact.
