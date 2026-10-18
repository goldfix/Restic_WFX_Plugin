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
- ✅ **Restic CLI Execution & JSON/NDJSON Parsing (`include/restic_cli.h`, `src/restic_cli.c`, `tests/test_restic_cli.c`)**:
  - Child process execution via `CreateProcessW` (`CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT`), anonymous Win32 pipes, `NUL` stdin redirection, and `SecureZeroMemory` credential wiping.
  - Snapshot listing (`restic snapshots --json --no-lock`) and single-depth directory listing (`restic ls --json --no-lock <snapshot_id> <filter_path>` NDJSON).
  - ISO-8601 / RFC3339 timestamp parser with fractional seconds and timezone offset conversion to UTC `FILETIME`.
  - Reworked after the code review (see "Code Review" below): separate stdout/stderr, handle whitelist, timeout + cancel callback, argument quoting, `RESTIC_ERR_PATH_NOT_FOUND`.
  - Test suite: `tests/test_restic_cli.c` (222 assertions incl. a self-contained live test against `tmp/restic.exe`).
- ✅ **Pre-integration Code Review & Hardening**: 15 issues fixed, `tests/test_plugin_dll.c` smoke test added, 340 assertions total (58 + 222 + 60) green under Debug, `/analyze`, ASan and Release.
- 🔲 **Virtual Path Navigation & Credential Session (`src/main.c`)**:
  - Level 1 (`\<RepoAlias>\`) and Level 2 (`\<RepoAlias>\<SnapshotDir>\<SubPath>`) navigation.
  - On-demand password prompt via `RequestProcW` (`RT_Password`) held in RAM for the active repository session.
  - Pass a `ResticExecOptions.cancel_proc` that forwards `ProgressProc` (user abort) and map results to Win32 errors: `RESTIC_ERR_PATH_NOT_FOUND` -> `ERROR_PATH_NOT_FOUND`, empty directory -> `ERROR_NO_MORE_FILES`, wrong password -> re-prompt, "restic.exe not found" (`config_resolve_restic_path` == FALSE) -> `RT_MsgOK` message.
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

5. **Non-Recursive Single-Level Directory Listing via Positional Path Filter**
   - Calling `restic ls <snapshot_id> --json` without a path filter dumps the entire snapshot tree recursively. Passing an absolute forward-slash positional filter (`restic ls --json --no-lock <snapshot_id> /` or `/C/Users`) without `--recursive` instructs Restic to return only the target directory node and its immediate children.
   - Using `--no-lock` avoids writing lock objects to the repository, speeding up read-only browsing and supporting read-only storage backends.

6. **Process Spawning Contract (`restic_exec_command`)**
   - Separate stdout/stderr pipes, `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` inheritance whitelist, heap-allocated command line, polling collector with timeout/cancel, bare executable names refused, credentials only in the environment block (wiped after `CreateProcessW`). Rationale for every point is in the Code Review table.

7. **Strict NDJSON, Tolerant Environment**
   - `ls` output is parsed strictly (a corrupt line is an error, never a silently truncated listing) because stdout is guaranteed JSON-only. The surrounding environment is tolerant: inherited `RESTIC_*` credential variables are stripped, `null` snapshot lists are accepted, unparsable timestamps degrade to the "no date" marker.

8. **Dual Memory Safety Verification (CRT Debug Heap + AddressSanitizer)**
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

- **Restic `ls <snapshot_id> <filter_path>` Self-Node Emission**:
  - *Issue:* When querying `restic ls --json <snapshot_id> /tmp/sample_data`, Restic emits a `"message_type":"node"` entry for `/tmp/sample_data` itself before emitting its direct children (`/tmp/sample_data/file.txt`). Without filtering, the parent directory would appear as a subfolder inside itself.
  - *Solution:* `is_direct_child_path()` in `src/restic_cli.c` verifies that every node's `path` is strictly a direct child of `filter_path` (excluding both `filter_path` itself and any deeper grandchildren).

- **Go Runtime Environment Requirements on Windows (`CreateProcessW`)**:
  - *Issue:* Passing a minimal environment block containing *only* `RESTIC_REPOSITORY` and `RESTIC_PASSWORD` strips `SystemRoot`, `LOCALAPPDATA`, and `TEMP`, causing Go's runtime and Windows CryptoAPI/DNS inside `restic.exe` to fail.
  - *Solution:* `build_child_environment()` clones `GetEnvironmentStringsW()`, strips any pre-existing `RESTIC_*` credential variables, appends `RESTIC_REPOSITORY` and `RESTIC_PASSWORD`, and wipes the buffer with `SecureZeroMemory` immediately after `CreateProcessW`.

- **MSVC `/analyze` Stack Usage Warning (`C6262`)**:
  - *Issue:* Allocating large local buffers (e.g., `WCHAR cmd[4096]` + `char chunk[8192]` > 16 KB) on the stack triggers MSVC Static Analyzer warning `C6262`.
  - *Solution:* Keep stack buffers within 8 KB total per function so `/analyze` passes with zero warnings under `/WX`.

- **`swprintf_s` / `strcpy_s` Overflow Aborts the Host**:
  - *Issue:* The `_s` functions call the CRT invalid-parameter handler on overflow, which terminates the process - fatal inside a plugin. Do not rely on their return value for sizing.
  - *Solution:* Compute the exact length first and allocate on the heap, or use `_TRUNCATE` variants (`wcsncpy_s`, `utf8_to_wide_trunc`).

- **cJSON Exports Itself From Every Windows DLL**:
  - *Issue:* `cJSON.h` defaults to `__declspec(dllexport)` on Windows, so the plugin exported ~90 extra symbols.
  - *Solution:* Define `CJSON_HIDE_SYMBOLS` for `cJSON.c` and all includers (Makefile `CFLAGS_COMMON`). Check with `dumpbin /exports buildestic.wfx64`; `test_plugin_dll.c` enforces it.

- **Restic Writes Errors to stderr Even With `--json`**:
  - *Issue:* `{"message_type":"exit_error",...}` is on stderr (not stdout). A merged pipe mixes warnings into the JSON stream.
  - *Solution:* Never merge the streams; use stdout for payload and stderr for diagnostics.

- **cmd.exe as a Deterministic Test Child**:
  - *Tip:* `restic_exec_command` can launch `%ComSpec% /c "..."` to test stream separation, exit codes, environment propagation, timeouts (`ping -n 6 127.0.0.1 >nul`) and large outputs (`for /L %i in (1,1,4000) do @echo ...`) without a real repository.

- **Git Bash Path Conversion When Calling Native Tools**:
  - *Issue:* MSYS2 rewrites POSIX-looking arguments such as `/tmp/x` before they reach `restic.exe`/`dumpbin`.
  - *Solution:* `export MSYS2_ARG_CONV_EXCL="*"` for manual experiments (`build.sh` already does this for NMake).

- **MSYS2 / Git Bash `/usr/bin/link.exe` Collision & POSIX Drive Prefixes**:
  - *Issue:* In Git Bash environments, `/usr/bin/link.exe` (GNU coreutils hard-link tool) can shadow MSVC's `link.exe`, and POSIX paths (`/d/...`) inside `INCLUDE`/`LIB` are not understood by native Win32 `cl.exe`.
  - *Solution:* Link via `cl.exe ... /link ...` in `Makefile` (so `cl.exe` invokes its own companion `link.exe`), and normalize POSIX drive prefixes in `INCLUDE`/`LIB` inside `build.sh`.

---

## Code Review — 2026-10-18 (pre-integration check)

Full review of `config.c`, `restic_cli.c`, `main.c`, `Makefile`, tests. Everything below was **found, fixed and covered by a test** unless marked "open".

### Bugs fixed
| # | Severity | Problem | Fix / where |
| --- | --- | --- | --- |
| 1 | High | `swprintf_s` into fixed buffers (`cmd_line[2048]`, `args[2048]`) **aborts the whole process on overflow** (invalid-parameter handler) - a 1000-char path would have crashed Total Commander | Command line and `ls` arguments are heap-allocated from exact lengths (`restic_exec_command`, `restic_list_directory`) |
| 2 | High | stdout and stderr shared one pipe; Restic's errors AND warnings go to stderr, so any warning would corrupt the JSON payload | Two pipes, drained together by one poll loop (no deadlock); `ResticCommandOutput` has `output` + `error_output` |
| 3 | High | Binary planting: `SearchPathW` and bare-name `CreateProcess` search the **current directory**, which follows the Total Commander panel | Manual `%PATH%` walk (`find_in_path_env`); exec refuses names without a path separator |
| 4 | High | Argument injection: the `ls` filter path (any byte sequence from a Linux backup, e.g. `"`) was wrapped in naive quotes; snapshot ID was unvalidated | `restic_quote_arg()` (CommandLineToArgvW rules, round-trip tested); snapshot ID must be hex |
| 5 | High | No timeout/cancel: an unreachable remote repository freezes Total Commander for minutes (`WaitForSingleObject(INFINITE)`) | `ResticExecOptions` (timeout + cancel callback polled every ~100 ms), new codes `RESTIC_ERR_TIMEOUT/CANCELLED` |
| 6 | Medium | Pipe-handle inheritance race: another thread of the host calling `CreateProcess` could inherit our write end (EOF never arrives) or leak handles into Restic | `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` whitelist (stdin NUL, stdout, stderr) |
| 7 | Medium | Plugin DLL exported ~90 `cJSON_*` symbols (`__declspec(dllexport)` is cJSON's Windows default) | `/DCJSON_HIDE_SYMBOLS`; export surface asserted by `test_plugin_dll.c` |
| 8 | Medium | Non-existent snapshot path looked identical to an empty directory (both = empty list) | `RESTIC_ERR_PATH_NOT_FOUND` via the filtered directory's self-node |
| 9 | Medium | `MultiByteToWideChar` on over-long error messages failed and left the buffer unterminated | `utf8_to_wide_trunc()` always truncates and terminates |
| 10 | Medium | Wrong/missing dates showed as 1601: unparsable times produced `FILETIME` 0; root aliases had no date | `FS_SET_TIME_UNKNOWN` ("no date" marker from the WFX docs) |
| 11 | Low | `restic snapshots --json` printing `null` (older Restic) was a parse error | Treated as an empty list |
| 12 | Low | Snapshot dir name built from unvalidated time/ID strings (could contain separators) | `restic_format_snapshot_dirname` validates both |
| 13 | Low | UTF-8 path buffer (2048) too small for a 1023-unit CJK path (3 bytes/unit) | `MAX_UTF8_PATH` (3073) |
| 14 | Low | Timezone offset larger than the instant silently skipped instead of failing | `restic_parse_iso8601_time` returns FALSE |
| 15 | Test | Live test depended on a hand-made `tmp/sample_repo` and snapshot order (non-reproducible) | Self-contained fixture rebuilt on every run |

### Open items / known limitations (documented, intentionally not fixed yet)
- **Whole output is buffered** before parsing. A single directory with millions of entries means hundreds of MB of NDJSON. A streaming parser (parse while reading the pipe) is the future improvement.
- **INI encoding:** `GetPrivateProfile*W` reads ANSI or UTF-16 LE only; a UTF-8 INI with non-ASCII characters is garbled. (Would need an own parser.)
- **`FsSetDefaultParams` delivers `DefaultIniName` as ANSI:** a user profile path with characters outside the system code page is mangled by `MultiByteToWideChar(CP_ACP)`, so the fallback INI would be created in the wrong place. Possible mitigation: verify the directory exists, else use `%APPDATA%\GHISLER`.
- **Child tree is not killed on cancel:** `TerminateProcess` kills `restic.exe` only (rclone/ssh helpers it started may linger). A Job Object with `KILL_ON_JOB_CLOSE` would fix this.
- **Linux names with a literal backslash** cannot be addressed (backslash is the WFX separator). Names invalid on Windows (`:`/`*`/`?`) are passed through unchanged.
- **Snapshot folder names use the time in Restic's recorded UTC offset**, not the local zone of the viewer (the file-list date column is correctly UTC-converted).
- **Explicit `ResticPath` pointing to a missing file silently falls back** to the plugin folder/PATH.
- **Dependency on `main.c` integration:** `restic_exec_command` is not yet called by the WFX layer; the cancel callback, `ERROR_NO_MORE_FILES` vs `ERROR_PATH_NOT_FOUND` mapping, and "restic.exe not found" message still have to be wired in.

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

### 2026-10-18 — Restic CLI Execution & JSON/NDJSON Parser Library
- **Objective:** Implement the `restic_cli` module (`include/restic_cli.h`, `src/restic_cli.c`) to execute `restic.exe` via Win32 pipes and parse `snapshots --json` and `ls --json` outputs using `cJSON`, tested against both synthetic payloads and live `tmp/restic.exe` repositories.
- **Changes:**
  - Created `include/restic_cli.h` and `src/restic_cli.c` with ISO-8601 to UTC `FILETIME` conversion, snapshot folder formatting/extraction (`YYYY-MM-DD_HH-MM-SS_[short_id]`), WFX path normalization, `cJSON` array/NDJSON parsers, and Win32 `CreateProcessW` pipe execution with `SecureZeroMemory` credential cleanup.
  - Created `tests/test_restic_cli.c` (43 assertions) covering timezone-aware timestamps, 64-bit file sizes (>4 GiB), direct-child NDJSON filtering, wrong-password detection (exit code 12), missing-repository detection (exit code 10), and live snapshot/directory listing via `tmp/restic.exe`.
  - Updated `Makefile` to compile `src/restic_cli.c` and run both `test_config.exe` and `test_restic_cli.exe` across `debug`, `test`, `analyze`, and `asan` targets.

### 2026-10-18 — Pre-integration Code Review, Hardening & Test Extension
- **Objective:** Review everything implemented so far (config, CLI/JSON, WFX layer, build), look for latent bugs, extend the tests, update the docs.
- **Changes:**
  - `src/restic_cli.c` largely rewritten around a safer `restic_exec_command` (heap command line, separate stdout/stderr pipes, handle whitelist, poll loop with timeout/cancel, hardened environment block). New API: `ResticExecOptions`, `restic_quote_arg`, `RESTIC_ERR_PATH_NOT_FOUND/TIMEOUT/CANCELLED`. `restic_exec_command`, `restic_list_snapshots`, `restic_list_directory` gained an `options` parameter.
  - `src/config.c`: PATH-only `restic.exe` lookup (no cwd), INI template notes the ANSI/UTF-16 requirement.
  - `src/main.c`: root aliases carry the "no date" marker. `include/fsplugin.h`: `FS_SET_TIME_UNKNOWN`.
  - `Makefile`: `/DCJSON_HIDE_SYMBOLS`, empty `SYSLIBS` (plugin imports only KERNEL32), `TEST_LIBS = Shell32.lib`, new `test_plugin_dll.exe` target, `test` builds the plugin first (except under ASan). `build.sh`: new `release-test` action.
  - Tests: `test_config.c` 39 -> 58 assertions, `test_restic_cli.c` 43 -> 222, new `test_plugin_dll.c` (60). Verified under Debug (CRT leak check), `/analyze`, AddressSanitizer and Release (`/MT /O2`), all with `/W4 /WX`.
  - Mutation checks: reintroducing `SearchPathW` and removing the quote escaping both make the corresponding tests fail.
  - `AGENT.md` sections 3.3, 3.4, 6, 7.2-7.4, 8.5-8.7 updated.
- **Next step:** wire `restic_cli` into `main.c` (Level 1/2 routing, `RequestProcW` password session, `ProgressProc`-based cancel callback, error-code mapping).
