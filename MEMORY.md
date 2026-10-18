# MEMORY.md — Project Memory

## Roadmap

- ✅ **Project Skeleton & WFX Exports**: Initial WFX entry points (`FsInitW`, `FsGetDefRootName`, `FsSetDefaultParams`, `FsFindFirstW`, `FsFindNextW`, `FsFindClose`) and mandatory ANSI stubs (`FsInit`, `FsFindFirst`, `FsFindNext`).
- ✅ **Build System & Compiler Hardening (`Makefile`, `build.sh`, `.vscode/tasks.json`, `.clang-format`)**:
  - Environment-agnostic incremental NMake `Makefile` + `build.sh` wrapper for MSVC x64 (`cl.exe`, `link.exe`, `nmake.exe`).
  - Strict pure C11 flags (`/TC /std:c11 /permissive- /W4 /WX /we4013`, strictly no C++), static CRT (`/MT` Release, `/MTd` Debug), binary hardening (`/GS`, `/DYNAMICBASE`, `/NXCOMPAT`), CRT Debug Heap leak checking (`<crtdbg.h>`), AddressSanitizer (`ASAN=1`), and MSVC static analysis (`ANALYZE=1`).
- ✅ **Configuration Management (`config.h` / `config.c`)**:
  - Primary (`<plugin_dir>\restic_plugin.ini`) and fallback (`DefaultIniName` directory from `FsSetDefaultParams`) INI path resolution (read-only lookup at runtime; a commented sample `restic_plugin.ini` lives in the repository root and is copied to `build\restic_plugin.ini` by `Makefile` for deployment staging).
  - Three-tier `restic.exe` binary resolution (`[Settings] ResticPath` -> `<plugin_dir>\restic.exe` -> system `PATH`).
  - `[Repositories]` section parsing, sanitization, case-insensitive deduplication, and Level 0 (`\`) virtual folder enumeration.
  - Automated unit test suite (`tests/test_config.c`, 39 assertions passing under Debug, `/analyze`, and AddressSanitizer).
- ✅ **cJSON Integration (`cJSON/cJSON.h`, `cJSON/cJSON.c`)**: Integrated `cJSON` library from `cJSON/` into `Makefile` (`/IcJSON`, compiled as pure C11 `/TC /std:c11`).
- ✅ **Restic CLI Execution & JSON/NDJSON Parsing (`include/restic_cli.h`, `src/restic_cli.c`, `tests/test_restic_cli.c`)**:
  - Child process execution via `CreateProcessW` (`CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT`), anonymous Win32 pipes, `NUL` stdin redirection, and `SecureZeroMemory` credential wiping.
  - Snapshot listing (`restic snapshots --json --no-lock`) and single-depth directory listing (`restic ls --json --no-lock <snapshot_id> <filter_path>` NDJSON).
  - ISO-8601 / RFC3339 timestamp parser with fractional seconds and timezone offset conversion to UTC `FILETIME`.
  - Reworked after the code review (see "Code Review" below): separate stdout/stderr, handle whitelist, timeout + cancel callback, argument quoting, `RESTIC_ERR_PATH_NOT_FOUND`.
  - Test suite: `tests/test_restic_cli.c` (223 assertions incl. a self-contained live test against `tmp/restic.exe`).
- ✅ **Pre-integration Code Review & Hardening**: 15 issues fixed, `tests/test_plugin_dll.c` smoke test added, 340 assertions total (58 + 222 + 60) green under Debug, `/analyze`, ASan and Release.
- ✅ **Virtual Path Navigation & Credential Session (`src/main.c`)**:
  - Level 0 (`\`), Level 1 (`\<RepoAlias>`) and Level 2+ (`\<RepoAlias>\<SnapshotDir>\<SubPath>`) virtual path routing in `FsFindFirstW`.
  - In-memory password sessions, one per repository alias (8-slot table behind an SRW lock; `RT_Password` via `RequestProcW`, up to 3 attempts on `RESTIC_ERR_WRONG_PASSWORD`, wiped with `SecureZeroMemory` on wrong password, `FsInitW`, or `DLL_PROCESS_DETACH`).
  - `ProgressProcW`-based cancellation (`ESC`), `LogProcW` + `RT_MsgOK` error reporting, and Win32 error code mapping (`ERROR_NO_MORE_FILES` for empty dirs, `ERROR_PATH_NOT_FOUND` for missing paths/repos, `ERROR_ACCESS_DENIED` for auth failure, `ERROR_CANCELLED` for user abort).
  - End-to-end unit and DLL tests: **512 assertions** (80 + 273 + 159), all passing across Debug, `/analyze`, ASan (79 + 273, DLL test skipped) and Release (79 + 273 + 159).
- ✅ **Post-integration Bug Review**: 8 further issues fixed in `main.c`/`restic_cli.c` plus one build pitfall documented (see "Code Review 2" below).
- ✅ **File & Folder Extraction (`FsGetFileW` / `FsGetFile`, `restic_dump_file`)**:
  - Streaming file extraction via `restic dump --no-lock <snapshot_id> <filter_path>` with a fixed 64 KB buffer (constant memory usage), lazy local file creation (auth/path errors never touch the local disk), `LastWriteTime` preservation, two-step overwrite protocol (`FS_FILE_EXISTS` / `FS_COPYFLAGS_OVERWRITE`), byte-level `ProgressProcW` reporting + `ESC` abort with partial-file cleanup.
  - Native Total Commander integration for recursive folder copy (`F5`), file viewing (`F3` / `Ctrl+Q`), while keeping modification/deletion/move (`FsPutFile`, `FsDeleteFile`, `FsRemoveDir`, `FsMkDir`, `FsRenMovFile`, `FS_COPYFLAGS_MOVE`) disabled.
- ✅ **Extraction Code Review (Code Review 3)**: atomic temp-file replace, inactivity timeout, throttled progress, fake-restic and edge-case tests (see "Code Review 3" below).
- ✅ **Manual Total Commander Tests** (user): initial manual verification succeeded.
- ✅ **DLL Versioning (`include/version.h`, `src/restic_wfx.rc`) & Final Documentation (`README.md`)**: `VS_VERSION_INFO` (`0.1.0.0`) embedded in `restic.wfx64` and verified by `test_plugin_dll.c`; complete English `README.md` prepared for the v0.1.0 release.

---

## Architectural & Build Decisions

1. **Combined Unicode + Mandatory ANSI WFX Stubs**
   - Total Commander's plugin loader checks for mandatory base exports (`FsInit`, `FsFindFirst`, `FsFindNext`, `FsFindClose`) when loading a `.wfx` / `.wfx64` DLL, then invokes the `W` variants (`FsInitW`, `FsFindFirstW`, `FsFindNextW`) on NT/Unicode systems. Exporting lightweight ANSI stubs alongside the Unicode implementation ensures unconditional compatibility across all Total Commander versions.

2. **Two-Tier Read-Only INI Resolution + Build-Staged Sample INI**
   - The plugin looks for `<plugin_dir>\restic_plugin.ini` first and falls back to Total Commander's user configuration directory provided via `FsSetDefaultParams` (`DefaultIniName`, typically `%APPDATA%\GHISLER\`). It never auto-creates or writes `restic_plugin.ini` at runtime. Instead, a fully commented sample `restic_plugin.ini` is tracked in the project root and copied into `build\restic_plugin.ini` by `Makefile` alongside `build\restic.wfx64`.

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
  - *Solution:* Define `CJSON_HIDE_SYMBOLS` for `cJSON.c` and all includers (Makefile `CFLAGS_COMMON`). Check with `dumpbin /exports build
estic.wfx64`; `test_plugin_dll.c` enforces it.

- **Restic Writes Errors to stderr Even With `--json`**:
  - *Issue:* `{"message_type":"exit_error",...}` is on stderr (not stdout). A merged pipe mixes warnings into the JSON stream.
  - *Solution:* Never merge the streams; use stdout for payload and stderr for diagnostics.

- **cmd.exe as a Deterministic Test Child**:
  - *Tip:* `restic_exec_command` can launch `%ComSpec% /c "..."` to test stream separation, exit codes, environment propagation, timeouts (`ping -n 6 127.0.0.1 >nul`) and large outputs (`for /L %i in (1,1,4000) do @echo ...`) without a real repository.

- **Locked `restic.wfx64` While Total Commander Runs**:
  - *Issue:* Windows refuses to overwrite a loaded DLL (`LNK1104: cannot open file 'build
estic.wfx64'`).
  - *Solution:* `build.sh` renames the locked file to `restic.wfx64.old` before invoking NMake; unload the plugin in Total Commander with `cm_UnloadPlugins` (or restart it), then delete the `.old` file.

- **Stale Objects After Switching Build Configuration**:
  - *Issue:* The `Makefile` does not track `CONFIG`/`ASAN`/`ANALYZE` changes, so Debug objects linked with Release ones (or vice versa) fail with `unresolved external symbol _calloc_dbg` / `LNK4098`.
  - *Solution:* `./build.sh clean` before changing configuration (the `asan`/`analyze` actions rebuild what they need; `release-test` after a Debug run needs a clean).

- **Heredocs Truncate Long Scripts in the Agent Shell**:
  - *Issue:* Large `cat <<EOF` / `python - <<EOF` bodies are silently cut (warning "delimited by end-of-file"), leaving half-written files; chained with `&& rm` they delete the source.
  - *Solution:* Use the `write`/`edit` tools for anything beyond a few dozen lines and never chain `rm` after a heredoc script.

- **Python Environment Already Active in Shell (`python`)**:
  - *Fact:* `python` is directly available in the working shell with the project's virtual environment already active. When creating or executing Python helper scripts, invoke `python` directly without creating or activating a new virtual environment.

- **Fake Restic via Batch File**:
  - *Technique:* `CreateProcessW` accepts a `.bat` as the executable (cmd starts implicitly), so tests can emulate `restic.exe` deterministically; the scenario is chosen from `%~4` (the path argument). `ping -n N 127.0.0.1 >nul` is a portable sleep and `<nul set /p "=text"` prints without a trailing CRLF.

- **ProgressProc During Directory Listing**:
  - *Facts (WFX SDK):* calls inside the `FsFindFirst/Next/Close` loop are ignored for the first 5 seconds, then Total Commander shows a progress dialog and returns 1 when the user aborts. Never pass strings that may hold secrets (repository URLs) - they are displayed.

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
- **`FsSetDefaultParams` delivers `DefaultIniName` as ANSI:** a user profile path with characters outside the system code page is mangled by `MultiByteToWideChar(CP_ACP)`, so the fallback INI would be looked up in the wrong place. Possible mitigation: verify the directory exists, else use `%APPDATA%\GHISLER`.
- **Child tree is not killed on cancel:** `TerminateProcess` kills `restic.exe` only (rclone/ssh helpers it started may linger). A Job Object with `KILL_ON_JOB_CLOSE` would fix this.
- **Linux names with a literal backslash** cannot be addressed (backslash is the WFX separator). Names invalid on Windows (`:`/`*`/`?`) are passed through unchanged.
- **Snapshot folder names use the time in Restic's recorded UTC offset**, not the local zone of the viewer (the file-list date column is correctly UTC-converted).
- **Explicit `ResticPath` pointing to a missing file silently falls back** to the plugin folder/PATH.

---

## Code Review 2 — 2026-10-18 (after the `main.c` integration, before manual Total Commander tests)

Checked against the official WFX SDK pages (`ProgressProc`, `RequestProc`, `FsFindFirst`). All items fixed and covered by tests in `test_plugin_dll.c` / `test_restic_cli.c`.

| # | Severity | Problem | Fix |
| --- | --- | --- | --- |
| 1 | High | **Credential leak on screen:** the cancel callback passed the repository *URL* as `TargetName` to `ProgressProc`; Total Commander shows it in its progress dialog and URLs may embed passwords (`rest:https://user:pass@host`) | The alias is passed instead (mutation-tested: reverting makes the test fail) |
| 2 | High | **Process abort on long text:** `swprintf_s` into a 768-char buffer for the error popup (259-char alias + 511-char Restic message) triggers the CRT invalid-parameter handler and kills Total Commander | `_snwprintf_s(..., _TRUNCATE)` with a 1024-char buffer; test with a 250-char alias |
| 3 | Medium | **Password session was not thread-safe** (Total Commander may call from worker threads) and single-slot | `SRWLOCK`-guarded table; the lock is never held while Restic runs or a dialog is open |
| 4 | Medium | **Single-slot session re-prompted on every panel switch** when the two panels show two different repositories | 8 per-alias slots, round-robin eviction |
| 5 | Medium | **Unvalidated passwords stayed cached:** a password typed for a mistyped/unreachable repository was kept, and an empty password produced a non-recoverable generic failure (Restic exit 1) with the bad password cached forever | Cache only after an outcome that does not disprove the password; empty password = cancel |
| 6 | Medium | `ProgressProc` was called with a fixed 50 %; Total Commander shows a progress dialog after 5 s | `PercentDone = 0` (indeterminate) |
| 7 | Low | No upper bound for a hung `restic.exe` if the progress dialog never appears | 30-minute `WFX_RESTIC_TIMEOUT_MS` |
| 8 | Low | Nodes with an empty name, `.` or `..` would confuse Total Commander's navigation | Skipped in `node_to_find_data` (unit test) |
| 9 | Test | `build/` objects of a previous configuration (e.g. Release after an ASan/Debug run) cause `_calloc_dbg` link errors | Documented pitfall: run `./build.sh clean` when switching configuration |

Verified non-issues: Restic names are limited to 255 bytes so `cFileName[MAX_PATH]` always fits; the path passed to `FsFindFirstW` has no mask/trailing separator, and double separators are collapsed by `restic_normalize_filter_path`; a listing may be requested directly for a deep path (Ctrl+D hotlist) - the router handles it and prompts for the password on first use.

---

## Code Review 3 — 2026-10-18 (file extraction: `restic_dump_file` / `FsGetFileW`)

| # | Severity | Problem | Fix |
| --- | --- | --- | --- |
| 1 | High | **Data loss on failed/cancelled overwrite:** the first implementation opened the target with `CREATE_ALWAYS` and deleted it on failure, so `ESC`, a network error or a wrong password during an *overwrite* destroyed the user's existing file | Data goes to `<dir>\~rp<tid>.tmp` and is moved over the target with `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` only after exit code 0; failures delete just the temporary file. Mutation-tested (writing straight to the target fails 2 tests) |
| 2 | High | **Large transfers killed after 30 minutes:** `WFX_RESTIC_TIMEOUT_MS` was a *total* run-time limit, so a long download of a big file from a slow backend was aborted | The timeout of `restic_dump_file` is an inactivity timeout (reset by every byte on stdout/stderr). Mutation-tested with a fake restic emitting one byte per second under a 1.8 s timeout |
| 3 | Medium | `ProgressProc` was invoked for every 64 KB chunk (thousands of UI updates per GB) | Throttled to the first chunk plus one call per ~100 ms (`first_reported` keeps `ESC` deterministic for short files) |
| 4 | Medium | Failure paths (exit codes, stderr text, partial output, timeouts) were only covered by the real binary, where they are timing/format dependent | `test_dump_with_fake_restic`: a generated `.bat` selected by the snapshot-path argument |
| 5 | Low | `restic dump` of a directory returns a tar stream; reachable only if a caller passes `ri == NULL` (Total Commander always passes `ri`) | Documented limitation; `ri->Attr` directory flag -> `FS_FILE_NOTSUPPORTED` |
| 6 | Low | Uniform `'x'` fixture payloads could not reveal text-mode (CRLF / Ctrl-Z) translation of the pipe | New `sub\sub2\binary.dat` (70 000 bytes with `00 0A 0D 1A`), verified byte for byte through the CLI layer and the DLL |
| 7 | Test | An old test expected the truncate-and-delete behaviour on cancel | Updated to the new semantics (target preserved) |

Verified non-issues: `build_error_message` copes with an empty stderr (`utf8_to_wide_trunc(NULL)` is a no-op); `FsGetFileW` runs on the UI thread because `FsGetBackgroundFlags` is not exported; cached passwords survive `LOCAL_WRITE` (it only happens after Restic authenticated); MSVC `/analyze` C6387 required returning the handle from the open helper instead of re-reading `st->hLocal`.

---

## Current State & Next Steps (read this first after a session compaction)

- **Functionality:** read-only browsing and file/folder extraction are complete: `\` (aliases) -> `\<Alias>` (snapshots `YYYY-MM-DD_HH-MM-SS_[id]`) -> `\<Alias>\<Snapshot>\<dirs/files>`, plus `FsGetFileW` (`F5` copy of files and recursive folders, `F3` Lister, `Ctrl+Q` Quick View). Exported API: the 11 `Fs*` read-only + extraction functions (`FsPutFile`, `FsDeleteFile`, `FsRemoveDir`, `FsMkDir`, `FsRenMovFile` are not exported, and `FS_COPYFLAGS_MOVE` is rejected with `FS_FILE_NOTSUPPORTED`).
- **Quality gates (all green):** `./build.sh test`, `analyze`, `asan`, `release-test`; 512 assertions (`test_config` 80, `test_restic_cli` 273, `test_plugin_dll` 159). Run `./build.sh clean` when switching configuration. Live tests need `tmp/restic.exe` (git-ignored, otherwise `[SKIP]`).
- **Next:** Ready for `v0.1.0` release tagging; optional future enhancements: streaming `ls` parser, optional directory cache, Job Object to kill child trees, UTF-8 INI.
- **Conventions:** pure C11 (`/TC`), English comments/docs, UNICODE, LF endings, repository free of machine-specific paths (`grep -rnI -E 'setup_x64|_progetti' --exclude='.env' --exclude-dir=.git --exclude-dir=tmp --exclude-dir=build .` must print nothing), Python (`python`) is already active in the shell environment (do not create new venvs), user conversation in Italian, update `AGENT.md`/`MEMORY.md` after significant changes.

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
  - `src/config.c`: PATH-only `restic.exe` lookup (no cwd).
  - `src/main.c`: root aliases carry the "no date" marker. `include/fsplugin.h`: `FS_SET_TIME_UNKNOWN`.
  - `Makefile`: `/DCJSON_HIDE_SYMBOLS`, empty `SYSLIBS` (plugin imports only KERNEL32), `TEST_LIBS = Shell32.lib`, new `test_plugin_dll.exe` target, `test` builds the plugin first (except under ASan). `build.sh`: new `release-test` action.
  - Tests: `test_config.c` 39 -> 58 assertions, `test_restic_cli.c` 43 -> 222, new `test_plugin_dll.c` (60). Verified under Debug (CRT leak check), `/analyze`, AddressSanitizer and Release (`/MT /O2`), all with `/W4 /WX`.
  - Mutation checks: reintroducing `SearchPathW` and removing the quote escaping both make the corresponding tests fail.
  - `AGENT.md` sections 3.3, 3.4, 6, 7.2-7.4, 8.5-8.7 updated.
- **Next step:** wire `restic_cli` into `main.c` (Level 1/2 routing, `RequestProcW` password session, `ProgressProc`-based cancel callback, error-code mapping).

### 2026-10-18 — WFX Virtual Path Routing, Password Session & End-to-End Integration (`src/main.c`)
- **Objective:** Complete the read-only MVP by integrating `restic_cli` into `src/main.c` (points 1–4: virtual path routing, in-memory password session with retry, `ProgressProcW` cancellation and error reporting, Win32 error mapping) and verifying with end-to-end tests.
- **Changes:**
  - Updated `src/main.c` with `parse_wfx_path()`, `execute_repo_query()`, `prompt_repository_password()`, `clear_password_session()`, `wfx_cancel_callback()`, `report_plugin_error()`, and `handle_restic_failure()`.
  - Added `test_wfx_routing_and_callbacks()` in `tests/test_config.c` (70 assertions total) testing malformed paths, unknown aliases, invalid snapshot directory names, missing `restic.exe` diagnostics, and password prompt cancellation.
  - Added `test_live_end_to_end_navigation()` in `tests/test_plugin_dll.c` (84 assertions total) driving `build\restic.wfx64` through Level 0 -> Level 1 -> Level 2+ (`data`, `empty_dir`, missing paths), password retry after wrong password, RAM password reuse, repo-switch session wipe, 3-attempt lockout, missing repository, and `ESC` abort via `ProgressProcW`.
  - (Superseded: the single-slot session described here became per-repository slots in the later review.) Verified all **376 assertions** (70 + 222 + 84) across `test`, `analyze`, `asan`, and `release-test` with zero warnings.
- **Next step:** Write final user & developer documentation in `README.md` (installation in Total Commander, `restic_plugin.ini` configuration, build/test instructions).

### 2026-10-18 — Post-integration Bug Review (before manual Total Commander tests)
- **Objective:** Hunt for bugs in the new `main.c` integration and extend tests/docs before the first manual run in Total Commander.
- **Changes:**
  - `src/main.c`: per-repository, lock-protected password table (`session_get/put/forget`), empty-password rejection, caching only when the password is not disproved, alias (not URL) and 0 % passed to `ProgressProc`, `_snwprintf_s(_TRUNCATE)` for every message, 30-minute safety timeout.
  - `src/restic_cli.c`: empty/`.`/`..` node names skipped.
  - `tests/test_plugin_dll.c` 84 -> 112 assertions (sessions, eviction, empty/cancelled password, long alias, ESC details, 6 worker threads); `tests/test_restic_cli.c` 222 -> 223.
  - Verified: `test`, `analyze`, `asan`, `release-test` green; release DLL exports exactly the 9 `Fs*` functions, imports only `KERNEL32.dll`, has ASLR (+high entropy) and DEP.
  - `AGENT.md` sections 2 (credential lifecycle), 7.5 and 8.7 updated.
- **Next step:** manual tests in Total Commander (install `build/restic.wfx64`, create `restic_plugin.ini`), then `README.md`.

### 2026-10-18 — Extraction Code Review (Code Review 3)
- **Objective:** Re-read `restic_dump_file` / `FsGetFileW` for bugs before the manual Total Commander tests; extend tests and docs.
- **Changes:**
  - `src/restic_cli.c`: `restic_dump_file` rewritten around `DumpState` (temporary file + atomic `MoveFileExW`, inactivity timeout, throttled progress, early refusal of existing/directory targets when `overwrite == FALSE`). `include/restic_cli.h` documents the new semantics.
  - `tests/test_restic_cli.c` 236 -> 273 (binary fixture, target preservation, local failures, argument validation, fake-restic batch scenarios, two mutation checks); `tests/test_plugin_dll.c` 123 -> 151 (`test_e2e_extraction_edge_cases`, new `ExtractRepo` alias, `binary.dat` in the recursive copy).
  - Verified `test`, `analyze`, `asan`, `release-test`; the release DLL exports 11 `Fs*` functions and imports only `KERNEL32.dll`.
  - `AGENT.md` sections 5 and 8.7 updated.
- **Next step:** manual tests in Total Commander (F5 on files/folders, overwrite dialog, `ESC`, F3/Ctrl+Q), then `README.md`.

### 2026-10-18 — Removal of the Default INI Auto-Creation (cleanup review)
- **Objective:** Remove the "create a default `restic_plugin.ini` on init" feature entirely and double-check that no leftovers remain.
- **Changes:**
  - `include/config.h` / `src/config.c`: removed the commented INI template, `create_default_ini_file`, `config_ensure_default_ini` and the `create_if_missing` parameter (`config_resolve_ini_path(out, cap)` is now a pure lookup). `src/main.c`: `FsInit`/`FsInitW`/`FsSetDefaultParams` no longer touch the disk.
  - Cleanup review findings: `FsGetFileW` returned `FS_FILE_READERROR` when `config_load` failed (inconsistent with the listing code, which reports "not found") -> now `FS_FILE_NOTFOUND`; stale wording in `config.h` and in the `DefaultIniName` limitation note fixed; a grep for `templat|auto-creat|commented|create_if_missing|ensure_default` finds only the intentional "no auto-creation" statements.
  - Tests: INI-priority test rewritten (verifies nothing is created), DLL scenario B adjusted, new `test_missing_ini_is_harmless` (no INI anywhere: all entry points stay inert, no prompt/popup, plugin and fallback directories remain empty). Totals: 503 assertions (79 + 273 + 151); `test`, `analyze`, `asan`, `release-test` green.
- **Next step:** manual tests in Total Commander (the user creates `restic_plugin.ini` by hand), then `README.md` (document the INI format there, since the plugin no longer generates a sample).

### 2026-10-18 — Root Sample `restic_plugin.ini` Staging & WFX Icon Specification
- **Objective:** Add a commented sample `restic_plugin.ini` in the repository root, copy it to `build\restic_plugin.ini` during builds for deployment staging, and document Total Commander's WFX icon requirements from the official MCP documentation.
- **Changes:**
  - Created `restic_plugin.ini` in the project root (all settings/repositories commented out, documenting placement, ANSI/UTF-16 LE encoding, `ResticPath` resolution, and local/UNC/REST/SFTP/S3/Rclone repository examples).
  - Updated `Makefile` (`INI_TEMPLATE` / `INI_OUT`) so `all`, `test`, and `analyze` copy `restic_plugin.ini` to `build\restic_plugin.ini`.
  - Added assertion in `tests/test_config.c` (`test_missing_ini_is_harmless`) verifying `build\restic_plugin.ini` is staged and parses with `0` active repositories (504 assertions total: 80 + 273 + 151).
  - Documented Total Commander WFX icon rules in `AGENT.md` (§5.1) and `MEMORY.md`: Total Commander loads the first `ICON` resource in the `.wfx64` DLL for Network Neighborhood (`16x16` mandatory; `16, 24, 32, 48, 256` 32-bit RGBA `.ico` recommended).

### 2026-10-18 — Python Environment Initialization & Activation Fix (`py_env.sh`)
- **Objective:** Fix virtual environment activation failure on Windows Git Bash/MSYS2 (`.venv/bin/activate: No such file or directory`) and ensure `.env` toolchain variables load properly.
- **Changes:**
  - `py_env.sh`: added `_activate_venv` helper supporting both Windows (`.venv/Scripts/activate`) and POSIX (`.venv/bin/activate`), checking exit code and file existence before reporting success.
  - Sourced `.env` upfront before calling `uv venv` / `uv sync` or activating the venv, so MSVC variables (`INCLUDE`, `LIB`, `PATH`) are available to build/tooling processes.
  - Accepted `activate` as an alias for `active` (`source py_env.sh activate`).
  - Added warning when `py_env.sh` is executed in a subshell rather than sourced.

### 2026-10-18 — Multi-Resolution Plugin Icon (`logo/restic_wfx.ico` + `src/restic_wfx.rc`)
- **Objective:** Merge the individual icon frames in `logo/` into a single multi-resolution `.ico` file, embed it as the first `ICON` resource in `restic.wfx64`, and verify it in `test_plugin_dll.c`.
- **Changes:**
  - Merged `logo/logo_16x16.ico`, `logo_24x24.ico`, `logo_32x32.ico`, `logo_48x48.ico` (32-bit BGRA BMP/DIB) and `logo_256x256.png` (32-bit RGBA PNG) into `logo/restic_wfx.ico` (5 frames) and removed the obsolete root `logo.png`.
  - Created `src/restic_wfx.rc` (`1 ICON "logo\\restic_wfx.ico"`) and updated `Makefile` to compile `build\restic_wfx.res` via `rc.exe` and link it into `build\restic.wfx64`.
  - Extended `test_export_surface` in `tests/test_plugin_dll.c` to verify `RT_GROUP_ICON` #1 and all 5 `RT_ICON` frames (510 assertions total: 80 + 273 + 157, all green across `test`, `analyze`, `asan`, `release-test`).
  - Recorded in `MEMORY.md` that `python` is available with its virtual environment already active in the shell (no need to create a new venv).

### 2026-10-18 — DLL Versioning (`include/version.h`, `VS_VERSION_INFO`) & Release `README.md`
- **Objective:** Prepare the project for its first public release (`v0.1.0`): add Windows version metadata to `restic.wfx64` ("Details" tab in Explorer/Total Commander) and write the comprehensive English `README.md`.
- **Changes:**
  - Created `include/version.h` (`0.1.0.0` / `"0.1.0"`) and added `VS_VERSION_INFO` (`VERSIONINFO`) to `src/restic_wfx.rc`; updated `Makefile` (`RCFLAGS` with `/Iinclude`).
  - Extended `test_export_surface` in `tests/test_plugin_dll.c` to inspect `RT_VERSION` (`VS_FIXEDFILEINFO` signature `0xFEEF04BD`, version numbers, and `VFT_DLL` type): 512 assertions total (80 + 273 + 159), all green across `test`, `analyze`, `asan`, `release-test`.
  - Wrote `README.md` covering plugin purpose, Restic overview and download links, Total Commander installation, `restic_plugin.ini` locations and backend examples, usage (`F5`/`F3`/`Ctrl+Q`/`ESC`), security model, build/test instructions, external libraries (`cJSON`, WFX SDK, Restic), and GitHub links.

