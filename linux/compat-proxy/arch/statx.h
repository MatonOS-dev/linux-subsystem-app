/* statx is a fixed-width kernel wire ABI shared by the supported targets. */
#ifndef PROXY_ARCH_STATX_H
#define PROXY_ARCH_STATX_H
#include <linux/stat.h>
_Static_assert(sizeof(struct statx)==256,"statx wire size");
_Static_assert(__builtin_offsetof(struct statx,stx_nlink)==16,"statx nlink offset");
_Static_assert(__builtin_offsetof(struct statx,stx_ino)==32,"statx inode offset");
static inline uint32_t arch_statx_mode(const void *wire){return ((const struct statx *)wire)->stx_mode;}
static inline void arch_statx_nlink(void *wire,uint64_t count){((struct statx *)wire)->stx_nlink=count;}
#endif
