# AGENT.md - Restic File System Plugin for Total Commander (WFX)

Technical specification, operational requirements, compiler hardening rules, and architectural guidelines for developing the Total Commander Restic plugin in standard C (C11).

---

## 1. Project Goal

Build a **File System Plugin (WFX)** extension for **Total Commander** written strictly in **pure standard C (C11)** (no C++ or other languages).

The primary goal of Phase 1 (MVP) is to provide interactive, read-only browsing of **Restic** backup repositories ([https://restic.net](https://restic.net)), allowing users to navigate seamlessly across configured repositories, snapshots, and directory trees directly within Total Commander's dual-pane interface.

**Current scope:** read-only browsing **plus extraction** of files and whole folders through Total Commander's native copy (`F5`), viewer (`F3`) and Quick View (`Ctrl+Q`). Repository modification (upload, delete, move/rename, mkdir) is deliberately out of scope and not exported.

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
   - Every value comes from the JSON that was **already downloaded** while enumerating the snapshot list: no field ever spawns `restic.exe`, no field is delayed (`ft_delayed`/`ft_ondemand` are never returned and deliberately not even defined in `include/fsplugin.h`) and no password prompt can be triggered from a background thread.
   - Parsed records live in a small volatile per-alias cache (8 slots, `SRWLOCK`, copies handed out, cleared on `FsContentPluginUnloading`/`FsInitW`/DLL unload). It is a *statistics* cache, not a directory cache: the `FsFindFirstW`/`FsFindNextW` listing path still queries the CLI on every navigation.

---

## 3. Configuration: `restic_plugin.ini`

### 3.1 INI File Path Resolution

1. **Primary Lookup:** inside the directory containing the plugin DLL (`<plugin_dir>\restic_plugin.ini`), resolved at load time in `DllMain` via `GetModuleFileNameW`.
2. **Fallback:** if `<plugin_dir>\restic_plugin.ini` does not exist (e.g., when installed under a shared/read-only path such as `C:\Program Files\`), the plugin looks for `restic_plugin.ini` inside the directory supplied by Total Commander via `FsSetDefaultParams` (`DefaultIniName`, typically `%APPDATA%\GHISLER\`).
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

The plugin exports **21 functions**: the 11 browsing/extraction entry points above plus the 10 optional content-column functions of §5.2 (7 ANSI + 3 Unicode variants). `FsContentSetValue` is *not* exported (no field is editable).

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
- **Text encoding:** Restic prints UTF-8, but the content interface of file system plugins is ANSI-only (no `W` variant exists), so every text field is converted to the active code page (`CP_ACP`); characters the code page cannot represent become `?`. When the caller's buffer is too small the text is cut on a character boundary (the longest prefix that fits), never in the middle of a multi-byte sequence.
- **Numbers:** Total Commander reads `ft_numeric_64` as *signed*. Counters are therefore saturated at `RESTIC_STATS_COUNTER_MAX` (`LLONG_MAX`) both when parsed (converting a `double` beyond the integer range is undefined behaviour) and when written, so absurd values can never show up as negative numbers.
- **Aliases outside the ANSI code page:** the path reaches `FsContentGetValue` as ANSI text, so an alias such as `バックアップ` arrives as `?????`. Each cache slot therefore also stores the ANSI-degraded spelling of its alias; a lookup first tries the exact (case-insensitive) alias and only then the degraded spelling, and **only when exactly one slot matches**. If two aliases degrade to the same text the columns stay empty rather than showing the statistics of the wrong repository.
- **Known limitation (cold start):** the cache is filled when `\<Alias>` is enumerated. If Total Commander shows a snapshot directory without that level having been listed in the current session (e.g. a path typed directly into the address bar), the columns stay empty until the user goes up to the alias level or refreshes it. Filling it implicitly would need an extra `restic snapshots` run from a UI-thread callback, which was rejected on purpose (see "no hidden restic run" above).
- **Unicode vs ANSI entry points:** Total Commander is Unicode and, per the SDK, "will call the Unicode functions on all NT-based systems if they are present. If not, it will call the ANSI functions". Only `FsContentGetSupportedField`, `FsContentGetSupportedFieldFlags`, `FsContentGetDefaultSortOrder` and `FsContentPluginUnloading` exist in ANSI form only; `FsContentGetValueW`, `FsContentGetDefaultViewW` and `FsContentStopGetValueW` are therefore exported too. That is not cosmetic: with only the ANSI variants exported, a real Total Commander session showed the field list (ANSI-only function) but produced neither a default view nor any value, while an in-process harness calling the ANSI functions directly worked. The W entry points return `ft_stringw` for text fields and take the path as `WCHAR*` (no code-page round trip), so non-ANSI host names, tags and source paths are shown correctly; the `maxlen` of a W buffer counts **bytes**, so wide capacity is `maxlen / sizeof(WCHAR)`.
- **Default view is stored verbatim, once:** Total Commander calls `FsContentGetDefaultView`, saves the returned `ViewContents` into `wincmd.ini` under `[CustomFields_<PluginName>]` with `AutoLoad=1`, and then reuses that stored view without calling the function again. A stored view is therefore as authoritative as the DLL: the first real test left `Contents1=[=restic.time]...` (written by an earlier build that hard-coded `restic`), which does not resolve because the plugin is registered as "Restic Backups". The fix is to delete the `[CustomFields_<PluginName>]` section (TC then re-asks the plugin) or rewrite its `Contentsx`; never hard-code the display name, use `[=<fs>.field]`.
- **Path spellings:** the SDK only says "the name of the file (in plugin namespace)", so both the canonical form and the loose segment scan above are supported (`test_content_path_spellings` covers `Alias\Snap`, `\Alias\Snap`, `\\Restic Backups\Alias\Snap`, `...\sub\file.txt`, `Snap`, `Snap\sub\file.txt`, a trailing backslash). The loose scan is a *fallback*: when the canonical parse succeeds the alias is authoritative, so a snapshot ID cached under another alias never leaks into a different repository's rows.
- **Diagnostic trace (opt-in, `src/wfx_trace.c`):** enabled by `RESTIC_WFX_TRACE=1` **or** by a file named `restic_wfx_trace.on` next to the plugin DLL (the file works even when the variable was not inherited, which is the common case: Total Commander is single-instance, so starting it again only activates the running copy). Every event is appended to `%TEMP%
estic_wfx_trace.log` (fallback: the plugin directory), max 400 lines, file recreated by the first write of a *plugin load* (Total Commander loads the DLL once, so a normal session keeps everything; reloading the plugin starts a new file). Recorded: `FsInitW` (plugin number), `FsSetDefaultParams` (INI path), every `FsFindFirstW` (level + path, or "rejected"), the field enumeration, the default-view request and every `FsContentGetValue` (`value '<path>' field=N unit=N -> code`, with `(unresolved)` plus a `cache: [Alias(count) ...]` line when a value cannot be resolved). Reading the log: (1) **no file** -> plugin not loaded, trace not enabled, or an older DLL is in use; (2) `FsInitW`/`findfirst` but **no `value` lines** -> Total Commander never asks for the fields, so the column definition does not point at this plugin (wrong plugin name) or the panel is not showing such a view; (3) `value ... -> -3 (unresolved)` -> the path spelling is still not recognised, and the log contains the exact string to support; (4) `-> -3` with `cache: []` -> the repository was never enumerated (cold start). `fscontent_trace_reset()` (delegating to `wfx_trace_reset()`) forgets the cached settings for the tests; production code never calls it.
- The column reference name for **user-created** views is the plugin key shown in **Configuration -> Options -> Plugins -> File system plugins** ("Restic Backups" on the test machine); the built-in default view uses `[=<fs>.field]` instead, so it does not depend on that key.
- Fields that need an extra Restic run (`restic stats --mode raw-data\|restore-size`, `restic diff` against the parent snapshot) are intentionally **not** published yet: they would turn a zero-cost column into one child process per refresh. They are recorded as future work in `MEMORY.md`.

### 5.1 Plugin Icon Specification (Total Commander WFX Standard)

- **Network Neighborhood Root Icon (implemented):** Total Commander automatically displays the **first `ICON` resource (by index)** embedded inside `restic.wfx64` to the left of `"Restic Backups"` in Network Neighborhood (no extra exported function is required). Per the official WFX specification:
  - Format: `logo\restic_wfx.ico` linked into `restic.wfx64` via `src\restic_wfx.rc` (`rc.exe /nologo /fobuild\restic_wfx.res src\restic_wfx.rc`).
  - Included frames inside `logo\restic_wfx.ico` (5 resolutions): **16×16, 24×24, 32×32, 48×48** (32-bit BGRA uncompressed BMP/DIB frames) plus **256×256** (32-bit RGBA PNG-compressed), built from `logo\logo_16x16.ico`, `logo\logo_24x24.ico`, `logo\logo_32x32.ico`, `logo\logo_48x48.ico`, and `logo\logo_256x256.png`.
- **Per-Item Custom Icons (`FsExtractCustomIconW`, optional):** only needed if custom icons are desired for individual repositories or snapshots inside the file list (`FS_ICONFLAG_SMALL` requests 16×16, otherwise 32×32). When omitted, Total Commander uses standard folder icons and normal Windows file-extension associations.
- **DLL Version Resource (`VS_VERSION_INFO`, implemented):** `include\version.h` defines the single source of truth for the plugin version (`1.1.0.0` / `"1.1.0"`), embedded into `restic.wfx64` via `src\restic_wfx.rc` so Windows Explorer and Total Commander display the version and metadata in file properties ("Details" tab).

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
│   └── version.h               # Single source of truth for plugin version macros (1.1.0)
├── src/
│   ├── main.c                  # DllMain, WFX entry points, virtual path routing
│   ├── config.c                # restic_plugin.ini resolution & parsing
│   ├── restic_cli.c            # Child process execution, Win32 pipes, JSON/NDJSON parsing
│   ├── restic_stats.c          # Snapshot statistics parser + per-alias LRU cache (SRWLOCK)
│   ├── wfx_path.c              # Path decomposition shared by routing and content columns
│   ├── fscontent.c             # FsContent* custom columns (field table, values, default view)
│   ├── wfx_trace.c             # Diagnostic trace: session header, %TEMP% log, switch file, 400-line cap
│   └── restic_wfx.rc           # Windows resource script (ICON #1 + VS_VERSION_INFO metadata)
└── tests/
    ├── test_config.c           # Configuration, PATH-lookup security, 300-repo scaling, root enumeration
    ├── test_restic_cli.c       # Parsers, quoting, process execution (cmd.exe stand-in) + live restic.exe tests
    ├── test_stats.c            # Path decomposition, statistics parser, cache, FsContent* columns
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
- **Output collection without deadlock:** one loop polls both pipes (`PeekNamedPipe` + `ReadFile`), waits at most 20 ms on the process handle only when idle, and performs a final drain after the child exits. Buffers grow geometrically and are always NUL-terminated and non-NULL after a successful spawn.
- **Timeout and cancellation:** `ResticExecOptions { timeout_ms, cancel_proc, cancel_ctx }`. `cancel_proc` is polled about every 100 ms (intended to forward Total Commander's `ProgressProc` abort result); on timeout/cancel the child is terminated (`RESTIC_ERR_TIMEOUT` / `RESTIC_ERR_CANCELLED`). `NULL` options mean no limit. `main.c` must always pass a cancel callback: an unreachable `rest:`/cloud repository makes Restic retry for minutes and would otherwise freeze Total Commander.
- **Exit codes:** `0` -> `RESTIC_OK`, `10` -> `RESTIC_ERR_REPO_NOT_FOUND`, `11` -> `RESTIC_ERR_REPO_LOCKED`, `12` -> `RESTIC_ERR_WRONG_PASSWORD`, anything else -> `RESTIC_ERR_CLI_FAILED`. `error_message` receives the `"message"` of the first JSON line on stderr (then stdout), or the raw trimmed text, safely truncated to 511 characters.

### 7.3 Argument Safety (`restic_list_directory`)

- Snapshot IDs go on the command line unquoted, so they are validated as hexadecimal (max 64 chars) before anything is spawned; `"--help"`, whitespace or `NULL` yield `RESTIC_ERR_INVALID_ARG`.
- The `ls` filter path (arbitrary bytes from the backed-up file system, e.g. Linux names containing a double quote) goes through `restic_quote_arg()`, which implements the `CommandLineToArgvW` escaping rules (backslashes before quotes doubled, quotes escaped). A round-trip test against `CommandLineToArgvW` covers it.
- Paths up to 1023 UTF-16 units (Total Commander's limit) are supported end to end (UTF-8 buffer of 3073 bytes).
- Known limitation: a Linux file name containing a literal backslash cannot be addressed, because the backslash is the WFX path separator.

### 7.4 Parsing Rules (`restic_parse_*`)

- `snapshots --json`: a literal `null` is an empty list; entries with an invalid time or non-hex ID are skipped (the directory name must never contain separators). Directory name = snapshot time **as recorded by Restic (its own UTC offset)**; the `FILETIME` fields are converted to UTC.
- `ls --json` NDJSON: strict - a malformed line aborts with `RESTIC_ERR_JSON_PARSE` (stdout contains JSON only). Only direct children of the filter path are returned. The filtered directory's own node is consumed to prove existence: a filter other than `/` whose node is missing or is not a `dir` yields `RESTIC_ERR_PATH_NOT_FOUND`. This distinguishes "missing" from "empty directory", which Total Commander needs (`ERROR_NO_MORE_FILES` = exists but empty, any other error = does not exist).
- Entry mapping: `dir` -> `FILE_ATTRIBUTE_DIRECTORY`; everything else (files, **symlinks**, devices) -> plain file, as the WFX specification requires ("Links should be returned as files"). All entries carry `FILE_ATTRIBUTE_READONLY`. 64-bit sizes are split into `nFileSizeHigh/Low`.
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
- **Win32 error mapping:** empty listings (`RESTIC_OK` with `count == 0`) set `ERROR_NO_MORE_FILES`; missing paths or repositories set `ERROR_PATH_NOT_FOUND`; wrong passwords after retries set `ERROR_ACCESS_DENIED`; locked repositories set `ERROR_LOCK_VIOLATION`. User-facing errors are reported via both `g_LogProc(MSGTYPE_IMPORTANTERROR)` and `g_RequestProc(RT_MsgOK)`.

### 7.6 Snapshot Statistics Cache & Content Columns (`restic_stats.c`, `fscontent.c`)

- **One child process, two parsers:** `restic_fetch_snapshots_json()` returns the raw stdout of `restic snapshots --json --no-lock`; `list_snapshots_and_cache_stats()` (in `main.c`) feeds it to `restic_parse_snapshots_json()` (directory entries) **and** to `restic_parse_snapshot_list()` (statistics records). The records are stored with `snapshot_stats_store()` before the directory handle is returned, so the columns are already populated when Total Commander starts asking for values. A statistics parse failure never fails the listing: the alias cache entry is invalidated instead.
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
- **Export surface:** `cJSON` is compiled with `/DCJSON_HIDE_SYMBOLS`. Without it the library marks every API `__declspec(dllexport)` and the plugin would export ~90 `cJSON_*` functions next to the WFX entry points. `tests/test_plugin_dll.c` asserts the exact surface: only the nine read-only `Fs*` functions are exported; `FsGetFile/PutFile/DeleteFile/MkDir/RemoveDir/RenMovFile/SetAttr/SetTime/ExecuteFile` (and any `cJSON_*`) must stay absent, because Total Commander enables an operation as soon as the symbol exists.
- **Imports:** only `KERNEL32.dll` (static CRT). No extra import libraries are linked into the plugin (`SYSLIBS` is empty; the tests add `Shell32.lib` for `CommandLineToArgvW`).

### 8.6 Build & Test Commands (`build.sh` & `Makefile`)

```bash
./build.sh debug     # Incremental Debug build (build/restic.wfx64, /MTd)
./build.sh release   # Optimized Release build (build/restic.wfx64, /MT)
./build.sh test      # Build plugin + run all four test programs (CRT heap & leak verification)
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
| `test_config.c` | Directory normalization, INI primary/fallback priority (verifying no INI file is auto-created when missing), `restic.exe` resolution order, **PATH-only lookup (a planted `restic.exe` in the cwd must be ignored)**, `[Repositories]` parsing edge cases, 301-alias scaling (array and INI-section buffer growth), root enumeration through `FsFindFirstW/Next/Close`, WFX path validation, missing-`restic.exe` diagnostic, password prompt cancellation, the **missing-INI scenario** (`FsInitW`/`FsSetDefaultParams`/`FsFindFirstW`/`FsGetFileW` stay inert, no password prompt or popup, and plugin/fallback directories remain empty), and verification that the staged `build\restic_plugin.ini` sample template is valid and has 0 active repositories (79-80 assertions). |
| `test_restic_cli.c` | ISO-8601 edge cases (exact epoch value, offsets, fractions, invalid calendar dates), snapshot naming, path normalization (1023-char CJK path), `restic_quote_arg` round-trip, snapshot/NDJSON parsers (prefix-sharing siblings, missing/empty/file targets, symlinks, 64-bit sizes, corrupt lines), process execution with `cmd.exe` as a deterministic stand-in (stdout/stderr separation, exit codes, environment injection/stripping/inheritance, timeout, cancellation, >100 KB output, >6000-char command line, spawn failures), and a **self-contained live test** against `tmp\restic.exe` including `restic_dump_file` streaming extraction, mtime preservation, 0-byte and 100 KB files, overwrite protection, binary fidelity (`00/0A/0D/1A` bytes), existing target preserved on every failure or abort, no leftover `~rp*.tmp`, local-side failures, argument validation, plus `test_dump_with_fake_restic` (a fake `restic` batch file that deterministically covers exit codes 10/11/12/other, stderr classification, partial output then failure, empty stderr, environment injection and the inactivity timeout: a silent child is killed, a slow but steady one survives) (273 assertions). |
| `test_stats.c` | Path decomposition (`wfx_parse_path`: root, alias, snapshot, deep path, malformed/non-hex/empty IDs), statistics parser (`ResticSnapshotInfo` from a fixture with/without `summary`, tags/paths joining and counts, derived duration, `short_id` fallback, skipped entries, `null`/empty/malformed payloads, NULL arguments), per-alias cache (store/lookup/invalidate/clear, case-insensitive alias, replaced entries, 12-alias eviction, empty record list), and the `FsContent*` surface (field enumeration and naming rules, unit strings, values per field with unit scaling, fields inherited by files inside a snapshot, empty alias/root/unknown paths, `ft_nosuchfield`/`ft_fileerror`/`ft_fieldempty` edge cases, buffer truncation, delayed flag, default view/headers/widths/options consistency, NULL buffers, truncated `maxlen`, sort order, no field flags, unloading clears the cache) and the **Code Review 4 hardening tests** (statistics parser vs. listing parser stay index-aligned on non-hex/empty/non-ASCII IDs and bad dates, counters saturate at 2^63-1 for `1e30`/`2^64-1`/negative/string values and never turn negative through `FsContentGetValue`, 200-tag lists are cut without a dangling separator and still counted, UTF-8 host/tag/path text is converted to the ANSI code page and cut on a character boundary, emoji aliases resolve through their ANSI spelling, ambiguous spellings are refused, a literal `?` alias never overwrites a Unicode alias, 4 reader threads + 1 writer thread hammering the cache for 16 000 lookups), plus the **path-spelling and diagnostic tests** (8 accepted spellings of the same snapshot folder - relative, plugin root prepended, bare name, deep file, trailing backslash - with the canonical form still refusing another alias; `snapshot_stats_lookup_any` unique/unknown/NULL/duplicate-alias/conflicting-ID cases and the `snapshot_stats_describe` format; the `RESTIC_WFX_TRACE` switch writing nothing when unset and a full call record when set) (415 assertions). |
| `test_plugin_dll.c` | Loads the built `restic.wfx64` like Total Commander: exact 21-function export surface (`FsGetFile`/`FsGetFileW` and the 7 `FsContent*` columns included; write/delete/move functions forbidden), custom-column surface through `GetProcAddress` (field enumeration terminates, names unique, default view references `[=restic.` with one header per column, empty values for unknown paths, sort order, unloading), embedded `RT_GROUP_ICON` #1 + 5 `RT_ICON` frames (`16x16`..`256x256`), embedded `RT_VERSION` (`VS_VERSION_INFO` / `VS_FIXEDFILEINFO` matching `include/version.h`), root name, primary INI next to the DLL, fallback INI via `FsSetDefaultParams` (no file auto-created when missing), two concurrent find handles, ANSI stub behavior, and **end-to-end live navigation & extraction** across Level 0 -> Level 1 -> Level 2+, `FsGetFileW` single-file and recursive folder copy (`F5`), overwrite prompt protocol (`FS_FILE_EXISTS` -> `FS_COPYFLAGS_OVERWRITE`), `FS_COPYFLAGS_MOVE` rejection, binary fidelity and mtime, failed overwrite keeps the old file, local-side `FS_FILE_WRITEERROR`, invalid remote names, NULL arguments, `ESC` at start and mid-transfer, password flows inside `FsGetFileW` (cancel, 3 wrong, retry, cache reuse), password retry, per-repository sessions (8-slot eviction), empty/cancelled password, 3-strike failure, missing repository, 250-character alias error message, `ProgressProc` (`ESC`) cancellation and 6 concurrent worker threads, plus the statistics columns end to end on the live fixture (host name, `FILETIME`, short ID, file count vs. `has_summary`, inheritance for files inside a snapshot, empty columns for a repository that was not enumerated yet, independent entry per alias) (214 assertions in Debug/Release; skipped in ASan runs). |

The live test needs only `tmp\restic.exe` (git-ignored; reported as `[SKIP]` when absent). On every run it rebuilds `tmp\test_fixture\{data,repo}` from scratch (300-file directory, Unicode and shell-metacharacter names, empty directory, 100 KB file, fixed mtime), initializes a repository whose password contains quotes, `%PATH%` and CJK text, takes two snapshots and verifies per-snapshot listings, timestamps and the error mapping (wrong password, missing repository, unknown snapshot, missing path, file used as directory, quote-injection attempt). The fixture stays on disk for inspection.

Test-quality note: the security-relevant tests were mutation-checked (reverting to `SearchPathW`, or removing the quote escaping, makes them fail).
