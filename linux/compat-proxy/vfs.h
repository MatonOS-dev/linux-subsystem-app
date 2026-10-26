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
int vfs_plain_nofollow_path(int,const char *,char **);
int vfs_plain_nofollow_dir_path(int,const char *,int);
/* Follow symlinks, but retain a final proc fd magic link for linkat. */
#define VFS_FOLLOW_LINK_SOURCE 2
char *vfs_resolve_mount(int,const char *,int,long *,void **);
char *vfs_resolve_mount_slow(int,const char *,int,long *,void **);
long vfs_mount_readonly(void *);
long vfs_mount_readonly_map(const char *,const char *);
int vfs_is_mount_source(const char *);
int vfs_has_nested_mount(const char *,const char *,int);
void vfs_fd_close(int);
long vfs_fd_dup(int,int);
char *vfs_fd_name(int);
char *vfs_fd_name_identity(int,uint64_t,uint64_t);
char *vfs_fd_name_cached(int);
void vfs_fd_record(int,int,const char *,int);
void vfs_fd_rename(const char *,const char *);
char *vfs_proc_fd_name(const char *);
void vfs_executable(const char *);
char *vfs_exe(void);
int vfs_bwrap_shm_mkdir(const char *);
int vfs_bwrap_tmpdir_mkdir(const char *);
long vfs_bwrap_erofs_mkdir(const char *);
long vfs_bwrap_erofs_file(const char *,unsigned);
char *vfs_reverse(const char *);
long vfs_mount(const char *,const char *,const char *,unsigned long);
long vfs_shm_init(void);
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
void vfs_private_fd_relocated(int,int);
void vfs_gpu_init(void);
void vfs_network_payload(int);
long vfs_gpu_publish(const char *,const char *,const char *,const char *,const char *,unsigned,const char *,unsigned);
long erofs_mount(const char *,const char *);
long erofs_backing(const char *,const char *);
char *erofs_physical(const char *);
char *erofs_physical_rooted(const char *,const char *);
size_t erofs_prefix_length(const char *,const char *);
char *erofs_reverse(const char *);
long proxy_pread_fd(int,void *,size_t,int64_t);
void *vfs_direct_begin(long,uintptr_t);
void vfs_direct_end(void *);
void vfs_direct_thread_exit(long);
void *vfs_direct_pause(void);
void vfs_direct_resume(void *);
#endif

long proxy_file_mmap(proxy_arg_t *);
