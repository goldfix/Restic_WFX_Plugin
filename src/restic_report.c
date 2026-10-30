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
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "restic_report.h"
#include "fsplugin.h"
#include "cJSON.h"

/* Bytes taken by one U+2588 / U+2591 block in UTF-8 */
#define BAR_BLOCK_UTF8 "\xE2\x96\x88"
#define BAR_EMPTY_UTF8 "\xE2\x96\x91"

/* Largest value Total Commander can display as a signed 64-bit number */
#define REPORT_COUNTER_MAX 9223372036854775807.0

/* Size of a buffer holding a formatted "YYYY-MM-DD HH:MM:SS" timestamp */
#define REPORT_TIME_BUFFER 64

/* Maximum length of a WFX path in UTF-16 units (Total Commander allows 1023) */
#define REPORT_MAX_PATH_UNITS 1024
/* Worst-case UTF-8 size of such a path, and of a repository alias */
#define REPORT_MAX_PATH_BYTES (REPORT_MAX_PATH_UNITS * 3 + 1)
#define REPORT_ALIAS_BYTES    (MAX_PATH * 3 + 1)

/* Lower bounds of the file size histogram: 0 B, >=1 B, >=10 KB, >=100 KB, >=1 MB, >=10 MB */
static const unsigned long long kHistogramLimits[RESTIC_REPORT_HIST_BUCKETS] = {
    0ULL, 1ULL, 10240ULL, 102400ULL, 1048576ULL, 10485760ULL
};
static const char* const kHistogramLabels[RESTIC_REPORT_HIST_BUCKETS] = {
    "0 B", "1 B - 10 KB", "10 KB - 100 KB", "100 KB - 1 MB", "1 MB - 10 MB", "> 10 MB"
};

/* ------------------------------------------------------------------------------------------ */
/* Small helpers                                                                               */
/* ------------------------------------------------------------------------------------------ */

/* Copies UTF-8 text, truncating on a character boundary so the result is always valid UTF-8 */
static void copy_utf8(char* dst, size_t capacity, const char* src) {
    if (dst == NULL || capacity == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    size_t length = strlen(src);
    if (length >= capacity) {
        length = capacity - 1;
        while (length > 0 && ((unsigned char)src[length] & 0xC0) == 0x80) --length;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

/* Converts UTF-16 to UTF-8. Returns FALSE (and an empty string) for empty text or a small buffer. */
static BOOL wide_to_utf8(const WCHAR* src, char* dst, size_t capacity) {
    if (dst == NULL || capacity == 0) return FALSE;
    dst[0] = '\0';
    if (src == NULL || src[0] == L'\0') return FALSE;
    int written = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)capacity, NULL, NULL);
    return written > 0;
}

static BOOL is_hex_id(const char* text) {
    if (text == NULL || text[0] == '\0' || strlen(text) > 64) return FALSE;
    for (const char* p = text; *p != '\0'; ++p) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Reads an unsigned 64-bit value of a JSON object, saturating instead of wrapping */
static unsigned long long json_u64(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0) return 0;
    if (item->valuedouble >= REPORT_COUNTER_MAX) return (unsigned long long)REPORT_COUNTER_MAX;
    return (unsigned long long)item->valuedouble;
}

static double json_double(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valuedouble : 0.0;
}

static const char* json_str(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* Compares two "YYYY-MM-DD HH:MM:SS" timestamps (lexicographic order equals chronological order) */
static void keep_oldest(char* slot, const char* candidate) {
    if (candidate[0] == '\0') return;
    if (slot[0] == '\0' || strcmp(candidate, slot) < 0) {
        copy_utf8(slot, RESTIC_REPORT_TIME_MAX, candidate);
    }
}

static void keep_newest(char* slot, const char* candidate) {
    if (candidate[0] == '\0') return;
    if (slot[0] == '\0' || strcmp(candidate, slot) > 0) {
        copy_utf8(slot, RESTIC_REPORT_TIME_MAX, candidate);
    }
}

/* Converts an ISO-8601 timestamp to the report format; empty string when unparsable */
static void iso_to_report_time(const char* iso, char* out, size_t capacity) {
    FILETIME ft;
    if (!restic_parse_iso8601_time(iso, &ft) ||
        !restic_report_format_time(&ft, out, capacity)) {
        copy_utf8(out, capacity, "");
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Formatting helpers                                                                          */
/* ------------------------------------------------------------------------------------------ */

void restic_report_format_bar(char* out, size_t out_capacity, double ratio, int width) {
    if (out == NULL || out_capacity == 0) return;
    out[0] = '\0';
    if (width <= 0) width = 1;
    if (width > 40) width = 40;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;

    int filled = (int)(ratio * (double)width + 0.5);
    if (ratio > 0.0 && filled == 0) filled = 1; /* Never show an empty bar for a non-zero value */
    if (filled > width) filled = width;

    size_t pos = 0;
    if (pos + 1 < out_capacity) out[pos++] = '[';
    for (int i = 0; i < width; ++i) {
        const char* block = (i < filled) ? BAR_BLOCK_UTF8 : BAR_EMPTY_UTF8;
        if (pos + 3 + 2 > out_capacity) break;
        memcpy(out + pos, block, 3);
        pos += 3;
    }
    if (pos + 2 <= out_capacity) out[pos++] = ']';
    out[pos] = '\0';
}

void restic_report_format_bytes(unsigned long long bytes, char* out, size_t out_capacity) {
    static const char* const kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    if (out == NULL || out_capacity == 0) return;
    double value  = (double)bytes;
    int unit      = 0;
    while (value >= 1024.0 && unit < (int)(_countof(kUnits) - 1)) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        (void)snprintf(out, out_capacity, "%llu B", bytes);
    } else {
        (void)snprintf(out, out_capacity, "%.2f %s", value, kUnits[unit]);
    }
}

void restic_report_format_uint(unsigned long long value, char* out, size_t out_capacity) {
    if (out == NULL || out_capacity == 0) return;
    char digits[32];
    (void)snprintf(digits, sizeof(digits), "%llu", value);

    size_t length = strlen(digits);
    size_t pos    = 0;
    for (size_t i = 0; i < length; ++i) {
        if (i > 0 && ((length - i) % 3) == 0) {
            if (pos + 1 >= out_capacity) break;
            out[pos++] = ',';
        }
        if (pos + 1 >= out_capacity) break;
        out[pos++] = digits[i];
    }
    out[pos] = '\0';
}

BOOL restic_report_format_time(const FILETIME* file_time, char* out, size_t out_capacity) {
    if (out == NULL || out_capacity == 0) return FALSE;
    out[0] = '\0';
    if (file_time == NULL || file_time->dwHighDateTime == 0xFFFFFFFF) return FALSE;

    SYSTEMTIME utc;
    if (!FileTimeToSystemTime(file_time, &utc)) return FALSE;
    (void)snprintf(out, out_capacity, "%04u-%02u-%02u %02u:%02u:%02u", utc.wYear, utc.wMonth,
                   utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
    return TRUE;
}

void restic_report_escape_write(FILE* fp, const char* text) {
    if (fp == NULL) return;
    if (text == NULL) text = "";
    for (const char* p = text; *p != '\0'; ++p) {
        switch (*p) {
            case '&': fputs("&amp;", fp); break;
            case '<': fputs("&lt;", fp); break;
            case '>': fputs("&gt;", fp); break;
            case '"': fputs("&quot;", fp); break;
            default: fputc((unsigned char)*p, fp); break;
        }
    }
}

void restic_report_redact_location(const char* location, char* out, size_t capacity) {
    if (out == NULL || capacity == 0) return;
    out[0] = '\0';
    if (location == NULL) return;

    /* scheme://user:password@host/...: hide what lies between "://" and the last '@' of the authority */
    const char* authority = strstr(location, "://");
    if (authority != NULL) {
        authority += 3;
        const char* at = NULL;
        for (const char* p = authority; *p != '\0' && *p != '/'; ++p) {
            if (*p == '@') at = p;
        }
        if (at != NULL) {
            (void)snprintf(out, capacity, "%.*s***%s", (int)(authority - location), location, at);
            return;
        }
    }
    copy_utf8(out, capacity, location);
}

/* ------------------------------------------------------------------------------------------ */
/* Parsers                                                                                     */
/* ------------------------------------------------------------------------------------------ */

BOOL restic_report_parse_storage_json(const char* json_text, ResticReportStorage* out_stats) {
    if (json_text == NULL || out_stats == NULL) return FALSE;
    memset(out_stats, 0, sizeof(*out_stats));

    cJSON* root = cJSON_Parse(json_text);
    if (root == NULL) return FALSE;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return FALSE;
    }

    out_stats->total_size        = json_u64(root, "total_size");
    out_stats->total_file_count  = json_u64(root, "total_file_count");
    out_stats->uncompressed_size = json_u64(root, "total_uncompressed_size");
    out_stats->blob_count        = json_u64(root, "total_blob_count");
    out_stats->compression_ratio = json_double(root, "compression_ratio");
    out_stats->space_saving_percent = json_double(root, "compression_space_saving");

    /* Repository format v1 (not compressed) prints no compression fields */
    if (out_stats->uncompressed_size == 0) {
        out_stats->uncompressed_size = out_stats->total_size;
    }
    if (out_stats->compression_ratio <= 0.0) {
        out_stats->compression_ratio = 1.0;
    }
    out_stats->valid = TRUE;
    cJSON_Delete(root);
    return TRUE;
}

/* Reads an integer field of the nested "added"/"removed" objects */
static unsigned long long json_nested_u64(const cJSON* object, const char* group, const char* key) {
    const cJSON* nested = cJSON_GetObjectItemCaseSensitive(object, group);
    return cJSON_IsObject(nested) ? json_u64(nested, key) : 0;
}

void restic_report_parse_diff_line(ResticReportDiff* diff, const char* line, size_t length) {
    if (diff == NULL || line == NULL || length == 0) return;

    cJSON* root = cJSON_ParseWithLength(line, length);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }

    /* Restic 0.17+ prints "message_type"; accept "struct_type" as well for other versions */
    const char* kind = json_str(root, "message_type");
    if (kind == NULL) kind = json_str(root, "struct_type");
    if (kind == NULL) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(kind, "change") == 0) {
        const char* path = json_str(root, "path");
        const char* modifier = json_str(root, "modifier");
        if (path != NULL) {
            diff->change_events++;
            if (diff->change_count < RESTIC_REPORT_MAX_DIFF_CHANGES) {
                ResticReportDiffChange* change = &diff->changes[diff->change_count++];
                copy_utf8(change->modifier, sizeof(change->modifier), modifier ? modifier : "?");
                copy_utf8(change->path, sizeof(change->path), path);
            }
        }
    } else if (strcmp(kind, "statistics") == 0 || strcmp(kind, "stats") == 0) {
        diff->valid         = TRUE;
        diff->changed_files = json_u64(root, "changed_files");
        diff->added_files   = json_nested_u64(root, "added", "files");
        diff->added_dirs    = json_nested_u64(root, "added", "dirs");
        diff->added_bytes   = json_nested_u64(root, "added", "bytes");
        diff->added_blobs   = json_nested_u64(root, "added", "data_blobs");
        diff->removed_files = json_nested_u64(root, "removed", "files");
        diff->removed_dirs  = json_nested_u64(root, "removed", "dirs");
        diff->removed_bytes = json_nested_u64(root, "removed", "bytes");
        diff->removed_blobs = json_nested_u64(root, "removed", "data_blobs");
    }

    cJSON_Delete(root);
}

/*
 * Returns the extension of a file name in lower case. A name without a dot, ending with a dot, or
 * whose only dot is the first character (".gitignore") has none: "(no ext)".
 */
static void extension_of(const char* name, char* out, size_t capacity) {
    const char* dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0') {
        copy_utf8(out, capacity, "(no ext)");
        return;
    }
    copy_utf8(out, capacity, dot);
    for (char* p = out; *p != '\0'; ++p) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
}

