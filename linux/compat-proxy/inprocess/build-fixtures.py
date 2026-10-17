#!/usr/bin/env python3
"""Pinned, test-only Linux ABI fixtures, never used by production launches."""
import hashlib
import pathlib
import shutil
import subprocess
import sys
root=pathlib.Path(__file__).resolve().parent
out=pathlib.Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
# This stage-one diagnostic deliberately pins the build host's Ubuntu glibc
# 2.39-0ubuntu8.9 pair. Refuse drift; this is not the runtime used by Flatpaks.
for source,name,digest in [
    ('/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2','libmatonos-inprocess-ld.so','c20a2dc8917c755f02b94049356320fe1f62ac7d9f8994731f807d9df39302da'),
    ('/lib/x86_64-linux-gnu/libc.so.6','libt.so','3a15d66867d83762c7f2f1e37359cb8f6c5743edb369c65285cb0b1c4f7498bf')]:
    assert hashlib.sha256(pathlib.Path(source).read_bytes()).hexdigest()==digest, 'test glibc pin changed: '+source
    shutil.copyfile(source,out/name)
for mode,flags in [('static',['-static-pie']),('glibc',[])]:
    target=out/('libmatonos-inprocess-'+mode+'.so')
    subprocess.run(['cc','-O2','-pthread',*flags,str(root/'payload.c'),'-o',str(target)],check=True)
    if mode=='glibc':
        data=target.read_bytes()
        assert data.count(b'libc.so.6\0')==1
        # Retain ELF offsets; the APK extracts only lib*.so names. SONAME is
        # unchanged in libc. Only this fixture's DT_NEEDED spelling changes.
        target.write_bytes(data.replace(b'libc.so.6\0',b'libt.so\0\0\0'))

for target in out.glob('*.so'):target.chmod(0o755)
