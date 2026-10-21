#ifndef PROXY_H
#define PROXY_H
#include <stdint.h>
#include <stddef.h>
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
#include "arch/stat.h"
#include "arch/statx.h"
#include "arch/socket.h"
struct image { uintptr_t entry, stack; };
void proxy_image_map(uintptr_t,uintptr_t,uintptr_t,const char *);
int proxy_load(const char *, char *const *, char *const *, struct image *);
int proxy_install(int);
int proxy_resident_fds_snapshot(int after);
int proxy_host_command(const char *);
void proxy_host_snapshot(char *const *);
extern int proxy_diagnostic_fd;
int proxy_diagnostics_init(void);
int proxy_dns_forwarder(const char *);
int proxy_dns_socket(const char *);
int proxy_mount(const char *, const char *);
const char *proxy_path(const char *, char *, size_t);
void proxy_log(const char *, long, long);
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
