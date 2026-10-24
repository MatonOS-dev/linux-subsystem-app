#define _GNU_SOURCE
/* SPDX-License-Identifier: Apache-2.0 */
#include "proxy.h"
#include "vfs.h"
#include "state.h"
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <elf.h>
#include <linux/memfd.h>
int state_android, state_gpu_bridge;
static int private_fd=-1;
/* Move a fresh private descriptor to the private range, keeping its
 * close-on-exec state. Consumes fd; returns the new fd or an error. */
long proxy_private_fd(long fd){
 if(fd<0 || fd>=PROXY_PRIVATE_FD_FLOOR)return fd;
 long flags=RAW(PROXY_SC_fcntl,fd,F_GETFD,0,0,0,0);
 long moved=RAW(PROXY_SC_fcntl,fd,(flags>0 && (flags&FD_CLOEXEC))?F_DUPFD_CLOEXEC:F_DUPFD,PROXY_PRIVATE_FD_FLOOR,0,0,0);
 RAW(PROXY_SC_close,fd,0,0,0,0,0);
 return moved;
}
int state_private_fd(int fd){return private_fd>=0 && fd==private_fd;}
int state_live(int fd){long f=RAW(PROXY_SC_fcntl,fd,F_GETFD,0,0,0,0);return f>=0 && !(f&FD_CLOEXEC);}
void state_bytes(struct state *s,void *p,size_t n){
 if(s->error)return;
 if(s->pos>UINT64_MAX-n || (s->reading && (s->pos>s->size || n>s->size-s->pos))){s->error=-EINVAL;return;}
 if(s->reading)p_copy(p,s->data+s->pos,n);
 else if(RAW(PROXY_SC_pwrite64,s->fd,p,n,s->pos,0,0)!=(long)n){s->error=-EIO;return;}
 s->pos+=n;
}
uint64_t state_word(struct state *s,uint64_t w){state_bytes(s,&w,8);return w;}
uint64_t state_count(struct state *s,uint64_t count){
 count=state_word(s,count);
 if(!s->error && s->reading && count>(s->size-s->pos)/8)s->error=-EINVAL;
 return count;
}
void state_string(struct state *s,char **p){
 size_t length=*p?p_len(*p):0;
 if(!s->reading && *p && length==SIZE_MAX){s->error=-EOVERFLOW;return;}
 uint64_t n=state_word(s,s->reading?0:*p?(uint64_t)length+1:0);
 if(s->error)return;
 if(s->reading && (s->pos>s->size || n>s->size-s->pos || n>SIZE_MAX)){s->error=-EINVAL;return;}
 if(s->reading){*p=NULL;if(n){*p=vfs_palloc(n);if(!*p){s->error=-ENOMEM;return;}}}
 if(n){state_bytes(s,*p,n);if(!s->error && ((*p)[n-1] || p_len(*p)!=n-1))s->error=-EINVAL;}
}
static void transfer(struct state *s){
 state_android=state_word(s,state_android);state_gpu_bridge=state_word(s,state_gpu_bridge);vfs_state(s);ipc_state(s);net_state(s);proxy_state(s);
}
static unsigned decimal(char *p,uint64_t value){char r[24];unsigned n=0,k=0;do{r[n++]=value%10+'0';value/=10;}while(value);while(n)p[k++]=r[--n];p[k]=0;return k;}
static int parse(const char *p,uint64_t *v){*v=0;if(!*p)return -EINVAL;for(;*p;p++){if(*p<'0'||*p>'9'||*v>(UINT64_MAX-9)/10)return -EINVAL;*v=*v*10+*p-'0';}return 0;}
/* A sealed anonymous FD and a random bearer nonce bind the stream to this
 * handoff. The FD is private while payload code runs; no filesystem state
 * filename or environment-selected executable is accepted. */
