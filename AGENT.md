# AGENT.md - Restic File System Plugin for Total Commander (WFX)

Technical specification, operational requirements, compiler hardening rules, and architectural guidelines for developing the Total Commander Restic plugin in standard C (C11).

---

## 1. Project Goal

Build a **File System Plugin (WFX)** extension for **Total Commander** written strictly in **pure standard C (C11)** (no C++ or other languages).

The primary goal of Phase 1 (MVP) is to provide interactive, read-only browsing of **Restic** backup repositories ([https://restic.net](https://restic.net)), allowing users to navigate seamlessly across configured repositories, snapshots, and directory trees directly within Total Commander's dual-pane interface.

**Current scope:** read-only browsing **plus extraction** of files and whole folders through Total Commander's native copy (`F5`), viewer (`F3`) and Quick View (`Ctrl+Q`) on files, 27 statistics columns (`FsContent*`) and an on-demand Markdown statistics report (`F3` / `Alt+Enter` / `stats` on a repository, snapshot or folder, section 5.3). Repository modification (upload, delete, move/rename, mkdir) is deliberately out of scope and not exported.

---

## 2. Core Architectural Decisions

1. **Interface Choice: WFX (File System Extension) instead of WCX (Packer)**
   - *Rationale:* The WCX archive model requires sequential, exhaustive enumeration of every file in an archive upon opening (`OpenArchive` / `ReadHeader` loop), freezing the UI on repositories containing hundreds of thousands of files. The WFX model enables lazy, on-demand directory-by-directory loading through the `FsFindFirstW` / `FsFindNextW` / `FsFindClose` lifecycle.

2. **Native CLI Integration**
   - No direct parsing of Restic's proprietary binary repository formats.
   - Invokes `restic.exe` as a hidden child process (`CREATE_NO_WINDOW`) via Win32 API (`CreateProcessW`).

3. **JSON & NDJSON Parsing**
   - Integrates the open-source **cJSON** library directly in source form from the `cJSON/` directory (`cJSON/cJSON.h`, `cJSON/cJSON.c`).
   - Uses line-by-line NDJSON streaming/parsing for `restic ls --json` output.

4. **Caching Policy (MVP)**
   - **No persistent or in-memory directory cache** (the only cache is the volatile snapshot statistics table of item 6).
   - Every directory navigation queries the CLI directly to keep the codebase minimal, deterministic, and free from cache invalidation complexity.

5. **Credential Lifecycle**
   - No passwords are stored on disk (neither in `restic_plugin.ini` nor in external files).
   - Credentials are requested at runtime when entering a repository via Total Commander's `RequestProcW` (`RT_Password`) callback.
   - Credentials remain in RAM only, in a small per-repository session table (8 slots, so two panels can browse two repositories without being re-prompted). Entries are wiped on wrong password, on `FsInitW` and on DLL unload; the oldest slot is recycled when the table is full.

6. **Backup Statistics as Native Custom Columns**
   - The `FsContent*` surface exposes the metadata of `restic snapshots --json` as Total Commander custom columns (`FsContentGetSupportedField`/`FsContentGetValue`), and `FsContentGetDefaultView` switches the panel to a ready-made column set when the plugin is entered.
   - Every value comes from the JSON that was **already downloaded** while enumerating the snapshot list: no field ever spawns `restic.exe` (the only extra run is the one-off cold-start fill done by the *directory listing*, see §5.2), no field is delayed (`ft_delayed`/`ft_ondemand` are never returned and deliberately not even defined in `include/fsplugin.h`) and no password prompt can be triggered from a background thread.
   - Parsed records live in a small volatile per-alias cache (8 slots, `SRWLOCK`, copies handed out, cleared on `FsContentPluginUnloading`/`FsInitW`/DLL unload). It is a *statistics* cache, not a directory cache: the `FsFindFirstW`/`FsFindNextW` listing path still queries the CLI on every navigation.

---

## 3. Configuration: `restic_plugin.ini`

### 3.1 INI File Path Resolution

1. **Primary Lookup:** inside the directory containing the plugin DLL (`<plugin_dir>\restic_plugin.ini`), resolved at load time in `DllMain` via `GetModuleFileNameW`.
2. **Fallback:** if `<plugin_dir>\restic_plugin.ini` does not exist (e.g., when installed under a shared/read-only path such as `C:\Program Files\`), the plugin looks for `restic_plugin.ini` inside the directory supplied by Total Commander via `FsSetDefaultParams` (`DefaultIniName`, typically `%APPDATA%\GHISLER\`). `DefaultIniName` is an **ANSI** `char[MAX_PATH]` (WFX 2.1), so a profile directory containing characters outside the active code page would be converted to `?`: when that happens (`wcschr(wide, L'?')`) or when `DefaultIniName` is empty, `config_set_fallback_from_default_params()` uses the UTF-16 environment variable `%COMMANDER_INI_PATH%` instead (Total Commander 11.03+, the directory of `wincmd.ini` without trailing backslash). A usable `DefaultIniName` always wins over the variable, so behaviour is unchanged on normal installations.
3. **No Auto-Creation at Runtime (Sample Template in Repo):** the plugin DLL never creates or modifies `restic_plugin.ini` on disk. If neither file exists: the root listing (`\`) returns `ERROR_NO_MORE_FILES` (empty folder), `\<Alias>[...]` listings return `ERROR_PATH_NOT_FOUND`, and `FsGetFileW` returns `FS_FILE_NOTFOUND`; no password prompt and no popup are shown. A fully commented sample `restic_plugin.ini` is tracked in the repository root and copied to `build\restic_plugin.ini` by `Makefile` during builds so the `build\` folder is ready for deployment.

### 3.2 INI File Structure

Standard Windows INI key-value syntax:

```ini
[Settings]
; Optional path to restic.exe (if empty, checks plugin folder then system PATH)
ResticPath=restic.exe

[Repositories]
; <Display_Alias>=<Restic_Repository_Target>
BackupLocale=D:\Backups\ResticRepo
ServerLinux=rest:https://backup.lan:8000/
CloudS3=s3:s3.us-east-1.amazonaws.com/company-backup/restic
NAS_SMB=\\nas\share\restic_repo
```

### 3.3 Restic Binary Resolution Order

1. Explicit path or custom executable configured in `ResticPath` under `[Settings]`.
2. Local `restic.exe` binary located in the plugin directory (`<plugin_dir>\restic.exe`).
3. Fallback to `restic.exe` found in the directories listed in the `PATH` environment variable.

Rules enforced by `config_resolve_restic_path()`:
- **The current directory is never searched.** `SearchPathW` and bare-name `CreateProcess` lookups include the process working directory (which follows the Total Commander panel), so a `restic.exe` planted in a browsed folder could hijack the plugin. `PATH` is therefore walked manually (`find_in_path_env`): empty entries are skipped, quoted entries are unquoted, an extension-less name gets `.exe` appended, and the hit is returned as an absolute path.
- When nothing is found the function returns `FALSE` and the bare name `restic.exe`. `restic_exec_command()` refuses bare names (`RESTIC_ERR_SPAWN_FAILED`), so the caller must report "restic.exe not found" instead of spawning anything.
- An explicit `ResticPath` that does not exist silently falls through to steps 2-3.

### 3.4 INI File Encoding

The file is read with the Win32 `GetPrivateProfile*W` APIs, which understand only **ANSI (system code page) or UTF-16 LE**. UTF-8 files are *not* decoded (non-ASCII aliases/paths would be garbled).

---

## 4. Virtual Navigation Hierarchy

Total Commander passes backslash-delimited paths rooted at `\`. The plugin maps virtual paths as follows:

| Level | WFX Path | Data Source | Action / Presentation |
| --- | --- | --- | --- |
| **0. Root** | `\` | `[Repositories]` section of `restic_plugin.ini` | Returns each configured alias (e.g., `BackupLocale`, `ServerLinux`) as a virtual directory (`FILE_ATTRIBUTE_DIRECTORY`). |
| **1. Repository** | `\<RepoAlias>\` | `restic snapshots --json --no-lock` | Executes Restic with `RESTIC_REPOSITORY` and `RESTIC_PASSWORD`. Returns each snapshot as a directory (`YYYY-MM-DD_HH-MM-SS_[short_id]`). |
| **2. Snapshot/Subdir** | `\<RepoAlias>\<SnapshotDir>\<SubPath>` | `restic ls --json --no-lock <snapshot_id> <filter_path>` | Lists direct child files and folders at the requested directory depth without recursive traversal, populating `WIN32_FIND_DATAW` (64-bit file sizes, UTC `FILETIME` timestamps, attributes). |

**Statistics report.** The listings contain only real entries: no artificial file is added at any
level. The report of a repository, a snapshot or a snapshot subfolder is produced on demand by
`F3` on the folder (intercepted inside the folder-size calculation, see 5.3) or by
`Alt+Enter` / `quote stats` (`FsExecuteFileW`); it is a Markdown document written to `%TEMP%` and
opened in the Total Commander Lister.

---

## 5. Implemented WFX API (Read-Only Browsing, File Extraction & Statistics Columns)

All exported functions use the `__stdcall` calling convention. Both Unicode (`W`) entry points and mandatory ANSI stubs are exported for full compatibility with the Total Commander plugin loader:

- **`FsInitW` / `FsInit`**
  - Stores the assigned `PluginNr` identifier and host callback pointers (`pProgressProcW`, `pLogProcW`, `pRequestProcW`).
- **`FsSetDefaultParams`**
  - Extracts and saves the fallback configuration directory from `DefaultIniName`.
- **`FsGetDefRootName`**
  - Returns the default root label displayed in Network Neighborhood (`"Restic Backups"`).
- **`FsFindFirstW` / `FsFindFirst`**
  - Initializes directory enumeration for `Path`, allocates a reentrant `ResticFindHandle`, and populates the first `WIN32_FIND_DATAW` record.
  - On empty directories, calls `SetLastError(ERROR_NO_MORE_FILES)` and returns `INVALID_HANDLE_VALUE`.
- **`FsFindNextW` / `FsFindNext`**
  - Advances the `ResticFindHandle` cursor and populates the next `WIN32_FIND_DATAW` entry, or returns `FALSE` when exhausted.
- **`FsFindClose`**
  - Frees the `ResticFindHandle` and all associated buffers.
- **`FsGetFileW` / `FsGetFile`**
  - Extracts a single file from a snapshot (`restic dump --no-lock <snapshot_id> <filter_path>`) via `restic_dump_file()`, streaming Restic's stdout in 64 KB chunks (constant memory) into a **temporary file** `<target dir>\~rp<thread id>.tmp` that atomically replaces `LocalName` (`MoveFileExW`) only after Restic exits with code 0. On any failure or `ESC` only the temporary file is deleted and a pre-existing target stays untouched (an interrupted overwrite never destroys the old file). The temporary file is created on the first byte, so authentication/path errors never touch the disk; 0-byte files are created at commit time.
  - Integrates with Total Commander's native copy/extract workflow: when the user copies a folder (`F5`), Total Commander walks the folder tree via `FsFindFirstW`/`FsFindNextW`/`FsFindClose`, creates the local directory hierarchy, and calls `FsGetFileW` for each file. Viewing files with `F3` (Lister) and `Ctrl+Q` (Quick View) also works automatically.
  - Implements Total Commander's two-step overwrite protocol (`FS_FILE_EXISTS` when `FS_COPYFLAGS_OVERWRITE` is not set; a read-only target is replaced when the flag is set), preserves `LastWriteTime` from `RemoteInfoStruct` (the "unknown time" marker is ignored), and reports progress to `ProgressProcW`: 0 % before the transfer, then the first chunk and at most one call per ~100 ms (capped at 99 %), 100 % on success. `ESC` -> `FS_FILE_USERABORT`. The timeout is an **inactivity** timeout (`WFX_RESTIC_TIMEOUT_MS`, 30 min without a single byte on stdout/stderr), so a slow multi-GB transfer is never killed.
  - Result mapping: `RESTIC_ERR_LOCAL_WRITE` (disk full, missing destination directory, destination is a directory) -> `FS_FILE_WRITEERROR` without popup; `PATH_NOT_FOUND`, invalid virtual path, unknown alias, root/repository/snapshot-level path -> `FS_FILE_NOTFOUND`; cancelled or empty password dialog -> `FS_FILE_USERABORT`; wrong password after 3 attempts and other Restic failures -> `FS_FILE_READERROR` with one popup. The password session (7.5) is shared with the listing code.
  - Rejects `FS_COPYFLAGS_MOVE` (`F6`, even combined with `OVERWRITE`) and `FS_COPYFLAGS_RESUME` with `FS_FILE_NOTSUPPORTED`; entries whose `RemoteInfoStruct.Attr` has `FILE_ATTRIBUTE_DIRECTORY` are refused too. Known limitation: Total Commander always passes `ri`, but a caller passing `ri == NULL` for a directory path would receive a tar stream (`restic dump` archives folders).

*Note:* Modification, deletion, and move functions (`FsPutFile`, `FsDeleteFile`, `FsRemoveDir`, `FsMkDir`, `FsRenMovFile`) are intentionally omitted to enforce strict read-only repository access.

The plugin exports **26 functions**: the 11 browsing/extraction entry points above, the 10 optional
content-column functions of §5.2 (7 ANSI + 3 Unicode variants), the `FsStatusInfo` /
`FsStatusInfoW` operation notifiers of §5.3 (ANSI and Unicode, used to detect `F3` on a folder),
the `FsExecuteFile` / `FsExecuteFileW` execute entry points of §5.3 (Alt+Enter and `quote stats`)
and `restic_test_force_f3`, a test-only hook that marks the next folder-size calculation as an `F3`
request so the automated tests can replay the real Total Commander sequence without a keyboard.
`FsContentSetValue` is *not* exported (no field is editable).

### 5.2 Custom Columns (`FsContent*`, read-only)

Total Commander calls these functions to display plugin fields in a custom columns view. All of them are cheap: the data is read from the snapshot statistics cache (`restic_stats.c`), which is filled when the snapshot list of an alias is enumerated.

| Function | Behaviour |
| --- | --- |
| `FsContentGetSupportedField` | Enumerates the 27 fields of the table below; returns `ft_nomorefields` after the last one. Field names avoid `.`, `\|` and `:` as required by the interface. |
| `FsContentGetValueW` / `FsContentGetValue` | Returns one field for the entry named by `FileName`. A **canonical** path (`<Alias>\<SnapshotDir>[\<SubPath>]`, resolved with `wfx_parse_path()`) is looked up in the cache of *that* alias only, so **files and folders inside a snapshot inherit the statistics of their snapshot**. Any other spelling (path relative to the current directory, plugin root name prepended, bare snapshot folder name - Total Commander's exact form is not documented) is resolved by scanning the path segments from the right (`lookup_loose`): a segment that parses as a snapshot folder name is accepted when `snapshot_stats_lookup_any()` finds exactly one cached snapshot, with two different snapshots sharing a short ID treated as ambiguous. Rows of the root and of an alias return `ft_fieldempty`; `CONTENT_DELAYIFSLOW` is ignored because nothing is slow. |
| `FsContentGetDefaultViewW` | Unicode variant of the default view: the four definitions are converted with `MultiByteToWideChar` into buffers of `maxlen` **bytes** (`maxlen / sizeof(WCHAR)` characters). |
| `FsContentStopGetValueW` | Unicode no-op, like `FsContentStopGetValue`. |
| `FsContentGetSupportedFieldFlags` | Always `0`: no field is editable (`contflags_edit`) and none substitutes a file property. |
| `FsContentGetDefaultSortOrder` | `-1` (newest/largest first) for the date, size and count fields, `1` otherwise. |
| `FsContentGetDefaultView` | Publishes `Date, Host, Tags, Files, Size (Mbytes), Added (Mbytes), Duration` with headers, widths (`200,60,110,120,140,-60,-70,-70,-60`; the first two are the name/extension columns) and options `0\|1`. The plugin's own fields are referenced with the **`<fs>` self name** (`[=<fs>.time]`, `[=<fs>.size.Mbytes]`), the form Total Commander stores in `wincmd.ini` for a file system plugin (see `[CustomFields_PluginName]` in the TC help and the `svn_wfx` sample plugin). The plugin's display name ("Restic Backups", proposed by TC at install time) is irrelevant to the built-in view and must never be hard-coded. |
| `FsContentStopGetValue` | No-op (no value is ever computed in the background). |
| `FsContentPluginUnloading` | Clears the snapshot statistics cache. |

Fields published (order = enumeration order):

| # | Field | Type | Units | Source in `snapshots --json` |
| --- | --- | --- | --- | --- |
| 0 | `time` | `ft_datetime` | - | `time` |
| 1 | `hostname` | `ft_string` | - | `hostname` |
| 2 | `tags` | `ft_string` | - | `tags` (joined with `, `) |
| 3 | `files` | `ft_numeric_64` | - | `summary.total_files_processed` |
| 4 | `size` | `ft_numeric_64` | `bytes\|kbytes\|Mbytes\|Gbytes` | `summary.total_bytes_processed` |
| 5 | `data_added` | `ft_numeric_64` | byte units | `summary.data_added` |
| 6 | `duration` | `ft_numeric_64` | `s\|min` | `backup_end - backup_start` |
| 7 | `short_id` | `ft_string` | - | `short_id` |
| 8 | `id` | `ft_string` | - | `id` |
| 9 | `parent` | `ft_string` | - | `parent` |
| 10 | `username` | `ft_string` | - | `username` |
| 11 | `paths` | `ft_string` | - | `paths` (joined with `; `) |
| 12 | `program_version` | `ft_string` | - | `program_version` |
| 13 | `backup_start` | `ft_datetime` | - | `summary.backup_start` |
| 14 | `backup_end` | `ft_datetime` | - | `summary.backup_end` |
| 15 | `files_new` | `ft_numeric_64` | - | `summary.files_new` |
| 16 | `files_changed` | `ft_numeric_64` | - | `summary.files_changed` |
| 17 | `files_unmodified` | `ft_numeric_64` | - | `summary.files_unmodified` |
| 18 | `dirs_new` | `ft_numeric_64` | - | `summary.dirs_new` |
| 19 | `dirs_changed` | `ft_numeric_64` | - | `summary.dirs_changed` |
| 20 | `dirs_unmodified` | `ft_numeric_64` | - | `summary.dirs_unmodified` |
| 21 | `data_blobs` | `ft_numeric_64` | - | `summary.data_blobs` |
| 22 | `tree_blobs` | `ft_numeric_64` | - | `summary.tree_blobs` |
| 23 | `data_added_packed` | `ft_numeric_64` | byte units | `summary.data_added_packed` |
| 24 | `avg_file_size` | `ft_numeric_64` | byte units | `total_bytes_processed / total_files_processed` |
| 25 | `packed_ratio` | `ft_numeric_floating` | - | `data_added / data_added_packed` |
| 26 | `has_summary` | `ft_boolean` | - | `summary` object present |

Rules and limitations:

- `FsContentGetDefaultView` receives **one `maxlen` for all four buffers**; the implementation writes each non-NULL buffer with `strncpy_s(dest, maxlen, src, _TRUNCATE)`, so every buffer supplied by the caller must be at least `maxlen` bytes long (both tests are sized accordingly and also assert the truncated-`maxlen` path).
- **Every summary-derived field degrades to `ft_fieldempty`** when the snapshot carries no `summary` object (snapshots written by older Restic versions or by other clients); the metadata fields stay available.
- **Text encoding:** Restic prints UTF-8. The ANSI content entry point (`FsContentGetValue`) must deliver text in the active code page (`CP_ACP`), so its fields are converted and characters the code page cannot represent become `?`; the Unicode entry point (`FsContentGetValueW`, see below) converts straight to UTF-16 and loses nothing. When the caller's buffer is too small the text is cut on a character boundary (the longest prefix that fits), never in the middle of a multi-byte sequence.
- **Numbers:** Total Commander reads `ft_numeric_64` as *signed*. Counters are therefore saturated at `RESTIC_STATS_COUNTER_MAX` (`LLONG_MAX`) both when parsed (converting a `double` beyond the integer range is undefined behaviour) and when written, so absurd values can never show up as negative numbers.
- **Aliases outside the ANSI code page:** the path reaches `FsContentGetValue` as ANSI text, so an alias such as `バックアップ` arrives as `?????`. Each cache slot therefore also stores the ANSI-degraded spelling of its alias; a lookup first tries the exact (case-insensitive) alias and only then the degraded spelling, and **only when exactly one slot matches**. If two aliases degrade to the same text the columns stay empty rather than showing the statistics of the wrong repository.
- **Cold start is filled on demand:** Total Commander can enter `\<Alias>\<SnapshotDir>[\<SubPath>]` directly (address bar, `Ctrl+D` hotlist, history) without ever listing the alias level, which used to leave the columns of that view empty until the user visited `\<Alias>`. `ensure_snapshot_stats_cached()` (called from `execute_repo_query`) now runs the same `restic snapshots --json` right after such a listing succeeded, **but only when the cache of that alias is still empty**. Consequences: the extra child process costs time once per alias and session (not per directory and not per cell), it reuses the password that just worked (no extra prompt, and a failure of that run is ignored), and `FsContentGetValue` itself still never starts Restic (see "no hidden restic run" above, which remains true for a column that is merely displayed, e.g. `\SecondRepo\<snap>` reached through a stored view).
- **Unicode vs ANSI entry points:** Total Commander is Unicode and, per the SDK, "will call the Unicode functions on all NT-based systems if they are present. If not, it will call the ANSI functions". Only `FsContentGetSupportedField`, `FsContentGetSupportedFieldFlags`, `FsContentGetDefaultSortOrder` and `FsContentPluginUnloading` exist in ANSI form only; `FsContentGetValueW`, `FsContentGetDefaultViewW` and `FsContentStopGetValueW` are therefore exported too. That is not cosmetic: with only the ANSI variants exported, a real Total Commander session showed the field list (ANSI-only function) but produced neither a default view nor any value, while an in-process harness calling the ANSI functions directly worked. The W entry points return `ft_stringw` for text fields and take the path as `WCHAR*` (no code-page round trip), so non-ANSI host names, tags and source paths are shown correctly; the `maxlen` of a W buffer counts **bytes**, so wide capacity is `maxlen / sizeof(WCHAR)`.
- **Default view is stored verbatim, once:** Total Commander calls `FsContentGetDefaultView`, saves the returned `ViewContents` into `wincmd.ini` under `[CustomFields_<PluginName>]` with `AutoLoad=1`, and then reuses that stored view without calling the function again. A stored view is therefore as authoritative as the DLL: the first real test left `Contents1=[=restic.time]...` (written by an earlier build that hard-coded `restic`), which does not resolve because the plugin is registered as "Restic Backups". The fix is to delete the `[CustomFields_<PluginName>]` section (TC then re-asks the plugin) or rewrite its `Contentsx`; never hard-code the display name, use `[=<fs>.field]`.
- **Path spellings:** the SDK only says "the name of the file (in plugin namespace)", so both the canonical form and the loose segment scan above are supported (`test_content_path_spellings` covers `Alias\Snap`, `\Alias\Snap`, `\\Restic Backups\Alias\Snap`, `...\sub\file.txt`, `Snap`, `Snap\sub\file.txt`, a trailing backslash). The loose scan is a *fallback*: when the canonical parse succeeds the alias is authoritative, so a snapshot ID cached under another alias never leaks into a different repository's rows. Inside the loose scan the segment in front of the snapshot name is tried as repository alias first and only then the cross-repository search runs, so two repositories that cache a snapshot with the same short ID (where the cross-repository lookup is deliberately ambiguous) still resolve to their own record.
- **Diagnostic trace (opt-in, `src/wfx_trace.c`):** enabled by `RESTIC_WFX_TRACE=1` **or** by a file named `restic_wfx_trace.on` next to the plugin DLL (the file works even when the variable was not inherited, which is the common case: Total Commander is single-instance, so starting it again only activates the running copy). Every event is appended to `%TEMP%\restic_wfx_trace.log` (fallback: the plugin directory), max 400 lines, file recreated by the first write of a *plugin load* (Total Commander loads the DLL once, so a normal session keeps everything; reloading the plugin starts a new file). Recorded: `FsInitW` (plugin number), `FsSetDefaultParams` (INI path), every `FsFindFirstW` (level + path, or "rejected"), the field enumeration, the default-view request and every `FsContentGetValue` (`value '<path>' field=N unit=N -> code`, with `(unresolved)` plus a `cache: [Alias(count) ...]` line when a value cannot be resolved). Every line is written as UTF-8: wide paths are converted with `wfx_trace_wide_to_utf8()` and printed with `%s`, because the narrow CRT `%ls` conversion fails in the `"C"` locale as soon as a character is outside ASCII (an accented user name would silently drop the whole line); the whole open/append/close sequence of a line runs under the trace `SRWLOCK`, so concurrent worker threads cannot interleave lines or truncate each other. Reading the log: (1) **no file** -> plugin not loaded, trace not enabled, or an older DLL is in use; (2) `FsInitW`/`findfirst` but **no `value` lines** -> Total Commander never asks for the fields, so the column definition does not point at this plugin (wrong plugin name) or the panel is not showing such a view; (3) `value ... -> -3 (unresolved)` -> the path spelling is still not recognised, and the log contains the exact string to support; (4) `-> -3` with `cache: []` -> the repository was never enumerated (cold start). `fscontent_trace_reset()` (delegating to `wfx_trace_reset()`) forgets the cached settings for the tests; production code never calls it.
- The column reference name for **user-created** views is the plugin key shown in **Configuration -> Options -> Plugins -> File system plugins** ("Restic Backups" on the test machine); the built-in default view uses `[=<fs>.field]` instead, so it does not depend on that key.
- Fields that need an extra Restic run (`restic stats --mode raw-data\|restore-size`, `restic diff` against the parent snapshot) are intentionally **not** published yet: they would turn a zero-cost column into one child process per refresh. They are recorded as future work in `MEMORY.md`.


### 5.3 Statistics Report (`F3` / `Alt+Enter`, Markdown)

The report is **not** a file of the listings: they contain exactly the snapshots and the files of the
backup. It is produced on demand for the folder the cursor is on and opened in the Total Commander Lister.

- **`F3` on a folder (CALCSIZE interception, `src/wfx_status.c` + `src/main.c`).** Total Commander has no
  "view a folder" entry point: `F3` on a directory starts a folder-size calculation, which is the sequence
  `FsStatusInfoW(dir, FS_STATUS_START, FS_STATUS_OP_CALCSIZE)` -> `FsFindFirstW(dir, ...)` ->
  `FsStatusInfoW(dir, FS_STATUS_END, FS_STATUS_OP_CALCSIZE)`. When that operation starts, `wfx_status_begin()`
  samples `GetAsyncKeyState(VK_F3)`: if `F3` is held down, a per-thread flag is set and the next
  `FsFindFirstW` of the same scope calls `show_stats_report_for_path()` and answers
  `INVALID_HANDLE_VALUE` + `ERROR_NO_MORE_FILES`. `wfx_status_is_f3_calcsize_aborted()` keeps every further
  `FsFindFirstW` of that scope answering "no files", so Total Commander never walks the tree of the folder
  being reported and the operation ends immediately. A space bar (folder size) leaves the flag unset and the
  normal calculation runs. A per-thread stack of depth 8 tracks nested operations and `FS_STATUS_END`
  forgets the request. `GetAsyncKeyState` is the only reason the plugin imports `USER32.dll`.
  - The tests drive this path through `restic_test_force_f3(TRUE)`, which sets the same flag instead of
    requiring a physical keystroke.
- **`FsExecuteFileW` (Alt+Enter, `quote stats`).** Per the SDK, `FS_EXEC_OK` also means "the command is not
  applicable and nothing more has to be done", `FS_EXEC_ERROR` means the execution failed. The verb
  `"properties"` (Alt+Enter on a folder) and `"quote stats"` (the user typed `stats`; `RemoteName` is the
  current directory; matching is case-insensitive, spacing is free, anything after `stats` is refused) call
  the same `show_stats_report_for_path()`. Its outcome (`StatsOutcome`) is mapped as: `STATS_SHOWN` and
  `STATS_NOT_APPLICABLE` -> `FS_EXEC_OK` (the plugin root - Total Commander sends `RemoteName="\"` +
  `"properties"` for a right click on the root -, a file, a cancelled password dialog, `ESC`),
  `STATS_FAILED` -> `FS_EXEC_ERROR` (unknown alias, missing `restic.exe`, wrong password, Restic failure; the
  message was already shown by `handle_restic_failure()`). `"open"` (Enter on a file) returns
  `FS_EXEC_YOURSELF`, so Total Commander downloads and opens the file as usual; every other verb returns
  `FS_EXEC_ERROR`. The ANSI `FsExecuteFile` only forwards `"open"`, because an ANSI path cannot carry a
  Unicode report destination. How Total Commander spells the `[..]` row for Alt+Enter is not documented and
  is not handled specially; verify it live.
- **`show_stats_report_for_path()` (`src/main.c`)** parses the path (`wfx_parse_path`), loads the
  configuration, finds the repository, builds the destination with `restic_report_build_temp_path()`, calls
  `restic_report_pick_destination()` (see below), `ProgressProcW(virtual_path, report_path, 0)`, runs the shared password/session loop (`session_get` /
  `prompt_repository_password` / `session_put`, three attempts, a wrong password is not cached), then
  `restic_report_generate()` and finally `restic_report_open_in_lister()`. Failures go through
  `handle_restic_failure()`; `ESC` stays silent. Only `WFX_PATH_REPO` and `WFX_PATH_SNAPSHOT_DIR` have a
  report (the plugin root lists aliases and has none). A path that Restic knows only as a **file** gives
  `RESTIC_ERR_PATH_NOT_FOUND`, reported as `STATS_NOT_APPLICABLE` without a message.
- **Destination (`restic_report_build_temp_path`).** `%TEMP%\restic_stats_<alias>.md`,
  `%TEMP%\restic_stats_<alias>_<short_id>.md`, `%TEMP%\restic_stats_<alias>_<short_id>_<leaf>.md`.
  Characters Windows rejects (`<>:"/\|?*` and control characters) become `_`, the alias is cut at 40
  characters and the leaf at 32, so the name always fits `MAX_PATH`. That speaking name is what the Lister
  shows in its title bar. `restic_report_pick_destination()` probes the file with `DELETE` access and full
  sharing: if an earlier Lister keeps it open without `FILE_SHARE_DELETE` (which would make the atomic
  `MoveFileExW` replacement fail) the name becomes `... (2).md` ... `(9).md`.
- **Opening (`restic_report_open_in_lister`).** `%COMMANDER_EXE%` (fallback: `GetModuleFileNameW(NULL)`) plus
  `"<exe>" /S=L "<file>"` through `CreateProcessW`; the process is not waited for. A test hook
  (`restic_report_set_lister_test_hook`) and the environment variable `RESTIC_WFX_NO_LISTER_GUI=1` replace
  the launch, so the automated tests never open a window.
- **Generation (`src/restic_report.c`).** `restic_report_generate()` dispatches on the level; every phase goes
  through `advance()` (progress + `ESC`):
  - repository: the snapshot list from the statistics cache (no process) or `snapshots --json`, then
    `stats --mode raw-data` (repository-wide); `stats --mode restore-size` runs **only** when no snapshot
    carries a summary block;
  - snapshot: the metadata from the cache (no process) or `snapshots --json <id>`, then
    `stats --mode raw-data <id>` and `diff --json <parent> <id>` when a parent exists; `restore-size` only
    when the snapshot has no summary;
  - subfolder: `ls --json --recursive <id> <filter>` only. The stream is consumed line by line through
    `restic_exec_lines()` (7.2) and aggregated incrementally (subfolder/extension buckets, six size buckets,
    top-15 largest files), so memory stays constant whatever the folder size; the snapshot metadata comes
    from the `struct_type: "snapshot"` line Restic prints first.
  - The logical restore size therefore comes from `total_bytes_processed` / `total_files_processed` of the
    most recent snapshot with a summary: the whole-tree scan of `stats --mode restore-size` is gone.
- **Cache.** One slot (`SRWLOCK`) keyed by (repository target, snapshot ID, subfolder) plus a 60 s TTL: a
  repeated request (F3 twice, or Alt+Enter right after F3) writes the cached Markdown to the destination
  without running Restic at all (the document is read back from the published file, so there is a single
  writer). `restic_report_clear_cache()` is called by `FsInitW`, `FsContentPluginUnloading` and
  `DllMain(DLL_PROCESS_DETACH)`, i.e. in the same places that wipe the password sessions; a cache hit does
  not verify the password, which is harmless because the data was produced by this very session.
- **Robustness.** Secondary query failures degrade the report (storage block marked unavailable, diff
  replaced by a note such as "Initial snapshot: there is no parent snapshot to compare with" or "the parent
  may have been removed with 'restic forget'"), while `ESC`, timeout, wrong password, missing repository and
  out-of-memory abort it - also when they hit a secondary query (`read_storage_stats()` returns fatal errors
  instead of blanking its block). An unknown snapshot ID (Restic prints `[]`), a path that Restic prints as a
  **file** rather than a directory, and a path that Restic does not echo back all give
  `RESTIC_ERR_PATH_NOT_FOUND`. The report is written to `<target dir>\~rprep<thread id>.tmp` and published
  with `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` only on success (`ferror`/`fclose` checked), so a cancelled
  generation never truncates an existing file; a read-only earlier copy has its attribute cleared first, as
  `restic_dump_file` does.
- **Progress.** `ResticReportProgressProc` receives rising percentages (5 % start, 20-25/45-55/65-80/85/90 %
  per phase, 15-85 % while streaming a folder) and returns TRUE on `ESC`. `main.c` uses one
  `WfxReportProgressContext` for both the progress and the cancellation callback: the cancellation polls
  about ten times per second and **repeats the last percentage**, so the bar only moves forward and the file
  name shown by Total Commander never changes (the first version reset it to 0 % every 100 ms, which looked
  like the dialog restarting). During a folder stream the clock is checked every 256 lines and the bar
  advances at most every 150 ms, so a huge folder neither floods the dialog nor stops updating.
- **Markdown output.** One document per level (`restic_report_write_repo_md`, `_snapshot_md`, `_subtree_md`):
  `# Restic <kind> report - <alias>`, then `## Storage and deduplication`, `## Snapshot history`,
  `## Files and directories`, `## Changes since the parent snapshot`, `## Size by subfolder`,
  `## Largest file types`, `## File size distribution`, `## Largest files`. Every table is a pipe table with
  fixed-width padding and an alignment row (`:---` / `---:`), so it is aligned as plain text in the Lister and
  renders in a Markdown viewer. Bars are pure ASCII: `#` filled, `.` empty (`[##########..........]`), because
  the Unicode blocks U+2588/U+2591 touched vertically and changed width through font fallback.
  `restic_report_write_text()` only neutralises what would break a cell (a `|` becomes `/`, CR/LF/TAB become
  spaces); `<`, `>`, `&` and quotes are written verbatim, since the document is no longer HTML. Tables that
  add up: "files in this folder" + the 15 biggest subfolders + `(other subfolders)`, and the 12 biggest
  extensions + `(other)`, computed by subtraction from the totals. A name without a dot, ending with a dot, or
  whose only dot is the first character (`.gitignore`) has the extension `(no ext)`; empty direct subfolders
  get a (0 B) row; the history has Files, Size and Data added columns (`-` for a snapshot without summary).
  The repository location is masked by `restic_report_redact_location()` (`rest:https://user:secret@host`
  becomes `rest:https://***@host`), because the file can be copied or shared. `copy_utf8()` truncates on a
  character boundary, so long paths never produce invalid UTF-8.
- **Limits.** At most 100 diff changes, 128 subfolders, 128 extensions and 15 largest files are collected; a
  file below the 128th subfolder is still counted in the totals and in the remainder row.

### 5.1 Plugin Icon Specification (Total Commander WFX Standard)

- **Network Neighborhood Root Icon (implemented):** Total Commander automatically displays the **first `ICON` resource (by index)** embedded inside `restic.wfx64` to the left of `"Restic Backups"` in Network Neighborhood (no extra exported function is required). Per the official WFX specification:
  - Format: `logo\restic_wfx.ico` linked into `restic.wfx64` via `src\restic_wfx.rc` (`rc.exe /nologo /fobuild\restic_wfx.res src\restic_wfx.rc`).
  - Included frames inside `logo\restic_wfx.ico` (5 resolutions): **16×16, 24×24, 32×32, 48×48** (32-bit BGRA uncompressed BMP/DIB frames) plus **256×256** (32-bit RGBA PNG-compressed), built from `logo\logo_16x16.ico`, `logo\logo_24x24.ico`, `logo\logo_32x32.ico`, `logo\logo_48x48.ico`, and `logo\logo_256x256.png`.
- **Per-Item Custom Icons (`FsExtractCustomIconW`, optional):** only needed if custom icons are desired for individual repositories or snapshots inside the file list (`FS_ICONFLAG_SMALL` requests 16×16, otherwise 32×32). When omitted, Total Commander uses standard folder icons and normal Windows file-extension associations.
- **DLL Version Resource (`VS_VERSION_INFO`, implemented):** `include\version.h` defines the single source of truth for the plugin version (`1.3.1.0` / `"1.3.1"`), embedded into `restic.wfx64` via `src\restic_wfx.rc` so Windows Explorer and Total Commander display the version and metadata in file properties ("Details" tab).

---

## 6. Source Tree Structure

```text
Restic_WFX_Plugin/
├── Makefile                    # NMake incremental build & test rules (Debug/Release/ASan/Analyze)
├── build.sh                    # Bash wrapper (expects an initialized MSVC x64 environment) invoking NMake
├── .clang-format               # Code formatting rules (C11, 4 spaces, 100 cols)
├── restic_wfx.def              # Explicit DLL exports to prevent MSVC name mangling
├── restic_plugin.ini           # Commented sample configuration file (copied to build\restic_plugin.ini)
├── pluginst.inf                # Total Commander auto-install manifest (copied to build\ and packed in the ZIP)
├── .agents/skills/             # Agent skills: memory-updater, kroki-diagrams, release-packager (scripts/release.py: bump version, docs, tests, ZIP)
├── logo/                       # Plugin icons (.ico 16/24/32/48 + multi-res restic_wfx.ico) & PNG logos (256/512/2048)
├── cJSON/
│   ├── cJSON.h                 # cJSON parser header
│   └── cJSON.c                 # cJSON parser implementation
├── include/
│   ├── fsplugin.h              # Total Commander WFX API declarations (+ FS_SET_TIME_UNKNOWN, ft_* constants)
│   ├── fscontent.h             # Content-column plugin name macro, field-count helper, trace reset hook
│   ├── wfx_trace.h             # Opt-in diagnostic trace interface (environment variable or switch file)
│   ├── config.h                # INI configuration & binary resolution interface
│   ├── restic_cli.h            # Restic CLI child process & JSON/NDJSON parser interface
│   ├── restic_stats.h          # ResticSnapshotInfo, snapshots --json parser, per-alias cache API
│   ├── wfx_path.h              # Plugin-namespace path decomposition (WfxParsedPath)
│   ├── wfx_status.h            # FsStatusInfo operation stack + report visibility filter
│   ├── restic_report.h         # Statistics report: data model, parsers, Markdown writers, Lister
│   └── version.h               # Single source of truth for plugin version macros (1.3.1)
├── src/
│   ├── main.c                  # DllMain, WFX entry points, virtual path routing
│   ├── config.c                # restic_plugin.ini resolution & parsing
│   ├── restic_cli.c            # Child process execution, Win32 pipes, JSON/NDJSON parsing
│   ├── restic_stats.c          # Snapshot statistics parser + per-alias LRU cache (SRWLOCK)
│   ├── wfx_path.c              # Path decomposition shared by routing and content columns
│   ├── fscontent.c             # FsContent* custom columns (field table, values, default view)
│   ├── wfx_status.c            # FsStatusInfo(W) entry points, per-thread stack, F3-on-folder detection
│   ├── restic_report.c         # Statistics report: restic queries, NDJSON aggregation, Markdown
│   ├── wfx_trace.c             # Diagnostic trace: session header, %TEMP% log, switch file, 400-line cap
│   └── restic_wfx.rc           # Windows resource script (ICON #1 + VS_VERSION_INFO metadata)
└── tests/
    ├── test_config.c           # Configuration, PATH-lookup security, 300-repo scaling, root enumeration
    ├── test_restic_cli.c       # Parsers, quoting, process execution (cmd.exe stand-in) + live restic.exe tests
    ├── test_stats.c            # Path decomposition, statistics parser, cache, FsContent* columns
    ├── test_report.c           # Statistics report: bars, parsers, F3 interception, Markdown writers, live run
    └── test_plugin_dll.c       # Black-box smoke test: LoadLibrary(restic.wfx64) + exported WFX surface
```

---

## 7. Low-Level Implementation Details

### 7.1 Reentrant Search Handle (`ResticFindHandle`)

Total Commander may open directories concurrently across both panels or during recursive operations. Each `FsFindFirstW` / `FsFindNextW` cycle maintains isolated state:

```c
typedef struct {
    WIN32_FIND_DATAW* items; /* Array of entries discovered for the target path */
    size_t count;            /* Total number of entries in items */
    size_t current_index;    /* Next entry index to return in FsFindNextW */
} ResticFindHandle;
```

### 7.2 Invoking `restic.exe` via Win32 Pipes (`src/restic_cli.c`)

`restic_exec_command(exe, repo, password, args, options, out)` is the single process-spawning primitive:

- **Executable:** must contain a path separator (bare names refused, see 3.3). The command line is built in a heap buffer sized from the inputs (limit 32767 chars, `RESTIC_ERR_INVALID_ARG` beyond it) - never in a fixed stack buffer, because `swprintf_s` aborts the host process on overflow.
- **Two pipes, two streams:** stdout and stderr use separate anonymous pipes. With `--json` stdout carries only the payload, while `{"message_type":"exit_error",...}` and warnings go to **stderr** (verified against restic 0.19.1). Merging them would corrupt JSON parsing whenever Restic prints a warning.
- **stdin = `NUL`** so Restic can never block on an interactive prompt.
- **Explicit handle whitelist:** `STARTUPINFOEXW` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` makes the child inherit exactly {stdin NUL, stdout pipe, stderr pipe}. Total Commander spawns processes from other threads; without the whitelist a concurrent `CreateProcess` could inherit our write ends (EOF would never arrive) and unrelated inheritable handles would leak into Restic. Parent-side read ends are non-inheritable.
- **Environment block** (`CREATE_UNICODE_ENVIRONMENT`): the parent environment (Go needs `SystemRoot`, `APPDATA`, `TEMP`...) minus any inherited `RESTIC_REPOSITORY`, `RESTIC_REPOSITORY_FILE`, `RESTIC_PASSWORD`, `RESTIC_PASSWORD_FILE`, `RESTIC_PASSWORD_COMMAND` (they would conflict), plus `RESTIC_REPOSITORY` and `RESTIC_PASSWORD`. The block is wiped with `SecureZeroMemory` right after `CreateProcessW`. The password is never placed on the command line.
- **Output collection without deadlock:** one loop polls both pipes (`PeekNamedPipe` + `ReadFile`), waits at most `IDLE_WAIT_MS` (1 ms) on the process handle and only when nothing was read, and performs a final drain after the child exits. The stdout pipe has a 64 KB kernel buffer (`STDOUT_PIPE_BYTES`): with the default 4 KB Restic blocked on `WriteFile` whenever the reader was a scheduling quantum late. The same 1 ms idle wait is used by `restic_dump_file`; measured on a 300 MB file the extraction went from about 170 to about 275 MB/s once the last 20 ms wait was gone. The streaming loop must test "did `ReadFile` return bytes" (`line_stream_drain(..., &read_any)`), **not** the length of the pending buffer: that buffer shrinks back to the unfinished line after every batch, so comparing lengths was always "no progress" and every batch slept. Buffers grow geometrically and are always NUL-terminated and non-NULL after a successful spawn.
- **Timeout and cancellation:** `ResticExecOptions { timeout_ms, cancel_proc, cancel_ctx }`. `timeout_ms` is an **inactivity** timeout (no byte on stdout/stderr for that long, measured from the last byte), so a long scan that keeps producing output is never killed. `cancel_proc` is polled about every 100 ms (intended to forward Total Commander's `ProgressProc` abort result); on timeout/cancel the child is terminated (`RESTIC_ERR_TIMEOUT` / `RESTIC_ERR_CANCELLED`). `NULL` options mean no limit. `main.c` must always pass a cancel callback: an unreachable `rest:`/cloud repository makes Restic retry for minutes and would otherwise freeze Total Commander.
- **Exit codes:** `0` -> `RESTIC_OK`, `10` -> `RESTIC_ERR_REPO_NOT_FOUND`, `11` -> `RESTIC_ERR_REPO_LOCKED`, `12` -> `RESTIC_ERR_WRONG_PASSWORD`, anything else -> `RESTIC_ERR_CLI_FAILED`. `error_message` receives the `"message"` of the first JSON line on stderr (then stdout), or the raw trimmed text, safely truncated to 511 characters.
- **Streaming variant (`restic_exec_lines()`):** same spawn path (`spawn_restic_command()`: bare-name refusal, command line limit, credential environment, handle whitelist) and the same exit-code mapping (`classify_exit_code()`), but stdout is handed line by line to a `ResticLineProc` instead of being buffered. A line is delivered without its terminator (`\n`, and a trailing `\r` as well); empty lines are delivered; a last line without terminator is delivered at the end; the pending buffer is compacted after every chunk, so memory does not grow with the stream. The callback returning `FALSE`, the cancellation callback or the timeout terminate the child (`RESTIC_ERR_CANCELLED` / `RESTIC_ERR_TIMEOUT`). Used by the statistics report for `restic ls --recursive` and `restic diff`.

### 7.3 Argument Safety (`restic_list_directory`)

- Snapshot IDs go on the command line unquoted, so they are validated as hexadecimal (max 64 chars) before anything is spawned; `"--help"`, whitespace or `NULL` yield `RESTIC_ERR_INVALID_ARG`.
- The `ls` filter path (arbitrary bytes from the backed-up file system, e.g. Linux names containing a double quote) goes through `restic_quote_arg()`, which implements the `CommandLineToArgvW` escaping rules (backslashes before quotes doubled, quotes escaped). A round-trip test against `CommandLineToArgvW` covers it.
- Paths up to 1023 UTF-16 units (Total Commander's limit) are supported end to end (UTF-8 buffer of 3073 bytes).
- Known limitation: a Linux file name containing a literal backslash cannot be addressed, because the backslash is the WFX path separator.

### 7.4 Parsing Rules (`restic_parse_*`)

- `snapshots --json`: a literal `null` is an empty list; entries with an invalid time or non-hex ID are skipped (the directory name must never contain separators). Directory name = snapshot time **as recorded by Restic (its own UTC offset)**; the `FILETIME` fields are converted to UTC.
- Snapshot folder names (`restic_extract_snapshot_id`): only the exact shape produced by `restic_format_snapshot_dirname()`, `YYYY-MM-DD_HH-MM-SS_[<hex>]`, is accepted - the `[` must start the name or follow the `_` separator and the `]` must end it, so a backup file named `report_[deadbeef].txt` can never be parsed as a snapshot (this also protects the loose content-column lookup, which scans path segments for snapshot names).
- `ls --json` NDJSON: strict - a malformed line aborts with `RESTIC_ERR_JSON_PARSE` (stdout contains JSON only). Only direct children of the filter path are returned. The filtered directory's own node is consumed to prove existence: a filter other than `/` whose node is missing or is not a `dir` yields `RESTIC_ERR_PATH_NOT_FOUND`. This distinguishes "missing" from "empty directory", which Total Commander needs (`ERROR_NO_MORE_FILES` = exists but empty, any other error = does not exist).
- Entry mapping: `dir` -> `FILE_ATTRIBUTE_DIRECTORY`; everything else (files, **symlinks**, devices) -> plain file, as the WFX specification requires ("Links should be returned as files"). All entries carry `FILE_ATTRIBUTE_READONLY`. 64-bit sizes are split into `nFileSizeHigh/Low`.
- Entry names: `""`, `"."`, `".."` and any name containing `\` or `/` are skipped. A backup of a Linux/macOS filesystem can legitimately contain a separator inside a file name, but Total Commander would read it as a directory boundary (and the recursive `F5` copy would write the file outside the target directory), so those nodes stay invisible; use `restic restore` for them. The conversion buffer of `utf8_to_wide_trunc()` is cut on a UTF-8 character boundary, so a truncated Restic message never ends with a replacement character.
- **Backups started from a network share (UNC):** `restic backup \\host\share\dir` stores a root node whose *name* is `\\host\share` (it contains separators), and, on Windows, `restic ls <snapshot> /` prints **nothing** for such a snapshot (restic's own path functions read `/\\host\share` as a volume), although `ls`/`dump` of `/\\host\share[/...]` work. Consequences handled in `restic_cli.c`: (1) the volume name is shown as a folder whose backslashes are replaced by U+FF3C FULLWIDTH REVERSE SOLIDUS (`RESTIC_UNC_SEPARATOR_DISPLAY`), e.g. `＼＼host＼share`; (2) `restic_normalize_filter_path()` maps a **first** path segment that starts with two U+FF3C back to `\\host\share` (names with one U+FF3C, or U+FF3C deeper in the path, are left untouched); (3) `restic_list_directory()` for the snapshot root additionally runs `restic snapshots --json --no-lock <id>` and `restic_add_unc_volumes()` adds the volume (`\\host\share`, taken from the snapshot `paths`; device paths `\\?\...`, incomplete ones and duplicates are ignored) when restic did not print it; a failure of that extra run leaves the listing as restic printed it, `ESC` cancels the whole listing. Regression tests: `test_unc_volume_names`, `test_unc_volumes_from_snapshots`, the live `test_live_unc_share` (back up through `\\localhost\C$\...`, skipped when the administrative share is unreachable) and the DLL-level `test_e2e_unc_share` (alias `UncRepo`).
- Unknown dates: when `mtime` is missing or unparsable the entry gets Total Commander's "no date" marker (`FS_SET_TIME_UNKNOWN`: high `0xFFFFFFFF`, low `0xFFFFFFFE`); repository aliases at the root use it too.

### 7.5 Virtual Path Routing, Password Session & Host Callbacks (`src/main.c`)

- **Path decomposition (`wfx_parse_path`, `src/wfx_path.c`):** splits the backslash-delimited path passed to `FsFindFirstW` into:
  - `WFX_PATH_ROOT` (`\`): calls `enumerate_root_repositories()`.
  - `WFX_PATH_REPO` (`\<RepoAlias>`): resolves `RepoAlias` via `config_find_repo()` and calls `restic_list_snapshots()`.
  - `WFX_PATH_SNAPSHOT_DIR` (`\<RepoAlias>\<SnapshotDir>[\<SubPath>]`): extracts the hexadecimal `snapshot_id` via `restic_extract_snapshot_id()` (rejecting malformed folder names before prompting or spawning) and calls `restic_list_directory()`.
- **Pre-flight binary check:** if `cfg.restic_path` contains no path separator (`config_resolve_restic_path` returned `FALSE`), `execute_repo_query()` logs and displays `"restic.exe not found..."` and returns `ERROR_FILE_NOT_FOUND` without prompting for a password.
- **In-memory password sessions:** `g_Sessions[8]` (`SessionSlot { alias, password }`) is guarded by an `SRWLOCK` (statically initialized, so it also works in unit tests where `DllMain` never runs) because Total Commander may call the plugin from worker threads. `execute_repo_query()` copies the password into a local buffer (`session_get`), runs Restic **without** holding the lock, and wipes the local copy with `SecureZeroMemory`. A password is cached (`session_put`) only when the outcome does not prove it wrong (`RESTIC_OK`, `PATH_NOT_FOUND`, `CANCELLED`, `TIMEOUT`); wrong passwords, missing repositories and spawn failures are never kept. `session_forget` / `clear_password_session` wipe one slot / all slots.
- **Password prompt & retry:** when no password is cached for the alias, `prompt_repository_password()` calls `RequestProcW(RT_Password)` with `ReturnedText[0] = 0` (the buffer is the dialog's default text). An **empty** password or a cancelled dialog yields `ERROR_CANCELLED` without any popup (Restic rejects empty passwords, and accepting one would only produce a confusing generic failure). Up to `MAX_PASSWORD_ATTEMPTS` (3) prompts are shown when Restic keeps answering `RESTIC_ERR_WRONG_PASSWORD` (exit code 12); a stale cached password counts as the first attempt and is forgotten.
- **User abort (`ESC`):** `wfx_cancel_callback()` polls `ProgressProcW` every ~100 ms while Restic runs and terminates `restic.exe` on a non-zero result (`ERROR_CANCELLED`, no popup, a valid cached password is kept). Per the WFX SDK, calls made during the `FsFindFirst/Next/Close` loop are ignored by Total Commander for the first 5 seconds and only then show a progress dialog, so fast listings never flicker. The call uses `PercentDone = 0` (indeterminate) and passes the **alias, never the repository URL**, as `TargetName`: the URL can embed credentials (`rest:https://user:pass@host`) and would be shown on screen. A 30-minute `WFX_RESTIC_TIMEOUT_MS` is a safety net against hung commands.
- **Transfer log:** after a successful extraction, `FsGetFileW` calls `g_LogProc(MSGTYPE_TRANSFERCOMPLETE)` with the documented format `Download complete: <remote> -> <local>`, so the copy shows up in Total Commander's log window / log file next to the errors reported with `MSGTYPE_IMPORTANTERROR`.
- **Win32 error mapping:** empty listings (`RESTIC_OK` with `count == 0`) set `ERROR_NO_MORE_FILES`; missing paths or repositories set `ERROR_PATH_NOT_FOUND`; wrong passwords after retries set `ERROR_ACCESS_DENIED`; locked repositories set `ERROR_LOCK_VIOLATION`. User-facing errors are reported via both `g_LogProc(MSGTYPE_IMPORTANTERROR)` and `g_RequestProc(RT_MsgOK)`.

### 7.6 Snapshot Statistics Cache & Content Columns (`restic_stats.c`, `fscontent.c`)

- **One child process, two parsers:** `restic_fetch_snapshots_json()` returns the raw stdout of `restic snapshots --json --no-lock`; `list_snapshots_and_cache_stats()` (in `main.c`) feeds it to `restic_parse_snapshots_json()` (directory entries) **and** to `restic_parse_snapshot_list()` (statistics records). The records are stored with `snapshot_stats_store()` before the directory handle is returned, so the columns are already populated when Total Commander starts asking for values. A statistics parse failure never fails the listing: the alias cache entry is invalidated instead (consequence: while the cache stays empty, every level 2 listing of that alias repeats the cold-start fill; this cannot normally happen because the same JSON was just accepted by the listing parser).
- **`ResticSnapshotInfo`** (fixed-size arrays, no pointers) holds `id`, `short_id`, `parent`, `hostname`, `username`, `program_version`, joined `tags`/`paths` (with their element counts), `time`, the whole `summary` block and the derived `duration_seconds`. Fixed arrays make a record trivially copyable across threads without lock lifetime issues.
- **Parser rules:** a literal `null` is an empty repository; entries without a parsable `time` or with a missing/non-hexadecimal ID are skipped exactly like in the listing parser (so both results stay index-aligned; this is unit-tested by comparing the two parsers on the same mixed input); `short_id` falls back to the first 8 characters of `id`; numbers are read from `valuedouble`: negative, missing, non-numeric (e.g. `"7"`) values stay `0` and values at or above 2^63 saturate at `RESTIC_STATS_COUNTER_MAX`; joined `tags`/`paths` are cut at their fixed buffer without leaving a dangling separator while `tag_count`/`path_count` keep counting every element.
- **Cache:** `g_Cache[8]` slots keyed by repository alias (`_wcsicmp`), guarded by a statically initialized `SRWLOCK` (usable in unit tests where `DllMain` never runs); writers (`store`, `invalidate`, `clear`) take it exclusively, the many readers (`lookup`, `lookup_alias`, `cached_aliases` -> one call per visible cell) take it **shared**; `store`/`invalidate` only ever match the exact alias, never the ANSI-degraded spelling. `snapshot_stats_lookup()` returns a **copy** of one record; `snapshot_stats_lookup_alias()` copies up to `capacity` records and reports the total (used by tests). `snapshot_stats_invalidate`/`snapshot_stats_clear` free the arrays; `FsContentPluginUnloading`, `FsInitW` and `DllMain(DLL_PROCESS_DETACH)` clear the whole table. When all slots are busy the least recently stored alias is recycled with `GetTickCount64()` as the stamp.
- **Field rendering (`fscontent.c`):** a static `ContentField` table holds name, `ft_*` type and units; `render_field()` validates the buffer size (`maxlen`) before every write, scales byte counts to the requested unit (1024-based) and returns `ft_fieldempty` for missing values (`ft_nosuchfield` for an unknown index, `ft_fileerror` for a NULL buffer). `resolve_snapshot()` converts the ANSI `FileName` to UTF-16 (`CP_ACP`), decomposes it with `wfx_parse_path()` and only accepts `WFX_PATH_SNAPSHOT_DIR` (deeper paths inherit the snapshot).

---

## 8. Compilation, Hardening & Quality Standards (MSVC `cl.exe`)

Because the plugin runs in-process inside `TOTALCMD64.EXE` and handles dynamic memory, string/JSON parsing, and external processes, strict compiler and linker practices are mandatory:

### 8.1 Strict Compiler Warning & Standard Flags
- `/TC`: Force compilation strictly as pure C (reject any C++ constructs).
- `/std:c11`: Enforce modern ISO C11 standard.
- `/permissive-`: Disable non-conforming MSVC language extensions.
- `/W4`: Enable high warning level across all project source files (third-party `cJSON.c` is compiled with `/W3`).
- `/WX`: Treat all warnings as errors so implicit conversions or uninitialized variables cannot slip through.
- `/we4013`: Treat implicit function declarations (missing prototypes) as errors—critical on 64-bit x64 architectures to prevent pointer truncation.

### 8.2 Static CRT Linking (`/MT` & `/MTd`)
- **Release (`/MT`):** Links the C Runtime statically into `restic.wfx64`. The plugin works out-of-the-box on any clean Windows installation without requiring the user to install matching *Visual C++ Redistributable* packages (`VCRUNTIME140.dll`).
- **Debug (`/MTd`):** Static Debug CRT enabling automatic heap integrity and leak checks (`_CrtCheckMemory`, `_CRTDBG_LEAK_CHECK_DF`).

### 8.3 Runtime Bug Detection & Sanitizers
- **AddressSanitizer (`/fsanitize=address /Zi`):** Detects out-of-bounds accesses, use-after-free, and double-free bugs at runtime (`./build.sh asan`).
- **CRT Debug Heap (`<crtdbg.h>`):** Enabled in Debug builds (when ASan is not active) via `_CRTDBG_MAP_ALLOC` and `_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF)`.

### 8.4 Static Analysis
- **MSVC Code Analysis (`/analyze`):** Deep data-flow inspection for `NULL` pointer dereferences, potential buffer overruns, and unchecked return values (`./build.sh analyze`).

### 8.5 Binary Security Hardening
- **Compiler:** `/GS` (Buffer Security Check / stack cookie protection).
- **Linker:** `/DYNAMICBASE` (ASLR - Address Space Layout Randomization) and `/NXCOMPAT` (DEP - Data Execution Prevention), plus `/DEF:restic_wfx.def` to export undecorated symbol names.
- **Export surface:** `cJSON` is compiled with `/DCJSON_HIDE_SYMBOLS`. Without it the library marks
  every API `__declspec(dllexport)` and the plugin would export ~90 `cJSON_*` functions next to the
  WFX entry points. `tests/test_plugin_dll.c` asserts the exact surface: the 15 read-only
  `Fs*` functions (11 browsing/extraction + `FsStatusInfo`/`FsStatusInfoW` + `FsExecuteFile`/
  `FsExecuteFileW`), the 10 content-column entry points and the `restic_test_force_f3` test hook are
  exported; `FsPutFile/DeleteFile/MkDir/RemoveDir/RenMovFile/SetAttr/SetTime` (and any `cJSON_*`)
  must stay absent, because Total Commander enables an operation as soon as the symbol exists.
- **Imports:** `KERNEL32.dll` (static CRT) plus `USER32.dll` - the latter only for
  `GetAsyncKeyState(VK_F3)`, which tells `F3` on a folder from the space bar inside
  `FS_STATUS_OP_CALCSIZE` (`SYSLIBS = User32.lib`; the tests also use `Shell32.lib` for
  `CommandLineToArgvW`).

### 8.6 Build & Test Commands (`build.sh` & `Makefile`)

```bash
./build.sh debug     # Incremental Debug build (build/restic.wfx64, /MTd)
./build.sh release   # Optimized Release build (build/restic.wfx64, /MT)
./build.sh test      # Build plugin + run all five test programs (CRT heap & leak verification)
./build.sh release-test  # Same tests against the optimized /MT /O2 build
./build.sh asan      # Rebuild and run unit tests under MSVC AddressSanitizer (DLL smoke test is skipped)
./build.sh analyze   # Rebuild plugin & tests with MSVC Static Analyzer (/analyze)
./build.sh package   # Clean, Release build + tests, then build/wfx_restic_<version>_x64.zip (Total Commander installable package)
./build.sh clean     # Remove build/ directory (required when switching between Debug/Release/ASan/Analyze)
```

**Release package (`package`):** the ZIP is created by an inline `python -c` command in the `Makefile` (`package` target; Python is available in the shell). The version is read from `include/version.h` (`RESTIC_WFX_PRODUCT_VER`) and cross-checked against `pluginst.inf` (`version=` and `file=restic.wfx64`) before zipping. Contents (all in the ZIP root, otherwise Total Commander does not detect `pluginst.inf`): `pluginst.inf`, `restic.wfx64`, `restic_plugin.ini`, `README.md`, `LICENSE`. **Releases are prepared with the `release-packager` skill** (`.agents/skills/release-packager/`): `python .agents/skills/release-packager/scripts/release.py X.Y.Z` bumps every version reference, runs Debug/`/analyze`/ASan/Release tests, creates and verifies the ZIP (and a `.sha256`). **Manually, when bumping the version, update both `include/version.h` and `pluginst.inf`** (`test_plugin_dll.c` fails if they diverge). Per the WFX SDK, a 64-bit plugin must carry the `64` suffix (`restic.wfx64`); `pluginst.inf` uses `type=wfx`, `defaultdir=restic`.

If Total Commander still has `build/restic.wfx64` loaded, `build.sh` renames the locked DLL to `restic.wfx64.old` so the link can proceed; run `cm_UnloadPlugins` in Total Commander (or restart it) to load the new build, then delete the `.old` file.


### 8.7 Test Suites (`tests/`)

| Program | Scope |
| --- | --- |
| `test_config.c` | Directory normalization, INI primary/fallback priority (verifying no INI file is auto-created when missing), `restic.exe` resolution order, **PATH-only lookup (a planted `restic.exe` in the cwd must be ignored)**, `[Repositories]` parsing edge cases, 301-alias scaling (array and INI-section buffer growth), root enumeration through `FsFindFirstW/Next/Close`, WFX path validation, missing-`restic.exe` diagnostic, password prompt cancellation, the **missing-INI scenario** (`FsInitW`/`FsSetDefaultParams`/`FsFindFirstW`/`FsGetFileW` stay inert, no password prompt or popup, and plugin/fallback directories remain empty), and verification that the staged `build\restic_plugin.ini` sample template is valid and has 0 active repositories, plus the **ANSI-unmappable `DefaultIniName`** cases (a `?` in the converted path or an empty name falls back to `%COMMANDER_INI_PATH%`, trailing separators are stripped, a usable name still wins, and no source leaves the fallback empty) (85 assertions). |
| `test_restic_cli.c` | ISO-8601 edge cases (exact epoch value, offsets, fractions, invalid calendar dates), snapshot naming, path normalization (1023-char CJK path), `restic_quote_arg` round-trip, snapshot/NDJSON parsers (prefix-sharing siblings, missing/empty/file targets, symlinks, 64-bit sizes, corrupt lines, `\` and `/` inside a node name), snapshot-name strictness (`report_[deadbeef].txt` and `file[deadbeef]` are refused, the exact `YYYY-MM-DD_HH-MM-SS_[<hex>]` shape is accepted), truncation of a long non-ASCII Restic error message on a character boundary (no `U+FFFD`), **UNC share backups** (display name <-> real name mapping in `restic_normalize_filter_path`, a UNC volume at the root, `restic_add_unc_volumes` with duplicates/device paths/garbage, and a live backup taken through `\\localhost\C$\...` that is listed, navigated and dumped), process execution with `cmd.exe` as a deterministic stand-in (stdout/stderr separation, exit codes, environment injection/stripping/inheritance, timeout, cancellation, >100 KB output, >6000-char command line, spawn failures), and a **self-contained live test** against `tmp\restic.exe` including `restic_dump_file` streaming extraction, mtime preservation, 0-byte and 100 KB files, overwrite protection, binary fidelity (`00/0A/0D/1A` bytes), existing target preserved on every failure or abort, no leftover `~rp*.tmp`, local-side failures, argument validation, plus `test_dump_with_fake_restic` (a fake `restic` batch file that deterministically covers exit codes 10/11/12/other, stderr classification, partial output then failure, empty stderr, environment injection and the inactivity timeout: a silent child is killed, a slow but steady one survives) plus **`restic_exec_lines`** (a 4000-line / >100 KB stream delivered line by line, CR LF terminators removed, empty lines and an unterminated last line, callback abort, exit-code mapping with earlier lines still delivered, timeout, cancellation, argument checks) (332 assertions). |
| `test_stats.c` | Path decomposition (`wfx_parse_path`: root, alias, snapshot, deep path, malformed/non-hex/empty IDs), statistics parser (`ResticSnapshotInfo` from a fixture with/without `summary`, tags/paths joining and counts, derived duration, `short_id` fallback, skipped entries, `null`/empty/malformed payloads, NULL arguments), per-alias cache (store/lookup/invalidate/clear, case-insensitive alias, replaced entries, 12-alias eviction, empty record list), and the `FsContent*` surface (field enumeration and naming rules, unit strings, values per field with unit scaling, fields inherited by files inside a snapshot, empty alias/root/unknown paths, `ft_nosuchfield`/`ft_fileerror`/`ft_fieldempty` edge cases, buffer truncation, delayed flag, default view/headers/widths/options consistency, NULL buffers, truncated `maxlen`, sort order, no field flags, unloading clears the cache) and the **Code Review 4 hardening tests** (statistics parser vs. listing parser stay index-aligned on non-hex/empty/non-ASCII IDs and bad dates, counters saturate at 2^63-1 for `1e30`/`2^64-1`/negative/string values and never turn negative through `FsContentGetValue`, 200-tag lists are cut without a dangling separator and still counted, UTF-8 host/tag/path text is converted to the ANSI code page and cut on a character boundary, emoji aliases resolve through their ANSI spelling, ambiguous spellings are refused, a literal `?` alias never overwrites a Unicode alias, 4 reader threads + 1 writer thread hammering the cache for 16 000 lookups), plus the **path-spelling and diagnostic tests** (8 accepted spellings of the same snapshot folder - relative, plugin root prepended, bare name, deep file, trailing backslash - with the canonical form still refusing another alias; `snapshot_stats_lookup_any` unique/unknown/NULL/duplicate-alias/conflicting-ID cases and the `snapshot_stats_describe` format; the `RESTIC_WFX_TRACE` switch writing nothing when unset, a full call record when set and UTF-8 paths (accent + CJK) in the log; UTF-8 truncation of 600-byte tags and of the diagnostic summary lands on a character boundary and still counts every alias; the loose lookup prefers the alias segment when two repositories share a short ID; the trace UTF-8 converters truncate on a character / surrogate-pair boundary instead of failing) (450 assertions). |
| `test_report.c` | Statistics report: ASCII bar formatting (0 %, tiny ratio, 100 %, out-of-range ratios, degenerate widths/buffers), byte/count/timestamp formatting, Markdown cell text (`|` becomes `/`, control characters become spaces, markup kept verbatim), the `FsStatusInfo`/`FsStatusInfoW` F3 detection (space bar vs. F3, one report per CALCSIZE scope, the aborted flag, the force helper, nested/unbalanced operations, ANSI entry point), `restic stats` JSON parsing (restore-size, raw-data, format-v1 fallback, garbage), `restic diff` NDJSON parsing (change list capped at 100 while every event is counted), `restic ls --recursive` aggregation (self node ignored, a file named by the filter is not a folder, sibling with a shared prefix ignored, direct vs. nested counts, histogram buckets summing to the totals, sorted subfolder/extension buckets, top-15 ranking with Windows separators, timestamps converted to UTC), the three Markdown writers with hostile file names (`<script>`, quotes, `&`) and the absence of HTML, report paths in `%TEMP%` (repository/snapshot/folder names, forbidden characters, length bounds, small buffer), the Lister opening (test hook, failing hook, `RESTIC_WFX_NO_LISTER_GUI`), and a live end-to-end generation for all three levels with the real `tmp\restic.exe` (progress callbacks, the RAM cache answering a repeated request with a single progress step, `restic_report_clear_cache`, `ESC` cancellation that leaves an existing file untouched, unknown snapshot/folder refused, argument validation), plus the review hardening tests (hidden-file and trailing-dot extensions, long paths cut on a UTF-8 boundary, the `(other ...)` remainder rows, credential masking in every writer, a history table with sizes, a live second backup whose report carries the diff, a read-only earlier copy replaced, `ESC` inside a secondary `restic stats`), the in-use report probe (`restic_report_pick_destination`: suffix numbering, read-only and missing files) and the real Lister launch through a stand-in executable (`/S=L`, quoting of a path with spaces and brackets, missing host) (280 assertions). |
| `test_plugin_dll.c` | Loads the built `restic.wfx64` like Total Commander: exact 26-function export surface (`FsGetFile`/`FsGetFileW` and the 10 `FsContent*` entry points included; write/delete/move functions forbidden), custom-column surface through `GetProcAddress` (field enumeration terminates, names unique, default view references `[=<fs>.` with one header per column, empty values for unknown paths, sort order, unloading), embedded `RT_GROUP_ICON` #1 + 5 `RT_ICON` frames (`16x16`..`256x256`), embedded `RT_VERSION` (`VS_VERSION_INFO` / `VS_FIXEDFILEINFO` matching `include/version.h`), root name, primary INI next to the DLL, fallback INI via `FsSetDefaultParams` (no file auto-created when missing), two concurrent find handles, ANSI stub behavior, and **end-to-end live navigation & extraction** across Level 0 -> Level 1 -> Level 2+, `FsGetFileW` single-file and recursive folder copy (`F5`), overwrite prompt protocol (`FS_FILE_EXISTS` -> `FS_COPYFLAGS_OVERWRITE`), `FS_COPYFLAGS_MOVE` rejection, binary fidelity and mtime, failed overwrite keeps the old file, local-side `FS_FILE_WRITEERROR`, invalid remote names, NULL arguments, `ESC` at start and mid-transfer, password flows inside `FsGetFileW` (cancel, 3 wrong, retry, cache reuse), password retry, per-repository sessions (8-slot eviction), empty/cancelled password, 3-strike failure, missing repository, 250-character alias error message, `ProgressProc` (`ESC`) cancellation and 6 concurrent worker threads, plus the statistics columns end to end on the live fixture (host name, `FILETIME`, short ID, file count vs. `has_summary`, inheritance for files inside a snapshot, empty columns for a repository that was never listed, independent entry per alias, cold start filled by one extra `restic snapshots` run when a snapshot folder is entered directly, one `MSGTYPE_TRANSFERCOMPLETE` with the documented `Download complete: <src> -> <dst>` text after a successful extraction, a snapshot of a UNC share navigated and extracted through the entry points, and the **statistics report** end to end: no artificial entry in any listing, F3 on a repository, a snapshot and a subfolder (the real `FsStatusInfoW` + `FsFindFirstW` sequence, with the CALCSIZE scope answering "no files" so the tree is never walked), the space bar still calculating the folder size, `FsExecuteFileW` with `properties`/`quote stats` producing the same Markdown (case/spacing tolerated, `quote statsx`/`quote ls` refused), `open` forwarded to Total Commander, a file or the plugin root answered `FS_EXEC_OK` without a message, an unknown repository `FS_EXEC_ERROR`, NULL arguments refused, all without a new password prompt and with the Lister suppressed by `RESTIC_WFX_NO_LISTER_GUI` (265 assertions in Debug/Release; skipped in ASan runs). |

The live test needs only `tmp\restic.exe` (git-ignored; reported as `[SKIP]` when absent). On every run it rebuilds `tmp\test_fixture\{data,repo}` from scratch (300-file directory, Unicode and shell-metacharacter names, empty directory, 100 KB file, fixed mtime), initializes a repository whose password contains quotes, `%PATH%` and CJK text, takes two snapshots and verifies per-snapshot listings, timestamps and the error mapping (wrong password, missing repository, unknown snapshot, missing path, file used as directory, quote-injection attempt). The fixture stays on disk for inspection.

Test-quality note: the security-relevant tests were mutation-checked (reverting to `SearchPathW`, or removing the quote escaping, makes them fail).
