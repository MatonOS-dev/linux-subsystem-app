/* Linux layer v3. Resident state is inherited by fork; this is a spike,
 * NOT a security boundary. The original Android filter remains authoritative. */
#define _GNU_SOURCE
#include "proxy.h"
#include "vfs.h"
#include "net.h"
#include "links.h"
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <linux/capability.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <linux/dma-buf.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/limits.h>
#include <linux/prctl.h>
#include <stddef.h>
#include <fcntl.h>
#include <errno.h>
#include "build/app-policy.h"
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif
#ifndef SYS_USER_DISPATCH
#define SYS_USER_DISPATCH 2
#endif
#define SIGSYS_BIT (1UL << (SIGSYS-1))
#define SA_RESTORER 0x04000000
#define NS_FLAGS (CLONE_NEWNS|CLONE_NEWCGROUP|CLONE_NEWUTS|CLONE_NEWIPC|CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNET)
struct kernel_action { uintptr_t handler,flags,restorer; unsigned long mask; };
static struct kernel_action chained;
/* Resident exec must discard libc handlers: bionic debuggerd uses bionic
 * TLS, which is no longer valid after entering a glibc image. */
static void crash_hex(uintptr_t value) {
    char out[18]="0x";static const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<16;i++)out[2+i]=digits[(value>>(60-4*i))&15];
    RAW(SYS_write,2,out,sizeof(out),0,0,0);
}
static void crash_text(const char *s){RAW(SYS_write,2,s,p_len(s),0,0,0);}
static uintptr_t maps_hex(const char **p){uintptr_t v=0;for(;;){unsigned c=(unsigned char)**p,d;if(c>='0'&&c<='9')d=c-'0';else if(c>='a'&&c<='f')d=c-'a'+10;else break;v=(v<<4)|d;(*p)++;}return v;}
struct image_map {uintptr_t lo,hi,offset;char *path;struct image_map *next;};
static struct image_map *image_maps;
void proxy_image_map(uintptr_t lo,uintptr_t hi,uintptr_t offset,const char *path){
    struct image_map *m=vfs_alloc(sizeof(*m));if(!m)return;
    m->path=vfs_alloc(p_len(path)+1);if(!m->path){vfs_free(m);return;}
    p_copy(m->path,path,p_len(path)+1);m->lo=lo;m->hi=hi;m->offset=offset;
    m->next=image_maps;image_maps=m;
}
static void crash_report(int sig,siginfo_t *info,void *context) {
    ucontext_t *uc=context;uintptr_t pc=uc->uc_mcontext.gregs[REG_RIP];
    crash_text("v3 fatal signal=");crash_hex(sig);
    crash_text(" pid=");crash_hex(RAW(SYS_getpid,0,0,0,0,0,0));
    crash_text(" tid=");crash_hex(RAW(SYS_gettid,0,0,0,0,0,0));
    crash_text(" code=");crash_hex(info->si_code);
    crash_text(" addr=");crash_hex((uintptr_t)info->si_addr);
    crash_text(" pc=");crash_hex(pc);crash_text("\n");
    for(struct image_map *m=image_maps;m;m=m->next)if(pc>=m->lo && pc<m->hi){
        crash_text("v3 fatal image=");crash_text(m->path);crash_text(" elf-offset=");crash_hex(pc-m->lo+m->offset);crash_text("\n");break;
    }
    /* Raw maps: bypass virtual proc paths; no libc/TLS/allocation in a crash. */
    long fd=RAW(SYS_openat,AT_FDCWD,"/proc/self/maps",O_RDONLY|O_CLOEXEC,0,0,0);
    if(fd>=0){char line[1024];size_t n=0;char c;
        while(RAW(SYS_read,fd,&c,1,0,0,0)==1){
            if(c!='\n'){if(n+1<sizeof(line))line[n++]=c;continue;}
            line[n]=0;const char *p=line;uintptr_t lo=maps_hex(&p),hi=0;
            if(*p=='-'){p++;hi=maps_hex(&p);}
            if(pc>=lo && pc<hi){crash_text("v3 fatal map=");crash_text(line);crash_text(" offset=");crash_hex(pc-lo);crash_text("\n");break;}n=0;
        }RAW(SYS_close,fd,0,0,0,0,0);
    }
    struct kernel_action action={0};RAW(SYS_rt_sigaction,sig,&action,0,8,0,0);
    unsigned long mask=1UL<<(sig-1);RAW(SYS_rt_sigprocmask,SIG_UNBLOCK,&mask,0,8,0,0);
    RAW(SYS_tgkill,RAW(SYS_getpid,0,0,0,0,0,0),RAW(SYS_gettid,0,0,0,0,0,0),sig,0,0,0);
    RAW(SYS_exit_group,128+sig,0,0,0,0,0);
}
static void exec_signals(void) {
    for(int sig=1;sig<=64;sig++){
        if(sig==SIGKILL || sig==SIGSTOP || sig==SIGSYS)continue;
        struct kernel_action old={0},next={0};
        if(RAW(SYS_rt_sigaction,sig,0,&old,8,0,0)<0)continue;
        if(old.handler==1)continue; /* SIG_IGN survives exec. */
        if(sig==SIGSEGV || sig==SIGBUS || sig==SIGILL || sig==SIGFPE || sig==SIGABRT)
            next=(struct kernel_action){(uintptr_t)crash_report,SA_SIGINFO|SA_RESTORER|SA_ONSTACK,(uintptr_t)proxy_restore,0};
        RAW(SYS_rt_sigaction,sig,&next,0,8,0,0);
    }
}
/* Spike supports a single user-dispatch owner; configuration is per-thread. */
static volatile unsigned char *dispatch_selector;
static long dispatch_owner;
struct thread_state { long tid; stack_t app_stack; };
struct thread_node {struct thread_state state;struct thread_node *next;};
static struct thread_node *threads;
static struct thread_state *thread_state(void) {
    long tid=RAW(SYS_gettid,0,0,0,0,0,0);
    for(struct thread_node *p=threads;p;p=p->next) if(p->state.tid==tid)return &p->state;
    struct thread_node *p=vfs_alloc(sizeof(*p));if(!p)return NULL;
    p->state.tid=tid;p->state.app_stack.ss_flags=SS_DISABLE;
    stack_t current;
    if(!RAW(SYS_sigaltstack,0,&current,0,0,0,0) && !(current.ss_flags&SS_ONSTACK)) {
        long area=RAW(SYS_mmap,0,1<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if((unsigned long)area>=(unsigned long)-4095){vfs_free(p);return NULL;}
        stack_t own={.ss_sp=(void *)area,.ss_size=1<<20};
        if(RAW(SYS_sigaltstack,&own,0,0,0,0,0)){RAW(SYS_munmap,area,1<<20,0,0,0,0);vfs_free(p);return NULL;}
    }
    do {p->next=__atomic_load_n(&threads,__ATOMIC_ACQUIRE);}
    while(!__atomic_compare_exchange_n(&threads,&p->next,p,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED));
    return &p->state;
}
const char *proxy_tmpdir="/tmp";
/* --trace: log every trapped call and translated path (stderr = app output). */
int proxy_trace;
static void emit(const char *s) { RAW(SYS_write,2,s,p_len(s),0,0,0); }
void proxy_log(const char *kind,long nr,long result) {
    char line[128]; size_t n=0;
    const char *prefix="v3 trap "; while(*prefix) line[n++]=*prefix++;
    while(*kind) line[n++]=*kind++;
    line[n++]=' '; long values[2]={nr,result};
    for(int i=0;i<2;i++) {
        unsigned long v=values[i]<0?(unsigned long)(-(values[i]+1))+1:(unsigned long)values[i];
        if(values[i]<0) line[n++]='-';
        char digits[24];int d=0; do {digits[d++]=(char)('0'+v%10);v/=10;} while(v);
        while(d) line[n++]=digits[--d];
        line[n++]=' ';
    }
    const char *label="pid=";while(*label)line[n++]=*label++;
    unsigned long pid=(unsigned long)RAW(SYS_getpid,0,0,0,0,0,0);char digits[24];unsigned d=0;
    do{digits[d++]=(char)('0'+pid%10);pid/=10;}while(pid);while(d)line[n++]=digits[--d];line[n++]='\n';
    RAW(SYS_write,2,line,n,0,0,0);
}
static int app_allowed(long nr) {
    for(unsigned i=0;i<sizeof(android_allowed)/sizeof(android_allowed[0]);i++)
        if(android_allowed[i]==nr) return 1;
    return 0;
}
static unsigned long namespaces;
static long namespace_clone_parent;
static unsigned long namespace_clone_flags;
uintptr_t proxy_clone_resume;
extern char proxy_clone_entry[],proxy_clone_after[];
static unsigned userns_remaining=~0U;
struct sysctl_fd {int fd;struct sysctl_fd *next;};
static struct sysctl_fd *sysctl_fds;
static int sysctl_tracked(int fd){for(struct sysctl_fd *p=sysctl_fds;p;p=p->next)if(p->fd==fd)return 1;return 0;}
static void sysctl_close(int fd){struct sysctl_fd **p=&sysctl_fds;while(*p){struct sysctl_fd *d=*p;if(d->fd==fd){*p=d->next;vfs_free(d);return;}p=&d->next;}}
static void sysctl_dup(int from,int to){if(from==to)return;int tracked=sysctl_tracked(from);sysctl_close(to);if(tracked){struct sysctl_fd *p=vfs_alloc(sizeof(*p));if(p){*p=(struct sysctl_fd){to,sysctl_fds};sysctl_fds=p;}}}
static long sysctl_open(int cloexec){long fd=RAW(SYS_memfd_create,"v3-userns-limit",cloexec?1:0,0,0,0,0);if(fd<0)return fd;struct sysctl_fd *p=vfs_alloc(sizeof(*p));if(!p){RAW(SYS_close,fd,0,0,0,0,0);return -ENOMEM;}*p=(struct sysctl_fd){(int)fd,sysctl_fds};sysctl_fds=p;return fd;}
static long sysctl_write(int fd,const char *buf,size_t len){if(!len || len>11)return -EINVAL;unsigned limit=0;size_t k=0;for(;k<len && buf[k]>='0' && buf[k]<='9';k++){unsigned digit=buf[k]-'0';if(limit>(~0U-digit)/10)return -ERANGE;limit=limit*10+digit;}if(!k || (k<len && !(k+1==len && buf[k]=='\n')))return -EINVAL;long r=RAW(SYS_write,fd,buf,len,0,0,0);if(r>=0)userns_remaining=limit;return r;}

static int self_exe(const char *path){char name[64]="/proc/",digits[24];size_t k=6;unsigned d=0;unsigned long pid=(unsigned long)RAW(SYS_getpid,0,0,0,0,0,0);do{digits[d++]=(char)('0'+pid%10);pid/=10;}while(pid);while(d)name[k++]=digits[--d];p_copy(name+k,"/exe",5);return p_equal(path,name) || p_equal(path,"/proc/self/exe");}
static long path_call(long nr,long *a,int one,int two) {
    if(!app_allowed(nr))return -ENOSYS;
    int at=one==1 || (nr==SYS_symlinkat && one==2);
    int fd=at?(int)a[one-1]:AT_FDCWD;
    int follow=nr!=SYS_readlinkat && nr!=SYS_readlink && nr!=SYS_unlinkat && nr!=SYS_renameat && nr!=SYS_renameat2 && nr!=SYS_symlinkat && nr!=SYS_mkdirat && nr!=SYS_mknodat;
    if(nr==SYS_newfstatat || nr==SYS_fchownat || nr==SYS_utimensat || nr==SYS_faccessat2)follow=!(a[nr==SYS_fchownat?4:3]&AT_SYMLINK_NOFOLLOW);
    if(nr==SYS_linkat)follow=(a[4]&AT_SYMLINK_FOLLOW)?VFS_FOLLOW_LINK_SOURCE:0;
    if(nr==SYS_statx)follow=!(a[2]&AT_SYMLINK_NOFOLLOW);
    if(nr==SYS_openat)follow=!(a[2]&O_NOFOLLOW) && (a[2]&(O_CREAT|O_EXCL))!=(O_CREAT|O_EXCL);
    /* Publish an unnamed inode via procfs: AT_EMPTY_PATH otherwise requires
     * CAP_DAC_READ_SEARCH. Resolve the destination and mount policy normally. */
    char empty_link[64];
    if(nr==SYS_linkat && a[1] && !*(char *)a[1] && (a[4]&AT_EMPTY_PATH)){
        char digits[24];size_t k=14;unsigned d=0;unsigned long n=(unsigned)fd;
        p_copy(empty_link,"/proc/self/fd/",14);
        do{digits[d++]=(char)('0'+n%10);n/=10;}while(n);
        while(d)empty_link[k++]=digits[--d];
        empty_link[k]=0;
        a[1]=(long)empty_link;a[0]=AT_FDCWD;fd=AT_FDCWD;
        a[4]=(a[4]&~AT_EMPTY_PATH)|AT_SYMLINK_FOLLOW;follow=VFS_FOLLOW_LINK_SOURCE;
    }
    /* Other empty-path operations act on the original descriptor. */
    if(a[one] && !*(char *)a[one] && at){long r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);if(!r && nr==SYS_newfstatat)links_fd_stat(fd,(void *)a[2]);if(!r && nr==SYS_statx){struct stat st;if(!RAW(SYS_fstat,fd,&st,0,0,0,0)){links_fd_stat(fd,&st);((struct statx *)a[4])->stx_nlink=st.st_nlink;}}return r;}
    const char *original_path=(void *)a[one];
    if(nr==SYS_openat && original_path){long synthetic=vfs_proc_open(original_path);if(synthetic!=-ENOSYS)return synthetic;}
    long e=0;void *pm=NULL,*qm=NULL;char *p=vfs_resolve_mount(fd,(char *)a[one],follow,&e,&pm);if(!p){if(proxy_trace && a[one]){const char *in=(const char *)a[one];RAW(SYS_write,2,"v3 unresolved ",14,0,0,0);RAW(SYS_write,2,in,p_len(in),0,0,0);RAW(SYS_write,2,"\n",1,0,0,0);}return e;}
    char *q=NULL;
    if(two>=0){q=vfs_resolve_mount(at?(int)a[two-1]:AT_FDCWD,(char *)a[two],0,&e,&qm);if(!q)goto done;if(pm!=qm){e=-EXDEV;goto done;}a[two]=(long)q;if(at)a[two-1]=AT_FDCWD;}
    if(nr==SYS_openat){e=vfs_proc_open(p);if(e!=-ENOSYS)goto done;}
    int writing=nr==SYS_mkdirat || nr==SYS_unlinkat || nr==SYS_renameat || nr==SYS_renameat2 || nr==SYS_linkat || nr==SYS_symlinkat || nr==SYS_fchmodat || nr==SYS_fchownat || nr==SYS_utimensat || nr==SYS_mknodat || (nr==SYS_openat && (a[2]&(O_WRONLY|O_RDWR|O_CREAT|O_TRUNC)));
    if(nr==SYS_mkdirat){struct stat existing;if(!RAW(SYS_newfstatat,AT_FDCWD,p,&existing,AT_SYMLINK_NOFOLLOW,0,0)){e=-EEXIST;goto done;}}
    /* Exclusive creation checks existence before mount write permission.
     * bwrap probes an already bound file this way before mounting a new
     * source over it. Never open/truncate the read-only backing inode. */
    if(nr==SYS_openat && (a[2]&(O_CREAT|O_EXCL))==(O_CREAT|O_EXCL)){
        struct stat existing;
        if(!RAW(SYS_newfstatat,AT_FDCWD,p,&existing,AT_SYMLINK_NOFOLLOW,0,0)){e=-EEXIST;goto done;}
    }
    if(writing && ((e=vfs_mount_readonly(pm)) || (q && (e=vfs_mount_readonly(qm)))))goto done;
    if((namespaces&CLONE_NEWUSER) && nr==SYS_openat && (a[2]&(O_WRONLY|O_RDWR)) && p_equal(p,"/proc/sys/user/max_user_namespaces")){e=sysctl_open((a[2]&O_CLOEXEC)!=0);goto done;}
    /* bwrap reads the overflow IDs for its uid maps; Android's policy denies
     * apps the generic proc_sysctl file. Serve the kernel default (65534). */
    if(nr==SYS_openat && !(a[2]&(O_WRONLY|O_RDWR)) && (p_equal(p,"/proc/sys/kernel/overflowuid") || p_equal(p,"/proc/sys/kernel/overflowgid"))){
        long fd=RAW(SYS_memfd_create,"v3-overflowid",(a[2]&O_CLOEXEC)?1:0,0,0,0,0);
        if(fd>=0 && (RAW(SYS_write,fd,"65534\n",6,0,0,0)!=6 || RAW(SYS_lseek,fd,0,SEEK_SET,0,0,0)!=0)){RAW(SYS_close,fd,0,0,0,0,0);fd=-EIO;}
        e=fd;goto done;
    }
    if(namespaces && nr==SYS_openat && (a[2]&(O_WRONLY|O_RDWR))){
        size_t n=p_len(p);const char *tail=p;while(*tail){if(*tail=='/')tail++;else break;}int proc=p[0]=='/' && p[1]=='p' && p[2]=='r' && p[3]=='o' && p[4]=='c' && p[5]=='/';
        if(proc && ((n>=8 && p_equal(p+n-8,"/uid_map")) || (n>=8 && p_equal(p+n-8,"/gid_map")) || (n>=10 && p_equal(p+n-10,"/setgroups")))){e=RAW(SYS_memfd_create,"v3-namespace-map",(a[2]&O_CLOEXEC)?1:0,0,0,0,0);goto done;}
    }
    /* bwrap unlinks --bind-data/--ro-bind-data temp files right after binding;
     * a real mount keeps the inode, our virtual one needs the name. */
    if(nr==SYS_unlinkat && !(a[2]&AT_REMOVEDIR) && vfs_is_mount_source(p)){e=0;goto done;}
    /* bwrap --dev needs /dev/full, which Android apps may not open: serve
     * /dev/zero (reads identical; writes succeed instead of ENOSPC). */
    if(p_equal(p,"/dev/full")) {
        struct stat probe;
        if(RAW(SYS_newfstatat,AT_FDCWD,"/dev/full",&probe,0,0,0)==-EACCES) {
            char *zero=vfs_alloc(10);
            if(!zero){e=-ENOMEM;goto done;}
            p_copy(zero,"/dev/zero",10);vfs_free(p);p=zero;
        }
    }
    if(proxy_trace){RAW(SYS_write,2,"v3 path ",8,0,0,0);RAW(SYS_write,2,p,p_len(p),0,0,0);RAW(SYS_write,2,"\n",1,0,0,0);}
    a[one]=(long)p;if(at)a[one-1]=AT_FDCWD;
    if(nr==SYS_readlinkat){char *tracked=vfs_proc_fd_name(p);if(tracked){size_t n=p_len(tracked);if(n>(size_t)a[3])n=(size_t)a[3];p_copy((void *)a[2],tracked,n);vfs_free(tracked);e=(long)n;goto done;}}
    if(nr==SYS_readlinkat && self_exe(p)){char *exe=vfs_exe();if(exe){size_t n=p_len(exe);if(n>(size_t)a[3])n=(size_t)a[3];p_copy((void *)a[2],exe,n);vfs_free(exe);e=(long)n;goto done;}}
    if(nr==SYS_openat && (a[2]&O_TMPFILE)==O_TMPFILE)e=links_open(p,a[2],a[3]);
    else if(nr==SYS_linkat)e=links_publish(p,q,a[4]);
    else if(nr==SYS_unlinkat || nr==SYS_renameat || nr==SYS_renameat2)e=links_name_call(nr,a,p,q);
    else if(nr==SYS_symlinkat || nr==SYS_mkdirat || nr==SYS_mknodat)e=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
    else e=links_path_call(nr,a,p);
    if(e>=0 && nr==SYS_openat)vfs_fd_record((int)e,fd,original_path);
    if(e>=0 && nr==SYS_readlinkat){char *raw=vfs_alloc((size_t)e+1);if(!raw){e=-ENOMEM;goto done;}p_copy(raw,(void *)a[2],(size_t)e);raw[e]=0;
        /* Only proc magic links expose backing names. Ordinary symlink targets
         * retain their stored text, as required by readlink. */
        if(p_len(p)>=6 && p[0]=='/' && p[1]=='p' && p[2]=='r' && p[3]=='o' && p[4]=='c' && p[5]=='/' && raw[0]=='/'){
            char *rev=vfs_reverse(raw);if(rev){size_t n=p_len(rev);if(n>(size_t)a[3])n=(size_t)a[3];p_copy((void *)a[2],rev,n);e=(long)n;vfs_free(rev);}}
        vfs_free(raw);
    }
done:
    /* Hardware passthrough is "where convenient": a host /dev node the app's
     * sandbox may not use is reported absent, so bwrap/Flatpak *-try binds and
     * device probes skip it instead of failing on EACCES. */
    if(p && p[0]=='/' && p[1]=='d' && p[2]=='e' && p[3]=='v' && p[4]=='/') {
        if(e==-EACCES && (nr==SYS_newfstatat || nr==SYS_statx || nr==SYS_faccessat2 || nr==SYS_faccessat ||
                (nr==SYS_openat && (a[2]&O_PATH))))e=-ENOENT;
        /* O_PATH opens need no permission, so the denial would only surface at
         * the caller's fstat (bwrap's bind source check): probe it here. */
        if(e>=0 && nr==SYS_openat && (a[2]&O_PATH)) {
            struct stat probe;
            if(RAW(SYS_fstat,e,&probe,0,0,0,0)==-EACCES){RAW(SYS_close,e,0,0,0,0,0);e=-ENOENT;}
        }
        /* Existence checks (access F_OK) need no permission either; Flatpak
         * uses them to pick a mandatory --dev-bind, so answer consistently. */
        if(e==0 && (nr==SYS_faccessat || nr==SYS_faccessat2)) {
            struct stat probe;
            if(RAW(SYS_newfstatat,AT_FDCWD,p,&probe,0,0,0)==-EACCES)e=-ENOENT;
        }
    }
    vfs_free(p);vfs_free(q);return e;
}
/* Translate before path dispatch so forbidden legacy instructions are never
 * replayed under the inherited Android filter. x86_64 stat layouts match. */
