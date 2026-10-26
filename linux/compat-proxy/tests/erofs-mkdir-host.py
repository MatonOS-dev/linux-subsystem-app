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
 CHECK(argc==3 || (argc==4 && (!strcmp(argv[2],"bwrap-symlink") || !strcmp(argv[2],"bwrap-root-alias"))));
 if(!strcmp(argv[2],"bwrap-root-alias")){
  struct stat st;
  CHECK(mount("/usr","/newroot/usr",NULL,MS_BIND,NULL)==0);
  int libfd=open("/newroot/usr/lib",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(libfd>=0);
  CHECK(mkdirat(libfd,"i386-linux-gnu",0755)==0);
  CHECK(fstatat(libfd,"i386-linux-gnu",&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISDIR(st.st_mode));
  CHECK(mount(argv[3],"/newroot/usr/lib/i386-linux-gnu",NULL,MS_BIND,NULL)==0);
  CHECK(stat("/newroot/usr/lib/i386-linux-gnu/marker",&st)==0 && S_ISREG(st.st_mode));
  close(libfd);puts("PASS bwrap staged-root bind target shadows EROFS symlink");return 0;
 }
 char oldroot[4096];CHECK(snprintf(oldroot,sizeof(oldroot),"%s/oldroot",argv[1])>0);
 if(!strcmp(argv[2],"bwrap-symlink")){
  char target[4096];CHECK(snprintf(target,sizeof(target),"%s/usr",argv[1])>0);
  CHECK(mount("/usr",target,NULL,MS_BIND,NULL)==0);
 }
 CHECK(syscall(SYS_pivot_root,argv[1],oldroot)==0);
 int root=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(root>=0);
 /* bwrap's mkdir-parents probes each component before binding extensions. */
 CHECK(mkdirat(root,"usr/lib/x86_64-linux-gnu/GL",0755)==-1 && errno==EEXIST);
 CHECK(mkdirat(root,"usr/lib/x86_64-linux-gnu/GL/default",0755)==-1 && errno==EEXIST);
 if(!strcmp(argv[2],"bwrap")){
  struct stat st;
  CHECK(mkdirat(root,"usr/lib/x86_64-linux-gnu/GL/extension/",0755)==0);
  CHECK(stat("/usr/lib/x86_64-linux-gnu/GL/extension",&st)==0 && S_ISDIR(st.st_mode));
 }else if(!strcmp(argv[2],"bwrap-symlink")){
  struct stat st;
  int libfd=open("/usr/lib",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(libfd>=0);
  CHECK(mkdirat(libfd,"i386-linux-gnu",0755)==0);
  CHECK(fstatat(libfd,"i386-linux-gnu",&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISDIR(st.st_mode));
  CHECK(stat("/usr/lib/i386-linux-gnu",&st)==0 && S_ISDIR(st.st_mode));
  /* Flatpak then binds the extension onto the directory bwrap created. The
   * virtual bind must shadow the original EROFS symlink too. */
  CHECK(mount(argv[3],"/usr/lib/i386-linux-gnu",NULL,MS_BIND,NULL)==0);
  CHECK(stat("/usr/lib/i386-linux-gnu/marker",&st)==0 && S_ISREG(st.st_mode));
  CHECK(lstat("/usr/lib/i386-linux-gnu",&st)==0 && S_ISDIR(st.st_mode));
  close(libfd);
 }else{
  CHECK(mkdirat(root,"usr/lib/x86_64-linux-gnu/GL/new-payload-dir",0755)==-1 && errno==EROFS);
  CHECK(mkdirat(root,"usr/lib/x86_64-linux-gnu/GL/new-payload-dir/",0755)==-1 && errno==EROFS);
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
shutil.copyfile(probe,bwrap)
bwrap.chmod(0o755)
cmd=[str(base/'build/linux-layer-v3'),'--android-filter','--erofs-mount','/usr',str(mount),str(bwrap),str(newroot),'bwrap']
result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
if result.returncode:
    raise RuntimeError(result.stdout+'\n'+result.stderr)
print(result.stdout.strip())

# Platform runtimes may expose the Compat.i386 extension point as a symlink.
# bwrap must be able to create its bind destination as a directory in its
# virtual namespace, shadowing the immutable EROFS symlink for this launch.
compat=(mount/'lib/i386-linux-gnu')
compat.parent.mkdir(parents=True,exist_ok=True)
(mount/'lib/x86_64-linux-gnu').mkdir(parents=True,exist_ok=True)
compat.symlink_to('x86_64-linux-gnu')
(newroot/'usr').mkdir(exist_ok=True)
extension=mount/'lib/x86_64-linux-gnu/extension'
extension.mkdir(parents=True,exist_ok=True)
(extension/'marker').touch()
cmd=[str(base/'build/linux-layer-v3'),'--android-filter','--erofs-mount','/usr',str(mount),
     str(bwrap),str(newroot),'bwrap-symlink','/oldroot'+str(extension)]
result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
if result.returncode:
    raise RuntimeError(result.stdout+'\n'+result.stderr)
print('PASS direct-root extension bind target shadows EROFS symlink')
cmd=[str(base/'build/linux-layer-v3'),'--android-filter','--erofs-mount','/usr',str(mount),
     '--root',str(root),'/bwrap',str(root),'bwrap-root-alias','/usr/lib/x86_64-linux-gnu/extension']
result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
if result.returncode:
    raise RuntimeError(result.stdout+'\n'+result.stderr)
print(result.stdout.strip())
