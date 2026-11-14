import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/wfx_status.c',
    '    if (scan->prompt_seen) return scan->prompt_depth;',
    '    if (0) return scan->prompt_depth;',
    sys.argv[1])
