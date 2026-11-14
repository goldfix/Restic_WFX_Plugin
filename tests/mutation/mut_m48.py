import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/wfx_status.c',
'            if (*p == L\'*\' || *p == L\'?\') wildcard = TRUE;',
'            if (0) wildcard = TRUE;',
sys.argv[1])
