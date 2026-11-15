#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Workaround for flaky proxy egress: vcpkg's bundled curl dies on some GitHub
distfiles (HTTP/2 framing / 502 tunnels). This driver parses the failing
URL from the install log, fetches it with the system curl (retries), drops
it into <vcpkg>/downloads (vcpkg hash-validates it on the next run), and
re-runs the install. Loops until the install succeeds.

Usage: python tools/vcpkg_prefetch.py
"""
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VCPKG = Path(r"C:\Users\wspne\source\vcpkg\vcpkg.exe")
DOWNLOADS = VCPKG.parent / "downloads"
LOG = ROOT / ".vcpkg_install.log"
TRIPLET = "x64-windows-static-ftpflow"
OVERLAY = ROOT / "triplets"

# Reuse build_native.py's toolchain environment assembly (INCLUDE / LIB /
# PATH pointing at MSVC + Windows SDK incl. rc.exe / mt.exe) so vcpkg's
# child CMake runs can link -- vcvars64.bat is unusable here (reg.exe is
# blocked by security policy).
sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_native  # noqa: E402


def build_child_env():
    msvc = build_native.find_msvc()
    install_root = msvc.parents[3]
    sdk_ver, sdk_inc, sdk_lib, sdk_bin = build_native.find_sdk()
    _, ninja_dir = build_native.find_cmake_ninja(install_root)
    return build_native.build_env(msvc, sdk_ver, sdk_inc, sdk_lib, sdk_bin, ninja_dir)


ENV = build_child_env()
# vcpkg-tool builds child processes with a scrubbed environment; only
# variables listed here are passed through. Without the SDK/MSVC vars the
# port CMake runs cannot find rc.exe / mt.exe (CMAKE_MT-NOTFOUND).
ENV["VCPKG_ENV_PASSTHROUGH"] = (
    "INCLUDE;LIB;PATH;WindowsSDKDir;WindowsSDKVersion;"
    "UniversalCRTSdkDir;VCToolsInstallDir;VCINSTALLDir")


def run_install() -> int:
    with open(LOG, "w", encoding="utf-8") as f:
        return subprocess.call(
            [str(VCPKG), "install", "--triplet", TRIPLET,
             "--overlay-triplets", str(OVERLAY)],
            cwd=str(ROOT), stdout=f, stderr=subprocess.STDOUT, env=ENV)


def fetch(url: str, name: str) -> bool:
    dst = DOWNLOADS / name
    if dst.exists() and dst.stat().st_size > 0:
        print(f"[prefetch] cached: {name}")
        return True
    part = DOWNLOADS / (name + ".part")
    for attempt in range(10):
        r = subprocess.run(
            ["curl", "-sSL", "--retry", "3", "--connect-timeout", "20",
             "--max-time", "900", "-o", str(part), "-w", "%{http_code} %{size_download}",
             url],
            capture_output=True, text=True)
        code = (r.stdout or "").strip().split(" ")[0][-3:]
        size = part.stat().st_size if part.exists() else 0
        print(f"[prefetch] {name} attempt {attempt + 1}: http={code} bytes={size}")
        if size > 1000 and code in ("200", "226"):
            part.rename(dst)
            return True
        time.sleep(3)
    return False


def main() -> int:
    if not VCPKG.exists():
        print("[prefetch] vcpkg.exe not found", file=sys.stderr)
        return 1
    DOWNLOADS.mkdir(parents=True, exist_ok=True)

    for round_no in range(1, 11):
        print(f"[prefetch] === install round {round_no} ===")
        rc = run_install()
        if rc == 0:
            print("[prefetch] INSTALL OK")
            return 0
        text = LOG.read_text(encoding="utf-8", errors="replace")

        # Map URL -> target name from "Downloading <url> -> <name>" lines.
        targets = dict(re.findall(r"Downloading (\S+) -> (\S+)", text))
        failed = re.findall(r"won't retry download from (\S+)", text)
        failed += re.findall(r"Failed to fetch (?P<u>\S+)", text)
        # Fall back: any Downloading line whose file is absent.
        if not failed:
            for url, name in targets.items():
                if not (DOWNLOADS / name).exists():
                    failed.append(url)

        if not failed:
            print("[prefetch] no downloadable failures left; real build error, see log:")
            print(text[-2000:])
            return 1

        progressed = False
        for url in failed:
            name = targets.get(url)
            if not name:
                name = url.rstrip("/").split("/")[-1]
            print(f"[prefetch] fetching {url} -> {name}")
            if fetch(url, name):
                progressed = True

        if not progressed:
            print("[prefetch] could not prefetch any failed URL; giving up.")
            return 1

    print("[prefetch] exhausted rounds")
    return 1


if __name__ == "__main__":
    sys.exit(main())
