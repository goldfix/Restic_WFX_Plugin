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

void wfx_status_begin(int operation) {
    if (tls_depth < WFX_STATUS_STACK_MAX) {
        tls_operations[tls_depth++] = operation;
    }
}

void wfx_status_end(void) {
    if (tls_depth > 0) {
        tls_depth--;
    }
}

int wfx_status_current_operation(void) {
    return (tls_depth > 0) ? tls_operations[tls_depth - 1] : 0;
}

BOOL wfx_status_should_expose_report(void) {
    switch (wfx_status_current_operation()) {
        /* Operations that walk directories and would copy or inspect the report as a real file */
        case FS_STATUS_OP_GET_MULTI:
        case FS_STATUS_OP_GET_MULTI_THREAD:
        case FS_STATUS_OP_CALCSIZE:
        case FS_STATUS_OP_SEARCH:
        case FS_STATUS_OP_SEARCH_TEXT:
        case FS_STATUS_OP_SYNC_SEARCH:
        case FS_STATUS_OP_SYNC_GET:
            return FALSE;
        default:
            /* FS_STATUS_OP_LIST, FS_STATUS_OP_GET_SINGLE (F3, Ctrl+Q, F5 on the file), no op */
            return TRUE;
    }
}

void wfx_status_reset(void) {
    tls_depth = 0;
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
