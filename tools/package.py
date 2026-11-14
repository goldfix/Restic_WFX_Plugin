#!/usr/bin/env python3
"""Creates the Total Commander installation package of the Restic WFX plugin.

The package carries both architectures, as the WFX SDK requires for a plugin
available for 32-bit and 64-bit Total Commander: `restic.wfx` (32-bit) and
`restic.wfx64` (64-bit) share the base name and live in the same folder, and
`pluginst.inf` registers the base name `restic.wfx`. A 64-bit Total Commander
picks `restic.wfx64` automatically, a 32-bit one loads `restic.wfx`.

Usage (from anywhere; the repository root is detected automatically):
    python tools/package.py

Preconditions: `build\\x64\\restic.wfx64` and `build\\x86\\restic.wfx` exist, i.e.
`./build.sh package` (Release build + tests of both architectures) has run.
The script validates the staged files and the PE machine type of both DLLs,
then writes `build/wfx_restic_<version>.zip` plus its `.sha256`.
"""
from __future__ import annotations

import hashlib
import re
import struct
import sys
import zipfile
from pathlib import Path
from typing import NoReturn

ROOT = Path(__file__).resolve().parents[1]
VERSION_H = ROOT / "include" / "version.h"
PLUGINST = ROOT / "pluginst.inf"
BUILD_DIR = ROOT / "build"

# DLLs to package: target architecture -> (staged file, expected PE machine)
DLLS = {
    "x86": (BUILD_DIR / "x86" / "restic.wfx", 0x014C),
    "x64": (BUILD_DIR / "x64" / "restic.wfx64", 0x8664),
}
PE_MACHINE_NAMES = {0x014C: "x86 (PE32)", 0x8664: "x64 (PE32+)", 0xAA64: "arm64 (PE32+)"}

# Files copied to the ZIP root: Total Commander only auto-installs an archive
# whose pluginst.inf sits in the root, and it extracts everything else next to it.
EXTRA_FILES = [("README.md", ROOT / "README.md"), ("LICENSE", ROOT / "LICENSE")]


def fail(message: str) -> NoReturn:
    print(f"[PACKAGE] ERROR: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_version() -> str:
    match = re.search(r'RESTIC_WFX_PRODUCT_VER\s+"([^"]+)"', VERSION_H.read_text(encoding="utf-8"))
    if not match:
        fail(f"RESTIC_WFX_PRODUCT_VER not found in {VERSION_H}")
    return match.group(1)


def pe_machine(path: Path) -> int:
    """Returns IMAGE_FILE_HEADER.Machine of a PE file without loading it."""
    with path.open("rb") as handle:
        if handle.read(2) != b"MZ":
            fail(f"{path} is not a PE file (no MZ header)")
        handle.seek(0x3C)
        offset = struct.unpack("<I", handle.read(4))[0]
        handle.seek(offset)
        if handle.read(4) != b"PE\0\0":
            fail(f"{path} is not a PE file (no PE signature)")
        return struct.unpack("<H", handle.read(2))[0]


def check_pluginst(version: str) -> None:
    text = PLUGINST.read_text(encoding="utf-8").replace("\r\n", "\n")
    # `file=` names the 32-bit DLL; the 64-bit build is found by appending 64 to the extension
    for needle in ("[plugininstall]", "type=wfx", "file=restic.wfx", "defaultdir=restic", f"version={version}"):
        if needle not in text:
            fail(f"pluginst.inf lacks '{needle}'")
    if "file=restic.wfx64" in text:
        fail("pluginst.inf must register the base name restic.wfx, not restic.wfx64")


def check_staging(version: str) -> None:
    """Every architecture directory must carry the files that go next to the DLL."""
    for arch, (dll, machine) in DLLS.items():
        if not dll.is_file():
            fail(f"{dll} not found: run ./build.sh package first")
        found = pe_machine(dll)
        if found != machine:
            fail(f"{dll.name} is {PE_MACHINE_NAMES.get(found, hex(found))}, expected {PE_MACHINE_NAMES[machine]}")
        stage = dll.parent
        for name in ("pluginst.inf", "restic_plugin.ini"):
            staged = stage / name
            if not staged.is_file():
                fail(f"{staged} not found: the Release build did not stage it")
        if "file=restic.wfx" not in (stage / "pluginst.inf").read_text(encoding="utf-8"):
            fail(f"{stage / 'pluginst.inf'} does not register restic.wfx")
        if f"version={version}" not in (stage / "pluginst.inf").read_text(encoding="utf-8"):
            fail(f"{stage / 'pluginst.inf'} does not carry version {version}")


def build_zip(version: str) -> Path:
    zip_path = BUILD_DIR / f"wfx_restic_{version}.zip"
    # pluginst.inf, the two DLLs, the INI template and the documents all sit in the ZIP root
    entries = [("pluginst.inf", BUILD_DIR / "x64" / "pluginst.inf")]
    entries += [(dll.name, dll) for dll, _ in DLLS.values()]
    entries += [("restic_plugin.ini", BUILD_DIR / "x64" / "restic_plugin.ini")]
    entries += EXTRA_FILES

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
        for arcname, source in entries:
            archive.write(source, arcname)

    with zipfile.ZipFile(zip_path) as archive:
        if archive.testzip() is not None:
            fail(f"{zip_path.name} failed the integrity check")
        if len(archive.namelist()) != len(entries):
            fail(f"{zip_path.name} contains {len(archive.namelist())} files, expected {len(entries)}")
    return zip_path


def main() -> int:
    version = read_version()
    check_pluginst(version)
    check_staging(version)
    zip_path = build_zip(version)

    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    (zip_path.parent / (zip_path.name + ".sha256")).write_text(f"{digest} *{zip_path.name}\n", encoding="ascii")
    print(f"[PACKAGE] {zip_path.relative_to(ROOT)} ({zip_path.stat().st_size} bytes, "
          f"x86 + x64, version {version})")
    print(f"[PACKAGE] SHA-256 {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
