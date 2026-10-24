/* One atomic decision file per object/site in the stub's cache directory.
 * Cache I/O is raw: virtual mounts and the proxy's own traps must not recurse. */
static char rewrite_cache_dir[1024];
struct rewrite_key {uint64_t ino,size,mtime,ctime,slice,site;uint32_t major,minor,mns,cns,build_length;unsigned char build_id[32];};
struct rewrite_disk {uint64_t magic;struct rewrite_key key;uint64_t function,length;uint32_t crc,reason,count;struct rewrite_site site;};
static uint32_t rewrite_crc(const unsigned char *p,size_t n){
    uint32_t crc=~0U;
    for(size_t i=0;i<n;i++)__asm__("crc32b %1,%0":"+r"(crc):"m"(p[i]));
    return ~crc;
}
/* Normalize only our registered edits so a second site in the same function
 * sees the original proof stream. All other live bytes remain authoritative. */
static unsigned char *rewrite_function_bytes(const unsigned char *p,size_t n){
    unsigned char *copy=vfs_alloc(n);if(!copy)return NULL;p_copy(copy,p,n);
    uintptr_t begin=(uintptr_t)p;
    for(struct rewrite_record *r=rewrite_records;r;r=r->next)if(r->active && r->address>=begin && r->window<=n && r->address-begin<=n-r->window){
        unsigned char expected[128];p_copy(expected,r->original,r->window);arch_rewrite_patch(expected,r->nr,r->window);
        size_t off=r->address-begin,j=0;while(j<r->window && copy[off+j]==expected[j])j++;
        /* Restore only a window that still exactly matches our publication. */
        if(j==r->window)p_copy(copy+off,r->original,r->window);
    }
    return copy;
}
static uint32_t rewrite_function_crc(const unsigned char *p,size_t n){
    unsigned char *copy=rewrite_function_bytes(p,n);if(!copy)return 0;
    uint32_t crc=rewrite_crc(copy,n);vfs_free(copy);return crc;
}
static void rewrite_key_fill(struct rewrite_key *k,const struct statx *s,uint64_t slice,uint64_t site){
    *k=(struct rewrite_key){.ino=s->stx_ino,.size=s->stx_size,.mtime=s->stx_mtime.tv_sec,.ctime=s->stx_ctime.tv_sec,.slice=slice,.site=site,.major=s->stx_dev_major,.minor=s->stx_dev_minor,.mns=s->stx_mtime.tv_nsec,.cns=s->stx_ctime.tv_nsec};
    Elf64_Ehdr eh;int fd=rewrite_current->fd;
    if(pack_pread(fd,&eh,sizeof(eh),0)!=sizeof(eh) || eh.e_phentsize!=sizeof(Elf64_Phdr) || eh.e_phnum>256)return;
    for(unsigned i=0;i<eh.e_phnum;i++){
        Elf64_Phdr ph;
        if(pack_pread(fd,&ph,sizeof(ph),eh.e_phoff+i*sizeof(ph))!=sizeof(ph))return;
        if(ph.p_type!=PT_NOTE || ph.p_filesz>4096)continue;
        unsigned char notes[4096];
        if(pack_pread(fd,notes,ph.p_filesz,ph.p_offset)!=(long)ph.p_filesz)continue;
        for(size_t at=0;at+12<=ph.p_filesz;){
            uint32_t name,length,type;p_copy(&name,notes+at,4);p_copy(&length,notes+at+4,4);p_copy(&type,notes+at+8,4);at+=12;
            if(name>ph.p_filesz-at || (((uint64_t)name+3)&~3ULL)>ph.p_filesz-at)break;
            size_t desc=at+((name+3U)&~3U);
            if(length>ph.p_filesz-desc)break;
            if(type==3 && name==4 && notes[at]=='G' && notes[at+1]=='N' && notes[at+2]=='U' && length && length<=sizeof(k->build_id)){
                k->build_length=length;p_copy(k->build_id,notes+desc,length);return;
            }
            at=desc+(((uint64_t)length+3)&~3ULL);
        }
    }
}
static int rewrite_cache_name(char *path,const struct rewrite_key *key,int temporary){
    if(!rewrite_cache_dir[0])return 0;
    size_t n=p_len(rewrite_cache_dir);p_copy(path,rewrite_cache_dir,n);path[n++]='/';
    uint64_t hash=1469598103934665603ULL;
    const unsigned char *bytes=(const void *)key;
    for(size_t i=0;i<sizeof(*key);i++){hash^=bytes[i];hash*=1099511628211ULL;}
    const char *hex="0123456789abcdef";
    for(int i=15;i>=0;i--)path[n++]=hex[(hash>>(4*i))&15];
    if(temporary){path[n++]='.';uint64_t pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0);for(int i=15;i>=0;i--)path[n++]=hex[(pid>>(4*i))&15];}
    path[n]=0;return 1;
}
void proxy_rewrite_cache_init(const char *root){
    /* Resident exec keeps the real per-stub root even when Flatpak replaces
     * XDG_CACHE_HOME with a virtual sandbox path. */
    if(rewrite_cache_dir[0])return;
    if(!root || root[0]!='/' || p_len(root)>sizeof(rewrite_cache_dir)-32)return;
    size_t n=p_len(root);p_copy(rewrite_cache_dir,root,n);
    p_copy(rewrite_cache_dir+n,"/maton-rewrite-v2",sizeof("/maton-rewrite-v2"));
    RAW(PROXY_SC_mkdirat,AT_FDCWD,rewrite_cache_dir,0700,0,0,0);
}
static int rewrite_cache_read(struct rewrite_scan *scan,uintptr_t begin){
    struct rewrite_disk disk;struct rewrite_key key;char path[1100];
    rewrite_key_fill(&key,&scan->identity,scan->slice,scan->offset+scan->target);
    if(!rewrite_cache_name(path,&key,0))return 0;
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY|O_CLOEXEC,0,0,0);if(fd<0)return 0;
    long got=RAW(PROXY_SC_read,fd,&disk,sizeof(disk),0,0,0);char tail;
    long extra=RAW(PROXY_SC_read,fd,&tail,1,0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);
    if(got!=sizeof(disk) || extra || disk.magic!=0x32574445524e544dULL || disk.count>1 || disk.reason>1)return 0;
    const unsigned char *a=(const void *)&key,*b=(const void *)&disk.key;
    for(size_t i=0;i<sizeof(key);i++)if(a[i]!=b[i])return 0;
    if(disk.function<scan->offset || !disk.length || disk.length>scan->size || disk.function-scan->offset>scan->size-disk.length)return 0;
    uintptr_t function=begin+disk.function-scan->offset;
    if(rewrite_target<function || rewrite_target-function>=disk.length || rewrite_function_crc((void *)function,disk.length)!=disk.crc)return 0;
    if(disk.count){
        if(disk.site.window<7 || disk.site.window>sizeof(disk.site.original) || disk.site.nr!=rewrite_nr || disk.site.offset<scan->offset ||
           disk.site.offset-scan->offset>scan->size-disk.site.window || disk.site.offset+disk.site.window!=scan->offset+scan->target+2)return 0;
        disk.site.offset-=(uint32_t)scan->offset;
        rewrite_apply(begin+disk.site.offset,disk.site.nr,disk.site.window,disk.site.original);
    }
    if(rewrite_log)proxy_log("rewrite-persistent-hit",scan->offset+scan->target,disk.count);
    return 1;
}
static void rewrite_cache_write(struct rewrite_scan *scan){
    if(!scan->function_size || scan->count>1 || scan->offset>UINT32_MAX)return;
    struct rewrite_disk disk={0};char path[1100],temp[1100];
    disk.magic=0x32574445524e544dULL;
    rewrite_key_fill(&disk.key,&scan->identity,scan->slice,scan->offset+scan->target);
    if(!rewrite_cache_name(path,&disk.key,0) || !rewrite_cache_name(temp,&disk.key,1))return;
    disk.function=scan->function_offset+scan->offset;disk.length=scan->function_size;disk.crc=scan->function_crc;
    disk.count=scan->count;disk.reason=!scan->count;
    if(scan->count){disk.site=scan->sites[0];disk.site.offset+=(uint32_t)scan->offset;}
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600,0,0);if(fd<0)return;
    long wrote=RAW(PROXY_SC_write,fd,&disk,sizeof(disk),0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);
    if(wrote==sizeof(disk))RAW(PROXY_SC_renameat,AT_FDCWD,temp,AT_FDCWD,path,0,0);
    RAW(PROXY_SC_unlinkat,AT_FDCWD,temp,0,0,0,0);
}
