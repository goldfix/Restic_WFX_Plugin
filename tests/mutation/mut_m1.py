import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_cli.c',
"    if (open_bracket > snapshot_dirname && open_bracket[-1] != L'_') {\n        return FALSE;\n    }\n    const WCHAR* close_bracket = wcschr(open_bracket + 1, L']');\n    if (!close_bracket || close_bracket == open_bracket + 1 || close_bracket[1] != L'\\0') {",
"    const WCHAR* close_bracket = wcschr(open_bracket + 1, L']');\n    if (!close_bracket || close_bracket == open_bracket + 1) {",
sys.argv[1])
