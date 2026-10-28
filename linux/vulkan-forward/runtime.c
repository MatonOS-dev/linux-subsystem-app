#include "runtime.h"
#include "backend.h"
#include "../gpu-bridge-common/bootstrap.h"
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
/* Named read-only launcher mapping: no libc data/code dependency. */
static const struct maton_gpu_bootstrap *cached_bootstrap;
#if defined(__x86_64__) || defined(__i386__)
typedef long (*mv_gateway_fn)(long,long,long,long,long,long,long);
static mv_gateway_fn syscall_gateway;
long mv_sys(long n,long a,long b,long c,long d,long e,long f) {
    mv_gateway_fn gateway=__atomic_load_n(&syscall_gateway,__ATOMIC_ACQUIRE);
    return gateway?gateway(n,a,b,c,d,e,f):mv_native_sys(n,a,b,c,d,e,f);
}
#endif
static const char *bootstrap_field(const struct maton_gpu_bootstrap *p,const char *key) {
    return !strcmp(key,"MATON_GPU_BRIDGE")?p->bridge:
           !strcmp(key,"MATON_THREAD_ADOPT")?p->adopt:
           !strcmp(key,"MATON_FAST_TLS")?p->fast:
           !strcmp(key,"MATON_LINUX_PHDR")?p->phdr:
           !strcmp(key,"MATON_LINUX_PHNUM")?p->phnum:
           !strcmp(key,"MATON_GPU_EGL")?p->egl:
           !strcmp(key,"MATON_GPU_GLES")?p->gles:
           !strcmp(key,"MATON_EGL_CACHE")?p->egl_cache:0;
}
const char *mv_env(const char *key) {
    const struct maton_gpu_bootstrap *cached=__atomic_load_n(&cached_bootstrap,__ATOMIC_ACQUIRE);
    if(cached)return bootstrap_field(cached,key);
    long fd=mv_open("/proc/self/maps");if(fd<0)return 0;
    char line[4096],input[4096];size_t n=0,at=0,available=0;const char *result=0;
    for(;;) {
        /* Proc maps can be large and each read crosses the compatibility layer.
         * Keep line assembly independent of read boundaries. Do not cache a
         * miss: a resident may publish its bootstrap later. */
        if(at==available) {
            long got=mv_read(fd,input,sizeof(input));
            if(got==-4)continue; /* EINTR; no libc errno dependency. */
            if(got<=0)break;
            at=0;available=(size_t)got;
        }
        char c=input[at++];
        if(c!='\n'){if(n+1<sizeof(line))line[n++]=c;continue;}
        line[n]=0;
        const char *tag="[anon:maton-gpu-bootstrap-v2]";int found=0;
        for(size_t j=0;j<n;j++){size_t k=0;while(tag[k] && j+k<n && line[j+k]==tag[k])k++;if(!tag[k]){found=1;break;}}
        if(found){
            size_t j=0;while(line[j] && line[j]!='-')j++;line[j]=0;
            const struct maton_gpu_bootstrap *p=(void *)mv_hex(line);
            if(p && p->magic==MATON_GPU_BOOTSTRAP_MAGIC) {
#if defined(__x86_64__) || defined(__i386__)
                /* Discovery itself uses the existing syscall route. Once the
                 * named mapping is known, forwarder internals use the same
                 * exempt gateway as the resident, preserving the caller TLS. */
                __atomic_store_n(&syscall_gateway,(mv_gateway_fn)mv_hex(p->syscall),__ATOMIC_RELEASE);
#endif
                __atomic_store_n(&cached_bootstrap,p,__ATOMIC_RELEASE);
                result=bootstrap_field(p,key);
            }
            break;
        }
        n=0;
    }
    mv_close(fd);return result;
}
uintptr_t mv_hex(const char *s) {
    uintptr_t n=0;if(!s)return 0;
    for(;*s;s++){unsigned c=(unsigned char)*s;unsigned v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:16;if(v>15 || n>(UINTPTR_MAX-v)/16)return 0;n=n*16+v;}return n;
}
void mv_diagnostic(void) {static const char s[]="maton-vulkan: bridge or thread adoption unavailable\n";mv_write(s,sizeof(s)-1);}
