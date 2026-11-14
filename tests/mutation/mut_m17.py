import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run("src/restic_report.c",
"    0ULL, 1ULL, 10240ULL, 102400ULL, 1048576ULL, 10485760ULL",
"    0ULL, 0ULL, 10240ULL, 102400ULL, 1048576ULL, 10485760ULL",
sys.argv[1])
