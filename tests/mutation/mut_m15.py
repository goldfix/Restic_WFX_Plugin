import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_cli.c',
" || *host == '?' || *host == '.'",
'',
sys.argv[1])