static int histogram_bucket(unsigned long long size) {
    for (int i = RESTIC_REPORT_HIST_BUCKETS - 1; i >= 1; --i) {
        if (size >= kHistogramLimits[i]) return i;
    }
    return 0;
}

/* Finds (or creates) the bucket of the first path segment of `relative`; NULL when the table is full */
static ResticReportSubdir* find_subdir(ResticReportFolder* folder, const char* relative,
                                       size_t name_length) {
    for (size_t i = 0; i < folder->subdir_count; ++i) {
        ResticReportSubdir* bucket = &folder->subdirs[i];
        if (strncmp(bucket->name, relative, name_length) == 0 && bucket->name[name_length] == '\0') {
            return bucket;
        }
    }
    if (name_length + 1 > RESTIC_REPORT_NAME_MAX || folder->subdir_count >= RESTIC_REPORT_MAX_SUBDIRS) {
        return NULL;
    }

    ResticReportSubdir* bucket = &folder->subdirs[folder->subdir_count++];
    memset(bucket, 0, sizeof(*bucket));
    memcpy(bucket->name, relative, name_length);
    return bucket;
}

static void accumulate_extension(ResticReportFolder* folder, const char* ext,
                                 unsigned long long size) {
    for (size_t i = 0; i < folder->ext_count; ++i) {
        ResticReportExtension* bucket = &folder->extensions[i];
        if (strcmp(bucket->ext, ext) == 0) {
            bucket->total_size += size;
            bucket->file_count++;
            return;
        }
    }

    if (folder->ext_count >= RESTIC_REPORT_MAX_EXTENSIONS) return; /* Counted in "(other)" */

    ResticReportExtension* bucket = &folder->extensions[folder->ext_count++];
    memset(bucket, 0, sizeof(*bucket));
    copy_utf8(bucket->ext, sizeof(bucket->ext), ext);
    bucket->total_size = size;
    bucket->file_count = 1;
}

/* Keeps the `largest` array sorted by size (descending) with a bounded insertion */
static void accumulate_largest(ResticReportFolder* folder, const char* relative,
                               unsigned long long size, const char* mtime) {
    if (folder->largest_count == RESTIC_REPORT_TOP_FILES &&
        size <= folder->largest[RESTIC_REPORT_TOP_FILES - 1].size) {
        return;
    }

    size_t position = folder->largest_count;
    if (position == RESTIC_REPORT_TOP_FILES) position = RESTIC_REPORT_TOP_FILES - 1;

    while (position > 0 && folder->largest[position - 1].size < size) {
        folder->largest[position] = folder->largest[position - 1];
        --position;
    }

    ResticReportLargestFile* entry = &folder->largest[position];
    memset(entry, 0, sizeof(*entry));
    copy_utf8(entry->rel_path, sizeof(entry->rel_path), relative);
    copy_utf8(entry->mtime, sizeof(entry->mtime), mtime);
    entry->size = size;

    if (folder->largest_count < RESTIC_REPORT_TOP_FILES) folder->largest_count++;
}

