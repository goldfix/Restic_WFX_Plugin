#pragma once
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include "restic_cli.h"
#include "restic_stats.h"

/*
 * Statistics report.
 *
 * The report is not a file of the listings any more: the user puts the cursor on a repository,
 * a snapshot or a snapshot subfolder and presses F3 (intercepted through FS_STATUS_OP_CALCSIZE)
 * or Alt+Enter (FsExecuteFileW). The plugin runs the required restic.exe queries, writes a
 * self-contained Markdown document to %TEMP% and opens the Total Commander Lister on it.
 *
 * Markdown was chosen over HTML because the native Lister shows it as perfectly aligned plain
 * text (the report is pure ASCII outside the titles: pipe tables and '#'/'.' bars), while a
 * Markdown Lister plugin or an external editor renders it graphically. HTML mode 5 of the Lister
 * rewrote the spacing around tags and the Unicode block characters of the first version
 * overlapped vertically and changed width through font fallback.
 */

/* Name prefix of the generated document in %TEMP%: <prefix><alias>[_<short_id>][_<leaf>].md */
#define RESTIC_REPORT_TEMP_PREFIX L"restic_stats_"

/* Collection limits: the reports never grow beyond these bounds, whatever the repository size */
#define RESTIC_REPORT_MAX_DIFF_CHANGES 100
#define RESTIC_REPORT_MAX_SUBDIRS      128
#define RESTIC_REPORT_MAX_EXTENSIONS   128
#define RESTIC_REPORT_TOP_SUBDIRS       15
#define RESTIC_REPORT_TOP_EXTENSIONS    12
#define RESTIC_REPORT_TOP_FILES         15
#define RESTIC_REPORT_HIST_BUCKETS       6
#define RESTIC_REPORT_PATH_MAX         512
#define RESTIC_REPORT_NAME_MAX         256
#define RESTIC_REPORT_EXT_MAX           32
#define RESTIC_REPORT_TIME_MAX          32

/*
 * Progress callback used while a report is generated. `percent` is 0..100 (0 = indeterminate).
 * Return TRUE to abort: the running restic.exe child is terminated and the temporary file removed.
 */
typedef BOOL (*ResticReportProgressProc)(int percent, void* user_data);

/* Output of `restic stats` (both --mode restore-size and --mode raw-data) */
typedef struct {
    BOOL valid;                          /* The command succeeded and the payload was usable */
    unsigned long long total_size;        /* restore-size: logical size; raw-data: stored size */
    unsigned long long total_file_count;  /* restore-size only */
    unsigned long long uncompressed_size; /* raw-data only */
    unsigned long long blob_count;        /* raw-data only */
    double compression_ratio;             /* 1.0 when the repository is not compressed */
    double space_saving_percent;
} ResticReportStorage;

/* One entry of `restic diff --json` (message_type "change") */
typedef struct {
    char modifier[8]; /* "+", "-", "M", "T"... */
    char path[RESTIC_REPORT_PATH_MAX];
} ResticReportDiffChange;

/* Summary of `restic diff --json <parent> <snapshot>` */
typedef struct {
    BOOL valid;                          /* A diff could be computed */
    char parent_short_id[RESTIC_STATS_SHORT_ID_MAX];
    unsigned long long changed_files;
    unsigned long long added_files;
    unsigned long long added_dirs;
    unsigned long long added_bytes;
    unsigned long long added_blobs;
    unsigned long long removed_files;
    unsigned long long removed_dirs;
    unsigned long long removed_bytes;
    unsigned long long removed_blobs;
    ResticReportDiffChange changes[RESTIC_REPORT_MAX_DIFF_CHANGES];
    size_t change_count;                 /* Entries kept for display (<= RESTIC_REPORT_MAX_DIFF_CHANGES) */
    unsigned long long change_events;    /* Entries seen in the stream */
} ResticReportDiff;

/* Aggregation of one direct subfolder: every file below it, at any depth */
typedef struct {
    char name[RESTIC_REPORT_NAME_MAX];
    unsigned long long total_size;
    unsigned long long file_count;
} ResticReportSubdir;

/* Aggregation of one file extension */
typedef struct {
    char ext[RESTIC_REPORT_EXT_MAX];
    unsigned long long total_size;
    unsigned long long file_count;
} ResticReportExtension;

/* One entry of the "largest files" ranking */
typedef struct {
    char rel_path[RESTIC_REPORT_PATH_MAX];
    char mtime[RESTIC_REPORT_TIME_MAX]; /* "YYYY-MM-DD HH:MM:SS" (UTC), empty when unknown */
    unsigned long long size;
} ResticReportLargestFile;

/*
 * Recursive aggregation of a snapshot subfolder (`restic ls --json --recursive`), filled one NDJSON
 * line at a time so a snapshot with hundreds of thousands of nodes never has to fit in memory.
 */
