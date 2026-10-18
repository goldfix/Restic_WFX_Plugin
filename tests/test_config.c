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
#include <wchar.h>
#include <string.h>
#include "config.h"
#include "fsplugin.h"

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

#define EXPECT_WSTR_EQ(actual, expected, msg) do { \
    g_tests_run++; \
    if (_wcsicmp((actual), (expected)) != 0) { \
        printf("[FAIL] %s:%d: %s (expected '%ls', got '%ls')\n", \
               __FUNCTION__, __LINE__, (msg), (expected), (actual)); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

/* Helper: writes an ASCII/UTF-8 text file to disk */
static BOOL write_text_file(const WCHAR* path, const char* content) {
    HANDLE hFile = CreateFileW(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    DWORD len = (DWORD)strlen(content);
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, content, len, &written, NULL);
    CloseHandle(hFile);
    return ok && (written == len);
}

/* Helper: creates a unique temporary directory for isolated tests */
static BOOL make_temp_dir(const WCHAR* prefix, WCHAR* out_dir, size_t capacity) {
    WCHAR base_temp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, base_temp);
    swprintf_s(out_dir, capacity, L"%ls%ls_%lu", base_temp, prefix, GetCurrentProcessId());
    CreateDirectoryW(out_dir, NULL);
    return TRUE;
}

/* Test 1: Directory normalization and FsSetDefaultParams extraction */
static void test_plugin_and_fallback_dirs(void) {
    config_set_plugin_dir(L"  \"C:\\TotalCmd\\plugins\\wfx\\restic\\\"  ");
    EXPECT_WSTR_EQ(config_get_plugin_dir(), L"C:\\TotalCmd\\plugins\\wfx\\restic", "Trim and strip trailing backslash");

    config_set_plugin_dir(L"C:\\");
    EXPECT_WSTR_EQ(config_get_plugin_dir(), L"C:\\", "Preserve drive root backslash");

    FsDefaultParamStruct dps;
    memset(&dps, 0, sizeof(dps));
    dps.size = sizeof(dps);
    strncpy_s(dps.DefaultIniName, sizeof(dps.DefaultIniName),
              "C:\\Users\\TestUser\\AppData\\Roaming\\GHISLER\\fsplugin.ini", _TRUNCATE);

    FsSetDefaultParams(&dps);
    EXPECT_WSTR_EQ(
        config_get_fallback_dir(),
        L"C:\\Users\\TestUser\\AppData\\Roaming\\GHISLER",
        "Extract parent directory from DefaultIniName"
    );
}

/* Test 2: INI path resolution (primary plugin dir vs fallback dir + auto-creation) */
static void test_ini_path_resolution_priority(void) {
    WCHAR plugin_dir[CONFIG_MAX_PATH]   = {0};
    WCHAR fallback_dir[CONFIG_MAX_PATH] = {0};
    make_temp_dir(L"restic_test_plug", plugin_dir, CONFIG_MAX_PATH);
    make_temp_dir(L"restic_test_fb", fallback_dir, CONFIG_MAX_PATH);

    WCHAR primary_ini[CONFIG_MAX_PATH]  = {0};
    WCHAR fallback_ini[CONFIG_MAX_PATH] = {0};
    swprintf_s(primary_ini, CONFIG_MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);
    swprintf_s(fallback_ini, CONFIG_MAX_PATH, L"%ls\\%ls", fallback_dir, CONFIG_INI_FILENAME);

    DeleteFileW(primary_ini);
    DeleteFileW(fallback_ini);

    config_set_plugin_dir(plugin_dir);
    config_set_fallback_dir(fallback_dir);

    /* Case A: Neither exists, create_if_missing = FALSE */
    WCHAR resolved[CONFIG_MAX_PATH] = {0};
    BOOL ok = config_resolve_ini_path(resolved, CONFIG_MAX_PATH, FALSE);
    EXPECT_TRUE(!ok, "Should return FALSE when no INI exists and create_if_missing is FALSE");
    EXPECT_WSTR_EQ(resolved, fallback_ini, "Candidate path should point to fallback INI");

    /* Case B: Neither exists, create_if_missing = TRUE -> creates in fallback_dir */
    ok = config_resolve_ini_path(resolved, CONFIG_MAX_PATH, TRUE);
    EXPECT_TRUE(ok, "Should create default INI in fallback_dir when create_if_missing is TRUE");
    EXPECT_WSTR_EQ(resolved, fallback_ini, "Resolved path should be fallback INI");
    EXPECT_TRUE(GetFileAttributesW(fallback_ini) != INVALID_FILE_ATTRIBUTES, "Fallback INI file must exist on disk");

    /* Case C: Primary INI exists -> takes priority over fallback INI */
    write_text_file(primary_ini, "[Settings]\r\nResticPath=restic.exe\r\n");
    ok = config_resolve_ini_path(resolved, CONFIG_MAX_PATH, FALSE);
    EXPECT_TRUE(ok, "Should resolve primary INI when present");
    EXPECT_WSTR_EQ(resolved, primary_ini, "Primary INI in plugin_dir must take precedence over fallback_dir");

    DeleteFileW(primary_ini);
    DeleteFileW(fallback_ini);
    RemoveDirectoryW(plugin_dir);
    RemoveDirectoryW(fallback_dir);
}

