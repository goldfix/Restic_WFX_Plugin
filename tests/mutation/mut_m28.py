import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_report.c',
'        while (length > 0 && ((unsigned char)src[length] & 0xC0) == 0x80) --length;',
'        (void)0;',
sys.argv[1])
