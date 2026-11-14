import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/main.c',
'    if (_wcsicmp(Verb, L"open") == 0) {',
'    if (_wcsicmp(Verb, L"open") == 1) {',
sys.argv[1])
