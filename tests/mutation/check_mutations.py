"""Static sanity check of the mutation scripts (no build, no test run).

Every script must find its pattern exactly once in its source file, and every id of mutations.txt
must have a script (and the other way round). A pattern that matches zero times would make the
harness report a mutation as "detected" without ever having changed the code.

Usage (from anywhere):  python tests/mutation/check_mutations.py
"""
import glob
import io
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
os.chdir(ROOT)

recorded = {}


def fake_run(src, old, new, mode):
    recorded['args'] = (src, old, new)


fake = types.ModuleType('mut')
fake.run = fake_run
sys.modules['mut'] = fake

bad = []
scripts = sorted(glob.glob(os.path.join(HERE, 'mut_m*.py')))
for script in scripts:
    recorded.clear()
    sys.argv = [script, 'apply']
    exec(compile(io.open(script, encoding='utf-8').read(), script, 'exec'),
         {'__file__': script, '__name__': '__main__'})
    source, old, new = recorded['args']
    text = io.open(source, encoding='utf-8', newline='').read()
    if '\r\n' in text:
        old = old.replace('\r\n', '\n').replace('\n', '\r\n')
    if text.count(old) != 1:
        bad.append((os.path.basename(script), text.count(old)))
    if old == new:
        bad.append((os.path.basename(script), 'replacement equals the original'))

listed = set()
for line in io.open(os.path.join(HERE, 'mutations.txt'), encoding='utf-8'):
    line = line.strip()
    if line and not line.startswith('#'):
        listed.add(line.split('|')[2])
on_disk = {os.path.basename(s) for s in scripts}
for missing in sorted(listed - on_disk):
    bad.append((missing, 'listed in mutations.txt but the script is missing'))
for orphan in sorted(on_disk - listed):
    bad.append((orphan, 'script is not listed in mutations.txt'))

print('scripts checked:', len(scripts))
print('problems:', bad)
sys.exit(1 if bad else 0)
