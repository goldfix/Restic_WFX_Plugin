#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#else
#include <stdlib.h>
#endif
#include <string.h>
#include <stdio.h>
#include "fsplugin.h"
#include "fscontent.h"
#include "restic_stats.h"
#include "wfx_path.h"
#include "wfx_trace.h"

/*
 * Custom columns ("Content" part of the WFX interface).
 *
 * Every published column is backed by data already downloaded by `restic snapshots --json` while
 * the snapshot list of a repository is enumerated (or filled once by the directory listing when a
 * snapshot folder is entered directly), so no field ever starts a Restic process and no field is
 * delayed. Values are read from the per-alias snapshot cache (see restic_stats.h),
 * which makes FsContentGetValue() cheap enough to be called once per visible entry.
 *
 * Fields are available for the snapshot folders and for every entry below them (files and
 * sub-folders inherit the statistics of the enclosing snapshot); root and alias rows are empty.
 */

/* Field identifiers; the order is also the FsContentGetSupportedField() enumeration order */
enum {
    CF_TIME = 0,
    CF_HOSTNAME,
    CF_TAGS,
    CF_FILES,
    CF_SIZE,
    CF_DATA_ADDED,
    CF_DURATION,
    CF_SHORT_ID,
    CF_ID,
    CF_PARENT,
    CF_USERNAME,
    CF_PATHS,
    CF_PROGRAM_VERSION,
    CF_BACKUP_START,
    CF_BACKUP_END,
    CF_FILES_NEW,
    CF_FILES_CHANGED,
    CF_FILES_UNMODIFIED,
    CF_DIRS_NEW,
    CF_DIRS_CHANGED,
    CF_DIRS_UNMODIFIED,
    CF_DATA_BLOBS,
    CF_TREE_BLOBS,
    CF_DATA_ADDED_PACKED,
    CF_AVG_FILE_SIZE,
    CF_PACKED_RATIO,
    CF_HAS_SUMMARY,
    CF_COUNT
};

/* Unit strings offered to Total Commander (vertical dash separated, as required by the SDK) */
#define UNITS_BYTES "bytes|kbytes|Mbytes|Gbytes"
#define UNITS_TIME  "s|min"

typedef struct {
    const char* name;  /* Field name: no dot, vertical dash or colon */
    int type;          /* ft_* value returned to Total Commander */
    const char* units; /* Empty string when the field has no units */
} ContentField;

static const ContentField kFields[CF_COUNT] = {
    { "time",              ft_datetime,         ""          },
    { "hostname",          ft_string,           ""          },
    { "tags",              ft_string,           ""          },
    { "files",             ft_numeric_64,       ""          },
    { "size",              ft_numeric_64,       UNITS_BYTES },
    { "data_added",        ft_numeric_64,       UNITS_BYTES },
    { "duration",          ft_numeric_64,       UNITS_TIME  },
    { "short_id",          ft_string,           ""          },
    { "id",                ft_string,           ""          },
    { "parent",            ft_string,           ""          },
    { "username",          ft_string,           ""          },
    { "paths",             ft_string,           ""          },
    { "program_version",   ft_string,           ""          },
    { "backup_start",      ft_datetime,         ""          },
    { "backup_end",        ft_datetime,         ""          },
    { "files_new",         ft_numeric_64,       ""          },
    { "files_changed",     ft_numeric_64,       ""          },
    { "files_unmodified",  ft_numeric_64,       ""          },
    { "dirs_new",          ft_numeric_64,       ""          },
    { "dirs_changed",      ft_numeric_64,       ""          },
    { "dirs_unmodified",   ft_numeric_64,       ""          },
    { "data_blobs",        ft_numeric_64,       ""          },
    { "tree_blobs",        ft_numeric_64,       ""          },
    { "data_added_packed", ft_numeric_64,       UNITS_BYTES },
    { "avg_file_size",     ft_numeric_64,       UNITS_BYTES },
    { "packed_ratio",      ft_numeric_floating, ""          },
    { "has_summary",       ft_boolean,          ""          },
};

/* Default custom columns view applied when Total Commander enters the plugin.
 * The plugin's own fields are referenced with the "<fs>" self name ("[=<fs>.time]"), the form
 * Total Commander stores in wincmd.ini for a file system plugin (see [CustomFields_PluginName] in
 * the TC help and the svn_wfx sample plugin). It keeps the view working whatever display name the
 * plugin was registered under ("Restic Backups" here, which even contains a space).
 * "\\n" is a literal backslash + 'n' separator, as required by the plugin interface. */
