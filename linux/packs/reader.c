#define _POSIX_C_SOURCE 200809L
#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mtp_index { mtp_object *items; size_t count; void *storage; mtp_alloc_fn alloc; mtp_free_fn dealloc; void *context; int owns_storage; };
#ifndef MTP_FREESTANDING
static void *heap_alloc(void *unused,size_t n){(void)unused;return malloc(n);}
static void heap_free(void *unused,void *p){(void)unused;free(p);}
#endif
static uint32_t u32(const unsigned char *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t u64(const unsigned char *p) { return (uint64_t)u32(p)|(uint64_t)u32(p+4)<<32; }
static int take(const unsigned char **p, const unsigned char *end, size_t n, const unsigned char **out) {
    if ((size_t)(end-*p)<n) return -1;
    *out=*p;
    *p+=n;
    return 0;
}
static int string(const unsigned char **p,const unsigned char *end,const char **out,mtp_index *idx) {
    const unsigned char *q; if(take(p,end,4,&q))return -1; uint32_t n=u32(q);
    if(take(p,end,n,&q))return -1;
    char *s=idx->alloc(idx->context,(size_t)n+1);
    if(!s)return -1;
    memcpy(s,q,n);s[n]=0;*out=s;return 0;
}
int mtp_index_open_buffer(const void *bytes,size_t length,mtp_index **out,mtp_alloc_fn alloc,mtp_free_fn dealloc,void *context) {
    if(!bytes||!out||!alloc||!dealloc||length<12)return -1;
    const unsigned char *b=bytes;size_t z=length;
    if(memcmp(b,"MTPKIDX1",8))return -1;
    uint32_t n=u32(b+8);if((size_t)n>((z-12)/40)||(n&&sizeof(mtp_object)>SIZE_MAX/(size_t)n))return -1;
    mtp_index *idx=alloc(context,sizeof(*idx));if(!idx)return -1;
    memset(idx,0,sizeof(*idx));idx->alloc=alloc;idx->dealloc=dealloc;idx->context=context;
    idx->items=alloc(context,(n?n:1)*sizeof(*idx->items));
    if(!idx->items){dealloc(context,idx);return -1;}
    memset(idx->items,0,(n?n:1)*sizeof(*idx->items));idx->count=n;idx->storage=(void*)bytes;
    const unsigned char *p=b+12,*end=b+z;
    for(uint32_t i=0;i<n;i++){
        mtp_object *o=&idx->items[i];const unsigned char *q;
        if(string(&p,end,&o->path,idx)||string(&p,end,&o->checksum,idx)||string(&p,end,&o->pack,idx)||string(&p,end,&o->kind,idx)||string(&p,end,&o->target,idx)||take(&p,end,4,&q))goto fail;
        o->xattrs_length=u32(q);if(take(&p,end,o->xattrs_length,&q))goto fail;o->xattrs=q;
        if(take(&p,end,28,&q))goto fail;
        o->offset=u64(q);o->length=u64(q+8);o->uid=u32(q+16);o->gid=u32(q+20);o->mode=u32(q+24);
        if(o->offset>UINT64_MAX-o->length)goto fail;
    }
    if(p!=end)goto fail;
    /* A pack describes the contents of a tree, but OSTree indexes may omit
     * the tree root itself. Keep the root in the same object table as every
     * other path so stat/open/access/readdir all take their ordinary path. */
    int has_root=0;
    for(uint32_t i=0;i<n;i++)if(!strcmp(idx->items[i].path,"/")){has_root=1;break;}
    if(!has_root) {
        if(n==UINT32_MAX || (size_t)n+1>SIZE_MAX/sizeof(*idx->items))goto fail;
        mtp_object *items=alloc(context,((size_t)n+1)*sizeof(*items));
        if(!items)goto fail;
        memcpy(items,idx->items,(size_t)n*sizeof(*items));
        dealloc(context,idx->items);
        idx->items=items;
        mtp_object root={0};
        root.path=idx->alloc(context,2);
        root.checksum=idx->alloc(context,1);
        root.pack=idx->alloc(context,1);
        root.kind=idx->alloc(context,4);
        root.target=idx->alloc(context,1);
        if(!root.path||!root.checksum||!root.pack||!root.kind||!root.target) {
            idx->dealloc(context,(void *)root.path);idx->dealloc(context,(void *)root.checksum);
            idx->dealloc(context,(void *)root.pack);idx->dealloc(context,(void *)root.kind);
            idx->dealloc(context,(void *)root.target);goto fail;
        }
        memcpy((char *)root.path,"/",2);((char *)root.checksum)[0]=0;
        ((char *)root.pack)[0]=0;memcpy((char *)root.kind,"dir",4);
        ((char *)root.target)[0]=0;
        root.mode=0040000|0755;
        idx->items[n]=root;
        idx->count=(size_t)n+1;
    }
    *out=idx;return 0;
fail: mtp_index_close(idx);return -1;
}
#ifndef MTP_FREESTANDING
int mtp_index_open(const char *filename, mtp_index **out) {
    FILE *f=fopen(filename,"rb"); if(!f)return -1;
    if(fseek(f,0,SEEK_END)){fclose(f);return -1;} long z=ftell(f);
    if(z<12 || fseek(f,0,SEEK_SET)){fclose(f);return -1;}
    unsigned char *b=malloc((size_t)z); if(!b){fclose(f);return -1;}
    if(fread(b,1,(size_t)z,f)!=(size_t)z){free(b);fclose(f);return -1;} fclose(f);
    int r=mtp_index_open_buffer(b,(size_t)z,out,heap_alloc,heap_free,NULL);
    if(r){free(b);return -1;}(*out)->owns_storage=1;return 0;
}
#endif
void mtp_index_close(mtp_index *i) { if(!i)return; for(size_t n=0;n<i->count;n++){i->dealloc(i->context,(void*)i->items[n].path);i->dealloc(i->context,(void*)i->items[n].checksum);i->dealloc(i->context,(void*)i->items[n].pack);i->dealloc(i->context,(void*)i->items[n].kind);i->dealloc(i->context,(void*)i->items[n].target);}i->dealloc(i->context,i->items);if(i->owns_storage)i->dealloc(i->context,i->storage);i->dealloc(i->context,i); }
size_t mtp_index_count(const mtp_index *i){return i?i->count:0;}
int mtp_normalize_path(const char *input,char *output,size_t size) {
    if(!input||!output||!size)return -1;
    const unsigned char *p=(const unsigned char *)input;size_t used=0;
    while(*p=='/')p++;
    while(*p) {
        const unsigned char *start=p;while(*p&&*p!='/') {
            if(*p<0x20||*p==0x7f)return -1;
            p++;
        }
        size_t n=(size_t)(p-start);
        if(n==1&&start[0]=='.') { while(*p=='/')p++;continue; }
        if(n==2&&start[0]=='.'&&start[1]=='.')return -1;
        if(n) {
            if(used && used+1>=size)return -1;
            if(used)output[used++]='/';
            if(n>=size-used)return -1;
            memcpy(output+used,start,n);used+=n;
        }
        while(*p=='/')p++;
    }
    output[used]=0;return 0;
}
int mtp_lookup(const mtp_index *i,const char *path,mtp_object *out) {
    if(!i||!path||!out)return -1;
    /* Match the path rules used by the proxy resolver. Linux pathname
     * arguments are bounded by PATH_MAX, so this stack buffer covers every
     * path the kernel can pass while keeping this entry point freestanding. */
    char normalized[4096];
    if(mtp_normalize_path(path,normalized,sizeof(normalized)))return -1;
    const char *key=*normalized?normalized:"/";
    for(size_t n=0;n<i->count;n++) {
        const char *stored=i->items[n].path;
        if(!strcmp(stored,key) || (*normalized && stored[0]=='/' && !strcmp(stored+1,normalized))) {
            *out=i->items[n];return 0;
        }
    }
    return 1;
}
int mtp_stat(const mtp_index *i,const char *path,mtp_object *out) { return mtp_lookup(i,path,out); }
int mtp_readdir(const mtp_index *i,const char *dir,size_t *cursor,const char **name,mtp_object *out) {
    if(!i||!dir||!cursor||!name)return -1;
    size_t dl=strlen(dir);
    if(dl && dir[dl-1]=='/')dl--;
    for(size_t n=*cursor;n<i->count;n++){const char *p=i->items[n].path;size_t l=strlen(p);if(l<=dl||memcmp(p,dir,dl)||p[dl]!='/')continue;
        const char *child=p+dl+1;if(!*child||strchr(child,'/'))continue;*cursor=n+1;*name=child;if(out)*out=i->items[n];return 1;}
    *cursor=i->count;return 0;
}
