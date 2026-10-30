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
#include "fsplugin.h"
#include "config.h"
#include "restic_cli.h"
#include "restic_stats.h"
#include "restic_report.h"
#include "wfx_path.h"
#include "wfx_status.h"
#include "wfx_trace.h"

#define SESSION_MAX_PASSWORD  512
#define SESSION_SLOTS         8
#define MAX_PASSWORD_ATTEMPTS 3
#define WFX_RESTIC_TIMEOUT_MS (30u * 60u * 1000u) /* Safety net if a hung command is never aborted */
/* Maximum length of a WFX path in UTF-16 units (Total Commander allows 1023) */
#define WFX_MAX_PATH_UNITS 1024

/* Host callbacks supplied by Total Commander in FsInitW */
static int            g_PluginNr     = 0;
static tProgressProcW g_ProgressProc = NULL;
static tLogProcW      g_LogProc      = NULL;
static tRequestProcW  g_RequestProc  = NULL;

/*
 * In-memory credential sessions, one slot per repository alias (two panels can browse two
 * repositories alternately without being prompted again). Guarded by an SRW lock because Total
 * Commander may call into the plugin from background threads. Slots are wiped on wrong password,
 * FsInitW and DLL unload. SRWLOCK_INIT keeps the lock usable even if DllMain never ran (unit tests).
 */
typedef struct {
    BOOL  used;
    WCHAR alias[MAX_PATH];
    WCHAR password[SESSION_MAX_PASSWORD];
} SessionSlot;

static SessionSlot g_Sessions[SESSION_SLOTS];
static unsigned    g_SessionEvictNext = 0;
static SRWLOCK     g_SessionLock      = SRWLOCK_INIT;

/**
 * Reentrant directory enumeration state returned as HANDLE by FsFindFirstW.
 */
typedef struct {
    WIN32_FIND_DATAW* items; /* Array of entries discovered for the target path */
    size_t count;            /* Total number of entries in items */
    size_t current_index;    /* Next entry index to return in FsFindNextW */
} ResticFindHandle;

/**
 * Context forwarded to the cancellation polling callback while restic.exe runs.
 */
typedef struct {
    const WCHAR* virtual_path;
    const WCHAR* repo_alias; /* Never the repository URL: it may embed credentials */
} WfxCancelContext;

static SessionSlot* session_find_locked(const WCHAR* alias) {
    for (int i = 0; i < SESSION_SLOTS; ++i) {
        if (g_Sessions[i].used && _wcsicmp(g_Sessions[i].alias, alias) == 0) {
            return &g_Sessions[i];
        }
    }
    return NULL;
}

/* Copies the cached password of `alias` into out_pwd. Returns FALSE if none is cached. */
static BOOL session_get(const WCHAR* alias, WCHAR* out_pwd, size_t cap) {
    BOOL found = FALSE;
    AcquireSRWLockExclusive(&g_SessionLock);
    const SessionSlot* slot = session_find_locked(alias);
    if (slot) {
        wcsncpy_s(out_pwd, cap, slot->password, _TRUNCATE);
        found = TRUE;
    }
    ReleaseSRWLockExclusive(&g_SessionLock);
    return found;
}

/* Stores (or replaces) the password of `alias`; the oldest slot is recycled when the table is full */
static void session_put(const WCHAR* alias, const WCHAR* password) {
    AcquireSRWLockExclusive(&g_SessionLock);
    SessionSlot* slot = session_find_locked(alias);
    for (int i = 0; !slot && i < SESSION_SLOTS; ++i) {
        if (!g_Sessions[i].used) slot = &g_Sessions[i];
    }
    if (!slot) {
        slot = &g_Sessions[g_SessionEvictNext++ % SESSION_SLOTS];
    }
    SecureZeroMemory(slot, sizeof(*slot));
    slot->used = TRUE;
    wcsncpy_s(slot->alias, MAX_PATH, alias, _TRUNCATE);
    wcsncpy_s(slot->password, SESSION_MAX_PASSWORD, password, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_SessionLock);
}

/* Wipes the cached password of a single repository */
static void session_forget(const WCHAR* alias) {
    AcquireSRWLockExclusive(&g_SessionLock);
    SessionSlot* slot = session_find_locked(alias);
    if (slot) SecureZeroMemory(slot, sizeof(*slot));
    ReleaseSRWLockExclusive(&g_SessionLock);
}

/* Wipes every cached password from RAM */
static void clear_password_session(void) {
    AcquireSRWLockExclusive(&g_SessionLock);
    SecureZeroMemory(g_Sessions, sizeof(g_Sessions));
    g_SessionEvictNext = 0;
    ReleaseSRWLockExclusive(&g_SessionLock);
}

