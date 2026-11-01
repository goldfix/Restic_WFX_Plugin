/*
 * F3 on plugin folders: thread-local keyboard hook, Path Guard and the injected "properties"
 * request. See include/wfx_status.h for the design and AGENT.md 5.3 for the user-facing behaviour.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <wchar.h>
#include "fsplugin.h"
#include "wfx_status.h"
#include "wfx_trace.h"

/* "\\\Restic Backups": the root Total Commander shows in Network Neighborhood */
static const WCHAR k_plugin_prefix[] = L"\\\\\\Restic Backups";
#define PLUGIN_PREFIX_CHARS (_countof(k_plugin_prefix) - 1)

/* Classes of the file list controls of a Total Commander panel (LCLListBox verified on 11.58 x64,
   TMyListBox is the name used by older versions) */
static const WCHAR k_list_box_class[]     = L"LCLListBox";
static const WCHAR k_list_box_class_alt[] = L"TMyListBox";
/* Main window class of Total Commander (the real spelling is TTOTAL_CMD) */
static const WCHAR k_main_class[] = L"TTotal_CMD";

/* ------------------------------------------------------------------ pure helpers */

BOOL restic_f3_parse_panel_text(const WCHAR* text, int* out_depth) {
    if (out_depth != NULL) *out_depth = -1;
    if (text == NULL || text[0] == L'\0') return FALSE;

    while (*text == L' ' || *text == L'\t') ++text;

    if (_wcsnicmp(text, k_plugin_prefix, PLUGIN_PREFIX_CHARS) != 0) return FALSE;

    const WCHAR* p = text + PLUGIN_PREFIX_CHARS;
    /* Guard against a different root whose name only starts with the same characters */
    if (*p != L'\0' && *p != L'\\' && *p != L'>') return FALSE;

    /* Count the real path segments: separators, the trailing ">" of the command line prompt and a
       trailing wildcard mask such as "*.*" do not belong to the folder path */
    int depth = 0;
    while (*p != L'\0') {
        while (*p == L'\\') ++p;
        if (*p == L'\0' || *p == L'>') break;
        const WCHAR* segment = p;
        BOOL wildcard        = FALSE;
        while (*p != L'\0' && *p != L'\\' && *p != L'>') {
            if (*p == L'*' || *p == L'?') wildcard = TRUE;
            ++p;
        }
        if (p > segment && !wildcard) ++depth;
    }

    if (out_depth != NULL) *out_depth = depth;
    return TRUE;
}

BOOL restic_f3_is_panel_list_class(const WCHAR* class_name) {
    if (class_name == NULL || class_name[0] == L'\0') return FALSE;
    return _wcsicmp(class_name, k_list_box_class) == 0 ||
           _wcsicmp(class_name, k_list_box_class_alt) == 0;
}

void restic_f3_scan_init(ResticF3Scan* scan) {
    memset(scan, 0, sizeof(*scan));
    scan->prompt_depth = -1;
    scan->bar_depth    = -1;
}

void restic_f3_scan_add(ResticF3Scan* scan, const WCHAR* text, BOOL on_focus_side) {
    if (scan == NULL || text == NULL || text[0] == L'\0') return;

    int depth          = -1;
    const BOOL plugin  = restic_f3_parse_panel_text(text, &depth);
    const size_t chars = wcslen(text);

    if (text[chars - 1] == L'>') {
        /* The command line prompt: "<path of the active panel>>"; the first one decides */
        if (!scan->prompt_seen) {
            scan->prompt_seen  = TRUE;
            scan->prompt_depth = plugin ? depth : -1;
        }
        return;
    }
    if (!on_focus_side) return;

    if (plugin) {
        scan->bar_plugin = TRUE;
        if (depth > scan->bar_depth) scan->bar_depth = depth;
    } else if (wcschr(text, L'\\') != NULL) {
        scan->bar_other = TRUE; /* a local, network, archive or other-plugin path */
    }
}

int restic_f3_scan_result(const ResticF3Scan* scan) {
    if (scan->prompt_seen) return scan->prompt_depth;
    if (scan->bar_other) return -1;
    return scan->bar_plugin ? scan->bar_depth : -1;
}

/* ------------------------------------------------------------------ request marker */

static volatile LONG64 s_request_tick;

void restic_f3_note_request(unsigned long long now_ms) {
    InterlockedExchange64(&s_request_tick, (LONG64)((now_ms == 0) ? 1 : now_ms));
}

