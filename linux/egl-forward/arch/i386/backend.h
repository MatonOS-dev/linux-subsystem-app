#ifndef MATON_EGL_I386_BACKEND_H
#define MATON_EGL_I386_BACKEND_H

/* Keep EGL's arch selection local while sharing the audited v2 adoption ABI
 * and int 0x80 syscall layer with the i386 Vulkan forwarder. */
#include "../../../vulkan-forward/arch/i386/backend.h"

#endif
