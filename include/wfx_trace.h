#pragma once
#include <windows.h>

/**
 * Optional diagnostic trace for the content columns and the plugin lifecycle.
 *
 * It is disabled unless the environment variable RESTIC_WFX_TRACE is set to a value other than
 * "0", or a file named restic_wfx_trace.on exists next to the plugin DLL (the file variant also
 * works when Total Commander was started before the variable was defined).
 *
 * When enabled, every call is appended to %TEMP%\restic_wfx_trace.log (fallback: the plugin
 * directory). The file is recreated by the first call of a session and capped at
 * WFX_TRACE_MAX_LINES lines, so it can stay enabled without filling the disk.
 */

#define WFX_TRACE_MAX_LINES 400

/** Enables the trace and remembers the directory of the plugin DLL (call from DllMain / FsInitW). */
void wfx_trace_init(HINSTANCE self);

/** Returns TRUE when the trace is enabled (result cached; also checks the switch file). */
BOOL wfx_trace_enabled(void);

/** Appends one formatted line (ignored when the trace is disabled). */
void wfx_trace_printf(const char* format, ...);

/** Appends `tag: <path>` for a wide path (ignored when the trace is disabled). */
void wfx_trace_path(const char* tag, const WCHAR* path);

/** Forgets the cached settings so the next call re-reads the environment (tests only). */
void wfx_trace_reset(void);

/** Returns the full trace file path currently in use (tests only; empty when unknown). */
void wfx_trace_file_path(WCHAR* out, size_t capacity);
