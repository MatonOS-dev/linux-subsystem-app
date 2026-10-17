#ifndef MATON_VK_RUNTIME_H
#define MATON_VK_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
void *memcpy(void *,const void *,size_t);
void *memset(void *,int,size_t);
int memcmp(const void *,const void *,size_t);
int strcmp(const char *,const char *);
void *mv_alloc(size_t);
void mv_free(void *);
const char *mv_env(const char *);
uintptr_t mv_hex(const char *);
void mv_diagnostic(void);
#endif