BOOL restic_f3_take_request(unsigned long long now_ms) {
    const unsigned long long noted = (unsigned long long)InterlockedExchange64(&s_request_tick, 0);
    /* Unsigned arithmetic: a clock that went backwards wraps to a huge difference and is refused */
    return noted != 0 && (now_ms - noted) <= RESTIC_F3_REQUEST_WINDOW_MS;
}

/* ------------------------------------------------------------------ Win32 glue */

#define HELPER_WINDOW_TITLE L"ResticWfxF3Helper"
#define HELPER_CLASS_NAME   L"ResticWfxF3HelperWindow"
#define HELPER_MSG_ALT_DOWN (WM_APP + 1)
#define HELPER_MSG_ENTER    (WM_APP + 2)

static HHOOK s_key_hook;
static DWORD s_hook_thread;  /* thread that owns the hook and the helper window */
static HWND s_helper_window; /* message-only window, created on Total Commander's thread */
static HINSTANCE s_helper_module;
static BOOL s_f3_swallowed; /* F3 key-down was suppressed: the report starts on its key-up */

static const WCHAR* class_of(HWND hwnd, WCHAR* buffer, size_t chars) {
    buffer[0] = L'\0';
    if (hwnd != NULL) GetClassNameW(hwnd, buffer, (int)chars);
    return buffer;
}

static BOOL modifiers_down(void) {
    return ((GetKeyState(VK_SHIFT) & 0x8000) != 0) || ((GetKeyState(VK_CONTROL) & 0x8000) != 0) ||
           ((GetKeyState(VK_MENU) & 0x8000) != 0);
}

static void send_input_key(WORD vk, BOOL up) {
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type       = INPUT_KEYBOARD;
    input.ki.wVk     = vk;
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &input, sizeof(input));
}

/* Context and callback of the panel scan */
typedef struct {
    LONG center_x;
    ResticF3Scan scan;
} F3PanelScan;

static BOOL CALLBACK scan_panel_child(HWND child, LPARAM param) {
    F3PanelScan* ctx = (F3PanelScan*)param;
    if (!IsWindowVisible(child)) return TRUE;

    WCHAR text[512];
    if (GetWindowTextW(child, text, _countof(text)) <= 0) return TRUE;

    RECT rect;
    const BOOL on_focus_side =
        GetWindowRect(child, &rect) && ctx->center_x >= rect.left && ctx->center_x <= rect.right;
    restic_f3_scan_add(&ctx->scan, text, on_focus_side);
    return TRUE;
}

/* Returns TRUE and fills the depth when the ACTIVE panel of Total Commander shows this plugin */
static BOOL active_panel_depth(int* out_depth) {
    *out_depth = -1;

    HWND focus = GetFocus();
    if (focus == NULL) return FALSE;

    WCHAR class_name[64];
    if (!restic_f3_is_panel_list_class(class_of(focus, class_name, _countof(class_name)))) return FALSE;

    HWND main_window = GetAncestor(focus, GA_ROOT);
    if (main_window == NULL) return FALSE;
    if (_wcsicmp(class_of(main_window, class_name, _countof(class_name)), k_main_class) != 0) return FALSE;
    /* Only while Total Commander is the application the user is typing into */
    if (GetForegroundWindow() != main_window) return FALSE;

    RECT focus_rect;
    if (!GetWindowRect(focus, &focus_rect)) return FALSE;

    F3PanelScan ctx;
    ctx.center_x = (focus_rect.left + focus_rect.right) / 2;
    restic_f3_scan_init(&ctx.scan);
    EnumChildWindows(main_window, scan_panel_child, (LPARAM)&ctx);

    *out_depth = restic_f3_scan_result(&ctx.scan);
    return *out_depth >= 0;
}

/*
 * Injected Alt+Enter, sent in two steps: the message posted from the hook presses Alt and the one
 * posted from the window procedure completes the chord, so the F3 key-up is always processed first.
 * A message-only window owned by the plugin is used instead of a timer because Total Commander only
 * dispatches messages addressed to a window.
 */
static LRESULT CALLBACK helper_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case HELPER_MSG_ALT_DOWN: {
            /* Never type into another application, and never into another panel: the guard is
               checked again now because the user may have moved on since the key was pressed */
            int depth = -1;
            if (!active_panel_depth(&depth)) {
                wfx_trace_printf("f3: injection skipped, the active panel is not the plugin");
                return 0;
            }
            restic_f3_note_request(GetTickCount64());
            send_input_key(VK_MENU, FALSE);
            PostMessageW(hwnd, HELPER_MSG_ENTER, 0, 0);
            return 0;
        }
        case HELPER_MSG_ENTER:
            /* Alt must be down when Enter is processed */
            Sleep(20);
            send_input_key(VK_RETURN, FALSE);
            send_input_key(VK_RETURN, TRUE);
            send_input_key(VK_MENU, TRUE);
            wfx_trace_printf("f3: Alt+Enter injected");
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

