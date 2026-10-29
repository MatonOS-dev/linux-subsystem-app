/* Embed the real proxy and exercise its internal channel writers with the
 * payload's SIGPIPE disposition at SIG_DFL. Chromium resets handlers in forked
 * children, so a layer write to a channel whose peer has gone away must fail
 * with EPIPE instead of killing the payload process. */
#include "../proxy.c"
#define CHECK(x) do { if(!(x))return __LINE__; } while(0)
int main(void) {
    struct kernel_action dfl={0};
    CHECK(RAW(PROXY_SC_rt_sigaction,SIGPIPE,&dfl,0,8,0,0)==0);
    const uint64_t sigpipe=UINT64_C(1)<<(SIGPIPE-1);

    /* Control: the same broken pipe is fatal to an unprotected raw write. */
    int control[2];CHECK(RAW(PROXY_SC_pipe2,control,O_CLOEXEC,0,0,0,0)==0);
    long child=RAW(PROXY_SC_clone,SIGCHLD,0,0,0,0,0);CHECK(child>=0);
    if(!child) {
        RAW(PROXY_SC_close,control[0],0,0,0,0,0);
        RAW(PROXY_SC_write,control[1],"x",1,0,0,0);
        RAW(PROXY_SC_exit,0,0,0,0,0,0);
    }
    CHECK(RAW(PROXY_SC_close,control[0],0,0,0,0,0)==0);
    int status=0;CHECK(RAW(PROXY_SC_wait4,child,&status,0,0,0,0)==child);
    CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGPIPE);
    RAW(PROXY_SC_close,control[1],0,0,0,0,0);

    /* A resident pipe channel whose reader was closed in the child. */
    int pipefd[2];CHECK(RAW(PROXY_SC_pipe2,pipefd,O_CLOEXEC,0,0,0,0)==0);
    CHECK(RAW(PROXY_SC_close,pipefd[0],0,0,0,0,0)==0);
    uint64_t before=0,after=0,pending=0;
    CHECK(RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,0,&before,8,0,0)==0);
    CHECK(proxy_internal_write(pipefd[1],"p",1)==-EPIPE);
    CHECK(RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,0,&after,8,0,0)==0);
    CHECK(before==after);
    CHECK(RAW(PROXY_SC_rt_sigpending,&pending,8,0,0,0,0)==0 && !(pending&sigpipe));
    RAW(PROXY_SC_close,pipefd[1],0,0,0,0,0);

    /* The adopted-thread rejection path writes to the payload's fd 2. */
    long saved=RAW(PROXY_SC_fcntl,2,F_DUPFD_CLOEXEC,10,0,0,0);CHECK(saved>=0);
    int stderr_pipe[2];CHECK(RAW(PROXY_SC_pipe2,stderr_pipe,O_CLOEXEC,0,0,0,0)==0);
    CHECK(RAW(PROXY_SC_close,stderr_pipe[0],0,0,0,0,0)==0);
    CHECK(RAW(PROXY_SC_dup3,stderr_pipe[1],2,0,0,0,0)==2);
    RAW(PROXY_SC_close,stderr_pipe[1],0,0,0,0,0);
    CHECK(proxy_internal_write(2,"e",1)==-EPIPE);
    CHECK(RAW(PROXY_SC_dup3,saved,2,0,0,0,0)==2);
    RAW(PROXY_SC_close,saved,0,0,0,0,0);

    /* A socket channel whose peer went away takes MSG_NOSIGNAL. */
    int socketfd[2];CHECK(RAW(PROXY_SC_socketpair,AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,socketfd,0,0)==0);
    CHECK(RAW(PROXY_SC_close,socketfd[1],0,0,0,0,0)==0);
    CHECK(proxy_internal_send(socketfd[0],"s",1,0)==-EPIPE);
    CHECK(proxy_internal_sendto(socketfd[0],"t",1,0,0,0)==-EPIPE);
    CHECK(RAW(PROXY_SC_rt_sigpending,&pending,8,0,0,0,0)==0 && !(pending&sigpipe));
    RAW(PROXY_SC_close,socketfd[0],0,0,0,0,0);

    /* A SIGPIPE already pending before the write survives; only a signal this
     * helper generated is consumed. */
    uint64_t old=0;
    CHECK(RAW(PROXY_SC_rt_sigprocmask,SIG_BLOCK,&sigpipe,&old,8,0,0)==0);
    CHECK(RAW(PROXY_SC_tgkill,RAW(PROXY_SC_getpid,0,0,0,0,0,0),RAW(PROXY_SC_gettid,0,0,0,0,0,0),SIGPIPE,0,0,0)==0);
    int pending_pipe[2];CHECK(RAW(PROXY_SC_pipe2,pending_pipe,O_CLOEXEC,0,0,0,0)==0);
    CHECK(RAW(PROXY_SC_close,pending_pipe[0],0,0,0,0,0)==0);
    CHECK(proxy_internal_write(pending_pipe[1],"q",1)==-EPIPE);
    pending=0;CHECK(RAW(PROXY_SC_rt_sigpending,&pending,8,0,0,0,0)==0 && (pending&sigpipe));
    struct {long sec,nsec;} zero={0,0};
    CHECK(RAW(PROXY_SC_rt_sigtimedwait,&sigpipe,0,&zero,8,0,0)==SIGPIPE);
    CHECK(RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0)==0);
    RAW(PROXY_SC_close,pending_pipe[1],0,0,0,0,0);

    static const char pass[]="PASS internal pipe/socket writes keep a default-SIGPIPE payload alive\n";
    CHECK(RAW(PROXY_SC_write,1,pass,sizeof(pass)-1,0,0,0)==(long)(sizeof(pass)-1));
    return 0;
}
