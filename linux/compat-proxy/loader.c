/* Linux layer v3: resident ELF loader. The proxy payload path is libc-free. */
#include "proxy.h"
#include "state.h"
#include "vfs.h"
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
#include "../gpu-bridge-common/adopt.h"
#endif
#include "../gpu-bridge-common/bootstrap.h"
#include "../gpu-bridge-common/bridge.h"
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
struct mapped { uintptr_t entry, phdr, base, area, size, target; unsigned phnum; char interp[4096]; };
static int resident_collision(uintptr_t low,uintptr_t high) {
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

size_t p_len(const char *s) { size_t n=0; while(s[n]) n++; return n; }
void p_copy(void *d,const void *s,size_t n) { unsigned char *a=d; const unsigned char *b=s; while(n--) *a++=*b++; }
int p_equal(const char *a,const char *b) { while(*a && *a==*b) {a++;b++;} return *a==*b; }
static long loader_open(const char *path){long fd=pack_open(path,O_RDONLY|O_CLOEXEC);return fd==-ENOSYS?RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY|O_CLOEXEC,0,0,0):fd;}
static void loader_close(int fd){pack_close(fd);RAW(PROXY_SC_close,fd,0,0,0,0,0);}
/* File mappings retain page-cache sharing. Only permission refusals permit the
 * private copy fallback; pack_call translates carrier offsets before mmap. */
