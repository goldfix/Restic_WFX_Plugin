import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_cli.c',
'        if (result == RESTIC_OK && stream.pending.len > stream.start) {',
'        if (0) {',
sys.argv[1])
