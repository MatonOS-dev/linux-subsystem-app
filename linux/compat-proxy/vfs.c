#define _GNU_SOURCE
/* Virtual pathname compatibility only: UID, seccomp and SELinux still constrain
 * every backing operation. No real mount or namespace is created. */
#include "proxy.h"
#include "vfs.h"
#include "links.h"
#include "sysvipc.h"
#include "state.h"
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <sys/stat.h>
#include <limits.h>
struct allocation {size_t size;};
struct arena_block {struct arena_block *next;size_t size,used;};
struct trap_arena {struct trap_arena *next;uintptr_t lo,hi;int active;uint64_t raw_calls,openat_calls,readlinkat_calls,newfstatat_calls,geteuid_calls,mmap_calls,munmap_calls,trap_count,raw_sum,openat_sum,readlinkat_sum,newfstatat_sum,geteuid_sum,mmap_sum,munmap_sum;uint64_t raw_by_nr[PROXY_SC_count],raw_sum_by_nr[PROXY_SC_count];struct arena_block *blocks;};
static struct trap_arena *trap_arenas;
static unsigned long fd_names_owner;
#ifdef PROXY_PROFILE
struct profile_site { uintptr_t site; long nr; uint64_t calls,cycles; };
static struct profile_site profile_sites[1024];
static volatile uint64_t profile_sections[16];
static volatile uint64_t profile_section_calls[16];
uint64_t proxy_profile_tsc(void){unsigned lo,hi;__asm__ volatile("lfence; rdtsc":"=a"(lo),"=d"(hi)::"memory");return ((uint64_t)hi<<32)|lo;}
void proxy_profile_raw_site(long nr,uintptr_t site,uint64_t cycles){if(!cycles){
#ifdef PROXY_RAW_COUNTER
proxy_raw_count_one(nr);
#endif
}size_t i=((site>>4)^(site>>13))&1023;for(size_t n=0;n<1024;n++,i=(i+1)&1023){struct profile_site *p=&profile_sites[i];if(!p->site){p->site=site;p->nr=nr;}if(p->site==site){if(cycles)__atomic_fetch_add(&p->cycles,cycles,__ATOMIC_RELAXED);else __atomic_fetch_add(&p->calls,1,__ATOMIC_RELAXED);return;}}}
void proxy_profile_section(unsigned section,uint64_t cycles){if(section<16){__atomic_fetch_add(&profile_sections[section],cycles,__ATOMIC_RELAXED);__atomic_fetch_add(&profile_section_calls[section],1,__ATOMIC_RELAXED);}}
static size_t profile_num(char *b,size_t n,uint64_t v){char d[24];size_t k=0;do{d[k++]=(char)('0'+v%10);v/=10;}while(v);while(k)b[n++]=d[--k];return n;}
void vfs_profile_dump(void){char b[128];for(size_t i=0;i<1024;i++){struct profile_site *p=&profile_sites[i];if(!p->site)continue;size_t n=0;b[n++]='S';b[n++]='I';b[n++]='T';b[n++]='E';b[n++]=' ';n=profile_num(b,n,(uint64_t)p->nr);b[n++]=' ';n=profile_num(b,n,p->site);b[n++]=' ';n=profile_num(b,n,p->calls);b[n++]=' ';n=profile_num(b,n,p->cycles);b[n++]='\n';RAW(PROXY_SC_write,2,b,n,0,0,0);}for(unsigned i=0;i<16;i++)if(profile_section_calls[i]){size_t n=0;b[n++]='S';b[n++]='E';b[n++]='C';b[n++]=' ';n=profile_num(b,n,i);b[n++]=' ';n=profile_num(b,n,profile_section_calls[i]);b[n++]=' ';n=profile_num(b,n,profile_sections[i]);b[n++]='\n';RAW(PROXY_SC_write,2,b,n,0,0,0);}}
#else
void vfs_profile_dump(void){}
#endif
volatile int proxy_raw_counter_enabled;
static struct trap_arena *arena_for_stack(uintptr_t sp){
    for(struct trap_arena *a=__atomic_load_n(&trap_arenas,__ATOMIC_ACQUIRE);a;a=a->next)
        if(a->active && sp>=a->lo && sp<a->hi)return a;
    return NULL;
}
static struct trap_arena *arena_for_range(uintptr_t lo,uintptr_t hi){
    for(struct trap_arena *a=__atomic_load_n(&trap_arenas,__ATOMIC_ACQUIRE);a;a=a->next)
        if(a->lo==lo && a->hi==hi)return a;
    return NULL;
}
static struct trap_arena *arena_for_address(uintptr_t sp){
    for(struct trap_arena *a=__atomic_load_n(&trap_arenas,__ATOMIC_ACQUIRE);a;a=a->next)
        if(sp>=a->lo && sp<a->hi)return a;
    return NULL;
}
#ifdef PROXY_RAW_COUNTER
void proxy_raw_count_one(long nr){if(!proxy_raw_counter_enabled)return;uintptr_t sp=(uintptr_t)&sp;struct trap_arena *a=arena_for_stack(sp);if(!a)return;a->raw_calls++;if(nr>=0 && nr<PROXY_SC_count)a->raw_by_nr[nr]++;switch(nr){case PROXY_SC_openat:a->openat_calls++;break;case PROXY_SC_readlinkat:a->readlinkat_calls++;break;case PROXY_SC_newfstatat:a->newfstatat_calls++;break;case PROXY_SC_geteuid:a->geteuid_calls++;break;case PROXY_SC_mmap:a->mmap_calls++;break;case PROXY_SC_munmap:a->munmap_calls++;break;}}
void proxy_raw_count_enable(void){proxy_raw_counter_enabled=1;}
uint64_t proxy_raw_count_now(void){if(!proxy_raw_counter_enabled)return 0;uintptr_t sp=(uintptr_t)&sp;struct trap_arena *a=arena_for_stack(sp);return a?a->raw_calls:0;}
uint64_t proxy_raw_count_for(long nr){if(!proxy_raw_counter_enabled)return 0;uintptr_t sp=(uintptr_t)&sp;struct trap_arena *a=arena_for_stack(sp);if(!a)return 0;switch(nr){case PROXY_SC_openat:return a->openat_calls;case PROXY_SC_readlinkat:return a->readlinkat_calls;case PROXY_SC_newfstatat:return a->newfstatat_calls;case PROXY_SC_geteuid:return a->geteuid_calls;case PROXY_SC_mmap:return a->mmap_calls;case PROXY_SC_munmap:return a->munmap_calls;default:return 0;}}
#endif
/* Trap-arena memory is only freed by the thread that allocated it, inside the
 * same trap, so only the caller's own arena is searched: walking other threads'
 * blocks would race with vfs_trap_thread_exit unmapping them. */
