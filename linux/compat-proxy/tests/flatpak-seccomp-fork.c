#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <linux/filter.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static const size_t child_stack_size=1024*1024;
static volatile uintptr_t signal_stack_local;
static int reserve_fd;

static void chained_syscall(int signal, siginfo_t *info, void *context) {
    (void)signal;(void)info;(void)context;
}
static void stack_probe(int signal) {
    (void)signal;
    volatile char local;
    signal_stack_local=(uintptr_t)&local;
}

static void duplicate_proc_fds(void) {
    DIR *directory=opendir("/proc/self/fd");
    assert(directory);
    int directory_fd=dirfd(directory),fds[128],count=0;
    struct dirent *entry;
    while((entry=readdir(directory))) {
        char *end=NULL;
        long fd=strtol(entry->d_name,&end,10);
        if(!entry->d_name[0] || *end || fd<0 || fd==directory_fd)continue;
        assert(count<(int)(sizeof(fds)/sizeof(fds[0])));
        fds[count++]=(int)fd;
    }
    assert(closedir(directory)==0);
    for(int i=0;i<count;i++) {
        int copy=dup(fds[i]);
        if(copy<0)fprintf(stderr,"listed fd %d failed dup errno=%d\n",fds[i],errno);
        assert(copy>=0);
        close(copy);
    }
}

static int exec_child(void *arg) {
    /* Zypak's spawn path snapshots /proc/self/fd and dup()s every entry. */
    duplicate_proc_fds();
    /* Match Chromium/zypak when it allocates at the top of the soft limit.
     * This target is the first private reserve fd (limit - 18). */
    int allocated=fcntl(15,F_DUPFD_CLOEXEC,reserve_fd);
    if(allocated!=reserve_fd)fprintf(stderr,"reserved fcntl got=%d wanted=%d errno=%d\n",allocated,reserve_fd,errno);
    assert(allocated==reserve_fd);
    duplicate_proc_fds();
    assert(dup2(reserve_fd,4)==4);
    char byte=0;
    assert(read(4,&byte,1)==1 && byte=='x');
    stack_t current_stack;
    assert(sigaltstack(NULL,&current_stack)==0);
    /* The query exposes virtual payload state, not the resident stack. */
    struct sigaction stack_action={.sa_handler=stack_probe,.sa_flags=SA_ONSTACK};
    sigemptyset(&stack_action.sa_mask);
    assert(sigaction(SIGUSR1,&stack_action,NULL)==0);
    assert(raise(SIGUSR1)==0);
    assert(signal_stack_local!=0);
    struct sigaction action={.sa_sigaction=chained_syscall,.sa_flags=SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGSYS,&action,NULL)==0);
    errno=0;
    assert(syscall(SYS_unshare,CLONE_NEWUSER)==-1 && errno==EPERM);
    char *const argv[]={(char *)arg,(char *)"after-clone-exec",NULL};
    execv(argv[0],argv);
    return 90;
}

int main(int argc,char **argv) {
    struct rlimit limit;
    assert(getrlimit(RLIMIT_NOFILE,&limit)==0 && limit.rlim_cur>=64);
    reserve_fd=(int)limit.rlim_cur-18;
    if(argc==2) {
        errno=0;
        assert(syscall(SYS_unshare,CLONE_NEWUSER)==-1 && errno==EPERM);
        if(!strcmp(argv[1],"after-clone-exec")) {
            char byte=0;
            assert(read(4,&byte,1)==1 && byte=='x');
        }
        puts("PASS Flatpak filter survives fork, clone retry, and exec");
        return 0;
    }
    struct sock_filter filter[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_unshare,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_clone3,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|ENOSYS),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program={.len=sizeof(filter)/sizeof(filter[0]),.filter=filter};
    assert(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);
    assert(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program)==0);

    int pipefd[2];
    assert(pipe(pipefd)==0);
    assert(dup2(pipefd[0],15)==15);
    assert(write(pipefd[1],"xx",2)==2);
    pid_t forked=fork();
    assert(forked>=0);
    if(!forked) {
        void *stack=malloc(child_stack_size);
        assert(stack);
        int pidfd=-1;
        /* Flatpak's Android filter denies clone3, so libc retries through
         * clone(2). Match posix_spawn's vfork-style request: the proxy must
         * strip CLONE_VM|CLONE_VFORK while retaining CLONE_PIDFD, then adopt
         * and exec the new process from the retry continuation. */
        struct clone_args args={
            .flags=CLONE_VM|CLONE_VFORK|CLONE_PIDFD,
            .pidfd=(uintptr_t)&pidfd,
            .exit_signal=SIGCHLD,
        };
        errno=0;
        assert(syscall(SYS_clone3,&args,sizeof(args))==-1 && errno==ENOSYS);
        pid_t cloned=clone(exec_child,(char *)stack+child_stack_size,
                           CLONE_VM|CLONE_VFORK|CLONE_PIDFD|SIGCHLD,
                           argv[0],&pidfd);
        assert(cloned>=0);
        assert(pidfd>=0);
        free(stack);
        int status=0;
        assert(waitpid(cloned,&status,0)==cloned);
        if(!WIFEXITED(status) || WEXITSTATUS(status)!=0)
            fprintf(stderr,"clone child status=0x%x signal=%d exit=%d\n",status,
                    WIFSIGNALED(status)?WTERMSIG(status):0,
                    WIFEXITED(status)?WEXITSTATUS(status):-1);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
        _exit(0);
    }
    int status=0;
    assert(waitpid(forked,&status,0)==forked);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    /* Exercise libc's own clone3 -> clone retry and spawn signal setup too. */
    char *spawn_argv[]={argv[0],"after-spawn-exec",NULL};
    extern char **environ;
    pid_t spawned;
    assert(posix_spawn(&spawned,argv[0],NULL,NULL,spawn_argv,environ)==0);
    assert(waitpid(spawned,&status,0)==spawned);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    return 0;
}
