"""Check trace bounds before connect, including denied sandbox connects."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
subprocess.run(['cc','-Wall','-Wextra','-Werror',str(root/'tests/unix-trace.c'),'-o',str(root/'build/unix-trace')],check=True)
p=subprocess.run([str(root/'build/linux-layer-v3'),'--android-filter','--trace',str(root/'build/unix-trace')],capture_output=True,timeout=15)
assert p.returncode==0,p.stderr
lines=[x for x in p.stderr.splitlines() if x.startswith(b'v3 unix ')]
assert lines==[b'v3 unix /tmp/maton-wsi-trace',b'v3 unix '+b''.join(b'\\x%02x'%x for x in [0,119,115,105,0,255])],lines
print('PASS pathname trace stops at NUL; abstract trace preserves addrlen bytes as escapes')
