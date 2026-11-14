import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_report.c',
'            SetFileAttributesW(destination, attr & ~FILE_ATTRIBUTE_READONLY);',
'            (void)attr;',
sys.argv[1])
