/* SPDX-License-Identifier: Apache-2.0
 * One explicitly owned Linux session. No libc/TLS calls from SIGSYS. Host fds
 * are capabilities behind Linux numbers; unrecognized operations fail closed.
 * Metadata uses a signal-safe lock; blocking I/O never holds that lock. */
#define _GNU_SOURCE
#include "session.h"
#include "../proxy.h"
#include "../vfs.h"
#include <stdatomic.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/futex.h>
#include <limits.h>
struct descriptor {int number,host,flags;unsigned refs;int closed;struct descriptor *next;};
struct area {uintptr_t lo,hi;struct area *next;};
struct resident_session {
    atomic_flag lock;
    atomic_flag exec_lock;
    struct descriptor *fds;
    struct area *areas;
    unsigned mask;
    unsigned long actions[64][4];
    struct rlimit limits[RLIM_NLIMITS];
};
#define R(n,a,b,c,d,e,f) proxy_raw(SYS_##n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f))
extern long proxy_resident_dispatch(long,const long[6]);
static void lock(struct resident_session *s){while(atomic_flag_test_and_set_explicit(&s->lock,memory_order_acquire))__asm__ volatile("pause");}
static void unlock(struct resident_session *s){atomic_flag_clear_explicit(&s->lock,memory_order_release);}
static struct descriptor *find(struct resident_session *s,int n){for(struct descriptor *d=s->fds;d;d=d->next)if(d->number==n)return d;return 0;}
static long insert(struct resident_session *s,int host,int flags,int minimum){
    struct descriptor *d=vfs_alloc(sizeof(*d));if(!d)return -ENOMEM;
    lock(s);int n=minimum;
    while(n<INT_MAX && (unsigned)n<s->limits[RLIMIT_NOFILE].rlim_cur && find(s,n))n++;
    if(n==INT_MAX || (unsigned)n>=s->limits[RLIMIT_NOFILE].rlim_cur){unlock(s);vfs_free(d);return -EMFILE;}
    *d=(struct descriptor){.number=n,.host=host,.flags=flags,.next=s->fds};s->fds=d;unlock(s);return n;
}
struct pins {struct descriptor *fds[6];unsigned count;};
static int translate(struct resident_session *s,int n,struct pins *pins){
    lock(s);struct descriptor *d=find(s,n);int host=-1;
    if(d && pins->count<6){d->refs++;pins->fds[pins->count++]=d;host=d->host;}
    unlock(s);return host;
}
static long destroy_fd(struct descriptor *d){long a[6]={d->host};long r=proxy_resident_dispatch(SYS_close,a);vfs_free(d);return r;}
static void unpin(struct resident_session *s,struct pins *pins){
    for(unsigned i=0;i<pins->count;i++){
        struct descriptor *d=pins->fds[i];lock(s);int destroy=!--d->refs && d->closed;unlock(s);
        if(destroy)destroy_fd(d);
    }
}
static long close_fd(struct resident_session *s,int n){
    lock(s);struct descriptor **p=&s->fds;while(*p && (*p)->number!=n)p=&(*p)->next;
    struct descriptor *d=*p;int destroy=0;
    if(d){*p=d->next;d->closed=1;destroy=d->refs==0;}
    unlock(s);if(!d)return -EBADF;
    return destroy?destroy_fd(d):0;
}
static int add_area(struct resident_session *s,uintptr_t lo,size_t size){
    if(!size || size>UINTPTR_MAX-lo)return -EINVAL;
    struct area *a=vfs_alloc(sizeof(*a));if(!a)return -ENOMEM;
    *a=(struct area){lo,lo+size,s->areas};s->areas=a;return 0;
}
static int owns(struct resident_session *s,uintptr_t lo,size_t size){
    if(!size || size>UINTPTR_MAX-lo)return 0;
    uintptr_t end=lo+size;
    while(lo<end){uintptr_t next=lo;for(struct area *a=s->areas;a;a=a->next)if(a->lo<=lo && a->hi>next)next=a->hi;if(next==lo)return 0;lo=next;}
    return 1;
}
static void remove_area(struct resident_session *s,uintptr_t lo,size_t size,struct area **spare){
    uintptr_t hi=lo+size;
    for(struct area **p=&s->areas;*p;){struct area *a=*p;
        if(a->hi<=lo || a->lo>=hi){p=&a->next;continue;}
        if(a->lo<lo && a->hi>hi){struct area *tail=*spare;*spare=NULL;*tail=(struct area){hi,a->hi,a->next};a->next=tail;a->hi=lo;return;}
        if(a->lo<lo){a->hi=lo;p=&a->next;}else if(a->hi>hi){a->lo=hi;p=&a->next;}else {*p=a->next;vfs_free(a);}
    }
}
struct resident_session *resident_session_begin(const int fds[2]){
    struct resident_session *s=vfs_alloc(sizeof(*s));if(!s)return 0;
    extern void vfs_resident_reset(void);vfs_resident_reset();
    extern void proxy_resident_reset(void);proxy_resident_reset();
    atomic_flag_clear(&s->exec_lock);atomic_flag_clear(&s->lock);s->mask=022;
    for(int i=0;i<RLIM_NLIMITS;i++){s->limits[i].rlim_cur=RLIM_INFINITY;s->limits[i].rlim_max=RLIM_INFINITY;}
    s->limits[RLIMIT_NOFILE]=(struct rlimit){1024,1024};
    for(int i=0;i<3;i++){
        long fd=i?R(fcntl,fds[i-1],F_DUPFD_CLOEXEC,0,0,0,0):R(openat,AT_FDCWD,"/dev/null",O_RDONLY|O_CLOEXEC,0,0,0);
        if(fd<0){resident_session_end(s);return 0;}
        long n=insert(s,fd,0,i);if(n<0){R(close,fd,0,0,0,0,0);resident_session_end(s);return 0;}
    }return s;
}
void resident_session_end(struct resident_session *s){
    if(!s)return;
    while(s->fds)close_fd(s,s->fds->number);
    while(s->areas){struct area *a=s->areas;s->areas=a->next;R(munmap,a->lo,a->hi-a->lo,0,0,0,0);vfs_free(a);}
    extern void vfs_resident_reset(void);vfs_resident_reset();
    extern void proxy_resident_reset(void);proxy_resident_reset();
    vfs_free(s);
}
int resident_session_image(struct resident_session *s,const uintptr_t maps[6]){
    struct resident_session temporary={0};
    for(unsigned i=0;i<6;i+=2)if(maps[i+1]){
        int e=add_area(&temporary,maps[i],maps[i+1]);
        if(e){while(temporary.areas){struct area *a=temporary.areas;temporary.areas=a->next;vfs_free(a);}return e;}
    }
    while(temporary.areas){struct area *a=temporary.areas;temporary.areas=a->next;a->next=s->areas;s->areas=a;}
    return 0;
}
static long session_call(struct resident_session *s,long n,const long original[6],struct pins *pins){
    long a[6];for(int i=0;i<6;i++)a[i]=original[i];
    long r;int fd=-1;
    switch(n){
    case SYS_rt_sigaction:
        if(a[0]<1 || a[0]>64 || a[3]!=8)return -EINVAL;
        if(a[0]==9 || a[0]==19)return -EINVAL;
        lock(s);if(a[2])p_copy((void *)a[2],s->actions[a[0]-1],32);
        if(a[1])p_copy(s->actions[a[0]-1],(void *)a[1],32);
        unlock(s);return 0;
    case SYS_umask:lock(s);r=s->mask;s->mask=a[0]&0777;unlock(s);return r;
    case SYS_prlimit64:
        if(a[0] && a[0]!=R(getpid,0,0,0,0,0,0))return -ESRCH;
        if((unsigned long)a[1]>=RLIM_NLIMITS)return -EINVAL;
        lock(s);struct rlimit next=s->limits[a[1]];
        if(a[2])next=*(const struct rlimit *)a[2];
        if(next.rlim_cur>next.rlim_max || next.rlim_max>s->limits[a[1]].rlim_max){unlock(s);return -EPERM;}
        if(a[3])*(struct rlimit *)a[3]=s->limits[a[1]];
        if(a[2])s->limits[a[1]]=next;
        unlock(s);return 0;
    case SYS_getrlimit:
        if((unsigned long)a[0]>=RLIM_NLIMITS)return -EINVAL;
        lock(s);*(struct rlimit *)a[1]=s->limits[a[0]];unlock(s);return 0;
    case SYS_setrlimit:{long b[6]={0,a[0],a[1],0};return resident_session_call(s,SYS_prlimit64,b);}
    case SYS_close:return close_fd(s,a[0]);
    case SYS_dup:case SYS_fcntl:
        fd=translate(s,a[0],pins);if(fd<0)return -EBADF;
        if(n==SYS_fcntl){
            if(a[1]==F_GETFD){lock(s);struct descriptor *d=find(s,a[0]);r=d?d->flags:-EBADF;unlock(s);return r;}
            if(a[1]==F_SETFD){lock(s);struct descriptor *d=find(s,a[0]);if(d)d->flags=a[2]&FD_CLOEXEC;unlock(s);return d?0:-EBADF;}
            if(a[1]!=F_DUPFD && a[1]!=F_DUPFD_CLOEXEC){if(a[1]!=F_GETFL && a[1]!=F_SETFL)return -ENOSYS;return R(fcntl,fd,a[1],a[2],0,0,0);}
            if(a[2]<0)return -EINVAL;
        }
        long duplicate[6]={fd,F_DUPFD_CLOEXEC,0};
        r=proxy_resident_dispatch(SYS_fcntl,duplicate);if(r<0)return r;
        {long logical=insert(s,r,n==SYS_fcntl && a[1]==F_DUPFD_CLOEXEC?FD_CLOEXEC:0,n==SYS_dup?0:a[2]);if(logical<0)R(close,r,0,0,0,0,0);return logical;}
    case SYS_socket:
        r=proxy_resident_dispatch(n,a);if(r<0)return r;
        {long logical=insert(s,r,(a[1]&02000000)?FD_CLOEXEC:0,0);if(logical<0)R(close,r,0,0,0,0,0);return logical;}
    case SYS_close_range:
        if((unsigned)a[0]>(unsigned)a[1] || (a[2]&~4L))return -EINVAL;
        {unsigned cursor=a[0];
        for(;;){lock(s);struct descriptor *selected=NULL;
            for(struct descriptor *d=s->fds;d;d=d->next)if((unsigned)d->number>=cursor && (unsigned)d->number<=(unsigned)a[1] && (!selected || d->number<selected->number))selected=d;
            if(!selected){unlock(s);break;}
            int number=selected->number;if(a[2]&4)selected->flags=FD_CLOEXEC;unlock(s);
            if(!(a[2]&4))close_fd(s,number);
            if(number==INT_MAX)break;
            cursor=(unsigned)number+1;
        }return 0;}
    case SYS_open:a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;n=SYS_openat;/* fall through */
    case SYS_openat:
        if((int)a[0]!=AT_FDCWD){a[0]=translate(s,a[0],pins);if(a[0]<0)return -EBADF;}
        lock(s);a[3]&=~s->mask;unlock(s);
        r=proxy_resident_dispatch(n,a);if(r<0)return r;
        {long logical=insert(s,r,(a[2]&O_CLOEXEC)?FD_CLOEXEC:0,0);if(logical<0){long b[6]={r};proxy_resident_dispatch(SYS_close,b);}return logical;}
    case SYS_read:case SYS_write:case SYS_connect:case SYS_bind:case SYS_sendto:case SYS_recvfrom:case SYS_getsockname:case SYS_getpeername:case SYS_setsockopt:case SYS_getsockopt:case SYS_readv:case SYS_writev:case SYS_pread64:case SYS_pwrite64:case SYS_lseek:case SYS_fstat:case SYS_getdents64:case SYS_ftruncate:case SYS_ioctl:case SYS_fchdir:
        a[0]=translate(s,a[0],pins);if(a[0]<0)return -EBADF;break;
    case SYS_newfstatat:case SYS_readlinkat:case SYS_faccessat:case SYS_faccessat2:
        if((int)a[0]!=AT_FDCWD){a[0]=translate(s,a[0],pins);if(a[0]<0)return -EBADF;}break;
    case SYS_mmap: {
        if(!(a[3]&MAP_ANONYMOUS)){a[4]=translate(s,a[4],pins);if(a[4]<0)return -EBADF;}
        if(a[1]<=0 || (unsigned long)a[1]>LONG_MAX-4095)return -EINVAL;
        a[1]=(a[1]+4095)&~4095UL;
        /* Reserve metadata before changing kernel VMAs. Allocation failure
         * must never leave stale ownership over an unmapped/reused ART hole. */
        struct area *record=vfs_alloc(sizeof(*record)),*spare=vfs_alloc(sizeof(*spare));
        if(!record || !spare){vfs_free(record);vfs_free(spare);return -ENOMEM;}
        lock(s);
        if((a[3]&MAP_FIXED) && !owns(s,a[0],a[1])){unlock(s);vfs_free(record);vfs_free(spare);return -EPERM;}
        r=proxy_resident_dispatch(n,a);
        if(r>=0){
            if(a[3]&MAP_FIXED)remove_area(s,r,a[1],&spare);
            *record=(struct area){r,r+a[1],s->areas};s->areas=record;record=NULL;
        }
        unlock(s);vfs_free(record);vfs_free(spare);return r;
    }
    case SYS_munmap:case SYS_mprotect:case SYS_madvise: {
        if(a[1]<=0 || (unsigned long)a[1]>LONG_MAX-4095)return -EINVAL;
        a[1]=(a[1]+4095)&~4095UL;
        struct area *spare=n==SYS_munmap?vfs_alloc(sizeof(*spare)):NULL;
        if(n==SYS_munmap && !spare)return -ENOMEM;
        lock(s);if(!owns(s,a[0],a[1])){unlock(s);vfs_free(spare);return -EPERM;}
        r=proxy_raw(n,a[0],a[1],a[2],0,0,0);
        if(!r && n==SYS_munmap)remove_area(s,a[0],a[1],&spare);
        unlock(s);vfs_free(spare);return r;
    }
    case SYS_unshare:case SYS_mount:case SYS_umount2:case SYS_pivot_root:case SYS_chroot:
    case SYS_chdir:case SYS_getcwd:case SYS_access:case SYS_stat:case SYS_lstat:case SYS_readlink:
    case SYS_getpid:case SYS_getppid:case SYS_gettid:case SYS_getuid:case SYS_geteuid:case SYS_getgid:case SYS_getegid:
    case SYS_futex:case SYS_clock_gettime:case SYS_clock_getres:case SYS_getrandom:case SYS_sched_yield:case SYS_nanosleep:case SYS_clock_nanosleep:case SYS_uname:case SYS_gettimeofday:case SYS_getpriority:case SYS_sched_getaffinity:
        break;
    default:return -ENOSYS;
    }
    return proxy_resident_dispatch(n,a);
}

