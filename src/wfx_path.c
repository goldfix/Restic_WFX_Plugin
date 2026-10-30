#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <wchar.h>
#include <string.h>
#include "wfx_path.h"
#include "restic_cli.h"
#include "restic_report.h"

BOOL wfx_parse_path(const WCHAR* path, WfxParsedPath* out) {
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out));
    out->level   = WFX_PATH_INVALID;
    out->subpath = L"";

    if (!path) return FALSE;

    const WCHAR* p = path;
    while (*p == L'\\') {
        p++;
    }
    if (*p == L'\0') {
        out->level = WFX_PATH_ROOT;
        return TRUE;
    }

    /* Segment 1: <RepoAlias> */
    const WCHAR* seg_start = p;
    while (*p != L'\0' && *p != L'\\') {
        p++;
    }
    size_t seg_len = (size_t)(p - seg_start);
    if (seg_len == 0 || seg_len >= MAX_PATH) {
        return FALSE;
    }
    wcsncpy_s(out->repo_alias, MAX_PATH, seg_start, seg_len);

    while (*p == L'\\') {
        p++;
    }
    if (*p == L'\0') {
        out->level = WFX_PATH_REPO;
        return TRUE;
    }

    /* Segment 2: <SnapshotDir> ("YYYY-MM-DD_HH-MM-SS_[short_id]") */
    seg_start = p;
    while (*p != L'\0' && *p != L'\\') {
        p++;
    }
    seg_len = (size_t)(p - seg_start);
    if (seg_len == 0 || seg_len >= MAX_PATH) {
        return FALSE;
    }

    WCHAR snapshot_dirname[MAX_PATH] = {0};
    wcsncpy_s(snapshot_dirname, MAX_PATH, seg_start, seg_len);
    if (!restic_extract_snapshot_id(snapshot_dirname, out->snapshot_id, sizeof(out->snapshot_id))) {
        return FALSE;
    }

    /* Remainder (empty or starting with '\') is the subpath inside the snapshot */
    out->subpath = p;
    out->level   = WFX_PATH_SNAPSHOT_DIR;
    return TRUE;
}

BOOL wfx_path_strip_report_segment(
    const WCHAR* path,
    WCHAR* out_parent,
    size_t out_capacity,
    BOOL* out_is_report
) {
    if (out_is_report) *out_is_report = FALSE;
    if (!path || !out_parent || out_capacity == 0) return FALSE;
    out_parent[0] = L'\0';

    const WCHAR* last_separator = wcsrchr(path, L'\\');
    const WCHAR* name           = last_separator ? last_separator + 1 : path;
    if (_wcsicmp(name, RESTIC_REPORT_FILENAME_W) != 0) {
        return wcsncpy_s(out_parent, out_capacity, path, _TRUNCATE) == 0;
    }

    /* The report name is the last segment: the parent path is everything before it. A path made of
       the report name alone yields an empty parent, which is the plugin root and has no report. */
    size_t parent_length = last_separator ? (size_t)(last_separator - path) : 0;
    if (parent_length + 1 > out_capacity) return FALSE;
    if (parent_length > 0) {
        wmemcpy(out_parent, path, parent_length);
    }
    out_parent[parent_length] = L'\0';
    if (out_is_report) *out_is_report = TRUE;
    return TRUE;
}
