/* Per-app DNS: replace only sockets addressed to the configured loopback :53.
 * The replacement is a real Unix socket, so kernel queues implement readiness,
 * datagrams, MSG_PEEK, timeouts, and concurrent outstanding query IDs. */
#include <sys/epoll.h>
#include <poll.h>
#include <time.h>
static int unix_call(long,proxy_arg_t [6],long *);
static struct sockaddr_in dns_endpoint;
static char dns_path[4096];
struct dns_socket {unsigned refs;int type,active,connected;struct sockaddr_in local;};
struct dns_fd {int fd;struct dns_socket *socket;struct dns_fd *next;};
struct dns_watch {int epoll,fd;struct epoll_event event;struct dns_watch *next;};
static struct dns_fd *dns_fds;
static struct dns_watch *dns_watches;
static unsigned dns_mutex;
static proxy_sigmask_t dns_lock(void){
    /* An app signal handler may itself use DNS. Mask signals during the short
     * metadata critical section to prevent reentering our own spin lock. */
    proxy_sigmask_t all=~UINT64_C(0),old=0;RAW(PROXY_SC_rt_sigprocmask,SIG_BLOCK,&all,&old,8,0,0);
    while(__atomic_exchange_n(&dns_mutex,1,__ATOMIC_ACQUIRE))__asm__ volatile("pause");
    return old;
}
static void dns_unlock(proxy_sigmask_t old){__atomic_store_n(&dns_mutex,0,__ATOMIC_RELEASE);RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0);}
/* Fork must snapshot complete descriptor metadata, never a half-converted
 * alias list or a spin lock held by a vanished thread. Stack-supplied clone
 * completes through the raw assembly gateway before returning to app code. */
static proxy_sigmask_t dns_fork_mask;
static long dns_fork_pid,dns_fork_tid;
static void dns_fork_prepare(void){
    proxy_sigmask_t mask=dns_lock();
    dns_fork_mask=mask;dns_fork_pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0);dns_fork_tid=RAW(PROXY_SC_gettid,0,0,0,0,0,0);
}
void dns_fork_result(long result){
    (void)result;
    if(dns_fork_pid && (RAW(PROXY_SC_getpid,0,0,0,0,0,0)!=dns_fork_pid || RAW(PROXY_SC_gettid,0,0,0,0,0,0)==dns_fork_tid)){
        proxy_sigmask_t mask=dns_fork_mask;dns_fork_pid=dns_fork_tid=0;dns_unlock(mask);
    }
}
static struct dns_socket *dns_lookup(int fd){for(struct dns_fd *p=dns_fds;p;p=p->next)if(p->fd==fd)return p->socket;return NULL;}
static void dns_forget(int fd){
    struct dns_fd **p=&dns_fds;
    while(*p){struct dns_fd *d=*p;if(d->fd!=fd){p=&d->next;continue;}*p=d->next;if(!--d->socket->refs)vfs_free(d->socket);vfs_free(d);break;}
    struct dns_watch **w=&dns_watches;
    while(*w){struct dns_watch *d=*w;if(d->fd!=fd && d->epoll!=fd){w=&d->next;continue;}*w=d->next;vfs_free(d);}
}
static void dns_close(int fd){proxy_sigmask_t mask=dns_lock();dns_forget(fd);dns_unlock(mask);}
static void dns_dup(int from,int to){
    if(from==to)return;
    proxy_sigmask_t mask=dns_lock();struct dns_socket *s=dns_lookup(from);dns_forget(to);
    if(s){struct dns_fd *d=vfs_alloc(sizeof(*d));if(d){*d=(struct dns_fd){to,s,dns_fds};s->refs++;dns_fds=d;}}
    dns_unlock(mask);
}
static void dns_network_payload(void){
    proxy_sigmask_t mask=dns_lock();
    struct dns_fd *d=dns_fds;
    while(d){struct dns_fd *next=d->next;if(d->socket->active){RAW(PROXY_SC_close,d->fd,0,0,0,0,0);dns_forget(d->fd);}d=next;}
    dns_unlock(mask);
}
/* poll may already be waiting when another thread first identifies a socket
 * as DNS. Native poll pins the old open file description across dup3. Recheck
 * unconverted Internet sockets in short slices so such a waiter follows the
 * application's fd after conversion. Active DNS fds use ordinary kernel poll. */
