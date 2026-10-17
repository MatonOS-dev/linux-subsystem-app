#include "runtime.h"
#include "backend.h"
void *memcpy(void *d,const void *s,size_t n) {unsigned char *a=d;const unsigned char *b=s;while(n--)*a++=*b++;return d;}
void *memset(void *d,int c,size_t n) {unsigned char *a=d;while(n--)*a++=(unsigned char)c;return d;}
int memcmp(const void *a,const void *b,size_t n) {const unsigned char *x=a,*y=b;while(n--){if(*x!=*y)return *x-*y;x++;y++;}return 0;}
int strcmp(const char *a,const char *b) {while(*a && *a==*b){a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
/* Each allocation owns its mapping. No shared mutable allocator state, locks,
 * fixed arena, thread limit, or allocator callbacks across the TLS boundary. */
union block { size_t length; max_align_t align; };
void *mv_alloc(size_t n) {
    if(n>SIZE_MAX-sizeof(union block))return 0;
    union block *p=mv_map(n+sizeof(*p));if(!p)return 0;
    p->length=n+sizeof(*p);return p+1;
}
void mv_free(void *p) {if(p){union block *h=(union block *)p-1;mv_unmap(h,h->length);}}
/* Data-only process environment ABI provided by both glibc and musl. No libc
 * functions, TLS variables, or code are imported. Missing symbol fails closed. */
extern char **environ __attribute__((weak));
const char *mv_env(const char *key) {
    if(!&environ || !environ)return 0;
    for(char **e=environ;*e;e++) {size_t i=0;while(key[i] && key[i]==(*e)[i])i++;if(!key[i] && (*e)[i]=='=')return *e+i+1;}
    return 0;
}
uintptr_t mv_hex(const char *s) {
    uintptr_t n=0;if(!s)return 0;
    for(;*s;s++){unsigned c=(unsigned char)*s;unsigned v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:16;if(v>15 || n>(UINTPTR_MAX-v)/16)return 0;n=n*16+v;}return n;
}
void mv_diagnostic(void) {static const char s[]="maton-vulkan: bridge or thread adoption unavailable\n";mv_write(s,sizeof(s)-1);}
