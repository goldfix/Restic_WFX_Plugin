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
#include "wfx_trace.h"
#include "cJSON.h"

/*
 * Bar characters are pure ASCII: '#'.' take exactly one byte and one column in every font. The
 * Unicode blocks of the first version (U+2588/U+2591) touched vertically in the Lister and changed
 * width through font fallback, which broke the table alignment.
 */
#define BAR_FILLED_CHAR '#'
#define BAR_EMPTY_CHAR  '.'
/* Width used by every table of the reports */
#define BAR_WIDTH 20

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
        if (pos + 2 >= out_capacity) break;
        out[pos++] = (i < filled) ? BAR_FILLED_CHAR : BAR_EMPTY_CHAR;
    }
    if (pos + 1 < out_capacity) out[pos++] = ']';
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

void restic_report_write_text(FILE* fp, const char* text) {
    if (fp == NULL) return;
    if (text == NULL) text = "";
    for (const char* p = text; *p != '\0'; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c == '|') {
            fputc('/', fp); /* One character wide, so the table columns stay aligned */
        } else if (c == '\r' || c == '\n' || c == '\t') {
            fputc(' ', fp); /* A line break would end the table row */
        } else {
            fputc(c, fp);
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

    const char* type          = json_str(root, "type");
    const BOOL is_dir         = (type != NULL && strcmp(type, "dir") == 0);
    const size_t filter_length = strlen(filter_path);
    const BOOL at_root         = (filter_length == 1 && filter_path[0] == '/');

    /* Restic repeats the filtered directory itself: never count it as one of its own children.
       When the filter names a file (or a symlink), Restic prints that node: it is not a folder, so
       `root_seen` stays FALSE and the caller reports "path not found" instead of a report of a
       single file. A node without a type is accepted, as Restic always sends one. */
    if (!at_root) {
        if (strcmp(path, filter_path) == 0) {
            if (type == NULL || is_dir) folder->root_seen = TRUE;
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

    const char* mtime_iso     = json_str(root, "mtime");
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
/* Markdown helpers                                                                            */
/* ------------------------------------------------------------------------------------------ */

/* Width of the label column of the key/value tables */
#define MD_LABEL_WIDTH 30

/* One column of a pipe table: `width` is the minimum number of characters of a cell */
typedef struct {
    const char* title;
    int width;
    BOOL right_align;
} MdColumn;

/* Minimum width of a column, never narrower than its own title */
static int column_width(const MdColumn* column) {
    const int title_width = (column->title != NULL) ? (int)strlen(column->title) : 0;
    return (column->width > title_width) ? column->width : title_width;
}

/* Writes one cell ("| value<padding> "); the closing pipe is written by the row helper */
static void write_md_cell(FILE* fp, const MdColumn* column, const char* value) {
    const int length  = (value != NULL) ? (int)strlen(value) : 0;
    const int padding = column_width(column) - length;

    fputs("| ", fp);
    if (column->right_align && padding > 0) {
        for (int i = 0; i < padding; ++i) fputc(' ', fp);
    }
    restic_report_write_text(fp, value);
    if (!column->right_align && padding > 0) {
        for (int i = 0; i < padding; ++i) fputc(' ', fp);
    }
    fputc(' ', fp);
}

/* Writes the header row and the alignment row of a table */
static void write_md_table_begin(FILE* fp, const MdColumn* columns, size_t count) {
    for (size_t i = 0; i < count; ++i) write_md_cell(fp, &columns[i], columns[i].title);
    fputs("|\n", fp);

    /* ':' marks the aligned side, the dashes give the column its width */
    for (size_t i = 0; i < count; ++i) {
        int dashes = column_width(&columns[i]);
        if (dashes < 3) dashes = 3;
        fputs("| ", fp);
        if (columns[i].right_align) {
            for (int d = 0; d < dashes - 1; ++d) fputc('-', fp);
            fputc(':', fp);
        } else {
            fputc(':', fp);
            for (int d = 0; d < dashes - 1; ++d) fputc('-', fp);
        }
        fputc(' ', fp);
    }
    fputs("|\n", fp);
}

/* Writes one table row; `cells` holds one entry per column (NULL = empty cell) */
static void write_md_row(FILE* fp, const MdColumn* columns, size_t count,
                         const char* const* cells) {
    for (size_t i = 0; i < count; ++i) write_md_cell(fp, &columns[i], cells[i]);
    fputs("|\n", fp);
}

/* Layout shared by every key/value table, so all of them line up in the Lister */
static const MdColumn kKeyValueColumns[2] = {
    {"Property", MD_LABEL_WIDTH, FALSE},
    {"Value", 5, FALSE}
};

/* Writes the header of a key/value table (`key_title` is "Property" or "Metric") */
static void write_md_kv_begin(FILE* fp, const char* key_title) {
    MdColumn columns[2];
    memcpy(columns, kKeyValueColumns, sizeof(columns));
    columns[0].title = key_title;
    write_md_table_begin(fp, columns, 2);
}

/* Writes one "| label | value |" row */
static void write_md_labeled(FILE* fp, const char* label, const char* value) {
    const char* cells[2] = {label, value};
    write_md_row(fp, kKeyValueColumns, 2, cells);
}

/* Writes the document title ("# <title> — <subtitle>") */
static void write_md_title(FILE* fp, const char* title, const char* subtitle) {
    fputs("# ", fp);
    fputs(title, fp);
    if (subtitle != NULL && subtitle[0] != '\0') {
        fputs(" \xE2\x80\x94 ", fp); /* U+2014 em dash: one column wide */
        restic_report_write_text(fp, subtitle);
    }
    fputs("\n\n", fp);
}

/* Writes a section headline ("## <title>") */
static void write_md_section(FILE* fp, const char* title) {
    fputs("\n## ", fp);
    fputs(title, fp);
    fputs("\n\n", fp);
}

/* Writes the repository location with any credentials embedded in a URL masked */
static void write_location(FILE* fp, const char* target) {
    if (target == NULL || target[0] == '\0') return;
    char safe[REPORT_MAX_PATH_BYTES];
    restic_report_redact_location(target, safe, sizeof(safe));
    write_md_labeled(fp, "Location", safe);
}

static double percentage_of(unsigned long long part, unsigned long long total) {
    if (total == 0) return 0.0;
    return ((double)part * 100.0) / (double)total;
}

/* Storage table shared by the repository and the snapshot report */
static void write_storage_block(FILE* fp, const ResticReportStorage* restore,
                                const ResticReportStorage* stored) {
    char text[64];
    write_md_section(fp, "Storage and deduplication");
    write_md_kv_begin(fp, "Metric");

    restic_report_format_bytes(restore->total_size, text, sizeof(text));
    write_md_labeled(fp, "Logical size (full restore)", text);
    if (restore->total_file_count > 0) {
        restic_report_format_uint(restore->total_file_count, text, sizeof(text));
        write_md_labeled(fp, "Files in the restore", text);
    }

    if (stored->valid) {
        restic_report_format_bytes(stored->uncompressed_size, text, sizeof(text));
        write_md_labeled(fp, "Uncompressed (deduplicated)", text);
        restic_report_format_bytes(stored->total_size, text, sizeof(text));
        write_md_labeled(fp, "Stored on disk (compressed)", text);

        (void)snprintf(text, sizeof(text), "%.2f x", stored->compression_ratio);
        write_md_labeled(fp, "Compression ratio", text);
        (void)snprintf(text, sizeof(text), "%.1f %%", stored->space_saving_percent);
        write_md_labeled(fp, "Saved by compression", text);

        if (stored->blob_count > 0) {
            restic_report_format_uint(stored->blob_count, text, sizeof(text));
            write_md_labeled(fp, "Stored blobs", text);
        }
    }

    if (stored->valid && stored->total_size > 0 && restore->total_size > 0) {
        (void)snprintf(text, sizeof(text), "%.2f x",
                       (double)restore->total_size / (double)stored->total_size);
        write_md_labeled(fp, "Deduplication factor", text);
        if (restore->total_size > stored->total_size) {
            (void)snprintf(text, sizeof(text), "%.1f %%",
                           (1.0 - (double)stored->total_size / (double)restore->total_size) * 100.0);
            write_md_labeled(fp, "Total space saved", text);
        }
    }
}

static void snapshot_time_text(const ResticSnapshotInfo* info, char* out, size_t capacity) {
    if (!restic_report_format_time(&info->time, out, capacity)) {
        copy_utf8(out, capacity, "(unknown)");
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Repository report                                                                           */
/* ------------------------------------------------------------------------------------------ */

/* Column layout of the snapshot history table */
static const MdColumn kHistoryColumns[] = {
    {"Short ID", 8, FALSE},
    {"Date / time (UTC)", 19, FALSE},
    {"Host", 18, FALSE},
    {"Files", 7, TRUE},
    {"Size", 10, TRUE},
    {"Data added", 10, TRUE},
    {"Growth", BAR_WIDTH + 2, FALSE}
};

void restic_report_write_repo_md(FILE* fp, const ResticReportRepo* report) {
    if (fp == NULL || report == NULL) return;

    char text[64];
    write_md_title(fp, "Restic repository report", report->alias);

    write_md_kv_begin(fp, "Property");
    write_md_labeled(fp, "Repository", report->alias);
    write_location(fp, report->repo_target);
    restic_report_format_uint((unsigned long long)report->snapshot_count, text, sizeof(text));
    write_md_labeled(fp, "Snapshots", text);
    if (report->snapshot_count > 0) {
        snapshot_time_text(&report->snapshots[0], text, sizeof(text));
        write_md_labeled(fp, "First snapshot (UTC)", text);
        snapshot_time_text(&report->snapshots[report->snapshot_count - 1], text, sizeof(text));
        write_md_labeled(fp, "Latest snapshot (UTC)", text);
    }

    write_storage_block(fp, &report->restore, &report->stored);

    /* Snapshot history, newest first (Restic prints the list oldest first) */
    write_md_section(fp, "Snapshot history");
    write_md_table_begin(fp, kHistoryColumns, _countof(kHistoryColumns));

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
        char bar[BAR_WIDTH + 4];
        snapshot_time_text(info, when, sizeof(when));
        double ratio = 0.0;
        if (info->has_summary) { /* Snapshots written without a summary have no figures */
            restic_report_format_uint(info->total_files_processed, files, sizeof(files));
            restic_report_format_bytes(info->total_bytes_processed, size, sizeof(size));
            restic_report_format_bytes(info->data_added_packed, added, sizeof(added));
            ratio = (max_added == 0) ? 0.0 : (double)info->data_added_packed / (double)max_added;
        }
        restic_report_format_bar(bar, sizeof(bar), ratio, BAR_WIDTH);

        const char* cells[7] = {info->short_id, when,      info->hostname, files,
                                size,            added,     bar};
        write_md_row(fp, kHistoryColumns, _countof(kHistoryColumns), cells);
    }
    fputs("\n", fp);
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

/* Column layout of the "files and directories" table */
static const MdColumn kCategoryColumns[] = {
    {"Category", 20, FALSE},
    {"Count", 9, TRUE},
    {"Share", 7, TRUE},
    {"Distribution", BAR_WIDTH + 2, FALSE}
};

/* Writes one "new / changed / unmodified" row with its share and bar */
static void write_category_row(FILE* fp, const char* label, unsigned long long value,
                               unsigned long long total) {
    char count[32];
    char share[32];
    char bar[BAR_WIDTH + 4];
    const double percent = percentage_of(value, total);

    restic_report_format_uint(value, count, sizeof(count));
    (void)snprintf(share, sizeof(share), "%.1f%%", percent);
    restic_report_format_bar(bar, sizeof(bar), percent / 100.0, BAR_WIDTH);

    const char* cells[4] = {label, count, share, bar};
    write_md_row(fp, kCategoryColumns, _countof(kCategoryColumns), cells);
}

/* Writes a row of the same table that only carries a meaningful count */
static void write_category_total(FILE* fp, const char* label, unsigned long long value) {
    char count[32];
    restic_report_format_uint(value, count, sizeof(count));
    const char* cells[4] = {label, count, "", ""};
    write_md_row(fp, kCategoryColumns, _countof(kCategoryColumns), cells);
}

static void write_diff_block(FILE* fp, const ResticReportSnapshot* report) {
    const ResticReportDiff* diff = report->diff;
    char text[128]; /* Three 20-digit counters plus separators */

    write_md_section(fp, "Changes since the parent snapshot");
    if (diff == NULL || !diff->valid) {
        const char* note = (report->diff_note != NULL && report->diff_note[0] != '\0')
                               ? report->diff_note
                               : "No diff is available for this snapshot.";
        restic_report_write_text(fp, note);
        fputs("\n", fp);
        return;
    }

    write_md_kv_begin(fp, "Metric");
    if (diff->parent_short_id[0] != '\0') {
        write_md_labeled(fp, "Parent snapshot", diff->parent_short_id);
    }
    restic_report_format_uint(diff->changed_files, text, sizeof(text));
    write_md_labeled(fp, "Modified files", text);

    (void)snprintf(text, sizeof(text), "%llu files, %llu dirs, %llu blobs", diff->added_files,
                   diff->added_dirs, diff->added_blobs);
    char added[192];
    char removed[192];
    restic_report_format_bytes(diff->added_bytes, added, sizeof(added));
    (void)snprintf(added + strlen(added), sizeof(added) - strlen(added), " (%s)", text);
    write_md_labeled(fp, "Added", added);

    (void)snprintf(text, sizeof(text), "%llu files, %llu dirs, %llu blobs", diff->removed_files,
                   diff->removed_dirs, diff->removed_blobs);
    restic_report_format_bytes(diff->removed_bytes, removed, sizeof(removed));
    (void)snprintf(removed + strlen(removed), sizeof(removed) - strlen(removed), " (%s)", text);
    write_md_labeled(fp, "Removed", removed);

    if (diff->change_count > 0) {
        fputs("\n```text\n", fp);
        for (size_t i = 0; i < diff->change_count; ++i) {
            const ResticReportDiffChange* change = &diff->changes[i];
            const char* marker                   = "[M]";
            if (strcmp(change->modifier, "+") == 0) {
                marker = "[+]";
            } else if (strcmp(change->modifier, "-") == 0) {
                marker = "[-]";
            }
            fputs(marker, fp);
            fputc(' ', fp);
            restic_report_write_text(fp, change->path);
            fputc('\n', fp);
        }
        if (diff->change_events > diff->change_count) {
            (void)snprintf(text, sizeof(text), "... %llu more changed paths not shown",
                           diff->change_events - diff->change_count);
            fputs(text, fp);
            fputc('\n', fp);
        }
        fputs("```\n", fp);
    }
}

void restic_report_write_snapshot_md(FILE* fp, const ResticReportSnapshot* report) {
    if (fp == NULL || report == NULL || report->info == NULL) return;
    const ResticSnapshotInfo* info = report->info;
    char text[64];
    char when[REPORT_TIME_BUFFER];

    write_md_title(fp, "Restic snapshot report", report->alias);

    write_md_kv_begin(fp, "Property");
    write_md_labeled(fp, "Snapshot ID", info->id);
    write_md_labeled(fp, "Short ID", info->short_id);
    write_md_labeled(fp, "Parent", info->parent[0] != '\0' ? info->parent : "(none: initial backup)");
    write_md_labeled(fp, "Host", info->hostname);
    write_md_labeled(fp, "User", info->username);
    write_md_labeled(fp, "Restic version", info->program_version);
    write_md_labeled(fp, "Tags", info->tags[0] != '\0' ? info->tags : "(none)");
    write_md_labeled(fp, "Source paths", info->paths);
    snapshot_time_text(info, when, sizeof(when));
    write_md_labeled(fp, "Created (UTC)", when);

    char start[REPORT_TIME_BUFFER];
    char end[REPORT_TIME_BUFFER];
    if (info->has_summary && restic_report_format_time(&info->backup_start, start, sizeof(start)) &&
        restic_report_format_time(&info->backup_end, end, sizeof(end))) {
        (void)snprintf(text, sizeof(text), "%s .. %s", start, end);
        write_md_labeled(fp, "Backup window (UTC)", text);
        format_duration(info->duration_seconds, text, sizeof(text));
        write_md_labeled(fp, "Duration", text);
    }

    if (!info->has_summary) {
        fputs("\nThis snapshot was stored without a summary block: the file and directory "
              "breakdown is not available.\n",
              fp);
    }

    write_storage_block(fp, &report->restore, &report->stored);

    if (info->has_summary) {
        const unsigned long long files_total = info->total_files_processed;
        const unsigned long long dirs_total =
            info->dirs_new + info->dirs_changed + info->dirs_unmodified;

        write_md_section(fp, "Files and directories");
        write_md_table_begin(fp, kCategoryColumns, _countof(kCategoryColumns));
        write_category_row(fp, "Files new", info->files_new, files_total);
        write_category_row(fp, "Files changed", info->files_changed, files_total);
        write_category_row(fp, "Files unchanged", info->files_unmodified, files_total);
        write_category_row(fp, "Dirs new", info->dirs_new, dirs_total);
        write_category_row(fp, "Dirs changed", info->dirs_changed, dirs_total);
        write_category_row(fp, "Dirs unchanged", info->dirs_unmodified, dirs_total);
        write_category_total(fp, "Files processed", files_total);
        write_category_total(fp, "Directories processed", dirs_total);
        if (files_total > 0) {
            restic_report_format_bytes(info->total_bytes_processed / files_total, text,
                                       sizeof(text));
            const char* cells[4] = {"Average file size", text, "", ""};
            write_md_row(fp, kCategoryColumns, _countof(kCategoryColumns), cells);
        }
        restic_report_format_bytes(info->total_bytes_processed, text, sizeof(text));
        const char* cells[4] = {"Bytes processed", text, "", ""};
        write_md_row(fp, kCategoryColumns, _countof(kCategoryColumns), cells);
    }

    write_diff_block(fp, report);
    fputs("\n", fp);
}

/* ------------------------------------------------------------------------------------------ */
/* Snapshot subfolder report                                                                   */
/* ------------------------------------------------------------------------------------------ */

/* Difference that never wraps below zero */
static unsigned long long minus(unsigned long long left, unsigned long long right) {
    return (left > right) ? (left - right) : 0;
}

/* Column layout of the subfolder and extension tables (they only differ in the last title) */
static const MdColumn kSubdirColumns[] = {
    {"Size", 10, TRUE},
    {"Share", 7, TRUE},
    {"Files", 7, TRUE},
    {"Bar", BAR_WIDTH + 2, FALSE},
    {"Name", 5, FALSE}
};
static const MdColumn kExtensionColumns[] = {
    {"Size", 10, TRUE},
    {"Share", 7, TRUE},
    {"Files", 7, TRUE},
    {"Bar", BAR_WIDTH + 2, FALSE},
    {"Extension", 5, FALSE}
};

/* Writes one share row: size, share of `total`, file count, bar and name */
static void write_share_row(FILE* fp, const MdColumn* columns, unsigned long long size,
                            unsigned long long total, unsigned long long files, const char* name) {
    char size_text[64];
    char percent_text[32];
    char count_text[32];
    char bar[BAR_WIDTH + 4];
    const double share = percentage_of(size, total);

    restic_report_format_bytes(size, size_text, sizeof(size_text));
    (void)snprintf(percent_text, sizeof(percent_text), "%.1f%%", share);
    restic_report_format_uint(files, count_text, sizeof(count_text));
    restic_report_format_bar(bar, sizeof(bar), share / 100.0, BAR_WIDTH);

    const char* cells[5] = {size_text, percent_text, count_text, bar, name};
    write_md_row(fp, columns, 5, cells);
}

/* Column layout of the file size histogram */
static const MdColumn kHistogramColumns[] = {
    {"Range", 14, FALSE},
    {"Files", 7, TRUE},
    {"Share", 7, TRUE},
    {"Total size", 11, TRUE},
    {"Distribution", BAR_WIDTH + 2, FALSE}
};

/* Column layout of the "largest files" table */
static const MdColumn kLargestColumns[] = {
    {"#", 3, TRUE},
    {"Size", 10, TRUE},
    {"Modified (UTC)", 19, FALSE},
    {"Path", 5, FALSE}
};

void restic_report_write_subtree_md(FILE* fp, const ResticReportSubtree* report) {
    if (fp == NULL || report == NULL || report->folder == NULL) return;
    const ResticReportFolder* folder = report->folder;
    char size_text[64];
    char text[128];

    write_md_title(fp, "Restic folder report", report->alias);

    write_md_kv_begin(fp, "Property");
    write_md_labeled(fp, "Folder", report->display_path != NULL ? report->display_path : "/");
    if (report->snapshot_short_id != NULL && report->snapshot_short_id[0] != '\0') {
        write_md_labeled(fp, "Snapshot", report->snapshot_short_id);
    }
    if (report->snapshot_time != NULL && report->snapshot_time[0] != '\0') {
        write_md_labeled(fp, "Snapshot date (UTC)", report->snapshot_time);
    }
    if (report->hostname != NULL && report->hostname[0] != '\0') {
        write_md_labeled(fp, "Host", report->hostname);
    }
    write_location(fp, report->repo_target);

    restic_report_format_bytes(folder->total_size, size_text, sizeof(size_text));
    write_md_labeled(fp, "Total size", size_text);
    (void)snprintf(text, sizeof(text), "%llu (%llu in this folder)", folder->total_files,
                   folder->direct_files);
    write_md_labeled(fp, "Files", text);
    (void)snprintf(text, sizeof(text), "%llu (%llu in this folder)", folder->total_dirs,
                   folder->direct_dirs);
    write_md_labeled(fp, "Directories", text);
    if (folder->total_files > 0) {
        restic_report_format_bytes(folder->total_size / folder->total_files, size_text,
                                   sizeof(size_text));
        write_md_labeled(fp, "Average file size", size_text);
    }
    if (folder->oldest_mtime[0] != '\0' && folder->newest_mtime[0] != '\0') {
        (void)snprintf(text, sizeof(text), "%s .. %s", folder->oldest_mtime, folder->newest_mtime);
        write_md_labeled(fp, "Modification span (UTC)", text);
    }

    /* Size by direct subfolder: the files of the folder itself, the biggest subfolders and one
       remainder row, so the shares always add up to 100 % */
    write_md_section(fp, "Size by subfolder");
    write_md_table_begin(fp, kSubdirColumns, _countof(kSubdirColumns));
    if (folder->direct_files > 0) {
        write_share_row(fp, kSubdirColumns, folder->direct_size, folder->total_size,
                        folder->direct_files, "(files in this folder)");
    }
    unsigned long long shown_size  = 0;
    unsigned long long shown_files = 0;
    for (size_t i = 0; i < folder->subdir_count && i < RESTIC_REPORT_TOP_SUBDIRS; ++i) {
        const ResticReportSubdir* bucket = &folder->subdirs[i];
        write_share_row(fp, kSubdirColumns, bucket->total_size, folder->total_size,
                        bucket->file_count, bucket->name);
        shown_size += bucket->total_size;
        shown_files += bucket->file_count;
    }
    const unsigned long long other_subdir_files =
        minus(minus(folder->total_files, folder->direct_files), shown_files);
    if (other_subdir_files > 0) {
        write_share_row(fp, kSubdirColumns, minus(minus(folder->total_size, folder->direct_size),
                                                shown_size),
                        folder->total_size, other_subdir_files, "(other subfolders)");
    }

    /* Largest file types, plus the remainder */
    write_md_section(fp, "Largest file types");
    write_md_table_begin(fp, kExtensionColumns, _countof(kExtensionColumns));
    shown_size  = 0;
    shown_files = 0;
    for (size_t i = 0; i < folder->ext_count && i < RESTIC_REPORT_TOP_EXTENSIONS; ++i) {
        const ResticReportExtension* bucket = &folder->extensions[i];
        write_share_row(fp, kExtensionColumns, bucket->total_size, folder->total_size,
                        bucket->file_count, bucket->ext);
        shown_size += bucket->total_size;
        shown_files += bucket->file_count;
    }
    if (folder->total_files > shown_files) {
        write_share_row(fp, kExtensionColumns, minus(folder->total_size, shown_size),
                        folder->total_size, folder->total_files - shown_files, "(other)");
    }

    /* File size distribution */
    write_md_section(fp, "File size distribution");
    write_md_table_begin(fp, kHistogramColumns, _countof(kHistogramColumns));
    for (int i = 0; i < RESTIC_REPORT_HIST_BUCKETS; ++i) {
        const double share = percentage_of(folder->hist_files[i], folder->total_files);
        char files_text[32];
        char percent_text[32];
        char bar[BAR_WIDTH + 4];
        restic_report_format_bytes(folder->hist_bytes[i], size_text, sizeof(size_text));
        restic_report_format_uint(folder->hist_files[i], files_text, sizeof(files_text));
        (void)snprintf(percent_text, sizeof(percent_text), "%.1f%%", share);
        restic_report_format_bar(bar, sizeof(bar), share / 100.0, BAR_WIDTH);

        const char* cells[5] = {kHistogramLabels[i], files_text, percent_text, size_text, bar};
        write_md_row(fp, kHistogramColumns, _countof(kHistogramColumns), cells);
    }

    /* Largest files in this folder */
    write_md_section(fp, "Largest files");
    if (folder->largest_count == 0) {
        fputs("No files in this folder.\n", fp);
    } else {
        write_md_table_begin(fp, kLargestColumns, _countof(kLargestColumns));
        for (size_t i = 0; i < folder->largest_count; ++i) {
            const ResticReportLargestFile* file = &folder->largest[i];
            char rank[16];
            (void)snprintf(rank, sizeof(rank), "%llu", (unsigned long long)(i + 1));
            restic_report_format_bytes(file->size, size_text, sizeof(size_text));
            const char* cells[4] = {rank, size_text,
                                    file->mtime[0] != '\0' ? file->mtime : "(unknown)",
                                    file->rel_path};
            write_md_row(fp, kLargestColumns, _countof(kLargestColumns), cells);
        }
    }
    fputs("\n", fp);
}

/* ------------------------------------------------------------------------------------------ */
/* Opening the report                                                                          */
/* ------------------------------------------------------------------------------------------ */

/* Maximum number of characters taken from the leaf folder name */
#define REPORT_LEAF_CHARS 32

/* Hook used by the tests instead of spawning a Lister window */
static ResticReportListerHook g_ListerHook = NULL;

void restic_report_set_lister_test_hook(ResticReportListerHook hook) {
    g_ListerHook = hook;
}

/* Appends at most `max_chars` characters of `text`, replacing what Windows rejects in a file name */
static void append_sanitized(WCHAR* out, size_t capacity, size_t* pos, const WCHAR* text,
                             size_t max_chars) {
    if (text == NULL) return;
    size_t added = 0;
    for (size_t i = 0; text[i] != L'\0' && added < max_chars; ++i) {
        WCHAR c = text[i];
        if (c < 0x20 || wcschr(L"<>:\"/\\|?*", c) != NULL) c = L'_';
        if (*pos + 1 >= capacity) return;
        out[(*pos)++] = c;
        ++added;
    }
    out[*pos] = L'\0';
}

BOOL restic_report_build_temp_path(const WCHAR* repository_alias, const char* snapshot_id,
                                   const WCHAR* subpath, WCHAR* out_path, size_t out_capacity) {
    if (out_path == NULL || out_capacity == 0) return FALSE;
    out_path[0] = L'\0';
    if (repository_alias == NULL || repository_alias[0] == L'\0') return FALSE;

    const DWORD temp_length = GetTempPathW((DWORD)out_capacity, out_path);
    if (temp_length == 0 || temp_length >= out_capacity) {
        out_path[0] = L'\0';
        return FALSE;
    }

    /* GetTempPathW always ends with a separator */
    size_t pos = temp_length;
    append_sanitized(out_path, out_capacity, &pos, RESTIC_REPORT_TEMP_PREFIX, 64);
    append_sanitized(out_path, out_capacity, &pos, repository_alias, 40);

    if (snapshot_id != NULL && snapshot_id[0] != '\0') {
        WCHAR short_id[RESTIC_STATS_SHORT_ID_MAX];
        size_t i = 0;
        for (; snapshot_id[i] != '\0' && i + 1 < _countof(short_id); ++i) {
            short_id[i] = (WCHAR)snapshot_id[i];
        }
        short_id[i] = L'\0';
        append_sanitized(out_path, out_capacity, &pos, L"_", 1);
        append_sanitized(out_path, out_capacity, &pos, short_id, 16);
    }

    if (subpath != NULL) {
        const WCHAR* leaf = subpath;
        for (const WCHAR* p = subpath; *p != L'\0'; ++p) {
            if (*p == L'\\' || *p == L'/') leaf = p + 1;
        }
        if (leaf[0] != L'\0') {
            append_sanitized(out_path, out_capacity, &pos, L"_", 1);
            append_sanitized(out_path, out_capacity, &pos, leaf, REPORT_LEAF_CHARS);
        }
    }

    if (pos + 4 >= out_capacity) {
        out_path[0] = L'\0';
        return FALSE;
    }
    wcscpy_s(out_path + pos, out_capacity - pos, L".md");
    return TRUE;
}

/* FALSE only when another process holds `path` open in a way that forbids replacing it */
static BOOL destination_replaceable(const WCHAR* path) {
    HANDLE file = CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        return TRUE;
    }
    return GetLastError() != ERROR_SHARING_VIOLATION; /* Missing file or other errors: just try */
}

BOOL restic_report_pick_destination(WCHAR* path, size_t capacity) {
    if (path == NULL || capacity == 0 || path[0] == L'\0') return FALSE;
    if (destination_replaceable(path)) return TRUE;

    const size_t length = wcslen(path);
    const size_t stem   = (length > 3 && _wcsicmp(path + length - 3, L".md") == 0) ? length - 3 : length;
    WCHAR candidate[REPORT_MAX_PATH_UNITS + 16];
    for (int n = 2; n <= 9; ++n) {
        if (stem + 8 > _countof(candidate) || stem + 8 > capacity) return FALSE;
        if (swprintf_s(candidate, _countof(candidate), L"%.*ls (%d).md", (int)stem, path, n) < 0) {
            return FALSE;
        }
        if (destination_replaceable(candidate)) {
            wcscpy_s(path, capacity, candidate);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL restic_report_open_in_lister(const WCHAR* local_report_path) {
    if (local_report_path == NULL || local_report_path[0] == L'\0') return FALSE;
    if (g_ListerHook != NULL) return g_ListerHook(local_report_path);

    /* Automated tests and scripted checks must never pop up a window */
    WCHAR flag[8] = {0};
    if (GetEnvironmentVariableW(L"RESTIC_WFX_NO_LISTER_GUI", flag, _countof(flag)) > 0) {
        wfx_trace_printf("lister suppressed by RESTIC_WFX_NO_LISTER_GUI");
        return TRUE;
    }

    /* Total Commander sets %COMMANDER_EXE%; inside the plugin process the host module path is the
       same executable, so it is a reliable fallback */
    WCHAR tc_exe[MAX_PATH] = {0};
    const DWORD length     = GetEnvironmentVariableW(L"COMMANDER_EXE", tc_exe, _countof(tc_exe));
    if (length == 0 || length >= _countof(tc_exe)) {
        if (GetModuleFileNameW(NULL, tc_exe, _countof(tc_exe)) == 0) return FALSE;
    }

    const size_t cmd_chars = wcslen(tc_exe) + wcslen(local_report_path) + 16;
    WCHAR* cmd_line        = (WCHAR*)malloc(cmd_chars * sizeof(WCHAR));
    if (cmd_line == NULL) return FALSE;
    if (swprintf_s(cmd_line, cmd_chars, L"\"%ls\" /S=L \"%ls\"", tc_exe, local_report_path) < 0) {
        free(cmd_line);
        return FALSE;
    }

    /* /S=L is the Total Commander internal command that opens a file in its Lister */
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb      = sizeof(si);
    const BOOL ok = CreateProcessW(NULL, cmd_line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        wfx_trace_printf("lister launch failed error=%lu", (unsigned long)GetLastError());
    }
    free(cmd_line);
    return ok;
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
    WCHAR alias_w[MAX_PATH];              /* Alias as Total Commander spells it (cache key) */
    char alias[REPORT_ALIAS_BYTES];       /* UTF-8 copy used by the Markdown writers */
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

/* Lifetime of a cached report: after that the report is regenerated */
#define REPORT_CACHE_TTL_MS 60000
/* A generated report is a few tens of KB; anything much larger is not cached */
#define REPORT_CACHE_MAX_BYTES (4u * 1024u * 1024u)

/* Writes an already generated document to `destination` (cache hit path: no restic.exe runs) */
static ResticResult write_report_text(const char* text, size_t length, const WCHAR* destination) {
    WCHAR temp[REPORT_MAX_PATH_UNITS + 64] = {0};
    FILE* fp                               = open_report_file(destination, temp, _countof(temp));
    if (fp == NULL) return RESTIC_ERR_LOCAL_WRITE;
    if (length > 0 && fwrite(text, 1, length, fp) != length) {
        fclose(fp);
        DeleteFileW(temp);
        return RESTIC_ERR_LOCAL_WRITE;
    }
    return publish_report(fp, temp, destination);
}

/* Reads a published report back into memory so that a repeated request needs no other process */
static BOOL read_report_text(const WCHAR* path, char** out_text, size_t* out_length) {
    *out_text   = NULL;
    *out_length = 0;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER size;
    BOOL ok      = GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
                   size.QuadPart <= REPORT_CACHE_MAX_BYTES;
    char* buffer = NULL;
    if (ok) {
        buffer = (char*)malloc((size_t)size.QuadPart + 1);
        ok     = (buffer != NULL);
    }
    if (ok) {
        DWORD got = 0;
        ok = ReadFile(file, buffer, (DWORD)size.QuadPart, &got, NULL) &&
             got == (DWORD)size.QuadPart;
        if (ok) {
            buffer[got] = '\0';
            *out_text   = buffer;
            *out_length = got;
        }
    }
    if (!ok) free(buffer);
    CloseHandle(file);
    return ok;
}

/*
 * One-slot cache of the last generated report.
 *
 * Pressing F3 twice, or Alt+Enter right after F3, would otherwise re-run every Restic query: with
 * the cache the document is written straight to the destination. The key is (repository target,
 * snapshot id, subfolder); an entry older than the TTL is ignored, so the report can never
 * describe a repository state that is more than a minute old.
 */
typedef struct {
    BOOL valid;
    WCHAR repo_target[REPORT_MAX_PATH_UNITS];
    char snapshot_id[RESTIC_STATS_ID_MAX]; /* Empty for the repository report */
    WCHAR subpath[REPORT_MAX_PATH_UNITS];  /* No leading separator */
    ULONGLONG stamp;
    char* text;
    size_t length;
} ReportCacheSlot;

static ReportCacheSlot g_ReportCache;
static SRWLOCK g_ReportCacheLock = SRWLOCK_INIT;

/* Copies the cached document into a heap buffer. FALSE on miss, expiry or allocation failure. */
static BOOL report_cache_lookup(const WCHAR* repo_target, const char* snapshot_id,
                                const WCHAR* subpath, char** out_text, size_t* out_length) {
    *out_text   = NULL;
    *out_length = 0;

    BOOL hit = FALSE;
    AcquireSRWLockShared(&g_ReportCacheLock);
    if (g_ReportCache.valid && g_ReportCache.text != NULL &&
        GetTickCount64() - g_ReportCache.stamp <= REPORT_CACHE_TTL_MS &&
        _wcsicmp(g_ReportCache.repo_target, repo_target) == 0 &&
        _stricmp(g_ReportCache.snapshot_id, snapshot_id) == 0 &&
        _wcsicmp(g_ReportCache.subpath, subpath) == 0) {
        char* copy = (char*)malloc(g_ReportCache.length + 1);
        if (copy != NULL) {
            memcpy(copy, g_ReportCache.text, g_ReportCache.length);
            copy[g_ReportCache.length] = '\0';
            *out_text                  = copy;
            *out_length                = g_ReportCache.length;
            hit                        = TRUE;
        }
    }
    ReleaseSRWLockShared(&g_ReportCacheLock);
    return hit;
}

/* Replaces the cached document with a heap copy of `text` */
static void report_cache_store(const WCHAR* repo_target, const char* snapshot_id,
                               const WCHAR* subpath, const char* text, size_t length) {
    if (length == 0 || length > REPORT_CACHE_MAX_BYTES) return;
    char* copy = (char*)malloc(length + 1);
    if (copy == NULL) return;
    memcpy(copy, text, length);
    copy[length] = '\0';

    AcquireSRWLockExclusive(&g_ReportCacheLock);
    free(g_ReportCache.text);
    g_ReportCache.text   = copy;
    g_ReportCache.length = length;
    wcsncpy_s(g_ReportCache.repo_target, _countof(g_ReportCache.repo_target), repo_target,
              _TRUNCATE);
    strncpy_s(g_ReportCache.snapshot_id, sizeof(g_ReportCache.snapshot_id), snapshot_id,
              _TRUNCATE);
    wcsncpy_s(g_ReportCache.subpath, _countof(g_ReportCache.subpath), subpath, _TRUNCATE);
    g_ReportCache.stamp = GetTickCount64();
    g_ReportCache.valid = TRUE;
    ReleaseSRWLockExclusive(&g_ReportCacheLock);
}

void restic_report_clear_cache(void) {
    AcquireSRWLockExclusive(&g_ReportCacheLock);
    free(g_ReportCache.text);
    memset(&g_ReportCache, 0, sizeof(g_ReportCache));
    ReleaseSRWLockExclusive(&g_ReportCacheLock);
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

/*
 * Returns the most recent snapshot that carries a summary block. Its `total_bytes_processed` is the
 * logical size a full restore would produce, i.e. exactly what `stats --mode restore-size` would
 * spend a whole tree scan to recompute.
 */
static const ResticSnapshotInfo* latest_snapshot_with_summary(const ResticSnapshotInfo* items,
                                                              size_t count) {
    if (items == NULL) return NULL;
    for (size_t index = count; index > 0; --index) {
        if (items[index - 1].has_summary) return &items[index - 1];
    }
    return NULL;
}

/* Fills `storage` from the summary block of a snapshot: no restic.exe process is needed */
static void restore_storage_from_summary(const ResticSnapshotInfo* info,
                                         ResticReportStorage* storage) {
    memset(storage, 0, sizeof(*storage));
    if (info == NULL || !info->has_summary) return;
    storage->valid            = TRUE;
    storage->total_size       = info->total_bytes_processed;
    storage->total_file_count = info->total_files_processed;
}

/* ------------------------------------------------------------------------------------------ */
/* Level 1: repository report                                                                  */
/* ------------------------------------------------------------------------------------------ */

static ResticResult generate_repo_report(ReportContext* ctx, const WCHAR* destination,
                                         WCHAR* err, size_t err_capacity) {
    ResticSnapshotInfo* snapshots = NULL;
    size_t snapshot_count         = 0;
    BOOL from_cache               = FALSE;
    ResticReportRepo report;
    WCHAR temp[REPORT_MAX_PATH_UNITS + 64] = {0};
    FILE* fp = NULL;
    memset(&report, 0, sizeof(report));

    /* The statistics cache normally holds the snapshot list already (the panel listing filled it):
       reusing it saves a whole restic snapshots process. The copy is optimistic, a concurrent
       invalidate simply falls back to the query below. */
    size_t cached_count = 0;
    if (snapshot_stats_lookup_alias(ctx->alias_w, NULL, 0, &cached_count) && cached_count > 0) {
        snapshots = (ResticSnapshotInfo*)calloc(cached_count, sizeof(ResticSnapshotInfo));
        if (snapshots != NULL &&
            snapshot_stats_lookup_alias(ctx->alias_w, snapshots, cached_count, &snapshot_count)) {
            from_cache = TRUE;
        } else {
            free(snapshots);
            snapshots      = NULL;
            snapshot_count = 0;
        }
    }

    ResticResult res =
        (from_cache)
            ? advance(ctx, RESTIC_OK, 25)
            : advance(ctx, load_snapshots(ctx, NULL, &snapshots, &snapshot_count, err,
                                          err_capacity), 25);
    if (res == RESTIC_OK && !from_cache && snapshot_count > 0) {
        (void)snapshot_stats_store(ctx->alias_w, snapshots, snapshot_count); /* Warm the columns */
    }
    if (res == RESTIC_OK) {
        /* Only a repository whose snapshots were all written without a summary needs the scan */
        restore_storage_from_summary(latest_snapshot_with_summary(snapshots, snapshot_count),
                                     &report.restore);
        res = advance(ctx,
                      report.restore.valid ? RESTIC_OK
                                           : read_storage_stats(ctx, L"restore-size", NULL,
                                                                &report.restore, err, err_capacity),
                      55);
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
        restic_report_write_repo_md(fp, &report);
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
    ResticSnapshotInfo cached_info;
    size_t loaded_count        = 0;
    BOOL from_cache            = FALSE;
    char note[256]             = {0};
    WCHAR temp[REPORT_MAX_PATH_UNITS + 64] = {0};
    FILE* fp = NULL;
    ResticReportSnapshot report;
    ResticReportDiff* diff = (ResticReportDiff*)calloc(1, sizeof(*diff)); /* Too big for the stack */
    memset(&report, 0, sizeof(report));
    memset(&cached_info, 0, sizeof(cached_info));
    MultiByteToWideChar(CP_UTF8, 0, snapshot_id, -1, snapshot_w, (int)_countof(snapshot_w));

    /* The metadata of a browsed snapshot is already cached: skip `snapshots --json <id>` */
    from_cache = snapshot_stats_lookup(ctx->alias_w, snapshot_id, &cached_info);

    ResticResult res = (diff != NULL) ? RESTIC_OK : RESTIC_ERR_OUT_OF_MEMORY;
    if (res == RESTIC_OK) {
        if (from_cache) {
            report.info = &cached_info;
            res         = advance(ctx, RESTIC_OK, 20);
        } else {
            res = advance(ctx, load_snapshots(ctx, snapshot_w, &loaded, &loaded_count, err,
                                              err_capacity), 20);
            if (res == RESTIC_OK) {
                report.info = restic_find_snapshot_by_short_id(loaded, loaded_count, snapshot_id);
                if (report.info == NULL) res = RESTIC_ERR_PATH_NOT_FOUND; /* Restic printed "[]" */
            }
        }
    }
    if (res == RESTIC_OK) {
        /* A snapshot with a summary needs no tree scan: the restore size is already in the record */
        restore_storage_from_summary(report.info, &report.restore);
        res = advance(ctx,
                      report.restore.valid
                          ? RESTIC_OK
                          : read_storage_stats(ctx, L"restore-size", snapshot_w, &report.restore,
                                               err, err_capacity),
                      45);
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
        restic_report_write_snapshot_md(fp, &report);
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
    ULONGLONG last_progress_tick; /* Progress is sent at most every 150 ms (see below) */
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

    /*
     * The total number of nodes is unknown in advance, so the percentage ramps up to 85 % with the
     * number of lines. Updating Total Commander on every line would flood the dialog, and updating
     * only every N lines (as the first version did) either sends a burst of identical values or
     * stops updating at all on a huge folder. The clock is therefore checked every 256 lines and
     * the bar advances at most every 150 ms.
     */
    if ((scan->line_count & 0xFFULL) == 0) {
        const ULONGLONG now = GetTickCount64();
        if (now - scan->last_progress_tick >= 150ULL) {
            scan->last_progress_tick = now;
            int percent              = 15 + (int)(scan->line_count / 500ULL);
            if (percent > 85) percent = 85;
            if (report_step(scan->ctx, percent)) return FALSE; /* ESC pressed */
        }
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
        restic_report_write_subtree_md(fp, &report);
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
    wcsncpy_s(ctx.alias_w, _countof(ctx.alias_w), repository_alias, _TRUNCATE);
    wide_to_utf8(repository_alias, ctx.alias, sizeof(ctx.alias));
    wide_to_utf8(repo_target, ctx.target, sizeof(ctx.target));

    /* Cache key: the subpath without its leading separators, so "\\dir" and "dir" are one report */
    const WCHAR* sub = (subpath_w != NULL) ? subpath_w : L"";
    while (*sub == L'\\') ++sub;
    char id_key[RESTIC_STATS_ID_MAX] = {0};
    if (snapshot_id != NULL) strncpy_s(id_key, sizeof(id_key), snapshot_id, _TRUNCATE);

    if (report_step(&ctx, 5)) return RESTIC_ERR_CANCELLED;

    /* A repeated request (F3 twice, Alt+Enter after F3) is answered from memory, without restic.exe */
    char* cached         = NULL;
    size_t cached_length = 0;
    if (report_cache_lookup(repo_target, id_key, sub, &cached, &cached_length)) {
        ResticResult cached_res = write_report_text(cached, cached_length, local_dest_path);
        free(cached);
        if (cached_res == RESTIC_OK) return RESTIC_OK;
    }

    ResticResult res;
    if (snapshot_id == NULL) {
        res = generate_repo_report(&ctx, local_dest_path, out_error_msg, err_capacity);
    } else if (*sub == L'\0') {
        res = generate_snapshot_report(&ctx, snapshot_id, local_dest_path, out_error_msg,
                                       err_capacity);
    } else {
        res = generate_subtree_report(&ctx, snapshot_id, subpath_w, local_dest_path,
                                      out_error_msg, err_capacity);
    }

    if (res == RESTIC_OK) {
        char* text    = NULL;
        size_t length = 0;
        if (read_report_text(local_dest_path, &text, &length)) {
            report_cache_store(repo_target, id_key, sub, text, length);
            free(text);
        }
    }
    return res;
}
