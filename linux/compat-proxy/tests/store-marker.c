#define _GNU_SOURCE
/* Exercise foreign ownership without root/chown privileges. All backing stats
 * are real; only the marker fd's returned owner is substituted here. */
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
 if(n==SYS_openat && strstr((char *)b,".v3-store-root"))
  assert((c&(O_PATH|O_NOFOLLOW))==(O_PATH|O_NOFOLLOW));
 if(n==SYS_fstat && r==0 && foreign)
  ((struct stat *)b)->st_uid=geteuid()+1;
 return r<0?-errno:r;
}
#include "../links.c"
int main(int argc,char **argv){
 assert(argc==2);
 assert(app_install_path("/data/user/0/org.example.app/install"));
 assert(app_install_path("/data/user/10/org.example.app/install"));
 assert(app_install_path("/data/user_de/12/org.example.app/install"));
 assert(app_install_path("/data/data/org.example.app/install"));
 assert(!app_install_path("/data/user/0/org.example.app/files/install"));
 assert(!app_install_path("/tmp/org.example.app/install"));
 assert(!app_install_path("/data/userx/0/org.example.app/install"));
 assert(marked_device(argv[1],NULL));foreign=1;assert(!marked_device(argv[1],NULL));
 puts("PASS foreign-owned regular marker ignored; marker stat always no-follow");
}
