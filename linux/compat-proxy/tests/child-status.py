"""Child diagnostics retain kernel status without consuming waitpid status."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(root/'tests/child-status.c'),'-o',str(root/'build/child-status')],check=True)
for traced in (False,True):
    p=subprocess.run([str(root/'build/linux-layer-v3'),'--android-filter']+(['--trace'] if traced else [])+[str(root/'build/child-status')],capture_output=True,timeout=15)
    assert p.returncode==0,(p.returncode,p.stderr)
    for row in p.stdout.decode().splitlines():
        fields=dict(item.split('=') for item in row.split())
        for kind in ('code','status'):
            record=f"v3 trap chld-{kind} {fields['child']} {fields[kind]} pid=".encode()
            assert (record in p.stderr)==traced,(record,p.stderr)
print('PASS child exit/signal diagnostics preserve notification and wait status, traced and untraced')
