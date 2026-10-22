#!/usr/bin/env python3
"""Runner for the SDK comparison: starts the peer and one client process per cell and writes records.

Python 3.10+ standard library only. See README.md for the method and the line protocol.

  sdkbench.py run   --install DIR --out DIR [--contenders ...] [--apis ...] [--modes ...] [--concurrency ...] [--reps N]
  sdkbench.py cold  --install DIR --out DIR [--contenders ...] [--reps N]
  sdkbench.py sizes --install DIR --out DIR [--contenders ...]

Replay: `run --workload workload-replay.json --replay replay/recordings/DATE` answers every request with a recorded provider
response instead of the generated one (see replay/README.md).
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import platform
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

HARNESS = Path(__file__).resolve().parent
PEER = HARNESS.parent.parent / "tools" / "benchmark_peer.mjs"
SCHEMA = 1
MAX_WARM_ROUNDS = 8


# ----------------------------------------------------------------------------------------------------------------
# workload and manifest

def load_workload(path: Path) -> dict:
    w = json.loads(path.read_text())
    w["user"] = w["user_prefix"] + "x" * w["user_payload_bytes"]
    w["expect"] = w["response_prefix"] + "x" * w["response_payload_bytes"]
    return w


def load_replay(directory: Path, w: dict, apis: list[str], modes: list[str]) -> dict:
    """The recorded responses for every selected api and mode. They must all carry the text in reply.txt, which becomes the
    text every client has to reproduce; the number of stream deltas differs between recordings, so it is not checked."""
    text = (directory / "reply.txt").read_text()
    digest = hashlib.sha256(text.encode()).hexdigest()
    files = {}
    for api in apis:
        for mode in modes:
            path = directory / f"{api}-{mode}.json"
            meta = json.loads(path.read_text())
            if meta["api"] != api or meta["mode"] != mode or meta["text_sha256"] != digest or not meta["recordings"]:
                raise SystemExit(f"{path}: not a recording of {api}/{mode} with the text of reply.txt")
            files[(api, mode)] = {"path": path, "file": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                                  "model": meta["model"], "recorded_utc": meta["recorded_utc"], "recordings": len(meta["recordings"])}
    w["expect"] = text
    return {"directory": directory.name, "reply_sha256": digest, "files": files}


def load_contenders(install: Path, only: list[str] | None) -> list[dict]:
    manifest = json.loads((HARNESS / "contenders.json").read_text())
    out = []
    for c in manifest["contenders"]:
        if only and c["id"] not in only:
            continue
        subst = {"${HARNESS}": str(HARNESS), "${INSTALL}": str(install)}

        def sub(value):
            if isinstance(value, str):
                for k, v in subst.items():
                    value = value.replace(k, v)
            elif isinstance(value, list):
                value = [sub(v) for v in value]
            elif isinstance(value, dict):
                value = {k: sub(v) for k, v in value.items()}
            return value

        out.append(sub(c))
    return out


# ----------------------------------------------------------------------------------------------------------------
# process plumbing

class Lines:
    """Line reader with a timeout over a pipe file descriptor."""

    def __init__(self, fd: int):
        self.fd, self.buf, self.eof = fd, b"", False

    def read(self, timeout: float) -> str | None:
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buf:
            if self.eof:
                break
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("timed out waiting for a line")
            ready, _, _ = select.select([self.fd], [], [], left)
            if not ready:
                continue
            chunk = os.read(self.fd, 1 << 20)
            if not chunk:
                self.eof = True
                break
            self.buf += chunk
        if not self.buf:
            return None
        line, _, rest = self.buf.partition(b"\n")
        self.buf = rest
        return line.decode("utf-8", "replace")

    def read_event(self, timeout: float, want: set[str]) -> dict | None:
        """Next protocol line (a JSON object with an `event`) whose event is in `want`; other lines are returned as {'event':'log'}."""
        deadline = time.monotonic() + timeout
        while True:
            line = self.read(max(0.0, deadline - time.monotonic()))
            if line is None:
                return None
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if isinstance(obj, dict) and obj.get("event") in want:
                return obj


def taskset(cpus: str | None, argv: list[str]) -> list[str]:
    return ["taskset", "-c", cpus] + argv if cpus else argv


def spawn(argv, *, env, stdin=subprocess.PIPE, stderr_path: Path | None = None, cwd=None):
    err = open(stderr_path, "wb") if stderr_path else subprocess.DEVNULL
    proc = subprocess.Popen(argv, env=env, stdin=stdin, stdout=subprocess.PIPE, stderr=err, cwd=cwd, start_new_session=True)
    if stderr_path:
        err.close()
    return proc


def stop(proc: subprocess.Popen | None) -> None:
    """Stop a process this runner started (and its session)."""
    if proc is None or proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
        proc.wait(timeout=5)
    except Exception:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait(timeout=5)
        except Exception:
            pass


class Peer:
    def __init__(self, workload: dict, api: str, mode: str, *, alpn: str, node: str, cpus: str | None, workdir: Path, barrier: int = 0, workers: int = 1,
                 replay_file: Path | None = None):
        w = workload
        cfg = {
            "family": w["apis"][api]["family"], "case": "text", "payload_bytes": w["user_payload_bytes"],
            "response_bytes": w["response_payload_bytes"], "server_delay_ms": 0, "output_tokens": w["max_tokens"],
            "stream_chunks": w["stream_chunks"] if mode == "stream" else 1,
            "sse_flush": "event", "alpn": alpn, "expect_streaming": mode == "stream",
            "api_key": w["api_key"], "system_text": w["system"], "debug_rejections": True,
        }
        if barrier > 1:
            cfg["warm_barrier"] = barrier
        if workers > 1:
            cfg["workers"] = workers
        if replay_file:
            cfg["replay_file"] = str(replay_file)
        self.cfg_path = workdir / f"peer-{api}-{mode}.json"
        self.cfg_path.write_text(json.dumps(cfg))
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin")}
        self.proc = spawn(taskset(cpus, [node, str(PEER), "--config", str(self.cfg_path)]), env=env, stderr_path=workdir / "peer.stderr")
        self.lines = Lines(self.proc.stdout.fileno())
        line = self.lines.read(30)
        ready = json.loads(line)
        if not ready.get("ready"):
            raise RuntimeError("peer did not start")
        self.port, self.ca = ready["port"], ready["ca_file"]
        self.origin = f"https://127.0.0.1:{self.port}"

    def stats(self) -> dict:
        self.proc.stdin.write(b'{"stats":true}\n')
        self.proc.stdin.flush()
        return json.loads(self.lines.read(30))

    def close(self) -> None:
        try:
            self.proc.stdin.write(b'{"shutdown":true}\n')
            self.proc.stdin.flush()
            self.proc.wait(timeout=5)
        except Exception:
            pass
        stop(self.proc)


def scratch_home(session: Path) -> Path:
    """An empty home and working directory for clients: no dotfiles, no .env file, nothing to read credentials from."""
    home = session / "work" / "home"
    home.mkdir(parents=True, exist_ok=True)
    return home


def client_env(peer: Peer, workload: dict, extra: dict, home: Path) -> dict:
    """A clean environment: no inherited credentials, telemetry off, the peer's CA offered through every usual variable."""
    env = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(home), "LANG": "C.UTF-8",
        "SSL_CERT_FILE": peer.ca, "NODE_EXTRA_CA_CERTS": peer.ca, "REQUESTS_CA_BUNDLE": peer.ca, "CURL_CA_BUNDLE": peer.ca,
        "OPENAI_API_KEY": workload["api_key"], "ANTHROPIC_API_KEY": workload["api_key"],
        "LITELLM_LOG": "ERROR", "LITELLM_TELEMETRY": "False", "ANONYMIZED_TELEMETRY": "False", "DO_NOT_TRACK": "1",
        "NO_PROXY": "127.0.0.1,localhost", "no_proxy": "127.0.0.1,localhost", "PYTHONUNBUFFERED": "1",
        "PYTHONDONTWRITEBYTECODE": "1", "AI_SDK_LOG_WARNINGS": "false", "RUST_LOG": "off",
    }
    env.update(extra)
    return env


