#!/usr/bin/env python3
"""Terminal queries on memfd/pipe/socket/dir avoid Android's EACCES ioctl path."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc,char **argv){
 if(argc>1){
  struct sock_filter f[]={
   BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
   BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,__NR_ioctl,0,4),
   BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[1])),
   BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,TCGETS,1,0),
   BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,TIOCGPGRP,0,1),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EACCES),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
  struct sock_fprog p={sizeof(f)/sizeof(f[0]),f};
  assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&p));
  execl(argv[1],argv[1],"--android-filter",argv[0],NULL);return 2;
 }
 int pipes[2],sockets[2];assert(!pipe(pipes));assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
 int fds[]={memfd_create("v3-mountinfo",0),open("/tmp",O_RDONLY|O_DIRECTORY),pipes[0],sockets[0]};
 for(unsigned i=0;i<sizeof(fds)/sizeof(fds[0]);i++){
  assert(fds[i]>=0);
  for(unsigned j=0;j<3;j++){
   unsigned long cmd=j==0?TCGETS:j==1?TIOCGPGRP:TIOCGWINSZ;
   errno=0;assert(ioctl(fds[i],cmd,(void *)1)==-1&&errno==ENOTTY);
  }
 }
 int device=open("/dev/null",O_RDONLY);assert(device>=0);
 errno=0;assert(ioctl(device,TCGETS,(void *)1)==-1&&errno==EACCES);
 const char message[]="PASS non-terminal queries avoid denied ioctl; character devices retain kernel policy\n";
 assert(write(1,message,sizeof(message)-1)==sizeof(message)-1);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='maton-tty-query-') as tmp:
    tmp=Path(tmp);c=tmp/'test.c';c.write_text(source);binary=tmp/'test'
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary),str(root/'build/linux-layer-v3')],check=True,timeout=20)
