/* Linux layer v3: resident ELF loader. The proxy payload path is libc-free. */
#include "proxy.h"
#include "state.h"
#include "vfs.h"
#include "links.h"
#if defined(__i386__)
#include "arch/i386/sysinfo.h"
#define PROXY_SYSINFO_AUX(inherited) proxy_i386_sysinfo_value(inherited)
#else
#define PROXY_SYSINFO_AUX(inherited) (inherited)
#endif
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sys/prctl.h>
#include <signal.h>
#include <sys/stat.h>
#ifdef MATON_GPU_BRIDGE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#if defined(__i386__)
#include "arch/i386/adopt.h"
#else
#include "arch/x86_64/adopt.h"
#include "../gpu-forward-spike/spike3/adopt.h"
#endif
#include "../gpu-forward-spike/bootstrap.h"
#include "../gpu-forward-spike/bridge.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
static struct maton_gpu_bootstrap *gpu_bootstrap;
#ifndef PROXY_LIBRARY
static const char *gpu_bridge_error(void) { return dlerror(); }
#endif
#endif

#define PAGE 4096UL
#define DOWN(x) ((x)&~(PAGE-1))
#define UP(x) (((x)+PAGE-1)&~(PAGE-1))
#define STACK_SIZE (4UL<<20)
/* A staged image: ET_DYN is one contiguous span; ET_EXEC keeps the kernel's
 * per-segment layout, one span per run of touching PT_LOAD pages, so the gaps
 * between them (wine-preloader: 0x08048000 and 0x7d400000) are neither
 * reserved, checked for collisions nor moved over live mappings. */
struct span { uintptr_t low, area, target, size; };
struct mapped { uintptr_t entry, phdr, base; struct span *spans; unsigned count, phnum; char interp[4096]; };
static void release_spans(struct mapped *m,int unmap){
    for(unsigned i=0;unmap && i<m->count;i++)RAW(PROXY_SC_munmap,m->spans[i].area,m->spans[i].size,0,0,0,0);
    vfs_free(m->spans);m->spans=NULL;m->count=0;
}
static int resident_collision(uintptr_t low,uintptr_t high) {
    if(proxy_native_overlap(low,high-low))return 1;
#if defined(__i386__) && defined(MATON_GPU_BRIDGE)
    /* ELF32 PIE is relocated into the kernel's available address space. Its
     * link-time 0x70000000 layout is not its live reservation after ASLR. */
    extern const unsigned char __ehdr_start[];
    const arch_elf_ehdr *eh=(const void *)__ehdr_start;
    const arch_elf_phdr *ph=(const void *)(__ehdr_start+eh->e_phoff);
    uintptr_t bias=0;
    for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD && !ph[i].p_offset) {
        bias=(uintptr_t)__ehdr_start-ph[i].p_vaddr;break;
    }
    for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD) {
        uintptr_t begin=DOWN(bias+ph[i].p_vaddr);
        uintptr_t end=UP(bias+ph[i].p_vaddr+ph[i].p_memsz);
        if(high>begin && low<end)return 1;
    }
    return 0;
#else
    return high>ARCH_RESIDENT_LO && low<ARCH_RESIDENT_HI;
#endif
}

/* Keep the Android-visible launch process alive if Flatpak/bwrap's direct
 * child exits while a forked Linux app process is still starting.  The
 * supervisor becomes a child subreaper, then runs the ordinary loader in a
 * child. Orphaned descendants are reparented here and reaped before this
 * process reports the application's final status to Process.waitFor(). */
#ifndef PROXY_LIBRARY
static void supervisor_pid_text(char out[24],long value) {
    char reverse[24];unsigned n=0;
    do { reverse[n++]=(char)('0'+value%10);value/=10; } while(value && n<sizeof(reverse));
    unsigned i=0;while(n)out[i++]=reverse[--n];out[i]=0;
}
static void strip_supervise_option(int argc,char **argv) {
    for(int i=1;i<argc;i++)argv[i]=argv[i+1];
}
/* SIGKILL every process this subreaper adopted, until none is left. A child's
 * own children reparent here as it dies, so repeat until waitid sees none. */
