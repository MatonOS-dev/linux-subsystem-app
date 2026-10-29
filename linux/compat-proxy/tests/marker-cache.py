#!/usr/bin/env python3
"""Copy/link siblings without re-probing their unchanged marker ancestry."""
import pathlib
import subprocess
import sys
import tempfile

base = pathlib.Path(__file__).resolve().parents[1]
loader = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else base/'build/linux-layer-v3'
with tempfile.TemporaryDirectory(prefix='steam-r45-marker-', dir='/mnt/data/aosp/out/pc-logs') as scratch:
    root = pathlib.Path(scratch)
    (root / '.v3-store-root').touch()
    source = root / 'copy.c'
    source.write_text(r'''#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc != 2 || chdir(argv[1])) return 1;
    for (int i=0; i<128; i++) {
        char a[40], b[40];
        snprintf(a,sizeof(a),"file-%d",i);
        snprintf(b,sizeof(b),"link-%d",i);
        int fd=open(a,O_CREAT|O_EXCL|O_RDWR,0600);
        if(fd<0 || write(fd,"x",1)!=1 || close(fd) || link(a,b)) return 2;
        struct stat st;
        if(stat(b,&st) || st.st_nlink!=2 || unlink(a) || unlink(b)) return 3;
    }
    puts("PASS sibling create/link/stat/unlink preserves marker cache");
    return 0;
}''')
    subprocess.run(['cc', '-static', '-O2', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(root/'copy')], check=True)
    log = root/'strace'
    subprocess.run(['strace', '-qq', '-e', 'trace=openat', '-o', str(log), str(loader), '--android-filter', str(root/'copy'), str(root)], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    probes = sum('.v3-store-root"' in line for line in log.read_text().splitlines())
    # One ancestry walk on first access; unrelated sibling files cannot
    # invalidate it. This is a syscall-count assertion, not a timing limit.
    assert probes < 128, f'{probes} marker probes for 128 siblings'
    print(f'PASS marker ancestry reused across 128 sibling copies: {probes} probes')
