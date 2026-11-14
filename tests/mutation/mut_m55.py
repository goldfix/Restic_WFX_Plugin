import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/wfx_status.c',
    '    if (scan->bar_other) return -1;',
    '    if (0) return -1;',
    sys.argv[1])
