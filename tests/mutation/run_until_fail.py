"""Runs a test executable and stops it at the first [FAIL] line.

A mutation is "detected" as soon as one assertion fails, so there is no reason to let the remaining
(slow, live) tests run.

Exit codes:
  0  the program passed                                        -> the mutation was MISSED
  1  an assertion failed, or the program crashed / returned an error -> the mutation is DETECTED
  2  the runner could not run the program (missing executable...)    -> harness problem
  3  the program passed but SKIPPED tests ([SKIP])                   -> inconclusive: a skipped test
     cannot detect anything, so it must never be read as "missed" or "detected"

The whole process tree is killed on a failure: the test programs start restic.exe, and an orphan that keeps
writing into the live fixture would corrupt the next mutation.
Prints the first failure line (or the last output line) so the harness can show it.
"""
import os
import subprocess
import sys
import threading

exe = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 900


def kill_tree(process):
    """Kills the process and everything it started (taskkill /T), then the process itself."""
    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=False)
    try:
        process.kill()
    except OSError:
        pass


if not os.path.isfile(exe):
    print("[runner] executable not found: %s" % exe)
    sys.exit(2)

try:
    proc = subprocess.Popen([exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
except OSError as exc:
    print("[runner] cannot start %s: %s" % (exe, exc))
    sys.exit(2)

first_fail = None
first_skip = None
last = ""
timer = threading.Timer(limit, kill_tree, args=(proc,))
timer.start()
try:
    for line in proc.stdout:
        line = line.rstrip()
        if line:
            last = line
        if line.startswith("[SKIP]") and first_skip is None:
            first_skip = line
        if line.startswith("[FAIL]"):
            first_fail = line
            kill_tree(proc)
            break
    proc.wait()
except Exception as exc:  # pragma: no cover - harness helper
    print("[runner] %s" % exc)
    kill_tree(proc)
    sys.exit(2)
finally:
    timer.cancel()

if first_fail is not None:
    print(first_fail)
    sys.exit(1)
if proc.returncode != 0:
    print("[exit %d] %s" % (proc.returncode, last))
    sys.exit(1)
if first_skip is not None:
    print(first_skip)
    sys.exit(3)
sys.exit(0)