static const char kDefaultViewContents[] =
    "[=<fs>.time]\\n"
    "[=<fs>.hostname]\\n"
    "[=<fs>.tags]\\n"
    "[=<fs>.files]\\n"
    "[=<fs>.size.Mbytes]\\n"
    "[=<fs>.data_added.Mbytes]\\n"
    "[=<fs>.duration]";
static const char kDefaultViewHeaders[] =
    "Date\\nHost\\nTags\\nFiles\\nSize\\nAdded\\nDuration";
/* The first two widths belong to the name and extension columns */
static const char kDefaultViewWidths[]    = "200,60,110,120,140,-60,-70,-70,-60";
static const char kDefaultViewOptions[]   = "0|1";

/* Total Commander limits plugin paths to 1023 characters */
#define CONTENT_PATH_MAX 1025

/* Reports the content of the cache once, so an empty column can be told apart from an empty cache */
static void trace_cache_state(void) {
    static LONG reported = 0;
    if (!wfx_trace_enabled() || InterlockedIncrement(&reported) != 1) return;
    char description[512] = {0};
    snapshot_stats_describe(description, sizeof(description));
    wfx_trace_printf("cache: [%s]", description);
}

/* ------------------------------------------------------------------------------------------ */
/* Value writers                                                                              */
/* ------------------------------------------------------------------------------------------ */

/*
 * Writes a UTF-8 string (as printed by Restic) in the ANSI code page: the ANSI content entry points
 * have no Unicode variant, so non-ASCII host names, tags or paths would otherwise show up as
 * mojibake. Characters missing from the code page become '?'. When the buffer is too small the
 * text is shortened on a character boundary (never mid multi-byte).
 */
static int write_string(const char* text, void* out, int maxlen) {
    if (text == NULL || text[0] == '\0' || out == NULL || maxlen <= 1) {
        return ft_fieldempty;
    }

    WCHAR wide[RESTIC_STATS_PATHS_MAX + 1];
    int wide_chars = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, (int)_countof(wide));
    if (wide_chars <= 1) {
        return ft_fieldempty;
    }

    char* dest = (char*)out;
    /* Every UTF-16 unit needs at least one output byte, so longer prefixes can never fit */
    int length = wide_chars - 1;
    if (length > maxlen - 1) length = maxlen - 1;
    for (; length > 0; --length) {
        int bytes = WideCharToMultiByte(CP_ACP, 0, wide, length, dest, maxlen - 1, NULL, NULL);
        if (bytes > 0) {
            dest[bytes] = '\0';
            return ft_string;
        }
    }
    return ft_fieldempty;
}

/*
 * Unicode counterpart of write_string(): the W content entry points receive a buffer of `maxlen`
 * bytes, so it is `maxlen / sizeof(WCHAR)` wide characters (including the terminator). The UTF-8
 * text is converted as a whole and then truncated, so a multi-byte character is never split.
 */
static int write_string_w(const char* text, void* out, int maxlen) {
    const int capacity = (maxlen > 0) ? maxlen / (int)sizeof(WCHAR) : 0;
    if (text == NULL || text[0] == '\0' || out == NULL || capacity < 2) {
        return ft_fieldempty;
    }

    WCHAR wide[RESTIC_STATS_PATHS_MAX + 1];
    int wide_chars = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, (int)_countof(wide));
    if (wide_chars <= 1) {
        return ft_fieldempty;
    }

    /* wcsncpy_s returns STRUNCATE when it had to shorten the text, which is expected here */
    WCHAR* dest   = (WCHAR*)out;
    dest[0]       = L'\0';
    errno_t error = wcsncpy_s(dest, (size_t)capacity, wide, _TRUNCATE);
    if (error != 0 && error != STRUNCATE) {
        dest[0] = L'\0';
        return ft_fieldempty;
    }
    return ft_stringw;
}

/* Total Commander reads 64-bit numbers as signed: saturate instead of wrapping to a negative value */
static int write_u64(unsigned long long value, void* out, int maxlen) {
    if (out == NULL || maxlen < (int)sizeof(LONGLONG)) return ft_fieldempty;
    if (value > RESTIC_STATS_COUNTER_MAX) value = RESTIC_STATS_COUNTER_MAX;
    *(LONGLONG*)out = (LONGLONG)value;
    return ft_numeric_64;
}

