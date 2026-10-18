# MEMORY.md — Project Memory

## Roadmap

- ✅ **Project Skeleton & WFX Exports**: Initial WFX entry points (`FsInitW`, `FsGetDefRootName`, `FsSetDefaultParams`, `FsFindFirstW`, `FsFindNextW`, `FsFindClose`) and mandatory ANSI stubs (`FsInit`, `FsFindFirst`, `FsFindNext`).
- ✅ **Build System & Compiler Hardening (`Makefile`, `build.sh`, `.vscode/tasks.json`, `.clang-format`)**:
  - Environment-agnostic incremental NMake `Makefile` + `build.sh` wrapper for MSVC x64 (`cl.exe`, `link.exe`, `nmake.exe`).
  - Strict pure C11 flags (`/TC /std:c11 /permissive- /W4 /WX /we4013`, strictly no C++), static CRT (`/MT` Release, `/MTd` Debug), binary hardening (`/GS`, `/DYNAMICBASE`, `/NXCOMPAT`), CRT Debug Heap leak checking (`<crtdbg.h>`), AddressSanitizer (`ASAN=1`), and MSVC static analysis (`ANALYZE=1`).
- ✅ **Configuration Management (`config.h` / `config.c`)**:
  - Primary (`<plugin_dir>\restic_plugin.ini`) and fallback (`DefaultIniName` directory from `FsSetDefaultParams`) INI path resolution with automatic default template creation.
  - Three-tier `restic.exe` binary resolution (`[Settings] ResticPath` -> `<plugin_dir>\restic.exe` -> system `PATH`).
  - `[Repositories]` section parsing, sanitization, case-insensitive deduplication, and Level 0 (`\`) virtual folder enumeration.
  - Automated unit test suite (`tests/test_config.c`, 39 assertions passing under Debug, `/analyze`, and AddressSanitizer).
- ✅ **cJSON Integration (`cJSON/cJSON.h`, `cJSON/cJSON.c`)**: Integrated `cJSON` library from `cJSON/` into `Makefile` (`/IcJSON`, compiled as pure C11 `/TC /std:c11`).
- 🔲 **Restic CLI Execution & Parsing (`restic_cli.h` / `restic_cli.c`)**:
  - Child process execution via `CreateProcessW` (`CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT`) and anonymous Win32 pipes.
  - Snapshot listing (`restic snapshots --json`) and directory listing (`restic ls <snapshot_id> --json` NDJSON).
- 🔲 **Virtual Path Navigation & Credential Session (`src/main.c`)**:
  - Level 1 (`\<RepoAlias>\`) and Level 2 (`\<RepoAlias>\<SnapshotDir>\<SubPath>`) navigation.
  - On-demand password prompt via `RequestProcW` (`RT_Password`) held in RAM for the active repository session.
- 🔲 **Final Documentation (`README.md`)**: User installation, configuration guide, and architecture diagrams.

---

## Architectural & Build Decisions

1. **Combined Unicode + Mandatory ANSI WFX Stubs**
   - Total Commander's plugin loader checks for mandatory base exports (`FsInit`, `FsFindFirst`, `FsFindNext`, `FsFindClose`) when loading a `.wfx` / `.wfx64` DLL, then invokes the `W` variants (`FsInitW`, `FsFindFirstW`, `FsFindNextW`) on NT/Unicode systems. Exporting lightweight ANSI stubs alongside the Unicode implementation ensures unconditional compatibility across all Total Commander versions.

2. **Two-Tier INI Resolution with Default Template Generation**
   - When the plugin is installed in a read-only or protected directory (e.g., `C:\Program Files\TotalCmd\plugins\...`) without a local `restic_plugin.ini`, it falls back to Total Commander's user configuration directory provided via `FsSetDefaultParams` (`DefaultIniName`, typically `%APPDATA%\GHISLER\`) and automatically generates a commented `restic_plugin.ini` template if missing.

3. **Explicit Directory Setters for Unit Testing**
   - `config_set_plugin_dir()` and `config_set_fallback_dir()` decouple configuration logic from `DllMain` (`HMODULE`) and Total Commander runtime state, allowing deterministic unit testing in `tests/test_config.c`.

4. **Environment-Agnostic NMake `Makefile` + `build.sh` Wrapper & Static CRT (`/MT` / `/MTd`)**
   - The repository contains zero hardcoded machine paths (no hardcoded batch setup scripts or absolute compiler paths). Anyone cloning the repo simply runs `./build.sh` or `nmake` from an initialized MSVC x64 environment.
   - Static CRT linking (`/MT` in Release, `/MTd` in Debug) embeds the C runtime into `restic.wfx64`, eliminating any dependency on external `VCRUNTIME140.dll` redistributables on target machines.

5. **Dual Memory Safety Verification (CRT Debug Heap + AddressSanitizer)**
   - In standard Debug builds, `_CRTDBG_MAP_ALLOC` and `_CrtCheckMemory()` verify heap integrity and report any memory leaks directly to `stderr`.
   - When compiled with `ASAN=1` (`__SANITIZE_ADDRESS__`), `_CRTDBG_MAP_ALLOC` is automatically disabled so MSVC AddressSanitizer can instrument allocations without macro conflicts.

---

## Common Pitfalls & Solutions

- **Win32 `GetPrivateProfile*W` Relative Path Redirection**:
  - *Issue:* Windows `GetPrivateProfileStringW` and `GetPrivateProfileSectionW` search inside `%WINDIR%` (`C:\Windows`) when passed a relative filename without path separators.
  - *Solution:* Always normalize `ini_path` with `GetFullPathNameW` before calling any `GetPrivateProfile*W` API.

- **Win32 `GetPrivateProfileSectionW` Buffer Truncation**:
  - *Issue:* Fixed-size buffers silently truncate large `[Repositories]` sections at `size - 2` characters.
  - *Solution:* `config_load_from_file` dynamically expands the section buffer (starting at 16 KB WCHARs up to 256 KB) whenever `chars_read == buf_cap - 2`.

- **NMake Line Continuation After Trailing Backslash**:
  - *Issue:* Writing `/Fd$(BUILD_DIR)\` at the end of a line in an NMake `Makefile` treats the trailing `\` as a line-continuation character, swallowing the path separator.
  - *Solution:* Always specify an explicit filename such as `/Fd$(BUILD_DIR)\vc.pdb` when placing compiler PDB flags in NMake variables.

- **MSYS2 / Git Bash `/usr/bin/link.exe` Collision & POSIX Drive Prefixes**:
  - *Issue:* In Git Bash environments, `/usr/bin/link.exe` (GNU coreutils hard-link tool) can shadow MSVC's `link.exe`, and POSIX paths (`/d/...`) inside `INCLUDE`/`LIB` are not understood by native Win32 `cl.exe`.
  - *Solution:* Link via `cl.exe ... /link ...` in `Makefile` (so `cl.exe` invokes its own companion `link.exe`), and normalize POSIX drive prefixes in `INCLUDE`/`LIB` inside `build.sh`.

---

## Work Session Log

### 2026-10-18 — Configuration Module, Build Hardening & Unit Tests
- **Objective:** Implement `restic_plugin.ini` configuration management, establish a hardened NMake/Bash build and test pipeline following MSVC C11 best practices, and document all architectural and compiler conventions in English.
- **Changes:**
  - Updated `include/fsplugin.h` and `restic_wfx.def` with complete WFX constants and mandatory ANSI + Unicode exports.
  - Created `include/config.h` and `src/config.c` implementing INI path resolution, `restic.exe` binary discovery, and `[Repositories]` parsing.
  - Updated `src/main.c` to enumerate root (`\`) repositories dynamically using `config_load()` and reentrant `ResticFindHandle`.
  - Created `Makefile` (NMake), `build.sh`, `.clang-format`, and updated `.vscode/tasks.json` with strict flags (`/std:c11 /permissive- /W4 /WX /we4013 /GS /MTd /MT /DYNAMICBASE /NXCOMPAT`), `/analyze`, and AddressSanitizer (`/fsanitize=address`).
  - Added CRT Debug Heap leak checking (`_CRTDBG_MAP_ALLOC`, `_CrtCheckMemory`) to `src/config.c`, `src/main.c`, and `tests/test_config.c`.
  - Verified all 39 assertions in `tests/test_config.c` across Debug, Release, Static Analysis (`/analyze`), and AddressSanitizer (`asan`) builds.
