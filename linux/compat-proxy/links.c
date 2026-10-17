#define _GNU_SOURCE
/* Compatibility only. Every backing operation retains the app's credentials. */
#include "proxy.h"
#include "vfs.h"
#include "links.h"
#include <sys/syscall.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <linux/limits.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <linux/stat.h>
#ifdef COMPAT_LINKS_TEST
#include <signal.h>
#endif
static size_t number(char *out,unsigned long value){char b[24];size_t n=0,k=0;do{b[n++]=(char)('0'+value%10);value/=10;}while(value);while(n)out[k++]=b[--n];return k;}
/* Stores are private, filesystem-local trees. Counts are persistent; only
 * corrupt or missing count metadata (signalled by the dirty marker) requires a full recount. */
static int component(const char *p){
 for(size_t i=0;p[i];i++)if((i==0 || p[i-1]=='/') && p[i]=='.'){
  const char *q=".v3-inodes";size_t j=0;while(q[j] && p[i+j]==q[j])j++;
  if(!q[j] && (!p[i+j] || p[i+j]=='/'))return 1;
 }
 return 0;
}
int links_hidden(const char *p){return component(p);}
static int catpath(char *out,const char *a,const char *b){size_t n=p_len(a),m=p_len(b);if(n+m+2>PATH_MAX)return 0;p_copy(out,a,n);out[n++]='/';p_copy(out+n,b,m+1);return 1;}
static int parent(char *out,const char *path){size_t n=p_len(path);if(n>=PATH_MAX)return 0;p_copy(out,path,n+1);while(n>1 && out[n-1]!='/')n--;if(n>1)n--;out[n]=0;return 1;}
static int private_dir(const char *p,dev_t dev){struct proxy_stat st;return !arch_stat_at(AT_FDCWD,p,&st,AT_SYMLINK_NOFOLLOW) && S_ISDIR(st.st_mode) && st.st_dev==dev && st.st_uid==(uid_t)RAW(PROXY_SC_geteuid,0,0,0,0,0,0) && (st.st_mode&0300)==0300 && !(st.st_mode&0022);}
/* A marker is ordinary visible metadata, but only its owner may establish it
 * outside the payload. Never follow a symlink when recognizing a boundary. */
