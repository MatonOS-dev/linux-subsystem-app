#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <ucontext.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <elf.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/stat.h>
extern long rw_open(long,const char *,long,long);
extern long rw_open9(long,const char *,long,long);
extern long rw_stat(long,const char *,struct stat *,long);
extern long rw_statx(long,const char *,long,long,struct statx *);
extern long rw_close(long);
extern long rw_fstat(long,struct stat *);
extern long rw_lseek(long,long,long);
extern long rw_readlink(const char *,char *,long);
extern long rw_readlinkat(long,const char *,char *,long);
extern long rw_getdents(long,void *,long);
extern long rw_fcntl(long,long,long);
extern long rw_preserve_link(const char *,char *,long);
extern long rw_preserve(const char *,struct stat *);
extern long rw_overlap_outer(long,const char *,long,long);
extern long rw_overlap_inner(long,const char *,long,long);
extern long rw_branch(long,const char *,long,long);
extern long rw_redzone(long,const char *,long,long);
extern long rw_straddle(long,const char *,long,long);
static int rewritten(const void *f){
    const unsigned char *p=f;
    for(unsigned i=0;i<20;i++){
        if(p[i]==0xb8 && p[i+5]==0xff && p[i+6]==0xd0)return 1;
        if(p[i]==0x0f && p[i+1]==5)return 0;
    }
    return 0;
}
static void one(void){
    long fd=rw_open(AT_FDCWD,"/etc/hosts",O_RDONLY,0);assert(fd>=0);close((int)fd);
    fd=rw_open9(AT_FDCWD,"/etc/hosts",O_RDONLY,0);assert(fd>=0);close((int)fd);
    struct stat st;assert(rw_stat(AT_FDCWD,"/etc/hosts",&st,0)==0 && st.st_size>0);
    struct statx sx;assert(rw_statx(AT_FDCWD,"/etc/hosts",0,STATX_BASIC_STATS,&sx)==0 && sx.stx_size>0);
}
static void *hammer(void *unused){(void)unused;for(int i=0;i<2000;i++)one();return NULL;}
static volatile sig_atomic_t signals;
static volatile sig_atomic_t inside_proxy,reenter_signals,reentry_failed;
static void tick(int n,siginfo_t *info,void *context){
    (void)n;(void)info;signals++;
    if(reenter_signals && signals%100==0){
        struct stat st;
        if(rw_stat(AT_FDCWD,"/etc/hosts",&st,0))reentry_failed=1;
    }
    uintptr_t pc=(uintptr_t)((ucontext_t *)context)->uc_mcontext.gregs[REG_RIP];
    if(pc>=0x600000000000ULL && pc<0x600004000000ULL)inside_proxy++;
}
static void denied(int n,siginfo_t *info,void *context){
    assert(n==SIGSYS && info->si_code==1);
    ((ucontext_t *)context)->uc_mcontext.gregs[REG_RAX]=-EPERM;
}
static void nested(void){
    struct sigaction sa={.sa_sigaction=denied,.sa_flags=SA_SIGINFO};
    assert(!sigaction(SIGSYS,&sa,NULL));
    struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_newfstatat,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog filter={sizeof(code)/sizeof(code[0]),code};
    assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));
    assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&filter));
    struct stat st;assert(rw_stat(AT_FDCWD,"/etc/hosts",&st,0)==-EPERM);
    _exit(0);
}
static void protected_file(const char *path,int enabled){
    int fd=open(path,O_RDONLY);struct stat st;assert(fd>=0 && !fstat(fd,&st));
    unsigned char *map=mmap(NULL,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);assert(map!=MAP_FAILED);
    Elf64_Ehdr *eh=(void *)map;Elf64_Shdr *sections=(void *)(map+eh->e_shoff);
    size_t offset=0;
    for(unsigned i=0;i<eh->e_shnum;i++)if(sections[i].sh_type==SHT_DYNSYM){
        Elf64_Sym *symbols=(void *)(map+sections[i].sh_offset);
        const char *strings=(void *)(map+sections[sections[i].sh_link].sh_offset);
        for(unsigned j=0;j<sections[i].sh_size/sizeof(*symbols);j++)
            if(!strcmp(strings+symbols[j].st_name,"rw_open")){
                Elf64_Shdr text=sections[symbols[j].st_shndx];
                offset=text.sh_offset+symbols[j].st_value-text.sh_addr;
            }
    }
    assert(offset);
    assert(!mprotect(map,st.st_size,PROT_READ|PROT_EXEC));
    long (*fn)(long,const char *,long,long)=(void *)(map+offset);
    assert(!rewritten(fn));
    long opened=fn(AT_FDCWD,"/etc/hosts",O_RDONLY,0);assert(opened>=0);close((int)opened);
    assert(rewritten(fn)==enabled);
    assert(!munmap(map,st.st_size));
    /* Replacing a pending file mapping with anonymous bytes via mremap must
     * invalidate its file provenance, even if the copied ELF looks identical. */
    map=mmap(NULL,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);assert(map!=MAP_FAILED);
    void *anon=mmap(NULL,st.st_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(anon!=MAP_FAILED);
    memcpy(anon,map,st.st_size);
    assert(mremap(anon,st.st_size,st.st_size,MREMAP_MAYMOVE|MREMAP_FIXED,map)==map);
    assert(!mprotect(map,st.st_size,PROT_READ|PROT_EXEC));
    assert(!rewritten(map+offset));assert(!munmap(map,st.st_size));
    map=mmap(NULL,st.st_size,PROT_EXEC,MAP_PRIVATE,fd,0);assert(map!=MAP_FAILED);
    assert(!mprotect(map,st.st_size,PROT_READ|PROT_EXEC));
    assert(!rewritten(map+offset));assert(!munmap(map,st.st_size));
    close(fd);
}
int main(int argc,char **argv){
    assert(argc==3);int enabled=atoi(argv[1]);
    assert(!rewritten(rw_open) && !rewritten(rw_open9));
    assert(!rewritten(rw_stat) && !rewritten(rw_statx));
    assert(!rewritten(rw_overlap_outer) && !rewritten(rw_overlap_inner));
    struct stat preserved;assert(!rw_preserve("/etc/hosts",&preserved));
    char preserved_link[4096];assert(!rw_preserve_link("/proc/self/exe",preserved_link,sizeof(preserved_link)));
    assert(!rewritten(rw_branch) && !rewritten(rw_redzone) && !rewritten(rw_straddle));
    one();
    long check=rw_open(AT_FDCWD,"/etc/hosts",O_RDONLY,0);assert(check>=0);
    struct stat st;assert(!rw_fstat(check,&st));assert(rw_lseek(check,0,SEEK_END)==st.st_size);
    assert(rw_fcntl(check,F_GETFD,0)>=0);
    long duplicate=rw_fcntl(check,F_DUPFD_CLOEXEC,128);assert(duplicate>=128);assert(!rw_close(duplicate));
    struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET};
    assert(!rw_fcntl(check,F_GETLK,(long)&lock));assert(!rw_close(check));
    char buffer[8192];assert(rw_readlink("/proc/self/exe",buffer,sizeof(buffer))>0);
    assert(rw_readlinkat(AT_FDCWD,"/proc/self/exe",buffer,sizeof(buffer))>0);
    check=rw_open(AT_FDCWD,"/tmp",O_RDONLY|O_DIRECTORY,0);assert(check>=0);
    assert(rw_getdents(check,buffer,sizeof(buffer))>=0);assert(!rw_close(check));
    assert(rewritten(rw_close)==enabled && rewritten(rw_fstat)==enabled && rewritten(rw_lseek)==enabled);
    assert(rewritten(rw_readlink)==enabled && rewritten(rw_readlinkat)==enabled && rewritten(rw_getdents)==enabled);
    assert(rewritten(rw_fcntl)==enabled);
    assert(rewritten(rw_open)==enabled && rewritten(rw_open9)==enabled);
    assert(rewritten(rw_stat)==enabled && rewritten(rw_statx)==enabled);
    protected_file(argv[2],enabled);
    long fd=rw_branch(AT_FDCWD,"/etc/hosts",O_RDONLY,0);assert(fd>=0);close((int)fd);
    fd=rw_redzone(AT_FDCWD,"/etc/hosts",O_RDONLY,0);assert(fd>=0);close((int)fd);
    fd=rw_straddle(AT_FDCWD,"/etc/hosts",O_RDONLY,0);assert(fd>=0);close((int)fd);
    /* JIT/anonymous executable bytes retain their original syscall opcode. */
    unsigned char code[]={0xb8,1,1,0,0,0x0f,5,0xc3};
    void *jit=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(jit!=MAP_FAILED);memcpy(jit,code,sizeof(code));assert(!mprotect(jit,4096,PROT_READ|PROT_EXEC));
    long (*call)(long,const char *,long,long)=jit;
    fd=call(AT_FDCWD,"/etc/hosts",O_RDONLY,0);assert(fd>=0);close((int)fd);
    assert(!memcmp(jit,code,sizeof(code)));assert(!munmap(jit,4096));
    for(int i=0;i<3;i++){
        void *lib=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);if(!lib){puts(dlerror());abort();}
        long (*fn)(long,const char *,long,long)=dlsym(lib,"rw_open");assert(fn && !rewritten(fn));
        fd=fn(AT_FDCWD,"/etc/hosts",O_RDONLY,0);assert(fd>=0);close((int)fd);assert(rewritten(fn)==enabled);assert(!dlclose(lib));
    }
    pid_t child=fork();assert(child>=0);
    if(!child){one();_exit(0);}int status;assert(waitpid(child,&status,0)==child && status==0);
    struct sigaction sa={.sa_sigaction=tick,.sa_flags=SA_SIGINFO};assert(!sigaction(SIGALRM,&sa,NULL));
    reenter_signals=enabled;
    ualarm(50,50);
    pthread_t threads[6];for(int i=0;i<6;i++)assert(!pthread_create(&threads[i],NULL,hammer,NULL));
    for(int i=0;i<6;i++)assert(!pthread_join(threads[i],NULL));
    ualarm(0,0);assert(signals>0 && !reentry_failed);
    if(enabled){
        assert(inside_proxy>0);
        child=fork();assert(child>=0);if(!child)nested();
        assert(waitpid(child,&status,0)==child && status==0);
    }
    printf("PASS rewrite=%d exec/dlopen/cache/dlclose/mprotect/munmap/fork/6-thread/signals/nested-seccomp/branch/redzone/straddle/JIT\n",enabled);
}
