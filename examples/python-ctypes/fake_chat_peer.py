"""Credential-free Chat Completions peer for the examples. Loopback only, standard library only.

It inspects each request and answers deterministically: when the conversation has no tool result
yet it asks for the `add` tool with the two integers found in the user text, otherwise it answers
with the sum it finds in the tool message. Every answer sleeps `PEER_DELAY_MS` so concurrency is
observable (GET /stats reports the largest number of requests in flight). Do not supply real
credentials.
"""
import json
import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DELAY_MS = int(os.environ.get("PEER_DELAY_MS", "300"))
EXPECTED_KEY = os.environ.get("PEER_EXPECTED_API_KEY")
stats = {"requests": 0, "max_inflight": 0}
lock = threading.Lock()
inflight = 0


def text_of(content):
    """Chat content is a string or an array of typed parts; the SDK's canonical path sends parts."""
    if isinstance(content, list):
        return " ".join(part.get("text", "") for part in content if isinstance(part, dict))
    return content or ""

class Peer(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def _reply(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/stats":
            return self._reply(200, stats)
        self.send_error(404)

    def do_POST(self):
        global inflight
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if EXPECTED_KEY is not None and self.headers.get("Authorization") != "Bearer " + EXPECTED_KEY:
            return self._reply(401, {"error": {"code": "invalid_api_key", "message": "synthetic credential mismatch"}})
        if self.path != "/v1/chat/completions" or request.get("stream") is not False:
            return self._reply(400, {"error": {"code": "invalid_request_error", "message": "unexpected request"}})
        with lock:
            stats["requests"] += 1
            inflight += 1
            stats["max_inflight"] = max(stats["max_inflight"], inflight)
        try:
            time.sleep(DELAY_MS / 1000)
            tool_messages = [m for m in request["messages"] if m.get("role") == "tool"]
            if not tool_messages:
                user_text = " ".join(text_of(m.get("content")) for m in request["messages"] if m.get("role") == "user")
                numbers = [int(t) for t in re.findall(r"\d+", user_text)]
                message = {"role": "assistant", "content": None, "tool_calls": [{
                    "id": "call_add_1", "type": "function",
                    "function": {"name": "add", "arguments": json.dumps({"a": numbers[0], "b": numbers[1]})}}]}
                finish = "tool_calls"
            else:
                message = {"role": "assistant", "content": "The sum is %s." % text_of(tool_messages[-1]["content"])}
                finish = "stop"
            self._reply(200, {"id": "peer-1", "object": "chat.completion", "created": 1, "model": request["model"],
                              "choices": [{"index": 0, "message": message, "finish_reason": finish}],
                              "usage": {"prompt_tokens": 11, "completion_tokens": 7, "total_tokens": 18}})
        finally:
            with lock:
                inflight -= 1


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    with ThreadingHTTPServer(("127.0.0.1", port), Peer) as server:
        print("ready on 127.0.0.1:%d" % port, flush=True)
        server.serve_forever()
