/* Exercise the production routing filter and dispatch guards. */
#include "../proxy.c"
#define CHECK(x) do {if(!(x)){proxy_log("reservation-test",__LINE__,0);return 1;}}while(0)
static volatile int caught;
static void inspect(int sig,siginfo_t *info,void *opaque) {
    (void)sig;(void)info;caught++;
    ARCH_CONTEXT((ucontext_t *)opaque,ARCH_RESULT)=4321;
}
int main(void) {
    proxy_bionic32_base=0x40000000;
    proxy_bionic64_base=UINT64_C(0x2000000000);
    CHECK(proxy_native_prepare()==0);
    CHECK(RAW(PROXY_SC_mmap,proxy_bionic64_base,BIONIC64_SIZE,PROT_READ,
        MAP_PRIVATE|MAP_ANONYMOUS|0x100000,-1,0)==-EEXIST);
    proxy_arg_t native64_args[6]={proxy_bionic64_base,4096,PROT_READ|PROT_EXEC,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,(proxy_arg_t)-1,0};
    CHECK(proxy_direct_dispatch(arch_encode_syscall(PROXY_SC_mmap),native64_args)==-EACCES);
    CHECK(proxy_direct_dispatch(arch_encode_syscall(PROXY_SC_mprotect),native64_args)==-EACCES);
    CHECK(proxy_direct_dispatch(arch_encode_syscall(PROXY_SC_munmap),native64_args)==-EACCES);
    CHECK(RAW(PROXY_SC_mmap,proxy_bionic32_base,4096,PROT_READ,
        MAP_PRIVATE|MAP_ANONYMOUS|0x100000,-1,0)==-EEXIST);
    proxy_arg_t args[6]={proxy_bionic32_base,4096,PROT_READ|PROT_EXEC,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,(proxy_arg_t)-1,0};
    CHECK(proxy_direct_dispatch(arch_encode_syscall(PROXY_SC_mmap),args)==-EACCES);
    args[2]=0;CHECK(proxy_direct_dispatch(arch_encode_syscall(PROXY_SC_munmap),args)==-EACCES);
    args[2]=PROT_READ|PROT_EXEC;
    CHECK(proxy_direct_dispatch(arch_encode_syscall(PROXY_SC_mprotect),args)==-EACCES);
    /* Deliberately replace one reserved page via the internal gateway. The
     * same numeric PC with x86_64 audit arch must not gain the i386 exemption. */
    unsigned char *site=(void *)(uintptr_t)proxy_bionic32_base;
    CHECK(RAW(PROXY_SC_mmap,site,4096,3,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(long)site);
    const unsigned char code[]={0xb8,1,1,0,0,0x0f,0x05,0xc3}; /* openat */
    p_copy(site,code,sizeof(code));CHECK(RAW(PROXY_SC_mprotect,site,4096,5,0,0,0)==0);
    struct kernel_action action={(uintptr_t)inspect,SA_SIGINFO|SA_RESTORER,(uintptr_t)proxy_restore,0};
    CHECK(RAW(PROXY_SC_rt_sigaction,SIGSYS,&action,0,8,0,0)==0);
    CHECK(RAW(PROXY_SC_prctl,PR_SET_NO_NEW_PRIVS,1,0,0,0,0)==0);
    CHECK(arch_filter_install(NULL,0)==0);
    long (*volatile invoke)(void)=(void *)site;
    CHECK(invoke()==4321 && caught==1);
    const char message[]="PASS reserved native range rejects payload mapping/unmapping; x86_64 syscall in i386 range still traps\n";
    RAW(PROXY_SC_write,1,message,sizeof(message)-1,0,0,0);
    RAW(PROXY_SC_exit_group,0,0,0,0,0,0);__builtin_unreachable();
}
