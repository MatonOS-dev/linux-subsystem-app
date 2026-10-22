/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
union semun { int val; struct semid_ds *buf; unsigned short *array; struct seminfo *__buf; };
#define CHECK(x) do{if(!(x)){fprintf(stderr,"edge line %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
#define ERROR(x,e) do{errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static void done(pid_t p){int st;CHECK(waitpid(p,&st,0)==p);CHECK(WIFEXITED(st)&&!WEXITSTATUS(st));}
static void waiters(int id,int n){for(int i=0;i<5000;i++){if(semctl(id,0,GETNCNT)==n)return;struct timespec t={0,1000000};nanosleep(&t,0);}CHECK(0);}
static void *attach_thread(void *arg){int id=*(int *)arg;for(int i=0;i<20;i++){void *p=shmat(id,0,SHM_RDONLY);CHECK(p!=(void *)-1);CHECK(!shmdt(p));}return 0;}
int main(void){
    alarm(30);int id=semget(IPC_PRIVATE,2,0600);CHECK(id>=0);
    struct semid_ds ds;CHECK(!semctl(id,0,IPC_STAT,(union semun){.buf=&ds}));
    ds.sem_perm.mode=0200;CHECK(!semctl(id,0,IPC_SET,(union semun){.buf=&ds}));
    ERROR(semctl(id,0,GETVAL),EACCES);
    CHECK(semctl(id&32767,0,SEM_STAT_ANY,(union semun){.buf=&ds})==id);
    ds.sem_perm.mode=0600;CHECK(!semctl(id,0,IPC_SET,(union semun){.buf=&ds}));
    CHECK(semctl(id&32767,0,SEM_STAT,(union semun){.buf=&ds})==id);
    CHECK(!semctl(id,0,SETVAL,(union semun){.val=1}));
    struct sembuf duplicate[2]={{0,-1,0},{0,1,0}};CHECK(!semop(id,duplicate,2));CHECK(semctl(id,0,GETVAL)==1);
    CHECK(!semctl(id,0,SETVAL,(union semun){.val=0}));
    pid_t p=fork();CHECK(p>=0);if(!p){struct sembuf take={0,-1,0};(void)semop(id,&take,1);_exit(1);}
    waiters(id,1);CHECK(!kill(p,SIGKILL));int st;CHECK(waitpid(p,&st,0)==p);waiters(id,0);
    CHECK(!semctl(id,0,SETVAL,(union semun){.val=1}));CHECK(semctl(id,0,GETVAL)==1);
    CHECK(!semctl(id,0,IPC_RMID));ERROR(semctl(id,0,GETVAL),EINVAL);
    int q=msgget(IPC_PRIVATE,0600);CHECK(q>=0);struct {long type;char text[8];} msg={1,"edge"};
    CHECK(!msgsnd(q,&msg,5,0));ERROR(msgrcv(q,(void *)1,8,0,0),EFAULT);CHECK(msgrcv(q,&msg,8,0,IPC_NOWAIT)==5);CHECK(msg.type==1&&!memcmp(msg.text,"edge",5));ERROR(msgrcv(q,&msg,8,0,IPC_NOWAIT),ENOMSG);
    int pipefd[2];CHECK(!pipe(pipefd));p=fork();CHECK(p>=0);
    if(!p){close(pipefd[0]);CHECK(write(pipefd[1],"x",1)==1);ERROR(msgrcv(q,&msg,8,0,0),EIDRM);_exit(0);}
    close(pipefd[1]);char c;CHECK(read(pipefd[0],&c,1)==1);close(pipefd[0]);
    /* A semaphore round trip plus a short delay gives the receive notification
     * time to reach the queue before RMID. */
    struct timespec t={0,20000000};nanosleep(&t,0);CHECK(!msgctl(q,IPC_RMID,0));done(p);
    int sh=shmget(IPC_PRIVATE,4096,0600);CHECK(sh>=0);
    pthread_t threads[2];for(int i=0;i<2;i++)CHECK(!pthread_create(&threads[i],0,attach_thread,&sh));for(int i=0;i<2;i++)CHECK(!pthread_join(threads[i],0));
    struct shmid_ds sd;CHECK(!shmctl(sh,IPC_STAT,&sd)&&sd.shm_nattch==0);
    char *a=shmat(sh,0,0);CHECK(a!=(void *)-1);a[0]=19;
    char *b=shmat(sh,a,SHM_REMAP);CHECK(b==a&&b[0]==19);CHECK(!shmctl(sh,IPC_STAT,&sd)&&sd.shm_nattch==1);
    int replacement=shmget(IPC_PRIVATE,4096,0600);CHECK(replacement>=0);
    b=shmat(replacement,a,SHM_REMAP);CHECK(b==a);CHECK(!shmctl(sh,IPC_STAT,&sd)&&sd.shm_nattch==0);
    CHECK(!shmctl(replacement,IPC_STAT,&sd)&&sd.shm_nattch==1);CHECK(!shmdt(b));CHECK(!shmctl(replacement,IPC_RMID,0));
    void *reserved=mmap(0,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(reserved!=MAP_FAILED);
    errno=0;CHECK(shmat(sh,reserved,0)==(void *)-1&&errno==EEXIST);CHECK(!munmap(reserved,4096));
    CHECK(!shmctl(sh,IPC_STAT,&sd)&&sd.shm_nattch==0);
    ERROR(shmctl(sh,SHM_LOCK,0),ENOSYS);ERROR(shmctl(sh,SHM_UNLOCK,0),ENOSYS);
    CHECK(!shmctl(sh,IPC_RMID,0));puts("PASS: permissions/STAT_ANY, duplicate atomic ops, killed waiter, msg EFAULT/RMID, threaded shm, REMAP/cancel; LOCK/UNLOCK explicitly unsupported");return 0;
}
