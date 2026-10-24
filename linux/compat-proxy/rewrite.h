/* Lazy ELF function decoder. This file intentionally has no architecture tests;
 * instruction proof and entry generation are selected by the backend. */
#ifdef ARCH_REWRITE_HEADER
#include ARCH_REWRITE_HEADER
static int rewrite_enabled, rewrite_log;
static uintptr_t rewrite_target;
static unsigned rewrite_nr;
struct rewrite_mapping {struct rewrite_mapping *next;uintptr_t begin;size_t size;uint64_t offset;int fd;char path[512];};
static struct rewrite_mapping *rewrite_mappings;
struct rewrite_record {struct rewrite_record *next;uintptr_t address,ret;uint64_t offset,sequence,time;unsigned nr,window,active;unsigned char original[128];char path[512];};
static struct rewrite_record *rewrite_records;
static struct rewrite_mapping *rewrite_current;
static uint64_t rewrite_sequence;
struct rewrite_attempt {struct rewrite_attempt *next;uintptr_t pc;struct rewrite_mapping *map;};
static struct rewrite_attempt *rewrite_attempts;

struct rewrite_site {uint32_t offset;unsigned nr,window;unsigned char original[128];};
struct rewrite_scan {struct rewrite_scan *next;struct statx identity;uint64_t offset,slice,target;size_t size,count,capacity,function_offset,function_size;uint32_t function_crc;struct rewrite_site *sites;};
static struct rewrite_scan *rewrite_scans;
static unsigned rewrite_busy;
static int proxy_rewrite_internal_site(uintptr_t pc){return arch_rewrite_internal_site(pc);}
static int proxy_rewrite_traps(long nr){return rewrite_enabled && (nr==PROXY_SC_mprotect || nr==PROXY_SC_munmap || nr==PROXY_SC_mremap);}
static uint64_t rewrite_clock(void){
    struct {int64_t sec,nsec;} t={0};
    if(RAW(PROXY_SC_clock_gettime,1,&t,0,0,0,0)<0)return 0;
    return (uint64_t)t.sec*1000000000+(uint64_t)t.nsec;
}
static unsigned rewrite_apply(uintptr_t addr,unsigned nr,unsigned w,const unsigned char *original){
    if((addr&4095)+w>4096)return 0;
    struct rewrite_record *record=vfs_palloc(sizeof(*record));
    if(!record)return 0;
    const unsigned char *old=(void *)addr;
    for(unsigned j=0;j<w;j++)if(old[j]!=original[j]){vfs_free(record);return 0;}
    long page=RAW(PROXY_SC_mmap,0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if((unsigned long)page>=(unsigned long)-4095){vfs_free(record);return 0;}
    p_copy((void *)(uintptr_t)page,(void *)(addr&~(uintptr_t)4095),4096);
    arch_rewrite_patch((unsigned char *)(uintptr_t)page+(addr&4095),nr,w);
    p_copy(record->original,original,w);
    record->address=addr;record->ret=addr+w;record->nr=nr;record->window=w;
    record->offset=rewrite_current->offset+addr-rewrite_current->begin;
    p_copy(record->path,rewrite_current->path,sizeof(record->path));
    record->time=rewrite_clock();record->sequence=++rewrite_sequence;
    record->active=1;record->next=rewrite_records;
    /* Publish the recovery record BEFORE executable bytes become visible. */
    __atomic_store_n(&rewrite_records,record,__ATOMIC_RELEASE);
    if(RAW(PROXY_SC_mprotect,page,4096,PROT_READ|PROT_EXEC,0,0,0)>=0 &&
       RAW(PROXY_SC_mremap,page,4096,4096,MREMAP_MAYMOVE|MREMAP_FIXED,addr&~(uintptr_t)4095,0)>=0){
        if(rewrite_log){crash_text("v3 rewrite patch path=");crash_text(record->path);crash_text("\n");proxy_log("rewrite-address",addr,nr);proxy_log("rewrite-offset",record->offset,record->sequence);}
        return 1;
    }
    record->active=0;
    RAW(PROXY_SC_munmap,page,4096,0,0,0,0);return 0;
}
int proxy_rewrite_init(const char *s,int logging){
    rewrite_log=logging;
    if(s && p_equal(s,"MATON_PROXY_REWRITE=0")){
        rewrite_enabled=0;proxy_log("rewrite-off",0,0);return 0;
    }
    rewrite_enabled=arch_rewrite_init();
    proxy_log(rewrite_enabled?"rewrite-on":"rewrite-off",0,0);
    proxy_rewrite_table=arch_rewrite_table;
    return rewrite_enabled;
}
#include "rewrite-cache.h"
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
    if(rewrite_target<(uintptr_t)p || rewrite_target-(uintptr_t)p>=size)return 0;
    if(scan){scan->function_offset=(uintptr_t)p-begin;scan->function_size=size;scan->function_crc=rewrite_function_crc(p,size);}
    size_t pos=0,count=0,capacity=size;
    if(!size || capacity>SIZE_MAX/sizeof(struct rewrite_insn) || capacity>SIZE_MAX/sizeof(size_t))return 0;
    struct rewrite_insn *ins=vfs_alloc(capacity*sizeof(*ins));
    size_t *offsets=vfs_alloc(capacity*sizeof(*offsets));
    if(!ins || !offsets){vfs_free(ins);vfs_free(offsets);return 0;}
    unsigned char *text=rewrite_function_bytes(p,size);
    if(!text){vfs_free(ins);vfs_free(offsets);return 0;}
    while(pos<size){
        if(count==capacity || !arch_rewrite_decode(text+pos,size-pos,&ins[count])){vfs_free(text);vfs_free(ins);vfs_free(offsets);return 0;}
        offsets[count]=pos;pos+=ins[count++].size;
    }
    vfs_free(text);
    /* Walk backward over the forward-decoded stream, never raw byte patterns.
     * Unknown RAX use, control flow and stack changes keep the site trapped. */
    for(size_t k=0;k<count;k++)if(ins[k].syscall){
        for(size_t q=k;q>0;){q--;
            if(ins[q].number){
                size_t w=offsets[k]+2-offsets[q];
                if(w<=128 && (uintptr_t)p+offsets[k]==rewrite_target && ins[q].nr==rewrite_nr)ins[q].window=(unsigned)w;
                break;
            }
            if(ins[q].rax || ins[q].flow || ins[q].target!=INT64_MIN || ins[q].stack_delta)break;
        }
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
        unsigned char original[128];p_copy(original,(void *)addr,w);
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
                        uint64_t file=text->sh_offset+(pc-text->sh_addr);
                        if(file<mapoff || file-mapoff>mapsize || range>mapsize-(file-mapoff))break;
                        uintptr_t function=begin+(uintptr_t)(file-mapoff);
                        if(rewrite_target<function || rewrite_target-function>=range)break;
                        int named=0;for(size_t j=0;j<nsymbols;j++)if(ELF64_ST_TYPE(symbols[j].st_info)==STT_FUNC && symbols[j].st_shndx==si && symbols[j].st_size && symbols[j].st_value<pc+range && symbols[j].st_value+symbols[j].st_size>pc){named=1;break;}
                        if(named)break;
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
static void rewrite_discover(int fd,uintptr_t begin,size_t size,uint64_t offset){
    if(!rewrite_enabled || !size || begin>UINTPTR_MAX-size)return;
    /* Try-lock, never wait in a signal frame (or after a concurrent fork).
     * Cache contention merely leaves this mapping on SIGSYS. */
    uint64_t started=rewrite_clock();unsigned patched=0;
    struct statx identity={0};struct rewrite_scan *scan=NULL;
    int identified=RAW(PROXY_SC_statx,fd,"",AT_EMPTY_PATH,STATX_BASIC_STATS,&identity,0)==0;
    uint64_t slice=pack_slice_start(fd);
    if(identified)for(scan=rewrite_scans;scan;scan=scan->next){
        struct statx *a=&scan->identity,*b=&identity;
        if(scan->target==rewrite_target-begin && scan->offset==offset && scan->size==size && scan->slice==slice &&
           a->stx_ino==b->stx_ino && a->stx_dev_major==b->stx_dev_major && a->stx_dev_minor==b->stx_dev_minor &&
           a->stx_size==b->stx_size && a->stx_mtime.tv_sec==b->stx_mtime.tv_sec &&
           a->stx_mtime.tv_nsec==b->stx_mtime.tv_nsec && a->stx_ctime.tv_sec==b->stx_ctime.tv_sec &&
           a->stx_ctime.tv_nsec==b->stx_ctime.tv_nsec)break;
    }
    if(scan && (!scan->function_size || scan->function_offset>size || scan->function_size>size-scan->function_offset || rewrite_function_crc((void *)(begin+scan->function_offset),scan->function_size)!=scan->function_crc))scan=NULL;
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
        if(scan){scan->target=rewrite_target-begin;scan->identity=identity;scan->offset=offset;scan->size=size;scan->slice=slice;
            if(rewrite_cache_read(scan,begin)){vfs_free(scan);goto done;}
        }
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
        if(rewrite_target<begin+f-offset || rewrite_target-(begin+f-offset)>=sym.st_size)continue;
        patched+=rewrite_function((unsigned char *)(begin+(uintptr_t)(f-offset)),sym.st_size,scan,begin,&sym,symbols,used/sizeof(*symbols));
        /* FDE fallback is forbidden for any named range, even a rejected one. */
        goto cleanup;
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
    if(scan){rewrite_cache_write(scan);scan->next=rewrite_scans;rewrite_scans=scan;}
done:
    if(proxy_trace || rewrite_log){proxy_log("rewrite-sites",patched,size);proxy_log("rewrite-scan-ns",rewrite_clock()-started,size);}
    __atomic_store_n(&rewrite_busy,0,__ATOMIC_RELEASE);
}
static void rewrite_invalidate(uintptr_t,size_t);
/* Registration only retains the backing object; no text decoding at load time. */
void proxy_rewrite_mapping(int fd,uintptr_t begin,size_t size,uint64_t offset){
    if(!rewrite_enabled || !size || begin>UINTPTR_MAX-size)return;
    rewrite_invalidate(begin,size);
    struct rewrite_mapping *m=vfs_palloc(sizeof(*m));if(!m)return;
    long copy=RAW(PROXY_SC_fcntl,fd,F_DUPFD_CLOEXEC,256,0,0,0);
    if(copy<0){vfs_free(m);return;}
    if(pack_dup(fd,(int)copy)<0 || resident_fd_set((int)copy,2)<0){RAW(PROXY_SC_close,copy,0,0,0,0,0);vfs_free(m);return;}
    m->begin=begin;m->size=size;m->offset=offset;m->fd=(int)copy;
    char name[64]="/proc/self/fd/";size_t n=p_len(name);char digits[16];size_t k=0;unsigned v=(unsigned)fd;
    do{digits[k++]=(char)('0'+v%10);v/=10;}while(v);
    while(k)name[n++]=digits[--k];
    name[n]=0;
    long len=RAW(PROXY_SC_readlinkat,AT_FDCWD,name,m->path,sizeof(m->path)-1,0,0);
    if(len>=0)m->path[len]=0;
    m->next=rewrite_mappings;__atomic_store_n(&rewrite_mappings,m,__ATOMIC_RELEASE);
}
void proxy_rewrite_relocate(uintptr_t old,uintptr_t dest,size_t size){
    for(struct rewrite_mapping *m=rewrite_mappings;m;m=m->next)
        if(m->size && m->begin>=old && m->begin-old<size)m->begin=dest+(m->begin-old);
}
static void proxy_rewrite_trap_site(uintptr_t continuation,long nr){
    if(!rewrite_enabled || !arch_rewrite_number(nr) || continuation<2)return;
    uintptr_t pc=continuation-2;
    struct rewrite_mapping *m;
    for(m=rewrite_mappings;m;m=m->next)if(m->size && pc>=m->begin && pc-m->begin<m->size)break;
    if(!m)return;
    /* The discovery lock also protects target/current and the attempts list. */
    if(__atomic_exchange_n(&rewrite_busy,1,__ATOMIC_ACQUIRE))return;
    for(struct rewrite_attempt *a=rewrite_attempts;a;a=a->next)if(a->map==m && a->pc==pc){__atomic_store_n(&rewrite_busy,0,__ATOMIC_RELEASE);return;}
    struct rewrite_attempt *a=vfs_palloc(sizeof(*a));
    if(!a){__atomic_store_n(&rewrite_busy,0,__ATOMIC_RELEASE);return;}
    a->map=m;a->pc=pc;a->next=rewrite_attempts;rewrite_attempts=a;
    rewrite_target=pc;rewrite_nr=(unsigned)nr;rewrite_current=m;
    rewrite_discover(m->fd,m->begin,m->size,m->offset);
}
static void rewrite_invalidate(uintptr_t begin,size_t size){
    for(struct rewrite_mapping *m=rewrite_mappings;m;m=m->next)if(m->size && begin<m->begin+m->size && begin+size>m->begin){
        m->size=0;resident_fd_set(m->fd,0);pack_close(m->fd);RAW(PROXY_SC_close,m->fd,0,0,0,0,0);
    }
    for(struct rewrite_record *r=rewrite_records;r;r=r->next)if(begin<r->address+r->window && begin+size>r->address)r->active=0;
}
static void proxy_rewrite_dump(void *context){
    ucontext_t *uc=context;uintptr_t pc=ARCH_CONTEXT(uc,ARCH_PC),sp=ARCH_CONTEXT(uc,ARCH_STACK);
    for(struct rewrite_record *r=__atomic_load_n(&rewrite_records,__ATOMIC_ACQUIRE);r;r=r->next){
        crash_text("v3 rewrite registry path=");crash_text(r->path);crash_text("\n");
        proxy_log("rewrite-site",r->address,r->nr);proxy_log("rewrite-file-offset",r->offset,r->window);
        proxy_log("rewrite-sequence-time",r->sequence,r->time);
        proxy_log("rewrite-encoding-active",r->original[0]==0xb8?0xb8:0x48c7c0,r->active);
        if((pc>>12)==(r->address>>12))proxy_log("rewrite-fault-page",pc,r->address);
        for(unsigned i=0;i<16;i++){uintptr_t value;if(proxy_copy(&value,(void *)(sp+i*sizeof(value)),sizeof(value)))break;
            if(value==r->ret || (value>>12)==(r->address>>12))proxy_log("rewrite-stack-hit",value,i);}
    }
}
static void proxy_rewrite_exit(void){
    if(!rewrite_log)return;
    unsigned count=0;
    for(struct rewrite_record *r=rewrite_records;r;r=r->next)if(r->active)count++;
    proxy_log("rewrite-process-sites",count,rewrite_sequence);
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
        rewrite_invalidate((uintptr_t)a[0],(size_t)a[1]);
        if(result!=(long)a[0])rewrite_invalidate((uintptr_t)result,(size_t)a[2]);
        for(unsigned i=0;i<128;i++)rewrite_pending[i].size=0;
        goto done;
    }
    uintptr_t begin=nr==PROXY_SC_mmap?(uintptr_t)result:(uintptr_t)a[0];
    size_t size=(size_t)a[1];
    if(size>SIZE_MAX-4095 || begin>UINTPTR_MAX-((size+4095)&~(size_t)4095))goto done;
    size=(size+4095)&~(size_t)4095;
    int registered=nr==PROXY_SC_mmap && !(a[3]&MAP_ANONYMOUS) &&
        (a[3]&MAP_PRIVATE) && (a[2]&(PROT_READ|PROT_WRITE|PROT_EXEC))==(PROT_READ|PROT_EXEC);
    if(!registered && (nr!=PROXY_SC_mprotect || (a[2]&(PROT_WRITE|PROT_EXEC))!=PROT_EXEC))rewrite_invalidate(begin,size);
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
void proxy_rewrite_cache_init(const char *root){(void)root;}
void proxy_rewrite_relocate(uintptr_t o,uintptr_t d,size_t n){(void)o;(void)d;(void)n;}
static void proxy_rewrite_trap_site(uintptr_t pc,long nr){(void)pc;(void)nr;}
static void proxy_rewrite_dump(void *ctx){(void)ctx;}
static void proxy_rewrite_exit(void){}
static long rewrite_slow(long nr,proxy_arg_t *a){(void)nr;(void)a;return -ENOSYS;}
static int proxy_rewrite_internal_site(uintptr_t pc){(void)pc;return 0;}
static int proxy_rewrite_traps(long nr){(void)nr;return 0;}
static void proxy_rewrite_after(long nr,proxy_arg_t *a,long r){(void)nr;(void)a;(void)r;}
int proxy_rewrite_init(const char *s,int logging){(void)logging;(void)s;proxy_log("rewrite-off",0,0);return 0;}
void proxy_rewrite_mapping(int fd,uintptr_t b,size_t n,uint64_t o){(void)fd;(void)b;(void)n;(void)o;}
#endif
