#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc,char **argv) {
    assert(argc==3);
    if(!strcmp(argv[1],"payload")) {
        errno=0;
        assert(syscall(SYS_unshare,CLONE_NEWUSER)==-1 && errno==EPERM);
        puts("PASS APK bwrap filter deferred in monitor and active after payload exec");
        return 0;
    }
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
    struct sock_fprog filter={.len=sizeof(code)/sizeof(code[0]),.filter=code};
    assert(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
    assert(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&filter)==0);
    if(!strcmp(argv[1],"launch")) {
        /* The outer Flatpak payload already has an active filter before
         * its portal launches a nested APK-named bwrap monitor. */
        execl(argv[2],argv[2],"monitor",argv[0],NULL);
        return 90;
    }
    /* bwrap's monitor may start further namespace setup after installing the
     * payload's filter. It must not inherit that payload-only restriction. */
    assert(syscall(SYS_unshare,CLONE_NEWUSER)==0);
    pid_t child=syscall(SYS_clone,CLONE_NEWUSER|SIGCHLD,0,0,0,0);assert(child>=0);
    if(!child){execl(argv[2],argv[2],"payload",argv[2],NULL);_exit(91);}
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(syscall(SYS_unshare,CLONE_NEWUSER)==0);
    return 0;
}
