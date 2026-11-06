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
/* The process credentials do not change in this compatibility layer. Cache
 * the kernel euid; marker ownership and filesystem state are still checked
 * on every path operation. */
static volatile uid_t cached_euid;
static volatile int cached_euid_valid;
static uid_t links_euid(void){
 if(__atomic_load_n(&cached_euid_valid,__ATOMIC_ACQUIRE))return __atomic_load_n(&cached_euid,__ATOMIC_RELAXED);
 uid_t value=(uid_t)RAW(PROXY_SC_geteuid,0,0,0,0,0,0);
 __atomic_store_n(&cached_euid,value,__ATOMIC_RELAXED);__atomic_store_n(&cached_euid_valid,1,__ATOMIC_RELEASE);return value;
}
/* Store selection is string-only; normal names never probe store ancestry. */
int links_app_data_root(char *root,const char *path){
 const char *p=0;
#ifdef COMPAT_LINKS_TEST_APP_ROOT
 const char *app=COMPAT_LINKS_TEST_APP_ROOT;size_t an=p_len(app),ai=0;
 while(ai<an && path[ai]==app[ai])ai++;
 if(ai==an && (!path[ai] || path[ai]=='/')){p_copy(root,app,an+1);return 1;}
#endif
#ifdef COMPAT_LINKS_TEST_ROOT
 const char *test=COMPAT_LINKS_TEST_ROOT;size_t tn=p_len(test);
 size_t ti=0;while(ti<tn && path[ti]==test[ti])ti++;
 if(ti==tn && path[ti]=='/'){p=path+ti+1;while(*p && *p!='/')p++;size_t n=p-path;p_copy(root,path,n);root[n]=0;return 1;}
#endif
 const char *prefix[]={"/data/data/","/data/user/","/data/user_de/"};
 for(unsigned k=0;k<3;k++){
  size_t n=p_len(prefix[k]),i=0;while(i<n && path[i]==prefix[k][i])i++;
  if(i!=n)continue;
  p=path+n;
  if(k){const char *u=p;while(*p>='0' && *p<='9')p++;if(p==u || *p!='/')return 0;p++;}
  const char *package=p;while(*p && *p!='/')p++;
  if(p==package)return 0;
  size_t len=p-path;if(len>=PATH_MAX)return 0;p_copy(root,path,len);root[len]=0;return 1;
 }
 return 0;
}
static int is_placeholder(const struct proxy_stat *st){return S_ISREG(st->st_mode) && (st->st_mode&07777)==S_ISVTX;}
static void refresh(const char *);
/* Immutable identities: ordinary fstat results need no readlink or lock. */
struct known_object {struct known_object *next;uint64_t dev,ino;char path[PATH_MAX];};
static struct known_object *known_objects[1024];
static unsigned object_bucket(uint64_t dev,uint64_t ino){return (unsigned)((ino^(ino>>32)^dev)%1024);}
static void remember_object(const char *path,const struct proxy_stat *st){
 unsigned bucket=object_bucket(st->st_dev,st->st_ino);
 for(struct known_object *k=__atomic_load_n(&known_objects[bucket],__ATOMIC_ACQUIRE);k;k=k->next)if(k->dev==st->st_dev && k->ino==st->st_ino)return;
 struct known_object *k=vfs_palloc(sizeof(*k));if(!k)return;
 k->dev=st->st_dev;k->ino=st->st_ino;p_copy(k->path,path,p_len(path)+1);
 do{k->next=__atomic_load_n(&known_objects[bucket],__ATOMIC_ACQUIRE);}while(!__atomic_compare_exchange_n(&known_objects[bucket],&k->next,k,0,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE));
}
/* One constant-time index entry per physical placeholder inode. Values are
 * relative object IDs, so directory relocation needs no target rewriting. */
static int index_path(char *out,const char *store,const struct proxy_stat *st){
 char key[36]="p-";const char hex[]="0123456789abcdef";
 for(unsigned i=0;i<16;i++){key[2+i]=hex[(st->st_dev>>(60-4*i))&15];key[19+i]=hex[(st->st_ino>>(60-4*i))&15];}
 key[18]='-';key[35]=0;return catpath(out,store,key);
}
/* With the store lock held, the root and its ownership have already been
 * checked. Reuse that store during replay instead of re-walking the marker
 * ancestry for every before/after reference in the same transaction. */
