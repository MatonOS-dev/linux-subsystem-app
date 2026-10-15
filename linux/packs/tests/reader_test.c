#include "../reader.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){mtp_index *i=NULL;mtp_object o;size_t c=0;const char *name;char path[128];
 assert(argc==2);assert(mtp_index_open(argv[1],&i)==0);assert(mtp_index_count(i)>=6);
 assert(mtp_normalize_path("//usr/./bin/tool",path,sizeof(path))==0&&!strcmp(path,"usr/bin/tool"));
 assert(mtp_normalize_path("usr/../etc/passwd",path,sizeof(path))<0);
 assert(mtp_lookup(i,"/",&o)==0&&!strcmp(o.kind,"dir"));
 assert(mtp_stat(i,"/usr/bin/tool",&o)==0&&!strcmp(o.kind,"file")&&(o.mode&0111));
 assert(mtp_lookup(i,"/usr/share/doc/link",&o)==0&&!strcmp(o.kind,"symlink")&&!strcmp(o.target,"nested/a"));
 int n=0,r;while((r=mtp_readdir(i,"/usr/bin",&c,&name,&o))>0)n++;
 assert(r==0&&n==1);mtp_index_close(i);puts("C reader passed");return 0;}
