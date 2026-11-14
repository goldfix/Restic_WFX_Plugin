import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/wfx_status.c',
    "    if (*p != L'\\0' && *p != L'\\\\' && *p != L'>') return FALSE;",
    "if (0) return FALSE;",
    sys.argv[1])