static int arena_contains(uintptr_t p){
    uintptr_t sp=(uintptr_t)&sp;struct trap_arena *a=arena_for_address(sp);if(!a)return 0;
    for(struct arena_block *b=a->blocks;b;b=b->next){uintptr_t lo=(uintptr_t)(b+1);if(p>=lo && p<lo+b->size-sizeof(*b))return 1;}
    return 0;
}
void vfs_trap_begin(void *base,size_t size){
    uintptr_t lo=(uintptr_t)base,hi;
    if(!base || !size || size>UINTPTR_MAX-lo)return;
    hi=lo+size;
    struct trap_arena *a=arena_for_range(lo,hi);
    if(!a){for(a=__atomic_load_n(&trap_arenas,__ATOMIC_ACQUIRE);a;a=a->next)if(!a->active&&!a->lo&&!a->hi)break;
      if(!a){long p=RAW(PROXY_SC_mmap,0,sizeof(*a),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if((unsigned long)p>=(unsigned long)-4095)return;
        a=(void *)p;*a=(struct trap_arena){.lo=lo,.hi=hi};
        struct trap_arena *head;
        do{head=__atomic_load_n(&trap_arenas,__ATOMIC_ACQUIRE);a->next=head;}
        while(!__atomic_compare_exchange_n(&trap_arenas,&head,a,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED));
      } else {a->lo=lo;a->hi=hi;}
    }
    if(!a->active){a->lo=lo;a->hi=hi;a->active=1;
        a->raw_calls=a->openat_calls=a->readlinkat_calls=a->newfstatat_calls=a->geteuid_calls=a->mmap_calls=a->munmap_calls=0;
        for(unsigned i=0;i<PROXY_SC_count;i++)a->raw_by_nr[i]=0;
        for(struct arena_block *b=a->blocks;b;b=b->next)b->used=0;
    }
}
void vfs_trap_thread_exit(void *base,size_t size){
 uintptr_t lo=(uintptr_t)base,hi=lo+size;if(!base||!size||hi<lo)return;
 struct trap_arena *a=arena_for_range(lo,hi);if(!a)return;
 a->active=0;
 struct arena_block *b=a->blocks;a->blocks=NULL;
 while(b){struct arena_block *next=b->next;RAW(PROXY_SC_munmap,b,sizeof(*b)+b->size,0,0,0,0);b=next;}
 a->lo=a->hi=0;
}
void vfs_trap_resume(void){uintptr_t sp=(uintptr_t)&sp;struct trap_arena *a=arena_for_address(sp);if(!a)return;a->active=1;a->raw_calls=a->openat_calls=a->readlinkat_calls=a->newfstatat_calls=a->geteuid_calls=a->mmap_calls=a->munmap_calls=0;for(unsigned i=0;i<PROXY_SC_count;i++)a->raw_by_nr[i]=0;for(struct arena_block *b=a->blocks;b;b=b->next)b->used=0;}
void vfs_trap_end(void){
    uintptr_t sp=(uintptr_t)&sp;struct trap_arena *a=arena_for_stack(sp);if(!a)return;
    a->trap_count++;a->raw_sum+=a->raw_calls;a->openat_sum+=a->openat_calls;a->readlinkat_sum+=a->readlinkat_calls;a->newfstatat_sum+=a->newfstatat_calls;a->geteuid_sum+=a->geteuid_calls;a->mmap_sum+=a->mmap_calls;a->munmap_sum+=a->munmap_calls;for(unsigned i=0;i<PROXY_SC_count;i++)a->raw_sum_by_nr[i]+=a->raw_by_nr[i];
    for(struct arena_block *b=a->blocks;b;b=b->next)b->used=0;
    a->active=0;
}
void vfs_raw_count_summary(uint64_t *traps,uint64_t *calls){uint64_t t=0,c=0;for(struct trap_arena *a=__atomic_load_n(&trap_arenas,__ATOMIC_ACQUIRE);a;a=a->next){t+=a->trap_count;c+=a->raw_sum;}if(traps)*traps=t;if(calls)*calls=c;}
void vfs_raw_category_summary(uint64_t out[6]){uint64_t v[6]={0};for(struct trap_arena *a=__atomic_load_n(&trap_arenas,__ATOMIC_ACQUIRE);a;a=a->next){v[0]+=a->openat_sum;v[1]+=a->readlinkat_sum;v[2]+=a->newfstatat_sum;v[3]+=a->geteuid_sum;v[4]+=a->mmap_sum;v[5]+=a->munmap_sum;}for(unsigned i=0;i<6;i++)out[i]=v[i];}
void vfs_raw_syscall_summary(uint64_t out[PROXY_SC_count]){for(unsigned i=0;i<PROXY_SC_count;i++)out[i]=0;for(struct trap_arena *a=__atomic_load_n(&trap_arenas,__ATOMIC_ACQUIRE);a;a=a->next)for(unsigned i=0;i<PROXY_SC_count;i++)out[i]+=a->raw_sum_by_nr[i];}
void vfs_raw_count_fork_child(void){__atomic_store_n(&fd_names_owner,0,__ATOMIC_RELEASE);for(struct trap_arena *a=__atomic_load_n(&trap_arenas,__ATOMIC_ACQUIRE);a;a=a->next){a->trap_count=a->raw_sum=a->openat_sum=a->readlinkat_sum=a->newfstatat_sum=a->geteuid_sum=a->mmap_sum=a->munmap_sum=0;a->raw_calls=a->openat_calls=a->readlinkat_calls=a->newfstatat_calls=a->geteuid_calls=a->mmap_calls=a->munmap_calls=0;for(unsigned i=0;i<PROXY_SC_count;i++)a->raw_by_nr[i]=a->raw_sum_by_nr[i]=0;}}
void *vfs_palloc(size_t n) {
    if(n>SIZE_MAX-sizeof(struct allocation)-4095) return NULL;
    size_t bytes=(n+sizeof(struct allocation)+4095)&~(size_t)4095;
    long p=RAW(PROXY_SC_mmap,0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if((unsigned long)p>=(unsigned long)-4095) return NULL;
    struct allocation *a=(void *)p;a->size=bytes;return a+1;
}
void *vfs_alloc(size_t n){
    uintptr_t sp=(uintptr_t)&sp;struct trap_arena *arena=arena_for_stack(sp);if(!arena)return vfs_palloc(n);
    if(n>SIZE_MAX-sizeof(struct allocation)-15)return NULL;
    size_t need=(n+sizeof(struct allocation)+15)&~(size_t)15;
    struct arena_block *b=arena->blocks;
    while(b && b->size-b->used<need)b=b->next;
    if(!b){size_t bytes=65536;if(need>bytes)bytes=(need+4095)&~(size_t)4095;if(bytes>SIZE_MAX-sizeof(*b))return NULL;
        long p=RAW(PROXY_SC_mmap,0,bytes+sizeof(*b),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if((unsigned long)p>=(unsigned long)-4095)return NULL;
        b=(void *)p;b->next=arena->blocks;b->size=bytes;b->used=0;arena->blocks=b;
    }
    struct allocation *a=(void *)((char *)(b+1)+b->used);a->size=need;b->used+=need;
    /* mmap-backed vfs_alloc historically returned zero-filled storage. Keep
     * that contract for structures which rely on implicit null fields. */
    unsigned char *out=(unsigned char *)(a+1);for(size_t i=0;i<n;i++)out[i]=0;
    return out;
}
void vfs_free(void *p) {if(p){if(arena_contains((uintptr_t)p))return;struct allocation *a=(struct allocation *)p-1;RAW(PROXY_SC_munmap,a,a->size,0,0,0,0);}}
static char *join(const char *a,const char *b) {
    size_t x=p_len(a),y=p_len(b);if(x>SIZE_MAX-y-2) return NULL;
    char *s=vfs_alloc(x+y+2);if(!s)return NULL;
    p_copy(s,a,x);if(x && s[x-1]=='/' && b[0]=='/')x--;
    else if(y && x && s[x-1]!='/' && b[0]!='/')s[x++]='/';
    p_copy(s+x,b,y+1);return s;
}
/* Directory flocks survive fork and resident exec. Reserve their descriptors
 * from payload close/dup/fcntl so bwrap's fd sweep cannot release ownership. */
#include <sys/file.h>
struct temp_lock { int fd; struct temp_lock *next; };
static struct temp_lock *temp_locks;
int proxy_diagnostic_fd=2;
int vfs_private_fd(int fd){if(pack_private(fd)||state_private_fd(fd))return 1;if(ipc_private_fd(fd))return 1;if(proxy_diagnostic_fd!=2 && fd==proxy_diagnostic_fd)return 1;for(struct temp_lock *l=temp_locks;l;l=l->next)if(l->fd==fd)return 1;return 0;}
static int temp_parent(void){extern const char *proxy_tmpdir;return (int)RAW(PROXY_SC_openat,AT_FDCWD,proxy_tmpdir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);}
static int temp_own(const char *path){
    int fd=(int)RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW,0,0,0);if(fd<0)return fd;
    struct temp_lock *l=vfs_palloc(sizeof(*l));if(!l){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -ENOMEM;}
    long e=RAW(PROXY_SC_flock,fd,LOCK_EX|LOCK_NB,0,0,0,0);
    if(e){vfs_free(l);RAW(PROXY_SC_close,fd,0,0,0,0,0);return (int)e;}
    /* Distinguish new locked directories from legacy PID-only directories. */
    long marker=RAW(PROXY_SC_openat,fd,".v3-owner",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600,0,0);
    if(marker<0){vfs_free(l);RAW(PROXY_SC_close,fd,0,0,0,0,0);return (int)marker;}
    RAW(PROXY_SC_close,marker,0,0,0,0,0);l->fd=fd;l->next=temp_locks;temp_locks=l;return 0;
}
static int temp_remove_contents(int fd,unsigned depth){
    if(depth>64)return 0;
    char buf[4096];long n;
    while((n=RAW(PROXY_SC_getdents64,fd,buf,sizeof(buf),0,0,0))>0){
        for(long off=0;off<n;){unsigned short len=*(unsigned short *)(buf+off+16);if(len<20||off+len>n)return 0;
            char *name=buf+off+19;off+=len;if(p_equal(name,".")||p_equal(name,".."))continue;
            long child=RAW(PROXY_SC_openat,fd,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);
            if(child>=0){int ok=temp_remove_contents((int)child,depth+1);RAW(PROXY_SC_close,child,0,0,0,0,0);if(!ok)return 0;RAW(PROXY_SC_unlinkat,fd,name,AT_REMOVEDIR,0,0,0);}
            else RAW(PROXY_SC_unlinkat,fd,name,0,0,0,0);
        }
    }return n==0;
}
void vfs_temp_gc(void){
    int parent=temp_parent();if(parent<0)return;
    if(RAW(PROXY_SC_flock,parent,LOCK_EX,0,0,0,0)<0){RAW(PROXY_SC_close,parent,0,0,0,0,0);return;}
    char buf[4096];long n;
    while((n=RAW(PROXY_SC_getdents64,parent,buf,sizeof(buf),0,0,0))>0){
        for(long off=0;off<n;){unsigned short len=*(unsigned short *)(buf+off+16);if(len<20||off+len>n)break;char *name=buf+off+19;off+=len;
            const char *tag="compat-proxy-";unsigned k=0;while(tag[k]&&name[k]==tag[k])k++;if(tag[k])continue;
            long pid=0;unsigned start=k;for(;name[k]>='0'&&name[k]<='9'&&pid<2147483647;k++)pid=pid*10+name[k]-'0';
            if(k==start||name[k]!='-'||pid<=0||pid>2147483647)continue;
            int dir=(int)RAW(PROXY_SC_openat,parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(dir<0)continue;
            if(RAW(PROXY_SC_flock,dir,LOCK_EX|LOCK_NB,0,0,0,0)<0){RAW(PROXY_SC_close,dir,0,0,0,0,0);continue;}
            long marker=RAW(PROXY_SC_openat,dir,".v3-owner",O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);
            int stale=marker>=0;
            if(marker>=0)RAW(PROXY_SC_close,marker,0,0,0,0,0);
            else if(marker==-ENOENT){
                /* Legacy names have no birth token. Keep them if this PID
                 * exists (including reuse); only ENOENT proves them stale. */
                char proc[40]="/proc/",digits[24];unsigned count=0;unsigned long value=(unsigned long)pid;
                do{digits[count++]=(char)('0'+value%10);value/=10;}while(value);
                unsigned p=6;while(count)proc[p++]=digits[--count];proc[p]=0;
                long live=RAW(PROXY_SC_openat,AT_FDCWD,proc,O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,0,0);
                stale=live==-ENOENT;if(live>=0)RAW(PROXY_SC_close,live,0,0,0,0,0);
            }
            if(stale&&temp_remove_contents(dir,0))RAW(PROXY_SC_unlinkat,parent,name,AT_REMOVEDIR,0,0,0);
            RAW(PROXY_SC_close,dir,0,0,0,0,0);
        }
    }
    RAW(PROXY_SC_close,parent,0,0,0,0,0);
}

static char *dupstr(const char *s){size_t n=p_len(s);char *p=vfs_alloc(n+1);if(p)p_copy(p,s,n+1);return p;}
static char *pdupstr(const char *s){size_t n=p_len(s);char *p=vfs_palloc(n+1);if(p)p_copy(p,s,n+1);return p;}
struct mount {struct mount *next;char *source,*target;unsigned long flags;/* Own backing filesystem hides an index even at its root. */int pack_shadow;};
static struct mount *table;
static char *gpu_poll_sources[2];
static char *root,*executable,*cwd;
static char *oldroot_view;
static int stacked_oldroot;
static int hide_resolver;
static int prefix(const char *,const char *);
struct resolve_cache_entry {
    unsigned valid;
    int follow;
    size_t key_len, value_len;
    void *identity;
    uint64_t hash;
    size_t next;
    char key[PATH_MAX], value[PATH_MAX];
};
static struct resolve_cache_entry *resolve_cache;
static size_t resolve_cache_count,resolve_cache_capacity;
static size_t *resolve_cache_buckets;
static size_t resolve_cache_bucket_count;
static volatile int resolve_cache_lock;
static void resolve_cache_acquire(void) {
    int unlocked=0;
    while(!__atomic_compare_exchange_n(&resolve_cache_lock,&unlocked,1,0,
            __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
        unlocked=0;
        __asm__ volatile("pause");
    }
}
static void resolve_cache_release(void) {
    __atomic_store_n(&resolve_cache_lock,0,__ATOMIC_RELEASE);
}
void vfs_resolve_cache_invalidate(void) {
    resolve_cache_acquire();
    for(size_t i=0;i<resolve_cache_count;i++)resolve_cache[i].valid=0;
    for(size_t i=0;i<resolve_cache_bucket_count;i++)resolve_cache_buckets[i]=0;
    resolve_cache_count=0;
    resolve_cache_release();
}
static int resolve_cacheable_path(const char *path) {
    return path && path[0]=='/' && !prefix(path,"/sys") &&
        !prefix(path,"/proc") && !prefix(path,"/dev");
}
static uint64_t resolve_cache_hash(const char *path,size_t length,int follow){
    uint64_t h=UINT64_C(1469598103934665603)^(uint64_t)follow;
    for(size_t i=0;i<length;i++){h^=(unsigned char)path[i];h*=UINT64_C(1099511628211);}
    return h?h:1;
}
static int resolve_cache_reserve(size_t needed){
    if(needed>resolve_cache_capacity){size_t capacity=resolve_cache_capacity?resolve_cache_capacity:16;
        while(capacity<needed){if(capacity>SIZE_MAX/2)return 0;capacity*=2;}
        if(capacity>SIZE_MAX/sizeof(*resolve_cache))return 0;
        size_t bytes=capacity*sizeof(*resolve_cache);struct resolve_cache_entry *entries=vfs_palloc(bytes);if(!entries)return 0;
        for(size_t i=0;i<resolve_cache_count;i++)entries[i]=resolve_cache[i];
        vfs_free(resolve_cache);resolve_cache=entries;resolve_cache_capacity=capacity;
    }
    if(!resolve_cache_bucket_count || needed>resolve_cache_bucket_count-resolve_cache_bucket_count/4){
        size_t buckets=resolve_cache_bucket_count?resolve_cache_bucket_count:32;
        while(needed>buckets-buckets/4){if(buckets>SIZE_MAX/2)return 0;buckets*=2;}
        if(buckets>SIZE_MAX/sizeof(*resolve_cache_buckets))return 0;
        size_t *table=vfs_palloc(buckets*sizeof(*table));if(!table)return 0;
        for(size_t i=0;i<resolve_cache_count;i++)if(resolve_cache[i].valid){size_t slot=(size_t)resolve_cache[i].hash&(buckets-1);resolve_cache[i].next=table[slot];table[slot]=i+1;}
        vfs_free(resolve_cache_buckets);resolve_cache_buckets=table;resolve_cache_bucket_count=buckets;
    }
    return 1;
}
void vfs_network_payload(int isolated){
    if(!isolated || hide_resolver || !root)return;
    long e=0;char *info=vfs_resolve(AT_FDCWD,"/.flatpak-info",1,&e);
    struct proxy_stat st;
    if(info && !arch_stat_at(AT_FDCWD,info,&st,0))hide_resolver=1;
    vfs_free(info);
}
void vfs_executable(const char *path){vfs_free(executable);executable=pdupstr(path);}
char *vfs_exe(void){return executable?dupstr(executable):NULL;}
/* Worker SIGSYS handlers resolve paths concurrently. A shared cache cannot
 * free/replace keys while another thread reads them, or publish a key before
 * its mount identity. Walk the small mount list instead; no per-call shared
 * writes, locks in a signal handler, or TLS dependency. */
static int prefix(const char *p,const char *s){size_t n=p_len(s);size_t i=0;while(i<n && p[i]==s[i])i++;return i==n && (n==1 || !p[n] || p[n]=='/');}
static struct mount *lookup(const char *path){
    struct mount *best=NULL;size_t n=0;
    for(struct mount *m=table;m;m=m->next){size_t k=p_len(m->target);if(prefix(path,m->target)&&k>n){n=k;best=m;}}
    return best;
}
static char *physical(const char *path){struct mount *m=lookup(path);if(!m)return dupstr(path);return join(m->source,path+(p_len(m->target)==1?0:p_len(m->target)));}
struct proxy_open_how {uint64_t flags,mode,resolve;};
#define PROXY_RESOLVE_NO_XDEV       0x01
#define PROXY_RESOLVE_NO_MAGICLINKS 0x02
#define PROXY_RESOLVE_NO_SYMLINKS   0x04
#define PROXY_RESOLVE_IN_ROOT       0x10
/* Resolve ordinary absolute names in one kernel path walk. Virtual mounts are
 * represented by their backing directory fd. This path is deliberately
 * limited to names without dot components or symlinks: dot-dot can cross a
 * virtual mount boundary, and an absolute symlink under a virtual mount is
 * rooted at the virtual root rather than that mount's backing directory.
 * Magic links, relative/dirfd paths, packed entries, mount roots and all
 * openat2 errors use the existing component walker, which also handles the
 * virtual-root escape and EXDEV rules. */
static char *resolve_openat2_fast(int fd,const char *path,int follow,void **identity,int *outfd){
    if(outfd)*outfd=-1;
    if(fd!=AT_FDCWD || !path || path[0]!='/' || p_equal(path,"/"))return NULL;
    for(size_t i=0;path[i];){while(path[i]=='/')i++;size_t start=i;while(path[i]&&path[i]!='/')i++;size_t n=i-start;
        if((n==1&&path[start]=='.')||(n==2&&path[start]=='.'&&path[start+1]=='.'))return NULL;
    }
    char *global=root?join(root,path):dupstr(path);if(!global)return NULL;
    if((hide_resolver && (p_equal(path,"/etc/resolv.conf") ||
            p_equal(path,"/run/host/monitor/resolv.conf")))){vfs_free(global);return NULL;}
    for(size_t i=0;global[i];){while(global[i]=='/')i++;size_t start=i;while(global[i]&&global[i]!='/')i++;
        if(p_equal(global+start,".v3-inodes")){vfs_free(global);return NULL;}}
    struct proxy_stat packed;
    if(pack_stat(global,&packed)!=-ENOSYS){vfs_free(global);return NULL;}
    struct mount *m=lookup(global);char *host_path=physical(global);
    if(!host_path){vfs_free(global);return NULL;}
    links_recover_path(host_path);
    struct proxy_open_how how={.flags=O_PATH|O_CLOEXEC|(!follow?O_NOFOLLOW:0),
        .resolve=PROXY_RESOLVE_NO_MAGICLINKS|PROXY_RESOLVE_NO_SYMLINKS};
    /* physical() already selected the mount's absolute backing name. Resolving
     * that name directly avoids opening and closing a helper root fd on every
     * trapped lookup; NO_SYMLINKS keeps this fast path conservative. */
    long targetfd=RAW(PROXY_SC_openat2,AT_FDCWD,host_path,&how,sizeof(how),0,0);
    vfs_free(host_path);
    if(targetfd<0){vfs_free(global);return NULL;}
    if(outfd){char *marker=dupstr("");if(marker){*outfd=(int)targetfd;vfs_free(global);return marker;}}
    char proc[48]="/proc/self/fd/";size_t k=14;unsigned value=(unsigned)targetfd;char digits[16];size_t d=0;
    do{digits[d++]=(char)('0'+value%10);value/=10;}while(value);
    while(d)proc[k++]=digits[--d];
    proc[k]=0;
    size_t cap=256;char *answer=NULL;
    for(;;){char *buffer=vfs_alloc(cap+1);if(!buffer)break;
        long n=RAW(PROXY_SC_readlinkat,AT_FDCWD,proc,buffer,cap,0,0);
        if(n<0){vfs_free(buffer);break;}
        if((size_t)n<cap){buffer[n]=0;answer=buffer;break;}
        vfs_free(buffer);if(cap>SIZE_MAX/2-1)break;cap*=2;
    }
    if(answer && outfd)*outfd=(int)targetfd;
    else RAW(PROXY_SC_close,targetfd,0,0,0,0,0);
    if(answer && identity)*identity=m;
    vfs_free(global);return answer;
}
int vfs_openat2_path_fd(int fd,const char *path,int follow,int *outfd){
    PROFILE_BEGIN();
    if(!outfd)return -EINVAL;
    *outfd=-1;char *marker=resolve_openat2_fast(fd,path,follow,NULL,outfd);
    if(!marker){PROFILE_END(5);return -ENOSYS;}
    vfs_free(marker);PROFILE_END(5);return 0;
}
char *vfs_reverse(const char *path){
    struct mount *best=NULL;size_t n=0;
    /* Prefer names reachable in this process's current root. Detached old
     * roots may still have the same backing objects via inherited fds. */
    for(unsigned pass=0;pass<2 && !best;pass++)for(struct mount *m=table;m;m=m->next){
        if(!pass && root && !prefix(m->target,root))continue;
        size_t k=p_len(m->source);
        if(prefix(path,m->source)&&(k>n || (k==n && best && p_len(m->target)<p_len(best->target)))){best=m;n=k;}
    }
    char *p=best?join(best->target,path+(n==1?0:n)):dupstr(path);
    if(p && root && prefix(p,root)){char *q=dupstr(p+p_len(root));vfs_free(p);p=q;if(p && !*p){vfs_free(p);p=dupstr("/");}}
    return p;
}
char *vfs_reverse_pack(const char *path){
    char *p=vfs_reverse(path);if(!p || !oldroot_view || !prefix(p,oldroot_view))return p;
    const char *relative=p+p_len(oldroot_view);char *q=dupstr(*relative?relative:"/");vfs_free(p);return q;
}
static char *readlink_dynamic(const char *path,long *error){
    int handled=0;long pe=0;char *packed=pack_link(path,&pe,&handled);
    if(handled){*error=pe;return packed;}
    /* Pin the final component without following it. Path lookup needs search
     * permission on ancestors; O_PATH does not request getattr on them.
     * Empty-path readlink then distinguishes a symlink from an ordinary
     * component without a pathname metadata probe. */
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,path,O_PATH|O_NOFOLLOW|O_CLOEXEC,0,0,0);
    if(fd<0){*error=fd;return NULL;}
    size_t n=256;
    for(;;){
        char *s=vfs_alloc(n+1);
        if(!s){RAW(PROXY_SC_close,fd,0,0,0,0,0);*error=-ENOMEM;return NULL;}
        long r=RAW(PROXY_SC_readlinkat,fd,"",s,n,0,0);
        if(r<0){RAW(PROXY_SC_close,fd,0,0,0,0,0);vfs_free(s);*error=r;return NULL;}
        if((size_t)r<n){RAW(PROXY_SC_close,fd,0,0,0,0,0);s[r]=0;return s;}
        vfs_free(s);
        if(n>SIZE_MAX/2-1){RAW(PROXY_SC_close,fd,0,0,0,0,0);*error=-EOVERFLOW;return NULL;}
        n*=2;
    }
}
static int proc_pack_fd(const char *path);
struct fd_name {char *path;size_t capacity,length;};
static struct fd_name *fd_names;
static size_t fd_names_capacity;
/* Only protect the fd-name bookkeeping, never a payload syscall (flock may
 * block). Block signals during the short critical section so an app handler
 * cannot reenter it. Fork children reset the copied lock before payload runs. */
static proxy_sigmask_t fd_names_lock(void){
    PROFILE_BEGIN();
    proxy_sigmask_t all=~UINT64_C(0),mask=0;
    RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&all,&mask,8,0,0);
    for(;;){unsigned long owner=__atomic_load_n(&fd_names_owner,__ATOMIC_RELAXED);
        if(!owner){unsigned long unlocked=0;if(__atomic_compare_exchange_n(&fd_names_owner,&unlocked,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)){PROFILE_END(1);return mask;}}
        __asm__ volatile("pause");
    }
}
static void fd_names_unlock(proxy_sigmask_t mask){
    PROFILE_BEGIN();
    __atomic_store_n(&fd_names_owner,0,__ATOMIC_RELEASE);
    RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&mask,0,8,0,0);
    PROFILE_END(2);
}
static void fd_name_close(int fd){
    proxy_sigmask_t mask=fd_names_lock();
    if(fd>=0 && (size_t)fd<fd_names_capacity)fd_names[fd].length=0;
    fd_names_unlock(mask);
}
static int fd_names_reserve(size_t needed){
    if(needed<=fd_names_capacity)return 0;
    if(needed>SIZE_MAX/sizeof(*fd_names))return -ENOMEM;
    size_t capacity=fd_names_capacity?fd_names_capacity:16;
    while(capacity<needed){if(capacity>SIZE_MAX/2){capacity=needed;break;}capacity*=2;}
    if(capacity>SIZE_MAX/sizeof(*fd_names))return -ENOMEM;
    struct fd_name *next=vfs_palloc(capacity*sizeof(*next));if(!next)return -ENOMEM;
    if(fd_names)p_copy(next,fd_names,fd_names_capacity*sizeof(*next));
    vfs_free(fd_names);fd_names=next;fd_names_capacity=capacity;return 0;
}
static int fd_name_set_locked(int fd,const char *path){
    if(fd<0 || (unsigned)fd==UINT_MAX)return -EBADF;
    int e=fd_names_reserve((size_t)fd+1);if(e)return e;
    struct fd_name *entry=&fd_names[fd];size_t n=p_len(path)+1;
    if(n>entry->capacity){size_t capacity=entry->capacity?entry->capacity:64;while(capacity<n){if(capacity>SIZE_MAX/2){capacity=n;break;}capacity*=2;}
        char *next=vfs_palloc(capacity);if(!next)return -ENOMEM;vfs_free(entry->path);entry->path=next;entry->capacity=capacity;}
    p_copy(entry->path,path,n);entry->length=n;return 0;
}
void vfs_fd_close(int fd){pack_close(fd);fd_name_close(fd);}
long vfs_fd_dup(int from,int to){if(from==to)return 0;long e=pack_dup(from,to);if(e)return e;char *name=vfs_fd_name(from);fd_name_close(to);if(name){vfs_fd_record(to,AT_FDCWD,name);vfs_free(name);}return 0;}
char *vfs_fd_name(int fd){
    PROFILE_BEGIN();
    proxy_sigmask_t mask=fd_names_lock();char *name=NULL;
    if(fd>=0 && (size_t)fd<fd_names_capacity){struct fd_name *p=&fd_names[fd];if(p->length){
        if(!root)name=dupstr(p->path);
        else if(prefix(p->path,root)){const char *relative=p->path+p_len(root);name=dupstr(*relative?relative:"/");}
    }}
    fd_names_unlock(mask);PROFILE_END(3);return name;
}
static int self_proc(const char *path){
    if(prefix(path,"/proc/self") || prefix(path,"/proc/thread-self"))return 1;
    if(!prefix(path,"/proc") || path[5]!='/')return 0;
    /* A numeric /proc/<tid> belongs to us only if it appears in our task
     * directory. Never trust another process's descriptor number. */
    const char *n=path+6;size_t count=0;
    while(n[count]>='0' && n[count]<='9')count++;
    if(!count || (n[count] && n[count]!='/') || count>SIZE_MAX-17)return 0;
    char *task=vfs_alloc(count+17);if(!task)return 0;
    p_copy(task,"/proc/self/task/",16);p_copy(task+16,n,count);task[16+count]=0;
    struct proxy_stat st;int own=!arch_stat_at(AT_FDCWD,task,&st,0) && S_ISDIR(st.st_mode);
    vfs_free(task);return own;
}
char *vfs_proc_fd_name(const char *path){
    if(!self_proc(path))return NULL;
    for(size_t i=0;path[i];i++)if(path[i]=='/' && path[i+1]=='f' && path[i+2]=='d' && path[i+3]=='/'){
        const char *n=path+i+4;unsigned fd=0;if(!*n)return NULL;
        while(*n>='0' && *n<='9'){unsigned digit=(unsigned)(*n++-'0');if(fd>(2147483647U-digit)/10)return NULL;fd=fd*10+digit;}
        if(*n)return NULL;
        return vfs_fd_name((int)fd);
    }
return NULL;
}
static char *basepath(int fd,long *error){if(fd==AT_FDCWD){
if(cwd){char *global=root?join(root,cwd):dupstr(cwd);char *host=global?physical(global):NULL;struct proxy_stat named,actual;if(host && !pack_stat(host,&named) && S_ISDIR(named.st_mode)){vfs_free(global);vfs_free(host);return dupstr(cwd);}int valid=host && !arch_stat_at(AT_FDCWD,host,&named,0) && !arch_stat_at(AT_FDCWD,".",&actual,0) && named.st_dev==actual.st_dev && named.st_ino==actual.st_ino;vfs_free(global);vfs_free(host);if(valid)return dupstr(cwd);vfs_free(cwd);cwd=NULL;}size_t n=256;for(;;){char *s=vfs_alloc(n);if(!s){*error=-ENOMEM;return NULL;}long r=RAW(PROXY_SC_getcwd,s,n,0,0,0,0);if(r>=0){char *q=vfs_reverse(s);vfs_free(s);return q;}vfs_free(s);if(r!=-ERANGE){*error=r;return NULL;}if(n>SIZE_MAX/2){*error=-EOVERFLOW;return NULL;}n*=2;}}
    if(pack_is_directory(fd)==0){*error=-ENOTDIR;return NULL;}
    char *tracked=vfs_fd_name(fd);if(tracked)return tracked;
    struct proxy_stat st;long status=arch_stat_fd(fd,&st);if(status<0){*error=status;return NULL;}if(!S_ISDIR(st.st_mode)){*error=-ENOTDIR;return NULL;}
    char p[64]="/proc/self/fd/",digits[24];size_t k=14;unsigned d=0;unsigned long v=(unsigned)fd;do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);while(d)p[k++]=digits[--d];p[k]=0;
    char *s=readlink_dynamic(p,error);if(!s)return NULL;char *q=vfs_reverse(s);vfs_free(s);return q;
}
static char *vfs_resolve_mount_impl(int fd,const char *path,int follow,long *error,void **identity,int *cacheable,int allow_fast){
    *error=0;if(!path){*error=-EFAULT;return NULL;}if(!*path){*error=-ENOENT;return NULL;}
    if(cacheable)*cacheable=1;
    /* Polling roots are immutable generated directories. Re-check the current
     * mount source so an unmount/overmount cannot reuse a stale fast answer. */
    if(p_equal(path,"/sys/bus") || p_equal(path,"/sys/class")){
        unsigned slot=p_equal(path,"/sys/class");
        if(gpu_poll_sources[slot]){char *global=root?join(root,path):dupstr(path);struct mount *m=global?lookup(global):NULL;
            int hit=m && p_equal(m->target,global) && p_equal(m->source,gpu_poll_sources[slot]);vfs_free(global);
            if(hit){if(identity)*identity=m;char *answer=dupstr(m->source);if(!answer)*error=-ENOMEM;return answer;}}
    }
    if(allow_fast){char *fast=resolve_openat2_fast(fd,path,follow,identity,NULL);if(fast)return fast;}
    char *input;
    if(path[0]=='/')input=dupstr(path);else {char *base=basepath(fd,error);if(!base)return NULL;input=join(base,path);vfs_free(base);}
    if(!input){*error=-ENOMEM;return NULL;}
    unsigned links=0;
restart:;
    char *out=vfs_alloc(p_len(input)+2);if(!out){vfs_free(input);*error=-ENOMEM;return NULL;}size_t used=1;out[0]='/';out[1]=0;
    for(size_t i=0;input[i];){while(input[i]=='/')i++;if(!input[i])break;size_t start=i;while(input[i] && input[i]!='/')i++;size_t len=i-start;
        if(len==1 && input[start]=='.')continue;
        if(len==2 && input[start]=='.' && input[start+1]=='.'){while(used>1 && out[used-1]!='/')used--;if(used>1)used--;out[used]=0;continue;}
        if(used>1)out[used++]='/';
        p_copy(out+used,input+start,len);used+=len;out[used]=0;
        /* Flatpak prepares host resolver binds even with --unshare-net.
         * Hide them only after sandbox construction, at payload exec; bwrap
         * must still be able to resolve its bind sources while setting up. */
        if(hide_resolver && (p_equal(out,"/etc/resolv.conf") ||
                p_equal(out,"/run/host/monitor/resolv.conf"))){*error=-ENOENT;goto fail;}
        if(p_equal(out+used-len,".v3-inodes")){*error=-ENOENT;goto fail;}
        size_t next=i;while(input[next]=='/')next++;
        if(!follow && !input[next] && input[i]!='/' )continue;
        char *global=root?join(root,out):dupstr(out);char *host=global?physical(global):NULL;vfs_free(global);if(!host){*error=-ENOMEM;goto fail;}
        links_recover_path(host);
        long e=0;char *target=readlink_dynamic(host,&e);
        if(target && cacheable)*cacheable=0;
        if(!target && input[i]=='/'){
            struct proxy_stat st;long status=pack_stat(host,&st);
            if(status==-ENOSYS){
                /* Path lookup needs search permission on ancestors, not
                 * getattr. An O_PATH|O_DIRECTORY open has the kernel's
                 * path-walk permissions and also rejects non-directory
                 * components without requesting directory metadata. */
                long dir=RAW(PROXY_SC_openat,AT_FDCWD,host,O_PATH|O_DIRECTORY|O_CLOEXEC,0,0,0);
                if(dir<0)status=dir;
                else {RAW(PROXY_SC_close,dir,0,0,0,0,0);status=0;}
            }else if(status>=0 && !S_ISDIR(st.st_mode))status=-ENOTDIR;
            if(status<0){vfs_free(host);*error=status;goto fail;}
        }
        /* proc fd magic links point to backing objects, outside a pivoted
         * pathname root. They are not ordinary payload absolute symlinks. */
        if(target && prefix(host,"/proc") && !p_equal(out+ (used>=4?used-4:0),"/exe")){
            int magic=0;for(size_t j=0;host[j];j++)if(host[j]=='/' && host[j+1]=='f' && host[j+2]=='d' && host[j+3]=='/')magic=1;
            int packed_fd=magic?proc_pack_fd(out):-1;
            if(packed_fd>=0 && pack_tracked(packed_fd)){
                /* Packed fds point at harmless kernel carriers (the empty
                 * directory for O_PATH opens, or the archive for files),
                 * not at the indexed object. Follow the recorded VFS name
                 * so bind-fd resolves to the source object, including files. */
                char *name=vfs_fd_name(packed_fd);
                if(name){char *replacement=join(name,input+i);vfs_free(name);vfs_free(target);vfs_free(host);
                    if(!replacement){*error=-ENOMEM;goto fail;}
                    if(++links>40){vfs_free(replacement);*error=-ELOOP;goto fail;}
                    vfs_free(input);vfs_free(out);input=replacement;goto restart;
                }
            }
            if(magic && !input[i] && follow==VFS_FOLLOW_LINK_SOURCE){
                /* Keep the proc magic link itself. readlink text for O_TMPFILE
                 * is a nonexistent "#inode (deleted)" name, not the inode. */
                if(identity){char *name=vfs_proc_fd_name(out);if(!name)name=vfs_reverse(target);char *global=name?(root?join(root,name):dupstr(name)):NULL;
                    *identity=global?lookup(global):NULL;vfs_free(global);vfs_free(name);}
                vfs_free(target);vfs_free(input);vfs_free(out);return host;
            }
            if(magic){char *answer=join(target,input+i);vfs_free(target);vfs_free(host);vfs_free(input);vfs_free(out);if(!answer)*error=-ENOMEM;return answer;}
        }
        vfs_free(host);
        if(target && self_proc(out) && used>=4 && p_equal(out+used-4,"/exe") && executable){vfs_free(target);target=dupstr(executable);}
        if(target){if(++links>40){vfs_free(target);*error=-ELOOP;goto fail;}char *head;
            if(target[0]=='/')head=dupstr(target);else {used-=len;if(used>1)used--;out[used]=0;head=join(out,target);}vfs_free(target);
            char *replacement=head?join(head,input+i):NULL;vfs_free(head);if(!replacement){*error=-ENOMEM;goto fail;}vfs_free(input);vfs_free(out);input=replacement;goto restart;
        }else if(e!=-EINVAL && e!=-ENOENT && e!=-ENOTDIR){*error=e;goto fail;}
    }
    {char *global=root?join(root,out):dupstr(out);if(identity)*identity=global?lookup(global):NULL;char *host=global?physical(global):NULL;vfs_free(global);if(host && input[p_len(input)-1]=='/' && !p_equal(host,"/")){char *q=join(host,"/");vfs_free(host);host=q;}vfs_free(input);vfs_free(out);if(!host)*error=-ENOMEM;return host;}
fail:vfs_free(input);vfs_free(out);return NULL;
}
char *vfs_resolve_mount(int fd,const char *path,int follow,long *error,void **identity){
    size_t key_len=path?p_len(path):0;
    if(fd==AT_FDCWD && follow && resolve_cacheable_path(path) && key_len<PATH_MAX){
        resolve_cache_acquire();
        uint64_t hash=resolve_cache_hash(path,key_len,follow);
        if(resolve_cache_bucket_count){size_t index=resolve_cache_buckets[(size_t)hash&(resolve_cache_bucket_count-1)];
          while(index){struct resolve_cache_entry *entry=&resolve_cache[index-1];
            if(entry->valid && entry->hash==hash && entry->follow==follow && entry->key_len==key_len && p_equal(entry->key,path)){
                char *answer=vfs_alloc(entry->value_len+1);
                if(answer){p_copy(answer,entry->value,entry->value_len+1);if(identity)*identity=entry->identity;}
                resolve_cache_release();
                if(!answer){*error=-ENOMEM;return NULL;}
                /* Recovery is observable even when pathname translation is cached. */
                links_recover_path(answer);
                *error=0;
                return answer;
            }
            index=entry->next;
          }
        }
        resolve_cache_release();
    }
    uint64_t hash=resolve_cache_hash(path?path:"",key_len,follow);
    uint64_t started=proxy_count_now();
    int cacheable=0;
    char *answer=vfs_resolve_mount_impl(fd,path,follow,error,identity,&cacheable,1);
    uint64_t ended=proxy_count_now();
    if(started && ended>=started)proxy_count_resolution(ended-started);
    if(answer && cacheable && identity && fd==AT_FDCWD && follow &&
            resolve_cacheable_path(path) && key_len<PATH_MAX){
        size_t value_len=p_len(answer);
        if(value_len<PATH_MAX){
            resolve_cache_acquire();
            size_t slot=resolve_cache_count;
            if(resolve_cache_reserve(resolve_cache_count+1)){
                resolve_cache_count++;
                struct resolve_cache_entry *entry=&resolve_cache[slot];size_t bucket=(size_t)hash&(resolve_cache_bucket_count-1);
                entry->valid=0;entry->follow=follow;entry->key_len=key_len;
                entry->value_len=value_len;entry->identity=*identity;entry->hash=hash;entry->next=resolve_cache_buckets[bucket];
                p_copy(entry->key,path,key_len+1);p_copy(entry->value,answer,value_len+1);
                entry->valid=1;resolve_cache_buckets[bucket]=slot+1;
            }
            resolve_cache_release();
        }
    }
    return answer;
}
char *vfs_resolve_mount_slow(int fd,const char *path,int follow,long *error,void **identity){
    int cacheable=0;
    return vfs_resolve_mount_impl(fd,path,follow,error,identity,&cacheable,0);
}
char *vfs_resolve(int fd,const char *path,int follow,long *error){return vfs_resolve_mount(fd,path,follow,error,NULL);}
/* A bind source that is a regular file stays referenced by its virtual mount
 * (Linux keeps the inode alive after bwrap unlinks its bind-data temp file). */
