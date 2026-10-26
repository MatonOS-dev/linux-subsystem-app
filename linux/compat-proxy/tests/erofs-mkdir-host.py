#!/usr/bin/env python3
"""Reproduce bwrap mkdir-parents after pivot_root with a mapped /usr image."""
import pathlib
import shutil
import subprocess

base=pathlib.Path(__file__).resolve().parents[1]
root=base/'build/erofs-mkdir'
shutil.rmtree(root,ignore_errors=True)
mount=root/'image'
target=mount/'lib/x86_64-linux-gnu/GL'
target.mkdir(parents=True)
(target/'default').mkdir()
newroot=root/'newroot'
(newroot/'oldroot').mkdir(parents=True)
source=root/'probe.c'
source.write_text(r'''#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#c,errno);return 1;} } while(0)
int main(int argc,char **argv){
 CHECK(argc==3);
 char oldroot[4096];CHECK(snprintf(oldroot,sizeof(oldroot),"%s/oldroot",argv[1])>0);
 CHECK(syscall(SYS_pivot_root,argv[1],oldroot)==0);
 int root=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(root>=0);
 /* bwrap's mkdir-parents probes each component before binding extensions. */
 CHECK(mkdirat(root,"usr/lib/x86_64-linux-gnu/GL",0755)==-1 && errno==EEXIST);
 CHECK(mkdirat(root,"usr/lib/x86_64-linux-gnu/GL/default",0755)==-1 && errno==EEXIST);
 if(!strcmp(argv[2],"bwrap")){
  struct stat st;
  CHECK(mkdirat(root,"usr/lib/x86_64-linux-gnu/GL/extension",0755)==0);
  CHECK(stat("/usr/lib/x86_64-linux-gnu/GL/extension",&st)==0 && S_ISDIR(st.st_mode));
 }else{
  CHECK(mkdirat(root,"usr/lib/x86_64-linux-gnu/GL/new-payload-dir",0755)==-1 && errno==EROFS);
 }
 close(root);
 puts("PASS pivoted EROFS mkdir-parents: existing=EEXIST, new=EROFS, bwrap bind target=virtual directory");
 return 0;
}''')
probe=root/'probe'
subprocess.run(['cc','-static','-O2','-Wall','-Wextra','-Werror',str(source),'-o',str(probe)],check=True)
cmd=[str(base/'build/linux-layer-v3'),'--android-filter','--trace','--erofs-mount','/usr',str(mount),str(probe),str(newroot),'payload']
result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
if result.returncode:
    raise RuntimeError(result.stdout+'\n'+result.stderr)
print(result.stdout.strip())
bwrap=root/'bwrap'
bwrap.symlink_to(probe)
cmd=[str(base/'build/linux-layer-v3'),'--android-filter','--erofs-mount','/usr',str(mount),str(bwrap),str(newroot),'bwrap']
result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
if result.returncode:
    raise RuntimeError(result.stdout+'\n'+result.stderr)
print(result.stdout.strip())
