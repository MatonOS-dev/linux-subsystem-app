#ifndef PROXY_H
#define PROXY_H
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <signal.h>
#include "arch/arch.h"
long proxy_raw(long, long, long, long, long, long, long);
void proxy_restore(void);
_Noreturn void proxy_enter(uintptr_t, uintptr_t);
#ifdef PROXY_RAW_COUNTER
void proxy_raw_count_one(long);
uint64_t proxy_raw_count_now(void);
uint64_t proxy_raw_count_for(long);
extern volatile int proxy_raw_counter_enabled;
void proxy_raw_count_enable(void);
#define RAW(n,a,b,c,d,e,f) (proxy_raw_count_one((long)(n)), arch_raw_call((n),(proxy_arg_t)(a),(proxy_arg_t)(b),(proxy_arg_t)(c),(proxy_arg_t)(d),(proxy_arg_t)(e),(proxy_arg_t)(f)))
#else
static inline void proxy_raw_count_one(long nr) { (void)nr; }
static inline uint64_t proxy_raw_count_now(void) { return 0; }
static inline uint64_t proxy_raw_count_for(long nr) { (void)nr;return 0; }
static inline void proxy_raw_count_enable(void) {}
#define RAW(n,a,b,c,d,e,f) arch_raw_call((n),(proxy_arg_t)(a),(proxy_arg_t)(b),(proxy_arg_t)(c),(proxy_arg_t)(d),(proxy_arg_t)(e),(proxy_arg_t)(f))
#endif
#ifdef PROXY_PROFILE
uint64_t proxy_profile_tsc(void);
void proxy_profile_raw_site(long,uintptr_t,uint64_t);
void proxy_profile_section(unsigned,uint64_t);
#define PROFILE_BEGIN() uint64_t _profile_started=proxy_profile_tsc()
#define PROFILE_END(section) proxy_profile_section((section),proxy_profile_tsc()-_profile_started)
#undef RAW
#define RAW(n,a,b,c,d,e,f) ({ uintptr_t _profile_site=(uintptr_t)__builtin_return_address(0); uint64_t _profile_t=proxy_profile_tsc(); proxy_profile_raw_site((long)(n),_profile_site,0); long _profile_result=arch_raw_call((n),(proxy_arg_t)(a),(proxy_arg_t)(b),(proxy_arg_t)(c),(proxy_arg_t)(d),(proxy_arg_t)(e),(proxy_arg_t)(f)); proxy_profile_raw_site((long)(n),_profile_site,proxy_profile_tsc()-_profile_t); _profile_result; })
#else
#define PROFILE_BEGIN() ((void)0)
#define PROFILE_END(section) ((void)(section))
#endif
#include "arch/stat.h"
/* The top of the payload's soft RLIMIT_NOFILE range holds descriptors owned by
 * proxy state (IPC and shared temporary locks). The seccomp filter lets normal
 * payload I/O reach Linux directly. */
extern long proxy_fd_reserve,proxy_fd_reserve_end;
extern int proxy_fd_reserve_ready;
void proxy_fd_reserve_init(void);
int proxy_fd_is_private(long);
long proxy_private_fd(long);
void proxy_private_fd_release(int);
long proxy_private_fd_move(long);
void state_private_fd_relocated(int,int);
void proxy_fd_number_moved(int,int);
#include "arch/statx.h"
#include "arch/socket.h"
struct image { uintptr_t entry, stack; };
struct user_desc;
void proxy_image_map(uintptr_t,uintptr_t,uintptr_t,const char *);
int proxy_load(const char *, char *const *, char *const *, struct image *);
int proxy_rewrite_init(const char *,int);
void proxy_rewrite_cache_init(const char *);
void proxy_rewrite_relocate(uintptr_t,uintptr_t,size_t);
void proxy_rewrite_mapping(int, uintptr_t, size_t, uint64_t);
int proxy_install(int);
int proxy_native_prepare(void);
int proxy_native_overlap(uint64_t,uint64_t);
void proxy_i386_tls_reservation(int);
int proxy_i386_tls_prepare_payload(void);
int proxy_tls_claim_reserved(const struct user_desc *, unsigned *);
int proxy_tls_release_reserved(void);
int proxy_resident_fds_snapshot(int after);
int proxy_resident_signals_begin(uint64_t *previous);
int proxy_resident_signals_end(uint64_t previous);
int proxy_resident_fd_owned(int fd);
int proxy_resident_reset_pending(void);
int proxy_host_command(const char *);
void proxy_host_snapshot(char *const *);
extern int proxy_diagnostic_fd;
int proxy_diagnostics_init(void);
/* Redirect Android liblog to the layer's own logd socket. */
void proxy_android_log_install(void);
/* Layer-internal writes must not deliver SIGPIPE to the payload: Chromium
 * resets dispositions in forked children, so the payload may sit at SIG_DFL
 * while a proxy channel outlives its peer. Keep the disposition/mask and any
 * already pending SIGPIPE unchanged. No libc, allocation, or large buffers:
 * this also runs on the SIGSYS stack and in resident helper threads. */
static inline long proxy_internal_write(int fd,const void *data,size_t size) {
    uint64_t bit=UINT64_C(1)<<(SIGPIPE-1),old=0,pending=0;
    long r=RAW(PROXY_SC_rt_sigprocmask,SIG_BLOCK,&bit,&old,8,0,0);
    if(r<0)return r;
    RAW(PROXY_SC_rt_sigpending,&pending,8,0,0,0,0);
    r=RAW(PROXY_SC_write,fd,data,size,0,0,0);
    if(r==-EPIPE && !(pending&bit)) { /* EPIPE: consume only our new signal. */
        struct {long sec,nsec;} zero={0,0};
        RAW(PROXY_SC_rt_sigtimedwait,&bit,0,&zero,8,0,0);
    }
    RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&old,0,8,0,0);
    return r;
}
/* Sockets take MSG_NOSIGNAL so a vanished peer only yields EPIPE. */
static inline long proxy_internal_sendto(int fd,const void *data,size_t size,
                                         unsigned flags,const void *addr,unsigned addrlen) {
    return RAW(PROXY_SC_sendto,fd,data,size,flags|MSG_NOSIGNAL,addr,addrlen);
}
static inline long proxy_internal_send(int fd,const void *data,size_t size,unsigned flags) {
    return proxy_internal_sendto(fd,data,size,flags,0,0);
}
/* Logging is the main user of the write helper. */
static inline long proxy_diagnostic_write(const void *data,size_t size) {
    return proxy_internal_write(proxy_diagnostic_fd,data,size);
}
int proxy_dns_forwarder(const char *);
int proxy_mount(const char *, const char *);
const char *proxy_path(const char *, char *, size_t);
void proxy_log(const char *, long, long);
void proxy_trace_clone_result(long, long);
extern int proxy_trace;
size_t p_len(const char *);
void p_copy(void *, const void *, size_t);
int p_equal(const char *,const char *);
void proxy_log_bwrap_args(const char *,char *const *);
void proxy_count_init(const char *);
uint64_t proxy_count_now(void);
void proxy_count_resolution(uint64_t);
void proxy_count_dump(void);
#endif