int vfs_is_mount_source(const char *path){for(struct mount *m=table;m;m=m->next)if(p_equal(m->source,path))return 1;return 0;}
/* A packed index describes immutable files. If a more-specific virtual mount
 * owns this path, let that mount's backing tree handle the lookup instead. */
int vfs_has_nested_mount(const char *path,const char *ancestor,int staging){
    size_t n=p_len(ancestor);
    for(struct mount *m=table;m;m=m->next){
        const char *target=m->target;
        if(root&&prefix(target,root)){target+=p_len(root);if(!*target)target="/";}
        /* Match vfs_reverse_pack's detached-root view as well. */
        if(oldroot_view && prefix(target,oldroot_view)){
            target+=p_len(oldroot_view);if(!*target)target="/";
        }
        /* pack_virtual_path removes bwrap's staging /newroot prefix.
         * Compare mount targets in that same view, or a packed runtime
         * symlink can win over the tmpfs used for extension merge-dirs. */
        if(staging && prefix(target,"/newroot")){target+=8;if(!*target)target="/";}
        if((p_len(target)>n || (m->pack_shadow && p_len(target)==n)) && prefix(target,ancestor) && prefix(path,target))return 1;
    }
    return 0;
}
long vfs_mount_readonly(void *identity){struct mount *m=identity;return m && (m->flags&MS_RDONLY)?-EROFS:0;}
int proxy_mount(const char *source,const char *target){if(!source || !target || target[0]!='/')return -EINVAL;struct mount *m=vfs_palloc(sizeof(*m));if(!m)return -ENOMEM;m->source=vfs_palloc(p_len(source)+1);m->target=vfs_palloc(p_len(target)+1);if(m->source)p_copy(m->source,source,p_len(source)+1);if(m->target)p_copy(m->target,target,p_len(target)+1);m->flags=0;m->pack_shadow=0;if(!m->source || !m->target){vfs_free(m->source);vfs_free(m->target);vfs_free(m);return -ENOMEM;}size_t n=p_len(m->target);while(n>1 && m->target[n-1]=='/')m->target[--n]=0;m->next=table;table=m;vfs_resolve_cache_invalidate();
    proxy_log("mount-owner",PROXY_SC_mount,0);
    RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 mount ",9,0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,source,p_len(source),0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd," -> ",4,0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,target,p_len(target),0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,"\n",1,0,0,0);return 0;}
const char *proxy_path(const char *path,char *buffer,size_t size){long e;char *p=vfs_resolve(AT_FDCWD,path,1,&e);if(!p)return NULL;size_t n=p_len(p);if(n>=size){vfs_free(p);return NULL;}p_copy(buffer,p,n+1);vfs_free(p);return buffer;}
static char *global_target(const char *target,long *error){
    char *virtual;
    if(target[0]=='/')virtual=dupstr(target);
    else {char *base=basepath(AT_FDCWD,error);if(!base)return NULL;virtual=join(base,target);vfs_free(base);}
    if(!virtual){*error=-ENOMEM;return NULL;}
    char *p=root?join(root,virtual):dupstr(virtual);vfs_free(virtual);
    if(!p){*error=-ENOMEM;return NULL;}
    /* lexical dot normalization for mount names, with no backing resolution */
    char *out=vfs_alloc(p_len(p)+2);if(!out){vfs_free(p);*error=-ENOMEM;return NULL;}size_t n=1;out[0]='/';out[1]=0;
    for(size_t i=0;p[i];){while(p[i]=='/')i++;size_t start=i;while(p[i]&&p[i]!='/')i++;size_t len=i-start;if(!len)break;if(len==1&&p[start]=='.')continue;if(len==2&&p[start]=='.'&&p[start+1]=='.'){while(n>1&&out[n-1]!='/')n--;if(n>1)n--;out[n]=0;continue;}if(n>1)out[n++]='/';p_copy(out+n,p+start,len);n+=len;out[n]=0;}vfs_free(p);return out;
}
void vfs_fd_record(int fd,int dirfd,const char *path){
    PROFILE_BEGIN();
    long e=0;char *global=global_target(path,&e);
    if(path[0]!='/'){
        char *base=basepath(dirfd,&e);vfs_free(global);
        char *relative=base?join(base,path):NULL;vfs_free(base);
        global=relative?global_target(relative,&e):NULL;vfs_free(relative);
    }
    if(!global)return;
    proxy_sigmask_t mask=fd_names_lock();
    (void)fd_name_set_locked(fd,global);
    fd_names_unlock(mask);vfs_free(global);PROFILE_END(4);
}
void vfs_fd_rename(const char *source,const char *destination){
    if(!source || !destination)return;
    char *old_view=vfs_reverse(source),*new_view=vfs_reverse(destination);long error=0;
    char *old=old_view?global_target(old_view,&error):NULL;
    char *next=new_view?global_target(new_view,&error):NULL;
    vfs_free(old_view);vfs_free(new_view);
    if(!old || !next){vfs_free(old);vfs_free(next);return;}
    size_t old_len=p_len(old);proxy_sigmask_t mask=fd_names_lock();
    for(size_t i=0;i<fd_names_capacity;i++){
        struct fd_name *entry=&fd_names[i];if(!entry->length || !prefix(entry->path,old))continue;
        char *renamed=join(next,entry->path+old_len);if(!renamed)continue;
        (void)fd_name_set_locked((int)i,renamed);vfs_free(renamed);
    }
    fd_names_unlock(mask);vfs_free(old);vfs_free(next);
}
char *vfs_temp_create(long *error){
    extern const char *proxy_tmpdir;
    static unsigned long sequence;
    char suffix[80]="compat-proxy-";size_t k=13;
    unsigned long values[2]={(unsigned long)RAW(PROXY_SC_getpid,0,0,0,0,0,0),0};
    RAW(PROXY_SC_getrandom,&values[1],sizeof(values[1]),0,0,0,0);values[1]^=++sequence;
    for(unsigned j=0;j<2;j++){char d[24];unsigned n=0;do{d[n++]=(char)('0'+values[j]%10);values[j]/=10;}while(values[j]);while(n)suffix[k++]=d[--n];suffix[k++]=j?0:'-';}
    char *src=join(proxy_tmpdir,suffix);if(!src){*error=-ENOMEM;return NULL;}
    int parent=temp_parent();if(parent<0)*error=parent;
    else{*error=RAW(PROXY_SC_flock,parent,LOCK_EX,0,0,0,0);if(!*error)*error=RAW(PROXY_SC_mkdirat,AT_FDCWD,src,0700,0,0,0);if(!*error)*error=temp_own(src);RAW(PROXY_SC_close,parent,0,0,0,0,0);}
    if(*error){vfs_free(src);return NULL;}return src;
}
long vfs_mount(const char *source,const char *target,const char *type,unsigned long flags){
    if(flags&(MS_PRIVATE|MS_SLAVE|MS_SHARED|MS_UNBINDABLE))return 0;
    long e=0;char *canonical=NULL;
    if(prefix(target,"/proc")){
        char *backing=vfs_resolve(AT_FDCWD,target,1,&e);
        if(!backing)return e;
        canonical=vfs_proc_fd_name(target);if(!canonical)canonical=vfs_reverse(backing);vfs_free(backing);if(!canonical)return -ENOMEM;
    }
    char *dest=global_target(canonical?canonical:target,&e);vfs_free(canonical);if(!dest)return e;
    if(flags&MS_REMOUNT){struct mount *m=lookup(dest);if(!m)e=-EINVAL;else m->flags=flags;vfs_free(dest);return e;}
    char *src=NULL;
    if(flags&MS_BIND)src=vfs_resolve(AT_FDCWD,source,1,&e);
    else if(type && p_equal(type,"proc"))src=dupstr("/proc");
    else if(type && p_equal(type,"devpts"))src=dupstr("/dev/pts");
    else if(type && p_equal(type,"mqueue"))src=dupstr("/dev/mqueue");
    else if(type && p_equal(type,"tmpfs")){
        src=vfs_temp_create(&e);
    }else e=-ENOSYS;
    if(!e && !src)e=-ENOMEM;
    if(!e){
        struct mount *prior=table;char *virtual_source=(flags&MS_BIND)?global_target(source,&e):NULL;
        if(!e)e=proxy_mount(src,dest);
        if(!e){table->flags=flags;struct proxy_stat st;table->pack_shadow=!(flags&MS_BIND) || pack_stat(src,&st)==-ENOSYS;}
        if(!e && virtual_source && (flags&MS_REC))for(struct mount *m=prior;m;m=m->next){if(prefix(m->target,virtual_source) && !p_equal(m->target,virtual_source)){
            char *child=join(dest,m->target+p_len(virtual_source));if(!child){e=-ENOMEM;break;}e=proxy_mount(m->source,child);vfs_free(child);if(e)break;table->flags=m->flags;table->pack_shadow=m->pack_shadow;
        }}
        vfs_free(virtual_source);
    }vfs_free(src);vfs_free(dest);return e;
}
long vfs_unmount(const char *target){RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 unmount ",11,0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,target,p_len(target),0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,"\n",1,0,0,0);if(stacked_oldroot && p_equal(target,".")){stacked_oldroot=0;return 0;}long e=0;char *dest=global_target(target,&e);if(!dest)return e;char *oldroot_global=oldroot_view?(root?join(root,oldroot_view):dupstr(oldroot_view)):NULL;if(oldroot_global && p_equal(dest,oldroot_global)){vfs_free(oldroot_view);oldroot_view=NULL;}vfs_free(oldroot_global);struct mount **p=&table;while(*p && !p_equal((*p)->target,dest))p=&(*p)->next;vfs_free(dest);if(!*p)return p_equal(target,".")?0:-EINVAL;struct mount *m=*p;*p=m->next;vfs_resolve_cache_invalidate();vfs_free(m->source);vfs_free(m->target);vfs_free(m);return 0;}
long vfs_root(const char *target){long e=0;char *next=global_target(target,&e);if(!next)return e;char *host=physical(next);if(!host){vfs_free(next);return -ENOMEM;}long fd=RAW(PROXY_SC_openat,AT_FDCWD,host,O_DIRECTORY|O_PATH|O_CLOEXEC,0,0,0);vfs_free(host);if(fd<0){vfs_free(next);return fd;}RAW(PROXY_SC_close,fd,0,0,0,0,0);char *saved=pdupstr(next);vfs_free(next);if(!saved)return -ENOMEM;vfs_resolve_cache_invalidate();vfs_free(root);root=saved;vfs_free(cwd);cwd=NULL;return 0;}
long vfs_pivot(const char *target,const char *old){long e=0;char *next=global_target(target,&e);if(!next)return e;vfs_resolve_cache_invalidate();RAW(PROXY_SC_write,proxy_diagnostic_fd,"v3 pivot ",9,0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,next,p_len(next),0,0,0);RAW(PROXY_SC_write,proxy_diagnostic_fd,"\n",1,0,0,0);
    stacked_oldroot=p_equal(target,old);
    if(!p_equal(target,old)){char *oldhost=physical(root?root:"/");char *dest=global_target(old,&e);
        if(!dest || !oldhost){vfs_free(next);vfs_free(dest);vfs_free(oldhost);return -ENOMEM;}struct mount *prior=table;
        e=proxy_mount(oldhost,dest);
        if(!e)for(struct mount *m=prior;m;m=m->next){
            const char *relative=m->target;
            /* Preserve the complete pre-pivot pathname view. Most entries
             * are stored below root; bootstrap roots can also have explicit
             * top-level aliases (for example /usr and /etc) outside it. */
            if(root && prefix(relative,root))relative+=p_len(root);
            if(!*relative || p_equal(relative,"/"))continue;
            char *alias=join(dest,relative);if(!alias){e=-ENOMEM;break;}
            e=proxy_mount(m->source,alias);vfs_free(alias);if(e)break;table->flags=m->flags;table->pack_shadow=m->pack_shadow;
        }
        char *next_cwd=NULL;
        if(!e && cwd){char *oldcwd=root?join(root,cwd):dupstr(cwd);if(!oldcwd)e=-ENOMEM;
            else if(prefix(oldcwd,next)){const char *relative=oldcwd+p_len(next);next_cwd=dupstr(*relative?relative:"/");}
            else {char *oldglobal=join(dest,cwd);if(oldglobal && prefix(oldglobal,next)){const char *relative=oldglobal+p_len(next);next_cwd=dupstr(*relative?relative:"/");}vfs_free(oldglobal);}
            vfs_free(oldcwd);if(!next_cwd && !e)e=-ENOMEM;}
        if(!e){proxy_sigmask_t mask=fd_names_lock();for(size_t i=0;i<fd_names_capacity;i++){struct fd_name *f=&fd_names[i];if(!f->length)continue;
            if(prefix(f->path,next))continue;
            const char *relative=f->path;
            if(root && prefix(relative,root)){relative+=p_len(root);if(!*relative)relative="/";}
            char *rebased=join(dest,relative);if(!rebased){e=-ENOMEM;break;}
            e=fd_name_set_locked((int)i,rebased);vfs_free(rebased);if(e)break;
        }fd_names_unlock(mask);}
        char *next_oldroot=NULL;if(!e && prefix(dest,next)){const char *relative=dest+p_len(next);next_oldroot=pdupstr(*relative?relative:"/");if(!next_oldroot)e=-ENOMEM;}
        char *saved_next=!e?pdupstr(next):NULL;char *saved_cwd=next_cwd?pdupstr(next_cwd):NULL;
        vfs_free(dest);vfs_free(oldhost);if(e || !saved_next || (next_cwd&&!saved_cwd)){vfs_free(next_cwd);vfs_free(next_oldroot);vfs_free(saved_next);vfs_free(saved_cwd);vfs_free(next);return e?e:-ENOMEM;}
        vfs_free(next_cwd);vfs_free(next);vfs_free(root);root=saved_next;vfs_free(oldroot_view);oldroot_view=next_oldroot;if(saved_cwd){vfs_free(cwd);cwd=saved_cwd;}return 0;}
    char *saved=pdupstr(next);vfs_free(next);if(!saved)return -ENOMEM;vfs_free(root);root=saved;vfs_free(cwd);cwd=NULL;return 0;
}
static int proc_pack_fd(const char *path){if(!self_proc(path))return -1;for(size_t i=0;path[i];i++)if(path[i]=='/'&&path[i+1]=='f'&&path[i+2]=='d'&&path[i+3]=='/'){const char *p=path+i+4;if(!*p)return -1;unsigned fd=0;for(;*p;p++){if(*p<'0'||*p>'9'||fd>(2147483647U-(unsigned)(*p-'0'))/10)return -1;fd=fd*10+(unsigned)(*p-'0');}return (int)fd;}return -1;}
long vfs_proc_open(const char *path,int flags){int source=proc_pack_fd(path);if(source>=0&&pack_tracked(source)){char *name=vfs_fd_name(source);if(name){long fd=pack_open(name,flags);if(fd!=-ENOSYS){if(fd>=0)vfs_fd_record((int)fd,AT_FDCWD,name);vfs_free(name);return fd;}vfs_free(name);}}size_t n=p_len(path);if(n<10 || !p_equal(path+n-10,"/mountinfo") || !table)return -ENOSYS;
    long fd=RAW(PROXY_SC_memfd_create,"v3-mountinfo",1,0,0,0,0);if(fd<0)return fd;
    const char *header="1 0 0:1 / ",*middle=" rw,nosuid - tmpfs compat-proxy rw\n";
    /* Virtual view required by bwrap's post-bind verification. Stable IDs are
     * not yet exposed: this is a setup-time snapshot. */
    for(struct mount *m=table;m;m=m->next){const char *t=m->target;
        if(root && !prefix(t,root))continue;
        if(root)t+=p_len(root);
        if(!*t)t="/";
        RAW(PROXY_SC_write,fd,header,p_len(header),0,0,0);RAW(PROXY_SC_write,fd,t,p_len(t),0,0,0);RAW(PROXY_SC_write,fd,middle,p_len(middle),0,0,0);
    }
    RAW(PROXY_SC_lseek,fd,0,0,0,0,0);return fd;
}
long vfs_chdir(int fd,const char *path){long e=0;void *identity=NULL;char *host;
    if(path)host=vfs_resolve_mount(fd,path,1,&e,&identity);
    else {char proc[64]="/proc/self/fd/",digits[24];size_t k=14;unsigned d=0;unsigned long v=(unsigned)fd;do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);while(d)proc[k++]=digits[--d];proc[k]=0;host=readlink_dynamic(proc,&e);}
    if(!host)return e;
    long packed=path?pack_open(host,O_RDONLY|O_DIRECTORY|O_CLOEXEC):-ENOSYS;
    if(path && packed!=-ENOSYS){e=packed<0?packed:RAW(PROXY_SC_fchdir,packed,0,0,0,0,0);if(packed>=0){pack_close(packed);RAW(PROXY_SC_close,packed,0,0,0,0,0);}}
    else e=path?RAW(PROXY_SC_chdir,host,0,0,0,0,0):RAW(PROXY_SC_fchdir,fd,0,0,0,0,0);
    if(!e){struct mount *m=identity;char *logical=!path?vfs_fd_name(fd):NULL;if(!logical)logical=m?join(m->target,host+(p_len(m->source)==1?0:p_len(m->source))):vfs_reverse(host);
        if(logical && m && root && prefix(logical,root)){char *q=dupstr(logical+p_len(root));vfs_free(logical);logical=q;if(logical && !*logical){vfs_free(logical);logical=dupstr("/");}}
        if(!logical)e=-ENOMEM;else {char *saved=pdupstr(logical);vfs_free(logical);if(!saved)e=-ENOMEM;else {vfs_free(cwd);cwd=saved;}}}
    vfs_free(host);return e;
}
char *vfs_getcwd(long *error){return basepath(AT_FDCWD,error);}
long vfs_bootstrap_root(const char *directory){
    /* CLI roots are host-side symlink scaffolds, not payload symlinks. Pin
     * their existing top-level endpoints before virtual resolution begins. */
    const char *names[]={"usr","bin","lib","lib64","etc","sbin","proc","dev","data","system","apex"};
    char *sources[11]={0};long error=0;
    for(unsigned i=0;i<11;i++){
        char *path=join(directory,names[i]);if(!path){error=-ENOMEM;break;}
        long fd=RAW(PROXY_SC_openat,AT_FDCWD,path,O_PATH|O_CLOEXEC,0,0,0);vfs_free(path);
        if(fd<0){if(fd!=-ENOENT)error=fd;continue;}
        char proc[64]="/proc/self/fd/",digits[24];unsigned d=0;size_t k=14;unsigned long v=(unsigned long)fd;do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);while(d)proc[k++]=digits[--d];proc[k]=0;
        sources[i]=readlink_dynamic(proc,&error);RAW(PROXY_SC_close,fd,0,0,0,0,0);if(!sources[i])break;
    }
    if(!error)error=proxy_mount(directory,"/");
    for(unsigned i=0;i<11;i++){if(!error && sources[i]){char *target=join("/",names[i]);if(!target)error=-ENOMEM;else error=proxy_mount(sources[i],target);vfs_free(target);}vfs_free(sources[i]);}
    return error;
}
/* GPU identity comes only from pc-gpu-detect's property snapshot, never PCI
 * sysfs. A private read-only virtual mount uses the existing VFS resolver for
 * all path, stat, readlink and directory operations (including relative fds).
 * Bionic's v0xfc6ed0ab property area ABI: 128-byte header, offset trie,
 * 96-byte short prop_info. Fail closed on unknown formats/dirty snapshots. */
