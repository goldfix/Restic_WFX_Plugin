import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
# Single-line pattern: the source files use CRLF endings
mut.run('src/restic_cli.c',
        'while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {',
        'while (0) {',
        sys.argv[1])
