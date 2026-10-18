#define _GNU_SOURCE
#include <stddef.h>
#include "../../vulkan-forward/arch/x86_64/backend.h"
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>

static struct adopt_thread thread;
static struct adopt_thread *current(void) { return &thread; }
static struct adopt_thread *lookup(uintptr_t fs) { return fs == thread.resident_fs ? &thread : 0; }
static long ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000000000L + t.tv_nsec;
}
int main(void) {
    struct adopt_api api = {.version = 2, .current = current, .lookup = lookup};
    unsigned long fs = 0;
    if (syscall(SYS_arch_prctl, 0x1003, &fs)) return 1;
    thread.resident_fs = fs;
    int fast = mv_fast_tls();
    struct mv_scope scope;
    const int count = 1000000;
    long start = ns();
    for (int i = 0; i < count; ++i) {
        if (!switch_to_android(&api, fast, &scope)) return 1;
        switch_back(&scope);
    }
    long elapsed = ns() - start;
    printf("TLS switch pair: %.1f ns (%s, %d pairs)\n", (double)elapsed / count,
           fast ? "FSGSBASE" : "arch_prctl", count);
    return 0;
}
