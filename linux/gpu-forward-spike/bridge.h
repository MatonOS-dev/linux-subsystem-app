#ifndef GPU_SPIKE_BRIDGE_H
#define GPU_SPIKE_BRIDGE_H
#include <stdint.h>
/* Process-local capability. Valid only before resident exec, on owner TID.
 * Explicit TLS switching fallback, never a zero-cost forwarding solution. */
struct gpu_bridge {
    uint64_t version;
    long owner;
    uintptr_t tls;
    void *(*open)(const char *, int);
    void *(*symbol)(void *, const char *);
};
#endif
