#define _GNU_SOURCE
/* SPDX-License-Identifier: Apache-2.0 */
#include "proxy.h"
#include "vfs.h"
#include "state.h"
#include <fcntl.h>
#include <limits.h>
#include <sys/resource.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <elf.h>
#include <linux/memfd.h>
int state_android, state_gpu_bridge;
char **state_environment;
static int private_fd=-1;
static int ready_fd=-1;
void state_notify_ready(int error){
 if(ready_fd<0)return;
 if(error)proxy_internal_write(ready_fd,&error,sizeof(error));
 RAW(PROXY_SC_close,ready_fd,0,0,0,0,0);ready_fd=-1;
}
enum { PROXY_PRIVATE_FD_COUNT=18 };
long proxy_fd_reserve=LONG_MAX,proxy_fd_reserve_end=LONG_MAX;
int proxy_fd_reserve_ready;
static int private_slots[PROXY_PRIVATE_FD_COUNT];
static unsigned char private_slot_used[PROXY_PRIVATE_FD_COUNT];
static int *private_moved;
static unsigned private_moved_capacity,private_moved_count;
static int private_capacity(unsigned count){
 if(count<=private_moved_capacity)return 0;
 size_t capacity=count>INT_MAX/2?(size_t)count:(size_t)count*2;
 if(capacity>SIZE_MAX/sizeof(int))return -ENOMEM;
 int *next=vfs_palloc(capacity*sizeof(int));if(!next)return -ENOMEM;
 if(private_moved_count)p_copy(next,private_moved,private_moved_count*sizeof(int));
 vfs_free(private_moved);private_moved=next;private_moved_capacity=(unsigned)capacity;return 0;
}

/* Eighteen descriptors occupy the top of the actual soft limit. The first is
 * kept as a permanent /dev/null placeholder; the other slots can hold private
 * state fds as they are created. */
