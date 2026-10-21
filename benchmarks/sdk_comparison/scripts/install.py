#!/usr/bin/env python3
"""Install the contenders into one directory from the pins and the hash-locked requirement files.

  install.py --install DIR [--only id,id,...]

Python contenders: the pinned commit is built into a wheel with a hash-locked builder environment, then installed with
`pip install --require-hashes --no-deps` next to its hash-locked dependencies, in a virtual environment of its own.
The environment passed to every step carries no credentials. Nothing here talks to a model provider.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import HARNESS, PINS, clean_env, git_checkout, installed_record, run, sha256_file  # noqa: E402

LOCKS = HARNESS / "locks"
PY = sys.executable


def venv(path: Path) -> Path:
    if path.exists():
        shutil.rmtree(path)
    run([PY, "-m", "venv", path])
    return path / "bin" / "python"


def pip_lock(python: Path, lock: Path, extra: list[str] | None = None) -> None:
    run([python, "-m", "pip", "install", "--no-cache-dir", "--disable-pip-version-check", "--require-hashes", "--no-deps",
         "--only-binary=:all:", "-r", lock] + (extra or []))


def site_report(python: Path) -> dict:
    code = r"""
import json, sys, importlib.metadata as m, site, pathlib
sp = pathlib.Path(site.getsitepackages()[0])
dists = sorted((d.metadata['Name'], d.version) for d in m.distributions())
print(json.dumps({'python': sys.version.split()[0], 'site_packages': str(sp), 'packages': [f'{n}=={v}' for n, v in dists],
                  'pth_files': sorted(p.name for p in sp.glob('*.pth'))}))
