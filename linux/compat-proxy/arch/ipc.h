/* Kernel IPC wire adapters. Shared namespace records never embed these
 * native structures; the backend supplies split-time fields where needed. */
#ifndef PROXY_ARCH_IPC_H
#define PROXY_ARCH_IPC_H
#include <linux/sem.h>
#include <linux/shm.h>
#include <linux/msg.h>
struct proxy_ipc_perm {int32_t key;uint32_t uid,gid,cuid,cgid,mode,seq,reserved;};
struct proxy_ipc_ds {
 struct proxy_ipc_perm perm;
 uint64_t count,size,bytes,qbytes;
 int64_t ctime,atime,dtime;
 int32_t cpid,lpid;
};
static inline struct proxy_ipc_perm arch_ipc_perm_decode(struct ipc64_perm p){
 return (struct proxy_ipc_perm){p.key,p.uid,p.gid,p.cuid,p.cgid,p.mode,p.seq,0};
}
static inline struct ipc64_perm arch_ipc_perm_encode(struct proxy_ipc_perm p){
 struct ipc64_perm w={0};w.key=p.key;w.uid=p.uid;w.gid=p.gid;
 w.cuid=p.cuid;w.cgid=p.cgid;w.mode=p.mode;w.seq=p.seq;return w;
}
static inline long arch_ipc_ds_read(int kind,void *buffer,struct proxy_ipc_ds *out,
 long (*copy)(void *,const void *,size_t)){
 *out=(struct proxy_ipc_ds){0};long r;
 if(kind==1){struct semid64_ds w;r=copy(&w,buffer,sizeof(w));if(!r)out->perm=arch_ipc_perm_decode(w.sem_perm);}
 else if(kind==2){struct shmid64_ds w;r=copy(&w,buffer,sizeof(w));if(!r)out->perm=arch_ipc_perm_decode(w.shm_perm);}
 else{struct msqid64_ds w;r=copy(&w,buffer,sizeof(w));if(!r){out->perm=arch_ipc_perm_decode(w.msg_perm);out->qbytes=w.msg_qbytes;}}
 return r;
}
static inline long arch_ipc_ds_write(int kind,void *buffer,const struct proxy_ipc_ds *s,
 long (*copy)(void *,const void *,size_t)){
 if(kind==1){struct semid64_ds w={0};w.sem_perm=arch_ipc_perm_encode(s->perm);w.sem_nsems=s->count;
  ARCH_IPC_SEM_TIMES(w,s->atime,s->ctime);return copy(buffer,&w,sizeof(w));}
 if(kind==2){struct shmid64_ds w={0};w.shm_perm=arch_ipc_perm_encode(s->perm);w.shm_segsz=s->size;
  w.shm_nattch=s->count;w.shm_cpid=s->cpid;w.shm_lpid=s->lpid;
  ARCH_IPC_SHM_TIMES(w,s->atime,s->dtime,s->ctime);return copy(buffer,&w,sizeof(w));}
 struct msqid64_ds w={0};w.msg_perm=arch_ipc_perm_encode(s->perm);w.msg_qnum=s->count;
 w.msg_qbytes=s->qbytes;w.msg_cbytes=s->bytes;w.msg_lspid=s->cpid;w.msg_lrpid=s->lpid;
 ARCH_IPC_MSG_TIMES(w,s->atime,s->dtime,s->ctime);return copy(buffer,&w,sizeof(w));
}
#endif
