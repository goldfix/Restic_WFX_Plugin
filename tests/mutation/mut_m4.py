import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/config.c',
'        config_set_fallback_dir(env_dir);\n        return;',
'        config_set_fallback_dir(NULL);\n        return;',
sys.argv[1])
