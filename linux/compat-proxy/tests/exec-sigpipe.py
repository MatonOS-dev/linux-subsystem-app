#!/usr/bin/env python3
"""Payload SIGPIPE inheritance and closed proxy diagnostic pipe isolation."""
import pathlib
import subprocess
root = pathlib.Path(__file__).resolve().parents[1]
exe = root / 'build/exec-sigpipe'
subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(root / 'tests/exec-sigpipe.c'), '-o', str(exe)], check=True)
for prefix in ([], [str(root / 'build/linux-layer-v3'), '--android-filter'],
               [str(root / 'build/linux-layer-v3'), '--android-filter', '--trace']):
    subprocess.run(prefix + [str(exe)], check=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    for mode in ('closed-log', 'closed-log-pending'):
        p = subprocess.Popen(prefix + [str(exe), mode], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        # Drain startup output without filling the log pipe, then close its ONLY
        # reader while the payload is blocked on stdin. The reserved log dup now
        # points to a broken pipe; payload SIGPIPE is deliberately default.
        import os
        import select
        while True:
            ready, _, _ = select.select([p.stdout, p.stderr], [], [], 10)
            assert ready, 'payload did not reach ready'
            if p.stdout in ready: break
            if p.stderr in ready: os.read(p.stderr.fileno(), 65536)
        assert p.stdout.readline() == b'ready\n'
        p.stderr.close()
        p.stdin.write(b'x'); p.stdin.flush(); p.stdin.close()
        assert p.wait(timeout=10) == 0, (prefix, p.returncode)
print('PASS closed diagnostic pipe preserves payload default SIGPIPE and liveness')

# The default-action child still dies by SIGPIPE, with bounded provenance
# records. The first argument is the signal, then si_code; sender/op follow.
p = subprocess.run([str(root / 'build/linux-layer-v3'), '--android-filter', '--trace', str(exe)],
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
assert b'fatal-signal-code 13 ' in p.stderr, p.stderr[-2000:]
assert b'fatal-signal-sender ' in p.stderr, p.stderr[-2000:]
print('PASS default SIGPIPE provenance retains waitpid signal status')

p = subprocess.run([str(root / 'build/linux-layer-v3'), '--android-filter', '--trace',
                    str(exe), 'sent-signal'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
assert p.returncode == -13, p.returncode
assert b'fatal-signal-code 13 -6' in p.stderr, p.stderr[-2000:]
assert b'fatal-signal-sender ' in p.stderr, p.stderr[-2000:]
print('PASS explicit SIGPIPE sender distinguished from broken-pipe origin')
