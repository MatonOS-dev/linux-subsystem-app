/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

union semun { int val; struct semid_ds *buf; unsigned short *array; struct seminfo *__buf; };
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (errno=%d %s)\n",__FILE__,__LINE__,#x,errno,strerror(errno)); exit(1); } } while (0)
#define ERROR(x,e) do { errno=0; CHECK((x)==-1 && errno==(e)); } while (0)
static void done(pid_t p) { int st; CHECK(waitpid(p,&st,0)==p); CHECK(WIFEXITED(st)&&WEXITSTATUS(st)==0); }
static void pause_ms(void) { struct timespec ts={0,1000000}; nanosleep(&ts,NULL); }
static void wait_count(int id,int cmd,int count) { for(int i=0;i<5000;i++){ if(semctl(id,0,cmd)==count)return; pause_ms(); } CHECK(0); }
static void semaphores(void) {
    int id=semget(0x4d410001,2,IPC_CREAT|IPC_EXCL|0600); CHECK(id>=0);
    ERROR(semget(0x4d410001,2,IPC_CREAT|IPC_EXCL|0600),EEXIST);
    CHECK(semget(0x4d410001,0,0)==id); ERROR(semget(0x4d410001,3,0),EINVAL);
    struct semid_ds ds; CHECK(!semctl(id,0,IPC_STAT,(union semun){.buf=&ds})); CHECK(ds.sem_nsems==2);
    struct seminfo info; CHECK(semctl(0,0,IPC_INFO,(union semun){.__buf=&info})>=0); CHECK(info.semvmx==32767&&info.semopm==500);
    unsigned short values[2]={1,0}; CHECK(!semctl(id,0,SETALL,(union semun){.array=values}));
    struct sembuf atomic[2]={{0,-1,IPC_NOWAIT},{1,-1,IPC_NOWAIT}};
    ERROR(semop(id,atomic,2),EAGAIN); CHECK(semctl(id,0,GETVAL)==1);
    ERROR(semctl(id,0,SETVAL,(union semun){.val=32768}),ERANGE);
    struct sembuf bad={2,1,0}; ERROR(semop(id,&bad,1),EFBIG);
    ERROR(syscall(SYS_semop,id,(void *)1,1),EFAULT);
    CHECK(!semctl(id,0,SETVAL,(union semun){.val=0}));
    struct sembuf take={0,-1,0},post={0,1,0};
    pid_t p=fork(); CHECK(p>=0); if(!p){CHECK(!semop(id,&take,1));_exit(0);}
    wait_count(id,GETNCNT,1); CHECK(!semop(id,&post,1)); done(p);
    struct timespec timeout={0,20000000}; ERROR(semtimedop(id,&take,1,&timeout),EAGAIN);
    struct timespec invalid={-1,0}; ERROR(semtimedop(id,&take,1,&invalid),EINVAL);
    CHECK(!semctl(id,0,SETVAL,(union semun){.val=1}));
    struct sembuf zero={0,0,0}; p=fork(); CHECK(p>=0); if(!p){CHECK(!semop(id,&zero,1));_exit(0);}
    wait_count(id,GETZCNT,1); CHECK(!semop(id,&take,1)); done(p);
    p=fork(); CHECK(p>=0); if(!p){ ERROR(semop(id,&take,1),EIDRM);_exit(0); }
    wait_count(id,GETNCNT,1); CHECK(!semctl(id,0,IPC_RMID)); done(p);
    id=semget(IPC_PRIVATE,1,0600); CHECK(id>=0); CHECK(!semctl(id,0,SETVAL,(union semun){.val=1}));
    int pipefd[2]; CHECK(!pipe(pipefd)); p=fork(); CHECK(p>=0);
    if(!p){close(pipefd[0]);struct sembuf undo={0,-1,SEM_UNDO};CHECK(!semop(id,&undo,1));CHECK(write(pipefd[1],"x",1)==1);for(;;)pause();}
    close(pipefd[1]); char c; CHECK(read(pipefd[0],&c,1)==1);close(pipefd[0]);CHECK(semctl(id,0,GETVAL)==0);
    CHECK(!kill(p,SIGKILL)); int st;CHECK(waitpid(p,&st,0)==p);CHECK(WIFSIGNALED(st));
    CHECK(!semop(id,&take,1)); CHECK(semctl(id,0,GETPID)==getpid());
    CHECK(!semctl(id,0,IPC_RMID)); puts("PASS: sem wait/post, atomicity, NOWAIT, timeout, RMID, SEM_UNDO kill -9");
}
static void shared_memory(void) {
    int id=shmget(0x4d410002,123,IPC_CREAT|IPC_EXCL|0600); CHECK(id>=0);
    struct shmid_ds ds; CHECK(!shmctl(id,IPC_STAT,&ds));CHECK(ds.shm_segsz==123&&ds.shm_nattch==0);
    char *p=shmat(id,NULL,0);CHECK(p!=(void *)-1);strcpy(p,"shared-by-memfd");
    CHECK(!shmctl(id,IPC_STAT,&ds));CHECK(ds.shm_nattch==1);
    int fd[2];CHECK(!pipe(fd));pid_t child=fork();CHECK(child>=0);
    if(!child){close(fd[0]);CHECK(!strcmp(p,"shared-by-memfd"));char *q=shmat(id,NULL,SHM_RDONLY);CHECK(q!=(void *)-1);CHECK(!strcmp(q,p));CHECK(!shmdt(q));strcpy(p,"child-write");CHECK(write(fd[1],"x",1)==1);CHECK(!shmdt(p));_exit(0);}
    close(fd[1]);char c;CHECK(read(fd[0],&c,1)==1);close(fd[0]);done(child);CHECK(!strcmp(p,"child-write"));
    CHECK(!shmctl(id,IPC_STAT,&ds));CHECK(ds.shm_nattch==1);
    CHECK(!shmctl(id,IPC_RMID,NULL));CHECK(!shmctl(id,IPC_STAT,&ds));CHECK(ds.shm_perm.mode&SHM_DEST);
    CHECK(!shmdt(p));ERROR(shmctl(id,IPC_STAT,&ds),EINVAL);ERROR(shmdt(p),EINVAL);
    id=shmget(IPC_PRIVATE,4096,0600);CHECK(id>=0);errno=0;CHECK(shmat(id,(void *)1,0)==(void *)-1&&errno==EINVAL);
    CHECK(!shmctl(id,IPC_STAT,&ds));CHECK(ds.shm_nattch==0);CHECK(!shmctl(id,IPC_RMID,NULL));
    puts("PASS: shm mappings, fork attachments, shared data, nattch, RMID after last detach");
}
struct message { long type; char data[64]; };
static void messages(void) {
    int id=msgget(0x4d410003,IPC_CREAT|IPC_EXCL|0600);CHECK(id>=0);
    struct message m={.type=7,.data="seven"};CHECK(!msgsnd(id,&m,6,0));m.type=2;strcpy(m.data,"two");CHECK(!msgsnd(id,&m,4,0));
    CHECK(msgrcv(id,&m,sizeof(m.data),-7,0)==4);CHECK(m.type==2&&!strcmp(m.data,"two"));
    ERROR(msgrcv(id,&m,1,7,IPC_NOWAIT),E2BIG);CHECK(msgrcv(id,&m,1,7,MSG_NOERROR)==1&&m.type==7);
    ERROR(msgrcv(id,&m,sizeof(m.data),0,IPC_NOWAIT),ENOMSG);
    m.type=2;CHECK(!msgsnd(id,&m,0,0));m.type=9;CHECK(!msgsnd(id,&m,0,0));CHECK(msgrcv(id,&m,0,2,MSG_EXCEPT)==0&&m.type==9);CHECK(msgrcv(id,&m,0,0,0)==0&&m.type==2);
    pid_t p=fork();CHECK(p>=0);if(!p){CHECK(msgrcv(id,&m,sizeof(m.data),42,0)==6);CHECK(!strcmp(m.data,"wake!"));_exit(0);}
    struct timespec delay={0,20000000};nanosleep(&delay,0);int status;CHECK(waitpid(p,&status,WNOHANG)==0);
    m.type=42;strcpy(m.data,"wake!");CHECK(!msgsnd(id,&m,6,0));done(p);
    struct msqid_ds ds;CHECK(!msgctl(id,IPC_STAT,&ds));ds.msg_qbytes=4;CHECK(!msgctl(id,IPC_SET,&ds));
    m.type=1;CHECK(!msgsnd(id,&m,4,0));ERROR(msgsnd(id,&m,1,IPC_NOWAIT),EAGAIN);
    p=fork();CHECK(p>=0);if(!p){CHECK(!msgsnd(id,&m,4,0));_exit(0);}
    nanosleep(&delay,0);CHECK(waitpid(p,&status,WNOHANG)==0);
    CHECK(msgrcv(id,&m,4,0,0)==4);done(p);CHECK(msgrcv(id,&m,4,0,0)==4);
    ERROR(msgsnd(id,&(struct message){.type=0},0,0),EINVAL);
    CHECK(!msgctl(id,IPC_RMID,NULL));puts("PASS: msg typed/negative/EXCEPT receive, NOERROR, blocking send/receive, qbytes");
}
int main(void){alarm(30);semaphores();shared_memory();messages();return 0;}