static int write_datetime(const FILETIME* ft, void* out, int maxlen) {
    if (ft == NULL || out == NULL || maxlen < (int)sizeof(FILETIME)) return ft_fieldempty;
    if (ft->dwLowDateTime == 0 && ft->dwHighDateTime == 0) return ft_fieldempty;
    *(FILETIME*)out = *ft;
    return ft_datetime;
}

static int write_double(double value, void* out, int maxlen) {
    if (out == NULL || maxlen < (int)sizeof(double)) return ft_fieldempty;
    *(double*)out = value;
    return ft_numeric_floating;
}

static int write_boolean(BOOL value, void* out, int maxlen) {
    if (out == NULL || maxlen < (int)sizeof(int)) return ft_fieldempty;
    *(int*)out = value ? 1 : 0;
    return ft_boolean;
}

/* Converts a byte count to the unit selected by the user (UnitIndex of the units string) */
static unsigned long long scale_bytes(unsigned long long value, int unit_index) {
    switch (unit_index) {
        case CONTENT_UNIT_KBYTES: return value / 1024ULL;
        case CONTENT_UNIT_MBYTES: return value / (1024ULL * 1024ULL);
        case CONTENT_UNIT_GBYTES: return value / (1024ULL * 1024ULL * 1024ULL);
        default: return value;
    }
}

/* Converts a duration in seconds to the unit selected by the user */
static unsigned long long scale_time(unsigned long long value, int unit_index) {
    return (unit_index == 1) ? (value / 60ULL) : value;
}

/* ------------------------------------------------------------------------------------------ */
/* Field rendering                                                                            */
/* ------------------------------------------------------------------------------------------ */

/* Text fields are written in the code page expected by the entry point that was called */
static int render_text(const char* text, BOOL wide, void* out, int maxlen) {
    return wide ? write_string_w(text, out, maxlen) : write_string(text, out, maxlen);
}

/*
 * Publishes one field value. Returns the ft_* type written to `out`, ft_fieldempty when the
 * snapshot does not carry the value (e.g. no summary) and ft_nosuchfield for an unknown field.
 */
