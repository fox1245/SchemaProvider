#!/usr/bin/env python3
"""Render the tables of a session from its records. Nothing in the output is typed by hand.

  render.py SESSION_DIR [SESSION_DIR ...] > tables.md
  render.py --smoke SESSION_DIR [...]        a validity matrix without timings, for functional runs

Python 3.10+ standard library only. Every number comes from `cells.jsonl`, `cold.jsonl`, `sizes.jsonl` and the
raw samples they point to; the descriptions of the contenders come from `contenders.json` and the `installed/` records
that `session.json` embeds.
"""
from __future__ import annotations

import gzip
import json
import math
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

HARNESS = Path(__file__).resolve().parent
MIN_P99, MIN_P95 = 1000, 100
PEER_BOUND = 0.8


def load_jsonl(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()] if path.exists() else []


def rank(sorted_values: list[float], p: float) -> float:
    """Nearest-rank percentile of an ascending list."""
    return sorted_values[max(0, math.ceil(p * len(sorted_values)) - 1)]


def ms(ns: float) -> str:
    v = ns / 1e6
    return f"{v:.3f}" if v < 1 else f"{v:.2f}" if v < 10 else f"{v:.1f}" if v < 100 else f"{v:.0f}"


def num(v: float, digits: int = 3) -> str:
    return f"{v:.{digits}g}" if v < 1000 else f"{v:,.0f}"


def span(values: list[float], fmt) -> str:
    lo, hi = min(values), max(values)
    return f"[{fmt(lo)}–{fmt(hi)}]" if len(values) > 1 and lo != hi else ""


class Cell:
    """All repetitions of one (client, api, mode, concurrency)."""

    def __init__(self, records: list[dict], session: Path):
        self.records = records
        self.ok = [r for r in records if r["status"] == "ok"]
        self.invalid = [r for r in records if r["status"] in ("invalid", "error")]
        self.samples = []
        self.per_rep_n = []
        for r in self.ok:
            with gzip.open(session / r["samples_path"], "rt") as f:
                s = sorted(json.load(f))
            self.samples.append(s)
            self.per_rep_n.append(len(s))
        self.pooled = sorted(v for s in self.samples for v in s)

    def pct(self, p: float, min_n: int) -> str | None:
        if not self.ok or min(self.per_rep_n) < min_n:
            return None
        return ms(rank(self.pooled, p))

    def per_rep(self, p: float) -> list[float]:
        return [rank(s, p) for s in self.samples]

    def throughput(self) -> list[float]:
        return [r["result"]["requests"] / (r["result"]["wall_ns"] / 1e9) for r in self.ok]

    def cpu_us_per_request(self) -> list[float]:
        return [(r["result"]["cpu_user_us"] + r["result"]["cpu_sys_us"]) / r["result"]["requests"] for r in self.ok]

    def hwm_kb(self) -> list[float]:
        return [r["result"]["hwm_end_kb"] for r in self.ok]

    def peer_bound(self) -> bool:
        # The peer can use one core per serving process, and no more cores than there are connections.
        return any((r["peer"].get("cpu_utilization") or 0) >= PEER_BOUND * min(r["peer"].get("serving_processes", 1), r["concurrency"]) for r in self.ok)

    def flags(self) -> set[str]:
        return {f for r in self.ok for f in r.get("flags", [])}


CONNECTION_REASON = "new connections inside the measured window"


def apply_annotations(records: list[dict], session: Path) -> list[dict]:
    """Records of one session, with its `annotations.json` applied and one record kept per repetition.

    annotations.json: {"flag_new_connections": {client: note}}. A record that was rejected only because connections were
    opened inside the window, for a client named there, is treated as valid and carries the note as a flag. The record files
    are never changed; the annotation says which clients and why, and the tables print it."""
    path = session / "annotations.json"
    notes = json.loads(path.read_text()).get("flag_new_connections", {}) if path.exists() else {}
    by_rep: dict[tuple, dict] = {}
    for r in records:
        rec = dict(r)
        if rec["status"] == "invalid" and rec["client"] in notes and rec.get("invalid_reasons") and all(CONNECTION_REASON in x for x in rec["invalid_reasons"]):
            counts = ", ".join(rec["invalid_reasons"])
            rec["status"] = "ok"
            rec["flags"] = list(rec.get("flags", [])) + [f"{counts}: {notes[rec['client']]}"]
            rec["annotated"] = True
        key = (rec["client"], rec["api"], rec["mode"], rec["concurrency"], rec["repetition"])
        old = by_rep.get(key)
        # prefer a record that was valid as run, then the later attempt
        if old is None or (old["status"] != "ok") or (old.get("annotated") and not rec.get("annotated") and rec["status"] == "ok") or (old.get("annotated") and rec.get("annotated")):
            by_rep[key] = rec
    return list(by_rep.values())