/* Flatpak verbose output does not include the sealed --args bundle in this
 * build. Log it without changing the fd offset or its seals. */
void proxy_log_bwrap_args(const char *path,char *const *argv){
    const char *base=path;for(const char *p=path;*p;p++)if(*p=='/')base=p+1;
    if(!p_equal(base,"matonos-app-bwrap") && !p_equal(base,"bwrap"))return;
    for(size_t i=0;argv && argv[i];i++){
        RAW(SYS_write,2,"v3 bwrap argv ",14,0,0,0);
        RAW(SYS_write,2,argv[i],p_len(argv[i]),0,0,0);RAW(SYS_write,2,"\n",1,0,0,0);
        if(!p_equal(argv[i],"--args") || !argv[i+1])continue;
        const char *n=argv[i+1];unsigned fd=0;int valid=*n!=0;
        for(;*n;n++){if(*n<'0'||*n>'9'||fd>(2147483647U-(unsigned)(*n-'0'))/10){valid=0;break;}fd=fd*10+(*n-'0');}
        if(!valid)continue;
        RAW(SYS_write,2,"v3 bwrap bundle begin\n",sizeof("v3 bwrap bundle begin\n")-1,0,0,0);
        char bytes[4096];long offset=0,count;
        while((count=RAW(SYS_pread64,fd,bytes,sizeof(bytes),offset,0,0))>0){
            for(long j=0;j<count;j++)if(!bytes[j])bytes[j]='\n';
            RAW(SYS_write,2,bytes,count,0,0,0);offset+=count;
        }
        RAW(SYS_write,2,"v3 bwrap bundle end\n",sizeof("v3 bwrap bundle end\n")-1,0,0,0);
    }
}
static long modernize(long nr,long *a,long *answer) {
    switch(nr){
    case SYS_access:a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return SYS_faccessat;
    case SYS_open:a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return SYS_openat;
    case SYS_creat:a[3]=a[1];a[2]=O_CREAT|O_WRONLY|O_TRUNC;a[1]=a[0];a[0]=AT_FDCWD;return SYS_openat;
    case SYS_stat:case SYS_lstat:a[3]=nr==SYS_lstat?AT_SYMLINK_NOFOLLOW:0;a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return SYS_newfstatat;
    case SYS_mkdir:a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return SYS_mkdirat;
    case SYS_rmdir:case SYS_unlink:a[2]=nr==SYS_rmdir?AT_REMOVEDIR:0;a[1]=a[0];a[0]=AT_FDCWD;return SYS_unlinkat;
    case SYS_rename:case SYS_link:a[3]=a[1];a[1]=a[0];a[0]=a[2]=AT_FDCWD;a[4]=0;return nr==SYS_link?SYS_linkat:SYS_renameat;
    case SYS_symlink:a[2]=a[1];a[1]=AT_FDCWD;return SYS_symlinkat;
    case SYS_readlink:a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return SYS_readlinkat;
    case SYS_chmod:a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return SYS_fchmodat;
    case SYS_chown:case SYS_lchown:a[4]=nr==SYS_lchown?AT_SYMLINK_NOFOLLOW:0;a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return SYS_fchownat;
    case SYS_mknod:a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return SYS_mknodat;
    case SYS_io_pgetevents:{
        struct aio_mask {unsigned long *mask;size_t size;};struct aio_mask *m=(void *)a[5];unsigned long old=0;
        if(m && m->mask){if(m->size!=8){*answer=-EINVAL;return -1;}unsigned long next=*m->mask&~SIGSYS_BIT;*answer=RAW(SYS_rt_sigprocmask,SIG_SETMASK,&next,&old,8,0,0);if(*answer)return -1;}
        *answer=RAW(SYS_io_getevents,a[0],a[1],a[2],a[3],a[4],0);
        if(m && m->mask)RAW(SYS_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0);
        return -1;
    }
    case SYS_accept:a[3]=0;return SYS_accept4;
    case SYS_pause:*answer=RAW(SYS_ppoll,0,0,0,0,8,0);return -1;
    case SYS_futimesat:{struct timespec times[2],*tp=NULL;if(a[2]){struct timeval *tv=(void *)a[2];tp=times;for(int i=0;i<2;i++){times[i].tv_sec=tv[i].tv_sec;times[i].tv_nsec=tv[i].tv_usec*1000;}}long b[6]={a[0],a[1],(long)tp,0,0,0};*answer=path_call(SYS_utimensat,b,1,-1);return -1;}
    case SYS_poll:{struct timespec ts,*t=NULL;if((int)a[2]>=0){ts.tv_sec=(int)a[2]/1000;ts.tv_nsec=((int)a[2]%1000)*1000000;t=&ts;}*answer=RAW(SYS_ppoll,a[0],a[1],t,0,8,0);return -1;}
    case SYS_select:{struct timespec ts,*t=NULL;struct timeval *tv=(void *)a[4];if(tv){ts.tv_sec=tv->tv_sec;ts.tv_nsec=tv->tv_usec*1000;t=&ts;}*answer=RAW(SYS_pselect6,a[0],a[1],a[2],a[3],t,0);if(tv){tv->tv_sec=ts.tv_sec;tv->tv_usec=ts.tv_nsec/1000;}return -1;}
    case SYS_alarm:{struct itimerval next={0},old;next.it_value.tv_sec=(unsigned)a[0];*answer=RAW(SYS_setitimer,ITIMER_REAL,&next,&old,0,0,0);if(!*answer)*answer=old.it_value.tv_sec+(old.it_value.tv_usec!=0);return -1;}
    case SYS_utime:case SYS_utimes:{struct timespec times[2],*tp=NULL;if(a[1]){tp=times;if(nr==SYS_utime){long *v=(void *)a[1];for(int i=0;i<2;i++){times[i].tv_sec=v[i];times[i].tv_nsec=0;}}else {struct timeval *v=(void *)a[1];for(int i=0;i<2;i++){times[i].tv_sec=v[i].tv_sec;times[i].tv_nsec=v[i].tv_usec*1000;}}}long b[6]={AT_FDCWD,a[0],(long)tp,0,0,0};*answer=path_call(SYS_utimensat,b,1,-1);return -1;}
    case SYS_getdents:{
        /* linux_dirent64 -> legacy x86_64 linux_dirent, including d_type
         * in the last byte. Consume at most caller capacity, no dropped entries. */
        void *buf=vfs_alloc((size_t)a[2]);if(!buf){*answer=-ENOMEM;return -1;}long n=links_dents(a[0],buf,a[2]);long used=0;
        if(n>0)for(long off=0;off<n;){unsigned char *p=(unsigned char *)buf+off;unsigned short len=*(unsigned short *)(p+16);if(len<20 || off+len>n){n=-EIO;break;}unsigned char *q=(unsigned char *)a[1]+used;p_copy(q,p,18);size_t name=p_len((char *)p+19)+1;p_copy(q+18,p+19,name);for(size_t k=18+name;k<len;k++)q[k]=0;q[len-1]=p[18];used+=len;off+=len;}
        *answer=n<0?n:used;vfs_free(buf);return -1;
    }
    case SYS_pipe:a[1]=0;return SYS_pipe2;
    case SYS_dup2:
        if(a[0]==a[1]){*answer=RAW(SYS_fcntl,a[0],F_GETFD,0,0,0,0);if(*answer>=0)*answer=a[0];return -1;}a[2]=0;return SYS_dup3;
    case SYS_getpgrp:a[0]=0;return SYS_getpgid;
    case SYS_epoll_create:if(a[0]<=0){*answer=-EINVAL;return -1;}a[0]=0;return SYS_epoll_create1;
    case SYS_epoll_wait:a[4]=0;a[5]=8;return SYS_epoll_pwait;
    case SYS_inotify_init:a[0]=0;return SYS_inotify_init1;
    case SYS_eventfd:a[1]=0;return SYS_eventfd2;
    case SYS_signalfd:a[3]=0;return SYS_signalfd4;
    case SYS_fork:a[0]=SIGCHLD;a[1]=a[2]=a[3]=a[4]=0;return SYS_clone;
    case SYS_time:{struct timespec ts;*answer=RAW(SYS_clock_gettime,0,&ts,0,0,0,0);if(!*answer){*answer=ts.tv_sec;if(a[0])*(long *)a[0]=ts.tv_sec;}return -1;}
    default:return nr;
    }
}
/* One exact IPv4 endpoint, inherited across fork and resident exec. */
static struct sockaddr_in dns_endpoint;
struct dns_fd {int fd;struct dns_fd *next;};
static struct dns_fd *dns_fds;
static int dns_tracked(int fd) {for(struct dns_fd *p=dns_fds;p;p=p->next)if(p->fd==fd)return 1;return 0;}
static void dns_close(int fd) {
    struct dns_fd **link=&dns_fds;
    while(*link){if((*link)->fd==fd){struct dns_fd *old=*link;*link=old->next;vfs_free(old);return;}link=&(*link)->next;}
}
static int dns_track(int fd) {
    if(dns_tracked(fd))return 0;
    struct dns_fd *p=vfs_alloc(sizeof(*p));if(!p)return -ENOMEM;
    p->fd=fd;p->next=dns_fds;dns_fds=p;return 0;
}
static void dns_dup(int from,int to) {int tracked=dns_tracked(from);dns_close(to);if(tracked)dns_track(to);}
static unsigned short dns_swap(unsigned short n) { return (unsigned short)((n<<8)|(n>>8)); }
int proxy_dns_forwarder(const char *text) {
    unsigned char bytes[4];
    for (unsigned i=0;i<4;i++) {
        unsigned n=0,digits=0;
        while(*text>='0' && *text<='9') {n=n*10+*text++-'0';if(n>255)return -EINVAL;digits++;}
        if(!digits || *text++!=(i==3?':':'.'))return -EINVAL;
        bytes[i]=(unsigned char)n;
    }
    unsigned port=0,digits=0;
    while(*text>='0' && *text<='9') {port=port*10+*text++-'0';if(port>65535)return -EINVAL;digits++;}
    if(*text || !digits || port<1024 || bytes[0]!=127)return -EINVAL;
    dns_endpoint.sin_family=AF_INET;
    dns_endpoint.sin_port=dns_swap((unsigned short)port);
    p_copy(&dns_endpoint.sin_addr,bytes,4);
    return 0;
}
static int dns_address(const void *address, unsigned long length, struct sockaddr_in *out) {
    if(!dns_endpoint.sin_port || !address || length<sizeof(*out))return 0;
    p_copy(out,address,sizeof(*out));
    if(out->sin_family!=AF_INET || out->sin_port!=dns_swap(53) ||
       out->sin_addr.s_addr!=dns_endpoint.sin_addr.s_addr)return 0;
    out->sin_port=dns_endpoint.sin_port;return 1;
}
/* Filesystem AF_UNIX addresses are paths: translate them through the virtual
 * mount table like any path call (Wayland, X11, PulseAudio, D-Bus sockets).
 * Abstract and unnamed addresses pass through. Returns 1 when handled. */