typedef struct {
    BOOL root_seen;                      /* Restic printed the filtered folder itself */
    unsigned long long total_size;
    unsigned long long total_files;
    unsigned long long direct_files;     /* Files directly in the folder */
    unsigned long long direct_size;
    unsigned long long total_dirs;
    unsigned long long direct_dirs;
    char oldest_mtime[RESTIC_REPORT_TIME_MAX];
    char newest_mtime[RESTIC_REPORT_TIME_MAX];
    ResticReportSubdir subdirs[RESTIC_REPORT_MAX_SUBDIRS];
    size_t subdir_count;
    ResticReportExtension extensions[RESTIC_REPORT_MAX_EXTENSIONS];
    size_t ext_count;
    /* Size buckets: 0 B, <10 KB, <100 KB, <1 MB, <10 MB, >=10 MB */
    unsigned long long hist_files[RESTIC_REPORT_HIST_BUCKETS];
    unsigned long long hist_bytes[RESTIC_REPORT_HIST_BUCKETS];
    ResticReportLargestFile largest[RESTIC_REPORT_TOP_FILES];
    size_t largest_count;
} ResticReportFolder;

/* ------------------------------------------------------------------------------------------ */
/* Opening the report                                                                          */
/* ------------------------------------------------------------------------------------------ */

/**
 * Builds the deterministic local path of the report in %TEMP%:
 *   repository: %TEMP%\restic_stats_<alias>.md
 *   snapshot:   %TEMP%\restic_stats_<alias>_<short_id>.md
 *   subfolder:  %TEMP%\restic_stats_<alias>_<short_id>_<leaf>.md
 * Characters Windows does not accept in a file name are replaced and the result stays well below
 * MAX_PATH. The speaking name is what the Total Commander Lister shows in its title bar.
 * Returns FALSE for invalid arguments or a buffer that is too small.
 */
BOOL restic_report_build_temp_path(
    const WCHAR* repository_alias,
    const char* snapshot_id,
    const WCHAR* subpath,
    WCHAR* out_path,
    size_t out_capacity
);

/**
 * Makes sure the report can be written over `path`. The Lister of an earlier report may still keep
 * the file open without FILE_SHARE_DELETE, which would make the atomic replacement fail; in that
 * case " (2)" ... " (9)" is inserted before the ".md" extension until a free name is found and
 * `path` is updated in place. Returns FALSE when no usable name exists or on invalid arguments.
 */
BOOL restic_report_pick_destination(WCHAR* path, size_t capacity);

/**
 * Opens `local_report_path` in the Total Commander Lister ("<tc_exe>" /S=L "<file>").
 *
 * %COMMANDER_EXE% (set by Total Commander) names the host executable; when it is missing the host
 * module path is used instead. The process is spawned and not waited for. A test hook
 * (restic_report_set_lister_test_hook) or the environment variable RESTIC_WFX_NO_LISTER_GUI=1
 * replaces the launch, so automated tests never open windows.
 */
BOOL restic_report_open_in_lister(const WCHAR* local_report_path);

/* Installs (or removes with NULL) the hook used instead of launching the Lister. The hook receives
   the report path and returns TRUE when it has handled the request. */
typedef BOOL (*ResticReportListerHook)(const WCHAR* local_report_path);
void restic_report_set_lister_test_hook(ResticReportListerHook hook);

/* ------------------------------------------------------------------------------------------ */
/* Parsers and aggregation (pure functions, unit tested)                                       */
/* ------------------------------------------------------------------------------------------ */

/* Parses the JSON object printed by `restic stats --json` (both modes). */
BOOL restic_report_parse_storage_json(const char* json_text, ResticReportStorage* out_stats);

/* Parses one line of `restic diff --json` into `diff`. Unknown lines are ignored. */
void restic_report_parse_diff_line(ResticReportDiff* diff, const char* line, size_t length);

/* Aggregates one line of `restic ls --json --recursive <snapshot> <filter>` into `folder`.
   Sets `folder->root_seen` when Restic prints the filtered folder itself. */
void restic_report_aggregate_ls_line(ResticReportFolder* folder, const char* filter_path,
                                     const char* line, size_t length);

/* Sorts the collected buckets by size (descending) and drops unused slots. */
void restic_report_finalize_folder(ResticReportFolder* folder);

/* ------------------------------------------------------------------------------------------ */
/* Formatting helpers (UTF-8 input/output, unit tested)                                        */
/* ------------------------------------------------------------------------------------------ */

/* Writes "[####....]", `width` characters wide, in pure ASCII (see format_bar). */
void restic_report_format_bar(char* out, size_t out_capacity, double ratio, int width);

/* Formats a byte count as "1.23 GB" (B, KB, MB, GB, TB). */
void restic_report_format_bytes(unsigned long long bytes, char* out, size_t out_capacity);

