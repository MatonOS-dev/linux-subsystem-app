/* Exercise stack replacement inside SIGSYS under the production BPF.
 * A nested TRAP while SIGSYS is blocked kills the process before logging. */
#include "../proxy.c"
extern long proxy_sigaltstack_on_stack(stack_t *,void *);
extern long payload_mprotect(long,long,long),payload_stack(void);
__asm__(".text\n.global payload_mprotect\npayload_mprotect: mov $10,%eax; syscall; ret\n.global payload_stack\npayload_stack: mov $131,%eax; xor %edi,%edi; xor %esi,%esi; syscall; ret\n");
static void *replacement;
static size_t stack_size=65536;
static volatile int installed,probed;
#define CHECK(x) do {if(!(x)){RAW(PROXY_SC_exit_group,__LINE__,0,0,0,0,0);__builtin_unreachable();}} while(0)
static void probe(int sig,siginfo_t *info,void *opaque) {
    CHECK(sig==SIGSYS && info->si_syscall==131);
    volatile char local;
    if(!installed) {
        stack_t next={.ss_sp=replacement,.ss_size=stack_size};
        CHECK(RAW(PROXY_SC_sigaltstack,&next,0,0,0,0,0)==-EPERM);
        CHECK(proxy_sigaltstack_on_stack(&next,(char *)replacement+stack_size-128)==0);
        ((ucontext_t *)opaque)->uc_stack=next;
        installed=1;
    } else {
        CHECK((uintptr_t)&local>=(uintptr_t)replacement &&
              (uintptr_t)&local<(uintptr_t)replacement+stack_size);
        probed++;
    }
    ARCH_CONTEXT(opaque,ARCH_RESULT)=0;
}
int main(void) {
    long original=RAW(PROXY_SC_mmap,0,stack_size,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    long next=RAW(PROXY_SC_mmap,0,stack_size,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(original>0 && next>0); replacement=(void *)next;
    stack_t initial={.ss_sp=(void *)original,.ss_size=stack_size};
    CHECK(RAW(PROXY_SC_sigaltstack,&initial,0,0,0,0,0)==0);
    struct kernel_action action={(uintptr_t)probe,SA_SIGINFO|SA_ONSTACK|0x04000000,(uintptr_t)proxy_restore,0};
    CHECK(RAW(PROXY_SC_rt_sigaction,SIGSYS,&action,0,8,0,0)==0);
    CHECK(RAW(PROXY_SC_prctl,PR_SET_NO_NEW_PRIVS,1,0,0,0,0)==0);
    CHECK(arch_app_install()==0 && arch_filter_install(NULL,0)==0);
    /* Two ordinary payload syscalls: reinstall, return via rt_sigreturn,
     * then deliver another frame on the replacement stack. */
    CHECK(payload_stack()==0);
    CHECK(payload_mprotect(original,4096,5)==0);
    CHECK(payload_stack()==0);
    CHECK(payload_mprotect(original,4096,5)==0);
    CHECK(installed && probed==1);
    const char msg[]="PASS filtered SIGSYS altstack reinstall and subsequent mprotect\n";
    RAW(PROXY_SC_write,1,msg,sizeof(msg)-1,0,0,0);
    RAW(PROXY_SC_exit_group,0,0,0,0,0,0);__builtin_unreachable();
}
