# Changelog - Restic WFX Plugin

## [1.1.0] — 2026-10-25

### Added

- **Native statistics columns (WDX-in-WFX, `FsContent*` family)**
  - 27 fields: `time`, `hostname`, `tags`, `files`, `size`, `data_added`, `duration`, `short_id`, `id`, `parent`, `username`, `paths`, `program_version`, `backup_start`, `backup_end`, `files_new`, `files_changed`, `files_unmodified`, `dirs_new`, `dirs_changed`, `dirs_unmodified`, `data_blobs`, `tree_blobs`, `data_added_packed`, `avg_file_size`, `packed_ratio`, `has_summary`.
  - Built-in default view references the plugin's own fields with the self name `<fs>` (`[=<fs>.time]`, `…`), so it works whatever display name Total Commander registered.
  - Unicode entry points mandatory for Total Commander are exported: `FsContentGetValueW`, `FsContentGetDefaultViewW`, `FsContentStopGetValueW` (in addition to the ANSI variants).
  - Per-alias snapshot cache, thread-safe (shared SRW lock for readers, exclusive for writers), populated once per repository enumeration.
- **Backups from network shares (UNC)**
  - `restic backup \\host\share\dir` is now navigable. The snapshot root shows one folder whose name uses the full-width reverse solidus U+FF3C (e.g. `＼＼host＼share`) because Total Commander file names cannot contain backslashes.
  - `restic ls <snapshot> /` prints no node for such snapshots (restic's own Windows path functions treat `/\\host\share` as a volume). The plugin therefore runs one extra `restic snapshots --json --no-lock <id>` per root listing and reconstructs the volumes from the snapshot `paths` (`restic_add_unc_volumes()`). Device paths (`\\?\…`), paths without a share, and duplicates are ignored. A failure leaves restic's own output unchanged; `ESC` cancels the listing.
  - The first path segment starting with two U+FF3C is mapped back to the real UNC volume; one U+FF3C, or U+FF3C deeper in the path, is left untouched so real full-width names keep working.
  - `FsGetFileW` (`F5`, `F3`, `Ctrl+Q`) works under the volume through the same normalisation.
- **Cold start filled on demand**
  - When Total Commander enters `\\<Alias>\<Snapshot>\…` directly (address bar, `Ctrl+D` hotlist, history) without having enumerated `\\<Alias>` first, the plugin runs the same `restic snapshots --json` once per alias and session, reusing the password just entered. No extra prompt.
- **Transfer log**
  - `FsGetFileW` calls `g_LogProc(MSGTYPE_TRANSFERCOMPLETE)` with the SDK-documented format `Download complete: <remote> -> <local>`.
- **Unicode INI fallback**
  - When `FsSetDefaultParams` passes a `DefaultIniName` containing characters not representable in the system ANSI codepage (`?`), or is empty, `config_set_fallback_from_default_params()` falls back to the UTF-16 environment variable `%COMMANDER_INI_PATH%` (Total Commander 11.03+, directory of `wincmd.ini`) instead of producing a wrong path.
- **Diagnostic trace (`wfx_trace.c`/`wfx_trace.h`)**
  - Opt-in via `RESTIC_WFX_TRACE=1` or a file named `restic_wfx_trace.on` next to the DLL.
  - Log to `%TEMP%\restic_wfx_trace.log` (max 400 lines, recreated when the plugin loads).
  - All wide-to-UTF-8 conversions go through `wfx_trace_wide_to_utf8()` / `wfx_trace_ansi_to_utf8()`; no narrow `%ls` (which fails in the `"C"` locale for non-ASCII characters).
  - The whole open/append/close sequence of a log line runs under the trace `SRWLOCK`, so concurrent worker threads cannot interleave or truncate lines.

### Changed

- **Snapshot directory name validation:** `restic_extract_snapshot_id()` accepts only the exact shape produced by Restic (`YYYY-MM-DD_HH-MM-SS_[<hex>]`, `[` at the start or after `_`, `]` at the end). `report_[deadbeef].txt` and `file[deadbeef]` are no longer mistaken for snapshots.
- **Node names containing separators:** entries with `\` or `/` in the name (legal in Linux/macOS backups) are skipped; Total Commander would treat the separator as a directory boundary and a recursive `F5` copy could write outside the target directory.
- **UTF-8 truncation:** `utf8_to_wide_trunc()` and `json_join_string_array()` cut on a character boundary, never in the middle of a multi-byte sequence.
- **Stack usage:** `filter_w` in `restic_list_directory()` / `restic_dump_file()` shrinks from `MAX_UTF8_PATH` (~6 KB) to `MAX_WFX_PATH + 1` (2 KB).
- **Fuzzy path resolution (`lookup_loose`):** the segment in front of the snapshot name is tried first as the repository alias; the cross-repository `lookup_any()` is only the fallback, and when ambiguous the columns stay empty instead of showing the wrong repository.
- **Statistics column cost:** no field ever spawns `restic.exe` (zero-cost); the only extra run is the one-time cold-start fill done by the *directory listing*, not by the field callbacks.

### Fixed

- Default view: `[=<fs>.field]` instead of `[=restic.field]`. Total Commander stores the view verbatim in `wincmd.ini` under `[CustomFields_<PluginName>]` with `AutoLoad=1` and reuses it without calling the plugin again, so a stale stored view must be deleted or rewritten by hand.
- ANSI text conversion for `FsContentGetValue`: UTF-8 → UTF-16 → `CP_ACP`, cut on a character boundary; characters not representable in the codepage become `?`.
- `ft_numeric_64` counters saturate at `LLONG_MAX`; absurd values can never wrap to negative.

### Documentation

- `AGENT.md`: §3.1 (`%COMMANDER_INI_PATH%` fallback), §5.2 (encoding, cold start, path spellings, trace, test counts), §7.4 (parser rules, snapshot names, node names, UNC), §7.5 (`MSGTYPE_TRANSFERCOMPLETE`), §8.7 (updated test counts).
- `README.md`: statistics columns section and "If the columns stay empty" updated; new note on network-share (UNC) backups.
- `MEMORY.md`: current state, quality gates, Code Review 5 and Code Review 6 log entries.

### Tests and quality

- Test suite grew from **982 to 1081** assertions in this cycle: config (85), CLI (316), statistics (450), DLL (230).
- **Mutation checks: 11/11 detected** (harness in `tmp/mut_check.sh`): snapshot-ID strictness, node-name separators, UTF-8 truncation, `%COMMANDER_INI_PATH%` fallback, cold start, `MSGTYPE_TRANSFERCOMPLETE`, UTF-8 boundary in join, `describe` UTF-8, alias preference in fuzzy lookup, surrogate-pair boundary in trace.
- Live tests: backup via `\\localhost\C$\…` (administrative share, `[SKIP]` when unreachable), plus direct DLL verification with `ctypes` (`tmp/unc_dll_check.py`).
- Final gates: `./build.sh test` and `analyze` **1081** (`/analyze` clean), `asan` **850**, `package`/Release **1080**.

### Package

- `build/wfx_restic_1.1.0_x64.zip`: 260903 bytes, SHA-256 `a436a89855623908fd0e3be4842fd5483c4106f72c1c207717d4a47953d8a625`.
- Contains: `pluginst.inf`, `restic.wfx64`, `restic_plugin.ini`, `README.md`, `LICENSE`.
- Version 1.1.0 is the single source of truth in `include/version.h`, reflected in `pluginst.inf`, `pyproject.toml` and the README badge.

---

## [1.2.0] — Planned (confirmed)

### Confirmed — planned for next release (not yet implemented)

- **Statistics Options B and C** from the original proposal:
  - Option B: HTML report opened with `F3` in the Lister.
  - Option C: typed command via `FsExecuteFile`.
- **Extra-process fields** (cost one child process per refresh, excluded from 1.1.0 on purpose):
  - `restic stats --mode raw-data|restore-size` fields (`restore_size`, `dedup_size`, `compression_ratio`).
  - `restic diff` vs parent snapshot fields (`changed_files`, `added_bytes`, `removed_bytes`).
- **Streaming `ls` parser** for very deep / very wide directories without loading the whole NDJSON into memory.
- **Optional directory cache** (the statistics cache is volatile; a persistent directory cache was judged worse than empty cells for columns).
- **Job Object** to kill the whole `restic.exe` child tree on cancellation/timeout.
- **UTF-8 INI** (`restic_plugin.ini` read as UTF-8 instead of ANSI `CP_ACP`).
- **Live Total Commander verification** of the `ft_*` numeric values and of the auto-applied default view after a full TC restart.

### Known limitations (carried over)

- Cold start: columns stay empty until the alias level has been enumerated in the session (filled on demand since 1.1.0 when a snapshot folder is entered directly).
- Snapshot folders reached through a stored default view that was saved before the alias was enumerated stay empty until the repository folder is refreshed.
- The user's real repository (`\\GOLDFIX-PC\tmp\test_restic_2`) could not be opened here because its password is unknown; verify with the final build before release.