static uint32_t gpu_u32(const char *p){return __atomic_load_n((const uint32_t *)p,__ATOMIC_ACQUIRE);}
static int gpu_property(const char *area,size_t size,const char *name,char value[92]){
    if(size<148 || gpu_u32(area+8)!=0x504f5250 || gpu_u32(area+12)!=0xfc6ed0ab)return 0;
    const char *data=area+128;size_t bytes=size-128;uint32_t node=0;
    const char *token=name;
    for(size_t budget=bytes/20;budget;){
        if(node>bytes-20 || (node&3))return 0;
        uint32_t offset=gpu_u32(data+node+16);size_t len=0;while(token[len] && token[len]!='.')len++;
        while(offset){
            if(!budget || offset>bytes-21 || (offset&3))return 0;
            budget--;
            uint32_t n=gpu_u32(data+offset);if(n>bytes-offset-21 || data[offset+20+n])return 0;
            int cmp=len<n?-1:len>n?1:0;
            if(!cmp)for(size_t j=0;j<len;j++)if(token[j]!=data[offset+20+j]){cmp=(unsigned char)token[j]-(unsigned char)data[offset+20+j];break;}
            if(!cmp)break;
            offset=gpu_u32(data+offset+(cmp<0?8:12));
        }
        if(!offset)return 0;
        node=offset;token+=len;
        if(*token=='.'){token++;continue;}
        offset=gpu_u32(data+node+4);
        size_t namelen=p_len(name);
        if(!offset || (offset&3) || offset>bytes || bytes-offset<97+namelen)return 0;
        for(size_t j=0;j<=namelen;j++)if(data[offset+96+j]!=name[j])return 0;
        uint32_t serial=gpu_u32(data+offset);unsigned n=serial>>24;
        if((serial&0x10001) || n>=92)return 0;
        p_copy(value,data+offset+4,n);value[n]=0;
        return serial==gpu_u32(data+offset);
    }
    return 0;
}
static size_t gpu_number(char *out,unsigned long number){char rev[24];size_t n=0,k=0;do{rev[n++]=(char)('0'+number%10);number/=10;}while(number);while(n)out[k++]=rev[--n];out[k]=0;return k;}
static long gpu_dir(const char *base,const char *name){char *p=join(base,name);if(!p)return -ENOMEM;long r=RAW(PROXY_SC_mkdirat,AT_FDCWD,p,0700,0,0,0);vfs_free(p);return r;}
static long gpu_file(const char *base,const char *name,const char *value){char *p=join(base,name);if(!p)return -ENOMEM;long fd=RAW(PROXY_SC_openat,AT_FDCWD,p,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0400,0,0);vfs_free(p);if(fd<0)return fd;size_t n=p_len(value);long r=RAW(PROXY_SC_write,fd,value,n,0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);return r==(long)n?0:r<0?r:-EIO;}
static long gpu_link(const char *base,const char *name,const char *target){char *p=join(base,name);if(!p)return -ENOMEM;long r=RAW(PROXY_SC_symlinkat,target,AT_FDCWD,p,0,0,0);vfs_free(p);return r;}
static int gpu_hex(const char *p){if(p_len(p)!=6 || p[0]!='0' || p[1]!='x')return 0;for(unsigned i=2;i<6;i++)if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')||(p[i]>='A'&&p[i]<='F')))return 0;return 1;}
static void gpu_append(char *dst,const char *src){p_copy(dst+p_len(dst),src,p_len(src)+1);}
static long gpu_node(const char *base,const char *name,unsigned minor){
    char child[128]="devices/pci0000:00/0000:00:00.0/drm/";p_copy(child+p_len(child),name,p_len(name)+1);
    long r=gpu_dir(base,child);if(r)return r;
    char *dir=join(base,child);if(!dir)return -ENOMEM;
    char number[24];gpu_number(number,minor);char event[160]="MAJOR=226\nMINOR=";
    gpu_append(event,number);gpu_append(event,"\nDEVNAME=dri/");gpu_append(event,name);gpu_append(event,"\n");
    r=gpu_file(dir,"uevent",event);if(!r)r=gpu_link(dir,"device","../..");if(!r)r=gpu_link(dir,"subsystem","../../../../../class/drm");vfs_free(dir);if(r)return r;
    char alias[64]="dev/char/226:";p_copy(alias+p_len(alias),number,p_len(number)+1);
    char target[160]="../../devices/pci0000:00/0000:00:00.0/drm/";p_copy(target+p_len(target),name,p_len(name)+1);
    r=gpu_link(base,alias,target);if(r)return r;
    char cls[128]="class/drm/";gpu_append(cls,name);
    return gpu_link(base,cls,target);
}
/* Separate entry point also lets host fixtures exercise precisely the tree
 * publication code without requiring a host GPU or Android property service. */
long vfs_gpu_publish(const char *base,const char *vendor,const char *device,const char *driver,const char *render,unsigned render_minor,const char *card,unsigned card_minor){
    const char *dirs[]={"dev","dev/char","devices","devices/pci0000:00","devices/pci0000:00/0000:00:00.0","devices/pci0000:00/0000:00:00.0/drm","bus","bus/pci","class","class/drm"};
    long r=0;for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++)if((r=gpu_dir(base,dirs[i])))return r;
    char *pci=join(base,"devices/pci0000:00/0000:00:00.0");if(!pci)return -ENOMEM;
    const char *names[]={"vendor","device","subsystem_vendor","subsystem_device","revision","class"};
    const char *values[]={vendor,device,"0x0000","0x0000","0x00","0x030000"};
    for(unsigned i=0;i<6 && !r;i++){char line[16];p_copy(line,values[i],p_len(values[i]));p_copy(line+p_len(values[i]),"\n",2);r=gpu_file(pci,names[i],line);}
    char event[256]="DRIVER=",id[5];gpu_append(event,driver);gpu_append(event,"\nPCI_ID=");
    p_copy(id,vendor+2,4);id[4]=0;gpu_append(event,id);gpu_append(event,":");
    p_copy(id,device+2,4);gpu_append(event,id);gpu_append(event,"\nPCI_SLOT_NAME=0000:00:00.0\n");
    if(!r)r=gpu_file(pci,"uevent",event);
    if(!r)r=gpu_link(pci,"subsystem","../../../bus/pci");
    vfs_free(pci);
    if(!r)r=gpu_node(base,render,render_minor);
    if(!r && card)r=gpu_node(base,card,card_minor);
    return r;
}
void vfs_gpu_init(void){
    long e=0;char *path=vfs_resolve(AT_FDCWD,"/dev/__properties__/u:object_r:vendor_maton_graphics_prop:s0",1,&e);if(!path)return;
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY|O_CLOEXEC,0,0,0);vfs_free(path);if(fd<0)return;
    struct proxy_stat st;if(arch_stat_fd(fd,&st) || st.st_size<148){RAW(PROXY_SC_close,fd,0,0,0,0,0);return;}
    char *area=(void *)RAW(PROXY_SC_mmap,0,st.st_size,PROT_READ,MAP_SHARED,fd,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);if((unsigned long)area>=(unsigned long)-4095)return;
    char vendor[92],device[92],driver[92],render[92],mode[92];
    int ok=gpu_property(area,st.st_size,"vendor.maton.graphics.vendor",vendor)&&gpu_property(area,st.st_size,"vendor.maton.graphics.device",device)&&gpu_property(area,st.st_size,"vendor.maton.graphics.driver",driver)&&gpu_property(area,st.st_size,"vendor.maton.graphics.render_node",render)&&gpu_property(area,st.st_size,"vendor.maton.graphics.mode",mode);
    RAW(PROXY_SC_munmap,area,st.st_size,0,0,0,0);
    if(!ok || !p_equal(mode,"hardware") || !gpu_hex(vendor) || !gpu_hex(device) || !driver[0] || p_equal(driver,"unknown") || p_len(render)<17)return;
    for(size_t i=0;driver[i];i++)if(!((driver[i]>='a'&&driver[i]<='z')||(driver[i]>='0'&&driver[i]<='9')||driver[i]=='_'||driver[i]=='-'))return;
    const char *prefix="/dev/dri/renderD";for(unsigned i=0;i<16;i++)if(render[i]!=prefix[i])return;
    for(unsigned i=16;render[i];i++)if(render[i]<'0'||render[i]>'9')return;
    path=vfs_resolve(AT_FDCWD,render,1,&e);if(!path)return;
    fd=RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY|O_CLOEXEC,0,0,0);vfs_free(path);if(fd<0)return;
    e=arch_stat_fd(fd,&st);RAW(PROXY_SC_close,fd,0,0,0,0,0);
    if(e)return;
    unsigned maj=(st.st_rdev>>8)&0xfff,min=(st.st_rdev&255)|((st.st_rdev>>12)&0xfffff00);
    if(!S_ISCHR(st.st_mode) || maj!=226 || min<128)return;
    /* Only publish a card when /dev/dri has exactly one primary and one render node. The
     * detector does not export its card identity; render/card indices are
     * not necessarily paired. Never invent a multi-GPU association. */
    char card[64]={0};unsigned card_minor=0,count=0,render_count=0;
    path=vfs_resolve(AT_FDCWD,"/dev/dri",1,&e);
    long dri=path?RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,0,0):-ENOENT;vfs_free(path);
    if(dri>=0){
        char buf[4096];long n;
        while((n=RAW(PROXY_SC_getdents64,dri,buf,sizeof(buf),0,0,0))>0){
            for(long off=0;off<n;){
                unsigned short len=*(unsigned short *)(buf+off+16);
                if(len<20 || len>n-off)break;
                const char *name=buf+off+19;
                int primary=name[0]=='c'&&name[1]=='a'&&name[2]=='r'&&name[3]=='d';
                int rendering=name[0]=='r'&&name[1]=='e'&&name[2]=='n'&&name[3]=='d';
                if((primary || rendering) && p_len(name)<sizeof(card)){
                    long c=RAW(PROXY_SC_openat,dri,name,O_RDONLY|O_CLOEXEC,0,0,0);struct proxy_stat cs;
                    if(c>=0){
                        if(!arch_stat_fd(c,&cs) && S_ISCHR(cs.st_mode) && ((cs.st_rdev>>8)&0xfff)==226){
                            unsigned cm=(cs.st_rdev&255)|((cs.st_rdev>>12)&0xfffff00);
                            if(primary && cm<64){count++;p_copy(card,name,p_len(name)+1);card_minor=cm;}
                            else if(rendering && cm>=128)render_count++;
                        }
                        RAW(PROXY_SC_close,c,0,0,0,0,0);
                    }
                }
                off+=len;
            }
        }
        if(n<0){count=0;render_count=0;}
        RAW(PROXY_SC_close,dri,0,0,0,0,0);
    }
    extern const char *proxy_tmpdir;char suffix[80]="v3-gpu-",number[24];gpu_number(number,RAW(PROXY_SC_getpid,0,0,0,0,0,0));p_copy(suffix+7,number,p_len(number)+1);unsigned long random=0;if(RAW(PROXY_SC_getrandom,&random,sizeof(random),0,0,0,0)!=sizeof(random))return;size_t k=p_len(suffix);suffix[k++]='-';gpu_number(suffix+k,random);
    char *base=join(proxy_tmpdir,suffix);if(!base)return;
    if(!RAW(PROXY_SC_mkdirat,AT_FDCWD,base,0700,0,0,0) && !vfs_gpu_publish(base,vendor,device,driver,render+9,min,count==1 && render_count==1?card:NULL,card_minor)){
        struct mount *prior=table;
        const char *trees[]={"dev","devices","bus","class"};
        for(unsigned i=0;i<4;i++){
            char *src=join(base,trees[i]),*dst=join("/sys",trees[i]);
            e=src&&dst?vfs_mount(src,dst,NULL,MS_BIND|MS_RDONLY):-ENOMEM;
            if(!e && i>=2){vfs_free(gpu_poll_sources[i-2]);gpu_poll_sources[i-2]=pdupstr(src);}
            vfs_free(src);vfs_free(dst);if(e)break;
        }
        if(e){while(table!=prior){struct mount *old=table;table=old->next;vfs_free(old->source);vfs_free(old->target);vfs_free(old);}for(unsigned i=0;i<2;i++){vfs_free(gpu_poll_sources[i]);gpu_poll_sources[i]=NULL;}}

    }
    vfs_free(base);
}

