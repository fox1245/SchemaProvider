"""Shared measurement loop for the Python contenders.

A client module supplies four things and calls `main()`:

    setup(args)            build the SDK client once (called before any request)
    call(ctx, args)        one request; an async function (asyncio model) or a plain function (thread model).
                           Buffered: returns the SDK's response object. Streamed: consumes every chunk and returns
                           the list of non-empty text deltas it saw.
    extract(raw)           buffered only: the response text, read after the request's clock has stopped
    describe(args)         dict of the settings the client actually used, recorded with the result

The line protocol is the one in README.md: a `ready` line after each warm-up round, `MORE` (another round) or `GO` on
stdin, then one `result` line.
Verification happens after the clock has stopped, outside the measured window.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import resource
import sys
import threading
import time


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--api", required=True, choices=["openai", "anthropic"])
    ap.add_argument("--mode", required=True, choices=["buffered", "stream"])
    ap.add_argument("--concurrency", type=int, default=1)
    ap.add_argument("--warmup", type=int, default=0)
    ap.add_argument("--requests", type=int, default=1)
    ap.add_argument("--base-prefix", required=True)
    ap.add_argument("--ca", required=True)
    ap.add_argument("--model", default="bench-model")
    ap.add_argument("--system", required=True)
    ap.add_argument("--user", required=True)
    ap.add_argument("--max-tokens", type=int, required=True)
    ap.add_argument("--expect", required=True)
    ap.add_argument("--chunks", type=int, default=1)
    ap.add_argument("--api-key", required=True)
    ap.add_argument("--cold", action="store_true")
    return ap.parse_args()


def status_kb(field: str) -> int:
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith(field):
                return int(line.split()[1])
    return -1


def emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def verify(args, payload) -> bool:
    if args.mode == "stream":
        return isinstance(payload, list) and (args.chunks == 0 or len(payload) == args.chunks) and "".join(payload) == args.expect
    return payload == args.expect


class Run:
    def __init__(self, args, count: int, record: bool):
        self.args, self.count, self.record = args, count, record
        self.latency = [0] * count if record else None
        self.payload = [None] * count if record else None
        self.failed, self.first_error = 0, None
        self.next = 0
        self.lock = threading.Lock()

    def claim(self) -> int:
        with self.lock:
            i = self.next
            self.next += 1
            return i

    def fail(self, exc: BaseException) -> None:
        with self.lock:
            self.failed += 1
            if self.first_error is None:
                self.first_error = f"{type(exc).__name__}: {exc}"[:300]


async def drive_async(ctx, call, extract, run: Run) -> None:
    ns = time.perf_counter_ns

    async def worker():
        while True:
            i = run.claim()
            if i >= run.count:
                return
            t0 = ns()
            try:
                raw = await call(ctx, run.args)
            except Exception as exc:  # a failed request is counted, never retried by the harness
                run.fail(exc)
                continue
            t1 = ns()
            if run.record:
                run.latency[i] = t1 - t0
                run.payload[i] = raw if run.args.mode == "stream" else extract(raw)

    await asyncio.gather(*(worker() for _ in range(min(run.args.concurrency, run.count))))


def drive_threads(ctx, call, extract, run: Run) -> None:
    ns = time.perf_counter_ns

    def worker():
        while True:
            i = run.claim()
            if i >= run.count:
                return
            t0 = ns()
            try:
                raw = call(ctx, run.args)
            except Exception as exc:
                run.fail(exc)
                continue
            t1 = ns()
            if run.record:
                run.latency[i] = t1 - t0
                run.payload[i] = raw if run.args.mode == "stream" else extract(raw)

    threads = [threading.Thread(target=worker) for _ in range(min(run.args.concurrency, run.count))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


def main(client: str, version: str, setup, call, extract, describe, model: str = "async") -> None:
    args = parse_args()
    ctx = setup(args)
    if args.cold:
        args.concurrency = 1
    if model == "async":
        # One event loop for the whole process: clients keep loop-bound connections alive between phases.
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)

        def runner(run):
            loop.run_until_complete(drive_async(ctx, call, extract, run))
    else:
        def runner(run):
            drive_threads(ctx, call, extract, run)

    if args.cold:
        run = Run(args, 1, True)
        runner(run)
        ok = run.failed == 0 and verify(args, run.payload[0])
        emit({"event": "first", "ok": ok})
        os._exit(0 if ok else 1)

    warm_failed, warm_error = 0, None
    while True:
        warm = Run(args, args.warmup, False)
        if args.warmup:
            runner(warm)
        warm_failed += warm.failed
        warm_error = warm_error or warm.first_error
        rss_ready, hwm_ready = status_kb("VmRSS:"), status_kb("VmHWM:")
        emit({"event": "ready"})
        reply = sys.stdin.readline()
        if reply.startswith("GO"):
            break
        if not reply.startswith("MORE"):
            os._exit(3)

    run = Run(args, args.requests, True)
    r0 = resource.getrusage(resource.RUSAGE_SELF)
    t0 = time.perf_counter_ns()
    runner(run)
    t1 = time.perf_counter_ns()
    r1 = resource.getrusage(resource.RUSAGE_SELF)

    # Everything below is outside the measured window.
    bad = sum(1 for p in run.payload if p is not None and not verify(args, p))
    ok = args.requests - run.failed
    first_error = run.first_error or warm_error
    emit({
        "event": "result", "schema": 1, "client": client, "version": version, "api": args.api, "mode": args.mode,
        "concurrency": args.concurrency, "warmup": args.warmup, "requests": args.requests, "ok": ok,
        "failed": run.failed + warm_failed, "first_error": first_error, "verify_failed": bad,
        "wall_ns": t1 - t0,
        "cpu_user_us": int((r1.ru_utime - r0.ru_utime) * 1e6), "cpu_sys_us": int((r1.ru_stime - r0.ru_stime) * 1e6),
        "rss_ready_kb": rss_ready, "hwm_ready_kb": hwm_ready, "rss_end_kb": status_kb("VmRSS:"), "hwm_end_kb": status_kb("VmHWM:"),
        "config": describe(args), "latency_ns": run.latency,
    })
    sys.stdout.flush()
    os._exit(0)


def sdk_limits(async_client_cls, n: int):
    """The connection limits object of the HTTP library an SDK's default async client is built on, sized `n`."""
    import importlib

    base = next(c for c in async_client_cls.__mro__ if c.__name__ == "AsyncClient" and c.__module__.split(".")[0] in ("httpx", "httpx2"))
    module = importlib.import_module(base.__module__.split(".")[0])
    return module.Limits(max_connections=n, max_keepalive_connections=n), module.__name__ + " " + module.__version__


def dist_version(name: str) -> str:
    from importlib.metadata import version

    return version(name)