/* Test 3: Restic executable resolution priority */
static void test_restic_binary_resolution(void) {
    WCHAR plugin_dir[CONFIG_MAX_PATH] = {0};
    WCHAR custom_dir[CONFIG_MAX_PATH] = {0};
    make_temp_dir(L"restic_bin_plug", plugin_dir, CONFIG_MAX_PATH);
    make_temp_dir(L"restic_bin_cust", custom_dir, CONFIG_MAX_PATH);

    WCHAR ini_path[CONFIG_MAX_PATH]       = {0};
    WCHAR local_restic[CONFIG_MAX_PATH]   = {0};
    WCHAR custom_restic[CONFIG_MAX_PATH]  = {0};
    swprintf_s(ini_path, CONFIG_MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);
    swprintf_s(local_restic, CONFIG_MAX_PATH, L"%ls\\restic.exe", plugin_dir);
    swprintf_s(custom_restic, CONFIG_MAX_PATH, L"%ls\\my_restic.exe", custom_dir);

    /* Create dummy binaries */
    write_text_file(local_restic, "MZ_DUMMY_LOCAL");
    write_text_file(custom_restic, "MZ_DUMMY_CUSTOM");

    config_set_plugin_dir(plugin_dir);
    config_set_fallback_dir(L"");

    /* Priority 1: Explicit custom path in [Settings] ResticPath */
    char ini_content[2048];
    snprintf(ini_content, sizeof(ini_content),
             "[Settings]\r\nResticPath=\"%ls\"\r\n", custom_restic);
    write_text_file(ini_path, ini_content);

    WCHAR resolved_bin[CONFIG_MAX_PATH] = {0};
    BOOL ok = config_resolve_restic_path(ini_path, resolved_bin, CONFIG_MAX_PATH);
    EXPECT_TRUE(ok, "Custom ResticPath should resolve");
    EXPECT_WSTR_EQ(resolved_bin, custom_restic, "Explicit ResticPath takes highest priority");

    /* Priority 2: ResticPath is default/empty -> picks <plugin_dir>\restic.exe */
    write_text_file(ini_path, "[Settings]\r\nResticPath=restic.exe\r\n");
    ok = config_resolve_restic_path(ini_path, resolved_bin, CONFIG_MAX_PATH);
    EXPECT_TRUE(ok, "Local plugin_dir restic.exe should resolve");
    EXPECT_WSTR_EQ(resolved_bin, local_restic, "Should pick <plugin_dir>\\restic.exe");

    /* Priority 3: Remove <plugin_dir>\restic.exe -> falls back to PATH or bare restic.exe */
    DeleteFileW(local_restic);
    config_resolve_restic_path(ini_path, resolved_bin, CONFIG_MAX_PATH);
    EXPECT_TRUE(resolved_bin[0] != L'\0', "Fallback restic binary string must not be empty");

    DeleteFileW(custom_restic);
    DeleteFileW(ini_path);
    RemoveDirectoryW(plugin_dir);
    RemoveDirectoryW(custom_dir);
}

