/* A virtual NEWNET has no kernel namespace. Never send its route requests to
 * the real network. This deliberately implements only bwrap's lo setup. */
#define _GNU_SOURCE
#include "proxy.h"
#include "vfs.h"
#include "net.h"
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <linux/sockios.h>
struct reply { struct reply *next; struct nlmsghdr header; struct nlmsgerr error; };
/* uevent: Android denies apps NETLINK_KOBJECT_UEVENT, which makes libudev's
 * monitor (and SDL's haptic/joystick init through it) fail. The fake socket
 * accepts libudev's bind/setsockopt and never delivers an event: no hotplug. */
struct endpoint { unsigned refs; unsigned port; int route, uevent; int emulated, kernel_fd, peer_fd, app_fd, socket_type; int domain, protocol; struct reply *first, *last; };
/* close/dup2 are not always trapped, so an fd number may since name another
 * file: ino pins the eventfd it was created as. */
struct descriptor { int fd; uint64_t ino; struct endpoint *ep; struct descriptor *next; };
static struct descriptor *fds;
static short virtual_lo_flags=IFF_LOOPBACK|IFF_UP|IFF_RUNNING;
static uint64_t inode(int fd) { struct proxy_stat st; return arch_stat_fd(fd,&st)?0:st.st_ino; }
void net_close(int fd);
static struct endpoint *lookup(int fd) {
    for(struct descriptor *p=fds;p;p=p->next)if(p->fd==fd){
        if(p->ino && inode(fd)==p->ino)return p->ep;
        proxy_log("stale net-fd",fd,p->ino);
        net_close(fd);return NULL;
    }
    return NULL;
}
void net_close(int fd) {
    struct descriptor **p=&fds;
    while(*p){struct descriptor *d=*p;if(d->fd!=fd){p=&d->next;continue;}
        *p=d->next;struct endpoint *e=d->ep;vfs_free(d);
        if(e->emulated&&e->app_fd==fd&&e->refs>1){e->app_fd=-1;for(struct descriptor *alias=fds;alias;alias=alias->next)if(alias->ep==e){e->app_fd=alias->fd;break;}if(e->app_fd<0)proxy_log("fail route-app-alias",fd,e->refs);}
        if(!--e->refs){while(e->first){struct reply *r=e->first;e->first=r->next;vfs_free(r);}
            if(e->emulated){int private_fd[2]={e->kernel_fd,e->peer_fd};for(unsigned i=0;i<2;i++)if(private_fd[i]>=0){if(!proxy_private_fd_release(private_fd[i]))RAW(PROXY_SC_close,private_fd[i],0,0,0,0,0);}}
            vfs_free(e);}return;
    }
}

/* Real-network NETLINK_ROUTE sockets need to bind under Android's app policy,
 * but untrusted apps cannot bind netlink_route_socket. Replace only the
 * autobind form with a pollable Unix datagram endpoint and keep a connected
 * kernel route socket behind it. */
static long get_sockopt(int fd,int level,int option,void *value,unsigned *len){return RAW(PROXY_SC_getsockopt,fd,level,option,value,len,0);}
static int privateize(long *fd){if(*fd<0)return (int)*fd;long p=proxy_private_fd((int)*fd);if(p<0){proxy_log("fail route-private-fd",*fd,p);return (int)p;}*fd=p;return 0;}
static long raw_route_socket(void){
    long fd=RAW(PROXY_SC_socket,AF_NETLINK,SOCK_RAW,NETLINK_ROUTE,0,0,0);if(fd<0){proxy_log("fail route-socket",NETLINK_ROUTE,fd);return fd;}
    struct sockaddr_nl kernel={.nl_family=AF_NETLINK};long r=RAW(PROXY_SC_connect,fd,&kernel,sizeof(kernel),0,0,0);
    if(r<0){proxy_log("fail route-autobind-connect",fd,r);RAW(PROXY_SC_close,fd,0,0,0,0,0);return r;}
    return fd;
}
static long kernel_port(int fd,unsigned *port){struct sockaddr_nl a={0};unsigned n=sizeof(a);long r=RAW(PROXY_SC_getsockname,fd,&a,&n,0,0,0);if(r<0)return r;if(n<sizeof(a)||a.nl_family!=AF_NETLINK)return -EPROTO;*port=a.nl_pid;return 0;}
static long emulate_route_bind(int appfd,const struct sockaddr_nl *addr,unsigned addrlen){
    if(addrlen<sizeof(*addr)||addr->nl_family!=AF_NETLINK)return 0;
    if(addr->nl_pid||addr->nl_groups)return 0;
    unsigned domain=0,protocol=0,n=sizeof(unsigned);long r=get_sockopt(appfd,SOL_SOCKET,SO_DOMAIN,&domain,&n);if(r<0){if(r!=-ENOTSOCK&&r!=-EBADF)proxy_log("fail route-domain",appfd,r);return 0;}
    n=sizeof(unsigned);r=get_sockopt(appfd,SOL_SOCKET,SO_PROTOCOL,&protocol,&n);if(r<0){proxy_log("fail route-protocol",appfd,r);return r;}
    if(domain!=AF_NETLINK||protocol!=NETLINK_ROUTE)return 0;
    long kernel=raw_route_socket();if(kernel<0)return kernel;
    unsigned port=0;r=kernel_port((int)kernel,&port);if(r<0){proxy_log("fail route-getsockname",kernel,r);RAW(PROXY_SC_close,kernel,0,0,0,0,0);return r;}
    unsigned type=0,type_len=sizeof(type);r=get_sockopt(appfd,SOL_SOCKET,SO_TYPE,&type,&type_len);if(r<0){proxy_log("fail route-socket-type",appfd,r);RAW(PROXY_SC_close,kernel,0,0,0,0,0);return r;}
    long fl=RAW(PROXY_SC_fcntl,appfd,F_GETFL,0,0,0,0),fdfl=RAW(PROXY_SC_fcntl,appfd,F_GETFD,0,0,0,0);
    if(fl<0||fdfl<0){r=fl<0?fl:fdfl;proxy_log("fail route-fd-flags",appfd,r);RAW(PROXY_SC_close,kernel,0,0,0,0,0);return r;}
    int pair[2]={-1,-1};struct endpoint *e=NULL;struct descriptor *d=NULL;int pairtype=SOCK_DGRAM|SOCK_NONBLOCK;
    r=RAW(PROXY_SC_socketpair,AF_UNIX,pairtype,0,pair,0,0);if(r<0){proxy_log("fail route-socketpair",appfd,r);RAW(PROXY_SC_close,kernel,0,0,0,0,0);return r;}
    r=RAW(PROXY_SC_fcntl,pair[0],F_SETFL,fl,0,0,0);if(r<0){proxy_log("fail route-app-status-flags",pair[0],r);goto fail_pair;}
    int snd=64*1024,rcv=64*1024;
    r=RAW(PROXY_SC_setsockopt,pair[1],SOL_SOCKET,SO_SNDBUF,&snd,sizeof(snd),0);if(r<0){proxy_log("fail route-pair-sndbuf",pair[1],r);goto fail_pair;}
    r=RAW(PROXY_SC_setsockopt,pair[0],SOL_SOCKET,SO_RCVBUF,&rcv,sizeof(rcv),0);if(r<0){proxy_log("fail route-pair-rcvbuf",pair[0],r);goto fail_pair;}
    e=vfs_palloc(sizeof(*e));d=vfs_palloc(sizeof(*d));if(!e||!d){vfs_free(e);vfs_free(d);e=NULL;d=NULL;r=-ENOMEM;goto fail_pair;}
    long hidden=kernel,peer=pair[1];kernel=-1;if((r=privateize(&hidden))<0)goto fail_pair;
    pair[1]=-1;if((r=privateize(&peer))<0){if(!proxy_private_fd_release((int)hidden))RAW(PROXY_SC_close,hidden,0,0,0,0,0);goto fail_pair;}
    r=RAW(PROXY_SC_dup3,pair[0],appfd,(fdfl&FD_CLOEXEC)?O_CLOEXEC:0,0,0,0);if(r<0){proxy_log("fail route-replace-fd",appfd,r);if(!proxy_private_fd_release((int)hidden))RAW(PROXY_SC_close,hidden,0,0,0,0,0);if(!proxy_private_fd_release((int)peer))RAW(PROXY_SC_close,peer,0,0,0,0,0);goto fail_pair;}
    RAW(PROXY_SC_close,pair[0],0,0,0,0,0);
    *e=(struct endpoint){.refs=1,.port=port,.route=1,.emulated=1,.kernel_fd=(int)hidden,.peer_fd=(int)peer,.app_fd=appfd,.socket_type=(int)type,.domain=AF_NETLINK,.protocol=NETLINK_ROUTE};
    *d=(struct descriptor){appfd,inode(appfd),e,fds};fds=d;
    /* SO_DOMAIN/PROTOCOL/TYPE are virtualized by net_call; store the original
     * values in the endpoint's route metadata (the type is fixed by socket()). */
    return 1;
fail_pair:
    if(e)vfs_free(e);
    if(d)vfs_free(d);
    if(pair[0]>=0)RAW(PROXY_SC_close,pair[0],0,0,0,0,0);
    if(pair[1]>=0)RAW(PROXY_SC_close,pair[1],0,0,0,0,0);
    if(kernel>=0)RAW(PROXY_SC_close,kernel,0,0,0,0,0);
    return r;
}

