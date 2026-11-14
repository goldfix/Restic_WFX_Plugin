import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_stats.c',
'            /* Never cut in the middle of a multi-byte UTF-8 sequence: drop the partial character\n               entirely, otherwise the column would end with a broken character. */\n            while (copy_len > 0 && ((unsigned char)text[copy_len] & 0xC0) == 0x80) {\n                --copy_len;\n            }',
'',
sys.argv[1])