#include "state.h"
static void pack_state(struct state *);
void vfs_state(struct state *s){
 pack_state(s);state_string(s,&oldroot_view);
 state_string(s,&root);state_string(s,&cwd);state_string(s,&executable);
 stacked_oldroot=state_word(s,stacked_oldroot);hide_resolver=state_word(s,hide_resolver);
 for(unsigned i=0;i<2;i++)state_string(s,&gpu_poll_sources[i]);
 uint64_t count=0;for(struct mount *m=table;m;m=m->next)count++;
 count=state_count(s,count);if(s->error)return;
 struct mount **tail=&table;
 for(uint64_t i=0;i<count&&!s->error;i++){
  struct mount *m=s->reading?vfs_palloc(sizeof(*m)):*tail;
  if(!m){s->error=-ENOMEM;return;}if(s->reading){*tail=m;m->next=NULL;m->source=m->target=NULL;}
  state_string(s,&m->source);state_string(s,&m->target);m->flags=state_word(s,m->flags);m->pack_shadow=state_word(s,m->pack_shadow);tail=&m->next;
 }
 count=0;for(size_t fd=0;fd<fd_names_capacity;fd++)if(fd_names[fd].length && state_live((int)fd))count++;
 count=state_count(s,count);if(s->error)return;
 if(s->reading)for(uint64_t i=0;i<count&&!s->error;i++){
  uint64_t rawfd=state_word(s,0);char *path=NULL;state_string(s,&path);
  if(s->error){vfs_free(path);return;}
  if(rawfd>INT_MAX){vfs_free(path);s->error=-EBADF;return;}
  int fd=(int)rawfd;int e=fd_names_reserve((size_t)fd+1);if(e){vfs_free(path);s->error=e;return;}
  struct fd_name *d=&fd_names[fd];size_t n=p_len(path)+1;
  if(d->path && n<=d->capacity){p_copy(d->path,path,n);d->length=n;vfs_free(path);}
  else {vfs_free(d->path);d->path=path;d->capacity=n;d->length=n;}
  if(!state_live(fd))s->error=-EBADF;
 }else for(size_t fd=0;fd<fd_names_capacity;fd++)if(fd_names[fd].length && state_live((int)fd)){state_word(s,fd);state_string(s,&fd_names[fd].path);}
 count=0;for(struct temp_lock *l=temp_locks;l;l=l->next)count++;
 count=state_count(s,count);if(s->error)return;
 if(s->reading)for(uint64_t i=0;i<count&&!s->error;i++){
  struct temp_lock *l=vfs_palloc(sizeof(*l));if(!l){s->error=-ENOMEM;return;}
  l->fd=state_word(s,0);l->next=temp_locks;temp_locks=l;if(!state_live(l->fd))s->error=-EBADF;
 }else for(struct temp_lock *l=temp_locks;l;l=l->next)state_word(s,l->fd);
}

#include "pack-vfd.c"

