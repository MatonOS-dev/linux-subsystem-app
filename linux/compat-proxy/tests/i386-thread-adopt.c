#define _GNU_SOURCE
#include "../arch/i386/adopt.h"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static _Atomic unsigned next_ldt = 16;
static __thread struct user_desc app_desc;
static __thread unsigned selector;
static __thread unsigned reserved_selector;
static __thread struct user_desc reserved_desc;
static __thread int reserved_claimed;
static __thread uintptr_t expected_app;
static __thread uintptr_t expected_android;
static __thread uintptr_t *app_block;
static __thread volatile sig_atomic_t signal_ok;

static int write_ldt(struct user_desc *d) {
    long r = syscall(SYS_modify_ldt, 1, d, sizeof(*d));
    return r < 0 ? -1 : 0;
}
static unsigned short read_gs(void) {
    unsigned short v;
    __asm__ volatile("movw %%gs,%0" : "=rm"(v) :: "memory");
    return v;
}
static void write_gs(unsigned short v) {
    __asm__ volatile("movw %0,%%gs" :: "rm"(v) : "memory");
}
static uintptr_t read_gs_word(void) {
    /* GCC's segment-qualified pointer emits a real %gs:0 TLS read. */
    return *(unsigned __seg_gs *)0;
}
static struct user_desc descriptor(uintptr_t base, unsigned entry) {
    return (struct user_desc){
        .entry_number = entry,
        .base_addr = (unsigned)base,
        .limit = 0xfffff,
        .seg_32bit = 1,
        .contents = 0,
        .read_exec_only = 0,
        .limit_in_pages = 1,
        .seg_not_present = 0,
        .useable = 1,
    };
}

/* Simulates the proxy's claim/release hooks. Every worker gets its own LDT
 * entry, reused as the proxy's multiplexed virtual slot while Android owns it. */
int proxy_tls_claim_reserved(const struct user_desc *owner, unsigned *out) {
    if (!owner || !out || !selector) return -1;
    struct user_desc d = *owner;
    d.entry_number = reserved_selector >> 3;
    if (write_ldt(&d)) return -1;
    *out = reserved_selector;
    reserved_claimed = 1;
    return 0;
}
int proxy_tls_release_reserved(void) {
    if (!reserved_claimed || write_ldt(&reserved_desc)) return -1;
    reserved_claimed = 0;
    return 0;
}

static void check_scope(void) {
    struct adopt_scope scope;
    unsigned short original = read_gs();
    assert(adopt_switch_in(&scope) == 0);
    assert(read_gs() == reserved_selector);
    assert(read_gs_word() == expected_android);
    assert(adopt_current() != NULL);
    assert(adopt_current()->tid == (int)syscall(SYS_gettid));
    struct adopt_scope nested;
    assert(adopt_switch_in(&nested) == 0);
    assert(nested.already_android);
    assert(read_gs_word() == expected_android);
    adopt_switch_out(&nested);
    assert(read_gs_word() == expected_android);
    adopt_switch_out(&scope);
    assert(!reserved_claimed);
    assert(read_gs() == original);
    assert(read_gs_word() == expected_app);
    assert(read_gs_word() == (unsigned)expected_app);
}

static void signal_handler(int signo) {
    (void)signo;
    struct adopt_scope scope;
    unsigned short original = read_gs();
    if (adopt_switch_in(&scope)) return;
    if (read_gs_word() == expected_android && read_gs() == reserved_selector) {
        adopt_switch_out(&scope);
        signal_ok = read_gs_word() == expected_app && read_gs() == original;
    } else {
        adopt_switch_out(&scope);
    }
}

static void setup_thread(uintptr_t android_tls) {
    unsigned entry = atomic_fetch_add_explicit(&next_ldt, 2, memory_order_relaxed);
    selector = (entry << 3) | 7u; /* LDT, RPL 3 */
    reserved_selector = ((entry + 1) << 3) | 7u;
    app_block = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(app_block != MAP_FAILED);
    expected_app = (uintptr_t)0xabc00000u ^ (uintptr_t)syscall(SYS_gettid);
    app_block[0] = expected_app;
    app_desc = descriptor((uintptr_t)app_block, entry);
    assert(write_ldt(&app_desc) == 0);
    reserved_desc = descriptor(0, entry + 1);
    reserved_desc.read_exec_only = 1;
    assert(write_ldt(&reserved_desc) == 0);
    expected_android = *(uintptr_t *)android_tls;
    write_gs((unsigned short)selector);
    assert(read_gs_word() == expected_app);
    assert(adopt_register_current(android_tls) == 0);
}

static void *worker(void *arg) {
    uintptr_t *android_tls = arg;
    setup_thread((uintptr_t)android_tls);
    for (unsigned i = 0; i < 100; ++i) check_scope();
    signal_ok = 0;
    assert(raise(SIGUSR1) == 0);
    assert(signal_ok);
    assert(read_gs_word() == expected_app);
    write_gs(0);
    return NULL;
}

int main(void) {
    struct sigaction action = { .sa_handler = signal_handler };
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    uintptr_t *tls = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(tls != MAP_FAILED);
    *tls = 0x13579bdf;
    pthread_t threads[6];
    for (unsigned i = 0; i < 6; ++i)
        assert(pthread_create(&threads[i], NULL, worker, tls) == 0);
    for (unsigned i = 0; i < 6; ++i) assert(pthread_join(threads[i], NULL) == 0);
    printf("PASS i386 adoption: six threads, GS TLS restore, reserved slot, signal handler\n");
    return 0;
}
