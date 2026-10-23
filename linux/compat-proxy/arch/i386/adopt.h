#ifndef MATON_I386_ADOPT_H
#define MATON_I386_ADOPT_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <asm/ldt.h>

/* Keep the v2 API prefix in lockstep with the x86_64 provider. i386 cannot
 * key by a TLS base: GS is a selector, often the same virtual selector on
 * every proxied thread. current() therefore uses gettid and a dynamic,
 * lock-free registry. lookup is intentionally NULL on i386. */
struct adopt_thread {
    _Atomic int tid;
    uintptr_t android_tls;
    unsigned android_selector;
    _Atomic unsigned scope_state; /* 0=app, 1=transition, 2=Android TLS */
    _Atomic unsigned scope_depth;
};
struct adopt_api {
    unsigned version;
    struct adopt_thread *(*current)(void);
    struct adopt_thread *(*lookup)(uintptr_t);
    long (*identity)(uintptr_t *);
    _Atomic unsigned clones, clone3s;
    size_t (*live)(void);
    _Atomic unsigned fork_prepares, fork_parents, fork_children, vforks;
};
struct adopt_scope {
    struct adopt_thread *thread;
    unsigned short previous_gs;
    unsigned active;
    unsigned already_android;
};

/* The thread creator must register its bionic TLS block before entering the
 * Linux payload. The proxy claim hook installs that block in its reserved GDT
 * entry each time the thread enters an Android call. */
int adopt_register_current(uintptr_t android_tls);
int adopt_install(void);
uintptr_t adopt_api_address(void);
struct adopt_thread *adopt_current(void);
struct adopt_api *adopt_api_get(void);
int adopt_switch_in(struct adopt_scope *scope);
void adopt_switch_out(struct adopt_scope *scope);

#endif
