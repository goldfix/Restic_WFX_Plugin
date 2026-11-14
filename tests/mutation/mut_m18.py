import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_report.c',
'        folder->direct_files++;',
'        folder->direct_files += 0;',
sys.argv[1])
