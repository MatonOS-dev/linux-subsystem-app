/* Linux layer v3: resident x86_64 ELF loader. No libc calls after entry. */
#include "proxy.h"
#include "vfs.h"
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sys/prctl.h>
#include <signal.h>

#define PAGE 4096UL
#define DOWN(x) ((x)&~(PAGE-1))
#define UP(x) (((x)+PAGE-1)&~(PAGE-1))
#define STACK_SIZE (4UL<<20)
struct mapped { uintptr_t entry, phdr, base; unsigned phnum; char interp[4096]; };
size_t p_len(const char *s) { size_t n=0; while(s[n]) n++; return n; }
void p_copy(void *d,const void *s,size_t n) { unsigned char *a=d; const unsigned char *b=s; while(n--) *a++=*b++; }
int p_equal(const char *a,const char *b) { while(*a && *a==*b) {a++;b++;} return *a==*b; }
static uintptr_t inherited_aux[64];
uintptr_t proxy_aux(unsigned type){return type<64?inherited_aux[type]:0;}
static int bad(long n) { return (unsigned long)n >= (unsigned long)-4095; }
static int map_elf(const char *path,struct mapped *out) {
    Elf64_Ehdr eh; Elf64_Phdr *ph=NULL;
    long fd=RAW(SYS_openat,AT_FDCWD,path,O_RDONLY|O_CLOEXEC,0,0,0);
    if(bad(fd)) return (int)fd;
    int error=-ENOEXEC;
    if(RAW(SYS_pread64,fd,&eh,sizeof(eh),0,0,0)!=(long)sizeof(eh)) goto done;
    if(eh.e_ident[0]!=127 || eh.e_ident[1]!='E' || eh.e_ident[2]!='L' || eh.e_ident[3]!='F' ||
       eh.e_ident[EI_CLASS]!=ELFCLASS64 || eh.e_machine!=EM_X86_64 ||
       (eh.e_type!=ET_DYN && eh.e_type!=ET_EXEC) || !eh.e_phnum ||
       eh.e_phentsize!=sizeof(Elf64_Phdr)) goto done;
    size_t bytes=eh.e_phnum*sizeof(Elf64_Phdr);
    ph=vfs_alloc(bytes);if(!ph){error=-ENOMEM;goto done;}
    if(RAW(SYS_pread64,fd,ph,bytes,eh.e_phoff,0,0)!=(long)bytes) goto done;
    uintptr_t low=UINTPTR_MAX,high=0;
    for(unsigned i=0;i<eh.e_phnum;i++) if(ph[i].p_type==PT_LOAD) {
        if(ph[i].p_filesz>ph[i].p_memsz || ph[i].p_vaddr+ph[i].p_memsz<ph[i].p_vaddr) goto done;
        if(DOWN(ph[i].p_vaddr)<low) low=DOWN(ph[i].p_vaddr);
        if(UP(ph[i].p_vaddr+ph[i].p_memsz)>high) high=UP(ph[i].p_vaddr+ph[i].p_memsz);
    }
    if(high<=low) goto done;
    /* Refuse collision with the resident proxy rather than overwriting it. */
    if(eh.e_type==ET_EXEC && high>0x600000000000UL && low<0x600004000000UL) goto done;
    long area=RAW(SYS_mmap,eh.e_type==ET_EXEC?low:0,high-low,PROT_NONE,
                  MAP_PRIVATE|MAP_ANONYMOUS|(eh.e_type==ET_EXEC?MAP_FIXED:0),-1,0);
    if(bad(area)) {error=(int)area; goto done;}
    uintptr_t bias=(uintptr_t)area-low;
    out->base=eh.e_type==ET_DYN?bias:0; out->entry=bias+eh.e_entry;
    out->phnum=eh.e_phnum; out->phdr=0; out->interp[0]=0;
    for(unsigned i=0;i<eh.e_phnum;i++) {
        Elf64_Phdr *p=&ph[i];
        if(p->p_type==PT_INTERP) {
            if(p->p_filesz>=sizeof(out->interp) || !p->p_filesz) goto done;
            if(RAW(SYS_pread64,fd,out->interp,p->p_filesz,p->p_offset,0,0)!=(long)p->p_filesz) goto done;
            if(out->interp[p->p_filesz-1])goto done;
        }
        if(p->p_type==PT_PHDR) out->phdr=bias+p->p_vaddr;
        if(p->p_type!=PT_LOAD || !p->p_memsz) continue;
        int prot=((p->p_flags&PF_R)?PROT_READ:0)|((p->p_flags&PF_W)?PROT_WRITE:0)|((p->p_flags&PF_X)?PROT_EXEC:0);
        uintptr_t start=bias+DOWN(p->p_vaddr),end=bias+UP(p->p_vaddr+p->p_memsz);
        long r=RAW(SYS_mmap,start,end-start,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
        if(bad(r)) {error=(int)r;goto done;}
        if(p->p_filesz && RAW(SYS_pread64,fd,bias+p->p_vaddr,p->p_filesz,p->p_offset,0,0)!=(long)p->p_filesz) goto done;
        r=RAW(SYS_mprotect,start,end-start,prot,0,0,0);
        if(bad(r)) {error=(int)r;goto done;}
        proxy_image_map(start,end,DOWN(p->p_vaddr),path);
        if(!out->phdr && eh.e_phoff>=p->p_offset && eh.e_phoff+bytes<=p->p_offset+p->p_filesz)
            out->phdr=bias+p->p_vaddr+eh.e_phoff-p->p_offset;
    }
    error=out->phdr?0:-ENOEXEC;
done:
    vfs_free(ph);RAW(SYS_close,fd,0,0,0,0,0); return error;
}
static uintptr_t stack_string(uintptr_t *cursor,const char *s) {
    size_t n=p_len(s)+1; *cursor-=n; p_copy((void *)*cursor,s,n); return *cursor;
}
static int load_inner(const char *path,char *const *argv,char *const *env,struct image *image,unsigned depth) {
    if(depth>4) return -ELOOP;
    char header[256];long path_error=0;
    char *resolved=vfs_resolve(AT_FDCWD,path,1,&path_error);
    if(!resolved)return (int)path_error;
    long fd=RAW(SYS_openat,AT_FDCWD,resolved,O_RDONLY|O_CLOEXEC,0,0,0);
    vfs_free(resolved);
    if(bad(fd)) return (int)fd;
    long got=RAW(SYS_pread64,fd,header,sizeof(header)-1,0,0,0);
    RAW(SYS_close,fd,0,0,0,0,0);
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
    size_t ac=0,ec=0,total=65536;
    while(argv && argv[ac]){size_t n=p_len(argv[ac]);if(n>SIZE_MAX-total-sizeof(uintptr_t)-1)return -E2BIG;total+=n+1+sizeof(uintptr_t);ac++;}
    while(env && env[ec]){size_t n=p_len(env[ec]);if(n>SIZE_MAX-total-sizeof(uintptr_t)-1)return -E2BIG;total+=n+1+sizeof(uintptr_t);ec++;}
    if(ac>SIZE_MAX/sizeof(uintptr_t)-1 || ec>SIZE_MAX/sizeof(uintptr_t)-1 || total>SIZE_MAX-4095)return -E2BIG;
    uintptr_t *av=vfs_alloc((ac+1)*sizeof(*av)),*ev=vfs_alloc((ec+1)*sizeof(*ev));
    if(!av || !ev){vfs_free(av);vfs_free(ev);return -ENOMEM;}
    size_t stack_size=UP(total);if(stack_size<STACK_SIZE)stack_size=STACK_SIZE;
    long stack=RAW(SYS_mmap,0,stack_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(bad(stack)){vfs_free(av);vfs_free(ev);return (int)stack;}
    uintptr_t cur=(uintptr_t)stack+stack_size;
    for(size_t i=0;i<ac;i++)av[i]=stack_string(&cur,argv[i]);
    for(size_t i=0;i<ec;i++)ev[i]=stack_string(&cur,env[i]);
    char *translated=vfs_resolve(AT_FDCWD,path,1,&path_error);
    if(!translated){vfs_free(av);vfs_free(ev);return (int)path_error;}
    struct mapped main={0},interp={0};int r=map_elf(translated,&main);vfs_free(translated);
    if(r){vfs_free(av);vfs_free(ev);return r;}
    uintptr_t entry=main.entry;
    if(main.interp[0]){
        translated=vfs_resolve(AT_FDCWD,main.interp,1,&path_error);
        if(!translated){vfs_free(av);vfs_free(ev);return (int)path_error;}
        r=map_elf(translated,&interp);vfs_free(translated);if(r){vfs_free(av);vfs_free(ev);return r;}entry=interp.entry;
    }
    uintptr_t execfn=stack_string(&cur,path),platform=stack_string(&cur,"x86_64");
    cur-=16; uintptr_t random=cur;
    if(RAW(SYS_getrandom,random,16,0,0,0,0)!=16) return -EIO;
    Elf64_auxv_t aux[]={
        {AT_PHDR,{main.phdr}},{AT_PHENT,{sizeof(Elf64_Phdr)}},{AT_PHNUM,{main.phnum}},
        {AT_PAGESZ,{PAGE}},{AT_BASE,{interp.base}},{AT_ENTRY,{main.entry}},
        {AT_UID,{RAW(SYS_getuid,0,0,0,0,0,0)}},{AT_EUID,{RAW(SYS_geteuid,0,0,0,0,0,0)}},
        {AT_GID,{RAW(SYS_getgid,0,0,0,0,0,0)}},{AT_EGID,{RAW(SYS_getegid,0,0,0,0,0,0)}},
        {AT_SECURE,{0}},{AT_RANDOM,{random}},{AT_EXECFN,{execfn}},{AT_PLATFORM,{platform}},
        {AT_HWCAP,{proxy_aux(AT_HWCAP)}},{AT_HWCAP2,{proxy_aux(AT_HWCAP2)}},
        {AT_SYSINFO_EHDR,{proxy_aux(AT_SYSINFO_EHDR)}},{AT_CLKTCK,{100}},{AT_NULL,{0}}};
    size_t words=1+ac+1+ec+1+sizeof(aux)/(sizeof(uintptr_t));
    uintptr_t sp=(cur-words*sizeof(uintptr_t))&~15UL,*v=(uintptr_t *)sp;
    *v++=ac; for(unsigned i=0;i<ac;i++) *v++=av[i]; *v++=0;
    for(unsigned i=0;i<ec;i++) *v++=ev[i];
    *v++=0; p_copy(v,aux,sizeof(aux));
    vfs_free(av);vfs_free(ev);image->entry=entry;image->stack=sp;return 0;
}
int proxy_load(const char *path,char *const *argv,char *const *env,struct image *image) {
    proxy_log_bwrap_args(path,argv);
    int r=load_inner(path,argv,env,image,0);if(!r)vfs_executable(path);return r;
}
#ifndef PROXY_LIBRARY
int main(int argc,char **argv,char **environ) {
    char **end=environ;while(*end)end++;
    Elf64_auxv_t *original=(void *)(end+1);
    for(;original->a_type;original++)if(original->a_type<64)inherited_aux[original->a_type]=original->a_un.a_val;
    extern const char *proxy_tmpdir;
    for(unsigned e=0;environ[e];e++){const char *s=environ[e];if(s[0]=='T' && s[1]=='M' && s[2]=='P' && s[3]=='D' && s[4]=='I' && s[5]=='R' && s[6]=='=')proxy_tmpdir=s+7;}
    if(proxy_diagnostics_init())return 1;
    vfs_temp_gc();
    int android=0,i=1; long parent=0;
    while(i<argc && argv[i][0]=='-') {
        if(p_equal(argv[i],"--parent-pid") && i+1<argc) {
            const char *number=argv[i+1];
            if(!*number)return 2;
            for(;*number;number++) {if(*number<'0'||*number>'9'||parent>214748364)return 2;parent=parent*10+*number-'0';}
            if(parent<=0 || RAW(SYS_prctl,PR_SET_PDEATHSIG,SIGKILL,0,0,0,0)<0 ||
                    RAW(SYS_getppid,0,0,0,0,0,0)!=parent)return 2;
            i+=2;
        }
#ifdef HOST_COMMAND_TEST
        else if(p_equal(argv[i],"--host-command-fd") && i+1<argc) {extern int proxy_host_test(int);int fd=0;for(char *s=argv[i+1];*s;s++){if(*s<'0'||*s>'9')return 2;fd=fd*10+*s-'0';}proxy_host_test(fd);i+=2;}
#endif
        else if(p_equal(argv[i],"--host-command") && i+1<argc) {if(proxy_host_command(argv[i+1]))return 2;i+=2;}
        else if(p_equal(argv[i],"--dns-socket") && i+1<argc) {if(proxy_dns_socket(argv[i+1]))return 2;i+=2;}
        else if(p_equal(argv[i],"--dns-forwarder") && i+1<argc) {if(proxy_dns_forwarder(argv[i+1]))return 2;i+=2;}
        else if(p_equal(argv[i],"--android-filter")) {android=1;i++;}
        else if(p_equal(argv[i],"--trace")) {extern int proxy_trace;proxy_trace=1;i++;}
        else if(p_equal(argv[i],"--map") && i+2<argc) {if(proxy_mount(argv[i+1],argv[i+2]))return 2;i+=3;}
        else if(p_equal(argv[i],"--root") && i+1<argc) {if(vfs_bootstrap_root(argv[i+1])) return 2;i+=2;}
        else {proxy_log("usage",0,-EINVAL);return 2;}
    }
    if(i==argc) return 2;
    vfs_gpu_init();
    struct image image; int r=proxy_load(argv[i],argv+i,environ,&image);
    if(r) {proxy_log("load-error",0,r);return 1;}
    r=proxy_install(android);
    if(r) {proxy_log("filter-error",0,r);return 1;}
    proxy_host_snapshot(environ);
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
