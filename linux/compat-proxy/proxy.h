#ifndef PROXY_H
#define PROXY_H
#include <stdint.h>
#include <stddef.h>
#include "arch/arch.h"
long proxy_raw(long, long, long, long, long, long, long);
void proxy_restore(void);
_Noreturn void proxy_enter(uintptr_t, uintptr_t);
#define RAW(n,a,b,c,d,e,f) arch_raw_call((n),(proxy_arg_t)(a),(proxy_arg_t)(b),(proxy_arg_t)(c),(proxy_arg_t)(d),(proxy_arg_t)(e),(proxy_arg_t)(f))
#include "arch/stat.h"
#include "arch/statx.h"
#include "arch/socket.h"
struct image { uintptr_t entry, stack; };
void proxy_image_map(uintptr_t,uintptr_t,uintptr_t,const char *);
int proxy_load(const char *, char *const *, char *const *, struct image *);
int proxy_install(int);
int proxy_host_command(const char *);
void proxy_host_snapshot(char *const *);
extern int proxy_diagnostic_fd;
int proxy_diagnostics_init(void);
int proxy_dns_forwarder(const char *);
int proxy_dns_socket(const char *);
int proxy_mount(const char *, const char *);
const char *proxy_path(const char *, char *, size_t);
void proxy_log(const char *, long, long);
size_t p_len(const char *);
void p_copy(void *, const void *, size_t);
int p_equal(const char *,const char *);
void proxy_log_bwrap_args(const char *,char *const *);
#endif
