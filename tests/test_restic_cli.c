#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
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

static unsigned long long ft_u64(FILETIME ft) {
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static unsigned long long file_size_of(const WIN32_FIND_DATAW* fd) {
    return ((unsigned long long)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
}

static const WIN32_FIND_DATAW* find_entry(const WIN32_FIND_DATAW* items, size_t count, const WCHAR* name) {
    for (size_t i = 0; i < count; ++i) {
        if (wcscmp(items[i].cFileName, name) == 0) return &items[i];
    }
    return NULL;
}

static BOOL is_dir_entry(const WIN32_FIND_DATAW* fd) {
    return (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/* ============================================================================================ */
/* Pure unit tests                                                                              */
/* ============================================================================================ */

/* ISO-8601 parsing: exact values, timezones, fractions and rejection of invalid dates */
static void test_iso8601_parsing(void) {
    FILETIME ft, ft2;

    EXPECT_TRUE(restic_parse_iso8601_time("1970-01-01T00:00:00Z", &ft), "Unix epoch parses");
    EXPECT_TRUE(ft_u64(ft) == 116444736000000000ULL, "Unix epoch equals the known FILETIME value");

    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T14:28:15.182004Z", &ft), "UTC Z timestamp");
    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T16:28:15.182004+02:00", &ft2), "+02:00 timestamp");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(ft2), "+02:00 converts to identical UTC FILETIME");
    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T09:28:15.182004-05:00", &ft2), "-05:00 timestamp");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(ft2), "-05:00 converts to identical UTC FILETIME");

    /* Compact and hour-only offsets */
    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T10:00:00+0530", &ft), "+0530 offset");
    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T04:30:00Z", &ft2), "Reference UTC time");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(ft2), "+0530 equals 04:30 UTC");
    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T12:00:00+02", &ft), "+02 offset");
    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T10:00:00Z", &ft2), "Reference UTC time");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(ft2), "+02 equals 10:00 UTC");

    /* Fractional seconds: 100 ns resolution, extra digits ignored */
    SYSTEMTIME st = {2020, 2, 0, 29, 12, 34, 56, 0};
    FILETIME base;
    EXPECT_TRUE(SystemTimeToFileTime(&st, &base), "Reference leap-day FILETIME");
    EXPECT_TRUE(restic_parse_iso8601_time("2020-02-29T12:34:56.5Z", &ft), "Fraction .5");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(base) + 5000000ULL, "Fraction .5 = 5,000,000 ticks");
    EXPECT_TRUE(restic_parse_iso8601_time("2020-02-29T12:34:56.1234567891Z", &ft), "10 fractional digits");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(base) + 1234567ULL, "Digits beyond 100 ns are ignored");
    EXPECT_TRUE(restic_parse_iso8601_time("2020-02-29T12:34:60Z", &ft), "Leap second accepted");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(base) + 30000000ULL, "Leap second maps to 12:34:59");

    /* Rejections */
    EXPECT_TRUE(!restic_parse_iso8601_time(NULL, &ft), "NULL string");
    EXPECT_TRUE(!restic_parse_iso8601_time("invalid-date", &ft), "Garbage");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18", &ft), "Date without time");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-02-30T10:00:00Z", &ft), "February 30");
    EXPECT_TRUE(!restic_parse_iso8601_time("2021-02-29T10:00:00Z", &ft), "Feb 29 of a non-leap year");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-13-01T10:00:00Z", &ft), "Month 13");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18T25:00:00Z", &ft), "Hour 25");
    EXPECT_TRUE(!restic_parse_iso8601_time("0001-01-01T00:00:00Z", &ft), "Year before FILETIME epoch");
    EXPECT_TRUE(!restic_parse_iso8601_time("1601-01-01T00:00:00+02:00", &ft), "Offset pushing before epoch");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18T10:00:00+24:00", &ft), "Offset hour 24");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18T10:00:00+02:75", &ft), "Offset minute 75");
}

/* Snapshot directory naming and ID extraction */
static void test_snapshot_naming(void) {
    WCHAR dirname[MAX_PATH] = {0};
    const char* full_id = "29095f00dedf06d1ed449d29e286bd48c3291aed7d35e5faa51954a61a4ae7a5";

    EXPECT_TRUE(restic_format_snapshot_dirname("2026-10-18T16:28:15.182004+02:00", full_id, dirname, MAX_PATH),
                "Format snapshot directory name");
    EXPECT_WSTR_EQ(dirname, L"2026-10-18_16-28-15_[29095f00]", "Directory name format");

    EXPECT_TRUE(!restic_format_snapshot_dirname("2026-10-18T16:28:15Z", "xyz123", dirname, MAX_PATH),
                "Reject non-hex snapshot ID");
    EXPECT_TRUE(!restic_format_snapshot_dirname("2026-10-18T16:28:15Z", "", dirname, MAX_PATH), "Reject empty ID");
    EXPECT_TRUE(!restic_format_snapshot_dirname("..\\..\\evil\\path\\xx", "abcd1234", dirname, MAX_PATH),
                "Reject malformed time (no path separators may leak into the name)");
    EXPECT_TRUE(!restic_format_snapshot_dirname("2026-10-18T16:28:15Z", "abcd1234", dirname, 5),
                "Reject undersized output buffer");

    char id[32] = {0};
    EXPECT_TRUE(restic_format_snapshot_dirname("2026-10-18T16:28:15Z", full_id, dirname, MAX_PATH), "Format again");
    EXPECT_TRUE(restic_extract_snapshot_id(dirname, id, sizeof(id)), "Extract short_id");
    EXPECT_STR_EQ(id, "29095f00", "Extracted short_id");
    EXPECT_TRUE(!restic_extract_snapshot_id(L"no_brackets", id, sizeof(id)), "Reject missing brackets");
    EXPECT_TRUE(!restic_extract_snapshot_id(L"bad_[not_hex!]", id, sizeof(id)), "Reject non-hex ID");
    EXPECT_TRUE(!restic_extract_snapshot_id(L"empty_[]", id, sizeof(id)), "Reject empty brackets");
    EXPECT_TRUE(!restic_extract_snapshot_id(L"reversed_]abcd[", id, sizeof(id)), "Reject reversed brackets");
    EXPECT_TRUE(!restic_extract_snapshot_id(L"long_[0123456789abcdef]", id, 8), "Reject ID larger than buffer");

    /* Only "<date>_[<hex>]" identifies a snapshot folder: files that merely contain a bracketed
       hexadecimal string must never be mistaken for one (neither by the path parser nor by the
       loose content-column lookup) */
    EXPECT_TRUE(!restic_extract_snapshot_id(L"report_[deadbeef].txt", id, sizeof(id)),
                "Reject a bracketed ID followed by more text");
    EXPECT_TRUE(!restic_extract_snapshot_id(L"file[deadbeef]", id, sizeof(id)),
                "Reject a '[' that does not follow '_'");
    EXPECT_TRUE(!restic_extract_snapshot_id(L"2026-10-18_16-28-15_[29095f00].bak", id, sizeof(id)),
                "Reject a snapshot-like name with a suffix");
    EXPECT_TRUE(restic_extract_snapshot_id(L"2026-10-18_16-28-15_[29095f00]", id, sizeof(id)) &&
                    strcmp(id, "29095f00") == 0,
                "Accept the exact directory format");
}

/* WFX path -> Restic filter path, and command-line quoting */
static void test_path_and_quote_helpers(void) {
    char norm[256] = {0};
    EXPECT_TRUE(restic_normalize_filter_path(L"", norm, sizeof(norm)), "Empty path");
    EXPECT_STR_EQ(norm, "/", "Empty -> /");
    EXPECT_TRUE(restic_normalize_filter_path(NULL, norm, sizeof(norm)), "NULL path");
    EXPECT_STR_EQ(norm, "/", "NULL -> /");
    EXPECT_TRUE(restic_normalize_filter_path(L"\\", norm, sizeof(norm)), "Root backslash");
    EXPECT_STR_EQ(norm, "/", "Backslash -> /");
    EXPECT_TRUE(restic_normalize_filter_path(L"\\C\\Users\\Docs\\", norm, sizeof(norm)), "Subpath");
    EXPECT_STR_EQ(norm, "/C/Users/Docs", "Separators converted, trailing slash stripped");
    EXPECT_TRUE(restic_normalize_filter_path(L"C\\\\Users\\\\\\Docs", norm, sizeof(norm)), "Missing lead + doubled separators");
    EXPECT_STR_EQ(norm, "/C/Users/Docs", "Leading slash added, duplicates collapsed");
    EXPECT_TRUE(restic_normalize_filter_path(L"\\\u00fcn\\\u65e5\u672c", norm, sizeof(norm)), "Unicode path");
    EXPECT_STR_EQ(norm, "/\xC3\xBCn/\xE6\x97\xA5\xE6\x9C\xAC", "UTF-8 encoding");
    EXPECT_TRUE(!restic_normalize_filter_path(L"\\abcdefghijklmnop", norm, 8), "Output buffer too small");

    /* A maximum-length (1023 UTF-16 units) path made of 3-byte characters must fit */
    WCHAR* long_path = (WCHAR*)calloc(1024, sizeof(WCHAR));
    char* long_utf8 = (char*)calloc(3200, 1);
    EXPECT_TRUE(long_path && long_utf8, "Allocate long path buffers");
    long_path[0] = L'\\';
    for (int i = 1; i < 1023; ++i) long_path[i] = L'\u65e5';
    EXPECT_TRUE(restic_normalize_filter_path(long_path, long_utf8, 3200), "1023-character CJK path");
    EXPECT_TRUE(strlen(long_utf8) == 1 + 1022 * 3, "Long path length preserved");
    free(long_path);
    free(long_utf8);

    /* Quoting: exact output ... */
    WCHAR* q = restic_quote_arg(L"plain");
    EXPECT_TRUE(q != NULL, "Quote plain");
    EXPECT_WSTR_EQ(q, L"\"plain\"", "Plain argument");
    free(q);
    q = restic_quote_arg(L"");
    EXPECT_WSTR_EQ(q, L"\"\"", "Empty argument");
    free(q);
    q = restic_quote_arg(L"say \"hi\"");
    EXPECT_WSTR_EQ(q, L"\"say \\\"hi\\\"\"", "Embedded quotes escaped");
    free(q);
    q = restic_quote_arg(L"C:\\dir\\");
    EXPECT_WSTR_EQ(q, L"\"C:\\dir\\\\\"", "Trailing backslash doubled");
    free(q);

    /* ... and round-trip through the same parser used by the C runtime of the child */
    const WCHAR* nasty[] = {
        L"simple", L"with space", L"quote\"inside", L"\"", L"\\\"", L"back\\\\\"slash",
        L"trail\\", L"trail\\\\", L"/C/odd [dir] & (x) ^ 100%", L"/a\\b\"c d\\\\", L"\u00fcn\u65e5"
    };
    for (size_t i = 0; i < _countof(nasty); ++i) {
        q = restic_quote_arg(nasty[i]);
        EXPECT_TRUE(q != NULL, "Quote nasty argument");
        size_t cmd_len = wcslen(q) + 16;
        WCHAR* cmd = (WCHAR*)malloc(cmd_len * sizeof(WCHAR));
        swprintf_s(cmd, cmd_len, L"prog.exe %ls", q);
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmd, &argc);
        EXPECT_TRUE(argv != NULL && argc == 2, "Quoted argument stays a single token");
        EXPECT_WSTR_EQ(argv[1], nasty[i], "Argument survives CommandLineToArgvW round-trip");
        LocalFree(argv);
        free(cmd);
        free(q);
    }
}

