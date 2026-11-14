import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/fscontent.c',
'                matched = snapshot_stats_lookup(alias, short_id, out_info);',
'                matched = FALSE;',
sys.argv[1])
