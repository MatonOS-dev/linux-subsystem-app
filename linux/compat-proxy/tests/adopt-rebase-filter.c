/* Use the actual resident adoption implementation, not a mock of rebase.
 * Observe its diagnostic failure paths because rebase has no return value. */
#include <linux/filter.h>
#define proxy_log rebase_log
#include "../arch/x86_64/thread-adopt.c"
#undef proxy_log
extern void proxy_log(const char *,long,long);
static volatile int rebase_errors;
void rebase_log(const char *line,long a,long b) {
    rebase_errors++;
    proxy_log(line,a,b);
}
extern int proxy_install(int);
extern long payload_stack(void),payload_mprotect(long,long,long);
__asm__(".text\n.global payload_stack\npayload_stack: mov $131,%eax; xor %edi,%edi; xor %esi,%esi; syscall; ret\n"
        ".global payload_mprotect\npayload_mprotect: mov $10,%eax; syscall; ret\n");
static struct adopt_thread *record;
static volatile int delivered;
#define CHECK(x) do {if(!(x)) {R(exit_group,__LINE__,0,0,0,0,0);__builtin_unreachable();}} while(0)
static void rebase_probe(int sig,siginfo_t *info,void *opaque) {
    CHECK(sig==SIGSYS && info->si_syscall==SYS_sigaltstack);
    void *previous=record->signal_stack;
    record->fork_context=opaque;
    fork_rebase(record);
    CHECK(!rebase_errors && record->signal_stack && record->signal_stack!=previous);
    CHECK(((ucontext_t *)opaque)->uc_stack.ss_sp==record->signal_stack);
    delivered++;
    ((ucontext_t *)opaque)->uc_mcontext.gregs[REG_RAX]=0;
}
static void exercise(void) {
    CHECK(R(unshare,CLONE_NEWUSER,0,0,0,0,0)==-EPERM);
    CHECK(payload_stack()==0 && delivered==1);
    stack_t current;
    CHECK(R(sigaltstack,0,&current,0,0,0,0)==0);
    CHECK(current.ss_sp==record->signal_stack && !(current.ss_flags&SS_DISABLE));
    CHECK(payload_mprotect((long)current.ss_sp,4096,PROT_READ|PROT_WRITE)==0);
    /* Clone-retry adoption occurs outside the registered stack: it must
     * install directly, without switching onto that same active mapping. */
    void *previous=record->signal_stack;
    fork_rebase(record);
    CHECK(!rebase_errors && record->signal_stack==previous);
    R(exit_group,0,0,0,0,0,0);__builtin_unreachable();
}
int main(void) {
    CHECK(adopt_install()==0);
    record=adopt_current(); CHECK(record);
    /* Host-command's raw monitor forks can inherit more than one record.
     * Once hc_exec clears TLS, the singleton recovery cannot choose a caller.
     * Rebase at fork time, while the inherited FS identifies it. */
    struct adopt_thread *stale=reserve();CHECK(stale);
    atomic_store(&stale->tid,INT_MAX);
    long monitor=R(clone,SIGCHLD,0,0,0,0,0);CHECK(monitor>=0);
    if(!monitor){
        proxy_host_fork_child();
        uintptr_t fs=backend_fs_read();backend_fs_write(0);
        CHECK(adopt_current()==record);
        backend_fs_write(fs);
        CHECK(live()==1 && atomic_load(&record->tid)==R(gettid,0,0,0,0,0,0));
        R(exit_group,0,0,0,0,0,0);__builtin_unreachable();
    }
    int monitor_status=0;
    CHECK(R(wait4,monitor,&monitor_status,0,0,0,0)==monitor && monitor_status==0);
    atomic_store(&stale->tid,0);
    CHECK(proxy_install(1)==0);
    /* Inherit the same errno policy shape as Flatpak, in addition to both
     * production proxy filters. Raw gateway calls must respect it too. */
    struct sock_filter filter[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_unshare,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_clone3,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|ENOSYS),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program={.len=sizeof(filter)/sizeof(filter[0]),.filter=filter};
    CHECK(R(seccomp,SECCOMP_SET_MODE_FILTER,0,&program,0,0,0)==0);
    struct {uintptr_t handler,flags,restorer;unsigned long mask;} action={
        (uintptr_t)rebase_probe,SA_SIGINFO|SA_ONSTACK|0x04000000,(uintptr_t)proxy_restore,0};
    CHECK(R(rt_sigaction,SIGSYS,&action,0,8,0,0)==0);
    /* Both the main process and an ordinary fork child must be able to
     * rebase from an active signal frame and then use mprotect. */
    long child=R(fork,0,0,0,0,0,0); CHECK(child>=0);
    if(!child) exercise();
    int status=-1;
    CHECK(R(wait4,child,&status,0,0,0,0)==child && status==0);
    exercise();
}