/* Logs an important error and displays an OK message dialog in Total Commander */
static void report_plugin_error(const WCHAR* repo_alias, const WCHAR* detail) {
    WCHAR msg[1024]; /* alias (<260) + Restic message (<512) + decoration always fits; _TRUNCATE anyway */
    if (repo_alias && repo_alias[0] != L'\0') {
        _snwprintf_s(
            msg,
            _countof(msg),
            _TRUNCATE,
            L"[%ls] %ls",
            repo_alias,
            (detail && detail[0] != L'\0') ? detail : L"Restic command failed."
        );
    } else {
        wcsncpy_s(
            msg,
            _countof(msg),
            (detail && detail[0] != L'\0') ? detail : L"Restic plugin error.",
            _TRUNCATE
        );
    }

    if (g_LogProc) {
        g_LogProc(g_PluginNr, MSGTYPE_IMPORTANTERROR, msg);
    }
    if (g_RequestProc) {
        WCHAR unused[4] = {0};
        g_RequestProc(g_PluginNr, RT_MsgOK, L"Restic Backups", msg, unused, _countof(unused));
    }
}

/*
 * Prompts the user for the repository password via Total Commander's RequestProcW.
 * Returns FALSE when the dialog is cancelled or an empty password is entered (Restic refuses
 * empty passwords, and accepting one would only produce a confusing generic failure).
 */
static BOOL prompt_repository_password(
    const WCHAR* repo_alias,
    BOOL is_retry,
    WCHAR* out_pwd,
    size_t cap
) {
    if (!g_RequestProc || !repo_alias || repo_alias[0] == L'\0' || cap == 0) {
        return FALSE;
    }

    WCHAR prompt_text[MAX_PATH + 96];
    _snwprintf_s(
        prompt_text,
        _countof(prompt_text),
        _TRUNCATE,
        is_retry ? L"Wrong password. Enter password for '%ls':"
                 : L"Enter password for repository '%ls':",
        repo_alias
    );

    out_pwd[0] = L'\0'; /* RequestProc shows ReturnedText as the default value */
    BOOL accepted = g_RequestProc(
        g_PluginNr,
        RT_Password,
        L"Restic Repository Password",
        prompt_text,
        out_pwd,
        (int)cap
    );
    out_pwd[cap - 1] = L'\0';
    if (!accepted || out_pwd[0] == L'\0') {
        SecureZeroMemory(out_pwd, cap * sizeof(WCHAR));
        return FALSE;
    }
    return TRUE;
}

/*
 * Polled every ~100 ms by restic_exec_command; returns TRUE if the user pressed ESC in TC.
 * Total Commander ignores ProgressProc calls during the first 5 seconds of a directory change and
 * only then shows a progress dialog, so fast listings never flicker. 0 % = indeterminate.
 */
static BOOL wfx_cancel_callback(void* user_data) {
    if (!g_ProgressProc) {
        return FALSE;
    }
    const WfxCancelContext* ctx = (const WfxCancelContext*)user_data;
    WCHAR* src = (ctx && ctx->virtual_path) ? (WCHAR*)ctx->virtual_path : L"\\";
    WCHAR* dst = (ctx && ctx->repo_alias) ? (WCHAR*)ctx->repo_alias : L"";
    return g_ProgressProc(g_PluginNr, src, dst, 0) != 0;
}

