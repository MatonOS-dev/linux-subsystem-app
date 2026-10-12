#ifndef VFS_H
#define VFS_H
void *vfs_alloc(size_t);
void vfs_free(void *);
char *vfs_resolve(int, const char *, int, long *);
/* Follow symlinks, but retain a final proc fd magic link for linkat. */
#define VFS_FOLLOW_LINK_SOURCE 2
char *vfs_resolve_mount(int,const char *,int,long *,void **);
long vfs_mount_readonly(void *);
void vfs_fd_close(int);
void vfs_fd_dup(int,int);
char *vfs_fd_name(int);
void vfs_fd_record(int,int,const char *);
char *vfs_proc_fd_name(const char *);
void vfs_executable(const char *);
char *vfs_exe(void);
char *vfs_reverse(const char *);
long vfs_mount(const char *,const char *,const char *,unsigned long);
long vfs_unmount(const char *);
long vfs_root(const char *);
long vfs_pivot(const char *,const char *);
long vfs_proc_open(const char *);
long vfs_bootstrap_root(const char *);
long vfs_chdir(int,const char *);
char *vfs_getcwd(long *);
#endif
