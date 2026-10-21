/* A resident pthread must install its stack with SIGSYS blocked; payload
 * sigaltstack must still trap. Exercise the actual kernel BPF in both cases. */
#include "../proxy.c"
extern long resident_stack(stack_t *,stack_t *),payload_stack(stack_t *,stack_t *);
extern char resident_stack_end[];
__asm__(".text\n"
        ".global resident_stack,resident_stack_end,payload_stack\n"
        "resident_stack: mov $131,%eax; syscall; ret\n"
        "resident_stack_end:\n"
        "payload_stack: mov $131,%eax; syscall; ret\n");
static volatile int trapped;
static void stack_trap(int sig,siginfo_t *info,void *opaque){
    if(sig==SIGSYS&&info->si_syscall==131)trapped++;
    ARCH_CONTEXT((ucontext_t*)opaque,ARCH_RESULT)=-EPERM;
}
#define CHECK(x) do {if(!(x)){RAW(PROXY_SC_exit_group,__LINE__,0,0,0,0,0);__builtin_unreachable();}} while(0)
int main(void){
    CHECK(RAW(PROXY_SC_prctl,PR_SET_NO_NEW_PRIVS,1,0,0,0,0)==0);
    struct kernel_action action={(uintptr_t)stack_trap,SA_SIGINFO|0x04000000,(uintptr_t)proxy_restore,0};
    CHECK(RAW(PROXY_SC_rt_sigaction,SIGSYS,&action,0,8,0,0)==0);
    proxy_sigmask_t mask=SIGSYS_BIT;
    CHECK(RAW(PROXY_SC_rt_sigprocmask,SIG_BLOCK,&mask,0,8,0,0)==0);
    struct arch_ip_range range={.begin=(uintptr_t)resident_stack,.end=(uintptr_t)resident_stack_end};
    CHECK(arch_app_install()==0);
    CHECK(arch_filter_install(&range,1)==0);
    stack_t ss={0};
    CHECK(resident_stack(NULL,&ss)==0&&trapped==0);
    CHECK(RAW(PROXY_SC_rt_sigprocmask,SIG_UNBLOCK,&mask,0,8,0,0)==0);
    CHECK(payload_stack(NULL,&ss)==-EPERM&&trapped==1);
    const char msg[]="PASS resident sigaltstack with SIGSYS blocked; payload stack still traps\n";
    RAW(PROXY_SC_write,1,msg,sizeof(msg)-1,0,0,0);
    RAW(PROXY_SC_exit_group,0,0,0,0,0,0);__builtin_unreachable();
}
