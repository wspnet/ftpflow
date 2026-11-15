#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Reproduce the openssl jom hang outside vcpkg, with a hard timeout.

Runs the exact jom command vcpkg uses for openssl:x64-windows-static-ftpflow,
but foreground with a timeout, so we can see *where* it wedges and try
variations (job count, targets) quickly.
"""
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(r"C:\Users\wspne\source\repos\ftpflow")
sys.path.insert(0, str(REPO / "tools"))
import build_native as bn  # noqa: E402

VCPKG = Path(r"C:\Users\wspne\source\vcpkg")
JOM = VCPKG / "downloads" / "tools" / "jom" / "jom-1_1_7" / "jom.exe"
SRC = VCPKG / "buildtrees" / "openssl" / "src" / "nssl-3.6.5-c2a478abe6.clean"
DESTDIR = VCPKG / "packages" / "openssl_x64-windows-static-ftpflow"

msvc = bn.find_msvc()
sdk_ver, sdk_inc, sdk_lib, sdk_bin = bn.find_sdk()
# ninja dir not needed here; reuse build_env signature with a dummy
env = bn.build_env(msvc, sdk_ver, sdk_inc, sdk_lib, sdk_bin, Path(".").__str__())
env["DESTDIR"] = str(DESTDIR)

jobs = sys.argv[1] if len(sys.argv) > 1 else "29"
targets = sys.argv[2] if len(sys.argv) > 2 else \
    "install_dev install_modules install_runtime install_ssldirs"
timeout = int(sys.argv[3]) if len(sys.argv) > 3 else 120

if targets == "single-cl":
    # Control experiment: run one cl.exe compile manually (the exact
    # command that froze under jom), foreground with timeout.
    cl = str(msvc / "bin" / "Hostx64" / "x64" / "cl.exe")
    cmd = [cl, "/Zi", "/Fdossl_static.pdb", "/MT", "/Zl", "/GF", "/Gy",
           "-nologo", "-DWIN32", "-D_WINDOWS", "-utf-8", "-MP", "-MT",
           "-O2", "-Oi", "-Gy", "-DNDEBUG", "-Z7",
           "-I.", "-Iinclude", "-Iproviders/common/include",
           "-Iproviders/implementations/include",
           "-DL_ENDIAN", "-DOPENSSL_PIC",
           "-DOPENSSL_BUILDING_OPENSSL", "-DOPENSSL_SYS_WIN32",
           "-DWIN32_LEAN_AND_MEAN", "-DUNICODE", "-D_UNICODE",
           "-D_CRT_SECURE_NO_DEPRECATE", "-D_WINSOCK_DEPRECATED_NO_WARNINGS",
           "-DNDEBUG", "-DSTATIC_LEGACY",
           "-c", "/Focrypto/thread/arch/libcrypto-lib-thread_posix.obj",
           "crypto/thread/arch/thread_posix.c"]
else:
    cmd = [str(JOM), "/K", "/J", jobs, "/NOLOGO", "/G", "/U",
           "/F", "makefile"] + targets.split()
print("[repro] cwd:", SRC)
if targets == "single-cl":
    (SRC / "crypto" / "thread" / "arch").mkdir(parents=True, exist_ok=True)
print("[repro] cmd:", " ".join(cmd))
print(f"[repro] timeout={timeout}s  jobs={jobs}")
try:
    r = subprocess.run(cmd, cwd=str(SRC), env=env,
                       capture_output=True, text=True, timeout=timeout)
    print("[repro] FINISHED rc =", r.returncode)
    print("--- stdout tail ---")
    print("\n".join(r.stdout.splitlines()[-25:]))
    print("--- stderr tail ---")
    print("\n".join(r.stderr.splitlines()[-25:]))
except subprocess.TimeoutExpired as e:
    print("[repro] HUNG: timeout after", timeout, "s")
    out = (e.stdout or b"").decode(errors="replace")
    err = (e.stderr or b"").decode(errors="replace")
    print("--- partial stdout tail ---")
    print("\n".join(out.splitlines()[-15:]) if out else "(none)")
    print("--- partial stderr tail ---")
    print("\n".join(err.splitlines()[-15:]) if err else "(none)")
