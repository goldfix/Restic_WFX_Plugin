# AGENT.md - Restic File System Plugin for Total Commander (WFX)

Technical specification, operational requirements, compiler hardening rules, and architectural guidelines for developing the Total Commander Restic plugin in standard C (C11).

---

## 1. Project Goal

Build a **File System Plugin (WFX)** extension for **Total Commander** written strictly in **pure standard C (C11)** (no C++ or other languages).

The primary goal of Phase 1 (MVP) is to provide interactive, read-only browsing of **Restic** backup repositories ([https://restic.net](https://restic.net)), allowing users to navigate seamlessly across configured repositories, snapshots, and directory trees directly within Total Commander's dual-pane interface.

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
   - **No persistent or in-memory directory cache.**
   - Every directory navigation queries the CLI directly to keep the codebase minimal, deterministic, and free from cache invalidation complexity.

5. **Credential Lifecycle**
   - No passwords are stored on disk (neither in `restic_plugin.ini` nor in external files).
   - Credentials are requested at runtime when entering a repository via Total Commander's `RequestProcW` (`RT_Password`) callback.
   - Credentials remain in a dedicated in-memory buffer for the active repository session and are wiped when switching repositories or unloading the plugin.

---

## 3. Configuration: `restic_plugin.ini`

### 3.1 INI File Path Resolution

1. **Primary Lookup:** inside the directory containing the plugin DLL (`<plugin_dir>\restic_plugin.ini`), resolved at load time in `DllMain` via `GetModuleFileNameW`.
2. **Fallback:** if the primary file is missing or inaccessible (e.g., when installed under protected paths such as `C:\Program Files\`), the plugin uses the directory supplied by Total Commander via `FsSetDefaultParams` (`DefaultIniName`, typically `%APPDATA%\GHISLER\`) and looks up or creates `restic_plugin.ini` in that directory.

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
3. Fallback to `restic.exe` resolved via the system `PATH` environment variable (`SearchPathW`).

---

## 4. Virtual Navigation Hierarchy

Total Commander passes backslash-delimited paths rooted at `\`. The plugin maps virtual paths as follows:

| Level | WFX Path | Data Source | Action / Presentation |
| --- | --- | --- | --- |
| **0. Root** | `\` | `[Repositories]` section of `restic_plugin.ini` | Returns each configured alias (e.g., `BackupLocale`, `ServerLinux`) as a virtual directory (`FILE_ATTRIBUTE_DIRECTORY`). |
| **1. Repository** | `\<RepoAlias>\` | `restic snapshots --json` | Executes Restic with `RESTIC_REPOSITORY` and `RESTIC_PASSWORD`. Returns each snapshot as a directory (`YYYY-MM-DD_HH-MM-SS_[short_id]`). |
| **2. Snapshot/Subdir** | `\<RepoAlias>\<SnapshotDir>\<SubPath>` | `restic ls <snapshot_id> --json` | Lists files and folders at the requested directory depth, populating `WIN32_FIND_DATAW` (64-bit file sizes, timestamps, attributes). |

---

## 5. Implemented WFX API (Phase 1 - Read-Only MVP)

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

*Note:* Modification functions (`FsGetFile`, `FsPutFile`, `FsDeleteFile`, `FsMkDir`) are intentionally omitted in Phase 1 to enforce strict read-only browsing.

---

## 6. Source Tree Structure

```text
totalcmd_plugin/
├── Makefile                    # NMake incremental build & test rules (Debug/Release/ASan/Analyze)
├── build.sh                    # Bash wrapper loading Portable MSVC x64 + invoking NMake
├── .clang-format               # Code formatting rules (C11, 4 spaces, 100 cols)
├── restic_wfx.def              # Explicit DLL exports to prevent MSVC name mangling
├── cJSON/
│   ├── cJSON.h                 # cJSON parser header
│   └── cJSON.c                 # cJSON parser implementation
├── include/
│   ├── fsplugin.h              # Official Total Commander WFX API declarations
│   └── config.h                # INI configuration & binary resolution interface
├── src/
│   ├── main.c                  # DllMain, WFX entry points, virtual path routing
│   ├── config.c                # restic_plugin.ini resolution & parsing
│   └── restic_cli.c            # Child process execution & Win32 anonymous pipes (planned)
└── tests/
    └── test_config.c           # Unit test suite for configuration & root enumeration
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

### 7.2 Invoking `restic.exe` via Win32 Pipes

- Create an anonymous pipe via `CreatePipe` with `bInheritHandle = TRUE` enabled only on the write handle.
- Configure `STARTUPINFOW` with `dwFlags = STARTF_USESTDHANDLES` and `hStdOutput = hWritePipe`.
- Build a UTF-16 environment block (`CREATE_UNICODE_ENVIRONMENT`) containing:
  - `RESTIC_REPOSITORY=<target>`
  - `RESTIC_PASSWORD=<session_password>`
- Spawn the process via `CreateProcessW(..., CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, ...)`.
- Immediately close `hWritePipe` in the parent process so `ReadFile` on `hReadPipe` receives `EOF` upon child termination.

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

### 8.6 Build & Test Commands (`build.sh` & `Makefile`)

```bash
./build.sh debug     # Incremental Debug build (build/restic.wfx64, /MTd)
./build.sh release   # Optimized Release build (build/restic.wfx64, /MT)
./build.sh test      # Build and run unit tests with CRT heap & leak verification
./build.sh asan      # Rebuild and run unit tests under MSVC AddressSanitizer
./build.sh analyze   # Rebuild plugin & tests with MSVC Static Analyzer (/analyze)
./build.sh clean     # Remove build/ directory
```
