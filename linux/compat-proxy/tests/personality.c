#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

static void check_personality(void) {
    /* Verify Linux's query, set return, and subsequent previous-value
     * behavior. Under Android's syscall filter the proxy may emulate the set;
     * on desktop Linux the kernel may perform it. */
    int before=personality(0xffffffffUL);
    assert(before>=0);
    /* Model Flatpak's payload filter, which denies personality before normal
     * syscall dispatch. The compat proxy must still preserve Linux semantics. */
    struct sock_filter filter[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_personality,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program={.len=sizeof(filter)/sizeof(filter[0]),.filter=filter};
    assert(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
    assert(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program)==0);
    errno=0;
    long old=syscall(SYS_personality,PER_LINUX32);
    assert(old==before);
    assert(personality(0xffffffffUL)==PER_LINUX32);
    old=syscall(SYS_personality,PER_LINUX);
    assert(old==PER_LINUX32);
    assert(personality(0xffffffffUL)==PER_LINUX);
    /* Container/Android syscall filters commonly refuse ADDR_NO_RANDOMIZE;
     * the proxy must still remember the requested full persona. */
    old=syscall(SYS_personality,ADDR_NO_RANDOMIZE);
    assert(old==PER_LINUX);
    assert(personality(0xffffffffUL)==ADDR_NO_RANDOMIZE);
    old=syscall(SYS_personality,PER_LINUX);
    assert(old==ADDR_NO_RANDOMIZE);
    assert(personality(0xffffffffUL)==PER_LINUX);
}

int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--exec-child")) {
        check_personality();
        puts("PASS personality semantics in exec'd child");
        return 0;
    }

    check_personality();
    pid_t child=fork();
    assert(child>=0);
    if(!child) {
        execl(argv[0],argv[0],"--exec-child",(char *)NULL);
        _exit(127);
    }
    int status=0;
    assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    puts("PASS personality query/set/previous semantics survive Flatpak payload seccomp");
    return 0;
}
