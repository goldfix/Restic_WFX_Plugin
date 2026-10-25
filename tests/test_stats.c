/*
 * Unit tests for the snapshot statistics layer:
 *  - wfx_parse_path()          (plugin namespace path decomposition)
 *  - restic_parse_snapshot_list() (parser of `restic snapshots --json`)
 *  - snapshot_stats_* cache    (per-alias LRU table)
 *  - FsContent* content columns (field enumeration, values, units, default view)
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
#include "fscontent.h"
#include "restic_stats.h"
#include "wfx_path.h"
#include "wfx_trace.h"

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

#define EXPECT_I64_EQ(actual, expected, msg) do { \
    g_tests_run++; \
    if ((long long)(actual) != (long long)(expected)) { \
        printf("[FAIL] %s:%d: %s (expected %lld, got %lld)\n", \
               __FUNCTION__, __LINE__, (msg), (long long)(expected), (long long)(actual)); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

#define SNAPSHOT_DIR_FIRST_W  L"\\RepoA\\2026-10-18_16-28-15_[5e395062]"
#define SNAPSHOT_DIR_FIRST    "\\RepoA\\2026-10-18_16-28-15_[5e395062]"
#define SNAPSHOT_DIR_SECOND_W L"\\RepoA\\2026-10-19_08-00-00_[9d02b401]"
#define SNAPSHOT_DIR_SECOND   "\\RepoA\\2026-10-19_08-00-00_[9d02b401]"

/* Two usable snapshots (with and without summary), plus entries that must be skipped */
static const char kSnapshotsJson[] =
    "["
    "{\"time\":\"2026-10-18T16:28:15.182004+02:00\","
    "\"tree\":\"dd8179b25cc9cbb9\","
    "\"paths\":[\"/D/data\",\"/D/more\"],"
    "\"hostname\":\"GOLDFIX-PC\","
    "\"username\":\"goldfix\","
    "\"tags\":[\"daily\",\"home\"],"
    "\"program_version\":\"restic 0.19.1\","
    "\"summary\":{\"backup_start\":\"2026-10-18T16:28:15.182004+02:00\","
    "\"backup_end\":\"2026-10-18T16:30:15.182004+02:00\","
    "\"files_new\":10,\"files_changed\":2,\"files_unmodified\":100,"
    "\"dirs_new\":1,\"dirs_changed\":1,\"dirs_unmodified\":5,"
    "\"data_blobs\":8,\"tree_blobs\":14,"
    "\"data_added\":397928,\"data_added_packed\":11041,"
    "\"total_files_processed\":112,\"total_bytes_processed\":170333},"
    "\"id\":\"5e39506246e7f0f5490b3cf8180f17d59cab4f6c96e2b87f5d0c3bb9117ed8e5\","
    "\"short_id\":\"5e395062\"},"
    "{\"time\":\"2026-10-19T08:00:00Z\","
    "\"paths\":[\"/D/data\"],"
    "\"hostname\":\"OTHER-HOST\","
    "\"id\":\"9d02b401a6a5599dfb90af49c45a62612bf8704c22c0265102b897080ac50484\"},"
    "{\"time\":\"not-a-date\",\"id\":\"aabbccdd\"},"
    "{\"short_id\":\"00112233\"},"
    "\"an entry that is not an object\""
    "]";

/* ------------------------------------------------------------------------------------------ */
/* wfx_parse_path                                                                             */
/* ------------------------------------------------------------------------------------------ */

static void test_parse_path_root(void) {
    WfxParsedPath parsed;
    EXPECT_TRUE(wfx_parse_path(L"\\", &parsed), "Root path is parsed");
    EXPECT_TRUE(parsed.level == WFX_PATH_ROOT, "Root path has WFX_PATH_ROOT level");
    EXPECT_WSTR_EQ(parsed.repo_alias, L"", "Root path has no alias");
    EXPECT_STR_EQ(parsed.snapshot_id, "", "Root path has no snapshot ID");
}

static void test_parse_path_alias(void) {
    WfxParsedPath parsed;
    EXPECT_TRUE(wfx_parse_path(L"\\RepoA", &parsed), "Alias path is parsed");
    EXPECT_TRUE(parsed.level == WFX_PATH_REPO, "Alias path has WFX_PATH_REPO level");
    EXPECT_WSTR_EQ(parsed.repo_alias, L"RepoA", "Alias is extracted");

    EXPECT_TRUE(wfx_parse_path(L"RepoA\\\\", &parsed) && parsed.level == WFX_PATH_REPO,
                "Trailing and leading separators are tolerated");
    EXPECT_WSTR_EQ(parsed.repo_alias, L"RepoA", "Alias survives separator collapsing");
}

static void test_parse_path_snapshot(void) {
    WfxParsedPath parsed;
    EXPECT_TRUE(wfx_parse_path(SNAPSHOT_DIR_FIRST_W, &parsed), "Snapshot path is parsed");
    EXPECT_TRUE(parsed.level == WFX_PATH_SNAPSHOT_DIR, "Snapshot path has WFX_PATH_SNAPSHOT_DIR level");
    EXPECT_WSTR_EQ(parsed.repo_alias, L"RepoA", "Snapshot path keeps the alias");
    EXPECT_STR_EQ(parsed.snapshot_id, "5e395062", "Snapshot short ID is extracted");

    EXPECT_TRUE(wfx_parse_path(L"\\RepoA\\2026-10-18_16-28-15_[5e395062]\\D\\data\\f.txt", &parsed),
                "Deep path inside a snapshot is parsed");
    EXPECT_STR_EQ(parsed.snapshot_id, "5e395062", "Deep path keeps the snapshot ID");
    EXPECT_WSTR_EQ(parsed.subpath, L"\\D\\data\\f.txt", "Deep path keeps the subpath");
}

static void test_parse_path_invalid(void) {
    WfxParsedPath parsed;
    EXPECT_TRUE(!wfx_parse_path(NULL, &parsed), "NULL path is rejected");
    EXPECT_TRUE(!wfx_parse_path(L"\\RepoA", NULL), "NULL output is rejected");
    EXPECT_TRUE(wfx_parse_path(L"\\\\\\", &parsed) && parsed.level == WFX_PATH_ROOT,
                "Separators only is the root, not an alias");
    EXPECT_TRUE(!wfx_parse_path(L"\\RepoA\\no-snapshot-id", &parsed), "Missing short ID is rejected");
    EXPECT_TRUE(!wfx_parse_path(L"\\RepoA\\2026-10-18_16-28-15_[zzzz]", &parsed),
                "Non-hexadecimal snapshot ID is rejected");
    EXPECT_TRUE(!wfx_parse_path(L"\\RepoA\\2026-10-18_16-28-15_[]", &parsed), "Empty snapshot ID is rejected");
}

/* ------------------------------------------------------------------------------------------ */
/* restic_parse_snapshot_list                                                                 */
/* ------------------------------------------------------------------------------------------ */

static void test_parse_snapshot_list_fields(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    EXPECT_TRUE(restic_parse_snapshot_list(kSnapshotsJson, &items, &count) == RESTIC_OK,
                "Snapshot JSON is parsed");
    EXPECT_I64_EQ(count, 2, "Only snapshots with a valid time and ID are kept");

    const ResticSnapshotInfo* first = &items[0];
    EXPECT_STR_EQ(first->short_id, "5e395062", "short_id is taken from the JSON");
    EXPECT_STR_EQ(first->id, "5e39506246e7f0f5490b3cf8180f17d59cab4f6c96e2b87f5d0c3bb9117ed8e5",
                  "Full snapshot ID is preserved");
    EXPECT_STR_EQ(first->hostname, "GOLDFIX-PC", "Hostname is preserved");
    EXPECT_STR_EQ(first->username, "goldfix", "Username is preserved");
    EXPECT_STR_EQ(first->program_version, "restic 0.19.1", "Restic version is preserved");
    EXPECT_STR_EQ(first->tags, "daily, home", "Tags are joined with a comma");
    EXPECT_I64_EQ(first->tag_count, 2, "Tag count is reported");
    EXPECT_STR_EQ(first->paths, "/D/data; /D/more", "Source paths are joined with a semicolon");
    EXPECT_I64_EQ(first->path_count, 2, "Path count is reported");
    EXPECT_STR_EQ(first->parent, "", "Missing parent leaves an empty ID");
    EXPECT_TRUE(first->time.dwHighDateTime != 0 || first->time.dwLowDateTime != 0,
                "Snapshot time is converted to FILETIME");
    EXPECT_TRUE(first->has_summary, "Summary presence is detected");

    EXPECT_I64_EQ(first->files_new, 10, "files_new is parsed");
    EXPECT_I64_EQ(first->files_changed, 2, "files_changed is parsed");
    EXPECT_I64_EQ(first->files_unmodified, 100, "files_unmodified is parsed");
    EXPECT_I64_EQ(first->dirs_new, 1, "dirs_new is parsed");
    EXPECT_I64_EQ(first->dirs_changed, 1, "dirs_changed is parsed");
    EXPECT_I64_EQ(first->dirs_unmodified, 5, "dirs_unmodified is parsed");
    EXPECT_I64_EQ(first->data_blobs, 8, "data_blobs is parsed");
    EXPECT_I64_EQ(first->tree_blobs, 14, "tree_blobs is parsed");
    EXPECT_I64_EQ(first->data_added, 397928, "data_added is parsed");
    EXPECT_I64_EQ(first->data_added_packed, 11041, "data_added_packed is parsed");
    EXPECT_I64_EQ(first->total_files_processed, 112, "total_files_processed is parsed");
    EXPECT_I64_EQ(first->total_bytes_processed, 170333, "total_bytes_processed is parsed");
    EXPECT_I64_EQ(first->duration_seconds, 120, "Duration is backup_end minus backup_start");

    restic_free_snapshot_list(items);
}