static int target_index(const char *store,const struct proxy_stat *identity,char *out){
 struct proxy_stat st;char id[36],*entry=vfs_alloc(PATH_MAX);if(!entry)return 0;if(!index_path(entry,store,identity)){vfs_free(entry);return 0;}
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,entry,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0){vfs_free(entry);return 0;}
 long n=RAW(PROXY_SC_read,fd,id,sizeof(id),0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);
 int valid=n==35 && id[0]=='i' && id[1]=='-' && !id[34];
 for(unsigned i=2;valid && i<34;i++)if(!((id[i]>='0' && id[i]<='9') || (id[i]>='a' && id[i]<='f')))valid=0;
 int found=valid && catpath(out,store,id) && !arch_stat_at(AT_FDCWD,out,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode);
 if(found)remember_object(out,&st);
 vfs_free(entry);return found;
}
static int target_locked(const char *store,const char *name,char *out){
 struct proxy_stat st;
 return !arch_stat_at(AT_FDCWD,name,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) &&
  is_placeholder(&st) && target_index(store,&st,out);
}
int links_target(const char *name,char *out){
 struct proxy_stat st;char root[PATH_MAX],*store=vfs_alloc(PATH_MAX);if(!store)return 0;
 if(arch_stat_at(AT_FDCWD,name,&st,AT_SYMLINK_NOFOLLOW) || !is_placeholder(&st) || st.st_uid!=links_euid() || !links_app_data_root(root,name) || !catpath(store,root,".v3-inodes")){vfs_free(store);return 0;}
 int found=target_index(store,&st,out);vfs_free(store);return found;
}
static long store_lock(const char *root,char *store){if(!catpath(store,root,".v3-inodes"))return -ENAMETOOLONG;long r=RAW(PROXY_SC_mkdirat,AT_FDCWD,store,0700,0,0,0);if(r && r!=-EEXIST)return r;struct proxy_stat st;if(arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode) || st.st_uid!=links_euid() || (st.st_mode&0077))return -EPERM;char file[PATH_MAX];if(!catpath(file,store,"lock"))return -ENAMETOOLONG;long fd=RAW(PROXY_SC_openat,AT_FDCWD,file,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(fd<0)return fd;do{r=RAW(PROXY_SC_flock,fd,LOCK_EX,0,0,0,0);}while(r==-EINTR);if(r){RAW(PROXY_SC_close,fd,0,0,0,0,0);return r;}struct proxy_stat own,named;if(arch_stat_fd(fd,&own) || arch_stat_at(AT_FDCWD,file,&named,AT_SYMLINK_NOFOLLOW) || own.st_ino!=named.st_ino || own.st_dev!=named.st_dev){RAW(PROXY_SC_close,fd,0,0,0,0,0);return store_lock(root,store);}return fd;}
static void unlock(long fd){RAW(PROXY_SC_flock,fd,LOCK_UN,0,0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);}
struct count_names_scratch { char buf[4096],path[PATH_MAX],target[PATH_MAX]; };
static long count_names(const char *,const char *,dev_t,uint64_t *);
static long count_names_impl(const char *dir,const char *object,dev_t dev,uint64_t *count,struct count_names_scratch *scratch){
#ifdef COMPAT_LINKS_TEST
 proxy_log("links tree walk",0,0);
#endif
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 char *buf=scratch->buf,*path=scratch->path,*target=scratch->target;long r=0,n;
 while((n=RAW(PROXY_SC_getdents64,fd,buf,sizeof(scratch->buf),0,0,0))>0){for(long off=0;off<n;){unsigned char *d=(void *)(buf+off);unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n){r=-EIO;goto end;}const char *name=(void *)(d+19);off+=len;if(p_equal(name,".") || p_equal(name,"..") || p_equal(name,".v3-inodes"))continue;if(!catpath(path,dir,name)){r=-ENAMETOOLONG;goto end;}struct proxy_stat st;r=arch_stat_at(AT_FDCWD,path,&st,AT_SYMLINK_NOFOLLOW);if(r==-ENOENT){r=0;continue;}if(r)goto end;if(is_placeholder(&st) && links_target(path,target) && p_equal(target,object)){if(*count==UINT64_MAX){r=-EOVERFLOW;goto end;}(*count)++;}else if(S_ISDIR(st.st_mode) && st.st_dev==dev){r=count_names(path,object,dev,count);if(r)goto end;}}}
 if(n<0)r=n;
