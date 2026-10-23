/* Per-process DNS proxy. Applications retain ordinary AF_INET UDP sockets;
 * only the configured loopback resolver endpoint is redirected to a local
 * UDP helper. The helper asks Android netd asynchronously through libandroid. */
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
static int dns_fast_tls;
#if defined(__x86_64__) && defined(MATON_GPU_BRIDGE)
#include "../gpu-forward-spike/spike3/adopt.h"
static void *(*dns_android_open)(const char *,int);
static void *(*dns_android_symbol)(void *,const char *);
static struct adopt_api *dns_adopt_api;
static int (*dns_pthread_create)(unsigned long *,const void *,void *(*)(void *),void *);
static int (*dns_res_nsend)(uint64_t, const uint8_t *, size_t, uint32_t);
static int (*dns_res_nresult)(int, int *, void *, size_t);
static void *dns_android_lib,*dns_bionic_lib;
void proxy_dns_bridge_configure(void *(*open)(const char *,int),void *(*symbol)(void *,const char *),uintptr_t adopt,int fast){
    dns_android_open=open;dns_android_symbol=symbol;dns_adopt_api=(struct adopt_api *)adopt;dns_fast_tls=fast;
}
static int dns_resolver_symbols(void){
    if(!dns_android_open||!dns_android_symbol)return 0;
    if(!dns_android_lib)dns_android_lib=dns_android_open("/system/lib64/libandroid.so",2);
    if(!dns_bionic_lib)dns_bionic_lib=dns_android_open("/system/lib64/libc.so",2);
    if(!dns_android_lib||!dns_bionic_lib)return 0;
    if(!dns_res_nsend)dns_res_nsend=(int (*)(uint64_t,const uint8_t *,size_t,uint32_t))dns_android_symbol(dns_android_lib,"android_res_nsend");
    if(!dns_res_nresult)dns_res_nresult=(int (*)(int,int *,void *,size_t))dns_android_symbol(dns_android_lib,"android_res_nresult");
    if(!dns_pthread_create)dns_pthread_create=(int (*)(unsigned long *,const void *,void *(*)(void *),void *))dns_android_symbol(dns_bionic_lib,"pthread_create");
    return dns_res_nsend&&dns_res_nresult&&dns_pthread_create;
}
#else
static int dns_resolver_symbols(void){return 0;}
#endif
/* Default Android private resolver; --dns-forwarder can select a test or
 * alternate loopback address, but state no longer depends on exec snapshots. */
static struct sockaddr_in dns_endpoint={.sin_family=AF_INET,.sin_port=0x3500,.sin_addr={.s_addr=0x3500007f}};
/* Legacy launch argument retained temporarily for callers; no filesystem
 * transport is used by this implementation. */
