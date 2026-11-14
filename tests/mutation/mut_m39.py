import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mut
mut.run('src/restic_report.c',
'        GetTickCount64() - g_ReportCache.stamp <= REPORT_CACHE_TTL_MS &&',
'        GetTickCount64() - g_ReportCache.stamp < 0 &&',
sys.argv[1])
