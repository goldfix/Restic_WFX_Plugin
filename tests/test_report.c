/*
 * Unit and integration tests for the Markdown statistics report:
 *  - restic_report_format_*       (bars, byte/count/timestamp formatting, table text)
 *  - wfx_status_* / FsStatusInfo* (operation filter that hides the virtual file)
 *  - restic_report_parse_*        (`restic stats` JSON, `restic diff` NDJSON)
 *  - restic_report_aggregate_ls_line / finalize (`restic ls --recursive` aggregation)
 *  - the three Markdown writers and the virtual directory entry
 *  - end-to-end generation with the real restic.exe (skipped when tmp\restic.exe is missing)
 */
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
#include "fsplugin.h"
#include "restic_cli.h"
#include "restic_report.h"
#include "restic_stats.h"
#include "wfx_path.h"
#include "wfx_status.h"

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define EXPECT_TRUE(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        printf("[FAIL] %s:%d: %s\n", __FUNCTION__, __LINE__, (msg)); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

#define EXPECT_STR_EQ(actual, expected, msg) do { \
    g_tests_run++; \
    if (strcmp((actual), (expected)) != 0) { \
        printf("[FAIL] %s:%d: %s (expected '%s', got '%s')\n", \
               __FUNCTION__, __LINE__, (msg), (expected), (actual)); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

#define EXPECT_WSTR_EQ(actual, expected, msg) do { \
    g_tests_run++; \
    if (wcscmp((actual), (expected)) != 0) { \
        printf("[FAIL] %s:%d: %s (expected '%ls', got '%ls')\n", \
               __FUNCTION__, __LINE__, (msg), (expected), (actual)); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

#define EXPECT_U64_EQ(actual, expected, msg) do { \
    g_tests_run++; \
    if ((unsigned long long)(actual) != (unsigned long long)(expected)) { \
        printf("[FAIL] %s:%d: %s (expected %llu, got %llu)\n", \
               __FUNCTION__, __LINE__, (msg), \
               (unsigned long long)(expected), (unsigned long long)(actual)); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

/* ------------------------------------------------------------------------------------------ */
/* Helpers                                                                                     */
/* ------------------------------------------------------------------------------------------ */

static size_t count_occurrences(const char* text, const char* needle) {
    size_t count = 0;
    size_t len   = strlen(needle);
    for (const char* p = text; (p = strstr(p, needle)) != NULL; p += len) {
        count++;
    }
    return count;
}

/* Reads a whole file into a heap string (NULL when it cannot be opened) */
static char* read_file_utf8(const WCHAR* path) {
    FILE* fp = NULL;
    if (_wfopen_s(&fp, path, L"rb") != 0 || fp == NULL) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    char* buffer = (char*)calloc((size_t)size + 1, 1);
    if (buffer != NULL && size > 0) {
        size_t got = fread(buffer, 1, (size_t)size, fp);
        buffer[got] = '\0';
    }
    fclose(fp);
    return buffer;
}

/* Simple recursive delete used by the live fixture */
static void remove_tree(const WCHAR* path) {
    WIN32_FIND_DATAW fd;
    WCHAR pattern[MAX_PATH * 2];
    swprintf_s(pattern, _countof(pattern), L"%ls\\*", path);
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            WCHAR child[MAX_PATH * 2];
            swprintf_s(child, _countof(child), L"%ls\\%ls", path, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                remove_tree(child);
            } else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path);
}

static BOOL write_text_file(const WCHAR* path, const char* content, size_t length) {
    FILE* fp = NULL;
    if (_wfopen_s(&fp, path, L"wb") != 0 || fp == NULL) return FALSE;
    size_t written = (length > 0) ? fwrite(content, 1, length, fp) : 0;
    fclose(fp);
    return written == length;
}

/* ------------------------------------------------------------------------------------------ */
/* Formatting                                                                                  */
/* ------------------------------------------------------------------------------------------ */

static void test_ascii_bar_formatting(void) {
    char bar[256];

    restic_report_format_bar(bar, sizeof(bar), 0.0, 10);
    EXPECT_TRUE(bar[0] == '[' && bar[strlen(bar) - 1] == ']', "A bar is enclosed in brackets");
    EXPECT_U64_EQ(count_occurrences(bar, "#"), 0, "0 % shows no filled block");
    EXPECT_U64_EQ(count_occurrences(bar, "."), 10, "0 % shows ten empty blocks");
    EXPECT_STR_EQ(bar, "[..........]", "Only ASCII characters are used");

    restic_report_format_bar(bar, sizeof(bar), 0.001, 10);
    EXPECT_U64_EQ(count_occurrences(bar, "#"), 1, "A tiny ratio still shows one block");

    restic_report_format_bar(bar, sizeof(bar), 0.5, 10);
    EXPECT_U64_EQ(count_occurrences(bar, "#"), 5, "Half full bar has half the blocks");
    EXPECT_STR_EQ(bar, "[#####.....]", "Half bar rendering");

    restic_report_format_bar(bar, sizeof(bar), 1.0, 10);
    EXPECT_U64_EQ(count_occurrences(bar, "#"), 10, "100 % fills the bar");

    restic_report_format_bar(bar, sizeof(bar), -1.0, 10);
    EXPECT_U64_EQ(count_occurrences(bar, "#"), 0, "A negative ratio is clamped to zero");

    restic_report_format_bar(bar, sizeof(bar), 2.5, 10);
    EXPECT_U64_EQ(count_occurrences(bar, "#"), 10, "A ratio above 1 is clamped");

    /* Degenerate parameters must never overrun or produce an unterminated string */
    restic_report_format_bar(bar, sizeof(bar), 0.5, 0);
    EXPECT_TRUE(strlen(bar) > 0, "A zero width still produces a valid bar");
    /* The bar never exceeds 40 blocks plus the brackets, whatever the buffer */
    restic_report_format_bar(bar, sizeof(bar), 0.5, 1000);
    EXPECT_TRUE(strlen(bar) == 42, "The width is capped to 40 blocks");
    restic_report_format_bar(bar, 3, 0.5, 10);
    EXPECT_TRUE(strlen(bar) < 3, "A tiny buffer stays within bounds");
    restic_report_format_bar(NULL, 0, 0.5, 10);
    g_tests_run++; /* NULL arguments are accepted */
}

static void test_number_and_time_formatting(void) {
    char text[64];

    restic_report_format_bytes(0, text, sizeof(text));
    EXPECT_STR_EQ(text, "0 B", "Zero bytes");
    restic_report_format_bytes(999, text, sizeof(text));
    EXPECT_STR_EQ(text, "999 B", "Bytes below 1 KB stay in bytes");
    restic_report_format_bytes(1536, text, sizeof(text));
    EXPECT_STR_EQ(text, "1.50 KB", "1536 bytes are 1.50 KB");
    restic_report_format_bytes(5ULL * 1024ULL * 1024ULL * 1024ULL, text, sizeof(text));
    EXPECT_STR_EQ(text, "5.00 GB", "GiB is scaled to GB");

    restic_report_format_uint(0, text, sizeof(text));
    EXPECT_STR_EQ(text, "0", "Zero");
    restic_report_format_uint(999, text, sizeof(text));
    EXPECT_STR_EQ(text, "999", "No separator below 1000");
    restic_report_format_uint(1234567, text, sizeof(text));
    EXPECT_STR_EQ(text, "1,234,567", "Thousands separators");
    restic_report_format_uint(9223372036854775807ULL, text, sizeof(text));
    EXPECT_STR_EQ(text, "9,223,372,036,854,775,807", "Large counters");

    SYSTEMTIME st;
    memset(&st, 0, sizeof(st));
    st.wYear = 2026;
    st.wMonth = 10;
    st.wDay = 18;
    st.wHour = 16;
    st.wMinute = 28;
    st.wSecond = 15;
    FILETIME ft;
    EXPECT_TRUE(SystemTimeToFileTime(&st, &ft) != 0, "Build a FILETIME");
    EXPECT_TRUE(restic_report_format_time(&ft, text, sizeof(text)) != FALSE, "Format a valid time");
    EXPECT_STR_EQ(text, "2026-10-18 16:28:15", "Time format is YYYY-MM-DD HH:MM:SS");

    FILETIME unknown;
    FS_SET_TIME_UNKNOWN(unknown);
    EXPECT_TRUE(restic_report_format_time(&unknown, text, sizeof(text)) == FALSE,
                "The unknown-time marker is not formatted");
    EXPECT_TRUE(restic_report_format_time(NULL, text, sizeof(text)) == FALSE, "NULL time");
}

static void test_markdown_text_escaping(void) {
    FILE* fp = NULL;
    EXPECT_TRUE(tmpfile_s(&fp) == 0 && fp != NULL, "Open a scratch file");

    restic_report_write_text(fp, "a|b\r\n\tc");
    restic_report_write_text(fp, "<a href=\"x\">&'");
    restic_report_write_text(fp, NULL);
    fflush(fp);
    rewind(fp);

    char buffer[128] = {0};
    size_t got       = fread(buffer, 1, sizeof(buffer) - 1, fp);
    buffer[got]      = '\0';
    fclose(fp);

    /* A pipe would start a new table column, a control character would end the row; markup
       characters are no longer special, so paths stay readable in the Lister */
    EXPECT_STR_EQ(buffer, "a/b   c<a href=\"x\">&'",
                  "Pipes become '/', control characters become spaces, markup is kept");
}

/* ------------------------------------------------------------------------------------------ */
/* Operation filter (FsStatusInfo)                                                             */
/* ------------------------------------------------------------------------------------------ */

static void test_status_info_f3_detection(void) {
    wfx_status_reset();
    EXPECT_U64_EQ(wfx_status_current_operation(), 0, "No operation on the stack");
    EXPECT_TRUE(wfx_status_consume_f3_calcsize() == FALSE,
                "A report is never requested without a CALCSIZE operation");
    EXPECT_TRUE(wfx_status_is_f3_calcsize_aborted() == FALSE, "Nothing was aborted");

    /* A listing or a copy never triggers the report */
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_START, FS_STATUS_OP_LIST);
    EXPECT_TRUE(wfx_status_consume_f3_calcsize() == FALSE, "A panel listing is not F3");
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_END, FS_STATUS_OP_LIST);
    EXPECT_TRUE(wfx_status_current_operation() == 0, "The operation stack is emptied");

    /* Space bar: a CALCSIZE without F3 must keep calculating the folder size */
    wfx_status_force_f3_next_calcsize(FALSE);
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_START, FS_STATUS_OP_CALCSIZE);
    EXPECT_TRUE(wfx_status_consume_f3_calcsize() == FALSE, "Space bar is not intercepted");
    EXPECT_TRUE(wfx_status_is_f3_calcsize_aborted() == FALSE, "The folder size may be calculated");
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_END, FS_STATUS_OP_CALCSIZE);

    /* F3: the report is requested exactly once per CALCSIZE scope */
    wfx_status_force_f3_next_calcsize(TRUE);
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_START, FS_STATUS_OP_CALCSIZE);
    EXPECT_TRUE(wfx_status_is_f3_calcsize_aborted() == FALSE, "Not aborted before the first query");
    EXPECT_TRUE(wfx_status_consume_f3_calcsize() != FALSE, "F3 is detected on the first query");
    EXPECT_TRUE(wfx_status_is_f3_calcsize_aborted() != FALSE,
                "The rest of the CALCSIZE scope answers \"no files\"");
    EXPECT_TRUE(wfx_status_consume_f3_calcsize() == FALSE, "Only one report per F3");
    EXPECT_TRUE(wfx_status_is_f3_calcsize_aborted() != FALSE, "Still aborted for the whole scope");
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_END, FS_STATUS_OP_CALCSIZE);
    EXPECT_TRUE(wfx_status_is_f3_calcsize_aborted() == FALSE,
                "FS_STATUS_END forgets the request");

    /* The force flag applies to the next CALCSIZE only */
    wfx_status_force_f3_next_calcsize(TRUE);
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_START, FS_STATUS_OP_CALCSIZE);
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_END, FS_STATUS_OP_CALCSIZE);
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_START, FS_STATUS_OP_CALCSIZE);
    EXPECT_TRUE(wfx_status_consume_f3_calcsize() == FALSE, "The force flag is consumed once");
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_END, FS_STATUS_OP_CALCSIZE);

    /* Nested operations keep their own CALCSIZE state */
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_START, FS_STATUS_OP_LIST);
    wfx_status_force_f3_next_calcsize(TRUE);
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_START, FS_STATUS_OP_CALCSIZE);
    EXPECT_TRUE(wfx_status_consume_f3_calcsize() != FALSE, "Nested F3 request");
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_END, FS_STATUS_OP_CALCSIZE);
    EXPECT_TRUE(wfx_status_consume_f3_calcsize() == FALSE, "Ending the inner scope clears it");
    FsStatusInfoW((WCHAR*)L"\\Repo", FS_STATUS_END, FS_STATUS_OP_LIST);
    EXPECT_U64_EQ(wfx_status_current_operation(), 0, "Nested stack unwinds");

    /* The ANSI entry point shares the same per-thread state */
    wfx_status_force_f3_next_calcsize(TRUE);
    FsStatusInfo((char*)"\\Repo", FS_STATUS_START, FS_STATUS_OP_CALCSIZE);
    EXPECT_TRUE(wfx_status_consume_f3_calcsize() != FALSE, "The ANSI entry point detects F3 too");
    FsStatusInfo((char*)"\\Repo", FS_STATUS_END, FS_STATUS_OP_CALCSIZE);

    /* Unbalanced starts must not overflow the stack, unbalanced ends must not underflow it */
    for (int i = 0; i < 32; ++i) {
        FsStatusInfoW(NULL, FS_STATUS_START, FS_STATUS_OP_LIST);
    }
    EXPECT_U64_EQ(wfx_status_current_operation(), FS_STATUS_OP_LIST, "A saturated stack still answers");
    for (int i = 0; i < 64; ++i) {
        FsStatusInfoW(NULL, FS_STATUS_END, FS_STATUS_OP_LIST);
    }
    EXPECT_U64_EQ(wfx_status_current_operation(), 0, "Extra ends are ignored");

    /* A START that does not fit the stack is dropped as a whole and must not arm F3 */
    wfx_status_reset();
    for (int i = 0; i < WFX_STATUS_STACK_MAX; ++i) {
        FsStatusInfoW(NULL, FS_STATUS_START, FS_STATUS_OP_LIST);
    }
    wfx_status_force_f3_next_calcsize(TRUE);
    FsStatusInfoW(NULL, FS_STATUS_START, FS_STATUS_OP_CALCSIZE);
    EXPECT_TRUE(wfx_status_consume_f3_calcsize() == FALSE &&
                    wfx_status_current_operation() == FS_STATUS_OP_LIST,
                "A dropped CALCSIZE start never requests a report");

    /* ...and it must not leave a request behind for a later space bar on the same thread (the test
       helper is cleared by hand: the dropped notification never reached it) */
    wfx_status_force_f3_next_calcsize(FALSE);
    for (int i = 0; i < WFX_STATUS_STACK_MAX; ++i) {
        FsStatusInfoW(NULL, FS_STATUS_END, FS_STATUS_OP_LIST);
    }
    FsStatusInfoW(NULL, FS_STATUS_START, FS_STATUS_OP_CALCSIZE); /* Space bar: no F3 held */
    EXPECT_TRUE(wfx_status_consume_f3_calcsize() == FALSE,
                "A request dropped with a full stack does not surface in a later CALCSIZE");
    FsStatusInfoW(NULL, FS_STATUS_END, FS_STATUS_OP_CALCSIZE);

    wfx_status_force_f3_next_calcsize(TRUE);
    wfx_status_reset();
    EXPECT_TRUE(wfx_status_consume_f3_calcsize() == FALSE, "The helper drops the pending request");
}

