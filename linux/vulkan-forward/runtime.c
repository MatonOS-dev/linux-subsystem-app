#include "runtime.h"
#include "backend.h"
#include "../gpu-forward-spike/bootstrap.h"
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
static const char *bootstrap_field(const struct maton_gpu_bootstrap *p,const char *key) {
    return !strcmp(key,"MATON_GPU_BRIDGE")?p->bridge:
           !strcmp(key,"MATON_THREAD_ADOPT")?p->adopt:
           !strcmp(key,"MATON_FAST_TLS")?p->fast:
           !strcmp(key,"MATON_LINUX_PHDR")?p->phdr:
           !strcmp(key,"MATON_LINUX_PHNUM")?p->phnum:
           !strcmp(key,"MATON_GPU_EGL")?p->egl:
           !strcmp(key,"MATON_GPU_GLES")?p->gles:0;
}
const char *mv_env(const char *key) {
    const struct maton_gpu_bootstrap *cached=__atomic_load_n(&cached_bootstrap,__ATOMIC_ACQUIRE);
    if(cached)return bootstrap_field(cached,key);
    long fd=mv_open("/proc/self/maps");if(fd<0)return 0;
    char line[4096];size_t n=0;const char *result=0;
    for(;;) {
        char c;if(mv_read(fd,&c,1)!=1)break;
        if(c!='\n'){if(n+1<sizeof(line))line[n++]=c;continue;}
        line[n]=0;
        const char *tag="[anon:maton-gpu-bootstrap-v2]";int found=0;
        for(size_t j=0;j<n;j++){size_t k=0;while(tag[k] && j+k<n && line[j+k]==tag[k])k++;if(!tag[k]){found=1;break;}}
        if(found){
            size_t j=0;while(line[j] && line[j]!='-')j++;line[j]=0;
            const struct maton_gpu_bootstrap *p=(void *)mv_hex(line);
            if(p && p->magic==MATON_GPU_BOOTSTRAP_MAGIC) {
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
