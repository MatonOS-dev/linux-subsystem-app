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
#include <errno.h>
#include <limits.h>
#include <linux/sockios.h>
struct reply { struct reply *next; struct nlmsghdr header; struct nlmsgerr error; };
struct endpoint { unsigned refs; unsigned port; int route; struct reply *first, *last; };
struct descriptor { int fd; struct endpoint *ep; struct descriptor *next; };
static struct descriptor *fds;
static struct endpoint *lookup(int fd) {
    for(struct descriptor *p=fds;p;p=p->next)if(p->fd==fd)return p->ep;
    return NULL;
}
void net_close(int fd) {
    struct descriptor **p=&fds;
    while(*p){struct descriptor *d=*p;if(d->fd!=fd){p=&d->next;continue;}
        *p=d->next;struct endpoint *e=d->ep;vfs_free(d);
        if(!--e->refs){while(e->first){struct reply *r=e->first;e->first=r->next;vfs_free(r);}vfs_free(e);}return;
    }
}
void net_dup(int from,int to) {
    if(from==to)return;
    struct endpoint *e=lookup(from);net_close(to);if(!e)return;
    struct descriptor *d=vfs_alloc(sizeof(*d));if(!d)return;
    *d=(struct descriptor){to,e,fds};fds=d;e->refs++;
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
    for(off=0;off<len;){struct nlmsghdr *h=(void *)((char *)buf+off);struct reply *r=vfs_alloc(sizeof(*r));if(!r)return -ENOMEM;
        int error=request_error(h);*r=(struct reply){.header={.nlmsg_len=NLMSG_LENGTH(sizeof(struct nlmsgerr)),.nlmsg_type=NLMSG_ERROR,.nlmsg_flags=NLM_F_CAPPED,.nlmsg_seq=h->nlmsg_seq,.nlmsg_pid=e->port},.error={.error=error,.msg=*h}};
        if(e->last)e->last->next=r;else e->first=r;e->last=r;
        uint64_t one=1;long written=RAW(PROXY_SC_write,fd,&one,sizeof(one),0,0,0);if(written<0)return written;
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
long net_call(long nr,proxy_arg_t *a,int newnet,int *handled) {
    *handled=0;
    if(nr==PROXY_SC_socket && newnet && ((a[0]==AF_NETLINK && a[2]==NETLINK_ROUTE) || (a[0]==AF_INET && (a[1]&~(SOCK_CLOEXEC|SOCK_NONBLOCK))==SOCK_DGRAM && !a[2]))){
        *handled=1;if((a[1]&~(SOCK_CLOEXEC|SOCK_NONBLOCK))!=SOCK_RAW && (a[1]&~(SOCK_CLOEXEC|SOCK_NONBLOCK))!=SOCK_DGRAM)return -EPROTOTYPE;
        struct endpoint *e=vfs_alloc(sizeof(*e));struct descriptor *d=vfs_alloc(sizeof(*d));if(!e || !d){vfs_free(e);vfs_free(d);return -ENOMEM;}
        long fd=RAW(PROXY_SC_eventfd2,0,EFD_SEMAPHORE|EFD_NONBLOCK|((a[1]&SOCK_CLOEXEC)?EFD_CLOEXEC:0),0,0,0,0);
        if(fd<0){vfs_free(e);vfs_free(d);return fd;}
        *e=(struct endpoint){.refs=1,.route=a[0]==AF_NETLINK,.port=(unsigned)RAW(PROXY_SC_getpid,0,0,0,0,0,0)};*d=(struct descriptor){(int)fd,e,fds};fds=d;return fd;
    }
    struct endpoint *e=lookup((int)a[0]);if(!e)return 0;
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
 count=state_word(s,count);if(count>65536){s->error=-E2BIG;return;}
 struct descriptor *d=fds;
 for(uint64_t i=0;i<count&&!s->error;i++){
  if(s->reading){d=vfs_alloc(sizeof(*d));if(!d){s->error=-ENOMEM;return;}d->ep=vfs_alloc(sizeof(*d->ep));if(!d->ep){s->error=-ENOMEM;return;}d->ep->refs=1;d->ep->first=d->ep->last=NULL;d->next=fds;fds=d;}
  else while(d&&!state_live(d->fd))d=d->next;
  d->fd=state_word(s,d->fd);
  int alias=-1;
  if(!s->reading)for(struct descriptor *p=fds;p!=d;p=p->next)if(p->ep==d->ep&&state_live(p->fd)){alias=p->fd;break;}
  alias=state_word(s,(int64_t)alias);
  if(alias>=0){
   if(s->reading){struct endpoint *ep=lookup(alias);if(!ep){s->error=-EINVAL;return;}vfs_free(d->ep);d->ep=ep;ep->refs++;}
   else d=d->next;
   continue;
  }
  d->ep->port=state_word(s,d->ep->port);d->ep->route=state_word(s,d->ep->route);
  uint64_t replies=0;for(struct reply *r=d->ep->first;r;r=r->next)replies++;
  replies=state_word(s,replies);if(replies>65536){s->error=-E2BIG;return;}
  struct reply **tail=&d->ep->first;
  for(uint64_t j=0;j<replies&&!s->error;j++){
   struct reply *r=s->reading?vfs_alloc(sizeof(*r)):*tail;if(!r){s->error=-ENOMEM;return;}
   if(s->reading){r->next=NULL;*tail=r;d->ep->last=r;}
   state_bytes(s,&r->header,sizeof(r->header));state_bytes(s,&r->error,sizeof(r->error));tail=&r->next;
  }
  if(!s->reading)d=d->next;
 }
}
