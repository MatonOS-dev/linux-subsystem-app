/* SPDX-License-Identifier: Apache-2.0 */
/* White-box crash boundary test. The separate workload tests exercise SIGSYS. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../sysvipc.c"
const char *proxy_tmpdir;
void *vfs_alloc(size_t n){return calloc(1,n);}
void vfs_free(void *p){free(p);}
char *vfs_temp_create(long *e){*e=0;return strdup(proxy_tmpdir);}
size_t p_len(const char *p){return strlen(p);}
void p_copy(void *d,const void *s,size_t n){memcpy(d,s,n);}
/* raw.S's unused clone continuation. */
void links_fork(int mode){(void)mode;}
uintptr_t proxy_clone_resume;
static long call(long nr,long x,long y,long z,long w){long a[6]={x,y,z,w,0,0};return ipc_call(nr,a);}
int main(void){
  char dir[]="/tmp/v3-ipc-recovery-XXXXXX";assert(mkdtemp(dir));proxy_tmpdir=dir;assert(!ipc_init());
  int id=(int)call(PROXY_SC_semget,0,2,0600,0);assert(id>=0);
  unsigned short initial[2]={7,11};assert(!call(PROXY_SC_semctl,id,0,SETALL,(long)initial));
  for(int append=0;append<2;append++){
    int pipefd[2];assert(!pipe(pipefd));pid_t child=fork();assert(child>=0);
    if(!child){close(pipefd[0]);lock();struct ctx c={.fd=(int)R(openat,AT_FDCWD,arena_path,O_RDWR,0,0,0)};assert(c.fd>=0);assert(!remap(&c));assert(!begin(&c));
      struct obj *p=find(&c,id);cells(&c,p)[0].value=999; /* partial multi-cell commit */
      if(append){assert(get(&c,1,0x7722,3,IPC_CREAT|0600)>=0);assert(record(&c,1,id,0));}
      assert(write(pipefd[1],"x",1)==1);for(;;)pause();
    }
    close(pipefd[1]);char ch;assert(read(pipefd[0],&ch,1)==1);close(pipefd[0]);assert(!kill(child,SIGKILL));int status;assert(waitpid(child,&status,0)==child);
    unsigned short values[2]={0};assert(!call(PROXY_SC_semctl,id,0,GETALL,(long)values));assert(values[0]==7&&values[1]==11);
    assert(call(PROXY_SC_semget,0x7722,0,0,0)==-ENOENT);assert(!shared->transaction);assert(!shared->poisoned);
  }
  /* A grandchild can remain unscheduled after clone while its parent dies.
   * Its inherited ticket must keep the segment alive through root's RMID. */
  assert(!prctl(PR_SET_CHILD_SUBREAPER,1));int sh=(int)call(PROXY_SC_shmget,0,4096,0600,0);assert(sh>=0);char *mapping=(void *)call(PROXY_SC_shmat,sh,0,0,0);assert((long)mapping>=0);strcpy(mapping,"inherited-through-death");
  int ready[2],release[2],finished[2];assert(!pipe(ready)&&!pipe(release)&&!pipe(finished));pid_t worker=fork();assert(worker>=0);
  if(!worker){ipc_fork();assert(!ipc_fork_prepare());pid_t grandchild=fork();assert(grandchild>=0);
    if(!grandchild){char ch;assert(read(release[0],&ch,1)==1);ipc_fork_result(0);assert(!strcmp(mapping,"inherited-through-death"));struct shmid64_ds d;assert(!call(PROXY_SC_shmctl,sh,IPC_STAT,(long)&d,0));assert(d.shm_nattch==1);assert(!call(PROXY_SC_shmdt,(long)mapping,0,0,0));pid_t me=getpid();assert(write(finished[1],&me,sizeof(me))==sizeof(me));_exit(0);}
    /* Deliberately die before the parent's clone completion hook. */
    assert(write(ready[1],"x",1)==1);for(;;)pause();
  }
  char ch;assert(read(ready[0],&ch,1)==1);assert(!kill(worker,SIGKILL));int status;assert(waitpid(worker,&status,0)==worker);
  assert(!call(PROXY_SC_shmctl,sh,IPC_RMID,0,0));assert(!call(PROXY_SC_shmdt,(long)mapping,0,0,0));struct shmid64_ds d;assert(!call(PROXY_SC_shmctl,sh,IPC_STAT,(long)&d,0));assert(d.shm_nattch==1);
  assert(write(release[1],"x",1)==1);pid_t adopted;assert(read(finished[0],&adopted,sizeof(adopted))==sizeof(adopted));assert(waitpid(adopted,&status,0)==adopted&&WIFEXITED(status)&&!WEXITSTATUS(status));assert(call(PROXY_SC_shmctl,sh,IPC_STAT,(long)&d,0)==-EINVAL);
  int extra_fds[]={ready[0],ready[1],release[0],release[1],finished[0],finished[1]};for(size_t i=0;i<sizeof(extra_fds)/sizeof(*extra_fds);i++)close(extra_fds[i]);
  assert(!call(PROXY_SC_semctl,id,0,IPC_RMID,0));ipc_exit();unlink(journal_path);unlink(arena_path);assert(!rmdir(dir));
  puts("PASS: killed metadata writer rolls back values, object publication, arena growth and records; inherited fork reservation survives parent SIGKILL");
}