static int unix_call(long nr,long a[6],long *result) {
    if(nr!=SYS_connect && nr!=SYS_bind)return 0;
    const struct sockaddr_un *in=(const void *)a[1];
    long len=a[2];
    if(proxy_trace && in && len>2 && len<=(long)sizeof(*in) && in->sun_family==AF_UNIX){
        crash_text("v3 unix ");RAW(SYS_write,2,in->sun_path,(size_t)len-2,0,0,0);crash_text("\n");
    }
    if(!in || len<=(long)offsetof(struct sockaddr_un,sun_path) || len>(long)sizeof(*in) || in->sun_family!=AF_UNIX || !in->sun_path[0])return 0;
    char given[sizeof(in->sun_path)+1];size_t n=(size_t)len-offsetof(struct sockaddr_un,sun_path);
    p_copy(given,in->sun_path,n);given[n]=0;
    long e=0;char *real=vfs_resolve(AT_FDCWD,given,nr==SYS_connect,&e);
    if(!real){*result=e;return 1;}
    if(p_equal(real,given)){vfs_free(real);return 0;}
    struct sockaddr_un out;p_copy(&out,in,offsetof(struct sockaddr_un,sun_path));out.sun_family=AF_UNIX;
    size_t k=p_len(real);long dir=-1;
    if(k<sizeof(out.sun_path)) {
        p_copy(out.sun_path,real,k+1);
    } else {
        /* Too long for sun_path: reach it through an O_PATH handle on its dir. */
        char *slash=real+k;while(slash>real && *slash!='/')slash--;
        if(slash==real){vfs_free(real);*result=-ENAMETOOLONG;return 1;}
        *slash=0;dir=RAW(SYS_openat,AT_FDCWD,real,O_PATH|O_DIRECTORY|O_CLOEXEC,0,0,0);*slash='/';
        if(dir<0){vfs_free(real);*result=dir;return 1;}
        char via[sizeof(out.sun_path)];size_t m=0;const char *pre="/proc/self/fd/";while(*pre)via[m++]=*pre++;
        char digits[24];int d=0;unsigned long v=(unsigned long)dir;do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);while(d)via[m++]=digits[--d];
        size_t tail=p_len(slash);
        if(m+tail>=sizeof(via)){RAW(SYS_close,dir,0,0,0,0,0);vfs_free(real);*result=-ENAMETOOLONG;return 1;}
        p_copy(via+m,slash,tail+1);p_copy(out.sun_path,via,m+tail+1);k=m+tail;
    }
    vfs_free(real);
    *result=RAW(nr,a[0],(long)&out,(long)(offsetof(struct sockaddr_un,sun_path)+k+1),0,0,0);
    if(dir>=0)RAW(SYS_close,dir,0,0,0,0,0);
    return 1;
}
static long dns_call(long nr, long a[6]) {
    {long translated;if(unix_call(nr,a,&translated))return translated;}
    struct sockaddr_in address;
    if(nr==SYS_connect || nr==SYS_sendto) {
        unsigned slot=nr==SYS_connect?1:4;
        if(dns_address((void *)a[slot],a[slot+1],&address)) {
            long copy[6];p_copy(copy,a,sizeof(copy));copy[slot]=(long)&address;copy[slot+1]=sizeof(address);
            /* Reserve tracking before connect so allocation failure cannot
             * leave a successfully connected socket with an unvirtualized peer. */
            int previous=dns_tracked((int)a[0]);
            if(nr==SYS_connect && dns_track((int)a[0]))return -ENOMEM;
            long result=RAW(nr,copy[0],copy[1],copy[2],copy[3],copy[4],copy[5]);
            if(nr==SYS_connect && result<0 && result!=-EINPROGRESS && !previous)dns_close((int)a[0]);
            return result;
        }
    } else if(nr==SYS_sendmsg && a[1]) {
        struct msghdr message; p_copy(&message,(void *)a[1],sizeof(message));
        if(dns_address(message.msg_name,message.msg_namelen,&address)) {
            message.msg_name=&address;message.msg_namelen=sizeof(address);
            return RAW(nr,a[0],&message,a[2],0,0,0);
        }
    } else if(nr==SYS_sendmmsg && a[1] && a[2]>0) {
        /* Copy dynamically: no new message-count limit. Kernel validates vlen. */
        size_t count=(unsigned int)a[2];
        int translated=0;
        for(size_t i=0;i<count;i++) {
            struct msghdr *message=&((struct mmsghdr *)a[1])[i].msg_hdr;
            if(dns_address(message->msg_name,message->msg_namelen,&address)){translated=1;break;}
        }
        if(!translated)return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
        if(count>SIZE_MAX/sizeof(struct mmsghdr) || count>SIZE_MAX/sizeof(address))return -EINVAL;
        struct mmsghdr *messages=vfs_alloc(count*sizeof(*messages));
        struct sockaddr_in *addresses=vfs_alloc(count*sizeof(*addresses));
        if(!messages || !addresses){vfs_free(messages);vfs_free(addresses);return -ENOMEM;}
        p_copy(messages,(void *)a[1],count*sizeof(*messages));
        for(size_t i=0;i<count;i++)if(dns_address(messages[i].msg_hdr.msg_name,messages[i].msg_hdr.msg_namelen,&addresses[i])) {
            messages[i].msg_hdr.msg_name=&addresses[i];messages[i].msg_hdr.msg_namelen=sizeof(address);
        }
        long result=RAW(nr,a[0],messages,a[2],a[3],0,0);
        if(result>0)for(long i=0;i<result;i++)((struct mmsghdr *)a[1])[i].msg_len=messages[i].msg_len;
        vfs_free(messages);vfs_free(addresses);return result;
    }
    if(nr==SYS_getpeername && dns_tracked((int)a[0])) {
        /* Inspect full peer independently of caller's possibly truncated buffer. */
        unsigned int length=sizeof(address);
        long result=RAW(nr,a[0],&address,&length,0,0,0);
        if(result<0)return result;
        if(length==sizeof(address) && address.sin_family==AF_INET &&
           address.sin_addr.s_addr==dns_endpoint.sin_addr.s_addr && address.sin_port==dns_endpoint.sin_port) {
            address.sin_port=dns_swap(53);
            if(!a[1] || !a[2])return -EFAULT;
            unsigned int capacity=*(unsigned int *)a[2];
            p_copy((void *)a[1],&address,capacity<length?capacity:length);
            *(unsigned int *)a[2]=length;return 0;
        }
    }
    long result=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
    if(nr==SYS_connect && (result==0 || result==-EINPROGRESS))dns_close((int)a[0]);
    if(nr==SYS_socket && result>=0)dns_close((int)result);
    return result;
}
static int ours(long n) {
    switch(n) {
    case SYS_ioctl:case SYS_socket:case SYS_bind:case SYS_getsockname:case SYS_sendto:case SYS_sendmsg:case SYS_recvfrom:case SYS_recvmsg:case SYS_connect:case SYS_getpeername:case SYS_sendmmsg:case SYS_setsockopt:case SYS_getsockopt:
    case SYS_fstat:case SYS_getdents64:case SYS_exit_group:case SYS_write:case SYS_close:case SYS_dup:case SYS_dup3:case SYS_fcntl:
    case SYS_capget:case SYS_capset:case SYS_setreuid:case SYS_setregid:case SYS_setfsuid:case SYS_setfsgid:case SYS_setgroups:case SYS_setuid:case SYS_setgid:case SYS_setresuid:case SYS_setresgid:
    case SYS_utime:case SYS_utimes:case SYS_creat:case SYS_rmdir:case SYS_mknod:case SYS_mknodat:
    case SYS_prctl:case SYS_execve:case SYS_execveat:case SYS_rt_sigaction:case SYS_rt_sigprocmask:
    case SYS_sigaltstack:case SYS_unshare:case SYS_mount:case SYS_umount2:case SYS_pivot_root:
    case SYS_vfork:case SYS_clone:case SYS_clone3:case SYS_io_uring_setup:case SYS_io_uring_enter:case SYS_io_uring_register:
    case SYS_open:case SYS_openat:case SYS_openat2:case SYS_stat:case SYS_lstat:case SYS_newfstatat:
    case SYS_statx:case SYS_access:case SYS_faccessat:case SYS_faccessat2:case SYS_readlink:case SYS_readlinkat:
    case SYS_fchdir:case SYS_chdir:case SYS_getcwd:case SYS_mkdir:case SYS_mkdirat:case SYS_unlink:case SYS_unlinkat:
    case SYS_rename:case SYS_renameat:case SYS_renameat2:case SYS_link:case SYS_linkat:
    case SYS_symlink:case SYS_symlinkat:case SYS_chmod:case SYS_fchmodat:case SYS_chown:case SYS_lchown:
    case SYS_fchownat:case SYS_utimensat:case SYS_statfs:case SYS_chroot:return 1;
    default:return 0;
    }
}
static void dispatch_app(int sig,siginfo_t *info,void *ctx) {
    struct kernel_action action=chained;
    if(action.handler==1 && info->si_code<=0) return;
    if(action.handler<=1) { /* SIGSYS default is fatal, including ignored synchronous SIGSYS. */
        RAW(SYS_exit_group,128+sig,0,0,0,0,0);return;
    }
    if(action.flags&SA_RESETHAND) chained.handler=0;
    /* Keep SIGSYS deliverable even during app chaining. Do not implement SA_ONSTACK
     * by switching stacks here; the proxy never depends on an app-owned altstack. */
    unsigned long old=0,mask=action.mask&~SIGSYS_BIT;
    RAW(SYS_rt_sigprocmask,SIG_BLOCK,&mask,&old,8,0,0);
    if(action.flags&SA_SIGINFO) ((void (*)(int,siginfo_t *,void *))action.handler)(sig,info,ctx);
    else ((void (*)(int))action.handler)(sig);
    old&=~SIGSYS_BIT;RAW(SYS_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0);
}
static void exec_close_fds(void){
    long fd=RAW(SYS_openat,AT_FDCWD,"/proc/self/fd",O_DIRECTORY|O_RDONLY|O_CLOEXEC,0,0,0);if(fd<0)return;
    char *buf=vfs_alloc(8192);if(!buf){RAW(SYS_close,fd,0,0,0,0,0);return;}
    for(;;){long n=RAW(SYS_getdents64,fd,buf,8192,0,0,0);if(n<=0)break;
        for(long off=0;off<n;){unsigned short len=*(unsigned short *)(buf+off+16);if(len<20 || off+len>n)break;char *name=buf+off+19;long v=0;int valid=*name!=0;for(;*name;name++){if(*name<'0'||*name>'9'){valid=0;break;}v=v*10+*name-'0';}
            if(valid && v!=fd){long flags=RAW(SYS_fcntl,v,F_GETFD,0,0,0,0);if(flags>=0 && (flags&FD_CLOEXEC)){RAW(SYS_close,v,0,0,0,0,0);links_close((int)v);vfs_fd_close((int)v);net_close((int)v);sysctl_close((int)v);}}off+=len;
        }
    }
    vfs_free(buf);RAW(SYS_close,fd,0,0,0,0,0);
}
static void handler(int sig,siginfo_t *info,void *opaque) {
    ucontext_t *uc=opaque;
    volatile unsigned char *selector=dispatch_selector;
    unsigned char saved=0;
    if(selector) {saved=*selector;*selector=SYSCALL_DISPATCH_FILTER_ALLOW;}
    /* The retried clone inherits this pending record. Only its child adopts
     * new namespace flags; the parent's monitor keeps the original namespace. */
    if(namespace_clone_parent){
        if(RAW(SYS_getpid,0,0,0,0,0,0)!=namespace_clone_parent){
            namespaces|=namespace_clone_flags;
            if((namespace_clone_flags&CLONE_NEWUSER) && userns_remaining!=~0U)userns_remaining--;
        }
        namespace_clone_parent=0;namespace_clone_flags=0;
    }
    struct thread_state *thread=thread_state();
    if(info->si_code!=SYS_SECCOMP) {
        /* SYS_USER_DISPATCH belongs to the app, never to this syscall proxy. */
        dispatch_app(sig,info,opaque);(*(unsigned long *)&uc->uc_sigmask)&=~SIGSYS_BIT;
        RAW(SYS_sigaltstack,0,&uc->uc_stack,0,0,0,0);
        uc->uc_stack.ss_flags&=~SS_ONSTACK;
        if(selector && dispatch_selector==selector) *selector=saved;
        return;
    }
    long nr=info->si_syscall,original=nr;
    long a[6]={uc->uc_mcontext.gregs[REG_RDI],uc->uc_mcontext.gregs[REG_RSI],
        uc->uc_mcontext.gregs[REG_RDX],uc->uc_mcontext.gregs[REG_R10],
        uc->uc_mcontext.gregs[REG_R8],uc->uc_mcontext.gregs[REG_R9]};
    long result=-ENOSYS;
    /* signalfd readiness is evaluated against the thread's signal state.
     * Infinite poll must wait outside the SIGSYS frame, with the application's
     * mask restored, rather than nesting the wait in a signal handler. */
    if(nr==SYS_poll && (int)a[2]<0){
        uc->uc_mcontext.gregs[REG_RAX]=SYS_ppoll;
        uc->uc_mcontext.gregs[REG_RDX]=0;uc->uc_mcontext.gregs[REG_R10]=0;
        uc->uc_mcontext.gregs[REG_R8]=8;uc->uc_mcontext.gregs[REG_RIP]-=2;
        proxy_log("android-poll-retry",nr,0);goto clean_mask;
    }
    nr=modernize(nr,a,&result);
    if(nr<0)goto finish;
    int net_handled=0;result=net_call(nr,a,(namespaces&CLONE_NEWNET)!=0,&net_handled);
    if(net_handled)goto finish;
    result=-ENOSYS;
    if(!ours(nr)) {if(app_allowed(nr))result=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);goto finish;}
    switch(nr) {
    case SYS_getdents64:result=links_dents(a[0],(void *)a[1],a[2]);break;
    case SYS_fstat:result=RAW(nr,a[0],a[1],0,0,0,0);if(!result)links_fd_stat((int)a[0],(void *)a[1]);break;
    case SYS_exit_group:links_exit();result=RAW(nr,a[0],0,0,0,0,0);break;
    case SYS_write:result=sysctl_tracked((int)a[0])?sysctl_write((int)a[0],(void *)a[1],a[2]):RAW(nr,a[0],a[1],a[2],0,0,0);break;
    case SYS_close:result=RAW(nr,a[0],0,0,0,0,0);if(!result){dns_close((int)a[0]);links_close((int)a[0]);vfs_fd_close((int)a[0]);net_close((int)a[0]);sysctl_close((int)a[0]);}break;
    case SYS_dup:case SYS_dup3:case SYS_fcntl:
        result=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
        if(result>=0 && (nr!=SYS_fcntl || a[1]==F_DUPFD || a[1]==F_DUPFD_CLOEXEC)){long tracked=links_dup((int)a[0],(int)result);if(tracked<0){RAW(SYS_close,result,0,0,0,0,0);links_close((int)result);dns_close((int)result);vfs_fd_close((int)result);net_close((int)result);sysctl_close((int)result);result=tracked;break;}dns_dup((int)a[0],(int)result);vfs_fd_dup((int)a[0],(int)result);net_dup((int)a[0],(int)result);sysctl_dup((int)a[0],(int)result);}
        break;
    case SYS_ioctl:case SYS_socket:case SYS_bind:case SYS_getsockname:case SYS_sendto:case SYS_sendmsg:case SYS_recvfrom:case SYS_recvmsg:case SYS_connect:case SYS_getpeername:case SYS_sendmmsg:case SYS_setsockopt:case SYS_getsockopt:
        if(app_allowed(nr))result=dns_call(nr,a);
        break;
    case SYS_prctl:
        if(a[0]==PR_CAPBSET_DROP || a[0]==PR_CAP_AMBIENT || a[0]==PR_SET_KEEPCAPS) {result=0;break;}
        if(a[0]==PR_CAPBSET_READ){result=0;break;}
        if(a[0]==PR_SET_SYSCALL_USER_DISPATCH) {
            long tid=RAW(SYS_gettid,0,0,0,0,0,0);
            if(a[1]==PR_SYS_DISPATCH_ON) {
                uintptr_t restore=(uintptr_t)proxy_restore;
                /* The kernel may dispatch sigreturn itself. Reject an interval
                 * that would trap our restorer; never silently widen app policy. */
                if(!a[4] || (dispatch_owner && dispatch_owner!=tid) ||
                   restore<(uintptr_t)a[2] || restore+16>=(uintptr_t)a[2]+(uintptr_t)a[3]) {
                    result=-ENOTSUP;break;
                }
                volatile unsigned char *next=(volatile unsigned char *)a[4];
                unsigned char previous=*next;*next=SYSCALL_DISPATCH_FILTER_ALLOW;
                result=RAW(nr,a[0],a[1],a[2],a[3],a[4],0);
                if(!result) {dispatch_selector=next;dispatch_owner=tid;
                    if(!selector) {selector=next;saved=previous;}
                } else *next=previous;
            } else if(a[1]==PR_SYS_DISPATCH_OFF) {
                result=RAW(nr,a[0],a[1],a[2],a[3],a[4],0);
                if(!result && dispatch_owner==tid) {dispatch_selector=NULL;dispatch_owner=0;}
            } else result=-EINVAL;
        } else result=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
        break;
    case SYS_rt_sigaction:
        if(a[0]==SIGSYS) {
            if(a[3]!=8) {result=-EINVAL;break;}
            struct kernel_action next=chained;
            if(a[1]) p_copy(&next,(void *)a[1],sizeof(next));
            if(a[2]) p_copy((void *)a[2],&chained,sizeof(chained));
            if(a[1]) {next.mask&=~SIGSYS_BIT;chained=next;}result=0;
        } else {
            struct kernel_action action;
            if(a[1]) {p_copy(&action,(void *)a[1],sizeof(action));action.mask&=~SIGSYS_BIT;a[1]=(long)&action;}
            result=RAW(nr,a[0],a[1],a[2],a[3],0,0);
        }break;
    case SYS_rt_sigprocmask: {
        if(a[3]!=8) {result=-EINVAL;break;}
        unsigned long mask;
        if(a[1]) {p_copy(&mask,(void *)a[1],8);mask&=~SIGSYS_BIT;a[1]=(long)&mask;}
        result=RAW(nr,a[0],a[1],a[2],8,0,0);
        if(!result && a[1])RAW(SYS_rt_sigprocmask,SIG_SETMASK,0,&(*(unsigned long *)&uc->uc_sigmask),8,0,0);
        break;
    }
    case SYS_sigaltstack: {
        /* App altstack changes are virtual: the kernel retains our private stack.
         * The chained app handler's SA_ONSTACK is currently unsupported. */
        if(!thread) {result=-ENOMEM;break;}
        stack_t next=thread->app_stack;
        if(a[0]) {
            p_copy(&next,(void *)a[0],sizeof(next));
            if(next.ss_flags & ~(SS_DISABLE|(1U<<31))) {result=-EINVAL;break;}
            if(!(next.ss_flags&SS_DISABLE) && next.ss_size<2048) {result=-ENOMEM;break;}
        }
        if(a[1]) p_copy((void *)a[1],&thread->app_stack,sizeof(stack_t));
        if(a[0]) thread->app_stack=next;
        result=0;break;
    }
    case SYS_unshare:
        result=(a[0]&~(long)NS_FLAGS)?-ENOSYS:0;
        if(!result && (a[0]&CLONE_NEWUSER) && !userns_remaining)result=-EPERM;
        if(!result){namespaces|=a[0];if((a[0]&CLONE_NEWUSER) && userns_remaining!=~0U)userns_remaining--;}
        break;
    case SYS_mount:result=vfs_mount((char *)a[0],(char *)a[1],(char *)a[2],(unsigned long)a[3]);break;
    case SYS_umount2:result=vfs_unmount((char *)a[0]);break;
    case SYS_pivot_root:result=vfs_pivot((char *)a[0],(char *)a[1]);break;
    case SYS_chroot:result=vfs_root((char *)a[0]);break;
    case SYS_setreuid:case SYS_setregid:case SYS_setfsuid:case SYS_setfsgid:case SYS_setgroups:case SYS_setuid:case SYS_setgid:case SYS_setresuid:case SYS_setresgid:result=0;break;
    case SYS_capset:result=0;break;
    case SYS_capget:{struct __user_cap_header_struct *h=(void *)a[0];if(!h){result=-EFAULT;break;}
        unsigned count=h->version==_LINUX_CAPABILITY_VERSION_1?1:2;
        if(h->version!=_LINUX_CAPABILITY_VERSION_1 && h->version!=_LINUX_CAPABILITY_VERSION_2 && h->version!=_LINUX_CAPABILITY_VERSION_3){h->version=_LINUX_CAPABILITY_VERSION_3;result=-EINVAL;break;}
        if(a[1]){struct __user_cap_data_struct *d=(void *)a[1];for(unsigned i=0;i<count;i++)d[i]=(struct __user_cap_data_struct){0};}result=0;break;
    }
    case SYS_vfork:
        /* Fork-like clone is safe inside the handler only with no child stack;
         * preserve libc vfork's nonstandard return-address registers. */
        links_fork(1);result=RAW(SYS_clone,SIGCHLD,0,0,0,0,0);if(result<0)links_fork(-1);break;
    case SYS_clone:
        /* Stackless forks can clone inside this handler. A supplied child
         * stack needs the assembly retry gateway to preserve its continuation. */
        if((a[0]&NS_FLAGS) && (a[0]&CLONE_VM)){result=-ENOTSUP;break;}
        if((a[0]&CLONE_NEWUSER) && !userns_remaining){result=-EPERM;break;}
        if(!(a[0]&CLONE_VM)){
            if(a[0]&CLONE_FILES){result=-ENOTSUP;break;}
            if(a[1])goto clone_retry;
            unsigned long ns=a[0]&NS_FLAGS;links_fork(1);
            result=RAW(SYS_clone,a[0]&~(long)(NS_FLAGS|CLONE_VFORK),0,a[2],a[3],a[4],0);
            if(result<0)links_fork(-1);
            if(!result){namespaces|=ns;if((ns&CLONE_NEWUSER) && userns_remaining!=~0U)userns_remaining--;}
            break;
        }
clone_retry:
        if(!(a[0]&CLONE_VM) || (a[0]&CLONE_VFORK))links_fork(1);
        namespace_clone_parent=RAW(SYS_getpid,0,0,0,0,0,0);namespace_clone_flags=a[0]&NS_FLAGS;
        uc->uc_mcontext.gregs[REG_RSI]=a[1];uc->uc_mcontext.gregs[REG_RDX]=a[2];
        uc->uc_mcontext.gregs[REG_R10]=a[3];uc->uc_mcontext.gregs[REG_R8]=a[4];
        uc->uc_mcontext.gregs[REG_RDI]=a[0]&~(long)(NS_FLAGS|CLONE_VFORK|((a[0]&CLONE_VFORK)?CLONE_VM:0));
        proxy_clone_resume=uc->uc_mcontext.gregs[REG_RIP];
        uc->uc_mcontext.gregs[REG_RAX]=nr;uc->uc_mcontext.gregs[REG_RIP]=(uintptr_t)proxy_clone_entry;
        proxy_log("ours-clone-retry",nr,0);goto clean_mask;
    case SYS_clone3:result=-ENOSYS;break; /* libc falls back to clone. */
    case SYS_execve:case SYS_execveat: {
        const char *path=(const char *)a[nr==SYS_execve?0:1];
        if(nr==SYS_execveat && (a[0]!=AT_FDCWD || a[4])) {result=-ENOSYS;break;}
        if(!thread) {result=-ENOMEM;break;}
        struct image image;
        result=proxy_load(path,(char *const *)a[nr==SYS_execve?1:2],(char *const *)a[nr==SYS_execve?2:3],&image);
        if(!result) {
            exec_close_fds();
            exec_signals();
            /* exec starts the new linker without the previous libc TLS. */
            RAW(SYS_arch_prctl,0x1002,0,0,0,0,0);
            proxy_log("ours-exec",nr,0);
            /* A real exec resets caught signal actions. Our resident action stays. */
            if(chained.handler!=1) chained.handler=0;
            if(dispatch_selector) {
                RAW(SYS_prctl,PR_SET_SYSCALL_USER_DISPATCH,PR_SYS_DISPATCH_OFF,0,0,0,0);
                dispatch_selector=NULL;dispatch_owner=0;
            }
            if(thread) thread->app_stack.ss_flags=SS_DISABLE;
            uc->uc_mcontext.gregs[REG_RIP]=image.entry;uc->uc_mcontext.gregs[REG_RSP]=image.stack;
            uc->uc_mcontext.gregs[REG_RDX]=0;
        }break;
    }
    case SYS_chdir:result=vfs_chdir(AT_FDCWD,(char *)a[0]);break;
    case SYS_fchdir:result=vfs_chdir((int)a[0],NULL);break;
    case SYS_open:case SYS_stat:case SYS_lstat:case SYS_access:case SYS_readlink:
    case SYS_mkdir:case SYS_unlink:case SYS_chmod:case SYS_chown:case SYS_lchown:case SYS_statfs:
        result=path_call(nr,a,0,-1);break;
    case SYS_openat2:result=-ENOSYS;break; /* bwrap uses its checked openat fallback. */
    case SYS_openat:case SYS_newfstatat:case SYS_statx:case SYS_faccessat:case SYS_faccessat2:
    case SYS_mknodat:case SYS_readlinkat:case SYS_mkdirat:case SYS_unlinkat:case SYS_fchmodat:case SYS_fchownat:case SYS_utimensat:
        result=path_call(nr,a,1,-1);break;
    case SYS_link:case SYS_rename:result=path_call(nr,a,0,1);break;
    case SYS_linkat:case SYS_renameat:case SYS_renameat2:result=path_call(nr,a,1,3);break;
    case SYS_symlink:result=path_call(nr,a,1,-1);break;
    case SYS_symlinkat:result=path_call(nr,a,2,-1);break;
    case SYS_getcwd:{long e=0;char *rev=vfs_getcwd(&e);result=e;
        if(rev){size_t k=p_len(rev)+1;if(k>(size_t)a[1])result=-ERANGE;else {p_copy((void *)a[0],rev,k);result=(long)k;}vfs_free(rev);}break;
    }
    default:break;
    }