static void test_parse_snapshot_list_without_summary(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    EXPECT_TRUE(restic_parse_snapshot_list(kSnapshotsJson, &items, &count) == RESTIC_OK &&
                    count == 2,
                "Second snapshot is parsed");

    const ResticSnapshotInfo* second = &items[1];
    EXPECT_TRUE(!second->has_summary, "Snapshot without summary is flagged");
    EXPECT_STR_EQ(second->short_id, "9d02b401", "short_id falls back to the full ID prefix");
    EXPECT_STR_EQ(second->hostname, "OTHER-HOST", "Metadata is kept without a summary");
    EXPECT_I64_EQ(second->total_bytes_processed, 0, "Missing summary leaves sizes at zero");
    EXPECT_I64_EQ(second->duration_seconds, 0, "Missing summary leaves the duration at zero");
    EXPECT_I64_EQ(second->tag_count, 0, "Missing tags leave an empty list");

    restic_free_snapshot_list(items);
}

static void test_parse_snapshot_list_edge_cases(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 123;

    EXPECT_TRUE(restic_parse_snapshot_list("null", &items, &count) == RESTIC_OK && count == 0 &&
                    items == NULL,
                "Empty repository (null) yields no records");
    EXPECT_TRUE(restic_parse_snapshot_list("[]", &items, &count) == RESTIC_OK && count == 0,
                "Empty array yields no records");
    EXPECT_TRUE(restic_parse_snapshot_list("{not json", &items, &count) == RESTIC_ERR_JSON_PARSE,
                "Malformed JSON is rejected");
    EXPECT_TRUE(restic_parse_snapshot_list("{\"a\":1}", &items, &count) == RESTIC_ERR_JSON_PARSE,
                "A JSON object is rejected");
    EXPECT_TRUE(restic_parse_snapshot_list(NULL, &items, &count) == RESTIC_ERR_INVALID_ARG,
                "NULL JSON is rejected");
    EXPECT_TRUE(restic_parse_snapshot_list("[]", NULL, &count) == RESTIC_ERR_INVALID_ARG,
                "NULL output array is rejected");
    EXPECT_TRUE(restic_parse_snapshot_list("[]", &items, NULL) == RESTIC_ERR_INVALID_ARG,
                "NULL count is rejected");
    restic_free_snapshot_list(NULL); /* Must be safe */
}

static void test_find_snapshot_by_short_id(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    EXPECT_TRUE(restic_parse_snapshot_list(kSnapshotsJson, &items, &count) == RESTIC_OK,
                "Fixture is parsed");

    EXPECT_TRUE(restic_find_snapshot_by_short_id(items, count, "5e395062") == &items[0],
                "Snapshot is found by short ID");
    EXPECT_TRUE(restic_find_snapshot_by_short_id(items, count, "5E395062") == &items[0],
                "Short ID comparison is case-insensitive");
    EXPECT_TRUE(restic_find_snapshot_by_short_id(items, count, "ffffffff") == NULL,
                "Unknown short ID returns NULL");
    EXPECT_TRUE(restic_find_snapshot_by_short_id(items, count, "") == NULL, "Empty short ID returns NULL");
    EXPECT_TRUE(restic_find_snapshot_by_short_id(NULL, 0, "5e395062") == NULL,
                "NULL array returns NULL");

    restic_free_snapshot_list(items);
}

/* ------------------------------------------------------------------------------------------ */
/* Per-alias cache                                                                            */
/* ------------------------------------------------------------------------------------------ */

static void load_fixture(ResticSnapshotInfo** out_items, size_t* out_count) {
    *out_items = NULL;
    *out_count = 0;
    if (restic_parse_snapshot_list(kSnapshotsJson, out_items, out_count) != RESTIC_OK) {
        *out_items = NULL;
        *out_count = 0;
    }
}

static void test_cache_store_and_lookup(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    load_fixture(&items, &count);

    snapshot_stats_clear();
    EXPECT_I64_EQ(snapshot_stats_cached_aliases(), 0, "Cache starts empty");
    EXPECT_TRUE(snapshot_stats_store(L"RepoA", items, count), "Snapshot records are stored");
    EXPECT_I64_EQ(snapshot_stats_cached_aliases(), 1, "One alias is cached");

    ResticSnapshotInfo found;
    memset(&found, 0, sizeof(found));
    EXPECT_TRUE(snapshot_stats_lookup(L"RepoA", "5e395062", &found), "Snapshot is found in the cache");
    EXPECT_STR_EQ(found.hostname, "GOLDFIX-PC", "Cached record carries the metadata");
    EXPECT_I64_EQ(found.total_bytes_processed, 170333, "Cached record carries the summary");

    EXPECT_TRUE(snapshot_stats_lookup(L"repoa", "5e395062", &found), "Alias lookup ignores the case");
    EXPECT_TRUE(!snapshot_stats_lookup(L"RepoB", "5e395062", &found), "Unknown alias is not found");
    EXPECT_TRUE(!snapshot_stats_lookup(L"RepoA", "ffffffff", &found), "Unknown snapshot is not found");
    EXPECT_TRUE(!snapshot_stats_lookup(L"RepoA", "", &found), "Empty short ID is not found");
    EXPECT_TRUE(!snapshot_stats_lookup(NULL, "5e395062", &found), "NULL alias is rejected");
    EXPECT_TRUE(!snapshot_stats_lookup(L"RepoA", NULL, &found), "NULL short ID is rejected");
    EXPECT_TRUE(!snapshot_stats_lookup(L"RepoA", "5e395062", NULL), "NULL output is rejected");

    restic_free_snapshot_list(items);
}

static void test_cache_replace_and_invalidate(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    load_fixture(&items, &count);

    snapshot_stats_clear();
    EXPECT_TRUE(snapshot_stats_store(L"RepoA", items, count), "First store succeeds");
    EXPECT_TRUE(snapshot_stats_store(L"RepoA", items, 1), "Second store replaces the entry");
    EXPECT_I64_EQ(snapshot_stats_cached_aliases(), 1, "Replacing does not grow the cache");

    size_t total = 0;
    ResticSnapshotInfo one;
    memset(&one, 0, sizeof(one));
    EXPECT_TRUE(snapshot_stats_lookup_alias(L"RepoA", &one, 1, &total), "Alias lookup succeeds");
    EXPECT_I64_EQ(total, 1, "Alias lookup reports the replaced record count");
    EXPECT_STR_EQ(one.short_id, "5e395062", "Truncated copy keeps the first record");

    EXPECT_TRUE(!snapshot_stats_lookup_alias(L"RepoB", &one, 1, &total) && total == 0,
                "Unknown alias reports no records");

    snapshot_stats_invalidate(L"RepoA");
    EXPECT_I64_EQ(snapshot_stats_cached_aliases(), 0, "Invalidate drops the entry");
    snapshot_stats_invalidate(L"RepoA"); /* Must be safe on a missing entry */
    snapshot_stats_invalidate(NULL);

    /* An empty snapshot list is a valid value (repository without snapshots) */
    EXPECT_TRUE(snapshot_stats_store(L"Empty", NULL, 0), "Empty record list is accepted");
    EXPECT_TRUE(!snapshot_stats_lookup(L"Empty", "5e395062", &one), "Empty record list finds nothing");
    snapshot_stats_clear();
    EXPECT_TRUE(!snapshot_stats_store(NULL, items, 0), "NULL alias is rejected on store");

    restic_free_snapshot_list(items);
}

