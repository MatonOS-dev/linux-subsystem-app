#define _GNU_SOURCE
/* Exercise foreign ownership without root/chown privileges. All backing stats
 * are real; only the marker's returned owner is substituted in this fixture. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include "../proxy.h"
static int foreign;
size_t p_len(const char *s){return strlen(s);}
void p_copy(void *d,const void *s,size_t n){memcpy(d,s,n);}
int p_equal(const char *a,const char *b){return !strcmp(a,b);}
long proxy_raw(long n,long a,long b,long c,long d,long e,long f){
 long r=syscall(n,a,b,c,d,e,f);
 if(n==SYS_newfstatat && r==0 && strstr((char *)b,".v3-store-root")){
  assert(d==AT_SYMLINK_NOFOLLOW);
  if(foreign)((struct stat *)c)->st_uid=geteuid()+1;
 }
 return r<0?-errno:r;
}
#include "../links.c"
int main(int argc,char **argv){
 assert(argc==2);assert(marked(argv[1]));foreign=1;assert(!marked(argv[1]));
 puts("PASS foreign-owned regular marker ignored; marker stat always no-follow");
}
