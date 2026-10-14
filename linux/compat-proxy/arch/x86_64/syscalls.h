/* x86_64 kernel number mapping; kept in the selected backend. */
#include <sys/syscall.h>
static inline long arch_encode_syscall(long nr) {
    switch(nr) {
#ifdef SYS__sysctl
    case PROXY_SC__sysctl: return SYS__sysctl;
#endif
#ifdef SYS_accept
    case PROXY_SC_accept: return SYS_accept;
#endif
#ifdef SYS_accept4
    case PROXY_SC_accept4: return SYS_accept4;
#endif
#ifdef SYS_access
    case PROXY_SC_access: return SYS_access;
#endif
#ifdef SYS_acct
    case PROXY_SC_acct: return SYS_acct;
#endif
#ifdef SYS_add_key
    case PROXY_SC_add_key: return SYS_add_key;
#endif
#ifdef SYS_adjtimex
    case PROXY_SC_adjtimex: return SYS_adjtimex;
#endif
#ifdef SYS_afs_syscall
    case PROXY_SC_afs_syscall: return SYS_afs_syscall;
#endif
#ifdef SYS_alarm
    case PROXY_SC_alarm: return SYS_alarm;
#endif
#ifdef SYS_arch_prctl
    case PROXY_SC_arch_prctl: return SYS_arch_prctl;
#endif
#ifdef SYS_bind
    case PROXY_SC_bind: return SYS_bind;
#endif
#ifdef SYS_bpf
    case PROXY_SC_bpf: return SYS_bpf;
#endif
#ifdef SYS_brk
    case PROXY_SC_brk: return SYS_brk;
#endif
#ifdef SYS_cachestat
    case PROXY_SC_cachestat: return SYS_cachestat;
#endif
#ifdef SYS_capget
    case PROXY_SC_capget: return SYS_capget;
#endif
#ifdef SYS_capset
    case PROXY_SC_capset: return SYS_capset;
#endif
#ifdef SYS_chdir
    case PROXY_SC_chdir: return SYS_chdir;
#endif
#ifdef SYS_chmod
    case PROXY_SC_chmod: return SYS_chmod;
#endif
#ifdef SYS_chown
    case PROXY_SC_chown: return SYS_chown;
#endif
#ifdef SYS_chroot
    case PROXY_SC_chroot: return SYS_chroot;
#endif
#ifdef SYS_clock_adjtime
    case PROXY_SC_clock_adjtime: return SYS_clock_adjtime;
#endif
#ifdef SYS_clock_getres
    case PROXY_SC_clock_getres: return SYS_clock_getres;
#endif
#ifdef SYS_clock_gettime
    case PROXY_SC_clock_gettime: return SYS_clock_gettime;
#endif
#ifdef SYS_clock_nanosleep
    case PROXY_SC_clock_nanosleep: return SYS_clock_nanosleep;
#endif
#ifdef SYS_clock_settime
    case PROXY_SC_clock_settime: return SYS_clock_settime;
#endif
#ifdef SYS_clone
    case PROXY_SC_clone: return SYS_clone;
#endif
#ifdef SYS_clone3
    case PROXY_SC_clone3: return SYS_clone3;
#endif
#ifdef SYS_close
    case PROXY_SC_close: return SYS_close;
#endif
#ifdef SYS_close_range
    case PROXY_SC_close_range: return SYS_close_range;
#endif
#ifdef SYS_connect
    case PROXY_SC_connect: return SYS_connect;
#endif
#ifdef SYS_copy_file_range
    case PROXY_SC_copy_file_range: return SYS_copy_file_range;
#endif
#ifdef SYS_creat
    case PROXY_SC_creat: return SYS_creat;
#endif
#ifdef SYS_create_module
    case PROXY_SC_create_module: return SYS_create_module;
#endif
#ifdef SYS_delete_module
    case PROXY_SC_delete_module: return SYS_delete_module;
#endif
#ifdef SYS_dup
    case PROXY_SC_dup: return SYS_dup;
#endif
#ifdef SYS_dup2
    case PROXY_SC_dup2: return SYS_dup2;
#endif
#ifdef SYS_dup3
    case PROXY_SC_dup3: return SYS_dup3;
#endif
#ifdef SYS_epoll_create
    case PROXY_SC_epoll_create: return SYS_epoll_create;
#endif
#ifdef SYS_epoll_create1
    case PROXY_SC_epoll_create1: return SYS_epoll_create1;
#endif
#ifdef SYS_epoll_ctl
    case PROXY_SC_epoll_ctl: return SYS_epoll_ctl;
#endif
#ifdef SYS_epoll_ctl_old
    case PROXY_SC_epoll_ctl_old: return SYS_epoll_ctl_old;
#endif
#ifdef SYS_epoll_pwait
    case PROXY_SC_epoll_pwait: return SYS_epoll_pwait;
#endif
#ifdef SYS_epoll_pwait2
    case PROXY_SC_epoll_pwait2: return SYS_epoll_pwait2;
#endif
#ifdef SYS_epoll_wait
    case PROXY_SC_epoll_wait: return SYS_epoll_wait;
#endif
#ifdef SYS_epoll_wait_old
    case PROXY_SC_epoll_wait_old: return SYS_epoll_wait_old;
#endif
#ifdef SYS_eventfd
    case PROXY_SC_eventfd: return SYS_eventfd;
#endif
#ifdef SYS_eventfd2
    case PROXY_SC_eventfd2: return SYS_eventfd2;
#endif
#ifdef SYS_execve
    case PROXY_SC_execve: return SYS_execve;
#endif
#ifdef SYS_execveat
    case PROXY_SC_execveat: return SYS_execveat;
#endif
#ifdef SYS_exit
    case PROXY_SC_exit: return SYS_exit;
#endif
#ifdef SYS_exit_group
    case PROXY_SC_exit_group: return SYS_exit_group;
#endif
#ifdef SYS_faccessat
    case PROXY_SC_faccessat: return SYS_faccessat;
#endif
#ifdef SYS_faccessat2
    case PROXY_SC_faccessat2: return SYS_faccessat2;
#endif
#ifdef SYS_fadvise64
    case PROXY_SC_fadvise64: return SYS_fadvise64;
#endif
#ifdef SYS_fallocate
    case PROXY_SC_fallocate: return SYS_fallocate;
#endif
#ifdef SYS_fanotify_init
    case PROXY_SC_fanotify_init: return SYS_fanotify_init;
#endif
#ifdef SYS_fanotify_mark
    case PROXY_SC_fanotify_mark: return SYS_fanotify_mark;
#endif
#ifdef SYS_fchdir
    case PROXY_SC_fchdir: return SYS_fchdir;
#endif
#ifdef SYS_fchmod
    case PROXY_SC_fchmod: return SYS_fchmod;
#endif
#ifdef SYS_fchmodat
    case PROXY_SC_fchmodat: return SYS_fchmodat;
#endif
#ifdef SYS_fchmodat2
    case PROXY_SC_fchmodat2: return SYS_fchmodat2;
#endif
#ifdef SYS_fchown
    case PROXY_SC_fchown: return SYS_fchown;
#endif
#ifdef SYS_fchownat
    case PROXY_SC_fchownat: return SYS_fchownat;
#endif
#ifdef SYS_fcntl
    case PROXY_SC_fcntl: return SYS_fcntl;
#endif
#ifdef SYS_fdatasync
    case PROXY_SC_fdatasync: return SYS_fdatasync;
#endif
#ifdef SYS_fgetxattr
    case PROXY_SC_fgetxattr: return SYS_fgetxattr;
#endif
#ifdef SYS_file_getattr
    case PROXY_SC_file_getattr: return SYS_file_getattr;
#endif
#ifdef SYS_file_setattr
    case PROXY_SC_file_setattr: return SYS_file_setattr;
#endif
#ifdef SYS_finit_module
    case PROXY_SC_finit_module: return SYS_finit_module;
#endif
#ifdef SYS_flistxattr
    case PROXY_SC_flistxattr: return SYS_flistxattr;
#endif
#ifdef SYS_flock
    case PROXY_SC_flock: return SYS_flock;
#endif
#ifdef SYS_fork
    case PROXY_SC_fork: return SYS_fork;
#endif
#ifdef SYS_fremovexattr
    case PROXY_SC_fremovexattr: return SYS_fremovexattr;
#endif
#ifdef SYS_fsconfig
    case PROXY_SC_fsconfig: return SYS_fsconfig;
#endif
#ifdef SYS_fsetxattr
    case PROXY_SC_fsetxattr: return SYS_fsetxattr;
#endif
#ifdef SYS_fsmount
    case PROXY_SC_fsmount: return SYS_fsmount;
#endif
#ifdef SYS_fsopen
    case PROXY_SC_fsopen: return SYS_fsopen;
#endif
#ifdef SYS_fspick
    case PROXY_SC_fspick: return SYS_fspick;
#endif
#ifdef SYS_fstat
    case PROXY_SC_fstat: return SYS_fstat;
#endif
#ifdef SYS_fstatfs
    case PROXY_SC_fstatfs: return SYS_fstatfs;
#endif
#ifdef SYS_fsync
    case PROXY_SC_fsync: return SYS_fsync;
#endif
#ifdef SYS_ftruncate
    case PROXY_SC_ftruncate: return SYS_ftruncate;
#endif
#ifdef SYS_futex
    case PROXY_SC_futex: return SYS_futex;
#endif
#ifdef SYS_futex_requeue
    case PROXY_SC_futex_requeue: return SYS_futex_requeue;
#endif
#ifdef SYS_futex_wait
    case PROXY_SC_futex_wait: return SYS_futex_wait;
#endif
#ifdef SYS_futex_waitv
    case PROXY_SC_futex_waitv: return SYS_futex_waitv;
#endif
#ifdef SYS_futex_wake
    case PROXY_SC_futex_wake: return SYS_futex_wake;
#endif
#ifdef SYS_futimesat
    case PROXY_SC_futimesat: return SYS_futimesat;
#endif
#ifdef SYS_get_kernel_syms
    case PROXY_SC_get_kernel_syms: return SYS_get_kernel_syms;
#endif
#ifdef SYS_get_mempolicy
    case PROXY_SC_get_mempolicy: return SYS_get_mempolicy;
#endif
#ifdef SYS_get_robust_list
    case PROXY_SC_get_robust_list: return SYS_get_robust_list;
#endif
#ifdef SYS_get_thread_area
    case PROXY_SC_get_thread_area: return SYS_get_thread_area;
#endif
#ifdef SYS_getcpu
    case PROXY_SC_getcpu: return SYS_getcpu;
#endif
#ifdef SYS_getcwd
    case PROXY_SC_getcwd: return SYS_getcwd;
#endif
#ifdef SYS_getdents
    case PROXY_SC_getdents: return SYS_getdents;
#endif
#ifdef SYS_getdents64
    case PROXY_SC_getdents64: return SYS_getdents64;
#endif
#ifdef SYS_getegid
    case PROXY_SC_getegid: return SYS_getegid;
#endif
#ifdef SYS_geteuid
    case PROXY_SC_geteuid: return SYS_geteuid;
#endif
#ifdef SYS_getgid
    case PROXY_SC_getgid: return SYS_getgid;
#endif
#ifdef SYS_getgroups
    case PROXY_SC_getgroups: return SYS_getgroups;
#endif
#ifdef SYS_getitimer
    case PROXY_SC_getitimer: return SYS_getitimer;
#endif
#ifdef SYS_getpeername
    case PROXY_SC_getpeername: return SYS_getpeername;
#endif
#ifdef SYS_getpgid
    case PROXY_SC_getpgid: return SYS_getpgid;
#endif
#ifdef SYS_getpgrp
    case PROXY_SC_getpgrp: return SYS_getpgrp;
#endif
#ifdef SYS_getpid
    case PROXY_SC_getpid: return SYS_getpid;
#endif
#ifdef SYS_getpmsg
    case PROXY_SC_getpmsg: return SYS_getpmsg;
#endif
#ifdef SYS_getppid
    case PROXY_SC_getppid: return SYS_getppid;
#endif
#ifdef SYS_getpriority
    case PROXY_SC_getpriority: return SYS_getpriority;
#endif
#ifdef SYS_getrandom
    case PROXY_SC_getrandom: return SYS_getrandom;
#endif
#ifdef SYS_getresgid
    case PROXY_SC_getresgid: return SYS_getresgid;
#endif
#ifdef SYS_getresuid
    case PROXY_SC_getresuid: return SYS_getresuid;
#endif
#ifdef SYS_getrlimit
    case PROXY_SC_getrlimit: return SYS_getrlimit;
#endif
#ifdef SYS_getrusage
    case PROXY_SC_getrusage: return SYS_getrusage;
#endif
#ifdef SYS_getsid
    case PROXY_SC_getsid: return SYS_getsid;
#endif
#ifdef SYS_getsockname
    case PROXY_SC_getsockname: return SYS_getsockname;
#endif
#ifdef SYS_getsockopt
    case PROXY_SC_getsockopt: return SYS_getsockopt;
#endif
#ifdef SYS_gettid
    case PROXY_SC_gettid: return SYS_gettid;
#endif
#ifdef SYS_gettimeofday
    case PROXY_SC_gettimeofday: return SYS_gettimeofday;
#endif
#ifdef SYS_getuid
    case PROXY_SC_getuid: return SYS_getuid;
#endif
#ifdef SYS_getxattr
    case PROXY_SC_getxattr: return SYS_getxattr;
#endif
#ifdef SYS_getxattrat
    case PROXY_SC_getxattrat: return SYS_getxattrat;
#endif
#ifdef SYS_init_module
    case PROXY_SC_init_module: return SYS_init_module;
#endif
#ifdef SYS_inotify_add_watch
    case PROXY_SC_inotify_add_watch: return SYS_inotify_add_watch;
#endif
#ifdef SYS_inotify_init
    case PROXY_SC_inotify_init: return SYS_inotify_init;
#endif
#ifdef SYS_inotify_init1
    case PROXY_SC_inotify_init1: return SYS_inotify_init1;
#endif
#ifdef SYS_inotify_rm_watch
    case PROXY_SC_inotify_rm_watch: return SYS_inotify_rm_watch;
#endif
#ifdef SYS_io_cancel
    case PROXY_SC_io_cancel: return SYS_io_cancel;
#endif
#ifdef SYS_io_destroy
    case PROXY_SC_io_destroy: return SYS_io_destroy;
#endif
#ifdef SYS_io_getevents
    case PROXY_SC_io_getevents: return SYS_io_getevents;
#endif
#ifdef SYS_io_pgetevents
    case PROXY_SC_io_pgetevents: return SYS_io_pgetevents;
#endif
#ifdef SYS_io_setup
    case PROXY_SC_io_setup: return SYS_io_setup;
#endif
#ifdef SYS_io_submit
    case PROXY_SC_io_submit: return SYS_io_submit;
#endif
#ifdef SYS_io_uring_enter
    case PROXY_SC_io_uring_enter: return SYS_io_uring_enter;
#endif
#ifdef SYS_io_uring_register
    case PROXY_SC_io_uring_register: return SYS_io_uring_register;
#endif
#ifdef SYS_io_uring_setup
    case PROXY_SC_io_uring_setup: return SYS_io_uring_setup;
#endif
#ifdef SYS_ioctl
    case PROXY_SC_ioctl: return SYS_ioctl;
#endif
#ifdef SYS_ioperm
    case PROXY_SC_ioperm: return SYS_ioperm;
#endif
#ifdef SYS_iopl
    case PROXY_SC_iopl: return SYS_iopl;
#endif
#ifdef SYS_ioprio_get
    case PROXY_SC_ioprio_get: return SYS_ioprio_get;
#endif
#ifdef SYS_ioprio_set
    case PROXY_SC_ioprio_set: return SYS_ioprio_set;
#endif
#ifdef SYS_kcmp
    case PROXY_SC_kcmp: return SYS_kcmp;
#endif
#ifdef SYS_kexec_file_load
    case PROXY_SC_kexec_file_load: return SYS_kexec_file_load;
#endif
#ifdef SYS_kexec_load
    case PROXY_SC_kexec_load: return SYS_kexec_load;
#endif
#ifdef SYS_keyctl
    case PROXY_SC_keyctl: return SYS_keyctl;
#endif
#ifdef SYS_kill
    case PROXY_SC_kill: return SYS_kill;
#endif
#ifdef SYS_landlock_add_rule
    case PROXY_SC_landlock_add_rule: return SYS_landlock_add_rule;
#endif
#ifdef SYS_landlock_create_ruleset
    case PROXY_SC_landlock_create_ruleset: return SYS_landlock_create_ruleset;
#endif
#ifdef SYS_landlock_restrict_self
    case PROXY_SC_landlock_restrict_self: return SYS_landlock_restrict_self;
#endif
#ifdef SYS_lchown
    case PROXY_SC_lchown: return SYS_lchown;
#endif
#ifdef SYS_lgetxattr
    case PROXY_SC_lgetxattr: return SYS_lgetxattr;
#endif
#ifdef SYS_link
    case PROXY_SC_link: return SYS_link;
#endif
#ifdef SYS_linkat
    case PROXY_SC_linkat: return SYS_linkat;
#endif
#ifdef SYS_listen
    case PROXY_SC_listen: return SYS_listen;
#endif
#ifdef SYS_listmount
    case PROXY_SC_listmount: return SYS_listmount;
#endif
#ifdef SYS_listns
    case PROXY_SC_listns: return SYS_listns;
#endif
#ifdef SYS_listxattr
    case PROXY_SC_listxattr: return SYS_listxattr;
#endif
#ifdef SYS_listxattrat
    case PROXY_SC_listxattrat: return SYS_listxattrat;
#endif
#ifdef SYS_llistxattr
    case PROXY_SC_llistxattr: return SYS_llistxattr;
#endif
#ifdef SYS_lookup_dcookie
    case PROXY_SC_lookup_dcookie: return SYS_lookup_dcookie;
#endif
#ifdef SYS_lremovexattr
    case PROXY_SC_lremovexattr: return SYS_lremovexattr;
#endif
#ifdef SYS_lseek
    case PROXY_SC_lseek: return SYS_lseek;
#endif
#ifdef SYS_lsetxattr
    case PROXY_SC_lsetxattr: return SYS_lsetxattr;
#endif
#ifdef SYS_lsm_get_self_attr
    case PROXY_SC_lsm_get_self_attr: return SYS_lsm_get_self_attr;
#endif
#ifdef SYS_lsm_list_modules
    case PROXY_SC_lsm_list_modules: return SYS_lsm_list_modules;
#endif
#ifdef SYS_lsm_set_self_attr
    case PROXY_SC_lsm_set_self_attr: return SYS_lsm_set_self_attr;
#endif
#ifdef SYS_lstat
    case PROXY_SC_lstat: return SYS_lstat;
#endif
#ifdef SYS_madvise
    case PROXY_SC_madvise: return SYS_madvise;
#endif
#ifdef SYS_map_shadow_stack
    case PROXY_SC_map_shadow_stack: return SYS_map_shadow_stack;
#endif
#ifdef SYS_mbind
    case PROXY_SC_mbind: return SYS_mbind;
#endif
#ifdef SYS_membarrier
    case PROXY_SC_membarrier: return SYS_membarrier;
#endif
#ifdef SYS_memfd_create
    case PROXY_SC_memfd_create: return SYS_memfd_create;
#endif
#ifdef SYS_memfd_secret
    case PROXY_SC_memfd_secret: return SYS_memfd_secret;
#endif
#ifdef SYS_migrate_pages
    case PROXY_SC_migrate_pages: return SYS_migrate_pages;
#endif
#ifdef SYS_mincore
    case PROXY_SC_mincore: return SYS_mincore;
#endif
#ifdef SYS_mkdir
    case PROXY_SC_mkdir: return SYS_mkdir;
#endif
#ifdef SYS_mkdirat
    case PROXY_SC_mkdirat: return SYS_mkdirat;
#endif
#ifdef SYS_mknod
    case PROXY_SC_mknod: return SYS_mknod;
#endif
#ifdef SYS_mknodat
    case PROXY_SC_mknodat: return SYS_mknodat;
#endif
#ifdef SYS_mlock
    case PROXY_SC_mlock: return SYS_mlock;
#endif
#ifdef SYS_mlock2
    case PROXY_SC_mlock2: return SYS_mlock2;
#endif
#ifdef SYS_mlockall
    case PROXY_SC_mlockall: return SYS_mlockall;
#endif
#ifdef SYS_mmap
    case PROXY_SC_mmap: return SYS_mmap;
#endif
#ifdef SYS_modify_ldt
    case PROXY_SC_modify_ldt: return SYS_modify_ldt;
#endif
#ifdef SYS_mount
    case PROXY_SC_mount: return SYS_mount;
#endif
#ifdef SYS_mount_setattr
    case PROXY_SC_mount_setattr: return SYS_mount_setattr;
#endif
#ifdef SYS_move_mount
    case PROXY_SC_move_mount: return SYS_move_mount;
#endif
#ifdef SYS_move_pages
    case PROXY_SC_move_pages: return SYS_move_pages;
#endif
#ifdef SYS_mprotect
    case PROXY_SC_mprotect: return SYS_mprotect;
#endif
#ifdef SYS_mq_getsetattr
    case PROXY_SC_mq_getsetattr: return SYS_mq_getsetattr;
#endif
#ifdef SYS_mq_notify
    case PROXY_SC_mq_notify: return SYS_mq_notify;
#endif
#ifdef SYS_mq_open
    case PROXY_SC_mq_open: return SYS_mq_open;
#endif
#ifdef SYS_mq_timedreceive
    case PROXY_SC_mq_timedreceive: return SYS_mq_timedreceive;
#endif
#ifdef SYS_mq_timedsend
    case PROXY_SC_mq_timedsend: return SYS_mq_timedsend;
#endif
#ifdef SYS_mq_unlink
    case PROXY_SC_mq_unlink: return SYS_mq_unlink;
#endif
#ifdef SYS_mremap
    case PROXY_SC_mremap: return SYS_mremap;
#endif
#ifdef SYS_mseal
    case PROXY_SC_mseal: return SYS_mseal;
#endif
#ifdef SYS_msgctl
    case PROXY_SC_msgctl: return SYS_msgctl;
#endif
#ifdef SYS_msgget
    case PROXY_SC_msgget: return SYS_msgget;
#endif
#ifdef SYS_msgrcv
    case PROXY_SC_msgrcv: return SYS_msgrcv;
#endif
#ifdef SYS_msgsnd
    case PROXY_SC_msgsnd: return SYS_msgsnd;
#endif
#ifdef SYS_msync
    case PROXY_SC_msync: return SYS_msync;
#endif
#ifdef SYS_munlock
    case PROXY_SC_munlock: return SYS_munlock;
#endif
#ifdef SYS_munlockall
    case PROXY_SC_munlockall: return SYS_munlockall;
#endif
#ifdef SYS_munmap
    case PROXY_SC_munmap: return SYS_munmap;
#endif
#ifdef SYS_name_to_handle_at
    case PROXY_SC_name_to_handle_at: return SYS_name_to_handle_at;
#endif
#ifdef SYS_nanosleep
    case PROXY_SC_nanosleep: return SYS_nanosleep;
#endif
#ifdef SYS_newfstatat
    case PROXY_SC_newfstatat: return SYS_newfstatat;
#endif
#ifdef SYS_nfsservctl
    case PROXY_SC_nfsservctl: return SYS_nfsservctl;
#endif
#ifdef SYS_open
    case PROXY_SC_open: return SYS_open;
#endif
#ifdef SYS_open_by_handle_at
    case PROXY_SC_open_by_handle_at: return SYS_open_by_handle_at;
#endif
#ifdef SYS_open_tree
    case PROXY_SC_open_tree: return SYS_open_tree;
#endif
#ifdef SYS_open_tree_attr
    case PROXY_SC_open_tree_attr: return SYS_open_tree_attr;
#endif
#ifdef SYS_openat
    case PROXY_SC_openat: return SYS_openat;
#endif
#ifdef SYS_openat2
    case PROXY_SC_openat2: return SYS_openat2;
#endif
#ifdef SYS_pause
    case PROXY_SC_pause: return SYS_pause;
#endif
#ifdef SYS_perf_event_open
    case PROXY_SC_perf_event_open: return SYS_perf_event_open;
#endif
#ifdef SYS_personality
    case PROXY_SC_personality: return SYS_personality;
#endif
#ifdef SYS_pidfd_getfd
    case PROXY_SC_pidfd_getfd: return SYS_pidfd_getfd;
#endif
#ifdef SYS_pidfd_open
    case PROXY_SC_pidfd_open: return SYS_pidfd_open;
#endif
#ifdef SYS_pidfd_send_signal
    case PROXY_SC_pidfd_send_signal: return SYS_pidfd_send_signal;
#endif
#ifdef SYS_pipe
    case PROXY_SC_pipe: return SYS_pipe;
#endif
#ifdef SYS_pipe2
    case PROXY_SC_pipe2: return SYS_pipe2;
#endif
#ifdef SYS_pivot_root
    case PROXY_SC_pivot_root: return SYS_pivot_root;
#endif
#ifdef SYS_pkey_alloc
    case PROXY_SC_pkey_alloc: return SYS_pkey_alloc;
#endif
#ifdef SYS_pkey_free
    case PROXY_SC_pkey_free: return SYS_pkey_free;
#endif
#ifdef SYS_pkey_mprotect
    case PROXY_SC_pkey_mprotect: return SYS_pkey_mprotect;
#endif
#ifdef SYS_poll
    case PROXY_SC_poll: return SYS_poll;
#endif
#ifdef SYS_ppoll
    case PROXY_SC_ppoll: return SYS_ppoll;
#endif
#ifdef SYS_prctl
    case PROXY_SC_prctl: return SYS_prctl;
#endif
#ifdef SYS_pread64
    case PROXY_SC_pread64: return SYS_pread64;
#endif
#ifdef SYS_preadv
    case PROXY_SC_preadv: return SYS_preadv;
#endif
#ifdef SYS_preadv2
    case PROXY_SC_preadv2: return SYS_preadv2;
#endif
#ifdef SYS_prlimit64
    case PROXY_SC_prlimit64: return SYS_prlimit64;
#endif
#ifdef SYS_process_madvise
    case PROXY_SC_process_madvise: return SYS_process_madvise;
#endif
#ifdef SYS_process_mrelease
    case PROXY_SC_process_mrelease: return SYS_process_mrelease;
#endif
#ifdef SYS_process_vm_readv
    case PROXY_SC_process_vm_readv: return SYS_process_vm_readv;
#endif
#ifdef SYS_process_vm_writev
    case PROXY_SC_process_vm_writev: return SYS_process_vm_writev;
#endif
#ifdef SYS_pselect6
    case PROXY_SC_pselect6: return SYS_pselect6;
#endif
#ifdef SYS_ptrace
    case PROXY_SC_ptrace: return SYS_ptrace;
#endif
#ifdef SYS_putpmsg
    case PROXY_SC_putpmsg: return SYS_putpmsg;
#endif
#ifdef SYS_pwrite64
    case PROXY_SC_pwrite64: return SYS_pwrite64;
#endif
#ifdef SYS_pwritev
    case PROXY_SC_pwritev: return SYS_pwritev;
#endif
#ifdef SYS_pwritev2
    case PROXY_SC_pwritev2: return SYS_pwritev2;
#endif
#ifdef SYS_query_module
    case PROXY_SC_query_module: return SYS_query_module;
#endif
#ifdef SYS_quotactl
    case PROXY_SC_quotactl: return SYS_quotactl;
#endif
#ifdef SYS_quotactl_fd
    case PROXY_SC_quotactl_fd: return SYS_quotactl_fd;
#endif
#ifdef SYS_read
    case PROXY_SC_read: return SYS_read;
#endif
#ifdef SYS_readahead
    case PROXY_SC_readahead: return SYS_readahead;
#endif
#ifdef SYS_readlink
    case PROXY_SC_readlink: return SYS_readlink;
#endif
#ifdef SYS_readlinkat
    case PROXY_SC_readlinkat: return SYS_readlinkat;
#endif
#ifdef SYS_readv
    case PROXY_SC_readv: return SYS_readv;
#endif
#ifdef SYS_reboot
    case PROXY_SC_reboot: return SYS_reboot;
#endif
#ifdef SYS_recvfrom
    case PROXY_SC_recvfrom: return SYS_recvfrom;
#endif
#ifdef SYS_recvmmsg
    case PROXY_SC_recvmmsg: return SYS_recvmmsg;
#endif
#ifdef SYS_recvmsg
    case PROXY_SC_recvmsg: return SYS_recvmsg;
#endif
#ifdef SYS_remap_file_pages
    case PROXY_SC_remap_file_pages: return SYS_remap_file_pages;
#endif
#ifdef SYS_removexattr
    case PROXY_SC_removexattr: return SYS_removexattr;
#endif
#ifdef SYS_removexattrat
    case PROXY_SC_removexattrat: return SYS_removexattrat;
#endif
#ifdef SYS_rename
    case PROXY_SC_rename: return SYS_rename;
#endif
#ifdef SYS_renameat
    case PROXY_SC_renameat: return SYS_renameat;
#endif
#ifdef SYS_renameat2
    case PROXY_SC_renameat2: return SYS_renameat2;
#endif
#ifdef SYS_request_key
    case PROXY_SC_request_key: return SYS_request_key;
#endif
#ifdef SYS_restart_syscall
    case PROXY_SC_restart_syscall: return SYS_restart_syscall;
#endif
#ifdef SYS_rmdir
    case PROXY_SC_rmdir: return SYS_rmdir;
#endif
#ifdef SYS_rseq
    case PROXY_SC_rseq: return SYS_rseq;
#endif
#ifdef SYS_rt_sigaction
    case PROXY_SC_rt_sigaction: return SYS_rt_sigaction;
#endif
#ifdef SYS_rt_sigpending
    case PROXY_SC_rt_sigpending: return SYS_rt_sigpending;
#endif
#ifdef SYS_rt_sigprocmask
    case PROXY_SC_rt_sigprocmask: return SYS_rt_sigprocmask;
#endif
#ifdef SYS_rt_sigqueueinfo
    case PROXY_SC_rt_sigqueueinfo: return SYS_rt_sigqueueinfo;
#endif
#ifdef SYS_rt_sigreturn
    case PROXY_SC_rt_sigreturn: return SYS_rt_sigreturn;
#endif
#ifdef SYS_rt_sigsuspend
    case PROXY_SC_rt_sigsuspend: return SYS_rt_sigsuspend;
#endif
#ifdef SYS_rt_sigtimedwait
    case PROXY_SC_rt_sigtimedwait: return SYS_rt_sigtimedwait;
#endif
#ifdef SYS_rt_tgsigqueueinfo
    case PROXY_SC_rt_tgsigqueueinfo: return SYS_rt_tgsigqueueinfo;
#endif
#ifdef SYS_sched_get_priority_max
    case PROXY_SC_sched_get_priority_max: return SYS_sched_get_priority_max;
#endif
#ifdef SYS_sched_get_priority_min
    case PROXY_SC_sched_get_priority_min: return SYS_sched_get_priority_min;
#endif
#ifdef SYS_sched_getaffinity
    case PROXY_SC_sched_getaffinity: return SYS_sched_getaffinity;
#endif
#ifdef SYS_sched_getattr
    case PROXY_SC_sched_getattr: return SYS_sched_getattr;
#endif
#ifdef SYS_sched_getparam
    case PROXY_SC_sched_getparam: return SYS_sched_getparam;
#endif
#ifdef SYS_sched_getscheduler
    case PROXY_SC_sched_getscheduler: return SYS_sched_getscheduler;
#endif
#ifdef SYS_sched_rr_get_interval
    case PROXY_SC_sched_rr_get_interval: return SYS_sched_rr_get_interval;
#endif
#ifdef SYS_sched_setaffinity
    case PROXY_SC_sched_setaffinity: return SYS_sched_setaffinity;
#endif
#ifdef SYS_sched_setattr
    case PROXY_SC_sched_setattr: return SYS_sched_setattr;
#endif
#ifdef SYS_sched_setparam
    case PROXY_SC_sched_setparam: return SYS_sched_setparam;
#endif
#ifdef SYS_sched_setscheduler
    case PROXY_SC_sched_setscheduler: return SYS_sched_setscheduler;
#endif
#ifdef SYS_sched_yield
    case PROXY_SC_sched_yield: return SYS_sched_yield;
#endif
#ifdef SYS_seccomp
    case PROXY_SC_seccomp: return SYS_seccomp;
#endif
#ifdef SYS_security
    case PROXY_SC_security: return SYS_security;
#endif
#ifdef SYS_select
    case PROXY_SC_select: return SYS_select;
#endif
#ifdef SYS_semctl
    case PROXY_SC_semctl: return SYS_semctl;
#endif
#ifdef SYS_semget
    case PROXY_SC_semget: return SYS_semget;
#endif
#ifdef SYS_semop
    case PROXY_SC_semop: return SYS_semop;
#endif
#ifdef SYS_semtimedop
    case PROXY_SC_semtimedop: return SYS_semtimedop;
#endif
#ifdef SYS_sendfile
    case PROXY_SC_sendfile: return SYS_sendfile;
#endif
#ifdef SYS_sendmmsg
    case PROXY_SC_sendmmsg: return SYS_sendmmsg;
#endif
#ifdef SYS_sendmsg
    case PROXY_SC_sendmsg: return SYS_sendmsg;
#endif
#ifdef SYS_sendto
    case PROXY_SC_sendto: return SYS_sendto;
#endif
#ifdef SYS_set_mempolicy
    case PROXY_SC_set_mempolicy: return SYS_set_mempolicy;
#endif
#ifdef SYS_set_mempolicy_home_node
    case PROXY_SC_set_mempolicy_home_node: return SYS_set_mempolicy_home_node;
#endif
#ifdef SYS_set_robust_list
    case PROXY_SC_set_robust_list: return SYS_set_robust_list;
#endif
#ifdef SYS_set_thread_area
    case PROXY_SC_set_thread_area: return SYS_set_thread_area;
#endif
#ifdef SYS_set_tid_address
    case PROXY_SC_set_tid_address: return SYS_set_tid_address;
#endif
#ifdef SYS_setdomainname
    case PROXY_SC_setdomainname: return SYS_setdomainname;
#endif
#ifdef SYS_setfsgid
    case PROXY_SC_setfsgid: return SYS_setfsgid;
#endif
#ifdef SYS_setfsuid
    case PROXY_SC_setfsuid: return SYS_setfsuid;
#endif
#ifdef SYS_setgid
    case PROXY_SC_setgid: return SYS_setgid;
#endif
#ifdef SYS_setgroups
    case PROXY_SC_setgroups: return SYS_setgroups;
#endif
#ifdef SYS_sethostname
    case PROXY_SC_sethostname: return SYS_sethostname;
#endif
#ifdef SYS_setitimer
    case PROXY_SC_setitimer: return SYS_setitimer;
#endif
#ifdef SYS_setns
    case PROXY_SC_setns: return SYS_setns;
#endif
#ifdef SYS_setpgid
    case PROXY_SC_setpgid: return SYS_setpgid;
#endif
#ifdef SYS_setpriority
    case PROXY_SC_setpriority: return SYS_setpriority;
#endif
#ifdef SYS_setregid
    case PROXY_SC_setregid: return SYS_setregid;
#endif
#ifdef SYS_setresgid
    case PROXY_SC_setresgid: return SYS_setresgid;
#endif
#ifdef SYS_setresuid
    case PROXY_SC_setresuid: return SYS_setresuid;
#endif
#ifdef SYS_setreuid
    case PROXY_SC_setreuid: return SYS_setreuid;
#endif
#ifdef SYS_setrlimit
    case PROXY_SC_setrlimit: return SYS_setrlimit;
#endif
#ifdef SYS_setsid
    case PROXY_SC_setsid: return SYS_setsid;
#endif
#ifdef SYS_setsockopt
    case PROXY_SC_setsockopt: return SYS_setsockopt;
#endif
#ifdef SYS_settimeofday
    case PROXY_SC_settimeofday: return SYS_settimeofday;
#endif
#ifdef SYS_setuid
    case PROXY_SC_setuid: return SYS_setuid;
#endif
#ifdef SYS_setxattr
    case PROXY_SC_setxattr: return SYS_setxattr;
#endif
#ifdef SYS_setxattrat
    case PROXY_SC_setxattrat: return SYS_setxattrat;
#endif
#ifdef SYS_shmat
    case PROXY_SC_shmat: return SYS_shmat;
#endif
#ifdef SYS_shmctl
    case PROXY_SC_shmctl: return SYS_shmctl;
#endif
#ifdef SYS_shmdt
    case PROXY_SC_shmdt: return SYS_shmdt;
#endif
#ifdef SYS_shmget
    case PROXY_SC_shmget: return SYS_shmget;
#endif
#ifdef SYS_shutdown
    case PROXY_SC_shutdown: return SYS_shutdown;
#endif
#ifdef SYS_sigaltstack
    case PROXY_SC_sigaltstack: return SYS_sigaltstack;
#endif
#ifdef SYS_signalfd
    case PROXY_SC_signalfd: return SYS_signalfd;
#endif
#ifdef SYS_signalfd4
    case PROXY_SC_signalfd4: return SYS_signalfd4;
#endif
#ifdef SYS_socket
    case PROXY_SC_socket: return SYS_socket;
#endif
#ifdef SYS_socketpair
    case PROXY_SC_socketpair: return SYS_socketpair;
#endif
#ifdef SYS_splice
    case PROXY_SC_splice: return SYS_splice;
#endif
#ifdef SYS_stat
    case PROXY_SC_stat: return SYS_stat;
#endif
#ifdef SYS_statfs
    case PROXY_SC_statfs: return SYS_statfs;
#endif
#ifdef SYS_statmount
    case PROXY_SC_statmount: return SYS_statmount;
#endif
#ifdef SYS_statx
    case PROXY_SC_statx: return SYS_statx;
#endif
#ifdef SYS_swapoff
    case PROXY_SC_swapoff: return SYS_swapoff;
#endif
#ifdef SYS_swapon
    case PROXY_SC_swapon: return SYS_swapon;
#endif
#ifdef SYS_symlink
    case PROXY_SC_symlink: return SYS_symlink;
#endif
#ifdef SYS_symlinkat
    case PROXY_SC_symlinkat: return SYS_symlinkat;
#endif
#ifdef SYS_sync
    case PROXY_SC_sync: return SYS_sync;
#endif
#ifdef SYS_sync_file_range
    case PROXY_SC_sync_file_range: return SYS_sync_file_range;
#endif
#ifdef SYS_syncfs
    case PROXY_SC_syncfs: return SYS_syncfs;
#endif
#ifdef SYS_sysfs
    case PROXY_SC_sysfs: return SYS_sysfs;
#endif
#ifdef SYS_sysinfo
    case PROXY_SC_sysinfo: return SYS_sysinfo;
#endif
#ifdef SYS_syslog
    case PROXY_SC_syslog: return SYS_syslog;
#endif
#ifdef SYS_tee
    case PROXY_SC_tee: return SYS_tee;
#endif
#ifdef SYS_tgkill
    case PROXY_SC_tgkill: return SYS_tgkill;
#endif
#ifdef SYS_time
    case PROXY_SC_time: return SYS_time;
#endif
#ifdef SYS_timer_create
    case PROXY_SC_timer_create: return SYS_timer_create;
#endif
#ifdef SYS_timer_delete
    case PROXY_SC_timer_delete: return SYS_timer_delete;
#endif
#ifdef SYS_timer_getoverrun
    case PROXY_SC_timer_getoverrun: return SYS_timer_getoverrun;
#endif
#ifdef SYS_timer_gettime
    case PROXY_SC_timer_gettime: return SYS_timer_gettime;
#endif
#ifdef SYS_timer_settime
    case PROXY_SC_timer_settime: return SYS_timer_settime;
#endif
#ifdef SYS_timerfd_create
    case PROXY_SC_timerfd_create: return SYS_timerfd_create;
#endif
#ifdef SYS_timerfd_gettime
    case PROXY_SC_timerfd_gettime: return SYS_timerfd_gettime;
#endif
#ifdef SYS_timerfd_settime
    case PROXY_SC_timerfd_settime: return SYS_timerfd_settime;
#endif
#ifdef SYS_times
    case PROXY_SC_times: return SYS_times;
#endif
#ifdef SYS_tkill
    case PROXY_SC_tkill: return SYS_tkill;
#endif
#ifdef SYS_truncate
    case PROXY_SC_truncate: return SYS_truncate;
#endif
#ifdef SYS_tuxcall
    case PROXY_SC_tuxcall: return SYS_tuxcall;
#endif
#ifdef SYS_umask
    case PROXY_SC_umask: return SYS_umask;
#endif
#ifdef SYS_umount2
    case PROXY_SC_umount2: return SYS_umount2;
#endif
#ifdef SYS_uname
    case PROXY_SC_uname: return SYS_uname;
#endif
#ifdef SYS_unlink
    case PROXY_SC_unlink: return SYS_unlink;
#endif
#ifdef SYS_unlinkat
    case PROXY_SC_unlinkat: return SYS_unlinkat;
#endif
#ifdef SYS_unshare
    case PROXY_SC_unshare: return SYS_unshare;
#endif
#ifdef SYS_uprobe
    case PROXY_SC_uprobe: return SYS_uprobe;
#endif
#ifdef SYS_uretprobe
    case PROXY_SC_uretprobe: return SYS_uretprobe;
#endif
#ifdef SYS_uselib
    case PROXY_SC_uselib: return SYS_uselib;
#endif
#ifdef SYS_userfaultfd
    case PROXY_SC_userfaultfd: return SYS_userfaultfd;
#endif
#ifdef SYS_ustat
    case PROXY_SC_ustat: return SYS_ustat;
#endif
#ifdef SYS_utime
    case PROXY_SC_utime: return SYS_utime;
#endif
#ifdef SYS_utimensat
    case PROXY_SC_utimensat: return SYS_utimensat;
#endif
#ifdef SYS_utimes
    case PROXY_SC_utimes: return SYS_utimes;
#endif
#ifdef SYS_vfork
    case PROXY_SC_vfork: return SYS_vfork;
#endif
#ifdef SYS_vhangup
    case PROXY_SC_vhangup: return SYS_vhangup;
#endif
#ifdef SYS_vmsplice
    case PROXY_SC_vmsplice: return SYS_vmsplice;
#endif
#ifdef SYS_vserver
    case PROXY_SC_vserver: return SYS_vserver;
#endif
#ifdef SYS_wait4
    case PROXY_SC_wait4: return SYS_wait4;
#endif
#ifdef SYS_waitid
    case PROXY_SC_waitid: return SYS_waitid;
#endif
#ifdef SYS_write
    case PROXY_SC_write: return SYS_write;
#endif
#ifdef SYS_writev
    case PROXY_SC_writev: return SYS_writev;
#endif
    default: return -1;
    }
}
static inline long arch_decode_syscall(long nr) {
    switch(nr) {
#ifdef SYS__sysctl
    case SYS__sysctl: return PROXY_SC__sysctl;
#endif
#ifdef SYS_accept
    case SYS_accept: return PROXY_SC_accept;
#endif
#ifdef SYS_accept4
    case SYS_accept4: return PROXY_SC_accept4;
#endif
#ifdef SYS_access
    case SYS_access: return PROXY_SC_access;
#endif
#ifdef SYS_acct
    case SYS_acct: return PROXY_SC_acct;
#endif
#ifdef SYS_add_key
    case SYS_add_key: return PROXY_SC_add_key;
#endif
#ifdef SYS_adjtimex
    case SYS_adjtimex: return PROXY_SC_adjtimex;
#endif
#ifdef SYS_afs_syscall
    case SYS_afs_syscall: return PROXY_SC_afs_syscall;
#endif
#ifdef SYS_alarm
    case SYS_alarm: return PROXY_SC_alarm;
#endif
#ifdef SYS_arch_prctl
    case SYS_arch_prctl: return PROXY_SC_arch_prctl;
#endif
#ifdef SYS_bind
    case SYS_bind: return PROXY_SC_bind;
#endif
#ifdef SYS_bpf
    case SYS_bpf: return PROXY_SC_bpf;
#endif
#ifdef SYS_brk
    case SYS_brk: return PROXY_SC_brk;
#endif
#ifdef SYS_cachestat
    case SYS_cachestat: return PROXY_SC_cachestat;
#endif
#ifdef SYS_capget
    case SYS_capget: return PROXY_SC_capget;
#endif
#ifdef SYS_capset
    case SYS_capset: return PROXY_SC_capset;
#endif
#ifdef SYS_chdir
    case SYS_chdir: return PROXY_SC_chdir;
#endif
#ifdef SYS_chmod
    case SYS_chmod: return PROXY_SC_chmod;
#endif
#ifdef SYS_chown
    case SYS_chown: return PROXY_SC_chown;
#endif
#ifdef SYS_chroot
    case SYS_chroot: return PROXY_SC_chroot;
#endif
#ifdef SYS_clock_adjtime
    case SYS_clock_adjtime: return PROXY_SC_clock_adjtime;
#endif
#ifdef SYS_clock_getres
    case SYS_clock_getres: return PROXY_SC_clock_getres;
#endif
#ifdef SYS_clock_gettime
    case SYS_clock_gettime: return PROXY_SC_clock_gettime;
#endif
#ifdef SYS_clock_nanosleep
    case SYS_clock_nanosleep: return PROXY_SC_clock_nanosleep;
#endif
#ifdef SYS_clock_settime
    case SYS_clock_settime: return PROXY_SC_clock_settime;
#endif
#ifdef SYS_clone
    case SYS_clone: return PROXY_SC_clone;
#endif
#ifdef SYS_clone3
    case SYS_clone3: return PROXY_SC_clone3;
#endif
#ifdef SYS_close
    case SYS_close: return PROXY_SC_close;
#endif
#ifdef SYS_close_range
    case SYS_close_range: return PROXY_SC_close_range;
#endif
#ifdef SYS_connect
    case SYS_connect: return PROXY_SC_connect;
#endif
#ifdef SYS_copy_file_range
    case SYS_copy_file_range: return PROXY_SC_copy_file_range;
#endif
#ifdef SYS_creat
    case SYS_creat: return PROXY_SC_creat;
#endif
#ifdef SYS_create_module
    case SYS_create_module: return PROXY_SC_create_module;
#endif
#ifdef SYS_delete_module
    case SYS_delete_module: return PROXY_SC_delete_module;
#endif
#ifdef SYS_dup
    case SYS_dup: return PROXY_SC_dup;
#endif
#ifdef SYS_dup2
    case SYS_dup2: return PROXY_SC_dup2;
#endif
#ifdef SYS_dup3
    case SYS_dup3: return PROXY_SC_dup3;
#endif
#ifdef SYS_epoll_create
    case SYS_epoll_create: return PROXY_SC_epoll_create;
#endif
#ifdef SYS_epoll_create1
    case SYS_epoll_create1: return PROXY_SC_epoll_create1;
#endif
#ifdef SYS_epoll_ctl
    case SYS_epoll_ctl: return PROXY_SC_epoll_ctl;
#endif
#ifdef SYS_epoll_ctl_old
    case SYS_epoll_ctl_old: return PROXY_SC_epoll_ctl_old;
#endif
#ifdef SYS_epoll_pwait
    case SYS_epoll_pwait: return PROXY_SC_epoll_pwait;
#endif
#ifdef SYS_epoll_pwait2
    case SYS_epoll_pwait2: return PROXY_SC_epoll_pwait2;
#endif
#ifdef SYS_epoll_wait
    case SYS_epoll_wait: return PROXY_SC_epoll_wait;
#endif
#ifdef SYS_epoll_wait_old
    case SYS_epoll_wait_old: return PROXY_SC_epoll_wait_old;
#endif
#ifdef SYS_eventfd
    case SYS_eventfd: return PROXY_SC_eventfd;
#endif
#ifdef SYS_eventfd2
    case SYS_eventfd2: return PROXY_SC_eventfd2;
#endif
#ifdef SYS_execve
    case SYS_execve: return PROXY_SC_execve;
#endif
#ifdef SYS_execveat
    case SYS_execveat: return PROXY_SC_execveat;
#endif
#ifdef SYS_exit
    case SYS_exit: return PROXY_SC_exit;
#endif
#ifdef SYS_exit_group
    case SYS_exit_group: return PROXY_SC_exit_group;
#endif
#ifdef SYS_faccessat
    case SYS_faccessat: return PROXY_SC_faccessat;
#endif
#ifdef SYS_faccessat2
    case SYS_faccessat2: return PROXY_SC_faccessat2;
#endif
#ifdef SYS_fadvise64
    case SYS_fadvise64: return PROXY_SC_fadvise64;
#endif
#ifdef SYS_fallocate
    case SYS_fallocate: return PROXY_SC_fallocate;
#endif
#ifdef SYS_fanotify_init
    case SYS_fanotify_init: return PROXY_SC_fanotify_init;
#endif
#ifdef SYS_fanotify_mark
    case SYS_fanotify_mark: return PROXY_SC_fanotify_mark;
#endif
#ifdef SYS_fchdir
    case SYS_fchdir: return PROXY_SC_fchdir;
#endif
#ifdef SYS_fchmod
    case SYS_fchmod: return PROXY_SC_fchmod;
#endif
#ifdef SYS_fchmodat
    case SYS_fchmodat: return PROXY_SC_fchmodat;
#endif
#ifdef SYS_fchmodat2
    case SYS_fchmodat2: return PROXY_SC_fchmodat2;
#endif
#ifdef SYS_fchown
    case SYS_fchown: return PROXY_SC_fchown;
#endif
#ifdef SYS_fchownat
    case SYS_fchownat: return PROXY_SC_fchownat;
#endif
#ifdef SYS_fcntl
    case SYS_fcntl: return PROXY_SC_fcntl;
#endif
#ifdef SYS_fdatasync
    case SYS_fdatasync: return PROXY_SC_fdatasync;
#endif
#ifdef SYS_fgetxattr
    case SYS_fgetxattr: return PROXY_SC_fgetxattr;
#endif
#ifdef SYS_file_getattr
    case SYS_file_getattr: return PROXY_SC_file_getattr;
#endif
#ifdef SYS_file_setattr
    case SYS_file_setattr: return PROXY_SC_file_setattr;
#endif
#ifdef SYS_finit_module
    case SYS_finit_module: return PROXY_SC_finit_module;
#endif
#ifdef SYS_flistxattr
    case SYS_flistxattr: return PROXY_SC_flistxattr;
#endif
#ifdef SYS_flock
    case SYS_flock: return PROXY_SC_flock;
#endif
#ifdef SYS_fork
    case SYS_fork: return PROXY_SC_fork;
#endif
#ifdef SYS_fremovexattr
    case SYS_fremovexattr: return PROXY_SC_fremovexattr;
#endif
#ifdef SYS_fsconfig
    case SYS_fsconfig: return PROXY_SC_fsconfig;
#endif
#ifdef SYS_fsetxattr
    case SYS_fsetxattr: return PROXY_SC_fsetxattr;
#endif
#ifdef SYS_fsmount
    case SYS_fsmount: return PROXY_SC_fsmount;
#endif
#ifdef SYS_fsopen
    case SYS_fsopen: return PROXY_SC_fsopen;
#endif
#ifdef SYS_fspick
    case SYS_fspick: return PROXY_SC_fspick;
#endif
#ifdef SYS_fstat
    case SYS_fstat: return PROXY_SC_fstat;
#endif
#ifdef SYS_fstatfs
    case SYS_fstatfs: return PROXY_SC_fstatfs;
#endif
#ifdef SYS_fsync
    case SYS_fsync: return PROXY_SC_fsync;
#endif
#ifdef SYS_ftruncate
    case SYS_ftruncate: return PROXY_SC_ftruncate;
#endif
#ifdef SYS_futex
    case SYS_futex: return PROXY_SC_futex;
#endif
#ifdef SYS_futex_requeue
    case SYS_futex_requeue: return PROXY_SC_futex_requeue;
#endif
#ifdef SYS_futex_wait
    case SYS_futex_wait: return PROXY_SC_futex_wait;
#endif
#ifdef SYS_futex_waitv
    case SYS_futex_waitv: return PROXY_SC_futex_waitv;
#endif
#ifdef SYS_futex_wake
    case SYS_futex_wake: return PROXY_SC_futex_wake;
#endif
#ifdef SYS_futimesat
    case SYS_futimesat: return PROXY_SC_futimesat;
#endif
#ifdef SYS_get_kernel_syms
    case SYS_get_kernel_syms: return PROXY_SC_get_kernel_syms;
#endif
#ifdef SYS_get_mempolicy
    case SYS_get_mempolicy: return PROXY_SC_get_mempolicy;
#endif
#ifdef SYS_get_robust_list
    case SYS_get_robust_list: return PROXY_SC_get_robust_list;
#endif
#ifdef SYS_get_thread_area
    case SYS_get_thread_area: return PROXY_SC_get_thread_area;
#endif
#ifdef SYS_getcpu
    case SYS_getcpu: return PROXY_SC_getcpu;
#endif
#ifdef SYS_getcwd
    case SYS_getcwd: return PROXY_SC_getcwd;
#endif
#ifdef SYS_getdents
    case SYS_getdents: return PROXY_SC_getdents;
#endif
#ifdef SYS_getdents64
    case SYS_getdents64: return PROXY_SC_getdents64;
#endif
#ifdef SYS_getegid
    case SYS_getegid: return PROXY_SC_getegid;
#endif
#ifdef SYS_geteuid
    case SYS_geteuid: return PROXY_SC_geteuid;
#endif
#ifdef SYS_getgid
    case SYS_getgid: return PROXY_SC_getgid;
#endif
#ifdef SYS_getgroups
    case SYS_getgroups: return PROXY_SC_getgroups;
#endif
#ifdef SYS_getitimer
    case SYS_getitimer: return PROXY_SC_getitimer;
#endif
#ifdef SYS_getpeername
    case SYS_getpeername: return PROXY_SC_getpeername;
#endif
#ifdef SYS_getpgid
    case SYS_getpgid: return PROXY_SC_getpgid;
#endif
#ifdef SYS_getpgrp
    case SYS_getpgrp: return PROXY_SC_getpgrp;
#endif
#ifdef SYS_getpid
    case SYS_getpid: return PROXY_SC_getpid;
#endif
#ifdef SYS_getpmsg
    case SYS_getpmsg: return PROXY_SC_getpmsg;
#endif
#ifdef SYS_getppid
    case SYS_getppid: return PROXY_SC_getppid;
#endif
#ifdef SYS_getpriority
    case SYS_getpriority: return PROXY_SC_getpriority;
#endif
#ifdef SYS_getrandom
    case SYS_getrandom: return PROXY_SC_getrandom;
#endif
#ifdef SYS_getresgid
    case SYS_getresgid: return PROXY_SC_getresgid;
#endif
#ifdef SYS_getresuid
    case SYS_getresuid: return PROXY_SC_getresuid;
#endif
#ifdef SYS_getrlimit
    case SYS_getrlimit: return PROXY_SC_getrlimit;
#endif
#ifdef SYS_getrusage
    case SYS_getrusage: return PROXY_SC_getrusage;
#endif
#ifdef SYS_getsid
    case SYS_getsid: return PROXY_SC_getsid;
#endif
#ifdef SYS_getsockname
    case SYS_getsockname: return PROXY_SC_getsockname;
#endif
#ifdef SYS_getsockopt
    case SYS_getsockopt: return PROXY_SC_getsockopt;
#endif
#ifdef SYS_gettid
    case SYS_gettid: return PROXY_SC_gettid;
#endif
#ifdef SYS_gettimeofday
    case SYS_gettimeofday: return PROXY_SC_gettimeofday;
#endif
#ifdef SYS_getuid
    case SYS_getuid: return PROXY_SC_getuid;
#endif
#ifdef SYS_getxattr
    case SYS_getxattr: return PROXY_SC_getxattr;
#endif
#ifdef SYS_getxattrat
    case SYS_getxattrat: return PROXY_SC_getxattrat;
#endif
#ifdef SYS_init_module
    case SYS_init_module: return PROXY_SC_init_module;
#endif
#ifdef SYS_inotify_add_watch
    case SYS_inotify_add_watch: return PROXY_SC_inotify_add_watch;
#endif
#ifdef SYS_inotify_init
    case SYS_inotify_init: return PROXY_SC_inotify_init;
#endif
#ifdef SYS_inotify_init1
    case SYS_inotify_init1: return PROXY_SC_inotify_init1;
#endif
#ifdef SYS_inotify_rm_watch
    case SYS_inotify_rm_watch: return PROXY_SC_inotify_rm_watch;
#endif
#ifdef SYS_io_cancel
    case SYS_io_cancel: return PROXY_SC_io_cancel;
#endif
#ifdef SYS_io_destroy
    case SYS_io_destroy: return PROXY_SC_io_destroy;
#endif
#ifdef SYS_io_getevents
    case SYS_io_getevents: return PROXY_SC_io_getevents;
#endif
#ifdef SYS_io_pgetevents
    case SYS_io_pgetevents: return PROXY_SC_io_pgetevents;
#endif
#ifdef SYS_io_setup
    case SYS_io_setup: return PROXY_SC_io_setup;
#endif
#ifdef SYS_io_submit
    case SYS_io_submit: return PROXY_SC_io_submit;
#endif
#ifdef SYS_io_uring_enter
    case SYS_io_uring_enter: return PROXY_SC_io_uring_enter;
#endif
#ifdef SYS_io_uring_register
    case SYS_io_uring_register: return PROXY_SC_io_uring_register;
#endif
#ifdef SYS_io_uring_setup
    case SYS_io_uring_setup: return PROXY_SC_io_uring_setup;
#endif
#ifdef SYS_ioctl
    case SYS_ioctl: return PROXY_SC_ioctl;
#endif
#ifdef SYS_ioperm
    case SYS_ioperm: return PROXY_SC_ioperm;
#endif
#ifdef SYS_iopl
    case SYS_iopl: return PROXY_SC_iopl;
#endif
#ifdef SYS_ioprio_get
    case SYS_ioprio_get: return PROXY_SC_ioprio_get;
#endif
#ifdef SYS_ioprio_set
    case SYS_ioprio_set: return PROXY_SC_ioprio_set;
#endif
#ifdef SYS_kcmp
    case SYS_kcmp: return PROXY_SC_kcmp;
#endif
#ifdef SYS_kexec_file_load
    case SYS_kexec_file_load: return PROXY_SC_kexec_file_load;
#endif
#ifdef SYS_kexec_load
    case SYS_kexec_load: return PROXY_SC_kexec_load;
#endif
#ifdef SYS_keyctl
    case SYS_keyctl: return PROXY_SC_keyctl;
#endif
#ifdef SYS_kill
    case SYS_kill: return PROXY_SC_kill;
#endif
#ifdef SYS_landlock_add_rule
    case SYS_landlock_add_rule: return PROXY_SC_landlock_add_rule;
#endif
#ifdef SYS_landlock_create_ruleset
    case SYS_landlock_create_ruleset: return PROXY_SC_landlock_create_ruleset;
#endif
#ifdef SYS_landlock_restrict_self
    case SYS_landlock_restrict_self: return PROXY_SC_landlock_restrict_self;
#endif
#ifdef SYS_lchown
    case SYS_lchown: return PROXY_SC_lchown;
#endif
#ifdef SYS_lgetxattr
    case SYS_lgetxattr: return PROXY_SC_lgetxattr;
#endif
#ifdef SYS_link
    case SYS_link: return PROXY_SC_link;
#endif
#ifdef SYS_linkat
    case SYS_linkat: return PROXY_SC_linkat;
#endif
#ifdef SYS_listen
    case SYS_listen: return PROXY_SC_listen;
#endif
#ifdef SYS_listmount
    case SYS_listmount: return PROXY_SC_listmount;
#endif
#ifdef SYS_listns
    case SYS_listns: return PROXY_SC_listns;
#endif
#ifdef SYS_listxattr
    case SYS_listxattr: return PROXY_SC_listxattr;
#endif
#ifdef SYS_listxattrat
    case SYS_listxattrat: return PROXY_SC_listxattrat;
#endif
#ifdef SYS_llistxattr
    case SYS_llistxattr: return PROXY_SC_llistxattr;
#endif
#ifdef SYS_lookup_dcookie
    case SYS_lookup_dcookie: return PROXY_SC_lookup_dcookie;
#endif
#ifdef SYS_lremovexattr
    case SYS_lremovexattr: return PROXY_SC_lremovexattr;
#endif
#ifdef SYS_lseek
    case SYS_lseek: return PROXY_SC_lseek;
#endif
#ifdef SYS_lsetxattr
    case SYS_lsetxattr: return PROXY_SC_lsetxattr;
#endif
#ifdef SYS_lsm_get_self_attr
    case SYS_lsm_get_self_attr: return PROXY_SC_lsm_get_self_attr;
#endif
#ifdef SYS_lsm_list_modules
    case SYS_lsm_list_modules: return PROXY_SC_lsm_list_modules;
#endif
#ifdef SYS_lsm_set_self_attr
    case SYS_lsm_set_self_attr: return PROXY_SC_lsm_set_self_attr;
#endif
#ifdef SYS_lstat
    case SYS_lstat: return PROXY_SC_lstat;
#endif
#ifdef SYS_madvise
    case SYS_madvise: return PROXY_SC_madvise;
#endif
#ifdef SYS_map_shadow_stack
    case SYS_map_shadow_stack: return PROXY_SC_map_shadow_stack;
#endif
#ifdef SYS_mbind
    case SYS_mbind: return PROXY_SC_mbind;
#endif
#ifdef SYS_membarrier
    case SYS_membarrier: return PROXY_SC_membarrier;
#endif
#ifdef SYS_memfd_create
    case SYS_memfd_create: return PROXY_SC_memfd_create;
#endif
#ifdef SYS_memfd_secret
    case SYS_memfd_secret: return PROXY_SC_memfd_secret;
#endif
#ifdef SYS_migrate_pages
    case SYS_migrate_pages: return PROXY_SC_migrate_pages;
#endif
#ifdef SYS_mincore
    case SYS_mincore: return PROXY_SC_mincore;
#endif
#ifdef SYS_mkdir
    case SYS_mkdir: return PROXY_SC_mkdir;
#endif
#ifdef SYS_mkdirat
    case SYS_mkdirat: return PROXY_SC_mkdirat;
#endif
#ifdef SYS_mknod
    case SYS_mknod: return PROXY_SC_mknod;
#endif
#ifdef SYS_mknodat
    case SYS_mknodat: return PROXY_SC_mknodat;
#endif
#ifdef SYS_mlock
    case SYS_mlock: return PROXY_SC_mlock;
#endif
#ifdef SYS_mlock2
    case SYS_mlock2: return PROXY_SC_mlock2;
#endif
#ifdef SYS_mlockall
    case SYS_mlockall: return PROXY_SC_mlockall;
#endif
#ifdef SYS_mmap
    case SYS_mmap: return PROXY_SC_mmap;
#endif
#ifdef SYS_modify_ldt
    case SYS_modify_ldt: return PROXY_SC_modify_ldt;
#endif
#ifdef SYS_mount
    case SYS_mount: return PROXY_SC_mount;
#endif
#ifdef SYS_mount_setattr
    case SYS_mount_setattr: return PROXY_SC_mount_setattr;
#endif
#ifdef SYS_move_mount
    case SYS_move_mount: return PROXY_SC_move_mount;
#endif
#ifdef SYS_move_pages
    case SYS_move_pages: return PROXY_SC_move_pages;
#endif
#ifdef SYS_mprotect
    case SYS_mprotect: return PROXY_SC_mprotect;
#endif
#ifdef SYS_mq_getsetattr
    case SYS_mq_getsetattr: return PROXY_SC_mq_getsetattr;
#endif
#ifdef SYS_mq_notify
    case SYS_mq_notify: return PROXY_SC_mq_notify;
#endif
#ifdef SYS_mq_open
    case SYS_mq_open: return PROXY_SC_mq_open;
#endif
#ifdef SYS_mq_timedreceive
    case SYS_mq_timedreceive: return PROXY_SC_mq_timedreceive;
#endif
#ifdef SYS_mq_timedsend
    case SYS_mq_timedsend: return PROXY_SC_mq_timedsend;
#endif
#ifdef SYS_mq_unlink
    case SYS_mq_unlink: return PROXY_SC_mq_unlink;
#endif
#ifdef SYS_mremap
    case SYS_mremap: return PROXY_SC_mremap;
#endif
#ifdef SYS_mseal
    case SYS_mseal: return PROXY_SC_mseal;
#endif
#ifdef SYS_msgctl
    case SYS_msgctl: return PROXY_SC_msgctl;
#endif
#ifdef SYS_msgget
    case SYS_msgget: return PROXY_SC_msgget;
#endif
#ifdef SYS_msgrcv
    case SYS_msgrcv: return PROXY_SC_msgrcv;
#endif
#ifdef SYS_msgsnd
    case SYS_msgsnd: return PROXY_SC_msgsnd;
#endif
#ifdef SYS_msync
    case SYS_msync: return PROXY_SC_msync;
#endif
#ifdef SYS_munlock
    case SYS_munlock: return PROXY_SC_munlock;
#endif
#ifdef SYS_munlockall
    case SYS_munlockall: return PROXY_SC_munlockall;
#endif
#ifdef SYS_munmap
    case SYS_munmap: return PROXY_SC_munmap;
#endif
#ifdef SYS_name_to_handle_at
    case SYS_name_to_handle_at: return PROXY_SC_name_to_handle_at;
#endif
#ifdef SYS_nanosleep
    case SYS_nanosleep: return PROXY_SC_nanosleep;
#endif
#ifdef SYS_newfstatat
    case SYS_newfstatat: return PROXY_SC_newfstatat;
#endif
#ifdef SYS_nfsservctl
    case SYS_nfsservctl: return PROXY_SC_nfsservctl;
#endif
#ifdef SYS_open
    case SYS_open: return PROXY_SC_open;
#endif
#ifdef SYS_open_by_handle_at
    case SYS_open_by_handle_at: return PROXY_SC_open_by_handle_at;
#endif
#ifdef SYS_open_tree
    case SYS_open_tree: return PROXY_SC_open_tree;
#endif
#ifdef SYS_open_tree_attr
    case SYS_open_tree_attr: return PROXY_SC_open_tree_attr;
#endif
#ifdef SYS_openat
    case SYS_openat: return PROXY_SC_openat;
#endif
#ifdef SYS_openat2
    case SYS_openat2: return PROXY_SC_openat2;
#endif
#ifdef SYS_pause
    case SYS_pause: return PROXY_SC_pause;
#endif
#ifdef SYS_perf_event_open
    case SYS_perf_event_open: return PROXY_SC_perf_event_open;
#endif
#ifdef SYS_personality
    case SYS_personality: return PROXY_SC_personality;
#endif
#ifdef SYS_pidfd_getfd
    case SYS_pidfd_getfd: return PROXY_SC_pidfd_getfd;
#endif
#ifdef SYS_pidfd_open
    case SYS_pidfd_open: return PROXY_SC_pidfd_open;
#endif
#ifdef SYS_pidfd_send_signal
    case SYS_pidfd_send_signal: return PROXY_SC_pidfd_send_signal;
#endif
#ifdef SYS_pipe
    case SYS_pipe: return PROXY_SC_pipe;
#endif
#ifdef SYS_pipe2
    case SYS_pipe2: return PROXY_SC_pipe2;
#endif
#ifdef SYS_pivot_root
    case SYS_pivot_root: return PROXY_SC_pivot_root;
#endif
#ifdef SYS_pkey_alloc
    case SYS_pkey_alloc: return PROXY_SC_pkey_alloc;
#endif
#ifdef SYS_pkey_free
    case SYS_pkey_free: return PROXY_SC_pkey_free;
#endif
#ifdef SYS_pkey_mprotect
    case SYS_pkey_mprotect: return PROXY_SC_pkey_mprotect;
#endif
#ifdef SYS_poll
    case SYS_poll: return PROXY_SC_poll;
#endif
#ifdef SYS_ppoll
    case SYS_ppoll: return PROXY_SC_ppoll;
#endif
#ifdef SYS_prctl
    case SYS_prctl: return PROXY_SC_prctl;
#endif
#ifdef SYS_pread64
    case SYS_pread64: return PROXY_SC_pread64;
#endif
#ifdef SYS_preadv
    case SYS_preadv: return PROXY_SC_preadv;
#endif
#ifdef SYS_preadv2
    case SYS_preadv2: return PROXY_SC_preadv2;
#endif
#ifdef SYS_prlimit64
    case SYS_prlimit64: return PROXY_SC_prlimit64;
#endif
#ifdef SYS_process_madvise
    case SYS_process_madvise: return PROXY_SC_process_madvise;
#endif
#ifdef SYS_process_mrelease
    case SYS_process_mrelease: return PROXY_SC_process_mrelease;
#endif
#ifdef SYS_process_vm_readv
    case SYS_process_vm_readv: return PROXY_SC_process_vm_readv;
#endif
#ifdef SYS_process_vm_writev
    case SYS_process_vm_writev: return PROXY_SC_process_vm_writev;
#endif
#ifdef SYS_pselect6
    case SYS_pselect6: return PROXY_SC_pselect6;
#endif
#ifdef SYS_ptrace
    case SYS_ptrace: return PROXY_SC_ptrace;
#endif
#ifdef SYS_putpmsg
    case SYS_putpmsg: return PROXY_SC_putpmsg;
#endif
#ifdef SYS_pwrite64
    case SYS_pwrite64: return PROXY_SC_pwrite64;
#endif
#ifdef SYS_pwritev
    case SYS_pwritev: return PROXY_SC_pwritev;
#endif
#ifdef SYS_pwritev2
    case SYS_pwritev2: return PROXY_SC_pwritev2;
#endif
#ifdef SYS_query_module
    case SYS_query_module: return PROXY_SC_query_module;
#endif
#ifdef SYS_quotactl
    case SYS_quotactl: return PROXY_SC_quotactl;
#endif
#ifdef SYS_quotactl_fd
    case SYS_quotactl_fd: return PROXY_SC_quotactl_fd;
#endif
#ifdef SYS_read
    case SYS_read: return PROXY_SC_read;
#endif
#ifdef SYS_readahead
    case SYS_readahead: return PROXY_SC_readahead;
#endif
#ifdef SYS_readlink
    case SYS_readlink: return PROXY_SC_readlink;
#endif
#ifdef SYS_readlinkat
    case SYS_readlinkat: return PROXY_SC_readlinkat;
#endif
#ifdef SYS_readv
    case SYS_readv: return PROXY_SC_readv;
#endif
#ifdef SYS_reboot
    case SYS_reboot: return PROXY_SC_reboot;
#endif
#ifdef SYS_recvfrom
    case SYS_recvfrom: return PROXY_SC_recvfrom;
#endif
#ifdef SYS_recvmmsg
    case SYS_recvmmsg: return PROXY_SC_recvmmsg;
#endif
#ifdef SYS_recvmsg
    case SYS_recvmsg: return PROXY_SC_recvmsg;
#endif
#ifdef SYS_remap_file_pages
    case SYS_remap_file_pages: return PROXY_SC_remap_file_pages;
#endif
#ifdef SYS_removexattr
    case SYS_removexattr: return PROXY_SC_removexattr;
#endif
#ifdef SYS_removexattrat
    case SYS_removexattrat: return PROXY_SC_removexattrat;
#endif
#ifdef SYS_rename
    case SYS_rename: return PROXY_SC_rename;
#endif
#ifdef SYS_renameat
    case SYS_renameat: return PROXY_SC_renameat;
#endif
#ifdef SYS_renameat2
    case SYS_renameat2: return PROXY_SC_renameat2;
#endif
#ifdef SYS_request_key
    case SYS_request_key: return PROXY_SC_request_key;
#endif
#ifdef SYS_restart_syscall
    case SYS_restart_syscall: return PROXY_SC_restart_syscall;
#endif
#ifdef SYS_rmdir
    case SYS_rmdir: return PROXY_SC_rmdir;
#endif
#ifdef SYS_rseq
    case SYS_rseq: return PROXY_SC_rseq;
#endif
#ifdef SYS_rt_sigaction
    case SYS_rt_sigaction: return PROXY_SC_rt_sigaction;
#endif
#ifdef SYS_rt_sigpending
    case SYS_rt_sigpending: return PROXY_SC_rt_sigpending;
#endif
#ifdef SYS_rt_sigprocmask
    case SYS_rt_sigprocmask: return PROXY_SC_rt_sigprocmask;
#endif
#ifdef SYS_rt_sigqueueinfo
    case SYS_rt_sigqueueinfo: return PROXY_SC_rt_sigqueueinfo;
#endif
#ifdef SYS_rt_sigreturn
    case SYS_rt_sigreturn: return PROXY_SC_rt_sigreturn;
#endif
#ifdef SYS_rt_sigsuspend
    case SYS_rt_sigsuspend: return PROXY_SC_rt_sigsuspend;
#endif
#ifdef SYS_rt_sigtimedwait
    case SYS_rt_sigtimedwait: return PROXY_SC_rt_sigtimedwait;
#endif
#ifdef SYS_rt_tgsigqueueinfo
    case SYS_rt_tgsigqueueinfo: return PROXY_SC_rt_tgsigqueueinfo;
#endif
#ifdef SYS_sched_get_priority_max
    case SYS_sched_get_priority_max: return PROXY_SC_sched_get_priority_max;
#endif
#ifdef SYS_sched_get_priority_min
    case SYS_sched_get_priority_min: return PROXY_SC_sched_get_priority_min;
#endif
#ifdef SYS_sched_getaffinity
    case SYS_sched_getaffinity: return PROXY_SC_sched_getaffinity;
#endif
#ifdef SYS_sched_getattr
    case SYS_sched_getattr: return PROXY_SC_sched_getattr;
#endif
#ifdef SYS_sched_getparam
    case SYS_sched_getparam: return PROXY_SC_sched_getparam;
#endif
#ifdef SYS_sched_getscheduler
    case SYS_sched_getscheduler: return PROXY_SC_sched_getscheduler;
#endif
#ifdef SYS_sched_rr_get_interval
    case SYS_sched_rr_get_interval: return PROXY_SC_sched_rr_get_interval;
#endif
#ifdef SYS_sched_setaffinity
    case SYS_sched_setaffinity: return PROXY_SC_sched_setaffinity;
#endif
#ifdef SYS_sched_setattr
    case SYS_sched_setattr: return PROXY_SC_sched_setattr;
#endif
#ifdef SYS_sched_setparam
    case SYS_sched_setparam: return PROXY_SC_sched_setparam;
#endif
#ifdef SYS_sched_setscheduler
    case SYS_sched_setscheduler: return PROXY_SC_sched_setscheduler;
#endif
#ifdef SYS_sched_yield
    case SYS_sched_yield: return PROXY_SC_sched_yield;
#endif
#ifdef SYS_seccomp
    case SYS_seccomp: return PROXY_SC_seccomp;
#endif
#ifdef SYS_security
    case SYS_security: return PROXY_SC_security;
#endif
#ifdef SYS_select
    case SYS_select: return PROXY_SC_select;
#endif
#ifdef SYS_semctl
    case SYS_semctl: return PROXY_SC_semctl;
#endif
#ifdef SYS_semget
    case SYS_semget: return PROXY_SC_semget;
#endif
#ifdef SYS_semop
    case SYS_semop: return PROXY_SC_semop;
#endif
#ifdef SYS_semtimedop
    case SYS_semtimedop: return PROXY_SC_semtimedop;
#endif
#ifdef SYS_sendfile
    case SYS_sendfile: return PROXY_SC_sendfile;
#endif
#ifdef SYS_sendmmsg
    case SYS_sendmmsg: return PROXY_SC_sendmmsg;
#endif
#ifdef SYS_sendmsg
    case SYS_sendmsg: return PROXY_SC_sendmsg;
#endif
#ifdef SYS_sendto
    case SYS_sendto: return PROXY_SC_sendto;
#endif
#ifdef SYS_set_mempolicy
    case SYS_set_mempolicy: return PROXY_SC_set_mempolicy;
#endif
#ifdef SYS_set_mempolicy_home_node
    case SYS_set_mempolicy_home_node: return PROXY_SC_set_mempolicy_home_node;
#endif
#ifdef SYS_set_robust_list
    case SYS_set_robust_list: return PROXY_SC_set_robust_list;
#endif
#ifdef SYS_set_thread_area
    case SYS_set_thread_area: return PROXY_SC_set_thread_area;
#endif
#ifdef SYS_set_tid_address
    case SYS_set_tid_address: return PROXY_SC_set_tid_address;
#endif
#ifdef SYS_setdomainname
    case SYS_setdomainname: return PROXY_SC_setdomainname;
#endif
#ifdef SYS_setfsgid
    case SYS_setfsgid: return PROXY_SC_setfsgid;
#endif
#ifdef SYS_setfsuid
    case SYS_setfsuid: return PROXY_SC_setfsuid;
#endif
#ifdef SYS_setgid
    case SYS_setgid: return PROXY_SC_setgid;
#endif
#ifdef SYS_setgroups
    case SYS_setgroups: return PROXY_SC_setgroups;
#endif
#ifdef SYS_sethostname
    case SYS_sethostname: return PROXY_SC_sethostname;
#endif
#ifdef SYS_setitimer
    case SYS_setitimer: return PROXY_SC_setitimer;
#endif
#ifdef SYS_setns
    case SYS_setns: return PROXY_SC_setns;
#endif
#ifdef SYS_setpgid
    case SYS_setpgid: return PROXY_SC_setpgid;
#endif
#ifdef SYS_setpriority
    case SYS_setpriority: return PROXY_SC_setpriority;
#endif
#ifdef SYS_setregid
    case SYS_setregid: return PROXY_SC_setregid;
#endif
#ifdef SYS_setresgid
    case SYS_setresgid: return PROXY_SC_setresgid;
#endif
#ifdef SYS_setresuid
    case SYS_setresuid: return PROXY_SC_setresuid;
#endif
#ifdef SYS_setreuid
    case SYS_setreuid: return PROXY_SC_setreuid;
#endif
#ifdef SYS_setrlimit
    case SYS_setrlimit: return PROXY_SC_setrlimit;
#endif
#ifdef SYS_setsid
    case SYS_setsid: return PROXY_SC_setsid;
#endif
#ifdef SYS_setsockopt
    case SYS_setsockopt: return PROXY_SC_setsockopt;
#endif
#ifdef SYS_settimeofday
    case SYS_settimeofday: return PROXY_SC_settimeofday;
#endif
#ifdef SYS_setuid
    case SYS_setuid: return PROXY_SC_setuid;
#endif
#ifdef SYS_setxattr
    case SYS_setxattr: return PROXY_SC_setxattr;
#endif
#ifdef SYS_setxattrat
    case SYS_setxattrat: return PROXY_SC_setxattrat;
#endif
#ifdef SYS_shmat
    case SYS_shmat: return PROXY_SC_shmat;
#endif
#ifdef SYS_shmctl
    case SYS_shmctl: return PROXY_SC_shmctl;
#endif
#ifdef SYS_shmdt
    case SYS_shmdt: return PROXY_SC_shmdt;
#endif
#ifdef SYS_shmget
    case SYS_shmget: return PROXY_SC_shmget;
#endif
#ifdef SYS_shutdown
    case SYS_shutdown: return PROXY_SC_shutdown;
#endif
#ifdef SYS_sigaltstack
    case SYS_sigaltstack: return PROXY_SC_sigaltstack;
#endif
#ifdef SYS_signalfd
    case SYS_signalfd: return PROXY_SC_signalfd;
#endif
#ifdef SYS_signalfd4
    case SYS_signalfd4: return PROXY_SC_signalfd4;
#endif
#ifdef SYS_socket
    case SYS_socket: return PROXY_SC_socket;
#endif
#ifdef SYS_socketpair
    case SYS_socketpair: return PROXY_SC_socketpair;
#endif
#ifdef SYS_splice
    case SYS_splice: return PROXY_SC_splice;
#endif
#ifdef SYS_stat
    case SYS_stat: return PROXY_SC_stat;
#endif
#ifdef SYS_statfs
    case SYS_statfs: return PROXY_SC_statfs;
#endif
#ifdef SYS_statmount
    case SYS_statmount: return PROXY_SC_statmount;
#endif
#ifdef SYS_statx
    case SYS_statx: return PROXY_SC_statx;
#endif
#ifdef SYS_swapoff
    case SYS_swapoff: return PROXY_SC_swapoff;
#endif
#ifdef SYS_swapon
    case SYS_swapon: return PROXY_SC_swapon;
#endif
#ifdef SYS_symlink
    case SYS_symlink: return PROXY_SC_symlink;
#endif
#ifdef SYS_symlinkat
    case SYS_symlinkat: return PROXY_SC_symlinkat;
#endif
#ifdef SYS_sync
    case SYS_sync: return PROXY_SC_sync;
#endif
#ifdef SYS_sync_file_range
    case SYS_sync_file_range: return PROXY_SC_sync_file_range;
#endif
#ifdef SYS_syncfs
    case SYS_syncfs: return PROXY_SC_syncfs;
#endif
#ifdef SYS_sysfs
    case SYS_sysfs: return PROXY_SC_sysfs;
#endif
#ifdef SYS_sysinfo
    case SYS_sysinfo: return PROXY_SC_sysinfo;
#endif
#ifdef SYS_syslog
    case SYS_syslog: return PROXY_SC_syslog;
#endif
#ifdef SYS_tee
    case SYS_tee: return PROXY_SC_tee;
#endif
#ifdef SYS_tgkill
    case SYS_tgkill: return PROXY_SC_tgkill;
#endif
#ifdef SYS_time
    case SYS_time: return PROXY_SC_time;
#endif
#ifdef SYS_timer_create
    case SYS_timer_create: return PROXY_SC_timer_create;
#endif
#ifdef SYS_timer_delete
    case SYS_timer_delete: return PROXY_SC_timer_delete;
#endif
#ifdef SYS_timer_getoverrun
    case SYS_timer_getoverrun: return PROXY_SC_timer_getoverrun;
#endif
#ifdef SYS_timer_gettime
    case SYS_timer_gettime: return PROXY_SC_timer_gettime;
#endif
#ifdef SYS_timer_settime
    case SYS_timer_settime: return PROXY_SC_timer_settime;
#endif
#ifdef SYS_timerfd_create
    case SYS_timerfd_create: return PROXY_SC_timerfd_create;
#endif
#ifdef SYS_timerfd_gettime
    case SYS_timerfd_gettime: return PROXY_SC_timerfd_gettime;
#endif
#ifdef SYS_timerfd_settime
    case SYS_timerfd_settime: return PROXY_SC_timerfd_settime;
#endif
#ifdef SYS_times
    case SYS_times: return PROXY_SC_times;
#endif
#ifdef SYS_tkill
    case SYS_tkill: return PROXY_SC_tkill;
#endif
#ifdef SYS_truncate
    case SYS_truncate: return PROXY_SC_truncate;
#endif
#ifdef SYS_tuxcall
    case SYS_tuxcall: return PROXY_SC_tuxcall;
#endif
#ifdef SYS_umask
    case SYS_umask: return PROXY_SC_umask;
#endif
#ifdef SYS_umount2
    case SYS_umount2: return PROXY_SC_umount2;
#endif
#ifdef SYS_uname
    case SYS_uname: return PROXY_SC_uname;
#endif
#ifdef SYS_unlink
    case SYS_unlink: return PROXY_SC_unlink;
#endif
#ifdef SYS_unlinkat
    case SYS_unlinkat: return PROXY_SC_unlinkat;
#endif
#ifdef SYS_unshare
    case SYS_unshare: return PROXY_SC_unshare;
#endif
#ifdef SYS_uprobe
    case SYS_uprobe: return PROXY_SC_uprobe;
#endif
#ifdef SYS_uretprobe
    case SYS_uretprobe: return PROXY_SC_uretprobe;
#endif
#ifdef SYS_uselib
    case SYS_uselib: return PROXY_SC_uselib;
#endif
#ifdef SYS_userfaultfd
    case SYS_userfaultfd: return PROXY_SC_userfaultfd;
#endif
#ifdef SYS_ustat
    case SYS_ustat: return PROXY_SC_ustat;
#endif
#ifdef SYS_utime
    case SYS_utime: return PROXY_SC_utime;
#endif
#ifdef SYS_utimensat
    case SYS_utimensat: return PROXY_SC_utimensat;
#endif
#ifdef SYS_utimes
    case SYS_utimes: return PROXY_SC_utimes;
#endif
#ifdef SYS_vfork
    case SYS_vfork: return PROXY_SC_vfork;
#endif
#ifdef SYS_vhangup
    case SYS_vhangup: return PROXY_SC_vhangup;
#endif
#ifdef SYS_vmsplice
    case SYS_vmsplice: return PROXY_SC_vmsplice;
#endif
#ifdef SYS_vserver
    case SYS_vserver: return PROXY_SC_vserver;
#endif
#ifdef SYS_wait4
    case SYS_wait4: return PROXY_SC_wait4;
#endif
#ifdef SYS_waitid
    case SYS_waitid: return PROXY_SC_waitid;
#endif
#ifdef SYS_write
    case SYS_write: return PROXY_SC_write;
#endif
#ifdef SYS_writev
    case SYS_writev: return PROXY_SC_writev;
#endif
    default: return PROXY_SC_unknown;
    }
}
