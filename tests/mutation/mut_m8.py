import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_stats.c',
'            "%s(%zu) ",\n            alias_utf8,',
'            "%ls(%zu) ",\n            g_Cache[i].alias,',
sys.argv[1])
