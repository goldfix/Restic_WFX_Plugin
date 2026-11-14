import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run("src/restic_report.c",
"    } else if (strcmp(kind, \"statistics\") == 0 || strcmp(kind, \"stats\") == 0) {",
"    } else if (0) {",
sys.argv[1])
