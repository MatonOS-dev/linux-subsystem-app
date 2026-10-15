/* Linux layer v3. Resident state is inherited by fork; this is a spike,
 * NOT a security boundary. The original Android filter remains authoritative. */
#define _GNU_SOURCE
#define ARCH_WITH_CONTEXT
#include "proxy.h"
#include "state.h"
#include "vfs.h"
#include "net.h"
#include "links.h"
#include "sysvipc.h"
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
#include ARCH_POLICY_HEADER
#include "build/app-policy-dual.h"
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif
#ifndef SYS_USER_DISPATCH
#define SYS_USER_DISPATCH 2
#endif
#define SIGSYS_BIT (UINT64_C(1) << (SIGSYS-1))
#define SA_RESTORER 0x04000000
#define NS_FLAGS (CLONE_NEWNS|CLONE_NEWCGROUP|CLONE_NEWUTS|CLONE_NEWIPC|CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNET)
#ifdef HOST_COMMAND_TEST
static int hc_test_fd=-1;
#endif
static int private_close_fd(int fd){
#ifdef HOST_COMMAND_TEST
    if(fd==hc_test_fd)return 1;
#endif
    return vfs_private_fd(fd);
}
static struct kernel_action chained;
int proxy_diagnostics_init(void) {
    if(proxy_diagnostic_fd!=2)return 0;
    /* F_DUPFD keeps the original sink through resident exec, without CLOEXEC.
     * The VFS private-fd guard protects it from the payload's fd sweeps. */
    long fd=RAW(PROXY_SC_fcntl,2,F_DUPFD,256,0,0,0);
    if(fd<0)return (int)fd;
    proxy_diagnostic_fd=(int)fd;return 0;
}
/* Resident exec must discard libc handlers: bionic debuggerd uses bionic
 * TLS, which is no longer valid after entering a glibc image. */
