<p align="center">
  <img src="logo/logo_256x256.png" alt="Restic WFX Plugin Logo" width="180" />
</p>

<h1 align="center">Restic WFX Plugin for Total Commander</h1>

<p align="center">
  <b>Browse and extract files from <a href="https://restic.net">Restic</a> backup repositories directly inside Total Commander's dual-pane interface.</b>
</p>

<p align="center">
  <a href="https://github.com/goldfix/Restic_WFX_Plugin/blob/main/LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue.svg" alt="License: MIT" /></a>
  <img src="https://img.shields.io/badge/Language-C11-00599C.svg" alt="Language: Pure C11" />
  <img src="https://img.shields.io/badge/Platform-Windows%20x64-0078D6.svg" alt="Platform: Windows x64" />
  <img src="https://img.shields.io/badge/Total%20Commander-WFX64-orange.svg" alt="Total Commander WFX64" />
  <img src="https://img.shields.io/badge/Version-1.3.0-success.svg" alt="Version: 1.3.0" />
</p>

---

## Overview & Purpose

**Restic WFX** (`restic.wfx64`) is a native 64-bit **File System Plugin (WFX)** for [Total Commander](https://www.ghisler.com/) written in pure **C11** (Win32 API, zero external DLL dependencies beyond `KERNEL32.dll`).

It turns Total Commander's **Network Neighborhood** (`\\\Restic Backups\`) into a fast, strictly **read-only** browser and extractor for your [Restic](https://restic.net) backup repositories:

```text
\\\Restic Backups\
├── MyLocalBackup\                              <-- Level 0: Repository aliases (from restic_plugin.ini)
│   ├── !restic_stats.html                      <-- Repository report (F3)
│   ├── 2026-10-18_09-30-00_[a1b2c3d4]\         <-- Level 1: Snapshots (YYYY-MM-DD_HH-MM-SS_[short_id])
│   │   └── C\
│   │       └── Users\
│   │           └── alice\
│   │               └── Documents\
│   │                   └── report.pdf          <-- Level 2+: Snapshot directory tree & files
│   └── 2026-10-17_21-15-42_[e5f60718]\
└── CloudS3Backup\
```

### Key Features

- **Lazy On-Demand Directory Loading (WFX Architecture):** Unlike packer plugins (`.wcx`), which must enumerate every file in an archive upon opening, Restic WFX queries **only the single directory level you are currently viewing** (`restic ls --json --no-lock <snapshot> <path>`). Even repositories containing millions of files open responsively.
- **Native File & Folder Extraction (`F5`, `F3`, `Ctrl+Q`):**
  - Copy individual files or entire directory trees out of any snapshot using **`F5`** (preserving original modification timestamps and 64-bit file sizes).
  - Inspect files directly with Total Commander's **Lister (`F3`)** or **Quick View (`Ctrl+Q`)**.
  - Constant-memory streaming (`64 KB` chunks via `restic dump --no-lock`) into a temporary file with **atomic replacement** on completion: an interrupted or failed overwrite never corrupts or deletes your existing local file.
- **Interactive Statistics Report (`!restic_stats.html`):** every repository, snapshot and snapshot folder shows one extra virtual file. Press **`F3`** (Lister) or open it in a browser to get an HTML dashboard: repository-wide storage and deduplication plus the snapshot history, snapshot metadata with live compression statistics and a diff against the parent snapshot, or a folder breakdown by subfolder, file type, size range and largest files. The file is hidden during recursive copies, searches and folder size calculations, so an extracted backup never contains it.
- **Strictly Read-Only by Design:** Every Restic command runs with `--no-lock`. Repository modification entry points (`FsPutFile`, `FsDeleteFile`, `FsRemoveDir`, `FsMkDir`, `FsRenMovFile`) are intentionally not exported, and `F6` (Move) is rejected.
- **Zero Passwords on Disk:** Repository passwords are requested interactively via Total Commander's native password dialog (`RequestProcW`), kept only in volatile RAM per repository alias for the active session, passed to `restic.exe` via an isolated environment block (`RESTIC_PASSWORD`), and wiped with `SecureZeroMemory`.
- **Responsive Cancellation (`ESC`):** Long operations on remote backends can be aborted with `ESC` and use a 30-minute **inactivity** timeout (so slow multi-gigabyte transfers are never cut off as long as bytes keep flowing).
- **Backup Statistics as Native Columns:** Entering `Restic Backups` automatically switches the panel to a custom columns view showing the snapshot **date, host, tags, file count, size, added data and duration**. 27 fields are available (IDs, parent, user, source paths, new/changed/unmodified files and directories, blobs, packing ratio, ...), all taken from the JSON already fetched to list the snapshots: no extra `restic.exe` run per cell or per folder and no extra password prompt (the only exception is the one-off cold start described below).

---

## About Restic & Where to Get It

This plugin uses the official **Restic** command-line binary (`restic.exe`) under the hood to communicate with local, network, and cloud repositories.

**[Restic](https://restic.net)** is a fast, secure, cross-platform backup program written in Go that uses authenticated encryption (AES-256 + Poly1305-AES) and content-defined deduplication.

- **Official Website:** [https://restic.net](https://restic.net)
- **Official Documentation:** [https://restic.readthedocs.io/en/latest/](https://restic.readthedocs.io/en/latest/)
- **Download `restic.exe` for Windows (GitHub Releases):** [https://github.com/restic/restic/releases](https://github.com/restic/restic/releases)  
  *(Download `restic_<version>_windows_amd64.zip`, extract the binary, and rename it to **`restic.exe`**)*
- **Install via WinGet / Scoop / Chocolatey:**
  ```powershell
  winget install restic.restic
  # or: scoop install restic
  # or: choco install restic
  ```

---

## Installation in Total Commander

### Option A: Automatic Installation (Recommended)

1. Download `wfx_restic_<version>_x64.zip` from the [GitHub Releases](https://github.com/goldfix/Restic_WFX_Plugin/releases) page.
2. In Total Commander, navigate to the ZIP file and **double-click it** (or press `Enter`).
3. Total Commander reads the bundled `pluginst.inf`, asks where to install the plugin (default: `plugins\wfx\restic`) and registers it automatically as **`Restic Backups`** in Network Neighborhood.
4. Edit `restic_plugin.ini` (extracted next to `restic.wfx64`) and add your repositories (see below).
5. Open **Network Neighborhood** (`Alt+F1` / `Alt+F2` -> `\`) and enter **`Restic Backups`**.

The ZIP contains: `pluginst.inf`, `restic.wfx64`, `restic_plugin.ini` (commented sample), `README.md` and `LICENSE`.

> If a `restic_plugin.ini` already exists in the target folder (upgrade), make sure to keep your own copy of it: Total Commander may ask before overwriting files.

### Option B: Manual Installation

1. Extract `restic.wfx64` and `restic_plugin.ini` (from the ZIP, or from `build\` after building from source) and copy them (optionally together with `restic.exe`) into a dedicated plugin folder, for example:
   ```text
   C:\TotalCmd\plugins\wfx\restic\restic.wfx64
   ```
2. In Total Commander, open **Configuration** -> **Options...** -> **Plugins** -> **File system plugins (.WFX)** -> click **Configure**.
3. Click **Add**, select `restic.wfx64`, and confirm with **OK**.
4. Edit `restic_plugin.ini` (see below) and add your repositories.
5. Open **Network Neighborhood** (`Alt+F1` / `Alt+F2` -> `\`) and enter **`Restic Backups`**.

---

## Configuration (`restic_plugin.ini`)

A fully commented sample configuration file is included in the repository root ([`restic_plugin.ini`](restic_plugin.ini)) and is automatically copied to `build\restic_plugin.ini` during every build.

### Where to Place `restic_plugin.ini`

The plugin never auto-creates or modifies `restic_plugin.ini` at runtime. On every root listing or repository operation, it searches for `restic_plugin.ini` in the following order:

1. **Primary Location (Portable / Next to the Plugin DLL):**
   ```text
   <plugin_dir>\restic_plugin.ini
   ```
   *(e.g., `C:\TotalCmd\plugins\wfx\restic\restic_plugin.ini` — recommended for portable Total Commander installations)*
2. **Fallback Location (Total Commander User Configuration Directory):**
   If no INI file is present next to `restic.wfx64` (for example, when Total Commander is installed under a read-only `C:\Program Files\` directory), the plugin looks in the directory reported by Total Commander via `FsSetDefaultParams` (the folder containing `wincmd.ini` / `fsplugin.ini`, typically):
   ```text
   %APPDATA%\GHISLER\restic_plugin.ini
   ```

> **Encoding Note:** Save `restic_plugin.ini` as **ANSI** (system code page) or **UTF-16 LE (with BOM)**. Standard Win32 INI APIs (`GetPrivateProfile*W`) do not decode UTF-8 when the file contains non-ASCII characters.

### How `restic.exe` Is Located

When executing a command, the plugin searches for `restic.exe` in this order:
1. The path configured in `ResticPath=` under `[Settings]` (absolute path, or relative to the directory containing `restic_plugin.ini`).
2. `<plugin_dir>\restic.exe` (next to `restic.wfx64`).
3. The system `PATH` environment variable *(for security against working-directory hijacking, the process current directory is never searched)*.

### Example `restic_plugin.ini`

```ini
; ==============================================================================
; Restic File System Plugin (WFX) for Total Commander - Configuration File
; ==============================================================================

[Settings]
; Optional path to restic.exe.
; If omitted or commented out, the plugin checks <plugin_dir>\restic.exe first
; and then searches the system PATH.
ResticPath=C:\Tools\restic\restic.exe

[Repositories]
; Format: <Alias>=<Repository Path or URL>
; Each <Alias> is displayed as a top-level folder inside "\\\Restic Backups\".

; 1. Local disk repository
LocalDocs=D:\Backups\restic_docs

; 2. Network share (SMB / UNC path)
NasBackup=\\NAS\Backups\workstation_repo

; 3. Restic REST Server (HTTP / HTTPS)
HomeRestServer=rest:http://192.168.1.50:8000/workstation
RemoteRestTLS=rest:https://backup.example.com:8000/prod

; 4. SFTP repository (uses your system OpenSSH configuration / ssh-agent)
LinuxServerSftp=sftp:backupuser@192.168.1.20:/srv/restic/repo

; 5. Amazon S3 / S3-compatible storage (MinIO, Wasabi, Backblaze B2, Cloudflare R2)
;    Backend environment variables (e.g. AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY)
;    are inherited from Total Commander's process environment.
AwsS3=s3:s3.amazonaws.com/my-backup-bucket/restic
LocalMinio=s3:http://192.168.1.60:9000/restic-backups

; 6. Rclone backend (Google Drive, OneDrive, Dropbox, Box, etc.)
GoogleDrive=rclone:gdrive:Backups/restic_repo
```

---

## Usage in Total Commander

| Action | How It Works |
| --- | --- |
| **List Repositories** | Open **Network Neighborhood** (`\`) -> **`Restic Backups`**. Changes to `restic_plugin.ini` take effect immediately on panel refresh (`Ctrl+R` / `F2`). |
| **Browse Snapshots** | Enter a repository folder (`\<Alias>`). Prompted once for the repository password; kept in RAM for that alias across both panels until Total Commander unloads the plugin (or a wrong password is entered). Up to 3 password attempts are allowed. |
| **Explore Files** | Enter any snapshot folder (`YYYY-MM-DD_HH-MM-SS_[short_id]`) and navigate the backed-up directory tree. |
| **View File (`F3` / `Ctrl+Q`)** | Press **`F3`** (Lister) or **`Ctrl+Q`** (Quick View) on any file to stream it to a temporary location and view its contents. |
| **Restore / Extract (`F5`)** | Select one or more files or directories and press **`F5`** to copy them to the target panel, preserving original `LastWriteTime` timestamps. Standard Total Commander overwrite prompts are supported. |
| **Cancel Operation (`ESC`)** | Press **`ESC`** in Total Commander's progress dialog to terminate the underlying `restic.exe` child process and clean up any temporary file. |
| **Statistics Report (`F3`)** | Every repository, snapshot and folder contains the virtual file `!restic_stats.html`; `F3` renders a self-contained HTML report (see below). |
| **Backup Statistics (Columns)** | Enter a repository: Total Commander switches to the plugin's custom columns view automatically. Add or reorder columns via **Configuration -> Options -> Custom columns** (see below). |

---

## Backup Statistics (Custom Columns)

While a panel displays `Restic Backups`, Total Commander can show Restic's own backup statistics as columns. The plugin publishes those as *content fields* (`FsContent*`), so they can be used in a custom columns view, in the multi-rename tool and in sorting.

**Default view:** when you enter `Restic Backups` Total Commander switches the panel to a ready-made view (you can change or replace it at any time):

| Column | Example | Source |
| --- | --- | --- |
| Date | `2026-10-18 16:28` | snapshot time |
| Host | `GOLDFIX-PC` | `hostname` |
| Tags | `daily, home` | `tags` |
| Files | `12480` | `summary.total_files_processed` |
| Size | `1873` | `summary.total_bytes_processed` (MB) |
| Added | `42` | `summary.data_added` (MB of new data written to the repository, before compression) |
| Duration | `95` | backup end minus backup start (seconds) |

To build your own view: **Configuration -> Options -> Custom columns -> New**, add a column and pick a field from the plugin. In the field reference, `<plugin name>` is the key shown in **Configuration -> Options -> Plugins -> File system plugins** (the installer proposes `restic`, but it can differ - the test machine uses "Restic Backups"):

```text
[=<plugin name>.size.Mbytes]
[=<plugin name>.files_new]
[=<plugin name>.packed_ratio]
```

All published fields:

| Field | Meaning |
| --- | --- |
| `time`, `backup_start`, `backup_end`, `duration` | Snapshot time, backup window and its length (`s` or `min`) |
| `hostname`, `username`, `tags`, `paths`, `program_version` | Who/what produced the snapshot and which paths it covers |
| `id`, `short_id`, `parent` | Snapshot identifiers (the parent is empty for the first snapshot) |
| `files`, `size` | Files processed and bytes processed (`bytes`, `kbytes`, `Mbytes`, `Gbytes`) |
| `data_added`, `data_added_packed`, `packed_ratio` | New data added to the repository (uncompressed) and the size actually stored (compressed), and their ratio |
| `files_new`, `files_changed`, `files_unmodified`, `dirs_new`, `dirs_changed`, `dirs_unmodified` | What the backup actually changed |
| `data_blobs`, `tree_blobs`, `avg_file_size`, `has_summary` | Blob counts, average file size, and whether Restic stored a summary |

Notes:

- Columns are filled for snapshot folders **and** for every entry inside them (files inherit the statistics of their snapshot); the root and the repository rows stay empty.
- Snapshots created by older Restic versions (or by other clients) may have no summary: those columns stay empty instead of showing wrong zeros.
- Nothing is computed in the background: displaying a value never starts `restic.exe` and never asks for a password.
- Text is shown with full Unicode support: Total Commander calls the Unicode entry points of the plugin, so host names, tags and source paths keep their original characters.
- The columns are filled when a repository's snapshot list is opened (`\<Alias>`) and, if you jump straight into a snapshot folder without having opened its repository in the current session, by a single extra `restic snapshots` run performed right after that folder has been listed (once per repository and session, reusing the password you already entered).
- Fields that require an additional Restic run (real deduplicated repository size, diff against the parent snapshot) are not published yet.
- Files and folders whose name contains a `/` or `\` (legal on Linux/macOS) are not shown: Total Commander would read the separator as a directory boundary. Extract them with `restic restore` instead.
- **Backups of network shares** (`restic backup \\host\share\dir`) are supported. Restic stores the share as one folder named `\\host\share`; because a Total Commander name cannot contain backslashes, it is shown as `＼＼host＼share` (full-width reverse solidus). Open it like any other folder; copying it with `F5` creates a local folder with that name.

### If the columns stay empty

Total Commander shows the statistics columns only when the column definition points at this plugin. Two situations look identical (visible headers, blank values), so check them in this order:

1. **Refresh the repository folder once** (`\<Alias>`) if the columns of a snapshot folder are empty: the statistics are read while that folder is listed. Jumping straight into a snapshot folder is supported too (the plugin then runs one extra `restic snapshots` in the background), but a view that was built before the repository was opened in this session stays empty until it is refreshed.
2. **Check the column definition**: in **Configuration** -> **Options** -> **Custom columns** the fields must be written as `[=<plugin name>.size.Mbytes]`, where `<plugin name>` is the key shown in **Configuration** -> **Options** -> **Plugins** -> **File system plugins** (the installer proposes `restic`, but you can change it). The built-in default view does not depend on it, because it references its own fields with the special name `<fs>`, e.g. `[=<fs>.time]`, `[=<fs>.size.Mbytes]`, ...
3. **Clear a stale default view** if Total Commander has already saved one: it stores the default view in `wincmd.ini` under `[CustomFields_<plugin name>]` with `AutoLoad=1` and then never asks the plugin again. If that stored view references the wrong name (for example `Contents1=[=restic.time]...` from an older version, while the plugin is registered as "Restic Backups"), the columns stay empty. Close Total Commander, edit `wincmd.ini` and either delete the whole `[CustomFields_<plugin name>]` section, or change every `[=restic.` in its `Contentsx` line to `[=<fs>.`; then start Total Commander again.
4. **Turn on the diagnostic trace** if the columns are still empty:
   - either set the environment variable `RESTIC_WFX_TRACE=1` **before** starting Total Commander (starting a second instance only activates the running one, so use the *Start menu* shortcut from a shell where you ran `set RESTIC_WFX_TRACE=1`, or `setx RESTIC_WFX_TRACE 1` followed by a full restart of Total Commander),
   - or create an empty file named `restic_wfx_trace.on` next to `restic.wfx64`, then restart Total Commander (or reload the plugins with `cm_UnloadPlugins`).
   The log is written to `%TEMP%\restic_wfx_trace.log` (max 400 lines, recreated each time the plugin is loaded) and tells the cases apart:
   - **no file at all** -> the plugin was not loaded, or an older DLL is still in place;
   - the file lists `FsInitW`/`findfirst` but **no `value` lines** -> Total Commander never asks for the fields: the column definition does not point at this plugin (step 2) or a stale default view is stored in `wincmd.ini` (step 3);
   - `value '...' field=N unit=N -> -3 (unresolved)` -> the path spelling is still not recognised; the log contains the exact string, please report it;
   - `-> -3` together with `cache: []` -> the repository was never listed in this session (step 1).
   Remove the file or unset the variable when you are done.

---

---

## Statistics Report (`!restic_stats.html`)

Each repository, snapshot and snapshot folder contains one extra read-only virtual file named **`!restic_stats.html`**. Press **`F3`** (Lister) or **`Ctrl+Q`** (Quick View) on it: the plugin runs the required Restic queries and writes a self-contained HTML report. You can also copy it with **`F5`** and open it in any browser.

| Where | What the report contains |
| --- | --- |
| `\<Alias>\!restic_stats.html` | Whole-repository storage: logical restore size, uncompressed and compressed (stored) size, compression ratio and savings, deduplication factor, blob count, plus the full snapshot history with the data each backup added over time. |
| `\<Alias>\<Snapshot>\!restic_stats.html` | Snapshot metadata (IDs, parent, host, user, Restic version, tags, source paths, backup window and duration), live storage and compression figures for that snapshot, the new/changed/unmodified file and directory breakdown, and a diff against the parent snapshot with the changed paths. |
| `\<Alias>\<Snapshot>\<Folder>\!restic_stats.html` | Recursive analysis of that folder: total size and file/directory counts, the share of every direct subfolder, the largest file types, a file size histogram and the fifteen largest files. |

Notes:

- The report renders in both Lister modes: plain tables and Unicode bars (`[████░░░░]`) are readable in the built-in HTML text mode, and the embedded CSS turns it into a dark/light dashboard in a browser or in the Explorer preview.
- Generating a report needs a few extra `restic.exe` runs (`stats`, `diff`, `ls --recursive`), so it can take a moment on a slow or remote repository. **`ESC`** cancels it, the partially written file is discarded and an existing target file is left untouched.
- Optional queries degrade gracefully: a snapshot whose parent has been deleted still produces a report, with a note stating that the diff is unavailable.
- The repository location in the report hides the credentials of a URL (`rest:https://user:secret@host/repo` is shown as `rest:https://***@host/repo`), because the file can be copied or shared. Passwords are never written to the report.
- If a snapshot contains a real file called `!restic_stats.html` (for example a report you saved earlier into a backed-up folder), the plugin does not add its virtual file to that folder: your file stays reachable.
- The file is **not** listed during recursive copies (`F5` on a folder), searches (`Alt+F7`), folder size calculations (space bar) or directory synchronisation, because Total Commander reports those operations through `FsStatusInfoW`. Manual extraction of a whole snapshot therefore never adds a file the user never had.

## Building & Testing from Source

### Prerequisites

- **Windows 10/11 x64**
- **MSVC Build Tools (Visual Studio 2022+)** with x64 `cl.exe`, `link.exe`, `rc.exe`, and `nmake.exe` active in your shell environment.
- **Git** (with submodules initialized: `git submodule update --init --recursive`)
- *(Optional)* `tmp\restic.exe` placed in the `tmp\` folder to enable the live end-to-end integration tests (otherwise live tests are gracefully reported as `[SKIP]`).

### Build Commands

Using the Git Bash / MSYS2 wrapper (`build.sh`) or directly via `nmake`:

```bash
./build.sh release       # Build optimized Release DLL (build/restic.wfx64, static CRT /MT) + copy build/restic_plugin.ini
./build.sh debug         # Build Debug DLL (build/restic.wfx64, /MTd + CRT debug heap)
./build.sh test          # Build & run all 4 test suites in Debug mode (with CRT memory-leak verification)
./build.sh release-test  # Build & run all 4 test suites against the Release build
./build.sh analyze       # Run MSVC Static Code Analyzer (/analyze) with warnings-as-errors (/W4 /WX)
./build.sh asan          # Run unit tests under MSVC AddressSanitizer (/fsanitize=address)
./build.sh package       # Clean, build Release, run tests and create build/wfx_restic_<version>_x64.zip
./build.sh clean         # Clean build/ directory (run before switching between Debug/Release/ASan)
```

Or with `nmake` from an x64 Native Tools Command Prompt:

```bat
nmake /nologo CONFIG=Release all
nmake /nologo CONFIG=Debug test
```

Build artifacts are generated in `build\`:
- `build\restic.wfx64` — 64-bit Total Commander WFX plugin DLL (with embedded multi-resolution icon and `VERSIONINFO` metadata)
- `build\restic_plugin.ini` — Commented sample configuration file ready for deployment
- `build\pluginst.inf` — Total Commander auto-install manifest (`[plugininstall]`)
- `build\wfx_restic_<version>_x64.zip` — Release package (only with `./build.sh package`), installable by double-clicking it in Total Commander. The version is read from `include\version.h`.

---

## External Libraries & Acknowledgments

- **[cJSON](https://github.com/DaveGamble/cJSON)** by Dave Gamble — Ultralightweight ANSI C JSON parser, included as a Git submodule under [`cJSON/`](cJSON) and compiled statically with `CJSON_HIDE_SYMBOLS` (**MIT License**).
- **[Total Commander WFX Plugin SDK](https://www.ghisler.com/plugins.htm)** by Christian Ghisler — File System Plugin interface specification (`include/fsplugin.h`).
- **[Restic](https://restic.net)** ([github.com/restic/restic](https://github.com/restic/restic)) — Invoked as an external CLI process (`restic.exe`, **BSD 2-Clause License**).

---

## Project Links & License

- **GitHub Repository:** [https://github.com/goldfix/Restic_WFX_Plugin](https://github.com/goldfix/Restic_WFX_Plugin)
- **Issue Tracker:** [https://github.com/goldfix/Restic_WFX_Plugin/issues](https://github.com/goldfix/Restic_WFX_Plugin/issues)
- **Architecture & Developer Specification:** [`AGENT.md`](AGENT.md)
- **License:** Licensed under the **[MIT License](LICENSE)** — Copyright (c) 2026 Pietro Partescano.
