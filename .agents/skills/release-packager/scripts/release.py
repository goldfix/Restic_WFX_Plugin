#!/usr/bin/env python3
"""Release helper for the Restic WFX plugin.

Bumps every version reference in the repository, refreshes the documentation,
builds and tests the plugin (Debug, /analyze, AddressSanitizer, Release) and
creates the Total Commander installation ZIP (build/wfx_restic_<version>_x64.zip).

Usage (from anywhere; the repository root is detected automatically):
    python .agents/skills/release-packager/scripts/release.py 1.1.0
    python .agents/skills/release-packager/scripts/release.py 1.1.0 --dry-run
    python .agents/skills/release-packager/scripts/release.py 1.1.0 --skip-tests
    python .agents/skills/release-packager/scripts/release.py 1.1.0 --notes "Fixed X"

Requirements: an initialized MSVC x64 environment (cl, link, rc, nmake) and `bash`
(Git Bash / MSYS2) in PATH, exactly like ./build.sh.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]

VERSION_H = ROOT / "include" / "version.h"
PLUGINST = ROOT / "pluginst.inf"
PYPROJECT = ROOT / "pyproject.toml"
README = ROOT / "README.md"
AGENT_MD = ROOT / "AGENT.md"
MEMORY_MD = ROOT / "MEMORY.md"
BUILD_DIR = ROOT / "build"

# Files that must be in the ZIP root (Total Commander looks for pluginst.inf there)
ZIP_EXPECTED = {"pluginst.inf", "restic.wfx64", "restic_plugin.ini", "README.md", "LICENSE"}

# Build actions executed through build.sh, in order. `package` cleans, builds Release,
# runs the Release tests and creates the ZIP, so it must come last.
FULL_ACTIONS = ["test", "analyze", "asan", "package"]
QUICK_ACTIONS = ["package"]


# --------------------------------------------------------------------------- helpers
def read_text(path: Path) -> tuple[str, str]:
    """Returns (text normalized to LF, original newline sequence)."""
    raw = path.read_bytes().decode("utf-8")
    newline = "\r\n" if "\r\n" in raw else "\n"
    return raw.replace("\r\n", "\n"), newline


def write_text(path: Path, text: str, newline: str) -> None:
    path.write_bytes(text.replace("\n", newline).encode("utf-8"))


def log(msg: str) -> None:
    print(f"[release] {msg}", flush=True)


def parse_version(value: str) -> tuple[int, int, int]:
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", value.strip())
    if not match:
        raise SystemExit(f"Invalid version '{value}': expected MAJOR.MINOR.PATCH (e.g. 1.2.3)")
    parts = tuple(int(g) for g in match.groups())
    if any(p > 65535 for p in parts):
        raise SystemExit("Each version component must be <= 65535 (Windows VERSIONINFO limit)")
    return parts  # type: ignore[return-value]


def current_version() -> str:
    text, _ = read_text(VERSION_H)
    match = re.search(r'#define\s+RESTIC_WFX_PRODUCT_VER\s+"([^"]+)"', text)
    if not match:
        raise SystemExit("RESTIC_WFX_PRODUCT_VER not found in include/version.h")
    return match.group(1)


def sub_once(text: str, pattern: str, repl: str, what: str, flags: int = 0) -> str:
    """Regex replace that fails loudly when the pattern is missing."""
    new_text, count = re.subn(pattern, lambda _m: repl, text, count=1, flags=flags)
    if count != 1:
        raise SystemExit(f"Pattern for {what} not found: {pattern}")
    return new_text


# --------------------------------------------------------------------------- updaters
def update_version_h(major: int, minor: int, patch: int, ver: str) -> str:
    text, _ = read_text(VERSION_H)
    text = sub_once(text, r"(#define\s+RESTIC_WFX_VER_MAJOR\s+)\d+", f"#define RESTIC_WFX_VER_MAJOR      {major}", "VER_MAJOR")
    text = sub_once(text, r"(#define\s+RESTIC_WFX_VER_MINOR\s+)\d+", f"#define RESTIC_WFX_VER_MINOR      {minor}", "VER_MINOR")
    text = sub_once(text, r"(#define\s+RESTIC_WFX_VER_PATCH\s+)\d+", f"#define RESTIC_WFX_VER_PATCH      {patch}", "VER_PATCH")
    text = sub_once(text, r"#define\s+RESTIC_WFX_VER_BUILD\s+\d+", "#define RESTIC_WFX_VER_BUILD      0", "VER_BUILD")
    text = sub_once(text, r"#define\s+RESTIC_WFX_VERSION_CSV\s+[\d,]+", f"#define RESTIC_WFX_VERSION_CSV    {major},{minor},{patch},0", "VERSION_CSV")
    text = sub_once(text, r'#define\s+RESTIC_WFX_VERSION_STR\s+"[^"]*"', f'#define RESTIC_WFX_VERSION_STR    "{ver}.0"', "VERSION_STR")
    text = sub_once(text, r'#define\s+RESTIC_WFX_VERSION_WSTR\s+L"[^"]*"', f'#define RESTIC_WFX_VERSION_WSTR  L"{ver}.0"', "VERSION_WSTR")
    text = sub_once(text, r'#define\s+RESTIC_WFX_PRODUCT_VER\s+"[^"]*"', f'#define RESTIC_WFX_PRODUCT_VER    "{ver}"', "PRODUCT_VER")
    return text


def update_pluginst(ver: str) -> str:
    text, _ = read_text(PLUGINST)
    return sub_once(text, r"^version=.*$", f"version={ver}", "pluginst.inf version", re.MULTILINE)


def update_pyproject(ver: str) -> str:
    text, _ = read_text(PYPROJECT)
    # Only the [project] version (first `version = "..."` line), never dependency specifiers
    return sub_once(text, r'^version\s*=\s*"[^"]*"', f'version = "{ver}"', "pyproject version", re.MULTILINE)


def update_readme(ver: str) -> str:
    text, _ = read_text(README)
    return sub_once(
        text,
        r'img\.shields\.io/badge/Version-[^-"]+-success\.svg" alt="Version: [^"]*"',
        f'img.shields.io/badge/Version-{ver}-success.svg" alt="Version: {ver}"',
        "README version badge",
    )


def update_agent(ver: str) -> str:
    text, _ = read_text(AGENT_MD)
    text = sub_once(
        text,
        r"(plugin version \(`)[^`]*(` / `\")[^\"]*(\"`\))",
        f'plugin version (`{ver}.0` / `"{ver}"`)',
        "AGENT.md section 5.1 version",
    )
    text = sub_once(
        text,
        r"(version\.h\s+# Single source of truth for plugin version macros \()[^)]*(\))",
        f"version.h               # Single source of truth for plugin version macros ({ver})",
        "AGENT.md source tree version",
    )
    return text


def update_memory(ver: str, notes: str, mode: str) -> str:
    text, _ = read_text(MEMORY_MD)
    today = datetime.date.today().isoformat()

    release_line = f"- **Release version:** `{ver}` (single source of truth: `include/version.h`; bump with the `release-packager` skill)."
    if re.search(r"^- \*\*Release version:\*\* .*$", text, re.MULTILINE):
        text = sub_once(text, r"^- \*\*Release version:\*\* .*$", release_line, "MEMORY release line", re.MULTILINE)
    else:
        text = sub_once(text, r"^(- \*\*Conventions:\*\*)", release_line + "\n- **Conventions:**", "MEMORY conventions anchor", re.MULTILINE)

    # Keep the "Next" hint aligned (skipped silently when the wording was changed by hand)
    text = re.sub(r"Ready for `v[\d.]+` release tagging", f"Ready for `v{ver}` release tagging", text)

    heading = f"### {today} \u2014 Release v{ver} packaged (release-packager skill)"
    if heading not in text:
        entry = [
            "",
            heading,
            f"- **Objective:** Prepare release `v{ver}`.",
            "- **Changes:**",
            f"  - Version bumped to `{ver}` in `include/version.h`, `pluginst.inf`, `pyproject.toml`, `README.md` (badge), `AGENT.md` and `MEMORY.md`.",
        ]
        if mode == "full":
            entry.append("  - Debug tests, `/analyze`, AddressSanitizer and Release tests passed.")
        elif mode == "quick":
            entry.append("  - Release tests passed (`--skip-tests`: Debug, `/analyze` and ASan were not run).")
        if mode in ("full", "quick"):
            entry.append(f"  - `build/wfx_restic_{ver}_x64.zip` created and verified (ZIP contents, `pluginst.inf` version, DLL `VERSIONINFO`).")
        else:
            entry.append("  - Files and documentation only (`--no-build`): nothing was built, tested or packaged.")
        if notes:
            entry.append(f"  - Notes: {notes}")
        entry.append(f"- **Next step:** upload `build/wfx_restic_{ver}_x64.zip` to a GitHub Release tagged `v{ver}`.")
        text = text.rstrip("\n") + "\n" + "\n".join(entry) + "\n"
    return text


# --------------------------------------------------------------------------- build / verify
def run_build(actions: list[str]) -> None:
    bash = shutil.which("bash")
    if not bash:
        raise SystemExit("`bash` not found in PATH (Git Bash / MSYS2 is required, as for ./build.sh)")
    for action in actions:
        log(f"./build.sh {action}")
        result = subprocess.run([bash, "build.sh", action], cwd=ROOT)
        if result.returncode != 0:
            raise SystemExit(
                f"`./build.sh {action}` failed (exit {result.returncode}). "
                "Version files were already updated: fix the problem and re-run, or `git checkout` them."
            )


def read_dll_version(dll_path: Path) -> str:
    """Returns the FileVersion (a.b.c.d) stored in the VS_VERSION_INFO resource (Windows only)."""
    import ctypes
    from ctypes import wintypes

    version = ctypes.WinDLL("version", use_last_error=True)
    version.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    version.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    version.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    version.GetFileVersionInfoW.restype = wintypes.BOOL
    version.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    version.VerQueryValueW.restype = wintypes.BOOL

    size = version.GetFileVersionInfoSizeW(str(dll_path), None)
    if not size:
        raise SystemExit(f"No version resource found in {dll_path}")
    buf = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(str(dll_path), 0, size, buf):
        raise SystemExit(f"GetFileVersionInfoW failed for {dll_path}")
    ptr = ctypes.c_void_p()
    length = wintypes.UINT()
    if not version.VerQueryValueW(buf, "\\", ctypes.byref(ptr), ctypes.byref(length)):
        raise SystemExit("VerQueryValueW failed")

    class FixedFileInfo(ctypes.Structure):
        _fields_ = [(n, wintypes.DWORD) for n in (
            "dwSignature", "dwStrucVersion", "dwFileVersionMS", "dwFileVersionLS",
            "dwProductVersionMS", "dwProductVersionLS", "dwFileFlagsMask", "dwFileFlags",
            "dwFileOS", "dwFileType", "dwFileSubtype", "dwFileDateMS", "dwFileDateLS")]

    info = ctypes.cast(ptr, ctypes.POINTER(FixedFileInfo)).contents
    return f"{info.dwFileVersionMS >> 16}.{info.dwFileVersionMS & 0xFFFF}.{info.dwFileVersionLS >> 16}.{info.dwFileVersionLS & 0xFFFF}"


def verify_zip(ver: str) -> Path:
    zip_path = BUILD_DIR / f"wfx_restic_{ver}_x64.zip"
    if not zip_path.is_file():
        raise SystemExit(f"Expected package not found: {zip_path}")

    with zipfile.ZipFile(zip_path) as zf:
        names = set(zf.namelist())
        if names != ZIP_EXPECTED:
            raise SystemExit(f"Unexpected ZIP contents.\n  missing: {sorted(ZIP_EXPECTED - names)}\n  extra: {sorted(names - ZIP_EXPECTED)}")
        if zf.testzip() is not None:
            raise SystemExit("ZIP integrity check failed")

        inf = zf.read("pluginst.inf").decode("ascii", "replace")
        for needle in ("[plugininstall]", "type=wfx", "file=restic.wfx64", f"version={ver}"):
            if needle not in inf:
                raise SystemExit(f"pluginst.inf inside the ZIP lacks '{needle}'")

        with tempfile.TemporaryDirectory() as tmp:
            dll = Path(tmp) / "restic.wfx64"
            dll.write_bytes(zf.read("restic.wfx64"))
            dll_version = read_dll_version(dll)
    if dll_version != f"{ver}.0":
        raise SystemExit(f"DLL FileVersion is {dll_version}, expected {ver}.0")

    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    (zip_path.parent / (zip_path.name + ".sha256")).write_text(f"{digest} *{zip_path.name}\n", encoding="ascii")
    log(f"ZIP verified: {zip_path.name} ({zip_path.stat().st_size} bytes), DLL FileVersion {dll_version}")
    log(f"SHA-256: {digest}")
    return zip_path


# --------------------------------------------------------------------------- main
def main() -> int:
    parser = argparse.ArgumentParser(description="Bump version, update docs, build, test and package the Restic WFX plugin.")
    parser.add_argument("version", help="new version as MAJOR.MINOR.PATCH (e.g. 1.1.0)")
    parser.add_argument("--dry-run", action="store_true", help="show what would change without touching any file")
    parser.add_argument("--skip-tests", action="store_true", help="only run `./build.sh package` (Release build + Release tests + ZIP)")
    parser.add_argument("--no-build", action="store_true", help="update files/docs only; do not build, test or package")
    parser.add_argument("--notes", default="", help="free text added to the MEMORY.md session log entry")
    args = parser.parse_args()

    major, minor, patch = parse_version(args.version)
    ver = f"{major}.{minor}.{patch}"
    old = current_version()
    log(f"Repository: {ROOT}")
    log(f"Version: {old} -> {ver}")

    updates = {
        VERSION_H: (update_version_h(major, minor, patch, ver), read_text(VERSION_H)[1]),
        PLUGINST: (update_pluginst(ver), read_text(PLUGINST)[1]),
        PYPROJECT: (update_pyproject(ver), read_text(PYPROJECT)[1]),
        README: (update_readme(ver), read_text(README)[1]),
        AGENT_MD: (update_agent(ver), read_text(AGENT_MD)[1]),
    }
    mode = "nobuild" if args.no_build else ("quick" if args.skip_tests else "full")

    for path, (new_text, newline) in updates.items():
        changed = new_text != read_text(path)[0]
        log(f"{'would update' if args.dry_run else 'updating'}: {path.relative_to(ROOT)}{'' if changed else ' (already up to date)'}")
        if changed and not args.dry_run:
            write_text(path, new_text, newline)

    if args.dry_run:
        log("would update: MEMORY.md (release line + session log entry, written only after a successful build)")
        log("Dry run complete: no file was modified.")
        return 0

    def finish_memory() -> None:
        # The session log claims tests passed, so it is written only after the build succeeded
        log("updating: MEMORY.md")
        write_text(MEMORY_MD, update_memory(ver, args.notes, mode), read_text(MEMORY_MD)[1])

    if args.no_build:
        finish_memory()
        log("Files updated (--no-build): build, test and package skipped.")
        return 0

    run_build(QUICK_ACTIONS if args.skip_tests else FULL_ACTIONS)
    zip_path = verify_zip(ver)
    finish_memory()
    log(f"DONE. Upload {zip_path.relative_to(ROOT)} to a GitHub Release tagged v{ver}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
