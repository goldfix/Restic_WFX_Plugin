import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_report.c',
'        if (c < 0x20 || wcschr(L"<>:\\"/\\\\|?*", c) != NULL) c = L\'_\';',
'        (void)c;',
sys.argv[1])
