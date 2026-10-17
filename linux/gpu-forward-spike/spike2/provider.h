#ifndef MATON_SHADOW_PROVIDER_H
#define MATON_SHADOW_PROVIDER_H
#include <stdint.h>
struct shadow_lease { uintptr_t fs; void *private; };
struct shadow_provider {
    unsigned version;
    uintptr_t bootstrap_fs;
    /* Called with bootstrap FS under the client's serialization lock. */
    int (*create)(struct shadow_lease *);
    void (*destroy)(struct shadow_lease *);
};
void shadow_publish(void);
#endif
