/* Build-selected architecture boundary. Internal IDs must be encoded before
 * kernel entry or BPF comparisons. Context slots expose payload argument,
 * result, PC and stack words; only the backend knows REG_* indices.
 * This initial interface does not yet normalize kernel wire structures. */
#ifndef PROXY_ARCH_H
#define PROXY_ARCH_H
#include <stdint.h>
#include "syscalls.h"
enum arch_slot { ARCH_RESULT, ARCH_PC, ARCH_STACK, ARCH_ARG0, ARCH_ARG1,
                 ARCH_ARG2, ARCH_ARG3, ARCH_ARG4, ARCH_ARG5 };
#ifndef PROXY_ARCH_BACKEND
#define PROXY_ARCH_BACKEND "x86_64/backend.h"
#endif
#include PROXY_ARCH_BACKEND
#endif
