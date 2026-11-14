import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run("src/restic_report.c",
"    while (position > 0 && folder->largest[position - 1].size < size) {",
"    while (position > 0 && folder->largest[position - 1].size > size) {",
sys.argv[1])
