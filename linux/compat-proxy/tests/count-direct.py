#!/usr/bin/env python3
"""Rewritten syscalls must be measured separately from trapped syscalls."""
import os, pathlib, re, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='maton-count-direct-') as tmp:
    tmp=pathlib.Path(tmp)
    source=tmp/'calls.c'
    source.write_text('''#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
extern long rw_open(long,const char *,long,long);
int main(void){for(int i=0;i<100;i++){long fd=rw_open(AT_FDCWD,"/etc/hosts",O_RDONLY,0);assert(fd>=0);close(fd);}return 0;}
''')
    binary=tmp/'calls'
    subprocess.run(['cc','-O2',str(source),str(root/'tests/site-rewrite.S'),'-o',str(binary)],check=True)
    for rewrite in ('0','1'):
        counts=tmp/('counts'+rewrite)
        env=dict(os.environ,MATON_PROXY_REWRITE=rewrite)
        subprocess.run([str(root/'build/linux-layer-v3'),'--android-filter','--count',str(counts),str(binary)],env=env,check=True,timeout=15)
        records=re.findall(r'^DOP (\d+) (\d+) (\d+) (\d+)$',counts.read_text(),re.M)
        if rewrite=='0': assert not records,records
        else:
            # Canonical openat ID, not x86_64 syscall 257.
            opens=[tuple(map(int,r)) for r in records if int(r[1])==212]
            assert sum(r[2] for r in opens)>=99,opens
            assert sum(r[3] for r in opens)>0,opens
print('PASS counter-only rewritten direct calls measured; disabled rewrite has no direct records')
