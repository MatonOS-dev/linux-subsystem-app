#define _GNU_SOURCE
#include "adopt.h"
#include "../../compat-proxy/arch/x86_64/adopt.h"
#include "../spike2/thunks.h"
#include <stdlib.h>
#include <sys/syscall.h>
#include <signal.h>
extern long syscall(long,...);
static struct adopt_api *api;
void thunk_init(void) { api=(void *)(uintptr_t)strtoull(getenv("MATON_THREAD_ADOPT"),NULL,16); }
struct maton_call maton_enter(const char *name) {
    (void)name;
    struct adopt_thread *t=api->current(); if(!t) __builtin_trap();
    uintptr_t prior=adopt_fs_read();
    struct maton_call c={prior,(void (*)(void))api->identity};
    adopt_fs_write(t->resident_fs); return c;
}
void maton_leave(struct maton_call *c) {
    adopt_fs_write(c->previous);
}
long forwarded_identity(uintptr_t *identity,int signal_now) {
    struct maton_call c=maton_enter("identity");
    if(signal_now) adopt_raw(SYS_tgkill,adopt_raw(SYS_getpid,0,0,0),adopt_raw(SYS_gettid,0,0,0),SIGUSR1);
    long tid=((long (*)(uintptr_t *))c.target)(identity);
    maton_leave(&c); return tid;
}
void adopt_signal_gate(int sig) {
    struct adopt_thread *t=api->current();
    uintptr_t interrupted=adopt_fs_read();
    adopt_fs_write(t->app_fs);
    extern void app_signal(int);
    app_signal(sig);
    adopt_fs_write(interrupted);
}