finish:
    uc->uc_mcontext.gregs[REG_RAX]=result;
    if(proxy_trace)proxy_log(!app_allowed(original)?"android":(ours(original)?"ours":"android"),original,result);
clean_mask:
    /* sigreturn must never restore a blocked SIGSYS, including pthread masks. */
    (*(unsigned long *)&uc->uc_sigmask)&=~SIGSYS_BIT;
    RAW(SYS_sigaltstack,0,&uc->uc_stack,0,0,0,0);
    uc->uc_stack.ss_flags&=~SS_ONSTACK;
    if(selector && dispatch_selector==selector) *selector=saved;
}
static int app_install(void) {
    struct sock_filter bpf[1024];unsigned n=0;
#define ADD(code,jt,jf,k) do {bpf[n++]=(struct sock_filter)BPF_JUMP(code,k,jt,jf);} while(0)
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,arch));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,1,0,AUDIT_ARCH_X86_64);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_KILL_PROCESS);
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,nr));
    for(unsigned i=0;i<sizeof(android_allowed)/sizeof(android_allowed[0]);i++) {
        ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,android_allowed[i]);ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    }
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_TRAP);
    struct sock_fprog prog={(unsigned short)n,bpf};
    return (int)RAW(SYS_seccomp,SECCOMP_SET_MODE_FILTER,0,&prog,0,0,0);
}
int proxy_install(int android) {
    if(!thread_state()) return -ENOMEM;
    exec_signals();
    struct kernel_action action={(uintptr_t)handler,SA_SIGINFO|SA_RESTORER|SA_NODEFER|SA_ONSTACK,(uintptr_t)proxy_restore,0};
    long r=RAW(SYS_rt_sigaction,SIGSYS,&action,0,8,0,0);if(r) return (int)r;
    unsigned long mask=SIGSYS_BIT;r=RAW(SYS_rt_sigprocmask,SIG_UNBLOCK,&mask,0,8,0,0);if(r) return (int)r;
    r=RAW(SYS_prctl,PR_SET_NO_NEW_PRIVS,1,0,0,0,0);if(r) return (int)r;
    if(android) {emit("v3 installing android filter\n");r=app_install();if(r) return (int)r;emit("v3 android filter installed\n");}
    struct sock_filter bpf[1024];unsigned n=0;
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,arch));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,1,0,AUDIT_ARCH_X86_64);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_KILL_PROCESS);
    /* Linux payloads never get Binder, even though their Android stub does.
     * Put this kernel-enforced check BEFORE the raw-IP exemption: neither
     * translated ioctl nor a jump into proxy_raw may bypass it. DMA-BUF uses
     * the same type byte; preserve only its exact sync/fence encodings (the
     * IMPORT_SYNC_FILE collision is legacy Binder idle-timeout, not IPC). */
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,nr));
    /* x32 is unsupported, and must not skip the native ioctl guard. */
    ADD(BPF_JMP|BPF_JSET|BPF_K,0,1,0x40000000);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_KILL_PROCESS);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,7,SYS_ioctl);
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[1]));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,5,0,DMA_BUF_IOCTL_SYNC);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,4,0,DMA_BUF_IOCTL_EXPORT_SYNC_FILE);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,3,0,DMA_BUF_IOCTL_IMPORT_SYNC_FILE);
    ADD(BPF_ALU|BPF_AND|BPF_K,0,0,0xff00);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,0x6200);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ERRNO|EPERM);
    /* Only the raw syscall instruction is exempt. Kernel reports the IP after syscall. */
    uintptr_t ip=(uintptr_t)proxy_raw;
    /* Symbol immediately after the syscall: no instruction-size assumptions. */
    extern char proxy_raw_after[];
    ip=(uintptr_t)proxy_raw_after;
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,instruction_pointer)+4);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,3,(uint32_t)(ip>>32));
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,instruction_pointer));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,(uint32_t)ip);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    ip=(uintptr_t)proxy_clone_after;
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,instruction_pointer)+4);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,3,(uint32_t)(ip>>32));
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,instruction_pointer));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,(uint32_t)ip);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,nr));
    /* Forks must account for inherited temporary descriptors. Threads retry natively. */
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,5,SYS_clone);
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[0]));
    ADD(BPF_JMP|BPF_JSET|BPF_K,2,0,NS_FLAGS|CLONE_VFORK);
    ADD(BPF_JMP|BPF_JSET|BPF_K,0,1,CLONE_VM);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_TRAP);
    /* sendto without a destination stays native (both pointer halves). */
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,6,SYS_sendto);
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[4]));
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,3,0);
    ADD(BPF_LD|BPF_W|BPF_ABS,0,0,offsetof(struct seccomp_data,args[4])+4);
    ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,0);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_TRAP);
    for(long nr=0;nr<512;nr++) if(ours(nr) && nr!=SYS_clone && nr!=SYS_sendto) {
        ADD(BPF_JMP|BPF_JEQ|BPF_K,0,1,nr);ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_TRAP);
    }
    ADD(BPF_RET|BPF_K,0,0,SECCOMP_RET_ALLOW);
    struct sock_fprog prog={(unsigned short)n,bpf};
    return (int)RAW(SYS_seccomp,SECCOMP_SET_MODE_FILTER,0,&prog,0,0,0);
}
