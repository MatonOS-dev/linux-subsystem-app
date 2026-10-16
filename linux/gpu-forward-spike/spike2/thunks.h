#ifndef MATON_THUNKS_H
#define MATON_THUNKS_H
#include <stdint.h>
struct maton_call { uintptr_t previous; void (*target)(void); };
/* Resolver/handle adapter is deliberately a separate contract. It must lazily
 * acquire per-thread state on glibc FS, resolve on Android FS, then return with
 * Android FS active. leave restores precisely the entry FS (supports nesting). */
__attribute__((visibility("hidden"))) struct maton_call maton_enter(const char *name);
__attribute__((visibility("hidden"))) void maton_leave(struct maton_call *call);
#endif