static long dns_poll(void *ptr,unsigned long count,struct timespec *timeout,const void *sigmask,unsigned long sigsize){
    struct pollfd *fds=ptr;int pending=0;
    if(dns_path[0] && ptr){proxy_sigmask_t mask=dns_lock();for(unsigned long i=0;i<count;i++){struct dns_socket *s=dns_lookup(fds[i].fd);if(s && !s->active){pending=1;break;}}dns_unlock(mask);}
    if(!pending || (timeout && (timeout->tv_sec<0 || timeout->tv_nsec<0 || timeout->tv_nsec>=1000000000)))return RAW(PROXY_SC_ppoll,ptr,count,timeout,sigmask,sigsize,0);
    struct timespec start,now;long r=RAW(PROXY_SC_clock_gettime,CLOCK_MONOTONIC,&start,0,0,0,0);if(r<0)return r;
    for(;;){
        struct timespec slice={.tv_nsec=10000000};
        if(timeout){
            RAW(PROXY_SC_clock_gettime,CLOCK_MONOTONIC,&now,0,0,0,0);
            long sec=timeout->tv_sec-(now.tv_sec-start.tv_sec),nano=timeout->tv_nsec-(now.tv_nsec-start.tv_nsec);
            if(nano<0){sec--;nano+=1000000000;}
            if(sec<0){slice.tv_nsec=0;}else if(!sec && nano<slice.tv_nsec)slice.tv_nsec=nano;
        }
        r=RAW(PROXY_SC_ppoll,ptr,count,&slice,sigmask,sigsize,0);
        if(r)return r;
        if(timeout){RAW(PROXY_SC_clock_gettime,CLOCK_MONOTONIC,&now,0,0,0,0);long sec=now.tv_sec-start.tv_sec,nano=now.tv_nsec-start.tv_nsec;if(nano<0){sec--;nano+=1000000000;}if(sec>timeout->tv_sec || (sec==timeout->tv_sec && nano>=timeout->tv_nsec))return 0;}
    }
}
static unsigned short dns_swap(unsigned short n){return (unsigned short)((n<<8)|(n>>8));}
int proxy_dns_forwarder(const char *text){
    unsigned char bytes[4];
    for(unsigned i=0;i<4;i++){
        unsigned n=0,digits=0;
        while(*text>='0' && *text<='9'){n=n*10+*text++-'0';if(n>255)return -EINVAL;digits++;}
        if(!digits || *text++!=(i==3?':':'.'))return -EINVAL;
        bytes[i]=(unsigned char)n;
    }
    /* The Internet listener has been retired. This is the app-visible address. */
    if(!p_equal(text,"53") || bytes[0]!=127)return -EINVAL;
    dns_endpoint.sin_family=AF_INET;dns_endpoint.sin_port=dns_swap(53);p_copy(&dns_endpoint.sin_addr,bytes,4);return 0;
}
int proxy_dns_socket(const char *path){
    size_t n=p_len(path);if(path[0]!='/' || n>=sizeof(dns_path))return -EINVAL;
    p_copy(dns_path,path,n+1);return 0;
}
static int dns_address(const void *addr,unsigned long len){
    if(!dns_endpoint.sin_port || !dns_path[0] || !addr || len<sizeof(struct sockaddr_in))return 0;
    const struct sockaddr_in *in=addr;
    return in->sin_family==AF_INET && in->sin_port==dns_endpoint.sin_port && in->sin_addr.s_addr==dns_endpoint.sin_addr.s_addr;
}
static long dns_name(void *out,unsigned *len,const struct sockaddr_in *name){
    if(!len || !out)return -EFAULT;
    unsigned n=*len;if(n>sizeof(*name))n=sizeof(*name);p_copy(out,name,n);*len=sizeof(*name);return 0;
}
/* Caller holds metadata lock. Do not hold it across a DNS receive. */
static long dns_convert(int fd,struct dns_socket *s){
    if(namespaces&CLONE_NEWNET)return -EACCES;
    if(s && s->active)return 0;
    if(!s || (s->type!=SOCK_DGRAM && s->type!=SOCK_STREAM))return -EPROTOTYPE;
    long flags=RAW(PROXY_SC_fcntl,fd,F_GETFL,0,0,0,0);if(flags<0)return flags;
    /* Connect locally in nonblocking mode, then restore the application flags.
     * A full listener backlog fails promptly rather than hanging a syscall. */
    long sock=RAW(PROXY_SC_socket,AF_UNIX,(s->type==SOCK_DGRAM?SOCK_SEQPACKET:SOCK_STREAM)|SOCK_CLOEXEC|SOCK_NONBLOCK,0,0,0,0);
    if(sock<0)return sock;
    size_t n=p_len(dns_path);char *slash=dns_path+n;while(slash>dns_path&&*slash!='/')slash--;
    if(slash==dns_path){RAW(PROXY_SC_close,sock,0,0,0,0,0);return -EINVAL;}
    *slash=0;long dir=RAW(PROXY_SC_openat,AT_FDCWD,dns_path,O_PATH|O_DIRECTORY|O_CLOEXEC,0,0,0);*slash='/';
    if(dir<0){RAW(PROXY_SC_close,sock,0,0,0,0,0);return dir;}
    struct sockaddr_un address={.sun_family=AF_UNIX};char path[sizeof(address.sun_path)];
    size_t at=0;const char *prefix="/proc/self/fd/";while(*prefix)path[at++]=*prefix++;
    char digits[24];int nd=0;unsigned long value=(unsigned long)dir;
    do{digits[nd++]=(char)('0'+value%10);value/=10;}while(value);
    while(nd)path[at++]=digits[--nd];
    size_t tail=p_len(slash);if(at+tail+5>sizeof(path)){RAW(PROXY_SC_close,dir,0,0,0,0,0);RAW(PROXY_SC_close,sock,0,0,0,0,0);return -ENAMETOOLONG;}
    p_copy(path+at,slash,tail);at+=tail;p_copy(path+at,s->type==SOCK_DGRAM?".udp":".tcp",5);
    p_copy(address.sun_path,path,at+5);
    long result=RAW(PROXY_SC_connect,sock,&address,offsetof(struct sockaddr_un,sun_path)+at+5,0,0,0);
    RAW(PROXY_SC_close,dir,0,0,0,0,0);
    if(result<0){RAW(PROXY_SC_close,sock,0,0,0,0,0);return result;}
    /* Authenticate both directions; a stale/replaced socket cannot grant a
     * different UID access to queries. Android's UID owns the path. */
    struct ucred cred;unsigned size=sizeof(cred);
    result=RAW(PROXY_SC_getsockopt,sock,SOL_SOCKET,SO_PEERCRED,&cred,&size,0);
    if(result<0 || cred.uid!=(unsigned)RAW(PROXY_SC_getuid,0,0,0,0,0,0)){RAW(PROXY_SC_close,sock,0,0,0,0,0);return result<0?result:-EACCES;}
    unsigned local_size=sizeof(s->local);RAW(PROXY_SC_getsockname,fd,&s->local,&local_size,0,0,0);
    /* Preserve options which matter to resolvers (including existing timeout). */
    int options[]={SO_RCVTIMEO,SO_SNDTIMEO,SO_RCVBUF,SO_SNDBUF,SO_KEEPALIVE};
    for(unsigned i=0;i<sizeof(options)/sizeof(*options);i++){unsigned char value[32];unsigned len=sizeof(value);if(!RAW(PROXY_SC_getsockopt,fd,SOL_SOCKET,options[i],value,&len,0))RAW(PROXY_SC_setsockopt,sock,SOL_SOCKET,options[i],value,len,0);}
    RAW(PROXY_SC_fcntl,sock,F_SETFL,flags,0,0,0);
    /* All aliases share the new open file description. Keep each FD_CLOEXEC. */
    for(struct dns_fd *d=dns_fds;d;d=d->next)if(d->socket==s){long clo=RAW(PROXY_SC_fcntl,d->fd,F_GETFD,0,0,0,0);result=RAW(PROXY_SC_dup3,sock,d->fd,(clo&FD_CLOEXEC)?O_CLOEXEC:0,0,0,0);if(result<0)break;}
    if(result>=0){s->active=1;
        /* dup3 removes old epoll watches. Replay saved user events, including
         * EPOLLET/ONESHOT, on the replacement open file description. */
        for(struct dns_watch *w=dns_watches;w;w=w->next)if(dns_lookup(w->fd)==s){RAW(PROXY_SC_epoll_ctl,w->epoll,EPOLL_CTL_DEL,w->fd,0,0,0);long r=RAW(PROXY_SC_epoll_ctl,w->epoll,EPOLL_CTL_ADD,w->fd,&w->event,0,0);if(r<0 && result>=0)result=r;}
    }
    RAW(PROXY_SC_close,sock,0,0,0,0,0);return result<0?result:0;
}
static long dns_call(long nr,proxy_arg_t a[6]){
    {long translated;if(unix_call(nr,a,&translated))return translated;}
    if(nr==PROXY_SC_socket){
        long r=RAW(nr,a[0],a[1],a[2],0,0,0);
        if(r>=0 && dns_path[0] && a[0]==AF_INET){
            struct dns_socket *s=vfs_alloc(sizeof(*s));struct dns_fd *d=vfs_alloc(sizeof(*d));
            if(!s || !d){vfs_free(s);vfs_free(d);RAW(PROXY_SC_close,r,0,0,0,0,0);return -ENOMEM;}
            *s=(struct dns_socket){.refs=1,.type=(int)a[1]&~(SOCK_NONBLOCK|SOCK_CLOEXEC)};
            proxy_sigmask_t mask=dns_lock();dns_forget((int)r);*d=(struct dns_fd){(int)r,s,dns_fds};dns_fds=d;dns_unlock(mask);
        }return r;
    }
    proxy_sigmask_t mask=dns_lock();struct dns_socket *s=dns_lookup((int)a[0]);
    if(nr==PROXY_SC_epoll_ctl){
        struct dns_watch **p=&dns_watches;while(*p && ((*p)->epoll!=a[0] || (*p)->fd!=a[2]))p=&(*p)->next;
        struct dns_watch *new=NULL;
        if(nr==PROXY_SC_epoll_ctl && a[1]==EPOLL_CTL_ADD && dns_lookup((int)a[2])){new=vfs_alloc(sizeof(*new));if(!new){dns_unlock(mask);return -ENOMEM;}}
        long r=RAW(nr,a[0],a[1],a[2],a[3],0,0);
        if(!r){if(a[1]==EPOLL_CTL_DEL && *p){struct dns_watch *old=*p;*p=old->next;vfs_free(old);}else if(a[1]==EPOLL_CTL_MOD && *p)p_copy(&(*p)->event,(void *)(uintptr_t)a[3],sizeof(struct epoll_event));else if(new){*new=(struct dns_watch){.epoll=(int)a[0],.fd=(int)a[2],.next=dns_watches};p_copy(&new->event,(void *)(uintptr_t)a[3],sizeof(new->event));dns_watches=new;new=NULL;}}
        vfs_free(new);dns_unlock(mask);return r;
    }
    int target=0;
    if(nr==PROXY_SC_connect)target=dns_address((void *)(uintptr_t)a[1],a[2]);
    if(nr==PROXY_SC_sendto)target=dns_address((void *)(uintptr_t)a[4],a[5]);
    if(nr==PROXY_SC_sendmsg && a[1]){struct proxy_message m=arch_message_read((void *)(uintptr_t)a[1]);target=dns_address((void *)(uintptr_t)m.name,m.namelen);}
    if(nr==PROXY_SC_sendmmsg && a[1] && a[2]>0){struct proxy_message m=arch_mmsg_read((void *)(uintptr_t)a[1],0);target=dns_address((void *)(uintptr_t)m.name,m.namelen);}
    if(target){if(!s){dns_unlock(mask);return -EBADF;}long r=dns_convert((int)a[0],s);if(r<0){dns_unlock(mask);return r;}if(nr==PROXY_SC_connect){s->connected=1;dns_unlock(mask);return 0;}}
    int active=s && s->active,connected=active && s->connected;int type=s?s->type:0;struct sockaddr_in local={0};if(active)local=s->local;
    dns_unlock(mask);
    if(!active)return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
    if(nr==PROXY_SC_connect)return -EAFNOSUPPORT;
    if(nr==PROXY_SC_getpeername)return connected?dns_name((void *)(uintptr_t)a[1],(void *)(uintptr_t)a[2],&dns_endpoint):-ENOTCONN;
    if(nr==PROXY_SC_getsockname)return dns_name((void *)(uintptr_t)a[1],(void *)(uintptr_t)a[2],&local);
    if(nr==PROXY_SC_getsockopt && a[1]==SOL_SOCKET && (a[2]==SO_TYPE || a[2]==SO_DOMAIN || a[2]==SO_PROTOCOL)){
        int value=a[2]==SO_TYPE?type:a[2]==SO_DOMAIN?AF_INET:type==SOCK_DGRAM?IPPROTO_UDP:IPPROTO_TCP;
        if(!a[3] || !a[4])return -EFAULT;
        unsigned n=*(unsigned *)a[4];if(n>sizeof(value))n=sizeof(value);p_copy((void *)(uintptr_t)a[3],&value,n);*(unsigned *)a[4]=sizeof(value);return 0;
    }
    if(nr==PROXY_SC_sendto){if(a[4] && !target)return -EAFNOSUPPORT;return RAW(nr,a[0],a[1],a[2],a[3],0,0);}
    if(nr==PROXY_SC_sendmsg && a[1]){struct proxy_message m=arch_message_read((void *)(uintptr_t)a[1]);if(m.name && !dns_address((void *)(uintptr_t)m.name,m.namelen))return -EAFNOSUPPORT;m.name=0;m.namelen=0;return arch_message_call(nr,a[0],&m,a[2]);}
    if(nr==PROXY_SC_sendmmsg && a[1] && a[2]>0){
        unsigned count=(unsigned)a[2];if(count>1024)count=1024;
        void *m=vfs_alloc(arch_mmsg_size(count));if(!m)return -ENOMEM;p_copy(m,(void *)(uintptr_t)a[1],arch_mmsg_size(count));
        for(unsigned i=0;i<count;i++){struct proxy_message message=arch_mmsg_read(m,i);if(message.name && !dns_address((void *)(uintptr_t)message.name,message.namelen)){vfs_free(m);return -EAFNOSUPPORT;}arch_mmsg_no_name(m,i);}
        long r=RAW(nr,a[0],m,count,a[3],0,0);if(r>0)for(long i=0;i<r;i++)arch_mmsg_result((void *)(uintptr_t)a[1],m,i);vfs_free(m);return r;
    }
    if(nr==PROXY_SC_recvfrom){long r=RAW(nr,a[0],a[1],a[2],a[3],0,0);if(r>=0 && a[4]){long e=dns_name((void *)(uintptr_t)a[4],(void *)(uintptr_t)a[5],&dns_endpoint);if(e<0)return e;}return r;}
    if(nr==PROXY_SC_recvmsg && a[1]){void *out=(void *)(uintptr_t)a[1];struct proxy_message m=arch_message_read(out);void *name=(void *)(uintptr_t)m.name;unsigned len=m.namelen;m.name=0;m.namelen=0;long r=arch_message_call(nr,a[0],&m,a[2]);if(r>=0){m.flags&=~MSG_EOR;if(name){long e=dns_name(name,&len,&dns_endpoint);if(e<0)return e;m.namelen=len;}else m.namelen=0;arch_message_results(out,m);}return r;}
    return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
}
