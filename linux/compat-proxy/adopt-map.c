/* Arch-independent FS-base -> adoption-record hash. See adopt-map.h. */
#include "adopt-map.h"
#include <stdatomic.h>

#define ADOPT_MAP_EMPTY ((uintptr_t)0)
#define ADOPT_MAP_TOMB  ((uintptr_t)-1)
/* Smallest table: two cache lines of slots, enough for the first few threads.
 * Capacity only ever grows, and only when the live set outgrows it, so there
 * is no fixed thread cap. */
enum { ADOPT_MAP_MIN_CAPACITY = 64 };

struct adopt_map_slot {
    _Atomic uintptr_t key;
    _Atomic(struct adopt_thread *) value;
};
struct adopt_map {
    size_t capacity;               /* power of two, >= ADOPT_MAP_MIN_CAPACITY */
    _Atomic size_t used;           /* non-empty slots, tombstones included */
    _Atomic size_t live;           /* published bindings */
    struct adopt_map_slot slots[];
};

static _Atomic(struct adopt_map *) adopt_map_current;
static _Atomic(struct adopt_map *) adopt_map_grower;
static adopt_map_alloc_fn adopt_map_alloc;

/* Avalanche so clustered FS bases (page-aligned mmaps, grant tables) spread
 * across the table instead of piling into one probe run. */
static size_t adopt_map_mix(uintptr_t key) {
    uint64_t x = (uint64_t)key;
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return (size_t)x;
}
static size_t adopt_map_slot(uintptr_t key, size_t capacity) {
    return adopt_map_mix(key) & (capacity - 1);
}

/* Result codes for a table insert: 0 full, 1 claimed an empty slot, 2 updated
 * an existing key, 3 claimed a tombstone. */
static int adopt_map_put(struct adopt_map *m, uintptr_t key,
                         struct adopt_thread *thread) {
    size_t mask = m->capacity - 1;
    size_t i = adopt_map_slot(key, m->capacity);
    size_t probes = 0;
    while (probes <= m->capacity) {
        uintptr_t found = atomic_load_explicit(&m->slots[i].key, memory_order_acquire);
        if (found == key) {
            atomic_store_explicit(&m->slots[i].value, thread, memory_order_release);
            return 2;
        }
        if (found == ADOPT_MAP_EMPTY || found == ADOPT_MAP_TOMB) {
            uintptr_t expect = found;
            if (atomic_compare_exchange_strong_explicit(&m->slots[i].key, &expect, key,
                    memory_order_acq_rel, memory_order_acquire) || expect == key) {
                atomic_store_explicit(&m->slots[i].value, thread, memory_order_release);
                return found == ADOPT_MAP_EMPTY ? 1 : 3;
            }
            /* Another writer claimed this slot with a different key; re-read
             * it rather than skipping ahead (retried, probe-bounded). */
            continue;
        }
        i = (i + 1) & mask;
        probes++;
    }
    return 0;
}

static struct adopt_thread *adopt_map_get(struct adopt_map *m, uintptr_t key) {
    size_t mask = m->capacity - 1;
    size_t i = adopt_map_slot(key, m->capacity);
    for (size_t probes = 0; probes < m->capacity; probes++) {
        uintptr_t found = atomic_load_explicit(&m->slots[i].key, memory_order_acquire);
        if (found == ADOPT_MAP_EMPTY) return 0; /* end of the probe chain */
        if (found == key)
            return atomic_load_explicit(&m->slots[i].value, memory_order_acquire);
        i = (i + 1) & mask;
    }
    return 0;
}

static void adopt_map_remove(struct adopt_map *m, uintptr_t key,
                             struct adopt_thread *thread) {
    size_t mask = m->capacity - 1;
    size_t i = adopt_map_slot(key, m->capacity);
    for (size_t probes = 0; probes < m->capacity; probes++) {
        uintptr_t found = atomic_load_explicit(&m->slots[i].key, memory_order_acquire);
        if (found == ADOPT_MAP_EMPTY) return;
        if (found == key) {
            struct adopt_thread *value =
                atomic_load_explicit(&m->slots[i].value, memory_order_acquire);
            if (thread && value != thread) return; /* key recycled */
            atomic_store_explicit(&m->slots[i].value, 0, memory_order_release);
            atomic_store_explicit(&m->slots[i].key, ADOPT_MAP_TOMB, memory_order_release);
            atomic_fetch_sub_explicit(&m->live, 1, memory_order_relaxed);
            return;
        }
        i = (i + 1) & mask;
    }
}

