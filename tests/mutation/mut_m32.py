import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_cli.c',
'        ResticResult res      = line_stream_emit(stream, line_end, line_end + 1);',
'        ResticResult res      = line_stream_emit(stream, line_end + 1, line_end + 1);',
sys.argv[1])
