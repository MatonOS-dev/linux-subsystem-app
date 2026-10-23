#define _GNU_SOURCE
#include "adopt.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <unistd.h>

extern int proxy_tls_claim_reserved(const struct user_desc *, unsigned *);
extern int proxy_tls_release_reserved(void);

struct adopt_record {
    struct adopt_record *next;
    struct adopt_thread thread;
};
static _Atomic(struct adopt_record *) records;
static struct adopt_api api;

static int current_gs(void) {
    unsigned short value;
    __asm__ volatile("movw %%gs,%0" : "=rm"(value) :: "memory");
    return value;
}
static void load_gs(unsigned short value) {
    __asm__ volatile("movw %0,%%gs" :: "rm"(value) : "memory");
}
static int current_tid(void) { return (int)syscall(SYS_gettid); }

struct adopt_thread *adopt_current(void) {
    int tid = current_tid();
    for (struct adopt_record *r = atomic_load_explicit(&records,
             memory_order_acquire); r; r = r->next)
        if (atomic_load_explicit(&r->thread.tid, memory_order_acquire) == tid)
            return &r->thread;
    return NULL;
}

/* Called by the i386 clone/adoption path for each new resident thread. This
 * backend does not impose a thread-count cap: nodes remain published so a
 * signal-time reader can never observe freed storage. */
int adopt_register_current(uintptr_t android_tls) {
    if (!android_tls || android_tls > UINT32_MAX) return -EINVAL;
    int tid = current_tid();
    for (struct adopt_record *r = atomic_load_explicit(&records,
             memory_order_acquire); r; r = r->next) {
        if (atomic_load_explicit(&r->thread.tid, memory_order_acquire) == tid) {
            r->thread.android_tls = android_tls;
            return 0;
        }
    }
    struct adopt_record *fresh = calloc(1, sizeof(*fresh));
    if (!fresh) return -ENOMEM;
    fresh->thread.android_tls = android_tls;
    atomic_store_explicit(&fresh->thread.tid, tid, memory_order_relaxed);
    struct adopt_record *head;
    do {
        head = atomic_load_explicit(&records, memory_order_acquire);
        fresh->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&records, &head, fresh,
                  memory_order_release, memory_order_acquire));
    return 0;
}

static long identity(uintptr_t *thread) {
    if (thread) *thread = (uintptr_t)pthread_self();
    return current_tid();
}
static size_t live(void) {
    size_t count = 0;
    for (struct adopt_record *r = atomic_load_explicit(&records,
             memory_order_acquire); r; r = r->next) {
        if (atomic_load_explicit(&r->thread.tid, memory_order_acquire) > 0) {
            if (count == SIZE_MAX) abort();
            count++;
        }
    }
    return count;
}
static struct adopt_api api = {
    .version = 2,
    .current = adopt_current,
    .lookup = NULL,
    .identity = identity,
    .live = live,
    .switch_in = adopt_switch_in,
    .switch_out = adopt_switch_out,
};
struct adopt_api *adopt_api_get(void) { return &api; }
uintptr_t adopt_api_address(void) { return (uintptr_t)&api; }

/* Startup runs with bionic's normal GS selector. get_thread_area exposes the
 * selector's kernel descriptor without depending on bionic's private TCB
 * layout; the resulting base is the Android TLS block forwarded to claim. */
int adopt_install(void) {
    unsigned short gs = (unsigned short)current_gs();
    if (!gs || (gs & 4u)) return -ENOTSUP; /* null or LDT selector */
    struct user_desc descriptor = { .entry_number = gs >> 3 };
    if (syscall(244, &descriptor) < 0) return -errno;
    return adopt_register_current(descriptor.base_addr);
}

int adopt_switch_in(struct adopt_scope *scope) {
    if (!scope) return -EINVAL;
    memset(scope, 0, sizeof(*scope));
    struct adopt_thread *thread = adopt_current();
    if (!thread) return -ENOENT;
    scope->thread = thread;
    scope->previous_gs = (unsigned short)current_gs();
    for (;;) {
        unsigned state = atomic_load_explicit(&thread->scope_state,
                                               memory_order_acquire);
        if (state == 2) {
            atomic_fetch_add_explicit(&thread->scope_depth, 1,
                                      memory_order_relaxed);
            scope->active = 1;
            scope->already_android = 1;
            return 0;
        }
        if (state == 1) return -EBUSY;
        unsigned expected = 0;
        if (atomic_compare_exchange_weak_explicit(&thread->scope_state,
                &expected, 1, memory_order_acq_rel, memory_order_acquire))
            break;
    }
    struct user_desc owner = {
        .entry_number = (unsigned)-1,
        .base_addr = (unsigned)thread->android_tls,
        .limit = 0xfffff,
        .seg_32bit = 1,
        .contents = 0,
        .read_exec_only = 0,
        .limit_in_pages = 1,
        .seg_not_present = 0,
        .useable = 1,
    };
    unsigned selector = 0;
    int result = proxy_tls_claim_reserved(&owner, &selector);
    if (result) {
        atomic_store_explicit(&thread->scope_state, 0, memory_order_release);
        return result < 0 ? result : -EIO;
    }
    if (selector > 0xffff || !(selector & 3u)) {
        (void)proxy_tls_release_reserved();
        atomic_store_explicit(&thread->scope_state, 0, memory_order_release);
        return -EINVAL;
    }
    thread->android_selector = selector;
    /* The reserved selector may equal the payload's virtual third slot.
     * Clear the segment register first so the processor reloads its descriptor
     * even when the selector value is unchanged. */
    load_gs(0);
    load_gs((unsigned short)selector);
    atomic_store_explicit(&thread->scope_depth, 1, memory_order_relaxed);
    atomic_store_explicit(&thread->scope_state, 2, memory_order_release);
    scope->active = 1;
    return 0;
}

void adopt_switch_out(struct adopt_scope *scope) {
    if (!scope || !scope->active) return;
    if (scope->already_android) {
        atomic_fetch_sub_explicit(&scope->thread->scope_depth, 1,
                                  memory_order_relaxed);
    } else {
        atomic_store_explicit(&scope->thread->scope_state, 1,
                              memory_order_release);
        atomic_store_explicit(&scope->thread->scope_depth, 0,
                              memory_order_relaxed);
        if (proxy_tls_release_reserved()) abort();
        /* set_thread_area updates the descriptor table, not the CPU's hidden
         * GS cache. Reload the saved selector after release to make the
         * restored application descriptor active in that cache. */
        load_gs(0);
        load_gs(scope->previous_gs);
        atomic_store_explicit(&scope->thread->scope_state, 0,
                              memory_order_release);
    }
    scope->active = 0;
}
