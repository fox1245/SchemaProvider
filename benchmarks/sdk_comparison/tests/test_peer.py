#!/usr/bin/env python3
"""Checks of the SDK-comparison options of tools/benchmark_peer.mjs, with the standard library only.

  python3 test_peer.py            (needs node and openssl on PATH)

The peer validates every request; these tests show that a request that differs from the workload is rejected and that a
request that matches it is answered with the expected bytes.
"""
import http.client
import json
import os
import shutil
import ssl
import subprocess
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PEER = ROOT / "tools" / "benchmark_peer.mjs"
KEY, SYSTEM = "test-key", "You are a benchmark assistant."
USER = "benchmark-request:" + "x" * 128
EXPECT = "benchmark-ok:" + "x" * 1200


def base_config(**extra):
    cfg = {"family": "openai.chat", "case": "text", "payload_bytes": 128, "response_bytes": 1200, "server_delay_ms": 0,
           "output_tokens": 256, "stream_chunks": 50, "sse_flush": "event", "alpn": "h1", "api_key": KEY, "system_text": SYSTEM}
    cfg.update(extra)
    return cfg


class Peer:
    def __init__(self, cfg, tmp):
        path = Path(tmp) / "cfg.json"
        path.write_text(json.dumps(cfg))
        self.proc = subprocess.Popen(["node", str(PEER), "--config", str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        ready = json.loads(self.proc.stdout.readline())
        self.port, self.ca = ready["port"], ready["ca_file"]
        self.context = ssl.create_default_context(cafile=self.ca)

    def stats(self):
        self.proc.stdin.write('{"stats":true}\n')
        self.proc.stdin.flush()
        return json.loads(self.proc.stdout.readline())

    def post(self, body, headers=None, path="/openai/chat/text/v1/chat/completions", conn=None, keep=False):
        own = conn is None
        conn = conn or http.client.HTTPSConnection("127.0.0.1", self.port, context=self.context)
        h = {"authorization": f"Bearer {KEY}", "content-type": "application/json"}
        h.update(headers or {})
        conn.request("POST", path, json.dumps(body), h)
        r = conn.getresponse()
        data = r.read()
        if own and not keep:
            conn.close()
        return r.status, data, conn

    def close(self):
        try:
            self.proc.stdin.write('{"shutdown":true}\n')
            self.proc.stdin.flush()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
        self.proc.stdin.close()
        self.proc.stdout.close()


def chat_body(stream, **over):
    body = {"model": "bench-model", "stream": stream, "max_tokens": 256,
            "messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": USER}]}
    body.update(over)
    return body


def messages_body(stream, **over):
    body = {"model": "bench-model", "stream": stream, "max_tokens": 256, "system": SYSTEM,
            "messages": [{"role": "user", "content": USER}]}
    body.update(over)
    return body