/* ------------------------------------------------------------------------------------------ */
/* Parsers                                                                                     */
/* ------------------------------------------------------------------------------------------ */

static void test_storage_json_parsing(void) {
    ResticReportStorage storage;

    const char* restore =
        "{\"total_size\":19648291020,\"total_file_count\":148290,\"snapshots_count\":12}";
    EXPECT_TRUE(restic_report_parse_storage_json(restore, &storage) != FALSE,
                "restore-size payload is accepted");
    EXPECT_U64_EQ(storage.total_size, 19648291020ULL, "Logical size");
    EXPECT_U64_EQ(storage.total_file_count, 148290ULL, "File count");

    const char* raw =
        "{\"total_size\":642190120,\"total_uncompressed_size\":1520192810,"
        "\"compression_ratio\":2.3672,\"compression_progress\":100,"
        "\"compression_space_saving\":57.75,\"total_blob_count\":24190}";
    EXPECT_TRUE(restic_report_parse_storage_json(raw, &storage) != FALSE, "raw-data payload");
    EXPECT_U64_EQ(storage.total_size, 642190120ULL, "Stored size");
    EXPECT_U64_EQ(storage.uncompressed_size, 1520192810ULL, "Uncompressed size");
    EXPECT_U64_EQ(storage.blob_count, 24190ULL, "Blob count");
    EXPECT_TRUE(storage.compression_ratio > 2.36 && storage.compression_ratio < 2.37,
                "Compression ratio is read");
    EXPECT_TRUE(storage.space_saving_percent > 57.7 && storage.space_saving_percent < 57.8,
                "Space saving is read");

    /* Repository format v1 prints no compression fields at all */
    const char* v1 = "{\"total_size\":12345,\"total_blob_count\":7}";
    EXPECT_TRUE(restic_report_parse_storage_json(v1, &storage) != FALSE, "v1 payload is accepted");
    EXPECT_U64_EQ(storage.uncompressed_size, 12345ULL, "Missing uncompressed size falls back");
    EXPECT_TRUE(storage.compression_ratio == 1.0, "Missing ratio falls back to 1.0");

    EXPECT_TRUE(restic_report_parse_storage_json("not json", &storage) == FALSE, "Garbage rejected");
    EXPECT_TRUE(restic_report_parse_storage_json("[]", &storage) == FALSE, "An array is rejected");
    EXPECT_TRUE(restic_report_parse_storage_json(NULL, &storage) == FALSE, "NULL input rejected");
    EXPECT_TRUE(restic_report_parse_storage_json("{}", NULL) == FALSE, "NULL output rejected");
}

