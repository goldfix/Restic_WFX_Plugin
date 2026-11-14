import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_cli.c',
"if (at_root && fd->cFileName[0] == L'\\\\' && fd->cFileName[1] == L'\\\\') {",
"if (!at_root && fd->cFileName[0] == L'\\\\' && fd->cFileName[1] == L'\\\\') {",
sys.argv[1])
