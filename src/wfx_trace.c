#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include "wfx_trace.h"

/*
 * Diagnostic trace, opt-in and permanently shipped.
 *
 * Why it exists: Total Commander documents the content callbacks only as "the name of the file (in
 * plugin namespace)" and does not say how a plugin is referenced in a column definition. When the
 * columns of an installed build stay empty, these three states must be distinguishable:
 *   1. the plugin is not loaded / the trace is not enabled   -> no file at all
 *   2. the plugin is loaded but Total Commander never asks   -> file with "init"/"findfirst" only
 *   3. the plugin is asked, but the path cannot be resolved  -> "value ... -> -3" lines
 *
 * The settings are cached per process because they never change while the DLL is loaded.
 */

#define WFX_TRACE_FILE_NAME L"restic_wfx_trace.log"
#define WFX_TRACE_SWITCH    L"restic_wfx_trace.on"

static SRWLOCK g_Lock = SRWLOCK_INIT;
static LONG g_Enabled = -1; /* -1 = not checked, 0 = disabled, 1 = enabled */
static LONG g_Lines   = 0;
static WCHAR g_PluginDir[MAX_PATH];
static WCHAR g_FilePath[MAX_PATH];
static BOOL g_DirKnown = FALSE;

static void remember_plugin_dir(HINSTANCE self) {
    if (g_DirKnown || self == NULL) return;

    WCHAR module[MAX_PATH] = {0};
    DWORD length           = GetModuleFileNameW(self, module, (DWORD)_countof(module));
    if (length == 0 || length >= _countof(module)) return;

    /* Strip the file name but keep the trailing backslash */
    for (DWORD i = length; i > 0; --i) {
        if (module[i - 1] == L'\\' || module[i - 1] == L'/') {
            module[i] = L'\0';
            break;
        }
    }
    wcsncpy_s(g_PluginDir, _countof(g_PluginDir), module, _TRUNCATE);
    g_DirKnown = TRUE;
}

/* Builds the trace file path: %TEMP%\<name>, or the plugin directory when TEMP is unusable */
static BOOL build_file_path(WCHAR* out, size_t capacity) {
    WCHAR temp[MAX_PATH] = {0};
    DWORD length         = GetTempPathW((DWORD)_countof(temp), temp);
    if (length > 0 && length < _countof(temp) &&
        swprintf_s(out, capacity, L"%ls%ls", temp, WFX_TRACE_FILE_NAME) > 0) {
        return TRUE;
    }
    if (g_DirKnown && swprintf_s(out, capacity, L"%ls%ls", g_PluginDir, WFX_TRACE_FILE_NAME) > 0) {
        return TRUE;
    }
    out[0] = L'\0';
    return FALSE;
}

static BOOL switch_file_exists(void) {
    if (!g_DirKnown) return FALSE;
    WCHAR path[MAX_PATH] = {0};
    if (swprintf_s(path, _countof(path), L"%ls%ls", g_PluginDir, WFX_TRACE_SWITCH) <= 0) {
        return FALSE;
    }
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

BOOL wfx_trace_enabled(void) {
    LONG state = InterlockedCompareExchange(&g_Enabled, -1, -1);
    if (state >= 0) return state == 1;

    WCHAR value[16]    = {0};
    DWORD value_length = GetEnvironmentVariableW(
        L"RESTIC_WFX_TRACE", value, (DWORD)_countof(value)
    );
    BOOL enabled = FALSE;
    if (value_length > 0 && value_length < _countof(value)) {
        enabled = !(value[0] == L'0' && value[1] == L'\0');
    }
    if (!enabled) {
        enabled = switch_file_exists();
    }

    InterlockedExchange(&g_Enabled, enabled ? 1 : 0);
    return enabled;
}

/* Writes one line; the first line of a session recreates the file */
static void write_line(const char* text) {
    if (text == NULL) return;
    const LONG number = InterlockedIncrement(&g_Lines);
    if (number > WFX_TRACE_MAX_LINES) return;

    AcquireSRWLockExclusive(&g_Lock);
    if (g_FilePath[0] == L'\0') {
        build_file_path(g_FilePath, _countof(g_FilePath));
    }
    const WCHAR* path = g_FilePath;
    const BOOL first  = (number == 1);
    ReleaseSRWLockExclusive(&g_Lock);
    if (path[0] == L'\0') return;

    /* CREATE_ALWAYS on the first line truncates a file left over by a previous session */
    HANDLE file = CreateFileW(
        path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        first ? CREATE_ALWAYS : OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    WriteFile(file, text, (DWORD)strlen(text), &written, NULL);
    WriteFile(file, "\r\n", 2, &written, NULL);
    CloseHandle(file);
}

void wfx_trace_init(HINSTANCE self) {
    remember_plugin_dir(self);
    if (!wfx_trace_enabled()) return;

    char line[512] = {0};
    WCHAR path[MAX_PATH] = {0};
    build_file_path(path, _countof(path));
    _snprintf_s(
        line,
        _countof(line),
        _TRUNCATE,
        "--- restic wfx trace, pid=%lu, temp=%ls",
        (unsigned long)GetCurrentProcessId(),
        (path[0] != L'\0') ? path : L"<unavailable>"
    );
    write_line(line);
    wfx_trace_printf("plugin dir: %ls", (g_DirKnown ? g_PluginDir : L"<unknown>"));
}

void wfx_trace_printf(const char* format, ...) {
    if (format == NULL || !wfx_trace_enabled()) return;

    char text[1024] = {0};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(text, _countof(text), _TRUNCATE, format, args);
    va_end(args);
    write_line(text);
}

void wfx_trace_path(const char* tag, const WCHAR* path) {
    if (!wfx_trace_enabled()) return;
    wfx_trace_printf("%s: %ls", (tag != NULL) ? tag : "path", (path != NULL) ? path : L"<null>");
}

void wfx_trace_reset(void) {
    AcquireSRWLockExclusive(&g_Lock);
    InterlockedExchange(&g_Enabled, -1);
    InterlockedExchange(&g_Lines, 0);
    g_FilePath[0] = L'\0';
    ReleaseSRWLockExclusive(&g_Lock);
}

void wfx_trace_file_path(WCHAR* out, size_t capacity) {
    if (out == NULL || capacity == 0) return;
    out[0] = L'\0';
    if (!wfx_trace_enabled()) return;
    if (g_FilePath[0] == L'\0') {
        build_file_path(g_FilePath, _countof(g_FilePath));
    }
    wcsncpy_s(out, capacity, g_FilePath, _TRUNCATE);
}
