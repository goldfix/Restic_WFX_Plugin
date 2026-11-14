#!/usr/bin/env python3
"""Creates the Total Commander installation packages of the Restic WFX plugin.

One ZIP per architecture, because each one carries its own `pluginst.inf`:

    wfx_restic_<version>_x64.zip   pluginst.inf (file=restic.wfx64), restic.wfx64, restic_plugin.ini.sample, README.md, LICENSE
    wfx_restic_<version>_x86.zip   pluginst.inf (file=restic.wfx),   restic.wfx,   restic_plugin.ini.sample, README.md, LICENSE

`restic_plugin.ini` itself is deliberately NOT packaged: Total Commander extracts the archive over the existing
plugin folder on an upgrade, and a shipped INI would overwrite the repositories the user configured. The
commented sample travels as `restic_plugin.ini.sample`, a name the plugin never reads, so it can be extracted over
and over without touching the real configuration (the user copies it to `restic_plugin.ini` once).

Usage (from anywhere; the repository root is detected automatically):
    python tools/package.py            # both architectures
    python tools/package.py x64        # only the listed architecture(s)

Preconditions: the Release DLL and the staged `pluginst.inf` of each requested architecture exist in
`build\\<arch>` (i.e. `./build.sh package` ran). The script validates the staged files and the PE machine type
of the DLL, then writes `build/wfx_restic_<version>_<arch>.zip` plus its `.sha256`.
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

# architecture -> DLL file name, expected PE machine, `file=` value of its pluginst.inf
ARCHES = {
    "x86": {"dll": "restic.wfx", "machine": 0x014C},
    "x64": {"dll": "restic.wfx64", "machine": 0x8664},
}
PE_MACHINE_NAMES = {0x014C: "x86 (PE32)", 0x8664: "x64 (PE32+)", 0xAA64: "arm64 (PE32+)"}

# Documents copied to the ZIP root next to pluginst.inf (Total Commander only auto-installs an archive
# whose pluginst.inf sits in the root, and it extracts everything else beside it).
EXTRA_FILES = [("README.md", ROOT / "README.md"), ("LICENSE", ROOT / "LICENSE")]
# The sample configuration (the file the build stages next to the DLL), shipped under a name that is never loaded
SAMPLE_INI = ("restic_plugin.ini.sample", "restic_plugin.ini")


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


def manifest_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n")


def check_manifest(path: Path, dll_name: str, version: str) -> None:
    """The manifest of an architecture must name exactly the DLL that its package carries."""
    lines = manifest_lines(path)
    for needle in ("[plugininstall]", "type=wfx", "defaultdir=restic", f"file={dll_name}", f"version={version}"):
        if needle not in lines:
            fail(f"{path} lacks the line '{needle}'")
    files = [line for line in lines if line.startswith("file=")]
    if files != [f"file={dll_name}"]:
        fail(f"{path} must register only {dll_name}, found {files}")


def check_staging(arch: str, version: str) -> tuple[Path, Path]:
    """Returns (DLL, manifest) of one architecture after validating them."""
    info = ARCHES[arch]
    stage = BUILD_DIR / arch
    dll = stage / info["dll"]
    manifest = stage / "pluginst.inf"
    if not dll.is_file():
        fail(f"{dll} not found: run ./build.sh package first")
    if not manifest.is_file():
        fail(f"{manifest} not found: the Release build did not stage it")
    found = pe_machine(dll)
    if found != info["machine"]:
        fail(f"{dll.name} is {PE_MACHINE_NAMES.get(found, hex(found))}, expected {PE_MACHINE_NAMES[info['machine']]}")
    check_manifest(manifest, info["dll"], version)
    return dll, manifest


def build_zip(arch: str, version: str) -> Path:
    dll, manifest = check_staging(arch, version)
    zip_path = BUILD_DIR / f"wfx_restic_{version}_{arch}.zip"
    sample = dll.parent / SAMPLE_INI[1]
    if not sample.is_file():
        fail(f"{sample} not found: the Release build did not stage it")
    entries = [("pluginst.inf", manifest), (dll.name, dll), (SAMPLE_INI[0], sample)] + EXTRA_FILES

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
        for arcname, source in entries:
            archive.write(source, arcname)

    with zipfile.ZipFile(zip_path) as archive:
        if archive.testzip() is not None:
            fail(f"{zip_path.name} failed the integrity check")
        if sorted(archive.namelist()) != sorted(name for name, _ in entries):
            fail(f"{zip_path.name} has unexpected contents: {archive.namelist()}")
    return zip_path


def main(argv: list[str]) -> int:
    arches = argv or ["x64", "x86"]
    for arch in arches:
        if arch not in ARCHES:
            fail(f"unknown architecture '{arch}' (expected one of {', '.join(ARCHES)})")

    version = read_version()
    # Packages of another version would be confused with the new ones
    for old in BUILD_DIR.glob("wfx_restic_*"):
        if not old.name.startswith(f"wfx_restic_{version}_"):
            old.unlink()

    for arch in arches:
        zip_path = build_zip(arch, version)
        digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
        (zip_path.parent / (zip_path.name + ".sha256")).write_text(f"{digest} *{zip_path.name}\n", encoding="ascii")
        print(f"[PACKAGE] {zip_path.relative_to(ROOT)} ({zip_path.stat().st_size} bytes, {arch}, version {version})")
        print(f"[PACKAGE] SHA-256 {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
