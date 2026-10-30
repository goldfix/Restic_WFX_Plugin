#pragma once
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include "restic_cli.h"
#include "restic_stats.h"

/*
 * Virtual statistics report.
 *
 * Every repository root, snapshot root and snapshot subfolder lists one extra read-only virtual
 * file, "!restic_stats.html". Total Commander extracts it with F3 / Ctrl+Q / F5 through FsGetFileW,
 * which runs the required restic.exe queries and writes a self-contained HTML report.
 *
 * The report renders acceptably in both Lister modes: plain tables and <pre> blocks for the native
 * HTML text mode (5), plus CSS for the browser / Explorer preview mode (8).
 */

#define RESTIC_REPORT_FILENAME_W L"!restic_stats.html"

/* Size reported in the file panel before the report is generated (purely informational) */
#define RESTIC_REPORT_PLACEHOLDER_SIZE 16384ULL

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
/* Virtual file plumbing                                                                       */
/* ------------------------------------------------------------------------------------------ */

/**
 * Fills the directory entry of the virtual report file for a repository root (`snapshot_id` NULL)
 * or a snapshot folder. The timestamp comes from the statistics cache when available.
 * Returns FALSE only for invalid parameters.
 */
BOOL restic_report_fill_find_data(
    const WCHAR* repository_alias,
    const char* snapshot_id,
    WIN32_FIND_DATAW* find_data
);

/**
 * Inserts the virtual report as the first entry of a directory listing (`*items` is a malloc
 * array, grown with realloc). Nothing is added when the listing already contains a file with the
 * same name: a backed-up file called "!restic_stats.html" must stay reachable. Returns TRUE when
 * the entry was added.
 */
BOOL restic_report_prepend_entry(
    WIN32_FIND_DATAW** items,
    size_t* count,
    const WCHAR* repository_alias,
    const char* snapshot_id
);

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

/* Writes "[NNNN░░░░]" using U+2588 / U+2591, `width` blocks wide. */
void restic_report_format_bar(char* out, size_t out_capacity, double ratio, int width);

/* Formats a byte count as "1.23 GB" (B, KB, MB, GB, TB). */
void restic_report_format_bytes(unsigned long long bytes, char* out, size_t out_capacity);

/* Formats an integer with thousands separators ("12,480"). */
void restic_report_format_uint(unsigned long long value, char* out, size_t out_capacity);

/* Formats a FILETIME as "YYYY-MM-DD HH:MM:SS" (UTC). Returns FALSE for unknown/invalid times. */
BOOL restic_report_format_time(const FILETIME* file_time, char* out, size_t out_capacity);

/* Writes `text` to `fp`, escaping &, <, > and " (never write a path or host name unescaped). */
void restic_report_escape_write(FILE* fp, const char* text);

/**
 * Copies a repository location for display, masking the credentials of a URL
 * ("rest:https://user:secret@host/repo" -> "rest:https://***@host/repo"). The report file can be
 * copied or shared, so it must never carry a password. Other locations are copied unchanged.
 */
void restic_report_redact_location(const char* location, char* out, size_t capacity);

/* ------------------------------------------------------------------------------------------ */
/* HTML generation                                                                             */
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
void restic_report_write_repo_html(FILE* fp, const ResticReportRepo* report);
void restic_report_write_snapshot_html(FILE* fp, const ResticReportSnapshot* report);
void restic_report_write_subtree_html(FILE* fp, const ResticReportSubtree* report);

/**
 * Generates the report for a virtual path and writes it to `local_dest_path`.
 *
 * - `snapshot_id` NULL: repository report; otherwise snapshot report.
 * - `subpath_w` NULL or empty: snapshot root; otherwise subfolder report below `subpath_w`.
 * - `progress` (optional) is called with increasing percentages; TRUE aborts the generation.
 * - `options` (optional) supplies the inactivity timeout and the ESC polling callback.
 * - The file is written to a temporary file in the destination directory and moved over the target
 *   only on success, so a cancellation never leaves a truncated report behind.
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
