#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/syscall.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s errno=%d\n",__LINE__,#x,errno);return 1;}}while(0)
int main(int argc,char **argv){
 CHECK(argc==3);CHECK(unshare(CLONE_NEWNS|CLONE_NEWUSER)==0);
 CHECK(mount(argv[1],"/virtual-v3",NULL,MS_BIND,NULL)==0);CHECK(mount(argv[2],"/virtual-v3/nested",NULL,MS_BIND,NULL)==0);
 int fd=open("/virtual-v3",O_DIRECTORY);CHECK(fd>=0);int f=openat(fd,"fixture",O_RDONLY);CHECK(f>=0);close(f);
 CHECK(chdir("/virtual-v3/nested")==0);f=open("../fixture",O_RDONLY);CHECK(f>=0);close(f);
 CHECK(mount("/virtual-v3","/alias-v3",NULL,MS_BIND|MS_REC,NULL)==0);CHECK(chdir("/alias-v3/nested")==0);
 char cwd[1024];CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,"/alias-v3/nested"));CHECK(chdir("/virtual-v3/nested")==0);CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,"/virtual-v3/nested"));
 f=open("/virtual-v3/absolute",O_RDONLY);CHECK(f>=0);close(f);f=open("/virtual-v3/relative",O_RDONLY);CHECK(f>=0);close(f);
 struct stat st;CHECK(lstat("/virtual-v3/absolute",&st)==0 && S_ISLNK(st.st_mode));
 errno=0;CHECK(linkat(AT_FDCWD,"/virtual-v3/absolute",AT_FDCWD,"/virtual-v3/hard-symlink",0)==-1 && errno==EPERM);
 errno=0;CHECK(open("/virtual-v3/absolute",O_CREAT|O_EXCL|O_WRONLY,0600)==-1 && errno==EEXIST);
 errno=0;CHECK(mkdir("/virtual-v3/absolute",0700)==-1 && errno==EEXIST);
 errno=0;CHECK(open("/virtual-v3/fixture/",O_RDONLY)==-1 && errno==ENOTDIR);
 errno=0;CHECK(open("/virtual-v3/missing/../fixture",O_RDONLY)==-1 && errno==ENOENT);
 CHECK(umount2("/alias-v3/nested",MNT_DETACH)==0);CHECK(umount2("/alias-v3",MNT_DETACH)==0);
 char proc[64],link[1024];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);ssize_t n=readlink(proc,link,sizeof(link)-1);CHECK(n>0);link[n]=0;CHECK(!strcmp(link,"/virtual-v3"));close(fd);
 errno=0;CHECK(rename("/virtual-v3/fixture","/virtual-v3/nested/elsewhere")==-1 && errno==EXDEV);
 errno=0;CHECK(linkat(AT_FDCWD,"/virtual-v3/fixture",AT_FDCWD,"/virtual-v3/nested/elsewhere",0)==-1 && errno==EXDEV);
 CHECK(mount(NULL,"/virtual-v3",NULL,MS_BIND|MS_REMOUNT|MS_RDONLY,NULL)==0);errno=0;CHECK(open("/virtual-v3/new",O_CREAT|O_WRONLY,0600)==-1 && errno==EROFS);
 /* Mount-point probes must return EEXIST without writing the bound source. */
 errno=0;CHECK(openat(AT_FDCWD,"/virtual-v3/fixture",O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW,0600)==-1 && errno==EEXIST);
 errno=0;CHECK(open("/virtual-v3/absolute",O_CREAT|O_EXCL|O_WRONLY,0600)==-1 && errno==EEXIST);
 errno=0;CHECK(open("/virtual-v3/new",O_CREAT|O_EXCL|O_WRONLY,0600)==-1 && errno==EROFS);
 errno=0;CHECK(open("/virtual-v3/fixture",O_WRONLY|O_TRUNC)==-1 && errno==EROFS);
 CHECK(mount("tmpfs","/private-v3","tmpfs",0,NULL)==0);f=open("/private-v3/file",O_CREAT|O_RDWR,0600);CHECK(f>=0);close(f);
 CHECK(mount("proc","/proc-v3","proc",0,NULL)==0);f=open("/proc-v3/self/status",O_RDONLY);CHECK(f>=0);close(f);
 CHECK(mount("devpts","/pts-v3","devpts",0,NULL)==0);CHECK(mount("mqueue","/mq-v3","mqueue",0,NULL)==0);
 CHECK(umount2("/private-v3",MNT_DETACH)==0);errno=0;CHECK(open("/private-v3/file",O_RDONLY)==-1 && errno==ENOENT);
 n=readlink("/proc/self/exe",link,sizeof(link)-1);CHECK(n>0);link[n]=0;CHECK(!strcmp(link,argv[0]));
 CHECK(chdir("/virtual-v3")==0);CHECK(chroot("/virtual-v3")==0);CHECK(chdir("/")==0);f=open("/fixture",O_RDONLY);CHECK(f>=0);close(f);CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,"/"));
 /* Stack an actual restriction. It applies to handler RAW syscalls as well. */
 struct sock_filter code[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getppid,0,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EACCES),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 struct sock_fprog program={4,code};CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);CHECK(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program)==0);errno=0;CHECK(syscall(SYS_getppid)==-1 && errno==EACCES);
 puts("PASS relative/dirfd/dotdot/symlinks/reverse/EXDEV/readonly/tmpfs/root/stacked-seccomp");return 0;
}
