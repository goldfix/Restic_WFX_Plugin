import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/wfx_status.c',
    '    PostMessageW(tc_main_window, RESTIC_TC_WM_USER_CMD, (WPARAM)RESTIC_TC_CMD_LIST, 0);',
    '    (void)tc_main_window;',
    sys.argv[1])