void restic_report_aggregate_ls_line(ResticReportFolder* folder, const char* filter_path,
                                     const char* line, size_t length) {
    if (folder == NULL || filter_path == NULL || line == NULL || length == 0) return;

    cJSON* root = cJSON_ParseWithLength(line, length);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }

    /* Restic prints "message_type":"node" (older versions "struct_type":"node"); a line that
       only carries the node fields is accepted as well, so hand-written fixtures work too. */
    const char* kind = json_str(root, "message_type");
    if (kind == NULL) kind = json_str(root, "struct_type");
    const char* path = json_str(root, "path");
    if (path == NULL || (kind != NULL && strcmp(kind, "node") != 0)) {
        cJSON_Delete(root);
        return;
    }

    const size_t filter_length = strlen(filter_path);
    const BOOL at_root         = (filter_length == 1 && filter_path[0] == '/');

    /* Restic repeats the filtered directory itself: never count it as one of its own children */
    if (!at_root) {
        if (strcmp(path, filter_path) == 0) {
            folder->root_seen = TRUE;
            cJSON_Delete(root);
            return;
        }
        if (strncmp(path, filter_path, filter_length) != 0 ||
            path[filter_length] != '/') {
            cJSON_Delete(root);
            return;
        }
    } else if (path[0] != '/' || path[1] == '\0') {
        cJSON_Delete(root);
        return;
    }

    const char* relative = at_root ? path + 1 : path + filter_length + 1;
    if (relative[0] == '\0') {
        cJSON_Delete(root);
        return;
    }

    const char* type          = json_str(root, "type");
    const char* mtime_iso     = json_str(root, "mtime");
    const BOOL is_dir         = (type != NULL && strcmp(type, "dir") == 0);
    const char* slash         = strchr(relative, '/');
    const size_t first_length = slash ? (size_t)(slash - relative) : strlen(relative);

    if (is_dir) {
        folder->total_dirs++;
        if (slash == NULL) {
            folder->direct_dirs++;
            (void)find_subdir(folder, relative, first_length); /* Empty subfolders are listed too */
        }
        cJSON_Delete(root);
        return;
    }

    const unsigned long long size = json_u64(root, "size");
    folder->total_files++;
    folder->total_size += size;
    if (slash == NULL) {
        folder->direct_files++;
        folder->direct_size += size;
    } else {
        ResticReportSubdir* bucket = find_subdir(folder, relative, first_length);
        if (bucket != NULL) { /* A full table leaves the file to the "(other subfolders)" row */
            bucket->total_size += size;
            bucket->file_count++;
        }
    }

    char mtime[RESTIC_REPORT_TIME_MAX] = {0};
    if (mtime_iso != NULL) iso_to_report_time(mtime_iso, mtime, sizeof(mtime));
    keep_oldest(folder->oldest_mtime, mtime);
    keep_newest(folder->newest_mtime, mtime);

    const char* base = strrchr(relative, '/');
    base             = (base != NULL) ? base + 1 : relative;
    char ext[RESTIC_REPORT_EXT_MAX];
    extension_of(base, ext, sizeof(ext));
    accumulate_extension(folder, ext, size);

    const int bucket_index = histogram_bucket(size);
    folder->hist_files[bucket_index]++;
    folder->hist_bytes[bucket_index] += size;

    /* The ranking shows Windows-style separators, like Total Commander does */
    char display[RESTIC_REPORT_PATH_MAX];
    copy_utf8(display, sizeof(display), relative);
    for (char* p = display; *p != '\0'; ++p) {
        if (*p == '/') *p = '\\';
    }
    accumulate_largest(folder, display, size, mtime);

    cJSON_Delete(root);
}

static int compare_subdirs(const void* left, const void* right) {
    const ResticReportSubdir* a = (const ResticReportSubdir*)left;
    const ResticReportSubdir* b = (const ResticReportSubdir*)right;
    if (a->total_size < b->total_size) return 1;
    if (a->total_size > b->total_size) return -1;
    return strcmp(a->name, b->name);
}

static int compare_extensions(const void* left, const void* right) {
    const ResticReportExtension* a = (const ResticReportExtension*)left;
    const ResticReportExtension* b = (const ResticReportExtension*)right;
    if (a->total_size < b->total_size) return 1;
    if (a->total_size > b->total_size) return -1;
    return strcmp(a->ext, b->ext);
}

