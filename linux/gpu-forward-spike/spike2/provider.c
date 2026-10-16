#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include "provider.h"
#include "switch.h"
/* Built against the resident libc (bionic on device). pthread_create performs
 * all static/dynamic TLS and pthread_internal_t initialization; no guessed ABI.
 * This leases a LIVE parked pthread's TLS, not full thread adoption. Its cached
 * tid, altstack, robust list, stack bounds and kernel clear_tid remain donor-
 * owned. Do not expose this experiment as a production Vulkan ICD. */
struct donor { pthread_t thread; _Atomic int ready, stop; uintptr_t fs; };
static void *park(void *arg) {
    struct donor *d = arg;
    if (gs_raw(158, 0x1003, (long)&d->fs, 0)) __builtin_trap();
    atomic_store_explicit(&d->ready, 1, memory_order_release);
    gs_raw(202, (long)&d->ready, 129, 1);
    while (!atomic_load_explicit(&d->stop, memory_order_acquire))
        gs_raw(202, (long)&d->stop, 128, 0);
    return NULL;
}
static int create(struct shadow_lease *lease) {
    struct donor *d = calloc(1, sizeof(*d));
    if (!d) return -1;
    if (pthread_create(&d->thread, NULL, park, d)) { free(d); return -1; }
    while (!atomic_load_explicit(&d->ready, memory_order_acquire))
        gs_raw(202, (long)&d->ready, 128, 0);
    lease->fs = d->fs;
    lease->private = d;
    return 0;
}
static void destroy(struct shadow_lease *lease) {
    struct donor *d = lease->private;
    atomic_store_explicit(&d->stop, 1, memory_order_release);
    gs_raw(202, (long)&d->stop, 129, 1);
    if (pthread_join(d->thread, NULL)) __builtin_trap();
    free(d);
    *lease = (struct shadow_lease){0};
}
void shadow_publish(void) {
    static struct shadow_provider provider = { .version=1, .create=create, .destroy=destroy };
    if (gs_raw(158, 0x1003, (long)&provider.bootstrap_fs, 0)) __builtin_trap();
    char value[32];
    snprintf(value, sizeof(value), "%lx", (unsigned long)&provider);
    setenv("MATON_GPU_SHADOW", value, 1);
}