@unittest.skipUnless(shutil.which("node") and shutil.which("openssl"), "needs node and openssl")
class PeerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = os.environ.get("TMPDIR", "/tmp")

    def test_buffered_reply_carries_the_text(self):
        peer = Peer(base_config(expect_streaming=False), self.tmp)
        try:
            status, data, _ = peer.post(chat_body(False))
            self.assertEqual(status, 200)
            self.assertEqual(json.loads(data)["choices"][0]["message"]["content"], EXPECT)
        finally:
            peer.close()

    def test_stream_has_the_configured_number_of_deltas_and_the_same_text(self):
        for family, path, body, extract in (
            ("openai.chat", "/openai/chat/text/v1/chat/completions", chat_body(True), lambda e: e["choices"][0]["delta"].get("content", "")),
            ("anthropic.messages", "/anthropic/messages/text/v1/messages", messages_body(True),
             lambda e: e["delta"]["text"] if e.get("type") == "content_block_delta" else ""),
        ):
            peer = Peer(base_config(family=family, expect_streaming=True), self.tmp)
            try:
                status, data, _ = peer.post(body, {"x-api-key": KEY} if family.startswith("anthropic") else None, path)
                self.assertEqual(status, 200)
                events = [json.loads(l[6:]) for l in data.decode().splitlines() if l.startswith("data: ") and l != "data: [DONE]"]
                pieces = [extract(e) for e in events if extract(e)]
                self.assertEqual(len(pieces), 50, family)
                self.assertEqual("".join(pieces), EXPECT, family)
            finally:
                peer.close()

    def test_requests_that_differ_from_the_workload_are_rejected(self):
        peer = Peer(base_config(expect_streaming=False), self.tmp)
        try:
            cases = {
                "wrong key": (chat_body(False), {"authorization": "Bearer other"}),
                "stream flag": (chat_body(True), None),
                "model": (chat_body(False, model="other"), None),
                "token cap": (chat_body(False, max_tokens=7), None),
                "no system prompt": (chat_body(False, messages=[{"role": "user", "content": USER}]), None),
                "prompt": (chat_body(False, messages=[{"role": "system", "content": SYSTEM}, {"role": "user", "content": "hi"}]), None),
            }
            for name, (body, headers) in cases.items():
                status, _, _ = peer.post(body, headers)
                self.assertEqual(status, 400, name)
            self.assertEqual(peer.stats()["invalid"], len(cases))
        finally:
            peer.close()

    def test_h1_only_peer_never_negotiates_h2(self):
        peer = Peer(base_config(expect_streaming=False), self.tmp)
        try:
            ctx = ssl.create_default_context(cafile=peer.ca)
            ctx.set_alpn_protocols(["h2", "http/1.1"])
            conn = http.client.HTTPSConnection("127.0.0.1", peer.port, context=ctx)
            peer.post(chat_body(False), conn=conn)
            self.assertEqual(conn.sock.selected_alpn_protocol(), "http/1.1")
            conn.close()
            self.assertEqual(peer.stats()["h2"], 0)
        finally:
            peer.close()

    def test_connection_survives_an_idle_gap_and_is_counted_once(self):
        peer = Peer(base_config(expect_streaming=False), self.tmp)
        try:
            _, _, conn = peer.post(chat_body(False), keep=True)
            time.sleep(6.5)  # longer than node's default 5 s keep-alive timeout
            status, _, _ = peer.post(chat_body(False), conn=conn)
            conn.close()
            self.assertEqual(status, 200)
            self.assertEqual(peer.stats()["connections"], 1)
        finally:
            peer.close()

    def test_warm_barrier_holds_replies_until_n_requests_are_in_flight(self):
        peer = Peer(base_config(expect_streaming=False, warm_barrier=3), self.tmp)
        try:
            done = []

            def request():
                peer.post(chat_body(False))
                done.append(time.monotonic())

            threads = [threading.Thread(target=request) for _ in range(2)]
            for t in threads:
                t.start()
            time.sleep(0.8)
            self.assertEqual(done, [], "two requests must still be held")
            third = threading.Thread(target=request)
            third.start()
            for t in threads + [third]:
                t.join(10)
            self.assertEqual(len(done), 3)
            self.assertEqual(peer.stats()["connections"], 3)
            self.assertEqual(peer.stats()["barrier_timeouts"], 0)
        finally:
            peer.close()

    def test_several_worker_processes_share_one_port_one_barrier_and_one_set_of_statistics(self):
        peer = Peer(base_config(expect_streaming=False, workers=2, warm_barrier=4), self.tmp)
        try:
            done = []

            def request():
                status, _, _ = peer.post(chat_body(False))
                done.append(status)

            threads = [threading.Thread(target=request) for _ in range(3)]
            for t in threads:
                t.start()
            time.sleep(0.8)
            self.assertEqual(done, [], "three requests must be held until the fourth arrives")
            last = threading.Thread(target=request)
            last.start()
            for t in threads + [last]:
                t.join(10)
            self.assertEqual(done, [200] * 4)
            stats = peer.stats()
            self.assertEqual((stats["requests"], stats["connections"], stats["invalid"], stats["barrier_timeouts"]), (4, 4, 0, 0))
            self.assertGreater(stats["cpu_user_us"], 0)
        finally:
            peer.close()


if __name__ == "__main__":
    unittest.main()
