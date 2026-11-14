import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/main.c',
'                g_LogProc(g_PluginNr, MSGTYPE_TRANSFERCOMPLETE, done_msg);',
'                (void)done_msg;',
sys.argv[1])