static int marked(const char *dir){char name[PATH_MAX];struct proxy_stat st;return catpath(name,dir,".v3-store-root") && !arch_stat_at(AT_FDCWD,name,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && st.st_uid==(uid_t)RAW(PROXY_SC_geteuid,0,0,0,0,0,0);}
int links_marker(const char *path){const char *name=path;for(const char *p=path;*p;p++)if(*p=='/')name=p+1;return p_equal(name,".v3-store-root");}
static int tree(char *root,const char *path){
 struct proxy_stat st;char up[PATH_MAX];
 if(!parent(root,path) || arch_stat_at(AT_FDCWD,root,&st,0)<0)return 0;
 for(;;){
  if(marked(root))return private_dir(root,st.st_dev);
  if(!parent(up,root) || p_equal(up,root))return 0;
  p_copy(root,up,p_len(up)+1);
 }
}
int links_target(const char *name,char *out){long n=RAW(PROXY_SC_readlinkat,AT_FDCWD,name,out,PATH_MAX-1,0,0);if(n<=0)return 0;out[n]=0;if(out[0]!='/' || !component(out))return 0;struct proxy_stat st;return !arch_stat_at(AT_FDCWD,out,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode);}
static long store_lock(const char *root,char *store){if(!catpath(store,root,".v3-inodes"))return -ENAMETOOLONG;long r=RAW(PROXY_SC_mkdirat,AT_FDCWD,store,0700,0,0,0);if(r && r!=-EEXIST)return r;struct proxy_stat st;if(arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode) || st.st_uid!=(uid_t)RAW(PROXY_SC_geteuid,0,0,0,0,0,0) || (st.st_mode&0077))return -EPERM;char file[PATH_MAX];if(!catpath(file,store,"lock"))return -ENAMETOOLONG;long fd=RAW(PROXY_SC_openat,AT_FDCWD,file,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(fd<0)return fd;do{r=RAW(PROXY_SC_flock,fd,LOCK_EX,0,0,0,0);}while(r==-EINTR);if(r){RAW(PROXY_SC_close,fd,0,0,0,0,0);return r;}struct proxy_stat own,named;if(arch_stat_fd(fd,&own) || arch_stat_at(AT_FDCWD,file,&named,AT_SYMLINK_NOFOLLOW) || own.st_ino!=named.st_ino || own.st_dev!=named.st_dev){RAW(PROXY_SC_close,fd,0,0,0,0,0);return store_lock(root,store);}return fd;}
static void unlock(long fd){RAW(PROXY_SC_flock,fd,LOCK_UN,0,0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);}
static long count_names(const char *dir,const char *object,dev_t dev,uint64_t *count){
#ifdef COMPAT_LINKS_TEST
 proxy_log("links tree walk",0,0);
#endif
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 char buf[4096];long r=0,n;
 while((n=RAW(PROXY_SC_getdents64,fd,buf,sizeof(buf),0,0,0))>0){for(long off=0;off<n;){unsigned char *d=(void *)(buf+off);unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n){r=-EIO;goto end;}const char *name=(void *)(d+19);off+=len;if(p_equal(name,".") || p_equal(name,"..") || p_equal(name,".v3-inodes"))continue;char path[PATH_MAX],target[PATH_MAX];if(!catpath(path,dir,name)){r=-ENAMETOOLONG;goto end;}struct proxy_stat st;r=arch_stat_at(AT_FDCWD,path,&st,AT_SYMLINK_NOFOLLOW);if(r==-ENOENT){r=0;continue;}if(r)goto end;if(S_ISLNK(st.st_mode)){long k=RAW(PROXY_SC_readlinkat,AT_FDCWD,path,target,sizeof(target)-1,0,0);if(k>0){target[k]=0;if(p_equal(target,object)){if(*count==UINT64_MAX){r=-EOVERFLOW;goto end;}(*count)++;}}}else if(S_ISDIR(st.st_mode) && st.st_dev==dev){r=count_names(path,object,dev,count);if(r)goto end;}}}
 if(n<0)r=n;
end:RAW(PROXY_SC_close,fd,0,0,0,0,0);return r;
}
static long write_count(const char *object,uint64_t n){char meta[PATH_MAX];size_t k=p_len(object);if(k+7>=PATH_MAX)return -ENAMETOOLONG;p_copy(meta,object,k);p_copy(meta+k,".count",7);long fd=RAW(PROXY_SC_openat,AT_FDCWD,meta,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(fd<0)return fd;long r=RAW(PROXY_SC_write,fd,&n,sizeof(n),0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);return r==sizeof(n)?0:r<0?r:-EIO;}
static long checked_count(const char *object,uint64_t *n){
 char meta[PATH_MAX];size_t k=p_len(object);if(k+7>=PATH_MAX)return -ENAMETOOLONG;
 p_copy(meta,object,k);p_copy(meta+k,".count",7);
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,meta,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 struct proxy_stat st;long r=arch_stat_fd(fd,&st);
 if(!r && (!S_ISREG(st.st_mode) || st.st_size!=sizeof(*n)))r=-EIO;
 if(!r){r=RAW(PROXY_SC_read,fd,n,sizeof(*n),0,0,0);r=r==sizeof(*n)?0:r<0?r:-EIO;}
 RAW(PROXY_SC_close,fd,0,0,0,0,0);return r;
}
static uint64_t read_count(const char *object){uint64_t n=0;checked_count(object,&n);return n;}
/* Publish complete intents atomically. A killed preparer leaves only pending,
 * which cannot describe a namespace change and is safe to overwrite. */
static long journal_write(const char *store,const char *name,const void *data,size_t size){
 char pending[PATH_MAX],path[PATH_MAX];if(!catpath(pending,store,"pending") || !catpath(path,store,name))return -ENAMETOOLONG;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,pending,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(fd<0)return fd;
 size_t done=0;long r=0;while(done<size){r=RAW(PROXY_SC_write,fd,(const char *)data+done,size-done,0,0,0);if(r==-EINTR)continue;if(r<=0){if(!r)r=-EIO;break;}done+=r;}
 RAW(PROXY_SC_close,fd,0,0,0,0,0);if(done!=size)return r;
 return RAW(PROXY_SC_renameat2,AT_FDCWD,pending,AT_FDCWD,path,0,0);
}
static long recover(const char *,const char *);
static long ensure_count(const char *root,const char *store,const char *object){
 uint64_t n;long r=checked_count(object,&n);if(!r && n)return 0;
 char marker[PATH_MAX];if(!catpath(marker,store,"dirty"))return -ENAMETOOLONG;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,marker,O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(fd<0)return fd;RAW(PROXY_SC_close,fd,0,0,0,0,0);
 return recover(root,store);
}
/* One intent per store: flock serializes writers. Absolute baseline counts make
 * replay idempotent even when a count write was interrupted. Only the two
 * namespace entries involved in the operation are inspected during replay. */
struct intent { uint64_t magic, count[2], before[2]; char object[2][PATH_MAX], name[2][PATH_MAX]; };
static int terminated(const char *p){for(size_t i=0;i<PATH_MAX;i++)if(!p[i])return 1;return 0;}
static unsigned present(const char *name,const char *object){char target[PATH_MAX];return name[0] && links_target(name,target) && p_equal(target,object);}
static long finish_intent(const char *store){

 char path[PATH_MAX];if(!catpath(path,store,"intent"))return -ENAMETOOLONG;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd==-ENOENT)return 0;if(fd<0)return fd;
 struct intent *j=vfs_alloc(sizeof(*j));if(!j){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -ENOMEM;}
 long r=RAW(PROXY_SC_read,fd,j,sizeof(*j),0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(r!=sizeof(*j) || j->magic!=0x76336c696e6b0001ULL){vfs_free(j);return -EIO;}

#ifdef COMPAT_LINKS_TEST
 char stop[PATH_MAX];struct proxy_stat stopst;
 if(catpath(stop,store,"stop-intent") && !arch_stat_at(AT_FDCWD,stop,&stopst,0))RAW(PROXY_SC_kill,RAW(PROXY_SC_getpid,0,0,0,0,0,0),SIGSTOP,0,0,0,0);
#endif
 r=0;for(int i=0;i<2 && !r;i++){if(!j->object[i][0])continue;
  if(!terminated(j->object[i]) || !terminated(j->name[0]) || !terminated(j->name[1])){r=-EIO;break;}
  size_t prefix=p_len(store),length=p_len(j->object[i]);int valid=length>prefix+3;
  for(size_t k=0;valid && k<prefix;k++)if(j->object[i][k]!=store[k])valid=0;
  if(valid && (j->object[i][prefix]!='/' || j->object[i][prefix+1]!='i' || j->object[i][prefix+2]!='-'))valid=0;
  for(size_t k=prefix+3;valid && k<length;k++)if(j->object[i][k]=='/')valid=0;
  if(!valid || j->before[i]>2){r=-EIO;break;}
  uint64_t now=present(j->name[0],j->object[i])+(!p_equal(j->name[0],j->name[1])?present(j->name[1],j->object[i]):0);
  if(j->before[i]>j->count[i] || j->count[i]-j->before[i]>UINT64_MAX-now){r=-EOVERFLOW;break;}
  uint64_t count=j->count[i]-j->before[i]+now;
  r=write_count(j->object[i],count);
  if(!r && !count){r=RAW(PROXY_SC_unlinkat,AT_FDCWD,j->object[i],0,0,0,0);if(r==-ENOENT)r=0;char meta[PATH_MAX];size_t n=p_len(j->object[i]);if(n+7<PATH_MAX){p_copy(meta,j->object[i],n);p_copy(meta+n,".count",7);RAW(PROXY_SC_unlinkat,AT_FDCWD,meta,0,0,0,0);}}
 }
 vfs_free(j);if(!r)r=RAW(PROXY_SC_unlinkat,AT_FDCWD,path,0,0,0,0);return r;
}
static long begin_intent(const char *root,const char *store,const char *p,const char *q,const char *extra){
 struct intent *j=vfs_alloc(sizeof(*j));if(!j)return -ENOMEM;*j=(struct intent){.magic=0x76336c696e6b0001ULL};
 p_copy(j->name[0],p,p_len(p)+1);if(q)p_copy(j->name[1],q,p_len(q)+1);
 if(!links_target(p,j->object[0]))j->object[0][0]=0;
 if(q && !links_target(q,j->object[1]))j->object[1][0]=0;
 if(extra)p_copy(j->object[0],extra,p_len(extra)+1);
 if(p_equal(j->object[0],j->object[1]))j->object[1][0]=0;
 long r=0;for(int i=0;i<2;i++)if(j->object[i][0]){r=ensure_count(root,store,j->object[i]);if(r)break;j->count[i]=read_count(j->object[i]);j->before[i]=present(p,j->object[i])+(q && !p_equal(p,q)?present(q,j->object[i]):0);if(extra && j->count[i]==UINT64_MAX){r=-EOVERFLOW;break;}}
 if(!r && (j->object[0][0] || j->object[1][0]))r=journal_write(store,"intent",j,sizeof(*j));
 vfs_free(j);return r;
}
struct recount_record {struct recount_record *next;dev_t dev;ino_t ino;};
static struct recount_record *recounted;
static long recover(const char *root,const char *store){
 long moving=finish_intent(store);if(moving && moving!=-EIO)return moving;
 char marker[PATH_MAX];struct proxy_stat markerstat;if(!catpath(marker,store,"dirty"))return -ENAMETOOLONG;int changed=moving==-EIO || !arch_stat_at(AT_FDCWD,marker,&markerstat,AT_SYMLINK_NOFOLLOW);
 /* An interrupted first conversion restores its original name before GC. */
 char journal[PATH_MAX];if(!catpath(journal,store,"conversion"))return -ENAMETOOLONG;
 long j=RAW(PROXY_SC_openat,AT_FDCWD,journal,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);
 if(j>=0){char data[PATH_MAX*2];long n=RAW(PROXY_SC_read,j,data,sizeof(data)-1,0,0,0);RAW(PROXY_SC_close,j,0,0,0,0,0);
  if(n>0){data[n]=0;size_t k=p_len(data);if(k+1<(size_t)n){struct proxy_stat st;
   if(arch_stat_at(AT_FDCWD,data,&st,AT_SYMLINK_NOFOLLOW)==-ENOENT)RAW(PROXY_SC_renameat2,AT_FDCWD,data+k+1,AT_FDCWD,data,RENAME_NOREPLACE,0);
   else if(!arch_stat_at(AT_FDCWD,data+k+1,&st,AT_SYMLINK_NOFOLLOW)){long r=write_count(data+k+1,1);if(r)return r;}
  }}RAW(PROXY_SC_unlinkat,AT_FDCWD,journal,0,0,0,0);
 }

 if(catpath(journal,store,"pending"))RAW(PROXY_SC_unlinkat,AT_FDCWD,journal,0,0,0,0);
 if(!changed)return 0;
 struct proxy_stat identity;long identity_result=arch_stat_at(AT_FDCWD,store,&identity,AT_SYMLINK_NOFOLLOW);if(identity_result)return identity_result;
 for(struct recount_record *p=__atomic_load_n(&recounted,__ATOMIC_ACQUIRE);p;p=p->next)if(p->dev==identity.st_dev && p->ino==identity.st_ino)return -EIO;
 struct recount_record *record=vfs_alloc(sizeof(*record));if(!record)return -ENOMEM;
 record->dev=identity.st_dev;record->ino=identity.st_ino;
 do{record->next=__atomic_load_n(&recounted,__ATOMIC_ACQUIRE);}while(!__atomic_compare_exchange_n(&recounted,&record->next,record,0,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE));
 struct proxy_stat st;long r=arch_stat_at(AT_FDCWD,root,&st,0);if(r)return r;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,store,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return fd;char buf[4096];long n;
 while((n=RAW(PROXY_SC_getdents64,fd,buf,sizeof(buf),0,0,0))>0){for(long off=0;off<n;){unsigned char *d=(void *)(buf+off);unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n){r=-EIO;goto end;}char *name=(void *)(d+19);off+=len;if(name[0]!='i' || name[1]!='-')continue;size_t k=p_len(name);if(k>6 && p_equal(name+k-6,".count")){char meta[PATH_MAX],backing[PATH_MAX];struct proxy_stat bst;if(!catpath(meta,store,name)){r=-ENAMETOOLONG;goto end;}name[k-6]=0;int fits=catpath(backing,store,name);name[k-6]='.';if(!fits){r=-ENAMETOOLONG;goto end;}if(arch_stat_at(AT_FDCWD,backing,&bst,AT_SYMLINK_NOFOLLOW)==-ENOENT)RAW(PROXY_SC_unlinkat,AT_FDCWD,meta,0,0,0,0);continue;}char object[PATH_MAX];if(!catpath(object,store,name)){r=-ENAMETOOLONG;goto end;}char meta[PATH_MAX];size_t olen=p_len(object);if(olen+7>=PATH_MAX){r=-ENAMETOOLONG;goto end;}p_copy(meta,object,olen);p_copy(meta+olen,".count",7);struct proxy_stat mst;if(!changed && !arch_stat_at(AT_FDCWD,meta,&mst,AT_SYMLINK_NOFOLLOW))continue;
 uint64_t count=0;r=count_names(root,object,st.st_dev,&count);if(r)goto end;if(count)r=write_count(object,count);else{r=RAW(PROXY_SC_unlinkat,AT_FDCWD,object,0,0,0,0);if(r==-ENOENT)r=0;char meta[PATH_MAX];if(p_len(object)+7<PATH_MAX){p_copy(meta,object,p_len(object));p_copy(meta+p_len(object),".count",7);RAW(PROXY_SC_unlinkat,AT_FDCWD,meta,0,0,0,0);}}if(r)goto end;}}
 if(n<0)r=n;
end:RAW(PROXY_SC_close,fd,0,0,0,0,0);if(!r){RAW(PROXY_SC_unlinkat,AT_FDCWD,marker,0,0,0,0);if(catpath(journal,store,"intent"))RAW(PROXY_SC_unlinkat,AT_FDCWD,journal,0,0,0,0);}return r;
}
static void remove_empty_store(const char *dir){
 char store[PATH_MAX],file[PATH_MAX];struct proxy_stat st;
 if(!catpath(store,dir,".v3-inodes") || arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW))return;
 long lock=store_lock(dir,store);if(lock<0)return;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,store,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);int empty=fd>=0;char buf[4096];long n;
 if(fd>=0){while((n=RAW(PROXY_SC_getdents64,fd,buf,sizeof(buf),0,0,0))>0){for(long off=0;off<n;){unsigned char *d=(void *)(buf+off);unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n){empty=0;break;}char *name=(void *)(d+19);if(!p_equal(name,".") && !p_equal(name,"..") && !p_equal(name,"lock"))empty=0;off+=len;}}if(n<0)empty=0;RAW(PROXY_SC_close,fd,0,0,0,0,0);}
 if(empty && catpath(file,store,"lock")){RAW(PROXY_SC_unlinkat,AT_FDCWD,file,0,0,0,0);RAW(PROXY_SC_unlinkat,AT_FDCWD,store,AT_REMOVEDIR,0,0,0);}
 unlock(lock);
}
static long store_link(const char *source,const char *target){
 char root[PATH_MAX],store[PATH_MAX],object[PATH_MAX],destparent[PATH_MAX];struct proxy_stat src,dst;
 char sourceparent[PATH_MAX];if(!parent(sourceparent,source) || !parent(destparent,target))return -ENAMETOOLONG;
 long r=arch_stat_at(AT_FDCWD,sourceparent,&src,0);if(r)return r;
 r=arch_stat_at(AT_FDCWD,destparent,&dst,0);if(r)return r;if(src.st_dev!=dst.st_dev)return -EXDEV;
 if(!tree(root,source))return RAW(PROXY_SC_linkat,AT_FDCWD,source,AT_FDCWD,target,0,0);
 char other[PATH_MAX];if(!tree(other,target) || !p_equal(root,other))return -EXDEV;
 long lock=store_lock(root,store);if(lock<0)return lock;r=recover(root,store);if(r)goto end;
 r=arch_stat_at(AT_FDCWD,source,&src,AT_SYMLINK_NOFOLLOW);if(r)goto end;if(S_ISLNK(src.st_mode) && !links_target(source,object)){r=-EPERM;goto end;}
 struct proxy_stat exists;if(!arch_stat_at(AT_FDCWD,target,&exists,AT_SYMLINK_NOFOLLOW)){r=-EEXIST;goto end;}
 if(!links_target(source,object)){
  r=arch_stat_at(AT_FDCWD,source,&src,AT_SYMLINK_NOFOLLOW);if(r)goto end;if(!S_ISREG(src.st_mode) || src.st_uid!=(uid_t)RAW(PROXY_SC_geteuid,0,0,0,0,0,0)){r=-EPERM;goto end;}
  char id[35]="i-";unsigned char nonce[16];long random=RAW(PROXY_SC_getrandom,nonce,sizeof(nonce),0,0,0,0);if(random!=sizeof(nonce)){r=random<0?random:-EIO;goto end;}const char hex[]="0123456789abcdef";for(size_t k=0;k<sizeof(nonce);k++){id[2+2*k]=hex[nonce[k]>>4];id[3+2*k]=hex[nonce[k]&15];}id[34]=0;if(!catpath(object,store,id)){r=-ENAMETOOLONG;goto end;}
  char journal[PATH_MAX];if(!catpath(journal,store,"conversion")){r=-ENAMETOOLONG;goto end;}char data[PATH_MAX*2];size_t sn=p_len(source)+1,on=p_len(object)+1;p_copy(data,source,sn);p_copy(data+sn,object,on);r=journal_write(store,"conversion",data,sn+on);if(r)goto end;
  r=RAW(PROXY_SC_renameat2,AT_FDCWD,source,AT_FDCWD,object,RENAME_NOREPLACE,0);if(r)goto end;
  r=RAW(PROXY_SC_symlinkat,object,AT_FDCWD,source,0,0,0);if(r){RAW(PROXY_SC_renameat2,AT_FDCWD,object,AT_FDCWD,source,RENAME_NOREPLACE,0);goto end;}r=write_count(object,1);if(r)goto end;RAW(PROXY_SC_unlinkat,AT_FDCWD,journal,0,0,0,0);
 }
 r=begin_intent(root,store,source,target,object);if(r)goto end;
 r=RAW(PROXY_SC_symlinkat,object,AT_FDCWD,target,0,0,0);{long clean=finish_intent(store);if(!r)r=clean;}
end:unlock(lock);return r;
}
long links_name_call(long nr,proxy_arg_t *a,const char *p,const char *q){
 char root[PATH_MAX],other[PATH_MAX],store[PATH_MAX];
 if(q && (nr==PROXY_SC_renameat || (nr==PROXY_SC_renameat2 && !(a[4]&RENAME_NOREPLACE)))){char left[PATH_MAX],right[PATH_MAX];if(links_target(p,left) && links_target(q,right) && p_equal(left,right))return 0;}
 if(nr==PROXY_SC_unlinkat && (a[2]&AT_REMOVEDIR))remove_empty_store(p);
 if(!tree(root,p))return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
 if(q && (!tree(other,q) || !p_equal(root,other)))return -EXDEV;
 /* Only existing stores require serialization; ordinary files retain kernel behavior. */
 if(!catpath(store,root,".v3-inodes"))return -ENAMETOOLONG;
 struct proxy_stat st;if(arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW))return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
 long lock=store_lock(root,store);if(lock<0)return lock;long r=recover(root,store);if(!r)r=begin_intent(root,store,p,q,NULL);if(!r){r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);{long clean=finish_intent(store);if(!r)r=clean;}}unlock(lock);return r;
}
long links_path_call(long nr,proxy_arg_t *args,const char *p){
 char root[PATH_MAX],store[PATH_MAX],object[PATH_MAX];struct proxy_stat st;long lock=-1,r;
 if(tree(root,p) && catpath(store,root,".v3-inodes") && !arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW)){
  lock=store_lock(root,store);if(lock<0)return lock;r=recover(root,store);if(r){unlock(lock);return r;}
 }
 proxy_arg_t a[6];p_copy(a,args,sizeof(a));int stored=links_target(p,object);
 if(stored){r=ensure_count(root,store,object);if(r)goto end;if(nr==PROXY_SC_readlinkat){r=-EINVAL;goto end;}
  /* Only the final stored name is substituted. Keep flags intact: the
   * backing inode is already regular, so the kernel enforces NOFOLLOW,
   * O_EXCL, O_DIRECTORY and unknown flag semantics itself. */
  int path0=nr==PROXY_SC_getxattr || nr==PROXY_SC_lgetxattr || nr==PROXY_SC_listxattr || nr==PROXY_SC_llistxattr || nr==PROXY_SC_setxattr || nr==PROXY_SC_lsetxattr || nr==PROXY_SC_removexattr || nr==PROXY_SC_lremovexattr || nr==PROXY_SC_truncate || nr==PROXY_SC_statfs;
  a[path0?0:1]=(long)object;
 }
 r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
 if(!r && stored && nr==PROXY_SC_newfstatat)arch_stat_nlink((void *)(uintptr_t)a[2],read_count(object));
 if(!r && stored && nr==PROXY_SC_statx)arch_statx_nlink((void *)(uintptr_t)a[4],read_count(object));