static void test_diff_parsing(void) {
    static ResticReportDiff diff; /* Large: keep it out of the stack */
    memset(&diff, 0, sizeof(diff));

    const char* lines[] = {
        "{\"message_type\":\"change\",\"path\":\"/D/data/new.3dm\",\"modifier\":\"+\"}",
        "{\"message_type\":\"change\",\"path\":\"/D/data/old.jpg\",\"modifier\":\"-\"}",
        "{\"message_type\":\"change\",\"path\":\"/D/data/render.png\",\"modifier\":\"M\"}",
        "{\"message_type\":\"statistics\",\"source_snapshot\":\"aaaa\",\"target_snapshot\":\"bbbb\","
        "\"changed_files\":38,\"added\":{\"files\":142,\"dirs\":12,\"others\":0,"
        "\"data_blobs\":312,\"tree_blobs\":24,\"bytes\":44145020},"
        "\"removed\":{\"files\":19,\"dirs\":2,\"others\":0,\"data_blobs\":45,"
        "\"tree_blobs\":8,\"bytes\":8860416}}",
    };
    for (size_t i = 0; i < _countof(lines); ++i) {
        restic_report_parse_diff_line(&diff, lines[i], strlen(lines[i]));
    }

    EXPECT_TRUE(diff.valid != FALSE, "The statistics line enables the diff");
    EXPECT_U64_EQ(diff.changed_files, 38, "Changed files");
    EXPECT_U64_EQ(diff.added_files, 142, "Added files");
    EXPECT_U64_EQ(diff.added_dirs, 12, "Added dirs");
    EXPECT_U64_EQ(diff.added_bytes, 44145020ULL, "Added bytes");
    EXPECT_U64_EQ(diff.added_blobs, 312, "Added data blobs");
    EXPECT_U64_EQ(diff.removed_files, 19, "Removed files");
    EXPECT_U64_EQ(diff.removed_bytes, 8860416ULL, "Removed bytes");
    EXPECT_U64_EQ(diff.change_count, 3, "Three changes are kept");
    EXPECT_U64_EQ(diff.change_events, 3, "Three change events");
    EXPECT_STR_EQ(diff.changes[0].modifier, "+", "First modifier");
    EXPECT_STR_EQ(diff.changes[0].path, "/D/data/new.3dm", "First path");
    EXPECT_STR_EQ(diff.changes[2].modifier, "M", "Last modifier");

    /* Non-JSON lines, unknown kinds and missing fields are ignored */
    restic_report_parse_diff_line(&diff, "garbage", 7);
    restic_report_parse_diff_line(&diff, "{\"message_type\":\"other\"}", 24);
    restic_report_parse_diff_line(&diff, NULL, 0);
    EXPECT_U64_EQ(diff.change_count, 3, "Unrelated lines add nothing");

    /* The change list is capped, but every event is counted */
    memset(&diff, 0, sizeof(diff));
    for (int i = 0; i < 150; ++i) {
        char line[128];
        (void)snprintf(line, sizeof(line),
                       "{\"message_type\":\"change\",\"path\":\"/f%03d\",\"modifier\":\"M\"}", i);
        restic_report_parse_diff_line(&diff, line, strlen(line));
    }
    EXPECT_U64_EQ(diff.change_count, RESTIC_REPORT_MAX_DIFF_CHANGES, "The list is capped");
    EXPECT_U64_EQ(diff.change_events, 150, "All events are counted");
    EXPECT_STR_EQ(diff.changes[0].path, "/f000", "The first changes are kept");
}

static char* render_to_string(void (*writer)(FILE*, const void*), const void* payload);
static void write_repo_adapter(FILE* fp, const void* payload);
static void write_subtree_adapter(FILE* fp, const void* payload);
static void make_snapshot_info(ResticSnapshotInfo* info, const char* short_id, const char* host,
                               unsigned long long files, unsigned long long added);

/* Feeds one NDJSON line to the folder aggregator */
static void feed_folder_line(ResticReportFolder* folder, const char* filter, const char* line) {
    restic_report_aggregate_ls_line(folder, filter, line, strlen(line));
}

static void test_folder_aggregation(void) {
    static ResticReportFolder folder; /* Large: keep it out of the stack */
    memset(&folder, 0, sizeof(folder));
    const char* filter = "/D/data";

    feed_folder_line(&folder, filter,
                     "{\"name\":\"data\",\"type\":\"dir\",\"path\":\"/D/data\"}");
    feed_folder_line(&folder, filter,
                     "{\"name\":\"a.txt\",\"type\":\"file\",\"size\":100,"
                     "\"path\":\"/D/data/a.txt\",\"mtime\":\"2026-10-17T18:12:04+02:00\"}");
    feed_folder_line(&folder, filter,
                     "{\"name\":\"zero.bin\",\"type\":\"file\",\"size\":0,"
                     "\"path\":\"/D/data/zero.bin\",\"mtime\":\"2026-10-18T09:00:00Z\"}");
    feed_folder_line(&folder, filter,
                     "{\"name\":\"big.3dm\",\"type\":\"file\",\"size\":12000000,"
                     "\"path\":\"/D/data/big.3dm\",\"mtime\":\"2026-10-19T10:30:00Z\"}");
    feed_folder_line(&folder, filter,
                     "{\"name\":\"sub\",\"type\":\"dir\",\"path\":\"/D/data/sub\"}");
    feed_folder_line(&folder, filter,
                     "{\"name\":\"b.psd\",\"type\":\"file\",\"size\":5000,"
                     "\"path\":\"/D/data/sub/b.psd\",\"mtime\":\"2026-10-16T08:00:00Z\"}");
    feed_folder_line(&folder, filter,
                     "{\"name\":\"deep\",\"type\":\"dir\",\"path\":\"/D/data/sub/deep\"}");
    feed_folder_line(&folder, filter,
                     "{\"name\":\"c.dat\",\"type\":\"file\",\"size\":20000,"
                     "\"path\":\"/D/data/sub/deep/c.dat\",\"mtime\":\"2026-10-20T12:00:00Z\"}");
    /* Sibling with a shared prefix and a line of another kind must be ignored */
    feed_folder_line(&folder, filter,
                     "{\"name\":\"data2\",\"type\":\"file\",\"size\":777,"
                     "\"path\":\"/D/data2/ghost.txt\"}");
    feed_folder_line(&folder, filter, "{\"message_type\":\"snapshot\",\"struct_type\":\"snapshot\"}");
    feed_folder_line(&folder, filter, "garbage");

    EXPECT_U64_EQ(folder.total_files, 5, "Five files are counted");
    EXPECT_U64_EQ(folder.direct_files, 3, "Three files are directly in the folder");
    EXPECT_U64_EQ(folder.total_dirs, 2, "Two directories are counted");
    EXPECT_U64_EQ(folder.direct_dirs, 1, "One directory is a direct child");
    EXPECT_U64_EQ(folder.total_size, 12025100ULL, "Total size sums every file");
    EXPECT_STR_EQ(folder.oldest_mtime, "2026-10-16 08:00:00", "Oldest modification time (UTC)");
    EXPECT_STR_EQ(folder.newest_mtime, "2026-10-20 12:00:00", "Newest modification time (UTC)");

    /* The histogram covers every file exactly once */
    unsigned long long hist_files = 0;
    unsigned long long hist_bytes = 0;
    for (int i = 0; i < RESTIC_REPORT_HIST_BUCKETS; ++i) {
        hist_files += folder.hist_files[i];
        hist_bytes += folder.hist_bytes[i];
    }
    EXPECT_U64_EQ(hist_files, folder.total_files, "Histogram file counts add up");
    EXPECT_U64_EQ(hist_bytes, folder.total_size, "Histogram bytes add up");
    EXPECT_U64_EQ(folder.hist_files[0], 1, "One empty file");
    EXPECT_U64_EQ(folder.hist_files[1], 2, "Two files below 10 KB");
    EXPECT_U64_EQ(folder.hist_files[2], 1, "One file below 100 KB");
    EXPECT_U64_EQ(folder.hist_files[5], 1, "One file above 10 MB");

    restic_report_finalize_folder(&folder);

    EXPECT_U64_EQ(folder.subdir_count, 1, "Only one direct subfolder is tracked");
    EXPECT_STR_EQ(folder.subdirs[0].name, "sub", "Subfolder name");
    EXPECT_U64_EQ(folder.subdirs[0].total_size, 25000, "Subfolder size includes nested files");
    EXPECT_U64_EQ(folder.subdirs[0].file_count, 2, "Subfolder file count");

    EXPECT_U64_EQ(folder.ext_count, 5, "Five distinct extensions");
    EXPECT_STR_EQ(folder.extensions[0].ext, ".3dm", "Largest extension first");
    EXPECT_U64_EQ(folder.extensions[0].total_size, 12000000ULL, "Largest extension size");
    EXPECT_STR_EQ(folder.extensions[1].ext, ".dat", "Second extension by size");
    EXPECT_STR_EQ(folder.extensions[4].ext, ".bin", "Smallest extension last");

    EXPECT_U64_EQ(folder.largest_count, 5, "Every file fits in the ranking");
    EXPECT_U64_EQ(folder.largest[0].size, 12000000ULL, "Largest file first");
    EXPECT_STR_EQ(folder.largest[0].rel_path, "big.3dm", "Direct file path");
    EXPECT_STR_EQ(folder.largest[0].mtime, "2026-10-19 10:30:00", "Largest file timestamp");
    EXPECT_STR_EQ(folder.largest[1].rel_path, "sub\\deep\\c.dat",
                  "Nested paths use Windows separators");
    EXPECT_STR_EQ(folder.largest[4].rel_path, "zero.bin", "The empty file is the smallest entry");

    /* With filter "/" the first path segment is the file name */
    static ResticReportFolder root;
    memset(&root, 0, sizeof(root));
    feed_folder_line(&root, "/",
                     "{\"name\":\"x\",\"type\":\"file\",\"size\":10,\"path\":\"/x\"}");
    feed_folder_line(&root, "/", "{\"name\":\"\",\"type\":\"file\",\"size\":1,\"path\":\"/\"}");
    EXPECT_U64_EQ(root.total_files, 1, "A lone slash is not a file");
    EXPECT_STR_EQ(root.largest[0].rel_path, "x", "Root level file name");

    /* The ranking keeps only the 15 largest files, still sorted */
    static ResticReportFolder ranked;
    memset(&ranked, 0, sizeof(ranked));
    for (int i = 1; i <= 20; ++i) {
        char line[192];
        (void)snprintf(line, sizeof(line),
                       "{\"name\":\"f%d\",\"type\":\"file\",\"size\":%d,"
                       "\"path\":\"/R/f%d\"}",
                       i, i * 10, i);
        feed_folder_line(&ranked, "/R", line);
    }
    restic_report_finalize_folder(&ranked);
    EXPECT_U64_EQ(ranked.largest_count, RESTIC_REPORT_TOP_FILES, "The ranking is capped at 15 entries");
    EXPECT_STR_EQ(ranked.largest[0].rel_path, "f20", "The largest file stays first");
    EXPECT_U64_EQ(ranked.largest[0].size, 200, "Its size is stored");
    EXPECT_STR_EQ(ranked.largest[14].rel_path, "f6", "The 15th entry is the smallest kept");
}


