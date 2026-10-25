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
