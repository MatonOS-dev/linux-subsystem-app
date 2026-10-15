#pragma once
#include <stddef.h>

/* Loads a vendor library (Mesa) through Android's same-process-HAL
 * namespace, as Android's own libEGL loads GPU drivers: Xwayland lives in
 * /system_ext and may not link /vendor libraries directly. */
__attribute__((visibility("hidden"))) void* maton_sphal_load(const char* library);
/* Fills table[i] with library's names[i]; a missing symbol points at a stub
 * that aborts with its name, so a gap is reported instead of jumping to 0. */
__attribute__((visibility("hidden"))) void maton_sphal_resolve(const char* library, const char* const* names, void** table, size_t count);