"""
    out = run([python, "-I", "-c", code], capture=True).stdout
    return json.loads(out.strip().splitlines()[-1])


def check_pth(report: dict) -> None:
    allowed = set((LOCKS / "pth-allowlist.txt").read_text().split()) if (LOCKS / "pth-allowlist.txt").exists() else set()
    unknown = [p for p in report["pth_files"] if p not in allowed]
    if unknown or "litellm_init.pth" in report["pth_files"]:
        raise SystemExit(f"unexpected .pth files in site-packages: {unknown}")


def install_pypi(install: Path, name: str, pin: dict) -> None:
    """A published release: download the wheel for this platform with its locked hash, then install the lock."""
    advisory = None
    if pin["pypi"] == "litellm":  # refuse a release that a stored security advisory lists as affected
        advisory = run([PY, HARNESS / "scripts" / "litellm_advisories.py", "check", "--version", pin["version"]], capture=True).stdout.strip()
    env = install / f"venv-{name}"
    py = venv(env)
    run([py, "-m", "pip", "install", "--no-cache-dir", "--disable-pip-version-check", "--require-hashes", "--no-deps",
         "--only-binary=:all:", "-r", LOCKS / f"{name}.txt"])
    # Record the wheel that was installed: download it again by name and hash from the same lock.
    wheels = install / "wheels"
    wheels.mkdir(exist_ok=True)
    spec = f"{pin['pypi']}=={pin['version']}"
    lock_text = (LOCKS / f"{name}.txt").read_text()
    block = re.search(rf"^{re.escape(pin['pypi'])}=={re.escape(pin['version'])} \\\n((?:    --hash=sha256:[0-9a-f]+(?: \\)?\n)+)", lock_text, re.M)
    hashes = re.findall(r"--hash=sha256:([0-9a-f]+)", block.group(1))
    one = wheels / f"{name}.requirements.txt"
    one.write_text(f"{spec} \\\n" + " \\\n".join(f"    --hash=sha256:{h}" for h in hashes) + "\n")
    run([py, "-m", "pip", "download", "--no-cache-dir", "--disable-pip-version-check", "--require-hashes", "--no-deps",
         "--only-binary=:all:", "-r", one, "-d", wheels])
    wheel = next(wheels.glob(f"{pin['pypi'].replace('-', '_')}-{pin['version']}-*.whl"))
    meta = json.loads(urllib.request.urlopen(f"https://pypi.org/pypi/{pin['pypi']}/{pin['version']}/json", timeout=60).read())
    published = {f["filename"]: f["digests"]["sha256"] for f in meta["urls"]}
    if published.get(wheel.name) != sha256_file(wheel):
        raise SystemExit(f"{wheel.name}: sha256 differs from the PyPI JSON record")
    report = site_report(py)
    check_pth(report)
    installed_record(install, name, {
        "id": name, "kind": "pypi-release", "package": pin["pypi"], "version": pin["version"],
        "wheel": wheel.name, "wheel_sha256": sha256_file(wheel), "pypi_json_checked": True,
        "head_not_used_because": pin.get("head_not_used_because"), "advisory_check": advisory, "packages": report["packages"],
        "package_count": len(report["packages"]), "pth_files": report["pth_files"], "python": report["python"],
        "lock": f"locks/{name}.txt", "lock_sha256": sha256_file(LOCKS / f"{name}.txt"),
        "installed_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    })


def build_wheel(install: Path, name: str, pin: dict) -> Path:
    src = install / "src" / name
    git_checkout(pin["repo"], pin["sha"], src)
    builder = install / f"build-{pin['builder']}"
    bpy = venv(builder)
    pip_lock(bpy, LOCKS / f"build-{pin['builder']}.txt")
    wheels = install / "wheels"
    wheels.mkdir(exist_ok=True)
    for old in wheels.glob(f"*{name.replace('-', '_')}*.whl"):
        old.unlink()
    run([bpy, "-m", "pip", "wheel", "--no-cache-dir", "--no-deps", "--no-build-isolation", "-w", wheels, src])
    candidates = sorted(wheels.glob("*.whl"), key=lambda p: p.stat().st_mtime)
    wheel = candidates[-1]
    shutil.rmtree(builder)
    return wheel


def install_git(install: Path, name: str, pin: dict) -> None:
    wheel = build_wheel(install, name, pin)
    env = install / f"venv-{name}"
    py = venv(env)
    pip_lock(py, LOCKS / f"{name}.txt")
    run([py, "-m", "pip", "install", "--no-cache-dir", "--disable-pip-version-check", "--no-deps", "--no-index", wheel])
    report = site_report(py)
    check_pth(report)
    installed_record(install, name, {
        "id": name, "kind": "git-head", "repo": pin["repo"], "sha": pin["sha"], "default_branch": pin["default_branch"],
        "wheel": wheel.name, "wheel_sha256": sha256_file(wheel), "packages": report["packages"],
        "package_count": len(report["packages"]), "pth_files": report["pth_files"], "python": report["python"],
        "lock": f"locks/{name}.txt", "lock_sha256": sha256_file(LOCKS / f"{name}.txt"),
        "installed_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    })


def install_node(install: Path) -> None:
    """The Node release named in pins.json, from nodejs.org, checked against the release's SHASUMS256.txt."""
    import platform
    import tarfile

    version = PINS["node"]["version"]
    arch = {"x86_64": "x64", "aarch64": "arm64"}[platform.machine()]
    name = f"node-v{version}-linux-{arch}"
    base = f"https://nodejs.org/dist/v{version}"
    sums = urllib.request.urlopen(f"{base}/SHASUMS256.txt", timeout=60).read().decode()
    expected = next(l.split()[0] for l in sums.splitlines() if l.endswith(f"{name}.tar.xz"))
    archive = install / "downloads" / f"{name}.tar.xz"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        archive.write_bytes(urllib.request.urlopen(f"{base}/{name}.tar.xz", timeout=600).read())
    if sha256_file(archive) != expected:
        raise SystemExit(f"{archive}: sha256 differs from SHASUMS256.txt")
    target = install / "node"
    if target.exists():
        shutil.rmtree(target)
    with tarfile.open(archive) as tar:
        tar.extractall(install / "downloads", filter="data")
    shutil.move(str(install / "downloads" / name), target)
    installed_record(install, "node", {"id": "node", "version": version, "archive": archive.name, "archive_sha256": expected})


