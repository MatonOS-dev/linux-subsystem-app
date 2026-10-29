#!/usr/bin/env python3
"""Counter sink survives kernel peer exec; small handler stack remains usable."""
import pathlib, subprocess, tempfile, re
root=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='maton-count-') as tmp:
    tmp=pathlib.Path(tmp)
    source=tmp/'exec.c'
    source.write_text('''#include <unistd.h>
int main(int argc,char **argv){if(argc>1){char *a[]={argv[1],0};execv(argv[1],a);return 2;}char b[8];return readlink("/proc/self/exe",b,sizeof b)<0;}
''')
    subprocess.run(['cc',str(source),'-o',str(tmp/'64')],check=True)
    asm=tmp/'32.S';asm.write_text('.global _start\n_start: mov $20,%eax; int $0x80; xor %ebx,%ebx; mov $1,%eax; int $0x80; ud2\n')
    subprocess.run(['as','--32',str(asm),'-o',str(tmp/'32.o')],check=True)
    subprocess.run(['ld','-m','elf_i386',str(tmp/'32.o'),'-o',str(tmp/'32')],check=True)
    counts=tmp/'counts'
    subprocess.run([str(root/'build/linux-layer-v3'),'--android-filter','--count',str(counts),str(tmp/'64'),str(tmp/'32')],check=True,timeout=15)
    text=counts.read_text()
    headers=re.findall(r'^PID=(\d+) T=(\d+) H=(\d+) V=(\d+)$',text,re.M)
    assert len(headers)>=2,(headers,text)
    assert len({h[0] for h in headers})==1,headers
    assert all(int(h[1])>0 for h in headers),headers
    assert len(re.findall(r'^OP ',text,re.M))>5,text
print('PASS counter-only x86_64 to i386 kernel exec, PID/timestamp and per-op records')

subprocess.run(["python3",str(root/"tests/count-direct.py")],check=True)
