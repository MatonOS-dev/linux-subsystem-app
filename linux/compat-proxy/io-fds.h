/* Read-time emulation descriptors occupy the upper half of the select-compatible
 * range (at most 1024), below the private reserve. Ordinary allocations stay lowest-free.
 * Fixed-number aliases cannot be moved: classic seccomp is immutable, so add
 * an exact-fd routing filter, synchronized to all threads, before publishing
 * them. These rare alias numbers remain trapped after close/reuse. */
#include <limits.h>
struct io_alias {struct io_alias *next;int fd;unsigned audit;};
static struct io_alias *io_aliases;
static long fd_link_target(int,char *,size_t);
static long io_window_end(void){return proxy_fd_reserve<1024?proxy_fd_reserve:1024;}
static long io_window_begin(void){return io_window_end()/2;}
static int io_window_fd(int fd){return proxy_fd_reserve_ready && fd>=io_window_begin() && fd<io_window_end();}
static int io_alias_known(int fd){
    for(struct io_alias *p=__atomic_load_n(&io_aliases,__ATOMIC_ACQUIRE);p;p=p->next)if(p->fd==fd && p->audit==ARCH_AUDIT)return 1;
    return 0;
}
static long io_alias_route(int fd){
    if(fd<0 || io_window_fd(fd) || io_alias_known(fd) || proxy_trace)return 0;
    struct io_alias *alias=vfs_palloc(sizeof(*alias));if(!alias)return -ENOMEM;
    struct sock_filter bpf[48];unsigned n=0;
#define IO_ADD(code,jt,jf,k) bpf[n++]=(struct sock_filter)BPF_JUMP(code,k,jt,jf)
    IO_ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,arch));
    IO_ADD(BPF_JMP|BPF_JEQ|BPF_K,1,0,ARCH_AUDIT);
    IO_ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    /* Internal gateway calls must stay native, including a handler reading
     * the exact alias that caused SIGSYS. */
    for(unsigned g=0;g<3;g++){
        uint64_t pc=arch_gateway_pc(g);
        IO_ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,instruction_pointer)+4);
        IO_ADD(BPF_JMP|BPF_JEQ|BPF_K,0,3,(uint32_t)(pc>>32));
        IO_ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,instruction_pointer));
        IO_ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,(uint32_t)pc);
        IO_ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    }
    IO_ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,nr));
    const long ops[]={PROXY_SC_read,PROXY_SC_readv,PROXY_SC_write};
    for(unsigned i=0;i<sizeof(ops)/sizeof(ops[0]);i++){
        IO_ADD(BPF_JMP|BPF_JEQ|BPF_K,0,4,arch_encode_syscall(ops[i]));
        IO_ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[0]));
        IO_ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,(uint32_t)fd);
        IO_ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_TRAP);
        IO_ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    }
    IO_ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