static void crash_hex(uint64_t value) {
    char out[18]="0x";static const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<16;i++)out[2+i]=digits[(value>>(60-4*i))&15];
    RAW(PROXY_SC_write,proxy_diagnostic_fd,out,sizeof(out),0,0,0);
}
static void crash_text(const char *s){RAW(PROXY_SC_write,proxy_diagnostic_fd,s,p_len(s),0,0,0);}
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
    ucontext_t *uc=context;uintptr_t pc=ARCH_CONTEXT(uc,ARCH_PC);
    crash_text("v3 fatal signal=");crash_hex(sig);
    crash_text(" pid=");crash_hex(RAW(PROXY_SC_getpid,0,0,0,0,0,0));
    crash_text(" tid=");crash_hex(RAW(PROXY_SC_gettid,0,0,0,0,0,0));
    crash_text(" code=");crash_hex(info->si_code);
    crash_text(" addr=");crash_hex((uintptr_t)info->si_addr);
    crash_text(" pc=");crash_hex(pc);crash_text("\n");
    for(struct image_map *m=image_maps;m;m=m->next)if(pc>=m->lo && pc<m->hi){
        crash_text("v3 fatal image=");crash_text(m->path);crash_text(" elf-offset=");crash_hex(pc-m->lo+m->offset);crash_text("\n");break;
    }
    /* Raw maps: bypass virtual proc paths; no libc/TLS/allocation in a crash. */
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,"/proc/self/maps",O_RDONLY|O_CLOEXEC,0,0,0);
    if(fd>=0){char line[1024];size_t n=0;char c;
        while(RAW(PROXY_SC_read,fd,&c,1,0,0,0)==1){
            if(c!='\n'){if(n+1<sizeof(line))line[n++]=c;continue;}
            line[n]=0;const char *p=line;uintptr_t lo=maps_hex(&p),hi=0;
            if(*p=='-'){p++;hi=maps_hex(&p);}
            if(pc>=lo && pc<hi){crash_text("v3 fatal map=");crash_text(line);crash_text(" offset=");crash_hex(pc-lo);crash_text("\n");break;}n=0;
        }RAW(PROXY_SC_close,fd,0,0,0,0,0);
    }
    struct kernel_action action={0};RAW(PROXY_SC_rt_sigaction,sig,&action,0,8,0,0);
    proxy_sigmask_t mask=UINT64_C(1)<<(sig-1);RAW(PROXY_SC_rt_sigprocmask,SIG_UNBLOCK,&mask,0,8,0,0);
    RAW(PROXY_SC_tgkill,RAW(PROXY_SC_getpid,0,0,0,0,0,0),RAW(PROXY_SC_gettid,0,0,0,0,0,0),sig,0,0,0);
    RAW(PROXY_SC_exit_group,128+sig,0,0,0,0,0);
}
static void exec_signals(void) {
    for(int sig=1;sig<=64;sig++){
        if(sig==SIGKILL || sig==SIGSTOP || sig==SIGSYS)continue;
        struct kernel_action old={0},next={0};
        if(RAW(PROXY_SC_rt_sigaction,sig,0,&old,8,0,0)<0)continue;
        if(old.handler==1)continue; /* SIG_IGN survives exec. */
        if(sig==SIGSEGV || sig==SIGBUS || sig==SIGILL || sig==SIGFPE || sig==SIGABRT)
            next=(struct kernel_action){(uintptr_t)crash_report,SA_SIGINFO|SA_RESTORER|SA_ONSTACK,(uintptr_t)proxy_restore,0};
        RAW(PROXY_SC_rt_sigaction,sig,&next,0,8,0,0);
    }
}
/* Spike supports a single user-dispatch owner; configuration is per-thread. */
static volatile unsigned char *dispatch_selector;
static long dispatch_owner;
struct thread_state { long tid; stack_t app_stack; };
struct thread_node {struct thread_state state;struct thread_node *next;};
static struct thread_node *threads;
static struct thread_state *thread_state(void) {
    long tid=RAW(PROXY_SC_gettid,0,0,0,0,0,0);
    for(struct thread_node *p=threads;p;p=p->next) if(p->state.tid==tid)return &p->state;
    struct thread_node *p=vfs_alloc(sizeof(*p));if(!p)return NULL;
    p->state.tid=tid;p->state.app_stack.ss_flags=SS_DISABLE;
    stack_t current;
    if(!RAW(PROXY_SC_sigaltstack,0,&current,0,0,0,0) && !(current.ss_flags&SS_ONSTACK)) {
        long area=RAW(PROXY_SC_mmap,0,1<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if((unsigned long)area>=(unsigned long)-4095){vfs_free(p);return NULL;}
        stack_t own={.ss_sp=(void *)area,.ss_size=1<<20};
        if(RAW(PROXY_SC_sigaltstack,&own,0,0,0,0,0)){RAW(PROXY_SC_munmap,area,1<<20,0,0,0,0);vfs_free(p);return NULL;}
    }
    do {p->next=__atomic_load_n(&threads,__ATOMIC_ACQUIRE);}
    while(!__atomic_compare_exchange_n(&threads,&p->next,p,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED));
    return &p->state;
}
const char *proxy_tmpdir="/tmp";
/* --trace: log to the original diagnostic sink, independently of app stdio. */
int proxy_trace;
static void emit(const char *s) { RAW(PROXY_SC_write,proxy_diagnostic_fd,s,p_len(s),0,0,0); }
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
    unsigned long pid=(unsigned long)RAW(PROXY_SC_getpid,0,0,0,0,0,0);char digits[24];unsigned d=0;
    do{digits[d++]=(char)('0'+pid%10);pid/=10;}while(pid);while(d)line[n++]=digits[--d];line[n++]='\n';
    RAW(PROXY_SC_write,proxy_diagnostic_fd,line,n,0,0,0);
}
static int app_allowed(long nr) {
    for(unsigned i=0;i<sizeof(android_allowed)/sizeof(android_allowed[0]);i++)
        if(android_allowed[i]==arch_encode_syscall(nr)) return 1;
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
static long sysctl_open(int cloexec){long fd=RAW(PROXY_SC_memfd_create,"v3-userns-limit",cloexec?1:0,0,0,0,0);if(fd<0)return fd;struct sysctl_fd *p=vfs_alloc(sizeof(*p));if(!p){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -ENOMEM;}*p=(struct sysctl_fd){(int)fd,sysctl_fds};sysctl_fds=p;return fd;}
static long sysctl_write(int fd,const char *buf,size_t len){if(!len || len>11)return -EINVAL;unsigned limit=0;size_t k=0;for(;k<len && buf[k]>='0' && buf[k]<='9';k++){unsigned digit=buf[k]-'0';if(limit>(~0U-digit)/10)return -ERANGE;limit=limit*10+digit;}if(!k || (k<len && !(k+1==len && buf[k]=='\n')))return -EINVAL;long r=RAW(PROXY_SC_write,fd,buf,len,0,0,0);if(r>=0)userns_remaining=limit;return r;}

static int self_exe(const char *path){char name[64]="/proc/",digits[24];size_t k=6;unsigned d=0;unsigned long pid=(unsigned long)RAW(PROXY_SC_getpid,0,0,0,0,0,0);do{digits[d++]=(char)('0'+pid%10);pid/=10;}while(pid);while(d)name[k++]=digits[--d];p_copy(name+k,"/exe",5);return p_equal(path,name) || p_equal(path,"/proc/self/exe");}
static long path_call(long nr,proxy_arg_t *a,int one,int two) {
    if(!app_allowed(nr))return -ENOSYS;
    int at=one==1 || (nr==PROXY_SC_symlinkat && one==2);
    int fd=at?(int)a[one-1]:AT_FDCWD;
    int follow=nr!=PROXY_SC_readlinkat && nr!=PROXY_SC_readlink && nr!=PROXY_SC_unlinkat && nr!=PROXY_SC_renameat && nr!=PROXY_SC_renameat2 && nr!=PROXY_SC_symlinkat && nr!=PROXY_SC_mkdirat && nr!=PROXY_SC_mknodat;
    if(nr==PROXY_SC_newfstatat || nr==PROXY_SC_fchownat || nr==PROXY_SC_utimensat || nr==PROXY_SC_faccessat2)follow=!(a[nr==PROXY_SC_fchownat?4:3]&AT_SYMLINK_NOFOLLOW);
    if(nr==PROXY_SC_linkat)follow=(a[4]&AT_SYMLINK_FOLLOW)?VFS_FOLLOW_LINK_SOURCE:0;
    if(nr==PROXY_SC_statx)follow=!(a[2]&AT_SYMLINK_NOFOLLOW);
    if(nr==PROXY_SC_openat)follow=!(a[2]&O_NOFOLLOW) && (a[2]&(O_CREAT|O_EXCL))!=(O_CREAT|O_EXCL);
    /* Publish an unnamed inode via procfs: AT_EMPTY_PATH otherwise requires
     * CAP_DAC_READ_SEARCH. Resolve the destination and mount policy normally. */
    char empty_link[64];
    if(nr==PROXY_SC_linkat && a[1] && !*(char *)(uintptr_t)a[1] && (a[4]&AT_EMPTY_PATH)){
        char digits[24];size_t k=14;unsigned d=0;unsigned long n=(unsigned)fd;
        p_copy(empty_link,"/proc/self/fd/",14);
        do{digits[d++]=(char)('0'+n%10);n/=10;}while(n);
        while(d)empty_link[k++]=digits[--d];
        empty_link[k]=0;
        a[1]=(long)empty_link;a[0]=AT_FDCWD;fd=AT_FDCWD;
        a[4]=(a[4]&~AT_EMPTY_PATH)|AT_SYMLINK_FOLLOW;follow=VFS_FOLLOW_LINK_SOURCE;
    }
    /* Other empty-path operations act on the original descriptor. */
    if(a[one] && !*(char *)(uintptr_t)a[one] && at){long r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);if(!r && nr==PROXY_SC_newfstatat)links_payload_fd_stat(fd,(void *)(uintptr_t)a[2]);if(!r && nr==PROXY_SC_statx){struct proxy_stat st;if(!arch_stat_fd(fd,&st)){links_fd_stat(fd,&st);arch_statx_nlink((void *)(uintptr_t)a[4],st.st_nlink);}}return r;}
    const char *original_path=(void *)(uintptr_t)a[one];
    if(nr==PROXY_SC_openat && original_path){long synthetic=vfs_proc_open(original_path);if(synthetic!=-ENOSYS)return synthetic;}
    long e=0;void *pm=NULL,*qm=NULL;char *p=vfs_resolve_mount(fd,(char *)(uintptr_t)a[one],follow,&e,&pm);if(!p){if(proxy_trace && a[one]){const char *in=(const char *)(uintptr_t)a[one];RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 unresolved ",14,0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,in,p_len(in),0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,"\n",1,0,0,0);}return e;}
    char *q=NULL;
    if(two>=0){q=vfs_resolve_mount(at?(int)a[two-1]:AT_FDCWD,(char *)(uintptr_t)a[two],0,&e,&qm);if(!q)goto done;if(pm!=qm){e=-EXDEV;goto done;}a[two]=(long)q;if(at)a[two-1]=AT_FDCWD;}
    if(nr==PROXY_SC_openat){e=vfs_proc_open(p);if(e!=-ENOSYS)goto done;}
    int writing=nr==PROXY_SC_mkdirat || nr==PROXY_SC_unlinkat || nr==PROXY_SC_renameat || nr==PROXY_SC_renameat2 || nr==PROXY_SC_linkat || nr==PROXY_SC_symlinkat || nr==PROXY_SC_fchmodat || nr==PROXY_SC_fchownat || nr==PROXY_SC_utimensat || nr==PROXY_SC_mknodat || (nr==PROXY_SC_openat && (a[2]&(O_WRONLY|O_RDWR|O_CREAT|O_TRUNC)));
    if(nr==PROXY_SC_mkdirat){struct proxy_stat existing;if(!arch_stat_at(AT_FDCWD,p,&existing,AT_SYMLINK_NOFOLLOW)){e=-EEXIST;goto done;}}
    /* Exclusive creation checks existence before mount write permission.
     * bwrap probes an already bound file this way before mounting a new
     * source over it. Never open/truncate the read-only backing inode. */
    if(nr==PROXY_SC_openat && (a[2]&(O_CREAT|O_EXCL))==(O_CREAT|O_EXCL)){
        struct proxy_stat existing;
        if(!arch_stat_at(AT_FDCWD,p,&existing,AT_SYMLINK_NOFOLLOW)){e=-EEXIST;goto done;}
    }
    if(writing && ((e=vfs_mount_readonly(pm)) || (q && (e=vfs_mount_readonly(qm)))))goto done;
    if((namespaces&CLONE_NEWUSER) && nr==PROXY_SC_openat && (a[2]&(O_WRONLY|O_RDWR)) && p_equal(p,"/proc/sys/user/max_user_namespaces")){e=sysctl_open((a[2]&O_CLOEXEC)!=0);goto done;}
    /* bwrap reads the overflow IDs for its uid maps; Android's policy denies
     * apps the generic proc_sysctl file. Serve the kernel default (65534). */
    if(nr==PROXY_SC_openat && !(a[2]&(O_WRONLY|O_RDWR)) && (p_equal(p,"/proc/sys/kernel/overflowuid") || p_equal(p,"/proc/sys/kernel/overflowgid"))){
        long fd=RAW(PROXY_SC_memfd_create,"v3-overflowid",(a[2]&O_CLOEXEC)?1:0,0,0,0,0);
        if(fd>=0 && (RAW(PROXY_SC_write,fd,"65534\n",6,0,0,0)!=6 || RAW(PROXY_SC_lseek,fd,0,SEEK_SET,0,0,0)!=0)){RAW(PROXY_SC_close,fd,0,0,0,0,0);fd=-EIO;}
        e=fd;goto done;
    }
    if(namespaces && nr==PROXY_SC_openat && (a[2]&(O_WRONLY|O_RDWR))){
        size_t n=p_len(p);const char *tail=p;while(*tail){if(*tail=='/')tail++;else break;}int proc=p[0]=='/' && p[1]=='p' && p[2]=='r' && p[3]=='o' && p[4]=='c' && p[5]=='/';
        if(proc && ((n>=8 && p_equal(p+n-8,"/uid_map")) || (n>=8 && p_equal(p+n-8,"/gid_map")) || (n>=10 && p_equal(p+n-10,"/setgroups")))){e=RAW(PROXY_SC_memfd_create,"v3-namespace-map",(a[2]&O_CLOEXEC)?1:0,0,0,0,0);goto done;}
    }
    /* bwrap unlinks --bind-data/--ro-bind-data temp files right after binding;
     * a real mount keeps the inode, our virtual one needs the name. */
    if(nr==PROXY_SC_unlinkat && !(a[2]&AT_REMOVEDIR) && vfs_is_mount_source(p)){e=0;goto done;}
    /* bwrap --dev needs /dev/full, which Android apps may not open: serve
     * /dev/zero (reads identical; writes succeed instead of ENOSPC). */
    if(p_equal(p,"/dev/full")) {
        struct proxy_stat probe;
        if(arch_stat_at(AT_FDCWD,"/dev/full",&probe,0)==-EACCES) {
            char *zero=vfs_alloc(10);
            if(!zero){e=-ENOMEM;goto done;}
            p_copy(zero,"/dev/zero",10);vfs_free(p);p=zero;
        }
    }
    /* Android mounts devpts with ptmxmode=000: apps use /dev/ptmx, the same
     * multiplexer for this devpts instance. bwrap links /dev/ptmx to
     * pts/ptmx, so serve opens of that node from /dev/ptmx. */
    if(nr==PROXY_SC_openat && p_equal(p,"/dev/pts/ptmx")) {
        struct proxy_stat probe;
        if(!arch_stat_at(AT_FDCWD,"/dev/pts/ptmx",&probe,0) && !(probe.st_mode&0777)) {
            char *ptmx=vfs_alloc(10);
            if(!ptmx){e=-ENOMEM;goto done;}
            p_copy(ptmx,"/dev/ptmx",10);vfs_free(p);p=ptmx;
        }
    }
    if(proxy_trace){RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 path ",8,0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,p,p_len(p),0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,"\n",1,0,0,0);}
    a[one]=(long)p;if(at)a[one-1]=AT_FDCWD;
    if(nr==PROXY_SC_readlinkat){char *tracked=vfs_proc_fd_name(p);if(tracked){size_t n=p_len(tracked);if(n>(size_t)a[3])n=(size_t)a[3];p_copy((void *)(uintptr_t)a[2],tracked,n);vfs_free(tracked);e=(long)n;goto done;}}
    if(nr==PROXY_SC_readlinkat && self_exe(p)){char *exe=vfs_exe();if(exe){size_t n=p_len(exe);if(n>(size_t)a[3])n=(size_t)a[3];p_copy((void *)(uintptr_t)a[2],exe,n);vfs_free(exe);e=(long)n;goto done;}}
    if(nr==PROXY_SC_openat && (a[2]&O_TMPFILE)==O_TMPFILE)e=links_open(p,a[2],a[3]);
    else if(nr==PROXY_SC_linkat)e=links_publish(p,q,a[4]);
    else if(nr==PROXY_SC_unlinkat || nr==PROXY_SC_renameat || nr==PROXY_SC_renameat2)e=links_name_call(nr,a,p,q);
    else if(nr==PROXY_SC_symlinkat || nr==PROXY_SC_mkdirat || nr==PROXY_SC_mknodat)e=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
    else e=links_path_call(nr,a,p);
    if(e>=0 && nr==PROXY_SC_openat)vfs_fd_record((int)e,fd,original_path);
    if(e>=0 && nr==PROXY_SC_readlinkat){char *raw=vfs_alloc((size_t)e+1);if(!raw){e=-ENOMEM;goto done;}p_copy(raw,(void *)(uintptr_t)a[2],(size_t)e);raw[e]=0;
        /* Only proc magic links expose backing names. Ordinary symlink targets
         * retain their stored text, as required by readlink. */
        if(p_len(p)>=6 && p[0]=='/' && p[1]=='p' && p[2]=='r' && p[3]=='o' && p[4]=='c' && p[5]=='/' && raw[0]=='/'){
            char *rev=vfs_reverse(raw);if(rev){size_t n=p_len(rev);if(n>(size_t)a[3])n=(size_t)a[3];p_copy((void *)(uintptr_t)a[2],rev,n);e=(long)n;vfs_free(rev);}}
        vfs_free(raw);
    }
done:
    /* Hardware passthrough is "where convenient": a host /dev node the app's
     * sandbox may not use is reported absent, so bwrap/Flatpak *-try binds and
     * device probes skip it instead of failing on EACCES. */
    if(p && p[0]=='/' && p[1]=='d' && p[2]=='e' && p[3]=='v' && p[4]=='/') {
        if(e==-EACCES && (nr==PROXY_SC_newfstatat || nr==PROXY_SC_statx || nr==PROXY_SC_faccessat2 || nr==PROXY_SC_faccessat ||
                (nr==PROXY_SC_openat && (a[2]&O_PATH))))e=-ENOENT;
        /* O_PATH opens need no permission, so the denial would only surface at
         * the caller's fstat (bwrap's bind source check): probe it here. */
        if(e>=0 && nr==PROXY_SC_openat && (a[2]&O_PATH)) {
            struct proxy_stat probe;
            if(arch_stat_fd(e,&probe)==-EACCES){RAW(PROXY_SC_close,e,0,0,0,0,0);e=-ENOENT;}
        }
        /* Existence checks (access F_OK) need no permission either; Flatpak
         * uses them to pick a mandatory --dev-bind, so answer consistently. */
        if(e==0 && (nr==PROXY_SC_faccessat || nr==PROXY_SC_faccessat2)) {
            struct proxy_stat probe;
            if(arch_stat_at(AT_FDCWD,p,&probe,0)==-EACCES)e=-ENOENT;
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
        RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 bwrap argv ",14,0,0,0);
        RAW(PROXY_SC_write,proxy_diagnostic_fd,argv[i],p_len(argv[i]),0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,"\n",1,0,0,0);
        if(!p_equal(argv[i],"--args") || !argv[i+1])continue;
        const char *n=argv[i+1];unsigned fd=0;int valid=*n!=0;
        for(;*n;n++){if(*n<'0'||*n>'9'||fd>(2147483647U-(unsigned)(*n-'0'))/10){valid=0;break;}fd=fd*10+(*n-'0');}
        if(!valid)continue;
        RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 bwrap bundle begin\n",sizeof("v3 bwrap bundle begin\n")-1,0,0,0);
        char bytes[4096];long offset=0,count;
        while((count=RAW(PROXY_SC_pread64,fd,bytes,sizeof(bytes),offset,0,0))>0){
            for(long j=0;j<count;j++)if(!bytes[j])bytes[j]='\n';
            RAW(PROXY_SC_write,proxy_diagnostic_fd,bytes,count,0,0,0);offset+=count;
        }
        RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 bwrap bundle end\n",sizeof("v3 bwrap bundle end\n")-1,0,0,0);
    }
}
static long dns_poll(void *,unsigned long,struct timespec *,const void *,unsigned long);
static long modernize(long nr,proxy_arg_t *a,long *answer) {
    switch(nr){
    case PROXY_SC_access:a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_faccessat;
    case PROXY_SC_open:a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_openat;
    case PROXY_SC_creat:a[3]=a[1];a[2]=O_CREAT|O_WRONLY|O_TRUNC;a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_openat;
    case PROXY_SC_stat:case PROXY_SC_lstat:a[3]=nr==PROXY_SC_lstat?AT_SYMLINK_NOFOLLOW:0;a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_newfstatat;
    case PROXY_SC_mkdir:a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_mkdirat;
    case PROXY_SC_rmdir:case PROXY_SC_unlink:a[2]=nr==PROXY_SC_rmdir?AT_REMOVEDIR:0;a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_unlinkat;
    case PROXY_SC_rename:case PROXY_SC_link:a[3]=a[1];a[1]=a[0];a[0]=a[2]=AT_FDCWD;a[4]=0;return nr==PROXY_SC_link?PROXY_SC_linkat:PROXY_SC_renameat;
    case PROXY_SC_symlink:a[2]=a[1];a[1]=AT_FDCWD;return PROXY_SC_symlinkat;
    case PROXY_SC_readlink:a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_readlinkat;
    case PROXY_SC_chmod:a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_fchmodat;
    case PROXY_SC_chown:case PROXY_SC_lchown:a[4]=nr==PROXY_SC_lchown?AT_SYMLINK_NOFOLLOW:0;a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_fchownat;
    case PROXY_SC_mknod:a[3]=a[2];a[2]=a[1];a[1]=a[0];a[0]=AT_FDCWD;return PROXY_SC_mknodat;
    case PROXY_SC_io_pgetevents:{
        struct aio_mask {proxy_sigmask_t *mask;size_t size;};struct aio_mask *m=(void *)(uintptr_t)a[5];proxy_sigmask_t old=0;
        if(m && m->mask){if(m->size!=8){*answer=-EINVAL;return -1;}proxy_sigmask_t next=*m->mask&~SIGSYS_BIT;*answer=RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&next,&old,8,0,0);if(*answer)return -1;}
        *answer=RAW(PROXY_SC_io_getevents,a[0],a[1],a[2],a[3],a[4],0);
        if(m && m->mask)RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0);
        return -1;
    }
    case PROXY_SC_accept:a[3]=0;return PROXY_SC_accept4;
    case PROXY_SC_pause:*answer=RAW(PROXY_SC_ppoll,0,0,0,0,8,0);return -1;
    case PROXY_SC_futimesat:{struct timespec times[2],*tp=NULL;if(a[2]){struct timeval *tv=(void *)(uintptr_t)a[2];tp=times;for(int i=0;i<2;i++){times[i].tv_sec=tv[i].tv_sec;times[i].tv_nsec=tv[i].tv_usec*1000;}}proxy_arg_t b[6]={a[0],a[1],(long)tp,0,0,0};*answer=a[1]?path_call(PROXY_SC_utimensat,b,1,-1):RAW(PROXY_SC_utimensat,a[0],0,(long)tp,0,0,0);return -1;}
    case PROXY_SC_poll:{struct timespec ts,*t=NULL;if((int)a[2]>=0){ts.tv_sec=(int)a[2]/1000;ts.tv_nsec=((int)a[2]%1000)*1000000;t=&ts;}*answer=dns_poll((void *)(uintptr_t)a[0],a[1],t,NULL,8);return -1;}
    case PROXY_SC_select:{struct timespec ts,*t=NULL;struct timeval *tv=(void *)(uintptr_t)a[4];if(tv){ts.tv_sec=tv->tv_sec;ts.tv_nsec=tv->tv_usec*1000;t=&ts;}*answer=RAW(PROXY_SC_pselect6,a[0],a[1],a[2],a[3],t,0);if(tv){tv->tv_sec=ts.tv_sec;tv->tv_usec=ts.tv_nsec/1000;}return -1;}
    case PROXY_SC_alarm:{struct itimerval next={0},old;next.it_value.tv_sec=(unsigned)a[0];*answer=RAW(PROXY_SC_setitimer,ITIMER_REAL,&next,&old,0,0,0);if(!*answer)*answer=old.it_value.tv_sec+(old.it_value.tv_usec!=0);return -1;}
    case PROXY_SC_utime:case PROXY_SC_utimes:{struct timespec times[2],*tp=NULL;if(a[1]){tp=times;if(nr==PROXY_SC_utime){long *v=(void *)(uintptr_t)a[1];for(int i=0;i<2;i++){times[i].tv_sec=v[i];times[i].tv_nsec=0;}}else {struct timeval *v=(void *)(uintptr_t)a[1];for(int i=0;i<2;i++){times[i].tv_sec=v[i].tv_sec;times[i].tv_nsec=v[i].tv_usec*1000;}}}proxy_arg_t b[6]={AT_FDCWD,a[0],(long)tp,0,0,0};*answer=path_call(PROXY_SC_utimensat,b,1,-1);return -1;}
    case PROXY_SC_getdents:{
        /* linux_dirent64 -> legacy x86_64 linux_dirent, including d_type
         * in the last byte. Consume at most caller capacity, no dropped entries. */
        void *buf=vfs_alloc((size_t)a[2]);if(!buf){*answer=-ENOMEM;return -1;}long n=links_dents(a[0],buf,a[2]);long used=0;
        if(n>0)for(long off=0;off<n;){unsigned char *p=(unsigned char *)buf+off;unsigned short len=*(unsigned short *)(p+16);if(len<20 || off+len>n){n=-EIO;break;}unsigned char *q=(unsigned char *)a[1]+used;p_copy(q,p,18);size_t name=p_len((char *)p+19)+1;p_copy(q+18,p+19,name);for(size_t k=18+name;k<len;k++)q[k]=0;q[len-1]=p[18];used+=len;off+=len;}
        *answer=n<0?n:used;vfs_free(buf);return -1;
    }
    case PROXY_SC_pipe:a[1]=0;return PROXY_SC_pipe2;
    case PROXY_SC_dup2:
        if(a[0]==a[1]){*answer=RAW(PROXY_SC_fcntl,a[0],F_GETFD,0,0,0,0);if(*answer>=0)*answer=a[0];return -1;}a[2]=0;return PROXY_SC_dup3;
    case PROXY_SC_getpgrp:a[0]=0;return PROXY_SC_getpgid;
    case PROXY_SC_epoll_create:if(a[0]<=0){*answer=-EINVAL;return -1;}a[0]=0;return PROXY_SC_epoll_create1;
    case PROXY_SC_epoll_wait:a[4]=0;a[5]=8;return PROXY_SC_epoll_pwait;
    case PROXY_SC_inotify_init:a[0]=0;return PROXY_SC_inotify_init1;
    case PROXY_SC_eventfd:a[1]=0;return PROXY_SC_eventfd2;
    case PROXY_SC_signalfd:a[3]=0;return PROXY_SC_signalfd4;
    case PROXY_SC_fork:a[0]=SIGCHLD;a[1]=a[2]=a[3]=a[4]=0;return PROXY_SC_clone;
    case PROXY_SC_time:{struct timespec ts;*answer=RAW(PROXY_SC_clock_gettime,0,&ts,0,0,0,0);if(!*answer){*answer=ts.tv_sec;if(a[0])*(long *)(uintptr_t)a[0]=ts.tv_sec;}return -1;}
    default:return nr;
    }
}
/* DNS transport lives on the application's original fd numbers. */
#include "dns-unix.h"
/* Filesystem AF_UNIX addresses are paths: translate them through the virtual
 * mount table like any path call (Wayland, X11, PulseAudio, D-Bus sockets).
 * Abstract and unnamed addresses pass through. Returns 1 when handled. */
static int unix_call(long nr,proxy_arg_t a[6],long *result) {
    if(nr!=PROXY_SC_connect && nr!=PROXY_SC_bind)return 0;
    const struct sockaddr_un *in=(const void *)a[1];
    long len=a[2];
    if(proxy_trace && in && len>2 && len<=(long)sizeof(*in) && in->sun_family==AF_UNIX){
        size_t limit=(size_t)len-offsetof(struct sockaddr_un,sun_path);
        crash_text("v3 unix ");
        if(in->sun_path[0]) {
            size_t n=0;while(n<limit && in->sun_path[n])n++;
            RAW(PROXY_SC_write,proxy_diagnostic_fd,in->sun_path,n,0,0,0);
        } else {
            /* Abstract names are binary, including embedded/trailing NULs. */
            static const char hex[]="0123456789abcdef";
            for(size_t n=0;n<limit;n++) {
                unsigned char c=(unsigned char)in->sun_path[n];
                char escaped[4]={'\\','x',hex[c>>4],hex[c&15]};
                RAW(PROXY_SC_write,proxy_diagnostic_fd,escaped,sizeof(escaped),0,0,0);
            }
        }
        crash_text("\n");
    }
    if(!in || len<=(long)offsetof(struct sockaddr_un,sun_path) || len>(long)sizeof(*in) || in->sun_family!=AF_UNIX || !in->sun_path[0])return 0;
    char given[sizeof(in->sun_path)+1];size_t n=(size_t)len-offsetof(struct sockaddr_un,sun_path);
    p_copy(given,in->sun_path,n);given[n]=0;
    long e=0;char *real=vfs_resolve(AT_FDCWD,given,nr==PROXY_SC_connect,&e);
    if(!real){*result=e;return 1;}
    if(nr==PROXY_SC_connect && (namespaces&CLONE_NEWNET) && dns_path[0]) {
        size_t base=p_len(dns_path);
        if(p_len(real)==base+4) {
            int same=1;for(size_t i=0;i<base;i++)if(real[i]!=dns_path[i]){same=0;break;}
            if(same && (p_equal(real+base,".udp") || p_equal(real+base,".tcp"))){vfs_free(real);*result=-EACCES;return 1;}
        }
    }
    if(p_equal(real,given)){vfs_free(real);return 0;}
    struct sockaddr_un out;p_copy(&out,in,offsetof(struct sockaddr_un,sun_path));out.sun_family=AF_UNIX;
    size_t k=p_len(real);long dir=-1;
    if(k<sizeof(out.sun_path)) {
        p_copy(out.sun_path,real,k+1);
    } else {
        /* Too long for sun_path: reach it through an O_PATH handle on its dir. */
        char *slash=real+k;while(slash>real && *slash!='/')slash--;
        if(slash==real){vfs_free(real);*result=-ENAMETOOLONG;return 1;}
        *slash=0;dir=RAW(PROXY_SC_openat,AT_FDCWD,real,O_PATH|O_DIRECTORY|O_CLOEXEC,0,0,0);*slash='/';
        if(dir<0){vfs_free(real);*result=dir;return 1;}
        char via[sizeof(out.sun_path)];size_t m=0;const char *pre="/proc/self/fd/";while(*pre)via[m++]=*pre++;
        char digits[24];int d=0;unsigned long v=(unsigned long)dir;do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);while(d)via[m++]=digits[--d];
        size_t tail=p_len(slash);
        if(m+tail>=sizeof(via)){RAW(PROXY_SC_close,dir,0,0,0,0,0);vfs_free(real);*result=-ENAMETOOLONG;return 1;}
        p_copy(via+m,slash,tail+1);p_copy(out.sun_path,via,m+tail+1);k=m+tail;
    }
    vfs_free(real);
    *result=RAW(nr,a[0],(long)&out,(long)(offsetof(struct sockaddr_un,sun_path)+k+1),0,0,0);
    if(dir>=0)RAW(PROXY_SC_close,dir,0,0,0,0,0);
    return 1;
}
/* glibc >= 2.42 sets terminal attributes with the termios2 ioctls, which
 * Android's app policy does not allow on ptys (unpriv_tty_ioctls). termios2 is
 * termios plus two trailing speed fields; serve them with the classic calls.
 * Arbitrary speeds (BOTHER) mean nothing on a pty; they become B38400. */
#include "arch/termios.h"
/* No existing fault-safe pointer helper exists. Let pipe copy_{from,to}_user
 * check both ends; unlike /proc/self/mem this respects read-only mappings.
 * A termios2 fits atomically in an empty pipe; nonblocking avoids any wait. */
long proxy_copy(void *dst,const void *src,size_t size) {
    int fd[2];long r=RAW(PROXY_SC_pipe2,fd,O_CLOEXEC|O_NONBLOCK,0,0,0,0);
    if(r<0)return r;
    r=RAW(PROXY_SC_write,fd[1],src,size,0,0,0);
    if(r==(long)size)r=RAW(PROXY_SC_read,fd[0],dst,size,0,0,0);
    RAW(PROXY_SC_close,fd[0],0,0,0,0,0);RAW(PROXY_SC_close,fd[1],0,0,0,0,0);
    return r==(long)size?0:r<0?r:-EFAULT;
}
static int termios2_call(proxy_arg_t *a,long *result) {
    unsigned long cmd=(unsigned long)a[1]&0xffffffffu;
    struct proxy_termios2 copy={0};
    if(cmd==V3_TCGETS2){
        long r=RAW(PROXY_SC_ioctl,a[0],0x5401,&copy,0,0,0);
        if(!r){unsigned speed=copy.c_cflag&0010017;copy.c_ispeed=copy.c_ospeed=speed==0?0:38400;
            r=proxy_copy((void *)(uintptr_t)a[2],&copy,sizeof(copy));}
        *result=r;return 1;
    }
    if(cmd!=V3_TCSETS2 && cmd!=V3_TCSETSW2 && cmd!=V3_TCSETSF2)return 0;
    *result=proxy_copy(&copy,(void *)(uintptr_t)a[2],sizeof(copy));if(*result)return 1;
    if((copy.c_cflag&0010017)==0010000)copy.c_cflag=(copy.c_cflag&~0010017u)|0000017;
    *result=RAW(PROXY_SC_ioctl,a[0],cmd==V3_TCSETS2?0x5402:cmd==V3_TCSETSW2?0x5403:0x5404,(long)&copy,0,0,0);
    return 1;
}
/* close_range is present in the app filter: intercept it as well as close.
 * Enumerating proc avoids a UINT_MAX loop and preserves private descriptors. */
static long payload_close_range(unsigned first,unsigned last,unsigned flags) {
    if(first>last || (flags&~6u))return -EINVAL;
    if(flags&2u)return -ENOSYS; /* let libc retry without CLOSE_RANGE_UNSHARE */
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,"/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,0,0);
    if(fd<0)return fd;
    char buf[4096];long n;
    while((n=RAW(PROXY_SC_getdents64,fd,buf,sizeof(buf),0,0,0))>0){
        for(long off=0;off<n;){unsigned short len=*(unsigned short *)(buf+off+16);
            if(len<20 || off+len>n){n=-EIO;goto done;}
            const char *p=buf+off+19;off+=len;unsigned value=0;
            if(*p<'0'||*p>'9')continue;
            while(*p>='0'&&*p<='9'){value=value*10+(*p++-'0');}
            if(*p || value<first || value>last || value==(unsigned)fd || private_close_fd((int)value))continue;
            if(flags&4u)RAW(PROXY_SC_fcntl,value,F_SETFD,FD_CLOEXEC,0,0,0);
            else {RAW(PROXY_SC_close,value,0,0,0,0,0);dns_close(value);links_close(value);vfs_fd_close(value);net_close(value);sysctl_close(value);}
        }
    }
done:
    RAW(PROXY_SC_close,fd,0,0,0,0,0);return n<0?n:0;
}
static int ours(long n) {
    if(ipc_ours(n))return 1;
    if(n==PROXY_SC_ppoll)return dns_path[0]!=0;
    switch(n) {
    case PROXY_SC_epoll_ctl:case PROXY_SC_ioctl:case PROXY_SC_socket:case PROXY_SC_bind:case PROXY_SC_getsockname:case PROXY_SC_sendto:case PROXY_SC_sendmsg:case PROXY_SC_recvfrom:case PROXY_SC_recvmsg:case PROXY_SC_connect:case PROXY_SC_getpeername:case PROXY_SC_sendmmsg:case PROXY_SC_setsockopt:case PROXY_SC_getsockopt:
    case PROXY_SC_read:case PROXY_SC_pread64:case PROXY_SC_pwrite64:case PROXY_SC_readv:case PROXY_SC_writev:case PROXY_SC_mmap:case PROXY_SC_ftruncate:case PROXY_SC_lseek:
    case PROXY_SC_close_range:case PROXY_SC_flock:case PROXY_SC_fstat:case PROXY_SC_getdents64:case PROXY_SC_exit:case PROXY_SC_exit_group:case PROXY_SC_write:case PROXY_SC_close:case PROXY_SC_dup:case PROXY_SC_dup3:case PROXY_SC_fcntl:
    case PROXY_SC_capget:case PROXY_SC_capset:case PROXY_SC_setreuid:case PROXY_SC_setregid:case PROXY_SC_setfsuid:case PROXY_SC_setfsgid:case PROXY_SC_setgroups:case PROXY_SC_setuid:case PROXY_SC_setgid:case PROXY_SC_setresuid:case PROXY_SC_setresgid:
    case PROXY_SC_utime:case PROXY_SC_utimes:case PROXY_SC_creat:case PROXY_SC_rmdir:case PROXY_SC_mknod:case PROXY_SC_mknodat:
    case PROXY_SC_prctl:case PROXY_SC_execve:case PROXY_SC_execveat:case PROXY_SC_rt_sigaction:case PROXY_SC_rt_sigprocmask:
    case PROXY_SC_sigaltstack:case PROXY_SC_unshare:case PROXY_SC_mount:case PROXY_SC_umount2:case PROXY_SC_pivot_root:
    case PROXY_SC_vfork:case PROXY_SC_clone:case PROXY_SC_clone3:case PROXY_SC_io_uring_setup:case PROXY_SC_io_uring_enter:case PROXY_SC_io_uring_register:
    case PROXY_SC_open:case PROXY_SC_openat:case PROXY_SC_openat2:case PROXY_SC_stat:case PROXY_SC_lstat:case PROXY_SC_newfstatat:
    case PROXY_SC_statx:case PROXY_SC_access:case PROXY_SC_faccessat:case PROXY_SC_faccessat2:case PROXY_SC_readlink:case PROXY_SC_readlinkat:
    case PROXY_SC_fchdir:case PROXY_SC_chdir:case PROXY_SC_getcwd:case PROXY_SC_mkdir:case PROXY_SC_mkdirat:case PROXY_SC_unlink:case PROXY_SC_unlinkat:
    case PROXY_SC_rename:case PROXY_SC_renameat:case PROXY_SC_renameat2:case PROXY_SC_link:case PROXY_SC_linkat:
    case PROXY_SC_symlink:case PROXY_SC_symlinkat:case PROXY_SC_chmod:case PROXY_SC_fchmodat:case PROXY_SC_chown:case PROXY_SC_lchown:
    case PROXY_SC_fchownat:case PROXY_SC_utimensat:case PROXY_SC_statfs:case PROXY_SC_chroot:return 1;
    default:return 0;
    }
}
static void dispatch_app(int sig,siginfo_t *info,void *ctx) {
    struct kernel_action action=chained;
    if(action.handler==1 && info->si_code<=0) return;
    if(action.handler<=1) { /* SIGSYS default is fatal, including ignored synchronous SIGSYS. */
        RAW(PROXY_SC_exit_group,128+sig,0,0,0,0,0);return;
    }
    if(action.flags&SA_RESETHAND) chained.handler=0;
    /* Keep SIGSYS deliverable even during app chaining. Do not implement SA_ONSTACK
     * by switching stacks here; the proxy never depends on an app-owned altstack. */
    proxy_sigmask_t old=0,mask=action.mask&~SIGSYS_BIT;
    RAW(PROXY_SC_rt_sigprocmask,SIG_BLOCK,&mask,&old,8,0,0);
    if(action.flags&SA_SIGINFO) ((void (*)(int,siginfo_t *,void *))action.handler)(sig,info,ctx);
    else ((void (*)(int))action.handler)(sig);
    old&=~SIGSYS_BIT;RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0);
}
static void exec_close_fds(void){
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,"/proc/self/fd",O_DIRECTORY|O_RDONLY|O_CLOEXEC,0,0,0);if(fd<0)return;
    char *buf=vfs_alloc(8192);if(!buf){RAW(PROXY_SC_close,fd,0,0,0,0,0);return;}
    for(;;){long n=RAW(PROXY_SC_getdents64,fd,buf,8192,0,0,0);if(n<=0)break;
        for(long off=0;off<n;){unsigned short len=*(unsigned short *)(buf+off+16);if(len<20 || off+len>n)break;char *name=buf+off+19;long v=0;int valid=*name!=0;for(;*name;name++){if(*name<'0'||*name>'9'){valid=0;break;}v=v*10+*name-'0';}
            if(valid && v!=fd){long flags=RAW(PROXY_SC_fcntl,v,F_GETFD,0,0,0,0);if(flags>=0 && (flags&FD_CLOEXEC)){RAW(PROXY_SC_close,v,0,0,0,0,0);dns_close((int)v);links_close((int)v);vfs_fd_close((int)v);net_close((int)v);sysctl_close((int)v);}}off+=len;
        }
    }
    vfs_free(buf);RAW(PROXY_SC_close,fd,0,0,0,0,0);
}
#include "host-command.c"

