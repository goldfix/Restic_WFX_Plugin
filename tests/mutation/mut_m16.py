import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run("src/restic_report.c",
"if (ratio > 0.0 && filled == 0) filled = 1;",
"",
sys.argv[1])