def client_argv(c: dict, w: dict, peer: Peer, api: str, mode: str, *, concurrency: int, warmup: int, requests: int, cold: bool) -> list[str]:
    argv = list(c["cmd"]) + [
        "--api", api, "--mode", mode, "--concurrency", str(concurrency), "--warmup", str(warmup), "--requests", str(requests),
        "--base-prefix", peer.origin + w["apis"][api]["path_prefix"], "--ca", peer.ca, "--model", w["model"],
        "--system", w["system"], "--user", w["user"], "--max-tokens", str(w["max_tokens"]), "--expect", w["expect"],
        "--chunks", str(0 if getattr(client_argv, "replay", None) else w["stream_chunks"] if mode == "stream" else 1), "--api-key", w["api_key"],
    ]
    argv += list(c.get("args", []))
    argv += list(getattr(client_argv, "extra", []))  # experiments only; recorded in session.json
    if cold:
        argv.append("--cold")
    return argv


# ----------------------------------------------------------------------------------------------------------------
# provenance

def sh(argv: list[str], cwd: Path | None = None) -> str:
    try:
        return subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=20).stdout.strip()
    except Exception:
        return ""


def first_line(text: str) -> str:
    return text.splitlines()[0] if text else ""


def provenance(args, install: Path, contenders: list[dict], session: Path) -> dict:
    repo = HARNESS.parent.parent
    cpuinfo = Path("/proc/cpuinfo").read_text()
    model = next((l.split(":", 1)[1].strip() for l in cpuinfo.splitlines() if l.startswith(("model name", "Model name"))), "")
    if not model:  # Arm /proc/cpuinfo has no model name
        model = next((l.split(":", 1)[1].strip() for l in sh(["lscpu"]).splitlines() if l.startswith("Model name")), "unknown")
    governor = ""
    gov = Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
    if gov.exists():
        governor = gov.read_text().strip()
    commit, dirty = sh(["git", "rev-parse", "HEAD"], repo), bool(sh(["git", "status", "--porcelain"], repo))
    if not commit and (HARNESS / ".revision").exists():  # a copy of the tree without .git carries the revision it was copied from
        recorded = json.loads((HARNESS / ".revision").read_text())
        commit, dirty = recorded["commit"], recorded["dirty"]
    installed = {}
    for c in contenders:
        p = Path(c.get("installed", "")) if c.get("installed") else None
        if p and p.exists():
            installed[c["id"]] = json.loads(p.read_text())
    scripts = {n: hashlib.sha256((HARNESS / n).read_bytes()).hexdigest() for n in ("sdkbench.py", "render.py", "workload.json", "contenders.json") if (HARNESS / n).exists()}
    scripts[args.workload_path.name] = hashlib.sha256(args.workload_path.read_bytes()).hexdigest()
    scripts["benchmark_peer.mjs"] = hashlib.sha256(PEER.read_bytes()).hexdigest()
    return {
        "schema": SCHEMA,
        "utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "harness_commit": commit or "unknown", "harness_dirty": dirty,
        "cpu_model": model, "cpu_count": os.cpu_count(), "cpu_governor": governor or "unavailable",
        "client_cpus": args.client_cpus, "peer_cpus": args.peer_cpus, "peer_workers": args.peer_workers, "extra_client_args": args.client_args,
        "kernel": platform.release(), "machine": platform.machine(),
        "tools": {
            "python": platform.python_version(), "node": sh([args.node, "--version"]),
            "gcc": first_line(sh(["gcc", "--version"])), "cmake": first_line(sh(["cmake", "--version"])),
            "rustc": first_line(sh([str(install / "rust" / "cargo" / "bin" / "rustc"), "--version"])) or first_line(sh(["rustc", "--version"])),
            "openssl": sh(["openssl", "version"]), "libcurl": first_line(sh(["curl-config", "--version"])),
        },
        "script_sha256": scripts, "installed": installed, "session": session.name, "workload_file": args.workload_path.name,
        "replay": None if not args.replay_info else {**args.replay_info, "files": {f"{a}/{m}": {k: v for k, v in f.items() if k != "path"}
                                                                                  for (a, m), f in args.replay_info["files"].items()}},
    }


