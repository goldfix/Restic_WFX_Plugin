/*
 * F3 on plugin folders.
 *
 * Total Commander has no "view a folder" entry point: pressing F3 on a directory of a file system
 * plugin never reaches the plugin, Total Commander answers "No files selected!" by itself without
 * calling any WFX function. This module installs one thread-local keyboard hook on Total
 * Commander's user interface thread (SetWindowsHookExW with hMod = NULL and the current thread id:
 * no system-wide injection, the hook lives only in the Total Commander process).
 *
 * Flow (see AGENT.md 5.3):
 *   - The Path Guard decides whether the active panel is this plugin. The file list that owns the
 *     keyboard focus must be a panel list of Total Commander's main window, and the path of the
 *     ACTIVE panel must be inside \\\Restic Backups\... The command line prompt ("<path>>") always
 *     shows the active panel and is authoritative; only when the command line is hidden the path
 *     bars are used, and then only if no other path shares the horizontal extent of the focus.
 *     In every other case (local folder, archive, another plugin, command line, dialog) F3 keeps
 *     its native behaviour, even when the *other* panel still shows a backup.
 *   - VK_F3 is swallowed and, as soon as the key is released, an Alt+Enter is injected: that is
 *     Total Commander's own "properties" command, and it reaches the plugin as
 *     FsExecuteFileW(..., L"properties") with the RemoteName of the row under the cursor.
 *   - For a folder the plugin shows the statistics report. For a file it answers "not a folder" and
 *     asks Total Commander to run `cm_List` on the row, which is exactly what F3 does natively
 *     (FsGetFileW then the configured Lister), so viewing files keeps working. That hand-back is
 *     done only for requests that originate from F3: a user's own Alt+Enter on a file must not
 *     start a download.
 */
#ifndef WFX_STATUS_H
#define WFX_STATUS_H

#include <windows.h>

/*
 * Verifies that a panel path text belongs to this plugin and returns its depth:
 *   -1 -> not a plugin path;
 *    0 -> "\\\Restic Backups\"           (repository list);
 *    1 -> "\\\Restic Backups\<repo>\"    (snapshot list);
 *  2+  -> inside a snapshot.
 * Both spellings used by Total Commander are accepted: the path bar above the panel
 * ("\\\Restic Backups\Repo\*.*") and the command line prompt ("\\\Restic Backups\Repo>"), with or
 * without trailing separators and wildcards.
 */
BOOL restic_f3_parse_panel_text(const WCHAR* text, int* out_depth);

/* TRUE for the window class of a Total Commander file list (the Path Guard's first check) */
BOOL restic_f3_is_panel_list_class(const WCHAR* class_name);

/*
 * Pure core of the Path Guard: collects the texts of the windows of Total Commander and answers
 * which depth of the plugin the ACTIVE panel shows (or -1).
 *   - a text ending in '>' is the command line prompt, which always describes the active panel and
 *     therefore decides alone;
 *   - without a prompt, the path bars whose horizontal extent contains the focus are used, and a
 *     bar of another location in that extent makes the answer "not the plugin" (panels above each
 *     other share the whole width, so the geometry cannot tell them apart).
 * Texts without a backslash that are not part of a path (status bars, labels) are ignored.
 */
typedef struct {
    BOOL prompt_seen;
    int prompt_depth; /* -1 when the prompt shows another location */
    BOOL bar_other;   /* a path of another location on the focus side */
    BOOL bar_plugin;
    int bar_depth;
} ResticF3Scan;

void restic_f3_scan_init(ResticF3Scan* scan);
void restic_f3_scan_add(ResticF3Scan* scan, const WCHAR* text, BOOL on_focus_side);
int restic_f3_scan_result(const ResticF3Scan* scan);

/* Installs the hook (no-op unless called on the thread that owns the keyboard focus) */
void restic_f3_hook_ensure_installed(void);
/* Removes the hook, the helper window and its class; safe to call twice */
void restic_f3_hook_shutdown(void);
BOOL restic_f3_hook_is_installed(void);

/* Window in which a "properties" request still counts as the result of an F3 that was injected */
#define RESTIC_F3_REQUEST_WINDOW_MS 10000ULL

/* Records that an F3 was turned into a "properties" request at the given tick */
void restic_f3_note_request(unsigned long long now_ms);
/* TRUE once per recorded request, as long as it is not older than the window */
BOOL restic_f3_take_request(unsigned long long now_ms);

/* Total Commander internal commands (TOTALCMD.INC) sent through WM_USER + 51 */
#define RESTIC_TC_WM_USER_CMD ((UINT)(WM_USER + 51))
#define RESTIC_TC_CMD_LIST    903 /* cm_List: view the row under the cursor with the Lister */

/* Asks Total Commander to view the row under the cursor (cm_List) */
void restic_f3_request_native_view(HWND tc_main_window);

#endif /* WFX_STATUS_H */
