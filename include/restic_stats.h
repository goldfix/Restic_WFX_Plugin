#pragma once
#include <windows.h>
#include <stddef.h>
#include "restic_cli.h"

/* Fixed buffer sizes of ResticSnapshotInfo (all arrays are fixed so a snapshot can be copied) */
#define RESTIC_STATS_ID_MAX      65  /* 64 hex chars + NUL */
#define RESTIC_STATS_SHORT_ID_MAX 9  /* 8 hex chars + NUL */
#define RESTIC_STATS_HOST_MAX    128
#define RESTIC_STATS_USER_MAX    128
#define RESTIC_STATS_PROGRAM_MAX 64
#define RESTIC_STATS_TAGS_MAX    512
#define RESTIC_STATS_PATHS_MAX   512

/* Counters are saturated at this value (the largest signed 64-bit number, as seen by Total Commander) */
#define RESTIC_STATS_COUNTER_MAX 9223372036854775807ULL

/**
 * All metadata published by `restic snapshots --json` for a single snapshot.
 * Every string is UTF-8, exactly as printed by Restic. `tag_count`/`path_count` remember how many
 * elements were joined into the fixed `tags`/`paths` buffers (which may be truncated).
 */
typedef struct {
    char id[RESTIC_STATS_ID_MAX];            /* Full 64-character snapshot ID */
    char short_id[RESTIC_STATS_SHORT_ID_MAX];/* First 8 hex characters */
    char parent[RESTIC_STATS_ID_MAX];         /* Parent snapshot ID, empty for the first one */
    char hostname[RESTIC_STATS_HOST_MAX];
    char username[RESTIC_STATS_USER_MAX];
    char program_version[RESTIC_STATS_PROGRAM_MAX];
    char tags[RESTIC_STATS_TAGS_MAX];        /* ", " separated tag list */
    char paths[RESTIC_STATS_PATHS_MAX];      /* "; " separated source path list */
    unsigned int tag_count;
    unsigned int path_count;
    FILETIME time;                           /* Snapshot creation time (UTC) */
    BOOL has_summary;                        /* FALSE for snapshots written without a summary */
    FILETIME backup_start;
    FILETIME backup_end;
    unsigned long long files_new;
    unsigned long long files_changed;
    unsigned long long files_unmodified;
    unsigned long long dirs_new;
    unsigned long long dirs_changed;
    unsigned long long dirs_unmodified;
    unsigned long long data_blobs;
    unsigned long long tree_blobs;
    unsigned long long data_added;
    unsigned long long data_added_packed;
    unsigned long long total_files_processed;
    unsigned long long total_bytes_processed;
    unsigned long long duration_seconds;     /* backup_end - backup_start, 0 when unknown */
} ResticSnapshotInfo;

/**
 * Parses the array printed by `restic snapshots --json` into detailed snapshot records.
 * Entries without a parsable `time` or ID are skipped, so the result stays index-aligned with
 * `restic_parse_snapshots_json()` (the directory listing parser) for the same input.
 * A literal `null` (empty repository) yields 0 items. Caller frees with restic_free_snapshot_list().
 */
ResticResult restic_parse_snapshot_list(
    const char* json_text,
    ResticSnapshotInfo** out_items,
    size_t* out_count
);

/** Releases an array returned by restic_parse_snapshot_list() (NULL is accepted). */
void restic_free_snapshot_list(ResticSnapshotInfo* items);

/**
 * Finds a snapshot by its short ID (case-insensitive, first match wins).
 * Returns NULL when not found. The result borrows the caller's array.
 */
const ResticSnapshotInfo* restic_find_snapshot_by_short_id(
    const ResticSnapshotInfo* items,
    size_t count,
    const char* short_id
);

/* ------------------------------------------------------------------------------------------ */
/* Per-alias cache                                                                            */
/*                                                                                            */
/* FsContentGetValue() is called once per visible file, so the parsed snapshot records are    */
/* kept in a small LRU table keyed by repository alias. Lookups return a copy of the record,  */
/* which keeps the API free of lock-lifetime constraints.                                     */
/* ------------------------------------------------------------------------------------------ */

/** Stores (deep copy) the snapshot records of `alias`, replacing a previous entry. */
BOOL snapshot_stats_store(const WCHAR* alias, const ResticSnapshotInfo* items, size_t count);

/** Copies the record with short ID `short_id` of `alias` into `*out_info`. */
BOOL snapshot_stats_lookup(const WCHAR* alias, const char* short_id, ResticSnapshotInfo* out_info);

/**
 * Finds a snapshot by short ID in every cached repository. Succeeds only when the ID identifies a
 * single snapshot: two aliases may point to the same repository (same ID, same data), but two
 * different snapshots with the same short ID are treated as ambiguous and rejected.
 */
BOOL snapshot_stats_lookup_any(const char* short_id, ResticSnapshotInfo* out_info);

/**
 * Renders the cached aliases as "Alias(count) ..." for the content diagnostic trace.
 * Returns the number of occupied slots; `out` is always 0-terminated.
 */
size_t snapshot_stats_describe(char* out, size_t capacity);

/** Copies up to `capacity` records of `alias` into `out_items`; `*out_count` receives the total. */
BOOL snapshot_stats_lookup_alias(
    const WCHAR* alias,
    ResticSnapshotInfo* out_items,
    size_t capacity,
    size_t* out_count
);

/** Drops the cached entry of `alias` (no-op when absent). */
void snapshot_stats_invalidate(const WCHAR* alias);

/** Drops every cached entry. */
void snapshot_stats_clear(void);

/** Returns the number of occupied cache slots (test/diagnostic helper). */
size_t snapshot_stats_cached_aliases(void);
