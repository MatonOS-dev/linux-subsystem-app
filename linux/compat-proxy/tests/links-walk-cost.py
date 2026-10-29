#!/usr/bin/env python3
"""Directory ancestry must not replay the hard-link journal per component."""
import os
import pathlib
import subprocess
import tempfile

base = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='steam-links-walk-', dir='/mnt/data/aosp/out/pc-logs') as tmp:
    root = pathlib.Path(tmp)
    (root/'.v3-store-root').touch()
    deep = root/'a/b/c/d/e/f/g/h'
    deep.mkdir(parents=True)
    src = root/'probe.c'
    src.write_text(r'''#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc,char **argv){
 assert(argc==2 && !chdir(argv[1]));
 for(int i=0;i<64;i++){
  struct stat st;assert(!stat("a/b/c/d/e/f/g/h",&st));
  assert(S_ISDIR(st.st_mode));
 }
 return 0;
}''')
    subprocess.run(['cc','-static','-O2','-Wall','-Wextra','-Werror',str(src),'-o',str(root/'probe')],check=True)
    # An existing store exercises the formerly expensive recover() path.
    (root/'.v3-inodes').mkdir(mode=0o700)
    trace=root/'trace'
    p=subprocess.run(['strace','-qq','-e','trace=flock','-o',str(trace),os.environ.get('COMPAT_PROXY',str(base/'build/linux-layer-v3')),'--android-filter',str(root/'probe'),str(root)],capture_output=True,text=True,timeout=30)
    assert p.returncode==0,p.stderr
    locks=sum('LOCK_EX' in line for line in trace.read_text().splitlines())
    # At most the final metadata call needs a lock. Allow 32 startup
    # operations (loader /proc probes and executable lookup). No ancestor does.
    assert locks<=96, f'{locks} store locks for 64 directory stats'
    print(f'PASS directory walks: {locks} locks for 64 eight-component stats')
