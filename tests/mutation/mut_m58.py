import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_report.c',
    "    full[n++] = '`';\n    full[n++] = '[';",
    "    full[n++] = '[';",
    sys.argv[1])
