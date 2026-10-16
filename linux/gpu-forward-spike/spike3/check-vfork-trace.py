#!/usr/bin/env python3
"""Check strace -ff PREFIX.PID logs from adopt-fork-test before first exec."""
import glob
import re
import sys

files = {int(p.rsplit('.', 1)[1]): p for p in glob.glob(sys.argv[1] + '.*')}
children = []
for path in files.values():
    with open(path) as source:
        for line in source:
            if line.startswith('vfork(') or (
                line.startswith(('clone(', 'clone3(')) and 'CLONE_VFORK' in line
            ):
                match = re.search(r'= (\d+)\s*$', line)
                if match:
                    children.append(int(match[1]))
for child in children:
    with open(files[child]) as source:
        before, separator, _ = source.read().partition('execve(')
    assert separator, f'child {child} never attempted execve'
    assert not re.search(
        r'^(arch_prctl|prctl|set_tid_address|set_robust_list|rseq|clone|clone3|fork|gettid)\(',
        before, re.M,
    ), before
    assert 'SIGSYS' not in before, before
assert len(children) == 4, children
print('PASS: four spawn/vfork children before execve: no SIGSYS, FS switch, '
      'registry lookup, or resident thread setup')