static void test_cache_eviction(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    load_fixture(&items, &count);

    snapshot_stats_clear();
    WCHAR alias[32];
    for (int i = 0; i < 12; ++i) {
        swprintf_s(alias, _countof(alias), L"Repo%d", i);
        EXPECT_TRUE(snapshot_stats_store(alias, items, count), "Store succeeds for each alias");
    }
    const size_t cached = snapshot_stats_cached_aliases();
    EXPECT_TRUE(cached > 0 && cached <= 8, "Cache never exceeds its fixed number of slots");

    WCHAR newest[32];
    swprintf_s(newest, _countof(newest), L"Repo%d", 11);
    ResticSnapshotInfo found;
    EXPECT_TRUE(snapshot_stats_lookup(newest, "5e395062", &found),
                "The most recently stored alias survives eviction");

    snapshot_stats_clear();
    EXPECT_I64_EQ(snapshot_stats_cached_aliases(), 0, "Clear drops every entry");
    restic_free_snapshot_list(items);
}

/* ------------------------------------------------------------------------------------------ */
/* Content columns                                                                            */
/* ------------------------------------------------------------------------------------------ */

/* Returns the field index whose name is `name`, or -1 */
static int field_index(const char* name) {
    const int field_count = fscontent_field_count();
    for (int i = 0; i < field_count; ++i) {
        char field_name[64] = {0};
        char units[64]      = {0};
        if (FsContentGetSupportedField(i, field_name, units, (int)sizeof(field_name)) == ft_nomorefields) {
            break;
        }
        if (strcmp(field_name, name) == 0) return i;
    }
    return -1;
}

static void test_content_field_enumeration(void) {
    const int field_count = fscontent_field_count();
    EXPECT_TRUE(field_count >= 20, "A meaningful number of fields is published");

    char name[64]  = {0};
    char units[64] = {0};
    for (int i = 0; i < field_count; ++i) {
        name[0]  = '\0';
        units[0] = '\0';
        const int type = FsContentGetSupportedField(i, name, units, (int)sizeof(name));
        EXPECT_TRUE(type != ft_nomorefields, "Enumeration reaches every published field");
        EXPECT_TRUE(name[0] != '\0', "Field names are not empty");
        EXPECT_TRUE(strchr(name, '.') == NULL && strchr(name, '|') == NULL &&
                        strchr(name, ':') == NULL,
                    "Field names avoid the characters forbidden by the interface");
        EXPECT_TRUE(type == ft_string || type == ft_numeric_64 || type == ft_numeric_floating ||
                        type == ft_datetime || type == ft_boolean,
                    "Only implemented field types are published");
    }

    EXPECT_TRUE(FsContentGetSupportedField(field_count, name, units, (int)sizeof(name)) == ft_nomorefields,
                "Enumeration ends with ft_nomorefields");
    EXPECT_TRUE(FsContentGetSupportedField(-1, name, units, (int)sizeof(name)) == ft_nomorefields,
                "Negative index ends the enumeration");
    EXPECT_TRUE(FsContentGetSupportedField(0, NULL, NULL, 0) != ft_nomorefields,
                "NULL buffers are tolerated");

    /* Small buffers are truncated, never overrun */
    char tiny[4] = {0};
    EXPECT_TRUE(FsContentGetSupportedField(0, tiny, tiny, (int)sizeof(tiny)) != ft_nomorefields,
                "Short buffers are accepted");
    EXPECT_TRUE(strlen(tiny) < sizeof(tiny), "Truncated name stays NUL terminated");
}

static void test_content_field_units(void) {
    char units[64] = {0};
    char name[64]  = {0};

    const int size_field = field_index("size");
    EXPECT_TRUE(size_field >= 0, "Size field exists");
    FsContentGetSupportedField(size_field, name, units, (int)sizeof(name));
    EXPECT_STR_EQ(units, "bytes|kbytes|Mbytes|Gbytes", "Size field offers byte units");

    const int duration_field = field_index("duration");
    EXPECT_TRUE(duration_field >= 0, "Duration field exists");
    FsContentGetSupportedField(duration_field, name, units, (int)sizeof(name));
    EXPECT_STR_EQ(units, "s|min", "Duration field offers seconds and minutes");

    const int host_field = field_index("hostname");
    EXPECT_TRUE(host_field >= 0, "Hostname field exists");
    FsContentGetSupportedField(host_field, name, units, (int)sizeof(name));
    EXPECT_STR_EQ(units, "", "Text fields have no units");
}

static void test_content_values(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    load_fixture(&items, &count);

    snapshot_stats_clear();
    EXPECT_TRUE(snapshot_stats_store(L"RepoA", items, count), "Fixture is cached");

    char text[128] = {0};
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("hostname"), 0, text,
                                  (int)sizeof(text), 0) == ft_string,
                "Text field is served");
    EXPECT_STR_EQ(text, "GOLDFIX-PC", "Hostname value matches the snapshot");

    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("tags"), 0, text, (int)sizeof(text), 0) ==
                    ft_string,
                "Tags field is served");
    EXPECT_STR_EQ(text, "daily, home", "Tags value is the joined list");

    FILETIME when;
    memset(&when, 0, sizeof(when));
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("time"), 0, &when, (int)sizeof(when), 0) ==
                    ft_datetime,
                "Date field is served");
    EXPECT_TRUE(when.dwHighDateTime == items[0].time.dwHighDateTime &&
                    when.dwLowDateTime == items[0].time.dwLowDateTime,
                "Date value is the snapshot timestamp in UTC");

    LONGLONG number = 0;
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("files"), 0, &number,
                                  (int)sizeof(number), 0) == ft_numeric_64,
                "File count field is served");
    EXPECT_I64_EQ(number, 112, "File count value matches the summary");

    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("size"), 0, &number,
                                  (int)sizeof(number), 0) == ft_numeric_64,
                "Size field is served");
    EXPECT_I64_EQ(number, 170333, "Size in bytes matches the summary");
    FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("size"), CONTENT_UNIT_KBYTES, &number,
                      (int)sizeof(number), 0);
    EXPECT_I64_EQ(number, 166, "Size in kilobytes is scaled");
    FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("size"), CONTENT_UNIT_MBYTES, &number,
                      (int)sizeof(number), 0);
    EXPECT_I64_EQ(number, 0, "Size in megabytes is scaled");
    FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("size"), CONTENT_UNIT_GBYTES, &number,
                      (int)sizeof(number), 0);
    EXPECT_I64_EQ(number, 0, "Size in gigabytes is scaled");

    FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("data_added"), CONTENT_UNIT_KBYTES, &number,
                      (int)sizeof(number), 0);
    EXPECT_I64_EQ(number, 388, "Added data in kilobytes is scaled");

    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("duration"), 0, &number,
                                  (int)sizeof(number), 0) == ft_numeric_64,
                "Duration field is served");
    EXPECT_I64_EQ(number, 120, "Duration is reported in seconds");
    FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("duration"), 1, &number, (int)sizeof(number), 0);
    EXPECT_I64_EQ(number, 2, "Duration is reported in minutes");

    FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("avg_file_size"), 0, &number, (int)sizeof(number), 0);
    EXPECT_I64_EQ(number, 1520, "Average file size is derived from size and file count");

    double ratio = 0.0;
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("packed_ratio"), 0, &ratio,
                                  (int)sizeof(ratio), 0) == ft_numeric_floating,
                "Packing ratio field is served");
    EXPECT_TRUE(ratio > 36.0 && ratio < 36.1, "Packing ratio is data_added / data_added_packed");

    int flag = 0;
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("has_summary"), 0, &flag,
                                  (int)sizeof(flag), 0) == ft_boolean,
                "Summary flag field is served");
    EXPECT_I64_EQ(flag, 1, "Summary flag is true when the summary exists");

    snapshot_stats_clear();
    restic_free_snapshot_list(items);
}