/* Finds an extension bucket by name (NULL when absent) */
static const ResticReportExtension* find_extension(const ResticReportFolder* folder, const char* ext) {
    for (size_t i = 0; i < folder->ext_count; ++i) {
        if (strcmp(folder->extensions[i].ext, ext) == 0) return &folder->extensions[i];
    }
    return NULL;
}

static void feed_file(ResticReportFolder* folder, const char* filter, const char* relative,
                      unsigned long long size) {
    char line[3072];
    const char* base = strrchr(relative, '/');
    (void)snprintf(line, sizeof(line),
                   "{\"name\":\"%s\",\"type\":\"file\",\"size\":%llu,\"path\":\"%s/%s\"}",
                   base ? base + 1 : relative, size, filter, relative);
    feed_folder_line(folder, filter, line);
}

static void test_folder_extensions_and_truncation(void) {
    static ResticReportFolder folder;
    memset(&folder, 0, sizeof(folder));

    /* Empty subfolders are listed, and the filtered folder itself is remembered */
    feed_folder_line(&folder, "/R", "{\"name\":\"R\",\"type\":\"dir\",\"path\":\"/R\"}");
    feed_folder_line(&folder, "/R", "{\"name\":\"emptydir\",\"type\":\"dir\",\"path\":\"/R/emptydir\"}");
    EXPECT_TRUE(folder.root_seen != FALSE, "The filtered folder node is recognised");
    EXPECT_U64_EQ(folder.subdir_count, 1, "An empty direct subfolder gets a bucket");
    EXPECT_U64_EQ(folder.subdirs[0].file_count, 0, "...with no files");

    /* Extension rules: lower case, last dot only, hidden files and trailing dots have none */
    feed_file(&folder, "/R", ".gitignore", 1);
    feed_file(&folder, "/R", "Photo.PSD", 1);
    feed_file(&folder, "/R", "x.", 1);
    feed_file(&folder, "/R", "noext", 1);
    feed_file(&folder, "/R", "a.tar.gz", 1);
    feed_file(&folder, "/R", "deep/dir/Report.PDF", 1);
    EXPECT_TRUE(find_extension(&folder, "(no ext)") != NULL &&
                    find_extension(&folder, "(no ext)")->file_count == 3,
                ".gitignore, \"x.\" and a name without dot have no extension");
    EXPECT_TRUE(find_extension(&folder, ".psd") != NULL, "Extensions are lower-cased");
    EXPECT_TRUE(find_extension(&folder, ".gz") != NULL && find_extension(&folder, ".tar.gz") == NULL,
                "Only the last extension counts");
    EXPECT_TRUE(find_extension(&folder, ".pdf") != NULL, "A nested file uses its own name");
    EXPECT_TRUE(find_extension(&folder, ".gitignore") == NULL, "A leading dot is not an extension");

    /* A long path is cut on a character boundary, never in the middle of a UTF-8 sequence */
    char relative[1500] = "ab";
    for (int i = 0; i < 400; ++i) strcat_s(relative, sizeof(relative), "\xE6\x97\xA5");
    feed_file(&folder, "/R", relative, 999999);
    EXPECT_U64_EQ(folder.largest[0].size, 999999, "The long path is ranked first");
    EXPECT_TRUE(strlen(folder.largest[0].rel_path) < RESTIC_REPORT_PATH_MAX,
                "The stored path fits its buffer");
    EXPECT_TRUE(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, folder.largest[0].rel_path, -1,
                                    NULL, 0) > 0,
                "The truncated path is still valid UTF-8");
}

/* A filter that names a file must not be mistaken for a folder (F3 / Alt+Enter on a file) */
static void test_folder_root_must_be_a_directory(void) {
    static ResticReportFolder folder;
    memset(&folder, 0, sizeof(folder));

    /* Restic prints the node itself when the filter names a file */
    feed_folder_line(&folder, "/D/readme.txt",
                     "{\"name\":\"readme.txt\",\"type\":\"file\",\"size\":5,"
                     "\"path\":\"/D/readme.txt\"}");
    EXPECT_TRUE(folder.root_seen == FALSE, "A file named by the filter is not a folder");
    EXPECT_U64_EQ(folder.total_files, 0, "It is not counted as a child of itself either");
    EXPECT_U64_EQ(folder.total_size, 0, "No size is attributed to a folder that does not exist");

    /* The same filter as a directory is the expected case */
    feed_folder_line(&folder, "/D/data", "{\"name\":\"data\",\"type\":\"dir\",\"path\":\"/D/data\"}");
    EXPECT_TRUE(folder.root_seen != FALSE, "A directory named by the filter is recognised");

    /* A node without a type is treated as a directory, as Restic always sends one */
    static ResticReportFolder untyped;
    memset(&untyped, 0, sizeof(untyped));
    feed_folder_line(&untyped, "/X", "{\"name\":\"X\",\"path\":\"/X\"}");
    EXPECT_TRUE(untyped.root_seen != FALSE, "A node without a type still marks the root");
}

/* The "(other ...)" rows make every table add up to the folder total, whatever the cut-off */
static void test_folder_remainder_rows(void) {
    static ResticReportFolder folder;
    memset(&folder, 0, sizeof(folder));

    /* 20 files in 20 subfolders with 20 different extensions; sizes 20000, 19000, ... 1000 */
    for (int i = 1; i <= 20; ++i) {
        char relative[64];
        (void)snprintf(relative, sizeof(relative), "s%02d/f.e%02d", i, i);
        feed_file(&folder, "/R", relative, (unsigned long long)(21 - i) * 1000ULL);
    }
    restic_report_finalize_folder(&folder);

    ResticReportSubtree subtree;
    memset(&subtree, 0, sizeof(subtree));
    subtree.alias        = "RepoA";
    subtree.display_path = "\\R";
    subtree.folder       = &folder;
    char* md           = render_to_string(write_subtree_adapter, &subtree);
    EXPECT_TRUE(md != NULL, "The folder report is written");

    /* 15 subfolders are shown, the other 5 hold 5000+4000+3000+2000+1000 bytes */
    EXPECT_TRUE(strstr(md, "(other subfolders)") != NULL && strstr(md, "14.65 KB") != NULL,
                "Subfolders beyond the top 15 are summed up");
    /* 12 extensions are shown, the other 8 hold 8000+7000+...+1000 bytes */
    EXPECT_TRUE(strstr(md, "(other)") != NULL && strstr(md, "35.16 KB") != NULL,
                "Extensions beyond the top 12 are summed up");
    EXPECT_U64_EQ(count_occurrences(md, "(files in this folder)"), 0,
                  "No row for files that do not exist directly in the folder");
    free(md);
}

static void test_location_redaction(void) {
    char out[128];

    restic_report_redact_location("rest:https://user:p%40ss@host:8000/repo", out, sizeof(out));
    EXPECT_STR_EQ(out, "rest:https://***@host:8000/repo", "URL credentials are masked");
    restic_report_redact_location("rest:http://a@b@host/repo", out, sizeof(out));
    EXPECT_STR_EQ(out, "rest:http://***@host/repo", "Everything before the last @ of the authority");
    restic_report_redact_location("rest:http://host/path@dir", out, sizeof(out));
    EXPECT_STR_EQ(out, "rest:http://host/path@dir", "An @ in the path is not a credential");
    restic_report_redact_location("C:\\backup\\repo", out, sizeof(out));
    EXPECT_STR_EQ(out, "C:\\backup\\repo", "A local path is unchanged");
    restic_report_redact_location("s3:https://s3.amazonaws.com/bucket", out, sizeof(out));
    EXPECT_STR_EQ(out, "s3:https://s3.amazonaws.com/bucket", "A URL without credentials is unchanged");
    restic_report_redact_location("sftp:user@host:/srv/restic", out, sizeof(out));
    EXPECT_STR_EQ(out, "sftp:user@host:/srv/restic", "Only the user name, no password: unchanged");
    restic_report_redact_location(NULL, out, sizeof(out));
    EXPECT_STR_EQ(out, "", "NULL gives an empty string");
    restic_report_redact_location("rest:https://user:secret@host/repo", out, 12);
    EXPECT_TRUE(strlen(out) < 12 && strstr(out, "secret") == NULL, "A small buffer stays in bounds");

    /* The report files can be shared: no writer may let the password through */
    static ResticSnapshotInfo snapshot;
    make_snapshot_info(&snapshot, "aaaa1111", "host", 1, 1);
    ResticReportRepo repo;
    memset(&repo, 0, sizeof(repo));
    repo.alias          = "Remote";
    repo.repo_target    = "rest:https://user:secret@host/repo";
    repo.snapshots      = &snapshot;
    repo.snapshot_count = 1;
    char* md          = render_to_string(write_repo_adapter, &repo);
    EXPECT_TRUE(md != NULL && strstr(md, "secret") == NULL && strstr(md, "rest:https://***@host/repo") != NULL,
                "The repository report masks the password");
    free(md);

    static ResticReportFolder folder;
    memset(&folder, 0, sizeof(folder));
    ResticReportSubtree subtree;
    memset(&subtree, 0, sizeof(subtree));
    subtree.alias       = "Remote";
    subtree.repo_target = "rest:https://user:secret@host/repo";
    subtree.folder      = &folder;
    md                = render_to_string(write_subtree_adapter, &subtree);
    EXPECT_TRUE(md != NULL && strstr(md, "secret") == NULL, "The folder report masks the password");
    free(md);
}

/* ------------------------------------------------------------------------------------------ */
/* Markdown writers                                                                            */
/* ------------------------------------------------------------------------------------------ */