static int append_attr(unsigned char *buf,size_t cap,size_t *used,unsigned type,const void *value,size_t len){
    if(len>UINT16_MAX-sizeof(struct rtattr))return -EOVERFLOW;
    size_t raw=sizeof(struct rtattr)+len,aligned=RTA_ALIGN(raw);if(*used>cap||aligned>cap-*used)return -ENOSPC;
    struct rtattr *a=(void *)(buf+*used);a->rta_type=(unsigned short)type;a->rta_len=(unsigned short)raw;
    p_copy(RTA_DATA(a),(void *)value,len);if(aligned>raw)for(size_t i=raw;i<aligned;i++)buf[*used+i]=0;*used+=aligned;return 0;
}
static long queue_datagram(struct endpoint *e,const void *data,size_t len){
    long r=RAW(PROXY_SC_sendto,e->peer_fd,data,len,MSG_NOSIGNAL,0,0);
    while(r==-EAGAIN||r==-ENOBUFS){
        int snd_old=0,rcv_old=0;unsigned n=sizeof(snd_old);long g=get_sockopt(e->peer_fd,SOL_SOCKET,SO_SNDBUF,&snd_old,&n);if(g<0){proxy_log("fail route-sndbuf-read",e->peer_fd,g);return g;}
        n=sizeof(rcv_old);g=get_sockopt(e->app_fd,SOL_SOCKET,SO_RCVBUF,&rcv_old,&n);if(g<0){proxy_log("fail route-rcvbuf-read",e->app_fd,g);return g;}
        if(snd_old<=0||snd_old>INT_MAX/2||rcv_old<=0||rcv_old>INT_MAX/2){proxy_log("fail route-buffer-growth",snd_old,rcv_old);return r;}
        int snd_wanted=snd_old*2,rcv_wanted=rcv_old*2;long s=RAW(PROXY_SC_setsockopt,e->peer_fd,SOL_SOCKET,SO_SNDBUF,&snd_wanted,sizeof(snd_wanted),0);if(s<0){proxy_log("fail route-sndbuf-grow",e->peer_fd,s);return s;}
        s=RAW(PROXY_SC_setsockopt,e->app_fd,SOL_SOCKET,SO_RCVBUF,&rcv_wanted,sizeof(rcv_wanted),0);if(s<0){proxy_log("fail route-rcvbuf-grow",e->app_fd,s);return s;}
        int snd_new=0,rcv_new=0;n=sizeof(snd_new);g=get_sockopt(e->peer_fd,SOL_SOCKET,SO_SNDBUF,&snd_new,&n);if(g<0){proxy_log("fail route-sndbuf-verify",e->peer_fd,g);return g;}
        n=sizeof(rcv_new);g=get_sockopt(e->app_fd,SOL_SOCKET,SO_RCVBUF,&rcv_new,&n);if(g<0){proxy_log("fail route-rcvbuf-verify",e->app_fd,g);return g;}
        if(snd_new<=snd_old&&rcv_new<=rcv_old){proxy_log("fail route-buffer-no-growth",snd_old,rcv_old);return r;}r=RAW(PROXY_SC_sendto,e->peer_fd,data,len,MSG_NOSIGNAL,0,0);
    }
    if(r<0){proxy_log("fail route-queue-reply",e->peer_fd,r);return r;}if((size_t)r!=len){proxy_log("fail route-short-queue",r,(long)len);return -EIO;}return r;
}
static long interface_message(struct endpoint *e,int index,uint32_t seq){
    unsigned char buf[512]={0};struct nlmsghdr *h=(void *)buf;struct ifinfomsg *ifi=(void *)(buf+NLMSG_HDRLEN);
    h->nlmsg_type=RTM_NEWLINK;h->nlmsg_flags=NLM_F_MULTI;h->nlmsg_seq=seq;h->nlmsg_pid=e->port;h->nlmsg_len=NLMSG_LENGTH(sizeof(*ifi));
    ifi->ifi_family=AF_UNSPEC;ifi->ifi_index=index;
    struct {int fd;struct ifreq req;} q={.fd=-1};q.fd=RAW(PROXY_SC_socket,AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0,0,0,0);if(q.fd<0){proxy_log("fail route-iface-socket",index,q.fd);return q.fd;}
    q.req.ifr_ifindex=index;long r=RAW(PROXY_SC_ioctl,q.fd,SIOCGIFNAME,&q.req,0,0,0);if(r<0){proxy_log("fail route-iface-name",index,r);RAW(PROXY_SC_close,q.fd,0,0,0,0,0);return r;}
    ifi->ifi_index=index;
    r=RAW(PROXY_SC_ioctl,q.fd,SIOCGIFFLAGS,&q.req,0,0,0);if(r<0){proxy_log("fail route-iface-flags",index,r);RAW(PROXY_SC_close,q.fd,0,0,0,0,0);return r;}ifi->ifi_flags=(unsigned)q.req.ifr_flags;ifi->ifi_change=0;
    size_t used=NLMSG_ALIGN(h->nlmsg_len);r=append_attr(buf,sizeof(buf),&used,IFLA_IFNAME,q.req.ifr_name,p_len(q.req.ifr_name)+1);if(r<0){RAW(PROXY_SC_close,q.fd,0,0,0,0,0);return r;}
    r=RAW(PROXY_SC_ioctl,q.fd,SIOCGIFMTU,&q.req,0,0,0);if(r==0){uint32_t mtu=(uint32_t)q.req.ifr_mtu;r=append_attr(buf,sizeof(buf),&used,IFLA_MTU,&mtu,sizeof(mtu));if(r<0){RAW(PROXY_SC_close,q.fd,0,0,0,0,0);return r;}}
    else if(r!=-ENOTTY && r!=-EINVAL && r!=-EOPNOTSUPP)proxy_log("fail route-iface-mtu",index,r);
    RAW(PROXY_SC_close,q.fd,0,0,0,0,0);h->nlmsg_len=(uint32_t)used;return queue_datagram(e,buf,used);
}
static long link_indexes(int **indexes,size_t *count){
    *indexes=NULL;*count=0;long fd=raw_route_socket();if(fd<0)return fd;
    struct {struct nlmsghdr h;struct ifaddrmsg a;} req={.h={.nlmsg_len=NLMSG_LENGTH(sizeof(struct ifaddrmsg)),.nlmsg_type=RTM_GETADDR,.nlmsg_flags=NLM_F_REQUEST|NLM_F_DUMP,.nlmsg_seq=0x4d41544f},.a={.ifa_family=AF_UNSPEC}};
    long r=RAW(PROXY_SC_sendto,fd,&req,sizeof(req),MSG_NOSIGNAL,0,0);if(r<0){proxy_log("fail route-index-request",fd,r);RAW(PROXY_SC_close,fd,0,0,0,0,0);return r;}if(proxy_trace)proxy_log("route-index-request",fd,r);
    size_t cap=0;int *list=NULL;int done=0;
    while(!done){unsigned char small[4096];long n=RAW(PROXY_SC_recvfrom,fd,small,sizeof(small),MSG_PEEK|MSG_TRUNC,0,0);if(n<0){proxy_log("fail route-index-peek",fd,n);r=n;break;}if(proxy_trace)proxy_log("route-index-peek",fd,n);
        if((unsigned long)n>SIZE_MAX){r=-EOVERFLOW;break;}size_t bytes=(size_t)n;unsigned char *buf=vfs_alloc(bytes?bytes:1);if(!buf){r=-ENOMEM;break;}n=RAW(PROXY_SC_recvfrom,fd,buf,bytes,0,0,0);if(n<0){proxy_log("fail route-index-recv",fd,n);vfs_free(buf);r=n;break;}if((size_t)n!=bytes){proxy_log("fail route-index-short",n,(long)bytes);vfs_free(buf);r=-EIO;break;}
        size_t off=0;while(off+sizeof(struct nlmsghdr)<=bytes){struct nlmsghdr *h=(void *)(buf+off);if(h->nlmsg_len<sizeof(*h)||h->nlmsg_len>bytes-off){proxy_log("fail route-index-message",fd,h->nlmsg_len);r=-EPROTO;break;}if(h->nlmsg_type==NLMSG_DONE){done=1;break;}if(h->nlmsg_type==NLMSG_ERROR){struct nlmsgerr *er=NLMSG_DATA(h);if(h->nlmsg_len>=NLMSG_LENGTH(sizeof(*er))&&er->error){r=er->error;done=1;break;}}
            if(h->nlmsg_type==RTM_NEWADDR&&h->nlmsg_len>=NLMSG_LENGTH(sizeof(struct ifaddrmsg))){int idx=((struct ifaddrmsg *)NLMSG_DATA(h))->ifa_index;int found=0;for(size_t i=0;i<*count;i++)if(list[i]==idx){found=1;break;}if(idx>0&&!found){if(*count==cap){size_t next=cap?cap*2:8;if(next<cap||next>SIZE_MAX/sizeof(int)){r=-EOVERFLOW;break;}int *p=vfs_alloc(next*sizeof(int));if(!p){r=-ENOMEM;break;}if(*count)p_copy(p,list,*count*sizeof(int));vfs_free(list);list=p;cap=next;}list[(*count)++]=idx;}}
            size_t step=NLMSG_ALIGN(h->nlmsg_len);off+=step>bytes-off?h->nlmsg_len:step;
        }vfs_free(buf);if(r<0)break;
    }
    RAW(PROXY_SC_close,fd,0,0,0,0,0);if(r<0){vfs_free(list);*count=0;return r;}*indexes=list;return 0;
}
static long synth_getlink(struct endpoint *e,const struct nlmsghdr *request){
    if(request->nlmsg_len<NLMSG_LENGTH(sizeof(uint32_t)))return -EINVAL;
    struct ifinfomsg ifi={0};size_t payload=request->nlmsg_len-NLMSG_HDRLEN;
    if(payload>=sizeof(ifi))p_copy(&ifi,NLMSG_DATA(request),sizeof(ifi));
    else {unsigned char family=0;p_copy(&family,NLMSG_DATA(request),1);ifi.ifi_family=family;}
    int *indexes=NULL;size_t count=0;long r;
    if((request->nlmsg_flags&NLM_F_DUMP)||ifi.ifi_index==0){r=link_indexes(&indexes,&count);if(r<0)return r;}
    else {indexes=vfs_alloc(sizeof(int));if(!indexes)return -ENOMEM;indexes[0]=ifi.ifi_index;count=1;}
    r=0;for(size_t i=0;i<count;i++){r=interface_message(e,indexes[i],request->nlmsg_seq);if(r<0)break;}
    vfs_free(indexes);if(r<0)return r;
    struct {struct nlmsghdr h;int status;} done={.h={.nlmsg_len=NLMSG_LENGTH(sizeof(int)),.nlmsg_type=NLMSG_DONE,.nlmsg_flags=NLM_F_MULTI,.nlmsg_seq=request->nlmsg_seq,.nlmsg_pid=e->port},.status=0};return queue_datagram(e,&done,sizeof(done));
}
static long forward_request(struct endpoint *e,const void *data,size_t len){
    long r=RAW(PROXY_SC_sendto,e->kernel_fd,data,len,MSG_NOSIGNAL,0,0);if(r<0){proxy_log("fail route-forward-send",e->kernel_fd,r);return r;}if((size_t)r!=len){proxy_log("fail route-forward-short",r,(long)len);return -EIO;}
    int dump=0,ack=0;uint32_t seq=0;if(len>=sizeof(struct nlmsghdr)){const struct nlmsghdr *h=data;dump=!!(h->nlmsg_flags&NLM_F_DUMP);ack=!!(h->nlmsg_flags&NLM_F_ACK);seq=h->nlmsg_seq;}
    int done=0;while(!done){unsigned char peek[4096];long n=RAW(PROXY_SC_recvfrom,e->kernel_fd,peek,sizeof(peek),MSG_PEEK|MSG_TRUNC,0,0);if(n<0){proxy_log("fail route-forward-peek",e->kernel_fd,n);return n;}
        if((unsigned long)n>SIZE_MAX)return -EOVERFLOW;
        if(!n){proxy_log("fail route-forward-empty",e->kernel_fd,seq);return -EPROTO;}
        size_t size=(size_t)n;unsigned char *buf=vfs_alloc(size);if(!buf)return -ENOMEM;n=RAW(PROXY_SC_recvfrom,e->kernel_fd,buf,size,0,0,0);if(n<0){proxy_log("fail route-forward-recv",e->kernel_fd,n);vfs_free(buf);return n;}if((size_t)n!=size){proxy_log("fail route-forward-short-recv",n,(long)size);vfs_free(buf);return -EIO;}
        r=queue_datagram(e,buf,size);if(r<0){vfs_free(buf);return r;}size_t off=0;while(off<size){if(size-off<sizeof(struct nlmsghdr)){proxy_log("fail route-forward-header",e->kernel_fd,(long)(size-off));vfs_free(buf);return -EPROTO;}struct nlmsghdr *h=(void *)(buf+off);if(h->nlmsg_len<sizeof(*h)||h->nlmsg_len>size-off){proxy_log("fail route-forward-message",e->kernel_fd,h->nlmsg_len);vfs_free(buf);return -EPROTO;}if(h->nlmsg_seq==seq){if(h->nlmsg_type==NLMSG_DONE||h->nlmsg_type==NLMSG_ERROR)done=1;else if(!dump&&!ack)done=1;}size_t step=NLMSG_ALIGN(h->nlmsg_len);off+=step>size-off?h->nlmsg_len:step;}vfs_free(buf);if(!dump&&!ack)done=1;
    }return (long)len;
}
static long emulated_send(struct endpoint *e,const void *buf,size_t len){
    if(!buf||len<sizeof(struct nlmsghdr))return -EINVAL;
    size_t off=0;
    while(off<len){if(len-off<sizeof(struct nlmsghdr))return -EINVAL;const struct nlmsghdr *h=(const void *)((const char *)buf+off);if(h->nlmsg_len<sizeof(*h)||h->nlmsg_len>len-off){proxy_log("fail route-request-header",(long)len,(long)off);return -EINVAL;}if(proxy_trace)proxy_log("route-request",h->nlmsg_type,h->nlmsg_len);size_t step=NLMSG_ALIGN(h->nlmsg_len);if(h->nlmsg_type==RTM_GETLINK){long r=synth_getlink(e,h);if(r<0){proxy_log("fail route-getlink",h->nlmsg_len,r);return r;}}else {long r=forward_request(e,h,h->nlmsg_len);if(r<0)return r;}off+=step>len-off?h->nlmsg_len:step;}
    return (long)len;
}
void net_dup(int from,int to) {
    if(from==to)return;
    struct endpoint *e=lookup(from);net_close(to);if(!e)return;
    struct descriptor *d=vfs_palloc(sizeof(*d));if(!d){proxy_log("fail net-dup-state",from,to);return;}
    *d=(struct descriptor){to,inode(to),e,fds};fds=d;e->refs++;
}
static long address(void *out, unsigned *len, unsigned port) {
    if(!len)return -EFAULT;
    struct sockaddr_nl addr={.nl_family=AF_NETLINK,.nl_pid=port};
    unsigned n=*len;if(n>sizeof(addr))n=sizeof(addr);
    if(n && !out)return -EFAULT;
    p_copy(out,&addr,n);*len=sizeof(addr);return 0;
}
static int request_error(struct nlmsghdr *h) {
    if(!(h->nlmsg_flags&NLM_F_REQUEST))return -EINVAL;
    if(h->nlmsg_type==RTM_NEWADDR && h->nlmsg_len>=NLMSG_LENGTH(sizeof(struct ifaddrmsg))){
        struct ifaddrmsg *a=NLMSG_DATA(h);
        if(a->ifa_index!=1 || a->ifa_family!=AF_INET || a->ifa_prefixlen!=8)return -EOPNOTSUPP;
        unsigned len=h->nlmsg_len-NLMSG_LENGTH(sizeof(*a));struct rtattr *r=IFA_RTA(a);unsigned found=0;
        for(;RTA_OK(r,len);r=RTA_NEXT(r,len)){
            if(r->rta_type!=IFA_LOCAL && r->rta_type!=IFA_ADDRESS)return -EOPNOTSUPP;
            if(RTA_PAYLOAD(r)!=4)return -EINVAL;
            unsigned char *ip=RTA_DATA(r);if(ip[0]!=127 || ip[1] || ip[2] || ip[3]!=1)return -EOPNOTSUPP;
            found|=r->rta_type==IFA_LOCAL?1:2;
        }
        return len || found!=3?-EINVAL:0;
    }
    if(h->nlmsg_type==RTM_NEWLINK && h->nlmsg_len==NLMSG_LENGTH(sizeof(struct ifinfomsg))){
        struct ifinfomsg *i=NLMSG_DATA(h);
        return i->ifi_index==1 && i->ifi_change==IFF_UP && i->ifi_flags==IFF_UP?0:-EOPNOTSUPP;
    }
    return -EOPNOTSUPP;
}
static long send_request(int fd,struct endpoint *e,void *buf,size_t len,void *addr,unsigned addrlen) {
    if(addr){struct sockaddr_nl *a=addr;if(addrlen<sizeof(*a) || a->nl_family!=AF_NETLINK)return -EINVAL;if(a->nl_pid || a->nl_groups)return -EOPNOTSUPP;}
    if(!buf || len<sizeof(struct nlmsghdr) || len>UINT_MAX)return -EINVAL;
    /* Validate the whole datagram before enqueueing anything. */
    size_t off=0;
    while(off<len){if(len-off<sizeof(struct nlmsghdr))return -EINVAL;struct nlmsghdr *h=(void *)((char *)buf+off);
        if(h->nlmsg_len<sizeof(*h) || h->nlmsg_len>len-off)return -EINVAL;
        size_t n=NLMSG_ALIGN((size_t)h->nlmsg_len);off+=n>len-off?h->nlmsg_len:n;
    }
    for(off=0;off<len;){struct nlmsghdr *h=(void *)((char *)buf+off);struct reply *r=vfs_palloc(sizeof(*r));if(!r)return -ENOMEM;
        int error=request_error(h);*r=(struct reply){.header={.nlmsg_len=NLMSG_LENGTH(sizeof(struct nlmsgerr)),.nlmsg_type=NLMSG_ERROR,.nlmsg_flags=NLM_F_CAPPED,.nlmsg_seq=h->nlmsg_seq,.nlmsg_pid=e->port},.error={.error=error,.msg=*h}};
        if(e->last)e->last->next=r;else e->first=r;e->last=r;
        uint64_t one=1;long written=proxy_internal_write(fd,&one,sizeof(one));if(written<0)return written;
        if(proxy_trace)proxy_log("virtual-netlink",h->nlmsg_type,error);
        size_t n=NLMSG_ALIGN((size_t)h->nlmsg_len);off+=n>len-off?h->nlmsg_len:n;
    }
    return (long)len;
}
static long receive(int fd,struct endpoint *e,const void *iov,size_t count,int flags,int *msgflags) {
    struct reply *r=e->first;if(!r)return -EAGAIN;
    size_t total=0,used=0;char *data=(char *)&r->header;size_t len=r->header.nlmsg_len;
    for(size_t k=0;k<count;k++){if(arch_iovec_read(iov,k).len>SIZE_MAX-total)return -EINVAL;total+=arch_iovec_read(iov,k).len;size_t n=len-used;if(n>arch_iovec_read(iov,k).len)n=arch_iovec_read(iov,k).len;p_copy((void *)(uintptr_t)arch_iovec_read(iov,k).base,data+used,n);used+=n;}
    if(msgflags)*msgflags=total<len?MSG_TRUNC:0;
    if(!(flags&MSG_PEEK)){uint64_t one;long n=RAW(PROXY_SC_read,fd,&one,sizeof(one),0,0,0);if(n<0)return n;e->first=r->next;if(!e->first)e->last=NULL;vfs_free(r);}
    return flags&MSG_TRUNC?(long)len:(long)used;
}
static int interface_ioctl(unsigned request) {
    switch(request){
    case SIOCGIFNAME:case SIOCGIFCONF:case SIOCGIFFLAGS:case SIOCSIFFLAGS:
    case SIOCGIFADDR:case SIOCSIFADDR:case SIOCGIFDSTADDR:case SIOCSIFDSTADDR:
    case SIOCGIFBRDADDR:case SIOCSIFBRDADDR:case SIOCGIFNETMASK:case SIOCSIFNETMASK:
    case SIOCGIFMETRIC:case SIOCSIFMETRIC:case SIOCGIFMEM:case SIOCSIFMEM:
    case SIOCGIFMTU:case SIOCSIFMTU:case SIOCGIFHWADDR:case SIOCSIFHWADDR:
    case SIOCGIFINDEX:case SIOCSIFPFLAGS:case SIOCGIFPFLAGS:case SIOCGIFTXQLEN:case SIOCSIFTXQLEN:
        return 1;
    default:return 0;
    }
}
long net_call(long nr,proxy_arg_t *a,int newnet,int *handled) {
    *handled=0;
    if(nr==PROXY_SC_bind){
        struct sockaddr_nl candidate={0};if(a[1]&&a[2]>=(long)sizeof(candidate))p_copy(&candidate,(void *)(uintptr_t)a[1],sizeof(candidate));
        if(candidate.nl_family==AF_NETLINK&&!candidate.nl_pid&&!candidate.nl_groups){
            long emulated=emulate_route_bind((int)a[0],&candidate,(unsigned)a[2]);
            if(emulated){*handled=1;return emulated==1?0:emulated;}
        }
    }
    if(nr==PROXY_SC_ioctl && newnet && interface_ioctl((unsigned)a[1])){
        unsigned request=(unsigned)a[1];
        *handled=1;
        if(request!=SIOCGIFINDEX && request!=SIOCGIFFLAGS && request!=SIOCSIFFLAGS &&
           request!=SIOCGIFNAME && request!=SIOCGIFCONF)return -EOPNOTSUPP;
        if(!a[2])return -EFAULT;
        if(request==SIOCGIFCONF){
                uintptr_t cp=(uintptr_t)a[2], buffer=0;int length=0;
                p_copy(&length,(void *)cp,sizeof(length));
                if(length<0)return -EINVAL;
                if(cp<=UINT32_MAX){uint32_t ptr=0;p_copy(&ptr,(void *)(cp+4),sizeof(ptr));buffer=ptr;}
                else p_copy(&buffer,(void *)(cp+8),sizeof(buffer));
                unsigned row_size=cp<=UINT32_MAX?32u:40u;
                if(length<(int)row_size){p_copy((void *)cp,&(int){0},sizeof(int));return 0;}
                if(!buffer)return -EFAULT;
                unsigned char row[40]={0};p_copy(row,"lo",3);
                unsigned short family=AF_INET;p_copy(row+16,&family,sizeof(family));
                uint32_t loopback=htonl(INADDR_LOOPBACK);p_copy(row+20,&loopback,sizeof(loopback));
                p_copy((void *)buffer,row,row_size);p_copy((void *)cp,&row_size,sizeof(int));return 0;
        }
        struct ifreq *r=(void *)(uintptr_t)a[2];
        if(request==SIOCGIFNAME){if(r->ifr_ifindex!=1)return -ENODEV;p_copy(r->ifr_name,"lo",3);return 0;}
        if(r->ifr_name[0]!='l' || r->ifr_name[1]!='o' || r->ifr_name[2])return -ENODEV;
        if(request==SIOCGIFINDEX){r->ifr_ifindex=1;return 0;}
        if(request==SIOCGIFFLAGS){r->ifr_flags=virtual_lo_flags;return 0;}
        if((r->ifr_flags&~(IFF_LOOPBACK|IFF_UP|IFF_RUNNING)) || !(r->ifr_flags&IFF_LOOPBACK))return -EINVAL;
        virtual_lo_flags=(short)(IFF_LOOPBACK|(r->ifr_flags&IFF_UP)|(r->ifr_flags&IFF_UP?IFF_RUNNING:0));return 0;
    }
    if(nr==PROXY_SC_socket && newnet && ((a[0]==AF_NETLINK && a[2]==NETLINK_ROUTE) || (a[0]==AF_INET && (a[1]&~(SOCK_CLOEXEC|SOCK_NONBLOCK))==SOCK_DGRAM && !a[2]))){
        *handled=1;if((a[1]&~(SOCK_CLOEXEC|SOCK_NONBLOCK))!=SOCK_RAW && (a[1]&~(SOCK_CLOEXEC|SOCK_NONBLOCK))!=SOCK_DGRAM)return -EPROTOTYPE;
        struct endpoint *e=vfs_palloc(sizeof(*e));struct descriptor *d=vfs_palloc(sizeof(*d));if(!e || !d){vfs_free(e);vfs_free(d);return -ENOMEM;}
        long fd=RAW(PROXY_SC_eventfd2,0,EFD_SEMAPHORE|EFD_NONBLOCK|((a[1]&SOCK_CLOEXEC)?EFD_CLOEXEC:0),0,0,0,0);
        if(fd<0){vfs_free(e);vfs_free(d);return fd;}
        *e=(struct endpoint){.refs=1,.route=a[0]==AF_NETLINK,.port=(unsigned)RAW(PROXY_SC_getpid,0,0,0,0,0,0)};*d=(struct descriptor){(int)fd,inode((int)fd),e,fds};fds=d;return fd;
    }
    if(nr==PROXY_SC_socket && a[0]==AF_NETLINK && a[2]==NETLINK_KOBJECT_UEVENT){
        *handled=1;long type=a[1]&~(SOCK_CLOEXEC|SOCK_NONBLOCK);if(type!=SOCK_RAW && type!=SOCK_DGRAM)return -EPROTOTYPE;
        struct endpoint *e=vfs_palloc(sizeof(*e));struct descriptor *d=vfs_palloc(sizeof(*d));if(!e || !d){vfs_free(e);vfs_free(d);return -ENOMEM;}
        long fd=RAW(PROXY_SC_eventfd2,0,EFD_NONBLOCK|((a[1]&SOCK_CLOEXEC)?EFD_CLOEXEC:0),0,0,0,0);
        if(fd<0){vfs_free(e);vfs_free(d);return fd;}
        *e=(struct endpoint){.refs=1,.uevent=1,.port=(unsigned)RAW(PROXY_SC_getpid,0,0,0,0,0,0)};*d=(struct descriptor){(int)fd,inode((int)fd),e,fds};fds=d;
        if(proxy_trace)proxy_log("virtual-uevent-socket",fd,0);
        return fd;
    }
    struct endpoint *e=lookup((int)a[0]);if(!e)return 0;
    if(e->emulated){
        switch(nr){
        case PROXY_SC_getsockname:*handled=1;if(proxy_trace)proxy_log("route-getname",a[0],e->port);return address((void *)(uintptr_t)a[1],(void *)(uintptr_t)a[2],e->port);
        case PROXY_SC_getsockopt:{
            *handled=1;if(!a[3]||!a[4])return -EFAULT;int level=(int)a[1],option=(int)a[2];unsigned need=sizeof(int);int value=0;
            if(level==SOL_SOCKET&&option==SO_DOMAIN)value=e->domain;
            else if(level==SOL_SOCKET&&option==SO_TYPE)value=e->socket_type;
            else if(level==SOL_SOCKET&&option==SO_PROTOCOL)value=e->protocol;
            else return -ENOPROTOOPT;
            unsigned supplied=0;p_copy(&supplied,(void *)(uintptr_t)a[4],sizeof(supplied));if(!supplied)return -EINVAL;unsigned copy=supplied<need?supplied:need;p_copy((void *)(uintptr_t)a[3],&value,copy);p_copy((void *)(uintptr_t)a[4],&need,sizeof(need));return 0;
        }
        case PROXY_SC_sendto:{
            *handled=1;if(a[4]){struct sockaddr_nl peer={0};if(a[5]<(long)sizeof(peer))return -EINVAL;p_copy(&peer,(void *)(uintptr_t)a[4],sizeof(peer));if(peer.nl_family!=AF_NETLINK||peer.nl_pid||peer.nl_groups)return -EDESTADDRREQ;}
            return emulated_send(e,(void *)(uintptr_t)a[1],(size_t)a[2]);
        }
        case PROXY_SC_sendmsg:{
            *handled=1;if(!a[1])return -EFAULT;struct proxy_message m=arch_message_read((void *)(uintptr_t)a[1]);if(m.controllen)return -EINVAL;
            size_t size=0;for(size_t k=0;k<m.iovlen;k++){struct proxy_iovec v=arch_iovec_read((void *)(uintptr_t)m.iov,k);if(v.len>SIZE_MAX-size)return -EMSGSIZE;size+=v.len;}
            char *buf=vfs_alloc(size?size:1);if(!buf)return -ENOMEM;size_t off=0;for(size_t k=0;k<m.iovlen;k++){struct proxy_iovec v=arch_iovec_read((void *)(uintptr_t)m.iov,k);p_copy(buf+off,(void *)(uintptr_t)v.base,v.len);off+=v.len;}
            if(m.name){struct sockaddr_nl peer={0};if(m.namelen<sizeof(peer)){vfs_free(buf);return -EINVAL;}p_copy(&peer,(void *)(uintptr_t)m.name,sizeof(peer));if(peer.nl_family!=AF_NETLINK||peer.nl_pid||peer.nl_groups){vfs_free(buf);return -EDESTADDRREQ;}}
            long result=emulated_send(e,buf,size);vfs_free(buf);return result;
        }
        case PROXY_SC_write:*handled=1;return emulated_send(e,(void *)(uintptr_t)a[1],(size_t)a[2]);
        case PROXY_SC_read:*handled=1;return RAW(PROXY_SC_read,a[0],a[1],a[2],0,0,0);
        case PROXY_SC_readv:*handled=1;return RAW(PROXY_SC_readv,a[0],a[1],a[2],0,0,0);
        case PROXY_SC_recvfrom:{
            *handled=1;long result=RAW(PROXY_SC_recvfrom,a[0],a[1],a[2],a[3],0,0);if(result<0)return result;if(a[4]){long r=address((void *)(uintptr_t)a[4],(void *)(uintptr_t)a[5],0);if(r<0)return r;}return result;
        }
        case PROXY_SC_recvmsg:{
            *handled=1;if(!a[1])return -EFAULT;struct proxy_message m=arch_message_read((void *)(uintptr_t)a[1]);struct sockaddr_nl source={.nl_family=AF_NETLINK};
            uint64_t original=m.name;if(original)m.name=(uintptr_t)&source;long result=arch_message_call(PROXY_SC_recvmsg,(int)a[0],&m,a[2]);
            if(result>=0&&original){p_copy((void *)(uintptr_t)original,&source,sizeof(source));m.namelen=sizeof(source);}
            if(result>=0){if(proxy_trace&&result>=(long)sizeof(struct nlmsghdr)){struct nlmsghdr h={0};p_copy(&h,(void *)(uintptr_t)arch_iovec_read((void *)(uintptr_t)m.iov,0).base,sizeof(h));proxy_log("route-app-recv",h.nlmsg_type,h.nlmsg_seq);proxy_log("route-app-pid",h.nlmsg_pid,e->port);}m.controllen=0;arch_message_results((void *)(uintptr_t)a[1],m);}return result;
        }
        case PROXY_SC_bind:*handled=1;return -EINVAL;
        case PROXY_SC_connect:*handled=1;return -EOPNOTSUPP;
        default:break;
        }
    }
    if(e->uevent)switch(nr){
    case PROXY_SC_bind:{*handled=1;struct sockaddr_nl *s=(void *)(uintptr_t)a[1];if(!s || a[2]<(long)sizeof(*s) || s->nl_family!=AF_NETLINK)return -EINVAL;if(s->nl_pid)e->port=s->nl_pid;return 0;}
    case PROXY_SC_getsockname:*handled=1;return address((void *)(uintptr_t)a[1],(void *)(uintptr_t)a[2],e->port);
    case PROXY_SC_setsockopt:*handled=1;return 0;
    case PROXY_SC_recvfrom:case PROXY_SC_recvmsg:*handled=1;return -EAGAIN;
    case PROXY_SC_sendto:case PROXY_SC_sendmsg:case PROXY_SC_connect:case PROXY_SC_getsockopt:case PROXY_SC_ioctl:
        *handled=1;proxy_log("fail virtual-uevent unsupported",nr,(long)a[0]);return -EOPNOTSUPP;
    default:return 0;
    }
    if(nr==PROXY_SC_ioctl && !e->route){*handled=1;struct ifreq *r=(void *)(uintptr_t)a[2];if(a[1]!=SIOCGIFINDEX)return -EOPNOTSUPP;if(!r)return -EFAULT;if(r->ifr_name[0]!='l' || r->ifr_name[1]!='o' || r->ifr_name[2])return -ENODEV;r->ifr_ifindex=1;return 0;}
    if(!e->route && (nr==PROXY_SC_sendto || nr==PROXY_SC_sendmsg || nr==PROXY_SC_recvfrom || nr==PROXY_SC_recvmsg || nr==PROXY_SC_connect || nr==PROXY_SC_bind || nr==PROXY_SC_getsockname)){*handled=1;return -EOPNOTSUPP;}
    switch(nr){
    case PROXY_SC_bind:{*handled=1;struct sockaddr_nl *s=(void *)(uintptr_t)a[1];if(!s || a[2]<(long)sizeof(*s) || s->nl_family!=AF_NETLINK)return -EINVAL;if(s->nl_groups)return -EOPNOTSUPP;if(s->nl_pid)e->port=s->nl_pid;return 0;}
    case PROXY_SC_getsockname:*handled=1;return address((void *)(uintptr_t)a[1],(void *)(uintptr_t)a[2],e->port);
    case PROXY_SC_sendto:*handled=1;return send_request((int)a[0],e,(void *)(uintptr_t)a[1],a[2],(void *)(uintptr_t)a[4],a[5]);
    case PROXY_SC_sendmsg:{*handled=1;if(!a[1])return -EFAULT;
      struct proxy_message m=arch_message_read((void *)(uintptr_t)a[1]);
      if(m.controllen)return -EINVAL;
      size_t size=0;
      for(size_t k=0;k<m.iovlen;k++){struct proxy_iovec v=arch_iovec_read((void *)(uintptr_t)m.iov,k);if(v.len>UINT_MAX-size)return -EMSGSIZE;size+=v.len;}
      char *buf=vfs_alloc(size);if(!buf)return -ENOMEM;size_t off=0;
      for(size_t k=0;k<m.iovlen;k++){struct proxy_iovec v=arch_iovec_read((void *)(uintptr_t)m.iov,k);p_copy(buf+off,(void *)(uintptr_t)v.base,v.len);off+=v.len;}
      long r=send_request((int)a[0],e,buf,size,(void *)(uintptr_t)m.name,m.namelen);vfs_free(buf);return r;}
    case PROXY_SC_recvfrom:{*handled=1;struct iovec iov={(void *)(uintptr_t)a[1],a[2]};if(a[4]){long r=address((void *)(uintptr_t)a[4],(void *)(uintptr_t)a[5],0);if(r)return r;}return receive((int)a[0],e,&iov,1,a[3],NULL);}
    case PROXY_SC_recvmsg:{*handled=1;if(!a[1])return -EFAULT;
      struct proxy_message m=arch_message_read((void *)(uintptr_t)a[1]);
      if(m.name){unsigned len=m.namelen;long r=address((void *)(uintptr_t)m.name,&len,0);if(r)return r;m.namelen=len;}else m.namelen=0;
      m.controllen=0;long r=receive((int)a[0],e,(void *)(uintptr_t)m.iov,m.iovlen,a[2],&m.flags);
      arch_message_results((void *)(uintptr_t)a[1],m);return r;}

    case PROXY_SC_connect:case PROXY_SC_setsockopt:case PROXY_SC_getsockopt:*handled=1;return -EOPNOTSUPP;
    default:return 0;
    }
}