static void test_content_values_without_summary(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    load_fixture(&items, &count);

    snapshot_stats_clear();
    EXPECT_TRUE(snapshot_stats_store(L"RepoA", items, count), "Fixture is cached");

    LONGLONG number = 0;
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_SECOND, field_index("files"), 0, &number,
                                  (int)sizeof(number), 0) == ft_fieldempty,
                "Summary-based field is empty without a summary");
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_SECOND, field_index("size"), 0, &number,
                                  (int)sizeof(number), 0) == ft_fieldempty,
                "Size is empty without a summary");
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_SECOND, field_index("backup_start"), 0, &number,
                                  (int)sizeof(number), 0) == ft_fieldempty,
                "Backup start is empty without a summary");

    char text[128] = {0};
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_SECOND, field_index("hostname"), 0, text,
                                  (int)sizeof(text), 0) == ft_string,
                "Metadata is still served without a summary");
    EXPECT_STR_EQ(text, "OTHER-HOST", "Hostname comes from the snapshot itself");

    int flag = 1;
    FsContentGetValue(SNAPSHOT_DIR_SECOND, field_index("has_summary"), 0, &flag, (int)sizeof(flag), 0);
    EXPECT_I64_EQ(flag, 0, "Summary flag is false when the summary is missing");

    snapshot_stats_clear();
    restic_free_snapshot_list(items);
}

static void test_content_values_paths(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    load_fixture(&items, &count);

    snapshot_stats_clear();
    EXPECT_TRUE(snapshot_stats_store(L"RepoA", items, count), "Fixture is cached");

    char text[128] = {0};
    /* Entries below a snapshot inherit its statistics */
    EXPECT_TRUE(FsContentGetValue("\\RepoA\\2026-10-18_16-28-15_[5e395062]\\D\\data\\file.txt",
                                  field_index("hostname"), 0, text, (int)sizeof(text), 0) == ft_string,
                "Files inside a snapshot inherit the snapshot statistics");
    EXPECT_STR_EQ(text, "GOLDFIX-PC", "Inherited value is correct");

    EXPECT_TRUE(FsContentGetValue("\\RepoA", field_index("hostname"), 0, text, (int)sizeof(text), 0) ==
                    ft_fieldempty,
                "Alias rows have no statistics");
    EXPECT_TRUE(FsContentGetValue("\\", field_index("hostname"), 0, text, (int)sizeof(text), 0) ==
                    ft_fieldempty,
                "Root row has no statistics");
    EXPECT_TRUE(FsContentGetValue(NULL, field_index("hostname"), 0, text, (int)sizeof(text), 0) ==
                    ft_fieldempty,
                "Missing path returns an empty field");
    EXPECT_TRUE(FsContentGetValue("\\RepoB\\2026-10-18_16-28-15_[5e395062]", field_index("hostname"), 0,
                                  text, (int)sizeof(text), 0) == ft_fieldempty,
                "Uncached alias returns an empty field");
    EXPECT_TRUE(FsContentGetValue("\\RepoA\\not-a-snapshot", field_index("hostname"), 0, text,
                                  (int)sizeof(text), 0) == ft_fieldempty,
                "Malformed snapshot folder returns an empty field");

    LONGLONG number = 0;
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, fscontent_field_count(), 0, &number,
                                  (int)sizeof(number), 0) == ft_nosuchfield,
                "Unknown field index returns ft_nosuchfield");
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, -1, 0, &number, (int)sizeof(number), 0) ==
                    ft_nosuchfield,
                "Negative field index returns ft_nosuchfield");
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("files"), 0, NULL,
                                  (int)sizeof(number), 0) == ft_fileerror,
                "NULL value buffer returns ft_fileerror");
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("files"), 0, &number, 4, 0) ==
                    ft_fieldempty,
                "Too small numeric buffer returns ft_fieldempty");

    char tiny[4] = {0};
    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("hostname"), 0, tiny,
                                  (int)sizeof(tiny), 0) == ft_string,
                "Short text buffer is accepted");
    EXPECT_STR_EQ(tiny, "GOL", "Short text buffer is truncated safely");

    EXPECT_TRUE(FsContentGetValue(SNAPSHOT_DIR_FIRST, field_index("hostname"), 0, text,
                                  (int)sizeof(text), CONTENT_DELAYIFSLOW) == ft_string,
                "Delayed-field flag does not change the result (nothing is delayed)");

    snapshot_stats_clear();
    restic_free_snapshot_list(items);
}

static void test_content_default_view(void) {
    /* Total Commander passes a single maxlen for all four buffers, so every buffer must be that large */
    char contents[1024] = {0};
    char headers[1024]  = {0};
    char widths[1024]   = {0};
    char options[1024]  = {0};

    EXPECT_TRUE(FsContentGetDefaultView(contents, headers, widths, options, (int)sizeof(contents)),
                "A default view is published");
    EXPECT_TRUE(strstr(contents, "[=<fs>.time]") != NULL, "Default view starts with the snapshot date");
    EXPECT_TRUE(strstr(contents, "[=<fs>.size.Mbytes]") != NULL,
                "Default view uses the megabyte unit for sizes");
    /* The plugin references its own fields with the "<fs>" self name, as stored in wincmd.ini */
    EXPECT_TRUE(strstr(contents, "[=restic.") == NULL, "Default view does not hard-code a plugin name");
    EXPECT_TRUE(strstr(contents, "\\n") != NULL, "Columns are separated by a literal backslash + n");

    int fields = 0;
    for (const char* p = contents; (p = strstr(p, "[=")) != NULL; ++p) {
        ++fields;
    }
    EXPECT_I64_EQ(fields, 7, "Default view contains seven columns");

    int header_count = 1;
    for (const char* p = headers; (p = strstr(p, "\\n")) != NULL; ++p) {
        ++header_count;
    }
    EXPECT_I64_EQ(header_count, fields, "Each column has a header");

    EXPECT_TRUE(strchr(widths, ',') != NULL, "Column widths are published");
    EXPECT_TRUE(strchr(options, '|') != NULL, "View options are published");
    EXPECT_TRUE(!FsContentGetDefaultView(NULL, NULL, NULL, NULL, 0), "Zero length buffer is rejected");

    /* NULL individual buffers are tolerated: only the non-NULL ones are filled */
    char only_contents[1024] = {0};
    EXPECT_TRUE(FsContentGetDefaultView(only_contents, NULL, NULL, NULL, (int)sizeof(only_contents)),
                "NULL option buffers are accepted");
    EXPECT_TRUE(strstr(only_contents, "[=<fs>.duration]") != NULL,
                "Last column is the backup duration");

    /* A short maxlen truncates instead of overflowing the caller's buffer */
    char tiny[16] = {0};
    EXPECT_TRUE(FsContentGetDefaultView(tiny, NULL, NULL, NULL, (int)sizeof(tiny)),
                "Short buffer is accepted");
    EXPECT_I64_EQ((int)strlen(tiny), (int)sizeof(tiny) - 1, "Short buffer is filled up to its last byte");
    EXPECT_TRUE(strncmp(tiny, "[=<fs>.time]", 12) == 0, "Short buffer keeps the first column only");
}

static void test_content_flags_and_sorting(void) {
    EXPECT_TRUE(FsContentGetSupportedFieldFlags(-1) == 0, "No global field flags are advertised");
    EXPECT_TRUE(FsContentGetSupportedFieldFlags(0) == 0, "Fields are read-only");
    EXPECT_TRUE(FsContentGetSupportedFieldFlags(fscontent_field_count()) == 0,
                "Out-of-range index returns no flags");

    EXPECT_TRUE(FsContentGetDefaultSortOrder(field_index("time")) == -1,
                "Snapshot date sorts newest first by default");
    EXPECT_TRUE(FsContentGetDefaultSortOrder(field_index("size")) == -1,
                "Size sorts largest first by default");
    EXPECT_TRUE(FsContentGetDefaultSortOrder(field_index("hostname")) == 1,
                "Text fields sort ascending by default");
}

/* ------------------------------------------------------------------------------------------ */
/* Review hardening tests (code review 4)                                                     */
/* ------------------------------------------------------------------------------------------ */