void restic_report_finalize_folder(ResticReportFolder* folder) {
    if (folder == NULL) return;
    if (folder->subdir_count > 1) {
        qsort(folder->subdirs, folder->subdir_count, sizeof(folder->subdirs[0]), compare_subdirs);
    }
    if (folder->ext_count > 1) {
        qsort(folder->extensions, folder->ext_count, sizeof(folder->extensions[0]),
              compare_extensions);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* HTML helpers                                                                                */
/* ------------------------------------------------------------------------------------------ */

#define REPORT_LABEL_COLUMN 34

static const char kHtmlHeadOpen[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";

static const char kHtmlHeadStyle[] =
    "<style>\n"
    ":root{--bg:#0f172a;--card:#1e293b;--text:#f8fafc;--muted:#94a3b8;--accent:#38bdf8;\n"
    "--green:#4ade80;--amber:#fbbf24;--red:#f87171;--border:#334155;}\n"
    "@media (prefers-color-scheme:light){:root{--bg:#f8fafc;--card:#fff;--text:#0f172a;\n"
    "--muted:#475569;--accent:#0284c7;--green:#16a34a;--amber:#d97706;--red:#dc2626;\n"
    "--border:#cbd5e1;}}\n"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;\n"
    "background:var(--bg);color:var(--text);margin:20px;line-height:1.45;}\n"
    "h1,h2{color:var(--accent);margin:18px 0 8px;}\n"
    "h1{font-size:1.5em;}h2{font-size:1.15em;border-bottom:1px solid var(--border);}\n"
    "pre{font-family:Consolas,'Cascadia Mono','Courier New',monospace;font-size:13px;\n"
    "background:var(--card);border:1px solid var(--border);border-radius:8px;\n"
    "padding:14px;overflow-x:auto;}\n"
    ".muted{color:var(--muted);}\n"
    ".add{color:var(--green);}.mod{color:var(--amber);}.del{color:var(--red);}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n";

static const char kHtmlFoot[] = "</body>\n</html>\n";

/* Writes "  <label><padding><value>" inside a <pre> block */
static void write_labeled(FILE* fp, const char* label, const char* value) {
    size_t label_length = (label != NULL) ? strlen(label) : 0;
    fputs("  ", fp);
    restic_report_escape_write(fp, label);
    size_t used   = 2 + label_length;
    size_t spaces = (used < REPORT_LABEL_COLUMN) ? (REPORT_LABEL_COLUMN - used) : 1;
    for (size_t i = 0; i < spaces; ++i) fputc(' ', fp);
    restic_report_escape_write(fp, value);
    fputc('\n', fp);
}

/* Writes the document head (with <title>) and the page headline */
static void write_html_begin(FILE* fp, const char* title, const char* subtitle) {
    fputs(kHtmlHeadOpen, fp);
    fputs("<title>", fp);
    restic_report_escape_write(fp, title);
    fputs("</title>\n", fp);
    fputs(kHtmlHeadStyle, fp);
    fputs("<h1>", fp);
    restic_report_escape_write(fp, title);
    fputs("</h1>\n", fp);
    if (subtitle != NULL && subtitle[0] != '\0') {
        fputs("<p class=\"muted\">", fp);
        restic_report_escape_write(fp, subtitle);
        fputs("</p>\n", fp);
    }
}

/* Writes the repository location with any credentials embedded in a URL masked */
static void write_location(FILE* fp, const char* target) {
    if (target == NULL || target[0] == '\0') return;
    char safe[REPORT_MAX_PATH_BYTES];
    restic_report_redact_location(target, safe, sizeof(safe));
    write_labeled(fp, "Location", safe);
}

static void write_html_end(FILE* fp) {
    fputs(kHtmlFoot, fp);
}

static double percentage_of(unsigned long long part, unsigned long long total) {
    if (total == 0) return 0.0;
    return ((double)part * 100.0) / (double)total;
}

/* Storage table shared by the repository and the snapshot report */
static void write_storage_block(FILE* fp, const ResticReportStorage* restore,
                                const ResticReportStorage* stored) {
    char text[64];
    fputs("<h2>Storage and deduplication</h2>\n<pre>", fp);

    restic_report_format_bytes(restore->total_size, text, sizeof(text));
    write_labeled(fp, "Logical size (full restore)", text);
    if (restore->total_file_count > 0) {
        restic_report_format_uint(restore->total_file_count, text, sizeof(text));
        write_labeled(fp, "Files in the restore", text);
    }

    if (stored->valid) {
        restic_report_format_bytes(stored->uncompressed_size, text, sizeof(text));
        write_labeled(fp, "Uncompressed (deduplicated)", text);
        restic_report_format_bytes(stored->total_size, text, sizeof(text));
        write_labeled(fp, "Stored on disk (compressed)", text);

        (void)snprintf(text, sizeof(text), "%.2f x", stored->compression_ratio);
        write_labeled(fp, "Compression ratio", text);
        (void)snprintf(text, sizeof(text), "%.1f %%", stored->space_saving_percent);
        write_labeled(fp, "Saved by compression", text);

        if (stored->blob_count > 0) {
            restic_report_format_uint(stored->blob_count, text, sizeof(text));
            write_labeled(fp, "Stored blobs", text);
        }
    }

    if (stored->valid && stored->total_size > 0 && restore->total_size > 0) {
        (void)snprintf(text, sizeof(text), "%.2f x",
                       (double)restore->total_size / (double)stored->total_size);
        write_labeled(fp, "Deduplication factor", text);
        if (restore->total_size > stored->total_size) {
            (void)snprintf(text, sizeof(text), "%.1f %%",
                           (1.0 - (double)stored->total_size / (double)restore->total_size) * 100.0);
            write_labeled(fp, "Total space saved", text);
        }
    }
    fputs("</pre>\n", fp);
}

static void snapshot_time_text(const ResticSnapshotInfo* info, char* out, size_t capacity) {
    if (!restic_report_format_time(&info->time, out, capacity)) {
        copy_utf8(out, capacity, "(unknown)");
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Repository report                                                                           */
/* ------------------------------------------------------------------------------------------ */

/* Writes one row of the snapshot history table (also used for its header and separator) */
static void write_history_row(FILE* fp, const char* id, const char* when, const char* host,
                              const char* files, const char* size, const char* added,
                              const char* bar) {
    fprintf(fp, "  %-8s  %-19s  ", id, when);
    restic_report_escape_write(fp, host);
    for (size_t used = strlen(host); used < 18; ++used) fputc(' ', fp);
    fprintf(fp, "  %7s  %10s  %10s   ", files, size, added);
    restic_report_escape_write(fp, bar);
    fputc('\n', fp);
}

void restic_report_write_repo_html(FILE* fp, const ResticReportRepo* report) {
    if (fp == NULL || report == NULL) return;

    char text[64];
    write_html_begin(fp, "Restic repository report", report->alias);

    fputs("<pre>", fp);
    write_labeled(fp, "Repository", report->alias);
    write_location(fp, report->repo_target);
    restic_report_format_uint((unsigned long long)report->snapshot_count, text, sizeof(text));
    write_labeled(fp, "Snapshots", text);
    if (report->snapshot_count > 0) {
        snapshot_time_text(&report->snapshots[0], text, sizeof(text));
        write_labeled(fp, "First snapshot (UTC)", text);
        snapshot_time_text(&report->snapshots[report->snapshot_count - 1], text, sizeof(text));
        write_labeled(fp, "Latest snapshot (UTC)", text);
    }
    fputs("</pre>\n", fp);

    write_storage_block(fp, &report->restore, &report->stored);

    /* Snapshot history, newest first (Restic prints the list oldest first) */
    fputs("<h2>Snapshot history</h2>\n<pre>", fp);
    write_history_row(fp, "Short ID", "Date / time (UTC)", "Host", "Files", "Size", "Data added",
                      "Growth");
    write_history_row(fp, "--------", "-------------------", "------------------", "-------",
                      "----------", "----------", "----------------------");

    unsigned long long max_added = 0;
    for (size_t i = 0; i < report->snapshot_count; ++i) {
        if (report->snapshots[i].data_added_packed > max_added) {
            max_added = report->snapshots[i].data_added_packed;
        }
    }

    for (size_t index = report->snapshot_count; index > 0; --index) {
        const ResticSnapshotInfo* info = &report->snapshots[index - 1];
        char when[REPORT_TIME_BUFFER];
        char files[32] = "-";
        char size[32]  = "-";
        char added[32] = "-";
        char bar[256];
        snapshot_time_text(info, when, sizeof(when));
        double ratio = 0.0;
        if (info->has_summary) { /* Snapshots written without a summary have no figures */
            restic_report_format_uint(info->total_files_processed, files, sizeof(files));
            restic_report_format_bytes(info->total_bytes_processed, size, sizeof(size));
            restic_report_format_bytes(info->data_added_packed, added, sizeof(added));
            ratio = (max_added == 0) ? 0.0 : (double)info->data_added_packed / (double)max_added;
        }
        restic_report_format_bar(bar, sizeof(bar), ratio, 20);
        write_history_row(fp, info->short_id, when, info->hostname, files, size, added, bar);
    }
    fputs("</pre>\n", fp);
    write_html_end(fp);
}

/* ------------------------------------------------------------------------------------------ */
/* Snapshot report                                                                             */
/* ------------------------------------------------------------------------------------------ */

/* Formats a duration in seconds as "12 s" or "4 min 32 s" */
static void format_duration(unsigned long long seconds, char* out, size_t capacity) {
    if (seconds == 0) {
        copy_utf8(out, capacity, "< 1 s");
    } else if (seconds < 60) {
        (void)snprintf(out, capacity, "%llu s", seconds);
    } else if (seconds < 3600) {
        (void)snprintf(out, capacity, "%llu min %llu s", seconds / 60, seconds % 60);
    } else {
        (void)snprintf(out, capacity, "%llu h %llu min", seconds / 3600, (seconds % 3600) / 60);
    }
}

/* Writes the three "new / changed / unmodified" rows of one category with bars */
static void write_category_rows(FILE* fp, const char* prefix, const char* new_label,
                                const char* changed_label, const char* unmodified_label,
                                unsigned long long new_count, unsigned long long changed_count,
                                unsigned long long unmodified_count, unsigned long long total) {
    char label[96];
    char text[64];
    char bar[256];

    struct {
        const char* name;
        unsigned long long value;
    } rows[3] = {{new_label, new_count}, {changed_label, changed_count},
                 {unmodified_label, unmodified_count}};

    for (size_t i = 0; i < _countof(rows); ++i) {
        (void)snprintf(label, sizeof(label), "%s %s", prefix, rows[i].name);
        restic_report_format_uint(rows[i].value, text, sizeof(text));
        double ratio = percentage_of(rows[i].value, total) / 100.0;
        restic_report_format_bar(bar, sizeof(bar), ratio, 20);

        /* The bar follows the value on the same line, so both Lister modes stay aligned */
        char value[384];
        (void)snprintf(value, sizeof(value), "%s  %s", text, bar);
        write_labeled(fp, label, value);
    }
}

static void write_diff_block(FILE* fp, const ResticReportSnapshot* report) {
    const ResticReportDiff* diff = report->diff;
    char text[128]; /* Three 20-digit counters plus separators */

    fputs("<h2>Changes since the parent snapshot</h2>\n<pre>", fp);
    if (diff == NULL || !diff->valid) {
        const char* note = (report->diff_note != NULL && report->diff_note[0] != '\0')
                               ? report->diff_note
                               : "No diff is available for this snapshot.";
        restic_report_escape_write(fp, note);
        fputs("</pre>\n", fp);
        return;
    }

    if (diff->parent_short_id[0] != '\0') {
        write_labeled(fp, "Parent snapshot", diff->parent_short_id);
    }
    restic_report_format_uint(diff->changed_files, text, sizeof(text));
    write_labeled(fp, "Modified files", text);
    restic_report_format_bytes(diff->added_bytes, text, sizeof(text));
    write_labeled(fp, "Added", text);
    restic_report_format_bytes(diff->removed_bytes, text, sizeof(text));
    write_labeled(fp, "Removed", text);

    (void)snprintf(text, sizeof(text), "%llu files, %llu dirs, %llu blobs", diff->added_files,
                   diff->added_dirs, diff->added_blobs);
    write_labeled(fp, "  new items", text);
    (void)snprintf(text, sizeof(text), "%llu files, %llu dirs, %llu blobs", diff->removed_files,
                   diff->removed_dirs, diff->removed_blobs);
    write_labeled(fp, "  deleted items", text);

    if (diff->change_count > 0) {
        fputs("\n", fp);
        for (size_t i = 0; i < diff->change_count; ++i) {
            const ResticReportDiffChange* change = &diff->changes[i];
            const char* css_class              = "mod";
            const char* marker                 = "[M]";
            if (strcmp(change->modifier, "+") == 0) {
                css_class = "add";
                marker    = "[+]";
            } else if (strcmp(change->modifier, "-") == 0) {
                css_class = "del";
                marker    = "[-]";
            }
            fputs("<span class=\"", fp);
            fputs(css_class, fp);
            fputs("\">", fp);
            fputs(marker, fp);
            fputs("</span> ", fp);
            restic_report_escape_write(fp, change->path);
            fputc('\n', fp);
        }
        if (diff->change_events > diff->change_count) {
            (void)snprintf(text, sizeof(text), "... %llu more changed paths not shown",
                           diff->change_events - diff->change_count);
            fputs(text, fp);
            fputc('\n', fp);
        }
    }
    fputs("</pre>\n", fp);
}

void restic_report_write_snapshot_html(FILE* fp, const ResticReportSnapshot* report) {
    if (fp == NULL || report == NULL || report->info == NULL) return;
    const ResticSnapshotInfo* info = report->info;
    char text[64];
    char when[REPORT_TIME_BUFFER];

    write_html_begin(fp, "Restic snapshot report", report->alias);

    fputs("<pre>", fp);
    write_labeled(fp, "Snapshot ID", info->id);
    write_labeled(fp, "Short ID", info->short_id);
    write_labeled(fp, "Parent", info->parent[0] != '\0' ? info->parent : "(none: initial backup)");
    write_labeled(fp, "Host", info->hostname);
    write_labeled(fp, "User", info->username);
    write_labeled(fp, "Restic version", info->program_version);
    write_labeled(fp, "Tags", info->tags[0] != '\0' ? info->tags : "(none)");
    write_labeled(fp, "Source paths", info->paths);
    snapshot_time_text(info, when, sizeof(when));
    write_labeled(fp, "Created (UTC)", when);

    char start[REPORT_TIME_BUFFER];
    char end[REPORT_TIME_BUFFER];
    if (info->has_summary && restic_report_format_time(&info->backup_start, start, sizeof(start)) &&
        restic_report_format_time(&info->backup_end, end, sizeof(end))) {
        (void)snprintf(text, sizeof(text), "%s .. %s", start, end);
        write_labeled(fp, "Backup window (UTC)", text);
        format_duration(info->duration_seconds, text, sizeof(text));
        write_labeled(fp, "Duration", text);
    }
    fputs("</pre>\n", fp);

    if (!info->has_summary) {
        fputs("<p class=\"muted\">This snapshot was stored without a summary block.\n"
              "The file and directory breakdown is not available.</p>\n",
              fp);
    }

    write_storage_block(fp, &report->restore, &report->stored);

    if (info->has_summary) {
        unsigned long long files_total = info->total_files_processed;
        unsigned long long dirs_total  = info->dirs_new + info->dirs_changed + info->dirs_unmodified;

        fputs("<h2>Files and directories</h2>\n<pre>", fp);
        write_category_rows(fp, "Files", "new      ", "changed  ", "unchanged", info->files_new,
                            info->files_changed, info->files_unmodified, files_total);
        write_category_rows(fp, "Dirs", "new      ", "changed  ", "unchanged", info->dirs_new,
                            info->dirs_changed, info->dirs_unmodified, dirs_total);

        restic_report_format_uint(files_total, text, sizeof(text));
        write_labeled(fp, "Files processed", text);
        restic_report_format_uint(dirs_total, text, sizeof(text));
        write_labeled(fp, "Directories processed", text);
        if (files_total > 0) {
            restic_report_format_bytes(info->total_bytes_processed / files_total, text, sizeof(text));
            write_labeled(fp, "Average file size", text);
        }
        restic_report_format_bytes(info->total_bytes_processed, text, sizeof(text));
        write_labeled(fp, "Bytes processed", text);
        fputs("</pre>\n", fp);
    }

    write_diff_block(fp, report);
    write_html_end(fp);
}

/* ------------------------------------------------------------------------------------------ */
/* Snapshot subfolder report                                                                   */
/* ------------------------------------------------------------------------------------------ */

/* Difference that never wraps below zero */
static unsigned long long minus(unsigned long long left, unsigned long long right) {
    return (left > right) ? (left - right) : 0;
}

/* Writes the header of the subfolder / extension tables (same widths as write_share_row) */
static void write_share_header(FILE* fp, const char* name_column) {
    char line[160];
    (void)snprintf(line, sizeof(line), "  %-10s  %6s  %6s  %-22s  %s\n", "Size", "Share", "Files",
                   "Bar", name_column);
    fputs(line, fp);
    (void)snprintf(line, sizeof(line), "  %-10s  %6s  %6s  %-22s  %s\n", "----------", "------",
                   "------", "----------------------", "----");
    fputs(line, fp);
}

/* Writes one table row: size, share of `total`, file count, bar and name */
static void write_share_row(FILE* fp, unsigned long long size, unsigned long long total,
                            unsigned long long files, const char* name) {
    char size_text[64];
    char percent_text[32];
    char count_text[32];
    char bar[256];
    const double share = percentage_of(size, total);

    restic_report_format_bytes(size, size_text, sizeof(size_text));
    (void)snprintf(percent_text, sizeof(percent_text), "%.1f%%", share);
    (void)snprintf(count_text, sizeof(count_text), "%llu", files);
    restic_report_format_bar(bar, sizeof(bar), share / 100.0, 20);

    fprintf(fp, "  %-10s  %6s  %6s  ", size_text, percent_text, count_text);
    restic_report_escape_write(fp, bar);
    fputs("  ", fp);
    restic_report_escape_write(fp, name);
    fputc('\n', fp);
}

void restic_report_write_subtree_html(FILE* fp, const ResticReportSubtree* report) {
    if (fp == NULL || report == NULL || report->folder == NULL) return;
    const ResticReportFolder* folder = report->folder;
    char size_text[64];
    char text[128];

    write_html_begin(fp, "Restic folder report", report->alias);

    fputs("<pre>", fp);
    write_labeled(fp, "Folder", report->display_path != NULL ? report->display_path : "/");
    if (report->snapshot_short_id != NULL && report->snapshot_short_id[0] != '\0') {
        write_labeled(fp, "Snapshot", report->snapshot_short_id);
    }
    if (report->snapshot_time != NULL && report->snapshot_time[0] != '\0') {
        write_labeled(fp, "Snapshot date (UTC)", report->snapshot_time);
    }
    if (report->hostname != NULL && report->hostname[0] != '\0') {
        write_labeled(fp, "Host", report->hostname);
    }
    write_location(fp, report->repo_target);

    restic_report_format_bytes(folder->total_size, size_text, sizeof(size_text));
    write_labeled(fp, "Total size", size_text);
    (void)snprintf(text, sizeof(text), "%llu (%llu in this folder)", folder->total_files,
                   folder->direct_files);
    write_labeled(fp, "Files", text);
    (void)snprintf(text, sizeof(text), "%llu (%llu in this folder)", folder->total_dirs,
                   folder->direct_dirs);
    write_labeled(fp, "Directories", text);
    if (folder->total_files > 0) {
        restic_report_format_bytes(folder->total_size / folder->total_files, size_text,
                                   sizeof(size_text));
        write_labeled(fp, "Average file size", size_text);
    }
    if (folder->oldest_mtime[0] != '\0' && folder->newest_mtime[0] != '\0') {
        (void)snprintf(text, sizeof(text), "%s .. %s", folder->oldest_mtime, folder->newest_mtime);
        write_labeled(fp, "Modification span (UTC)", text);
    }
    fputs("</pre>\n", fp);

    /* Size by direct subfolder: the files of the folder itself, the biggest subfolders and one
       remainder row, so the shares always add up to 100 % */
    fputs("<h2>Size by subfolder</h2>\n<pre>", fp);
    write_share_header(fp, "Name");
    if (folder->direct_files > 0) {
        write_share_row(fp, folder->direct_size, folder->total_size, folder->direct_files,
                        "(files in this folder)");
    }
    unsigned long long shown_size  = 0;
    unsigned long long shown_files = 0;
    for (size_t i = 0; i < folder->subdir_count && i < RESTIC_REPORT_TOP_SUBDIRS; ++i) {
        const ResticReportSubdir* bucket = &folder->subdirs[i];
        write_share_row(fp, bucket->total_size, folder->total_size, bucket->file_count,
                        bucket->name);
        shown_size += bucket->total_size;
        shown_files += bucket->file_count;
    }
    const unsigned long long other_subdir_files =
        minus(minus(folder->total_files, folder->direct_files), shown_files);
    if (other_subdir_files > 0) {
        write_share_row(fp, minus(minus(folder->total_size, folder->direct_size), shown_size),
                        folder->total_size, other_subdir_files, "(other subfolders)");
    }
    fputs("</pre>\n", fp);

    /* Largest file types, plus the remainder */
    fputs("<h2>Largest file types</h2>\n<pre>", fp);
    write_share_header(fp, "Extension");
    shown_size  = 0;
    shown_files = 0;
    for (size_t i = 0; i < folder->ext_count && i < RESTIC_REPORT_TOP_EXTENSIONS; ++i) {
        const ResticReportExtension* bucket = &folder->extensions[i];
        write_share_row(fp, bucket->total_size, folder->total_size, bucket->file_count, bucket->ext);
        shown_size += bucket->total_size;
        shown_files += bucket->file_count;
    }
    if (folder->total_files > shown_files) {
        write_share_row(fp, minus(folder->total_size, shown_size), folder->total_size,
                        folder->total_files - shown_files, "(other)");
    }
    fputs("</pre>\n", fp);

    /* File size distribution */
    fputs("<h2>File size distribution</h2>\n<pre>", fp);
    {
        char line[192];
        (void)snprintf(line, sizeof(line), "  %-14s  %7s  %6s  %11s  %s\n", "Range", "Files",
                       "Share", "Total size", "Distribution");
        fputs(line, fp);
        (void)snprintf(line, sizeof(line), "  %-14s  %7s  %6s  %11s  %s\n", "--------------",
                       "-------", "------", "-----------", "--------------------");
        fputs(line, fp);
    }
    for (int i = 0; i < RESTIC_REPORT_HIST_BUCKETS; ++i) {
        const double share = percentage_of(folder->hist_files[i], folder->total_files);
        char percent_text[32];
        char bar[256];
        restic_report_format_bytes(folder->hist_bytes[i], size_text, sizeof(size_text));
        restic_report_format_bar(bar, sizeof(bar), share / 100.0, 20);
        (void)snprintf(percent_text, sizeof(percent_text), "%.1f%%", share);
        (void)snprintf(text, sizeof(text), "  %-14s  %7llu  %6s  %11s  ", kHistogramLabels[i],
                       folder->hist_files[i], percent_text, size_text);
        fputs(text, fp);
        restic_report_escape_write(fp, bar);
        fputc('\n', fp);
    }
    fputs("</pre>\n", fp);

    /* Largest files in this folder */
    fputs("<h2>Largest files</h2>\n<pre>", fp);
    if (folder->largest_count == 0) {
        fputs("  (no files)\n", fp);
    } else {
        fputs("   #  Size        Modified (UTC)       Path\n", fp);
        fputs("  --  ----------  -------------------  ----\n", fp);
        for (size_t i = 0; i < folder->largest_count; ++i) {
            const ResticReportLargestFile* file = &folder->largest[i];
            restic_report_format_bytes(file->size, size_text, sizeof(size_text));
            (void)snprintf(text, sizeof(text), "%3llu  %11s  %-19s  ",
                           (unsigned long long)(i + 1), size_text,
                           file->mtime[0] != '\0' ? file->mtime : "(unknown)");
            fputs(text, fp);
            restic_report_escape_write(fp, file->rel_path);
            fputc('\n', fp);
        }
    }
    fputs("</pre>\n", fp);
    write_html_end(fp);
}

/* ------------------------------------------------------------------------------------------ */
/* Virtual file entry                                                                          */
/* ------------------------------------------------------------------------------------------ */

BOOL restic_report_fill_find_data(const WCHAR* repository_alias, const char* snapshot_id,
                                  WIN32_FIND_DATAW* find_data) {
    if (repository_alias == NULL || find_data == NULL) return FALSE;

    memset(find_data, 0, sizeof(*find_data));
    wcsncpy_s(find_data->cFileName, MAX_PATH, RESTIC_REPORT_FILENAME_W, _TRUNCATE);
    find_data->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
    find_data->nFileSizeLow     = (DWORD)(RESTIC_REPORT_PLACEHOLDER_SIZE & 0xFFFFFFFFULL);
    find_data->nFileSizeHigh    = (DWORD)(RESTIC_REPORT_PLACEHOLDER_SIZE >> 32);

    ResticSnapshotInfo info;
    BOOL known = FALSE;
    if (snapshot_id != NULL && snapshot_id[0] != '\0') {
        known = snapshot_stats_lookup(repository_alias, snapshot_id, &info);
    } else {
        known = snapshot_stats_lookup_latest(repository_alias, &info);
    }

    if (known) {
        find_data->ftLastWriteTime  = info.time;
        find_data->ftCreationTime   = info.time;
        find_data->ftLastAccessTime = info.time;
    } else {
        FS_SET_TIME_UNKNOWN(find_data->ftLastWriteTime);
    }
    return TRUE;
}

BOOL restic_report_prepend_entry(WIN32_FIND_DATAW** items, size_t* count,
                                 const WCHAR* repository_alias, const char* snapshot_id) {
    if (items == NULL || count == NULL || repository_alias == NULL) return FALSE;

    /* A backed-up file with the very same name must stay reachable: the real file wins */
    for (size_t i = 0; i < *count; ++i) {
        if (_wcsicmp((*items)[i].cFileName, RESTIC_REPORT_FILENAME_W) == 0) return FALSE;
    }

    WIN32_FIND_DATAW entry;
    if (!restic_report_fill_find_data(repository_alias, snapshot_id, &entry)) return FALSE;

    WIN32_FIND_DATAW* grown =
        (WIN32_FIND_DATAW*)realloc(*items, (*count + 1) * sizeof(WIN32_FIND_DATAW));
    if (grown == NULL) return FALSE;
    if (*count > 0) memmove(grown + 1, grown, *count * sizeof(WIN32_FIND_DATAW));
    grown[0] = entry;
    *items   = grown;
    (*count)++;
    return TRUE;
}

/* ------------------------------------------------------------------------------------------ */
/* Generation (runs restic.exe and writes the report)                                         */
/* ------------------------------------------------------------------------------------------ */

/* State shared by the three report builders */
typedef struct {
    const WCHAR* restic_exe;
    const WCHAR* repo_target;
    const WCHAR* password;
    const ResticExecOptions* options;
    ResticReportProgressProc progress;
    void* progress_ctx;
    int last_percent;
    char alias[REPORT_ALIAS_BYTES];       /* UTF-8 copies used by the HTML writers */
    char target[REPORT_MAX_PATH_BYTES];
} ReportContext;

/* Reports a new progress percentage; TRUE means the user asked to abort */
static BOOL report_step(ReportContext* ctx, int percent) {
    if (ctx->progress == NULL || percent <= ctx->last_percent) return FALSE;
    ctx->last_percent = percent;
    return ctx->progress(percent, ctx->progress_ctx) ? TRUE : FALSE;
}

/* After a successful phase moves the progress forward; ESC turns the phase into a cancellation */
static ResticResult advance(ReportContext* ctx, ResticResult res, int percent) {
    return (res == RESTIC_OK && report_step(ctx, percent)) ? RESTIC_ERR_CANCELLED : res;
}

/* Failures that must abort the whole report instead of degrading one of its sections */
static BOOL result_is_fatal(ResticResult res) {
    switch (res) {
        case RESTIC_ERR_CANCELLED:
        case RESTIC_ERR_TIMEOUT:
        case RESTIC_ERR_WRONG_PASSWORD:
        case RESTIC_ERR_REPO_NOT_FOUND:
        case RESTIC_ERR_REPO_LOCKED:
        case RESTIC_ERR_OUT_OF_MEMORY:
        case RESTIC_ERR_SPAWN_FAILED:
            return TRUE;
        default:
            return FALSE;
    }
}

/* Runs a Restic command that prints one JSON document; on success `*out_json` is heap allocated */
static ResticResult report_run_json(ReportContext* ctx, const WCHAR* args, char** out_json,
                                    WCHAR* err, size_t err_capacity) {
    *out_json = NULL;
    ResticCommandOutput out;
    ResticResult res = restic_exec_command(ctx->restic_exe, ctx->repo_target, ctx->password, args,
                                           ctx->options, &out);
    if (res != RESTIC_OK) {
        if (err != NULL && err_capacity > 0) {
            wcsncpy_s(err, err_capacity, out.error_message, _TRUNCATE);
        }
        restic_free_command_output(&out);
        return res;
    }
    *out_json = out.output;
    out.output = NULL;
    restic_free_command_output(&out);
    return RESTIC_OK;
}

/* Creates "<directory of dest>\~rprep<thread>.tmp" and returns the open file */
static FILE* open_report_file(const WCHAR* destination, WCHAR* temp_path, size_t temp_capacity) {
    size_t dir_length = 0;
    for (size_t i = 0; destination[i] != L'\0'; ++i) {
        if (destination[i] == L'\\' || destination[i] == L'/') dir_length = i + 1;
    }
    if (dir_length + 32 > temp_capacity) return NULL;

    wmemcpy(temp_path, destination, dir_length);
    if (swprintf_s(temp_path + dir_length, temp_capacity - dir_length, L"~rprep%lx.tmp",
                   GetCurrentThreadId()) < 0) {
        return NULL;
    }

    FILE* fp = NULL;
    if (_wfopen_s(&fp, temp_path, L"wb") != 0) return NULL;
    return fp;
}

/* Closes the report file and publishes it over the destination (or discards it on failure) */
static ResticResult publish_report(FILE* fp, const WCHAR* temp_path, const WCHAR* destination) {
    BOOL ok = (ferror(fp) == 0);
    if (fclose(fp) != 0) ok = FALSE;

    if (ok) {
        /* The listed entry is read-only, so Total Commander can hand over an earlier copy that is
           read-only too; FsGetFileW already established that overwriting it is allowed */
        DWORD attr = GetFileAttributesW(destination);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY) != 0 &&
            (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            SetFileAttributesW(destination, attr & ~FILE_ATTRIBUTE_READONLY);
        }
        ok = MoveFileExW(temp_path, destination, MOVEFILE_REPLACE_EXISTING) != 0;
    }
    if (!ok) DeleteFileW(temp_path);
    return ok ? RESTIC_OK : RESTIC_ERR_LOCAL_WRITE;
}

/*
 * Reads `restic stats --mode <mode> [snapshot]`. A failure only leaves the block unavailable,
 * except for cancellation, timeout and similar fatal conditions, which abort the report.
 */
static ResticResult read_storage_stats(ReportContext* ctx, const WCHAR* mode,
                                       const WCHAR* snapshot_w, ResticReportStorage* storage,
                                       WCHAR* err, size_t err_capacity) {
    WCHAR args[192];
    memset(storage, 0, sizeof(*storage));
    if (snapshot_w != NULL && snapshot_w[0] != L'\0') {
        (void)swprintf_s(args, _countof(args), L"stats --json --no-lock --mode %ls %ls", mode,
                         snapshot_w);
    } else {
        (void)swprintf_s(args, _countof(args), L"stats --json --no-lock --mode %ls", mode);
    }

    char* json       = NULL;
    ResticResult res = report_run_json(ctx, args, &json, err, err_capacity);
    if (res == RESTIC_OK) {
        restic_report_parse_storage_json(json, storage);
        free(json);
        return RESTIC_OK;
    }
    return result_is_fatal(res) ? res : RESTIC_OK;
}

/* Loads `snapshots --json [id]` into a snapshot array (free with restic_free_snapshot_list) */
static ResticResult load_snapshots(ReportContext* ctx, const WCHAR* snapshot_w,
                                   ResticSnapshotInfo** out_items, size_t* out_count,
                                   WCHAR* err, size_t err_capacity) {
    WCHAR args[160];
    *out_items = NULL;
    *out_count = 0;
    if (snapshot_w != NULL && snapshot_w[0] != L'\0') {
        (void)swprintf_s(args, _countof(args), L"snapshots --json --no-lock %ls", snapshot_w);
    } else {
        wcsncpy_s(args, _countof(args), L"snapshots --json --no-lock", _TRUNCATE);
    }

    char* json       = NULL;
    ResticResult res = report_run_json(ctx, args, &json, err, err_capacity);
    if (res != RESTIC_OK) return res;

    res = restic_parse_snapshot_list(json, out_items, out_count);
    free(json);
    return res;
}

/* ------------------------------------------------------------------------------------------ */
/* Level 1: repository report                                                                  */
/* ------------------------------------------------------------------------------------------ */

static ResticResult generate_repo_report(ReportContext* ctx, const WCHAR* destination,
                                         WCHAR* err, size_t err_capacity) {
    ResticSnapshotInfo* snapshots = NULL;
    size_t snapshot_count         = 0;
    ResticReportRepo report;
    WCHAR temp[REPORT_MAX_PATH_UNITS + 64] = {0};
    FILE* fp = NULL;
    memset(&report, 0, sizeof(report));

    ResticResult res =
        advance(ctx, load_snapshots(ctx, NULL, &snapshots, &snapshot_count, err, err_capacity), 25);
    if (res == RESTIC_OK) {
        res = advance(ctx, read_storage_stats(ctx, L"restore-size", NULL, &report.restore, err,
                                              err_capacity), 55);
    }
    if (res == RESTIC_OK) {
        res = advance(ctx, read_storage_stats(ctx, L"raw-data", NULL, &report.stored, err,
                                              err_capacity), 80);
    }
    if (res == RESTIC_OK) {
        fp = open_report_file(destination, temp, _countof(temp));
        if (fp == NULL) res = RESTIC_ERR_LOCAL_WRITE;
    }
    if (res == RESTIC_OK) {
        report.alias          = ctx->alias;
        report.repo_target    = ctx->target;
        report.snapshots      = snapshots;
        report.snapshot_count = snapshot_count;
        restic_report_write_repo_html(fp, &report);
        res = publish_report(fp, temp, destination);
    }
    restic_free_snapshot_list(snapshots);
    return res;
}

/* ------------------------------------------------------------------------------------------ */
/* Level 2: snapshot report                                                                    */
/* ------------------------------------------------------------------------------------------ */

static BOOL diff_line_callback(const char* line, size_t length, void* user_data) {
    restic_report_parse_diff_line((ResticReportDiff*)user_data, line, length);
    return TRUE;
}

/* Runs `restic diff parent snapshot`; a failure only produces an explanatory note */
static ResticResult read_diff(ReportContext* ctx, const ResticSnapshotInfo* info,
                              const WCHAR* snapshot_w, ResticReportDiff* diff, char* note,
                              size_t note_capacity) {
    if (info->parent[0] == '\0' || !is_hex_id(info->parent)) {
        copy_utf8(note, note_capacity,
                  "Initial snapshot: there is no parent snapshot to compare with.");
        return RESTIC_OK;
    }

    copy_utf8(diff->parent_short_id, sizeof(diff->parent_short_id), info->parent);
    WCHAR parent_w[65] = {0};
    WCHAR args[192];
    WCHAR ignored[128] = {0};
    MultiByteToWideChar(CP_UTF8, 0, info->parent, -1, parent_w, (int)_countof(parent_w));
    (void)swprintf_s(args, _countof(args), L"diff --json --no-lock %ls %ls", parent_w, snapshot_w);

    ResticResult res = restic_exec_lines(ctx->restic_exe, ctx->repo_target, ctx->password, args,
                                         ctx->options, diff_line_callback, diff, ignored,
                                         _countof(ignored));
    if (result_is_fatal(res)) return res;

    if (!diff->valid) {
        (void)snprintf(note, note_capacity,
                       "The diff against parent snapshot %s could not be computed "
                       "(the parent may have been removed with 'restic forget').",
                       diff->parent_short_id);
    }
    return RESTIC_OK;
}

static ResticResult generate_snapshot_report(ReportContext* ctx, const char* snapshot_id,
                                             const WCHAR* destination, WCHAR* err,
                                             size_t err_capacity) {
    WCHAR snapshot_w[65]       = {0};
    ResticSnapshotInfo* loaded = NULL;
    size_t loaded_count        = 0;
    char note[256]             = {0};
    WCHAR temp[REPORT_MAX_PATH_UNITS + 64] = {0};
    FILE* fp = NULL;
    ResticReportSnapshot report;
    ResticReportDiff* diff = (ResticReportDiff*)calloc(1, sizeof(*diff)); /* Too big for the stack */
    memset(&report, 0, sizeof(report));
    MultiByteToWideChar(CP_UTF8, 0, snapshot_id, -1, snapshot_w, (int)_countof(snapshot_w));

    ResticResult res = (diff != NULL) ? RESTIC_OK : RESTIC_ERR_OUT_OF_MEMORY;
    if (res == RESTIC_OK) {
        res = advance(ctx, load_snapshots(ctx, snapshot_w, &loaded, &loaded_count, err,
                                          err_capacity), 20);
    }
    if (res == RESTIC_OK) {
        report.info = restic_find_snapshot_by_short_id(loaded, loaded_count, snapshot_id);
        if (report.info == NULL) res = RESTIC_ERR_PATH_NOT_FOUND; /* Restic printed "[]" */
    }
    if (res == RESTIC_OK) {
        res = advance(ctx, read_storage_stats(ctx, L"restore-size", snapshot_w, &report.restore,
                                              err, err_capacity), 45);
    }
    if (res == RESTIC_OK) {
        res = advance(ctx, read_storage_stats(ctx, L"raw-data", snapshot_w, &report.stored, err,
                                              err_capacity), 65);
    }
    if (res == RESTIC_OK) {
        res = advance(ctx, read_diff(ctx, report.info, snapshot_w, diff, note, sizeof(note)), 85);
    }
    if (res == RESTIC_OK) {
        fp = open_report_file(destination, temp, _countof(temp));
        if (fp == NULL) res = RESTIC_ERR_LOCAL_WRITE;
    }
    if (res == RESTIC_OK) {
        report.alias       = ctx->alias;
        report.repo_target = ctx->target;
        report.diff        = diff;
        report.diff_note   = note;
        restic_report_write_snapshot_html(fp, &report);
        res = publish_report(fp, temp, destination);
    }
    free(diff);
    restic_free_snapshot_list(loaded);
    return res;
}

/* ------------------------------------------------------------------------------------------ */
/* Level 3: snapshot subfolder report                                                          */
/* ------------------------------------------------------------------------------------------ */

/* State of one `restic ls --json --recursive` scan */
typedef struct {
    ResticReportFolder* folder;
    ReportContext* ctx;
    const char* filter_path;
    BOOL first_line_checked;
    char snapshot_short_id[RESTIC_STATS_SHORT_ID_MAX];
    char snapshot_time[REPORT_TIME_BUFFER];
    char hostname[RESTIC_STATS_HOST_MAX];
    unsigned long long line_count;
} SubtreeScan;

/* Bounded substring search: the line is not NUL terminated */
static BOOL line_contains(const char* line, size_t length, const char* needle) {
    const size_t needle_length = strlen(needle);
    for (size_t i = 0; i + needle_length <= length; ++i) {
        if (memcmp(line + i, needle, needle_length) == 0) return TRUE;
    }
    return FALSE;
}

/* Reads the snapshot header line that Restic prints before the nodes */
static void read_snapshot_header(SubtreeScan* scan, const char* line, size_t length) {
    cJSON* root = cJSON_ParseWithLength(line, length);
    if (root != NULL && cJSON_IsObject(root)) {
        copy_utf8(scan->snapshot_short_id, sizeof(scan->snapshot_short_id), json_str(root, "short_id"));
        copy_utf8(scan->hostname, sizeof(scan->hostname), json_str(root, "hostname"));
        const char* time_iso = json_str(root, "time");
        if (time_iso != NULL) {
            iso_to_report_time(time_iso, scan->snapshot_time, sizeof(scan->snapshot_time));
        }
    }
    cJSON_Delete(root);
}

static BOOL subtree_line_callback(const char* line, size_t length, void* user_data) {
    SubtreeScan* scan = (SubtreeScan*)user_data;
    if (length == 0) return TRUE;

    /* The snapshot header is always the first line of the stream */
    if (!scan->first_line_checked) {
        scan->first_line_checked = TRUE;
        if (line_contains(line, length, "\"struct_type\":\"snapshot\"") ||
            line_contains(line, length, "\"message_type\":\"snapshot\"")) {
            read_snapshot_header(scan, line, length);
            return TRUE;
        }
    }

    restic_report_aggregate_ls_line(scan->folder, scan->filter_path, line, length);
    scan->line_count++;
    /* The total number of nodes is unknown in advance: ramp the progress up to 85 % */
    if ((scan->line_count % 250ULL) == 0) {
        int percent = 15 + (int)(scan->line_count / 250ULL);
        if (percent > 85) percent = 85;
        if (report_step(scan->ctx, percent)) return FALSE; /* ESC pressed */
    }
    return TRUE;
}

static ResticResult generate_subtree_report(ReportContext* ctx, const char* snapshot_id,
                                            const WCHAR* subpath_w, const WCHAR* destination,
                                            WCHAR* err, size_t err_capacity) {
    char filter[REPORT_MAX_PATH_BYTES]        = {0};
    WCHAR filter_w[REPORT_MAX_PATH_UNITS + 1] = {0};
    WCHAR snapshot_w[65]                      = {0};

    if (!restic_normalize_filter_path(subpath_w, filter, sizeof(filter)) ||
        MultiByteToWideChar(CP_UTF8, 0, filter, -1, filter_w, (int)_countof(filter_w)) <= 0) {
        return RESTIC_ERR_INVALID_ARG;
    }
    MultiByteToWideChar(CP_UTF8, 0, snapshot_id, -1, snapshot_w, (int)_countof(snapshot_w));

    WCHAR* quoted = restic_quote_arg(filter_w);
    if (quoted == NULL) return RESTIC_ERR_OUT_OF_MEMORY;
    const size_t args_chars = wcslen(L"ls --json --no-lock --recursive  ") + wcslen(snapshot_w) +
                              wcslen(quoted) + 1;
    WCHAR* ls_args = (WCHAR*)malloc(args_chars * sizeof(WCHAR));
    ResticReportFolder* folder = (ResticReportFolder*)calloc(1, sizeof(*folder));
    char* display_path         = (char*)calloc(1, REPORT_MAX_PATH_BYTES);
    if (ls_args == NULL || folder == NULL || display_path == NULL) {
        free(quoted);
        free(ls_args);
        free(folder);
        free(display_path);
        return RESTIC_ERR_OUT_OF_MEMORY;
    }
    (void)swprintf_s(ls_args, args_chars, L"ls --json --no-lock --recursive %ls %ls", snapshot_w,
                     quoted);
    free(quoted);
    wide_to_utf8(subpath_w, display_path, REPORT_MAX_PATH_BYTES);

    SubtreeScan scan;
    memset(&scan, 0, sizeof(scan));
    scan.folder      = folder;
    scan.ctx         = ctx;
    scan.filter_path = filter;

    ResticResult res = restic_exec_lines(ctx->restic_exe, ctx->repo_target, ctx->password, ls_args,
                                         ctx->options, subtree_line_callback, &scan, err,
                                         err_capacity);
    free(ls_args);

    /* Restic prints only the header for a path that does not exist */
    if (res == RESTIC_OK && !folder->root_seen) res = RESTIC_ERR_PATH_NOT_FOUND;
    if (res == RESTIC_OK) {
        restic_report_finalize_folder(folder);
        res = advance(ctx, RESTIC_OK, 90);
    }

    WCHAR temp[REPORT_MAX_PATH_UNITS + 64] = {0};
    FILE* fp = NULL;
    if (res == RESTIC_OK) {
        fp = open_report_file(destination, temp, _countof(temp));
        if (fp == NULL) res = RESTIC_ERR_LOCAL_WRITE;
    }
    if (res == RESTIC_OK) {
        ResticReportSubtree report;
        memset(&report, 0, sizeof(report));
        report.alias             = ctx->alias;
        report.repo_target       = ctx->target;
        report.snapshot_short_id = scan.snapshot_short_id[0] != '\0' ? scan.snapshot_short_id
                                                                     : snapshot_id;
        report.snapshot_time     = scan.snapshot_time;
        report.hostname          = scan.hostname;
        report.display_path      = display_path;
        report.folder            = folder;
        restic_report_write_subtree_html(fp, &report);
        res = publish_report(fp, temp, destination);
    }
    free(folder);
    free(display_path);
    return res;
}

/* ------------------------------------------------------------------------------------------ */
/* Public entry point                                                                          */
/* ------------------------------------------------------------------------------------------ */

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
) {
    if (out_error_msg != NULL && err_capacity > 0) out_error_msg[0] = L'\0';
    if (restic_exe == NULL || repo_target == NULL || password == NULL ||
        repository_alias == NULL || local_dest_path == NULL || local_dest_path[0] == L'\0') {
        return RESTIC_ERR_INVALID_ARG;
    }
    if (snapshot_id != NULL && !is_hex_id(snapshot_id)) {
        return RESTIC_ERR_INVALID_ARG;
    }

    ReportContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.restic_exe   = restic_exe;
    ctx.repo_target  = repo_target;
    ctx.password     = password;
    ctx.options      = options;
    ctx.progress     = progress;
    ctx.progress_ctx = progress_ctx;
    ctx.last_percent = -1;
    wide_to_utf8(repository_alias, ctx.alias, sizeof(ctx.alias));
    wide_to_utf8(repo_target, ctx.target, sizeof(ctx.target));

    if (report_step(&ctx, 5)) return RESTIC_ERR_CANCELLED;
    if (snapshot_id == NULL) {
        return generate_repo_report(&ctx, local_dest_path, out_error_msg, err_capacity);
    }

    const WCHAR* sub = (subpath_w != NULL) ? subpath_w : L"";
    while (*sub == L'\\') ++sub;
    if (*sub == L'\0') {
        return generate_snapshot_report(&ctx, snapshot_id, local_dest_path, out_error_msg,
                                        err_capacity);
    }
    return generate_subtree_report(&ctx, snapshot_id, subpath_w, local_dest_path, out_error_msg,
                                   err_capacity);
}