/* Fills a snapshot record used by the writer tests */
static void make_snapshot_info(ResticSnapshotInfo* info, const char* short_id, const char* host,
                               unsigned long long files, unsigned long long added) {
    memset(info, 0, sizeof(*info));
    (void)snprintf(info->id, sizeof(info->id),
                   "%s%s", short_id, "0123456789abcdef0123456789abcdef0123456789abcdef01234567");
    strncpy_s(info->short_id, sizeof(info->short_id), short_id, _TRUNCATE);
    strncpy_s(info->hostname, sizeof(info->hostname), host, _TRUNCATE);
    strncpy_s(info->username, sizeof(info->username), "user", _TRUNCATE);
    strncpy_s(info->program_version, sizeof(info->program_version), "restic 0.19.1", _TRUNCATE);
    strncpy_s(info->paths, sizeof(info->paths), "D:\\data; \\\\host\\share", _TRUNCATE);
    strncpy_s(info->tags, sizeof(info->tags), "daily, <b>tag</b>", _TRUNCATE);
    info->time.dwLowDateTime  = 0;
    info->time.dwHighDateTime = 0;
    info->has_summary         = TRUE;
    info->files_new           = 120;
    info->files_changed       = 5;
    info->files_unmodified    = 900;
    info->dirs_new            = 12;
    info->dirs_changed        = 1;
    info->dirs_unmodified     = 40;
    info->data_blobs          = 300;
    info->tree_blobs          = 25;
    info->data_added          = added + 1000;
    info->data_added_packed   = added;
    info->total_files_processed = files;
    info->total_bytes_processed = files * 2048ULL;
}

/* Renders a report into a temporary file and returns the heap string with its content */
static char* render_to_string(void (*writer)(FILE*, const void*), const void* payload) {
    WCHAR path[MAX_PATH];
    if (GetTempPathW(MAX_PATH, path) == 0) return NULL;
    wcscat_s(path, MAX_PATH, L"~rp_report_test.md");
    DeleteFileW(path);

    FILE* fp = NULL;
    if (_wfopen_s(&fp, path, L"wb") != 0 || fp == NULL) return NULL;
    writer(fp, payload);
    fclose(fp);

    char* content = read_file_utf8(path);
    DeleteFileW(path);
    return content;
}

/* Adapters with the exact signature the writers expect */
static void write_repo_adapter(FILE* fp, const void* payload) {
    restic_report_write_repo_md(fp, (const ResticReportRepo*)payload);
}
static void write_snapshot_adapter(FILE* fp, const void* payload) {
    restic_report_write_snapshot_md(fp, (const ResticReportSnapshot*)payload);
}
static void write_subtree_adapter(FILE* fp, const void* payload) {
    restic_report_write_subtree_md(fp, (const ResticReportSubtree*)payload);
}

static void test_markdown_report_generation(void) {
    static ResticSnapshotInfo snapshots[3];
    make_snapshot_info(&snapshots[0], "aaaa1111", "host<A>", 500, 1000);
    make_snapshot_info(&snapshots[1], "bbbb2222", "host&B", 700, 4000000);
    make_snapshot_info(&snapshots[2], "cccc3333", "legacy", 0, 0);
    snapshots[2].has_summary = FALSE; /* Written by a Restic version without a summary */

    ResticReportRepo repo;
    memset(&repo, 0, sizeof(repo));
    repo.alias               = "Repo<&>\"'";
    repo.repo_target         = "D:\\backup<1>";
    repo.snapshots           = snapshots;
    repo.snapshot_count      = _countof(snapshots);
    repo.restore.valid       = TRUE;
    repo.restore.total_size  = 19648291020ULL;
    repo.restore.total_file_count = 148290;
    repo.stored.valid        = TRUE;
    repo.stored.total_size   = 642190120ULL;
    repo.stored.uncompressed_size = 1520192810ULL;
    repo.stored.compression_ratio = 2.3672;
    repo.stored.space_saving_percent = 57.75;
    repo.stored.blob_count   = 24190;

    char* md = render_to_string(write_repo_adapter, &repo);
    EXPECT_TRUE(md != NULL, "The repository report is written");
    EXPECT_TRUE(strncmp(md, "# Restic repository report ", 26) == 0,
                "A Markdown document starts with its level-1 headline");
    EXPECT_TRUE(strstr(md, "<!DOCTYPE") == NULL && strstr(md, "<pre>") == NULL &&
                    strstr(md, "&lt;") == NULL && strstr(md, "&amp;") == NULL,
                "No HTML tag or entity is left in the document");
    EXPECT_TRUE(strstr(md, "| Property") != NULL && strstr(md, "| :---") != NULL,
                "Tables use the pipe syntax with an alignment row");
    EXPECT_TRUE(strstr(md, "## Storage and deduplication") != NULL, "Storage section present");
    EXPECT_TRUE(strstr(md, "Deduplication factor") != NULL, "Deduplication factor is computed");
    EXPECT_TRUE(strstr(md, "## Snapshot history") != NULL, "History section present");
    EXPECT_TRUE(strstr(md, "bbbb2222") != NULL && strstr(md, "aaaa1111") != NULL &&
                    strstr(md, "cccc3333") != NULL,
                "Every snapshot is listed");
    EXPECT_TRUE(strstr(md, "1000.00 KB") != NULL, "The history shows the size of each snapshot");
    EXPECT_TRUE(strstr(md, "Repo<&>\"'") != NULL, "The alias is written verbatim (no entity)");
    EXPECT_TRUE(strstr(md, "D:\\backup<1>") != NULL, "The repository path is written verbatim");
    EXPECT_TRUE(strstr(md, "18.30 GB") != NULL, "Sizes are human readable");
    EXPECT_TRUE(count_occurrences(md, "#") > 0 && count_occurrences(md, "[") > 0,
                "Growth bars are drawn with ASCII blocks");
    EXPECT_TRUE(strstr(md, "\xE2\x96\x88") == NULL && strstr(md, "\xE2\x96\x91") == NULL,
                "No Unicode block character survives");
    free(md);

    static ResticReportDiff diff;
    memset(&diff, 0, sizeof(diff));
    ResticReportSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.alias            = "RepoA";
    snapshot.repo_target      = "D:\\backup";
    snapshot.info             = &snapshots[1];
    snapshot.restore.valid    = TRUE;
    snapshot.restore.total_size = 170340;
    snapshot.stored.valid     = TRUE;
    snapshot.stored.total_size = 9316;
    snapshot.stored.uncompressed_size = 399014;
    snapshot.stored.compression_ratio = 42.8;
    snapshot.stored.space_saving_percent = 97.66;
    snapshot.diff               = &diff;
    diff.valid                  = TRUE;
    strncpy_s(diff.parent_short_id, sizeof(diff.parent_short_id), "aaaa1111",
              _TRUNCATE);
    diff.changed_files          = 38;
    diff.added_files            = 142;
    diff.added_dirs             = 12;
    diff.added_bytes            = 44145020;
    diff.removed_files          = 19;
    diff.removed_bytes          = 8860416;
    diff.change_count           = 2;
    diff.change_events          = 2;
    strncpy_s(diff.changes[0].modifier, sizeof(diff.changes[0].modifier), "+",
              _TRUNCATE);
    strncpy_s(diff.changes[0].path, sizeof(diff.changes[0].path),
              "/D/<script>alert(1)</script>", _TRUNCATE);
    strncpy_s(diff.changes[1].modifier, sizeof(diff.changes[1].modifier), "-",
              _TRUNCATE);
    strncpy_s(diff.changes[1].path, sizeof(diff.changes[1].path), "/D/old.jpg",
              _TRUNCATE);

    md = render_to_string(write_snapshot_adapter, &snapshot);
    EXPECT_TRUE(md != NULL, "The snapshot report is written");
    EXPECT_TRUE(strncmp(md, "# Restic snapshot report ", 24) == 0, "Snapshot headline present");
    EXPECT_TRUE(strstr(md, "## Files and directories") != NULL, "Breakdown section present");
    EXPECT_TRUE(strstr(md, "## Changes since the parent snapshot") != NULL,
                "Diff section present");
    EXPECT_TRUE(strstr(md, "aaaa1111") != NULL, "The parent ID is shown");
    EXPECT_TRUE(strstr(md, "```text") != NULL && strstr(md, "[+] /D/<script>alert(1)</script>") != NULL,
                "Added paths are listed in a fenced block, verbatim");
    EXPECT_TRUE(strstr(md, "[-] /D/old.jpg") != NULL, "Removed paths are marked");
    EXPECT_TRUE(strstr(md, "42.10 MB (142 files, 12 dirs, 0 blobs)") != NULL,
                "Added bytes and item counts are on one line");
    EXPECT_TRUE(strstr(md, "daily, <b>tag</b>") != NULL, "Tags are written verbatim");
    free(md);

    /* A snapshot without a parent, and one whose diff failed */
    snapshot.info               = &snapshots[0];
    diff.valid                  = FALSE;
    snapshot.diff_note          = "Initial snapshot: there is no parent snapshot to compare with.";
    md = render_to_string(write_snapshot_adapter, &snapshot);
    EXPECT_TRUE(md != NULL && strstr(md, "Initial snapshot") != NULL,
                "The missing parent is explained");
    free(md);

    static ResticReportFolder folder;
    memset(&folder, 0, sizeof(folder));
    folder.total_size   = 1000;
    folder.total_files  = 3;
    folder.direct_files = 1;
    folder.direct_size  = 250;
    folder.total_dirs   = 1;
    folder.direct_dirs  = 1;
    strncpy_s(folder.oldest_mtime, sizeof(folder.oldest_mtime), "2026-10-01 10:00:00", _TRUNCATE);
    strncpy_s(folder.newest_mtime, sizeof(folder.newest_mtime), "2026-10-20 11:00:00", _TRUNCATE);
    folder.subdir_count = 1;
    strncpy_s(folder.subdirs[0].name, sizeof(folder.subdirs[0].name), "sub<dir>", _TRUNCATE);
    folder.subdirs[0].total_size = 750;
    folder.subdirs[0].file_count = 2;
    folder.ext_count = 1;
    strncpy_s(folder.extensions[0].ext, sizeof(folder.extensions[0].ext), ".3dm", _TRUNCATE);
    folder.extensions[0].total_size = 750;
    folder.extensions[0].file_count = 2;
    folder.hist_files[1] = 2;
    folder.hist_bytes[1] = 250;
    folder.hist_files[5] = 1;
    folder.hist_bytes[5] = 750;
    folder.largest_count = 1;
    strncpy_s(folder.largest[0].rel_path, sizeof(folder.largest[0].rel_path), "sub\\big<1>.3dm",
              _TRUNCATE);
    strncpy_s(folder.largest[0].mtime, sizeof(folder.largest[0].mtime), "2026-10-20 11:00:00",
              _TRUNCATE);
    folder.largest[0].size = 750;

    ResticReportSubtree subtree;
    memset(&subtree, 0, sizeof(subtree));
    subtree.alias             = "RepoA";
    subtree.repo_target       = "D:\\backup";
    subtree.snapshot_short_id = "bbbb2222";
    subtree.snapshot_time     = "2026-10-18 16:28:15";
    subtree.hostname          = "host";
    subtree.display_path      = "\\RepoA\\2026-10-18_16-28-15_[bbbb2222]\\D\\data";
    subtree.folder            = &folder;

    md = render_to_string(write_subtree_adapter, &subtree);
    EXPECT_TRUE(md != NULL, "The folder report is written");
    EXPECT_TRUE(strncmp(md, "# Restic folder report ", 22) == 0, "Folder headline present");
    EXPECT_TRUE(strstr(md, "## Size by subfolder") != NULL, "Subfolder section present");
    EXPECT_TRUE(strstr(md, "## Largest file types") != NULL, "Extensions section present");
    EXPECT_TRUE(strstr(md, "## File size distribution") != NULL && strstr(md, "1 B - 10 KB") != NULL,
                "Histogram section present");
    EXPECT_TRUE(strstr(md, "## Largest files") != NULL, "Largest files section present");
    EXPECT_TRUE(strstr(md, "sub<dir>") != NULL, "Subfolder names are written verbatim");
    EXPECT_TRUE(strstr(md, "sub\\big<1>.3dm") != NULL,
                "File names use Windows separators and are written verbatim");
    EXPECT_TRUE(strstr(md, "files in this folder") != NULL, "Direct file bytes are reported");
    EXPECT_TRUE(strstr(md, "Modification span") != NULL, "The mtime span is shown");
    EXPECT_TRUE(count_occurrences(md, "#") > 0, "At least one bar block is filled");
    free(md);

    /* Invalid arguments must not crash or write anything */
    restic_report_write_repo_md(NULL, &repo);
    restic_report_write_snapshot_md(NULL, &snapshot);
    restic_report_write_subtree_md(NULL, &subtree);
    restic_report_write_snapshot_md(stdout, NULL);
    g_tests_run++; /* Reaching this point is the assertion */
}

