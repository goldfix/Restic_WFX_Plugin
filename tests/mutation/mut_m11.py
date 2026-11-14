import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut

OLD = (
    "        if (units > 0 && src[units - 1] >= 0xD800 && src[units - 1] <= 0xDBFF) {\n"
    "            --units;\n"
    "        }\n"
)
NEW = ""
mut.run('src/wfx_trace.c', OLD, NEW, sys.argv[1])
