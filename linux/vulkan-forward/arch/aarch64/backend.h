/* Compile-only slot. Runtime forwarding fails closed until adoption and the
 * TPIDR_EL0 (arm64) / tp (riscv64) syscall backend is implemented. */
#ifndef MATON_VK_BACKEND_H
#define MATON_VK_BACKEND_H
#include <stddef.h>
struct adopt_api;
struct mv_scope { unsigned unused; };
static inline long mv_open(const char *p) {(void)p;return -1;}
static inline long mv_read(long f,void *p,size_t n) {(void)f;(void)p;(void)n;return -1;}
static inline void mv_close(long f) {(void)f;}
static inline void *mv_map(size_t n) {(void)n; return 0;}
static inline void mv_unmap(void *p,size_t n) {(void)p;(void)n;}
static inline void mv_write(const char *p,size_t n) {(void)p;(void)n;}
static inline int mv_fast_tls(void) {return 0;}
static inline int switch_to_android(struct adopt_api *a,int f,struct mv_scope *s) {(void)a;(void)f;(void)s;return 0;}
static inline void switch_back(struct mv_scope *s) {(void)s;}
#endif
