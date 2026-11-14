import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_cli.c',
'if (out_idx == 1 && strncmp(p, UNC_DISPLAY_PAIR_UTF8, 6) == 0) {',
'if (out_idx == 999 && strncmp(p, UNC_DISPLAY_PAIR_UTF8, 6) == 0) {',
sys.argv[1])
