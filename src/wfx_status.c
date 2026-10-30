#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include "fsplugin.h"
#include "wfx_status.h"
#include "wfx_trace.h"

/*
 * Per-thread operation stack. Total Commander calls FsStatusInfoW(FS_STATUS_START) before an
 * operation and FsStatusInfoW(FS_STATUS_END) afterwards, on the thread that performs it, so plain
 * TLS (no additional lock) is both correct and race free.
 */
static __declspec(thread) int tls_operations[WFX_STATUS_STACK_MAX];
static __declspec(thread) int tls_depth = 0;
static __declspec(thread) BOOL tls_f3_pending = FALSE; /* F3 seen, report not produced yet */
static __declspec(thread) BOOL tls_f3_aborted = FALSE; /* Report produced in this CALCSIZE scope */
static __declspec(thread) BOOL tls_force_f3_next = FALSE;

void wfx_status_force_f3_next_calcsize(BOOL force) {
    tls_force_f3_next = force;
}

void wfx_status_begin(int operation) {
    /* A notification that does not fit the stack is dropped as a whole: arming F3 for an operation
       that is not on the stack would leave a request that nothing can ever consume or clear */
    if (tls_depth >= WFX_STATUS_STACK_MAX) return;
    tls_operations[tls_depth++] = operation;
    if (operation == FS_STATUS_OP_CALCSIZE) {
        const BOOL f3_down = tls_force_f3_next || ((GetAsyncKeyState(VK_F3) & 0x8000) != 0);
        tls_force_f3_next  = FALSE;
        if (f3_down) {
            tls_f3_pending = TRUE;
            tls_f3_aborted = FALSE;
            wfx_trace_printf("F3 intercepted on FS_STATUS_OP_CALCSIZE");
        }
    }
}

void wfx_status_end(void) {
    if (tls_depth > 0) {
        const int ended = tls_operations[--tls_depth];
        if (ended == FS_STATUS_OP_CALCSIZE) {
            tls_f3_pending = FALSE;
            tls_f3_aborted = FALSE;
        }
    } else {
        /* An unbalanced END still clears the request, so a stray notification cannot leak it into
           the next operation of this thread */
        tls_f3_pending = FALSE;
        tls_f3_aborted = FALSE;
    }
}

int wfx_status_current_operation(void) {
    return (tls_depth > 0) ? tls_operations[tls_depth - 1] : 0;
}

BOOL wfx_status_consume_f3_calcsize(void) {
    if (tls_f3_pending && wfx_status_current_operation() == FS_STATUS_OP_CALCSIZE) {
        tls_f3_pending = FALSE;
        tls_f3_aborted = TRUE;
        return TRUE;
    }
    return FALSE;
}

BOOL wfx_status_is_f3_calcsize_aborted(void) {
    return tls_f3_aborted && wfx_status_current_operation() == FS_STATUS_OP_CALCSIZE;
}

void wfx_status_reset(void) {
    tls_depth         = 0;
    tls_f3_pending    = FALSE;
    tls_f3_aborted    = FALSE;
    tls_force_f3_next = FALSE;
}

static void apply_status(int start_end, int operation) {
    wfx_trace_printf("status %s operation=%d", start_end == FS_STATUS_START ? "start" : "end", operation);
    if (start_end == FS_STATUS_START) {
        wfx_status_begin(operation);
    } else if (start_end == FS_STATUS_END) {
        wfx_status_end();
    }
}

void __stdcall FsStatusInfo(char* RemoteDir, int InfoStartEnd, int InfoOperation) {
    (void)RemoteDir;
    apply_status(InfoStartEnd, InfoOperation);
}

void __stdcall FsStatusInfoW(WCHAR* RemoteDir, int InfoStartEnd, int InfoOperation) {
    (void)RemoteDir;
    apply_status(InfoStartEnd, InfoOperation);
}
