#ifndef VFS_H
#define VFS_H
#include "proxy.h"
void *vfs_alloc(size_t);
void *vfs_palloc(size_t);
void vfs_free(void *);
void vfs_trap_begin(void *,size_t);
void vfs_trap_resume(void);
void vfs_trap_end(void);
void vfs_trap_thread_exit(void *,size_t);
void vfs_raw_count_summary(uint64_t *,uint64_t *);
void vfs_raw_category_summary(uint64_t[6]);
void vfs_raw_syscall_summary(uint64_t[PROXY_SC_count]);
void vfs_raw_count_fork_child(void);
void vfs_profile_dump(void);
void vfs_resolve_cache_invalidate(void);
char *vfs_resolve(int, const char *, int, long *);
int vfs_openat2_path_fd(int,const char *,int,int *,char **);
/* Follow symlinks, but retain a final proc fd magic link for linkat. */
#define VFS_FOLLOW_LINK_SOURCE 2
char *vfs_resolve_mount(int,const char *,int,long *,void **);
char *vfs_resolve_mount_slow(int,const char *,int,long *,void **);
long vfs_mount_readonly(void *);
int vfs_is_mount_source(const char *);
int vfs_has_nested_mount(const char *,const char *,int);
void vfs_fd_close(int);
long vfs_fd_dup(int,int);
char *vfs_fd_name(int);
void vfs_fd_record(int,int,const char *);
void vfs_fd_rename(const char *,const char *);
char *vfs_proc_fd_name(const char *);
void vfs_executable(const char *);
char *vfs_exe(void);
char *vfs_reverse(const char *);
char *vfs_reverse_pack(const char *);
long vfs_mount(const char *,const char *,const char *,unsigned long);
long vfs_unmount(const char *);
long vfs_root(const char *);
long vfs_pivot(const char *,const char *);
long vfs_proc_open(const char *,int);
long vfs_bootstrap_root(const char *);
long vfs_chdir(int,const char *);
char *vfs_getcwd(long *);
void vfs_temp_gc(void);
char *vfs_temp_create(long *);
int vfs_private_fd(int);
void vfs_gpu_init(void);
void vfs_network_payload(int);
long vfs_gpu_publish(const char *,const char *,const char *,const char *,const char *,unsigned,const char *,unsigned);
long pack_backing(const char *,const char *);
long pack_mount(const char *,const char *,const char *);
long pack_open(const char *,int);
long pack_pread(int,void *,size_t,int64_t);
void pack_close(int);
long pack_dup(int,int);
int pack_tracked(int);
int pack_private(int);
int pack_is_directory(int);
long pack_path(long,proxy_arg_t *,const char *,int *);
long pack_call(long,proxy_arg_t *,int *);
char *pack_link(const char *,long *,int *);
int pack_stat(const char *,struct proxy_stat *);
int pack_mount_root(const char *);
void *vfs_direct_begin(long,uintptr_t);
void vfs_direct_end(void *);
void vfs_direct_thread_exit(long);
void *vfs_direct_pause(void);
void vfs_direct_resume(void *);
#endif

long proxy_file_mmap(proxy_arg_t *);
uint64_t pack_slice_start(int);
