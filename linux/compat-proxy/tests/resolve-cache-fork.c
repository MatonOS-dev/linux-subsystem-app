/* Deterministic fork and signal reentry at the cache critical section. */
#include "../vfs.c"
#include <signal.h>
extern void proxy_restore(void);
#define CHECK(x) do {if(!(x)){RAW(PROXY_SC_exit_group,__LINE__,0,0,0,0,0);__builtin_unreachable();}}while(0)
static volatile int delivered;
static void reenter(int sig) { (void)sig;vfs_resolve_cache_invalidate();delivered++; }
int main(void) {
    CHECK(resolve_cache_reserve(1));
    resolve_cache_count=1;
    proxy_sigmask_t mask=resolve_cache_acquire();
    long child=RAW(PROXY_SC_clone,SIGCHLD,0,0,0,0,0);
    CHECK(child>=0);
    if(!child) {
        vfs_raw_count_fork_child();
        CHECK(!resolve_cache && !resolve_cache_count && !resolve_cache_capacity);
        CHECK(!resolve_cache_buckets && !resolve_cache_bucket_count);
        proxy_sigmask_t child_mask=resolve_cache_acquire();
        resolve_cache_release(child_mask);
        RAW(PROXY_SC_exit_group,0,0,0,0,0,0);__builtin_unreachable();
    }
    resolve_cache_release(mask);
    int status=-1;CHECK(RAW(PROXY_SC_wait4,child,&status,0,0,0,0)==child && status==0);
    CHECK(resolve_cache && resolve_cache_count==1);
    struct {uintptr_t handler;unsigned long flags;uintptr_t restorer;proxy_sigmask_t mask;} action={
        (uintptr_t)reenter,0x04000000,(uintptr_t)proxy_restore,0};
    CHECK(!RAW(PROXY_SC_rt_sigaction,SIGUSR1,&action,0,8,0,0));
    mask=resolve_cache_acquire();
    CHECK(!RAW(PROXY_SC_tgkill,RAW(PROXY_SC_getpid,0,0,0,0,0,0),RAW(PROXY_SC_gettid,0,0,0,0,0,0),SIGUSR1,0,0,0));
    CHECK(!delivered);
    resolve_cache_release(mask);
    CHECK(delivered==1);
    const char msg[]="PASS fork discards locked cache snapshot; signal reentry waits for unlock\n";
    RAW(PROXY_SC_write,1,msg,sizeof(msg)-1,0,0,0);
    RAW(PROXY_SC_exit_group,0,0,0,0,0,0);__builtin_unreachable();
}