static struct adopt_map *adopt_map_allocate(size_t capacity) {
    if (!adopt_map_alloc) return 0;
    if (capacity > SIZE_MAX / sizeof(struct adopt_map_slot)) return 0;
    size_t bytes = sizeof(struct adopt_map) + capacity * sizeof(struct adopt_map_slot);
    if (bytes < sizeof(struct adopt_map)) return 0; /* overflow */
    struct adopt_map *m = adopt_map_alloc(bytes);
    if (m) m->capacity = capacity;
    return m;
}

/* Own growth via a CAS-claimed flag; rehash, then publish with release. Other
 * writers that lose the claim retry against the published table. */
static void adopt_map_grow(struct adopt_map *old) {
    struct adopt_map *expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&adopt_map_grower, &expected, old,
            memory_order_acq_rel, memory_order_acquire))
        return;
    if (old->capacity <= SIZE_MAX / 2) {
        struct adopt_map *fresh = adopt_map_allocate(old->capacity * 2);
        if (fresh) {
            for (size_t i = 0; i < old->capacity; i++) {
                uintptr_t key = atomic_load_explicit(&old->slots[i].key, memory_order_acquire);
                if (key == ADOPT_MAP_EMPTY || key == ADOPT_MAP_TOMB) continue;
                struct adopt_thread *value =
                    atomic_load_explicit(&old->slots[i].value, memory_order_acquire);
                int r = adopt_map_put(fresh, key, value);
                if (r == 1 || r == 3)
                    atomic_fetch_add_explicit(&fresh->used, 1, memory_order_relaxed);
                if (r) atomic_fetch_add_explicit(&fresh->live, 1, memory_order_relaxed);
            }
            atomic_store_explicit(&adopt_map_current, fresh, memory_order_release);
        }
    }
    atomic_store_explicit(&adopt_map_grower, 0, memory_order_release);
}

void adopt_map_configure(adopt_map_alloc_fn alloc) { adopt_map_alloc = alloc; }

void adopt_map_reset(void) {
    struct adopt_map *fresh = adopt_map_allocate(ADOPT_MAP_MIN_CAPACITY);
    if (!fresh) return;
    atomic_store_explicit(&adopt_map_current, fresh, memory_order_release);
    atomic_store_explicit(&adopt_map_grower, 0, memory_order_release);
}

struct adopt_thread *adopt_map_lookup(uintptr_t key) {
    if (key == ADOPT_MAP_EMPTY) return 0;
    for (unsigned retry = 0; retry < 64; retry++) {
        struct adopt_map *m = atomic_load_explicit(&adopt_map_current, memory_order_acquire);
        if (!m) return 0;
        struct adopt_thread *thread = adopt_map_get(m, key);
        if (thread) return thread;
        if (atomic_load_explicit(&adopt_map_current, memory_order_acquire) == m) return 0;
    }
    return 0;
}

void adopt_map_bind(uintptr_t key, struct adopt_thread *thread) {
    if (key == ADOPT_MAP_EMPTY || !thread) return;
    /* Bounded retries keep a signal handler from spinning if a concurrent
     * grower holds the table; the tid-scan fallback covers a dropped insert. */
    for (unsigned retry = 0; retry < 64; retry++) {
        struct adopt_map *m = atomic_load_explicit(&adopt_map_current, memory_order_acquire);
        if (!m) { adopt_map_reset(); continue; }
        if (atomic_load_explicit(&m->live, memory_order_relaxed) * 2 >= m->capacity ||
            atomic_load_explicit(&m->used, memory_order_relaxed) * 4 >= m->capacity * 3) {
            adopt_map_grow(m);
            continue;
        }
        int r = adopt_map_put(m, key, thread);
        if (r == 0) { adopt_map_grow(m); continue; }
        if (r == 1) atomic_fetch_add_explicit(&m->used, 1, memory_order_relaxed);
        if (r == 1 || r == 3) atomic_fetch_add_explicit(&m->live, 1, memory_order_relaxed);
        if (atomic_load_explicit(&adopt_map_current, memory_order_acquire) == m) return;
        /* The table was replaced while we bound; make sure the key lands in
         * the newly published table too. */
    }
}

void adopt_map_unbind(uintptr_t key, struct adopt_thread *thread) {
    if (key == ADOPT_MAP_EMPTY) return;
    for (unsigned retry = 0; retry < 64; retry++) {
        struct adopt_map *m = atomic_load_explicit(&adopt_map_current, memory_order_acquire);
        if (!m) return;
        adopt_map_remove(m, key, thread);
        if (atomic_load_explicit(&adopt_map_current, memory_order_acquire) == m) return;
    }
}

size_t adopt_map_size(void) {
    struct adopt_map *m = atomic_load_explicit(&adopt_map_current, memory_order_acquire);
    return m ? atomic_load_explicit(&m->live, memory_order_relaxed) : 0;
}
