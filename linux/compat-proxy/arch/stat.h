/* Canonical metadata used by VFS/inode/IPC algorithms. Native stat buffers
 * never cross this boundary; selected backend headers verify their ABI. */
#ifndef PROXY_ARCH_STAT_H
#define PROXY_ARCH_STAT_H
#include <sys/stat.h>
struct proxy_stat {
 uint64_t st_dev,st_ino,st_nlink,st_rdev;
 int64_t st_size;
 uint32_t st_mode,st_uid;
};
static inline void arch_stat_decode(struct proxy_stat *to,const void *buffer){
 const struct stat *from=buffer;
 to->st_dev=from->st_dev;to->st_ino=from->st_ino;
 to->st_nlink=from->st_nlink;to->st_rdev=from->st_rdev;
 to->st_size=from->st_size;to->st_mode=from->st_mode;to->st_uid=from->st_uid;
}
static inline void arch_stat_nlink(void *buffer,uint64_t count){
 ((struct stat *)buffer)->st_nlink=count;
}
static inline long arch_stat_at(int fd,const char *path,struct proxy_stat *out,int flags){
 *out=(struct proxy_stat){0};
 struct stat wire;long r=RAW(PROXY_SC_newfstatat,fd,path,&wire,flags,0,0);
 if(!r)arch_stat_decode(out,&wire);
 return r;
}
static inline long arch_stat_fd(int fd,struct proxy_stat *out){
 *out=(struct proxy_stat){0};
 struct stat wire;long r=RAW(PROXY_SC_fstat,fd,&wire,0,0,0,0);
 if(!r)arch_stat_decode(out,&wire);
 return r;
}
static inline void arch_stat_synthesize(void *wire,uint32_t mode,uint32_t uid,uint32_t gid,uint64_t size,uint64_t ino){
 struct stat *s=wire;*s=(struct stat){0};s->st_mode=mode;s->st_uid=uid;s->st_gid=gid;s->st_size=size;s->st_ino=ino;s->st_nlink=1;s->st_blksize=4096;
}
static inline int64_t arch_sendfile_offset(const void *wire){return *(const long *)wire;}
static inline void arch_sendfile_result(void *wire,int64_t value){*(long *)wire=value;}
#endif
