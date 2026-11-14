import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/main.c',
'    } else if (res == RESTIC_ERR_PATH_NOT_FOUND) {',
'    } else if (0) {',
sys.argv[1])
