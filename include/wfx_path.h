#pragma once
#include <windows.h>

/**
 * Virtual path hierarchy levels inside the WFX plugin.
 */
typedef enum {
    WFX_PATH_ROOT = 0,     /* "\" -> list configured repository aliases */
    WFX_PATH_REPO,         /* "\<RepoAlias>" -> list snapshots */
    WFX_PATH_SNAPSHOT_DIR, /* "\<RepoAlias>\<SnapshotDir>[\<SubPath>]" -> list directory */
    WFX_PATH_INVALID       /* Malformed path or invalid snapshot directory name */
} WfxPathLevel;

/**
 * Decomposed components of a WFX virtual path.
 */
typedef struct {
    WfxPathLevel level;
    WCHAR repo_alias[MAX_PATH];
    char snapshot_id[65];
    const WCHAR* subpath; /* Points into the caller's Path buffer (or L"") */
} WfxParsedPath;

/**
 * Splits a plugin-namespace path into level, repository alias, snapshot ID and subpath.
 * Returns FALSE when the path is malformed or the second segment is not a snapshot directory
 * name ("YYYY-MM-DD_HH-MM-SS_[short_id]"). `out->subpath` aliases the caller's buffer.
 */
BOOL wfx_parse_path(const WCHAR* path, WfxParsedPath* out);

/**
 * Detects a trailing segment equal to the virtual statistics report name ("!restic_stats.html")
 * and copies the path without it into `out_parent` (e.g. L"\\Repo" or L"\\Repo\\Snap\\dir").
 * `*out_is_report` is always set; the return value is FALSE only when `out_parent` is too small.
 */
BOOL wfx_path_strip_report_segment(
    const WCHAR* path,
    WCHAR* out_parent,
    size_t out_capacity,
    BOOL* out_is_report
);
