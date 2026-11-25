#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ftpflow -- one-key Windows installer packager.

Produces a proper Windows setup wizard (FTPFlow-Setup.exe) that lets the user
pick the install location, create Start Menu / Desktop shortcuts, and offers a
clean uninstaller. Built on Inno Setup; the app's Qt runtime is gathered by
windeployqt first.

The app itself is compiled with the static CRT (/MT) and Qt is bundled by
windeployqt, so end users need *no* Visual C++ redistributable and *no* Qt.

Usage:
    python tools/build_installer.py           # build the setup wizard
    python tools/build_installer.py --clean   # wipe dist/ first
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP_DIR = ROOT / "build" / "release-full" / "app"
DIST = ROOT / "dist"
STAGE = DIST / "ftpflow"
QT_BIN = Path(r"C:\Qt\6.12.0\msvc2022_64\bin")
WINDEPLOYQT = QT_BIN / "windeployqt.exe"
ICON = ROOT / "assets" / "ftpflow.ico"
ISS = ROOT / "tools" / "installer.iss"

APP_EXE = "ftpflow.exe"

# Inno Setup compiler -- searched in the usual install locations.
ISCC_CANDIDATES = [
    Path(r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe"),
    Path(r"C:\Program Files\Inno Setup 6\ISCC.exe"),
    Path(r"C:\Program Files (x86)\Inno Setup 7\ISCC.exe"),
    Path(r"C:\Program Files\Inno Setup 7\ISCC.exe"),
    Path.home() / "AppData" / "Local" / "Programs" / "Inno Setup 6" / "ISCC.exe",
    Path.home() / "AppData" / "Local" / "Programs" / "Inno Setup 7" / "ISCC.exe",
]

# Simplified-Chinese language file (downloads from the issrc release tag).
ZH_ISL_URL = ("https://raw.githubusercontent.com/jrsoftware/issrc/"
              "is-6_7_3/Files/Languages/Unofficial/ChineseSimplified.isl")


def die(msg: str) -> None:
    print("[build_installer] ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def run(cmd, **kw):
    print("  > " + " ".join(map(str, cmd)))
    subprocess.run(cmd, check=True, **kw)


def find_iscc() -> Path:
    for c in ISCC_CANDIDATES:
        if c.exists():
            return c
    die("Inno Setup 未安装。请先安装：winget install JRSoftware.InnoSetup")


def ensure_chinese(isl_dir: Path) -> None:
    target = isl_dir / "ChineseSimplified.isl"
    if target.exists():
        return
    print("  [语言] 下载简体中文语言文件 ...")
    try:
        import urllib.request
        urllib.request.urlretrieve(ZH_ISL_URL, target)
        print("  [语言] 中文语言文件已就绪")
    except Exception as e:  # pragma: no cover
        print(f"  [语言] 下载失败（{e}），安装界面将使用英文")


def stage_portable() -> None:
    """windeployqt the app into dist/ftpflow (also the source for the wizard)."""
    exe = APP_DIR / APP_EXE
    if not exe.exists():
        die(f"{exe} not found. Build first:  python tools/build_native.py --full")

    if STAGE.exists():
        shutil.rmtree(STAGE)
    STAGE.mkdir(parents=True, exist_ok=True)

    print("[1/3] 复制应用并收集 Qt 运行时 (windeployqt) ...")
    shutil.copy2(exe, STAGE / APP_EXE)
    if not WINDEPLOYQT.exists():
        die(f"windeployqt not found at {WINDEPLOYQT}")
    run([
        str(WINDEPLOYQT),
        "--release", "--no-translations", "--no-compiler-runtime",
        "--dir", str(STAGE),
        str(STAGE / APP_EXE),
    ])
    if ICON.exists():
        shutil.copy2(ICON, STAGE / "ftpflow.ico")


def build_setup(iscc: Path) -> None:
    print("[2/3] 编译图形安装向导 (Inno Setup) ...")
    ensure_chinese(iscc.parent / "Languages")
    run([str(iscc), str(ISS)])


def report() -> None:
    setup = DIST / "FTPFlow-Setup.exe"
    if not setup.exists():
        die("未找到产物 FTPFlow-Setup.exe")
    size = setup.stat().st_size / 1024 / 1024
    print("\n[3/3] 完成。")
    print(f"  安装程序 : {setup}")
    print(f"  大小     : {size:.1f} MB")
    print("  用户双击 FTPFlow-Setup.exe 即可选择安装位置、创建快捷方式。")
    print("  （dist/ftpflow/ 为免安装便携版，可直接运行。）")


def main() -> None:
    ap = argparse.ArgumentParser(description="Build the FTPFlow setup wizard.")
    ap.add_argument("--clean", action="store_true", help="wipe dist/ before packaging")
    args = ap.parse_args()

    if args.clean and DIST.exists():
        shutil.rmtree(DIST)

    stage_portable()
    iscc = find_iscc()
    build_setup(iscc)
    report()


if __name__ == "__main__":
    main()