static void handler(int sig,siginfo_t *info,void *opaque) {
    ucontext_t *uc=opaque;
    volatile unsigned char *selector=dispatch_selector;
    unsigned char saved=0;
    if(selector) {saved=*selector;*selector=SYSCALL_DISPATCH_FILTER_ALLOW;}
    /* The retried clone inherits this pending record. Only its child adopts
     * new namespace flags; the parent's monitor keeps the original namespace. */
    if(namespace_clone_parent){
        if(RAW(PROXY_SC_getpid,0,0,0,0,0,0)!=namespace_clone_parent){
            ipc_fork();
            namespaces|=namespace_clone_flags;
            if((namespace_clone_flags&CLONE_NEWUSER) && userns_remaining!=~0U)userns_remaining--;
        }
        namespace_clone_parent=0;namespace_clone_flags=0;
    }
    struct thread_state *thread=thread_state();
    if(info->si_code!=SYS_SECCOMP) {
        /* SYS_USER_DISPATCH belongs to the app, never to this syscall proxy. */
        dispatch_app(sig,info,opaque);(*arch_signal_mask(uc))&=~SIGSYS_BIT;
        RAW(PROXY_SC_sigaltstack,0,arch_signal_stack(uc),0,0,0,0);
        arch_signal_stack_ready(uc);
        if(selector && dispatch_selector==selector) *selector=saved;
        return;
    }
    long nr,original;
    proxy_arg_t a[6]={ARCH_CONTEXT(uc,ARCH_ARG0),ARCH_CONTEXT(uc,ARCH_ARG1),
        ARCH_CONTEXT(uc,ARCH_ARG2),ARCH_CONTEXT(uc,ARCH_ARG3),
        ARCH_CONTEXT(uc,ARCH_ARG4),ARCH_CONTEXT(uc,ARCH_ARG5)};
    struct arch_call call=arch_decode_call(info->si_syscall,a);
    nr=original=call.id;
    long result=-ENOSYS;
    if(call.error){result=call.error;goto finish;}
    /* signalfd readiness is evaluated against the thread's signal state.
     * Infinite poll must wait outside the SIGSYS frame, with the application's
     * mask restored, rather than nesting the wait in a signal handler. */
    if(nr==PROXY_SC_poll && (int)a[2]<0){
        ARCH_CONTEXT(uc,ARCH_RESULT)=arch_encode_syscall(PROXY_SC_ppoll);
        ARCH_CONTEXT(uc,ARCH_ARG2)=0;ARCH_CONTEXT(uc,ARCH_ARG3)=0;
        ARCH_CONTEXT(uc,ARCH_ARG4)=8;arch_restart_syscall(uc);
        proxy_log("android-poll-retry",nr,0);goto clean_mask;
    }
    /* Metadata/ownership descriptors are capabilities, never payload I/O. */
    switch(nr){
    case PROXY_SC_mmap:if(a[4]>=0 && vfs_private_fd(a[4])){result=-EBADF;goto finish;}break;
    case PROXY_SC_read:case PROXY_SC_write:case PROXY_SC_pread64:case PROXY_SC_pwrite64:
    case PROXY_SC_readv:case PROXY_SC_writev:case PROXY_SC_ftruncate:case PROXY_SC_lseek:
    case PROXY_SC_fstat:case PROXY_SC_getdents64:
        if(vfs_private_fd(a[0])){result=-EBADF;goto finish;}break;
    default:break;
    }
    nr=modernize(nr,a,&result);
    if(nr<0)goto finish;
    if(ipc_ours(nr)){result=ipc_call(nr,a);goto finish;}
    int net_handled=0;result=net_call(nr,a,(namespaces&CLONE_NEWNET)!=0,&net_handled);
    if(net_handled)goto finish;
    result=-ENOSYS;
    if(!ours(nr)) {if(app_allowed(nr))result=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);goto finish;}
    switch(nr) {
    case PROXY_SC_read:case PROXY_SC_pread64:case PROXY_SC_pwrite64:case PROXY_SC_readv:case PROXY_SC_writev:case PROXY_SC_mmap:case PROXY_SC_ftruncate:case PROXY_SC_lseek:
        result=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);break;
    case PROXY_SC_ppoll:if(app_allowed(nr))result=dns_poll((void *)(uintptr_t)a[0],a[1],(void *)(uintptr_t)a[2],(void *)(uintptr_t)a[3],a[4]);break;
    case PROXY_SC_flock:result=vfs_private_fd((int)a[0])?-EBADF:(app_allowed(nr)?RAW(nr,a[0],a[1],0,0,0,0):-ENOSYS);break;
    case PROXY_SC_getdents64:result=links_dents(a[0],(void *)(uintptr_t)a[1],a[2]);break;
    case PROXY_SC_fstat:result=RAW(nr,a[0],a[1],0,0,0,0);if(!result)links_payload_fd_stat((int)a[0],(void *)(uintptr_t)a[1]);break;
    case PROXY_SC_exit:ipc_thread_exit();result=RAW(nr,a[0],0,0,0,0,0);break;
    case PROXY_SC_exit_group:if(proxy_trace)proxy_log("exit-status",nr,a[0]);ipc_exit();links_exit();result=RAW(nr,a[0],0,0,0,0,0);break;
    case PROXY_SC_write:result=sysctl_tracked((int)a[0])?sysctl_write((int)a[0],(void *)(uintptr_t)a[1],a[2]):RAW(nr,a[0],a[1],a[2],0,0,0);break;
    case PROXY_SC_close_range:result=payload_close_range((unsigned)a[0],(unsigned)a[1],(unsigned)a[2]);break;
    case PROXY_SC_close:if(private_close_fd((int)a[0])){result=0;break;}result=RAW(nr,a[0],0,0,0,0,0);if(!result){dns_close((int)a[0]);links_close((int)a[0]);vfs_fd_close((int)a[0]);net_close((int)a[0]);sysctl_close((int)a[0]);}break;
    case PROXY_SC_dup:case PROXY_SC_dup3:case PROXY_SC_fcntl:
        if(vfs_private_fd((int)a[0])||(nr==PROXY_SC_dup3&&vfs_private_fd((int)a[1]))){result=-EBADF;break;}
        result=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
        if(result>=0 && (nr!=PROXY_SC_fcntl || a[1]==F_DUPFD || a[1]==F_DUPFD_CLOEXEC)){long tracked=links_dup((int)a[0],(int)result);if(tracked<0){RAW(PROXY_SC_close,result,0,0,0,0,0);links_close((int)result);dns_close((int)result);vfs_fd_close((int)result);net_close((int)result);sysctl_close((int)result);result=tracked;break;}dns_dup((int)a[0],(int)result);vfs_fd_dup((int)a[0],(int)result);net_dup((int)a[0],(int)result);sysctl_dup((int)a[0],(int)result);}
        break;
    case PROXY_SC_epoll_ctl:case PROXY_SC_ioctl:case PROXY_SC_socket:case PROXY_SC_bind:case PROXY_SC_getsockname:case PROXY_SC_sendto:case PROXY_SC_sendmsg:case PROXY_SC_recvfrom:case PROXY_SC_recvmsg:case PROXY_SC_connect:case PROXY_SC_getpeername:case PROXY_SC_sendmmsg:case PROXY_SC_setsockopt:case PROXY_SC_getsockopt:
        if(nr==PROXY_SC_ioctl && vfs_private_fd((int)a[0])){result=-EBADF;break;}
        if(nr==PROXY_SC_ioctl && termios2_call(a,&result)){if(proxy_trace)proxy_log("tty-ioctl",a[1],result);break;}
        if(app_allowed(nr))result=dns_call(nr,a);
        if(nr==PROXY_SC_ioctl && proxy_trace)proxy_log("tty-ioctl",a[1],result);
        break;
    case PROXY_SC_prctl:
        if(a[0]==PR_CAPBSET_DROP || a[0]==PR_CAP_AMBIENT || a[0]==PR_SET_KEEPCAPS) {result=0;break;}
        if(a[0]==PR_CAPBSET_READ){result=0;break;}
        if(a[0]==PR_SET_SYSCALL_USER_DISPATCH) {
            long tid=RAW(PROXY_SC_gettid,0,0,0,0,0,0);
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
    case PROXY_SC_rt_sigaction:
        if(a[0]==SIGSYS) {
            if(a[3]!=8) {result=-EINVAL;break;}
            struct kernel_action next=chained;
            if(a[1]) p_copy(&next,(void *)(uintptr_t)a[1],sizeof(next));
            if(a[2]) p_copy((void *)(uintptr_t)a[2],&chained,sizeof(chained));
            if(a[1]) {next.mask&=~SIGSYS_BIT;chained=next;}result=0;
        } else {
            struct kernel_action action;
            if(a[1]) {p_copy(&action,(void *)(uintptr_t)a[1],sizeof(action));action.mask&=~SIGSYS_BIT;a[1]=(long)&action;}
            result=RAW(nr,a[0],a[1],a[2],a[3],0,0);
        }break;
    case PROXY_SC_rt_sigprocmask: {
        if(a[3]!=8) {result=-EINVAL;break;}
        proxy_sigmask_t mask;
        if(a[1]) {p_copy(&mask,(void *)(uintptr_t)a[1],8);mask&=~SIGSYS_BIT;a[1]=(long)&mask;}
        result=RAW(nr,a[0],a[1],a[2],8,0,0);
        if(!result && a[1])RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,0,&(*arch_signal_mask(uc)),8,0,0);
        break;
    }
    case PROXY_SC_sigaltstack: {
        /* App altstack changes are virtual: the kernel retains our private stack.
         * The chained app handler's SA_ONSTACK is currently unsupported. */
        if(!thread) {result=-ENOMEM;break;}
        stack_t next=thread->app_stack;
        if(a[0]) {
            p_copy(&next,(void *)(uintptr_t)a[0],sizeof(next));
            if(next.ss_flags & ~(SS_DISABLE|(1U<<31))) {result=-EINVAL;break;}
            if(!(next.ss_flags&SS_DISABLE) && next.ss_size<2048) {result=-ENOMEM;break;}
        }
        if(a[1]) p_copy((void *)(uintptr_t)a[1],&thread->app_stack,sizeof(stack_t));
        if(a[0]) thread->app_stack=next;
        result=0;break;
    }
    case PROXY_SC_unshare:
        result=(a[0]&~(long)NS_FLAGS)?-ENOSYS:0;
        if(!result && (a[0]&CLONE_NEWUSER) && !userns_remaining)result=-EPERM;
        if(!result){namespaces|=a[0];if((a[0]&CLONE_NEWUSER) && userns_remaining!=~0U)userns_remaining--;}
        break;
    case PROXY_SC_mount:result=vfs_mount((char *)(uintptr_t)a[0],(char *)(uintptr_t)a[1],(char *)(uintptr_t)a[2],(unsigned long)a[3]);break;
    case PROXY_SC_umount2:result=vfs_unmount((char *)(uintptr_t)a[0]);break;
    case PROXY_SC_pivot_root:result=vfs_pivot((char *)(uintptr_t)a[0],(char *)(uintptr_t)a[1]);break;
    case PROXY_SC_chroot:result=vfs_root((char *)(uintptr_t)a[0]);break;
    case PROXY_SC_setreuid:case PROXY_SC_setregid:case PROXY_SC_setfsuid:case PROXY_SC_setfsgid:case PROXY_SC_setgroups:case PROXY_SC_setuid:case PROXY_SC_setgid:case PROXY_SC_setresuid:case PROXY_SC_setresgid:result=0;break;
    case PROXY_SC_capset:result=0;break;
    case PROXY_SC_capget:{struct __user_cap_header_struct *h=(void *)(uintptr_t)a[0];if(!h){result=-EFAULT;break;}
        unsigned count=h->version==_LINUX_CAPABILITY_VERSION_1?1:2;
        if(h->version!=_LINUX_CAPABILITY_VERSION_1 && h->version!=_LINUX_CAPABILITY_VERSION_2 && h->version!=_LINUX_CAPABILITY_VERSION_3){h->version=_LINUX_CAPABILITY_VERSION_3;result=-EINVAL;break;}
        if(a[1]){struct __user_cap_data_struct *d=(void *)(uintptr_t)a[1];for(unsigned i=0;i<count;i++)d[i]=(struct __user_cap_data_struct){0};}result=0;break;
    }
    case PROXY_SC_vfork:
        /* Fork-like clone is safe inside the handler only with no child stack;
         * preserve libc vfork's nonstandard return-address registers. */
        result=ipc_fork_prepare();if(result)break;
        links_fork(1);dns_fork_prepare();result=RAW(PROXY_SC_clone,SIGCHLD,0,0,0,0,0);dns_fork_result(result);ipc_fork_result(result);if(result<0)links_fork(-1);if(!result)ipc_fork();break;
    case PROXY_SC_clone:
        /* Stackless forks can clone inside this handler. A supplied child
         * stack needs the assembly retry gateway to preserve its continuation. */
        if((a[0]&NS_FLAGS) && (a[0]&CLONE_VM)){result=-ENOTSUP;break;}
        if((a[0]&CLONE_NEWUSER) && !userns_remaining){result=-EPERM;break;}
        if(!(a[0]&CLONE_VM)){
            if(a[0]&CLONE_FILES){result=-ENOTSUP;break;}
            if(a[1])goto clone_retry;
            unsigned long ns=a[0]&NS_FLAGS;result=ipc_fork_prepare();if(result)break;links_fork(1);
            dns_fork_prepare();
            result=RAW(PROXY_SC_clone,a[0]&~(long)(NS_FLAGS|CLONE_VFORK),0,a[2],a[3],a[4],0);
            dns_fork_result(result);ipc_fork_result(result);
            if(result<0)links_fork(-1);
            if(!result){ipc_fork();namespaces|=ns;if((ns&CLONE_NEWUSER) && userns_remaining!=~0U)userns_remaining--;}
            break;
        }
