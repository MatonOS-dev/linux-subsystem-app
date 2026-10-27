/* Resident Android libraries survive a Linux exec, so their descriptors must
 * survive too. This is ownership bookkeeping, not a sandbox boundary. */
struct resident_fd { struct resident_fd *next; int fd, state; };
static struct resident_fd *resident_fds;
static int resident_fds_enabled;
static struct resident_fd *resident_fd_find(int fd) {
    for(struct resident_fd *p=__atomic_load_n(&resident_fds,__ATOMIC_ACQUIRE);p;p=p->next)
        if(p->fd==fd)return p;
    return NULL;
}
static int resident_fd_set(int fd,int state) {
    if(fd<0)return 0;
    struct resident_fd *p=resident_fd_find(fd);
    if(!p && state) {
        struct resident_fd *fresh=vfs_palloc(sizeof(*fresh));if(!fresh)return -ENOMEM;
        fresh->fd=fd;fresh->state=state;
        for(;;) {
            struct resident_fd *head=__atomic_load_n(&resident_fds,__ATOMIC_ACQUIRE);
            for(p=head;p;p=p->next)if(p->fd==fd)break;
            if(p){vfs_free(fresh);break;}
            fresh->next=head;
            if(__atomic_compare_exchange_n(&resident_fds,&head,fresh,0,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE))return 0;
        }
    }
    if(p)__atomic_store_n(&p->state,state,__ATOMIC_RELEASE);
    return 0;
}
static int resident_fd_owned(int fd) {
    struct resident_fd *p=resident_fd_find(fd);
    return p && __atomic_load_n(&p->state,__ATOMIC_ACQUIRE)==2;
}
/* Called on the single loader thread immediately around Android EGL warmup.
 * Existing launch/control descriptors remain payload-owned. */
int proxy_resident_fds_snapshot(int after) {
    long dir=RAW(PROXY_SC_openat,AT_FDCWD,"/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,0,0);
    if(dir<0)return (int)dir;
    char buf[4096];long n;int error=0;
    while((n=RAW(PROXY_SC_getdents64,dir,buf,sizeof(buf),0,0,0))>0) {
        for(long off=0;off<n;) {
            unsigned short len=*(unsigned short *)(buf+off+16);
            if(len<20 || off+len>n){error=-EIO;goto done;}
            const char *s=buf+off+19;off+=len;int fd=0;
            if(*s<'0'||*s>'9')continue;
            while(*s>='0'&&*s<='9')fd=fd*10+(*s++-'0');
            if(*s || fd==dir)continue;
            struct resident_fd *p=resident_fd_find(fd);
            if(!after || !p || p->state!=1) {
                error=resident_fd_set(fd,after?2:1);if(error)goto done;
            }
        }
    }
    if(n<0)error=(int)n;
done:
    RAW(PROXY_SC_close,dir,0,0,0,0,0);
    if(after) {
        for(struct resident_fd *p=resident_fds;p;p=p->next)if(p->state==1)p->state=0;
        resident_fds_enabled=1;
    }
    return error;
}
/* Trap only descriptor lifetime operations even in the BPF native ranges.
 * Driver mmap/ioctl and Android pthread/allocator calls remain native. */
static int resident_fd_call(long n) {
    switch(n) {
    case PROXY_SC_open:case PROXY_SC_openat:case PROXY_SC_openat2:
    case PROXY_SC_close:case PROXY_SC_close_range:
    case PROXY_SC_dup:case PROXY_SC_dup2:case PROXY_SC_dup3:case PROXY_SC_fcntl:
    case PROXY_SC_socket:case PROXY_SC_socketpair:case PROXY_SC_pipe:case PROXY_SC_pipe2:
    case PROXY_SC_accept:case PROXY_SC_accept4:case PROXY_SC_recvmsg:
    case PROXY_SC_epoll_create:case PROXY_SC_epoll_create1:
    case PROXY_SC_eventfd:case PROXY_SC_eventfd2:case PROXY_SC_inotify_init:case PROXY_SC_inotify_init1:
    case PROXY_SC_memfd_create:case PROXY_SC_timerfd_create:case PROXY_SC_signalfd:case PROXY_SC_signalfd4:
        return 1;
    default:return 0;
    }
}
static void resident_fd_rights(struct msghdr *msg) {
    size_t used=0;
    while(used<=msg->msg_controllen && msg->msg_controllen-used>=sizeof(struct cmsghdr)) {
        struct cmsghdr *c=(void *)((char *)msg->msg_control+used);
        if(c->cmsg_len<CMSG_LEN(0) || c->cmsg_len>msg->msg_controllen-used)break;
        if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS)
            for(size_t i=0;i<(c->cmsg_len-CMSG_LEN(0))/sizeof(int);i++)
                if(resident_fd_set(((int *)CMSG_DATA(c))[i],2))RAW(PROXY_SC_exit_group,125,0,0,0,0,0);
        size_t step=CMSG_ALIGN(c->cmsg_len);if(step<c->cmsg_len)break;
        used+=step;
    }
}
static long resident_fd_native(long n,proxy_arg_t a[6]) {
    int duplicate_target=-1,duplicate_source_owned=0;
    if(n==PROXY_SC_dup2 || n==PROXY_SC_dup3) {
        duplicate_target=(int)a[1];
        duplicate_source_owned=resident_fd_owned((int)a[0]);
    }
    /* Publish release before close makes this number available to a new
     * native open on another thread. Linux closes the fd even on EINTR. */
    if(n==PROXY_SC_close && resident_fds_enabled)resident_fd_set((int)a[0],0);
    long r=RAW(n,a[0],a[1],a[2],a[3],a[4],a[5]);
    if(r<0 || !resident_fds_enabled || !resident_fd_call(n))return r;
    /* dup2/dup3 may replace a resident descriptor with an ordinary payload
     * descriptor. Once the kernel has performed that replacement, retire the
     * old ownership record; preserve it only when duplicating another resident
     * descriptor into the target. */
    if(duplicate_target>=0 && duplicate_target!=(int)a[0])
        resident_fd_set(duplicate_target,duplicate_source_owned?2:0);
    int fd=-1,error=0;
    switch(n) {
    case PROXY_SC_close:break;
    case PROXY_SC_close_range:
        if(!(a[2]&4))for(struct resident_fd *p=resident_fds;p;p=p->next)
            if((unsigned)p->fd>=(unsigned)a[0] && (unsigned)p->fd<=(unsigned)a[1])resident_fd_set(p->fd,0);
        break;
    case PROXY_SC_recvmsg:resident_fd_rights((void *)(uintptr_t)a[1]);break;
    case PROXY_SC_socketpair:case PROXY_SC_pipe:case PROXY_SC_pipe2: {
        int *pair=(void *)(uintptr_t)a[n==PROXY_SC_socketpair?3:0];
        error=resident_fd_set(pair[0],2);if(!error)error=resident_fd_set(pair[1],2);break;
    }
    case PROXY_SC_dup2:case PROXY_SC_dup3:break;
    case PROXY_SC_fcntl:if(a[1]==F_DUPFD || a[1]==F_DUPFD_CLOEXEC)fd=(int)r;break;
    default:fd=(int)r;break;
    }
    if(fd>=0)error=resident_fd_set(fd,2);
    /* Losing ownership under real memory exhaustion would corrupt the next
     * Linux exec. Fail the process instead of publishing a stale fd table. */
    if(error)RAW(PROXY_SC_exit_group,125,0,0,0,0,0);
    return r;
}