void proxy_fd_reserve_init(void){
 if(proxy_fd_reserve_ready)return;
 struct {uint64_t cur,max;} real={0,0};
 if(RAW(PROXY_SC_prlimit64,0,RLIMIT_NOFILE,0,&real,0,0))return;
 uint64_t soft=real.cur;
 if(soft<PROXY_PRIVATE_FD_COUNT+8 || soft>INT_MAX)return;
 long base=(long)(soft-PROXY_PRIVATE_FD_COUNT);
 long master=RAW(PROXY_SC_openat,AT_FDCWD,"/dev/null",O_RDONLY,0,0,0);
 if(master<0)return;
 for(unsigned i=0;i<PROXY_PRIVATE_FD_COUNT;i++){
  long target=base+i;
  long r=RAW(PROXY_SC_dup3,master,target,0,0,0,0);
  if(r<0){RAW(PROXY_SC_close,master,0,0,0,0,0);return;}
  private_slots[i]=(int)target;
 }
 RAW(PROXY_SC_close,master,0,0,0,0,0);
 proxy_fd_reserve=base;proxy_fd_reserve_end=(long)soft;proxy_fd_reserve_ready=1;
}
int proxy_fd_is_private(long fd){
 if(!proxy_fd_reserve_ready)return 0;
 for(unsigned i=0;i<PROXY_PRIVATE_FD_COUNT;i++)if(private_slots[i]==fd)return 1;
 for(unsigned i=0;i<private_moved_count;i++)if(private_moved[i]==fd)return 1;
 return 0;
}
/* Move a fresh private descriptor into one of the fixed, occupied slots. */
long proxy_private_fd(long fd){
 proxy_fd_reserve_init();
 if(fd<0 || !proxy_fd_reserve_ready || proxy_fd_is_private(fd))return fd;
 unsigned slot=PROXY_PRIVATE_FD_COUNT;
 for(unsigned i=1;i<PROXY_PRIVATE_FD_COUNT;i++)if(!private_slot_used[i]){slot=i;break;}
 if(slot==PROXY_PRIVATE_FD_COUNT){
  /* Nested bwrap tmpfs ownership locks grow with the mount tree. The reserve
   * is an optimization, not an artificial descriptor limit. */
  int e=private_capacity(private_moved_count+1);
  if(e){RAW(PROXY_SC_close,fd,0,0,0,0,0);return e;}
  private_moved[private_moved_count++]=(int)fd;return fd;
 }
 long flags=RAW(PROXY_SC_fcntl,fd,F_GETFD,0,0,0,0);
 long target=private_slots[slot];
 long moved=RAW(PROXY_SC_dup3,fd,target,(flags>=0 && (flags&FD_CLOEXEC))?O_CLOEXEC:0,0,0,0);
 RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(moved>=0)private_slot_used[slot]=1;
 return moved;
}
void proxy_private_fd_release(int fd){
 for(unsigned i=1;i<PROXY_PRIVATE_FD_COUNT;i++)if(private_slots[i]==fd && private_slot_used[i]){
  long spare=RAW(PROXY_SC_openat,AT_FDCWD,"/dev/null",O_RDONLY,0,0,0);
  if(spare>=0){RAW(PROXY_SC_dup3,spare,fd,0,0,0,0);RAW(PROXY_SC_close,spare,0,0,0,0,0);private_slot_used[i]=0;}
  return;
 }
 for(unsigned i=0;i<private_moved_count;i++)if(private_moved[i]==fd){private_moved[i]=private_moved[--private_moved_count];return;}
}
void state_private_fd_relocated(int oldfd,int newfd){if(private_fd==oldfd)private_fd=newfd;}
long proxy_private_fd_move(long fd){
 if(!proxy_fd_is_private(fd))return 0;
 unsigned source_slot=PROXY_PRIVATE_FD_COUNT;
 for(unsigned i=1;i<PROXY_PRIVATE_FD_COUNT;i++)if(private_slots[i]==fd && !private_slot_used[i]){
  private_slots[i]=-1;private_slot_used[i]=2;RAW(PROXY_SC_close,fd,0,0,0,0,0);return fd;
 }
 for(unsigned i=0;i<PROXY_PRIVATE_FD_COUNT;i++)if(private_slots[i]==fd){source_slot=i;break;}
 unsigned target_slot=PROXY_PRIVATE_FD_COUNT;
 for(unsigned i=0;i<PROXY_PRIVATE_FD_COUNT;i++)
  if(i!=source_slot && private_slots[i]>=0 && !private_slot_used[i]){target_slot=i;break;}
 if(target_slot==PROXY_PRIVATE_FD_COUNT){int e=private_capacity(private_moved_count+1);if(e)return e;}
 long flags=RAW(PROXY_SC_fcntl,fd,F_GETFD,0,0,0,0);if(flags<0)return flags;
 long moved;
 if(target_slot<PROXY_PRIVATE_FD_COUNT)
  moved=RAW(PROXY_SC_dup3,fd,private_slots[target_slot],(flags&FD_CLOEXEC)?O_CLOEXEC:0,0,0,0);
 else {
  /* An app may request any descriptor below RLIMIT_NOFILE, including every
   * slot in our reserve. If the reserve is full, spill one private fd into
   * the first ordinary hole instead of making the app's dup fail. */
  moved=RAW(PROXY_SC_fcntl,fd,(flags&FD_CLOEXEC)?F_DUPFD_CLOEXEC:F_DUPFD,3,0,0,0);
 }
 if(moved<0)return moved;
 if(target_slot<PROXY_PRIVATE_FD_COUNT)private_slot_used[target_slot]=1;
 if(source_slot<PROXY_PRIVATE_FD_COUNT){private_slots[source_slot]=-1;private_slot_used[source_slot]=2;}
 else for(unsigned i=0;i<private_moved_count;i++)if(private_moved[i]==fd){private_moved[i]=private_moved[--private_moved_count];break;}
 if(target_slot==PROXY_PRIVATE_FD_COUNT)private_moved[private_moved_count++]=(int)moved;
 RAW(PROXY_SC_close,fd,0,0,0,0,0);return moved;
}
long proxy_private_fd_limit(uint64_t soft){
 for(unsigned i=0;i<PROXY_PRIVATE_FD_COUNT;i++)if(private_slots[i]>=0&&(uint64_t)private_slots[i]>=soft){
  int old=private_slots[i];long flags=RAW(PROXY_SC_fcntl,old,F_GETFD,0,0,0,0);if(flags<0)return flags;
  long moved=RAW(PROXY_SC_fcntl,old,(flags&FD_CLOEXEC)?F_DUPFD_CLOEXEC:F_DUPFD,3,0,0,0);
  if(moved<0)return moved;
  if((uint64_t)moved>=soft){RAW(PROXY_SC_close,moved,0,0,0,0,0);return -EMFILE;}
  private_slots[i]=(int)moved;RAW(PROXY_SC_close,old,0,0,0,0,0);proxy_fd_number_moved(old,(int)moved);
 }
 for(unsigned i=0;i<private_moved_count;i++)if((uint64_t)private_moved[i]>=soft){
  int old=private_moved[i];long flags=RAW(PROXY_SC_fcntl,old,F_GETFD,0,0,0,0);if(flags<0)return flags;
  long moved=RAW(PROXY_SC_fcntl,old,(flags&FD_CLOEXEC)?F_DUPFD_CLOEXEC:F_DUPFD,3,0,0,0);
  if(moved<0)return moved;
  if((uint64_t)moved>=soft){RAW(PROXY_SC_close,moved,0,0,0,0,0);return -EMFILE;}
  private_moved[i]=(int)moved;RAW(PROXY_SC_close,old,0,0,0,0,0);proxy_fd_number_moved(old,(int)moved);
 }
 return 0;
}
int state_private_fd(int fd){return private_fd>=0 && fd==private_fd;}
void proxy_fd_state(struct state *s){
 if(!s->reading){state_word(s,proxy_fd_reserve);state_word(s,proxy_fd_reserve_end);state_word(s,proxy_fd_reserve_ready);for(unsigned i=0;i<PROXY_PRIVATE_FD_COUNT;i++){state_word(s,private_slots[i]);state_word(s,private_slot_used[i]);}state_word(s,private_moved_count);for(unsigned i=0;i<private_moved_count;i++)state_word(s,private_moved[i]);return;}
 proxy_fd_reserve=state_word(s,0);proxy_fd_reserve_end=state_word(s,0);proxy_fd_reserve_ready=state_word(s,0);
 for(unsigned i=0;i<PROXY_PRIVATE_FD_COUNT;i++){private_slots[i]=(int)state_word(s,0);private_slot_used[i]=(unsigned char)state_word(s,0);if(private_slots[i]>=0&&RAW(PROXY_SC_fcntl,private_slots[i],F_GETFD,0,0,0,0)<0)s->error=-EBADF;}
 uint64_t count=state_word(s,0);if(count>INT_MAX){s->error=-EINVAL;return;}if(private_capacity((unsigned)count)){s->error=-ENOMEM;return;}private_moved_count=(unsigned)count;
 for(unsigned i=0;i<private_moved_count;i++){private_moved[i]=(int)state_word(s,0);if(RAW(PROXY_SC_fcntl,private_moved[i],F_GETFD,0,0,0,0)<0)s->error=-EBADF;}
}
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
 state_android=state_word(s,state_android);state_gpu_bridge=state_word(s,state_gpu_bridge);
 vfs_state(s);if(s->error){if(s->reading && proxy_trace)proxy_log("exec-state-vfs-error",s->pos,s->error);return;}
 ipc_state(s);if(s->error){if(s->reading && proxy_trace)proxy_log("exec-state-ipc-error",s->pos,s->error);return;}
 net_state(s);if(s->error){if(s->reading && proxy_trace)proxy_log("exec-state-net-error",s->pos,s->error);return;}
 proxy_state(s);
}
static unsigned decimal(char *p,uint64_t value){char r[24];unsigned n=0,k=0;do{r[n++]=value%10+'0';value/=10;}while(value);while(n)p[k++]=r[--n];p[k]=0;return k;}
static int parse(const char *p,uint64_t *v){*v=0;if(!*p)return -EINVAL;for(;*p;p++){if(*p<'0'||*p>'9'||*v>(UINT64_MAX-9)/10)return -EINVAL;*v=*v*10+*p-'0';}return 0;}
/* A rejected handoff used to leave status 2 behind with no trace at all: the
 * fresh resident still has its zero-initialised trace flag and the payload's
 * sink has not been imported yet. Every check that runs before the sink can be
 * restored therefore reports on the descriptor the kernel cleared for exec,
 * fd 2, which the app-side launcher relays for the whole process lifetime. */
