/* Embed the real proxy and drive the layer's liblog path the way a payload
 * process does: the payload sits at SIGPIPE = SIG_DFL (Chromium resets
 * dispositions in children), it sweeps every descriptor above 2 with
 * close_range(), and bionic-side logging then happens from the same process.
 *
 * The logging must reach logd, the process must stay alive, and the layer's
 * logd descriptor must never be closed or handed to payload data. Before the
 * layer owned the socket, liblog cached its own logd descriptor in a range the
 * layer never trapped: the sweep closed it, a payload allocation recycled the
 * number, and the next write hit payload data (EPIPE -> fatal SIGPIPE). */
#include "../proxy.c"
#define CHECK(x) do { if(!(x))return __LINE__; } while(0)
static int listener;
static unsigned char record[512];
static long receive(void) {
    return RAW(PROXY_SC_recvfrom,listener,record,sizeof(record),MSG_DONTWAIT,0,0);
}
static int expect(const char *tag,const char *message) {
    if(receive()<=(long)(ANDROID_LOG_HEADER_BYTES+1))return 1;
    if(record[0]!=0 || record[ANDROID_LOG_HEADER_BYTES]!=4)return 2;
    if(!p_equal((char *)record+ANDROID_LOG_HEADER_BYTES+1,tag))return 3;
    if(!p_equal((char *)record+ANDROID_LOG_HEADER_BYTES+1+p_len(tag)+1,message))return 4;
    uint16_t tid=(uint16_t)RAW(PROXY_SC_gettid,0,0,0,0,0,0);
    if(record[1]!=(unsigned char)tid || record[2]!=(unsigned char)(tid>>8))return 5;
    return 0;
}
int main(int argc,char **argv) {
    CHECK(argc>1);
    struct kernel_action dfl={0};
    CHECK(RAW(PROXY_SC_rt_sigaction,SIGPIPE,&dfl,0,8,0,0)==0);
    /* Logd-side listener on the fixture path, protected like any layer fd so
     * the payload's own sweep below does not remove it. */
    listener=(int)RAW(PROXY_SC_socket,AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,0,0,0);CHECK(listener>=0);
    struct sockaddr_un address={.sun_family=AF_UNIX};
    size_t length=p_len(argv[1]);
    CHECK(length && length+1<=sizeof(address.sun_path));
    p_copy(address.sun_path,argv[1],length+1);
    unsigned addrlen=(unsigned)(offsetof(struct sockaddr_un,sun_path)+length+1);
    CHECK(RAW(PROXY_SC_bind,listener,&address,addrlen,0,0,0)==0);
    proxy_fd_reserve_init();
    CHECK(proxy_fd_reserve_ready);
    listener=(int)proxy_private_fd(listener);CHECK(listener>=0);
    proxy_android_log_path(argv[1]);

    /* 1. The first bionic-side log opens the layer socket in the private
     *    reserve, so no payload close/dup target can reach it. */
    CHECK(proxy_android_log_write(ANDROID_LOG_ID_DEFAULT,4,"maton-layer","first")>0);
    int log_fd=proxy_android_log_descriptor();
    CHECK(log_fd>=3 && proxy_fd_is_private(log_fd));
    CHECK(expect("maton-layer","first")==0);

    /* 2. The payload's descriptor sweep (Chromium LaunchProcess closes every
     *    descriptor it does not keep) must leave the socket and its number
     *    alone. */
    CHECK(payload_close_range(3,~0u,0)==0);
    CHECK(RAW(PROXY_SC_fcntl,log_fd,F_GETFD,0,0,0,0)>=0);
    CHECK(proxy_android_log_descriptor()==log_fd);
    CHECK(proxy_android_log_write(ANDROID_LOG_ID_DEFAULT,4,"maton-layer","swept")>0);
    CHECK(expect("maton-layer","swept")==0);

    /* 3. A payload dup2 onto that number relocates the layer socket first; the
     *    payload keeps the number for its own pipe and no log data lands in it. */
    int pipefd[2];CHECK(RAW(PROXY_SC_pipe2,pipefd,O_CLOEXEC|O_NONBLOCK,0,0,0,0)==0);
    proxy_arg_t duplicate[6]={pipefd[1],log_fd,0,0,0,0};
    CHECK(descriptor_dup_dispatch(PROXY_SC_dup2,duplicate)==log_fd);
    CHECK(proxy_android_log_descriptor()!=log_fd);
    CHECK(proxy_fd_is_private(proxy_android_log_descriptor()));
    CHECK(proxy_android_log_write(ANDROID_LOG_ID_DEFAULT,4,"maton-layer","renumbered")>0);
    CHECK(expect("maton-layer","renumbered")==0);
    unsigned char data[64];
    CHECK(RAW(PROXY_SC_read,pipefd[0],data,sizeof(data),0,0,0)==-EAGAIN);

    /* 4. The payload's number now has no reader: a layer write through a stale
     *    or recycled number would raise SIGPIPE here and kill this process. */
    CHECK(RAW(PROXY_SC_close,pipefd[0],0,0,0,0,0)==0);
    CHECK(proxy_android_log_write(ANDROID_LOG_ID_DEFAULT,4,"maton-layer","after-reuse")>0);
    CHECK(expect("maton-layer","after-reuse")==0);
    /* Control: the very same write through the payload's recycled number is
     * fatal at this disposition, so the checks above are not vacuous. */
    long control=RAW(PROXY_SC_clone,SIGCHLD,0,0,0,0,0);CHECK(control>=0);
    if(!control) {
        RAW(PROXY_SC_write,log_fd,"x",1,0,0,0);
        RAW(PROXY_SC_exit,0,0,0,0,0,0);
    }
    int status=0;CHECK(RAW(PROXY_SC_wait4,control,&status,0,0,0,0)==control);
    CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGPIPE);

    /* 5. A vanished logd peer reports an error without SIGPIPE and without
     *    touching the payload's mask or pending set. */
    CHECK(RAW(PROXY_SC_close,listener,0,0,0,0,0)==0);
    uint64_t before=0,after=0,pending=0;
    CHECK(RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,0,&before,8,0,0)==0);
    CHECK(proxy_android_log_write(ANDROID_LOG_ID_DEFAULT,4,"maton-layer","no-peer")<0);
    CHECK(RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,0,&after,8,0,0)==0);
    CHECK(before==after);
    CHECK(RAW(PROXY_SC_rt_sigpending,&pending,8,0,0,0,0)==0);
    CHECK(!(pending&(UINT64_C(1)<<(SIGPIPE-1))));
    RAW(PROXY_SC_unlink,argv[1],0,0,0,0,0);

    static const char pass[]="PASS bionic-side logd writes survive a default-SIGPIPE payload\n";
    CHECK(RAW(PROXY_SC_write,1,pass,sizeof(pass)-1,0,0,0)==(long)(sizeof(pass)-1));
    return 0;
}
