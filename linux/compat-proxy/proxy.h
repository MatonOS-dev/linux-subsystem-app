#ifndef PROXY_H
#define PROXY_H
#include <stdint.h>
#include <stddef.h>
long proxy_raw(long, long, long, long, long, long, long);
void proxy_restore(void);
_Noreturn void proxy_enter(uintptr_t, uintptr_t);
#define RAW(n,a,b,c,d,e,f) proxy_raw((n),(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f))
struct image { uintptr_t entry, stack; };
int proxy_load(const char *, char *const *, char *const *, struct image *);
int proxy_install(int);
int proxy_mount(const char *, const char *);
const char *proxy_path(const char *, char *, size_t);
void proxy_log(const char *, long, long);
size_t p_len(const char *);
void p_copy(void *, const void *, size_t);
int p_equal(const char *,const char *);
void proxy_log_bwrap_args(const char *,char *const *);
#endif
