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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#c,errno);return 1;} } while(0)
int main(int argc,char **argv){
 CHECK(argc==3 || (argc==4 && (!strcmp(argv[2],"bwrap-symlink") || !strcmp(argv[2],"bwrap-root-alias") || !strcmp(argv[2],"bwrap-app-dest") || !strcmp(argv[2],"bwrap-root-alias-passed"))));
 if(!strcmp(argv[2],"bwrap-root-alias-passed")){
  struct stat st;
  CHECK(mount("/usr","/newroot/usr",NULL,MS_BIND,NULL)==0);
  CHECK(mkdir("/newroot/run",0755)==0 || errno==EEXIST);
  CHECK(mkdir("/newroot/run/parent",0755)==0 || errno==EEXIST);
  CHECK(mount("/usr","/newroot/run/parent/usr",NULL,MS_BIND,NULL)==0);
  int libfd=open("/newroot/run/parent/usr/lib",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(libfd>=0);
  /* pressure-vessel hands bwrap a directory descriptor over SCM_RIGHTS. The
   * receiver's proxy never recorded a logical name for it, so path
   * translation must fall back to the physical fd text — and the shadow must
   * still cover the alias bwrap actually checks. */
  int sv[2];CHECK(socketpair(AF_UNIX,SOCK_DGRAM,0,sv)==0);
  struct msghdr send={0};char byte='x';char control[CMSG_SPACE(sizeof(int))];
  send.msg_control=control;send.msg_controllen=sizeof(control);
  struct cmsghdr *c=CMSG_FIRSTHDR(&send);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&libfd,sizeof(libfd));
  send.msg_iov=(struct iovec[]){{&byte,1}};send.msg_iovlen=1;
  CHECK(sendmsg(sv[0],&send,0)==1);
  struct msghdr recvmsg_h={0};char rbyte=0,rc[CMSG_SPACE(sizeof(int))];
  recvmsg_h.msg_control=rc;recvmsg_h.msg_controllen=sizeof(rc);
  recvmsg_h.msg_iov=(struct iovec[]){{&rbyte,1}};recvmsg_h.msg_iovlen=1;
  CHECK(recvmsg(sv[1],&recvmsg_h,0)==1);close(sv[0]);close(sv[1]);
  int passed=-1;
  for(struct cmsghdr *r=CMSG_FIRSTHDR(&recvmsg_h);r;r=CMSG_NXTHDR(&recvmsg_h,r))
   if(r->cmsg_level==SOL_SOCKET && r->cmsg_type==SCM_RIGHTS){memcpy(&passed,CMSG_DATA(r),sizeof(passed));}
  CHECK(passed>=0);close(libfd);
  CHECK(mkdirat(passed,"i386-linux-gnu",0755)==0);
  /* The sibling alias (/newroot/usr) points at the same image; bwrap checks
   * the destination through whichever spelling it used. Both must be
   * directories now, not EROFS symlinks. */
  CHECK(fstatat(passed,"i386-linux-gnu",&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISDIR(st.st_mode));
  CHECK(stat("/newroot/usr/lib/i386-linux-gnu",&st)==0 && S_ISDIR(st.st_mode));
  int destfd=openat(passed,"i386-linux-gnu",O_PATH|O_NOFOLLOW|O_CLOEXEC);CHECK(destfd>=0);
  CHECK(fstat(destfd,&st)==0 && S_ISDIR(st.st_mode));close(destfd);
  CHECK(mount(argv[3],"/newroot/run/parent/usr/lib/i386-linux-gnu",NULL,MS_BIND,NULL)==0);
  CHECK(stat("/newroot/run/parent/usr/lib/i386-linux-gnu/marker",&st)==0 && S_ISREG(st.st_mode));
  close(passed);puts("PASS passed-fd bind target shadows EROFS symlink across aliases");return 0;
 }
 if(!strcmp(argv[2],"bwrap-app-dest")){
  int root=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(root>=0);
  CHECK(mkdirat(root,"app/lib/i386-linux-gnu/GL",0755)==0);
  int file=openat(root,"app/lib/i386-linux-gnu/GL32",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);CHECK(file>=0);
  CHECK(write(file,"app-bind-target",15)==15);close(file);
  struct stat st;CHECK(fstatat(root,"app/lib/i386-linux-gnu/GL32",&st,0)==0 && S_ISREG(st.st_mode) && st.st_size==15);
  close(root);puts("PASS bwrap creates missing directory and file bind destinations in app image");return 0;
 }
 if(!strcmp(argv[2],"bwrap-root-alias")){
  struct stat st;
  CHECK(mount("/usr","/newroot/usr",NULL,MS_BIND,NULL)==0);
  CHECK(mkdir("/newroot/run",0755)==0 || errno==EEXIST);
  CHECK(mkdir("/newroot/run/parent",0755)==0 || errno==EEXIST);
  CHECK(mount("/usr","/newroot/run/parent/usr",NULL,MS_BIND,NULL)==0);
  int libfd=open("/newroot/run/parent/usr/lib",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(libfd>=0);
  CHECK(mkdirat(libfd,"i386-linux-gnu",0755)==0);
  CHECK(fstatat(libfd,"i386-linux-gnu",&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISDIR(st.st_mode));
  int destfd=openat(libfd,"i386-linux-gnu",O_PATH|O_NOFOLLOW|O_CLOEXEC);CHECK(destfd>=0);
  CHECK(fstat(destfd,&st)==0 && S_ISDIR(st.st_mode));close(destfd);
  CHECK(mount(argv[3],"/newroot/run/parent/usr/lib/i386-linux-gnu",NULL,MS_BIND,NULL)==0);
  CHECK(stat("/newroot/run/parent/usr/lib/i386-linux-gnu/marker",&st)==0 && S_ISREG(st.st_mode));
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
  int file=openat(root,"usr/lib/x86_64-linux-gnu/GL/extension.conf",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);CHECK(file>=0);
  CHECK(write(file,"runtime-bind-target",19)==19);close(file);
  CHECK(fstatat(root,"usr/lib/x86_64-linux-gnu/GL/extension.conf",&st,0)==0 && S_ISREG(st.st_mode) && st.st_size==19);
 }else if(!strcmp(argv[2],"bwrap-symlink")){
  struct stat st;
  int libfd=open("/usr/lib",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(libfd>=0);
  CHECK(mkdirat(libfd,"i386-linux-gnu",0755)==0);
  CHECK(fstatat(libfd,"i386-linux-gnu",&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISDIR(st.st_mode));
  int destfd=openat(libfd,"i386-linux-gnu",O_PATH|O_NOFOLLOW|O_CLOEXEC);CHECK(destfd>=0);
  CHECK(fstat(destfd,&st)==0 && S_ISDIR(st.st_mode));close(destfd);
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
assert 'v3 trap ' in result.stderr
assert 'v3 raw-calls syscall=' not in result.stderr  # counters disabled: no zero-only log spam
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

# Steam's GL32 extension has a file and directory destination under /app.
# Exercise both types against a separate immutable app-image prefix.
app_image=root/'app-image'
(app_image/'lib/i386-linux-gnu').mkdir(parents=True)
cmd=[str(base/'build/linux-layer-v3'),'--android-filter','--erofs-mount','/app',str(app_image),
     str(bwrap),str(newroot),'bwrap-app-dest']
result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
if result.returncode:
    raise RuntimeError(result.stdout+'\n'+result.stderr)
print(result.stdout.strip())

cmd=[str(base/'build/linux-layer-v3'),'--android-filter','--erofs-mount','/usr',str(mount),
     '--root',str(root),'/bwrap',str(root),'bwrap-root-alias','/usr/lib/x86_64-linux-gnu/extension']
result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
if result.returncode:
    raise RuntimeError(result.stdout+'\n'+result.stderr)
print(result.stdout.strip())

# The same setup with the destination directory descriptor arriving over
# SCM_RIGHTS, so the receiver's proxy must recover the alias another way.
cmd=[str(base/'build/linux-layer-v3'),'--android-filter','--erofs-mount','/usr',str(mount),
     '--root',str(root),'/bwrap',str(root),'bwrap-root-alias-passed','/usr/lib/x86_64-linux-gnu/extension']
result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
if result.returncode:
    raise RuntimeError(result.stdout+'\n'+result.stderr)
print(result.stdout.strip())

# A resident reset uses the same sealed state as a mixed-ABI kernel exec.
# Keep distinct /usr and /app images: serializing the head twice loses /app's
# EROFS identity, so its readonly bind can no longer acquire bwrap targets.
trampoline_source=root/'exec-i386.c'
trampoline_source.write_text(r'''
void entry(unsigned *stack){
 char **argv=(char **)(stack+1),**env=argv+stack[0]+1;
 __asm__ volatile("int $0x80"::"a"(11),"b"(argv[1]),"c"(argv+1),"d"(env):"memory");
 __asm__ volatile("int $0x80"::"a"(1),"b"(77):"memory");
 __builtin_unreachable();
}
__asm__(".global _start\n_start:\n mov %esp,%eax\n push %eax\n call entry\n");
''')
trampoline=root/'exec-i386'
subprocess.run(['cc','-m32','-nostdlib','-static','-no-pie','-fno-pie',
                '-fno-stack-protector','-O2',str(trampoline_source),'-o',str(trampoline)],check=True)
cmd=[str(base/'build/linux-layer-v3'),'--android-filter',
     '--map-ro',str(app_image),'/app','--erofs-mount','/app',str(app_image),
     '--erofs-mount','/usr',str(mount),str(trampoline),str(bwrap),str(newroot),'bwrap-app-dest']
result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
if result.returncode:
    raise RuntimeError(result.stdout+'\n'+result.stderr)
print('PASS separate app/runtime EROFS identities and bwrap bind destinations survive two kernel ABI handoffs')
