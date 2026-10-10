# SchemaProvider outside NeoGraph

These examples use the installed C++ SDK directly. They do not link NeoGraph. Install the matching **0.3.0 alpha / interface 7** SDK using the [SDK build recipe](../README.md#install-and-use-the-unstable-c-sdk), then set `SDK_PREFIX` to that installation directory. A static SDK used inside the example shared library must be built with position-independent code (`-DCMAKE_POSITION_INDEPENDENT_CODE=ON` on ELF platforms).

Both consumers load a named key using [`cppdotenv::dotenv_values`](https://github.com/fox1245/cppdotenv) without modifying the process environment. Their CMake projects fetch pinned cppdotenv source if needed; an offline build can set `FETCHCONTENT_SOURCE_DIR_CPPDOTENV` to an existing checkout. No model download or local inference runtime is needed.

Run the commands below from the SchemaProvider repository root.

## Credential-free peer

The standard-library peer implements the small buffered Chat tool loop used by both examples. It is a protocol fixture, not an LLM. Use only synthetic credentials with it; do not send a real API key to a local fixture.

```sh
printf 'EXAMPLE_API_KEY=LOCAL_EXAMPLE_SYNTHETIC_KEY\n' > examples/local.env
PEER_EXPECTED_API_KEY=LOCAL_EXAMPLE_SYNTHETIC_KEY \
  python3 examples/python-ctypes/fake_chat_peer.py 8765
```

Leave that terminal running. If the port is occupied, choose another and change the consumers' base URL. The peer validates the synthetic Authorization header when `PEER_EXPECTED_API_KEY` is set. Its default 300 ms response delay makes overlapping requests observable; `GET /stats` reports the request count and maximum requests in flight.

## C++ standalone tool loop

```sh
cmake -S examples/cpp-standalone -B examples/cpp-standalone/build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build examples/cpp-standalone/build
examples/cpp-standalone/build/sp_chat_standalone \
  http://127.0.0.1:8765 example-model examples/local.env EXAMPLE_API_KEY
```

On a multi-configuration generator, build with `--config Release` and use the executable under `build/Release`.

The program creates `sp::runtime::Client`, declares `add(a,b)`, executes the requested tool locally, and sends its result back. Assistant history remains in the SDK's typed representation, including sealed native state. The expected final answer is `The sum is 5.`

CLI: `sp_chat_standalone <base-url> <model> <dotenv-path> <key-name> [question]`. It uses `/v1/chat/completions`, a 64-token output cap, a 10-second deadline per request, no automatic retries, and at most four tool turns. Missing credentials, failed requests, invalid tool arguments and incomplete final answers return a nonzero exit status.

For OpenAI, use `https://api.openai.com`, a tool-capable model, your private dotenv path and `OPENAI_API_KEY`. A real run incurs provider charges. No real-provider acceptance is established by the local fixture.

## Shared library → ctypes → LangGraph

```sh
cmake -S examples/python-ctypes -B examples/python-ctypes/build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build examples/python-ctypes/build
python3 -m venv examples/python-ctypes/.venv
examples/python-ctypes/.venv/bin/python -m pip install 'langgraph==1.2.14'
examples/python-ctypes/.venv/bin/python examples/python-ctypes/langgraph_agent.py \
  --base-url http://127.0.0.1:8765 --dotenv examples/local.env --key-name EXAMPLE_API_KEY
```

The library is `libsp_capi.so` on Linux, `libsp_capi.dylib` on macOS, or `sp_capi.dll` on Windows. The wrapper checks `SP_CAPI_LIBRARY` first, otherwise the example's `build/` directory. For Windows/MSVC, build with `--config Release`, set `SP_CAPI_LIBRARY` to the DLL in `build/Release`, and use `.venv\Scripts\python.exe`. Python, the shim and all native dependencies must have matching architecture; shared SDK/libcurl dependencies must be discoverable by the platform loader.

The graph executes `llm → tools → llm`: C++ performs the request and retains typed history; Python runs `add` and controls the graph. It prints the final answer plus four independent parallel graph answers. This script makes **ten requests** in the ordinary one-tool-per-chat case; use the local peer unless you intend those paid calls. `--descriptor` accepts a JSON descriptor for another Chat endpoint, with `--model`, `--dotenv` and `--key-name` selecting your deployment.

### Boundary and lifetime rules

- This is an **example-only C ABI**, not a supported SchemaProvider C API or independent Python package. It exposes text and client-executed tools for the `openai.chat` family only. Other families and typed media are SDK features, not exposed by this small wrapper.
- C++ objects, exceptions and allocators do not cross the boundary. Handles are opaque; turns and answers are UTF-8 JSON. Strings returned by the library must be freed with `sp_capi_string_free`; the ctypes wrapper does so even if UTF-8 decoding fails.
- Use `with Client(...)` and `with client.conversation(...)`, or call `close()` explicitly. Do not depend on garbage collection for native teardown. A conversation retains its SDK client after the parent handle closes; creating a new conversation from a closed client is an error.
- `ctypes.CDLL` releases the GIL during native calls. Independent conversations can share a client and overlap; one conversation serializes its sends. The Python wrapper serializes `close()` against creation/send. Raw C callers must never destroy a handle concurrently with its use.
- The shim calls the SDK's existing blocking `complete()` convenience layer; it does not add another execution engine. It is not an asyncio-native awaitable or a streaming-event bridge: `streaming=True` changes the provider wire, but this wrapper still returns one final JSON outcome. Async callers can offload it to a worker thread; cancelling that Python wait does not cancel the native request.
- Failed sends leave conversation history unchanged. Attempt evidence distinguishes whether a request may already have reached the provider; missing evidence is not permission to retry. The example does not retry a failed send automatically at the graph level.
- Reading dotenv inside C++ keeps the key out of Python variables and process-environment exports, **not out of the process**. ctypes loads the library into Python's address space. This is neither a security sandbox nor a secure-erasure guarantee; do not print or log credentials.

### Exercised scope

The examples were built and run on Linux x86_64/WSL with Python 3.12 and LangGraph 1.2.14. The local graph produced `The sum is 5.`, and four two-request chats overlapped (peer observed four requests in flight). Credential loading was checked against an accepting/rejecting synthetic header oracle. Parent-close lifetime, closed handles, repeated close and deadline-history rollback were exercised. The parent-close use-after-free was reproduced and corrected under ASan+UBSan.

Windows and macOS shared-library build/run are **not yet verified for these examples**; platform filenames and build instructions are recipes, not execution evidence. Local runs do not establish vendor media acceptance or a speedup over other clients.

## Other C++ agent frameworks

[`RunEdgeAI/agents.cpp`](https://github.com/RunEdgeAI/agents.cpp) documents hosted-provider integrations, tools and workflow patterns. [`mozilla-ai/agent.cpp`](https://github.com/mozilla-ai/agent.cpp) documents local agents tightly coupled to llama.cpp and explicitly says it is not intended for external LLM APIs. These are primary-source discoveries, not built SchemaProvider integrations or performance comparisons. SchemaProvider is a protocol client; it does not require an agent framework at all.
