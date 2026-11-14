import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_report.c',
'    return GetLastError() != ERROR_SHARING_VIOLATION; /* Missing file or other errors: just try */',
'    return TRUE;',
sys.argv[1])
