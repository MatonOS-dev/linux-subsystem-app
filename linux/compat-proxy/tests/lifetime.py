#!/usr/bin/env python3
"""Real loader child dies with its stub-shaped parent; also reject stale PID."""
import ctypes
import os
import signal
import subprocess
import sys
import time

loader = os.path.abspath(sys.argv[1])
assert subprocess.run([loader, '--parent-pid', '2147483647', '/bin/true']).returncode == 2
# Adopt the grandchild so the test can reap it and inspect the actual signal.
assert ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) == 0
read, write = os.pipe()
parent = os.fork()
if parent == 0:
    os.close(read)
    child = subprocess.Popen([loader, '--parent-pid', str(os.getpid()),
                              '--android-filter', '/bin/sleep', '60'],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.write(write, str(child.pid).encode())
    os.close(write)
    child.wait()
    os._exit(1)
os.close(write)
child = int(os.read(read, 32))
os.close(read)
try:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            if open(f'/proc/{child}/wchan').read().strip() == 'hrtimer_nanosleep':
                break
        except FileNotFoundError:
            raise AssertionError('loader exited before running the sleep payload')
        time.sleep(.02)
    else:
        raise AssertionError('loader did not reach sleep payload')
    os.kill(parent, signal.SIGKILL)
    os.waitpid(parent, 0)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        pid, status = os.waitpid(child, os.WNOHANG)
        if pid:
            assert os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGKILL, status
            print('PASS NDK loader parent-death SIGKILL and stale parent rejection')
            break
        time.sleep(.02)
    else:
        raise AssertionError('loader survived parent death')
finally:
    for pid in (parent, child):
        try: os.kill(pid, signal.SIGKILL)
        except ProcessLookupError: pass
    for pid in (parent, child):
        try: os.waitpid(pid, 0)
        except ChildProcessError: pass