struct dns_socket {unsigned refs;int type,active,connected;struct sockaddr_in local;unsigned short local_port;};
struct dns_fd {int fd;struct dns_socket *socket;struct dns_fd *next;};
static struct dns_fd *dns_fds;
static unsigned dns_mutex;
static volatile long dns_owner_pid;
static void dns_after_fork(void){long pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0);long owner=__atomic_load_n(&dns_owner_pid,__ATOMIC_ACQUIRE);if(owner==pid)return;if(__atomic_compare_exchange_n(&dns_owner_pid,&owner,pid,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)&&owner)__atomic_store_n(&dns_mutex,0,__ATOMIC_RELEASE);}
static proxy_sigmask_t dns_lock(void){dns_after_fork();proxy_sigmask_t all=~UINT64_C(0),old=0;RAW(PROXY_SC_rt_sigprocmask,SIG_BLOCK,&all,&old,8,0,0);while(__atomic_exchange_n(&dns_mutex,1,__ATOMIC_ACQUIRE))__asm__ volatile("pause");return old;}
static void dns_unlock(proxy_sigmask_t old){__atomic_store_n(&dns_mutex,0,__ATOMIC_RELEASE);RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0);}
static struct dns_socket *dns_lookup(int fd){for(struct dns_fd *p=dns_fds;p;p=p->next)if(p->fd==fd)return p->socket;return NULL;}
static void dns_untrack_port(unsigned short port);
static void dns_forget(int fd){struct dns_fd **p=&dns_fds;while(*p){struct dns_fd *d=*p;if(d->fd!=fd){p=&d->next;continue;}*p=d->next;if(!--d->socket->refs){if(d->socket->active)dns_untrack_port(d->socket->local_port);vfs_free(d->socket);}vfs_free(d);return;}}
static void dns_close(int fd){proxy_sigmask_t m=dns_lock();dns_forget(fd);dns_unlock(m);}
static void dns_dup(int from,int to){if(from==to)return;proxy_sigmask_t m=dns_lock();struct dns_socket *s=dns_lookup(from);dns_forget(to);if(s){struct dns_fd *d=vfs_palloc(sizeof(*d));if(d){*d=(struct dns_fd){to,s,dns_fds};s->refs++;dns_fds=d;}}dns_unlock(m);}
static unsigned short dns_swap(unsigned short n){return (unsigned short)((n<<8)|(n>>8));}
int proxy_dns_forwarder(const char *text){unsigned char b[4];for(unsigned i=0;i<4;i++){unsigned n=0,d=0;while(*text>='0'&&*text<='9'){n=n*10+*text++-'0';if(n>255)return -EINVAL;d++;}if(!d||*text++!=(i==3?':':'.'))return -EINVAL;b[i]=n;}if(!p_equal(text,"53")||b[0]!=127)return -EINVAL;dns_endpoint=(struct sockaddr_in){.sin_family=AF_INET,.sin_port=dns_swap(53)};p_copy(&dns_endpoint.sin_addr,b,4);return 0;}
static int dns_address(const void *addr,unsigned long len){if(!dns_endpoint.sin_port||!addr||len<sizeof(struct sockaddr_in))return 0;const struct sockaddr_in *a=addr;return a->sin_family==AF_INET&&a->sin_port==dns_endpoint.sin_port&&a->sin_addr.s_addr==dns_endpoint.sin_addr.s_addr;}
static struct sockaddr_in dns_helper_addr;
static int dns_helper_ready;
static volatile int dns_helper_state;
struct dns_pending {int fd;struct sockaddr_in peer;struct dns_pending *next;};
static struct dns_pending *dns_pending;
static int dns_server_fd=-1,dns_epoll_fd=-1;
static long dns_helper_pid;
#define DNS_PORTS 256
static unsigned dns_ports[DNS_PORTS];
static int dns_port_owned(unsigned short port){for(unsigned i=0;i<DNS_PORTS;i++)if(__atomic_load_n(&dns_ports[i],__ATOMIC_ACQUIRE)==port)return 1;return 0;}
static void dns_track_port(unsigned short port){if(!port||dns_port_owned(port))return;for(unsigned i=0;i<DNS_PORTS;i++){unsigned empty=0;if(__atomic_compare_exchange_n(&dns_ports[i],&empty,port,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED))return;}}
static void dns_untrack_port(unsigned short port){for(unsigned i=0;i<DNS_PORTS;i++){unsigned value=__atomic_load_n(&dns_ports[i],__ATOMIC_ACQUIRE);if(value==port)__atomic_compare_exchange_n(&dns_ports[i],&value,0,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED);}}
static __attribute__((unused)) void *dns_worker(void *unused){(void)unused;
#if defined(__x86_64__) && defined(MATON_GPU_BRIDGE)
    int server=(int)RAW(PROXY_SC_socket,AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0,0,0,0);if(server<0){__atomic_store_n(&dns_helper_state,-1,__ATOMIC_RELEASE);return NULL;}
    struct sockaddr_in bindaddr={.sin_family=AF_INET,.sin_addr={.s_addr=0x0100007f}};
    if(RAW(PROXY_SC_bind,server,&bindaddr,sizeof(bindaddr),0,0,0)<0){__atomic_store_n(&dns_helper_state,-1,__ATOMIC_RELEASE);return NULL;}
    unsigned len=sizeof(dns_helper_addr);if(RAW(PROXY_SC_getsockname,server,&dns_helper_addr,&len,0,0,0)<0){__atomic_store_n(&dns_helper_state,-1,__ATOMIC_RELEASE);return NULL;}
    int ep=(int)RAW(PROXY_SC_epoll_create1,EPOLL_CLOEXEC,0,0,0,0,0);if(ep<0){__atomic_store_n(&dns_helper_state,-1,__ATOMIC_RELEASE);return NULL;}dns_server_fd=server;dns_epoll_fd=ep;dns_helper_pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0);
    struct epoll_event ev={.events=EPOLLIN,.data.fd=server};RAW(PROXY_SC_epoll_ctl,ep,EPOLL_CTL_ADD,server,&ev,0,0);
    dns_helper_ready=1;__atomic_store_n(&dns_helper_state,1,__ATOMIC_RELEASE);
    for(;;){struct epoll_event events[32];long n=RAW(PROXY_SC_epoll_pwait,ep,events,32,-1,0,8);if(n<0)continue;
        for(long i=0;i<n;i++){
            int fd=events[i].data.fd;
            if(fd==server){unsigned char query[65536];struct sockaddr_in peer;unsigned plen=sizeof(peer);long q=RAW(PROXY_SC_recvfrom,server,query,sizeof(query),0,&peer,&plen);if(q<=0||!dns_port_owned(peer.sin_port))continue;
                int r=dns_res_nsend(0,query,(size_t)q,0);if(r<0)continue;struct dns_pending *p=vfs_palloc(sizeof(*p));if(!p){RAW(PROXY_SC_close,r,0,0,0,0,0);continue;}*p=(struct dns_pending){.fd=r,.peer=peer,.next=dns_pending};dns_pending=p;struct epoll_event re={.events=EPOLLIN,.data.fd=r};RAW(PROXY_SC_epoll_ctl,ep,EPOLL_CTL_ADD,r,&re,0,0);
            }else {struct dns_pending **pp=&dns_pending;while(*pp&&(*pp)->fd!=fd)pp=&(*pp)->next;if(!*pp)continue;struct dns_pending *p=*pp;unsigned char answer[65536];int rcode=0;RAW(PROXY_SC_epoll_ctl,ep,EPOLL_CTL_DEL,fd,0,0,0);int sz=dns_res_nresult(fd,&rcode,answer,sizeof(answer));if(sz>0)RAW(PROXY_SC_sendto,server,answer,sz,0,&p->peer,sizeof(p->peer));*pp=p->next;vfs_free(p);}
        }
    }
