#include "../reader.h"
#include <assert.h>
#include <stdio.h>
int main(int argc,char **argv){mtp_index *i=NULL;mtp_object o;size_t c=0;const char *name;
 assert(argc==2);assert(mtp_index_open(argv[1],&i)==0);assert(mtp_index_count(i)==3);
 assert(mtp_lookup(i,"/usr/bin/a",&o)==0&&o.length==8192&&o.offset%4096==0);
 int n=0,r;while((r=mtp_readdir(i,"/usr/bin",&c,&name,&o))>0)n++;
 assert(r==0&&n==3);mtp_index_close(i);puts("C reader passed");return 0;}
