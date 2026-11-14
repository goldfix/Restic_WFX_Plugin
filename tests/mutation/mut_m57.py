import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/wfx_status.c',
    '    return noted != 0 &&',
    '    return',
    sys.argv[1])