int state_import(const char *number,const char *token){
 uint64_t fd,nonce;if(parse(number,&fd)||fd>INT32_MAX||parse(token,&nonce))return -EINVAL;
 private_fd=fd;
 long seals=RAW(PROXY_SC_fcntl,fd,F_GET_SEALS,0,0,0,0);
 if(seals!=(F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE))return -EPERM;
 struct proxy_stat st;if(arch_stat_fd(fd,&st)||st.st_size<32||(uint64_t)st.st_size>SIZE_MAX)return -EINVAL;
 long data=RAW(PROXY_SC_mmap,0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
 if((unsigned long)data>=(unsigned long)-4095)return data;
 struct state s={.fd=fd,.reading=1,.size=st.st_size,.data=(void *)data};
 uint64_t magic=state_word(&s,0),version=state_word(&s,0),pid=state_word(&s,0),key=state_word(&s,0);
 if(magic!=UINT64_C(0x335854454e4f544d)||version!=4||pid!=(uint64_t)RAW(PROXY_SC_getpid,0,0,0,0,0,0)||key!=nonce)s.error=-EPERM;
 if(!s.error)transfer(&s);
 if(s.pos!=s.size)s.error=-EINVAL;
 RAW(PROXY_SC_munmap,data,st.st_size,0,0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);private_fd=-1;
 return s.error;
}
/* Return zero for this loader's class; otherwise kernel-exec only the verified
 * sibling. Open it once and use execveat(AT_EMPTY_PATH), closing rename races. */
int state_exec(const char *path,char *const *args,char *const *env){
 long error=0;char *resolved=vfs_resolve(AT_FDCWD,path,1,&error);if(!resolved)return error;
 long fd=pack_open(resolved,O_RDONLY|O_CLOEXEC);if(fd==-ENOSYS)fd=RAW(PROXY_SC_openat,AT_FDCWD,resolved,O_RDONLY|O_CLOEXEC,0,0,0);vfs_free(resolved);if(fd<0)return fd;
 unsigned char elf[20];long got=pack_pread(fd,elf,sizeof(elf),0);pack_close(fd);RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(got!=sizeof(elf)||elf[0]!=127||elf[1]!='E'||elf[2]!='L'||elf[3]!='F')return 0;
 int target=arch_handoff_class(elf);if(!target)return 0;
 if(target<0)return -ENOEXEC;
 char self[4096];long n=RAW(PROXY_SC_readlinkat,AT_FDCWD,"/proc/self/exe",self,sizeof(self)-1,0,0);if(n<=0)return -ENOEXEC;self[n]=0;
 while(n && self[n-1]!='/')n--;
 if(!n)return -ENOEXEC;
 char directory[4096];p_copy(directory,self,n);directory[n]=0;
 long parent=RAW(PROXY_SC_openat,AT_FDCWD,directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);
 struct proxy_stat dir;
 if(parent<0)return parent;
 int unsafe=arch_stat_fd(parent,&dir)||(dir.st_mode&0022);
 RAW(PROXY_SC_close,parent,0,0,0,0,0);if(unsafe)return -EPERM;
 const char *peer=arch_peer_loader(self+n);
 if((size_t)n+p_len(peer)>=sizeof(self))return -ENAMETOOLONG;
 p_copy(self+n,peer,p_len(peer)+1);
 fd=RAW(PROXY_SC_openat,AT_FDCWD,self,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 struct proxy_stat running,chosen;
 if(arch_stat_at(AT_FDCWD,"/proc/self/exe",&running,0)||arch_stat_fd(fd,&chosen)||!S_ISREG(chosen.st_mode)||chosen.st_uid!=running.st_uid||(chosen.st_mode&0022)||!(chosen.st_mode&0111))error=-EPERM;
 unsigned char header[20];if(!error && (RAW(PROXY_SC_pread64,fd,header,20,0,0,0)!=20||arch_handoff_class(header)!=1))error=-ENOEXEC;
 if(error){RAW(PROXY_SC_close,fd,0,0,0,0,0);return error;}
 private_fd=proxy_private_fd(RAW(PROXY_SC_memfd_create,"matonos-v3-exec",MFD_ALLOW_SEALING,0,0,0,0));
 if(private_fd<0){error=private_fd;private_fd=-1;RAW(PROXY_SC_close,fd,0,0,0,0,0);return error;}
 uint64_t nonce=0;if(RAW(PROXY_SC_getrandom,&nonce,8,0,0,0,0)!=8)error=-EIO;
 struct state s={.fd=private_fd,.error=error};
 state_word(&s,UINT64_C(0x335854454e4f544d));state_word(&s,4);state_word(&s,RAW(PROXY_SC_getpid,0,0,0,0,0,0));state_word(&s,nonce);
 transfer(&s);error=s.error;
 if(!error)error=RAW(PROXY_SC_fcntl,private_fd,F_ADD_SEALS,F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE,0,0,0);
 char num[24],key[24];decimal(num,private_fd);decimal(key,nonce);
 size_t count=0;while(args&&args[count]){if(count==SIZE_MAX){error=-EOVERFLOW;break;}count++;}
 if(count>SIZE_MAX/sizeof(char *)-7)error=-EOVERFLOW;
 char **av=error?NULL:vfs_alloc((count+7)*sizeof(*av));if(!av&&!error)error=-ENOMEM;
 if(!error){av[0]=self;av[1]="--resident-state";av[2]=num;av[3]=key;av[4]=(char *)path;
 for(size_t i=0;i<count;i++)av[i+5]=args[i];
 av[count+5]=NULL;
 /* Payload F_SETFD is native now. This sealed handoff FD must cross the
  * kernel exec so the new resident can import it, regardless of that flag. */
 long handoff_flags=RAW(PROXY_SC_fcntl,private_fd,F_GETFD,0,0,0,0);
 if(handoff_flags<0)error=handoff_flags;
 else if((handoff_flags&FD_CLOEXEC) && RAW(PROXY_SC_fcntl,private_fd,F_SETFD,handoff_flags&~FD_CLOEXEC,0,0,0)<0)error=-EIO;
 else {error=RAW(PROXY_SC_execveat,fd,"",av,env,0x1000,0);
  /* A failed exec leaves the caller alive; restore its private descriptor's
   * original kernel flag along with the rest of its state. */
  if(error<0 && (handoff_flags&FD_CLOEXEC))RAW(PROXY_SC_fcntl,private_fd,F_SETFD,handoff_flags,0,0,0);}}
 vfs_free(av);RAW(PROXY_SC_close,private_fd,0,0,0,0,0);private_fd=-1;RAW(PROXY_SC_close,fd,0,0,0,0,0);return error;
}
