#ifndef MATON_SHADOW_CLIENT_H
#define MATON_SHADOW_CLIENT_H
#include "provider.h"
/* Call only on glibc FS. The destructor joins and frees the parked donor. */
struct shadow_lease *shadow_current(void);
#endif