static HWND create_helper_window(void) {
    if (s_helper_module == NULL &&
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)helper_window_proc, &s_helper_module)) {
        return NULL;
    }

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = helper_window_proc;
    wc.hInstance     = s_helper_module;
    wc.lpszClassName = HELPER_CLASS_NAME;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        wfx_trace_printf("f3: RegisterClassExW failed error=%lu", (unsigned long)GetLastError());
        return NULL;
    }
    HWND hwnd = CreateWindowExW(0, HELPER_CLASS_NAME, HELPER_WINDOW_TITLE, 0, 0, 0, 0, 0, HWND_MESSAGE,
                                NULL, s_helper_module, NULL);
    if (hwnd == NULL) {
        wfx_trace_printf("f3: helper window failed error=%lu", (unsigned long)GetLastError());
    }
    return hwnd;
}

static void schedule_properties_request(void) {
    if (s_helper_window == NULL) return;
    PostMessageW(s_helper_window, HELPER_MSG_ALT_DOWN, 0, 0);
}

static LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && wparam == VK_F3) {
        if ((lparam & 0x80000000UL) != 0) {
            /* The F3 that was swallowed is released: now Alt+Enter can be injected */
            if (s_f3_swallowed) {
                s_f3_swallowed = FALSE;
                schedule_properties_request();
            }
        } else if (!modifiers_down()) {
            int depth = -1;
            if (active_panel_depth(&depth)) {
                s_f3_swallowed = TRUE;
                wfx_trace_printf("f3: intercepted at depth=%d", depth);
                return 1; /* Total Commander never sees F3, so it cannot show its own dialog */
            }
            /* Not our panel (local folder, archive, another plugin, command line, dialog): Total
               Commander keeps its own F3, and no earlier interception may fire on this key-up */
            s_f3_swallowed = FALSE;
            wfx_trace_printf("f3: left to Total Commander");
        }
    }
    return CallNextHookEx(s_key_hook, code, wparam, lparam);
}

void restic_f3_hook_ensure_installed(void) {
    /* A background thread (F5 copy, search, Ctrl+B) has no keyboard focus: retry on a later listing */
    if (GetFocus() == NULL) return;

    if (s_key_hook != NULL) {
        if (s_helper_window == NULL && GetCurrentThreadId() == s_hook_thread) {
            s_helper_window = create_helper_window();
        }
        return;
    }

    const DWORD thread_id = GetCurrentThreadId();
    HHOOK key_hook        = SetWindowsHookExW(WH_KEYBOARD, keyboard_hook_proc, NULL, thread_id);
    if (key_hook == NULL) {
        wfx_trace_printf("f3: SetWindowsHookExW failed error=%lu", (unsigned long)GetLastError());
        return;
    }
    s_key_hook      = key_hook;
    s_hook_thread   = thread_id;
    s_helper_window = create_helper_window();
    wfx_trace_printf("f3: hook installed on thread=%lu", (unsigned long)thread_id);
}

BOOL restic_f3_hook_is_installed(void) {
    return s_key_hook != NULL;
}

void restic_f3_hook_shutdown(void) {
    if (s_key_hook != NULL) {
        UnhookWindowsHookEx(s_key_hook);
        s_key_hook = NULL;
    }
    if (s_helper_window != NULL) {
        /* A window can only be destroyed by the thread that created it */
        if (IsWindow(s_helper_window) && GetCurrentThreadId() == s_hook_thread) {
            DestroyWindow(s_helper_window);
        }
        s_helper_window = NULL;
    }
    /* The window procedure lives in this DLL: a class left registered would point into unloaded
       code when the plugin is loaded again */
    if (s_helper_module != NULL) {
        UnregisterClassW(HELPER_CLASS_NAME, s_helper_module);
        s_helper_module = NULL;
    }
    s_f3_swallowed = FALSE;
    s_hook_thread  = 0;
}

void restic_f3_request_native_view(HWND tc_main_window) {
    if (tc_main_window == NULL || !IsWindow(tc_main_window)) return;
    wfx_trace_printf("f3: not a folder, asking Total Commander to view the file");
    PostMessageW(tc_main_window, RESTIC_TC_WM_USER_CMD, (WPARAM)RESTIC_TC_CMD_LIST, 0);
}