end:RAW(PROXY_SC_close,fd,0,0,0,0,0);return r;
}
static long count_names(const char *dir,const char *object,dev_t dev,uint64_t *count){
 struct count_names_scratch *scratch=vfs_alloc(sizeof(*scratch));if(!scratch)return -ENOMEM;
 long result=count_names_impl(dir,object,dev,count,scratch);vfs_free(scratch);return result;
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
static long journal_write_impl(const char *store,const char *name,const void *data,size_t size,char *scratch){
 char *pending=scratch,*path=scratch+PATH_MAX;if(!catpath(pending,store,"pending") || !catpath(path,store,name))return -ENAMETOOLONG;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,pending,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(fd<0)return fd;
 size_t done=0;long r=0;while(done<size){r=RAW(PROXY_SC_write,fd,(const char *)data+done,size-done,0,0,0);if(r==-EINTR)continue;if(r<=0){if(!r)r=-EIO;break;}done+=r;}
 RAW(PROXY_SC_close,fd,0,0,0,0,0);if(done!=size)return r;
 return RAW(PROXY_SC_renameat2,AT_FDCWD,pending,AT_FDCWD,path,0,0);
}
static long journal_write(const char *store,const char *name,const void *data,size_t size){char *scratch=vfs_alloc(PATH_MAX*2);if(!scratch)return -ENOMEM;long r=journal_write_impl(store,name,data,size,scratch);vfs_free(scratch);return r;}
/* A single staging name is covered by the store lock. The index is complete
 * before the placeholder becomes visible. Recovery discards unpublished work. */
static void discard_new_impl(const char *store,char *scratch){
 char *path=scratch,*entry=scratch+PATH_MAX;struct proxy_stat st;
 if(catpath(path,store,"new") && !arch_stat_at(AT_FDCWD,path,&st,AT_SYMLINK_NOFOLLOW)){
  if(index_path(entry,store,&st))RAW(PROXY_SC_unlinkat,AT_FDCWD,entry,0,0,0,0);
  RAW(PROXY_SC_unlinkat,AT_FDCWD,path,0,0,0,0);
 }
}
static void discard_new(const char *store){char *scratch=vfs_alloc(PATH_MAX*2);if(!scratch)return;discard_new_impl(store,scratch);vfs_free(scratch);}
static long placeholder_impl(const char *store,const char *name,const char *object,char *scratch){
 char *temp=scratch,*entry=scratch+PATH_MAX;if(!catpath(temp,store,"new"))return -ENAMETOOLONG;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,S_ISVTX,0,0);if(fd<0)return fd;
 struct proxy_stat st;long r=arch_stat_fd(fd,&st);RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(!r && !index_path(entry,store,&st))r=-ENAMETOOLONG;
 const char *id=object;for(const char *p=object;*p;p++)if(*p=='/')id=p+1;
 if(!r)r=journal_write(store,entry+p_len(store)+1,id,p_len(id)+1);
 if(!r)r=RAW(PROXY_SC_renameat2,AT_FDCWD,temp,AT_FDCWD,name,RENAME_NOREPLACE,0);
 if(r)discard_new(store);
 return r;
}
static long placeholder(const char *store,const char *name,const char *object){char *scratch=vfs_alloc(PATH_MAX*2);if(!scratch)return -ENOMEM;long r=placeholder_impl(store,name,object,scratch);vfs_free(scratch);return r;}
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
struct intent { uint64_t magic, count[2], before[2]; char object[2][PATH_MAX], name[2][PATH_MAX]; struct proxy_stat placeholder[2]; };
static int terminated(const char *p){for(size_t i=0;i<PATH_MAX;i++)if(!p[i])return 1;return 0;}
static unsigned present(const char *store,const char *name,const char *object){char target[PATH_MAX];return name[0] && target_locked(store,name,target) && p_equal(target,object);}
struct finish_intent_scratch { char path[PATH_MAX],entry[PATH_MAX],meta[PATH_MAX],stop[PATH_MAX]; };
static long finish_intent_impl(const char *store,struct finish_intent_scratch *scratch){

 char *path=scratch->path;if(!catpath(path,store,"intent"))return -ENAMETOOLONG;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd==-ENOENT)return 0;if(fd<0)return fd;
 struct intent *j=vfs_alloc(sizeof(*j));if(!j){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -ENOMEM;}
 long r=RAW(PROXY_SC_read,fd,j,sizeof(*j),0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(r!=sizeof(*j) || j->magic!=0x76336c696e6b0002ULL){vfs_free(j);return -EIO;}

#ifdef COMPAT_LINKS_TEST
 char *stop=scratch->stop;struct proxy_stat stopst;
 if(catpath(stop,store,"stop-intent") && !arch_stat_at(AT_FDCWD,stop,&stopst,0))RAW(PROXY_SC_kill,RAW(PROXY_SC_getpid,0,0,0,0,0,0),SIGSTOP,0,0,0,0);
#endif
 r=0;for(int i=0;i<2 && !r;i++){if(!j->object[i][0])continue;
  if(!terminated(j->object[i]) || !terminated(j->name[0]) || !terminated(j->name[1])){r=-EIO;break;}
  size_t prefix=p_len(store),length=p_len(j->object[i]);int valid=length>prefix+3;
  for(size_t k=0;valid && k<prefix;k++)if(j->object[i][k]!=store[k])valid=0;
  if(valid && (j->object[i][prefix]!='/' || j->object[i][prefix+1]!='i' || j->object[i][prefix+2]!='-'))valid=0;
  for(size_t k=prefix+3;valid && k<length;k++)if(j->object[i][k]=='/')valid=0;
  if(!valid || j->before[i]>2){r=-EIO;break;}
  uint64_t now=present(store,j->name[0],j->object[i])+(!p_equal(j->name[0],j->name[1])?present(store,j->name[1],j->object[i]):0);
  if(j->before[i]>j->count[i] || j->count[i]-j->before[i]>UINT64_MAX-now){r=-EOVERFLOW;break;}
  uint64_t count=j->count[i]-j->before[i]+now;
  r=write_count(j->object[i],count);
  if(!r && !count){r=RAW(PROXY_SC_unlinkat,AT_FDCWD,j->object[i],0,0,0,0);if(r==-ENOENT)r=0;char *meta=scratch->meta;size_t n=p_len(j->object[i]);if(n+7<PATH_MAX){p_copy(meta,j->object[i],n);p_copy(meta+n,".count",7);RAW(PROXY_SC_unlinkat,AT_FDCWD,meta,0,0,0,0);}}
 }
 /* Remove retired placeholder identities, including overwritten destinations.
  * Rename/exchange keep an identity if either affected name still owns it. */
 for(int i=0;i<2 && !r;i++)if(j->placeholder[i].st_ino){
  int live=0;for(int k=0;k<2;k++){struct proxy_stat st;if(j->name[k][0] && !arch_stat_at(AT_FDCWD,j->name[k],&st,AT_SYMLINK_NOFOLLOW) && st.st_dev==j->placeholder[i].st_dev && st.st_ino==j->placeholder[i].st_ino)live=1;}
  char *entry=scratch->entry;if(!live && index_path(entry,store,&j->placeholder[i])){long e=RAW(PROXY_SC_unlinkat,AT_FDCWD,entry,0,0,0,0);if(e && e!=-ENOENT)r=e;}
 }
 vfs_free(j);if(!r)r=RAW(PROXY_SC_unlinkat,AT_FDCWD,path,0,0,0,0);return r;
}
static long finish_intent(const char *store){struct finish_intent_scratch *scratch=vfs_alloc(sizeof(*scratch));if(!scratch)return -ENOMEM;long r=finish_intent_impl(store,scratch);vfs_free(scratch);return r;}
static long begin_intent(const char *root,const char *store,const char *p,const char *q,const char *extra){
 struct intent *j=vfs_alloc(sizeof(*j));if(!j)return -ENOMEM;*j=(struct intent){.magic=0x76336c696e6b0002ULL};
 p_copy(j->name[0],p,p_len(p)+1);if(q)p_copy(j->name[1],q,p_len(q)+1);
 if(!target_locked(store,p,j->object[0]))j->object[0][0]=0;
 if(q && !target_locked(store,q,j->object[1]))j->object[1][0]=0;
 if(j->object[0][0])arch_stat_at(AT_FDCWD,p,&j->placeholder[0],AT_SYMLINK_NOFOLLOW);
 if(j->object[1][0])arch_stat_at(AT_FDCWD,q,&j->placeholder[1],AT_SYMLINK_NOFOLLOW);
 if(extra)p_copy(j->object[0],extra,p_len(extra)+1);
 if(p_equal(j->object[0],j->object[1]))j->object[1][0]=0;
 long r=0;for(int i=0;i<2;i++)if(j->object[i][0]){r=checked_count(j->object[i],&j->count[i]);if(r || !j->count[i]){r=ensure_count(root,store,j->object[i]);if(!r)r=checked_count(j->object[i],&j->count[i]);}if(r)break;j->before[i]=present(store,p,j->object[i])+(q && !p_equal(p,q)?present(store,q,j->object[i]):0);if(extra && j->count[i]==UINT64_MAX){r=-EOVERFLOW;break;}}
 if(!r && (j->object[0][0] || j->object[1][0]))r=journal_write(store,"intent",j,sizeof(*j));
 vfs_free(j);return r;
}
struct recover_scratch { char marker[PATH_MAX], journal[PATH_MAX], data[PATH_MAX*2], buf[4096], meta[PATH_MAX], backing[PATH_MAX], object[PATH_MAX]; };
static long recover_impl(const char *root,const char *store,struct recover_scratch *scratch){
 char *marker=scratch->marker,*journal=scratch->journal,*data=scratch->data,*buf=scratch->buf,*meta=scratch->meta,*backing=scratch->backing,*object=scratch->object;
 discard_new(store);
 long moving=finish_intent(store);if(moving && moving!=-EIO)return moving;
 struct proxy_stat markerstat;if(!catpath(marker,store,"dirty"))return -ENAMETOOLONG;int changed=moving==-EIO || !arch_stat_at(AT_FDCWD,marker,&markerstat,AT_SYMLINK_NOFOLLOW);
 /* Exchange leaves either the original source plus an unpublished placeholder,
  * or the published source plus its data object. Never move the source in replay. */
 if(!catpath(journal,store,"conversion"))return -ENAMETOOLONG;
 long j=RAW(PROXY_SC_openat,AT_FDCWD,journal,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);
 if(j>=0){long n=RAW(PROXY_SC_read,j,data,sizeof(scratch->data)-1,0,0,0);RAW(PROXY_SC_close,j,0,0,0,0,0);
  if(n<=0)return -EIO;
  data[n]=0;size_t k=p_len(data);
  if(k+1>=(size_t)n)return -EIO;
  struct proxy_stat st;
  if(!arch_stat_at(AT_FDCWD,data+k+1,&st,AT_SYMLINK_NOFOLLOW)){
   if(is_placeholder(&st)){char entry[PATH_MAX];if(index_path(entry,store,&st))RAW(PROXY_SC_unlinkat,AT_FDCWD,entry,0,0,0,0);RAW(PROXY_SC_unlinkat,AT_FDCWD,data+k+1,0,0,0,0);}
   else {long r=write_count(data+k+1,1);if(r)return r;}
  }
  RAW(PROXY_SC_unlinkat,AT_FDCWD,journal,0,0,0,0);
 }

 if(catpath(journal,store,"pending"))RAW(PROXY_SC_unlinkat,AT_FDCWD,journal,0,0,0,0);
 if(!changed)return 0;
 struct proxy_stat st;long r=arch_stat_at(AT_FDCWD,root,&st,0);if(r)return r;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,store,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return fd;long n;
 while((n=RAW(PROXY_SC_getdents64,fd,buf,sizeof(scratch->buf),0,0,0))>0){for(long off=0;off<n;){unsigned char *d=(void *)(buf+off);unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n){r=-EIO;goto end;}char *name=(void *)(d+19);off+=len;if(name[0]!='i' || name[1]!='-')continue;size_t k=p_len(name);if(k>6 && p_equal(name+k-6,".count")){struct proxy_stat bst;if(!catpath(meta,store,name)){r=-ENAMETOOLONG;goto end;}name[k-6]=0;int fits=catpath(backing,store,name);name[k-6]='.';if(!fits){r=-ENAMETOOLONG;goto end;}if(arch_stat_at(AT_FDCWD,backing,&bst,AT_SYMLINK_NOFOLLOW)==-ENOENT)RAW(PROXY_SC_unlinkat,AT_FDCWD,meta,0,0,0,0);continue;}if(!catpath(object,store,name)){r=-ENAMETOOLONG;goto end;}size_t olen=p_len(object);if(olen+7>=PATH_MAX){r=-ENAMETOOLONG;goto end;}p_copy(meta,object,olen);p_copy(meta+olen,".count",7);struct proxy_stat mst;if(!changed && !arch_stat_at(AT_FDCWD,meta,&mst,AT_SYMLINK_NOFOLLOW))continue;
 uint64_t count=0;r=count_names(root,object,st.st_dev,&count);if(r)goto end;if(count)r=write_count(object,count);else{r=RAW(PROXY_SC_unlinkat,AT_FDCWD,object,0,0,0,0);if(r==-ENOENT)r=0;if(p_len(object)+7<PATH_MAX){p_copy(meta,object,p_len(object));p_copy(meta+p_len(object),".count",7);RAW(PROXY_SC_unlinkat,AT_FDCWD,meta,0,0,0,0);}}if(r)goto end;}}
 if(n<0)r=n;
