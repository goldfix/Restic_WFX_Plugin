/*
 * Black-box smoke test of the built plugin DLL: it is loaded the way Total Commander loads it
 * (LoadLibrary + GetProcAddress) and driven through the exported WFX entry points only.
 * Requires build\restic.wfx64 (skipped when absent, e.g. in AddressSanitizer test runs).
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define PLUGIN_DLL L"build\\restic.wfx64"

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
    if (wcscmp((actual), (expected)) != 0) { \
        printf("[FAIL] %s:%d: %s (expected '%ls', got '%ls')\n", \
               __FUNCTION__, __LINE__, (msg), (expected), (actual)); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

/* Local copies of the WFX prototypes (the plugin is only reachable through GetProcAddress) */
typedef struct {
    int size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char DefaultIniName[MAX_PATH];
} DefaultParams;

typedef int    (__stdcall *PFN_InitW)(int, void*, void*, void*);
typedef void   (__stdcall *PFN_GetDefRootName)(char*, int);
typedef void   (__stdcall *PFN_SetDefaultParams)(DefaultParams*);
typedef HANDLE (__stdcall *PFN_FindFirstW)(WCHAR*, WIN32_FIND_DATAW*);
typedef HANDLE (__stdcall *PFN_FindFirst)(char*, WIN32_FIND_DATAA*);
typedef BOOL   (__stdcall *PFN_FindNextW)(HANDLE, WIN32_FIND_DATAW*);
typedef int    (__stdcall *PFN_FindClose)(HANDLE);

typedef struct {
    HMODULE module;
    PFN_InitW init_w;
    PFN_GetDefRootName get_root_name;
    PFN_SetDefaultParams set_default_params;
    PFN_FindFirstW find_first_w;
    PFN_FindFirst find_first;
    PFN_FindNextW find_next_w;
    PFN_FindClose find_close;
} Plugin;

