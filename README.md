# SchemaProvider

A multi-vendor LLM client library for C++20.

> **Status: early design stage. There is no usable code here yet.**
> This repository is being set up as the future home of `SchemaProvider`, which currently lives inside [NeoGraph](https://github.com/fox1245/NeoGraph). The direction is still under discussion in [fox1245/NeoGraph#321](https://github.com/fox1245/NeoGraph/issues/321); nothing below is a promise.

## What it is meant to be
One `Provider` interface over the API families the major vendors expose (Messages, Responses, Chat Completions, Gemini/Interactions), usable on its own and as the LLM layer of NeoGraph.

## Goals under consideration
- Typed errors carrying status, retryability, vendor code and request id, so a provider failure is never returned as a successful completion.
- Token usage including cached and reasoning tokens, with each vendor's counting rules kept as data.
- Per-call request knobs declared per vendor, with a clear error for a key that is not declared.
- Structure in typed code per API family; the values that change often (endpoints, headers, knobs, rules, retryable codes, model lists) in JSON.
- A conformance suite built from recorded real vendor request/response pairs and streams, so behaviour learned from the live APIs is a spec that both the current and the new implementation must pass.
- A portable conversation history plus an opaque, per-origin sidecar for vendor reasoning state (signed or encrypted reasoning cannot be translated between vendors).

## Not goals for now
- Python bindings (deferred until the C++ API settles).
- A graph or agent runtime: that stays in NeoGraph.

## Relationship to NeoGraph
NeoGraph's engine already depends only on the `Provider` interface, not on `SchemaProvider`. Splitting the two is mostly making that boundary real. The staging (contract cleanup inside NeoGraph, then the conformance suite, then the new implementation, then the switch) is written up in [NeoGraph#321](https://github.com/fox1245/NeoGraph/issues/321).

## License
MIT. See [LICENSE](LICENSE).
