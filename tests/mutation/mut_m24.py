import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_report.c',
"if (dot == NULL || dot == name || dot[1] == '\\0') {",
"if (dot == NULL || dot[1] == '\\0') {",
sys.argv[1])