/* Test 4: [Repositories] section parsing, sanitization, deduplication, and WFX root enumeration */
static void test_repositories_parsing_and_wfx_root(void) {
    WCHAR plugin_dir[CONFIG_MAX_PATH] = {0};
    make_temp_dir(L"restic_repo_plug", plugin_dir, CONFIG_MAX_PATH);

    WCHAR ini_path[CONFIG_MAX_PATH] = {0};
    swprintf_s(ini_path, CONFIG_MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);

    const char* sample_ini =
        "[Settings]\r\n"
        "ResticPath=restic.exe\r\n"
        "\r\n"
        "[Repositories]\r\n"
        "; Comment line with semicolon\r\n"
        "# Comment line with hash\r\n"
        "BackupLocale=C:\\Backups\\OldPath\r\n"
        "ServerLinux = rest:https://backup.lan:8000/\r\n"
        "CloudS3 = \"s3:s3.us-east-1.amazonaws.com/company-backup/restic\"\r\n"
        "NAS_SMB=\\\\nas\\share\\restic_repo\r\n"
        "Invalid/Alias=C:\\ShouldBeIgnored\r\n"
        "..=C:\\ShouldBeIgnoredToo\r\n"
        "EmptyTarget=\r\n"
        "backuplocale = D:\\Backups\\ResticRepo\r\n";

    write_text_file(ini_path, sample_ini);
    config_set_plugin_dir(plugin_dir);
    config_set_fallback_dir(L"");

    ResticConfig cfg;
    BOOL ok = config_load(&cfg);
    EXPECT_TRUE(ok, "config_load should succeed");
    EXPECT_TRUE(cfg.repo_count == 4, "Should parse 4 valid unique repositories");

    const ResticRepoEntry* r0 = config_find_repo(&cfg, L"BackupLocale");
    EXPECT_TRUE(r0 != NULL, "Find BackupLocale");
    EXPECT_WSTR_EQ(r0->target, L"D:\\Backups\\ResticRepo", "Case-insensitive duplicate should update target");

    const ResticRepoEntry* r1 = config_find_repo(&cfg, L"serverlinux");
    EXPECT_TRUE(r1 != NULL, "Case-insensitive lookup for ServerLinux");
    EXPECT_WSTR_EQ(r1->target, L"rest:https://backup.lan:8000/", "ServerLinux target match");

    const ResticRepoEntry* r2 = config_find_repo(&cfg, L"CloudS3");
    EXPECT_TRUE(r2 != NULL, "Find CloudS3");
    EXPECT_WSTR_EQ(r2->target, L"s3:s3.us-east-1.amazonaws.com/company-backup/restic", "Quotes stripped from target");

    const ResticRepoEntry* r3 = config_find_repo(&cfg, L"NAS_SMB");
    EXPECT_TRUE(r3 != NULL, "Find NAS_SMB");
    EXPECT_WSTR_EQ(r3->target, L"\\\\nas\\share\\restic_repo", "UNC path preserved");

    EXPECT_TRUE(config_find_repo(&cfg, L"NonExistent") == NULL, "Unknown alias returns NULL");
    config_free(&cfg);

    /* Verify WFX Level 0 Root ("\") enumeration through FsFindFirstW / FsFindNextW / FsFindClose */
    WIN32_FIND_DATAW find_data;
    HANDLE hFind = FsFindFirstW(L"\\", &find_data);
    EXPECT_TRUE(hFind != INVALID_HANDLE_VALUE, "FsFindFirstW on root '\\' should succeed");
    EXPECT_TRUE((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, "Root item 0 must be directory");
    EXPECT_WSTR_EQ(find_data.cFileName, L"BackupLocale", "Root item 0 name");

    EXPECT_TRUE(FsFindNextW(hFind, &find_data), "FsFindNextW item 1");
    EXPECT_WSTR_EQ(find_data.cFileName, L"ServerLinux", "Root item 1 name");

    EXPECT_TRUE(FsFindNextW(hFind, &find_data), "FsFindNextW item 2");
    EXPECT_WSTR_EQ(find_data.cFileName, L"CloudS3", "Root item 2 name");

    EXPECT_TRUE(FsFindNextW(hFind, &find_data), "FsFindNextW item 3");
    EXPECT_WSTR_EQ(find_data.cFileName, L"NAS_SMB", "Root item 3 name");

    EXPECT_TRUE(!FsFindNextW(hFind, &find_data), "FsFindNextW after last item must return FALSE");
    FsFindClose(hFind);

    /* Empty [Repositories] -> ERROR_NO_MORE_FILES */
    write_text_file(ini_path, "[Settings]\r\nResticPath=restic.exe\r\n[Repositories]\r\n");
    hFind = FsFindFirstW(L"\\", &find_data);
    EXPECT_TRUE(hFind == INVALID_HANDLE_VALUE, "Empty repositories should return INVALID_HANDLE_VALUE");
    EXPECT_TRUE(GetLastError() == ERROR_NO_MORE_FILES, "Last error must be ERROR_NO_MORE_FILES");

    DeleteFileW(ini_path);
    RemoveDirectoryW(plugin_dir);
}

int main(void) {
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    printf("Running Restic WFX Configuration Unit Tests...\n");

    test_plugin_and_fallback_dirs();
    test_ini_path_resolution_priority();
    test_restic_binary_resolution();
    test_repositories_parsing_and_wfx_root();

#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    EXPECT_TRUE(_CrtCheckMemory(), "CRT heap integrity check must pass");
#endif

    if (g_tests_failed == 0) {
        printf("[PASS] All %d assertions succeeded.\n", g_tests_run);
        return 0;
    } else {
        printf("[FAIL] %d of %d assertions failed.\n", g_tests_failed, g_tests_run);
        return 1;
    }
}
