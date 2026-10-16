#ifndef PROXY_X86_64_ADOPT_H
#define PROXY_X86_64_ADOPT_H
#include <stdint.h>
/* RESILIENCE RULE: application adoption implements only the Linux kernel ABI.
 * Decode clone_args, ucontext/sigcontext/FP state, robust_list_head and rseq;
 * never inspect a libc TCB, struct pthread, wrapper instructions, TLS offsets,
 * stack argument layout, allocator state or libc version. Treat CLONE_SETTLS
 * as an opaque architecture TLS pointer. Private records below are our own.
 * Resident pthread/fork operations use public opaque libc APIs only. Unknown
 * flags/fields/structure extensions must fail explicitly, with a diagnostic. */
/* FS-independent backend; no PLT, errno, compiler TLS, or FSGSBASE requirement. */
static inline long adopt_raw(long n,long a,long b,long c) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");
    return r;
}
static inline uintptr_t adopt_fs_read(void) {
    uintptr_t p;
    if(adopt_raw(158,0x1003,(long)&p,0)) __builtin_trap();
    return p;
}
static inline void adopt_fs_write(uintptr_t p) {
    if(adopt_raw(158,0x1002,p,0)) __builtin_trap();
}
#endif
