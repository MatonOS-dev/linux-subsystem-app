#ifndef MATON_SWITCH_H
#define MATON_SWITCH_H
#include <stdint.h>
/* These routines have no libc or compiler TLS dependencies. Build without stack
 * protector, sanitizers, profiling, or instrumentation. x86_64 only. */
static inline long gs_raw(long n, long a, long b, long c) {
    long r;
    /* FUTEX_WAIT consumes arg4 (timeout), unlike the other calls here. */
    register long timeout __asm__("r10") = 0;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(timeout)
                     : "rcx", "r11", "memory");
    return r;
}
extern int gs_fsgsbase;
static inline uintptr_t gs_read(void) {
    uintptr_t p;
    if (gs_fsgsbase) __asm__ volatile("rdfsbase %0" : "=r"(p) :: "memory");
    else if (gs_raw(158, 0x1003, (long)&p, 0)) __builtin_trap();
    return p;
}
static inline void gs_write(uintptr_t p) {
    if (gs_fsgsbase) __asm__ volatile("wrfsbase %0" :: "r"(p) : "memory");
    else if (gs_raw(158, 0x1002, p, 0)) __builtin_trap();
}
/* Initialize on glibc FS, before entering any foreign code. forced fallback is
 * for tests; production uses only the kernel's AT_HWCAP2, never CPUID alone. */
void gs_init(int force_fallback);
#endif
