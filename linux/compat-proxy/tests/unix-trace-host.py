"""Check AF_UNIX address traces and syscall byte/ancillary diagnostics."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
subprocess.run(['cc','-Wall','-Wextra','-Werror',str(root/'tests/unix-trace.c'),'-o',str(root/'build/unix-trace')],check=True)
p=subprocess.run([str(root/'build/linux-layer-v3'),'--android-filter','--trace',str(root/'build/unix-trace')],capture_output=True,timeout=15)
assert p.returncode==0,p.stderr
lines=[x for x in p.stderr.splitlines() if x.startswith(b'v3 unix ')]
assert lines==[b'v3 unix /tmp/maton-wsi-trace',b'v3 unix '+b''.join(b'\\x%02x'%x for x in [0,119,115,105,0,255])],lines
io=[x for x in p.stderr.splitlines() if x.startswith(b'v3 unix-io ')]
ops=[x.split(b' op=',1)[1].split(b' ',1)[0] for x in io]
assert ops==[b'write',b'read',b'sendto',b'recvfrom',b'sendmsg',b'recvmsg'],io
for line in io:
    assert b' fd=' in line and b' type=1 requested=' in line and b' returned=' in line,line
    assert b' msg_flags=' in line and b' pid=' in line,line
assert any(b' cmsgs=1:1 rights=1 cred_pid=-' in x for x in io),io
assert any(b' cmsgs=1:2' in x and b' rights=1 cred_pid=' in x for x in io),io
print('PASS AF_UNIX path and byte/ancillary traces cover read/write, sendto/recvfrom and sendmsg/recvmsg')
