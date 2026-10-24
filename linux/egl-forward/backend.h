#if defined(__x86_64__)
#include "../vulkan-forward/arch/x86_64/backend.h"
#elif defined(__i386__)
#include "arch/i386/backend.h"
#elif defined(__aarch64__)
#include "../vulkan-forward/arch/aarch64/backend.h"
#else
#error Unsupported EGL forwarder architecture
#endif
