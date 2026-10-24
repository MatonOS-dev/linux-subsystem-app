/* Load-time ELF scanner. This file intentionally has no architecture tests;
 * instruction proof and entry generation are selected by the backend. */
#ifdef ARCH_REWRITE_HEADER
#include ARCH_REWRITE_HEADER
static int rewrite_enabled;
struct rewrite_site {uint32_t offset;unsigned nr,window;unsigned char original[9];};
struct rewrite_scan {struct rewrite_scan *next;struct statx identity;uint64_t offset,slice;size_t size,count,capacity;struct rewrite_site *sites;};
static struct rewrite_scan *rewrite_scans;
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
/* A complete ELF or FDE range is the unit of proof. Unsupported instructions,
 * unproven stack state, indirect flow or interior branch targets reject it. */
static int rewrite_redzone_path_clean(const struct rewrite_insn *ins,const size_t *offsets,size_t count,size_t size,size_t start){
    unsigned char *seen=vfs_alloc(count);size_t *queue=vfs_alloc(count*sizeof(*queue));
    if(!seen || !queue){vfs_free(seen);vfs_free(queue);return 0;}
    size_t head=0,tail=0;queue[tail++]=start;seen[start]=1;int safe=1;
    while(head<tail && safe){size_t k=queue[head++];
        if(ins[k].stack_access || ins[k].stack_delta){safe=0;break;}
        size_t succ[2],ns=0;
        if(ins[k].flow!=2 && ins[k].flow!=1 && k+1<count)succ[ns++]=k+1;
        if(ins[k].target!=INT64_MIN && ins[k].flow!=3){int64_t target=(int64_t)offsets[k]+ins[k].target;
            if(target>=0 && (uint64_t)target<size){size_t q=0;while(q<count && offsets[q]!=(size_t)target)q++;if(q<count)succ[ns++]=q;}}
        for(size_t q=0;q<ns;q++)if(!seen[succ[q]]){seen[succ[q]]=1;queue[tail++]=succ[q];}
    }
    vfs_free(seen);vfs_free(queue);return safe;
}
static unsigned rewrite_function(unsigned char *p,size_t size,struct rewrite_scan *scan,uintptr_t begin,const Elf64_Sym *symbol,const Elf64_Sym *all,size_t nsymbols){
    size_t pos=0,count=0,capacity=size;
    if(!size || capacity>SIZE_MAX/sizeof(struct rewrite_insn) || capacity>SIZE_MAX/sizeof(size_t))return 0;
    struct rewrite_insn *ins=vfs_alloc(capacity*sizeof(*ins));
    size_t *offsets=vfs_alloc(capacity*sizeof(*offsets));
    if(!ins || !offsets){vfs_free(ins);vfs_free(offsets);return 0;}
    while(pos<size){
        if(count==capacity || !arch_rewrite_decode(p+pos,size-pos,&ins[count])){vfs_free(ins);vfs_free(offsets);return 0;}
        offsets[count]=pos;pos+=ins[count++].size;
    }
    for(size_t j=0;j<count;j++)if(ins[j].target!=INT64_MIN && ins[j].flow!=3){
        int64_t target=(int64_t)offsets[j]+ins[j].target;
        if(target>=0 && (uint64_t)target<size){
            int boundary=0;
            for(size_t k=0;k<count;k++)if((uint64_t)target==offsets[k])boundary=1;
            if(!boundary){vfs_free(ins);vfs_free(offsets);return 0;}
        }
        for(size_t k=0;k<count;k++)if(ins[k].window &&
            target>(int64_t)offsets[k] && target<(int64_t)(offsets[k]+ins[k].window)){vfs_free(ins);vfs_free(offsets);return 0;}
    }
    int interesting=0;
    for(size_t k=0;k<count;k++)if(ins[k].window)interesting=1;
    if(!interesting){vfs_free(ins);vfs_free(offsets);return 0;}
    int has_stack=0;
    for(size_t k=0;k<count;k++)if(ins[k].stack_delta || ins[k].stack_access)has_stack=1;
    int64_t *depth=vfs_alloc(count*sizeof(*depth));
    int *clean=vfs_alloc(count*sizeof(*clean));
    if(!depth || !clean){vfs_free(depth);vfs_free(clean);vfs_free(ins);vfs_free(offsets);return 0;}
    for(size_t k=0;k<count;k++){depth[k]=INT64_MIN;clean[k]=-1;}
    depth[0]=0;int changed_depth=1;
    while(changed_depth){
        changed_depth=0;
        for(size_t k=0;k<count;k++)if(depth[k]!=INT64_MIN){
            int64_t out=depth[k];
            if(ins[k].stack_delta==INT_MIN){continue;}
            if((ins[k].stack_delta>0 && out>INT64_MAX-ins[k].stack_delta) ||
               (ins[k].stack_delta<0 && out<INT64_MIN-ins[k].stack_delta)){vfs_free(depth);vfs_free(clean);vfs_free(ins);vfs_free(offsets);return 0;}
            out+=ins[k].stack_delta;
            size_t succ[2],ns=0;
            if(ins[k].flow!=2 && ins[k].flow!=1 && k+1<count)succ[ns++]=k+1;
            if(ins[k].target!=INT64_MIN && ins[k].flow!=3){
                int64_t target=(int64_t)offsets[k]+ins[k].target;
                if(target>=0 && (uint64_t)target<size){size_t q=0;while(q<count && offsets[q]!=(size_t)target)q++;if(q<count && ins[k].flow!=1)succ[ns++]=q;else if(q<count)succ[ns++]=q;}
            }
            for(size_t q=0;q<ns;q++){
                size_t to=succ[q];
                if(depth[to]==INT64_MIN){depth[to]=out;changed_depth=1;}
                else if(depth[to]!=out){vfs_free(depth);vfs_free(clean);vfs_free(ins);vfs_free(offsets);return 0;}
            }
        }
    }
    clean[0]=1;int changed_clean=1;
    while(changed_clean){changed_clean=0;
        for(size_t k=0;k<count;k++)if(clean[k]>=0){
            int out=clean[k] && !ins[k].stack_access && !ins[k].stack_delta;
            size_t succ[2],ns=0;
            if(ins[k].flow!=2 && ins[k].flow!=1 && k+1<count)succ[ns++]=k+1;
            if(ins[k].target!=INT64_MIN && ins[k].flow!=3){int64_t target=(int64_t)offsets[k]+ins[k].target;
                if(target>=0 && (uint64_t)target<size){size_t q=0;while(q<count && offsets[q]!=(size_t)target)q++;if(q<count)succ[ns++]=q;}}
            for(size_t q=0;q<ns;q++){size_t to=succ[q];if(clean[to]<0){clean[to]=out;changed_clean=1;}else if(clean[to] && !out){clean[to]=0;changed_clean=1;}}
        }
    }
    /* Aliases with identical bounds are fine. Partial/contained overlaps from
     * either ELF symbol table make the instruction stream ambiguous. */
    for(size_t j=0;j<nsymbols;j++){
        const Elf64_Sym *other=&all[j];
        if((ELF64_ST_TYPE(other->st_info)!=STT_FUNC && ELF64_ST_TYPE(other->st_info)!=STT_GNU_IFUNC) ||
           other->st_shndx!=symbol->st_shndx || !other->st_size)continue;
        if(other->st_value==symbol->st_value && other->st_size==symbol->st_size)continue;
        if(other->st_value>UINT64_MAX-other->st_size){vfs_free(clean);vfs_free(depth);vfs_free(ins);vfs_free(offsets);return 0;}
        if(other->st_value<symbol->st_value+symbol->st_size && other->st_value+other->st_size>symbol->st_value){vfs_free(clean);vfs_free(depth);vfs_free(ins);vfs_free(offsets);return 0;}
    }
    unsigned changed=0;
    for(size_t k=0;k<count;k++)if(ins[k].window && ((!has_stack) || (depth[k]!=INT64_MIN && depth[k]>0) ||
        (depth[k]==0 && clean[k]==1 && rewrite_redzone_path_clean(ins,offsets,count,size,k)))){
        uintptr_t addr=(uintptr_t)p+offsets[k];
        unsigned w=ins[k].window;
        if((addr&4095)+w>4096)continue;
        unsigned char original[9];p_copy(original,(void *)addr,w);
        if(scan && addr-begin<=UINT32_MAX){
            if(scan->count==scan->capacity){
                size_t next=scan->capacity?scan->capacity*2:16;
                if(next<scan->capacity || next>SIZE_MAX/sizeof(*scan->sites))next=0;
                struct rewrite_site *grown=next?vfs_palloc(next*sizeof(*grown)):NULL;
                if(grown){if(scan->sites){p_copy(grown,scan->sites,scan->count*sizeof(*grown));vfs_free(scan->sites);}scan->sites=grown;scan->capacity=next;}
            }
            if(scan->count<scan->capacity){
                struct rewrite_site *site=&scan->sites[scan->count++];
                site->offset=(uint32_t)(addr-begin);site->nr=ins[k].nr;site->window=w;
                p_copy(site->original,original,w);
            }
        }
        changed+=rewrite_apply(addr,ins[k].nr,w,original);
    }
    vfs_free(clean);vfs_free(depth);vfs_free(ins);vfs_free(offsets);return changed;
}
static int rewrite_uleb(const unsigned char *p,size_t n,size_t *at,uint64_t *value){
    uint64_t v=0;unsigned shift=0;
    while(*at<n && shift<64){unsigned char b=p[(*at)++];v|=(uint64_t)(b&127)<<shift;if(!(b&128)){*value=v;return 1;}shift+=7;}
    return 0;
}
static int rewrite_encoded(const unsigned char *p,size_t n,size_t *at,unsigned enc,uint64_t pc,uint64_t *value){
    unsigned form=enc&15;uint64_t v=0;int64_t sv=0;
    if(enc==0xff || (enc&0x80))return 0;
    if(form==0){if(n-*at<8)return 0;p_copy(&v,p+*at,8);*at+=8;}
    else if(form==3 || form==11){if(n-*at<4)return 0;int32_t x;p_copy(&x,p+*at,4);*at+=4;sv=x;v=(uint64_t)sv;}
    else if(form==1){if(!rewrite_uleb(p,n,at,&v))return 0;}
    else return 0;
    if(enc&0x70){if((enc&0x70)!=0x10)return 0;v+=pc;}
    *value=v;return 1;
}
static int rewrite_fde_encoding(const unsigned char *p,size_t n,size_t cie,unsigned *encoding){
    if(cie+9>n)return 0;
    size_t at=cie+8;
    unsigned version=p[at++];if(version<1 || version>4)return 0;
    size_t aug=at;while(at<n && p[at])at++;if(at==n)return 0;
    int has_z=p[aug]=='z';at++;
    uint64_t ignored;if(!rewrite_uleb(p,n,&at,&ignored))return 0;
    /* data alignment is SLEB128 */
    unsigned shift=0;while(at<n && shift<64){unsigned char b=p[at++];shift+=7;if(!(b&128))break;}if(at>n)return 0;
    if(version==1){if(at>=n)return 0;at++;}else if(!rewrite_uleb(p,n,&at,&ignored))return 0;
    *encoding=0;
    if(has_z){uint64_t len;if(!rewrite_uleb(p,n,&at,&len) || len>n-at)return 0;size_t end=at+(size_t)len;
        for(size_t i=aug+1;p[i] && i<n;i++){
            if(p[i]=='R'){if(at>=end)return 0;*encoding=p[at++];}
            else if(p[i]=='L'){if(at>=end)return 0;at++;}
            else if(p[i]=='P'){if(at>=end)return 0;unsigned e=p[at++];uint64_t x;if(!rewrite_encoded(p,end,&at,e,at,&x))return 0;}
            else if(p[i]=='S'){}
            else return 0;
        }
    }
    return 1;
}
static void rewrite_fde_ranges(const unsigned char *ehdata,size_t ehn,uint64_t ehaddr,
                              const Elf64_Shdr *sections,unsigned nsections,
                              struct rewrite_scan *scan,uintptr_t begin,size_t mapsize,uint64_t mapoff,
                              const Elf64_Sym *symbols,size_t nsymbols){
    size_t at=0;
    while(at+4<=ehn){size_t rec=at;uint64_t len32=0;p_copy(&len32,ehdata+at,4);at+=4;if(!len32)continue;
        size_t idsize=4;uint64_t len=len32;if(len32==UINT32_MAX){if(at+8>ehn)break;p_copy(&len,ehdata+at,8);at+=8;idsize=8;}
        if(len>ehn-at || len<idsize)break;
        size_t end=at+(size_t)len;uint64_t id=0;
        if(idsize==4){uint32_t x;p_copy(&x,ehdata+at,4);id=x;}else p_copy(&id,ehdata+at,8);
        if(id!=0){size_t ciefield=at;uint64_t cie=(uint64_t)ciefield-id;unsigned enc=0;
            if(cie<rec && rewrite_fde_encoding(ehdata,ehn,(size_t)cie,&enc)){
                size_t p=at+idsize;uint64_t pc=0,range=0;uint64_t field_pc=ehaddr+p;
                if(rewrite_encoded(ehdata,end,&p,enc,field_pc,&pc) &&
                   rewrite_encoded(ehdata,end,&p,enc&15,0,&range) && range && pc<=UINT64_MAX-range){
                    for(unsigned si=0;si<nsections;si++){
                        const Elf64_Shdr *text=&sections[si];
                        if(text->sh_type!=SHT_PROGBITS || (text->sh_flags&(SHF_EXECINSTR|SHF_ALLOC))!=(SHF_EXECINSTR|SHF_ALLOC) || pc<text->sh_addr || pc-text->sh_addr>text->sh_size || range>text->sh_size-(pc-text->sh_addr))continue;
                        int named=0;for(size_t j=0;j<nsymbols;j++)if(ELF64_ST_TYPE(symbols[j].st_info)==STT_FUNC && symbols[j].st_shndx==si && symbols[j].st_size && symbols[j].st_value<pc+range && symbols[j].st_value+symbols[j].st_size>pc){named=1;break;}
                        if(named)break;
                        uint64_t file=text->sh_offset+(pc-text->sh_addr);
                        if(file<mapoff || file-mapoff>mapsize || range>mapsize-(file-mapoff))break;
                        Elf64_Sym synthetic={.st_value=pc,.st_size=range,.st_shndx=(Elf64_Section)si,.st_info=ELF64_ST_INFO(STB_LOCAL,STT_FUNC)};
                        rewrite_function((unsigned char *)(begin+(uintptr_t)(file-mapoff)),(size_t)range,scan,begin,&synthetic,symbols,nsymbols);break;
                    }
                }
            }
        }
        at=end;
    }
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
    if(identified){
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
    if(symbol_bytes){symbols=vfs_alloc(symbol_bytes);if(!symbols)goto cleanup;}
    size_t used=0;
    for(unsigned i=0;i<eh.e_shnum;i++){
        Elf64_Shdr *sh=&sections[i];
        if((sh->sh_type!=SHT_SYMTAB && sh->sh_type!=SHT_DYNSYM) || sh->sh_entsize!=sizeof(Elf64_Sym))continue;
        if(sh->sh_size%sizeof(Elf64_Sym) || pack_pread(fd,(unsigned char *)symbols+used,sh->sh_size,sh->sh_offset)!=(long)sh->sh_size)goto cleanup;
        used+=sh->sh_size;
    }
    for(size_t j=0;j<used/sizeof(*symbols);j++){
        Elf64_Sym sym=symbols[j];
        if(ELF64_ST_TYPE(sym.st_info)!=STT_FUNC || !sym.st_size ||
           !sym.st_shndx || sym.st_shndx>=eh.e_shnum || sym.st_value>UINT64_MAX-sym.st_size)continue;
        Elf64_Shdr text=sections[sym.st_shndx];
        if(text.sh_type!=SHT_PROGBITS || (text.sh_flags&(SHF_EXECINSTR|SHF_ALLOC))!=(SHF_EXECINSTR|SHF_ALLOC) || sym.st_value<text.sh_addr ||
           sym.st_value-text.sh_addr>text.sh_size || sym.st_size>text.sh_size-(sym.st_value-text.sh_addr))continue;
        if(text.sh_offset>UINT64_MAX-(sym.st_value-text.sh_addr))continue;
        uint64_t f=text.sh_offset+sym.st_value-text.sh_addr;
        if(f<offset || f-offset>=size || sym.st_size>size-(f-offset))continue;
        patched+=rewrite_function((unsigned char *)(begin+(uintptr_t)(f-offset)),sym.st_size,scan,begin,&sym,symbols,used/sizeof(*symbols));
    }
    if(eh.e_shstrndx<eh.e_shnum){
        Elf64_Shdr *names=&sections[eh.e_shstrndx];
        if(names->sh_size<=SIZE_MAX && names->sh_offset<=UINT64_MAX-names->sh_size){
            unsigned char *strings=vfs_alloc((size_t)names->sh_size);
            if(strings && pack_pread(fd,strings,(size_t)names->sh_size,names->sh_offset)==(long)names->sh_size){
                for(unsigned i=0;i<eh.e_shnum;i++)if(sections[i].sh_name<names->sh_size){
                    size_t q=sections[i].sh_name;const char *want=".eh_frame";size_t k=0;
                    while(q<names->sh_size && strings[q] && want[k] && strings[q]==want[k]){q++;k++;}
                    if(want[k] || q>=names->sh_size || strings[q])continue;
                    Elf64_Shdr *frame=&sections[i];
                    if(frame->sh_size<=SIZE_MAX && frame->sh_offset<=UINT64_MAX-frame->sh_size){
                        unsigned char *data=vfs_alloc((size_t)frame->sh_size);
                        if(data && pack_pread(fd,data,(size_t)frame->sh_size,frame->sh_offset)==(long)frame->sh_size)
                            rewrite_fde_ranges(data,(size_t)frame->sh_size,frame->sh_addr,sections,eh.e_shnum,scan,begin,size,offset,symbols,used/sizeof(*symbols));
                        vfs_free(data);
                    }
                    break;
                }
            }
            vfs_free(strings);
        }
    }
cleanup:
    vfs_free(symbols);vfs_free(sections);
    if(scan){scan->next=rewrite_scans;rewrite_scans=scan;}
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
