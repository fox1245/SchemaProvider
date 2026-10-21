"""Helpers shared by lock.py and install.py (standard library only)."""
from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HARNESS = Path(__file__).resolve().parent.parent
PINS = json.loads((HARNESS / "pins.json").read_text())


def clean_env(extra: dict | None = None) -> dict:
    """An environment with no credentials: only the variables a build needs."""
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": os.environ.get("HOME", "/tmp"), "LANG": "C.UTF-8",
           "PYTHONDONTWRITEBYTECODE": "1"}
    for name in ("TMPDIR", "SSL_CERT_FILE"):
        if name in os.environ:
            env[name] = os.environ[name]
    env.update(extra or {})
    return env


def assert_no_secrets(env: dict) -> None:
    bad = [k for k in env if re.search(r"(KEY|TOKEN|SECRET|PASSWORD)", k, re.I)]
    if bad:
        raise SystemExit(f"refusing to run with credential-like variables: {bad}")


def run(argv, *, cwd=None, env=None, check=True, capture=False) -> subprocess.CompletedProcess:
    env = clean_env() if env is None else env
    assert_no_secrets(env)
    print("+", " ".join(str(a) for a in argv), flush=True)
    return subprocess.run([str(a) for a in argv], cwd=cwd, env=env, check=check, text=True, capture_output=capture)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def git_checkout(repo: str, sha: str, dest: Path) -> str:
    """Fetch exactly `sha` into `dest` (a detached checkout) and verify it."""
    if not (dest / ".git").exists():
        dest.mkdir(parents=True, exist_ok=True)
        run(["git", "init", "-q", str(dest)])
        run(["git", "-C", dest, "remote", "add", "origin", repo])
    run(["git", "-C", dest, "fetch", "-q", "--depth", "1", "origin", sha])
    run(["git", "-C", dest, "checkout", "-q", "--detach", "FETCH_HEAD"])
    head = run(["git", "-C", dest, "rev-parse", "HEAD"], capture=True).stdout.strip()
    if head != sha:
        raise SystemExit(f"{dest}: checked out {head}, pinned {sha}")
    return head


def installed_record(install: Path, name: str, record: dict) -> None:
    out = install / "installed"
    out.mkdir(parents=True, exist_ok=True)
    text = json.dumps(record, indent=1, sort_keys=True) + "\n"
    (out / f"{name}.json").write_text(text.replace(str(Path.home()), "~"))