clone_retry:
        if(!(a[0]&CLONE_VM) || (a[0]&CLONE_VFORK)){result=ipc_fork_prepare();if(result)break;links_fork(1);dns_fork_prepare();(*arch_signal_mask(uc))=~SIGSYS_BIT;}
        namespace_clone_parent=RAW(PROXY_SC_getpid,0,0,0,0,0,0);namespace_clone_flags=a[0]&NS_FLAGS;
        ARCH_CONTEXT(uc,ARCH_ARG1)=a[1];ARCH_CONTEXT(uc,ARCH_ARG2)=a[2];
        ARCH_CONTEXT(uc,ARCH_ARG3)=a[3];ARCH_CONTEXT(uc,ARCH_ARG4)=a[4];
        ARCH_CONTEXT(uc,ARCH_ARG0)=a[0]&~(long)(NS_FLAGS|CLONE_VFORK|((a[0]&CLONE_VFORK)?CLONE_VM:0));
        proxy_clone_resume=ARCH_CONTEXT(uc,ARCH_PC);
        ARCH_CONTEXT(uc,ARCH_RESULT)=arch_encode_syscall(nr);ARCH_CONTEXT(uc,ARCH_PC)=(uintptr_t)proxy_clone_entry;
        proxy_log("ours-clone-retry",nr,0);goto clean_mask;
    case PROXY_SC_clone3:result=-ENOSYS;break; /* libc falls back to clone. */
    case PROXY_SC_execve:case PROXY_SC_execveat: {
        const char *path=(const char *)(uintptr_t)a[nr==PROXY_SC_execve?0:1];
        if(nr==PROXY_SC_execveat && (a[0]!=AT_FDCWD || a[4])) {result=-ENOSYS;break;}
        if(!thread) {result=-ENOMEM;break;}
        if(proxy_trace){emit("v3 exec ");emit(path);emit("\n");}
        struct image image;
        result=proxy_load(path,(char *const *)(uintptr_t)a[nr==PROXY_SC_execve?1:2],(char *const *)(uintptr_t)a[nr==PROXY_SC_execve?2:3],&image);
        if(!result) {
            vfs_network_payload((namespaces&CLONE_NEWNET)!=0);
            if(namespaces&CLONE_NEWNET)dns_network_payload();
            proxy_host_snapshot((char *const *)(uintptr_t)a[nr==PROXY_SC_execve?2:3]);
            ipc_exec();
            exec_close_fds();
            exec_signals();
            /* exec starts the new linker without the previous libc TLS. */
            ARCH_CLEAR_TLS();
            proxy_log("ours-exec",nr,0);
            /* A real exec resets caught signal actions. Our resident action stays. */
            if(chained.handler!=1) chained.handler=0;
            if(dispatch_selector) {
                RAW(PROXY_SC_prctl,PR_SET_SYSCALL_USER_DISPATCH,PR_SYS_DISPATCH_OFF,0,0,0,0);
                dispatch_selector=NULL;dispatch_owner=0;
            }
            if(thread) thread->app_stack.ss_flags=SS_DISABLE;
            ARCH_CONTEXT(uc,ARCH_PC)=image.entry;ARCH_CONTEXT(uc,ARCH_STACK)=image.stack;
            arch_exec_context(uc);
        }break;
    }
    case PROXY_SC_chdir:result=vfs_chdir(AT_FDCWD,(char *)(uintptr_t)a[0]);break;
    case PROXY_SC_fchdir:result=vfs_chdir((int)a[0],NULL);break;
    case PROXY_SC_open:case PROXY_SC_stat:case PROXY_SC_lstat:case PROXY_SC_access:case PROXY_SC_readlink:
    case PROXY_SC_mkdir:case PROXY_SC_unlink:case PROXY_SC_chmod:case PROXY_SC_chown:case PROXY_SC_lchown:case PROXY_SC_statfs:
        result=path_call(nr,a,0,-1);break;
    case PROXY_SC_openat2:result=-ENOSYS;break; /* bwrap uses its checked openat fallback. */
    /* futimens(fd): a NULL path names the fd itself; there is no path to map. */
    case PROXY_SC_utimensat:if(!a[1]){result=app_allowed(nr)?RAW(PROXY_SC_utimensat,a[0],0,a[2],a[3],0,0):-ENOSYS;break;}result=path_call(nr,a,1,-1);break;
    case PROXY_SC_openat:case PROXY_SC_newfstatat:case PROXY_SC_statx:case PROXY_SC_faccessat:case PROXY_SC_faccessat2:
    case PROXY_SC_mknodat:case PROXY_SC_readlinkat:case PROXY_SC_mkdirat:case PROXY_SC_unlinkat:case PROXY_SC_fchmodat:case PROXY_SC_fchownat:
        result=path_call(nr,a,1,-1);break;
    case PROXY_SC_link:case PROXY_SC_rename:result=path_call(nr,a,0,1);break;
    case PROXY_SC_linkat:case PROXY_SC_renameat:case PROXY_SC_renameat2:result=path_call(nr,a,1,3);break;
    case PROXY_SC_symlink:result=path_call(nr,a,1,-1);break;
    case PROXY_SC_symlinkat:result=path_call(nr,a,2,-1);break;
    case PROXY_SC_getcwd:{long e=0;char *rev=vfs_getcwd(&e);result=e;
        if(rev){size_t k=p_len(rev)+1;if(k>(size_t)a[1])result=-ERANGE;else {p_copy((void *)(uintptr_t)a[0],rev,k);result=(long)k;}vfs_free(rev);}break;
    }
    default:break;
    }
