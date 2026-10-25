#pragma once
#include <windows.h>
#include "fsplugin.h"

/*
 * Content-column helpers shared with the unit tests.
 *
 * Note: the field values are addressed by Total Commander as `[=<fs>.field]` (the `<fs>` self
 * name) in the default view, and as `[=<plugin name>.field]` in user-created views; neither name
 * is stored here because the display name is chosen by Total Commander at install time and can
 * change (see src/fscontent.c and AGENT.md §5.2).
 */

/** Number of fields published by FsContentGetSupportedField (test helper). */
int fscontent_field_count(void);

/**
 * Forgets the cached RESTIC_WFX_TRACE setting, the line counter and the log path (test helper):
 * delegates to wfx_trace_reset(). Production code never needs it, because Total Commander sets
 * the environment (or the switch file) before loading the plugin.
 */
void fscontent_trace_reset(void);
