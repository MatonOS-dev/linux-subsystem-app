/* Message/iovec/cmsg wire access is restricted to the architecture boundary.
 * Kernel replay still performs actual SCM_RIGHTS and validates user buffers. */
#ifndef PROXY_ARCH_SOCKET_H
#define PROXY_ARCH_SOCKET_H
#include <sys/socket.h>
#include <sys/uio.h>
#include <errno.h>
/* preadv's split position words use the selected kernel's unsigned long. */
static inline int64_t arch_vector_offset(proxy_arg_t low,proxy_arg_t high){return sizeof(long)==8?(int64_t)low:(int64_t)((uint64_t)(uint32_t)low|((uint64_t)(uint32_t)high<<32));}
static inline proxy_arg_t arch_vector_low(uint64_t off){return sizeof(long)==8?(proxy_arg_t)off:(uint32_t)off;}
static inline proxy_arg_t arch_vector_high(uint64_t off){return sizeof(long)==8?0:(uint32_t)(off>>32);}
struct proxy_iovec {uint64_t base,len;};
struct proxy_message {uint64_t name,namelen,iov,iovlen,control,controllen;int32_t flags;};
struct proxy_cmsg {uint64_t len;int32_t level,type;};
static inline size_t arch_iovec_size(size_t count){return count*sizeof(struct iovec);}
static inline void arch_iovec_write(void *wire,size_t index,struct proxy_iovec v){struct iovec *w=wire;w[index].iov_base=(void *)(uintptr_t)v.base;w[index].iov_len=v.len;}
static inline struct proxy_iovec arch_iovec_read(const void *wire,size_t index){
 const struct iovec *v=wire;return (struct proxy_iovec){(uintptr_t)v[index].iov_base,v[index].iov_len};
}
static inline struct proxy_message arch_message_read(const void *wire){
 const struct msghdr *w=wire;return (struct proxy_message){(uintptr_t)w->msg_name,
  (uint32_t)w->msg_namelen,(uintptr_t)w->msg_iov,w->msg_iovlen,
  (uintptr_t)w->msg_control,w->msg_controllen,w->msg_flags};
}
static inline void arch_message_results(void *wire,struct proxy_message m){
 struct msghdr *w=wire;w->msg_namelen=m.namelen;
 w->msg_controllen=m.controllen;w->msg_flags=m.flags;
}
static inline long arch_message_call(long nr,int fd,struct proxy_message *m,long flags){
 struct msghdr w={0};w.msg_name=(void *)(uintptr_t)m->name;w.msg_namelen=m->namelen;
 w.msg_iov=(void *)(uintptr_t)m->iov;w.msg_iovlen=m->iovlen;
 w.msg_control=(void *)(uintptr_t)m->control;w.msg_controllen=m->controllen;w.msg_flags=m->flags;
 long r=RAW(nr,fd,&w,flags,0,0,0);
 if(r>=0)*m=arch_message_read(&w);
 return r;
}
static inline struct proxy_cmsg arch_cmsg_read(const void *wire){
 const struct cmsghdr *w=wire;return (struct proxy_cmsg){w->cmsg_len,w->cmsg_level,w->cmsg_type};
}
struct arch_mmsghdr {struct msghdr msg_hdr;unsigned msg_len;};
static inline size_t arch_mmsg_size(unsigned count){return count*sizeof(struct arch_mmsghdr);}
static inline struct proxy_message arch_mmsg_read(void *wire,unsigned index){
 return arch_message_read(&((struct arch_mmsghdr *)wire)[index].msg_hdr);
}
static inline void arch_mmsg_no_name(void *wire,unsigned index){
 struct arch_mmsghdr *m=wire;m[index].msg_hdr.msg_name=0;m[index].msg_hdr.msg_namelen=0;
}
static inline void arch_mmsg_result(void *out,const void *wire,unsigned index){
 ((struct arch_mmsghdr *)out)[index].msg_len=((const struct arch_mmsghdr *)wire)[index].msg_len;
}
#endif
