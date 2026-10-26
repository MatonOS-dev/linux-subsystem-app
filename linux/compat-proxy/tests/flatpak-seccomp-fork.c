#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int exec_child(void *arg) {
    char *const argv[]={(char *)arg,(char *)"after-clone-exec",NULL};
    execv(argv[0],argv);
    return 90;
}

int main(int argc,char **argv) {
    if(argc==2) {
        errno=0;
        assert(syscall(SYS_unshare,CLONE_NEWUSER)==-1 && errno==EPERM);
        puts("PASS Flatpak filter survives fork, clone retry, and exec");
        return 0;
    }
    struct sock_filter filter[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_unshare,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program={.len=sizeof(filter)/sizeof(filter[0]),.filter=filter};
    assert(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
    assert(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program)==0);

    pid_t forked=fork();
    assert(forked>=0);
    if(!forked) {
        size_t stack_size=1024*1024;
        void *stack=malloc(stack_size);
        assert(stack);
        int pidfd=-1;
        pid_t cloned=clone(exec_child,(char *)stack+stack_size,0x1011,argv[0],&pidfd);
        assert(cloned>=0);
        assert(pidfd>=0);
        int status=0;
        assert(waitpid(cloned,&status,0)==cloned);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
        _exit(0);
    }
    int status=0;
    assert(waitpid(forked,&status,0)==forked);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    return 0;
}
