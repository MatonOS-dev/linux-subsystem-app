#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

/* C reference raw gateway for this unfiltered unit; the filtered companion
 * links the production assembly gateway and BPF. */
long proxy_raw(long n,long a,long b,long c,long d,long e,long f) {
    register long r10 __asm__("r10")=d,r8 __asm__("r8")=e,r9 __asm__("r9")=f;
    long result;
    __asm__ volatile("syscall":"=a"(result):"a"(n),"D"(a),"S"(b),"d"(c),
                     "r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");
    return result;
}
extern long proxy_sigaltstack_on_stack(stack_t *,void *);
static void *replacement;
static size_t stack_size=65536;
static volatile sig_atomic_t installed,delivered_on_replacement;

static void probe(int signal,siginfo_t *info,void *opaque) {
    (void)signal;
    (void)info;
    if(!installed) {
        stack_t next={.ss_sp=replacement,.ss_size=stack_size};
        long result=proxy_sigaltstack_on_stack(&next,
                            (char *)replacement+stack_size-128);
        assert(result==0);
        ucontext_t *context=opaque;
        context->uc_stack.ss_sp=replacement;
        context->uc_stack.ss_size=stack_size;
        context->uc_stack.ss_flags=0;
        installed=1;
        return;
    }
    volatile char local;
    uintptr_t address=(uintptr_t)&local;
    delivered_on_replacement=address>=(uintptr_t)replacement &&
                             address<(uintptr_t)replacement+stack_size;
}

int main(void) {
    void *original=mmap(NULL,stack_size,PROT_READ|PROT_WRITE,
                        MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    replacement=mmap(NULL,stack_size,PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(original!=MAP_FAILED && replacement!=MAP_FAILED);
    stack_t initial={.ss_sp=original,.ss_size=stack_size};
    assert(sigaltstack(&initial,NULL)==0);
    struct sigaction action={.sa_sigaction=probe,.sa_flags=SA_SIGINFO|SA_ONSTACK};
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1,&action,NULL)==0);

    /* The first handler runs on original and replaces it while still active.
     * This used to fail with EPERM because the caller remained on original. */
    assert(raise(SIGUSR1)==0 && installed);
    stack_t current;
    assert(sigaltstack(NULL,&current)==0);
    assert(current.ss_sp==replacement && !(current.ss_flags&SS_DISABLE));
    assert(raise(SIGUSR1)==0 && delivered_on_replacement);
    puts("PASS active-handler sigaltstack replacement from fresh stack");
    return 0;
}
