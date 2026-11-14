import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_cli.c',
'        restic_add_unc_volumes(snapshots_out.output, out_items, out_count);',
'        (void)snapshots_out.output;',
sys.argv[1])