#else
    return NULL;
#endif
}
static __attribute__((unused)) uintptr_t dns_fs_read(void){uintptr_t fs=0;if(dns_fast_tls)__asm__ volatile("rdfsbase %0":"=r"(fs)::"memory");else RAW(PROXY_SC_arch_prctl,0x1003,&fs,0,0,0,0);return fs;}
static __attribute__((unused)) void dns_fs_write(uintptr_t fs){if(dns_fast_tls)__asm__ volatile("wrfsbase %0"::"r"(fs):"memory");else RAW(PROXY_SC_arch_prctl,0x1002,fs,0,0,0,0);}
static __attribute__((unused)) int dns_pthread_create_android(unsigned long *thread,void *(*start)(void *),void *arg){
#if defined(__x86_64__) && defined(MATON_GPU_BRIDGE)
    if(!dns_adopt_api||!dns_adopt_api->current)return -1;
    struct adopt_thread *t=dns_adopt_api->current();
    if(!t)return -1;
    uintptr_t prior=dns_fs_read();
    dns_fs_write(t->resident_fs);
    int result=dns_pthread_create(thread,NULL,start,arg);
    dns_fs_write(prior);
    return result;
#else
    (void)thread;(void)start;(void)arg;return -1;
#endif
}
static int dns_start(void){long pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0);if(dns_helper_pid&&dns_helper_pid!=pid){if(dns_server_fd>=0)RAW(PROXY_SC_close,dns_server_fd,0,0,0,0,0);if(dns_epoll_fd>=0)RAW(PROXY_SC_close,dns_epoll_fd,0,0,0,0,0);while(dns_pending){struct dns_pending *p=dns_pending;dns_pending=p->next;RAW(PROXY_SC_close,p->fd,0,0,0,0,0);vfs_free(p);}dns_server_fd=dns_epoll_fd=-1;dns_helper_pid=0;dns_helper_ready=0;dns_helper_addr=(struct sockaddr_in){0};__atomic_store_n(&dns_helper_state,0,__ATOMIC_RELEASE);}if(__atomic_load_n(&dns_helper_state,__ATOMIC_ACQUIRE)==1)return 0;if(__atomic_load_n(&dns_helper_state,__ATOMIC_ACQUIRE)<0)return -EIO;if(!dns_resolver_symbols())return -ENOSYS;
#if defined(__x86_64__) && defined(MATON_GPU_BRIDGE)
    int expected=0;if(__atomic_compare_exchange_n(&dns_helper_state,&expected,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){unsigned long thread=0;int r=dns_pthread_create_android(&thread,dns_worker,NULL);if(r){__atomic_store_n(&dns_helper_state,-1,__ATOMIC_RELEASE);return -r;}}
    for(unsigned i=0;i<10000000;i++){int s=__atomic_load_n(&dns_helper_state,__ATOMIC_ACQUIRE);if(s==1)return 0;if(s<0)return -EIO;__asm__ volatile("pause");}return -ETIMEDOUT;
#else
    return -ENOSYS;
#endif
}
static long dns_poll(void *ptr,unsigned long count,struct timespec *timeout,const void *sigmask,unsigned long sigsize){return RAW(PROXY_SC_ppoll,ptr,count,timeout,sigmask,sigsize,0);}
static long dns_name(void *out,unsigned *len,const struct sockaddr_in *name){if(!out||!len)return -EFAULT;unsigned n=*len;if(n>sizeof(*name))n=sizeof(*name);p_copy(out,name,n);*len=sizeof(*name);return 0;}
static void dns_record_local(int fd,struct dns_socket *s){unsigned n=sizeof(s->local);if(!RAW(PROXY_SC_getsockname,fd,&s->local,&n,0,0,0))s->local_port=s->local.sin_port;}
static long dns_activate(int fd,struct dns_socket *s){if(s->active)return 0;struct sockaddr_in local={0};unsigned n=sizeof(local);long r=RAW(PROXY_SC_getsockname,fd,&local,&n,0,0,0);if(r<0||!local.sin_port){struct sockaddr_in bindaddr={.sin_family=AF_INET,.sin_addr={.s_addr=0x0100007f}};r=RAW(PROXY_SC_bind,fd,&bindaddr,sizeof(bindaddr),0,0,0);if(r<0)return r;}dns_record_local(fd,s);s->active=1;dns_track_port(s->local_port);return 0;}
static long dns_call(long nr,proxy_arg_t a[6]){
    if(nr==PROXY_SC_socket){long r=RAW(nr,a[0],a[1],a[2],0,0,0);if(r>=0&&a[0]==AF_INET){int type=(int)a[1]&0xf;if(type==SOCK_DGRAM||type==SOCK_STREAM){struct dns_socket *s=vfs_palloc(sizeof(*s));struct dns_fd *d=vfs_palloc(sizeof(*d));if(!s||!d){vfs_free(s);vfs_free(d);RAW(PROXY_SC_close,r,0,0,0,0,0);return -ENOMEM;}*s=(struct dns_socket){.refs=1,.type=type};proxy_sigmask_t m=dns_lock();*d=(struct dns_fd){(int)r,s,dns_fds};dns_fds=d;dns_unlock(m);}}return r;}
    proxy_sigmask_t lock=dns_lock();struct dns_socket *s=dns_lookup((int)a[0]);int tracked=s!=NULL;int active=s&&s->active;int connected=s&&s->connected;struct sockaddr_in local=s?s->local:(struct sockaddr_in){0};dns_unlock(lock);
    int target=0;if(nr==PROXY_SC_connect)target=dns_address((void *)(uintptr_t)a[1],a[2]);if(nr==PROXY_SC_sendto)target=dns_address((void *)(uintptr_t)a[4],a[5]);if(nr==PROXY_SC_sendmsg&&a[1]){struct proxy_message m=arch_message_read((void *)(uintptr_t)a[1]);target=dns_address((void *)(uintptr_t)m.name,m.namelen);}if(nr==PROXY_SC_sendmmsg&&a[1]&&a[2]){unsigned count=(unsigned)a[2];if(count>1024)count=1024;for(unsigned i=0;i<count&&!target;i++){struct proxy_message m=arch_mmsg_read((void *)(uintptr_t)a[1],i);target=dns_address((void *)(uintptr_t)m.name,m.namelen);}}
    if(target){if(nr==PROXY_SC_connect&&tracked&&s->type==SOCK_STREAM)return -ECONNREFUSED;if(!tracked)return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);if(dns_start()<0)return -EIO;
        if(nr==PROXY_SC_connect){long r=RAW(nr,a[0],&dns_helper_addr,sizeof(dns_helper_addr),0,0,0);if(r<0)return r;lock=dns_lock();s=dns_lookup((int)a[0]);if(s){s->active=1;s->connected=1;dns_record_local((int)a[0],s);dns_track_port(s->local_port);}dns_unlock(lock);return 0;}
        if(nr==PROXY_SC_sendto){struct sockaddr_in loop=dns_helper_addr;long ar=dns_activate((int)a[0],s);if(ar<0)return ar;return RAW(nr,a[0],a[1],a[2],a[3],&loop,sizeof(loop));}
        if(nr==PROXY_SC_sendmsg&&a[1]){struct proxy_message m=arch_message_read((void *)(uintptr_t)a[1]);struct proxy_message x=m;x.name=(uintptr_t)&dns_helper_addr;x.namelen=sizeof(dns_helper_addr);long ar=dns_activate((int)a[0],s);if(ar<0)return ar;return arch_message_call(nr,a[0],&x,a[2]);}
        if(nr==PROXY_SC_sendmmsg&&a[1]&&a[2]){unsigned count=(unsigned)a[2];if(count>1024)count=1024;size_t size=arch_mmsg_size(count);void *copy=vfs_alloc(size);if(!copy)return -ENOMEM;p_copy(copy,(void *)(uintptr_t)a[1],size);long ar=dns_activate((int)a[0],s);if(ar<0){vfs_free(copy);return ar;}for(unsigned i=0;i<count;i++){struct proxy_message m=arch_mmsg_read(copy,i);if(m.name&&dns_address((void *)(uintptr_t)m.name,m.namelen)){struct arch_mmsghdr *h=copy;h[i].msg_hdr.msg_name=&dns_helper_addr;h[i].msg_hdr.msg_namelen=sizeof(dns_helper_addr);}}long r=RAW(nr,a[0],copy,count,a[3],0,0);if(r>0)for(long i=0;i<r;i++)arch_mmsg_result((void *)(uintptr_t)a[1],copy,(unsigned)i);vfs_free(copy);return r;}}
    if(active&&nr==PROXY_SC_connect)return -ECONNREFUSED;
    if(active&&nr==PROXY_SC_getpeername)return connected?dns_name((void *)(uintptr_t)a[1],(unsigned *)(uintptr_t)a[2],&dns_endpoint):-ENOTCONN;
    if(active&&nr==PROXY_SC_getsockname)return dns_name((void *)(uintptr_t)a[1],(unsigned *)(uintptr_t)a[2],&local);
    if(active&&nr==PROXY_SC_recvfrom){long r=RAW(nr,a[0],a[1],a[2],a[3],0,0);if(r>=0&&a[4])dns_name((void *)(uintptr_t)a[4],(unsigned *)(uintptr_t)a[5],&dns_endpoint);return r;}
    if(active&&nr==PROXY_SC_recvmsg&&a[1]){void *out=(void *)(uintptr_t)a[1];struct proxy_message m=arch_message_read(out);void *name=(void *)(uintptr_t)m.name;unsigned namelen=m.namelen;m.name=0;m.namelen=0;long r=arch_message_call(nr,a[0],&m,a[2]);if(r>=0&&name){dns_name(name,&namelen,&dns_endpoint);m.namelen=namelen;arch_message_results(out,m);}return r;}
    (void)dns_port_owned;(void)dns_track_port;(void)dns_untrack_port;
    return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
}