/* ------------------------------------------------------------------------------------------ */
/* Report path and Lister opening                                                              */
/* ------------------------------------------------------------------------------------------ */

/* Records the path handed to the Lister by restic_report_open_in_lister() */
static WCHAR g_lister_path[MAX_PATH] = {0};
static int g_lister_calls             = 0;

static BOOL lister_hook(const WCHAR* path) {
    g_lister_calls++;
    wcsncpy_s(g_lister_path, _countof(g_lister_path), path, _TRUNCATE);
    return TRUE;
}

static BOOL failing_lister_hook(const WCHAR* path) {
    (void)path;
    return FALSE;
}

/* Removes the file name part of a path, leaving the trailing separator ("D:\tmp\") */
static void temp_directory_of(const WCHAR* path, WCHAR* out, size_t capacity) {
    size_t keep = 0;
    for (size_t i = 0; path[i] != L'\0'; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') keep = i + 1;
    }
    if (keep + 1 > capacity) keep = 0;
    wmemcpy(out, path, keep);
    out[keep] = L'\0';
}

static void test_report_temp_path(void) {
    WCHAR temp[MAX_PATH];
    EXPECT_TRUE(GetTempPathW(MAX_PATH, temp) > 0, "The temporary directory is known");

    WCHAR path[MAX_PATH];
    EXPECT_TRUE(restic_report_build_temp_path(L"BackupLocale", NULL, NULL, path, _countof(path)) != FALSE,
                "A repository report path is built");
    WCHAR expected[MAX_PATH];
    swprintf_s(expected, _countof(expected), L"%lsrestic_stats_BackupLocale.md", temp);
    EXPECT_WSTR_EQ(path, expected, "Repository: %TEMP%\\restic_stats_<alias>.md");

    EXPECT_TRUE(restic_report_build_temp_path(L"BackupLocale", "5e395062", L"\\D\\data", path,
                                              _countof(path)) != FALSE,
                "A folder report path is built");
    swprintf_s(expected, _countof(expected), L"%lsrestic_stats_BackupLocale_5e395062_data.md", temp);
    EXPECT_WSTR_EQ(path, expected, "Folder: %TEMP%\\restic_stats_<alias>_<id>_<leaf>.md");

    EXPECT_TRUE(restic_report_build_temp_path(L"BackupLocale", "5e395062", NULL, path,
                                              _countof(path)) != FALSE,
                "A snapshot report path is built");
    swprintf_s(expected, _countof(expected), L"%lsrestic_stats_BackupLocale_5e395062.md", temp);
    EXPECT_WSTR_EQ(path, expected, "Snapshot: %TEMP%\\restic_stats_<alias>_<id>.md");

    /* Characters that Windows rejects in a file name are replaced, the leaf is the last segment */
    EXPECT_TRUE(restic_report_build_temp_path(L"My:Repo|?", "5e395062", L"\\a<b>\\c/d:e*f", path,
                                              _countof(path)) != FALSE,
                "An exotic alias still produces a path");
    swprintf_s(expected, _countof(expected), L"%lsrestic_stats_My_Repo___5e395062_d_e_f.md", temp);
    EXPECT_WSTR_EQ(path, expected, "Forbidden characters become underscores");

    /* Long names are cut, the result stays well inside MAX_PATH */
    char long_id[120];
    memset(long_id, 'a', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = '\0';
    EXPECT_TRUE(restic_report_build_temp_path(L"A-Very-Long-Alias-Name-That-Goes-On-And-On",
                                              long_id, L"\\sub", path, _countof(path)) != FALSE,
                "A long alias is accepted");
    EXPECT_TRUE(wcslen(path) < 120, "The generated name is bounded");
    EXPECT_TRUE(wcschr(path, L'/') == NULL, "Path separators inside names are replaced");

    WCHAR tiny[8] = {0};
    EXPECT_TRUE(restic_report_build_temp_path(L"BackupLocale", NULL, NULL, tiny, _countof(tiny)) == FALSE,
                "A buffer that is too small is refused");
    EXPECT_TRUE(restic_report_build_temp_path(NULL, NULL, NULL, path, _countof(path)) == FALSE,
                "A NULL alias is refused");
    EXPECT_TRUE(restic_report_build_temp_path(L"", NULL, NULL, path, _countof(path)) == FALSE,
                "An empty alias is refused");
    EXPECT_TRUE(restic_report_build_temp_path(L"Repo", NULL, NULL, NULL, 0) == FALSE,
                "A NULL buffer is refused");
}

/* Another process (an open Lister) holding an earlier report must not break the next one */
static void test_pick_destination(void) {
    WCHAR path[MAX_PATH];
    EXPECT_TRUE(restic_report_build_temp_path(L"PickRepo", "aa11bb22", NULL, path, _countof(path)) != FALSE,
                "Build a report path");
    WCHAR original[MAX_PATH];
    wcscpy_s(original, _countof(original), path);
    DeleteFileW(path);

    WCHAR probe[MAX_PATH];
    wcscpy_s(probe, _countof(probe), path);
    EXPECT_TRUE(restic_report_pick_destination(probe, _countof(probe)) != FALSE &&
                    wcscmp(probe, original) == 0,
                "A missing file keeps its name");

    /* An open reader without FILE_SHARE_DELETE is what a Lister may look like */
    EXPECT_TRUE(write_text_file(path, "first", 5) != FALSE, "Create the earlier report");
    HANDLE reader = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    EXPECT_TRUE(reader != INVALID_HANDLE_VALUE, "Hold the earlier report open");
    wcscpy_s(probe, _countof(probe), path);
    EXPECT_TRUE(restic_report_pick_destination(probe, _countof(probe)) != FALSE, "A free name is found");
    WCHAR expected[MAX_PATH];
    wcscpy_s(expected, _countof(expected), original);
    expected[wcslen(expected) - 3] = L'\0';
    wcscat_s(expected, _countof(expected), L" (2).md");
    EXPECT_WSTR_EQ(probe, expected, "The name gets a \" (2)\" suffix before the extension");

    /* The suffixed name is busy too: the next number is used */
    EXPECT_TRUE(write_text_file(probe, "second", 6) != FALSE, "Create the second report");
    HANDLE reader2 = CreateFileW(probe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
    EXPECT_TRUE(reader2 != INVALID_HANDLE_VALUE, "Hold the second report open");
    WCHAR third[MAX_PATH];
    wcscpy_s(third, _countof(third), path);
    EXPECT_TRUE(restic_report_pick_destination(third, _countof(third)) != FALSE &&
                    wcsstr(third, L" (3).md") != NULL,
                "...and the next free number is chosen");
    CloseHandle(reader2);
    DeleteFileW(probe);

    /* Once the reader is gone the original name is usable again, and a read-only file is no problem */
    CloseHandle(reader);
    SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY);
    wcscpy_s(probe, _countof(probe), path);
    EXPECT_TRUE(restic_report_pick_destination(probe, _countof(probe)) != FALSE &&
                    wcscmp(probe, original) == 0,
                "A closed (even read-only) file is simply replaced");
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(path);

    EXPECT_TRUE(restic_report_pick_destination(NULL, 0) == FALSE &&
                    restic_report_pick_destination(probe, 0) == FALSE,
                "Invalid arguments are refused");
}

/* Launches a stand-in for TOTALCMD64.EXE (a batch file) and checks the command line it receives */
static void test_lister_launch_command_line(void) {
    WCHAR dir[MAX_PATH], bat[MAX_PATH], log[MAX_PATH], report[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    swprintf_s(bat, _countof(bat), L"%ls~rp_fake_tc.bat", dir);
    swprintf_s(log, _countof(log), L"%ls~rp_fake_tc.log", dir);
    swprintf_s(report, _countof(report), L"%lsrestic stats [x] (2).md", dir);
    DeleteFileW(log);

    char script[MAX_PATH * 3];
    char log_a[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, log, -1, log_a, sizeof(log_a), NULL, NULL);
    (void)snprintf(script, sizeof(script), "@echo off\r\n>\"%s\" echo %%*\r\n", log_a);
    EXPECT_TRUE(write_text_file(bat, script, strlen(script)) != FALSE, "Write the stand-in executable");

    WCHAR saved[MAX_PATH] = {0};
    const DWORD had = GetEnvironmentVariableW(L"COMMANDER_EXE", saved, _countof(saved));
    SetEnvironmentVariableW(L"RESTIC_WFX_NO_LISTER_GUI", NULL);
    restic_report_set_lister_test_hook(NULL);
    SetEnvironmentVariableW(L"COMMANDER_EXE", bat);

    EXPECT_TRUE(restic_report_open_in_lister(report) != FALSE, "The stand-in is started");
    char seen[512] = {0};
    for (int i = 0; i < 100 && seen[0] == '\0'; ++i) { /* The process is not waited for */
        Sleep(50);
        char* content = read_file_utf8(log);
        if (content != NULL) {
            strncpy_s(seen, sizeof(seen), content, _TRUNCATE);
            free(content);
        }
    }
    EXPECT_TRUE(strstr(seen, "/S=L") != NULL, "Total Commander is asked to start the Lister (/S=L)");
    EXPECT_TRUE(strstr(seen, "\"") != NULL && strstr(seen, "restic stats [x] (2).md") != NULL,
                "...on the full report path, quoted because it contains spaces");

    /* A host that does not exist is reported, not ignored */
    SetEnvironmentVariableW(L"COMMANDER_EXE", L"C:\\no\\such\\dir\\totalcmd64.exe");
    EXPECT_TRUE(restic_report_open_in_lister(report) == FALSE, "A missing executable makes the call fail");

    SetEnvironmentVariableW(L"COMMANDER_EXE", had > 0 && had < _countof(saved) ? saved : NULL);
    DeleteFileW(bat);
    DeleteFileW(log);
}

static void test_lister_opening(void) {
    restic_report_set_lister_test_hook(NULL);
    EXPECT_TRUE(restic_report_open_in_lister(NULL) == FALSE, "A NULL path is refused");
    EXPECT_TRUE(restic_report_open_in_lister(L"") == FALSE, "An empty path is refused");

    /* The hook replaces the real launch, so no window is opened by the tests */
    g_lister_calls = 0;
    g_lister_path[0] = L'\0';
    restic_report_set_lister_test_hook(lister_hook);
    EXPECT_TRUE(restic_report_open_in_lister(L"C:\\Temp\\restic_stats_x.md") != FALSE,
                "The hook reports success");
    EXPECT_U64_EQ(g_lister_calls, 1, "The hook is called once");
    EXPECT_WSTR_EQ(g_lister_path, L"C:\\Temp\\restic_stats_x.md", "The hook receives the report path");

    restic_report_set_lister_test_hook(failing_lister_hook);
    EXPECT_TRUE(restic_report_open_in_lister(L"C:\\Temp\\restic_stats_x.md") == FALSE,
                "A failing hook makes the call fail");

    /* Without a hook the environment variable suppresses the GUI launch */
    restic_report_set_lister_test_hook(NULL);
    SetEnvironmentVariableW(L"RESTIC_WFX_NO_LISTER_GUI", L"1");
    EXPECT_TRUE(restic_report_open_in_lister(L"C:\\Temp\\restic_stats_x.md") != FALSE,
                "RESTIC_WFX_NO_LISTER_GUI=1 suppresses the window");
    SetEnvironmentVariableW(L"RESTIC_WFX_NO_LISTER_GUI", NULL);
}

/* ------------------------------------------------------------------------------------------ */
/* Live generation with the real restic.exe                                                    */
/* ------------------------------------------------------------------------------------------ */

typedef struct {
    int last_percent;
    int calls;
    int abort_at; /* 0 = never abort */
} ProgressRecorder;

static BOOL live_progress_callback(int percent, void* user_data) {
    ProgressRecorder* recorder = (ProgressRecorder*)user_data;
    recorder->calls++;
    if (percent > recorder->last_percent) recorder->last_percent = percent;
    return (recorder->abort_at != 0 && percent >= recorder->abort_at) ? TRUE : FALSE;
}

/* ESC simulation that fires only once the first phase of the report is over */
static volatile LONG g_cancel_flag = 0;

static BOOL cancel_when_flagged(void* user_data) {
    (void)user_data;
    return g_cancel_flag != 0 ? TRUE : FALSE;
}

static BOOL flag_after_first_phase(int percent, void* user_data) {
    (void)user_data;
    if (percent >= 25) g_cancel_flag = 1;
    return FALSE;
}

/* Builds the WFX-style subpath ("\C\dir\...") of an absolute Windows directory */
static void build_wfx_subpath(const WCHAR* absolute, WCHAR* out, size_t capacity) {
    out[0] = L'\\';
    size_t pos = 1;
    for (size_t i = 0; absolute[i] != L'\0' && pos + 2 < capacity; ++i) {
        WCHAR c = absolute[i];
        if (c == L':') continue;
        out[pos++] = (c == L'\\') ? L'\\' : c;
    }
    out[pos] = L'\0';
}

static void test_live_report_generation(void) {
    WCHAR restic_bin[MAX_PATH], base[MAX_PATH], data[MAX_PATH], repo[MAX_PATH], dest[MAX_PATH];
    GetFullPathNameW(L"tmp\\restic.exe", MAX_PATH, restic_bin, NULL);
    if (GetFileAttributesW(restic_bin) == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] tmp\\restic.exe not found: live report generation skipped.\n");
        return;
    }

    GetFullPathNameW(L"tmp\\report_fixture", MAX_PATH, base, NULL);
    remove_tree(base);
    EXPECT_TRUE(CreateDirectoryW(base, NULL) != FALSE, "Create the report fixture directory");
    swprintf_s(data, _countof(data), L"%ls\\data", base);
    swprintf_s(repo, _countof(repo), L"%ls\\repo", base);
    EXPECT_TRUE(CreateDirectoryW(data, NULL) != FALSE, "Create the source directory");
    EXPECT_TRUE(CreateDirectoryW(repo, NULL) != FALSE, "Create the repository directory");
    WCHAR sub[MAX_PATH];
    swprintf_s(sub, _countof(sub), L"%ls\\sub", data);
    EXPECT_TRUE(CreateDirectoryW(sub, NULL) != FALSE, "Create a subdirectory");

    char payload[4096];
    memset(payload, 'A', sizeof(payload));
    WCHAR path[MAX_PATH];
    swprintf_s(path, _countof(path), L"%ls\\readme.txt", data);
    EXPECT_TRUE(write_text_file(path, "hello\n", 6) != FALSE, "Write a small file");
    swprintf_s(path, _countof(path), L"%ls\\big.bin", sub);
    EXPECT_TRUE(write_text_file(path, payload, sizeof(payload)) != FALSE, "Write a large file");
    swprintf_s(path, _countof(path), L"%ls\\empty.3dm", sub);
    EXPECT_TRUE(write_text_file(path, "", 0) != FALSE, "Write an empty file");

    const WCHAR* password = L"report p@ss \"w0rd\" \u00fc \u65e5\u672c";
    WCHAR err[512] = {0};
    ResticCommandOutput out;
    ResticResult res = restic_exec_command(restic_bin, repo, password,
                                          L"init --json --repository-version 2", NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Initialise the report repository");
    restic_free_command_output(&out);

    WCHAR* quoted = restic_quote_arg(data);
    WCHAR args[MAX_PATH * 2 + 64];
    swprintf_s(args, _countof(args), L"backup %ls --json --quiet", quoted);
    free(quoted);
    res = restic_exec_command(restic_bin, repo, password, args, NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Back up the fixture");
    restic_free_command_output(&out);

    WIN32_FIND_DATAW* items = NULL;
    size_t count            = 0;
    res = restic_list_snapshots(restic_bin, repo, password, NULL, &items, &count, err,
                                _countof(err));
    char snapshot_id[32] = {0};
    EXPECT_TRUE(res == RESTIC_OK && count == 1 &&
                    restic_extract_snapshot_id(items[0].cFileName, snapshot_id, sizeof(snapshot_id)),
                "The fixture snapshot is listed");
    free(items);

    EXPECT_TRUE(restic_report_build_temp_path(L"LiveRepo", NULL, NULL, dest, MAX_PATH) != FALSE,
                "The repository report path is built");
    DeleteFileW(dest);
    restic_report_clear_cache();

    ProgressRecorder recorder;
    memset(&recorder, 0, sizeof(recorder));

    /* Level 1: whole repository */
    res = restic_report_generate(restic_bin, repo, password, L"LiveRepo", NULL, NULL, dest, NULL,
                                 live_progress_callback, &recorder, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK, "The repository report is generated");
    char* md = read_file_utf8(dest);
    EXPECT_TRUE(md != NULL, "The repository report exists");
    EXPECT_TRUE(strstr(md, "Restic repository report") != NULL, "Repository headline");
    EXPECT_TRUE(strstr(md, "LiveRepo") != NULL, "The alias appears in the report");
    EXPECT_TRUE(strstr(md, "Snapshot history") != NULL && strstr(md, snapshot_id) != NULL,
                "The snapshot appears in the history");
    EXPECT_TRUE(strstr(md, "Deduplication factor") != NULL, "Storage statistics were read");
    EXPECT_TRUE(strstr(md, "| :---") != NULL && strstr(md, "<!DOCTYPE") == NULL,
                "The document is a complete Markdown document");
    free(md);
    EXPECT_TRUE(recorder.calls > 0 && recorder.last_percent >= 80,
                "Progress is reported up to the write phase");

    /* The same request right after the previous one is answered from the RAM cache: only the first
       5 % step of the generation is reported, no restic.exe process runs */
    recorder.calls        = 0;
    recorder.last_percent = 0;
    res = restic_report_generate(restic_bin, repo, password, L"LiveRepo", NULL, NULL, dest, NULL,
                                 live_progress_callback, &recorder, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && recorder.calls == 1,
                "A repeated request is served from the report cache without running restic.exe");
    char* cached_md = read_file_utf8(dest);
    EXPECT_TRUE(cached_md != NULL && strstr(cached_md, "## Snapshot history") != NULL,
                "The cached document is written to the destination");
    free(cached_md);

    /* Clearing the cache forces a real generation again */
    restic_report_clear_cache();
    recorder.calls = 0;
    res = restic_report_generate(restic_bin, repo, password, L"LiveRepo", NULL, NULL, dest, NULL,
                                 live_progress_callback, &recorder, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && recorder.calls > 1,
                "After restic_report_clear_cache the report is generated again");

    /* Level 2: snapshot report */
    recorder.calls        = 0;
    recorder.last_percent = 0;
    res = restic_report_generate(restic_bin, repo, password, L"LiveRepo", snapshot_id, NULL, dest,
                                 NULL, live_progress_callback, &recorder, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK, "The snapshot report is generated");
    md = read_file_utf8(dest);
    EXPECT_TRUE(md != NULL, "The snapshot report exists");
    EXPECT_TRUE(strstr(md, "Restic snapshot report") != NULL, "Snapshot headline");
    EXPECT_TRUE(strstr(md, "Files and directories") != NULL, "The summary was read");
    EXPECT_TRUE(strstr(md, "Initial snapshot") != NULL, "No parent: the diff is explained");
    EXPECT_TRUE(strstr(md, "Compression ratio") != NULL, "Live storage statistics were read");
    free(md);

    /* Level 3: subfolder report */
    WCHAR subpath[MAX_PATH];
    build_wfx_subpath(data, subpath, _countof(subpath));
    res = restic_report_generate(restic_bin, repo, password, L"LiveRepo", snapshot_id, subpath,
                                 dest, NULL, live_progress_callback, &recorder, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK, "The folder report is generated");
    md = read_file_utf8(dest);
    EXPECT_TRUE(md != NULL, "The folder report exists");
    EXPECT_TRUE(strstr(md, "Restic folder report") != NULL, "Folder headline");
    EXPECT_TRUE(strstr(md, "readme.txt") != NULL, "Small file listed");
    EXPECT_TRUE(strstr(md, "big.bin") != NULL, "Largest file listed");
    EXPECT_TRUE(strstr(md, "Size by subfolder") != NULL && strstr(md, "sub") != NULL,
                "The subfolder is aggregated");
    EXPECT_TRUE(strstr(md, ".3dm") != NULL, "The extension table is filled");
    EXPECT_TRUE(count_occurrences(md, "#") > 0 && count_occurrences(md, "[") > 0,
                "ASCII bars are drawn");
    free(md);

    /* A second backup: the snapshot now has a parent, so its report carries the diff */
    swprintf_s(path, _countof(path), L"%ls\\readme.txt", data);
    EXPECT_TRUE(write_text_file(path, "changed content\n", 16) != FALSE, "Modify a file");
    quoted = restic_quote_arg(data);
    swprintf_s(args, _countof(args), L"backup %ls --json --quiet", quoted);
    free(quoted);
    res = restic_exec_command(restic_bin, repo, password, args, NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Take a second backup");
    restic_free_command_output(&out);
    res = restic_list_snapshots(restic_bin, repo, password, NULL, &items, &count, err, _countof(err));
    char second_id[32] = {0};
    EXPECT_TRUE(res == RESTIC_OK && count == 2 &&
                    restic_extract_snapshot_id(items[1].cFileName, second_id, sizeof(second_id)),
                "The second snapshot is listed");
    free(items);

    SetFileAttributesW(dest, FILE_ATTRIBUTE_READONLY); /* An earlier copy of the report */
    res = restic_report_generate(restic_bin, repo, password, L"LiveRepo", second_id, NULL, dest, NULL,
                                 NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK, "The report of a snapshot with a parent is generated");
    EXPECT_TRUE((GetFileAttributesW(dest) & FILE_ATTRIBUTE_READONLY) == 0,
                "A read-only earlier copy is replaced");
    md = read_file_utf8(dest);
    EXPECT_TRUE(md != NULL, "The snapshot report exists");
    EXPECT_TRUE(strstr(md, "Parent snapshot") != NULL && strstr(md, "Modified files") != NULL,
                "The diff summary is present");
    EXPECT_TRUE(strstr(md, "[M]") != NULL && strstr(md, "readme.txt") != NULL,
                "The modified file is listed");
    EXPECT_TRUE(strstr(md, "Initial snapshot") == NULL, "...and the report is not the initial one");
    free(md);

    /* ESC during generation aborts and leaves the destination untouched */
    EXPECT_TRUE(write_text_file(dest, "keep", 4) != FALSE, "Pre-create the destination");
    recorder.abort_at     = 1;
    recorder.calls        = 0;
    recorder.last_percent = 0;
    res = restic_report_generate(restic_bin, repo, password, L"LiveRepo", NULL, NULL, dest, NULL,
                                 live_progress_callback, &recorder, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_CANCELLED, "A user abort cancels the generation");
    md = read_file_utf8(dest);
    EXPECT_TRUE(md != NULL && strcmp(md, "keep") == 0,
                "A cancelled generation does not replace an existing file");
    free(md);
    DeleteFileW(dest);

    /* ESC while a secondary `restic stats` runs must abort the report, not blank one of its blocks */
    {
        restic_report_clear_cache(); /* The same request must really run restic.exe again */
        ResticExecOptions cancel_options = {0, cancel_when_flagged, NULL};
        g_cancel_flag                    = 0;
        res = restic_report_generate(restic_bin, repo, password, L"LiveRepo", NULL, NULL, dest,
                                     &cancel_options, flag_after_first_phase, NULL, err,
                                     _countof(err));
        EXPECT_TRUE(res == RESTIC_ERR_CANCELLED, "ESC during a statistics query cancels the report");
        EXPECT_TRUE(GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES, "Nothing was written");
    }

    /* A missing snapshot is reported, not silently rendered */
    recorder.abort_at = 0;
    res = restic_report_generate(restic_bin, repo, password, L"LiveRepo", "ffffffff", NULL, dest,
                                 NULL, live_progress_callback, &recorder, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND, "An unknown snapshot is refused");
    EXPECT_TRUE(GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES, "Nothing was written");

    /* A folder that does not exist in the snapshot is reported, not rendered as an empty one */
    res = restic_report_generate(restic_bin, repo, password, L"LiveRepo", snapshot_id,
                                 L"\\no\\such\\folder", dest, NULL, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND, "An unknown folder is refused");
    EXPECT_TRUE(GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES, "Nothing was written");

    /* Invalid arguments */
    EXPECT_TRUE(restic_report_generate(restic_bin, repo, password, L"LiveRepo", NULL, NULL, NULL,
                                       NULL, NULL, NULL, err, _countof(err)) == RESTIC_ERR_INVALID_ARG,
                "A NULL destination is refused");
    EXPECT_TRUE(restic_report_generate(restic_bin, repo, password, L"LiveRepo", "not-hex", NULL,
                                       dest, NULL, NULL, NULL, err, _countof(err)) ==
                    RESTIC_ERR_INVALID_ARG,
                "A non-hexadecimal snapshot ID is refused");

    remove_tree(base);
}

int main(void) {
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    printf("Running Restic Markdown statistics report tests...\n");

    test_ascii_bar_formatting();
    test_number_and_time_formatting();
    test_markdown_text_escaping();
    test_status_info_f3_detection();
    test_storage_json_parsing();
    test_diff_parsing();
    test_folder_aggregation();
    test_folder_extensions_and_truncation();
    test_folder_remainder_rows();
    test_folder_root_must_be_a_directory();
    test_markdown_report_generation();
    test_location_redaction();
    test_report_temp_path();
    test_pick_destination();
    test_lister_opening();
    test_lister_launch_command_line();
    test_live_report_generation();

#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    if (!_CrtCheckMemory()) {
        printf("[FAIL] CRT heap integrity check failed.\n");
        g_tests_failed++;
    }
#endif

    if (g_tests_failed == 0) {
        printf("[PASS] All %d assertions succeeded.\n", g_tests_run);
        return 0;
    }
    printf("[FAIL] %d of %d assertions failed.\n", g_tests_failed, g_tests_run);
    return 1;
}