#include "state.h"
void net_state(struct state *s){
 uint64_t count=0;for(struct descriptor *d=fds;d;d=d->next)if(state_live(d->fd))count++;
 count=state_count(s,count);if(s->error)return;
 struct descriptor *d=fds;
 for(uint64_t i=0;i<count&&!s->error;i++){
  if(s->reading){d=vfs_palloc(sizeof(*d));if(!d){s->error=-ENOMEM;return;}d->ep=vfs_palloc(sizeof(*d->ep));if(!d->ep){s->error=-ENOMEM;return;}d->ep->refs=1;d->ep->first=d->ep->last=NULL;d->next=fds;fds=d;}
  else while(d&&!state_live(d->fd))d=d->next;
  d->fd=state_word(s,d->fd);if(s->reading)d->ino=inode(d->fd);
  int alias=-1;
  if(!s->reading)for(struct descriptor *p=fds;p!=d;p=p->next)if(p->ep==d->ep&&state_live(p->fd)){alias=p->fd;break;}
  alias=state_word(s,(int64_t)alias);
  if(alias>=0){
   if(s->reading){struct endpoint *ep=lookup(alias);if(!ep){s->error=-EINVAL;return;}vfs_free(d->ep);d->ep=ep;ep->refs++;}
   else d=d->next;
   continue;
  }
  d->ep->port=state_word(s,d->ep->port);d->ep->route=state_word(s,d->ep->route);d->ep->uevent=state_word(s,d->ep->uevent);
  d->ep->emulated=state_word(s,d->ep->emulated);d->ep->kernel_fd=state_word(s,d->ep->kernel_fd);d->ep->peer_fd=state_word(s,d->ep->peer_fd);
  int app_fd=d->ep->app_fd;if(!s->reading&&d->ep->emulated){for(struct descriptor *p=fds;p;p=p->next)if(p->ep==d->ep&&state_live(p->fd)){app_fd=p->fd;break;}}
  d->ep->app_fd=state_word(s,app_fd);
  d->ep->socket_type=state_word(s,d->ep->socket_type);d->ep->domain=state_word(s,d->ep->domain);d->ep->protocol=state_word(s,d->ep->protocol);
  if(s->reading&&d->ep->emulated){int private_fd[3]={d->ep->kernel_fd,d->ep->peer_fd,d->ep->app_fd};for(unsigned k=0;k<3;k++){long valid=RAW(PROXY_SC_fcntl,private_fd[k],F_GETFD,0,0,0,0);if(valid<0){proxy_log("fail route-state-fd",private_fd[k],valid);s->error=(int)valid;return;}}}
  uint64_t replies=0;for(struct reply *r=d->ep->first;r;r=r->next)replies++;
  replies=state_count(s,replies);if(s->error)return;
  struct reply **tail=&d->ep->first;
  for(uint64_t j=0;j<replies&&!s->error;j++){
   struct reply *r=s->reading?vfs_palloc(sizeof(*r)):*tail;if(!r){s->error=-ENOMEM;return;}
   if(s->reading){r->next=NULL;*tail=r;d->ep->last=r;}
   state_bytes(s,&r->header,sizeof(r->header));state_bytes(s,&r->error,sizeof(r->error));tail=&r->next;
  }
  if(!s->reading)d=d->next;
 }
}

