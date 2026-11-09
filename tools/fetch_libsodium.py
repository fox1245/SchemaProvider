#!/usr/bin/env python3
"""Fetch, verify, unpack, and optionally build the pinned libsodium source archive."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request


class FetchError(Exception):
    pass


def _no_duplicate_object_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise FetchError(f"duplicate JSON key {key!r} in dependencies file")
        result[key] = value
    return result


def _is_sha256_hex(value: str) -> bool:
    if len(value) != 64:
        return False
    for ch in value:
        if ch not in "0123456789abcdefABCDEF":
            return False
    return True


def load_libsodium_pin(dependencies_path: Path) -> dict[str, str]:
    try:
        raw = dependencies_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise FetchError(f"cannot read {dependencies_path}: {exc}") from exc

    try:
        doc = json.loads(raw, object_pairs_hook=_no_duplicate_object_pairs)
    except json.JSONDecodeError as exc:
        raise FetchError(f"invalid JSON in {dependencies_path}: {exc}") from exc

    if not isinstance(doc, dict):
        raise FetchError(f"{dependencies_path}: root must be a JSON object")
    if set(doc.keys()) != {"version", "libsodium"}:
        raise FetchError(f"{dependencies_path}: root keys must be 'version' and 'libsodium'")
    if doc.get("version") != 1 or isinstance(doc.get("version"), bool):
        raise FetchError(f"{dependencies_path}: version must be 1")

    sodium = doc.get("libsodium")
    if not isinstance(sodium, dict):
        raise FetchError(f"{dependencies_path}: 'libsodium' must be a JSON object")

    required_keys = {"version", "minimum_version", "archive", "url", "sha256"}
    if set(sodium.keys()) != required_keys:
        raise FetchError(
            f"{dependencies_path}: 'libsodium' keys must be {sorted(required_keys)}"
        )

    pin: dict[str, str] = {}
    for key in required_keys:
        val = sodium.get(key)
        if not isinstance(val, str) or not val:
            raise FetchError(f"{dependencies_path}: libsodium.{key} must be a non-empty string")
        pin[key] = val

    if not _is_sha256_hex(pin["sha256"]):
        raise FetchError(f"{dependencies_path}: libsodium.sha256 must be 64 hexadecimal digits")
    pin["sha256"] = pin["sha256"].lower()

    if pin["version"] not in pin["archive"] or not pin["archive"].endswith(".tar.gz"):
        raise FetchError(
            f"{dependencies_path}: libsodium.archive ({pin['archive']!r}) does not match "
            f"version ({pin['version']!r})"
        )
    if not pin["url"].startswith("https://") or not pin["url"].endswith("/" + pin["archive"]):
        raise FetchError(
            f"{dependencies_path}: libsodium.url ({pin['url']!r}) must be an https:// URL "
            f"ending with '/{pin['archive']}'"
        )
    return pin


def sha256_file(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(65536)
            if not chunk:
                break
            hasher.update(chunk)
    return hasher.hexdigest().lower()


def verify_archive_digest(
    archive_path: Path,
    pinned_sha256: str,
    caller_sha256: str | None = None,
) -> str:
    actual_sha256 = sha256_file(archive_path)
    print(f"sha256: {actual_sha256}", flush=True)
    pinned_lower = pinned_sha256.lower()
    if caller_sha256 is not None and caller_sha256.lower() != pinned_lower:
        raise FetchError(
            f"SHA-256 mismatch for {archive_path}: caller requested {caller_sha256.lower()}, "
            f"pinned digest is {pinned_lower}, got {actual_sha256}"
        )
    if actual_sha256 != pinned_lower:
        raise FetchError(
            f"SHA-256 mismatch for {archive_path}: expected {pinned_lower}, "
            f"got {actual_sha256}"
        )
    return actual_sha256


def download_archive(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(
        prefix=destination.name + ".",
        suffix=".part",
        dir=str(destination.parent),
    )
    os.close(fd)
    tmp_path = Path(tmp_name)
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "SchemaProvider-fetch/1.0"})
        with urllib.request.urlopen(request, timeout=120) as response, tmp_path.open("wb") as out:
            shutil.copyfileobj(response, out, length=65536)
        os.replace(tmp_path, destination)
    except Exception as exc:
        tmp_path.unlink(missing_ok=True)
        raise FetchError(f"failed to download {url}: {exc}") from exc


def resolve_archive(
    pin: dict[str, str],
    cache_dir: Path | None,
    archive_override: Path | None,
    caller_sha256: str | None,
    temp_dir: Path,
    offline: bool = False,
) -> Path:
    url = pin["url"]
    archive_name = pin["archive"]
    pinned_sha256 = pin["sha256"]

    if archive_override is not None:
        if archive_override.is_dir():
            archive_path = archive_override / archive_name
        else:
            archive_path = archive_override
        if archive_path.is_file():
            verify_archive_digest(archive_path, pinned_sha256, caller_sha256)
            return archive_path
        if offline:
            raise FetchError(f"{archive_path} is not present and downloads are disabled (--offline)")
        download_archive(url, archive_path)
        try:
            verify_archive_digest(archive_path, pinned_sha256, caller_sha256)
        except Exception:
            archive_path.unlink(missing_ok=True)
            raise
        return archive_path

    if cache_dir is not None:
        cache_dir.mkdir(parents=True, exist_ok=True)
        cached_archive = cache_dir / archive_name
        if cached_archive.is_file():
            verify_archive_digest(cached_archive, pinned_sha256, caller_sha256)
            return cached_archive
        if offline:
            raise FetchError(f"{cached_archive} is not present and downloads are disabled (--offline)")
        download_archive(url, cached_archive)
        try:
            verify_archive_digest(cached_archive, pinned_sha256, caller_sha256)
        except Exception:
            cached_archive.unlink(missing_ok=True)
            raise
        return cached_archive

    downloaded = temp_dir / archive_name
    download_archive(url, downloaded)
    verify_archive_digest(downloaded, pinned_sha256, caller_sha256)
    return downloaded


def safe_unpack_tarball(archive_path: Path, unpack_dir: Path, version: str) -> Path:
    unpack_dir.mkdir(parents=True, exist_ok=True)
    unpack_root = unpack_dir.resolve()
    expected_subdir = unpack_root / f"libsodium-{version}"
    if expected_subdir.exists():
        shutil.rmtree(expected_subdir)

    try:
        with tarfile.open(archive_path, mode="r:gz") as tar:
            members = tar.getmembers()
            if not members:
                raise FetchError(f"{archive_path}: archive is empty")
            for member in members:
                member_path = Path(member.name)
                if member_path.is_absolute() or ".." in member_path.parts:
                    raise FetchError(f"{archive_path}: unsafe member path {member.name!r}")
                target_path = (unpack_root / member_path).resolve()
                if target_path != unpack_root and unpack_root not in target_path.parents:
                    raise FetchError(f"{archive_path}: member escapes unpack directory: {member.name!r}")
                if member.issym() or member.islnk():
                    link_target = (target_path.parent / member.linkname).resolve()
                    if link_target != unpack_root and unpack_root not in link_target.parents:
                        raise FetchError(
                            f"{archive_path}: link member escapes unpack directory: {member.name!r}"
                        )
                elif not (member.isfile() or member.isdir()):
                    raise FetchError(f"{archive_path}: unsupported tar member type for {member.name!r}")

            if hasattr(tarfile, "data_filter"):
                tar.extractall(path=unpack_root, filter="data")
            else:
                tar.extractall(path=unpack_root)
    except (tarfile.TarError, OSError) as exc:
        raise FetchError(f"failed to unpack {archive_path}: {exc}") from exc

    if not expected_subdir.is_dir():
        raise FetchError(f"expected unpacked directory {expected_subdir} in {unpack_root}")

    if not (expected_subdir / "configure").is_file():
        raise FetchError(f"unpacked libsodium tree at {expected_subdir} is missing ./configure")
    if not (expected_subdir / "LICENSE").is_file():
        raise FetchError(f"unpacked libsodium tree at {expected_subdir} is missing LICENSE")
    return expected_subdir


def build_and_install(source_dir: Path, prefix: Path, wasm: bool, jobs: int) -> None:
    prefix_abs = prefix.resolve()
    prefix_abs.mkdir(parents=True, exist_ok=True)

    configure_args = [
        "./configure",
        "--disable-shared",
        "--enable-static",
        "--with-pic",
        f"--prefix={prefix_abs}",
    ]
    if wasm:
        configure_cmd = [
            "emconfigure",
            *configure_args,
            "--without-pthreads",
            "--disable-ssp",
            "--disable-asm",
        ]
        make_cmd = ["emmake", "make", f"-j{jobs}"]
        install_cmd = ["emmake", "make", "install"]
    else:
        configure_cmd = configure_args
        make_cmd = ["make", f"-j{jobs}"]
        install_cmd = ["make", "install"]

    for cmd in (configure_cmd, make_cmd, install_cmd):
        try:
            subprocess.run(cmd, cwd=str(source_dir), check=True)
        except (OSError, subprocess.CalledProcessError) as exc:
            raise FetchError(f"command failed ({' '.join(cmd)}): {exc}") from exc

    license_src = source_dir / "LICENSE"
    license_dst_dir = prefix_abs / "share" / "doc" / "libsodium"
    license_dst_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(license_src, license_dst_dir / "LICENSE")


def parse_args(argv: list[str]) -> argparse.Namespace:
    default_deps = Path(__file__).resolve().parent.parent / "config" / "dependencies.json"
    parser = argparse.ArgumentParser(
        description=(
            "Download, verify SHA-256 against config/dependencies.json, unpack, and "
            "optionally build and install a static libsodium library."
        )
    )
    parser.add_argument(
        "--offline",
        action="store_true",
        help="never download: the archive must already be at --archive or in --cache-dir (the build uses this)",
    )
    parser.add_argument(
        "positional_unpack_dir",
        nargs="?",
        default=None,
        help="Optional positional directory to unpack the verified source archive into.",
    )
    parser.add_argument(
        "--dependencies",
        "--config",
        dest="dependencies",
        type=Path,
        default=default_deps,
        help="Path to config/dependencies.json (default: %(default)s).",
    )
    parser.add_argument(
        "--cache-dir",
        "--cache",
        dest="cache_dir",
        type=Path,
        default=None,
        help="Directory caching the downloaded source archive.",
    )
    parser.add_argument(
        "--archive",
        "--archive-path",
        dest="archive",
        type=Path,
        default=None,
        help="Explicit path to the source archive file (or directory containing it).",
    )
    parser.add_argument(
        "--unpack-dir",
        "--source-dir",
        "--extract-dir",
        "--output-dir",
        "-o",
        dest="unpack_dir",
        type=Path,
        default=None,
        help="Directory to unpack the verified libsodium source archive into.",
    )
    parser.add_argument(
        "--prefix",
        "--install-prefix",
        dest="prefix",
        type=Path,
        default=None,
        help="Optional installation prefix; when set, configures, builds, and installs static libsodium.",
    )
    parser.add_argument(
        "--wasm",
        "--emscripten",
        dest="wasm",
        action="store_true",
        help="Build for WebAssembly under emconfigure/emmake with --without-pthreads --disable-ssp --disable-asm.",
    )
    parser.add_argument(
        "--target",
        choices=("native", "wasm", "emscripten"),
        default=None,
        help="Build target platform ('native' or 'wasm').",
    )
    parser.add_argument(
        "--sha256",
        "--expected-sha256",
        dest="sha256",
        default=None,
        help="Additional SHA-256 hex digest check (must equal both the archive and config/dependencies.json).",
    )
    parser.add_argument(
        "--jobs",
        "-j",
        dest="jobs",
        type=int,
        default=max(1, os.cpu_count() or 1),
        help="Number of parallel make jobs when building (default: %(default)s).",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        pin = load_libsodium_pin(args.dependencies)
        caller_sha256: str | None = None
        if args.sha256 is not None:
            if not _is_sha256_hex(args.sha256):
                raise FetchError("--sha256 must be 64 hexadecimal digits")
            caller_sha256 = args.sha256.lower()

        unpack_dir = args.unpack_dir
        if unpack_dir is not None and args.positional_unpack_dir is not None:
            raise FetchError("specify unpack directory either positionally or via --unpack-dir, not both")
        if unpack_dir is None and args.positional_unpack_dir is not None:
            unpack_dir = Path(args.positional_unpack_dir)

        if unpack_dir is None and args.prefix is None and args.cache_dir is None and args.archive is None:
            raise FetchError("at least one of --unpack-dir, --prefix, --cache-dir, or --archive is required")

        wasm = bool(args.wasm or args.target in ("wasm", "emscripten"))
        if args.jobs < 1:
            raise FetchError("--jobs must be a positive integer")

        effective_cache_dir = args.cache_dir
        if (
            effective_cache_dir is None
            and args.archive is None
            and unpack_dir is not None
            and (unpack_dir / pin["archive"]).is_file()
        ):
            effective_cache_dir = unpack_dir

        with tempfile.TemporaryDirectory(prefix="sp-libsodium-") as tmp_root_str:
            tmp_root = Path(tmp_root_str)
            archive_path = resolve_archive(
                pin=pin,
                cache_dir=effective_cache_dir,
                archive_override=args.archive,
                caller_sha256=caller_sha256,
                temp_dir=tmp_root,
                offline=args.offline,
            )

            if unpack_dir is not None or args.prefix is not None:
                effective_unpack_dir = unpack_dir if unpack_dir is not None else (tmp_root / "src")
                source_dir = safe_unpack_tarball(archive_path, effective_unpack_dir, pin["version"])
                if args.prefix is not None:
                    build_and_install(source_dir=source_dir, prefix=args.prefix, wasm=wasm, jobs=args.jobs)
    except FetchError as exc:
        print(f"fetch_libsodium.py: error: {exc}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
