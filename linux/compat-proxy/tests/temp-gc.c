#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../proxy.h"
const char *proxy_tmpdir;
/* This VFS-only fixture has no in-flight IPC fork descriptors. */
int ipc_private_fd(int fd){(void)fd;return 0;}
int state_private_fd(int fd){(void)fd;return 0;}
int proxy_trace;
void proxy_log(const char *m,long a,long b){(void)m;(void)a;(void)b;}
size_t p_len(const char *s){return strlen(s);}
void p_copy(void *d,const void *s,size_t n){memcpy(d,s,n);}
int p_equal(const char *a,const char *b){return !strcmp(a,b);}
long proxy_raw(long n,long a,long b,long c,long d,long e,long f){long r=syscall(n,a,b,c,d,e,f);return r<0?-errno:r;}
#include "../vfs.c"
int main(int argc,char **argv){
  assert(argc==2);proxy_tmpdir=argv[1];char live[512],stale[512],legacy[512],outside[512],link[520];
  snprintf(live,sizeof(live),"%s/compat-proxy-%d-1",argv[1],getpid());
  snprintf(stale,sizeof(stale),"%s/compat-proxy-%d-2",argv[1],getpid());
  snprintf(legacy,sizeof(legacy),"%s/compat-proxy-%d-3",argv[1],getpid());
  snprintf(outside,sizeof(outside),"%s/keep",argv[1]);
  assert(!mkdir(live,0700)&&!mkdir(stale,0700)&&!mkdir(legacy,0700)&&!mkdir(outside,0700));
  assert(!temp_own(live)&&!temp_own(stale));
  assert(temp_locks->fd>=512&&temp_locks->next->fd>=512);
  /* An unlocked marker is stale even when the PID in the name exists. */
  close(temp_locks->fd);temp_locks=temp_locks->next;
  snprintf(link,sizeof(link),"%s/link",stale);assert(!symlink(outside,link));
  int sync[2];assert(!pipe(sync));pid_t child=fork();assert(child>=0);
  if(!child){close(sync[1]);char b;assert(read(sync[0],&b,1)==0);_exit(0);}
  close(sync[0]);assert(vfs_private_fd(temp_locks->fd));close(temp_locks->fd);
  vfs_temp_gc();assert(!access(live,F_OK)&&access(stale,F_OK)<0&&!access(legacy,F_OK)&&!access(outside,F_OK));
  close(sync[1]);assert(waitpid(child,NULL,0)==child);
  vfs_temp_gc();assert(access(live,F_OK)<0&&!access(legacy,F_OK)&&!access(outside,F_OK));
  puts("PASS stale/reused PID, live inherited flock, legacy live PID and symlink-safe cleanup");
}
