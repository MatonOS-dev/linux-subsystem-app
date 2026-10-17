#ifndef MATON_ADOPT_MAP_H
#define MATON_ADOPT_MAP_H
#include <stddef.h>
#include <stdint.h>
/* Arch-independent FS-base -> adoption-record table.
 *
 * The adoption backend keys each live thread on its current FS base (the value
 * the kernel loads into the segment register); CPU-specific reads (rdfsbase or
 * arch_prctl(ARCH_GET_FS)) stay in arch/<arch>/ and are passed in as a plain
 * uintptr_t. This file has no architecture conditionals and no libc
 * dependency, so every architecture can share one implementation.
 *
 * Layout is a single open-addressed table with linear probing. Key 0 is the
 * empty sentinel: FS base 0 is the pre-TLS startup window and is deliberately
 * never inserted, so callers keep their tid-scan fallback until the thread has
 * a real TLS block. Key -1 is a tombstone so probe chains survive removal.
 *
 * Publication: the live table pointer is swapped with release semantics;
 * lookups never take a lock, never allocate and never block. Growth rehashes
 * into a freshly allocated, zero-initialised table and swaps the published
 * pointer, so a reader that raced the swap either sees the old or the new
 * table, both internally consistent. Tables are never freed: a lock-free
 * reader may still hold a pointer (same free-in-place policy as the adoption
 * chunk registry). */
struct adopt_thread;
/* Must return zero-initialised memory of the requested size, or NULL. */
typedef void *(*adopt_map_alloc_fn)(size_t);
void adopt_map_configure(adopt_map_alloc_fn alloc);
/* Publish a fresh empty table (startup, and fork-child rebase). */
void adopt_map_reset(void);
struct adopt_thread *adopt_map_lookup(uintptr_t fs);
void adopt_map_bind(uintptr_t fs, struct adopt_thread *thread);
/* Remove only if the stored record still matches `thread` (an FS base can be
 * recycled by glibc for a later thread). */
void adopt_map_unbind(uintptr_t fs, struct adopt_thread *thread);
size_t adopt_map_size(void);
#endif
