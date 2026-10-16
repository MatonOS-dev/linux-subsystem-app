#ifndef MATON_ADOPT_H
#define MATON_ADOPT_H
#include <stdint.h>
#if !defined(__x86_64__)
/* arm64 slot: TPIDR_EL0, arm64 sigcontext and clone argument order.
 * riscv64 slot: tp, riscv sigcontext and clone argument order.
 * Each needs its own resume/return assembly and seccomp audit ABI. */
#error "thread adoption backend is currently x86_64 only"
#endif
#include <stdatomic.h>
struct adopt_thread {
    _Atomic int tid;
    uintptr_t resident_fs;
    _Atomic uintptr_t app_fs;
    uintptr_t resident_sp;
    uintptr_t child_tid;
    unsigned long robust;
    _Atomic int ready;
    long regs[23];
    unsigned long mask;
    _Alignas(64) unsigned char fp[16384];
    uint64_t xfeatures;
};
struct adopt_api {
    unsigned version;
    struct adopt_thread *(*current)(void);
    long (*identity)(uintptr_t *);
    _Atomic unsigned clones,clone3s;
};
int adopt_install(void);
_Noreturn void adopt_launch(uintptr_t entry,uintptr_t stack);
struct adopt_thread *adopt_current(void);
#endif