static void supervisor_kill_tree(long self) {
    char buf[2048];long killed=0;
    for(int round=0;;round++) {
        long found=0,dir=RAW(PROXY_SC_openat,AT_FDCWD,"/proc",O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,0,0);
        if(dir<0){proxy_log("fail supervisor-kill /proc",dir,self);return;}
        long got;
        while((got=RAW(PROXY_SC_getdents64,dir,buf,sizeof(buf),0,0,0))>0)
            for(long off=0;off<got;) {
                struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *e=(void *)(buf+off);off+=e->reclen;
                long pid=0;const char *c=e->name;
                for(;*c>='0'&&*c<='9';c++)pid=pid*10+*c-'0';
                if(*c || !pid || pid==self)continue;
                char path[48]="/proc/",stat[512];char *o=path+6;supervisor_pid_text(o,pid);o+=p_len(o);p_copy(o,"/stat",6);
                long f=RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY|O_CLOEXEC,0,0,0);if(f<0)continue;
                long n=RAW(PROXY_SC_read,f,stat,sizeof(stat)-1,0,0,0);RAW(PROXY_SC_close,f,0,0,0,0,0);if(n<=0)continue;
                stat[n]=0;char *close=0;for(char *q=stat;*q;q++)if(*q==')')close=q;
                if(!close || close[1]!=' ' || !close[2] || close[3]!=' ')continue;
                long ppid=0;for(c=close+4;*c>='0'&&*c<='9';c++)ppid=ppid*10+*c-'0';
                if(ppid!=self)continue;
                if(!RAW(PROXY_SC_kill,pid,SIGKILL,0,0,0,0)){found++;killed++;}
            }
        RAW(PROXY_SC_close,dir,0,0,0,0,0);
        /* Each killed child becomes reapable after handing its own children
         * to us; reap that many, then rescan for the new orphans. */
        siginfo_t info;
        for(long reaped=0;reaped<found;) {
            long waited=RAW(PROXY_SC_waitid,0,0,&info,4,0,0); /* P_ALL, WEXITED */
            if(waited==-EINTR)continue;
            if(waited<0)break;
            reaped++;
        }
        if(!found && RAW(PROXY_SC_waitid,0,0,&info,4|1,0,0)==-ECHILD)break; /* WNOHANG */
        if(round>4096){proxy_log("fail supervisor-kill descendants keep forking",killed,self);break;}
    }
    proxy_log("supervisor-terminate killed descendants",killed,self);
}
static int supervise_launch(int argc,char **argv,char **environ) {
    if(argc<2 || !p_equal(argv[1],"--supervise"))return -1;

    for(int i=2;i<argc;i++)if(p_equal(argv[i],"--trace")){proxy_trace=1;break;}

    long expected_parent=0;
    for(int i=2;i+1<argc;i++)if(p_equal(argv[i],"--parent-pid")) {
        const char *number=argv[i+1];if(!*number)return 2;
        for(;*number;number++) {
            if(*number<'0'||*number>'9'||expected_parent>214748364)return 2;
            expected_parent=expected_parent*10+*number-'0';
        }
        if(expected_parent<=0)return 2;
        break;
    }
    long setup=RAW(PROXY_SC_prctl,PR_SET_CHILD_SUBREAPER,1,0,0,0,0);
    if(setup<0) {
        strip_supervise_option(argc,argv);
        return (int)RAW(PROXY_SC_execve,argv[0],argv,environ,0,0,0)==0?0:2;
    }
    /* SIGTERM, not SIGKILL: the supervisor must outlive the stub long enough
     * to kill the Linux processes it adopted (see supervisor_kill_tree). */
    if(expected_parent &&
       (RAW(PROXY_SC_prctl,PR_SET_PDEATHSIG,SIGTERM,0,0,0,0)<0 ||
        RAW(PROXY_SC_getppid,0,0,0,0,0,0)!=expected_parent))return 2;

    long supervisor=RAW(PROXY_SC_getpid,0,0,0,0,0,0);
    /* Android's inherited app filter allows clone(SIGCHLD), not fork. */
    long child=RAW(PROXY_SC_clone,SIGCHLD,0,0,0,0,0);
    if(child<0)return 2;
    if(proxy_trace)proxy_log("supervisor-launch",supervisor,child);
    if(child==0) {
        char supervisor_pid[32];
        supervisor_pid_text(supervisor_pid,(long)supervisor);
        strip_supervise_option(argc,argv);
        for(int i=1;i+1<argc;i++)if(p_equal(argv[i],"--parent-pid"))argv[i+1]=supervisor_pid;
        RAW(PROXY_SC_execve,argv[0],argv,environ,0,0,0);
        return 127;
    }

    int app_status=2,main_reaped=0;
    if(proxy_trace)proxy_log("supervisor-wait-main",child,supervisor);
    /* The stub ends the session (Quit, or a subsystem update deleting the APK
     * every Linux process runs from) by SIGTERM, directly or as the PDEATHSIG
     * of the stub process. Adopted descendants would otherwise escape to init
     * and keep running from the old install, so kill the whole tree.
     * Signals arrive through a signalfd; the child above keeps a clean mask.
     * Only our direct/adopted children are reaped; payload parents retain
     * their own children's wait status. */
    uint64_t mask=(UINT64_C(1)<<(SIGCHLD-1))|(UINT64_C(1)<<(SIGTERM-1))|(UINT64_C(1)<<(SIGHUP-1))|(UINT64_C(1)<<(SIGINT-1));
    long sfd=RAW(PROXY_SC_rt_sigprocmask,SIG_BLOCK,&mask,0,8,0,0);
    if(!sfd)sfd=RAW(PROXY_SC_signalfd4,-1,&mask,8,O_CLOEXEC,0,0);
    if(sfd<0){proxy_log("fail supervisor-signalfd",sfd,supervisor);RAW(PROXY_SC_kill,child,SIGKILL,0,0,0,0);supervisor_kill_tree(supervisor);return 2;}
    for(;;) {
        /* Drain first: a child may have exited before SIGCHLD was blocked. */
        for(;;) {
            siginfo_t info={0};
            long waited=RAW(PROXY_SC_waitid,0,0,&info,4|1,0,0); /* P_ALL, WEXITED|WNOHANG */
            if(waited==-EINTR)continue;
            if(waited==-ECHILD)return main_reaped?app_status:2;
            if(waited<0){proxy_log("fail supervisor-waitid",waited,supervisor);return 2;}
            if(!info.si_pid)break;
            long status=info.si_code==CLD_EXITED?(long)info.si_status<<8:
                        info.si_status|(info.si_code==CLD_DUMPED?0x80:0);
            if(info.si_pid==child) {
                main_reaped=1;
                app_status=info.si_code==CLD_EXITED?info.si_status:128+info.si_status;
                if(proxy_trace)proxy_log("supervisor-main-status",child,status);
            } else if(proxy_trace)proxy_log("supervisor-descendant-status",info.si_pid,status);
        }
        uint32_t signal[32];
        long n=RAW(PROXY_SC_read,sfd,signal,sizeof(signal),0,0,0);
        if(n==-EINTR)continue;
        if(n!=(long)sizeof(signal)){proxy_log("fail supervisor-signalfd-read",n,supervisor);return 2;}
        if(signal[0]==SIGCHLD)continue;
        proxy_log("supervisor-terminate signal",(long)signal[0],supervisor);
        supervisor_kill_tree(supervisor);
        return 128+(int)signal[0];
    }
}
#endif

/* Resident workers must not consume a payload parent's SIGCHLD. Linux
 * signalfd observes process-pending signals, but an unblocked EGL/Binder
 * thread can take (and discard, with SIG_DFL) that notification first.
 * Create native workers with CHLD blocked, then restore the launch thread's
 * mask so the payload retains ordinary Linux signal semantics. */
int proxy_resident_signals_begin(uint64_t *previous) {
    uint64_t child=UINT64_C(1)<<(SIGCHLD-1);
    return (int)RAW(PROXY_SC_rt_sigprocmask,SIG_BLOCK,&child,previous,8,0,0);
}
int proxy_resident_signals_end(uint64_t previous) {
    return (int)RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&previous,0,8,0,0);
}

size_t p_len(const char *s) { size_t n=0; while(s[n]) n++; return n; }
void p_copy(void *d,const void *s,size_t n) { unsigned char *a=d; const unsigned char *b=s; while(n--) *a++=*b++; }
int p_equal(const char *a,const char *b) { while(*a && *a==*b) {a++;b++;} return *a==*b; }
/* OSTree checkouts can be emulated hardlink names. Read the same backing
 * inode as payload openat, including its executable mode and ELF/shebang. */
static long loader_open(const char *path){
    proxy_arg_t args[6]={AT_FDCWD,(proxy_arg_t)path,O_RDONLY|O_CLOEXEC,0,0,0};
    return links_path_call(PROXY_SC_openat,args,path);
}
static void loader_close(int fd){RAW(PROXY_SC_close,fd,0,0,0,0,0);}
/* File mappings use ordinary kernel descriptors and retain page-cache sharing.
 * Only permission refusals permit the private copy fallback. */
/* Fill an anonymous replacement for a denied file mapping, then apply the
 * requested protection. Bytes past EOF stay zero, as in a file mapping. */