end:if(lock>=0)unlock(lock);return r;
}
static void refresh(const char *object){char root[PATH_MAX],store[PATH_MAX];size_t i=0;for(;object[i];i++){const char marker[]="/.v3-inodes/";size_t j=0;while(marker[j] && object[i+j]==marker[j])j++;if(!marker[j])break;}if(!object[i] || i>=PATH_MAX)return;p_copy(root,object,i);root[i]=0;long lock=store_lock(root,store);if(lock>=0){if(!recover(root,store)){struct proxy_stat st;if(!arch_stat_at(AT_FDCWD,object,&st,0))ensure_count(root,store,object);}unlock(lock);}}
void links_recover_path(const char *path){char root[PATH_MAX],store[PATH_MAX];struct proxy_stat st;if(!tree(root,path) || !catpath(store,root,".v3-inodes") || arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW))return;long lock=store_lock(root,store);if(lock>=0){recover(root,store);unlock(lock);}}
/* Inspect the kernel fd target, not its virtual display name. */
int links_fd_stored(int fd){char proc[80]="/proc/self/fd/",path[PATH_MAX];size_t k=14;k+=number(proc+k,(unsigned)fd);proc[k]=0;long n=RAW(PROXY_SC_readlinkat,AT_FDCWD,proc,path,sizeof(path)-1,0,0);if(n<=0)return 0;path[n]=0;struct proxy_stat st;return component(path) && !arch_stat_fd(fd,&st) && S_ISREG(st.st_mode);}
void links_fd_stat(int fd,struct proxy_stat *st){char proc[80]="/proc/self/fd/",path[PATH_MAX];size_t k=14;k+=number(proc+k,(unsigned)fd);proc[k]=0;long n=RAW(PROXY_SC_readlinkat,AT_FDCWD,proc,path,sizeof(path)-1,0,0);if(n>0){path[n]=0;links_stat(path,st);}}
void links_stat(const char *object,struct proxy_stat *st){if(component(object) && S_ISREG(st->st_mode)){refresh(object);st->st_nlink=read_count(object);}}
void links_statx(const char *object,struct statx *st){if(component(object) && S_ISREG(arch_statx_mode(st))){refresh(object);arch_statx_nlink(st,read_count(object));}}
static long dents_locked(long fd,void *buf,long size){
 long n;do{n=RAW(PROXY_SC_getdents64,fd,buf,size,0,0,0);if(n<=0)return n;long used=0;for(long off=0;off<n;){unsigned char *d=(unsigned char *)buf+off;unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n)return -EIO;char *name=(void *)(d+19);if(!p_equal(name,".v3-inodes")){char target[PATH_MAX];long k=RAW(PROXY_SC_readlinkat,fd,name,target,sizeof(target)-1,0,0);if(k>0){target[k]=0;if(component(target))d[18]=8;}for(unsigned short i=0;i<len;i++)((unsigned char *)buf)[used+i]=d[i];used+=len;}off+=len;}if(used)return used;}while(n>0);return n;
}
long links_dents(long fd,void *buf,long size){
 char proc[80]="/proc/self/fd/",path[PATH_MAX],root[PATH_MAX],store[PATH_MAX];size_t k=14;k+=number(proc+k,(unsigned)fd);proc[k]=0;long lock=-1,n=RAW(PROXY_SC_readlinkat,AT_FDCWD,proc,path,sizeof(path)-3,0,0);struct proxy_stat st;
 if(n>0){path[n++]='/';path[n++]='.';path[n]=0;if(tree(root,path) && catpath(store,root,".v3-inodes") && !arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW)){lock=store_lock(root,store);if(lock<0)return lock;long r=recover(root,store);if(r){unlock(lock);return r;}}}
 long r=dents_locked(fd,buf,size);if(lock>=0)unlock(lock);return r;
}
void links_payload_fd_stat(int fd,void *wire){struct proxy_stat s;arch_stat_decode(&s,wire);links_fd_stat(fd,&s);arch_stat_nlink(wire,s.st_nlink);}

long links_publish(const char *source,const char *target,long flags){
 if(flags&~AT_SYMLINK_FOLLOW)return -EINVAL;
 char root[PATH_MAX],other[PATH_MAX];
 if(!tree(root,source) || !tree(other,target))return RAW(PROXY_SC_linkat,AT_FDCWD,source,AT_FDCWD,target,flags,0);
 return store_link(source,target);
}
