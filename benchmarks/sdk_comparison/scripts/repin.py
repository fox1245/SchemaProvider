#!/usr/bin/env python3
"""Move the pins of the git-sourced contenders to the current head of their default branch.

  repin.py            show what would change
  repin.py --write    update pins.json and the rev in clients/rust_genai/Cargo.toml

Afterwards regenerate the lock files (needs network and uv) and reinstall:
  python3 scripts/lock.py --install DIR --uv PATH_TO_UV
  (cd clients/rust_genai && cargo update -p genai)          # Cargo.lock
  (cd clients/js && npm install --package-lock-only --ignore-scripts)    # package-lock.json
  python3 scripts/install.py --install DIR
LiteLLM is a release pin and is never moved by this script.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

HARNESS = Path(__file__).resolve().parent.parent
PINS = HARNESS / "pins.json"


def head(repo: str, branch: str) -> str:
    out = subprocess.run(["git", "ls-remote", repo, f"refs/heads/{branch}"], capture_output=True, text=True, check=True).stdout
    return out.split()[0]


def npm_versions(repo: str, sha: str, packages: dict) -> dict:
    owner_repo = repo.removeprefix("https://github.com/")
    directories = {"ai": "ai", "@ai-sdk/openai": "openai", "@ai-sdk/anthropic": "anthropic"}
    found = {}
    for name in packages:
        url = f"https://raw.githubusercontent.com/{owner_repo}/{sha}/packages/{directories[name]}/package.json"
        found[name] = json.loads(urllib.request.urlopen(url, timeout=60).read())["version"]
    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()
    pins = json.loads(PINS.read_text())
    changed = []
    for group in ("python", "native", "javascript"):
        for name, pin in pins[group].items():
            if "sha" not in pin or "default_branch" not in pin:
                continue
            new = head(pin["repo"], pin["default_branch"])
            if new != pin["sha"]:
                changed.append((name, pin["sha"][:12], new[:12]))
                pin["sha"] = new
                if name == "vercel-ai":
                    pin["npm"] = npm_versions(pin["repo"], new, pin["npm"])
                if name == "rust-genai":
                    cargo = HARNESS / "clients" / "rust_genai" / "Cargo.toml"
                    text = cargo.read_text()
                    updated = re.sub(r'(rev = ")[0-9a-f]{40}(")', rf"\g<1>{new}\g<2>", text)
                    if args.write:
                        cargo.write_text(updated)
    for name, old, new in changed:
        print(f"{name}: {old} -> {new}")
    if not changed:
        print("every pin is at the head of its default branch")
    elif args.write:
        PINS.write_text(json.dumps(pins, indent=2) + "\n")
        print("pins.json updated; regenerate the lock files (see the top of this script)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