def table(headers: list[str], rows: list[list[str]], left: bool = False) -> str:
    out = ["| " + " | ".join(headers) + " |", "|" + "|".join(["---"] + ["---" if left else "---:"] * (len(headers) - 1)) + "|"]
    out += ["| " + " | ".join(r) + " |" for r in rows]
    return "\n".join(out)


def smoke(sessions: list[Path]) -> int:
    """A correctness matrix without any timing: for each cell, whether the run was valid."""
    manifest = json.loads((HARNESS / "contenders.json").read_text())["contenders"]
    marks: dict[tuple, dict] = defaultdict(dict)
    na: dict[tuple, str] = {}
    notes: dict[tuple, dict] = defaultdict(dict)
    failures = []
    levels = set()
    for session in sessions:
        for r in load_jsonl(session / "cells.jsonl"):
            key = (r["client"], r["api"], r["mode"])
            if r["status"] == "n/a":
                na[key] = r["reason"]
                continue
            lv = r["concurrency"]
            levels.add(lv)
            if marks[key].get(lv, "").startswith("ok") and r["status"] != "ok":
                continue  # an earlier attempt already came out valid
            if r["status"] == "ok":
                marks[key][lv] = "ok†" if r.get("flags") else "ok"
                if r.get("flags"):
                    notes[key][lv] = r["flags"]
            else:
                marks[key][lv] = {"invalid": "INVALID", "error": "ERROR"}[r["status"]]
                failures.append((key, lv, f"{r['cell']} attempt {r.get('attempt', 0)}: " + "; ".join(r.get("invalid_reasons") or [r.get("reason", "")])))
    levels = sorted(levels)
    out = ["| client | api | mode | " + " | ".join(f"c={lv}" for lv in levels) + " |", "|---|---|---|" + "---|" * len(levels)]
    for c in manifest:
        for api in ("openai", "anthropic"):
            for mode in ("buffered", "stream"):
                key = (c["id"], api, mode)
                if key in na:
                    out.append(f"| {c['label']} | {api} | {mode} | " + " | ".join(["n/a"] * len(levels)) + " |")
                elif key in marks:
                    out.append(f"| {c['label']} | {api} | {mode} | " + " | ".join(marks[key].get(lv, "–") for lv in levels) + " |")
    print("\n".join(out))
    if na:
        print("\nn/a:")
        for reason in sorted(set(na.values())):
            print("- " + ", ".join(f"{c} {a} {m}" for (c, a, m), why in sorted(na.items()) if why == reason) + ": " + reason)
    if any(notes.values()):
        print("\n† valid, with a property of the SDK that the record states:")
        seen = {}
        for key, per_level in sorted(notes.items()):
            for lv, flags in per_level.items():
                for flag in flags:
                    text = flag.split(": ", 1)[-1] if ": " in flag else flag
                    seen.setdefault((key[0], text), []).append(f"{key[1]} {key[2]} c={lv}: " + flag.split(" ", 1)[0] + (" connections" if "connection" in flag else ""))
        for (client, text), where in sorted(seen.items()):
            print(f"- {client}: {text}\n  ({'; '.join(where[:12])}{'; …' if len(where) > 12 else ''})")
    if failures:
        print("\nnot valid:")
        for key, lv, line in failures:
            superseded = marks[key].get(lv, "").startswith("ok")
            print("- " + line[:400] + (" (superseded by a valid rerun)" if superseded else ""))
    return 0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "--smoke":
        return smoke([Path(p) for p in sys.argv[2:]])
    sessions = [Path(p) for p in sys.argv[1:]]
    replayed = [bool(json.loads((s / "session.json").read_text()).get("replay")) for s in sessions]
    if any(replayed):
        if not all(replayed):
            raise SystemExit("a replay session cannot be rendered together with a mock-reply session")
        sys.path.insert(0, str(HARNESS / "replay"))
        from render_replay import render as render_replay  # noqa: E402
        sys.stdout.write(render_replay(sessions))
        return 0
    manifest = json.loads((HARNESS / "contenders.json").read_text())["contenders"]
    order = [c["id"] for c in manifest]
    meta = {c["id"]: c for c in manifest}
    workload = json.loads((HARNESS / "workload.json").read_text())

    cells_by_key: dict[tuple, list[dict]] = defaultdict(list)
    src: dict[tuple, Path] = {}
    na: dict[tuple, str] = {}
    cold: dict[tuple, dict] = {}
    sizes: dict[str, dict] = {}
    provs = []
    annotations: list[dict] = []
    comments: dict[str, str] = {}
    annotations_clients: list[str] = []
    for session in sessions:
        provs.append(json.loads((session / "session.json").read_text()))
        raw = load_jsonl(session / "cells.jsonl")
        for r in raw:
            if r["status"] == "n/a":
                na[(r["client"], r["api"], r["mode"])] = r["reason"]
        for r in apply_annotations([r for r in raw if r["status"] != "n/a"], session):
            key = (r["client"], r["api"], r["mode"], r["concurrency"])
            cells_by_key[key].append(r)
            src[key] = session
        annotation_path = session / "annotations.json"
        if annotation_path.exists():
            annotations.append(json.loads(annotation_path.read_text()))
            comments.update(annotations[-1].get("comments", {}))
            annotations_clients.extend(annotations[-1].get("flag_new_connections", {}))
        for r in load_jsonl(session / "cold.jsonl"):
            cold[(r["client"], r["api"], r["mode"])] = r
        for r in load_jsonl(session / "sizes.jsonl"):
            sizes[r["client"]] = r
    cells = {k: Cell(v, src[k]) for k, v in cells_by_key.items()}
    levels = sorted({k[3] for k in cells})
    clients = [c for c in order if any(k[0] == c for k in cells) or any(k[0] == c for k in na)]
    p = provs[0]

    out = []
    w = out.append
    w("# SDK comparison tables\n")
    w("Generated by `render.py` from the records of the session(s): " + ", ".join(f"`{s.name}`" for s in sessions) + ". Nothing below is typed by hand.\n")
    w("## Provenance\n")
    w(f"- Run (UTC): {p['utc']}; harness commit `{p['harness_commit'][:12]}`{' (working tree dirty)' if p['harness_dirty'] else ''}.")
    w(f"- CPU: {p['cpu_model']}, {p['cpu_count']} CPUs, governor `{p['cpu_governor']}`; kernel {p['kernel']} ({p['machine']}).")
    w(f"- Client processes pinned to CPUs: `{p['client_cpus'] or 'not pinned'}`; peer pinned to: `{p['peer_cpus'] or 'not pinned'}`, {p.get('peer_workers', 1)} peer process(es).")
    tools = dict(p["tools"])
    if not tools.get("rustc") and p["installed"].get("rust-genai", {}).get("rustc"):
        tools["rustc"] = p["installed"]["rust-genai"]["rustc"]
    w("- Tools: " + "; ".join(f"{k} {v}" for k, v in tools.items() if v) + ".")
    w("- Script hashes (sha256): " + "; ".join(f"`{k}` {v[:12]}" for k, v in p["script_sha256"].items()) + ".")
    w("- Workload: one system and one user message, `max_tokens` "
      f"{workload['max_tokens']}, HTTP/1.1 only, TLS verified against the peer's CA, reply text {len(workload['response_prefix']) + workload['response_payload_bytes']} bytes, "
      f"streamed as {workload['stream_chunks']} text deltas. Load shape: **closed loop**, i.e. service time at fixed concurrency; it says nothing about arrival-rate behaviour.\n")

    for a in annotations:
        for client, note in a.get("flag_new_connections", {}).items():
            w(f"- **Annotation (made after the run, from its records):** cells of {meta[client]['label']} that the runner rejected only because connections were opened inside the "
              f"measured window are shown as valid with a † flag, as for the other clients whose pool the runner cannot hold steady. {note} The records are unchanged; "
              f"`annotations.json` in the session directory names the clients.")
    for client in annotations_clients:
        mine = [cell for k, cell in cells.items() if k[0] == client]
        flagged = sum(1 for cell in mine for r in cell.ok if r.get("annotated"))
        total = sum(len(cell.ok) for cell in mine)
        w(f"  - {meta[client]['label']}: {flagged} of {total} of its valid-or-annotated cell records were rejected by the strict rule and are shown with †.")
    for client, text in comments.items():
        w(f"- Comment on {meta[client]['label']}, written after the run from its records: {text}")
    w("")
    w("## Contenders\n")
    rows = []
    for c in clients:
        inst = p["installed"].get(c, {})
        ident = inst.get("sha") or inst.get("source_commit") or ("PyPI release" if inst.get("kind") == "pypi-release" else "built from this repository")
        ident = f"`{ident[:12]}`" if len(ident) == 40 else ident
        version = inst.get("version") or inst.get("crate_version") or ""
        if not version and inst.get("wheel"):
            version = inst["wheel"].split("-")[1]
        if inst.get("npm"):
            version = ", ".join(f"{k} {v}" for k, v in inst["npm"].items())
        wheel = inst.get("wheel_sha256") or inst.get("binary_sha256") or ""
        rows.append([meta[c]["label"], meta[c]["language"], ident, version, f"`{wheel[:12]}`" if wheel else "", str(inst.get("package_count") or inst.get("crates") or "")])
    w(table(["client", "language", "installed at", "version", "wheel / binary sha256", "packages"], rows, left=True) + "\n")

    builds = [c for c in clients if p["installed"].get(c, {}).get("libcurl_build")]
    if builds:
        w("### libcurl builds\n")
        w("Rows that link libcurl, with the libcurl they ran against. The version string is the one the program printed at run time (`curl_version()`); "
          "the TLS and HTTP/2 libraries and the build's features come from the installed record. A row's numbers belong to its libcurl: do not compare "
          "or quote a SchemaProvider result without it.\n")
        rows = []
        for c in builds:
            b = p["installed"][c]["libcurl_build"]
            seen = next((r["result"]["config"].get("libcurl") for k, cell in cells.items() if k[0] == c for r in cell.ok), None)
            rows.append([meta[c]["label"], f"`{seen or b['libcurl']}`", b.get("tls") or "", b.get("http2") or "", b["origin"], " ".join(b["features"])])
        w(table(["row", "libcurl at run time", "TLS", "HTTP/2", "origin", "features"], rows, left=True) + "\n")

    w("### What each client does per request, and how it was driven\n")
    for c in clients:
        m = meta[c]
        candidates = [r for k, cell in cells.items() if k[0] == c for r in cell.ok]
        sample = max(candidates, key=lambda r: r["concurrency"]) if candidates else None
        cfg = sample["result"]["config"] if sample else {}
        w(f"- **{m['label']}** ({m['kind']}). " + (f"Settings as run at c={sample['concurrency']}. Driven by: {cfg.get('concurrency_model', '')}. " if cfg else "")
          + (f"HTTP: {cfg.get('http')}. Pool: {cfg.get('pool')}. CA: {cfg.get('ca')}. Retries: {cfg.get('retries')}. Telemetry: {cfg.get('telemetry')}. " if cfg else "")
          + (f"Also per request: {'; '.join(m['per_request'])}." if m.get("per_request") else ""))
    w("")

    na_rows = sorted(na.items())
    if na_rows:
        w("### Not applicable\n")
        for (c, api, mode), reason in na_rows:
            w(f"- {meta[c]['label']}, {api} {mode}: n/a, {reason}.")
        w("")

    for api in ("openai", "anthropic"):
        for mode in ("buffered", "stream"):
            present = [c for c in clients if any((c, api, mode, lv) in cells for lv in levels)]
            if not present:
                continue
            title = f"{'OpenAI Chat Completions' if api == 'openai' else 'Anthropic Messages'}, {'buffered' if mode == 'buffered' else 'streamed'}"
            w(f"## {title}\n")
            marks = set()

            def head(cols):
                return ["client"] + [f"c={lv}" for lv in levels]

            # latency
            rows = []
            for c in present:
                row = [meta[c]["label"] + (" (floor)" if meta[c]["kind"] == "null" else "")]
                for lv in levels:
                    cell = cells.get((c, api, mode, lv))
                    if not cell or not cell.ok:
                        row.append("invalid" if cell and cell.invalid else "–")
                        continue
                    parts = [ms(rank(cell.pooled, 0.5)), ms(rank(cell.pooled, 0.9))]
                    p99 = cell.pct(0.99, MIN_P99)
                    parts.append(p99 if p99 is not None else "n<1000")
                    parts.append(ms(cell.pooled[-1]))
                    mark = ""
                    if cell.peer_bound():
                        mark += "‡"
                    if cell.flags():
                        mark += "†"
                    row.append(" / ".join(parts) + mark)
                rows.append(row)
            w("**Latency in ms: p50 / p90 / p99 / max**, samples pooled over the repetitions; p99 is shown only when every repetition has at least 1,000 samples.\n")
            w(table(head(levels), rows) + "\n")

            rows = []
            for c in present:
                row = [meta[c]["label"]]
                for lv in levels:
                    cell = cells.get((c, api, mode, lv))
                    if not cell or not cell.ok:
                        row.append("–")
                        continue
                    t = cell.throughput()
                    row.append(f"{num(statistics.median(t))} {span(t, num)}".strip())
                rows.append(row)
            w("**Throughput in requests/s** (median over repetitions, [min–max] across them):\n")
            w(table(head(levels), rows) + "\n")

            rows = []
            for c in present:
                row = [meta[c]["label"]]
                for lv in levels:
                    cell = cells.get((c, api, mode, lv))
                    if not cell or not cell.ok:
                        row.append("–")
                        continue
                    v = cell.cpu_us_per_request()
                    row.append(f"{num(statistics.median(v))} {span(v, num)}".strip())
                rows.append(row)
            w("**Client CPU per request in µs** (user+system of the whole process over the measured window, from its own usage counters):\n")
            w(table(head(levels), rows) + "\n")

            rows = []
            for c in present:
                row = [meta[c]["label"]]
                base = cells.get((c, api, mode, levels[0]))
                for lv in levels:
                    cell = cells.get((c, api, mode, lv))
                    if not cell or not cell.ok:
                        row.append("–")
                        continue
                    hwm = statistics.median(cell.hwm_kb()) / 1024
                    cellstr = f"{hwm:.0f}"
                    if lv != levels[0] and base and base.ok:
                        marginal = (statistics.median(cell.hwm_kb()) - statistics.median(base.hwm_kb())) / (lv - levels[0]) / 1024
                        cellstr += f" (+{marginal * 1024:.0f} KiB/req)"
                    row.append(cellstr)
                rows.append(row)
            w("**Peak RSS in MiB** (VmHWM at the end of the window; **includes the interpreter or runtime and its imports**). "
              f"In brackets: marginal memory per in-flight request, (peak at c=N minus peak at c={levels[0]}) / (N − {levels[0]}), from separate fresh processes:\n")
            w(table(head(levels), rows) + "\n")
            notes = []
            if any(cell.peer_bound() for k, cell in cells.items() if k[1] == api and k[2] == mode):
                notes.append("‡ the peer used at least 80% of the cores it could use during the window: the cell measures the peer as much as the client. Peer CPU is recorded in the records and never subtracted.")
            grouped: dict[tuple, list[int]] = defaultdict(list)
            for k, cell in cells.items():
                if k[1] != api or k[2] != mode:
                    continue
                for r in cell.ok:
                    for flag in r.get("flags", []):
                        head, _, note = flag.partition(": ")
                        count = re.search(r"\d+", head)
                        grouped[(k[0], note or head)].append(int(count.group()) if count and note and "connections" in head else 0)
            for (client, note), counts in sorted(grouped.items(), key=lambda kv: order.index(kv[0][0])):
                seen = f" (new connections per cell, in {len(counts)} cells: {min(counts)} to {max(counts)}, median {int(statistics.median(counts))})" if any(counts) else ""
                notes.append(f"† {meta[client]['label']}: {note}{seen}")
            inv = [(k, cell) for k, cell in sorted(cells.items()) if k[1] == api and k[2] == mode and cell.invalid and not cell.ok]
            for k, cell in inv:
                notes.append(f"invalid: {meta[k[0]]['label']} c={k[3]}: " + "; ".join(sorted({reason for r in cell.invalid for reason in (r.get('invalid_reasons') or [r.get('reason', '')])}))[:400])
            w("\n".join(f"- {n}" for n in notes) + ("\n" if notes else ""))

    if cold:
        w("## Cold start\n")
        w("Wall time from spawning the process to its first completed, verified request against a running peer (so it includes imports, client construction, the TLS handshake and one request). "
          "One uncounted spawn precedes the counted ones so that program files are in the page cache. Median [min–max] in ms over fresh processes.\n")
        rows = []
        for c in clients:
            row = [meta[c]["label"]]
            for api in ("openai", "anthropic"):
                for mode in ("buffered", "stream"):
                    r = cold.get((c, api, mode))
                    if not r or r["status"] != "ok":
                        row.append("–")
                        continue
                    s = [t * 1000 for t in r["seconds"]]
                    row.append(f"{statistics.median(s):.0f} [{min(s):.0f}–{max(s):.0f}] (n={len(s)})")
            rows.append(row)
        w(table(["client", "openai buffered", "openai stream", "anthropic buffered", "anthropic stream"], rows) + "\n")

    if sizes:
        w("## Install footprint\n")
        rows = []
        for c in clients:
            r = sizes.get(c)
            if not r or r["status"] != "ok":
                continue
            if "stripped_bytes" in r:
                rows.append([meta[c]["label"], "stripped binary", f"{r['stripped_bytes'] / 1e6:.2f} MB", f"{len(r['shared_libraries'])} shared libraries (not counted in the size)"])
            else:
                rows.append([meta[c]["label"], "full install", f"{r['install_bytes'] / 1e6:.1f} MB", f"{r['packages']} packages"])
        w(table(["client", "what", "size", "notes"], rows) + "\n")

    return print("\n".join(out)) or 0


if __name__ == "__main__":
    sys.exit(main())
