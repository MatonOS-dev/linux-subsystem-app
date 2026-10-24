/* Load-time ELF scanner. This file intentionally has no architecture tests;
 * instruction proof and entry generation are selected by the backend. */
#ifdef ARCH_REWRITE_HEADER
#include ARCH_REWRITE_HEADER
static int rewrite_enabled;
struct rewrite_site {uint32_t offset;unsigned nr,window;unsigned char original[9];};
struct rewrite_scan {struct rewrite_scan *next;struct statx identity;uint64_t offset,slice;size_t size;unsigned count;struct rewrite_site sites[512];};
static struct rewrite_scan *rewrite_scans;
static unsigned rewrite_cache_count;
static unsigned rewrite_busy;
static int proxy_rewrite_traps(long nr){return rewrite_enabled && (nr==PROXY_SC_mprotect || nr==PROXY_SC_munmap || nr==PROXY_SC_mremap);}
static uint64_t rewrite_clock(void){
    struct {int64_t sec,nsec;} t={0};
    if(RAW(PROXY_SC_clock_gettime,1,&t,0,0,0,0)<0)return 0;
    return (uint64_t)t.sec*1000000000+(uint64_t)t.nsec;
}
static unsigned rewrite_apply(uintptr_t addr,unsigned nr,unsigned w,const unsigned char *original){
    if((addr&4095)+w>4096)return 0;
    const unsigned char *old=(void *)addr;
    for(unsigned j=0;j<w;j++)if(old[j]!=original[j])return 0;
    long page=RAW(PROXY_SC_mmap,0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if((unsigned long)page>=(unsigned long)-4095)return 0;
    p_copy((void *)(uintptr_t)page,(void *)(addr&~(uintptr_t)4095),4096);
    arch_rewrite_patch((unsigned char *)(uintptr_t)page+(addr&4095),nr,w);
    if(RAW(PROXY_SC_mprotect,page,4096,PROT_READ|PROT_EXEC,0,0,0)>=0 &&
       RAW(PROXY_SC_mremap,page,4096,4096,MREMAP_MAYMOVE|MREMAP_FIXED,addr&~(uintptr_t)4095,0)>=0)return 1;
    RAW(PROXY_SC_munmap,page,4096,0,0,0,0);return 0;
}
int proxy_rewrite_init(const char *s){
    if(!s || !p_equal(s,"MATON_PROXY_REWRITE=1"))return 0;
    rewrite_enabled=arch_rewrite_init();
    proxy_rewrite_table=arch_rewrite_table;
    return rewrite_enabled;
}
/* A complete bounded symbol is the unit of proof. Unsupported instructions,
 * stack use, indirect flow or interior branch targets reject it atomically. */
static unsigned rewrite_function(unsigned char *p,size_t size,struct rewrite_scan *scan,uintptr_t begin,const Elf64_Sym *symbol,const Elf64_Sym *all,size_t nsymbols){
    struct rewrite_insn ins[512];size_t offsets[512],pos=0,count=0;
    if(size>4096 || !size)return 0;
    while(pos<size){
        if(count==512 || !arch_rewrite_decode(p+pos,size-pos,&ins[count]))return 0;
        offsets[count]=pos;pos+=ins[count++].size;
    }
    for(size_t j=0;j<count;j++)if(ins[j].target!=INT64_MIN){
        int64_t target=(int64_t)offsets[j]+ins[j].target;
        if(target>=0 && (uint64_t)target<size){
            int boundary=0;
            for(size_t k=0;k<count;k++)if((uint64_t)target==offsets[k])boundary=1;
            if(!boundary)return 0;
        }
        for(size_t k=0;k<count;k++)if(ins[k].window &&
            target>(int64_t)offsets[k] && target<(int64_t)(offsets[k]+ins[k].window))return 0;
    }
    int interesting=0;
    for(size_t k=0;k<count;k++)if(ins[k].window)interesting=1;
    if(!interesting)return 0;
    /* Aliases with identical bounds are fine. Partial/contained overlaps from
     * either ELF symbol table make the instruction stream ambiguous. */
    for(size_t j=0;j<nsymbols;j++){
        const Elf64_Sym *other=&all[j];
        if((ELF64_ST_TYPE(other->st_info)!=STT_FUNC && ELF64_ST_TYPE(other->st_info)!=STT_GNU_IFUNC) ||
           other->st_shndx!=symbol->st_shndx || !other->st_size)continue;
        if(other->st_value==symbol->st_value && other->st_size==symbol->st_size)continue;
        if(other->st_value>UINT64_MAX-other->st_size)return 0;
        if(other->st_value<symbol->st_value+symbol->st_size && other->st_value+other->st_size>symbol->st_value)return 0;
    }
    unsigned changed=0;
    for(size_t k=0;k<count;k++)if(ins[k].window){
        uintptr_t addr=(uintptr_t)p+offsets[k];
        unsigned w=ins[k].window;
        if((addr&4095)+w>4096)continue;
        unsigned char original[9];p_copy(original,(void *)addr,w);
        if(scan && scan->count<512 && addr-begin<=UINT32_MAX){
            struct rewrite_site *site=&scan->sites[scan->count++];
            site->offset=(uint32_t)(addr-begin);site->nr=ins[k].nr;site->window=w;
            p_copy(site->original,original,w);
        }
        changed+=rewrite_apply(addr,ins[k].nr,w,original);
    }
    return changed;
}
/* File offsets rather than load biases also cover independently mapped PT_LOADs.
 * pack_pread uses logical file offsets for packed ELF carriers. */
void proxy_rewrite_mapping(int fd,uintptr_t begin,size_t size,uint64_t offset){
    if(!rewrite_enabled || !size || begin>UINTPTR_MAX-size)return;
    /* Try-lock, never wait in a signal frame (or after a concurrent fork).
     * Cache contention merely leaves this mapping on SIGSYS. */
    if(__atomic_exchange_n(&rewrite_busy,1,__ATOMIC_ACQUIRE))return;
    uint64_t started=rewrite_clock();unsigned patched=0;
    struct statx identity={0};struct rewrite_scan *scan=NULL;
    int identified=RAW(PROXY_SC_statx,fd,"",AT_EMPTY_PATH,STATX_BASIC_STATS,&identity,0)==0;
    uint64_t slice=pack_slice_start(fd);
    if(identified)for(scan=rewrite_scans;scan;scan=scan->next){
        struct statx *a=&scan->identity,*b=&identity;
        if(scan->offset==offset && scan->size==size && scan->slice==slice &&
           a->stx_ino==b->stx_ino && a->stx_dev_major==b->stx_dev_major && a->stx_dev_minor==b->stx_dev_minor &&
           a->stx_size==b->stx_size && a->stx_mtime.tv_sec==b->stx_mtime.tv_sec &&
           a->stx_mtime.tv_nsec==b->stx_mtime.tv_nsec && a->stx_ctime.tv_sec==b->stx_ctime.tv_sec &&
           a->stx_ctime.tv_nsec==b->stx_ctime.tv_nsec)break;
    }
    if(scan){
        for(unsigned i=0;i<scan->count;i++){
            struct rewrite_site *site=&scan->sites[i];
            patched+=rewrite_apply(begin+site->offset,site->nr,site->window,site->original);
        }
        if(proxy_trace)proxy_log("rewrite-cache-hit",scan->count,size);
        goto done;
    }
    if(identified && rewrite_cache_count<64){
        scan=vfs_palloc(sizeof(*scan));
        if(scan){scan->identity=identity;scan->offset=offset;scan->size=size;scan->slice=slice;}
    }
    Elf64_Ehdr eh;Elf64_Shdr *sections=NULL;Elf64_Sym *symbols=NULL;
    if(pack_pread(fd,&eh,sizeof(eh),0)!=sizeof(eh) ||
       eh.e_ident[0]!=127 || eh.e_ident[1]!='E' || eh.e_ident[2]!='L' || eh.e_ident[3]!='F' ||
       eh.e_ident[EI_CLASS]!=ELFCLASS64 || eh.e_machine!=ARCH_ELF_MACHINE ||
       eh.e_shentsize!=sizeof(Elf64_Shdr) || !eh.e_shnum || eh.e_shnum>4096)goto cleanup;
    size_t section_bytes=(size_t)eh.e_shnum*sizeof(*sections);
    if(eh.e_shoff>UINT64_MAX-section_bytes)goto cleanup;
    sections=vfs_alloc(section_bytes);
    if(!sections || pack_pread(fd,sections,section_bytes,eh.e_shoff)!=(long)section_bytes)goto cleanup;
    size_t symbol_bytes=0;
    for(unsigned i=0;i<eh.e_shnum;i++){
        Elf64_Shdr *sh=&sections[i];
        if((sh->sh_type==SHT_SYMTAB || sh->sh_type==SHT_DYNSYM) && sh->sh_entsize==sizeof(Elf64_Sym)){
            if(sh->sh_size>16*1024*1024-symbol_bytes || sh->sh_offset>UINT64_MAX-sh->sh_size)goto cleanup;
            symbol_bytes+=sh->sh_size;
        }
    }
    if(!symbol_bytes)goto cleanup;
    symbols=vfs_alloc(symbol_bytes);if(!symbols)goto cleanup;
    size_t used=0;
    for(unsigned i=0;i<eh.e_shnum;i++){
        Elf64_Shdr *sh=&sections[i];
        if((sh->sh_type!=SHT_SYMTAB && sh->sh_type!=SHT_DYNSYM) || sh->sh_entsize!=sizeof(Elf64_Sym))continue;
        if(sh->sh_size%sizeof(Elf64_Sym) || pack_pread(fd,(unsigned char *)symbols+used,sh->sh_size,sh->sh_offset)!=(long)sh->sh_size)goto cleanup;
        used+=sh->sh_size;
    }
    for(size_t j=0;j<used/sizeof(*symbols);j++){
        Elf64_Sym sym=symbols[j];
        if(ELF64_ST_TYPE(sym.st_info)!=STT_FUNC || !sym.st_size || sym.st_size>4096 ||
           !sym.st_shndx || sym.st_shndx>=eh.e_shnum || sym.st_value>UINT64_MAX-sym.st_size)continue;
        Elf64_Shdr text=sections[sym.st_shndx];
        if(text.sh_type!=SHT_PROGBITS || (text.sh_flags&(SHF_EXECINSTR|SHF_ALLOC))!=(SHF_EXECINSTR|SHF_ALLOC) || sym.st_value<text.sh_addr ||
           sym.st_value-text.sh_addr>text.sh_size || sym.st_size>text.sh_size-(sym.st_value-text.sh_addr))continue;
        if(text.sh_offset>UINT64_MAX-(sym.st_value-text.sh_addr))continue;
        uint64_t f=text.sh_offset+sym.st_value-text.sh_addr;
        if(f<offset || f-offset>=size || sym.st_size>size-(f-offset))continue;
        patched+=rewrite_function((unsigned char *)(begin+(uintptr_t)(f-offset)),sym.st_size,scan,begin,&sym,symbols,used/sizeof(*symbols));
    }
cleanup:
    vfs_free(symbols);vfs_free(sections);
    if(scan){scan->next=rewrite_scans;rewrite_scans=scan;rewrite_cache_count++;}
done:
    if(proxy_trace){proxy_log("rewrite-sites",patched,size);proxy_log("rewrite-scan-ns",rewrite_clock()-started,size);}
    __atomic_store_n(&rewrite_busy,0,__ATOMIC_RELEASE);
}
/* Track only unpublished, readable, private file mappings. Keep no duplicate
 * descriptor: if the loader closes/reuses it before mprotect, identity failure
 * leaves the range trapped. Records contain no persistent pointer into text. */
struct rewrite_pending {uintptr_t begin;size_t size;uint64_t offset;int fd,prot;struct proxy_stat identity;};
static struct rewrite_pending rewrite_pending[128];
static unsigned rewrite_pending_busy;
static void proxy_rewrite_after(long nr,proxy_arg_t *a,long result){
    if(!rewrite_enabled || result<0 ||
       (nr!=PROXY_SC_mmap && nr!=PROXY_SC_munmap && nr!=PROXY_SC_mprotect && nr!=PROXY_SC_mremap))return;
    if(__atomic_exchange_n(&rewrite_pending_busy,1,__ATOMIC_ACQUIRE))return;
    if(nr==PROXY_SC_mremap){
        for(unsigned i=0;i<128;i++)rewrite_pending[i].size=0;
        goto done;
    }
    uintptr_t begin=nr==PROXY_SC_mmap?(uintptr_t)result:(uintptr_t)a[0];
    size_t size=(size_t)a[1];
    if(size>SIZE_MAX-4095 || begin>UINTPTR_MAX-((size+4095)&~(size_t)4095))goto done;
    size=(size+4095)&~(size_t)4095;
    for(unsigned i=0;i<128;i++){
        struct rewrite_pending *p=&rewrite_pending[i];
        if(!p->size || begin>=p->begin+p->size || begin+size<=p->begin)continue;
        if(nr==PROXY_SC_mprotect && begin>=p->begin && begin+size<=p->begin+p->size){
            struct proxy_stat now;
            if(!(p->prot&PROT_EXEC) && (p->prot&PROT_READ) &&
               (a[2]&(PROT_READ|PROT_EXEC))==(PROT_READ|PROT_EXEC) && !(a[2]&PROT_WRITE) && !arch_stat_fd(p->fd,&now) &&
               now.st_dev==p->identity.st_dev && now.st_ino==p->identity.st_ino && now.st_size==p->identity.st_size)
                proxy_rewrite_mapping(p->fd,begin,size,p->offset+begin-p->begin);
        }
        /* Partial overlaps invalidate the record conservatively. */
        p->size=0;
    }
    if(nr==PROXY_SC_mmap && !(a[3]&MAP_ANONYMOUS) && (a[3]&MAP_PRIVATE) &&
       !(a[2]&PROT_EXEC) && (a[2]&PROT_READ)){
        for(unsigned i=0;i<128;i++)if(!rewrite_pending[i].size){
            struct proxy_stat st;
            if(!arch_stat_fd((int)a[4],&st))rewrite_pending[i]=(struct rewrite_pending){begin,size,(uint64_t)a[5],(int)a[4],(int)a[2],st};
            break;
        }
    }
done:
    __atomic_store_n(&rewrite_pending_busy,0,__ATOMIC_RELEASE);
}

#else
static long rewrite_slow(long nr,proxy_arg_t *a){(void)nr;(void)a;return -ENOSYS;}
static int proxy_rewrite_traps(long nr){(void)nr;return 0;}
static void proxy_rewrite_after(long nr,proxy_arg_t *a,long r){(void)nr;(void)a;(void)r;}
int proxy_rewrite_init(const char *s){(void)s;return 0;}
void proxy_rewrite_mapping(int fd,uintptr_t b,size_t n,uint64_t o){(void)fd;(void)b;(void)n;(void)o;}
#endif