/* The statistics parser must keep exactly the entries the directory listing parser keeps */
static void test_parsers_stay_index_aligned(void) {
    static const char json[] =
        "["
        "{\"time\":\"2026-10-18T16:28:15Z\",\"id\":\"zzzz1234aaaa\"},"           /* non-hex ID */
        "{\"time\":\"2026-10-18T16:28:16Z\",\"short_id\":\"\",\"id\":\"abcdef12\"}," /* ID fallback */
        "{\"time\":\"2026-10-18T16:28:17Z\",\"short_id\":\"12ab\\u00e9\"},"          /* non-ASCII */
        "{\"time\":\"2026-13-45T25:61:61Z\",\"id\":\"aabbccdd\"},"                   /* bad date */
        "{\"time\":\"2026-10-18T16:28:18Z\",\"id\":\"AABBCCDD11\"}"
        "]";

    ResticSnapshotInfo* stats = NULL;
    size_t stats_count        = 0;
    WIN32_FIND_DATAW* listing = NULL;
    size_t listing_count      = 0;
    EXPECT_TRUE(restic_parse_snapshot_list(json, &stats, &stats_count) == RESTIC_OK,
                "Statistics parser accepts the mixed input");
    EXPECT_TRUE(restic_parse_snapshots_json(json, &listing, &listing_count) == RESTIC_OK,
                "Listing parser accepts the mixed input");
    EXPECT_I64_EQ(listing_count, 2, "Listing keeps only the two valid snapshots");
    EXPECT_I64_EQ(stats_count, listing_count, "Both parsers keep the same number of entries");

    for (size_t i = 0; i < stats_count; ++i) {
        WCHAR wide_id[16] = {0};
        for (size_t c = 0; c < strlen(stats[i].short_id) && c < 15; ++c) {
            wide_id[c] = (WCHAR)stats[i].short_id[c];
        }
        EXPECT_TRUE(wcsstr(listing[i].cFileName, wide_id) != NULL,
                    "Entry i of the statistics matches folder i of the listing");
    }

    free(listing);
    restic_free_snapshot_list(stats);
}

/* Absurd numbers must neither be undefined behaviour nor turn negative in Total Commander */
static void test_counters_are_saturated(void) {
    static const char json[] =
        "[{\"time\":\"2026-10-18T16:28:15Z\",\"id\":\"aabbccdd\",\"summary\":{"
        "\"total_bytes_processed\":1e30,"
        "\"total_files_processed\":18446744073709551615,"
        "\"files_new\":9223372036854775807,"
        "\"data_added\":-5,"
        "\"data_blobs\":\"7\","
        "\"tree_blobs\":12}}]";

    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    EXPECT_TRUE(restic_parse_snapshot_list(json, &items, &count) == RESTIC_OK && count == 1,
                "Snapshot with extreme counters is parsed");
    EXPECT_TRUE(items[0].total_bytes_processed == RESTIC_STATS_COUNTER_MAX,
                "1e30 bytes saturates at the signed 64-bit maximum");
    EXPECT_TRUE(items[0].total_files_processed == RESTIC_STATS_COUNTER_MAX,
                "2^64-1 files saturates at the signed 64-bit maximum");
    EXPECT_TRUE(items[0].files_new == RESTIC_STATS_COUNTER_MAX, "2^63-1 stays at the maximum");
    EXPECT_I64_EQ(items[0].data_added, 0, "Negative counters are ignored");
    EXPECT_I64_EQ(items[0].data_blobs, 0, "Counters stored as strings are ignored");
    EXPECT_I64_EQ(items[0].tree_blobs, 12, "Ordinary counters are unaffected");

    snapshot_stats_clear();
    EXPECT_TRUE(snapshot_stats_store(L"Sat", items, count), "Extreme record is cached");
    const char* path = "\\Sat\\2026-10-18_16-28-15_[aabbccdd]";

    LONGLONG value = 0;
    EXPECT_TRUE(FsContentGetValue((char*)path, field_index("size"), 0, &value, (int)sizeof(value), 0) ==
                    ft_numeric_64,
                "Saturated size is published");
    EXPECT_TRUE(value > 0, "Saturated size never turns negative");
    EXPECT_TRUE(FsContentGetValue((char*)path, field_index("files"), 0, &value, (int)sizeof(value), 0) ==
                    ft_numeric_64 && value > 0,
                "Saturated file count never turns negative");
    EXPECT_TRUE(FsContentGetValue((char*)path, field_index("avg_file_size"), 0, &value,
                                  (int)sizeof(value), 0) == ft_numeric_64 && value == 1,
                "Average of two saturated values is computed without overflow");

    snapshot_stats_clear();
    restic_free_snapshot_list(items);
}

/* Very long tag lists are cut safely while the element count stays exact */
static void test_long_lists_are_truncated(void) {
    char json[8192];
    int len = snprintf(json, sizeof(json),
                       "[{\"time\":\"2026-10-18T16:28:15Z\",\"id\":\"aabbccdd\",\"tags\":[");
    for (int i = 0; i < 200; ++i) {
        len += snprintf(json + len, sizeof(json) - (size_t)len, "%s\"tag-%03d\"", i ? "," : "", i);
    }
    snprintf(json + len, sizeof(json) - (size_t)len, "]}]");

    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    EXPECT_TRUE(restic_parse_snapshot_list(json, &items, &count) == RESTIC_OK && count == 1,
                "Snapshot with 200 tags is parsed");
    EXPECT_I64_EQ(items[0].tag_count, 200, "Every tag is counted even when the text is cut");
    size_t used = strlen(items[0].tags);
    EXPECT_TRUE(used > 0 && used < RESTIC_STATS_TAGS_MAX, "Joined tags fit the fixed buffer");
    EXPECT_TRUE(strncmp(items[0].tags, "tag-000, tag-001, tag-002", 25) == 0,
                "Tags keep their order and separator");
    EXPECT_TRUE(items[0].tags[used - 1] != ' ' && items[0].tags[used - 1] != ',',
                "The cut never leaves a dangling separator");

    snapshot_stats_clear();
    EXPECT_TRUE(snapshot_stats_store(L"Tags", items, count), "Record is cached");
    char narrow[64] = {0};
    EXPECT_TRUE(FsContentGetValue("\\Tags\\2026-10-18_16-28-15_[aabbccdd]", field_index("tags"), 0,
                                  narrow, (int)sizeof(narrow), 0) == ft_string,
                "Long tag list is served into a small buffer");
    EXPECT_I64_EQ(strlen(narrow), sizeof(narrow) - 1, "Small buffer is filled and terminated");

    snapshot_stats_clear();
    restic_free_snapshot_list(items);
}

/* Restic prints UTF-8; the ANSI-only content interface must receive the active code page */
static void test_text_fields_use_the_ansi_code_page(void) {
    static const char json[] =
        "[{\"time\":\"2026-10-18T16:28:15Z\",\"id\":\"aabbccdd\","
        "\"hostname\":\"Jos\\u00e9-PC\",\"tags\":[\"\\u65e5\\u672c\"],"
        "\"paths\":[\"/C/Users/Jos\\u00e9\"]}]";
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    EXPECT_TRUE(restic_parse_snapshot_list(json, &items, &count) == RESTIC_OK && count == 1,
                "Snapshot with non-ASCII text is parsed");
    snapshot_stats_clear();
    EXPECT_TRUE(snapshot_stats_store(L"Uni", items, count), "Record is cached");
    const char* path = "\\Uni\\2026-10-18_16-28-15_[aabbccdd]";

    char host[64] = {0};
    EXPECT_TRUE(FsContentGetValue((char*)path, field_index("hostname"), 0, host, (int)sizeof(host), 0) ==
                    ft_string,
                "Non-ASCII host name is served");
    char expected[64] = {0};
    WideCharToMultiByte(CP_ACP, 0, L"Jos\u00e9-PC", -1, expected, (int)sizeof(expected), NULL, NULL);
    EXPECT_STR_EQ(host, expected, "Host name is converted from UTF-8 to the ANSI code page");
    if (GetACP() == 1252) {
        EXPECT_TRUE((unsigned char)host[3] == 0xE9, "On Windows-1252 the accent is the single byte 0xE9");
    }

    char tags[64] = {0};
    EXPECT_TRUE(FsContentGetValue((char*)path, field_index("tags"), 0, tags, (int)sizeof(tags), 0) ==
                    ft_string && tags[0] != '\0',
                "Tag outside the code page is still served");
    WideCharToMultiByte(CP_ACP, 0, L"\u65e5\u672c", -1, expected, (int)sizeof(expected), NULL, NULL);
    EXPECT_STR_EQ(tags, expected, "Unrepresentable characters follow the code page conversion rules");

    /* A buffer too small for the converted text is cut on a character boundary */
    char tiny[5] = {0};
    EXPECT_TRUE(FsContentGetValue((char*)path, field_index("paths"), 0, tiny, (int)sizeof(tiny), 0) ==
                    ft_string,
                "Path is served into a tiny buffer");
    EXPECT_TRUE(strlen(tiny) <= sizeof(tiny) - 1 && strncmp(tiny, "/C/U", 4) == 0,
                "Tiny buffer receives a terminated prefix of the path");

    char one[1] = {'x'};
    EXPECT_TRUE(FsContentGetValue((char*)path, field_index("hostname"), 0, one, 1, 0) == ft_fieldempty,
                "A one byte buffer cannot hold text and returns ft_fieldempty");

    snapshot_stats_clear();
    restic_free_snapshot_list(items);
}

