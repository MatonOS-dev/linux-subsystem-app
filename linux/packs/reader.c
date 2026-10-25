#define _POSIX_C_SOURCE 200809L
#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mtp_index { mtp_object *items; size_t count; mtp_layer *layers; size_t layer_count; void *storage; mtp_alloc_fn alloc; mtp_free_fn dealloc; void *context; int owns_storage; };
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
#ifdef MTPK_V2_ONLY
    int v2=1;
    if(memcmp(b,"MTPKIDX2",8))return -1;
#else
    int v2=!memcmp(b,"MTPKIDX2",8);
    if(!v2&&memcmp(b,"MTPKIDX1",8))return -1;
#endif
    mtp_index *idx=alloc(context,sizeof(*idx));if(!idx)return -1;
    memset(idx,0,sizeof(*idx));idx->alloc=alloc;idx->dealloc=dealloc;idx->context=context;
    const unsigned char *p=b+8,*end=b+z,*q;
    if(take(&p,end,4,&q))goto fail;
    uint32_t nl=v2?u32(q):0;
    if(nl){
        if(nl && sizeof(*idx->layers)>SIZE_MAX/(size_t)nl)goto fail;
        idx->layers=alloc(context,(size_t)nl*sizeof(*idx->layers));if(!idx->layers)goto fail;
        memset(idx->layers,0,(size_t)nl*sizeof(*idx->layers));idx->layer_count=nl;
        for(uint32_t j=0;j<nl;j++){
            if(string(&p,end,&idx->layers[j].split,idx)||string(&p,end,&idx->layers[j].entry,idx)||take(&p,end,40,&q))goto fail;
            idx->layers[j].image_size=u64(q);memcpy(idx->layers[j].sha256,q+8,32);
        }
        if(take(&p,end,4,&q))goto fail;
    }
    uint32_t n=u32(q);if((size_t)n>((size_t)(end-p)/40)||(n&&sizeof(mtp_object)>SIZE_MAX/(size_t)n))goto fail;
    idx->items=alloc(context,(n?n:1)*sizeof(*idx->items));
    if(!idx->items){dealloc(context,idx);return -1;}
    memset(idx->items,0,(n?n:1)*sizeof(*idx->items));idx->count=n;idx->storage=(void*)bytes;
    for(uint32_t i=0;i<n;i++){
        mtp_object *o=&idx->items[i];const unsigned char *q;
        if(string(&p,end,&o->path,idx)||string(&p,end,&o->checksum,idx))goto fail;
        if(v2){ if(string(&p,end,&o->kind,idx)||string(&p,end,&o->target,idx)||take(&p,end,4,&q))goto fail; o->pack=idx->alloc(context,1);if(!o->pack)goto fail;((char*)o->pack)[0]=0; }
        else if(string(&p,end,&o->pack,idx)||string(&p,end,&o->kind,idx)||string(&p,end,&o->target,idx)||take(&p,end,4,&q))goto fail;
        o->xattrs_length=u32(q);if(take(&p,end,o->xattrs_length,&q))goto fail;o->xattrs=q;
        if(v2){if(take(&p,end,36,&q))goto fail;o->layer=u32(q);o->size=u64(q+4);o->mtime=u64(q+12);o->uid=u32(q+20);o->gid=u32(q+24);o->mode=u32(q+28);if(u32(q+32)|| (o->layer!=UINT32_MAX&&o->layer>=nl))goto fail;}
        else {if(take(&p,end,28,&q))goto fail;o->offset=u64(q);o->length=u64(q+8);o->uid=u32(q+16);o->gid=u32(q+20);o->mode=u32(q+24);o->layer=UINT32_MAX;o->size=o->length;if(o->offset>UINT64_MAX-o->length)goto fail;}
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
        /* Lookup and readdir use the writer's path-sorted order. Keep the
         * synthesized root in that order too; appending it makes binary
         * lookup of "/" fail for indexes that omit an explicit root record. */
        uint32_t at=0;
        while(at<n && strcmp(idx->items[at].path,"/")<0)at++;
        for(uint32_t j=n;j>at;j--)idx->items[j]=idx->items[j-1];
        idx->items[at]=root;
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
void mtp_index_close(mtp_index *i) { if(!i)return; for(size_t n=0;n<i->count;n++){i->dealloc(i->context,(void*)i->items[n].path);i->dealloc(i->context,(void*)i->items[n].checksum);i->dealloc(i->context,(void*)i->items[n].pack);i->dealloc(i->context,(void*)i->items[n].kind);i->dealloc(i->context,(void*)i->items[n].target);}for(size_t n=0;n<i->layer_count;n++){i->dealloc(i->context,(void*)i->layers[n].split);i->dealloc(i->context,(void*)i->layers[n].entry);}i->dealloc(i->context,i->layers);i->dealloc(i->context,i->items);if(i->owns_storage)i->dealloc(i->context,i->storage);i->dealloc(i->context,i); }
size_t mtp_index_count(const mtp_index *i){return i?i->count:0;}
size_t mtp_layer_count(const mtp_index *i){return i?i->layer_count:0;}
int mtp_get_layer(const mtp_index *i,size_t n,mtp_layer *out){if(!i||!out||n>=i->layer_count)return -1;*out=i->layers[n];return 0;}
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
    /* Pack writers sort records by UTF-8 path bytes. Use that stable order as
     * the immutable lookup index instead of rescanning every object on each
     * open/stat. Accept both canonical slash-prefixed paths and older relative
     * records. */
    size_t lo=0,hi=i->count;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(strcmp(i->items[mid].path,key)<0)lo=mid+1;else hi=mid;}
    if(lo<i->count&&!strcmp(i->items[lo].path,key)){*out=i->items[lo];return 0;}
    if(*normalized){char absolute[4097];size_t length=strlen(normalized);absolute[0]='/';for(size_t n=0;n<=length;n++)absolute[n+1]=normalized[n];lo=0;hi=i->count;
        while(lo<hi){size_t mid=lo+(hi-lo)/2;if(strcmp(i->items[mid].path,absolute)<0)lo=mid+1;else hi=mid;}
        if(lo<i->count&&!strcmp(i->items[lo].path,absolute)){*out=i->items[lo];return 0;}}
    return 1;
}
int mtp_stat(const mtp_index *i,const char *path,mtp_object *out) { return mtp_lookup(i,path,out); }
int mtp_readdir(const mtp_index *i,const char *dir,size_t *cursor,const char **name,mtp_object *out) {
    if(!i||!dir||!cursor||!name)return -1;
    char normalized[4096];if(mtp_normalize_path(dir,normalized,sizeof(normalized)))return -1;
    int absolute=i->count&&i->items[0].path[0]=='/';size_t dl=strlen(normalized);
    while(dl&&normalized[dl-1]=='/')normalized[--dl]=0;
    char prefix[4096];size_t prefix_length=0;
    if(absolute)prefix[prefix_length++]='/';
    if(dl){if(dl>sizeof(prefix)-prefix_length-2)return -1;for(size_t n=0;n<dl;n++)prefix[prefix_length++]=normalized[n];}
    if(dl){prefix[prefix_length++]='/';}
    prefix[prefix_length]=0;
    size_t n=*cursor;
    if(!n){size_t lo=0,hi=i->count;while(lo<hi){size_t mid=lo+(hi-lo)/2;if(strcmp(i->items[mid].path,prefix)<0)lo=mid+1;else hi=mid;}n=lo;}
    for(;n<i->count;n++){const char *p=i->items[n].path;size_t l=strlen(p);if(l<prefix_length||memcmp(p,prefix,prefix_length))break;
        const char *child=p+prefix_length;if(!*child||strchr(child,'/'))continue;*cursor=n+1;*name=child;if(out)*out=i->items[n];return 1;}
    *cursor=i->count;return 0;
}
