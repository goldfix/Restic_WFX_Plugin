#pragma once
#include <windows.h>

/**
 * Tracks the operation Total Commander is currently performing (FsStatusInfo / FsStatusInfoW).
 *
 * The virtual statistics report is a convenience for the interactive user: it must never be
 * extracted by operations that walk the whole tree (recursive F5 copy, Alt+F7 search, space bar
 * folder size calculation, directory synchronisation), otherwise the copied backup would contain
 * an extra file the user never had.
 *
 * Total Commander may drive the plugin from several background threads, so the state is kept per
 * thread (TLS) as a small stack for nested operations.
 */

/* Maximum nesting depth of FS_STATUS_START/FS_STATUS_END pairs tracked per thread */
#define WFX_STATUS_STACK_MAX 8

/* Pushes an operation reported with FS_STATUS_START (deeper than WFX_STATUS_STACK_MAX is ignored) */
void wfx_status_begin(int operation);

/* Pops the innermost operation reported with FS_STATUS_END (a no-op on an empty stack) */
void wfx_status_end(void);

/* Returns the innermost operation of this thread, or 0 when Total Commander reported none */
int wfx_status_current_operation(void);

/**
 * Returns TRUE when the virtual statistics report may be listed, i.e. when the current operation is
 * interactive. Mass traversal operations and searches hide it; no reported operation at all (older
 * Total Commander versions, unit tests calling the entry points directly) counts as interactive.
 */
BOOL wfx_status_should_expose_report(void);

/* Forgets the whole stack of this thread (test helper) */
void wfx_status_reset(void);
