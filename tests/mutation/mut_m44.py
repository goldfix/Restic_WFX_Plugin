import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/main.c',
"    return *p == L'\\0';\n}\n\nint __stdcall FsExecuteFileW",
'    return TRUE;\n}\n\nint __stdcall FsExecuteFileW',
sys.argv[1])
