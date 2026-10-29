#!/usr/bin/env python3
"""Relative tree copies: ordinary ancestors need no O_PATH/readlink probes."""
import os, pathlib, subprocess, tempfile
base=pathlib.Path(__file__).resolve().parents[1]
loader=os.environ.get('COMPAT_PROXY',str(base/'build/linux-layer-v3'))
with tempfile.TemporaryDirectory(prefix='steam-r46-walk-',dir='/mnt/data/aosp/out/pc-logs') as scratch:
    root=pathlib.Path(scratch)
    deep=root/'a/b/c/d/e/f/g/h'
    deep.mkdir(parents=True)
    (deep/'file').write_text('x')
    (root/'cross').symlink_to('/other-view')
    src=root/'probe.c'
    src.write_text(r'''#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void){
 assert(!chdir("/walk"));
 for(int i=0;i<32;i++){
  /* Relative AT_FDCWD deliberately exercises the component walker. */
  int fd=open("a/b/c/d/e/f/g/h/file",O_RDONLY);assert(fd>=0);close(fd);
 }
 int fd=open("cross/file",O_RDONLY);assert(fd>=0);close(fd);
 assert(open("a/b/c/d/e/f/g/h/file/child",O_RDONLY)<0 && errno==ENOTDIR);
 assert(open("missing/child",O_RDONLY)<0 && errno==ENOENT);
 puts("PASS relative component walk, cross-mount symlink, ENOTDIR and ENOENT");
}''')
    subprocess.run(['cc','-static','-O2','-Wall','-Wextra','-Werror',str(src),'-o',str(root/'probe')],check=True)
    trace=root/'trace'
    cmd=['strace','-qq','-e','trace=openat,readlinkat,newfstatat','-o',str(trace),loader,'--android-filter','--map',str(root),'/walk','--map',str(deep),'/other-view',str(root/'probe')]
    result=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
    assert result.returncode==0,result.stderr
    # O_PATH probes for every ancestor were the expensive fallback. They
    # should occur only for symlinks or denied getattr, never ordinary dirs.
    ancestor=str(root/'a/b/c/d')+'"'
    probes=sum(ancestor in l and l.startswith('openat(') and 'O_PATH' in l for l in trace.read_text().splitlines())
    assert probes==0,f'{probes} redundant O_PATH probes for ordinary ancestor'
    print(result.stdout.strip())