end:RAW(PROXY_SC_close,fd,0,0,0,0,0);if(!r){RAW(PROXY_SC_unlinkat,AT_FDCWD,marker,0,0,0,0);if(catpath(journal,store,"intent"))RAW(PROXY_SC_unlinkat,AT_FDCWD,journal,0,0,0,0);}return r;
}
static long recover(const char *root,const char *store){
 struct recover_scratch *scratch=vfs_alloc(sizeof(*scratch));if(!scratch)return -ENOMEM;
 long result=recover_impl(root,store,scratch);vfs_free(scratch);return result;
}

struct remove_store_scratch { char store[PATH_MAX],file[PATH_MAX],buf[4096]; };
static void remove_empty_store_impl(const char *dir,struct remove_store_scratch *scratch){
 char *store=scratch->store,*file=scratch->file;struct proxy_stat st;
 if(!catpath(store,dir,".v3-inodes") || arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW))return;
 long lock=store_lock(dir,store);if(lock<0)return;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,store,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);int empty=fd>=0;char *buf=scratch->buf;long n;
 if(fd>=0){while((n=RAW(PROXY_SC_getdents64,fd,buf,sizeof(scratch->buf),0,0,0))>0){for(long off=0;off<n;){unsigned char *d=(void *)(buf+off);unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n){empty=0;break;}char *name=(void *)(d+19);if(!p_equal(name,".") && !p_equal(name,"..") && !p_equal(name,"lock"))empty=0;off+=len;}}if(n<0)empty=0;RAW(PROXY_SC_close,fd,0,0,0,0,0);}
 if(empty && catpath(file,store,"lock")){RAW(PROXY_SC_unlinkat,AT_FDCWD,file,0,0,0,0);RAW(PROXY_SC_unlinkat,AT_FDCWD,store,AT_REMOVEDIR,0,0,0);}
 unlock(lock);
}
static void remove_empty_store(const char *dir){struct remove_store_scratch *scratch=vfs_alloc(sizeof(*scratch));if(!scratch)return;remove_empty_store_impl(dir,scratch);vfs_free(scratch);}
struct store_link_scratch { char root[PATH_MAX],store[PATH_MAX],object[PATH_MAX],destparent[PATH_MAX],sourceparent[PATH_MAX],other[PATH_MAX],journal[PATH_MAX],data[PATH_MAX*2]; };
static long store_link_impl(const char *source,const char *target,struct store_link_scratch *scratch){
 char *root=scratch->root,*store=scratch->store,*object=scratch->object,*destparent=scratch->destparent;struct proxy_stat src,dst;
 char *sourceparent=scratch->sourceparent;if(!parent(sourceparent,source) || !parent(destparent,target))return -ENAMETOOLONG;
 long r=arch_stat_at(AT_FDCWD,sourceparent,&src,0);if(r)return r;
 r=arch_stat_at(AT_FDCWD,destparent,&dst,0);if(r)return r;if(src.st_dev!=dst.st_dev)return -EXDEV;
 if(!links_app_data_root(root,source))return -EPERM;
 char *other=scratch->other;if(!links_app_data_root(other,target))return -EPERM;if(!p_equal(root,other))return -EXDEV;
 long lock=store_lock(root,store);if(lock<0)return lock;r=recover(root,store);if(r)goto end;
 r=arch_stat_at(AT_FDCWD,source,&src,AT_SYMLINK_NOFOLLOW);if(r)goto end;if(S_ISLNK(src.st_mode)){r=-EPERM;goto end;}
 struct proxy_stat exists;if(!arch_stat_at(AT_FDCWD,target,&exists,AT_SYMLINK_NOFOLLOW)){r=-EEXIST;goto end;}
 if(!target_locked(store,source,object)){
  r=arch_stat_at(AT_FDCWD,source,&src,AT_SYMLINK_NOFOLLOW);if(r)goto end;if(!S_ISREG(src.st_mode) || src.st_uid!=links_euid()){r=-EPERM;goto end;}
  char id[35]="i-";unsigned char nonce[16];long random=RAW(PROXY_SC_getrandom,nonce,sizeof(nonce),0,0,0,0);if(random!=sizeof(nonce)){r=random<0?random:-EIO;goto end;}const char hex[]="0123456789abcdef";for(size_t k=0;k<sizeof(nonce);k++){id[2+2*k]=hex[nonce[k]>>4];id[3+2*k]=hex[nonce[k]&15];}id[34]=0;if(!catpath(object,store,id)){r=-ENAMETOOLONG;goto end;}
  char *journal=scratch->journal;if(!catpath(journal,store,"conversion")){r=-ENAMETOOLONG;goto end;}char *data=scratch->data;size_t sn=p_len(source)+1,on=p_len(object)+1;p_copy(data,source,sn);p_copy(data+sn,object,on);r=journal_write(store,"conversion",data,sn+on);if(r)goto end;
  r=placeholder(store,object,object);if(r)goto end;
  r=RAW(PROXY_SC_renameat2,AT_FDCWD,source,AT_FDCWD,object,RENAME_EXCHANGE,0);
  if(r){long failure=(r==-EINVAL || r==-ENOSYS)?-EPERM:r;recover(root,store);r=failure;goto end;}
  remember_object(object,&src);
  r=write_count(object,1);if(r)goto end;RAW(PROXY_SC_unlinkat,AT_FDCWD,journal,0,0,0,0);
 }
 r=begin_intent(root,store,source,target,object);if(r)goto end;
 r=placeholder(store,target,object);{long clean=finish_intent(store);if(!r)r=clean;}
end:unlock(lock);return r;
}
static long store_link(const char *source,const char *target){
 struct store_link_scratch *scratch=vfs_alloc(sizeof(*scratch));if(!scratch)return -ENOMEM;
 long result=store_link_impl(source,target,scratch);vfs_free(scratch);return result;
}
long links_name_call(long nr,proxy_arg_t *a,const char *p,const char *q){
 if(nr==PROXY_SC_unlinkat && (a[2]&AT_REMOVEDIR))remove_empty_store(p);
 struct proxy_stat ps,qs;int left=!arch_stat_at(AT_FDCWD,p,&ps,AT_SYMLINK_NOFOLLOW) && is_placeholder(&ps);
 int right=q && !arch_stat_at(AT_FDCWD,q,&qs,AT_SYMLINK_NOFOLLOW) && is_placeholder(&qs);
 if(!left && !right){
  /* Directory moves can carry placeholders; serialize them with the GC sweep's scan. */
  int dirmove=q && (nr==PROXY_SC_renameat || nr==PROXY_SC_renameat2) && !arch_stat_at(AT_FDCWD,p,&ps,AT_SYMLINK_NOFOLLOW) && S_ISDIR(ps.st_mode);
  char *paths=dirmove?vfs_alloc(PATH_MAX*2):NULL;struct proxy_stat ss;
  if(!paths || !links_app_data_root(paths,p) || !catpath(paths+PATH_MAX,paths,".v3-inodes") || arch_stat_at(AT_FDCWD,paths+PATH_MAX,&ss,AT_SYMLINK_NOFOLLOW) || !S_ISDIR(ss.st_mode)){if(paths)vfs_free(paths);return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);}
  long lock=store_lock(paths,paths+PATH_MAX);if(lock<0){vfs_free(paths);return lock;}
  long r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);unlock(lock);vfs_free(paths);return r;
 }
 struct { char root[PATH_MAX],other[PATH_MAX],store[PATH_MAX],left[PATH_MAX],right[PATH_MAX]; } *scratch=vfs_alloc(sizeof(*scratch));if(!scratch)return -ENOMEM;
 char *root=scratch->root,*other=scratch->other,*store=scratch->store;
 if(left && right && q && (nr==PROXY_SC_renameat || (nr==PROXY_SC_renameat2 && !(a[4]&RENAME_NOREPLACE)))){if(links_target(p,scratch->left) && links_target(q,scratch->right) && p_equal(scratch->left,scratch->right)){vfs_free(scratch);return 0;}}
 if(!links_app_data_root(root,left?p:q)){vfs_free(scratch);return -EPERM;}
 if(q && left && (!links_app_data_root(other,q) || !p_equal(root,other))){vfs_free(scratch);return -EXDEV;}
 if(!catpath(store,root,".v3-inodes")){vfs_free(scratch);return -ENAMETOOLONG;}
 long lock=store_lock(root,store);if(lock<0){vfs_free(scratch);return lock;}long r=recover(root,store);if(!r)r=begin_intent(root,store,p,q,NULL);if(!r){r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);{long clean=finish_intent(store);if(!r)r=clean;}}unlock(lock);vfs_free(scratch);return r;
}
long links_path_call(long nr,proxy_arg_t *args,const char *p){
 proxy_arg_t a[6];p_copy(a,args,sizeof(a));
 int path0=nr==PROXY_SC_getxattr || nr==PROXY_SC_lgetxattr || nr==PROXY_SC_listxattr || nr==PROXY_SC_llistxattr || nr==PROXY_SC_setxattr || nr==PROXY_SC_lsetxattr || nr==PROXY_SC_removexattr || nr==PROXY_SC_lremovexattr || nr==PROXY_SC_truncate || nr==PROXY_SC_statfs;
 int metadata=nr==PROXY_SC_fchmodat || nr==PROXY_SC_fchmodat2 || nr==PROXY_SC_fchownat || nr==PROXY_SC_utimensat || nr==PROXY_SC_listxattr || nr==PROXY_SC_llistxattr || nr==PROXY_SC_getxattr || nr==PROXY_SC_lgetxattr || nr==PROXY_SC_setxattr || nr==PROXY_SC_lsetxattr || nr==PROXY_SC_removexattr || nr==PROXY_SC_lremovexattr || nr==PROXY_SC_getxattrat || nr==PROXY_SC_listxattrat || nr==PROXY_SC_setxattrat || nr==PROXY_SC_removexattrat || nr==PROXY_SC_name_to_handle_at || nr==PROXY_SC_inotify_add_watch;
 char object[PATH_MAX];struct proxy_stat st;
 /* chmod/chown/time succeed on inaccessible files: identify before changing. */
 int stored=metadata && links_target(p,object);
 if(stored){refresh(object);a[path0?0:1]=(long)object;}
 long r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
 if(!r && (nr==PROXY_SC_newfstatat || nr==PROXY_SC_statx)){
  links_payload_path_stat(p,(void *)(uintptr_t)a[nr==PROXY_SC_statx?4:2],nr==PROXY_SC_statx);return r;
 }
 if(nr==PROXY_SC_openat && r>=0 && (a[2]&O_PATH) && !(a[2]&O_DIRECTORY)){
  if(arch_stat_fd((int)r,&st) || !is_placeholder(&st))return r;
  if(!links_target(p,object))return r;
  refresh(object);
  long replacement=RAW(nr,a[0],object,a[2],a[3],a[4],a[5]);RAW(PROXY_SC_close,r,0,0,0,0,0);return replacement;
 }
 if(r!=-EACCES || stored)return r;
 if(!links_target(p,object))return r;
 /* O_EXCL must still observe the existing placeholder name. */
 if(nr==PROXY_SC_openat && (a[2]&(O_CREAT|O_EXCL))==(O_CREAT|O_EXCL))return -EEXIST;
 refresh(object);
 a[path0?0:1]=(long)object;
 return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
}
static void refresh_impl(const char *object,char *root,char *store){size_t i=0;for(;object[i];i++){const char marker[]="/.v3-inodes/";size_t j=0;while(marker[j] && object[i+j]==marker[j])j++;if(!marker[j])break;}if(!object[i] || i>=PATH_MAX)return;p_copy(root,object,i);root[i]=0;long lock=store_lock(root,store);if(lock>=0){if(!recover(root,store)){struct proxy_stat st;if(!arch_stat_at(AT_FDCWD,object,&st,0))ensure_count(root,store,object);}unlock(lock);}}
static void refresh(const char *object){char *scratch=vfs_alloc(PATH_MAX*2);if(!scratch)return;refresh_impl(object,scratch,scratch+PATH_MAX);vfs_free(scratch);}
/* Only regular files can be store objects; skip the /proc lookup for the rest. */
static char *fd_backing_path(int fd){
 char proc[80]="/proc/self/fd/",path[PATH_MAX];size_t k=14;k+=number(proc+k,(unsigned)fd);proc[k]=0;
 long n=RAW(PROXY_SC_readlinkat,AT_FDCWD,proc,path,sizeof(path)-1,0,0);
 if(n<=0)return NULL;
 path[n]=0;size_t length=(size_t)n+1;char *copy=vfs_alloc(length);
 if(copy)p_copy(copy,path,length);
 return copy;
}
long links_fd_chmod(long nr,proxy_arg_t *a){
 struct proxy_stat st;int fd=(int)a[0];
 if(!arch_stat_fd(fd,&st) && is_placeholder(&st)){
  char *path=fd_backing_path(fd),object[PATH_MAX];
  int stored=path && links_target(path,object);vfs_free(path);
  if(stored){refresh(object);return RAW(PROXY_SC_fchmodat,AT_FDCWD,object,a[nr==PROXY_SC_fchmod?1:2],0,0,0);}
  /* An unindexed 01000 inode is an accepted false positive. */
 }
 return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
}
void links_fd_stat(int fd,struct proxy_stat *st){
 if(!S_ISREG(st->st_mode))return;
 if(is_placeholder(st)){
  char *path=fd_backing_path(fd),object[PATH_MAX];
  if(path && links_target(path,object)){refresh(object);if(!arch_stat_at(AT_FDCWD,object,st,0))st->st_nlink=read_count(object);}
  vfs_free(path);return;
 }
 for(struct known_object *k=__atomic_load_n(&known_objects[object_bucket(st->st_dev,st->st_ino)],__ATOMIC_ACQUIRE);k;k=k->next)
  if(k->dev==st->st_dev && k->ino==st->st_ino){
   /* Inodes are reused after a store object is deleted: trust the hit only if the
    * path still names this inode and its count reads; else keep the kernel nlink. */
   struct proxy_stat now;if(arch_stat_at(AT_FDCWD,k->path,&now,0) || now.st_dev!=st->st_dev || now.st_ino!=st->st_ino)return;
   refresh(k->path);uint64_t n=0;if(checked_count(k->path,&n)==0)st->st_nlink=n;return;}
}
void links_stat(const char *object,struct proxy_stat *st){if(component(object) && S_ISREG(st->st_mode)){refresh(object);st->st_nlink=read_count(object);}}
static long dents_locked(long fd,void *buf,long size){
 long n;do{n=RAW(PROXY_SC_getdents64,fd,buf,size,0,0,0);if(n<=0)return n;long used=0;for(long off=0;off<n;){unsigned char *d=(unsigned char *)buf+off;unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n)return -EIO;char *name=(void *)(d+19);if(!p_equal(name,".v3-inodes")){for(unsigned short i=0;i<len;i++)((unsigned char *)buf)[used+i]=d[i];used+=len;}off+=len;}if(used)return used;}while(n>0);return n;
}
static int proc_self_fd_directory(long fd){
 char *name=vfs_fd_name((int)fd);if(!name)return 0;
 int match=p_equal(name,"/proc/self/fd")||p_equal(name,"proc/self/fd")||
           p_equal(name,"/proc/thread-self/fd")||p_equal(name,"proc/thread-self/fd");
 vfs_free(name);return match;
}
static long proc_self_fd_dents(long fd,void *buf,long size){
 long n;do{
  n=RAW(PROXY_SC_getdents64,fd,buf,size,0,0,0);if(n<=0)return n;
  long used=0;unsigned resident_hidden=0;
  for(long off=0;off<n;){
   unsigned char *d=(unsigned char *)buf+off;unsigned short len=*(unsigned short *)(d+16);
   if(len<20 || off+len>n)return -EIO;
   const char *name=(const char *)(d+19);unsigned value=0;int numeric=*name!=0,private=0;
   for(const char *p=name;*p;p++){
    if(*p<'0'||*p>'9'||value>(2147483647U-(unsigned)(*p-'0'))/10){numeric=0;break;}
    value=value*10+(unsigned)(*p-'0');
   }
   /* Resident Android driver descriptors survive the emulated exec but
    * are not Linux payload descriptors. In particular, forwarding these
    * through Zypak's enumerate-and-send protocol truncates SCM_RIGHTS. */
   if(numeric && proxy_resident_fd_owned((int)value)){private=1;resident_hidden++;}
   else if(numeric && vfs_private_fd((int)value))private=1;
   if(!private){for(unsigned short i=0;i<len;i++)((unsigned char *)buf)[used+i]=d[i];used+=len;}
   off+=len;
  }
  if(proxy_trace && resident_hidden)proxy_log("proc-resident-fds-hidden",fd,resident_hidden);
  if(used)return used;
 }while(n>0);
 return n;
}
long links_dents(long fd,void *buf,long size){
 if(proc_self_fd_directory(fd))return proc_self_fd_dents(fd,buf,size);
 return dents_locked(fd,buf,size);
}
void links_payload_fd_stat(int fd,void *wire){
 struct proxy_stat s;arch_stat_decode(&s,wire);
 if(is_placeholder(&s)){char *path=fd_backing_path(fd);if(path){links_payload_path_stat(path,wire,0);vfs_free(path);}return;}
 links_fd_stat(fd,&s);arch_stat_nlink(wire,s.st_nlink);
}
void links_payload_path_stat(const char *path,void *wire,int extended){
 struct proxy_stat s;char object[PATH_MAX];
 if(extended){struct statx *x=wire;s=(struct proxy_stat){.st_mode=x->stx_mode};}
 else arch_stat_decode(&s,wire);
 if(is_placeholder(&s) && links_target(path,object)){
  refresh(object);
  if(extended){if(!RAW(PROXY_SC_statx,AT_FDCWD,object,0,STATX_BASIC_STATS,wire,0))arch_statx_nlink(wire,read_count(object));}
  else if(!RAW(PROXY_SC_newfstatat,AT_FDCWD,object,wire,0,0,0))arch_stat_nlink(wire,read_count(object));
 }else if(component(path)){links_stat(path,&s);if(extended)arch_statx_nlink(wire,s.st_nlink);else arch_stat_nlink(wire,s.st_nlink);}
}

long links_publish(const char *source,const char *target,long flags){
 if(flags&~AT_SYMLINK_FOLLOW)return -EINVAL;
 return store_link(source,target);
}
