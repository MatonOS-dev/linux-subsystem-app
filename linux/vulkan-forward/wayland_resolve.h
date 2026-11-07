#ifndef MATON_VK_WAYLAND_RESOLVE_H
#define MATON_VK_WAYLAND_RESOLVE_H

/* Resolve an exported symbol from the application's loaded Wayland client.
 * This code is freestanding: it does not call dlopen/dlsym or libc. */
void *mv_wayland_symbol(const char *name);

void *mv_client_library(const char *);
void *mv_client_symbol(void *,const char *);

#endif