/* /proc/net/tcp{,6} for the stub. Android denies apps the kernel tables
 * (proc_net_tcp_udp), yet Linux tools such as lsof read them to map socket
 * inodes to addresses. Every proxy process records its connected and accepted
 * TCP sockets as one file per socket inode under the stub-wide TMPDIR; a read
 * lists the records whose owner fd still names that socket. Listening sockets
 * are not listed. */
#include <fcntl.h>
/* This file is shared by every ABI in the stub. Explicit padding keeps the
 * inode at the same offset when one ABI records a socket and another reads it. */
struct record { int32_t pid, fd; uint32_t uid, family; uint8_t local[16], remote[16]; uint16_t lport, rport; uint32_t reserved; uint64_t inode; };
_Static_assert(offsetof(struct record,inode)==56 && sizeof(struct record)==64,"TCP record wire layout");
static char *number(char *out, uint64_t v) { char d[24]; int n = 0; do d[n++] = (char)('0' + v % 10); while (v /= 10); while (n) *out++ = d[--n]; *out = 0; return out; }
static char *hex(char *out, uint32_t v, int width) { static const char h[] = "0123456789ABCDEF"; for (int i = width - 1; i >= 0; i--) *out++ = h[(v >> (i * 4)) & 15]; *out = 0; return out; }
static int directory(char *out, size_t size) {
    extern const char *proxy_tmpdir; size_t n = p_len(proxy_tmpdir);
    if (n + 24 > size) return -ENAMETOOLONG;
    p_copy(out, proxy_tmpdir, n); p_copy(out + n, "/matonos-net-tcp", 17);
    long e = RAW(PROXY_SC_mkdirat, AT_FDCWD, out, 0700, 0, 0, 0);
    return e == -EEXIST ? 0 : (int)e;
}
/* Address of a sockaddr in kernel order; 0 when not AF_INET/AF_INET6. */
static int tcp_address(const void *sa, unsigned len, uint32_t *family, uint8_t addr[16], uint16_t *port) {
    const struct sockaddr *s = sa;
    if (s->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) { const struct sockaddr_in *i = sa; *family = AF_INET; p_copy(addr, &i->sin_addr, 4); *port = i->sin_port; return 1; }
    if (s->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) { const struct sockaddr_in6 *i = sa; *family = AF_INET6; p_copy(addr, &i->sin6_addr, 16); *port = i->sin6_port; return 1; }
    return 0;
}
void tcp_note(long nr, const proxy_arg_t *a, long result) {
    int fd;
    if (nr == PROXY_SC_connect && (result == 0 || result == -EINPROGRESS)) fd = (int)a[0];
    else if (nr == PROXY_SC_accept4 && result >= 0) fd = (int)result;
    else return;
    int type = 0; unsigned tlen = sizeof(type);
    if (RAW(PROXY_SC_getsockopt, fd, SOL_SOCKET, SO_TYPE, &type, &tlen, 0) || type != SOCK_STREAM) return;
    struct record r = {0}; struct sockaddr_storage name; unsigned len = sizeof(name); uint32_t family = 0;
    if (RAW(PROXY_SC_getsockname, fd, &name, &len, 0, 0, 0) || !tcp_address(&name, len, &r.family, r.local, &r.lport)) return;
    if (nr == PROXY_SC_connect) { if (!a[1] || !tcp_address((const void *)a[1], (unsigned)a[2], &family, r.remote, &r.rport)) return; }
    else { len = sizeof(name); if (RAW(PROXY_SC_getpeername, fd, &name, &len, 0, 0, 0) || !tcp_address(&name, len, &family, r.remote, &r.rport)) return; }
    if (family != r.family) return;
    struct proxy_stat st; if (arch_stat_fd(fd, &st)) return;
    r.inode = st.st_ino; r.pid = (int32_t)RAW(PROXY_SC_getpid, 0, 0, 0, 0, 0, 0); r.fd = fd; r.uid = (uint32_t)RAW(PROXY_SC_getuid, 0, 0, 0, 0, 0, 0);
    char path[4096]; if (directory(path, sizeof(path))) return;
    char *end = path + p_len(path); *end++ = '/'; number(end, r.inode);
    long out = RAW(PROXY_SC_openat, AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600, 0, 0);
    if (out < 0) return;
    RAW(PROXY_SC_write, out, &r, sizeof(r), 0, 0, 0); RAW(PROXY_SC_close, out, 0, 0, 0, 0, 0);
}
/* The record is live while its owner fd is still that socket. */
static int live(const struct record *r) {
    char path[64] = "/proc/", *p = number(path + 6, (uint64_t)r->pid); p_copy(p, "/fd/", 5); number(p + 4, (uint64_t)r->fd);
    char want[48] = "socket:[", got[48]; char *w = number(want + 8, r->inode); *w++ = ']'; *w = 0;
    long n = RAW(PROXY_SC_readlinkat, AT_FDCWD, path, got, sizeof(got) - 1, 0, 0);
    if (n < 0) return 0;
    got[n] = 0; return p_equal(got, want);
}
static void put(int fd, const char *s) { RAW(PROXY_SC_write, fd, s, p_len(s), 0, 0, 0); }
static char *endpoint(char *o, const struct record *r, const uint8_t *a, uint16_t port) {
    for (int i = 0; i < (r->family == AF_INET ? 1 : 4); i++) { uint32_t w; p_copy(&w, a + 4 * i, 4); o = hex(o, w, 8); }
    *o++ = ':'; return hex(o, (uint32_t)((port >> 8) | ((port & 255) << 8)), 4);
}
struct tcp_proc_scratch { char dir[4096],buf[4096]; };
static long tcp_proc_open_impl(const char *path,struct tcp_proc_scratch *scratch) {
    size_t n = p_len(path); uint32_t family;
    if (n >= 9 && p_equal(path + n - 9, "/net/tcp6")) family = AF_INET6;
    else if (n >= 8 && p_equal(path + n - 8, "/net/tcp")) family = AF_INET;
    else return -ENOSYS;
    if (path[0] != '/' || path[1] != 'p' || path[2] != 'r' || path[3] != 'o' || path[4] != 'c' || path[5] != '/') return -ENOSYS;
    long out = RAW(PROXY_SC_memfd_create, "v3-net-tcp", 1, 0, 0, 0, 0); if (out < 0) return out;
    put((int)out, family == AF_INET
        ? "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
        : "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    char *dir=scratch->dir;
    long d = directory(dir, sizeof(scratch->dir)) ? -ENOENT : RAW(PROXY_SC_openat, AT_FDCWD, dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0, 0, 0);
    if (d >= 0) {
        char *buf=scratch->buf; long got; unsigned sl = 0;
        while ((got = RAW(PROXY_SC_getdents64, d, buf, sizeof(scratch->buf), 0, 0, 0)) > 0)
            for (long off = 0; off < got;) {
                struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *e = (void *)(buf + off); off += e->reclen;
                if (e->name[0] == '.') continue;
                struct record r; long f = RAW(PROXY_SC_openat, d, e->name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0, 0, 0); if (f < 0) continue;
                long k = RAW(PROXY_SC_read, f, &r, sizeof(r), 0, 0, 0); RAW(PROXY_SC_close, f, 0, 0, 0, 0, 0);
                if (k != sizeof(r)) continue;
                if (!live(&r)) { RAW(PROXY_SC_unlinkat, d, e->name, 0, 0, 0, 0); continue; }
                if (r.family != family) continue;
                char line[256], *o = line; char num[24];
                number(num, sl++); for (size_t pad = p_len(num); pad < 4; pad++) *o++ = ' ';
                p_copy(o, num, p_len(num)); o += p_len(num); *o++ = ':'; *o++ = ' ';
                o = endpoint(o, &r, r.local, r.lport); *o++ = ' '; o = endpoint(o, &r, r.remote, r.rport);
                p_copy(o, " 01 00000000:00000000 00:00000000 00000000 ", 44); o += 43;
                o = number(o, r.uid); p_copy(o, " 0 ", 4); o = number(o + 3, r.inode); p_copy(o, " 1 0000000000000000 20 4 0 10 -1\n", 34);
                put((int)out, line);
            }
        RAW(PROXY_SC_close, d, 0, 0, 0, 0, 0);
    }
    RAW(PROXY_SC_lseek, out, 0, 0, 0, 0, 0);
    return out;
}
long tcp_proc_open(const char *path){
    struct tcp_proc_scratch *scratch=vfs_alloc(sizeof(*scratch));if(!scratch)return -ENOMEM;
    long result=tcp_proc_open_impl(path,scratch);vfs_free(scratch);return result;
}
