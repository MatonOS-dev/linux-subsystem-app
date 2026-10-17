#include "wayland_resolve.h"
#include "runtime.h"
#include "backend.h"

#include <elf.h>

struct mv_link_map {
    Elf64_Addr addr;
    char *name;
    Elf64_Dyn *dynamic;
    struct mv_link_map *next, *prev;
};
struct mv_r_debug {
    int version;
    struct mv_link_map *map;
    Elf64_Addr breakpoint;
    int state;
    Elf64_Addr loader_base;
};
struct mv_aux { Elf64_Addr type, value; };

struct mv_object {
    Elf64_Addr base;
    Elf64_Dyn *dynamic;
    Elf64_Addr low, high;
};

static int contains(Elf64_Addr low, Elf64_Addr high, Elf64_Addr p, size_t n) {
    return p >= low && p <= high && n <= high - p;
}

static Elf64_Addr dynamic_pointer(Elf64_Addr base, Elf64_Addr p) {
    return p < base ? base + p : p;
}

static void *lookup_dynamic(const struct mv_object *o, const char *wanted) {
    Elf64_Dyn *strtab = 0, *symtab = 0, *gnu_hash = 0, *sysv_hash = 0;
    size_t syment = sizeof(Elf64_Sym), strsz = 0;
    for (Elf64_Dyn *d=o->dynamic; d && contains(o->low,o->high,(Elf64_Addr)d,sizeof(*d)) && d->d_tag!=DT_NULL; ++d) {
        switch (d->d_tag) {
        case DT_STRTAB: strtab=d; break;
        case DT_SYMTAB: symtab=d; break;
        case DT_STRSZ: strsz=d->d_un.d_val; break;
        case DT_SYMENT: syment=d->d_un.d_val; break;
        case DT_GNU_HASH: gnu_hash=d; break;
        case DT_HASH: sysv_hash=d; break;
        }
    }
    if (!strtab || !symtab || !strsz || syment!=sizeof(Elf64_Sym)) return 0;
    const char *strings=(const char *)dynamic_pointer(o->base,strtab->d_un.d_ptr);
    Elf64_Sym *symbols=(Elf64_Sym *)dynamic_pointer(o->base,symtab->d_un.d_ptr);
    if (!contains(o->low,o->high,(Elf64_Addr)strings,strsz) ||
        !contains(o->low,o->high,(Elf64_Addr)symbols,sizeof(*symbols))) return 0;

    uint32_t count=0;
    if (sysv_hash) {
        uint32_t *h=(uint32_t *)dynamic_pointer(o->base,sysv_hash->d_un.d_ptr);
        if (!contains(o->low,o->high,(Elf64_Addr)h,2*sizeof(uint32_t))) return 0;
        count=h[1];
    } else if (gnu_hash) {
        uint32_t *h=(uint32_t *)dynamic_pointer(o->base,gnu_hash->d_un.d_ptr);
        if (!contains(o->low,o->high,(Elf64_Addr)h,4*sizeof(uint32_t))) return 0;
        uint32_t buckets=h[0], first=h[1], bloom=h[2];
        if (!buckets || !bloom) return 0;
        Elf64_Xword *filter=(Elf64_Xword *)(h+4);
        uint32_t *bucket=(uint32_t *)(filter+bloom);
        if (!contains(o->low,o->high,(Elf64_Addr)bucket,(size_t)buckets*sizeof(*bucket))) return 0;
        uint32_t *chain=bucket+buckets;
        for (uint32_t i=0;i<buckets;i++) {
            uint32_t index=bucket[i];
            if (index<first) continue;
            for (uint32_t steps=0; steps<65536; steps++,index++) {
                uint32_t *word=chain+(index-first);
                if (!contains(o->low,o->high,(Elf64_Addr)word,sizeof(*word))) return 0;
                if (index+1>count) count=index+1;
                if (*word&1) break;
            }
        }
    }
    if (!count || !contains(o->low,o->high,(Elf64_Addr)symbols,(size_t)count*sizeof(*symbols))) return 0;

    uint32_t hash=5381;
    for (const unsigned char *p=(const unsigned char *)wanted;*p;p++) hash=hash*33+*p;
    uint32_t *gnu=gnu_hash?(uint32_t *)dynamic_pointer(o->base,gnu_hash->d_un.d_ptr):0;
    uint32_t gnu_buckets=gnu?gnu[0]:0, gnu_first=gnu?gnu[1]:0, gnu_bloom=gnu?gnu[2]:0;
    uint32_t *bucket=gnu?(uint32_t *)((Elf64_Xword *)(gnu+4)+gnu_bloom):0;
    uint32_t *chain=gnu?bucket+gnu_buckets:0;
    uint32_t index=gnu?(bucket[hash%gnu_buckets]):0;
    if (gnu && index>=gnu_first) {
        for (uint32_t steps=0;index<count && steps<65536;steps++,index++) {
            uint32_t ch=chain[index-gnu_first];
            if (((ch|1)==(hash|1)) && symbols[index].st_name<strsz && !strcmp(strings+symbols[index].st_name,wanted)) {
                unsigned bind=ELF64_ST_BIND(symbols[index].st_info), vis=ELF64_ST_VISIBILITY(symbols[index].st_other);
                if ((bind==STB_GLOBAL || bind==STB_WEAK) && (vis==STV_DEFAULT || vis==STV_PROTECTED)) return (void *)(o->base+symbols[index].st_value);
            }
            if (ch&1) break;
        }
    }
    if (sysv_hash) for (uint32_t i=1;i<count;i++) {
        if (symbols[i].st_name<strsz && !strcmp(strings+symbols[i].st_name,wanted)) {
            unsigned bind=ELF64_ST_BIND(symbols[i].st_info), vis=ELF64_ST_VISIBILITY(symbols[i].st_other);
            if ((bind==STB_GLOBAL || bind==STB_WEAK) && (vis==STV_DEFAULT || vis==STV_PROTECTED)) return (void *)(o->base+symbols[i].st_value);
        }
    }
    return 0;
}

