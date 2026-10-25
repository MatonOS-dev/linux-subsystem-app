#include "../reader.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){
 mtp_index *i=NULL;mtp_object o;mtp_layer l;size_t cursor=0;const char *name;
 assert(argc==2);assert(mtp_index_open(argv[1],&i)==0);
 assert(mtp_layer_count(i)==1);assert(mtp_get_layer(i,0,&l)==0);
 assert(!strcmp(l.split,"pack_test")&&!strcmp(l.entry,"assets/matonos/pack.erofs"));
 assert(l.image_size==4096&&l.sha256[0]==0xab);
 assert(mtp_stat(i,"/usr/bin/tool",&o)==0&&!strcmp(o.kind,"file"));
 assert(o.layer==0&&o.size==3&&o.mtime==123&&o.mode==0100755);
 assert(mtp_stat(i,"/usr/bin/missing",&o)==1);
 assert(mtp_readdir(i,"/usr/bin",&cursor,&name,&o)==1&&!strcmp(name,"tool"));
 assert(mtp_readdir(i,"/usr/bin",&cursor,&name,&o)==0);
 mtp_index_close(i);puts("C EROFS layer index reader passed");return 0;
}
