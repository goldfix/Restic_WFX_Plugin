import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_report.c',
'    if (res == RESTIC_OK && !folder->root_seen) res = RESTIC_ERR_PATH_NOT_FOUND;',
'    (void)0;',
sys.argv[1])
