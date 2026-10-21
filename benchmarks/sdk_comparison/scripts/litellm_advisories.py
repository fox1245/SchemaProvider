#!/usr/bin/env python3
"""Check the pinned LiteLLM release against the published security advisories.

  litellm_advisories.py fetch   [--out locks/litellm-advisories.json]   needs the GitHub CLI, logged in; read-only
  litellm_advisories.py check   [--file locks/litellm-advisories.json] [--version X]

`fetch` stores two read-only queries and the time they were made: the repository's published security advisories
(`repos/BerriAI/litellm/security-advisories`) and the GitHub Advisory Database entries for the `litellm` pip package.
`check` exits non-zero if any stored advisory, withdrawn ones excepted, lists the version as affected. Versions are
compared by their numeric release segments, which is exact for final releases; a pre-release version is refused.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

HARNESS = Path(__file__).resolve().parent.parent
PINS = json.loads((HARNESS / "pins.json").read_text())
DEFAULT = HARNESS / "locks" / "litellm-advisories.json"


def gh(*args: str) -> list[dict]:
    out = subprocess.run(["gh", "api", *args], capture_output=True, text=True, check=True).stdout
    # --paginate prints one JSON array per page
    decoder, pos, items = json.JSONDecoder(), 0, []
    while pos < len(out):
        while pos < len(out) and out[pos].isspace():
            pos += 1
        if pos >= len(out):
            break
        value, pos = decoder.raw_decode(out, pos)
        items += value if isinstance(value, list) else [value]
    return items


def fetch(path: Path) -> None:
    repo = gh("repos/BerriAI/litellm/security-advisories", "--paginate")
    database = gh("-X", "GET", "/advisories", "-f", "ecosystem=pip", "-f", "affects=litellm", "-F", "per_page=100", "--paginate")
    entries = []
    for source, items in (("repository", repo), ("database", database)):
        for a in items:
            ranges = [{"affected": v.get("vulnerable_version_range"), "patched": v.get("patched_versions") or v.get("first_patched_version")}
                      for v in a.get("vulnerabilities") or [] if (v.get("package") or {}).get("name") == "litellm"]
            entries.append({"source": source, "ghsa": a.get("ghsa_id"), "cve": a.get("cve_id"), "severity": a.get("severity"),
                            "state": a.get("state"), "published": a.get("published_at"), "withdrawn": a.get("withdrawn_at"),
                            "summary": a.get("summary"), "ranges": ranges})
    path.write_text(json.dumps({"fetched_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), "advisories": entries}, indent=1) + "\n")
    print(f"{len(entries)} advisories stored in {path}")


def release(version: str) -> tuple[int, ...]:
    return tuple(int(x) for x in re.findall(r"\d+", re.split(r"(?:rc|dev|a|b|-)", version, maxsplit=1)[0]))


def in_range(version: tuple[int, ...], expression: str) -> bool:
    for clause in expression.split(","):
        m = re.match(r"\s*(>=|<=|>|<|==|=)?\s*([0-9][^\s]*)", clause)
        if not m:
            return True  # unreadable: assume affected
        op, bound = m.group(1) or "==", release(m.group(2))
        ok = {">=": version >= bound, "<=": version <= bound, ">": version > bound, "<": version < bound, "==": version == bound, "=": version == bound}[op]
        if not ok:
            return False
    return True


def check(path: Path, version: str) -> int:
    if re.search(r"(rc|dev|a|b)\d", version):
        print(f"{version} is a pre-release; only final releases can be checked")
        return 2
    data = json.loads(path.read_text())
    v = release(version)
    hits = []
    for a in data["advisories"]:
        if a.get("withdrawn"):
            continue
        for r in a["ranges"]:
            if r["affected"] and in_range(v, r["affected"]):
                hits.append((a["ghsa"], a["cve"], a["severity"], r["affected"], r["patched"]))
    print(f"LiteLLM {version}: advisories fetched {data['fetched_utc']}, {len(data['advisories'])} entries")
    if hits:
        for h in hits:
            print("  AFFECTED:", *h)
        return 1
    print("  no published advisory lists this version as affected")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["fetch", "check"])
    ap.add_argument("--file", "--out", dest="file", default=str(DEFAULT))
    ap.add_argument("--version", default=PINS["python"]["litellm"]["version"])
    args = ap.parse_args()
    return (fetch(Path(args.file)) or 0) if args.command == "fetch" else check(Path(args.file), args.version)


if __name__ == "__main__":
    sys.exit(main())