/* Parsing `restic snapshots --json` */
static void test_parse_snapshots_json(void) {
    const char* sample_json =
        "[\n"
        "  {\"time\": \"2026-10-18T16:28:15.182004+02:00\", \"id\": \"29095f00dedf06d1ed449d29e286bd48c3291aed7d35e5faa51954a61a4ae7a5\", \"short_id\": \"29095f00\"},\n"
        "  {\"time\": \"2026-10-19T08:05:00Z\", \"id\": \"abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890\"},\n"
        "  {\"time\": \"not-a-time\", \"id\": \"aaaaaaaa\"},\n"
        "  {\"time\": \"2026-10-20T08:05:00Z\", \"id\": \"not-hex-id\"},\n"
        "  {\"id\": \"bbbbbbbb\"},\n"
        "  42\n"
        "]";

    WIN32_FIND_DATAW* items = NULL;
    size_t count = 0;
    ResticResult res = restic_parse_snapshots_json(sample_json, &items, &count);
    EXPECT_TRUE(res == RESTIC_OK, "Parse snapshots array");
    EXPECT_TRUE(count == 2, "Malformed entries are skipped, valid ones kept");
    EXPECT_WSTR_EQ(items[0].cFileName, L"2026-10-18_16-28-15_[29095f00]", "Snapshot 0 name");
    EXPECT_WSTR_EQ(items[1].cFileName, L"2026-10-19_08-05-00_[abcdef12]", "Snapshot 1 name (falls back to id prefix)");
    EXPECT_TRUE(is_dir_entry(&items[0]), "Snapshot is a directory");
    EXPECT_TRUE(ft_u64(items[0].ftLastWriteTime) != 0, "Snapshot has a modification time");
    free(items);

    res = restic_parse_snapshots_json("[]", &items, &count);
    EXPECT_TRUE(res == RESTIC_OK && count == 0 && items == NULL, "Empty array");
    res = restic_parse_snapshots_json("null", &items, &count);
    EXPECT_TRUE(res == RESTIC_OK && count == 0 && items == NULL, "Literal null is an empty list");
    res = restic_parse_snapshots_json("{not_an_array}", &items, &count);
    EXPECT_TRUE(res == RESTIC_ERR_JSON_PARSE, "Malformed JSON");
    res = restic_parse_snapshots_json("{\"a\":1}", &items, &count);
    EXPECT_TRUE(res == RESTIC_ERR_JSON_PARSE, "Object instead of array");
    res = restic_parse_snapshots_json("", &items, &count);
    EXPECT_TRUE(res == RESTIC_ERR_JSON_PARSE, "Empty text");
    res = restic_parse_snapshots_json(NULL, &items, &count);
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "NULL text");
}

#define HEADER_LINE "{\"time\":\"2026-10-18T16:28:15+02:00\",\"id\":\"29095f00\",\"message_type\":\"snapshot\"}\r\n"

/* Parsing `restic ls --json` NDJSON */
static void test_parse_ls_ndjson(void) {
    WIN32_FIND_DATAW* items = NULL;
    size_t count = 0;
    ResticResult res;

    /* Direct children only: self node, grandchildren and prefix-sharing siblings are excluded.
       5 GiB + 7 bytes = 5368709127 -> High DWORD 1, Low DWORD 1073741831. */
    const char* sample =
        HEADER_LINE
        "{\"name\":\"sample_data\",\"type\":\"dir\",\"path\":\"/tmp/sample_data\",\"mtime\":\"2026-10-18T16:28:12+02:00\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"big_archive.tar\",\"type\":\"file\",\"path\":\"/tmp/sample_data/big_archive.tar\",\"size\":5368709127,\"mtime\":\"2026-10-18T16:28:12+02:00\",\"message_type\":\"node\"}\r\n"
        "\r\n"
        "{\"name\":\"sub1\",\"type\":\"dir\",\"path\":\"/tmp/sample_data/sub1\",\"mtime\":\"2026-10-18T16:28:12+02:00\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"deep.txt\",\"type\":\"file\",\"path\":\"/tmp/sample_data/sub1/deep.txt\",\"size\":42,\"message_type\":\"node\"}\r\n"
        "{\"name\":\"sample_data2\",\"type\":\"dir\",\"path\":\"/tmp/sample_data2\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"x.txt\",\"type\":\"file\",\"path\":\"/tmp/sample_data2/x.txt\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"sample_data.bak\",\"type\":\"file\",\"path\":\"/tmp/sample_data.bak\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"link\",\"type\":\"symlink\",\"path\":\"/tmp/sample_data/link\",\"mtime\":\"garbage\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"empty.txt\",\"type\":\"file\",\"path\":\"/tmp/sample_data/empty.txt\",\"size\":0,\"message_type\":\"node\"}";

    res = restic_parse_ls_ndjson(sample, "/tmp/sample_data", &items, &count);
    EXPECT_TRUE(res == RESTIC_OK, "Parse NDJSON stream");
    EXPECT_TRUE(count == 4, "big_archive.tar, sub1, link and empty.txt only");
    const WIN32_FIND_DATAW* big = find_entry(items, count, L"big_archive.tar");
    EXPECT_TRUE(big != NULL && !is_dir_entry(big), "File entry");
    EXPECT_TRUE(big->nFileSizeHigh == 1 && big->nFileSizeLow == 1073741831U, "64-bit size split into High/Low");
    EXPECT_TRUE(file_size_of(big) == 5368709127ULL, "64-bit size recombines");
    EXPECT_TRUE((big->dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0, "Entries are read-only");
    const WIN32_FIND_DATAW* sub = find_entry(items, count, L"sub1");
    EXPECT_TRUE(sub != NULL && is_dir_entry(sub), "Directory entry");
    EXPECT_TRUE(find_entry(items, count, L"sample_data") == NULL, "Filter directory itself is not listed");
    EXPECT_TRUE(find_entry(items, count, L"deep.txt") == NULL, "Grandchildren are not listed");
    EXPECT_TRUE(find_entry(items, count, L"sample_data2") == NULL, "Prefix-sharing sibling directory is excluded");
    EXPECT_TRUE(find_entry(items, count, L"sample_data.bak") == NULL, "Prefix-sharing sibling file is excluded");
    const WIN32_FIND_DATAW* link = find_entry(items, count, L"link");
    EXPECT_TRUE(link != NULL && !is_dir_entry(link), "Symlinks are reported as plain files");
    EXPECT_TRUE(link->ftLastWriteTime.dwHighDateTime == 0xFFFFFFFF && link->ftLastWriteTime.dwLowDateTime == 0xFFFFFFFE,
                "Unparsable mtime -> 'no date' marker");
    const WIN32_FIND_DATAW* empty = find_entry(items, count, L"empty.txt");
    EXPECT_TRUE(empty != NULL && file_size_of(empty) == 0, "Zero-byte file");
    free(items);

    /* Root listing needs no self node */
    res = restic_parse_ls_ndjson(HEADER_LINE
        "{\"name\":\"C\",\"type\":\"dir\",\"path\":\"/C\",\"message_type\":\"node\"}\r\n",
        "/", &items, &count);
    EXPECT_TRUE(res == RESTIC_OK && count == 1 && wcscmp(items[0].cFileName, L"C") == 0, "Root listing");
    free(items);
    res = restic_parse_ls_ndjson(HEADER_LINE, "/", &items, &count);
    EXPECT_TRUE(res == RESTIC_OK && count == 0 && items == NULL, "Empty snapshot root is valid");

    /* Names that would break Total Commander's navigation are never reported */
    res = restic_parse_ls_ndjson(HEADER_LINE
        "{\"name\":\"\",\"type\":\"file\",\"path\":\"/x\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\".\",\"type\":\"dir\",\"path\":\"/.\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"..\",\"type\":\"dir\",\"path\":\"/..\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"evil\\\\sep.txt\",\"type\":\"file\",\"path\":\"/evil\\\\sep.txt\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"sub/dir\",\"type\":\"dir\",\"path\":\"/sub_div\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"ok.txt\",\"type\":\"file\",\"path\":\"/ok.txt\",\"message_type\":\"node\"}\r\n",
        "/", &items, &count);
    EXPECT_TRUE(res == RESTIC_OK && count == 1 && wcscmp(items[0].cFileName, L"ok.txt") == 0,
                "Empty, '.' and '..' node names are skipped");
    EXPECT_TRUE(find_entry(items, count, L"evil\\sep.txt") == NULL,
                "A name containing a backslash is skipped (a Linux/macOS backup can contain one)");
    EXPECT_TRUE(find_entry(items, count, L"sub/dir") == NULL,
                "A name containing a forward slash is skipped");
    free(items);

    /* Existing but empty directory vs. missing path vs. file used as directory */
    res = restic_parse_ls_ndjson(HEADER_LINE
        "{\"name\":\"empty_dir\",\"type\":\"dir\",\"path\":\"/a/empty_dir\",\"message_type\":\"node\"}\r\n",
        "/a/empty_dir", &items, &count);
    EXPECT_TRUE(res == RESTIC_OK && count == 0, "Empty directory exists");
    res = restic_parse_ls_ndjson(HEADER_LINE, "/a/missing", &items, &count);
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND && items == NULL && count == 0, "Missing directory");
    res = restic_parse_ls_ndjson(HEADER_LINE
        "{\"name\":\"f.txt\",\"type\":\"file\",\"path\":\"/a/f.txt\",\"size\":3,\"message_type\":\"node\"}\r\n",
        "/a/f.txt", &items, &count);
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND, "A file is not a listable directory");

    /* Names: UTF-8 decoding and over-long names */
    res = restic_parse_ls_ndjson(
        "{\"name\":\"\xC3\xBCn\xE6\x97\xA5\xE6\x9C\xAC\",\"type\":\"file\",\"path\":\"/\xC3\xBCn\xE6\x97\xA5\xE6\x9C\xAC\",\"message_type\":\"node\"}\n",
        "/", &items, &count);
    EXPECT_TRUE(res == RESTIC_OK && count == 1, "Unicode name");
    EXPECT_WSTR_EQ(items[0].cFileName, L"\u00fcn\u65e5\u672c", "UTF-8 name decoded to UTF-16");
    free(items);

    /* Corrupt or non-object lines */
    res = restic_parse_ls_ndjson(HEADER_LINE "this is not json\r\n", "/", &items, &count);
    EXPECT_TRUE(res == RESTIC_ERR_JSON_PARSE && items == NULL && count == 0, "Garbage line aborts the parse");
    res = restic_parse_ls_ndjson(HEADER_LINE "[1,2,3]\r\n", "/", &items, &count);
    EXPECT_TRUE(res == RESTIC_ERR_JSON_PARSE, "Non-object line");
    res = restic_parse_ls_ndjson("{\"name\":\"x\",\"type\":\"file\",\"pa", "/", &items, &count);
    EXPECT_TRUE(res == RESTIC_ERR_JSON_PARSE, "Truncated line");
    res = restic_parse_ls_ndjson("", "/", &items, &count);
    EXPECT_TRUE(res == RESTIC_OK && count == 0, "Empty stream");
    res = restic_parse_ls_ndjson(NULL, "/", &items, &count);
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "NULL stream");
}

