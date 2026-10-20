#pragma once
// The reference host for a JavaScript environment (Emscripten, SP_BUILD_JS_HOST=ON, SP_THREADS=OFF): a browser, a
// Worker, Node 18+ or Deno. It implements the host seam of runtime/host.h over what every such environment has:
//  * an Executor over queueMicrotask and setTimeout;
//  * an HTTP transport over globalThis.fetch, ReadableStream and AbortController.
// Nothing here suspends the WebAssembly stack: no Asyncify, no JSPI, and not <emscripten/fetch.h>, which sits on
// XMLHttpRequest and does not exist in a Worker or in Node. JavaScript calls into the module from its own event loop,
// and the module returns to it; a blocking call therefore cannot work here, and the Client refuses it
// (`Executor::host_pumped`).
//
// Bytes cross the boundary as the response arrives, cut wherever the network cut them. SSE and UTF-8 framing is the
// runtime's (transport::SseFramer), not JavaScript's.
#include "runtime/host.h"

#include <memory>

namespace sp::runtime::js {

// A host-pumped executor: `post` runs a task from a microtask (in order, a bounded batch at a time, then from a
// timer so that input can arrive), `schedule` is a setTimeout, and `in_thread` is true while a task runs. It is not
// thread-safe and has no threads: it belongs to the one JavaScript thread that created it.
std::shared_ptr<Executor> make_executor();

// An HTTP transport over fetch. `executor` is where every callback of the transport is delivered, so pass the one
// the Client uses; no callback runs before `start` returns or from inside another.
//  * The request goes out with `redirect: "manual"` and no other optional field (a Worker rejects some), so a redirect
//    is returned to the runtime as the 3xx answer it is. An environment that hides it (a browser reports an opaque
//    redirect with status 0) fails the attempt with FailureKind::Protocol.
//  * Request headers go through `Headers`, so the environment's own rules apply (a browser drops the headers it
//    forbids); a request it rejects as malformed fails the attempt with FailureKind::Other before anything is sent.
//  * `first_byte_timeout` and `idle_timeout` are honored (an idle bound does not run while the runtime holds the
//    stream paused); `connect_timeout` cannot be told apart in fetch and is ignored; `deadline` is the client's.
//  * A request with a `file_body` fails with FailureKind::UploadSource: the host has no files.
//  * A cancelled attempt aborts the fetch and ends with Status::Cancelled.
// Reads `globalThis.fetch` at each request.
std::shared_ptr<HostTransport> make_fetch_transport(std::shared_ptr<Executor> executor);

}  // namespace sp::runtime::js