/* Aliases outside the ANSI code page reach the content columns as '?' and must still resolve */
static void test_cache_ansi_alias_fallback(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    load_fixture(&items, &count);
    ResticSnapshotInfo* other = (ResticSnapshotInfo*)malloc(count * sizeof(*other));
    EXPECT_TRUE(other != NULL, "Second record set is allocated");
    memcpy(other, items, count * sizeof(*other));
    for (size_t i = 0; i < count; ++i) {
        strcpy_s(other[i].hostname, sizeof(other[i].hostname), "SECOND-HOST");
    }

    /* Two aliases that differ only by characters no ANSI code page can represent (emoji) */
    const WCHAR alias_a[] = L"Emoji\xD83D\xDE00";
    const WCHAR alias_b[] = L"Emoji\xD83D\xDE01";
    const WCHAR dir[]     = L"2026-10-18_16-28-15_[5e395062]";
    WCHAR wide_path[MAX_PATH] = {0};
    char ansi_path[MAX_PATH]  = {0};
    swprintf_s(wide_path, MAX_PATH, L"\\%ls\\%ls", alias_a, dir);
    WideCharToMultiByte(CP_ACP, 0, wide_path, -1, ansi_path, (int)sizeof(ansi_path), NULL, NULL);

    snapshot_stats_clear();
    EXPECT_TRUE(snapshot_stats_store(alias_a, items, count), "First Unicode alias is cached");
    char host[64] = {0};
    EXPECT_TRUE(FsContentGetValue(ansi_path, field_index("hostname"), 0, host, (int)sizeof(host), 0) ==
                    ft_string,
                "ANSI spelling of a Unicode alias finds its statistics");
    EXPECT_STR_EQ(host, "GOLDFIX-PC", "... and they are the right ones");

    EXPECT_TRUE(snapshot_stats_store(alias_b, other, count), "Second Unicode alias is cached");
    ResticSnapshotInfo info;
    EXPECT_TRUE(snapshot_stats_lookup(alias_a, "5e395062", &info) && strcmp(info.hostname, "GOLDFIX-PC") == 0,
                "Exact lookup of the first alias is unaffected");
    EXPECT_TRUE(snapshot_stats_lookup(alias_b, "5e395062", &info) && strcmp(info.hostname, "SECOND-HOST") == 0,
                "Exact lookup of the second alias is unaffected");

    if (GetACP() != CP_UTF8) {
        host[0] = '\0';
        EXPECT_TRUE(FsContentGetValue(ansi_path, field_index("hostname"), 0, host, (int)sizeof(host), 0) ==
                        ft_fieldempty,
                    "An ambiguous ANSI spelling is refused instead of showing the wrong repository");
    }

    snapshot_stats_invalidate(alias_b);
    EXPECT_TRUE(FsContentGetValue(ansi_path, field_index("hostname"), 0, host, (int)sizeof(host), 0) ==
                    ft_string && strcmp(host, "GOLDFIX-PC") == 0,
                "Once the ambiguity is gone the remaining alias resolves again");

    /* Storing must never rewrite another alias through the degraded spelling */
    EXPECT_TRUE(snapshot_stats_store(L"Emoji?", other, count), "Alias that looks like a degraded name is cached");
    EXPECT_TRUE(snapshot_stats_lookup(alias_a, "5e395062", &info) && strcmp(info.hostname, "GOLDFIX-PC") == 0,
                "A literal '?' alias does not overwrite the Unicode one");
    EXPECT_I64_EQ(snapshot_stats_cached_aliases(), 2, "Both aliases occupy their own slot");

    snapshot_stats_clear();
    free(other);
    restic_free_snapshot_list(items);
}

/* Readers (shared lock) race with a writer that keeps replacing and dropping entries */
typedef struct {
    const ResticSnapshotInfo* items;
    size_t count;
    volatile LONG stop;
    volatile LONG inconsistent;
    volatile LONG hits;
} StressContext;

static DWORD WINAPI stress_writer(LPVOID arg) {
    StressContext* ctx = (StressContext*)arg;
    while (InterlockedCompareExchange(&ctx->stop, 0, 0) == 0) {
        snapshot_stats_store(L"RepoA", ctx->items, ctx->count);
        snapshot_stats_store(L"RepoB", ctx->items, ctx->count);
        snapshot_stats_invalidate(L"RepoA");
    }
    return 0;
}

static DWORD WINAPI stress_reader(LPVOID arg) {
    StressContext* ctx = (StressContext*)arg;
    for (int i = 0; i < 4000; ++i) {
        ResticSnapshotInfo info;
        if (snapshot_stats_lookup(L"RepoA", "5e395062", &info)) {
            InterlockedIncrement(&ctx->hits);
            if (strcmp(info.hostname, "GOLDFIX-PC") != 0 || info.total_files_processed != 112) {
                InterlockedIncrement(&ctx->inconsistent);
            }
        }
        char host[64] = {0};
        int type = FsContentGetValue("\\RepoB\\2026-10-18_16-28-15_[5e395062]", field_index("hostname"),
                                     0, host, (int)sizeof(host), 0);
        if (type == ft_string && strcmp(host, "GOLDFIX-PC") != 0) {
            InterlockedIncrement(&ctx->inconsistent);
        }
    }
    return 0;
}

static void test_cache_concurrent_access(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    load_fixture(&items, &count);
    snapshot_stats_clear();

    StressContext ctx = {items, count, 0, 0, 0};
    HANDLE threads[5] = {NULL, NULL, NULL, NULL, NULL}; /* [0] = writer, [1..4] = readers */
    int started       = 0;
    for (int i = 0; i < 5; ++i) {
        threads[i] = CreateThread(NULL, 0, (i == 0) ? stress_writer : stress_reader, &ctx, 0, NULL);
        if (threads[i] != NULL) ++started;
    }

    /* Readers finish by themselves; the writer runs until they are done */
    if (started == 5) {
        WaitForMultipleObjects(4, &threads[1], TRUE, 60000);
    }
    InterlockedExchange(&ctx.stop, 1);
    for (int i = 0; i < 5; ++i) {
        if (threads[i] != NULL) {
            WaitForSingleObject(threads[i], 60000);
            CloseHandle(threads[i]);
        }
    }
    EXPECT_I64_EQ(started, 5, "Writer and four reader threads start");
    EXPECT_I64_EQ(ctx.inconsistent, 0, "Readers never observe a torn or mismatching record");
    ResticSnapshotInfo info;
    EXPECT_TRUE(snapshot_stats_store(L"RepoA", items, count) &&
                    snapshot_stats_lookup(L"RepoA", "5e395062", &info),
                "The cache is still fully functional after the stress run");

    snapshot_stats_clear();
    restic_free_snapshot_list(items);
}

static void test_content_unloading_clears_cache(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    load_fixture(&items, &count);

    snapshot_stats_clear();
    EXPECT_TRUE(snapshot_stats_store(L"RepoA", items, count), "Fixture is cached");
    FsContentPluginUnloading();
    EXPECT_I64_EQ(snapshot_stats_cached_aliases(), 0, "Unloading the plugin clears the cache");

    FsContentStopGetValue("\\RepoA");
    FsContentStopGetValue(NULL);
    g_tests_run++; /* Reaching this line means the no-op entry points did not crash */

    restic_free_snapshot_list(items);
}