# ----------------------------------------------------------------------------------------------------------------
# one measured cell

def cell_id(cid, api, mode, conc, rep):
    return f"{cid}.{api}.{mode}.c{conc}.r{rep}"


def counts_for(w: dict, conc: int, args) -> tuple[int, int]:
    requests = max(w["min_measured_requests"], w["measured_requests_per_slot"] * conc)
    warmup = max(w["min_warmup_requests"], w["warmup_requests_per_slot"] * conc)
    if args.requests:
        requests = args.requests
    if args.warmup is not None:
        warmup = args.warmup
    return requests, max(warmup, conc)  # at least one round at the full concurrency


def delta(a: dict, b: dict, key: str) -> int:
    return int(b.get(key, 0)) - int(a.get(key, 0))


def run_cell(c: dict, w: dict, api: str, mode: str, conc: int, rep: int, args, prov: dict, session: Path) -> dict:
    cid = cell_id(c["id"], api, mode, conc, rep)
    requests, warmup = counts_for(w, conc, args)
    work = session / "work"
    work.mkdir(exist_ok=True)
    alpn = "h1"
    record = {"schema": SCHEMA, "kind": "cell", "cell": cid, "client": c["id"], "api": api, "mode": mode, "concurrency": conc,
              "repetition": rep, "requests": requests, "warmup": warmup, "provenance": prov, "load": "closed loop: service time at fixed concurrency"}
    na = c.get("na", {}).get(f"{api}/{mode}") or c.get("na", {}).get(api)
    if na and not args.try_na:
        record.update({"status": "n/a", "reason": na})
        return record
    peer = client = None
    try:
        cap = c.get("concurrency_cap_note", {}).get(f"{api}/{mode}")
        replay = args.replay_info["files"][(api, mode)] if args.replay_info else None
        peer = Peer(w, api, mode, alpn=alpn, node=args.node, cpus=args.peer_cpus, workdir=work, barrier=0 if cap else conc, workers=args.peer_workers,
                    replay_file=replay["path"] if replay else None)
        argv = taskset(args.client_cpus, client_argv(c, w, peer, api, mode, concurrency=conc, warmup=warmup, requests=requests, cold=False))
        home = scratch_home(session)
        client = spawn(argv, env=client_env(peer, w, c.get("env", {}), home), stderr_path=session / "logs" / f"{cid}.stderr", cwd=home)
        lines = Lines(client.stdout.fileno())
        # Warm-up rounds: the client reports `ready` after each round. Another round runs until a whole round opened no new
        # connection (or MAX_WARM_ROUNDS), so the window starts with every connection the client will use already open.
        reuse_expected = c.get("connection_reuse", True)
        rounds, previous = 0, None
        while True:
            if lines.read_event(args.timeout, {"ready"}) is None:
                raise RuntimeError("client exited before it was ready")
            rounds += 1
            s0 = peer.stats()
            if not reuse_expected or rounds >= (3 if c.get("pool_note") else MAX_WARM_ROUNDS) or s0["connections"] == previous:
                break
            previous = s0["connections"]
            client.stdin.write(b"MORE\n")
            client.stdin.flush()
        t_go = time.monotonic()
        client.stdin.write(b"GO\n")
        client.stdin.flush()
        result = lines.read_event(args.timeout, {"result"})
        t_done = time.monotonic()
        if result is None:
            raise RuntimeError("client exited without a result")
        s1 = peer.stats()
        client.wait(timeout=30)
    except Exception as exc:  # the record says what failed
        record.update({"status": "error", "reason": f"{type(exc).__name__}: {exc}"})
        tail = (session / "logs" / f"{cid}.stderr")
        if tail.exists():
            record["stderr_tail"] = tail.read_text(errors="replace")[-600:]
        return record
    finally:
        stop(client)
        if peer:
            peer.close()

    samples = result.pop("latency_ns")
    raw = session / "samples" / f"{cid}.json.gz"
    with gzip.open(raw, "wt") as f:
        json.dump(samples, f)
    d = {k: delta(s0, s1, k) for k in ("requests", "h1", "h2", "invalid", "connections", "tls_errors", "response_bytes", "request_bytes",
                                       "replays", "replay_scheduled_us", "replay_late_us")}
    barrier_timeouts = s1.get("barrier_timeouts", 0)
    peer_cpu_us = delta(s0, s1, "cpu_user_us") + delta(s0, s1, "cpu_system_us")
    wall_ns = result["wall_ns"]
    reasons = []
    if result["failed"]:
        reasons.append(f"client reported {result['failed']} failed requests: {result.get('first_error')}")
    if result["verify_failed"] or result["ok"] != requests:
        reasons.append(f"verification: ok={result['ok']} of {requests}, mismatches={result['verify_failed']}")
    if d["requests"] != requests:
        reasons.append(f"peer saw {d['requests']} requests in the window, expected {requests} (retries or extra calls)")
    if d["invalid"]:
        reasons.append(f"peer rejected {d['invalid']} requests")
    if d["h2"]:
        reasons.append(f"{d['h2']} requests used HTTP/2")
    if args.replay_info and d["replays"] != requests:
        reasons.append(f"peer replayed {d['replays']} recordings in the window, expected {requests}")
    flags = []
    if d["tls_errors"]:
        message = f"{d['tls_errors']} TLS handshakes were abandoned by the client"
        if c.get("pool_note") and d["connections"]:
            flags.append(f"{message} while it opened connections after the warm-up: {c['pool_note']}")
        else:
            reasons.append(f"{d['tls_errors']} TLS errors")
    if barrier_timeouts:
        reasons.append("the warm-up never had the full concurrency in flight (the peer's barrier timed out)")
    if cap and conc > 1:
        flags.append(f"requested concurrency {conc} is not what was in flight: {cap}")
    reuse = c.get("connection_reuse", True)
    if d["connections"] and reuse:
        message = f"{d['connections']} new connections inside the measured window"
        if c.get("pool_note"):
            flags.append(f"{message}: {c['pool_note']}")
        else:
            reasons.append(message)
    elif not reuse:
        flags.append(f"{d['connections']} connections opened for {requests} requests: the SDK opens a connection per request")
    record.update({
        "status": "ok" if not reasons else "invalid", "invalid_reasons": reasons, "flags": flags,
        "result": result, "samples_path": str(raw.relative_to(session)),
        "peer": {"window": d, "cpu_us": peer_cpu_us, "serving_processes": args.peer_workers, "cpu_utilization": peer_cpu_us / (wall_ns / 1000.0) if wall_ns else None,
                 "observed": s1.get("observed"), "alpn": alpn},
        "connection_reuse_expected": reuse,
        "replay": None if not args.replay_info else {
            "file": replay["file"], "sha256": replay["sha256"], "model": replay["model"],
            "scheduled_mean_us": d["replays"] and d["replay_scheduled_us"] / d["replays"],
            "late_mean_us": d["replays"] and d["replay_late_us"] / d["replays"]},
        "warm_rounds": rounds, "warm_settled": s0["connections"] == previous, "runner_wall_s": t_done - t_go, "start_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    })
    return record


# ----------------------------------------------------------------------------------------------------------------
# cold start

def run_cold(c: dict, w: dict, api: str, mode: str, args, prov: dict, session: Path) -> dict:
    cid = f"{c['id']}.{api}.{mode}.cold"
    work = session / "work"
    work.mkdir(exist_ok=True)
    record = {"schema": SCHEMA, "kind": "cold", "cell": cid, "client": c["id"], "api": api, "mode": mode, "provenance": prov,
              "definition": "wall time from spawning the process to its first completed and verified request, against a running peer; "
                            "one uncounted spawn first so that program files are in the page cache"}
    na = c.get("na", {}).get(f"{api}/{mode}") or c.get("na", {}).get(api)
    if na and not args.try_na:
        record.update({"status": "n/a", "reason": na})
        return record
    peer = None
    times, failures = [], []
    try:
        peer = Peer(w, api, mode, alpn="h1", node=args.node, cpus=args.peer_cpus, workdir=work, workers=args.peer_workers)
        for i in range(args.cold_reps + 1):
            argv = taskset(args.client_cpus, client_argv(c, w, peer, api, mode, concurrency=1, warmup=0, requests=1, cold=True))
            home = scratch_home(session)
            env = client_env(peer, w, c.get("env", {}), home)
            t0 = time.monotonic()
            proc = spawn(argv, env=env, stderr_path=session / "logs" / f"{cid}.stderr", cwd=home)
            try:
                ev = Lines(proc.stdout.fileno()).read_event(args.timeout, {"first"})
                t1 = time.monotonic()
                if ev is None or not ev.get("ok"):
                    failures.append(f"spawn {i}: no verified first request")
                elif i > 0:
                    times.append(t1 - t0)
            except TimeoutError:
                failures.append(f"spawn {i}: timeout")
            finally:
                stop(proc)
    finally:
        if peer:
            peer.close()
    record.update({"status": "ok" if len(times) >= args.cold_reps and not failures else "invalid", "invalid_reasons": failures,
                   "seconds": times, "spawns": len(times)})
    return record


# ----------------------------------------------------------------------------------------------------------------
# install footprint

def dir_bytes(path: Path) -> int:
    total = 0
    for root, _, files in os.walk(path):
        for name in files:
            p = Path(root, name)
            if not p.is_symlink():
                try:
                    total += p.stat().st_size
                except OSError:
                    pass
    return total


def measure_size(c: dict) -> dict:
    spec = c.get("footprint")
    record = {"schema": SCHEMA, "kind": "size", "client": c["id"]}
    if not spec:
        record.update({"status": "n/a", "reason": "no footprint declared"})
        return record
    kind, path = spec["kind"], Path(spec["path"])
    if not path.exists():
        record.update({"status": "error", "reason": f"{path} does not exist"})
        return record
    if kind == "binary":
        stripped = Path(tempfile.mkdtemp(prefix="sdkbench-strip-")) / path.name
        shutil.copy2(path, stripped)
        subprocess.run(["strip", str(stripped)], check=False)
        record.update({"status": "ok", "binary_bytes": path.stat().st_size, "stripped_bytes": stripped.stat().st_size,
                       "shared_libraries": sorted(l.split()[0] for l in sh(["ldd", str(path)]).splitlines() if "=>" in l or l.strip().startswith("/"))})
        shutil.rmtree(stripped.parent, ignore_errors=True)
    elif kind == "venv":
        site = next(path.glob("lib/python*/site-packages"))
        dists = sorted(p.name for p in site.glob("*.dist-info"))
        record.update({"status": "ok", "install_bytes": dir_bytes(path), "site_packages_bytes": dir_bytes(site), "packages": len(dists)})
    elif kind == "node_modules":
        count = 0
        for p in path.iterdir():
            if p.is_dir() and not p.name.startswith("."):
                count += len([q for q in p.iterdir() if q.is_dir()]) if p.name.startswith("@") else 1
        record.update({"status": "ok", "install_bytes": dir_bytes(path), "packages": count})
    else:
        record.update({"status": "error", "reason": f"unknown footprint kind {kind}"})
    return record


# ----------------------------------------------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("phase", choices=["run", "cold", "sizes", "all"])
    ap.add_argument("--install", required=True, help="directory holding the contender environments")
    ap.add_argument("--out", required=True, help="session directory for the records")
    ap.add_argument("--contenders", help="comma-separated ids (default: all)")
    ap.add_argument("--apis", default="openai,anthropic")
    ap.add_argument("--modes", default="buffered,stream")
    ap.add_argument("--concurrency", default=None, help="comma-separated levels (default: the workload's)")
    ap.add_argument("--reps", type=int, default=5, help="fresh-process repetitions per cell")
    ap.add_argument("--cold-reps", type=int, default=20)
    ap.add_argument("--reruns", type=int, default=1, help="how many more times to run a cell that came out invalid (every attempt is kept in the records)")
    ap.add_argument("--requests", type=int, help="override the measured request count (smoke runs; disables p99 in the tables)")
    ap.add_argument("--warmup", type=int, help="override the warm-up request count")
    ap.add_argument("--client-cpus", help="taskset list for client processes")
    ap.add_argument("--peer-cpus", help="taskset list for the peer")
    ap.add_argument("--peer-workers", type=int, default=1, help="serving processes of the peer (they share one port)")
    ap.add_argument("--node", help="Node binary for the peer (default: node/bin/node under --install, else node on PATH)")
    ap.add_argument("--timeout", type=float, default=900.0, help="per-cell seconds")
    ap.add_argument("--try-na", action="store_true", help="run cells that the manifest declares n/a (to confirm the reason)")
    ap.add_argument("--client-args", default="", help="extra arguments appended to every client's command line (experiments; recorded)")
    ap.add_argument("--workload", default="workload.json", help="workload file, relative to this directory (default: workload.json)")
    ap.add_argument("--replay", help="directory of recorded provider responses (replay/recordings/DATE); the run phase only")
    args = ap.parse_args()

    install = Path(args.install).expanduser().resolve()
    if not args.node:
        args.node = str(install / "node" / "bin" / "node") if (install / "node" / "bin" / "node").exists() else (shutil.which("node") or "node")
    session = Path(args.out).expanduser().resolve()
    for sub in ("samples", "logs", "work"):
        (session / sub).mkdir(parents=True, exist_ok=True)
    args.workload_path = HARNESS / args.workload
    w = load_workload(args.workload_path)
    args.replay_info = None
    if args.replay:
        if args.phase != "run":
            raise SystemExit("--replay applies to the run phase only")
        args.replay_info = load_replay(Path(args.replay).expanduser().resolve(), w, args.apis.split(","), args.modes.split(","))
    client_argv.replay = bool(args.replay_info)
    contenders = load_contenders(install, args.contenders.split(",") if args.contenders else None)
    levels = [int(x) for x in args.concurrency.split(",")] if args.concurrency else w["concurrency_levels"]
    client_argv.extra = args.client_args.split()
    prov = provenance(args, install, contenders, session)
    (session / "session.json").write_text(json.dumps(prov, indent=1))

    def append(name: str, rec: dict) -> None:
        with open(session / name, "a") as f:
            f.write(json.dumps(rec) + "\n")

    phases = ["run", "cold", "sizes"] if args.phase == "all" else [args.phase]
    for c in contenders:
        for phase in phases:
            if phase == "sizes":
                append("sizes.jsonl", {**measure_size(c), "provenance": prov})
                continue
            for api in args.apis.split(","):
                for mode in args.modes.split(","):
                    if phase == "cold":
                        rec = run_cold(c, w, api, mode, args, prov, session)
                        append("cold.jsonl", rec)
                        print(f"[cold] {rec['cell']}: {rec['status']}", flush=True)
                        continue
                    reason = c.get("na", {}).get(f"{api}/{mode}") or c.get("na", {}).get(api)
                    if reason and not args.try_na:
                        rec = {"schema": SCHEMA, "kind": "cell", "cell": f"{c['id']}.{api}.{mode}.na", "client": c["id"], "api": api,
                               "mode": mode, "status": "n/a", "reason": reason, "provenance": prov}
                        append("cells.jsonl", rec)
                        print(f"[cell] {rec['cell']}: n/a {reason}", flush=True)
                        continue
                    for conc in levels:
                        for rep in range(args.reps):
                            for attempt in range(1 + args.reruns):
                                rec = run_cell(c, w, api, mode, conc, rep, args, prov, session)
                                rec["attempt"] = attempt
                                append("cells.jsonl", rec)
                                print(f"[cell] {rec['cell']}: {rec['status']} {rec.get('reason', '') or '; '.join(rec.get('invalid_reasons', []))}", flush=True)
                                if rec["status"] == "ok":
                                    break
    return 0


if __name__ == "__main__":
    sys.exit(main())
