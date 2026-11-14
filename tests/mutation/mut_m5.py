import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/main.c',
'    if (snapshot_stats_lookup_alias(repo->alias, NULL, 0, &cached) && cached > 0) {',
'    if (snapshot_stats_lookup_alias(repo->alias, NULL, 0, &cached) || cached == 0) {',
sys.argv[1])
