#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ftpflow one-key build (Windows / MSVC / Ninja).

Assembles the MSVC + Windows SDK environment directly from the filesystem
(no vcvars/registry dependency), then configures and builds with CMake +
Ninja. All targets are compiled with the static CRT (/MT) -- enforced in
the root CMakeLists.txt.

Usage:
  python tools/build_native.py                 # release, skeleton deps
  python tools/build_native.py --debug         # debug build
  python tools/build_native.py --full          # + libcurl/libssh2/zstd (vcpkg)
  python tools/build_native.py --tests         # build and run unit tests
  python tools/build_native.py --clean         # wipe build dir first
  python tools/build_native.py --no-gui        # skip the Qt desktop target
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VSWHERE = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
KITS10 = Path(r"C:\Program Files (x86)\Windows Kits\10")


def die(msg: str) -> None:
    print("[build_native] ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def find_msvc() -> Path:
    if not VSWHERE.exists():
        die("vswhere.exe not found. Install VS 2022 Build Tools with the "
            "'Desktop development with C++' workload.")
    install = subprocess.run(
        [str(VSWHERE), "-latest", "-products", "*",
         "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"],
        capture_output=True, text=True).stdout.strip()
    if not install:
        die("No MSVC toolchain found via vswhere.")
    msvc_root = Path(install) / "VC" / "Tools" / "MSVC"
    versions = sorted(msvc_root.glob("14.*"))
    if not versions:
        die(f"No MSVC 14.x toolset under {msvc_root}")
    return versions[-1]


def find_sdk() -> tuple:
    """Return (version, include_dir, lib_dir, bin_dir) for Win10 SDK."""
    if not KITS10.exists():
        die("Windows 10 SDK not found under Program Files (x86).")
    inc_root = KITS10 / "Include"
    versions = sorted(v for v in inc_root.iterdir() if v.is_dir())
    if not versions:
        die("No Windows SDK include versions found.")
    ver = versions[-1]
    include = inc_root / ver
    bin_dir = KITS10 / "bin" / ver.name / "x64"
    lib = KITS10 / "Lib" / ver.name
    for sub in ("ucrt", "um"):
        if not (include / sub).exists():
            die(f"SDK {ver.name} missing include/{sub}")
    if not (bin_dir / "rc.exe").exists():
        die(f"SDK {ver.name} missing x64 rc.exe")
    return ver, include, lib, bin_dir


def build_env(msvc: Path, sdk_ver, sdk_inc, sdk_lib, sdk_bin, ninja_dir):
    env = dict(os.environ)
    cl_bin = msvc / "bin" / "Hostx64" / "x64"
    include = [
        str(msvc / "include"),
        str(sdk_inc / "ucrt"),
        str(sdk_inc / "um"),
        str(sdk_inc / "shared"),
    ]
    lib = [
        str(msvc / "lib" / "x64"),
        str(sdk_lib / "ucrt" / "x64"),
        str(sdk_lib / "um" / "x64"),
    ]
    path = [str(cl_bin), str(sdk_bin)]
    if ninja_dir:
        path.append(ninja_dir)
    env["INCLUDE"] = ";".join(include)
    env["LIB"] = ";".join(lib)
    env["PATH"] = ";".join(path) + ";" + env.get("PATH", "")
    env.setdefault("Platform", "x64")
    # Variables vcvars64.bat would normally export. CMake (and vcpkg's
    # child CMake runs) use them to locate rc.exe / mt.exe; without them
    # compiler ABI test programs fail to link (CMAKE_MT-NOTFOUND).
    sdk_dir = str(Path(KITS10)) + "\\"
    env["WindowsSDKDir"] = sdk_dir
    env["UniversalCRTSdkDir"] = sdk_dir
    env["WindowsSDKVersion"] = sdk_ver.name + "\\"
    env["VCINSTALLDir"] = str(msvc.parents[2]) + "\\"
    env["VCToolsInstallDir"] = str(msvc) + "\\"
    return env


def find_cmake_ninja(msvc_install: Path):
    ext = msvc_install / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake"
    cmake = ext / "CMake" / "bin" / "cmake.exe"
    ninja_dir = ext / "Ninja"
    cmake = str(cmake) if cmake.exists() else shutil.which("cmake")
    if not cmake:
        die("cmake.exe not found (neither in VS nor on PATH).")
    return cmake, (str(ninja_dir) if ninja_dir.exists() else "")


def main() -> int:
    parser = argparse.ArgumentParser(description="ftpflow one-key build")
    parser.add_argument("--debug", action="store_true", help="Debug build")
    parser.add_argument("--full", action="store_true",
                        help="release-full preset (libcurl/libssh2/zstd)")
    parser.add_argument("--tests", action="store_true",
                        help="run unit tests after building")
    parser.add_argument("--clean", action="store_true",
                        help="delete the build directory first")
    parser.add_argument("--no-gui", action="store_true",
                        help="configure with FTPFLOW_BUILD_GUI=OFF")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--vcpkg-concurrency", type=int, default=1,
                        help="vcpkg dependency build parallelism "
                             "(VCPKG_MAX_CONCURRENCY); 1 = serial nmake, "
                             "skips jom which deadlocks on this machine")
    args = parser.parse_args()

    msvc = find_msvc()
    install_root = msvc.parents[3]           # .../VC/Tools/MSVC -> install
    sdk_ver, sdk_inc, sdk_lib, sdk_bin = find_sdk()
    cmake, ninja_dir = find_cmake_ninja(install_root)
    env = build_env(msvc, sdk_ver, sdk_inc, sdk_lib, sdk_bin, ninja_dir)

    preset = "release-full" if args.full else ("debug" if args.debug else "release")

    if args.full:
        vcpkg_root = ROOT.parent.parent / "vcpkg"
        if not (vcpkg_root / "vcpkg.exe").exists():
            die(f"--full needs vcpkg at {vcpkg_root} "
                "(git clone + bootstrap-vcpkg.bat), or set VCPKG_ROOT.")
        env["VCPKG_ROOT"] = str(vcpkg_root)
        env["VCPKG_DEFAULT_TRIPLET"] = "x64-windows-static-ftpflow"
        env["VCPKG_OVERLAY_TRIPLETS"] = str(ROOT / "triplets")
        # jom's parallel child coordination deadlocks on this machine
        # (cl.exe spawns, runs ~16ms, then blocks forever; three observed
        # freezes mid openssl build). vcpkg_build_nmake skips jom entirely
        # when VCPKG_CONCURRENCY == 1 and falls back to serial nmake --
        # slower but reliable. vcpkg-tool maps env VCPKG_MAX_CONCURRENCY
        # to the CMake variable VCPKG_CONCURRENCY.
        env["VCPKG_MAX_CONCURRENCY"] = str(args.vcpkg_concurrency)

    build_dir = ROOT / "build" / preset
    if args.clean and build_dir.exists():
        shutil.rmtree(build_dir)

    def run(step: list) -> int:
        print("[build_native] " + " ".join(step[1:4]))
        return subprocess.call(step, cwd=str(ROOT), env=env)

    configure = [cmake, "--preset", preset]
    if args.no_gui:
        configure += ["-DFTPFLOW_BUILD_GUI=OFF"]
    else:
        # Auto-detect a Qt install (aqt-style layout) and feed CMake.
        qt_candidates = [
            Path(r"C:\Qt\6.12.0\msvc2022_64"),
            Path(r"C:\Qt\6.11.3\msvc2022_64"),
        ]
        qt_root = os.environ.get("Qt6_DIR", "")
        if qt_root:
            qt_candidates.insert(0, Path(qt_root))
        for q in qt_candidates:
            if (q / "lib" / "cmake" / "Qt6").exists():
                configure.append(f"-DCMAKE_PREFIX_PATH={q}")
                print(f"[build_native] Qt6 detected at {q}")
                break
    rc = run(configure)
    if rc != 0 or args.configure_only:
        return rc

    rc = run([cmake, "--build", "--preset", preset])
    if rc != 0 or not args.tests:
        return rc

    return run([cmake, "--build", "--preset", preset, "--target", "run_tests"])


if __name__ == "__main__":
    sys.exit(main())