/* Path spellings Total Commander may use for content fields must all resolve to the same record */
static void test_content_path_spellings(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    load_fixture(&items, &count);

    snapshot_stats_clear();
    EXPECT_TRUE(snapshot_stats_store(L"RepoA", items, count), "Fixture is cached");

    static const char* kAccepted[] = {
        "\\RepoA\\2026-10-18_16-28-15_[5e395062]",
        "RepoA\\2026-10-18_16-28-15_[5e395062]",
        "\\\\Restic Backups\\RepoA\\2026-10-18_16-28-15_[5e395062]",
        "\\\\\\Restic Backups\\RepoA\\2026-10-18_16-28-15_[5e395062]\\D\\data\\file.txt",
        "\\Restic Backups\\RepoA\\2026-10-18_16-28-15_[5e395062]\\D",
        "2026-10-18_16-28-15_[5e395062]",
        "2026-10-18_16-28-15_[5e395062]\\D\\data\\file.txt",
        "\\RepoA\\2026-10-18_16-28-15_[5e395062]\\",
    };

    for (size_t i = 0; i < _countof(kAccepted); ++i) {
        char text[128] = {0};
        const int result =
            FsContentGetValue((char*)kAccepted[i], field_index("hostname"), 0, text, (int)sizeof(text), 0);
        if (result != ft_string || strcmp(text, "GOLDFIX-PC") != 0) {
            printf("[FAIL] %s:%d: path '%s' resolved to %d ('%s')\n",
                   __FUNCTION__, __LINE__, kAccepted[i], result, text);
            g_tests_run++;
            g_tests_failed++;
            snapshot_stats_clear();
            restic_free_snapshot_list(items);
            return;
        }
        g_tests_run++;
    }

    /* The canonical form keeps its alias: a snapshot of another repository stays empty */
    char text[128] = {0};
    EXPECT_TRUE(FsContentGetValue("\\RepoB\\2026-10-18_16-28-15_[5e395062]", field_index("hostname"), 0,
                                  text, (int)sizeof(text), 0) == ft_fieldempty,
                "Canonical path of an uncached alias stays empty");

    snapshot_stats_clear();
    restic_free_snapshot_list(items);
}

/* Cross-repository lookup by ID: only an unambiguous match may be used */
static void test_cache_lookup_any(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    load_fixture(&items, &count);

    snapshot_stats_clear();
    ResticSnapshotInfo found;
    memset(&found, 0, sizeof(found));
    EXPECT_TRUE(!snapshot_stats_lookup_any("5e395062", &found), "Empty cache has no match");

    EXPECT_TRUE(snapshot_stats_store(L"RepoA", items, count), "RepoA is cached");
    EXPECT_TRUE(snapshot_stats_lookup_any("5e395062", &found), "Unique snapshot is found");
    EXPECT_STR_EQ(found.hostname, "GOLDFIX-PC", "Cross-repository match returns the record");
    EXPECT_TRUE(!snapshot_stats_lookup_any("ffffffff", &found), "Unknown ID is not found");
    EXPECT_TRUE(!snapshot_stats_lookup_any("", &found), "Empty ID is not found");
    EXPECT_TRUE(!snapshot_stats_lookup_any(NULL, &found), "NULL ID is rejected");
    EXPECT_TRUE(!snapshot_stats_lookup_any("5e395062", NULL), "NULL output is rejected");

    /* The same repository cached under a second alias keeps the answer unambiguous */
    EXPECT_TRUE(snapshot_stats_store(L"RepoA-Mirror", items, count), "Same records under a second alias");
    EXPECT_TRUE(snapshot_stats_lookup_any("5e395062", &found),
                "Identical records under two aliases are not ambiguous");
    EXPECT_STR_EQ(found.hostname, "GOLDFIX-PC", "Duplicate match returns the record");

    /* Two different snapshots sharing a short ID must be rejected */
    ResticSnapshotInfo clash[1];
    memset(clash, 0, sizeof(clash));
    _snprintf_s(clash[0].id, sizeof(clash[0].id), _TRUNCATE, "ffffffff%056d", 1);
    _snprintf_s(clash[0].short_id, sizeof(clash[0].short_id), _TRUNCATE, "5e395062");
    strcpy_s(clash[0].hostname, sizeof(clash[0].hostname), "OTHER");
    EXPECT_TRUE(snapshot_stats_store(L"RepoC", clash, 1), "Conflicting snapshot is cached");
    EXPECT_TRUE(!snapshot_stats_lookup_any("5e395062", &found),
                "Same short ID with different IDs is ambiguous");

    char description[512] = {0};
    EXPECT_I64_EQ(snapshot_stats_describe(description, sizeof(description)), 3,
                  "Describe reports every cached alias");
    EXPECT_TRUE(strstr(description, "RepoA(2) ") != NULL, "Describe lists the alias and its count");
    EXPECT_TRUE(strstr(description, "RepoC(1) ") != NULL, "Describe lists every alias");
    EXPECT_STR_EQ(description, "RepoA(2) RepoA-Mirror(2) RepoC(1) ", "Describe format");
    EXPECT_I64_EQ(snapshot_stats_describe(NULL, 10), 0, "Describe rejects a NULL buffer");
    EXPECT_I64_EQ(snapshot_stats_describe(description, 0), 0, "Describe rejects a zero capacity");

    snapshot_stats_clear();
    restic_free_snapshot_list(items);
}

/* The diagnostic trace is opt-in: without RESTIC_WFX_TRACE (or the switch file) nothing is written */
static void trace_file_path(WCHAR* out, size_t capacity) {
    wfx_trace_file_path(out, capacity);
    if (out[0] != L'\0') return;

    /* The module returns nothing while the trace is disabled: rebuild the expected path */
    WCHAR temp_path[MAX_PATH] = {0};
    out[0]                    = L'\0';
    if (GetTempPathW((DWORD)capacity, temp_path) == 0) return;
    swprintf_s(out, capacity, L"%lsrestic_wfx_trace.log", temp_path);
}

static void read_trace_into(char* log, size_t capacity) {
    WCHAR path[MAX_PATH] = {0};
    trace_file_path(path, _countof(path));
    log[0]   = '\0';
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD read = 0;
    if (!ReadFile(f, log, (DWORD)capacity - 1, &read, NULL)) read = 0;
    CloseHandle(f);
    log[read] = '\0';
}

static void test_content_trace_is_opt_in(void) {
    WCHAR path[MAX_PATH] = {0};
    trace_file_path(path, _countof(path));
    EXPECT_TRUE(path[0] != L'\0', "Temporary folder is available");

    /* Disabled: an unresolved path must not create the trace file */
    DeleteFileW(path);
    SetEnvironmentVariableW(L"RESTIC_WFX_TRACE", NULL);
    fscontent_trace_reset();
    char text[64] = {0};
    EXPECT_TRUE(FsContentGetValue("\\NoSuchRepo\\no-snapshot", field_index("hostname"), 0, text,
                                  (int)sizeof(text), 0) == ft_fieldempty,
                "Unresolved path stays empty");
    EXPECT_TRUE(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES,
                "No trace file is written when RESTIC_WFX_TRACE is not set");

    /* Enabled: the call must be recorded, with the path Total Commander passed */
    EXPECT_TRUE(SetEnvironmentVariableW(L"RESTIC_WFX_TRACE", L"1") != 0, "Trace can be enabled");
    fscontent_trace_reset();
    EXPECT_TRUE(wfx_trace_enabled(), "Trace reports that it is enabled");
    wfx_trace_init(NULL);
    FsContentGetValue("\\NoSuchRepo\\no-snapshot", field_index("hostname"), 0, text,
                      (int)sizeof(text), 0);
    FsContentGetValue("\\RepoA\\2026-10-18_16-28-15_[5e395062]", field_index("tags"), 0, text,
                      (int)sizeof(text), 0);

    char log[8192] = {0};
    read_trace_into(log, sizeof(log));
    EXPECT_TRUE(log[0] != '\0', "Trace file is created when enabled");
    EXPECT_TRUE(strstr(log, "restic wfx trace") != NULL, "Trace starts with a session header");
    EXPECT_TRUE(strstr(log, "\\NoSuchRepo\\no-snapshot") != NULL,
                "Trace records the path passed by Total Commander");
    EXPECT_TRUE(strstr(log, "cache: [") != NULL, "Trace records the state of the cache");
    EXPECT_TRUE(strstr(log, "field=") != NULL, "Trace records the requested field");
    EXPECT_TRUE(strstr(log, "-> -3") != NULL, "Trace records the ft_fieldempty result code");

    /* "0" disables the trace again, exactly like an unset variable */
    SetEnvironmentVariableW(L"RESTIC_WFX_TRACE", L"0");
    wfx_trace_reset();
    EXPECT_TRUE(!wfx_trace_enabled(), "Value \"0\" disables the trace");

    DeleteFileW(path);
    SetEnvironmentVariableW(L"RESTIC_WFX_TRACE", NULL);
    fscontent_trace_reset();
}

