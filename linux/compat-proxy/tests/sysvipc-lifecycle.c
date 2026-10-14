/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <spawn.h>
extern char **environ;
#include <sys/syscall.h>
#include <time.h>
union semun { int val;struct semid_ds *buf;unsigned short *array; };
static void done(pid_t p){int st;assert(waitpid(p,&st,0)==p);assert(WIFEXITED(st)&&!WEXITSTATUS(st));}
static int worker_ready,worker_release;
static void *last_thread(void *unused){(void)unused;struct timespec delay={0,30000000};nanosleep(&delay,0);assert(write(worker_ready,"x",1)==1);char c;assert(read(worker_release,&c,1)==1);syscall(SYS_exit,0);return 0;}
int main(int argc,char **argv){
  if(argc==4){int sh=atoi(argv[1]),sem=atoi(argv[2]);struct shmid_ds d;assert(!shmctl(sh,IPC_STAT,&d));assert(d.shm_nattch==1);char *p=shmat(sh,0,0);assert(p!=(void *)-1);assert(!strcmp(p,"before-exec"));strcpy(p,"after-exec");assert(!shmdt(p));assert(semctl(sem,0,GETVAL)==0);return 0;}
  int sh=shmget(IPC_PRIVATE,8192,0600),sem=semget(IPC_PRIVATE,2,0600);assert(sh>=0&&sem>=0);char *p=shmat(sh,0,0);assert(p!=(void *)-1);strcpy(p,"before-exec");
  assert(!semctl(sem,0,SETVAL,(union semun){.val=1}));pid_t child=fork();assert(child>=0);
  if(!child){struct sembuf take={0,-1,SEM_UNDO};assert(!semop(sem,&take,1));char a[32],b[32];snprintf(a,sizeof(a),"%d",sh);snprintf(b,sizeof(b),"%d",sem);execl(argv[0],argv[0],a,b,"exec",NULL);_exit(100);}
  done(child);assert(!strcmp(p,"after-exec"));assert(semctl(sem,0,GETVAL)==1);
  strcpy(p,"before-exec");assert(!semctl(sem,0,SETVAL,(union semun){.val=0}));char sa[32],sb[32];snprintf(sa,sizeof(sa),"%d",sh);snprintf(sb,sizeof(sb),"%d",sem);char *spawn_args[]={argv[0],sa,sb,"exec",NULL};assert(!posix_spawn(&child,argv[0],NULL,NULL,spawn_args,environ));done(child);assert(!strcmp(p,"after-exec"));
  unsigned short values[2]={1,0};assert(!semctl(sem,0,SETALL,(union semun){.array=values}));
  child=fork();assert(child>=0);if(!child){for(int i=0;i<200;i++){struct sembuf ops[2]={{0,-1,0},{1,1,0}};assert(!semop(sem,ops,2));}return 0;}
  for(int i=0;i<200;i++){struct sembuf ops[2]={{1,-1,0},{0,1,0}};assert(!semop(sem,ops,2));}done(child);
  struct shmid_ds d;assert(!shmctl(sh,IPC_STAT,&d));assert(d.shm_nattch==1);assert(!shmctl(sh,IPC_RMID,0));assert(!shmdt(p));assert(shmctl(sh,IPC_STAT,&d)<0&&errno==EINVAL);assert(!semctl(sem,0,IPC_RMID));
  int fds[2];assert(!pipe(fds));child=fork();assert(child>=0);
  if(!child){close(fds[0]);int s=semget(0x663311,1,IPC_CREAT|IPC_EXCL|0600);int h=shmget(0x663312,4096,IPC_CREAT|IPC_EXCL|0600);assert(s>=0&&h>=0);char *x=shmat(h,0,0);assert(x!=(void *)-1);strcpy(x,"created-after-fork");int ids[2]={s,h};assert(write(fds[1],ids,sizeof(ids))==sizeof(ids));struct sembuf take={0,-1,0};assert(!semop(s,&take,1));assert(!strcmp(x,"parent-found-it"));assert(!shmdt(x));assert(!shmctl(h,IPC_RMID,0));assert(!semctl(s,0,IPC_RMID));return 0;}
  close(fds[1]);int ids[2];assert(read(fds[0],ids,sizeof(ids))==sizeof(ids));close(fds[0]);assert(semget(0x663311,0,0)==ids[0]);assert(shmget(0x663312,0,0)==ids[1]);char *x=shmat(ids[1],0,0);assert(x!=(void *)-1);assert(!strcmp(x,"created-after-fork"));strcpy(x,"parent-found-it");assert(!shmdt(x));struct sembuf post={0,1,0};assert(!semop(ids[0],&post,1));done(child);
  sem=semget(IPC_PRIVATE,1,0600);assert(sem>=0);assert(!semctl(sem,0,SETVAL,(union semun){.val=1}));int ready[2],release[2];assert(!pipe(ready)&&!pipe(release));child=fork();assert(child>=0);
  if(!child){close(ready[0]);close(release[1]);worker_ready=ready[1];worker_release=release[0];struct sembuf take={0,-1,SEM_UNDO};assert(!semop(sem,&take,1));pthread_t thread;assert(!pthread_create(&thread,0,last_thread,0));pthread_exit(0);}
  close(ready[1]);close(release[0]);char c;assert(read(ready[0],&c,1)==1);close(ready[0]);assert(semctl(sem,0,GETVAL)==0);assert(write(release[1],"x",1)==1);close(release[1]);done(child);assert(semctl(sem,0,GETVAL)==1);assert(!semctl(sem,0,IPC_RMID));
  puts("PASS: fork/exec shared namespace, exec detach, SEM_UNDO retained until exit, 200-round process ping-pong, objects created after fork, last-thread exit undo");return 0;
}