static void import_reject(const char *stage,long first,long second){
 proxy_log(stage,first,second);
}
/* A sealed anonymous FD and a random bearer nonce bind the stream to this
 * handoff. The FD is private while payload code runs; no filesystem state
 * filename or environment-selected executable is accepted. */
int state_import(const char *number,const char *token){
 uint64_t fd,nonce;
 if(parse(number,&fd)||fd>INT32_MAX){import_reject("resident-import-fd",(long)fd,0);return -EINVAL;}
 /* The nonce is the bearer credential; never write its value to a log. */
 if(parse(token,&nonce)){import_reject("resident-import-token",0,0);return -EINVAL;}
 private_fd=fd;
 long seals=RAW(PROXY_SC_fcntl,fd,F_GET_SEALS,0,0,0,0);
 const long wanted=F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE;
 if(seals!=wanted){import_reject("resident-import-seals",seals,wanted);return -EPERM;}
 struct proxy_stat st;long stat_error=arch_stat_fd(fd,&st);
 if(stat_error||st.st_size<32||(uint64_t)st.st_size>SIZE_MAX){
  import_reject("resident-import-size",stat_error,stat_error?0:(long)st.st_size);return -EINVAL;}
 long data=RAW(PROXY_SC_mmap,0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
 if((unsigned long)data>=(unsigned long)-4095){import_reject("resident-import-mmap",data,st.st_size);return data;}
 struct state s={.fd=fd,.reading=1,.size=st.st_size,.data=(void *)data};
 uint64_t magic=state_word(&s,0),version=state_word(&s,0),pid=state_word(&s,0),key=state_word(&s,0);
 uint64_t self=RAW(PROXY_SC_getpid,0,0,0,0,0,0);
 if(magic!=UINT64_C(0x335854454e4f544d)){import_reject("resident-import-magic",(long)magic,(long)version);s.error=-EPERM;}
 else if(version!=18){import_reject("resident-import-version",(long)version,18);s.error=-EPERM;}
 else if(pid!=self){import_reject("resident-import-pid",(long)pid,(long)self);s.error=-EPERM;}
 else if(key!=nonce){import_reject("resident-import-nonce",0,0);s.error=-EPERM;}
 /* Restore the diagnostic sink before importing VFS/fd state. Payload stderr
  * may be redirected (Steam uses steamui_html.txt), and import can fail before
  * proxy_state reaches the saved sink. Keep that failure observable. */
 if(!s.error){
  int trace=(int)state_word(&s,0),diagnostic=(int)state_word(&s,0);
  if(!s.error){proxy_trace=trace;proxy_diagnostic_fd=diagnostic;
   ready_fd=(int)state_word(&s,0);
   if(proxy_trace)proxy_log("exec-state-enter",ARCH_AUDIT,s.size);
   uint64_t count=state_count(&s,0);
   if(!s.error && count>SIZE_MAX/sizeof(char *)-1)s.error=-E2BIG;
   if(!s.error){state_environment=vfs_palloc(((size_t)count+1)*sizeof(char *));if(!state_environment)s.error=-ENOMEM;}
   for(uint64_t i=0;i<count && !s.error;i++){state_string(&s,&state_environment[i]);if(!state_environment[i])s.error=-EINVAL;}
   if(!s.error){state_environment[count]=NULL;transfer(&s);}}
 }
 if(!s.error && s.pos!=s.size)s.error=-EINVAL;
 /* A failed import must be visible even when the payload asked for no tracing:
  * the new resident is the only observer of its own rejected handoff. */
 if(proxy_trace || s.error)proxy_log("exec-state-import",ARCH_AUDIT,s.error);
 RAW(PROXY_SC_munmap,data,st.st_size,0,0,0,0);proxy_private_fd_release((int)fd);RAW(PROXY_SC_close,fd,0,0,0,0,0);private_fd=-1;
 return s.error;
}
/* Return zero for this loader's class; otherwise kernel-exec only the verified
 * sibling. Open it once and use execveat(AT_EMPTY_PATH), closing rename races. */
static int exec_resident(const char *path,char *const *args,char *const *env,int reset){
 long error=0;char *resolved=vfs_resolve(AT_FDCWD,path,1,&error);if(!resolved)return error;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,resolved,O_RDONLY|O_CLOEXEC,0,0,0);vfs_free(resolved);if(fd<0)return fd;
 unsigned char elf[20];long got=proxy_pread_fd(fd,elf,sizeof(elf),0);RAW(PROXY_SC_close,fd,0,0,0,0,0);
 int is_elf=got==sizeof(elf)&&elf[0]==127&&elf[1]=='E'&&elf[2]=='L'&&elf[3]=='F';
 if(!is_elf && !reset)return 0;
 int target=is_elf?arch_handoff_class(elf):0;if(!target && !reset)return 0;
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
 const char *peer=target?arch_peer_loader(self+n):arch_reset_loader(self+n);
 if((size_t)n+p_len(peer)>=sizeof(self))return -ENAMETOOLONG;
 p_copy(self+n,peer,p_len(peer)+1);
 fd=RAW(PROXY_SC_openat,AT_FDCWD,self,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 struct proxy_stat running,chosen;
 if(arch_stat_at(AT_FDCWD,"/proc/self/exe",&running,0)||arch_stat_fd(fd,&chosen)||!S_ISREG(chosen.st_mode)||chosen.st_uid!=running.st_uid||(chosen.st_mode&0022)||!(chosen.st_mode&0111))error=-EPERM;
 unsigned char header[20];if(!error && (RAW(PROXY_SC_pread64,fd,header,20,0,0,0)!=20||arch_handoff_class(header)!=(target?1:0)))error=-ENOEXEC;
 if(error){RAW(PROXY_SC_close,fd,0,0,0,0,0);return error;}
 private_fd=proxy_private_fd(RAW(PROXY_SC_memfd_create,"matonos-v3-exec",MFD_ALLOW_SEALING,0,0,0,0));
 if(private_fd<0){error=private_fd;private_fd=-1;RAW(PROXY_SC_close,fd,0,0,0,0,0);return error;}
 uint64_t nonce=0;if(RAW(PROXY_SC_getrandom,&nonce,8,0,0,0,0)!=8)error=-EIO;
 struct state s={.fd=private_fd,.error=error};
 state_word(&s,UINT64_C(0x335854454e4f544d));state_word(&s,18);state_word(&s,RAW(PROXY_SC_getpid,0,0,0,0,0,0));state_word(&s,nonce);
 state_word(&s,proxy_trace);state_word(&s,proxy_diagnostic_fd);state_word(&s,(uint64_t)(int64_t)ready_fd);
 /* The resident is an Android executable, not the Linux payload. Its linker
  * must not consume payload LD_* settings (paths are still virtual here).
  * Carry the exact environment, including order/duplicates, in the stream. */
 extern uint32_t proxy_bionic32_base;
 extern uint64_t proxy_bionic64_base;
 size_t ec=0;while(env && env[ec])ec++;
 state_word(&s,ec);
 for(size_t i=0;i<ec;i++){char *value=env[i];state_string(&s,&value);}
 transfer(&s);error=s.error;
 char native_base[64],native64_base[64];
 const char *key64="MATON_V3_BIONIC64_BASE=";
 p_copy(native64_base,key64,p_len(key64));decimal(native64_base+p_len(key64),proxy_bionic64_base);
 const char *base_key="MATON_V3_BIONIC32_BASE=";
 p_copy(native_base,base_key,p_len(base_key));decimal(native_base+p_len(base_key),proxy_bionic32_base);
 char **native_env=NULL;
 if(!error && ec>SIZE_MAX/sizeof(char *)-3)error=-E2BIG;
 if(!error){native_env=vfs_alloc((ec+3)*sizeof(char *));if(!native_env)error=-ENOMEM;}
 if(!error){size_t n=0;for(size_t i=0;i<ec;i++){
  const char *value=env[i];
  if(value[0]=='L' && value[1]=='D' && value[2]=='_')continue;
  size_t k=0;while(base_key[k] && base_key[k]==value[k])k++;if(!base_key[k])continue;
  k=0;while(key64[k] && key64[k]==value[k])k++;if(!key64[k])continue;
  native_env[n++]=env[i];
 }if(proxy_bionic32_base)native_env[n++]=native_base;if(proxy_bionic64_base)native_env[n++]=native64_base;native_env[n]=NULL;if(proxy_trace)proxy_log("exec-native-env",ec,ec-n);}
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
 else {if(proxy_trace)proxy_log("exec-peer-enter",ARCH_AUDIT,fd);
  error=RAW(PROXY_SC_execveat,fd,"",av,native_env,0x1000,0);
  if(proxy_trace)proxy_log("exec-peer-return",ARCH_AUDIT,error);
  /* A failed exec leaves the caller alive; restore its private descriptor's
   * original kernel flag along with the rest of its state. */
  if(error<0 && (handoff_flags&FD_CLOEXEC))RAW(PROXY_SC_fcntl,private_fd,F_SETFD,handoff_flags,0,0,0);}}
 vfs_free(native_env);vfs_free(av);proxy_private_fd_release(private_fd);RAW(PROXY_SC_close,private_fd,0,0,0,0,0);private_fd=-1;RAW(PROXY_SC_close,fd,0,0,0,0,0);return error;
}

int state_exec(const char *path,char *const *args,char *const *env){
 int reset=proxy_resident_reset_pending();
 if(reset)proxy_log("resident-reset-exec",ARCH_AUDIT,0);
 return exec_resident(path,args,env,reset);
}
/* An explicit host-command fd mapping may replace a driver's cached number.
 * Real exec discards that driver's objects. Do the same instead of retaining
 * stale resident pointers, while carrying the virtual Linux process state. */
int state_reexec(const char *path,char *const *args,char *const *env,int ready){
 long flags=RAW(PROXY_SC_fcntl,ready,F_GETFD,0,0,0,0);if(flags<0)return flags;
 long r=RAW(PROXY_SC_fcntl,ready,F_SETFD,flags&~FD_CLOEXEC,0,0,0);if(r<0)return r;
 ready_fd=ready;
 r=exec_resident(path,args,env,1);
 ready_fd=-1;
 RAW(PROXY_SC_fcntl,ready,F_SETFD,flags,0,0,0);
 return r;
}