static BOOL make_unique_dir(const WCHAR* tag, WCHAR* out, size_t cap) {
    WCHAR tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(out, cap, L"%ls%ls_%lu", tmp, tag, GetCurrentProcessId());
    return CreateDirectoryW(out, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static BOOL write_file(const WCHAR* path, const char* text) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0, len = (DWORD)strlen(text);
    BOOL ok = (h != INVALID_HANDLE_VALUE) && WriteFile(h, text, len, &written, NULL) && written == len;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return ok;
}

/* Copies the DLL into dir (so DllMain resolves that directory as the plugin directory) and loads it */
static BOOL load_plugin_copy(const WCHAR* dir, Plugin* p) {
    WCHAR dest[MAX_PATH];
    memset(p, 0, sizeof(*p));
    swprintf_s(dest, MAX_PATH, L"%ls\\restic.wfx64", dir);
    if (!CopyFileW(PLUGIN_DLL, dest, FALSE)) return FALSE;
    p->module = LoadLibraryW(dest);
    if (!p->module) return FALSE;
    p->init_w            = (PFN_InitW)(void*)GetProcAddress(p->module, "FsInitW");
    p->get_root_name     = (PFN_GetDefRootName)(void*)GetProcAddress(p->module, "FsGetDefRootName");
    p->set_default_params = (PFN_SetDefaultParams)(void*)GetProcAddress(p->module, "FsSetDefaultParams");
    p->find_first_w      = (PFN_FindFirstW)(void*)GetProcAddress(p->module, "FsFindFirstW");
    p->find_first        = (PFN_FindFirst)(void*)GetProcAddress(p->module, "FsFindFirst");
    p->find_next_w       = (PFN_FindNextW)(void*)GetProcAddress(p->module, "FsFindNextW");
    p->find_close        = (PFN_FindClose)(void*)GetProcAddress(p->module, "FsFindClose");
    return TRUE;
}

static void unload_plugin_copy(Plugin* p, const WCHAR* dir) {
    WCHAR dest[MAX_PATH];
    if (p->module) FreeLibrary(p->module);
    p->module = NULL;
    swprintf_s(dest, MAX_PATH, L"%ls\\restic.wfx64", dir);
    DeleteFileW(dest);
}

/* Exact export surface: read-only entry points present, write operations and cJSON absent */
static void test_export_surface(void) {
    HMODULE m = LoadLibraryW(PLUGIN_DLL);
    EXPECT_TRUE(m != NULL, "Plugin DLL loads");

    const char* required[] = {"FsInit", "FsInitW", "FsFindFirst", "FsFindFirstW", "FsFindNext",
                              "FsFindNextW", "FsFindClose", "FsGetDefRootName", "FsSetDefaultParams"};
    for (size_t i = 0; i < _countof(required); ++i) {
        FARPROC fn = GetProcAddress(m, required[i]);
        g_tests_run++;
        if (!fn) {
            printf("[FAIL] test_export_surface: missing export %s\n", required[i]);
            g_tests_failed++;
        }
    }

    /* Optional functions must NOT be exported unless implemented: Total Commander would call them */
    const char* forbidden[] = {"FsGetFile", "FsGetFileW", "FsPutFile", "FsPutFileW", "FsDeleteFile",
                               "FsDeleteFileW", "FsMkDir", "FsMkDirW", "FsRemoveDir", "FsRemoveDirW",
                               "FsRenMovFile", "FsRenMovFileW", "FsSetAttr", "FsSetTime", "FsExecuteFile",
                               "cJSON_Parse", "cJSON_Delete", "DllMain"};
    for (size_t i = 0; i < _countof(forbidden); ++i) {
        FARPROC fn = GetProcAddress(m, forbidden[i]);
        g_tests_run++;
        if (fn) {
            printf("[FAIL] test_export_surface: unexpected export %s\n", forbidden[i]);
            g_tests_failed++;
        }
    }
    FreeLibrary(m);
}

/* Scenario A: restic_plugin.ini sits next to the DLL (primary lookup) */
static void test_primary_ini_next_to_dll(void) {
    WCHAR dir[MAX_PATH], ini[MAX_PATH];
    EXPECT_TRUE(make_unique_dir(L"restic_dll_primary", dir, MAX_PATH), "Create plugin directory");
    swprintf_s(ini, MAX_PATH, L"%ls\\restic_plugin.ini", dir);
    EXPECT_TRUE(write_file(ini, "[Repositories]\r\nAlpha=D:\\r1\r\nBeta=D:\\r2\r\n"), "Write INI");

    Plugin p;
    EXPECT_TRUE(load_plugin_copy(dir, &p), "Load plugin copy");
    EXPECT_TRUE(p.init_w && p.get_root_name && p.set_default_params && p.find_first_w && p.find_next_w &&
                p.find_close && p.find_first, "Entry points resolved");

    char root_name[64] = {0};
    p.get_root_name(root_name, (int)sizeof(root_name));
    EXPECT_TRUE(strcmp(root_name, "Restic Backups") == 0, "Default root name");
    char tiny[5] = {'x', 'x', 'x', 'x', 'x'};
    p.get_root_name(tiny, (int)sizeof(tiny));
    EXPECT_TRUE(strcmp(tiny, "Rest") == 0, "Root name truncated and terminated for small buffers");

    EXPECT_TRUE(p.init_w(1, NULL, NULL, NULL) == 0, "FsInitW returns 0");

    WIN32_FIND_DATAW fd;
    HANDLE h = p.find_first_w(L"\\", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "Root enumeration starts");
    EXPECT_WSTR_EQ(fd.cFileName, L"Alpha", "First alias");
    EXPECT_TRUE((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, "Alias is a directory");
    EXPECT_TRUE(p.find_next_w(h, &fd), "Second alias available");
    EXPECT_WSTR_EQ(fd.cFileName, L"Beta", "Second alias");
    EXPECT_TRUE(!p.find_next_w(h, &fd), "End of list");
    EXPECT_TRUE(p.find_close(h) == 0, "FsFindClose returns 0");

    /* Two simultaneous enumerations (both panels) keep independent state */
    WIN32_FIND_DATAW fd2;
    HANDLE h1 = p.find_first_w(L"\\", &fd);
    HANDLE h2 = p.find_first_w(L"\\", &fd2);
    EXPECT_TRUE(h1 != INVALID_HANDLE_VALUE && h2 != INVALID_HANDLE_VALUE && h1 != h2, "Two live handles");
    EXPECT_TRUE(p.find_next_w(h1, &fd), "Advance first handle");
    EXPECT_WSTR_EQ(fd.cFileName, L"Beta", "First handle advanced");
    EXPECT_TRUE(p.find_next_w(h2, &fd2), "Advance second handle");
    EXPECT_WSTR_EQ(fd2.cFileName, L"Beta", "Second handle independent");
    p.find_close(h1);
    p.find_close(h2);

    /* Unrouted paths and the ANSI stub fail cleanly instead of crashing */
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(L"\\Alpha", &fd) == INVALID_HANDLE_VALUE, "Sub-paths not implemented yet");
    EXPECT_TRUE(GetLastError() != ERROR_NO_MORE_FILES, "Missing path is not reported as an empty directory");
    WIN32_FIND_DATAA fda;
    char root_a[] = "\\";
    EXPECT_TRUE(p.find_first(root_a, &fda) == INVALID_HANDLE_VALUE, "ANSI stub refuses to enumerate");
    EXPECT_TRUE(p.find_close(INVALID_HANDLE_VALUE) == 0 && p.find_close(NULL) == 0, "FsFindClose tolerates invalid handles");

    unload_plugin_copy(&p, dir);
    DeleteFileW(ini);
    RemoveDirectoryW(dir);
}

/* Scenario B: no INI next to the DLL -> FsSetDefaultParams fallback directory, template auto-created */
static void test_fallback_ini_from_default_params(void) {
    WCHAR dir[MAX_PATH], fb_dir[MAX_PATH], fb_ini[MAX_PATH];
    EXPECT_TRUE(make_unique_dir(L"restic_dll_noini", dir, MAX_PATH) &&
                make_unique_dir(L"restic_dll_fallback", fb_dir, MAX_PATH), "Create directories");
    swprintf_s(fb_ini, MAX_PATH, L"%ls\\restic_plugin.ini", fb_dir);
    DeleteFileW(fb_ini);

    Plugin p;
    EXPECT_TRUE(load_plugin_copy(dir, &p), "Load plugin copy");
    EXPECT_TRUE(p.init_w && p.set_default_params && p.find_first_w && p.find_next_w && p.find_close,
                "Entry points resolved");

    DefaultParams dps;
    memset(&dps, 0, sizeof(dps));
    dps.size = (int)sizeof(dps);
    WideCharToMultiByte(CP_ACP, 0, fb_dir, -1, dps.DefaultIniName, (int)sizeof(dps.DefaultIniName), NULL, NULL);
    strcat_s(dps.DefaultIniName, sizeof(dps.DefaultIniName), "\\wincmd.ini");
    p.init_w(1, NULL, NULL, NULL);
    p.set_default_params(&dps);

    WIN32_FIND_DATAW fd;
    SetLastError(0);
    HANDLE h = p.find_first_w(L"\\", &fd);
    EXPECT_TRUE(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_NO_MORE_FILES,
                "Template without repositories -> empty root");
    EXPECT_TRUE(GetFileAttributesW(fb_ini) != INVALID_FILE_ATTRIBUTES, "Default INI created in the fallback directory");

    EXPECT_TRUE(write_file(fb_ini, "[Repositories]\r\nOnlyOne=D:\\repo\r\n"), "Edit the fallback INI");
    h = p.find_first_w(L"\\", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "INI edits are picked up without reloading the plugin");
    EXPECT_WSTR_EQ(fd.cFileName, L"OnlyOne", "Alias from fallback INI");
    EXPECT_TRUE(!p.find_next_w(h, &fd), "Single alias");
    p.find_close(h);

    unload_plugin_copy(&p, dir);
    DeleteFileW(fb_ini);
    RemoveDirectoryW(dir);
    RemoveDirectoryW(fb_dir);
}

int main(void) {
    printf("Running Restic WFX plugin DLL smoke tests...\n");
    if (GetFileAttributesW(PLUGIN_DLL) == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] build\\restic.wfx64 not found: DLL smoke tests skipped.\n");
        return 0;
    }

    test_export_surface();
    test_primary_ini_next_to_dll();
    test_fallback_ini_from_default_params();

    if (g_tests_failed == 0) {
        printf("[PASS] All %d assertions succeeded.\n", g_tests_run);
        return 0;
    }
    printf("[FAIL] %d of %d assertions failed.\n", g_tests_failed, g_tests_run);
    return 1;
}