static int wayland_name(const char *s) {
    static const char needle[]="libwayland-client.so.0";
    for (;*s;s++) { size_t i=0; while (needle[i] && s[i]==needle[i]) i++; if (!needle[i]) return 1; }
    return 0;
}

static void *from_link_map(const char *name) {
    struct mv_aux aux; Elf64_Addr phdr=0; size_t phnum=0;
    long fd=mv_open("/proc/self/auxv"); if(fd<0)return 0;
    while(mv_read(fd,&aux,sizeof(aux))==(long)sizeof(aux)) {
        if(aux.type==AT_PHDR)phdr=aux.value;
        if(aux.type==AT_PHNUM)phnum=aux.value;
        if(aux.type==AT_NULL)break;
    }
    mv_close(fd); if(!phdr || !phnum || phnum>4096)return 0;
    Elf64_Phdr *p=(Elf64_Phdr *)phdr; Elf64_Dyn *dynamic=0;Elf64_Addr bias=0;
    for(size_t i=0;i<phnum;i++)if(p[i].p_type==PT_PHDR)bias=phdr-p[i].p_vaddr;
    for(size_t i=0;i<phnum;i++)if(p[i].p_type==PT_DYNAMIC)dynamic=(Elf64_Dyn *)(bias+p[i].p_vaddr);
    if(!dynamic)return 0;
    struct mv_r_debug *debug=0;
    for(Elf64_Dyn *d=dynamic;d->d_tag!=DT_NULL;d++)if(d->d_tag==DT_DEBUG)debug=(void *)d->d_un.d_ptr;
    if(!debug || debug->version<1)return 0;
    for(struct mv_link_map *m=debug->map;m;m=m->next) {
        if(m->name && wayland_name(m->name)) {
            struct mv_object o={.base=m->addr,.dynamic=m->dynamic,.low=m->addr,.high=UINTPTR_MAX};
            return lookup_dynamic(&o,name);
        }
    }
    return 0;
}

static int parse_hex(const char **p, Elf64_Addr *out) {
    Elf64_Addr v=0; int digits=0;
    while(**p) { unsigned c=(unsigned char)**p; unsigned d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16; if(d==16)break; if(v>(UINTPTR_MAX-d)/16)return 0;v=v*16+d;(*p)++;digits++; }
    *out=v;return digits!=0;
}

static void *from_maps(const char *name) {
    long fd=mv_open("/proc/self/maps");if(fd<0)return 0;
    char line[4096];size_t n=0;Elf64_Addr start=0,low=UINTPTR_MAX,high=0;int matched=0;void *result=0;
    for(;;) {
        char c;long got=mv_read(fd,&c,1);if(got!=1)break;
        if(c!='\n' && n+1<sizeof(line)){line[n++]=c;continue;}
        line[n]=0;const char *p=line;Elf64_Addr lo,hi,off;
        if(parse_hex(&p,&lo)&&*p=='-'){p++;if(parse_hex(&p,&hi)){while(*p==' ')p++;while(*p && *p!=' ')p++;while(*p==' ')p++;if(parse_hex(&p,&off)){
            while(*p==' ')p++; if(wayland_name(p)){
                if(off==0)start=lo;
                if(lo<low)low=lo;if(hi>high)high=hi;matched=1;
            }
        }}}
        n=0;if(!got)break;
    }
    if(matched && start && low<high) {
            Elf64_Ehdr *eh=(Elf64_Ehdr *)start;
            if(contains(low,high,(Elf64_Addr)eh,sizeof(*eh)) && !memcmp(eh->e_ident,ELFMAG,SELFMAG) && eh->e_ident[EI_CLASS]==ELFCLASS64) {
                Elf64_Phdr *ph=(void *)(start+eh->e_phoff);Elf64_Addr bias=start;
                for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD && ph[i].p_offset==0){bias=start-(ph[i].p_vaddr&~(Elf64_Addr)4095);break;}
                Elf64_Dyn *dynamic=0;
                for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_DYNAMIC)dynamic=(void *)(bias+ph[i].p_vaddr);
                struct mv_object o={.base=bias,.dynamic=dynamic,.low=low,.high=high};
                result=dynamic?lookup_dynamic(&o,name):0;
            }
    }
    mv_close(fd);return result;
}

void *mv_wayland_symbol(const char *name) {
    if(!name || !*name)return 0;
    void *symbol=from_link_map(name);
    return symbol?symbol:from_maps(name);
}
