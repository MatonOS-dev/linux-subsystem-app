#define _POSIX_C_SOURCE 200809L
#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mtp_index { mtp_object *items; size_t count; void *storage; };
static uint32_t u32(const unsigned char *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t u64(const unsigned char *p) { return (uint64_t)u32(p)|(uint64_t)u32(p+4)<<32; }
static int take(const unsigned char **p, const unsigned char *end, size_t n, const unsigned char **out) {
    if ((size_t)(end-*p)<n) return -1;
    *out=*p;
    *p+=n;
    return 0;
}
static int string(const unsigned char **p,const unsigned char *end,const char **out) {
    const unsigned char *q; if(take(p,end,4,&q))return -1; uint32_t n=u32(q);
    if(take(p,end,n,&q))return -1;
    char *s=malloc((size_t)n+1);
    if(!s)return -1;
    memcpy(s,q,n);s[n]=0;*out=s;return 0;
}
int mtp_index_open(const char *filename, mtp_index **out) {
    FILE *f=fopen(filename,"rb"); if(!f)return -1;
    if(fseek(f,0,SEEK_END)){fclose(f);return -1;} long z=ftell(f);
    if(z<12 || fseek(f,0,SEEK_SET)){fclose(f);return -1;}
    unsigned char *b=malloc((size_t)z); if(!b){fclose(f);return -1;}
    if(fread(b,1,(size_t)z,f)!=(size_t)z){free(b);fclose(f);return -1;} fclose(f);
    if(memcmp(b,"MTPKIDX1",8)){free(b);return -1;}
    uint32_t n=u32(b+8); if((size_t)n > ((size_t)z-12)/40){free(b);return -1;}
    mtp_index *idx=calloc(1,sizeof(*idx)); if(!idx){free(b);return -1;}
    idx->items=calloc(n?n:1,sizeof(*idx->items)); if(!idx->items){free(idx);free(b);return -1;}
    idx->count=n;idx->storage=b; const unsigned char *p=b+12,*end=b+z;
    for(uint32_t i=0;i<n;i++) {
        mtp_object *o=&idx->items[i]; const unsigned char *q;
        if(string(&p,end,&o->path)||string(&p,end,&o->checksum)||string(&p,end,&o->pack)||string(&p,end,&o->kind)||string(&p,end,&o->target)||take(&p,end,4,&q))goto fail;
        o->xattrs_length=u32(q);if(take(&p,end,o->xattrs_length,&q))goto fail;o->xattrs=q;
        if(take(&p,end,28,&q))goto fail;
        o->offset=u64(q);o->length=u64(q+8);o->uid=u32(q+16);o->gid=u32(q+20);o->mode=u32(q+24);
    }
    if(p!=end)goto fail;
    *out=idx;
    return 0;
fail: mtp_index_close(idx);return -1;
}
void mtp_index_close(mtp_index *i) { if(!i)return; for(size_t n=0;n<i->count;n++){free((void*)i->items[n].path);free((void*)i->items[n].checksum);free((void*)i->items[n].pack);free((void*)i->items[n].kind);free((void*)i->items[n].target);}free(i->items);free(i->storage);free(i); }
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
    for(size_t n=0;n<i->count;n++)if(!strcmp(i->items[n].path,path)){*out=i->items[n];return 0;}
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