static int render_field(
    int field,
    const ResticSnapshotInfo* info,
    int unit_index,
    void* out,
    int maxlen,
    BOOL wide
) {
    /* Fields produced by the backup summary are only valid when restic published one */
    const BOOL needs_summary = (field == CF_FILES || field == CF_SIZE || field == CF_DATA_ADDED ||
                                field == CF_DURATION || field == CF_BACKUP_START ||
                                field == CF_BACKUP_END || field == CF_FILES_NEW ||
                                field == CF_FILES_CHANGED || field == CF_FILES_UNMODIFIED ||
                                field == CF_DIRS_NEW || field == CF_DIRS_CHANGED ||
                                field == CF_DIRS_UNMODIFIED || field == CF_DATA_BLOBS ||
                                field == CF_TREE_BLOBS || field == CF_DATA_ADDED_PACKED ||
                                field == CF_AVG_FILE_SIZE || field == CF_PACKED_RATIO);
    if (needs_summary && !info->has_summary) {
        return ft_fieldempty;
    }

    switch (field) {
        case CF_TIME: return write_datetime(&info->time, out, maxlen);
        case CF_HOSTNAME: return render_text(info->hostname, wide, out, maxlen);
        case CF_TAGS: return render_text(info->tags, wide, out, maxlen);
        case CF_SHORT_ID: return render_text(info->short_id, wide, out, maxlen);
        case CF_ID: return render_text(info->id, wide, out, maxlen);
        case CF_PARENT: return render_text(info->parent, wide, out, maxlen);
        case CF_USERNAME: return render_text(info->username, wide, out, maxlen);
        case CF_PATHS: return render_text(info->paths, wide, out, maxlen);
        case CF_PROGRAM_VERSION: return render_text(info->program_version, wide, out, maxlen);
        case CF_BACKUP_START: return write_datetime(&info->backup_start, out, maxlen);
        case CF_BACKUP_END: return write_datetime(&info->backup_end, out, maxlen);
        case CF_HAS_SUMMARY: return write_boolean(info->has_summary, out, maxlen);
        case CF_FILES:
            return write_u64(info->total_files_processed, out, maxlen);
        case CF_SIZE:
            return write_u64(scale_bytes(info->total_bytes_processed, unit_index), out, maxlen);
        case CF_DATA_ADDED:
            return write_u64(scale_bytes(info->data_added, unit_index), out, maxlen);
        case CF_DATA_ADDED_PACKED:
            return write_u64(scale_bytes(info->data_added_packed, unit_index), out, maxlen);
        case CF_DURATION:
            return write_u64(scale_time(info->duration_seconds, unit_index), out, maxlen);
        case CF_FILES_NEW: return write_u64(info->files_new, out, maxlen);
        case CF_FILES_CHANGED: return write_u64(info->files_changed, out, maxlen);
        case CF_FILES_UNMODIFIED: return write_u64(info->files_unmodified, out, maxlen);
        case CF_DIRS_NEW: return write_u64(info->dirs_new, out, maxlen);
        case CF_DIRS_CHANGED: return write_u64(info->dirs_changed, out, maxlen);
        case CF_DIRS_UNMODIFIED: return write_u64(info->dirs_unmodified, out, maxlen);
        case CF_DATA_BLOBS: return write_u64(info->data_blobs, out, maxlen);
        case CF_TREE_BLOBS: return write_u64(info->tree_blobs, out, maxlen);
        case CF_AVG_FILE_SIZE:
            if (info->total_files_processed == 0) return ft_fieldempty;
            return write_u64(
                scale_bytes(info->total_bytes_processed / info->total_files_processed, unit_index),
                out,
                maxlen
            );
        case CF_PACKED_RATIO:
            if (info->data_added_packed == 0) return ft_fieldempty;
            return write_double(
                (double)info->data_added / (double)info->data_added_packed, out, maxlen
            );
        default: return ft_nosuchfield;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Path resolution                                                                            */
/* ------------------------------------------------------------------------------------------ */

/* Converts the ANSI plugin path into the snapshot record it belongs to */

/*
 * Scans the segments of the path from the right and accepts a snapshot folder name when it
 * identifies exactly one cached snapshot. Used for the spellings Total Commander may pass for
 * content fields that are not the canonical plugin path (path relative to the current directory,
 * plugin root name prepended, a bare snapshot folder name).
 */
static BOOL lookup_loose(const WCHAR* path, ResticSnapshotInfo* out_info) {
    WCHAR buffer[CONTENT_PATH_MAX] = {0};
    if (wcsncpy_s(buffer, _countof(buffer), path, _TRUNCATE) != 0) return FALSE;

    WCHAR* end = buffer + wcslen(buffer);
    while (end > buffer) {
        while (end > buffer && end[-1] == L'\\') --end; /* Skip trailing separators */
        if (end == buffer) break;

        WCHAR* start = end;
        while (start > buffer && start[-1] != L'\\') --start;

        const WCHAR saved = *end;
        *end              = L'\0';
        char short_id[RESTIC_STATS_SHORT_ID_MAX] = {0};
        BOOL matched = FALSE;
        if (restic_extract_snapshot_id(start, short_id, sizeof(short_id))) {
            /* A segment in front of the snapshot name is the repository alias: try it first, so a
               snapshot ID cached under several aliases resolves to the row it belongs to. */
            const WCHAR* prev_end = start;
            while (prev_end > buffer && prev_end[-1] == L'\\') --prev_end;
            const WCHAR* prev_start = prev_end;
            while (prev_start > buffer && prev_start[-1] != L'\\') --prev_start;
            const size_t alias_len = (size_t)(prev_end - prev_start);
            if (alias_len > 0 && alias_len < MAX_PATH) {
                WCHAR alias[MAX_PATH] = {0};
                wcsncpy_s(alias, _countof(alias), prev_start, alias_len);
                matched = snapshot_stats_lookup(alias, short_id, out_info);
            }
            if (!matched) {
                matched = snapshot_stats_lookup_any(short_id, out_info);
            }
        }
        *end = saved;
        if (matched) return TRUE;
        if (start == buffer) break;
        end = start - 1;
    }
    return FALSE;
}

static BOOL resolve_snapshot_w(const WCHAR* path, ResticSnapshotInfo* out_info) {
    if (path == NULL || path[0] == L'\0' || out_info == NULL) return FALSE;

    WfxParsedPath parsed;
    if (wfx_parse_path(path, &parsed)) {
        if (parsed.level == WFX_PATH_ROOT) return FALSE;
        if (parsed.level == WFX_PATH_SNAPSHOT_DIR) {
            /* Canonical path: trust its alias, so a snapshot ID can never be read from another
               repository even if the same ID is cached under a different alias */
            return snapshot_stats_lookup(parsed.repo_alias, parsed.snapshot_id, out_info);
        }
    }

    /* Not the canonical form: let the snapshot folder name identify the record */
    return lookup_loose(path, out_info);
}

/* ANSI entry point: the path travels through the caller's code page and is widened afterwards */
static BOOL resolve_snapshot(const char* file_name, ResticSnapshotInfo* out_info) {
    if (file_name == NULL || file_name[0] == '\0' || out_info == NULL) return FALSE;

    WCHAR wide[CONTENT_PATH_MAX] = {0};
    if (MultiByteToWideChar(CP_ACP, 0, file_name, -1, wide, (int)_countof(wide)) <= 0) {
        return FALSE;
    }
    return resolve_snapshot_w(wide, out_info);
}

/*
 * Trace-only rendering of a path. The log is UTF-8, so the ANSI spelling of the ANSI entry point
 * is converted back to Unicode first (otherwise non-ASCII names would be logged as mojibake).
 */
static void trace_ansi_path(const char* path, char* out, size_t capacity) {
    if (out == NULL || capacity == 0) return;
    out[0] = '\0';
    if (path == NULL) {
        strncpy_s(out, capacity, "<null>", _TRUNCATE);
        return;
    }
    if (wfx_trace_ansi_to_utf8(path, out, capacity) == 0) {
        strncpy_s(out, capacity, "<empty>", _TRUNCATE);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Exported content functions                                                                 */
/* ------------------------------------------------------------------------------------------ */

int fscontent_field_count(void) {
    return (int)CF_COUNT;
}

void fscontent_trace_reset(void) {
    wfx_trace_reset();
}

int __stdcall FsContentGetSupportedField(int FieldIndex, char* FieldName, char* Units, int maxlen) {
    if (FieldIndex == 0) {
        static LONG reported = 0;
        if (InterlockedIncrement(&reported) == 1) {
            wfx_trace_printf("fields enumerated (maxlen=%d)", maxlen);
        }
    }

    if (FieldIndex < 0 || FieldIndex >= (int)CF_COUNT) {
        return ft_nomorefields;
    }

    const ContentField* field = &kFields[FieldIndex];
    if (FieldName != NULL && maxlen > 0) {
        strncpy_s(FieldName, (size_t)maxlen, field->name, _TRUNCATE);
    }
    if (Units != NULL && maxlen > 0) {
        strncpy_s(Units, (size_t)maxlen, field->units, _TRUNCATE);
    }
    return field->type;
}

int __stdcall FsContentGetValue(
    char* FileName,
    int FieldIndex,
    int UnitIndex,
    void* FieldValue,
    int maxlen,
    int flags
) {
    /* No field needs a background thread: everything is served from the snapshot cache */
    (void)flags;

    if (FieldIndex < 0 || FieldIndex >= (int)CF_COUNT) {
        return ft_nosuchfield;
    }
    if (FieldValue == NULL) {
        return ft_fileerror;
    }

    ResticSnapshotInfo info;
    char shown[CONTENT_PATH_MAX * 3] = {0};
    if (wfx_trace_enabled()) trace_ansi_path(FileName, shown, sizeof(shown));
    if (!resolve_snapshot(FileName, &info)) {
        wfx_trace_printf(
            "value '%s' field=%d unit=%d -> %d (unresolved)",
            shown,
            FieldIndex,
            UnitIndex,
            ft_fieldempty
        );
        trace_cache_state();
        return ft_fieldempty;
    }

    const int result = render_field(FieldIndex, &info, UnitIndex, FieldValue, maxlen, FALSE);
    wfx_trace_printf(
        "value '%s' field=%d unit=%d -> %d", shown, FieldIndex, UnitIndex, result
    );
    return result;
}

/*
 * Unicode counterpart of FsContentGetValue. Total Commander calls the W functions when they are
 * exported and only falls back to the ANSI ones otherwise, so a Unicode plugin should provide
 * them (FsContentGetSupportedField, FsContentGetSupportedFieldFlags, FsContentGetDefaultSortOrder
 * and FsContentPluginUnloading exist in ANSI form only).
 */
int __stdcall FsContentGetValueW(
    WCHAR* FileName,
    int FieldIndex,
    int UnitIndex,
    void* FieldValue,
    int maxlen,
    int flags
) {
    (void)flags;

    if (FieldIndex < 0 || FieldIndex >= (int)CF_COUNT) {
        return ft_nosuchfield;
    }
    if (FieldValue == NULL) {
        return ft_fileerror;
    }

    ResticSnapshotInfo info;
    char shown[CONTENT_PATH_MAX * 3] = {0};
    if (wfx_trace_enabled()) {
        wfx_trace_wide_to_utf8((FileName != NULL) ? FileName : L"<null>", shown, sizeof(shown));
    }
    if (!resolve_snapshot_w(FileName, &info)) {
        wfx_trace_printf(
            "valueW '%s' field=%d unit=%d -> %d (unresolved)",
            shown,
            FieldIndex,
            UnitIndex,
            ft_fieldempty
        );
        trace_cache_state();
        return ft_fieldempty;
    }

    const int result = render_field(FieldIndex, &info, UnitIndex, FieldValue, maxlen, TRUE);
    wfx_trace_printf(
        "valueW '%s' field=%d unit=%d -> %d", shown, FieldIndex, UnitIndex, result
    );
    return result;
}

int __stdcall FsContentGetSupportedFieldFlags(int FieldIndex) {
    /* Read-only plugin: no field can be edited and no field substitutes a file property */
    (void)FieldIndex;
    return 0;
}

int __stdcall FsContentGetDefaultSortOrder(int FieldIndex) {
    switch (FieldIndex) {
        case CF_TIME:
        case CF_BACKUP_START:
        case CF_BACKUP_END:
        case CF_FILES:
        case CF_SIZE:
        case CF_DATA_ADDED:
        case CF_DATA_ADDED_PACKED:
        case CF_DURATION:
        case CF_AVG_FILE_SIZE: return -1; /* Newest/largest first */
        default: return 1;
    }
}

BOOL __stdcall FsContentGetDefaultView(
    char* ViewContents,
    char* ViewHeaders,
    char* ViewWidths,
    char* ViewOptions,
    int maxlen
) {
    if (maxlen <= 0) return FALSE;

    if (ViewContents != NULL) strncpy_s(ViewContents, (size_t)maxlen, kDefaultViewContents, _TRUNCATE);
    if (ViewHeaders != NULL) strncpy_s(ViewHeaders, (size_t)maxlen, kDefaultViewHeaders, _TRUNCATE);
    if (ViewWidths != NULL) strncpy_s(ViewWidths, (size_t)maxlen, kDefaultViewWidths, _TRUNCATE);
    if (ViewOptions != NULL) strncpy_s(ViewOptions, (size_t)maxlen, kDefaultViewOptions, _TRUNCATE);

    wfx_trace_printf("default view requested (maxlen=%d): '%s'", maxlen, kDefaultViewContents);
    return TRUE;
}

void __stdcall FsContentStopGetValue(char* FileName) {
    /* Nothing to abort: every value is computed synchronously from cached data */
    (void)FileName;
}

/* Copies an ASCII definition into the wide buffer (maxlen is in bytes, as documented) */
static BOOL copy_view_text(const char* source, WCHAR* dest, int maxlen) {
    if (dest == NULL) return TRUE;
    const int capacity = (maxlen > 0) ? maxlen / (int)sizeof(WCHAR) : 0;
    if (capacity < 2) return FALSE;
    dest[0] = L'\0';
    if (MultiByteToWideChar(CP_UTF8, 0, source, -1, dest, capacity) <= 0) {
        dest[0] = L'\0';
        return FALSE;
    }
    return TRUE;
}

BOOL __stdcall FsContentGetDefaultViewW(
    WCHAR* ViewContents,
    WCHAR* ViewHeaders,
    WCHAR* ViewWidths,
    WCHAR* ViewOptions,
    int maxlen
) {
    if (maxlen <= 0 || maxlen < (int)sizeof(WCHAR) * 2) return FALSE;

    BOOL ok = copy_view_text(kDefaultViewContents, ViewContents, maxlen);
    ok      = copy_view_text(kDefaultViewHeaders, ViewHeaders, maxlen) && ok;
    ok      = copy_view_text(kDefaultViewWidths, ViewWidths, maxlen) && ok;
    ok      = copy_view_text(kDefaultViewOptions, ViewOptions, maxlen) && ok;

    wfx_trace_printf("default viewW requested (maxlen=%d)", maxlen);
    return ok;
}

void __stdcall FsContentStopGetValueW(WCHAR* FileName) {
    /* Nothing to abort: every value is computed synchronously from cached data */
    (void)FileName;
}

void __stdcall FsContentPluginUnloading(void) {
    snapshot_stats_clear();
}
