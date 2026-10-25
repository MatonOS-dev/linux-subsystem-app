#include "../reader.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){
 mtp_index *i=NULL;mtp_object o;mtp_layer l;size_t cursor=0;const char *name;
 assert(argc==2);assert(mtp_index_open(argv[1],&i)==0);
 assert(mtp_layer_count(i)>0);assert(mtp_get_layer(i,0,&l)==0);
 assert(!strncmp(l.split,"pack_",5)&&!strcmp(l.entry,"assets/matonos/pack.erofs"));
 assert(l.image_size>0);
 assert(mtp_stat(i,"/usr/bin/tool",&o)==0&&!strcmp(o.kind,"file"));
 fprintf(stderr,"reader tool layer=%u count=%zu size=%llu mtime=%llu mode=%o\n",o.layer,mtp_layer_count(i),(unsigned long long)o.size,(unsigned long long)o.mtime,o.mode);
 assert(o.layer<mtp_layer_count(i)&&o.size>0&&o.mtime>0&&o.mode==0100755);
 assert(mtp_stat(i,"/usr/bin/missing",&o)==1);
 assert(mtp_readdir(i,"/usr/bin",&cursor,&name,&o)==1&&!strcmp(name,"tool"));
 assert(mtp_readdir(i,"/usr/bin",&cursor,&name,&o)==0);
 mtp_index_close(i);puts("C EROFS layer index reader passed");return 0;
}
