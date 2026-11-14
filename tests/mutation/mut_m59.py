import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_report.c',
    'L"\\"%ls\\" /S=L \\"%ls\\""',
    'L"\\"%ls\\" /S=L:T7 \\"%ls\\""',
    sys.argv[1])