static long mmap_copy_file(proxy_arg_t *a,long area) {
    size_t done=0;
    while(done<(size_t)a[1]){
        long n=proxy_pread_fd(a[4],(void *)((uintptr_t)area+done),a[1]-done,(uint64_t)a[5]+done);
        if(n==-EINTR)continue;
        if(n<0)return n;
        if(!n)break;
        done+=n;
    }
    return RAW(PROXY_SC_mprotect,area,a[1],a[2],0,0,0);
}
long proxy_file_mmap(proxy_arg_t *a) {
    long r=RAW(PROXY_SC_mmap,a[0],a[1],a[2],a[3],a[4],a[5]);
    if(r!=-EACCES && r!=-EPERM){
        if(!PROXY_RAW_ERR(r) && (a[2]&(PROT_READ|PROT_EXEC))==(PROT_READ|PROT_EXEC) && !(a[2]&PROT_WRITE) && (a[3]&MAP_PRIVATE))
            proxy_rewrite_mapping((int)a[4],(uintptr_t)r,(size_t)a[1],(uint64_t)a[5]);
        return r;
    }
    if((a[3]&MAP_SHARED) && (a[2]&PROT_WRITE))return r;
    if(!(a[3]&(MAP_SHARED|MAP_PRIVATE)))return r;
    long area=RAW(PROXY_SC_mmap,a[0],a[1],PROT_READ|PROT_WRITE,
                  (a[3]&~(MAP_SHARED|MAP_PRIVATE))|MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if((unsigned long)area>=(unsigned long)-4095)return area;
    r=mmap_copy_file(a,area);
    if(r<0){RAW(PROXY_SC_munmap,area,a[1],0,0,0,0);return r;}
    struct proxy_stat st;
    struct copy_log {struct copy_log *next;uint64_t dev,ino;};
    static struct copy_log *logs;
    if(!arch_stat_fd(a[4],&st)){
        struct copy_log *l=logs;
        while(l && (l->dev!=st.st_dev || l->ino!=st.st_ino))l=l->next;
        if(!l){
            l=vfs_palloc(sizeof(*l));
            if(l){*l=(struct copy_log){logs,st.st_dev,st.st_ino};logs=l;}
            proxy_log("exec-mmap-copy",a[4],0);
        }
    }
    if((a[2]&(PROT_READ|PROT_EXEC))==(PROT_READ|PROT_EXEC) && !(a[2]&PROT_WRITE) && (a[3]&MAP_PRIVATE))
        proxy_rewrite_mapping((int)a[4],(uintptr_t)area,(size_t)a[1],(uint64_t)a[5]);
    return area;
}
/* Android denies execmod: a private file mapping whose pages were written
 * (relocated PE images in Wine, patched code) cannot become executable.
 * Linux allows it, so move the range into anonymous memory, which apps may
 * make executable, and retry. The range loses its file backing in maps. */
long proxy_exec_mprotect(proxy_arg_t *a) {
    long r=RAW(PROXY_SC_mprotect,a[0],a[1],a[2],0,0,0);
    if(r!=-EACCES || !(a[2]&PROT_EXEC) || (a[0]&4095) || !a[1])return r;
    size_t size=((size_t)a[1]+4095)&~(size_t)4095;
    long copy=RAW(PROXY_SC_mmap,0,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(PROXY_RAW_ERR(copy)){proxy_log("fail exec-mprotect-copy",a[0],copy);return r;}
    long readable=RAW(PROXY_SC_mprotect,a[0],size,PROT_READ,0,0,0);
    if(readable){proxy_log("fail exec-mprotect-read",a[0],readable);RAW(PROXY_SC_munmap,copy,size,0,0,0,0);return r;}
    p_copy((void *)(uintptr_t)copy,(const void *)(uintptr_t)a[0],size);
    long area=RAW(PROXY_SC_mmap,a[0],size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    if(area!=(long)a[0]){proxy_log("fail exec-mprotect-remap",a[0],area);RAW(PROXY_SC_munmap,copy,size,0,0,0,0);return PROXY_RAW_ERR(area)?area:-ENOMEM;}
    p_copy((void *)(uintptr_t)area,(const void *)(uintptr_t)copy,size);
    RAW(PROXY_SC_munmap,copy,size,0,0,0,0);
    r=RAW(PROXY_SC_mprotect,a[0],size,a[2],0,0,0);
    proxy_log(r?"fail exec-mprotect-anon":"exec-mprotect-anon",a[0],r?r:(long)size);
    return r;
}
static uintptr_t inherited_aux[64];
uintptr_t proxy_aux(unsigned type){return type<64?inherited_aux[type]:0;}
static int bad(long n) { return (unsigned long)n >= (unsigned long)-4095; }
/* Keep exec path diagnostics opt-in: these mirror the translation used by
 * openat and make pack lookup failures visible in the Android trace. */
static void loader_trace_path(const char *tag,const char *path){
    if(!proxy_trace)return;
    proxy_diagnostic_write(tag,p_len(tag));
    if(path)proxy_diagnostic_write(path,p_len(path));
    proxy_diagnostic_write("\n",1);
}
static void loader_trace_resolve(const char *path,const char *stage){
    if(!proxy_trace)return;
    long error=0;void *mount=NULL;
    char *resolved=vfs_resolve_mount(AT_FDCWD,path,1,&error,&mount);
    proxy_log(stage,resolved?1:error,mount!=NULL);
    if(resolved){
        loader_trace_path("v3 exec resolved ",resolved);
        vfs_free(resolved);
    } else loader_trace_path("v3 exec unresolved ",path);
}
/* Preflight both ELF classes before MAP_FIXED or a kernel exec of our peer.
 * Use fixed-width ELF records, not the current backend's native typedefs. */
static int validate_exec(const char *path,long supplied_fd,int interpreter,unsigned depth);
/* Check the ELF headers of an opened, executable regular file. */
static int validate_elf(long fd,const struct proxy_stat *stat,int interpreter,unsigned depth) {
    Elf64_Ehdr h={0};Elf32_Ehdr h32;
    const int error=interpreter?-ELIBBAD:-ENOEXEC;
    unsigned char ident[EI_NIDENT];
    if(proxy_pread_fd(fd,ident,sizeof(ident),0)!=sizeof(ident))return error;
    if(ident[0]!=127 || ident[1]!='E' || ident[2]!='L' || ident[3]!='F' ||
       ident[EI_DATA]!=ELFDATA2LSB || ident[EI_VERSION]!=EV_CURRENT)return error;
    int bits=ident[EI_CLASS];
    if(interpreter && bits!=interpreter)return error;
    if(bits==ELFCLASS32){
        if(proxy_pread_fd(fd,&h32,sizeof(h32),0)!=sizeof(h32))return error;
        h.e_type=h32.e_type;h.e_machine=h32.e_machine;h.e_phoff=h32.e_phoff;
        h.e_phnum=h32.e_phnum;h.e_phentsize=h32.e_phentsize;
        if(h.e_phentsize!=sizeof(Elf32_Phdr))return error;
    }else if(bits==ELFCLASS64){
        if(proxy_pread_fd(fd,&h,sizeof(h),0)!=sizeof(h))return error;
        if(h.e_phentsize!=sizeof(Elf64_Phdr))return error;
    }else return error;
    if(arch_exec_class(bits,h.e_machine)<0)return error;
    if((h.e_type!=ET_EXEC && h.e_type!=ET_DYN) || !h.e_phnum ||
       h.e_phoff>(uint64_t)stat->st_size ||
       (uint64_t)h.e_phnum*h.e_phentsize>(uint64_t)stat->st_size-h.e_phoff)return error;
    char interp[4096];interp[0]=0;unsigned loads=0,has_phdr=0;
    for(unsigned i=0;i<h.e_phnum;i++){
        Elf64_Phdr p={0};uint64_t offset=h.e_phoff+(uint64_t)i*h.e_phentsize;
        if(bits==ELFCLASS32){
            Elf32_Phdr q;
            if(proxy_pread_fd(fd,&q,sizeof(q),offset)!=sizeof(q))return error;
            p.p_type=q.p_type;p.p_offset=q.p_offset;p.p_filesz=q.p_filesz;
            p.p_memsz=q.p_memsz;p.p_vaddr=q.p_vaddr;p.p_align=q.p_align;
        }else if(proxy_pread_fd(fd,&p,sizeof(p),offset)!=sizeof(p))return error;
        if(p.p_type==PT_PHDR)has_phdr=1;
        if(p.p_type!=PT_LOAD && p.p_type!=PT_INTERP)continue;
        if(p.p_offset>(uint64_t)stat->st_size || p.p_filesz>(uint64_t)stat->st_size-p.p_offset)return error;
        if(p.p_type==PT_LOAD){
            uint64_t limit=bits==ELFCLASS32?UINT32_MAX:UINT64_MAX;
            if(p.p_filesz>p.p_memsz || p.p_vaddr>limit-4095 || p.p_memsz>limit-4095-p.p_vaddr)return error;
            if(p.p_memsz)loads++;
            if(h.e_phoff>=p.p_offset && h.e_phoff-p.p_offset+(uint64_t)h.e_phnum*h.e_phentsize<=p.p_filesz)has_phdr=1;
        }else{
            if(interpreter || interp[0] || p.p_filesz<2 || p.p_filesz>sizeof(interp))return error;
            if(proxy_pread_fd(fd,interp,p.p_filesz,p.p_offset)!=(long)p.p_filesz || interp[p.p_filesz-1])return error;
        }
    }
    if(!loads || !has_phdr)return error;
    if(interp[0]){
        loader_trace_path("v3 exec validate PT_INTERP ",interp);
        int interp_error=validate_exec(interp,-1,bits,depth+1);
        if(proxy_trace)proxy_log("exec-validate-interp",interp_error,depth);
        return interp_error;
    }
    return 0;
}
static int validate_exec(const char *path,long supplied_fd,int interpreter,unsigned depth) {
    if(depth>1)return -ELIBBAD;
    long error=0;
    char *resolved=NULL;
    long fd=supplied_fd;
    if(fd<0)resolved=vfs_resolve(AT_FDCWD,path,1,&error);
    if(proxy_trace){
        proxy_log("exec-validate-resolve",supplied_fd>=0?1:(resolved?1:error),depth);
        if(supplied_fd>=0)loader_trace_path("v3 exec validate resolved ",path);
        else if(resolved)loader_trace_path("v3 exec validate resolved ",resolved);
    }
    if(supplied_fd<0 && !resolved)return error;
    if(supplied_fd<0)fd=loader_open(resolved);
    if(proxy_trace)proxy_log("exec-validate-open",fd,depth);
    if(fd<0){vfs_free(resolved);return fd;}
    struct proxy_stat st;error=arch_stat_fd(fd,&st);
    if(!error && (!S_ISREG(st.st_mode) || !(st.st_mode&0111)))error=-EACCES;
    /* Payload execution is emulated by this loader, not delegated to the
     * kernel. The backing filesystem may be mounted noexec (Android app data
     * and the runtime EROFS mount can be), which must not override Linux
     * execute bits after the virtual filesystem has resolved the path. */
    vfs_free(resolved);
    if(!error)error=validate_elf(fd,&st,interpreter,depth);
    if(proxy_trace)proxy_log("exec-validate-result",error,depth);
    if(supplied_fd<0)loader_close(fd);
    return error;
}
/* Stage and map the PT_LOAD segments of an opened ELF. The program header
 * buffer is returned through *owned so the caller frees it on every path. */
static int map_elf_fd(const char *path,long fd,struct mapped *out,arch_elf_phdr **owned) {
    int error=-ENOEXEC;
    arch_elf_ehdr eh; arch_elf_phdr *ph;
    if(proxy_pread_fd(fd,&eh,sizeof(eh),0)!=(long)sizeof(eh)) return error;
    if(eh.e_ident[0]!=127 || eh.e_ident[1]!='E' || eh.e_ident[2]!='L' || eh.e_ident[3]!='F' ||
       eh.e_ident[EI_CLASS]!=ARCH_ELF_CLASS || eh.e_machine!=ARCH_ELF_MACHINE ||
       (eh.e_type!=ET_DYN && eh.e_type!=ET_EXEC) || !eh.e_phnum ||
       eh.e_phentsize!=sizeof(arch_elf_phdr)) return error;
    size_t bytes=eh.e_phnum*sizeof(arch_elf_phdr);
    ph=*owned=vfs_alloc(bytes);if(!ph)return -ENOMEM;
    if(proxy_pread_fd(fd,ph,bytes,eh.e_phoff)!=(long)bytes) return error;
    unsigned loads=0;uintptr_t previous=0;
    for(unsigned i=0;i<eh.e_phnum;i++) if(ph[i].p_type==PT_LOAD && ph[i].p_memsz) {
        if(ph[i].p_filesz>ph[i].p_memsz ||
           (uint64_t)ph[i].p_vaddr+ph[i].p_memsz>UINTPTR_MAX-(PAGE-1)) return error;
        /* ELF requires PT_LOAD ascending by p_vaddr; spans rely on it. */
        if(loads && ph[i].p_vaddr<previous) return error;
        previous=ph[i].p_vaddr;loads++;
    }
    if(!loads) return error;
    out->spans=vfs_alloc(loads*sizeof(*out->spans));if(!out->spans)return -ENOMEM;
    for(unsigned i=0;i<eh.e_phnum;i++) if(ph[i].p_type==PT_LOAD && ph[i].p_memsz) {
        uintptr_t low=DOWN(ph[i].p_vaddr),high=UP(ph[i].p_vaddr+ph[i].p_memsz);
        struct span *last=out->count?&out->spans[out->count-1]:NULL;
        if(last && (eh.e_type==ET_DYN || low<=last->low+last->size)) {
            if(high>last->low+last->size)last->size=high-last->low;
        }else out->spans[out->count++]=(struct span){low,0,0,high-low};
    }
    /* Refuse collision with the resident proxy rather than overwriting it. */
    for(unsigned k=0;eh.e_type==ET_EXEC && k<out->count;k++)
        if(resident_collision(out->spans[k].low,out->spans[k].low+out->spans[k].size)) return error;
    /* Stage away from the caller, including fixed-address executables. */
    for(unsigned k=0;k<out->count;k++) {
        struct span *span=&out->spans[k];
        long area=RAW(PROXY_SC_mmap,0,span->size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(bad(area)) return (int)area;
        span->area=area;span->target=eh.e_type==ET_EXEC?span->low:(uintptr_t)area;
    }
    /* Addresses the payload sees: ET_DYN at its staging area, ET_EXEC as linked. */
    uintptr_t load_bias=eh.e_type==ET_DYN?out->spans[0].area-out->spans[0].low:0;
    out->base=load_bias; out->entry=load_bias+eh.e_entry;
    out->phnum=eh.e_phnum; out->phdr=0; out->interp[0]=0;
    unsigned current=0;
    for(unsigned i=0;i<eh.e_phnum;i++) {
        arch_elf_phdr *p=&ph[i];
        if(p->p_type==PT_INTERP) {
            if(p->p_filesz>sizeof(out->interp) || !p->p_filesz) return error;
            if(proxy_pread_fd(fd,out->interp,p->p_filesz,p->p_offset)!=(long)p->p_filesz) return error;
            if(out->interp[p->p_filesz-1])return error;
        }
        if(p->p_type==PT_PHDR) out->phdr=load_bias+p->p_vaddr;
        if(p->p_type!=PT_LOAD || !p->p_memsz) continue;
        while(DOWN(p->p_vaddr)>=out->spans[current].low+out->spans[current].size)current++;
        const struct span *span=&out->spans[current];
        uintptr_t bias=span->area-span->low;
        int prot=((p->p_flags&PF_R)?PROT_READ:0)|((p->p_flags&PF_W)?PROT_WRITE:0)|((p->p_flags&PF_X)?PROT_EXEC:0);
        uintptr_t start=bias+DOWN(p->p_vaddr),end=bias+UP(p->p_vaddr+p->p_memsz);
        long r;
        uintptr_t file_end=bias+UP(p->p_vaddr+p->p_filesz);
        int congruent=(p->p_offset&(PAGE-1))==(p->p_vaddr&(PAGE-1));
        if(p->p_filesz && congruent){
            proxy_arg_t args[]={start,file_end-start,prot,MAP_PRIVATE|MAP_FIXED,fd,DOWN(p->p_offset)};
            r=proxy_file_mmap(args);
            if(bad(r))return (int)r;
            /* Only the last file page needs COW zeroing. Never dirty text
             * pages without BSS (including bytes beyond p_filesz). */
            if(p->p_memsz>p->p_filesz && file_end>bias+p->p_vaddr+p->p_filesz){
                uintptr_t tail=bias+p->p_vaddr+p->p_filesz;
                r=RAW(PROXY_SC_mprotect,DOWN(tail),PAGE,PROT_READ|PROT_WRITE,0,0,0);
                if(bad(r))return (int)r;
                for(uintptr_t z=tail;z<file_end;z++)*(unsigned char *)z=0;
                r=RAW(PROXY_SC_mprotect,DOWN(tail),PAGE,prot,0,0,0);
                if(bad(r))return (int)r;
            }
            if(end>file_end){
                r=RAW(PROXY_SC_mmap,file_end,end-file_end,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
                if(bad(r))return (int)r;
            }
        }else{
            r=RAW(PROXY_SC_mmap,start,end-start,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
            if(bad(r))return (int)r;
            if(p->p_filesz && proxy_pread_fd(fd,(void *)(bias+p->p_vaddr),p->p_filesz,p->p_offset)!=(long)p->p_filesz) return error;
            r=RAW(PROXY_SC_mprotect,start,end-start,prot,0,0,0);
            if(bad(r))return (int)r;
        }
        proxy_image_map(span->target+start-span->area,span->target+end-span->area,DOWN(p->p_vaddr),path);
        if(!out->phdr && eh.e_phoff>=p->p_offset && (uint64_t)eh.e_phoff+bytes<=(uint64_t)p->p_offset+p->p_filesz)
            out->phdr=load_bias+p->p_vaddr+eh.e_phoff-p->p_offset;
    }
    return out->phdr?0:-ENOEXEC;
}
static int map_elf(const char *path,long supplied_fd,struct mapped *out) {
    long fd=supplied_fd;
    if(fd<0){fd=loader_open(path);if(bad(fd))return (int)fd;}
    out->spans=NULL;out->count=0;
    arch_elf_phdr *ph=NULL;
    int error=map_elf_fd(path,fd,out,&ph);
    if(error)release_spans(out,1);
    vfs_free(ph);if(supplied_fd<0)loader_close(fd); return error;
}
/* All reads, validation, allocation and protection checks precede this point.
 * Move staged VMAs one page at a time (protections may split the reservation).
 * Like kernel exec after begin_new_exec, an unrecoverable commit failure must
 * terminate, never resume a caller whose fixed-address pages were replaced. */
static void commit_elf(struct mapped *m){
    for(unsigned k=0;k<m->count;k++){
        const struct span *span=&m->spans[k];
        if(span->area==span->target)continue;
        for(uintptr_t offset=0;offset<span->size;offset+=PAGE){
            long r=RAW(PROXY_SC_mremap,span->area+offset,PAGE,PAGE,3,span->target+offset,0);
            if(bad(r))RAW(PROXY_SC_exit_group,127,0,0,0,0,0);
        }
        proxy_rewrite_relocate(span->area,span->target,span->size);
    }
    release_spans(m,0);
}
static uintptr_t stack_string(uintptr_t *cursor,const char *s) {
    size_t n=p_len(s)+1; *cursor-=n; p_copy((void *)*cursor,s,n); return *cursor;
}
static int load_inner(const char *path,char *const *argv,char *const *env,struct image *image,unsigned depth) {
    if(depth>4) return -ELOOP;
    loader_trace_resolve(path,"exec-resolve");
    char header[256]={0};long path_error=0;
    char *resolved=vfs_resolve(AT_FDCWD,path,1,&path_error);
    if(!resolved)return (int)path_error;
    long fd=loader_open(resolved);
    if(bad(fd)){vfs_free(resolved);return (int)fd;}
    struct proxy_stat executable;int permission=arch_stat_fd(fd,&executable);
    if(!permission && (!S_ISREG(executable.st_mode) || !(executable.st_mode&0111)))permission=-EACCES;
    if(permission){loader_close(fd);vfs_free(resolved);return permission;}
    long got=proxy_pread_fd(fd,header,sizeof(header)-1,0);
    if(got<0){loader_close(fd);vfs_free(resolved);return (int)got;}
    header[got]=0;
    if(got>=2 && header[0]=='#' && header[1]=='!') {
        loader_close(fd);vfs_free(resolved);
        char *end=header+2;while(*end && *end!='\n') end++;
        if(!*end) return -ENOEXEC;
        *end=0;while(end>header+2 && (end[-1]==' ' || end[-1]=='\t')) *--end=0;
        char *interpreter=header+2;while(*interpreter==' ' || *interpreter=='\t') interpreter++;
        char *arg=interpreter;while(*arg && *arg!=' ' && *arg!='\t') arg++;
        if(*arg) {*arg++=0;while(*arg==' ' || *arg=='\t') arg++;}
        if(!*interpreter) return -ENOEXEC;
        size_t count=0;while(argv && argv[count]){if(count>SIZE_MAX/sizeof(char *)-5)return -E2BIG;count++;}
        char **args=vfs_alloc((count+4)*sizeof(*args));if(!args)return -ENOMEM;size_t n=0;args[n++]=interpreter;
        if(*arg) args[n++]=arg;
        args[n++]=(char *)path;
        for(size_t i=1;argv && argv[i];i++)args[n++]=argv[i];
        args[n]=NULL;
        int r=load_inner(interpreter,args,env,image,depth+1);vfs_free(args);return r;
    }
    int checked=validate_exec(resolved,fd,0,0);if(checked){if(proxy_trace)proxy_log("exec-validate",checked,0);loader_close(fd);vfs_free(resolved);return checked;}
    if(proxy_trace)proxy_log("exec-validate",0,0);
    int handoff=state_exec(path,(const unsigned char *)header,(size_t)got,argv,env);if(handoff){if(proxy_trace)proxy_log("exec-state",handoff,0);loader_close(fd);vfs_free(resolved);return handoff;}
#define EXEC_RETURN(value) do { int exec_result=(value); loader_close(fd); vfs_free(resolved); return exec_result; } while(0)
    size_t ac=0,ec=0,total=65536;
    while(argv && argv[ac]){size_t n=p_len(argv[ac]);if(n>SIZE_MAX-total-sizeof(uintptr_t)-1)EXEC_RETURN(-E2BIG);total+=n+1+sizeof(uintptr_t);ac++;}
    while(env && env[ec]){size_t n=p_len(env[ec]);if(n>SIZE_MAX-total-sizeof(uintptr_t)-1)EXEC_RETURN(-E2BIG);total+=n+1+sizeof(uintptr_t);ec++;}
    if(ac>SIZE_MAX/sizeof(uintptr_t)-1 || ec>SIZE_MAX/sizeof(uintptr_t)-1 || total>SIZE_MAX-4095)EXEC_RETURN(-E2BIG);
    uintptr_t *av=vfs_alloc((ac+1)*sizeof(*av)),*ev=vfs_alloc((ec+1)*sizeof(*ev));
    if(!av || !ev){vfs_free(av);vfs_free(ev);EXEC_RETURN(-ENOMEM);}
    size_t stack_size=UP(total);if(stack_size<STACK_SIZE)stack_size=STACK_SIZE;
    long stack=RAW(PROXY_SC_mmap,0,stack_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(bad(stack)){vfs_free(av);vfs_free(ev);EXEC_RETURN((int)stack);}
    uintptr_t cur=(uintptr_t)stack+stack_size;
    /* Kernel layout, ascending: argv strings, env strings, execfn. Wine's
     * preloader rewrites argv in place and relies on that contiguity. */
    uintptr_t execfn=stack_string(&cur,path);
    for(size_t i=ec;i-->0;)ev[i]=stack_string(&cur,env[i]);
    for(size_t i=ac;i-->0;)av[i]=stack_string(&cur,argv[i]);
    /* Note argv before any old mapping is replaced: it may live there. */
    proxy_note_payload_argv(argv);
    struct mapped *maps=vfs_alloc(2*sizeof(*maps));if(!maps){RAW(PROXY_SC_munmap,stack,stack_size,0,0,0,0);vfs_free(av);vfs_free(ev);EXEC_RETURN(-ENOMEM);}
    maps[0].spans=maps[1].spans=NULL;maps[0].count=maps[1].count=0;
    struct mapped *main=&maps[0],*interp=&maps[1];int r=map_elf(resolved,fd,main);loader_close(fd);vfs_free(resolved);fd=-1;resolved=NULL;if(proxy_trace)proxy_log("exec-map-main",r,main->count);
    if(r)goto failed;
    uintptr_t entry=main->entry;
    if(main->interp[0]){
        loader_trace_path("v3 exec PT_INTERP ",main->interp);
        loader_trace_resolve(main->interp,"interp-resolve");
        char *translated=vfs_resolve(AT_FDCWD,main->interp,1,&path_error);
        if(!translated){r=path_error;goto failed;}
        r=map_elf(translated,-1,interp);vfs_free(translated);if(proxy_trace)proxy_log("exec-map-interp",r,interp->count);if(r)goto failed;entry=interp->entry;
    }
    uintptr_t platform=stack_string(&cur,ARCH_PLATFORM);
    cur-=16; uintptr_t random=cur;
    if(RAW(PROXY_SC_getrandom,random,16,0,0,0,0)!=16){r=-EIO;goto failed;}
    arch_elf_auxv aux[]={
        {AT_PHDR,{main->phdr}},{AT_PHENT,{sizeof(arch_elf_phdr)}},{AT_PHNUM,{main->phnum}},
        {AT_PAGESZ,{PAGE}},{AT_BASE,{interp->base}},{AT_ENTRY,{main->entry}},
        {AT_UID,{RAW(PROXY_SC_getuid,0,0,0,0,0,0)}},{AT_EUID,{RAW(PROXY_SC_geteuid,0,0,0,0,0,0)}},
        {AT_GID,{RAW(PROXY_SC_getgid,0,0,0,0,0,0)}},{AT_EGID,{RAW(PROXY_SC_getegid,0,0,0,0,0,0)}},
        {AT_SECURE,{0}},{AT_RANDOM,{random}},{AT_EXECFN,{execfn}},{AT_PLATFORM,{platform}},
        {AT_HWCAP,{proxy_aux(AT_HWCAP)}},{AT_HWCAP2,{proxy_aux(AT_HWCAP2)}},
        {ARCH_AUX_SYSINFO,{PROXY_SYSINFO_AUX(proxy_aux(ARCH_AUX_SYSINFO))}},{AT_SYSINFO_EHDR,{proxy_aux(AT_SYSINFO_EHDR)}},{AT_CLKTCK,{100}},{AT_NULL,{0}}};
    size_t words=1+ac+1+ec+1+sizeof(aux)/(sizeof(uintptr_t));
    uintptr_t sp=(cur-words*sizeof(uintptr_t))&~15UL,*v=(uintptr_t *)sp;
    *v++=ac; for(unsigned i=0;i<ac;i++) *v++=av[i]; *v++=0;
    for(unsigned i=0;i<ec;i++) *v++=ev[i];
    *v++=0; p_copy(v,aux,sizeof(aux));
    vfs_free(av);vfs_free(ev);commit_elf(main);commit_elf(interp);
    image->entry=entry;image->stack=sp;
#ifdef MATON_GPU_BRIDGE
    /* The resident survives exec. Publish this payload, rather than leaving
     * GPU clients looking at the original bwrap/launcher link map. The private
     * mapping is copy-on-write across fork, like the payload it describes. */
    if(gpu_bootstrap) {
        if(RAW(PROXY_SC_mprotect,gpu_bootstrap,PAGE,PROT_READ|PROT_WRITE,0,0,0)){vfs_free(maps);return -EACCES;}
        maton_gpu_payload(gpu_bootstrap,main->phdr,main->phnum);
        if(RAW(PROXY_SC_mprotect,gpu_bootstrap,PAGE,PROT_READ,0,0,0)){vfs_free(maps);return -EACCES;}
    }
#endif
    vfs_free(maps);return 0;
failed:
    if(proxy_trace)proxy_log("exec-load-failed",r,0);
    release_spans(main,1);release_spans(interp,1);
    RAW(PROXY_SC_munmap,stack,stack_size,0,0,0,0);
    vfs_free(av);vfs_free(ev);vfs_free(maps);return r;
#undef EXEC_RETURN
}
int proxy_load(const char *path,char *const *argv,char *const *env,struct image *image) {
    char *exec_alias=vfs_exec_alias(path);
    if(!exec_alias && path){
        /* path may live in the old image (an ET_EXEC re-exec of itself
         * passes a BSS buffer); commit_elf replaces that memory before
         * vfs_executable reads it. Keep a loader-owned copy. */
        size_t n=p_len(path);
        exec_alias=vfs_alloc(n+1);if(!exec_alias)return -ENOMEM;
        p_copy(exec_alias,path,n+1);
    }
    if(exec_alias)path=exec_alias;
    proxy_log_bwrap_args(path,argv);
#if defined(__i386__)
    proxy_i386_sysinfo_configure(env);
#endif
    const char *rewrite_option=0;
    for(size_t i=0;env && env[i];i++)
        if(p_equal(env[i],"MATON_PROXY_REWRITE=0")){rewrite_option=env[i];break;}
    int rewrite_logging=0;
    for(size_t i=0;env && env[i];i++)if(p_equal(env[i],"MATON_PROXY_REWRITE_LOG=1"))rewrite_logging=1;
    proxy_rewrite_init(rewrite_option,rewrite_logging);
    const char *rewrite_cache=NULL;
    for(size_t i=0;env && env[i];i++){size_t j=0;const char *key="XDG_CACHE_HOME=";while(key[j] && env[i][j]==key[j])j++;if(!key[j])rewrite_cache=env[i]+j;}
    proxy_rewrite_cache_init(rewrite_cache);
    int r=load_inner(path,argv,env,image,0);
    if(!r)r=proxy_i386_tls_prepare_payload();
    if(!r)vfs_executable(path);
    vfs_free(exec_alias);
    return r;
}
#ifndef PROXY_LIBRARY
static int loader_main(int argc,char **argv,char **environ) {
    int supervised=supervise_launch(argc,argv,environ);
    if(supervised>=0)return supervised;
    char **end=environ;while(*end)end++;
    arch_elf_auxv *original=(void *)(end+1);
    for(;original->a_type;original++)if(original->a_type<64)inherited_aux[original->a_type]=original->a_un.a_val;
    extern const char *proxy_tmpdir;
    for(unsigned e=0;environ[e];e++){const char *s=environ[e];if(s[0]=='T' && s[1]=='M' && s[2]=='P' && s[3]=='D' && s[4]=='I' && s[5]=='R' && s[6]=='=')proxy_tmpdir=s+7;}
    int resumed=argc>4 && p_equal(argv[1],"--resident-state");
    if(resumed){
        /* A kernel exec replaced this image's globals: the trace flag is zero
         * and the imported sink does not exist yet, so a resident that dies
         * before import is completely silent. Report entry on fd 2, the pipe
         * the app-side launcher relays for the whole process, and record
         * whether a seccomp filter chain crossed the exec (mode 2 = FILTER).
         * Absent this line the failure is in the kernel-to-bionic startup. */
        proxy_log("resident-enter",argc,RAW(PROXY_SC_prctl,PR_GET_SECCOMP,0,0,0,0,0));
        if(state_import(argv[2],argv[3]))return 2;
        environ=state_environment;
        /* Do not leave the bearer token in the kernel's original cmdline after
         * payload entry. Import closed the private state FD already. */
        for(unsigned j=2;j<4;j++)for(char *p=argv[j];*p;p++)*p=0;
    }
#if defined(__i386__) && defined(MATON_GPU_BRIDGE)
    /* A fresh Linux launch uses PER_LINUX on a 64-bit kernel. Bionic's
     * unconditional ELF32 PER_LINUX32 selection is loader implementation
     * state, not a payload request. Resumed exec restores its caller instead. */
    if(!resumed) {
        long persona=RAW(PROXY_SC_personality,UINT32_MAX,0,0,0,0,0);
        if(persona>=0) {
            long r=RAW(PROXY_SC_personality,(unsigned int)persona & ~0xffU,0,0,0,0,0);
            if(r<0){proxy_log("startup-personality",0,r);return 1;}
        }
    }
#endif
    if(proxy_diagnostics_init())return 1;
    /* Before any Android graphics library can log: liblog's own logd socket
     * would live at a payload-visible number that a payload fd sweep recycles. */
    proxy_android_log_install();
    vfs_temp_gc();
    int android=0,i=resumed?4:1; long parent=0;
#ifdef MATON_GPU_BRIDGE
    int gpu_bridge_enabled=state_gpu_bridge;
    static void *gpu_egl, *gpu_gles;
#endif
    while(i<argc && argv[i][0]=='-') {
#ifdef MATON_GPU_BRIDGE
        if(p_equal(argv[i],"--gpu-bridge")) {gpu_bridge_enabled=1;i++;}
        else
#endif
        if(p_equal(argv[i],"--parent-pid") && i+1<argc) {
            const char *number=argv[i+1];
            if(!*number)return 2;
            for(;*number;number++) {if(*number<'0'||*number>'9'||parent>214748364)return 2;parent=parent*10+*number-'0';}
            if(parent<=0 || RAW(PROXY_SC_prctl,PR_SET_PDEATHSIG,SIGKILL,0,0,0,0)<0 ||
                    RAW(PROXY_SC_getppid,0,0,0,0,0,0)!=parent)return 2;
            i+=2;
        }
#ifdef HOST_COMMAND_TEST
        else if(p_equal(argv[i],"--host-command-fd") && i+1<argc) {extern int proxy_host_test(int);int fd=0;for(char *s=argv[i+1];*s;s++){if(*s<'0'||*s>'9')return 2;fd=fd*10+*s-'0';}proxy_host_test(fd);i+=2;}
#endif
        else if(p_equal(argv[i],"--host-command") && i+1<argc) {if(proxy_host_command(argv[i+1]))return 2;i+=2;}
        else if(p_equal(argv[i],"--dns-forwarder") && i+1<argc) {if(proxy_dns_forwarder(argv[i+1]))return 2;
            i+=2;}
        else if(p_equal(argv[i],"--android-filter")) {android=1;i++;}
        else if(p_equal(argv[i],"--i386-tls-reserve")) {proxy_i386_tls_reservation(1);i++;}
        else if(p_equal(argv[i],"--trace")) {extern int proxy_trace;proxy_trace=1;i++;}
        else if(p_equal(argv[i],"--count") && i+1<argc) {proxy_count_init(argv[i+1]);i+=2;}
        else if(p_equal(argv[i],"--erofs-mount") && i+2<argc) {if(erofs_mount(argv[i+1],argv[i+2]))return 2;i+=3;}
        else if(p_equal(argv[i],"--erofs-backing") && i+2<argc) {if(erofs_backing(argv[i+1],argv[i+2]))return 2;i+=3;}
        else if(p_equal(argv[i],"--map-ro") && i+2<argc) {if(vfs_mount_readonly_map(argv[i+1],argv[i+2]))return 2;i+=3;}
        else if(p_equal(argv[i],"--map") && i+2<argc) {if(proxy_mount(argv[i+1],argv[i+2]))return 2;i+=3;}
        else if(p_equal(argv[i],"--root") && i+1<argc) {if(vfs_bootstrap_root(argv[i+1])) return 2;i+=2;}
        else {proxy_log("usage",0,-EINVAL);return 2;}
    }
    if(i==argc) return 2;
    if(resumed)android=state_android;else state_android=android;
#ifdef MATON_GPU_BRIDGE
    state_gpu_bridge=gpu_bridge_enabled;
#if defined(__i386__)
    if(gpu_bridge_enabled)proxy_i386_tls_reservation(1);
#endif
#endif
    int prepared=proxy_native_prepare();
    if(prepared){proxy_log("native-reservation-error",0,prepared);return 1;}
    vfs_gpu_init();
    struct image image; int r=proxy_load(argv[i],argv+i+(resumed?1:0),environ,&image);
    if(r) {proxy_log("load-error",0,r);return 1;}
#ifdef MATON_GPU_BRIDGE
    uint64_t resident_signal_mask=0;
    if(gpu_bridge_enabled && proxy_resident_signals_begin(&resident_signal_mask))return 7;
    static struct gpu_bridge bridge;
    if(gpu_bridge_enabled) {
        bridge.version=2;
        bridge.open=dlopen;
        bridge.symbol=dlsym;
        bridge.error=gpu_bridge_error;
    }
    if(gpu_bridge_enabled) {
        gpu_egl=dlopen(NULL,RTLD_NOW|RTLD_LOCAL);
        gpu_gles=gpu_egl;
    }
#endif
    /* Enable native fd lifetime tracking without calling EGL. On some
     * drivers even eglGetDisplay starts cache workers, breaking a freshly
     * execed Linux zygote's single-thread invariant. Graphics libraries loaded
     * later still get native syscall semantics via Android mapping lookup. */
#ifdef MATON_GPU_BRIDGE
    if(gpu_bridge_enabled) {
        if(proxy_resident_fds_snapshot(0)) return 7;
        if(proxy_resident_fds_snapshot(1)) return 7;
        if(proxy_resident_signals_end(resident_signal_mask))return 7;
    }
#endif
    r=proxy_install(android);
    if(r) {proxy_log("filter-error",0,r);return 1;}
#ifdef MATON_GPU_BRIDGE
    if(gpu_bridge_enabled) {
        bridge.owner=RAW(PROXY_SC_gettid,0,0,0,0,0,0);
#if defined(__x86_64__)
        if(RAW(PROXY_SC_arch_prctl,0x1003,&bridge.tls,0,0,0,0)) return 3;
#endif
        int adopt_result=adopt_install();
        if(adopt_result) {proxy_log("adopt-install-error",0,adopt_result);return 5;}
    }
#endif
    proxy_host_snapshot(environ);
#ifdef MATON_GPU_BRIDGE
    if(gpu_bridge_enabled) {
        char value[32];
        snprintf(value,sizeof(value),"%lx",(unsigned long)&bridge);
        struct maton_gpu_bootstrap *bootstrap=mmap(NULL,4096,PROT_READ|PROT_WRITE,
                                      MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(bootstrap==MAP_FAILED) return 6;
        bootstrap->magic=MATON_GPU_BOOTSTRAP_MAGIC;
        snprintf(bootstrap->syscall,sizeof(bootstrap->syscall),"%lx",(unsigned long)proxy_forwarder_sys);
        snprintf(bootstrap->bridge,sizeof(bootstrap->bridge),"%s",value);
        snprintf(bootstrap->adopt,sizeof(bootstrap->adopt),"%lx",(unsigned long)adopt_api_address());
        snprintf(bootstrap->fast,sizeof(bootstrap->fast),"%lx",(unsigned long)(getauxval(AT_HWCAP2)&2));
        snprintf(bootstrap->egl,sizeof(bootstrap->egl),"%lx",(unsigned long)gpu_egl);
        snprintf(bootstrap->gles,sizeof(bootstrap->gles),"%lx",(unsigned long)gpu_gles);
        snprintf(bootstrap->egl_cache,sizeof(bootstrap->egl_cache),"%s",getenv("XDG_CACHE_HOME")?getenv("XDG_CACHE_HOME"):"");
        uintptr_t *payload=(uintptr_t *)image.stack;
        payload+=1+payload[0]+1; /* argc, argv, NULL */
        while(*payload)payload++;
        arch_elf_auxv *payload_aux=(void *)(payload+1);
        for(;payload_aux->a_type;payload_aux++) {
            if(payload_aux->a_type==AT_PHDR)
                snprintf(bootstrap->phdr,sizeof(bootstrap->phdr),"%lx",(unsigned long)payload_aux->a_un.a_val);
            if(payload_aux->a_type==AT_PHNUM)
                snprintf(bootstrap->phnum,sizeof(bootstrap->phnum),"%lx",(unsigned long)payload_aux->a_un.a_val);
        }
        if(prctl(PR_SET_VMA,PR_SET_VMA_ANON_NAME,bootstrap,4096,MATON_GPU_BOOTSTRAP_NAME) ||
           mprotect(bootstrap,4096,PROT_READ)) return 6;
        gpu_bootstrap=bootstrap;
        state_notify_ready(0);
        adopt_launch(image.entry,image.stack);
    }
#endif
    state_notify_ready(0);
    proxy_enter(image.entry,image.stack);
}
int main(int argc,char **argv,char **environ) {
    int result=loader_main(argc,argv,environ);
    state_notify_ready(result?-ENOEXEC:0);
    return result;
}

#endif

/* Clang may emit this helper for aggregate initialization even with -fno-builtin. */
void *memset(void *destination, int value, size_t count) {
    unsigned char *bytes = destination;
    for (size_t i = 0; i < count; ++i) bytes[i] = (unsigned char)value;
    return destination;
}

void *memcpy(void *destination,const void *source,size_t count){p_copy(destination,source,count);return destination;}
