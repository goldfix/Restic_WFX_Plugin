import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_cli.c',
'    if (!stream->proc(stream->pending.data + stream->start, length, stream->user_data)) {',
'    if (!stream->proc(stream->pending.data + stream->start, length, stream->user_data) && 0) {',
sys.argv[1])
