import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/wfx_status.c',
    '    return _wcsicmp(class_name, k_list_box_class) == 0 ||',
    '    return 1 ||',
    sys.argv[1])
