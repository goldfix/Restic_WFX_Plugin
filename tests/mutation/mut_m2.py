import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut

OLD = (
    '        wcscmp(fd->cFileName, L"..") == 0 || wcschr(fd->cFileName, L\'' + chr(92) + chr(92) + '\') != NULL ||\n'
    '        wcschr(fd->cFileName, L\'/\') != NULL) {'
)
NEW = '        wcscmp(fd->cFileName, L"..") == 0) {'

mut.run('src/restic_cli.c', OLD, NEW, sys.argv[1])