def install_js(install: Path) -> None:
    """The npm packages pinned in clients/js/package-lock.json (integrity-checked by `npm ci`), without running install scripts."""
    pin = PINS["javascript"]["vercel-ai"]
    target = install / "js"
    if target.exists():
        shutil.rmtree(target)
    target.mkdir()
    for name in ("package.json", "package-lock.json", "harness.mjs", "c_vercel_ai.mjs"):
        shutil.copy2(HARNESS / "clients" / "js" / name, target / name)
    node_bin = install / "node" / "bin"
    env = clean_env({"PATH": f"{node_bin}:/usr/bin:/bin", "npm_config_cache": str(install / "npm-cache"), "npm_config_update_notifier": "false"})
    run([node_bin / "npm", "ci", "--ignore-scripts", "--no-audit", "--no-fund"], cwd=target, env=env)
    lock = json.loads((target / "package-lock.json").read_text())
    packages = sorted(f"{k.split('node_modules/')[-1]}@{v['version']}" for k, v in lock["packages"].items() if k)
    versions = {n: json.loads((target / "node_modules" / n / "package.json").read_text())["version"] for n in pin["npm"]}
    if versions != pin["npm"]:
        raise SystemExit(f"installed {versions}, pinned {pin['npm']}")
    installed_record(install, "vercel-ai", {
        "id": "vercel-ai", "kind": "npm-release-of-head", "repo": pin["repo"], "sha": pin["sha"], "default_branch": pin["default_branch"],
        "npm": versions, "head_not_built_because": pin["head_not_built_because"], "packages": packages, "package_count": len(packages),
        "lock": "clients/js/package-lock.json", "lock_sha256": sha256_file(HARNESS / "clients" / "js" / "package-lock.json"),
        "install_scripts": "not run (--ignore-scripts)", "installed_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    })


def locked(argv: list) -> list:
    """Wrap a compile in `flock $SDKB_BUILD_LOCK` when a host shares its CPUs between builders."""
    lock = os.environ.get("SDKB_BUILD_LOCK")
    return ["flock", lock, *argv] if lock else list(argv)


def revision() -> dict:
    """The commit of the tree being installed: from git, else from the .revision file that the copy step writes."""
    top = HARNESS.parent.parent
    head = run(["git", "-C", top, "rev-parse", "HEAD"], capture=True, check=False)
    if head.returncode == 0:
        dirty = bool(run(["git", "-C", top, "status", "--porcelain"], capture=True, check=False).stdout.strip())
        return {"commit": head.stdout.strip(), "dirty": dirty}
    file = HARNESS / ".revision"
    return json.loads(file.read_text()) if file.exists() else {"commit": "unknown", "dirty": None}


def cmake_build(install: Path, source: Path, build: Path, targets: list[str], defines: list[str], jobs: int) -> None:
    run(["cmake", "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Release"] + defines)
    run(locked(["cmake", "--build", build, "--target", *targets, "-j", str(jobs)]))


def native_dep(install: Path, name: str) -> Path:
    pin = PINS["native"][name]
    dest = install / "deps" / name
    git_checkout(pin["repo"], pin["sha"], dest)
    return dest


def compiler_info() -> dict:
    return {"cxx": run(["g++", "--version"], capture=True).stdout.splitlines()[0],
            "cmake": run(["cmake", "--version"], capture=True).stdout.splitlines()[0],
            "libcurl": run(["curl-config", "--version"], capture=True).stdout.strip()}


def libcurl_prefix(install: Path) -> Path:
    return install / f"libcurl-{PINS['native']['curl']['version']}"


def libcurl_facts(prefix: Path | None) -> dict:
    """Which libcurl a build uses: the version line of the `curl` that belongs to it, the TLS library and the HTTP/2 library."""
    if prefix is None:
        line = run(["curl", "--version"], capture=True).stdout.splitlines()[0]
        features = run(["curl-config", "--features"], capture=True).stdout.split()
        origin = "the distribution's libcurl"
    else:
        env = clean_env({"LD_LIBRARY_PATH": str(prefix / "lib")})
        line = run([prefix / "bin" / "curl", "--version"], capture=True, env=env).stdout.splitlines()[0]
        features = run([prefix / "bin" / "curl-config", "--features"], capture=True, env=env).stdout.split()
        origin = f"built from the official tarball into {prefix.name}"
    words = line.split()
    pick = lambda name: next((w for w in words if w.lower().startswith(name)), None)  # noqa: E731
    return {"origin": origin, "version_line": line, "libcurl": pick("libcurl/"), "tls": pick("openssl/"), "http2": pick("nghttp2/"), "features": features}


def install_null(install: Path, prefix: Path | None = None) -> None:
    suffix = "" if prefix is None else "_curl823"
    out = install / "bin" / f"null_client{suffix}"
    run(["sh", HARNESS / "clients" / "null_curl" / "build.sh", out], env=clean_env({"CURL_PREFIX": str(prefix)} if prefix else {}))
    installed_record(install, "null-curl" + ("" if prefix is None else "-curl823"), {
        "id": "null-curl", "kind": "built from this repository", "binary_sha256": sha256_file(out),
        "compiler": compiler_info(), "flags": "-O2 -std=gnu11", "libcurl_build": libcurl_facts(prefix)})


def download_verified(url: str, sha256: str, dest: Path) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if not dest.exists() or sha256_file(dest) != sha256:
        dest.write_bytes(urllib.request.urlopen(url, timeout=600).read())
    if sha256_file(dest) != sha256:
        raise SystemExit(f"{dest.name}: sha256 {sha256_file(dest)} differs from the published {sha256}")
    return dest


def install_libcurl(install: Path, jobs: int) -> None:
    """The newest libcurl, built into a private prefix from the official tarball (no root, no system packages).

    OpenSSL is the system's, as in the distribution build; HTTP/2 comes from an nghttp2 built into the same prefix, because
    the host has no development files for it. Compression libraries, PSL, IDN, LDAP, SSH and RTMP are left out: neither
    client asks for them."""
    import tarfile

    prefix = libcurl_prefix(install)
    curl, nghttp2 = PINS["native"]["curl"], PINS["native"]["nghttp2"]
    env = clean_env({"PATH": os.environ.get("PATH", "/usr/bin:/bin")})
    work = install / "deps" / "libcurl-build"
    work.mkdir(parents=True, exist_ok=True)
    archives = {}
    for name, pin in (("nghttp2", nghttp2), ("curl", curl)):
        archive = download_verified(pin["url"], pin["sha256"], install / "downloads" / pin["url"].rsplit("/", 1)[1])
        archives[name] = archive
        with tarfile.open(archive) as tar:
            tar.extractall(work, filter="data")
    nh_src, curl_src = work / f"nghttp2-{nghttp2['version']}", work / f"curl-{curl['version']}"
    run([nh_src / "configure", f"--prefix={prefix}", "--enable-lib-only", "--disable-shared", "--enable-static", "--with-pic"], cwd=nh_src, env=env)
    run(locked(["make", f"-j{jobs}", "install"]), cwd=nh_src, env=env)
    run([curl_src / "configure", f"--prefix={prefix}", "--with-openssl", f"--with-nghttp2={prefix}", "--without-zlib", "--without-brotli",
         "--without-zstd", "--without-libpsl", "--without-libidn2", "--without-librtmp", "--without-libssh", "--without-libssh2",
         "--disable-ldap", "--disable-ldaps", "--disable-docs", "--disable-manual", "--enable-shared", "--disable-static",
         f"LDFLAGS=-Wl,-rpath,{prefix}/lib"], cwd=curl_src, env=env)
    run(locked(["make", f"-j{jobs}", "install"]), cwd=curl_src, env=env)
    facts = libcurl_facts(prefix)
    if curl["version"] not in (facts["libcurl"] or ""):
        raise SystemExit(f"built {facts['libcurl']}, pinned {curl['version']}")
    installed_record(install, "libcurl-" + curl["version"], {
        "id": "libcurl", "version": curl["version"], "tarball_sha256": curl["sha256"], "nghttp2_version": nghttp2["version"],
        "nghttp2_tarball_sha256": nghttp2["sha256"], "libcurl_build": facts, "installed_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")})
    shutil.rmtree(work)


def install_sp(install: Path, jobs: int, prefix: Path | None = None) -> None:
    """SchemaProvider as it is in this checkout (the harness lives in the same repository), against the system libcurl or
    against the libcurl in `prefix`."""
    source = HARNESS.parent.parent
    asio, yyjson = native_dep(install, "asio") / "asio", native_dep(install, "yyjson")  # asio's repository keeps the headers in asio/include
    tag = "" if prefix is None else "-curl823"
    build = install / "build" / f"sp{tag}"
    curl_defines = [] if prefix is None else [f"-DCMAKE_PREFIX_PATH={prefix}", f"-DCURL_DIR={prefix}/lib/cmake/CURL", f"-DCURL_INCLUDE_DIR={prefix}/include",
                                              f"-DCURL_LIBRARY={prefix}/lib/libcurl.so", f"-DCMAKE_BUILD_RPATH={prefix}/lib"]
    cmake_build(install, source, build, ["sdkb_sp_client"],
                [f"-DASIO_ROOT={asio}", f"-DYYJSON_ROOT={yyjson}", "-DSP_BUILD_TESTS=OFF", "-DSP_BUILD_CANARY=OFF",
                 "-DSP_BUILD_SDK_COMPARISON=ON", "-DBUILD_SHARED_LIBS=OFF", f"-DSDKB_SP_REVISION={revision()['commit'][:12]}"] + curl_defines, jobs)
    out = install / "bin" / f"sp_client{tag.replace('-', '_')}"
    out.parent.mkdir(exist_ok=True)
    shutil.copy2(build / "benchmarks" / "sdk_comparison" / "clients" / "sp" / "sdkb_sp_client", out)
    commands = json.loads((build / "compile_commands.json").read_text())
    command = next(e["command"] for e in commands if e["file"].endswith("sdk_comparison/clients/sp/sp_client.cpp"))
    flags = " ".join(t for t in command.split() if t.startswith(("-O", "-D", "-std", "-f", "-m", "-g")) and not t.startswith("-DSDKB"))
    rev = revision()
    installed_record(install, "sp" + tag, {
        "id": "sp", "libcurl_build": libcurl_facts(prefix), "kind": "built from the harness's own checkout", "source_commit": rev["commit"], "source_dirty": rev["dirty"],
        "version": "0.4.0", "build_type": "Release", "compile_flags": flags, "compiler": compiler_info(),
        "asio": PINS["native"]["asio"]["tag"], "yyjson": PINS["native"]["yyjson"]["tag"], "binary_sha256": sha256_file(out),
        "installed_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    })


def install_ai_sdk_cpp(install: Path, jobs: int) -> None:
    pin = PINS["native"]["ai-sdk-cpp"]
    src = install / "src" / "ai-sdk-cpp"
    git_checkout(pin["repo"], pin["sha"], src)
    zlib = native_dep(install, "zlib")
    build = install / "build" / "ai-sdk-cpp"
    cmake_build(install, HARNESS / "clients" / "ai_sdk_cpp", build, ["sdkb_ai_sdk_cpp_client"],
                [f"-DAI_SDK_CPP_SOURCE={src}", f"-DZLIB_SOURCE={zlib}", f"-DAI_SDK_CPP_REVISION={pin['sha'][:12]}"], jobs)
    out = install / "bin" / "ai_sdk_cpp_client"
    out.parent.mkdir(exist_ok=True)
    shutil.copy2(build / "sdkb_ai_sdk_cpp_client", out)
    installed_record(install, "ai-sdk-cpp", {
        "id": "ai-sdk-cpp", "kind": "git-head", "repo": pin["repo"], "sha": pin["sha"], "default_branch": pin["default_branch"],
        "version": "0.1.0 (CMake project version)", "build_type": "Release", "compiler": compiler_info(),
        "zlib_commit": PINS["native"]["zlib"]["sha"], "binary_sha256": sha256_file(out),
        "http_library": "cpp-httplib " + run(["sed", "-n", "s/#define CPPHTTPLIB_VERSION \"\\(.*\\)\"/\\1/p", src / "third_party" / "httplib-header-only" / "httplib.h"], capture=True).stdout.strip(),
        "installed_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    })


def install_rust(install: Path) -> dict:
    """rustup into <install>/rust, with the toolchain version named in pins.json."""
    import platform

    rust = install / "rust"
    env = clean_env({"RUSTUP_HOME": str(rust / "rustup"), "CARGO_HOME": str(rust / "cargo")})
    if (rust / "cargo" / "bin" / "rustc").exists():
        return env
    triple = {"x86_64": "x86_64-unknown-linux-gnu", "aarch64": "aarch64-unknown-linux-gnu"}[platform.machine()]
    rust.mkdir(parents=True, exist_ok=True)
    base = f"https://static.rust-lang.org/rustup/dist/{triple}"
    init = rust / "rustup-init"
    init.write_bytes(urllib.request.urlopen(f"{base}/rustup-init", timeout=300).read())
    expected = urllib.request.urlopen(f"{base}/rustup-init.sha256", timeout=60).read().decode().split()[0]
    if sha256_file(init) != expected:
        raise SystemExit("rustup-init: sha256 differs from the published one")
    init.chmod(0o755)
    run([init, "-y", "--no-modify-path", "--profile", "minimal", "--default-toolchain", PINS["rust"]["version"]], env=env)
    return env


def install_genai(install: Path, jobs: int) -> None:
    pin = PINS["native"]["rust-genai"]
    env = install_rust(install)
    env["PATH"] = f"{install / 'rust' / 'cargo' / 'bin'}:{env['PATH']}"
    crate = HARNESS / "clients" / "rust_genai"
    target = install / "build" / "genai"
    run(locked(["cargo", "build", "--release", "--locked", "--jobs", str(jobs), "--target-dir", target]), cwd=crate, env=env)
    out = install / "bin" / "genai_client"
    out.parent.mkdir(exist_ok=True)
    shutil.copy2(target / "release" / "sdkb-genai-client", out)
    lock = (crate / "Cargo.lock").read_text()
    resolved = re.search(r'name = "genai"\nversion = "([^"]+)"\nsource = "git\+[^#]*#([0-9a-f]+)"', lock)
    if not resolved or resolved.group(2) != pin["sha"]:
        raise SystemExit(f"Cargo.lock resolves genai to {resolved.group(2) if resolved else None}, pinned {pin['sha']}")
    installed_record(install, "rust-genai", {
        "id": "rust-genai", "kind": "git-head", "repo": pin["repo"], "sha": pin["sha"], "default_branch": pin["default_branch"],
        "crate_version": resolved.group(1), "rustc": run(["rustc", "--version"], capture=True, env=env).stdout.strip(),
        "cargo_lock_sha256": sha256_file(crate / "Cargo.lock"), "crates": len(re.findall(r"^\[\[package\]\]", lock, re.M)),
        "binary_sha256": sha256_file(out), "build": "cargo build --release --locked", "installed_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    })


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", required=True)
    ap.add_argument("--only", help="comma-separated ids (default: everything)")
    ap.add_argument("--jobs", type=int, default=3, help="parallel compile jobs")
    args = ap.parse_args()
    install = Path(args.install).expanduser().resolve()
    steps = {
        "node": lambda: install_node(install),
        "null-curl": lambda: install_null(install),
        "sp": lambda: install_sp(install, args.jobs),
        "libcurl": lambda: install_libcurl(install, args.jobs),
        "null-curl-curl823": lambda: install_null(install, libcurl_prefix(install)),
        "sp-curl823": lambda: install_sp(install, args.jobs, libcurl_prefix(install)),
        "ai-sdk-cpp": lambda: install_ai_sdk_cpp(install, args.jobs),
        "rust-genai": lambda: install_genai(install, args.jobs),
        "vercel-ai": lambda: (install_node(install) if not (install / "node" / "bin" / "node").exists() else None, install_js(install)),
    }
    for name, pin in PINS["python"].items():
        steps[name] = (lambda n=name, p=pin: install_pypi(install, n, p) if "pypi" in p else install_git(install, n, p))
    for name in (args.only.split(",") if args.only else steps):
        if name not in steps:
            raise SystemExit(f"unknown id {name}; known: {', '.join(steps)}")
        steps[name]()
    return 0


if __name__ == "__main__":
    sys.exit(main())
