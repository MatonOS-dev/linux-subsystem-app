#pragma once
#include <sys/types.h>

int maton_shm_open(const char* name, int flags, mode_t mode);
int maton_shm_unlink(const char* name);

/* The NDK r30 sysroot ships no <sys/memfd.h> although libc has exported
 * memfd_create since API 30; meson's has_function probe passes by writing
 * its own prototype, and upstream callers then fail with an implicit
 * declaration. libc.so exports the symbol, so declare it here. */
int memfd_create(const char* name, unsigned int flags);