#undef IO_ADD
    struct sock_fprog prog={(unsigned short)n,bpf};
    long r=RAW(PROXY_SC_seccomp,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_TSYNC,&prog,0,0,0);
    if(r){vfs_free(alias);return r<0?r:-EBUSY;}
    alias->fd=fd;alias->audit=ARCH_AUDIT;
    do{alias->next=__atomic_load_n(&io_aliases,__ATOMIC_ACQUIRE);}
    while(!__atomic_compare_exchange_n(&io_aliases,&alias->next,alias,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED));
    return 0;
}
static int io_emulated_fd(int fd){
    if(fd<0)return 0;
    /* Classify the live object, not its sticky routing history: duplicating
     * an ordinary file that reused an alias must not grow more alias filters.
     * Also cover inherited descriptors, SCM_RIGHTS and pidfd_getfd. This
     * probe is on fd adoption/dup only, never an ordinary read hot path. */
    char target[64];long n=fd_link_target(fd,target,sizeof(target)-1);
    if(n<=0)return 0;
    target[n]=0;
    return p_equal(target,"anon_inode:[signalfd]") || p_equal(target,"anon_inode:inotify") || p_equal(target,"/memfd:v3-userns-limit (deleted)");
}
static long io_fd_move(long fd){
    if(fd<0 || io_window_fd((int)fd) || proxy_trace)return fd;
    if(proxy_fd_reserve_ready){
        long flags=RAW(PROXY_SC_fcntl,fd,F_GETFD,0,0,0,0);
        long moved=RAW(PROXY_SC_fcntl,fd,(flags>=0 && (flags&FD_CLOEXEC))?F_DUPFD_CLOEXEC:F_DUPFD,io_window_begin(),0,0,0);
        if(moved>=0 && moved<io_window_end()){RAW(PROXY_SC_close,fd,0,0,0,0,0);return moved;}
        if(moved>=0)RAW(PROXY_SC_close,moved,0,0,0,0,0);
    }
    /* A lowered RLIMIT or a full window must not invent EMFILE while low
     * descriptors are available. Keep the kernel allocation and route it. */
    long r=io_alias_route((int)fd);
    if(r){RAW(PROXY_SC_close,fd,0,0,0,0,0);return r;}
    return fd;
}
static long io_adopt_inherited(void){
    long dir=RAW(PROXY_SC_openat,AT_FDCWD,"/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,0,0);
    if(dir<0)return dir;
    char buf[4096];long n,error=0;
    while((n=RAW(PROXY_SC_getdents64,dir,buf,sizeof(buf),0,0,0))>0){
        for(long off=0;off<n;){
            unsigned short len=*(unsigned short *)(buf+off+16);
            if(len<20 || off+len>n){error=-EIO;goto done;}
            const char *p=buf+off+19;off+=len;unsigned fd=0;
            if(*p<'0'||*p>'9')continue;
            while(*p>='0'&&*p<='9')fd=fd*10+(*p++-'0');
            if(*p || fd==(unsigned)dir || io_window_fd((int)fd) || io_alias_known((int)fd))continue;
            if(io_emulated_fd((int)fd)){error=io_alias_route((int)fd);if(error)goto done;}
        }
    }
    if(n<0)error=n;
done:
    RAW(PROXY_SC_close,dir,0,0,0,0,0);return error;
}
static void io_fd_state(struct state *s){
    uint64_t count=0;
    for(struct io_alias *p=io_aliases;p;p=p->next)count++;
    count=state_count(s,count);
    if(!s->reading){for(struct io_alias *p=io_aliases;p;p=p->next){state_word(s,p->fd);state_word(s,p->audit);}}
    else for(uint64_t i=0;i<count && !s->error;i++){
        struct io_alias *p=vfs_palloc(sizeof(*p));if(!p){s->error=-ENOMEM;return;}
        uint64_t fd=state_word(s,0);if(fd>INT_MAX){vfs_free(p);s->error=-EINVAL;return;}
        p->fd=(int)fd;p->audit=(unsigned)state_word(s,0);p->next=io_aliases;io_aliases=p;
    }
}

static long io_received_message(const void *wire){
    struct proxy_message m=arch_message_read(wire);
    size_t offset=0,limit=(size_t)m.controllen;
    while(offset<=limit && limit-offset>=sizeof(struct cmsghdr)){
        struct cmsghdr h;if(proxy_copy(&h,(void *)(uintptr_t)(m.control+offset),sizeof(h)))return -EFAULT;
        if(h.cmsg_len<CMSG_LEN(0) || h.cmsg_len>limit-offset)break;
        if(h.cmsg_level==SOL_SOCKET && h.cmsg_type==SCM_RIGHTS){
            size_t count=(h.cmsg_len-CMSG_LEN(0))/sizeof(int);
            for(size_t i=0;i<count;i++){
                int fd;if(proxy_copy(&fd,(void *)(uintptr_t)(m.control+offset+CMSG_LEN(0)+i*sizeof(int)),sizeof(fd)))return -EFAULT;
                if(io_emulated_fd(fd)){long e=io_alias_route(fd);if(e)return e;}
            }
        }
        size_t step=CMSG_ALIGN(h.cmsg_len);if(!step || step>limit-offset)break;offset+=step;
    }
    return 0;
}
