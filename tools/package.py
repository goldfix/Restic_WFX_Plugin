#!/usr/bin/env python3
"""Creates the Total Commander installation package of the Restic WFX plugin.

ONE ZIP carries both architectures, as the WFX SDK ("64-bit Support") describes: the 32-bit plugin is
`restic.wfx`, the 64-bit one has the same base name with `64` appended to the extension, `restic.wfx64`, and
both sit in the same folder. `pluginst.inf` registers the base name `restic.wfx`; a 64-bit Total Commander
loads `restic.wfx64` automatically when it exists, a 32-bit one loads `restic.wfx`.

    wfx_restic_<version>.zip:  pluginst.inf (file=restic.wfx), restic.wfx, restic.wfx64,
                               restic_plugin.ini.sample, README.md, LICENSE

`restic_plugin.ini` itself is deliberately NOT packaged: Total Commander extracts the archive over the existing
plugin folder on an upgrade, and a shipped INI would overwrite the repositories the user configured. The
commented sample travels as `restic_plugin.ini.sample`, a name the plugin never reads, so it can be extracted
over and over without touching the real configuration (the user copies it to `restic_plugin.ini` once).

Usage (from anywhere; the repository root is detected automatically):
    python tools/package.py

Preconditions: `build\\x64\\restic.wfx64` and `build\\x86\\restic.wfx` exist, each with the staged `pluginst.inf`
and `restic_plugin.ini`, i.e. `./build.sh package` (Release build + tests of both architectures) has run.
The script validates the staged files and the PE machine type of both DLLs, then writes
`build/wfx_restic_<version>.zip` plus its `.sha256`.
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
BUILD_DIR = ROOT / "build"

# architecture -> (staged DLL, expected PE machine)
DLLS = {
    "x86": (BUILD_DIR / "x86" / "restic.wfx", 0x014C),
    "x64": (BUILD_DIR / "x64" / "restic.wfx64", 0x8664),
}
PE_MACHINE_NAMES = {0x014C: "x86 (PE32)", 0x8664: "x64 (PE32+)", 0xAA64: "arm64 (PE32+)"}

# `file=` must name the 32-bit DLL (the base name): the 64-bit one is found by appending 64 to the extension
MANIFEST_LINES = ("[plugininstall]", "type=wfx", "file=restic.wfx", "defaultdir=restic")
# The sample configuration is staged next to every DLL; it travels under a name the plugin never reads
SAMPLE_INI = ("restic_plugin.ini.sample", "restic_plugin.ini")
# Documents copied to the ZIP root next to pluginst.inf (Total Commander only auto-installs an archive
# whose pluginst.inf sits in the root, and it extracts everything else beside it).
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


def check_manifest(path: Path, version: str) -> None:
    """The manifest must register the base name, once: no restic.wfx64 line, no second file=."""
    lines = path.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n")
    for needle in MANIFEST_LINES + (f"version={version}",):
        if needle not in lines:
            fail(f"{path} lacks the line '{needle}'")
    files = [line for line in lines if line.startswith("file=")]
    if files != ["file=restic.wfx"]:
        fail(f"{path} must register only the base name restic.wfx, found {files}")


def check_staging(version: str) -> None:
    """Every architecture folder must carry a valid DLL and the files that go next to it."""
    for arch, (dll, machine) in DLLS.items():
        if not dll.is_file():
            fail(f"{dll} not found: run ./build.sh package first")
        found = pe_machine(dll)
        if found != machine:
            fail(f"{dll.name} is {PE_MACHINE_NAMES.get(found, hex(found))}, expected {PE_MACHINE_NAMES[machine]}")
        for name in ("pluginst.inf", SAMPLE_INI[1]):
            if not (dll.parent / name).is_file():
                fail(f"{dll.parent / name} not found: the Release build did not stage it")
        check_manifest(dll.parent / "pluginst.inf", version)
    if (BUILD_DIR / "x86" / SAMPLE_INI[1]).read_bytes() != (BUILD_DIR / "x64" / SAMPLE_INI[1]).read_bytes():
        fail("the staged sample configurations of x86 and x64 differ")


def build_zip(version: str) -> Path:
    zip_path = BUILD_DIR / f"wfx_restic_{version}.zip"
    x64_stage = BUILD_DIR / "x64"
    entries = [("pluginst.inf", x64_stage / "pluginst.inf")]
    entries += [(dll.name, dll) for dll, _ in DLLS.values()]
    entries += [(SAMPLE_INI[0], x64_stage / SAMPLE_INI[1])]
    entries += EXTRA_FILES

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
        for arcname, source in entries:
            archive.write(source, arcname)

    with zipfile.ZipFile(zip_path) as archive:
        if archive.testzip() is not None:
            fail(f"{zip_path.name} failed the integrity check")
        if sorted(archive.namelist()) != sorted(name for name, _ in entries):
            fail(f"{zip_path.name} has unexpected contents: {archive.namelist()}")
    return zip_path


def main() -> int:
    version = read_version()
    check_staging(version)

    # Packages of another version (or of an older naming scheme) would be confused with the new one
    for old in BUILD_DIR.glob("wfx_restic_*"):
        if old.name not in (f"wfx_restic_{version}.zip", f"wfx_restic_{version}.zip.sha256"):
            old.unlink()

    zip_path = build_zip(version)
    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    (zip_path.parent / (zip_path.name + ".sha256")).write_text(f"{digest} *{zip_path.name}\n", encoding="ascii")
    print(f"[PACKAGE] {zip_path.relative_to(ROOT)} ({zip_path.stat().st_size} bytes, x86 + x64, version {version})")
    print(f"[PACKAGE] SHA-256 {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
