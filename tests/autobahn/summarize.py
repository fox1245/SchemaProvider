#!/usr/bin/env python3
"""Writes a compact summary of an Autobahn|Testsuite client report (index.json) and fails unless
every case ended OK, NON-STRICT or INFORMATIONAL with a clean close.

Usage: python3 summarize.py <reports/clients/index.json> [agent] > results.json
"""
import collections
import json
import sys

ACCEPTED = {"OK", "NON-STRICT", "INFORMATIONAL"}


def case_key(case):
    return [int(part) for part in case.split(".")]


def main():
    index = json.load(open(sys.argv[1], encoding="utf-8"))
    agent = sys.argv[2] if len(sys.argv) > 2 else "schemaprovider"
    cases = index[agent]
    summary = {
        "agent": agent,
        "cases": len(cases),
        "behavior": dict(collections.Counter(v["behavior"] for v in cases.values())),
        "behavior_close": dict(collections.Counter(v["behaviorClose"] for v in cases.values())),
        "not_ok": {k: [cases[k]["behavior"], cases[k]["behaviorClose"]] for k in sorted(cases, key=case_key)
                   if cases[k]["behavior"] != "OK" or cases[k]["behaviorClose"] != "OK"},
    }
    json.dump(summary, sys.stdout, indent=1)
    sys.stdout.write("\n")
    failed = [k for k, v in cases.items() if v["behavior"] not in ACCEPTED or v["behaviorClose"] not in ACCEPTED]
    if failed:
        sys.stderr.write("not accepted: " + ", ".join(sorted(failed, key=case_key)) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