/* The switch file next to the plugin DLL enables the trace without touching the environment */
static void test_trace_switch_file(void) {
    wfx_trace_init(GetModuleHandleW(NULL)); /* Registers the directory of this executable */
    WCHAR dir[MAX_PATH]  = {0};
    WCHAR flag[MAX_PATH] = {0};
    DWORD length         = GetModuleFileNameW(NULL, dir, (DWORD)_countof(dir));
    EXPECT_TRUE(length > 0 && length < _countof(dir), "Executable path is available");
    for (DWORD i = length; i > 0; --i) {
        if (dir[i - 1] == L'\\' || dir[i - 1] == L'/') {
            dir[i] = L'\0'; /* Keep the trailing separator */
            break;
        }
    }
    EXPECT_TRUE(swprintf_s(flag, _countof(flag), L"%lsrestic_wfx_trace.on", dir) > 0,
                "Switch file path can be built");

    SetEnvironmentVariableW(L"RESTIC_WFX_TRACE", NULL);
    DeleteFileW(flag); /* Leftover of a previously interrupted run */
    wfx_trace_reset();
    EXPECT_TRUE(!wfx_trace_enabled(), "Trace is off without the switch file");

    HANDLE file = CreateFileW(flag, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE, "Switch file can be created");
    CloseHandle(file);
    wfx_trace_reset();
    EXPECT_TRUE(wfx_trace_enabled(), "Switch file enables the trace");

    DeleteFileW(flag);
    wfx_trace_reset();
    EXPECT_TRUE(!wfx_trace_enabled(), "Removing the switch file disables the trace again");
}

/* Unicode entry points: wide text (no code page loss) and the wide default view */
static void test_content_unicode_values(void) {
    ResticSnapshotInfo* items = NULL;
    size_t count              = 0;
    load_fixture(&items, &count);

    /* A host name outside the ANSI code page survives only through the W entry points */
    ResticSnapshotInfo utf8[1];
    memset(utf8, 0, sizeof(utf8));
    _snprintf_s(utf8[0].id, sizeof(utf8[0].id), _TRUNCATE, "%064d", 7);
    _snprintf_s(utf8[0].short_id, sizeof(utf8[0].short_id), _TRUNCATE, "abcdef01");
    strcpy_s(utf8[0].hostname, sizeof(utf8[0].hostname), "Jos\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC");
    utf8[0].has_summary = TRUE;

    snapshot_stats_clear();
    EXPECT_TRUE(snapshot_stats_store(L"RepoA", items, count), "Fixture is cached");
    EXPECT_TRUE(snapshot_stats_store(L"RepoU", utf8, 1), "UTF-8 record is cached");

    WCHAR wide[128] = {0};
    EXPECT_TRUE(FsContentGetValueW(L"\\RepoA\\2026-10-18_16-28-15_[5e395062]", 1, 0, wide,
                                   (int)sizeof(wide), 0) == ft_stringw,
                "Unicode text field reports ft_stringw");
    EXPECT_TRUE(wcscmp(wide, L"GOLDFIX-PC") == 0, "Unicode text field carries the host name");

    EXPECT_TRUE(FsContentGetValueW(L"\\RepoU\\2026-10-18_16-28-15_[abcdef01]", 1, 0, wide,
                                   (int)sizeof(wide), 0) == ft_stringw,
                "Unicode text of a non-ANSI host name is returned");
    EXPECT_TRUE(wcscmp(wide, L"Jos\x00E9 \x65E5\x672C") == 0,
                "Unicode text keeps the characters the ANSI code page cannot represent");

    /* Numeric and date fields are identical in both variants */
    LONGLONG numeric = 0;
    EXPECT_TRUE(FsContentGetValueW(L"\\RepoA\\2026-10-18_16-28-15_[5e395062]", 3, 0, &numeric,
                                   (int)sizeof(numeric), 0) == ft_numeric_64,
                "Unicode numeric field reports ft_numeric_64");
    EXPECT_I64_EQ(numeric, 112, "Unicode numeric field carries the value");

    FILETIME stamp;
    memset(&stamp, 0, sizeof(stamp));
    EXPECT_TRUE(FsContentGetValueW(L"\\RepoA\\2026-10-18_16-28-15_[5e395062]", 0, 0, &stamp,
                                   (int)sizeof(stamp), 0) == ft_datetime,
                "Unicode date field reports ft_datetime");
    EXPECT_TRUE(stamp.dwHighDateTime != 0 || stamp.dwLowDateTime != 0,
                "Unicode date field carries the snapshot time");

    /* A buffer that cannot hold a full character is handled without overrun */
    WCHAR one_byte = 0x1234;
    EXPECT_TRUE(FsContentGetValueW(L"\\RepoA\\2026-10-18_16-28-15_[5e395062]", 1, 0, &one_byte,
                                   (int)sizeof(WCHAR), 0) == ft_fieldempty,
                "Buffer smaller than one wide character returns ft_fieldempty");

    char tiny[8] = {0};
    EXPECT_TRUE(FsContentGetValueW(L"\\RepoA\\2026-10-18_16-28-15_[5e395062]", 1, 0, tiny,
                                   (int)sizeof(tiny), 0) == ft_stringw,
                "Short wide buffer is accepted");
    /* 8 bytes = 4 wide characters = 3 characters plus the terminator */
    EXPECT_TRUE(wcscmp((WCHAR*)tiny, L"GOL") == 0, "Short wide buffer is truncated on a boundary");

    /* Wide default view */
    WCHAR contents[512] = {0};
    WCHAR headers[512]  = {0};
    WCHAR widths[512]   = {0};
    WCHAR options[512]  = {0};
    EXPECT_TRUE(FsContentGetDefaultViewW(contents, headers, widths, options,
                                         (int)sizeof(contents)) != 0,
                "Wide default view is published");
    EXPECT_TRUE(wcsstr(contents, L"[=<fs>.time]") != NULL, "Wide default view lists the fields");
    EXPECT_TRUE(wcsstr(contents, L"\\n") != NULL, "Wide default view keeps the literal separator");
    EXPECT_TRUE(wcsstr(widths, L",") != NULL && wcsstr(options, L"|") != NULL,
                "Wide default view provides widths and options");
    EXPECT_TRUE(!FsContentGetDefaultViewW(contents, NULL, NULL, NULL, 0),
                "Wide default view rejects a zero length buffer");
    EXPECT_TRUE(FsContentGetDefaultViewW(contents, NULL, NULL, NULL, (int)sizeof(contents)) != 0,
                "Wide default view tolerates NULL buffers");

    FsContentStopGetValueW(L"\\RepoA");
    FsContentStopGetValueW(NULL);
    g_tests_run++; /* The no-op stop entry point must accept anything */

    snapshot_stats_clear();
    restic_free_snapshot_list(items);
}

int main(void) {
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
    printf("Running Restic WFX snapshot statistics tests...\n");

    test_parse_path_root();
    test_parse_path_alias();
    test_parse_path_snapshot();
    test_parse_path_invalid();

    test_parse_snapshot_list_fields();
    test_parse_snapshot_list_without_summary();
    test_parse_snapshot_list_edge_cases();
    test_find_snapshot_by_short_id();

    test_cache_store_and_lookup();
    test_cache_replace_and_invalidate();
    test_cache_eviction();

    test_content_field_enumeration();
    test_content_field_units();
    test_content_values();
    test_content_values_without_summary();
    test_content_values_paths();
    test_content_default_view();
    test_content_flags_and_sorting();
    test_content_unloading_clears_cache();

    test_parsers_stay_index_aligned();
    test_counters_are_saturated();
    test_long_lists_are_truncated();
    test_text_fields_use_the_ansi_code_page();
    test_cache_ansi_alias_fallback();
    test_cache_concurrent_access();
    test_content_path_spellings();
    test_cache_lookup_any();
    test_content_trace_is_opt_in();
    test_trace_switch_file();
    test_content_unicode_values();

    if (g_tests_failed == 0) {
        printf("[PASS] All %d assertions succeeded.\n", g_tests_run);
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
        _CrtDumpMemoryLeaks();
#endif
        return 0;
    }
    printf("[FAIL] %d of %d assertions failed.\n", g_tests_failed, g_tests_run);
    return 1;
}
