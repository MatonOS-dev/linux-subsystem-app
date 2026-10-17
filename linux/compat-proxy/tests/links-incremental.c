#define _GNU_SOURCE
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#define CHECK(x) do {if(!(x)){perror(#x);return 1;}}while(0)
static double stamp(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static double scan(void){char p[80];struct stat st;double t=stamp();for(int i=0;i<25000;i++){snprintf(p,sizeof(p),"%s/%d",i<20000?"src":"dst",i<20000?i:i-20000);if(stat(p,&st))exit(2);}return stamp()-t;}
int main(int argc,char **argv){CHECK(argc==3);CHECK(!chdir(argv[1]));
 if(!strcmp(argv[2],"scale")){char a[80],b[80];double plain=scan(),t=stamp();for(int i=0;i<5000;i++){snprintf(a,sizeof(a),"src/%d",i);snprintf(b,sizeof(b),"dst/%d",i);CHECK(!unlink(b));CHECK(!link(a,b));}double mutation=stamp()-t,stored=scan();printf("scaling: 20000 files, 5000 links, 25000 stats: plain=%.6fs stored=%.6fs ratio=%.3f mutations=%.6fs\n",plain,stored,stored/plain,mutation);CHECK(stored<5*plain);return 0;}
 if(!strcmp(argv[2],"mutate")){struct stat st;
  errno=0;CHECK(renameat2(AT_FDCWD,"src/2",AT_FDCWD,"src/2",1)==-1 && errno==EEXIST);CHECK(!stat("src/2",&st) && st.st_nlink==2);
  CHECK(!rename("dst/2","dst/3"));CHECK(!stat("src/2",&st) && st.st_nlink==2);CHECK(!stat("src/3",&st) && st.st_nlink==1);
  CHECK(!renameat2(AT_FDCWD,"dst/3",AT_FDCWD,"dst/4",2));CHECK(!stat("src/2",&st) && st.st_nlink==2);CHECK(!stat("src/4",&st) && st.st_nlink==2);
  errno=0;CHECK(link("src/2","dst/4")==-1 && errno==EEXIST);
  CHECK(!unlink("src/3"));puts("PASS overwrite, exchange, failed link and zero-count collection");return 0;
 }
 if(!strcmp(argv[2],"fallback")){struct stat st;for(int i=0;i<100;i++){CHECK(!stat("src/0",&st) && st.st_nlink==1);CHECK(!stat("src/1",&st) && st.st_nlink==1);}puts("PASS corrupt count recounted once then read directly");return 0;}
 if(!strcmp(argv[2],"crash")){CHECK(!link("src/0","dst/crash"));return 0;}
 if(!strcmp(argv[2],"recover")){struct stat st;CHECK(!stat("src/0",&st));CHECK(st.st_nlink==3);CHECK(!stat("src/1",&st));CHECK(st.st_nlink==2);puts("PASS killed intent replay updates affected object only");return 0;}
 return 2;
}
