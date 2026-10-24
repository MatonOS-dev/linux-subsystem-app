#ifndef PROXY_I386_SYSINFO_H
#define PROXY_I386_SYSINFO_H
#include <stdint.h>
void proxy_i386_sysinfo_configure(char *const *env);
uintptr_t proxy_i386_sysinfo_value(uintptr_t inherited);
long proxy_i386_sysinfo_dispatch(long nr, int64_t *args);
#endif