long proxy_file_mmap(proxy_arg_t *a) {
    int handled=0;
    long r=pack_call(PROXY_SC_mmap,a,&handled);
    if(!handled)r=RAW(PROXY_SC_mmap,a[0],a[1],a[2],a[3],a[4],a[5]);
    if(r!=-EACCES && r!=-EPERM){
        if(r>=0 && (a[2]&(PROT_READ|PROT_EXEC))==(PROT_READ|PROT_EXEC) && !(a[2]&PROT_WRITE) && (a[3]&MAP_PRIVATE))
            proxy_rewrite_mapping((int)a[4],(uintptr_t)r,(size_t)a[1],(uint64_t)a[5]);
        return r;
    }
    if((a[3]&MAP_SHARED) && (a[2]&PROT_WRITE))return r;
    if(!(a[3]&(MAP_SHARED|MAP_PRIVATE)))return r;
    long area=RAW(PROXY_SC_mmap,a[0],a[1],PROT_READ|PROT_WRITE,
                  (a[3]&~(MAP_SHARED|MAP_PRIVATE))|MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if((unsigned long)area>=(unsigned long)-4095)return area;
    size_t done=0;
    while(done<(size_t)a[1]){
        long n=pack_pread(a[4],(void *)((uintptr_t)area+done),a[1]-done,(uint64_t)a[5]+done);
        if(n==-EINTR)continue;
        if(n<0){r=n;goto failed;}
        if(!n)break;
        done+=n;
    }
    r=RAW(PROXY_SC_mprotect,area,a[1],a[2],0,0,0);
    if(r<0)goto failed;
    struct proxy_stat st;
    struct copy_log {struct copy_log *next;uint64_t dev,ino,slice;};
    static struct copy_log *logs;
    if(!arch_stat_fd(a[4],&st)){
        uint64_t slice=pack_slice_start(a[4]);
        struct copy_log *l=logs;
        while(l && (l->dev!=st.st_dev || l->ino!=st.st_ino || l->slice!=slice))l=l->next;
        if(!l){
            l=vfs_palloc(sizeof(*l));
            if(l){*l=(struct copy_log){logs,st.st_dev,st.st_ino,slice};logs=l;}
            proxy_log("exec-mmap-copy",a[4],0);
        }
    }
    if((a[2]&(PROT_READ|PROT_EXEC))==(PROT_READ|PROT_EXEC) && !(a[2]&PROT_WRITE) && (a[3]&MAP_PRIVATE))
        proxy_rewrite_mapping((int)a[4],(uintptr_t)area,(size_t)a[1],(uint64_t)a[5]);
    return area;
failed:
    RAW(PROXY_SC_munmap,area,a[1],0,0,0,0);return r;
}
static uintptr_t inherited_aux[64];
uintptr_t proxy_aux(unsigned type){return type<64?inherited_aux[type]:0;}
static int bad(long n) { return (unsigned long)n >= (unsigned long)-4095; }
/* Keep exec path diagnostics opt-in: these mirror the translation used by
 * openat and make pack lookup failures visible in the Android trace. */
static void loader_trace_path(const char *tag,const char *path){
    if(!proxy_trace)return;
    RAW(PROXY_SC_write,proxy_diagnostic_fd,tag,p_len(tag),0,0,0);
    if(path)RAW(PROXY_SC_write,proxy_diagnostic_fd,path,p_len(path),0,0,0);
    RAW(PROXY_SC_write,proxy_diagnostic_fd,"\n",1,0,0,0);
}
static void loader_trace_resolve(const char *path,const char *stage){
    if(!proxy_trace)return;
    long error=0;void *mount=NULL;
    char *resolved=vfs_resolve_mount(AT_FDCWD,path,1,&error,&mount);
    proxy_log(stage,resolved?1:error,mount!=NULL);
    if(resolved){
        loader_trace_path("v3 exec resolved ",resolved);
        char *packed=vfs_reverse_pack(resolved);
        loader_trace_path("v3 exec pack-object ",packed?packed:"(no reverse pack path)");
        if(packed)vfs_free(packed);
        struct proxy_stat st;long ps=pack_stat(resolved,&st);
        proxy_log("exec-pack-stat",ps,ps?0:st.st_mode);
        long fd=pack_open(resolved,O_RDONLY|O_CLOEXEC);
        proxy_log("exec-pack-open",fd,0);
        if(fd>=0)pack_close((int)fd);
        /* Match path_call's newfstatat path: resolve the original virtual
         * name first, then pass that translated name to the pack handler. */
        long stat_error=0;void *stat_mount=NULL;
        char *stat_path=vfs_resolve_mount(AT_FDCWD,path,1,&stat_error,&stat_mount);
        /* pack_path synthesizes the architecture's native stat wire layout. */
        struct stat wire_stat;
        proxy_arg_t a[6]={(long)AT_FDCWD,(long)stat_path,(long)&wire_stat,0,0,0};int handled=0;
        long sr=stat_path?pack_path(PROXY_SC_newfstatat,a,stat_path,&handled):stat_error;
        proxy_log("exec-openat-stat-compare",handled?sr:-ENOSYS,handled);
        if(stat_path)vfs_free(stat_path);
        vfs_free(resolved);
    } else loader_trace_path("v3 exec unresolved ",path);
}
/* Preflight both ELF classes before MAP_FIXED or a kernel exec of our peer.
 * Use fixed-width ELF records, not the current backend's native typedefs. */
static int validate_exec(const char *path,int interpreter,unsigned depth) {
    if(depth>1)return -ELIBBAD;
    long error=0;
    char *resolved=vfs_resolve(AT_FDCWD,path,1,&error);
    if(proxy_trace){
        proxy_log("exec-validate-resolve",resolved?1:error,depth);
        if(resolved){loader_trace_path("v3 exec validate resolved ",resolved);
            char *object=vfs_reverse_pack(resolved);loader_trace_path("v3 exec validate pack-object ",object?object:"(none)");if(object)vfs_free(object);}
    }
    if(!resolved)return error;
    long fd=loader_open(resolved);
    if(proxy_trace)proxy_log("exec-validate-open",fd,depth);
    if(fd<0){vfs_free(resolved);return fd;}
    struct proxy_stat st;
    error=pack_stat(resolved,&st);
    if(error==-ENOSYS)error=arch_stat_fd(fd,&st);
    if(!error && (!S_ISREG(st.st_mode) || !(st.st_mode&0111)))error=-EACCES;
    if(!error && pack_stat(resolved,&st)==-ENOSYS)error=RAW(PROXY_SC_faccessat,AT_FDCWD,resolved,1,0,0,0);
    vfs_free(resolved);
    Elf64_Ehdr h={0};Elf32_Ehdr h32;
    if(error)goto done;
    error=interpreter?-ELIBBAD:-ENOEXEC;
    unsigned char ident[EI_NIDENT];
    if(pack_pread(fd,ident,sizeof(ident),0)!=sizeof(ident))goto done;
    if(ident[0]!=127 || ident[1]!='E' || ident[2]!='L' || ident[3]!='F' ||
       ident[EI_DATA]!=ELFDATA2LSB || ident[EI_VERSION]!=EV_CURRENT)goto done;
    int bits=ident[EI_CLASS];
    if(interpreter && bits!=interpreter)goto done;
    if(bits==ELFCLASS32){
        if(pack_pread(fd,&h32,sizeof(h32),0)!=sizeof(h32))goto done;
        h.e_type=h32.e_type;h.e_machine=h32.e_machine;h.e_phoff=h32.e_phoff;
        h.e_phnum=h32.e_phnum;h.e_phentsize=h32.e_phentsize;
        if(h.e_phentsize!=sizeof(Elf32_Phdr))goto done;
    }else if(bits==ELFCLASS64){
        if(pack_pread(fd,&h,sizeof(h),0)!=sizeof(h))goto done;
        if(h.e_phentsize!=sizeof(Elf64_Phdr))goto done;
    }else goto done;
    if(arch_exec_class(bits,h.e_machine)<0)goto done;
    if((h.e_type!=ET_EXEC && h.e_type!=ET_DYN) || !h.e_phnum ||
       h.e_phoff>(uint64_t)st.st_size ||
       (uint64_t)h.e_phnum*h.e_phentsize>(uint64_t)st.st_size-h.e_phoff)goto done;
    char interp[4096];interp[0]=0;unsigned loads=0,has_phdr=0;
    for(unsigned i=0;i<h.e_phnum;i++){
        Elf64_Phdr p={0};uint64_t offset=h.e_phoff+(uint64_t)i*h.e_phentsize;
        if(bits==ELFCLASS32){
            Elf32_Phdr q;
            if(pack_pread(fd,&q,sizeof(q),offset)!=sizeof(q))goto done;
            p.p_type=q.p_type;p.p_offset=q.p_offset;p.p_filesz=q.p_filesz;
            p.p_memsz=q.p_memsz;p.p_vaddr=q.p_vaddr;p.p_align=q.p_align;
        }else if(pack_pread(fd,&p,sizeof(p),offset)!=sizeof(p))goto done;
        if(p.p_type==PT_PHDR)has_phdr=1;
        if(p.p_type!=PT_LOAD && p.p_type!=PT_INTERP)continue;
        if(p.p_offset>(uint64_t)st.st_size || p.p_filesz>(uint64_t)st.st_size-p.p_offset)goto done;
        if(p.p_type==PT_LOAD){
            uint64_t limit=bits==ELFCLASS32?UINT32_MAX:UINT64_MAX;
            if(p.p_filesz>p.p_memsz || p.p_vaddr>limit-4095 || p.p_memsz>limit-4095-p.p_vaddr)goto done;
            if(p.p_memsz)loads++;
            if(h.e_phoff>=p.p_offset && h.e_phoff-p.p_offset+(uint64_t)h.e_phnum*h.e_phentsize<=p.p_filesz)has_phdr=1;
        }else{
            if(interpreter || interp[0] || p.p_filesz<2 || p.p_filesz>sizeof(interp))goto done;
            if(pack_pread(fd,interp,p.p_filesz,p.p_offset)!=(long)p.p_filesz || interp[p.p_filesz-1])goto done;
        }
    }
    if(!loads || !has_phdr)goto done;
    if(interp[0]){
        loader_trace_path("v3 exec validate PT_INTERP ",interp);
        error=validate_exec(interp,bits,depth+1);
        if(proxy_trace)proxy_log("exec-validate-interp",error,depth);
    }else error=0;
done:
    if(proxy_trace)proxy_log("exec-validate-result",error,depth);
    loader_close(fd);return error;
}
static int map_elf(const char *path,struct mapped *out) {
    arch_elf_ehdr eh; arch_elf_phdr *ph=NULL;
    long fd=loader_open(path);
    if(bad(fd)) return (int)fd;
    int error=-ENOEXEC;
    if(pack_pread(fd,&eh,sizeof(eh),0)!=(long)sizeof(eh)) goto done;
    if(eh.e_ident[0]!=127 || eh.e_ident[1]!='E' || eh.e_ident[2]!='L' || eh.e_ident[3]!='F' ||
       eh.e_ident[EI_CLASS]!=ARCH_ELF_CLASS || eh.e_machine!=ARCH_ELF_MACHINE ||
       (eh.e_type!=ET_DYN && eh.e_type!=ET_EXEC) || !eh.e_phnum ||
       eh.e_phentsize!=sizeof(arch_elf_phdr)) goto done;
    size_t bytes=eh.e_phnum*sizeof(arch_elf_phdr);
    ph=vfs_alloc(bytes);if(!ph){error=-ENOMEM;goto done;}
    if(pack_pread(fd,ph,bytes,eh.e_phoff)!=(long)bytes) goto done;
    uintptr_t low=UINTPTR_MAX,high=0;
    for(unsigned i=0;i<eh.e_phnum;i++) if(ph[i].p_type==PT_LOAD) {
        if(ph[i].p_filesz>ph[i].p_memsz ||
           (uint64_t)ph[i].p_vaddr+ph[i].p_memsz>UINTPTR_MAX-(PAGE-1)) goto done;
        if(DOWN(ph[i].p_vaddr)<low) low=DOWN(ph[i].p_vaddr);
        if(UP(ph[i].p_vaddr+ph[i].p_memsz)>high) high=UP(ph[i].p_vaddr+ph[i].p_memsz);
    }
    if(high<=low) goto done;
    /* Refuse collision with the resident proxy rather than overwriting it. */
    if(eh.e_type==ET_EXEC && resident_collision(low,high)) goto done;
    /* Stage away from the caller, including fixed-address executables. */
    long area=RAW(PROXY_SC_mmap,0,high-low,PROT_NONE,
                  MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(bad(area)) {error=(int)area; goto done;}
    out->area=area;out->size=high-low;out->target=eh.e_type==ET_EXEC?low:(uintptr_t)area;
    uintptr_t bias=(uintptr_t)area-low;
    out->base=eh.e_type==ET_DYN?bias:0; out->entry=bias+eh.e_entry;
    out->phnum=eh.e_phnum; out->phdr=0; out->interp[0]=0;
    for(unsigned i=0;i<eh.e_phnum;i++) {
        arch_elf_phdr *p=&ph[i];
        if(p->p_type==PT_INTERP) {
            if(p->p_filesz>sizeof(out->interp) || !p->p_filesz) goto done;
            if(pack_pread(fd,out->interp,p->p_filesz,p->p_offset)!=(long)p->p_filesz) goto done;
            if(out->interp[p->p_filesz-1])goto done;
        }
        if(p->p_type==PT_PHDR) out->phdr=bias+p->p_vaddr;
        if(p->p_type!=PT_LOAD || !p->p_memsz) continue;
        int prot=((p->p_flags&PF_R)?PROT_READ:0)|((p->p_flags&PF_W)?PROT_WRITE:0)|((p->p_flags&PF_X)?PROT_EXEC:0);
        uintptr_t start=bias+DOWN(p->p_vaddr),end=bias+UP(p->p_vaddr+p->p_memsz);
        long r;
        uintptr_t file_end=bias+UP(p->p_vaddr+p->p_filesz);
        int congruent=(p->p_offset&(PAGE-1))==(p->p_vaddr&(PAGE-1));
        if(p->p_filesz && congruent){
            proxy_arg_t args[]={start,file_end-start,prot,MAP_PRIVATE|MAP_FIXED,fd,DOWN(p->p_offset)};
            r=proxy_file_mmap(args);
            if(bad(r)){error=(int)r;goto done;}
            /* Only the last file page needs COW zeroing. Never dirty text
             * pages without BSS (including bytes beyond p_filesz). */
            if(p->p_memsz>p->p_filesz && file_end>bias+p->p_vaddr+p->p_filesz){
                uintptr_t tail=bias+p->p_vaddr+p->p_filesz;
                r=RAW(PROXY_SC_mprotect,DOWN(tail),PAGE,PROT_READ|PROT_WRITE,0,0,0);
                if(bad(r)){error=(int)r;goto done;}
                for(uintptr_t z=tail;z<file_end;z++)*(unsigned char *)z=0;
                r=RAW(PROXY_SC_mprotect,DOWN(tail),PAGE,prot,0,0,0);
                if(bad(r)){error=(int)r;goto done;}
            }
            if(end>file_end){
                r=RAW(PROXY_SC_mmap,file_end,end-file_end,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
                if(bad(r)){error=(int)r;goto done;}
            }
        }else{
            r=RAW(PROXY_SC_mmap,start,end-start,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
            if(bad(r)){error=(int)r;goto done;}
            if(p->p_filesz && pack_pread(fd,(void *)(bias+p->p_vaddr),p->p_filesz,p->p_offset)!=(long)p->p_filesz) goto done;
            r=RAW(PROXY_SC_mprotect,start,end-start,prot,0,0,0);
            if(bad(r)){error=(int)r;goto done;}
        }
        proxy_image_map(out->target+start-out->area,out->target+end-out->area,DOWN(p->p_vaddr),path);
        if(!out->phdr && eh.e_phoff>=p->p_offset && (uint64_t)eh.e_phoff+bytes<=(uint64_t)p->p_offset+p->p_filesz)
            out->phdr=bias+p->p_vaddr+eh.e_phoff-p->p_offset;
    }
    error=out->phdr?0:-ENOEXEC;
    if(!error && eh.e_type==ET_EXEC){out->entry-=bias;out->phdr-=bias;}
done:
    if(error && out->size){RAW(PROXY_SC_munmap,out->area,out->size,0,0,0,0);out->size=0;}
    vfs_free(ph);loader_close(fd); return error;
}
/* All reads, validation, allocation and protection checks precede this point.
 * Move staged VMAs one page at a time (protections may split the reservation).
 * Like kernel exec after begin_new_exec, an unrecoverable commit failure must
 * terminate, never resume a caller whose fixed-address pages were replaced. */
static void commit_elf(struct mapped *m){
    if(m->area==m->target)return;
    for(uintptr_t offset=0;offset<m->size;offset+=PAGE){
        long r=RAW(PROXY_SC_mremap,m->area+offset,PAGE,PAGE,3,m->target+offset,0);
        if(bad(r))RAW(PROXY_SC_exit_group,127,0,0,0,0,0);
    }
    proxy_rewrite_relocate(m->area,m->target,m->size);
}
static uintptr_t stack_string(uintptr_t *cursor,const char *s) {
    size_t n=p_len(s)+1; *cursor-=n; p_copy((void *)*cursor,s,n); return *cursor;
}
static int load_inner(const char *path,char *const *argv,char *const *env,struct image *image,unsigned depth) {
    if(depth>4) return -ELOOP;
    loader_trace_resolve(path,"exec-resolve");
    char header[256];long path_error=0;
    char *resolved=vfs_resolve(AT_FDCWD,path,1,&path_error);
    if(!resolved)return (int)path_error;
    long fd=loader_open(resolved);
    struct proxy_stat packed;int packed_status=pack_stat(resolved,&packed);
    vfs_free(resolved);
    if(bad(fd)) return (int)fd;
    struct proxy_stat executable;
    int permission=packed_status==-ENOSYS?arch_stat_fd(fd,&executable):packed_status;
    if(!packed_status)executable=packed;
    if(!permission && (!S_ISREG(executable.st_mode) || !(executable.st_mode&0111)))permission=-EACCES;
    if(permission){loader_close(fd);return permission;}
    long got=pack_pread(fd,header,sizeof(header)-1,0);
    loader_close(fd);
    if(got<0) return (int)got;
    header[got]=0;
    if(got>=2 && header[0]=='#' && header[1]=='!') {
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
    int checked=validate_exec(path,0,0);if(checked){if(proxy_trace)proxy_log("exec-validate",checked,0);return checked;}
    if(proxy_trace)proxy_log("exec-validate",0,0);
    int handoff=state_exec(path,argv,env);if(handoff){if(proxy_trace)proxy_log("exec-state",handoff,0);return handoff;}
    size_t ac=0,ec=0,total=65536;
    while(argv && argv[ac]){size_t n=p_len(argv[ac]);if(n>SIZE_MAX-total-sizeof(uintptr_t)-1)return -E2BIG;total+=n+1+sizeof(uintptr_t);ac++;}
    while(env && env[ec]){size_t n=p_len(env[ec]);if(n>SIZE_MAX-total-sizeof(uintptr_t)-1)return -E2BIG;total+=n+1+sizeof(uintptr_t);ec++;}
    if(ac>SIZE_MAX/sizeof(uintptr_t)-1 || ec>SIZE_MAX/sizeof(uintptr_t)-1 || total>SIZE_MAX-4095)return -E2BIG;
    uintptr_t *av=vfs_alloc((ac+1)*sizeof(*av)),*ev=vfs_alloc((ec+1)*sizeof(*ev));
    if(!av || !ev){vfs_free(av);vfs_free(ev);return -ENOMEM;}
    size_t stack_size=UP(total);if(stack_size<STACK_SIZE)stack_size=STACK_SIZE;
    long stack=RAW(PROXY_SC_mmap,0,stack_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(bad(stack)){vfs_free(av);vfs_free(ev);return (int)stack;}
    uintptr_t cur=(uintptr_t)stack+stack_size;
    for(size_t i=0;i<ac;i++)av[i]=stack_string(&cur,argv[i]);
    for(size_t i=0;i<ec;i++)ev[i]=stack_string(&cur,env[i]);
    char *translated=vfs_resolve(AT_FDCWD,path,1,&path_error);
    if(!translated){RAW(PROXY_SC_munmap,stack,stack_size,0,0,0,0);vfs_free(av);vfs_free(ev);return (int)path_error;}
    struct mapped main={0},interp={0};int r=map_elf(translated,&main);vfs_free(translated);if(proxy_trace)proxy_log("exec-map-main",r,main.size);
    if(r)goto failed;
    uintptr_t entry=main.entry;
    if(main.interp[0]){
        loader_trace_path("v3 exec PT_INTERP ",main.interp);
        loader_trace_resolve(main.interp,"interp-resolve");
        translated=vfs_resolve(AT_FDCWD,main.interp,1,&path_error);
        if(!translated){r=path_error;goto failed;}
        r=map_elf(translated,&interp);vfs_free(translated);if(proxy_trace)proxy_log("exec-map-interp",r,interp.size);if(r)goto failed;entry=interp.entry;
    }
    uintptr_t execfn=stack_string(&cur,path),platform=stack_string(&cur,ARCH_PLATFORM);
    cur-=16; uintptr_t random=cur;
    if(RAW(PROXY_SC_getrandom,random,16,0,0,0,0)!=16){r=-EIO;goto failed;}
    int musl_syscall_fast=1;
    for(size_t i=0;env && env[i];i++)
        if(p_equal(env[i],"MATON_PROXY_REWRITE=0")){musl_syscall_fast=0;break;}
    arch_elf_auxv aux[]={
        {AT_PHDR,{main.phdr}},{AT_PHENT,{sizeof(arch_elf_phdr)}},{AT_PHNUM,{main.phnum}},
        {AT_PAGESZ,{PAGE}},{AT_BASE,{interp.base}},{AT_ENTRY,{main.entry}},
        {AT_UID,{RAW(PROXY_SC_getuid,0,0,0,0,0,0)}},{AT_EUID,{RAW(PROXY_SC_geteuid,0,0,0,0,0,0)}},
        {AT_GID,{RAW(PROXY_SC_getgid,0,0,0,0,0,0)}},{AT_EGID,{RAW(PROXY_SC_getegid,0,0,0,0,0,0)}},
        {AT_SECURE,{0}},{AT_RANDOM,{random}},{AT_EXECFN,{execfn}},{AT_PLATFORM,{platform}},
        {AT_HWCAP,{proxy_aux(AT_HWCAP)}},{AT_HWCAP2,{proxy_aux(AT_HWCAP2)}},
        {ARCH_AUX_SYSINFO,{PROXY_SYSINFO_AUX(proxy_aux(ARCH_AUX_SYSINFO))}},{AT_SYSINFO_EHDR,{proxy_aux(AT_SYSINFO_EHDR)}},
        /* Private auxv contract for the pinned static musl build. The tools
         * keep using ordinary syscalls when launched outside the proxy. */
        {0x6fff0001,{musl_syscall_fast?(uintptr_t)proxy_musl_syscall_entry:0}},
        {AT_CLKTCK,{100}},{AT_NULL,{0}}};
    size_t words=1+ac+1+ec+1+sizeof(aux)/(sizeof(uintptr_t));
    uintptr_t sp=(cur-words*sizeof(uintptr_t))&~15UL,*v=(uintptr_t *)sp;
    *v++=ac; for(unsigned i=0;i<ac;i++) *v++=av[i]; *v++=0;
    for(unsigned i=0;i<ec;i++) *v++=ev[i];
    *v++=0; p_copy(v,aux,sizeof(aux));
    vfs_free(av);vfs_free(ev);commit_elf(&main);commit_elf(&interp);
    image->entry=entry;image->stack=sp;
#ifdef MATON_GPU_BRIDGE
    /* The resident survives exec. Publish this payload, rather than leaving
     * GPU clients looking at the original bwrap/launcher link map. The private
     * mapping is copy-on-write across fork, like the payload it describes. */
    if(gpu_bootstrap) {
        if(RAW(PROXY_SC_mprotect,gpu_bootstrap,PAGE,PROT_READ|PROT_WRITE,0,0,0))return -EACCES;
        maton_gpu_payload(gpu_bootstrap,main.phdr,main.phnum);
        if(RAW(PROXY_SC_mprotect,gpu_bootstrap,PAGE,PROT_READ,0,0,0))return -EACCES;
    }
#endif
    return 0;
failed:
    if(proxy_trace)proxy_log("exec-load-failed",r,0);
    if(main.size)RAW(PROXY_SC_munmap,main.area,main.size,0,0,0,0);
    if(interp.size)RAW(PROXY_SC_munmap,interp.area,interp.size,0,0,0,0);
    RAW(PROXY_SC_munmap,stack,stack_size,0,0,0,0);
    vfs_free(av);vfs_free(ev);return r;
}
int proxy_load(const char *path,char *const *argv,char *const *env,struct image *image) {
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
    return r;
}
#ifndef PROXY_LIBRARY
int main(int argc,char **argv,char **environ) {
    char **end=environ;while(*end)end++;
    arch_elf_auxv *original=(void *)(end+1);
    for(;original->a_type;original++)if(original->a_type<64)inherited_aux[original->a_type]=original->a_un.a_val;
    extern const char *proxy_tmpdir;
    for(unsigned e=0;environ[e];e++){const char *s=environ[e];if(s[0]=='T' && s[1]=='M' && s[2]=='P' && s[3]=='D' && s[4]=='I' && s[5]=='R' && s[6]=='=')proxy_tmpdir=s+7;}
    int resumed=argc>4 && p_equal(argv[1],"--resident-state");
    if(resumed){
        if(state_import(argv[2],argv[3]))return 2;
        /* Do not leave the bearer token in the kernel's original cmdline after
         * payload entry. Import closed the private state FD already. */
        for(unsigned j=2;j<4;j++)for(char *p=argv[j];*p;p++)*p=0;
    }
    if(proxy_diagnostics_init())return 1;
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
        else if(p_equal(argv[i],"--pack-backing") && i+2<argc) {if(pack_backing(argv[i+1],argv[i+2]))return 2;i+=3;}
        else if(p_equal(argv[i],"--pack-mount") && i+3<argc) {if(pack_mount(argv[i+1],argv[i+2],argv[i+3]))return 2;i+=4;}
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
    vfs_gpu_init();
    struct image image; int r=proxy_load(argv[i],argv+i+(resumed?1:0),environ,&image);
    if(r) {proxy_log("load-error",0,r);return 1;}
#ifdef MATON_GPU_BRIDGE
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
    /* Load Android's EGL vendor implementation before the proxy snapshots
     * native-library syscall ranges. EGL dispatch loads the hardware driver
     * lazily on first initialization; calls from the adopted Linux thread
     * must keep those driver's Android syscalls native too. */
#ifdef MATON_GPU_BRIDGE
    if(gpu_bridge_enabled) {
        if(proxy_resident_fds_snapshot(0)) return 7;
        EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint major=0,minor=0,count=0;
        if(display!=EGL_NO_DISPLAY && eglInitialize(display,&major,&minor) &&
           eglBindAPI(EGL_OPENGL_ES_API)) {
            const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE};
            EGLConfig config=0;
            if(eglChooseConfig(display,attrs,&config,1,&count)&&count) {
                const EGLint pbuffer_attrs[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
                const EGLint context_attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
                EGLSurface surface=eglCreatePbufferSurface(display,config,pbuffer_attrs);
                EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,context_attrs);
                if(surface!=EGL_NO_SURFACE && context!=EGL_NO_CONTEXT &&
                   eglMakeCurrent(display,surface,surface,context)) {
                    GLuint shader=glCreateShader(GL_VERTEX_SHADER);
                    const GLchar *source="attribute vec4 p; void main(){gl_Position=p;}";
                    if(shader) { glShaderSource(shader,1,&source,NULL); glCompileShader(shader); glDeleteShader(shader); }
                    GLubyte pixel[4]={0};
                    glClearColor(1.0f,0.0f,0.0f,1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glFinish();
                    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
                }
                if(context!=EGL_NO_CONTEXT) eglDestroyContext(display,context);
                if(surface!=EGL_NO_SURFACE) eglDestroySurface(display,surface);
            }
        }
        if(proxy_resident_fds_snapshot(1)) return 7;
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
        adopt_launch(image.entry,image.stack);
    }
#endif
    proxy_enter(image.entry,image.stack);
}

#endif

/* Clang may emit this helper for aggregate initialization even with -fno-builtin. */
void *memset(void *destination, int value, size_t count) {
    unsigned char *bytes = destination;
    for (size_t i = 0; i < count; ++i) bytes[i] = (unsigned char)value;
    return destination;
}

void *memcpy(void *destination,const void *source,size_t count){p_copy(destination,source,count);return destination;}
