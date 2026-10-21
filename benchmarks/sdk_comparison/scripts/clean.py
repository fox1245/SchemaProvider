#!/usr/bin/env python3
"""Remove what install.py created under an install directory, keeping the `installed/` records.

  clean.py --install DIR [--yes]

Lists what it would delete and deletes only with --yes. Only the directories install.py itself creates are touched.
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

KNOWN = ["bin", "build", "deps", "downloads", "js", "node", "npm-cache", "rust", "src", "wheels"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", required=True)
    ap.add_argument("--yes", action="store_true")
    args = ap.parse_args()
    root = Path(args.install).expanduser().resolve()
    targets = [root / name for name in KNOWN] + sorted(p for p in root.glob("venv-*")) + sorted(p for p in root.glob("build-*") if p.is_dir())
    for path in targets:
        if path.exists():
            size = sum(f.stat().st_size for f in path.rglob("*") if f.is_file() and not f.is_symlink())
            print(f"{'delete' if args.yes else 'would delete'} {path} ({size / 1e6:.0f} MB)")
            if args.yes:
                shutil.rmtree(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
