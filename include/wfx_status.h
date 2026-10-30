#pragma once
#include <windows.h>

/**
 * Tracks the operation Total Commander is currently performing (FsStatusInfo / FsStatusInfoW) and
 * detects the F3 pressed on a folder.
 *
 * The statistics report is no longer a virtual file inside the listings: the user puts the cursor
 * on a repository, a snapshot or a snapshot subfolder and presses F3 (or Alt+Enter). Total Commander
 * has no "view a folder" entry point, so pressing F3 on a directory starts the folder-size
 * calculation (FS_STATUS_OP_CALCSIZE) instead. The F3 key state at the moment that operation starts
 * tells the two intents apart:
 *
 *   F3        -> remember that a report must be produced; the next FsFindFirstW of that CALCSIZE
 *                scope generates it and returns "no files", which ends the operation immediately.
 *   Space bar -> nothing happens here, Total Commander calculates the folder size as usual.
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
 * Returns TRUE once per F3-triggered FS_STATUS_OP_CALCSIZE scope, when FsFindFirstW is called for
 * the selected folder. Further calls in the same scope return FALSE, and
 * wfx_status_is_f3_calcsize_aborted() keeps returning TRUE until FS_STATUS_END: the whole CALCSIZE
 * scope answers "no files", so Total Commander never walks the tree of the folder being reported.
 */
BOOL wfx_status_consume_f3_calcsize(void);

/* TRUE once the F3 request of the current CALCSIZE scope has been served */
BOOL wfx_status_is_f3_calcsize_aborted(void);

/**
 * Test helper: when set to TRUE the next FS_STATUS_OP_CALCSIZE start is treated as if F3 was held
 * down, so unit and DLL tests are deterministic without physical keyboard input.
 */
void wfx_status_force_f3_next_calcsize(BOOL force);

/* Forgets the whole stack and every F3 flag of this thread (test helper) */
void wfx_status_reset(void);
