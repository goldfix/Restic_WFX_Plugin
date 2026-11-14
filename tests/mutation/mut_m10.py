import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/wfx_trace.c',
'    int written = WideCharToMultiByte(\n        CP_UTF8, 0, src, -1, out, (int)capacity, NULL, NULL\n    );',
'    int written = WideCharToMultiByte(\n        CP_ACP, 0, src, -1, out, (int)capacity, NULL, NULL\n    );',
sys.argv[1])
