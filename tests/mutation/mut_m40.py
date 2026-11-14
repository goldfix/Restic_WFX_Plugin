import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_report.c',
'            if (type == NULL || is_dir) folder->root_seen = TRUE;',
'            folder->root_seen = TRUE;',
sys.argv[1])
