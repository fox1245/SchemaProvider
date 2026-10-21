#!/usr/bin/env python3
"""render.py on a synthetic session: percentile rules, peer-bound marks and invalid cells."""
import gzip
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HARNESS = Path(__file__).resolve().parents[1]


def session_with(cells, samples):
    root = Path(tempfile.mkdtemp(prefix="sdkb-render-"))
    (root / "samples").mkdir()
    prov = {"utc": "2026-01-01T00:00:00Z", "harness_commit": "0" * 40, "harness_dirty": False, "cpu_model": "test cpu", "cpu_count": 4,
            "cpu_governor": "performance", "client_cpus": None, "peer_cpus": None, "kernel": "6", "machine": "x86_64",
            "tools": {"python": "3.12"}, "script_sha256": {"sdkbench.py": "ab" * 32}, "installed": {}}
    (root / "session.json").write_text(json.dumps(prov))
    lines = []
    for name, record in cells.items():
        if name in samples:
            with gzip.open(root / "samples" / f"{name}.json.gz", "wt") as f:
                json.dump(samples[name], f)
            record["samples_path"] = f"samples/{name}.json.gz"
        lines.append(json.dumps(record))
    (root / "cells.jsonl").write_text("\n".join(lines) + "\n")
    return root


def cell(name, conc, rep, n, status="ok", util=0.1, flags=()):
    return {"cell": name, "kind": "cell", "client": "null-curl", "api": "openai", "mode": "buffered", "concurrency": conc, "repetition": rep,
            "status": status, "invalid_reasons": ["3 new connections inside the measured window"] if status == "invalid" else [],
            "flags": list(flags), "provenance": {},
            "result": {"requests": n, "wall_ns": n * 1_000_000, "cpu_user_us": n * 100, "cpu_sys_us": n * 50, "hwm_end_kb": 10_000 + conc,
                       "config": {"concurrency_model": "test"}},
            "peer": {"cpu_utilization": util, "window": {"connections": 0}}}


def render(root):
    return subprocess.run([sys.executable, str(HARNESS / "render.py"), str(root)], capture_output=True, text=True, check=True).stdout


class RenderTests(unittest.TestCase):
    def test_p99_needs_a_thousand_samples_in_every_repetition(self):
        big = [1_000_000 + i for i in range(1000)]
        cells = {"a.c1.r0": cell("a.c1.r0", 1, 0, 1000), "a.c1.r1": cell("a.c1.r1", 1, 1, 1000),
                 "a.c8.r0": cell("a.c8.r0", 8, 0, 100), "a.c8.r1": cell("a.c8.r1", 8, 1, 1000)}
        samples = {"a.c1.r0": big, "a.c1.r1": big, "a.c8.r0": big[:100], "a.c8.r1": big}
        out = render(session_with(cells, samples))
        row = next(l for l in out.splitlines() if l.startswith("| null client") and " / " in l)
        cols = [c.strip() for c in row.split("|")[2:4]]
        self.assertNotIn("n<1000", cols[0])  # c=1: every repetition has 1000 samples
        self.assertIn("n<1000", cols[1])     # c=8: one repetition has 100

    def test_peer_bound_cells_are_marked_and_invalid_cells_say_so(self):
        cells = {"a.c1.r0": cell("a.c1.r0", 1, 0, 1000, util=0.95), "a.c8.r0": cell("a.c8.r0", 8, 0, 1000, status="invalid")}
        samples = {"a.c1.r0": [1_000_000] * 1000}
        out = render(session_with(cells, samples))
        self.assertIn("‡", out)
        self.assertIn("invalid", out)
        self.assertIn("3 new connections", out)


if __name__ == "__main__":
    unittest.main()