/* Wraps a heap-allocated WIN32_FIND_DATAW array into a ResticFindHandle */
static HANDLE wrap_find_items(
    WIN32_FIND_DATAW* items,
    size_t count,
    WIN32_FIND_DATAW* first_out
) {
    if (count == 0) {
        free(items);
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    ResticFindHandle* handle = (ResticFindHandle*)calloc(1, sizeof(ResticFindHandle));
    if (!handle) {
        free(items);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    handle->items         = items;
    handle->count         = count;
    handle->current_index = 1;
    *first_out            = items[0];
    return (HANDLE)handle;
}

/* Maps a ResticResult failure to a Win32 error code and notifies Total Commander if needed */
static void handle_restic_failure(
    ResticResult res,
    const WCHAR* repo_alias,
    const WCHAR* err_msg
) {
    switch (res) {
        case RESTIC_ERR_PATH_NOT_FOUND:
            SetLastError(ERROR_PATH_NOT_FOUND);
            break;
        case RESTIC_ERR_CANCELLED:
            SetLastError(ERROR_CANCELLED);
            break;
        case RESTIC_ERR_WRONG_PASSWORD:
            report_plugin_error(
                repo_alias,
                (err_msg && err_msg[0] != L'\0') ? err_msg : L"Wrong repository password."
            );
            SetLastError(ERROR_ACCESS_DENIED);
            break;
        case RESTIC_ERR_REPO_NOT_FOUND:
            report_plugin_error(
                repo_alias,
                (err_msg && err_msg[0] != L'\0') ? err_msg : L"Repository does not exist."
            );
            SetLastError(ERROR_PATH_NOT_FOUND);
            break;
        case RESTIC_ERR_REPO_LOCKED:
            report_plugin_error(
                repo_alias,
                (err_msg && err_msg[0] != L'\0') ? err_msg : L"Repository is locked."
            );
            SetLastError(ERROR_LOCK_VIOLATION);
            break;
        case RESTIC_ERR_SPAWN_FAILED:
            report_plugin_error(
                repo_alias,
                (err_msg && err_msg[0] != L'\0') ? err_msg : L"Failed to execute restic.exe."
            );
            SetLastError(ERROR_FILE_NOT_FOUND);
            break;
        case RESTIC_ERR_TIMEOUT:
            report_plugin_error(repo_alias, L"Restic command timed out.");
            SetLastError(ERROR_TIMEOUT);
            break;
        case RESTIC_ERR_OUT_OF_MEMORY:
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            break;
        case RESTIC_ERR_INVALID_ARG:
            SetLastError(ERROR_INVALID_PARAMETER);
            break;
        default:
            report_plugin_error(
                repo_alias,
                (err_msg && err_msg[0] != L'\0') ? err_msg : L"Restic command failed."
            );
            SetLastError(ERROR_GEN_FAILURE);
            break;
    }
}

/* True when the outcome does not prove the password wrong, so it is worth caching for the next call */
static BOOL password_worth_caching(ResticResult res) {
    return res == RESTIC_OK || res == RESTIC_ERR_PATH_NOT_FOUND || res == RESTIC_ERR_LOCAL_WRITE ||
           res == RESTIC_ERR_CANCELLED || res == RESTIC_ERR_TIMEOUT;
}

/*
 * Runs `restic snapshots --json` once and feeds both parsers: the directory listing returned to
 * Total Commander and the snapshot statistics kept in the per-alias cache for the content columns.
 * A cache failure is not fatal: the listing still succeeds and the columns stay empty.
 */
static ResticResult list_snapshots_and_cache_stats(
    const WCHAR* restic_exe,
    const ResticRepoEntry* repo,
    const WCHAR* password,
    const ResticExecOptions* exec_opts,
    WIN32_FIND_DATAW** items,
    size_t* count,
    WCHAR* err_msg,
    size_t err_cap
) {
    char* json          = NULL;
    ResticResult res    = restic_fetch_snapshots_json(
        restic_exe, repo->target, password, exec_opts, &json, err_msg, err_cap
    );
    if (res != RESTIC_OK) {
        return res;
    }

    ResticSnapshotInfo* stats      = NULL;
    size_t stats_count             = 0;
    ResticResult stats_res         = restic_parse_snapshot_list(json, &stats, &stats_count);
    if (stats_res == RESTIC_OK) {
        snapshot_stats_store(repo->alias, stats, stats_count);
    } else {
        snapshot_stats_invalidate(repo->alias);
    }
    restic_free_snapshot_list(stats);

    res = restic_parse_snapshots_json(json, items, count);
    free(json);
    return res;
}

/*
 * Fills the statistics cache on demand. Total Commander may enter a snapshot folder directly
 * (address bar, Ctrl+D directory hotlist, history) without ever listing \<Alias>, and every
 * custom column of that level would stay empty. Once a directory listing of a repository whose
 * cache is still cold has succeeded, run exactly the same `restic snapshots --json` the alias
 * level would have run. It happens at most once per alias and session, needs no extra password
 * (the one that just worked is reused) and a failure is ignored: the listing already succeeded.
 */
static void ensure_snapshot_stats_cached(
    const WCHAR* restic_exe,
    const ResticRepoEntry* repo,
    const WCHAR* password,
    const ResticExecOptions* exec_opts
) {
    size_t cached = 0;
    if (snapshot_stats_lookup_alias(repo->alias, NULL, 0, &cached) && cached > 0) {
        return; /* Warm cache: nothing to do */
    }

    WIN32_FIND_DATAW* snap_items = NULL;
    size_t snap_count            = 0;
    WCHAR err_msg[256]           = {0};
    ResticResult res             = list_snapshots_and_cache_stats(
        restic_exe, repo, password, exec_opts, &snap_items, &snap_count, err_msg, _countof(err_msg)
    );
    if (res == RESTIC_OK) {
        free(snap_items);
    }
}

/*
 * Context of the progress callback used while a statistics report is generated.
 *
 * A report runs several restic commands, and every one of them polls the cancellation callback about
 * ten times per second. Reporting 0 % (indeterminate) and the alias there would reset the bar that
 * the report just advanced and change the file name shown by Total Commander, which looks like the
 * dialog starting over again. The last percentage is therefore kept in `current_percent` and
 * repeated, so the bar only moves forward and the destination name never changes.
 */
typedef struct {
    const WCHAR* remote_name;
    const WCHAR* local_name;
    volatile LONG current_percent;
} WfxReportProgressContext;

/* Forwards the report progress to Total Commander; returns TRUE on user abort */
static BOOL wfx_report_progress_callback(int percent, void* user_data) {
    WfxReportProgressContext* ctx = (WfxReportProgressContext*)user_data;
    if (ctx != NULL) InterlockedExchange(&ctx->current_percent, (LONG)percent);
    if (!g_ProgressProc) return FALSE;
    WCHAR* src = (ctx && ctx->remote_name) ? (WCHAR*)ctx->remote_name : L"";
    WCHAR* dst = (ctx && ctx->local_name) ? (WCHAR*)ctx->local_name : L"";
    return g_ProgressProc(g_PluginNr, src, dst, percent) != 0;
}

/* Polled every ~100 ms by the running restic command: repeats the last percentage, detects ESC */
static BOOL wfx_report_cancel_callback(void* user_data) {
    WfxReportProgressContext* ctx = (WfxReportProgressContext*)user_data;
    if (!g_ProgressProc) return FALSE;
    WCHAR* src = (ctx && ctx->remote_name) ? (WCHAR*)ctx->remote_name : L"";
    WCHAR* dst = (ctx && ctx->local_name) ? (WCHAR*)ctx->local_name : L"";
    const int percent =
        (ctx != NULL) ? (int)InterlockedCompareExchange(&ctx->current_percent, 0, 0) : 0;
    return g_ProgressProc(g_PluginNr, src, dst, percent) != 0;
}

/* How a request for a statistics report ended (FsExecuteFileW maps it to the SDK return codes) */
typedef enum {
    STATS_SHOWN,          /* The report was generated and handed to the Lister */
    STATS_NOT_APPLICABLE, /* Nothing to show (plugin root, a file, user cancelled): not an error */
    STATS_FAILED          /* Configuration, password or Restic problem */
} StatsOutcome;

/*
 * Generates the Markdown statistics report of a repository, snapshot or snapshot subfolder and
 * opens it in the Total Commander Lister. Both entry points that can request it end up here: F3 on
 * a folder (intercepted inside the folder-size calculation, see FsFindFirstW) and Alt+Enter or
 * "quote stats" (FsExecuteFileW).
 */
static StatsOutcome show_stats_report_for_path(const WCHAR* virtual_path) {
    if (virtual_path == NULL || virtual_path[0] == L'\0') return STATS_FAILED;

    WfxParsedPath parsed;
    if (!wfx_parse_path(virtual_path, &parsed)) return STATS_FAILED;
    /* The plugin root lists the aliases and has no repository of its own */
    if (parsed.level != WFX_PATH_REPO && parsed.level != WFX_PATH_SNAPSHOT_DIR) {
        return STATS_NOT_APPLICABLE;
    }

    ResticConfig cfg;
    if (!config_load(&cfg)) {
        config_free(&cfg);
        return STATS_FAILED;
    }
    const ResticRepoEntry* repo = config_find_repo(&cfg, parsed.repo_alias);
    if (!repo) {
        config_free(&cfg);
        return STATS_FAILED;
    }
    if (wcschr(cfg.restic_path, L'\\') == NULL && wcschr(cfg.restic_path, L'/') == NULL) {
        report_plugin_error(
            repo->alias,
            L"restic.exe not found. Set ResticPath in restic_plugin.ini or place restic.exe in the plugin directory / PATH."
        );
        config_free(&cfg);
        return STATS_FAILED;
    }

    const char* snapshot_id = (parsed.level == WFX_PATH_SNAPSHOT_DIR) ? parsed.snapshot_id : NULL;
    const WCHAR* subpath    = (parsed.level == WFX_PATH_SNAPSHOT_DIR) ? parsed.subpath : NULL;

    /* The speaking file name of the report is what the Lister shows in its title bar */
    WCHAR report_path[WFX_MAX_PATH_UNITS + 1] = {0};
    if (!restic_report_build_temp_path(repo->alias, snapshot_id, subpath, report_path,
                                       _countof(report_path)) ||
        !restic_report_pick_destination(report_path, _countof(report_path))) {
        config_free(&cfg);
        return STATS_FAILED;
    }

    if (g_ProgressProc && g_ProgressProc(g_PluginNr, (WCHAR*)virtual_path, report_path, 0) != 0) {
        config_free(&cfg);
        return STATS_NOT_APPLICABLE; /* ESC before anything started */
    }

    /* One context for both callbacks: the bar only moves forward and the file name never changes */
    WfxReportProgressContext report_prog_ctx = {virtual_path, report_path, 5};
    ResticExecOptions exec_opts = {WFX_RESTIC_TIMEOUT_MS, wfx_report_cancel_callback,
                                   &report_prog_ctx};

    WCHAR password[SESSION_MAX_PASSWORD] = {0}; /* Local copy, wiped before returning */
    BOOL have_password                   = session_get(repo->alias, password, _countof(password));
    WCHAR err_msg[512]                   = {0};
    ResticResult res                     = RESTIC_ERR_WRONG_PASSWORD;

    for (int attempt = 0; attempt < MAX_PASSWORD_ATTEMPTS; ++attempt) {
        if (!have_password) {
            if (!prompt_repository_password(repo->alias, attempt > 0, password, _countof(password))) {
                SecureZeroMemory(password, sizeof(password));
                config_free(&cfg);
                if (g_ProgressProc) {
                    g_ProgressProc(g_PluginNr, (WCHAR*)virtual_path, report_path, 100);
                }
                return STATS_NOT_APPLICABLE; /* The user cancelled the password dialog */
            }
            have_password = TRUE;
        }

        err_msg[0] = L'\0';
        res        = restic_report_generate(
            cfg.restic_path,
            repo->target,
            password,
            repo->alias,
            snapshot_id,
            subpath,
            report_path,
            &exec_opts,
            wfx_report_progress_callback,
            &report_prog_ctx,
            err_msg,
            _countof(err_msg)
        );

        if (res == RESTIC_ERR_WRONG_PASSWORD) {
            session_forget(repo->alias);
            SecureZeroMemory(password, sizeof(password));
            have_password = FALSE;
            continue;
        }
        break;
    }

    if (have_password && password_worth_caching(res)) {
        session_put(repo->alias, password);
    }
    SecureZeroMemory(password, sizeof(password));

    if (g_ProgressProc) {
        g_ProgressProc(g_PluginNr, (WCHAR*)virtual_path, report_path, 100);
    }

    StatsOutcome outcome = STATS_FAILED;
    if (res == RESTIC_OK) {
        wfx_trace_path("report written", report_path);
        if (restic_report_open_in_lister(report_path)) outcome = STATS_SHOWN;
    } else if (res == RESTIC_ERR_CANCELLED) {
        wfx_trace_printf("report cancelled by the user");
        outcome = STATS_NOT_APPLICABLE;
    } else if (res == RESTIC_ERR_PATH_NOT_FOUND) {
        /* Restic knows no such folder: the cursor was on a file (or on something that vanished) */
        wfx_trace_printf("report: not a folder of the snapshot");
        outcome = STATS_NOT_APPLICABLE;
    } else {
        handle_restic_failure(res, repo->alias, err_msg);
    }

    config_free(&cfg);
    return outcome;
}

/* Executes a Level 1 (snapshots) or Level 2 (directory) query with password prompt/retry */
static HANDLE execute_repo_query(
    const WCHAR* virtual_path,
    const WCHAR* restic_exe,
    const ResticRepoEntry* repo,
    const WfxParsedPath* parsed,
    WIN32_FIND_DATAW* FindData
) {
    /* Verify that restic.exe was resolved to an explicit path before prompting for a password */
    if (wcschr(restic_exe, L'\\') == NULL && wcschr(restic_exe, L'/') == NULL) {
        report_plugin_error(
            repo->alias,
            L"restic.exe not found. Set ResticPath in restic_plugin.ini or place restic.exe in the plugin directory / PATH."
        );
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    WfxCancelContext cancel_ctx = {virtual_path, repo->alias};
    ResticExecOptions exec_opts = {WFX_RESTIC_TIMEOUT_MS, wfx_cancel_callback, &cancel_ctx};

    WCHAR password[SESSION_MAX_PASSWORD] = {0}; /* Local copy, wiped before returning */
    BOOL have_password                   = session_get(repo->alias, password, _countof(password));
    WIN32_FIND_DATAW* items              = NULL;
    size_t count                         = 0;
    WCHAR err_msg[512]                   = {0};
    ResticResult res                     = RESTIC_ERR_WRONG_PASSWORD;

    for (int attempt = 0; attempt < MAX_PASSWORD_ATTEMPTS; ++attempt) {
        if (!have_password) {
            if (!prompt_repository_password(repo->alias, attempt > 0, password, _countof(password))) {
                SecureZeroMemory(password, sizeof(password));
                SetLastError(ERROR_CANCELLED);
                return INVALID_HANDLE_VALUE;
            }
            have_password = TRUE;
        }

        err_msg[0] = L'\0';
        if (parsed->level == WFX_PATH_REPO) {
            res = list_snapshots_and_cache_stats(
                restic_exe, repo, password, &exec_opts, &items, &count, err_msg, _countof(err_msg)
            );
        } else {
            res = restic_list_directory(
                restic_exe,
                repo->target,
                password,
                parsed->snapshot_id,
                parsed->subpath,
                &exec_opts,
                &items,
                &count,
                err_msg,
                _countof(err_msg)
            );
            if (res == RESTIC_OK) {
                ensure_snapshot_stats_cached(restic_exe, repo, password, &exec_opts);
            }
        }

        if (res == RESTIC_ERR_WRONG_PASSWORD) {
            session_forget(repo->alias);
            SecureZeroMemory(password, sizeof(password));
            have_password = FALSE;
            continue;
        }
        break;
    }

    if (have_password && password_worth_caching(res)) {
        session_put(repo->alias, password);
    }
    SecureZeroMemory(password, sizeof(password));

    if (res != RESTIC_OK) {
        handle_restic_failure(res, repo->alias, err_msg);
        return INVALID_HANDLE_VALUE;
    }

    return wrap_find_items(items, count, FindData);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        config_set_plugin_dir_from_module((HMODULE)hinstDLL);
        wfx_trace_init(hinstDLL);
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        clear_password_session();
        snapshot_stats_clear();
        restic_report_clear_cache();
    }
    return TRUE;
}

void __stdcall FsGetDefRootName(char* DefRootName, int maxlen) {
    if (DefRootName != NULL && maxlen > 0) {
        strncpy_s(DefRootName, (size_t)maxlen, "Restic Backups", _TRUNCATE);
    }
}

int __stdcall FsInit(
    int PluginNr,
    tProgressProc pProgressProc,
    tLogProc pLogProc,
    tRequestProc pRequestProc
) {
    (void)pProgressProc;
    (void)pLogProc;
    (void)pRequestProc;
    g_PluginNr = PluginNr;
    return 0;
}

int __stdcall FsInitW(
    int PluginNr,
    tProgressProcW pProgressProcW,
    tLogProcW pLogProcW,
    tRequestProcW pRequestProcW
) {
    clear_password_session();
    snapshot_stats_clear();
    restic_report_clear_cache();
    g_PluginNr     = PluginNr;
    g_ProgressProc = pProgressProcW;
    g_LogProc      = pLogProcW;
    g_RequestProc  = pRequestProcW;
    wfx_trace_printf("FsInitW plugin no=%d", PluginNr);
    return 0;
}

void __stdcall FsSetDefaultParams(FsDefaultParamStruct* dps) {
    if (dps != NULL) {
        config_set_fallback_from_default_params(dps->DefaultIniName);
        char ini_utf8[MAX_PATH * 4 + 1] = {0};
        if (wfx_trace_enabled()) {
            wfx_trace_ansi_to_utf8(dps->DefaultIniName, ini_utf8, sizeof(ini_utf8));
        }
        wfx_trace_printf("default ini: %s", ini_utf8);
    }
}

/* Populates a ResticFindHandle for the root path "\" from [Repositories] */
static HANDLE enumerate_root_repositories(WIN32_FIND_DATAW* FindData) {
    ResticConfig cfg;
    if (!config_load(&cfg) || cfg.repo_count == 0) {
        config_free(&cfg);
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    WIN32_FIND_DATAW* items = (WIN32_FIND_DATAW*)calloc(cfg.repo_count, sizeof(WIN32_FIND_DATAW));
    if (!items) {
        config_free(&cfg);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    size_t count = cfg.repo_count;
    for (size_t i = 0; i < count; ++i) {
        items[i].dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        FS_SET_TIME_UNKNOWN(items[i].ftLastWriteTime); /* Aliases have no date */
        wcsncpy_s(items[i].cFileName, MAX_PATH, cfg.repos[i].alias, _TRUNCATE);
    }

    config_free(&cfg);
    return wrap_find_items(items, count, FindData);
}

HANDLE __stdcall FsFindFirst(char* Path, WIN32_FIND_DATAA* FindData) {
    (void)Path;
    if (FindData) {
        memset(FindData, 0, sizeof(WIN32_FIND_DATAA));
    }
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return INVALID_HANDLE_VALUE;
}

HANDLE __stdcall FsFindFirstW(WCHAR* Path, WIN32_FIND_DATAW* FindData) {
    if (!Path || !FindData) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    /*
     * Total Commander has no "view a folder" entry point: F3 on a directory is implemented by
     * intercepting the folder-size calculation that TC starts for it. wfx_status remembers the F3
     * key state seen when that operation began; here the report is produced once and the whole
     * CALCSIZE scope answers "no files", so TC never walks the tree of the folder being reported.
     */
    if (wfx_status_is_f3_calcsize_aborted()) {
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }
    if (wfx_status_consume_f3_calcsize()) {
        wfx_trace_path("F3 on folder", Path);
        (void)show_stats_report_for_path(Path); /* The outcome is not reported through F3 */
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    memset(FindData, 0, sizeof(WIN32_FIND_DATAW));

    WfxParsedPath parsed;
    if (!wfx_parse_path(Path, &parsed)) {
        wfx_trace_path("findfirst rejected", Path);
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    wfx_trace_printf("findfirst level=%d", (int)parsed.level);
    wfx_trace_path("findfirst", Path);

    /* Level 0: Plugin Root "\" -> list configured repository aliases */
    if (parsed.level == WFX_PATH_ROOT) {
        return enumerate_root_repositories(FindData);
    }

    /* Level 1 & Level 2+: resolve repository configuration and invoke Restic */
    ResticConfig cfg;
    if (!config_load(&cfg)) {
        config_free(&cfg);
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    const ResticRepoEntry* repo = config_find_repo(&cfg, parsed.repo_alias);
    if (!repo) {
        config_free(&cfg);
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    HANDLE hdl = execute_repo_query(Path, cfg.restic_path, repo, &parsed, FindData);
    config_free(&cfg);
    return hdl;
}

BOOL __stdcall FsFindNext(HANDLE Hdl, WIN32_FIND_DATAA* FindData) {
    (void)Hdl;
    (void)FindData;
    return FALSE;
}

BOOL __stdcall FsFindNextW(HANDLE Hdl, WIN32_FIND_DATAW* FindData) {
    if (Hdl == INVALID_HANDLE_VALUE || Hdl == NULL || FindData == NULL) {
        return FALSE;
    }

    ResticFindHandle* handle = (ResticFindHandle*)Hdl;
    if (handle->current_index >= handle->count) {
        return FALSE;
    }

    *FindData = handle->items[handle->current_index++];
    return TRUE;
}

int __stdcall FsFindClose(HANDLE Hdl) {
    if (Hdl != INVALID_HANDLE_VALUE && Hdl != NULL) {
        ResticFindHandle* handle = (ResticFindHandle*)Hdl;
        free(handle->items);
        free(handle);
    }
    return 0;
}

typedef struct {
    WCHAR*    remote_name;
    WCHAR*    local_name;
    ULONGLONG total_size;
} WfxDumpProgressContext;

/* Reports byte-level progress to Total Commander during FsGetFileW; returns TRUE on user abort */
static BOOL wfx_dump_progress_callback(ULONGLONG bytes_written, void* user_data) {
    if (!g_ProgressProc) {
        return FALSE;
    }
    const WfxDumpProgressContext* ctx = (const WfxDumpProgressContext*)user_data;
    int pct = 0;
    if (ctx && ctx->total_size > 0) {
        ULONGLONG raw = (bytes_written * 100ULL) / ctx->total_size;
        pct = (raw >= 99ULL) ? 99 : (int)raw;
    }
    WCHAR* src = (ctx && ctx->remote_name) ? ctx->remote_name : L"";
    WCHAR* dst = (ctx && ctx->local_name) ? ctx->local_name : L"";
    return g_ProgressProc(g_PluginNr, src, dst, pct) != 0;
}

/*
 * Test-only entry point: forces the next folder-size calculation to be treated as an F3 request, so
 * the automated tests can replay the real Total Commander sequence without a physical keyboard.
 * Total Commander never calls it; it only sets a per-thread flag.
 */
void __stdcall restic_test_force_f3(BOOL force) {
    wfx_status_force_f3_next_calcsize(force);
}

int __stdcall FsExecuteFile(HWND MainWin, char* RemoteName, char* Verb) {
    /* The ANSI variant cannot carry a Unicode report path: only "open" is honoured */
    (void)MainWin;
    (void)RemoteName;
    if (Verb && _stricmp(Verb, "open") == 0) {
        return FS_EXEC_YOURSELF;
    }
    return FS_EXEC_ERROR;
}

/* TRUE for the command line verb "quote stats" (any spacing, nothing after the word) */
static BOOL is_stats_command(const WCHAR* verb) {
    if (_wcsnicmp(verb, L"quote", 5) != 0) return FALSE;
    const WCHAR* p = verb + 5;
    if (*p != L' ') return FALSE;
    while (*p == L' ') ++p;
    if (_wcsnicmp(p, L"stats", 5) != 0) return FALSE;
    p += 5;
    while (*p == L' ') ++p;
    return *p == L'\0';
}

int __stdcall FsExecuteFileW(HWND MainWin, WCHAR* RemoteName, WCHAR* Verb) {
    (void)MainWin;
    if (!RemoteName || !Verb) return FS_EXEC_ERROR;

    /* Enter on a file: Total Commander downloads and opens it the usual way */
    if (_wcsicmp(Verb, L"open") == 0) {
        return FS_EXEC_YOURSELF;
    }

    /* Alt+Enter on a folder, or the command line verb "quote stats" (RemoteName is then the current
       directory). Per the SDK FS_EXEC_OK also means "not applicable, nothing more to do": the plugin
       root, a file or a cancelled dialog must not end in an error message. */
    if (_wcsicmp(Verb, L"properties") == 0 || is_stats_command(Verb)) {
        return show_stats_report_for_path(RemoteName) == STATS_FAILED ? FS_EXEC_ERROR : FS_EXEC_OK;
    }

    return FS_EXEC_ERROR;
}

int __stdcall FsGetFile(char* RemoteName, char* LocalName, int CopyFlags, RemoteInfoStruct* ri) {
    (void)RemoteName;
    (void)LocalName;
    (void)CopyFlags;
    (void)ri;
    return FS_FILE_NOTSUPPORTED;
}

int __stdcall FsGetFileW(WCHAR* RemoteName, WCHAR* LocalName, int CopyFlags, RemoteInfoStruct* ri) {
    if (!RemoteName || !LocalName || RemoteName[0] == L'\0' || LocalName[0] == L'\0') {
        return FS_FILE_NOTFOUND;
    }

    /* Restic snapshots are read-only: move (delete after copy) and partial resume are unsupported */
    if ((CopyFlags & (FS_COPYFLAGS_MOVE | FS_COPYFLAGS_RESUME)) != 0) {
        return FS_FILE_NOTSUPPORTED;
    }
    if (ri != NULL && (ri->Attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return FS_FILE_NOTSUPPORTED;
    }

    WfxParsedPath parsed;
    if (!wfx_parse_path(RemoteName, &parsed) || parsed.level != WFX_PATH_SNAPSHOT_DIR ||
        !parsed.subpath) {
        return FS_FILE_NOTFOUND;
    }
    const WCHAR* inside = parsed.subpath;
    while (*inside == L'\\') ++inside;
    if (*inside == L'\0') {
        return FS_FILE_NOTFOUND; /* A snapshot folder is not a file */
    }

    /* Total Commander calls FsGetFileW first without FS_COPYFLAGS_OVERWRITE to probe for collisions */
    DWORD local_attr = GetFileAttributesW(LocalName);
    if (local_attr != INVALID_FILE_ATTRIBUTES) {
        if ((local_attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return FS_FILE_WRITEERROR;
        }
        if ((CopyFlags & FS_COPYFLAGS_OVERWRITE) == 0) {
            return FS_FILE_EXISTS;
        }
    }

    ResticConfig cfg;
    if (!config_load(&cfg)) {
        config_free(&cfg);
        return FS_FILE_NOTFOUND; /* No configuration file: the repository alias cannot exist */
    }
    const ResticRepoEntry* repo = config_find_repo(&cfg, parsed.repo_alias);
    if (!repo) {
        config_free(&cfg);
        return FS_FILE_NOTFOUND;
    }
    if (wcschr(cfg.restic_path, L'\\') == NULL && wcschr(cfg.restic_path, L'/') == NULL) {
        report_plugin_error(
            repo->alias,
            L"restic.exe not found. Set ResticPath in restic_plugin.ini or place restic.exe in the plugin directory / PATH."
        );
        config_free(&cfg);
        return FS_FILE_READERROR;
    }

    if (g_ProgressProc && g_ProgressProc(g_PluginNr, RemoteName, LocalName, 0) != 0) {
        config_free(&cfg);
        return FS_FILE_USERABORT;
    }

    ULONGLONG total_size            = ri ? (((ULONGLONG)ri->SizeHigh << 32) | ri->SizeLow) : 0;
    const FILETIME* mtime           = ri ? &ri->LastWriteTime : NULL;
    BOOL overwrite                  = (CopyFlags & FS_COPYFLAGS_OVERWRITE) != 0;
    WfxDumpProgressContext prog_ctx = {RemoteName, LocalName, total_size};

    WCHAR password[SESSION_MAX_PASSWORD] = {0};
    BOOL have_password                   = session_get(repo->alias, password, _countof(password));
    WCHAR err_msg[512]                   = {0};
    ResticResult res                     = RESTIC_ERR_WRONG_PASSWORD;

    for (int attempt = 0; attempt < MAX_PASSWORD_ATTEMPTS; ++attempt) {
        if (!have_password) {
            if (!prompt_repository_password(repo->alias, attempt > 0, password, _countof(password))) {
                SecureZeroMemory(password, sizeof(password));
                config_free(&cfg);
                return FS_FILE_USERABORT;
            }
            have_password = TRUE;
        }

        err_msg[0] = L'\0';
        res        = restic_dump_file(
            cfg.restic_path,
            repo->target,
            password,
            parsed.snapshot_id,
            parsed.subpath,
            LocalName,
            overwrite,
            mtime,
            WFX_RESTIC_TIMEOUT_MS,
            wfx_dump_progress_callback,
            &prog_ctx,
            err_msg,
            _countof(err_msg)
        );

        if (res == RESTIC_ERR_WRONG_PASSWORD) {
            session_forget(repo->alias);
            SecureZeroMemory(password, sizeof(password));
            have_password = FALSE;
            continue;
        }
        break;
    }

    if (have_password && password_worth_caching(res)) {
        session_put(repo->alias, password);
    }
    SecureZeroMemory(password, sizeof(password));

    int ret = FS_FILE_READERROR;
    switch (res) {
        case RESTIC_OK:
            /* LogProc protocol (WFX SDK): both names separated by " -> " and prefixed with the
               operation, so the FTP log window / log file shows what has been copied */
            if (g_LogProc != NULL) {
                WCHAR done_msg[2 * MAX_PATH + 64] = {0};
                _snwprintf_s(
                    done_msg,
                    _countof(done_msg),
                    _TRUNCATE,
                    L"Download complete: %ls -> %ls",
                    RemoteName,
                    LocalName
                );
                g_LogProc(g_PluginNr, MSGTYPE_TRANSFERCOMPLETE, done_msg);
            }
            if (g_ProgressProc) {
                g_ProgressProc(g_PluginNr, RemoteName, LocalName, 100);
            }
            ret = FS_FILE_OK;
            break;
        case RESTIC_ERR_CANCELLED:
            ret = FS_FILE_USERABORT;
            break;
        case RESTIC_ERR_PATH_NOT_FOUND:
            ret = FS_FILE_NOTFOUND;
            break;
        case RESTIC_ERR_LOCAL_WRITE:
            ret = FS_FILE_WRITEERROR;
            break;
        default:
            handle_restic_failure(res, repo->alias, err_msg);
            ret = FS_FILE_READERROR;
            break;
    }

    config_free(&cfg);
    return ret;
}
