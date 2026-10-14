#!/usr/bin/env python3
"""PTY shell startup, fault-safe termios2 and diagnostics after stdio replacement."""
import pathlib, subprocess
root=pathlib.Path(__file__).resolve().parents[1]
subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(root/'tests/pty.c'),'-o',str(root/'build/pty')],check=True)
p=subprocess.run([str(root/'build/linux-layer-v3'),'--android-filter','--trace',str(root/'build/pty')],capture_output=True,timeout=20)
(root/'build/pty.log').write_bytes(p.stderr)
assert p.returncode==0,(p.returncode,p.stderr[-4000:])
assert b'v3 exec /bin/sh' in p.stderr,p.stderr[-4000:]
print(p.stdout.decode(),end='')
print('PASS original diagnostic sink survives close/dup2/fcntl/close_range and shell exec')