/* Formats an integer with thousands separators ("12,480"). */
void restic_report_format_uint(unsigned long long value, char* out, size_t out_capacity);

/* Formats a FILETIME as "YYYY-MM-DD HH:MM:SS" (UTC). Returns FALSE for unknown/invalid times. */
BOOL restic_report_format_time(const FILETIME* file_time, char* out, size_t out_capacity);

/*
 * Writes `text` as the content of a Markdown table cell: a pipe becomes '/' (a real pipe would
 * start a new column, and '/' keeps the cell one character wide), CR / LF / TAB become spaces.
 * Everything else (&, <, >, ", ') is written verbatim: the report is no longer HTML, and escaping
 * it would make paths unreadable in the Lister.
 */
void restic_report_write_text(FILE* fp, const char* text);

/**
 * Copies a repository location for display, masking the credentials of a URL
 * ("rest:https://user:secret@host/repo" -> "rest:https://***@host/repo"). The report file can be
 * copied or shared, so it must never carry a password. Other locations are copied unchanged.
 */
void restic_report_redact_location(const char* location, char* out, size_t capacity);

/* ------------------------------------------------------------------------------------------ */
/* Markdown generation                                                                         */
/* ------------------------------------------------------------------------------------------ */

/* Report payloads; strings are UTF-8. `repo_target` may be empty, `alias` must not be NULL. */
typedef struct {
    const char* alias;
    const char* repo_target;
    const ResticSnapshotInfo* snapshots; /* Oldest first, as printed by `restic snapshots` */
    size_t snapshot_count;
    ResticReportStorage restore; /* Whole repository, --mode restore-size */
    ResticReportStorage stored;  /* Whole repository, --mode raw-data */
} ResticReportRepo;

typedef struct {
    const char* alias;
    const char* repo_target;
    const ResticSnapshotInfo* info;
    ResticReportStorage restore; /* This snapshot, --mode restore-size */
    ResticReportStorage stored;  /* This snapshot, --mode raw-data */
    const ResticReportDiff* diff; /* Borrowed: the structure is too large to be copied */
    const char* diff_note;       /* Shown instead of the change list when no diff is available */
} ResticReportSnapshot;

typedef struct {
    const char* alias;
    const char* repo_target;
    const char* snapshot_short_id;
    const char* snapshot_time;   /* "YYYY-MM-DD HH:MM:SS" (UTC) */
    const char* hostname;
    const char* display_path;    /* Path as shown in Total Commander (UTF-8) */
    const ResticReportFolder* folder;
} ResticReportSubtree;

/* Writers take an already opened text file (unit tested without running restic) */
void restic_report_write_repo_md(FILE* fp, const ResticReportRepo* report);
void restic_report_write_snapshot_md(FILE* fp, const ResticReportSnapshot* report);
void restic_report_write_subtree_md(FILE* fp, const ResticReportSubtree* report);

/**
 * Drops the one-slot cache of the last generated report. It is filled by restic_report_generate()
 * and answers a repeated request (F3 twice, or Alt+Enter after F3) in memory, without running
 * restic.exe again. An entry expires after one minute and is dropped by FsInitW,
 * FsContentPluginUnloading and DLL_PROCESS_DETACH, so it is never older than that.
 */
void restic_report_clear_cache(void);

/**
 * Generates the report for a virtual path and writes it to `local_dest_path`.
 *
 * - `snapshot_id` NULL: repository report; otherwise snapshot report.
 * - `subpath_w` NULL or empty: snapshot root; otherwise subfolder report below `subpath_w`.
 * - `progress` (optional) is called with increasing percentages; TRUE aborts the generation.
 * - `options` (optional) supplies the inactivity timeout and the ESC polling callback.
 * - The file is written to a temporary file in the destination directory and moved over the target
 *   only on success, so a cancellation never leaves a truncated report behind.
 * - The snapshot list, the snapshot metadata and the restore size come from the statistics cache
 *   when it is warm (0 extra restic.exe processes); only the figures that the cache cannot provide
 *   (`stats --mode raw-data`, `diff`, the recursive `ls` of a folder report) run a live query.
 * - A second identical request within one minute is answered from memory.
 * - Secondary query failures degrade the report instead of failing it; RESTIC_ERR_* is returned
 *   only when the report itself cannot be produced.
 */
ResticResult restic_report_generate(
    const WCHAR* restic_exe,
    const WCHAR* repo_target,
    const WCHAR* password,
    const WCHAR* repository_alias,
    const char* snapshot_id,
    const WCHAR* subpath_w,
    const WCHAR* local_dest_path,
    const ResticExecOptions* options,
    ResticReportProgressProc progress,
    void* progress_ctx,
    WCHAR* out_error_msg,
    size_t err_capacity
);
