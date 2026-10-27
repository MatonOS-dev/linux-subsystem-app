/* Replay the portal's Flatpak argv boundary, then execute stock bwrap. The
 * offline fixture substitutes only Flatpak's deployment lookup/arg assembly. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc,char **argv) {
    assert(argc>=2);
    if(!strcmp(argv[1],"payload")) {
        errno=0;assert(unshare(CLONE_NEWUSER)==-1 && errno==EPERM);
        puts("PASS portal sandbox devpts setup, disabled-userns assertion and payload filter");
        return 0;
    }
    if(!strcmp(argv[1],"run")) {
        assert(argc==8 && !strcmp(argv[2],"--sandbox") &&
               !strncmp(argv[3],"--parent-pid=",13) && !strcmp(argv[4],"--parent-expose-pids"));
        /* Actual day-r20 bundle has --dev + --assert-userns-disabled,
         * no --userns/--pidns fds. Non-root bwrap adds CLONE_NEWUSER itself. */
        execl(argv[5],argv[5],"--args","68","--","/fixture","payload",NULL);
        return 91;
    }
    assert(argc==3 && !strcmp(argv[1],"launch"));
    assert(unshare(CLONE_NEWUSER)==0);
    int limit=open("/proc/sys/user/max_user_namespaces",O_WRONLY|O_CLOEXEC);
    assert(limit>=0 && write(limit,"0",1)==1 && close(limit)==0);
    struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_unshare,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_clone,0,3),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[0])),
        BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K,CLONE_NEWUSER,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog filter={sizeof(code)/sizeof(code[0]),code};
    assert(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
    assert(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&filter)==0);
    errno=0;assert(unshare(CLONE_NEWUSER)==-1 && errno==EPERM);
    int fd=memfd_create("portal-bwrap-args",0);assert(fd>=0);
    const char args[]="--ro-bind\0/usr\0/usr\0--symlink\0usr/lib\0/lib\0--symlink\0usr/lib64\0/lib64\0--symlink\0usr/bin\0/bin\0--proc\0/proc\0--dev\0/dev\0--unshare-pid\0--assert-userns-disabled";
    assert(write(fd,args,sizeof(args))==sizeof(args));
    const char bind[]="--ro-bind";
    assert(write(fd,bind,sizeof(bind))==sizeof(bind));
    assert(write(fd,argv[0],strlen(argv[0])+1)==(ssize_t)strlen(argv[0])+1);
    assert(write(fd,"/fixture",9)==9);
    assert(lseek(fd,0,SEEK_SET)==0);
    assert(dup2(fd,68)==68 && close(fd)==0);
    pid_t child=fork();assert(child>=0);
    if(!child){
        char parent[64];snprintf(parent,sizeof(parent),"--parent-pid=%ld",(long)getppid());
        execl(argv[0],"flatpak","run","--sandbox",parent,"--parent-expose-pids",argv[2],argv[0],"fixture",NULL);
        _exit(92);
    }
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    return 0;
}
