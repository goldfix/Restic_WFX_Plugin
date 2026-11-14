import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_report.c',
'    return result_is_fatal(res) ? res : RESTIC_OK;',
'    return RESTIC_OK;',
sys.argv[1])
