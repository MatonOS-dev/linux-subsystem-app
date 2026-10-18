/* statx is a fixed-width kernel wire ABI shared by the supported targets. */
#ifndef PROXY_ARCH_STATX_H
#define PROXY_ARCH_STATX_H
#include <linux/stat.h>
_Static_assert(sizeof(struct statx)==256,"statx wire size");
_Static_assert(__builtin_offsetof(struct statx,stx_nlink)==16,"statx nlink offset");
_Static_assert(__builtin_offsetof(struct statx,stx_ino)==32,"statx inode offset");
static inline uint32_t arch_statx_mode(const void *wire){return ((const struct statx *)wire)->stx_mode;}
static inline void arch_statx_nlink(void *wire,uint64_t count){((struct statx *)wire)->stx_nlink=count;}
static inline void arch_statx_device(void *wire,uint64_t dev){
 struct statx *s=wire;s->stx_dev_major=(uint32_t)(((dev>>8)&0xfff)|((dev>>32)&~0xfffULL));s->stx_dev_minor=(uint32_t)((dev&0xff)|((dev>>12)&~0xffULL));
}
static inline void arch_statx_synthesize(void *wire,uint32_t mode,uint32_t uid,uint32_t gid,uint64_t size,uint64_t ino){
 struct statx *s=wire;*s=(struct statx){0};s->stx_mask=STATX_TYPE|STATX_MODE|STATX_UID|STATX_GID|STATX_SIZE|STATX_INO|STATX_NLINK|STATX_BLOCKS;s->stx_mode=mode;s->stx_uid=uid;s->stx_gid=gid;s->stx_size=size;s->stx_ino=ino;s->stx_nlink=1;s->stx_blksize=4096;
}
#endif
