# Changelog

All notable changes to the Restic WFX plugin.

## [1.3.1] — 2026-10-30

Reworks the statistics report introduced in 1.3.0.

### Changed

- **The report is opened from the folder, not from a file.** The virtual `!restic_stats.html` entry is gone:
  listings contain only real snapshots and files again, so nothing artificial can be copied or counted. Put the
  cursor on a repository, a snapshot or a snapshot folder and press `F3` or `Alt+Enter`, or type `stats`; the
  report opens in the Total Commander Lister. The space bar still calculates the folder size.
- **Markdown instead of HTML.** Pipe tables aligned in monospace and ASCII bars (`#` and `.`) read cleanly as
  plain text in the Lister and render in any Markdown viewer. The Unicode bar characters that overlapped in the
  Lister are gone. The report is written to the temporary folder with a descriptive name.
- **Faster.**
  - Report generation reuses the snapshot list, the snapshot metadata and the restore size that the columns
    already hold, so a repository or snapshot report no longer scans the whole tree; a repeated request within
    one minute is answered from memory.
  - Output from Restic is read without idle sleeps and with a larger pipe buffer: file extraction is about 60 %
    faster and folder reports on trees with many subfolders no longer stall.
  - The 30-minute timeout counts silence, not total time.
- The progress bar now only moves forward instead of being reset to zero about ten times a second.

### Fixed

- A report requested for a file or for a path that does not exist does nothing instead of producing an empty
  document, and `Alt+Enter` on a file or on the plugin root is no longer reported as an error.
- A new report no longer fails when the previous one is still open in the Lister; it gets a new name.

## [1.3.0] — 2026-10-30

### Added

- **Statistics report as a virtual file.** Every repository, snapshot and snapshot folder lists one extra read-only entry, `!restic_stats.html`. `F3` (Lister) or a browser shows a self-contained HTML report:
  - repository: storage and deduplication figures plus the snapshot history;
  - snapshot: metadata, live compression statistics and a diff against the parent snapshot;
  - folder: breakdown by subfolder, file type and size range, and the largest files.
- The report is hidden during recursive copies, searches, folder size calculations and directory synchronisation, so an extracted backup never contains it. It can be cancelled with `ESC`, never replaces an existing file when cancelled, and shows repository URLs without their credentials.
- A backed-up file that happens to be called `!restic_stats.html` stays reachable: the virtual entry is not added next to it.

## [1.2.0] — 2026-10-26

### Added

- **Network share (UNC) backups.** Snapshots taken from a share such as `\\server\share\dir` are now browsable. The share appears as a folder whose backslashes are replaced by the visually identical full-width reverse solidus (U+FF3C), and navigation and extraction work below it.
- **Direct entry into snapshots.** Opening a snapshot folder without having listed the repository first now fills the statistics once and reuses the password already entered, instead of showing empty columns.
- **Transfer log.** Completion of an extraction is reported in Total Commander's transfer log.
- **Diagnostic trace.** Opt-in logging (environment variable or switch file), off by default.
- **Configuration path fallback** for Total Commander installations whose configuration path cannot be expressed in the system ANSI code page.

### Changed

- Snapshot folder names are validated strictly, so ordinary files whose names look like a snapshot ID are no longer opened as snapshots.
- Entry names containing a path separator (legal on Linux/macOS backups) are skipped instead of being traversed as subdirectories.
- Truncation of long paths and UTF-8 text respects character boundaries; ANSI conversion never splits a multi-byte character.
- Repository alias resolution prefers the exact alias, so statistics are not taken from a similarly named repository.
- Reduced stack usage when building paths for large numbers of entries.

### Fixed

- Invalid or non-representable configuration paths no longer produce unreadable paths.
- Numeric column overflow cannot produce negative values.
- Default column view no longer depends on a hard-coded plugin name, which Total Commander kept reusing after the plugin was renamed or re-registered.

## [1.1.0] — 2026-10-25

### Added

- **Statistics columns.** File listing can show backup statistics (time, host, tags, file/dir counts, sizes, data added, duration, hashes, paths, summary availability) as native content columns. Values come from the snapshot list already loaded by the plugin, so displaying a column never starts a new process. Ships with a ready-to-use default view that works regardless of the name the plugin is registered under.

## [Unreleased]

### Planned

- Optional extra-process columns: repository statistics and differences against the parent snapshot.
- Streaming directory listing for very deep or very wide folders.
- Optional directory cache.
- Child process tree termination on cancellation.
- UTF-8 configuration file support.
