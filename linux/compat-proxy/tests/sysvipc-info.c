/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/syscall.h>
#include <unistd.h>
union semun { int val;unsigned short *array;struct seminfo *__buf;struct semid_ds *buf; };
int main(void){
  struct seminfo si;assert(semctl(0,0,IPC_INFO,(union semun){.__buf=&si})>=0);assert(si.semmsl==32000&&si.semmni==32000&&si.semmns==1024000000&&si.semopm==500&&si.semvmx==32767&&si.semaem==32767);
  int sem=semget(IPC_PRIVATE,si.semmsl,0600);assert(sem>=0);assert(!semctl(sem,31999,SETVAL,(union semun){.val=32767}));assert(semctl(sem,31999,GETVAL)==32767);
  assert(semget(IPC_PRIVATE,32001,0600)<0&&errno==EINVAL);assert(semctl(sem,32000,GETVAL)<0&&errno==EINVAL);
  struct semid_ds sd;assert(semctl(sem,0,IPC_STAT,(union semun){.buf=(void *)1})<0&&errno==EFAULT);assert(!semctl(sem,0,IPC_STAT,(union semun){.buf=&sd}));assert(sd.sem_nsems==32000);assert(!semctl(sem,0,IPC_RMID));int old=sem;sem=semget(IPC_PRIVATE,1,0600);assert(sem>=0&&sem!=old);assert(semctl(old,0,GETVAL)<0&&errno==EINVAL);assert(semctl(sem&32767,0,SEM_STAT,(union semun){.buf=&sd})==sem);assert(!semctl(sem,0,IPC_RMID));
  struct shminfo hi;assert(shmctl(0,IPC_INFO,(void *)&hi)>=0);assert(hi.shmmax==ULONG_MAX-(1UL<<24)&&hi.shmmin==1&&hi.shmmni==4096&&hi.shmall==ULONG_MAX-(1UL<<24));
  assert(shmget(IPC_PRIVATE,0,0600)<0&&errno==EINVAL);int sh=shmget(IPC_PRIVATE,8193,0600);assert(sh>=0);struct shmid_ds hd;assert(!shmctl(sh,IPC_STAT,&hd));assert(hd.shm_segsz==8193);
  assert(shmctl(sh,IPC_STAT,(void *)1)<0&&errno==EFAULT);assert(!shmctl(sh,IPC_RMID,0));
  struct msginfo mi;assert(msgctl(0,IPC_INFO,(void *)&mi)>=0);assert(mi.msgmni==32000&&mi.msgmax==8192&&mi.msgmnb==16384);
  int q=msgget(IPC_PRIVATE,0600);assert(q>=0);struct msqid_ds md;assert(!msgctl(q,IPC_STAT,&md));assert(md.msg_qbytes==16384&&!md.msg_qnum);assert(msgctl(q,IPC_STAT,(void *)1)<0&&errno==EFAULT);assert(!msgctl(q,IPC_RMID,0));
  puts("PASS: full 32000-semaphore set, kernel default IPC_INFO limits, native ABI sizes, bounds and EFAULT");return 0;
}
