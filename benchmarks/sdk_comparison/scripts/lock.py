#!/usr/bin/env python3
"""Regenerate the hash-locked requirement files in ../locks from ../pins.json.

Needs network, git and uv. The lock files are committed; installation only reads them.

  lock.py --install DIR --uv PATH_TO_UV
"""
from __future__ import annotations

import argparse
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import HARNESS, PINS, clean_env, git_checkout, run  # noqa: E402

LOCKS = HARNESS / "locks"

BUILDERS = {
    "hatch-1.32.4": ["hatchling==1.32.4"],
    "hatch-1.26.3": ["hatchling==1.26.3", "hatch-fancy-pypi-readme>=22.4,<26"],
    "setuptools": ["setuptools>=48"],
    "poetry": ["poetry-core"],
}


def caret(spec: str) -> str:
    """Poetry caret ranges to PEP 440."""
    spec = spec.strip()
    if spec.startswith("^"):
        parts = [int(p) for p in spec[1:].split(".")]
        upper = [parts[0] + 1] if parts[0] else [0, (parts[1] if len(parts) > 1 else 0) + 1]
        return f">={spec[1:]},<{'.'.join(map(str, upper))}"
    return spec


def project_requirements(src: Path, extras: list[str]) -> list[str]:
    data = tomllib.loads((src / "pyproject.toml").read_text())
    if "project" in data and "dependencies" in data["project"]:
        reqs = list(data["project"]["dependencies"])
        optional = data["project"].get("optional-dependencies", {})
        for extra in extras:
            reqs += optional.get(extra, [])
        return reqs
    poetry = data["tool"]["poetry"]
    reqs, wanted = [], set()
    for extra in extras:
        wanted.update(poetry.get("extras", {}).get(extra, []))
    for name, spec in poetry["dependencies"].items():
        if name == "python":
            continue
        version = spec.get("version", "") if isinstance(spec, dict) else spec
        if isinstance(spec, dict) and spec.get("optional") and name not in wanted:
            continue
        ranges = ",".join(caret(p) for p in version.split(",")) if version and version != "*" else ""
        reqs.append(f"{name}{ranges.replace(' ', '')}")
    return reqs


def compile_lock(uv: str, name: str, requirements: list[str], exclude_newer: str) -> None:
    (LOCKS / "in").mkdir(parents=True, exist_ok=True)
    infile = LOCKS / "in" / f"{name}.in"
    infile.write_text("\n".join(requirements) + "\n")
    run([uv, "pip", "compile", str(infile), "--generate-hashes", "--python-version", "3.12", "--no-build",
         "--exclude-newer", exclude_newer, "--universal", "-o", str(LOCKS / f"{name}.txt"), "--no-header"], env=clean_env())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", required=True)
    ap.add_argument("--uv", required=True)
    args = ap.parse_args()
    install = Path(args.install)
    newer = PINS["exclude_newer"]
    for name, reqs in BUILDERS.items():
        compile_lock(args.uv, f"build-{name}", reqs, newer)
    for name, pin in PINS["python"].items():
        if "pypi" in pin:
            compile_lock(args.uv, name, [f"{pin['pypi']}=={pin['version']}"], newer)
            continue
        src = install / "src" / name
        git_checkout(pin["repo"], pin["sha"], src)
        compile_lock(args.uv, name, project_requirements(src, pin.get("extras", [])), newer)
    return 0


if __name__ == "__main__":
    sys.exit(main())