finish:
    ARCH_CONTEXT(uc,ARCH_RESULT)=arch_finish_call(call,result);
    if(proxy_trace)proxy_log(!app_allowed(original)?"android":(ours(original)?"ours":"android"),original,result);
clean_mask:
    /* sigreturn must never restore a blocked SIGSYS, including pthread masks. */
    (*arch_signal_mask(uc))&=~SIGSYS_BIT;
    RAW(PROXY_SC_sigaltstack,0,arch_signal_stack(uc),0,0,0,0);
    arch_signal_stack_ready(uc);
    if(selector && dispatch_selector==selector) *selector=saved;
}
#include ARCH_FILTER_HEADER
int proxy_install(int android) {
    ipc_init(); /* IPC failure must never prevent payload startup. */
    int diagnostics=proxy_diagnostics_init();if(diagnostics)return diagnostics;
    if(!thread_state()) return -ENOMEM;
    exec_signals();
    struct kernel_action action={(uintptr_t)handler,SA_SIGINFO|SA_RESTORER|SA_NODEFER|SA_ONSTACK,(uintptr_t)proxy_restore,0};
    long r=RAW(PROXY_SC_rt_sigaction,SIGSYS,&action,0,8,0,0);if(r) return (int)r;
    proxy_sigmask_t mask=SIGSYS_BIT;r=RAW(PROXY_SC_rt_sigprocmask,SIG_UNBLOCK,&mask,0,8,0,0);if(r) return (int)r;
    r=RAW(PROXY_SC_prctl,PR_SET_NO_NEW_PRIVS,1,0,0,0,0);if(r) return (int)r;
    if(android) {emit("v3 installing android filter\n");r=arch_app_install();if(r) return (int)r;emit("v3 android filter installed\n");}
    return arch_filter_install();
}


