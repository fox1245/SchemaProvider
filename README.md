# SchemaProvider

SchemaProvider is a C++20 client for five LLM API families: Chat Completions, Responses, Messages, Gemini generate and Interactions. A **family** is a wire protocol, not a model or endpoint operator. Each family has a typed request encoder and response codec; the runtime supplies HTTP/SSE transport, deadlines, cancellation and owned results.

The current package is **0.1.0 alpha**, with interface revision **4** and shared-library generation **4**. These are separate version domains: neither the linked-interface check nor shared-library version promises source or cross-toolchain ABI compatibility. [Current Linux execution evidence](docs/CONFORMANCE.md#interface-4-execution-record) is separate from [earlier interface 3 cohorts](docs/POC_PLAN.md#current-typed-c-cutover-provenance); neither establishes every platform/vendor/model combination as qualified.

## Install and use the unstable C++ SDK

Build prerequisites are CMake 3.20+, a C++20 compiler and standard library implementing `std::stop_token`, Python 3 for configuration generation, standalone Asio headers, libcurl 7.88+, OpenSSL Crypto and yyjson. Supply an installed yyjson library or its source tree; configuration does not download dependencies.

```sh
cmake -S . -B build-sdk -DSP_BUILD_TESTS=OFF -DSP_BUILD_CANARY=OFF \
  -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="$PWD/install-sdk" \
  -DYYJSON_ROOT="$YYJSON_ROOT"
cmake --build build-sdk
cmake --install build-sdk
```

Use a separate build directory and `-DBUILD_SHARED_LIBS=ON` for shared libraries. Set `CMAKE_PREFIX_PATH` to the installation prefix in your consumer project:

```cmake
cmake_minimum_required(VERSION 3.20)
project(first_request LANGUAGES CXX)
find_package(SchemaProvider 0.1.0 EXACT CONFIG REQUIRED)
add_executable(first_request main.cpp)
target_link_libraries(first_request PRIVATE SchemaProvider::runtime)
target_compile_features(first_request PRIVATE cxx_std_20)
```

For a reproducible deployment, pin the exact package version you built. The installed targets are `SchemaProvider::{core,json,descriptor,codecs,transport,runtime}`. Most applications need only `SchemaProvider::runtime`, which brings its dependencies. Public headers use names such as `<runtime/client.h>` and `<core/value.h>`; the target exports the `include/SchemaProvider` include root.

## First request

This complete program talks to a local Chat Completions peer and prints its text. It needs no model, credentials or paid API. Save it as `main.cpp` and run the [local peer recipe](docs/USAGE.md#run-the-first-request-without-a-hosted-api) before starting the executable.

```cpp
#include <runtime/client.h>
#include <chrono>
#include <iostream>
#include <utility>
#include <variant>

int main() {
    auto loaded = sp::descriptor::load(R"json({
      "descriptor_version": 1, "revision": 1,
      "id": "local-chat", "family": "openai.chat",
      "connection": {
        "base_url": "http://127.0.0.1:8765",
        "paths": {"buffered": "/v1/chat/completions",
                  "streaming": "/v1/chat/completions"}
      }
    })json");
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded)) {
        std::cerr << error->pointer << ": " << error->expected << '\n';
        return 1;
    }

    try {
        sp::runtime::require_interface_contract(
            sp::EXPECTED_INTERFACE_REVISION, sp::capability::RequiredProvider);
        sp::runtime::Client client(
            std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded)));

        sp::chat::Request request;
        request.model = "example-model";
        request.messages.push_back({sp::Role::User, "Say hello."});
        request.max_output_tokens = 64;
        sp::runtime::RunOptions run;
        run.streaming = false;
        run.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto result = client.complete(std::move(request), run);

        if (const auto* failure = std::get_if<sp::Failure>(result.get())) {
            std::cerr << failure->error.safe_message << '\n';
            return 2;
        }
        const auto& completion = std::get<sp::Completion>(*result);
        for (const auto& message : completion.messages) {
            for (const auto& part : message.parts) {
                if (const auto* text = std::get_if<sp::Text>(&part))
                    std::cout << text->value << '\n';
            }
        }
        if (completion.usage.output_total)
            std::cout << "output tokens: " << completion.usage.output_total->value << '\n';
        else
            std::cout << "output tokens: unknown\n";
    } catch (const sp::descriptor::ConfigError& error) {
        std::cerr << error.pointer << ": " << error.expected << '\n';
        return 3;
    } catch (const sp::runtime::InterfaceContractError& error) {
        std::cerr << error.what() << '\n';
        return 4;
    }
}
```

With the linked local peer running, expected output is:

```text
Hello.
output tokens: 0
```

The descriptor selects an admitted family, origin and literal routes. The typed request supplies model, conversation and controls. `complete()` returns `std::shared_ptr<const sp::Outcome>`, containing either `Completion` or `Failure`; it does not turn an HTTP error or a truncated stream into successful text. Retain that pointer when results must outlive the client. A missing usage count means **unknown**, while a present count whose value is zero means **known zero**.

For a hosted endpoint, load your own HTTPS descriptor and pass credentials through `sp::runtime::Options::api_key`. Do not put credentials in descriptor JSON or print encoded request bodies. [Configuration and credential rules](docs/USAGE.md#descriptors-credentials-and-defaults) explain validation and defaults.

## Owned requests, results and configuration

The typed SDK replaces descriptor interpretation with family code and closed immutable configuration. Encoders own message ordering, tool validation and native replay checks; codecs own stream causality, usage meaning and terminal evidence. Interface 4 adds declared reasoning/tool/sampling controls, Responses provider-held cursors and an explicit Gemini portable-history path without restoring the interpreter. JSON supplies admitted routes, bindings, stop mappings, defaults and model/error facts; it cannot add loops, callbacks, templates or event actions.

Use `Client::prepare()` when your host must validate and inspect a request before persisting a dispatch receipt or claiming a budget. Preparation encodes once, reserves a bounded slot and keeps the original deadline, but sends nothing. Move the resulting `PreparedRequest` into `start()` or `complete()` once. Dropping it releases the slot. The direct `complete(Request, RunOptions)` and `start(Request, RunOptions, Callbacks)` paths prepare internally.

[The usage guide](docs/USAGE.md) covers family-specific controls, deployment headers, asynchronous callbacks, backpressure, failures, nullable usage, raw observations, native continuation and the distinct Responses cursor/Gemini portable-history paths. [The migration guide](docs/MIGRATION.md) maps removed NeoGraph APIs to the typed boundary and explains the interface 3→4 source cutover.

## Relationship to NeoGraph

SchemaProvider can run without NeoGraph. The SDK owns protocol requests, transport, codecs and outcomes. NeoGraph's `neograph::llm::SchemaProvider` adapter owns the coroutine/cancellation bridge and host delivery policy. Graphs, tool execution, budgets, journals and run identity remain NeoGraph responsibilities. The adapter's `Provider::prepare` plus `dispatch`/`invoke` is distinct from the SDK's `Client::prepare` plus `start`/`complete`.

The old NeoGraph `Provider::complete`, `OpenAIProvider`, `RateLimitedProvider` and descriptor interpreter are removed, without compatibility shims. Standalone image endpoints, Veo operations and OpenRouter Decisions remain separate typed NeoGraph clients; this SDK covers artifacts received within supported chat protocols, not those endpoints. Python consumers use NeoGraph's bindings; this SDK does not ship a separate Python package. Native Bedrock/Vertex authentication and Responses WebSocket are outside the current SDK.

## Documentation and evidence

- [Usage](docs/USAGE.md): runnable local first use, configuration, request controls and result handling.
- [Migration](docs/MIGRATION.md): interface 4, preserved controls and the NeoGraph adapter cutover.
- [Design](docs/DESIGN.md): current architecture, with labeled historical proposals.
- [Conformance](docs/CONFORMANCE.md): current interface 4 execution record, behavioral properties and remaining gates.
- [PoC and historical evidence](docs/POC_PLAN.md): separate cohort-scoped results, including the old-ABI lossless NO-GO and interface 3 typed-cutover provenance.
- [Roadmap](docs/ROADMAP.md): implemented work versus unclaimed release/platform/vendor gates.
- [Research](docs/RESEARCH.md) and [decisions](docs/decisions/): source-linked rationale; external papers are not proof of SDK behavior.
- [SDK benchmark records](benchmarks/current-sdk-benchmark-results.json) and [summary](benchmarks/current-sdk-summary.json): model-free protocol/ownership measurements, not model inference speed.
- [Qualification summary](config/qualification-final-summary.json): the paid baseline achieved 293/300 pairs, not an all-family pass. Google carrier acceptance does not prove signature validation or native consumption. That campaign has no remaining authorized calls.

## License

MIT. See [LICENSE](LICENSE).
