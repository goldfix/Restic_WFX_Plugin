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
    test_exec_streams_and_exit_codes();
    test_exec_environment();
    test_exec_limits_and_failures();
    test_exec_large_output_and_long_args();
    test_live_restic_cli();

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
