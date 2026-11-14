import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/wfx_status.c',
    '(now_ms - noted) <= RESTIC_F3_REQUEST_WINDOW_MS',
    '((void)now_ms, 1)',
    sys.argv[1])
