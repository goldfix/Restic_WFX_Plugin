import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/main.c',
    '        if (outcome == STATS_NOT_A_FOLDER && from_f3) restic_f3_request_native_view(MainWin);',
    '        (void)from_f3;\n        if (outcome == STATS_NOT_A_FOLDER) restic_f3_request_native_view(MainWin);',
    sys.argv[1])