/* ============================================================================================ */
/* Child process tests (use cmd.exe as a deterministic stand-in for restic.exe)                  */
/* ============================================================================================ */

static BOOL get_cmd_exe(WCHAR* out, DWORD cap) {
    DWORD n = GetEnvironmentVariableW(L"ComSpec", out, cap);
    return n > 0 && n < cap;
}

static ResticResult run_cmd(const WCHAR* args, const ResticExecOptions* opts, ResticCommandOutput* out) {
    WCHAR cmd[MAX_PATH];
    if (!get_cmd_exe(cmd, MAX_PATH)) return RESTIC_ERR_SPAWN_FAILED;
    return restic_exec_command(cmd, L"repo", L"pw", args, opts, out);
}

/* stdout/stderr are kept apart and exit codes are mapped */
static void test_exec_streams_and_exit_codes(void) {
    ResticCommandOutput out;

    ResticResult res = run_cmd(L"/c \"echo hello\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK && out.exit_code == 0, "Successful command");
    EXPECT_TRUE(strstr(out.output, "hello") != NULL, "stdout captured");
    EXPECT_TRUE(out.error_output != NULL && out.error_length == 0, "stderr is an empty (non-NULL) string");
    restic_free_command_output(&out);

    res = run_cmd(L"/c \"echo to_stdout& echo to_stderr 1>&2& exit /b 3\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && out.exit_code == 3, "Exit code 3 -> RESTIC_ERR_CLI_FAILED");
    EXPECT_TRUE(strstr(out.output, "to_stdout") != NULL && strstr(out.output, "to_stderr") == NULL,
                "stderr text never leaks into stdout");
    EXPECT_TRUE(strstr(out.error_output, "to_stderr") != NULL && strstr(out.error_output, "to_stdout") == NULL,
                "stderr captured separately");
    EXPECT_TRUE(wcsncmp(out.error_message, L"to_stderr", 9) == 0, "Error message taken from stderr, newline trimmed");
    EXPECT_TRUE(wcsstr(out.error_message, L"\n") == NULL, "No trailing newline in error message");
    restic_free_command_output(&out);

    res = run_cmd(L"/c \"exit /b 10\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_REPO_NOT_FOUND, "Exit 10 -> repository not found");
    restic_free_command_output(&out);
    res = run_cmd(L"/c \"exit /b 11\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_REPO_LOCKED, "Exit 11 -> repository locked");
    restic_free_command_output(&out);
    res = run_cmd(L"/c \"exit /b 12\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD, "Exit 12 -> wrong password");
    restic_free_command_output(&out);

    /* A JSON error object on stderr is reduced to its "message" field */
    res = run_cmd(L"/c \"echo {\"message_type\":\"exit_error\",\"message\":\"Fatal: boom\"} 1>&2& exit /b 1\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED, "JSON error exit");
    EXPECT_WSTR_EQ(out.error_message, L"Fatal: boom", "JSON message extracted");
    restic_free_command_output(&out);

    /* Very long messages are truncated and terminated, never rejected */
    WCHAR* long_args = (WCHAR*)calloc(2048, sizeof(WCHAR));
    EXPECT_TRUE(long_args != NULL, "Allocate");
    wcscpy_s(long_args, 2048, L"/c \"echo ");
    for (int i = 0; i < 1500; ++i) wcscat_s(long_args, 2048, L"y");
    wcscat_s(long_args, 2048, L" 1>&2& exit /b 1\"");
    res = run_cmd(long_args, NULL, &out);
    free(long_args);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED, "Long stderr line");
    EXPECT_TRUE(wcslen(out.error_message) == 511, "Message truncated to the buffer size");
    restic_free_command_output(&out);
}

static BOOL always_cancel(void* user_data) {
    (void)user_data;
    return TRUE;
}

/* restic_exec_lines(): stdout is delivered line by line, in constant memory */
typedef struct {
    size_t lines;
    size_t bytes;
    size_t stop_after; /* 0 = never ask to stop */
    BOOL saw_cr;
    char first[64];
    char last[64];
} LineCapture;

static BOOL capture_line(const char* line, size_t length, void* user_data) {
    LineCapture* capture = (LineCapture*)user_data;
    if (capture->lines == 0) {
        strncpy_s(capture->first, sizeof(capture->first), line, length < 63 ? length : 63);
    }
    strncpy_s(capture->last, sizeof(capture->last), line, length < 63 ? length : 63);
    if (memchr(line, '\r', length) != NULL) capture->saw_cr = TRUE;
    capture->lines++;
    capture->bytes += length;
    return capture->stop_after == 0 || capture->lines < capture->stop_after;
}

static ResticResult run_cmd_lines(const WCHAR* args, const ResticExecOptions* opts, LineCapture* capture,
                                  WCHAR* err, size_t err_capacity) {
    WCHAR cmd[MAX_PATH];
    if (!get_cmd_exe(cmd, MAX_PATH)) return RESTIC_ERR_SPAWN_FAILED;
    memset(capture->first, 0, sizeof(capture->first));
    memset(capture->last, 0, sizeof(capture->last));
    capture->lines = capture->bytes = 0;
    capture->saw_cr = FALSE;
    return restic_exec_lines(cmd, L"repo", L"pw", args, opts, capture_line, capture, err, err_capacity);
}

static void test_exec_lines(void) {
    LineCapture capture;
    WCHAR err[512] = {0};
    memset(&capture, 0, sizeof(capture));

    /* 4000 lines (>100 KB) arrive one by one, terminators removed, nothing buffered up */
    ResticResult res = run_cmd_lines(
        L"/c \"for /L %i in (1,1,4000) do @echo line_%i_0123456789_0123456789\"", NULL, &capture, err,
        _countof(err));
    EXPECT_TRUE(res == RESTIC_OK, "A long listing is streamed");
    EXPECT_TRUE(capture.lines == 4000, "Every line is delivered exactly once");
    EXPECT_TRUE(capture.saw_cr == FALSE, "CR LF terminators are removed");
    EXPECT_TRUE(strcmp(capture.first, "line_1_0123456789_0123456789") == 0 &&
                    strcmp(capture.last, "line_4000_0123456789_0123456789") == 0,
                "First and last line are intact");
    EXPECT_TRUE(capture.bytes > 100000, "The whole payload went through the callback");

    /* Empty lines are delivered too; a last line without terminator is not lost */
    res = run_cmd_lines(L"/c \"echo.& echo a& echo.& <nul set /p =tail& exit /b 0\"", NULL, &capture, err,
                        _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && capture.lines == 4, "Empty lines and an unterminated tail count");
    EXPECT_TRUE(strcmp(capture.last, "tail") == 0, "The unterminated last line is delivered");

    /* The callback can stop the command: the child is killed and the result is a cancellation */
    capture.stop_after = 10;
    res = run_cmd_lines(L"/c \"for /L %i in (1,1,4000) do @echo line_%i\"", NULL, &capture, err,
                        _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_CANCELLED && capture.lines == 10,
                "A callback returning FALSE cancels the command at that line");
    capture.stop_after = 0;

    /* Exit codes keep their meaning; output printed before the failure was still delivered */
    res = run_cmd_lines(L"/c \"echo one& echo oops 1>&2& exit /b 12\"", NULL, &capture, err,
                        _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD && capture.lines == 1 &&
                    strcmp(capture.first, "one") == 0,
                "Exit 12 -> wrong password, earlier lines delivered");
    EXPECT_TRUE(wcsncmp(err, L"oops", 4) == 0, "The reason comes from stderr");
    res = run_cmd_lines(L"/c \"exit /b 3\"", NULL, &capture, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && capture.lines == 0, "Exit 3 -> generic failure");

    /* Timeout and cancellation callback behave as for restic_exec_command */
    ResticExecOptions options = {300, NULL, NULL};
    res = run_cmd_lines(L"/c \"ping -n 8 127.0.0.1 >nul\"", &options, &capture, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_TIMEOUT, "A silent command hits the timeout");
    options.timeout_ms = 0;
    options.cancel_proc = always_cancel;
    res = run_cmd_lines(L"/c \"ping -n 8 127.0.0.1 >nul\"", &options, &capture, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_CANCELLED, "The cancellation callback stops the stream");

    /* Argument validation and spawn failures */
    WCHAR cmd[MAX_PATH];
    EXPECT_TRUE(get_cmd_exe(cmd, MAX_PATH), "ComSpec available");
    EXPECT_TRUE(restic_exec_lines(cmd, L"repo", L"pw", L"/c exit", NULL, NULL, NULL, err, _countof(err)) ==
                    RESTIC_ERR_INVALID_ARG,
                "A missing callback is refused");
    EXPECT_TRUE(restic_exec_lines(L"cmd.exe", L"repo", L"pw", L"/c exit", NULL, capture_line, &capture, err,
                                  _countof(err)) == RESTIC_ERR_SPAWN_FAILED,
                "A bare executable name is refused");
}

/* Credentials are injected, conflicting inherited variables removed, the rest inherited */
static void test_exec_environment(void) {
    WCHAR cmd[MAX_PATH];
    EXPECT_TRUE(get_cmd_exe(cmd, MAX_PATH), "ComSpec available");

    SetEnvironmentVariableW(L"RESTIC_PASSWORD", L"parent_secret");
    SetEnvironmentVariableW(L"RESTIC_PASSWORD_COMMAND", L"bogus_cmd");
    SetEnvironmentVariableW(L"RESTIC_PASSWORD_FILE", L"bogus_file");
    SetEnvironmentVariableW(L"RESTIC_REPOSITORY_FILE", L"bogus_repo_file");
    SetEnvironmentVariableW(L"RESTIC_TEST_MARKER", L"inherited_marker");

    ResticCommandOutput out;
    ResticResult res = restic_exec_command(
        cmd, L"C:\\repo dir", L"p@ss w0rd \"q\" 100%",
        L"/c \"echo [%RESTIC_REPOSITORY%][%RESTIC_PASSWORD%][%RESTIC_PASSWORD_COMMAND%]"
        L"[%RESTIC_PASSWORD_FILE%][%RESTIC_REPOSITORY_FILE%][%RESTIC_TEST_MARKER%][%SystemRoot%]\"",
        NULL, &out);

    SetEnvironmentVariableW(L"RESTIC_PASSWORD", NULL);
    SetEnvironmentVariableW(L"RESTIC_PASSWORD_COMMAND", NULL);
    SetEnvironmentVariableW(L"RESTIC_PASSWORD_FILE", NULL);
    SetEnvironmentVariableW(L"RESTIC_REPOSITORY_FILE", NULL);
    SetEnvironmentVariableW(L"RESTIC_TEST_MARKER", NULL);

    EXPECT_TRUE(res == RESTIC_OK, "Environment probe ran");
    EXPECT_TRUE(strstr(out.output, "[C:\\repo dir][p@ss w0rd \"q\" 100%]") != NULL,
                "Repository and password (spaces, quotes, percent) delivered verbatim");
    EXPECT_TRUE(strstr(out.output, "[%RESTIC_PASSWORD_COMMAND%][%RESTIC_PASSWORD_FILE%][%RESTIC_REPOSITORY_FILE%]") != NULL,
                "Conflicting inherited RESTIC_* variables are removed");
    EXPECT_TRUE(strstr(out.output, "[inherited_marker]") != NULL, "Other variables are inherited");
    EXPECT_TRUE(strstr(out.output, "[%SystemRoot%]") == NULL, "System variables are inherited");
    restic_free_command_output(&out);
}

static int g_cancel_polls = 0;
static BOOL cancel_after_first_poll(void* ctx) {
    (void)ctx;
    g_cancel_polls++;
    return TRUE;
}

/* Timeout, cancellation and spawn / argument failures */
static void test_exec_limits_and_failures(void) {
    ResticCommandOutput out;
    ResticExecOptions opts;
    ULONGLONG t0;

    memset(&opts, 0, sizeof(opts));
    opts.timeout_ms = 400;
    t0 = GetTickCount64();
    ResticResult res = run_cmd(L"/c \"ping -n 6 127.0.0.1 >nul\"", &opts, &out);
    EXPECT_TRUE(res == RESTIC_ERR_TIMEOUT, "Long-running child hits the timeout");
    EXPECT_TRUE(GetTickCount64() - t0 < 4000, "Timeout returns promptly");
    restic_free_command_output(&out);

    memset(&opts, 0, sizeof(opts));
    opts.cancel_proc = cancel_after_first_poll;
    g_cancel_polls = 0;
    t0 = GetTickCount64();
    res = run_cmd(L"/c \"ping -n 6 127.0.0.1 >nul\"", &opts, &out);
    EXPECT_TRUE(res == RESTIC_ERR_CANCELLED && g_cancel_polls >= 1, "Cancellation callback aborts the child");
    EXPECT_TRUE(GetTickCount64() - t0 < 4000, "Cancellation returns promptly");
    restic_free_command_output(&out);

    /* A generous timeout must not disturb a fast command */
    memset(&opts, 0, sizeof(opts));
    opts.timeout_ms = 30000;
    res = run_cmd(L"/c \"echo ok\"", &opts, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Fast command within timeout");
    restic_free_command_output(&out);

    res = restic_exec_command(L"C:\\no\\such\\dir\\restic.exe", L"r", L"p", L"version", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_SPAWN_FAILED, "Missing executable");
    EXPECT_TRUE(out.output == NULL, "No buffers allocated on spawn failure");
    res = restic_exec_command(L"restic.exe", L"r", L"p", L"version", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_SPAWN_FAILED, "Bare executable names are refused (no cwd search)");
    res = restic_exec_command(L"", L"r", L"p", L"version", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_SPAWN_FAILED, "Empty executable name");
    res = restic_exec_command(NULL, L"r", L"p", L"version", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "NULL executable");
    res = restic_exec_command(L"C:\\x\\restic.exe", L"r", L"p", L"version", NULL, NULL);
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "NULL result");

    WCHAR* huge = (WCHAR*)calloc(40000, sizeof(WCHAR));
    EXPECT_TRUE(huge != NULL, "Allocate");
    wmemset(huge, L'x', 39999);
    WCHAR cmd[MAX_PATH];
    EXPECT_TRUE(get_cmd_exe(cmd, MAX_PATH), "ComSpec available");
    res = restic_exec_command(cmd, L"r", L"p", huge, NULL, &out);
    free(huge);
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "Command line above the CreateProcess limit");

    restic_free_command_output(&out); /* Safe on a zeroed / already released struct */
    restic_free_command_output(NULL);
}

/* Large outputs (buffer growth) and long command lines are handled */
static void test_exec_large_output_and_long_args(void) {
    ResticCommandOutput out;

    ResticResult res = run_cmd(L"/c \"for /L %i in (1,1,4000) do @echo line_%i_0123456789_0123456789\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Large output command");
    EXPECT_TRUE(out.length > 100000, "More than 100 KB captured");
    EXPECT_TRUE(strlen(out.output) == out.length, "Length matches the NUL-terminated data");
    EXPECT_TRUE(strstr(out.output, "line_1_0123") != NULL && strstr(out.output, "line_4000_0123") != NULL,
                "First and last lines are present");
    restic_free_command_output(&out);

    WCHAR* args = (WCHAR*)calloc(6100, sizeof(WCHAR));
    EXPECT_TRUE(args != NULL, "Allocate");
    wcscpy_s(args, 6100, L"/c \"echo ");
    for (int i = 0; i < 6000; ++i) wcscat_s(args, 6100, L"x");
    wcscat_s(args, 6100, L"\"");
    res = run_cmd(args, NULL, &out);
    free(args);
    EXPECT_TRUE(res == RESTIC_OK && out.length >= 6000, "Command line beyond the old 2048-character limit");
    restic_free_command_output(&out);
}

/* ============================================================================================ */
/* Live integration test against a real restic.exe and a self-built repository                  */
/* ============================================================================================ */

static void remove_tree(const WCHAR* dir) {
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    swprintf_s(pattern, MAX_PATH, L"%ls\\*", dir);
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            WCHAR child[MAX_PATH];
            swprintf_s(child, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(child);
            } else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL); /* Restic marks pack files read-only */
                DeleteFileW(child);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static BOOL write_file(const WCHAR* dir, const WCHAR* name, DWORD size, const FILETIME* mtime) {
    WCHAR path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%ls\\%ls", dir, name);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    char chunk[1024];
    memset(chunk, 'x', sizeof(chunk));
    for (DWORD left = size; left > 0;) {
        DWORD n = left < sizeof(chunk) ? left : (DWORD)sizeof(chunk), w = 0;
        if (!WriteFile(h, chunk, n, &w, NULL) || w != n) {
            CloseHandle(h);
            return FALSE;
        }
        left -= n;
    }
    if (mtime) SetFileTime(h, NULL, NULL, mtime);
    CloseHandle(h);
    return TRUE;
}

/* Deterministic binary payload containing 0x00, 0x0A, 0x0D and 0x1A: exposes any text-mode translation */
static unsigned char pattern_byte(unsigned long i) {
    return (unsigned char)((i * 7u + i / 251u) & 0xFFu);
}

static BOOL write_pattern(const WCHAR* dir, const WCHAR* name, DWORD size) {
    WCHAR path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%ls\\%ls", dir, name);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = TRUE;
    for (DWORD i = 0; ok && i < size; ++i) {
        unsigned char b = pattern_byte(i);
        DWORD w = 0;
        ok = WriteFile(h, &b, 1, &w, NULL) && w == 1;
    }
    CloseHandle(h);
    return ok;
}

/* TRUE when the file holds exactly `size` bytes of the deterministic pattern */
static BOOL verify_pattern_file(const WCHAR* path, DWORD size) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = TRUE;
    DWORD pos = 0, got = 0;
    unsigned char buf[4096];
    while (ok && ReadFile(h, buf, sizeof(buf), &got, NULL) && got > 0) {
        for (DWORD i = 0; ok && i < got; ++i) ok = (buf[i] == pattern_byte(pos + i));
        pos += got;
    }
    CloseHandle(h);
    return ok && pos == size;
}

/* TRUE when the file content equals `text` exactly */
static BOOL file_equals(const WCHAR* path, const char* text) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    char buf[256] = {0};
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got == strlen(text) &&
              memcmp(buf, text, got) == 0;
    CloseHandle(h);
    return ok;
}

/* Number of leftover "~rp*.tmp" temporary files in dir */
static int count_partial_files(const WCHAR* dir) {
    WCHAR pattern[MAX_PATH];
    swprintf_s(pattern, MAX_PATH, L"%ls\\~rp*.tmp", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do { n++; } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static BOOL make_dir(const WCHAR* parent, const WCHAR* name, WCHAR* out_path) {
    swprintf_s(out_path, MAX_PATH, L"%ls\\%ls", parent, name);
    return CreateDirectoryW(out_path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Builds the fixture tree backed up by the live test. Returns FALSE on any I/O failure. */
static BOOL create_fixture(const WCHAR* data) {
    SYSTEMTIME st = {2020, 2, 0, 29, 12, 34, 56, 0};
    FILETIME fixed_mtime;
    SystemTimeToFileTime(&st, &fixed_mtime);
    WCHAR sub[MAX_PATH], dir[MAX_PATH];

    BOOL ok = write_file(data, L"readme.txt", 5, &fixed_mtime) &&
              write_file(data, L"with space.txt", 5, NULL) &&
              write_file(data, L"\u00fcn\u00efc\u00f6d\u00e9_\u65e5\u672c\u8a9e.txt", 7, NULL) &&
              write_file(data, L"zero.bin", 0, NULL) &&
              write_file(data, L"big.bin", 100000, NULL) &&
              make_dir(data, L"sub", sub) && write_file(sub, L"inner.txt", 3, NULL) &&
              make_dir(sub, L"sub2", dir) && write_file(dir, L"deep.txt", 4, NULL) &&
              write_pattern(dir, L"binary.dat", 70000) &&
              make_dir(data, L"empty_dir", dir) &&
              make_dir(data, L"odd [dir] & (x) ^ 100% !", sub) && write_file(sub, L"odd_inner.txt", 9, NULL) &&
              make_dir(data, L"many", dir);
    for (int i = 1; ok && i <= 300; ++i) {
        WCHAR name[128];
        swprintf_s(name, _countof(name), L"file_with_a_reasonably_long_name_to_grow_the_buffer_%04d.txt", i);
        ok = write_file(dir, name, 1, NULL);
    }
    return ok;
}

/* "C:\a\b" -> "\C\a\b": how a Windows backup path appears below the snapshot root */
static void to_wfx_path(const WCHAR* abs_path, const WCHAR* leaf, WCHAR* out, size_t cap) {
    if (leaf && leaf[0] != L'\0') {
        swprintf_s(out, cap, L"\\%c%ls\\%ls", abs_path[0], abs_path + 2, leaf);
    } else {
        swprintf_s(out, cap, L"\\%c%ls", abs_path[0], abs_path + 2);
    }
}

static ULONGLONG g_last_dump_bytes = 0;
static BOOL      g_abort_dump      = FALSE;

static BOOL dump_progress_cb(ULONGLONG bytes_written, void* user_data) {
    (void)user_data;
    g_last_dump_bytes = bytes_written;
    return g_abort_dump && (bytes_written > 0);
}

/*
 * Backups started from a network share (`restic backup \\host\share\dir`) have a root node named
 * "\\host\share". Name and ls behaviour are covered here without a network: in the test text a '|'
 * stands for one JSON-escaped backslash so the fixtures stay readable.
 */
static void bars_to_json(const char* text, char* out, size_t capacity) {
    size_t used = 0;
    for (; *text != '\0' && used + 3 < capacity; ++text) {
        if (*text == '|') {
            out[used++] = '\\';
            out[used++] = '\\';
        } else {
            out[used++] = *text;
        }
    }
    out[used] = '\0';
}

static void test_unc_volume_names(void) {
    char norm[256] = {0};

    /* Display name -> real name, only for the first segment */
    EXPECT_TRUE(restic_normalize_filter_path(L"\\\uFF3C\uFF3Chost\uFF3Cshare\\dir\\sub", norm, sizeof(norm)),
                "UNC display path");
    EXPECT_STR_EQ(norm, "/\\\\host\\share/dir/sub", "Display name maps back to the UNC volume");
    EXPECT_TRUE(restic_normalize_filter_path(L"\uFF3C\uFF3Chost\uFF3Cshare\\", norm, sizeof(norm)),
                "UNC volume alone, no leading separator");
    EXPECT_STR_EQ(norm, "/\\\\host\\share", "Volume without separators around it");
    EXPECT_TRUE(restic_normalize_filter_path(L"\\dir\uFF3C\uFF3Cx", norm, sizeof(norm)),
                "Full-width characters below the first segment");
    EXPECT_STR_EQ(norm, "/dir\xEF\xBC\xBC\xEF\xBC\xBCx", "Only the first segment is mapped");
    EXPECT_TRUE(restic_normalize_filter_path(L"\\\uFF3Chost\\x", norm, sizeof(norm)), "Single full-width character");
    EXPECT_STR_EQ(norm, "/\xEF\xBC\xBChost/x", "A real name with one U+FF3C is left alone");

    /* Old restic versions printed the volume as a child of "/": it is shown, not dropped */
    char text[1024] = {0};
    char json[1024] = {0};
    WIN32_FIND_DATAW* items = NULL;
    size_t count = 0;
    snprintf(text, sizeof(text),
             HEADER_LINE
             "{\"name\":\"||host|share\",\"type\":\"dir\",\"path\":\"/||host|share\",\"message_type\":\"node\"}\n"
             "{\"name\":\"a|b\",\"type\":\"file\",\"path\":\"/a|b\",\"message_type\":\"node\"}\n");
    bars_to_json(text, json, sizeof(json));
    ResticResult res = restic_parse_ls_ndjson(json, "/", &items, &count);
    EXPECT_TRUE(res == RESTIC_OK && count == 1, "UNC volume at the root is listed, other names with '\\' are not");
    EXPECT_WSTR_EQ(items[0].cFileName, L"\uFF3C\uFF3Chost\uFF3Cshare", "Root volume is shown with full-width solidus");
    EXPECT_TRUE(is_dir_entry(&items[0]), "UNC volume is a directory");
    free(items);

    /* Below the volume: ordinary names, direct children of the (backslash containing) filter */
    snprintf(text, sizeof(text),
             HEADER_LINE
             "{\"name\":\"||host|share\",\"type\":\"dir\",\"path\":\"/||host|share\",\"message_type\":\"node\"}\n"
             "{\"name\":\"dir\",\"type\":\"dir\",\"path\":\"/||host|share/dir\",\"message_type\":\"node\"}\n"
             "{\"name\":\"f.txt\",\"type\":\"file\",\"path\":\"/||host|share/dir/f.txt\",\"size\":3,\"message_type\":\"node\"}\n");
    bars_to_json(text, json, sizeof(json));
    res = restic_parse_ls_ndjson(json, "/\\\\host\\share", &items, &count);
    EXPECT_TRUE(res == RESTIC_OK && count == 1 && wcscmp(items[0].cFileName, L"dir") == 0,
                "Children of a UNC volume are listed with their own names");
    free(items);
}

/* Volumes recovered from `restic snapshots --json` (restic prints nothing for `ls <id> /`) */
static void test_unc_volumes_from_snapshots(void) {
    char text[1024] = {0};
    char json[1024] = {0};
    snprintf(text, sizeof(text),
             "[{\"paths\":[\"||host|share|dir\",\"C:|data\"]},"
             "{\"paths\":[\"||HOST|Share|other\",\"||?|C:|x\",\"||nohost\",\"||other|s2\",\"||h|\"]}]");
    bars_to_json(text, json, sizeof(json));

    WIN32_FIND_DATAW* items = (WIN32_FIND_DATAW*)calloc(1, sizeof(WIN32_FIND_DATAW));
    EXPECT_TRUE(items != NULL, "Allocate the existing root entry");
    wcscpy_s(items[0].cFileName, MAX_PATH, L"C");
    items[0].dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    size_t count = 1;

    EXPECT_TRUE(restic_add_unc_volumes(json, &items, &count) == RESTIC_OK, "UNC volumes are added");
    EXPECT_TRUE(count == 3, "Drive entry plus two distinct UNC volumes (duplicates, device paths and incomplete paths ignored)");
    EXPECT_TRUE(find_entry(items, count, L"\uFF3C\uFF3Chost\uFF3Cshare") != NULL, "host\\share is listed (case-insensitive duplicate dropped)");
    EXPECT_TRUE(find_entry(items, count, L"\uFF3C\uFF3Cother\uFF3Cs2") != NULL, "Second share is listed");
    EXPECT_TRUE(find_entry(items, count, L"C") != NULL, "Existing entries are kept");
    const WIN32_FIND_DATAW* volume = find_entry(items, count, L"\uFF3C\uFF3Chost\uFF3Cshare");
    EXPECT_TRUE(volume != NULL && is_dir_entry(volume) && (volume->dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0,
                "Volumes are read-only directories");

    /* Running it again changes nothing, and bad input is refused without touching the list */
    EXPECT_TRUE(restic_add_unc_volumes(json, &items, &count) == RESTIC_OK && count == 3, "Idempotent");
    EXPECT_TRUE(restic_add_unc_volumes("{\"not\":\"an array\"}", &items, &count) == RESTIC_ERR_JSON_PARSE && count == 3,
                "A non-array result is refused");
    EXPECT_TRUE(restic_add_unc_volumes("not json", &items, &count) == RESTIC_ERR_JSON_PARSE && count == 3,
                "Garbage is refused");
    EXPECT_TRUE(restic_add_unc_volumes(NULL, &items, &count) == RESTIC_ERR_INVALID_ARG, "NULL json is an invalid argument");
    free(items);

    WIN32_FIND_DATAW* none = NULL;
    size_t none_count = 0;
    EXPECT_TRUE(restic_add_unc_volumes("[{\"paths\":[\"C:/data\"]}]", &none, &none_count) == RESTIC_OK,
                "Drive-only snapshots are accepted");
    EXPECT_TRUE(none == NULL && none_count == 0, "Drive-only snapshots add nothing");
}

/*
 * Live: a backup started from a network share. Uses the administrative share of this machine
 * (\\localhost\C$\...) so no real network is needed; skipped when it cannot be reached.
 */
static void test_live_unc_share(void) {
    WCHAR restic_bin[MAX_PATH] = {0}, data_abs[MAX_PATH] = {0}, repo[MAX_PATH] = {0}, dest[MAX_PATH] = {0};
    GetFullPathNameW(L"tmp\\restic.exe", MAX_PATH, restic_bin, NULL);
    const DWORD data_len = GetFullPathNameW(L"tmp\\test_fixture\\data", MAX_PATH, data_abs, NULL);
    GetFullPathNameW(L"tmp\\test_fixture\\repo_unc", MAX_PATH, repo, NULL);
    GetFullPathNameW(L"tmp\\test_fixture\\unc_readme.txt", MAX_PATH, dest, NULL);
    if (data_len < 3 || data_len >= MAX_PATH || GetFileAttributesW(restic_bin) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(data_abs) == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] tmp\\restic.exe or the live fixture is missing: UNC share test skipped.\n");
        return;
    }

    WCHAR unc[2 * MAX_PATH];
    swprintf_s(unc, _countof(unc), L"\\\\localhost\\%c$%ls", data_abs[0], data_abs + 2);
    if (GetFileAttributesW(unc) == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] %lc$ administrative share not reachable: UNC share test skipped.\n", data_abs[0]);
        return;
    }

    const WCHAR* password = L"p@ss \"w0rd\" \u00fc %PATH% \u65e5\u672c"; /* Same as the DLL fixture */
    WCHAR err[512] = {0};
    ResticCommandOutput out;
    remove_tree(repo);
    ResticResult res = restic_exec_command(restic_bin, repo, password, L"init --json --repository-version 2", NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Init the UNC test repository");
    restic_free_command_output(&out);

    WCHAR* q_unc = restic_quote_arg(unc);
    WCHAR args[2 * MAX_PATH + 64];
    swprintf_s(args, _countof(args), L"backup %ls --json --quiet", q_unc);
    free(q_unc);
    res = restic_exec_command(restic_bin, repo, password, args, NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Backup started from a UNC path");
    restic_free_command_output(&out);

    WIN32_FIND_DATAW* items = NULL;
    size_t count = 0;
    res = restic_list_snapshots(restic_bin, repo, password, NULL, &items, &count, err, _countof(err));
    char snap[32] = {0};
    EXPECT_TRUE(res == RESTIC_OK && count == 1 && restic_extract_snapshot_id(items[0].cFileName, snap, sizeof(snap)),
                "The UNC snapshot is listed");
    free(items);
    items = NULL;

    /* Root: restic prints nothing for "/", the volume comes from the snapshot's paths */
    res = restic_list_directory(restic_bin, repo, password, snap, L"\\", NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 1, "Snapshot root lists the UNC volume");
    WCHAR volume[MAX_PATH] = {0};
    swprintf_s(volume, _countof(volume), L"\uFF3C\uFF3Clocalhost\uFF3C%c$", data_abs[0]);
    EXPECT_TRUE(_wcsicmp(items[0].cFileName, volume) == 0 && is_dir_entry(&items[0]),
                "The volume is shown as a folder named with full-width solidus characters");
    wcscpy_s(volume, _countof(volume), items[0].cFileName); /* Keep the exact spelling restic stored */
    free(items);
    items = NULL;

    /* The display name navigates: volume level, then the original directory, then a file */
    WCHAR sub[2 * MAX_PATH];
    swprintf_s(sub, _countof(sub), L"\\%ls", volume);
    res = restic_list_directory(restic_bin, repo, password, snap, sub, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count >= 1, "The UNC volume can be opened");
    free(items);
    items = NULL;

    swprintf_s(sub, _countof(sub), L"\\%ls%ls", volume, data_abs + 2);
    res = restic_list_directory(restic_bin, repo, password, snap, sub, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && find_entry(items, count, L"readme.txt") != NULL,
                "The backed up directory below the UNC volume lists its files");
    free(items);
    items = NULL;

    swprintf_s(sub, _countof(sub), L"\\%ls%ls\\readme.txt", volume, data_abs + 2);
    DeleteFileW(dest);
    res = restic_dump_file(restic_bin, repo, password, snap, sub, dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    WIN32_FILE_ATTRIBUTE_DATA fad;
    EXPECT_TRUE(res == RESTIC_OK && GetFileAttributesExW(dest, GetFileExInfoStandard, &fad) &&
                    fad.nFileSizeLow == 5 && fad.nFileSizeHigh == 0,
                "A file below the UNC volume can be extracted");
    DeleteFileW(dest);

    res = restic_list_directory(restic_bin, repo, password, snap, L"\\\uFF3C\uFF3Cnohost\uFF3Cnoshare", NULL,
                                &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND, "A volume that is not in the snapshot is reported as missing");
}

static void test_live_restic_cli(void) {
    WCHAR restic_bin[MAX_PATH], base[MAX_PATH], data[MAX_PATH], repo[MAX_PATH];
    GetFullPathNameW(L"tmp\\restic.exe", MAX_PATH, restic_bin, NULL);
    if (GetFileAttributesW(restic_bin) == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] tmp\\restic.exe not found: live Restic integration tests skipped.\n");
        return;
    }

    /* Fresh, deterministic fixture on every run (left on disk afterwards for inspection) */
    GetFullPathNameW(L"tmp\\test_fixture", MAX_PATH, base, NULL);
    remove_tree(base);
    EXPECT_TRUE(CreateDirectoryW(base, NULL), "Create fixture base directory");
    EXPECT_TRUE(make_dir(base, L"data", data) && make_dir(base, L"repo", repo), "Create data/repo directories");
    EXPECT_TRUE(create_fixture(data), "Create fixture files");

    const WCHAR* password = L"p@ss \"w0rd\" \u00fc %PATH% \u65e5\u672c";
    WCHAR err[512] = {0};
    WIN32_FIND_DATAW* items = NULL;
    size_t count = 0;
    ResticResult res;
    ResticCommandOutput out;

    /* Error mapping before the repository exists */
    res = restic_list_snapshots(restic_bin, repo, password, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_REPO_NOT_FOUND, "Missing repository -> RESTIC_ERR_REPO_NOT_FOUND");
    EXPECT_TRUE(err[0] != L'\0', "Missing repository reports a reason");

    /* A long non-ASCII repository path produces a message that does not fit the error buffer:
       the UTF-8 -> UTF-16 truncation must stop on a character boundary, so the text must never end
       with (or contain) the U+FFFD replacement character */
    {
        WCHAR long_repo[1024] = {0};
        wcscpy_s(long_repo, _countof(long_repo), L"C:\\NoSuchRepo\\");
        for (size_t i = 0; i < 300; ++i) {
            wcscat_s(long_repo, _countof(long_repo), L"\u65e5");
        }
        WCHAR long_err[512] = {0};
        res = restic_list_snapshots(restic_bin, long_repo, password, NULL, &items, &count,
                                    long_err, _countof(long_err));
        EXPECT_TRUE(res == RESTIC_ERR_REPO_NOT_FOUND, "Long non-ASCII repository path -> not found");
        EXPECT_TRUE(long_err[0] != L'\0', "Long non-ASCII repository path reports a reason");
        EXPECT_TRUE(wcschr(long_err, L'\uFFFD') == NULL,
                    "Truncated UTF-8 error text contains no replacement character");
        free(items);
        items = NULL;
        count = 0;
    }

    WCHAR* q_data = restic_quote_arg(data);
    WCHAR args[2048];
    res = restic_exec_command(restic_bin, repo, password, L"init --json --repository-version 2", NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "restic init with a password containing quotes, %PATH% and CJK text");
    restic_free_command_output(&out);

    res = restic_list_snapshots(restic_bin, repo, L"wrong password", NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD, "Wrong password -> RESTIC_ERR_WRONG_PASSWORD");
    EXPECT_TRUE(wcsstr(err, L"password") != NULL, "Wrong password reason mentions the password");

    res = restic_list_snapshots(restic_bin, repo, password, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 0 && items == NULL, "Empty repository -> no snapshots");

    /* Two snapshots, the second after changing one file */
    swprintf_s(args, _countof(args), L"backup %ls --json --quiet", q_data);
    res = restic_exec_command(restic_bin, repo, password, args, NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "First backup");
    restic_free_command_output(&out);
    EXPECT_TRUE(write_file(data, L"with space.txt", 12, NULL), "Modify fixture");
    res = restic_exec_command(restic_bin, repo, password, args, NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Second backup");
    restic_free_command_output(&out);
    free(q_data);

    res = restic_list_snapshots(restic_bin, repo, password, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 2, "Two snapshots listed");
    EXPECT_TRUE(wcscmp(items[0].cFileName, items[1].cFileName) != 0, "Snapshot directory names are unique");
    EXPECT_TRUE(ft_u64(items[0].ftLastWriteTime) <= ft_u64(items[1].ftLastWriteTime), "Oldest snapshot first");
    char snap1[32] = {0}, snap2[32] = {0};
    EXPECT_TRUE(restic_extract_snapshot_id(items[0].cFileName, snap1, sizeof(snap1)) &&
                restic_extract_snapshot_id(items[1].cFileName, snap2, sizeof(snap2)),
                "Snapshot IDs recoverable from directory names");
    free(items);

    /* Level 2: root of the snapshot holds only the drive letter */
    res = restic_list_directory(restic_bin, repo, password, snap2, L"\\", NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 1, "Snapshot root contains one entry");
    EXPECT_TRUE(is_dir_entry(&items[0]) && _wcsnicmp(items[0].cFileName, data, 1) == 0 && items[0].cFileName[1] == L'\0',
                "Root entry is the drive letter directory");
    free(items);

    WCHAR wfx[MAX_PATH * 2];
    to_wfx_path(data, NULL, wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, repo, password, snap2, wfx, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK, "List fixture directory (snapshot 2)");
    EXPECT_TRUE(count == 9, "Nine direct children (nothing from deeper levels)");
    const WIN32_FIND_DATAW* e = find_entry(items, count, L"readme.txt");
    EXPECT_TRUE(e && !is_dir_entry(e) && file_size_of(e) == 5, "readme.txt size");
    FILETIME expected;
    restic_parse_iso8601_time("2020-02-29T12:34:56Z", &expected);
    EXPECT_TRUE(e && ft_u64(e->ftLastWriteTime) == ft_u64(expected), "mtime survives the local-offset -> UTC conversion");
    e = find_entry(items, count, L"with space.txt");
    EXPECT_TRUE(e && file_size_of(e) == 12, "Snapshot 2 shows the modified size");
    e = find_entry(items, count, L"\u00fcn\u00efc\u00f6d\u00e9_\u65e5\u672c\u8a9e.txt");
    EXPECT_TRUE(e && file_size_of(e) == 7, "Unicode file name");
    e = find_entry(items, count, L"zero.bin");
    EXPECT_TRUE(e && file_size_of(e) == 0, "Zero-byte file");
    e = find_entry(items, count, L"big.bin");
    EXPECT_TRUE(e && file_size_of(e) == 100000, "big.bin size");
    EXPECT_TRUE((e = find_entry(items, count, L"sub")) != NULL && is_dir_entry(e), "Subdirectory");
    EXPECT_TRUE((e = find_entry(items, count, L"empty_dir")) != NULL && is_dir_entry(e), "Empty subdirectory");
    EXPECT_TRUE((e = find_entry(items, count, L"many")) != NULL && is_dir_entry(e), "Directory with many files");
    EXPECT_TRUE((e = find_entry(items, count, L"odd [dir] & (x) ^ 100% !")) != NULL && is_dir_entry(e),
                "Directory name with shell/glob metacharacters");
    free(items);

    /* The older snapshot still reflects the original content */
    res = restic_list_directory(restic_bin, repo, password, snap1, wfx, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 9, "List fixture directory (snapshot 1)");
    e = find_entry(items, count, L"with space.txt");
    EXPECT_TRUE(e && file_size_of(e) == 5, "Snapshot 1 keeps the original size");
    free(items);

    /* Output larger than the initial 4 KB / typical buffers: 300 NDJSON lines */
    to_wfx_path(data, L"many", wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, repo, password, snap2, wfx, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 300, "300 entries (~100 KB of NDJSON) listed");
    EXPECT_TRUE(find_entry(items, count, L"file_with_a_reasonably_long_name_to_grow_the_buffer_0300.txt") != NULL,
                "Last entry present");
    free(items);

    /* Names containing metacharacters travel through the command line intact */
    to_wfx_path(data, L"odd [dir] & (x) ^ 100% !", wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, repo, password, snap2, wfx, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 1 && wcscmp(items[0].cFileName, L"odd_inner.txt") == 0,
                "Directory with metacharacters lists its content");
    free(items);

    to_wfx_path(data, L"empty_dir", wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, repo, password, snap2, wfx, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 0, "Existing empty directory is not an error");

    /* Missing paths, files used as directories, quote injection attempts */
    to_wfx_path(data, L"does_not_exist", wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, repo, password, snap2, wfx, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND && items == NULL, "Missing directory -> RESTIC_ERR_PATH_NOT_FOUND");
    to_wfx_path(data, L"readme.txt", wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, repo, password, snap2, wfx, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND, "File used as directory -> RESTIC_ERR_PATH_NOT_FOUND");
    to_wfx_path(data, L"no\"such\" --help \"dir", wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, repo, password, snap2, wfx, NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND, "Embedded quotes cannot inject extra arguments");

    /* Snapshot ID handling */
    res = restic_list_directory(restic_bin, repo, password, "deadbeef", L"\\", NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && wcsstr(err, L"deadbeef") != NULL, "Unknown snapshot ID reports Restic's reason");
    res = restic_list_directory(restic_bin, repo, password, "--help", L"\\", NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "Option-like snapshot ID rejected before spawning");
    res = restic_list_directory(restic_bin, repo, password, "abc def", L"\\", NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "Snapshot ID with whitespace rejected");
    res = restic_list_directory(restic_bin, repo, password, NULL, L"\\", NULL, &items, &count, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "NULL snapshot ID rejected");

    /* File extraction via restic_dump_file */
    WCHAR dump_dest[MAX_PATH];
    swprintf_s(dump_dest, _countof(dump_dest), L"%ls\\dumped_file.bin", base);
    DeleteFileW(dump_dest);

    /* 1. Missing snapshot file or wrong password never creates the local file */
    to_wfx_path(data, L"no_such_file.txt", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND, "Missing file in snapshot -> RESTIC_ERR_PATH_NOT_FOUND");
    EXPECT_TRUE(GetFileAttributesW(dump_dest) == INVALID_FILE_ATTRIBUTES, "Local file not created when remote path is missing");

    to_wfx_path(data, L"readme.txt", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, repo, L"wrong_pwd", snap2, wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD, "Wrong password on dump -> RESTIC_ERR_WRONG_PASSWORD");
    EXPECT_TRUE(GetFileAttributesW(dump_dest) == INVALID_FILE_ATTRIBUTES, "Local file not created when password fails");

    /* 2. Extract readme.txt (5 bytes) and preserve mtime */
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, dump_dest, FALSE, &expected, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK, "Dump readme.txt succeeds");
    HANDLE hf = CreateFileW(dump_dest, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    EXPECT_TRUE(hf != INVALID_HANDLE_VALUE, "Extracted readme.txt exists");
    char buf[16] = {0};
    DWORD got = 0;
    FILETIME actual_ft = {0};
    BOOL read_ok = ReadFile(hf, buf, 10, &got, NULL);
    GetFileTime(hf, NULL, NULL, &actual_ft);
    CloseHandle(hf);
    EXPECT_TRUE(read_ok && got == 5 && memcmp(buf, "xxxxx", 5) == 0, "Extracted content matches readme.txt");
    EXPECT_TRUE(ft_u64(actual_ft) == ft_u64(expected), "Extracted file preserves snapshot LastWriteTime");

    /* 3. overwrite = FALSE refuses to clobber an existing file; overwrite = TRUE succeeds even on READONLY */
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, dump_dest, FALSE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_LOCAL_WRITE, "Refuses to overwrite when overwrite is FALSE");
    SetFileAttributesW(dump_dest, FILE_ATTRIBUTE_READONLY);
    to_wfx_path(data, L"big.bin", wfx, _countof(wfx));
    g_last_dump_bytes = 0;
    g_abort_dump      = FALSE;
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, dump_dest, TRUE, NULL, 0, dump_progress_cb, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK, "Streaming 100 KB file over multi-chunk pipe succeeds");
    WIN32_FILE_ATTRIBUTE_DATA big_fad;
    EXPECT_TRUE(GetFileAttributesExW(dump_dest, GetFileExInfoStandard, &big_fad) &&
                big_fad.nFileSizeLow == 100000 && big_fad.nFileSizeHigh == 0 &&
                (big_fad.dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0,
                "100 KB file is complete and the READONLY target was replaced");
    EXPECT_TRUE(g_last_dump_bytes > 0 && g_last_dump_bytes <= 100000ULL, "Progress callback reported a plausible byte count");

    /* 4. Zero-byte file extraction */
    DeleteFileW(dump_dest);
    to_wfx_path(data, L"zero.bin", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, dump_dest, FALSE, NULL, 0, NULL, NULL, err, _countof(err));
    WIN32_FILE_ATTRIBUTE_DATA fad;
    EXPECT_TRUE(res == RESTIC_OK && GetFileAttributesExW(dump_dest, GetFileExInfoStandard, &fad) &&
                fad.nFileSizeLow == 0 && fad.nFileSizeHigh == 0, "Zero-byte file created on disk");

    /* 5. User abort during transfer leaves no local file (the target did not exist before) */
    DeleteFileW(dump_dest);
    to_wfx_path(data, L"big.bin", wfx, _countof(wfx));
    g_abort_dump = TRUE;
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, dump_dest, TRUE, NULL, 0, dump_progress_cb, NULL, err, _countof(err));
    g_abort_dump = FALSE;
    EXPECT_TRUE(res == RESTIC_ERR_CANCELLED, "Dump cancelled via progress callback");
    EXPECT_TRUE(GetFileAttributesW(dump_dest) == INVALID_FILE_ATTRIBUTES, "Incomplete local file removed on abort");
    EXPECT_TRUE(count_partial_files(base) == 0, "No temporary ~rp*.tmp file is left behind after an abort");

    /* 6. Binary fidelity: 0x00/0x0A/0x0D/0x1A bytes survive the pipe byte for byte (no text-mode translation) */
    to_wfx_path(data, L"sub\\sub2\\binary.dat", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, dump_dest, FALSE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && verify_pattern_file(dump_dest, 70000), "Binary file extracted byte for byte");

    /* 7. A failed overwrite never destroys the existing target (data goes to a temporary file first) */
    to_wfx_path(data, L"no_such_file.txt", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND && verify_pattern_file(dump_dest, 70000),
                "Missing remote file keeps the existing target intact");
    to_wfx_path(data, L"big.bin", wfx, _countof(wfx));
    g_abort_dump = TRUE;
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, dump_dest, TRUE, NULL, 0, dump_progress_cb, NULL, err, _countof(err));
    g_abort_dump = FALSE;
    EXPECT_TRUE(res == RESTIC_ERR_CANCELLED && verify_pattern_file(dump_dest, 70000) && count_partial_files(base) == 0,
                "Cancelled overwrite keeps the existing target intact and removes the temporary file");
    res = restic_dump_file(restic_bin, repo, L"wrong_pwd", snap2, wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD && verify_pattern_file(dump_dest, 70000),
                "Wrong password keeps the existing target intact");

    /* 8. Local-side failures */
    to_wfx_path(data, L"readme.txt", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, dump_dest, FALSE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_LOCAL_WRITE && verify_pattern_file(dump_dest, 70000),
                "overwrite=FALSE on an existing target fails before touching it");
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, base, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_LOCAL_WRITE, "A directory as the local target is refused");
    WCHAR bad_dest[MAX_PATH];
    swprintf_s(bad_dest, _countof(bad_dest), L"%ls\\no_such_dir\\out.bin", base);
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, bad_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_LOCAL_WRITE, "Non-existent local directory -> RESTIC_ERR_LOCAL_WRITE (non-empty file)");
    to_wfx_path(data, L"zero.bin", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, bad_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_LOCAL_WRITE, "Non-existent local directory -> RESTIC_ERR_LOCAL_WRITE (zero-byte file)");
    EXPECT_TRUE(count_partial_files(base) == 0, "No temporary file after local failures");

    /* 9. Argument validation (nothing is spawned) */
    res = restic_dump_file(restic_bin, repo, password, snap2, L"\\", dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "Dumping the snapshot root is refused");
    res = restic_dump_file(restic_bin, repo, password, "--help", wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "Option-like snapshot ID refused");
    res = restic_dump_file(restic_bin, repo, password, NULL, wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "NULL snapshot ID refused");
    res = restic_dump_file(restic_bin, repo, password, snap2, wfx, L"", TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "Empty local path refused");
    res = restic_dump_file(L"restic.exe", repo, password, snap2, wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_SPAWN_FAILED, "Bare executable name refused (cwd is never searched)");
    res = restic_dump_file(restic_bin, repo, password, "deadbeef", wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && verify_pattern_file(dump_dest, 70000),
                "Unknown snapshot -> CLI failure, target intact");
    DeleteFileW(dump_dest);
}

/*
 * restic_dump_file against a fake "restic" batch file: deterministic coverage of exit codes, stderr
 * classification, partial output followed by failure, environment injection and the INACTIVITY
 * timeout, none of which can be provoked reliably with the real binary.
 * The scenario is selected by the 4th command-line argument (the snapshot path).
 */
static void test_dump_with_fake_restic(void) {
    WCHAR dir[MAX_PATH], bat[MAX_PATH], dest[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    swprintf_s(dir + wcslen(dir), MAX_PATH - wcslen(dir), L"restic_fake_%lu", GetCurrentProcessId());
    CreateDirectoryW(dir, NULL);
    swprintf_s(bat, MAX_PATH, L"%ls\\fake_restic.bat", dir);
    swprintf_s(dest, MAX_PATH, L"%ls\\out.bin", dir);

    const char* script =
        "@echo off\r\n"
        "if \"%~4\"==\"/ok\"      (<nul set /p \"=hello\" & exit /b 0)\r\n"
        "if \"%~4\"==\"/env\"     (<nul set /p \"=%RESTIC_REPOSITORY%|%RESTIC_PASSWORD%\" & exit /b 0)\r\n"
        "if \"%~4\"==\"/empty\"   exit /b 0\r\n"
        "if \"%~4\"==\"/partial\" (<nul set /p \"=partial-data\" & echo Fatal: boom 1>&2 & exit /b 1)\r\n"
        "if \"%~4\"==\"/missing\" (echo Fatal: cannot dump file: path not found in snapshot 1>&2 & exit /b 1)\r\n"
        "if \"%~4\"==\"/silent\"  exit /b 1\r\n"
        "if \"%~4\"==\"/norepo\"  (echo Fatal: repository does not exist 1>&2 & exit /b 10)\r\n"
        "if \"%~4\"==\"/locked\"  exit /b 11\r\n"
        "if \"%~4\"==\"/badpw\"   exit /b 12\r\n"
        "if \"%~4\"==\"/hang\"    (ping -n 8 127.0.0.1 >nul & exit /b 0)\r\n"
        "if \"%~4\"==\"/slow\"    (<nul set /p \"=a\" & ping -n 2 127.0.0.1 >nul & <nul set /p \"=b\" & ping -n 2 127.0.0.1 >nul & <nul set /p \"=c\" & ping -n 2 127.0.0.1 >nul & exit /b 0)\r\n"
        "exit /b 99\r\n";
    HANDLE hb = CreateFileW(bat, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    EXPECT_TRUE(hb != INVALID_HANDLE_VALUE, "Create fake restic batch file");
    DWORD w = 0;
    BOOL wrote = WriteFile(hb, script, (DWORD)strlen(script), &w, NULL);
    CloseHandle(hb);
    EXPECT_TRUE(wrote, "Write fake restic batch file");

    WCHAR err[512];
    ResticResult res;
    DeleteFileW(dest);
#define FAKE_DUMP(path, overwrite, timeout) \
    restic_dump_file(bat, L"repo-url", L"s3cret", "abcd1234", path, dest, overwrite, NULL, timeout, NULL, NULL, err, _countof(err))

    res = FAKE_DUMP(L"\\ok", FALSE, 0);
    EXPECT_TRUE(res == RESTIC_OK && file_equals(dest, "hello"), "Fake restic: stdout bytes are stored verbatim");
    EXPECT_TRUE(count_partial_files(dir) == 0, "No temporary file after success");

    res = FAKE_DUMP(L"\\env", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_OK && file_equals(dest, "repo-url|s3cret"),
                "RESTIC_REPOSITORY and RESTIC_PASSWORD reach the child through the environment");

    res = FAKE_DUMP(L"\\empty", TRUE, 0);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    EXPECT_TRUE(res == RESTIC_OK && GetFileAttributesExW(dest, GetFileExInfoStandard, &fad) && fad.nFileSizeLow == 0,
                "Exit code 0 without output replaces the target with an empty file");

    /* Failures after partial output must keep the previous target and leave nothing behind */
    DeleteFileW(dest);
    EXPECT_TRUE(FAKE_DUMP(L"\\ok", FALSE, 0) == RESTIC_OK, "Re-create target");
    res = FAKE_DUMP(L"\\partial", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && wcsstr(err, L"boom") != NULL,
                "Partial output + exit code 1 -> RESTIC_ERR_CLI_FAILED with Restic's message");
    EXPECT_TRUE(file_equals(dest, "hello") && count_partial_files(dir) == 0,
                "Partial data never replaces the target and the temporary file is removed");
    DeleteFileW(dest);
    res = FAKE_DUMP(L"\\partial", FALSE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES,
                "Failed extraction leaves no target file at all");

    res = FAKE_DUMP(L"\\missing", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND, "'not found in snapshot' on stderr -> RESTIC_ERR_PATH_NOT_FOUND");
    res = FAKE_DUMP(L"\\silent", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && err[0] == L'\0', "Exit code 1 without any output is handled (empty reason)");
    res = FAKE_DUMP(L"\\norepo", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_REPO_NOT_FOUND && err[0] != L'\0', "Exit code 10 -> RESTIC_ERR_REPO_NOT_FOUND");
    res = FAKE_DUMP(L"\\locked", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_REPO_LOCKED, "Exit code 11 -> RESTIC_ERR_REPO_LOCKED");
    res = FAKE_DUMP(L"\\badpw", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD, "Exit code 12 -> RESTIC_ERR_WRONG_PASSWORD");
    res = FAKE_DUMP(L"\\unknown", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED, "Other exit codes -> RESTIC_ERR_CLI_FAILED");

    /* Inactivity timeout: silence is punished, steady progress is not */
    ULONGLONG t0 = GetTickCount64();
    res = FAKE_DUMP(L"\\hang", TRUE, 700);
    ULONGLONG hang_ms = GetTickCount64() - t0;
    EXPECT_TRUE(res == RESTIC_ERR_TIMEOUT && hang_ms < 5000, "Silent child is killed by the inactivity timeout");
    t0 = GetTickCount64();
    res = FAKE_DUMP(L"\\slow", TRUE, 1800);
    ULONGLONG slow_ms = GetTickCount64() - t0;
    EXPECT_TRUE(res == RESTIC_OK && file_equals(dest, "abc"), "Child that keeps producing bytes survives the inactivity timeout");
    EXPECT_TRUE(slow_ms > 1800, "The whole transfer lasted longer than the timeout value (it is not a total-time limit)");
#undef FAKE_DUMP

    DeleteFileW(dest);
    DeleteFileW(bat);
    RemoveDirectoryW(dir);
}

int main(void) {
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    printf("Running Restic CLI & JSON Parser Unit/Integration Tests...\n");

    test_iso8601_parsing();
    test_snapshot_naming();
    test_path_and_quote_helpers();
    test_parse_snapshots_json();
    test_parse_ls_ndjson();
    test_unc_volume_names();
    test_unc_volumes_from_snapshots();
    test_exec_streams_and_exit_codes();
    test_exec_environment();
    test_exec_limits_and_failures();
    test_exec_large_output_and_long_args();
    test_exec_lines();
    test_dump_with_fake_restic();
    test_live_restic_cli();
    test_live_unc_share();

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