void proxy_state(struct state *s){
 extern const char *proxy_tmpdir;
 chained.handler=state_word(s,chained.handler==1?1:0);
 proxy_trace=state_word(s,proxy_trace);proxy_diagnostic_fd=state_word(s,proxy_diagnostic_fd);
 namespaces=state_word(s,namespaces);userns_remaining=state_word(s,userns_remaining);
 state_string(s,(char **)&proxy_tmpdir);
 state_bytes(s,&dns_endpoint,sizeof(dns_endpoint));state_bytes(s,dns_path,sizeof(dns_path));
 state_string(s,(char **)&host_command_socket);host_command_started=state_word(s,host_command_started);
 uint64_t count=0;for(struct dns_fd *d=dns_fds;d;d=d->next)if(state_live(d->fd))count++;
 count=state_word(s,count);if(count>65536){s->error=-E2BIG;return;}
 if(!s->reading){
  for(struct dns_fd *d=dns_fds;d;d=d->next)if(state_live(d->fd)){
   int alias=-1;for(struct dns_fd *p=dns_fds;p!=d;p=p->next)if(p->socket==d->socket&&state_live(p->fd)){alias=p->fd;break;}
   state_word(s,d->fd);state_word(s,(int64_t)alias);
   if(alias<0){state_word(s,d->socket->type);state_word(s,d->socket->active);state_word(s,d->socket->connected);state_bytes(s,&d->socket->local,sizeof(d->socket->local));}
  }
 }else for(uint64_t i=0;i<count&&!s->error;i++){
  int fd=state_word(s,0),alias=state_word(s,0);
  if(alias>=0){dns_dup(alias,fd);if(!dns_lookup(fd))s->error=-EINVAL;continue;}
  struct dns_fd *d=vfs_alloc(sizeof(*d));struct dns_socket *socket=vfs_alloc(sizeof(*socket));
  if(!d||!socket){s->error=-ENOMEM;return;}
  socket->refs=1;socket->type=state_word(s,0);socket->active=state_word(s,0);socket->connected=state_word(s,0);state_bytes(s,&socket->local,sizeof(socket->local));
  *d=(struct dns_fd){fd,socket,dns_fds};dns_fds=d;
 }
 count=0;for(struct dns_watch *w=dns_watches;w;w=w->next)if(state_live(w->fd)&&state_live(w->epoll))count++;
 count=state_word(s,count);if(count>65536){s->error=-E2BIG;return;}
 if(!s->reading){for(struct dns_watch *w=dns_watches;w;w=w->next)if(state_live(w->fd)&&state_live(w->epoll)){state_word(s,w->fd);state_word(s,w->epoll);state_word(s,w->event.events);state_word(s,w->event.data.u64);}}
 else for(uint64_t i=0;i<count&&!s->error;i++){
  struct dns_watch *w=vfs_alloc(sizeof(*w));if(!w){s->error=-ENOMEM;return;}
  w->fd=state_word(s,0);w->epoll=state_word(s,0);w->event.events=state_word(s,0);w->event.data.u64=state_word(s,0);w->next=dns_watches;dns_watches=w;
 }
 count=0;for(struct sysctl_fd *d=sysctl_fds;d;d=d->next)if(state_live(d->fd))count++;
 count=state_word(s,count);if(count>65536){s->error=-E2BIG;return;}
 if(!s->reading){for(struct sysctl_fd *d=sysctl_fds;d;d=d->next)if(state_live(d->fd))state_word(s,d->fd);}
 else for(uint64_t i=0;i<count&&!s->error;i++){struct sysctl_fd *d=vfs_alloc(sizeof(*d));if(!d){s->error=-ENOMEM;return;}d->fd=state_word(s,0);d->next=sysctl_fds;sysctl_fds=d;}
}