long resident_session_call(struct resident_session *s,long n,const long original[6]){
    struct pins pins={0};long result=session_call(s,n,original,&pins);unpin(s,&pins);return result;
}

/* Loading completes before any Linux resources are discarded. The caller must
 * quiesce Linux workers after staging; ART threads/fds are never enumerated. */
int resident_session_exec(struct resident_session *s,const char *path,char *const argv[],char *const env[],uintptr_t entry[2]) {
    if(atomic_flag_test_and_set(&s->exec_lock))return -EAGAIN;
    struct image image;
    int e=proxy_load(path,argv,env,&image);if(e){atomic_flag_clear(&s->exec_lock);return e;}
    uintptr_t maps[6];extern void inprocess_image_maps(uintptr_t[6]);
    inprocess_image_maps(maps);
    struct resident_session staged={0};
    e=resident_session_image(&staged,maps);
    if(e){while(staged.areas){struct area *a=staged.areas;staged.areas=a->next;vfs_free(a);}extern void inprocess_unload(void);inprocess_unload();atomic_flag_clear(&s->exec_lock);return e;}
    extern int inprocess_quiesce(void);inprocess_quiesce();
    while(s->areas){struct area *a=s->areas;s->areas=a->next;R(munmap,a->lo,a->hi-a->lo,0,0,0,0);vfs_free(a);}
    s->areas=staged.areas;
    for(struct descriptor *d=s->fds,*next;d;d=next){next=d->next;if(d->flags&FD_CLOEXEC)close_fd(s,d->number);}
    for(unsigned i=0;i<64;i++)if(s->actions[i][0]!=1)for(unsigned j=0;j<4;j++)s->actions[i][j]=0;
    entry[0]=image.entry;entry[1]=image.stack;atomic_flag_clear(&s->exec_lock);return 0;
}

long resident_session_bind(struct resident_session *s,const char *source,const char *target){return s?proxy_mount(source,target):-EINVAL;}
long resident_session_pack(struct resident_session *s,const char *target,const char *index,const char *directory){return s?pack_mount(target,index,directory):-EINVAL;}
long resident_session_dns(struct resident_session *s,const char *endpoint,const char *path){
    if(!s)return -EINVAL;
    int e=proxy_dns_forwarder(endpoint);return e?e:proxy_dns_socket(path);
}
