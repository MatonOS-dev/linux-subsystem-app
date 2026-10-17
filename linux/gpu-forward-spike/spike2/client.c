#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "switch.h"
#include "client.h"
static struct shadow_provider *provider;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_key_t key;
static atomic_flag bootstrap_lock = ATOMIC_FLAG_INIT;
static void lock(void) { while (atomic_flag_test_and_set_explicit(&bootstrap_lock, memory_order_acquire)) __asm__ volatile("pause"); }
static void unlock(void) { atomic_flag_clear_explicit(&bootstrap_lock, memory_order_release); }
static void destroy(void *ptr) {
    struct shadow_lease *lease = ptr;
    lock();
    uintptr_t saved = gs_read();
    gs_write(provider->bootstrap_fs);
    provider->destroy(lease);
    gs_write(saved);
    unlock();
    free(lease);
}
static void initialize(void) {
    gs_init(0);
    const char *value = getenv("MATON_GPU_SHADOW");
    if (!value) return;
    provider = (void *)(uintptr_t)strtoull(value, NULL, 16);
    if (provider->version != 1 || pthread_key_create(&key, destroy)) abort();
}
struct shadow_lease *shadow_current(void) {
    pthread_once(&once, initialize);
    if (!provider) return NULL;
    struct shadow_lease *lease = pthread_getspecific(key);
    if (lease) return lease;
    lease = calloc(1, sizeof(*lease));
    if (!lease) return NULL;
    lock();
    uintptr_t saved = gs_read();
    gs_write(provider->bootstrap_fs);
    int error = provider->create(lease);
    gs_write(saved);
    unlock();
    if (error) { free(lease); return NULL; }
    if (pthread_setspecific(key, lease)) { destroy(lease); return NULL; }
    return lease;
}
