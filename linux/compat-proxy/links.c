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
static int private_dir(const char *p,dev_t dev){struct proxy_stat st;return !arch_stat_at(AT_FDCWD,p,&st,AT_SYMLINK_NOFOLLOW) && S_ISDIR(st.st_mode) && st.st_dev==dev && st.st_uid==links_euid() && (st.st_mode&0300)==0300 && !(st.st_mode&0022);}
/* A marker is ordinary visible metadata, but only its owner may establish it
 * outside the payload. Never follow a symlink when recognizing a boundary. */
static int marked_device(const char *dir,uint64_t *device){
 char name[PATH_MAX];if(!catpath(name,dir,".v3-store-root"))return 0;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,name,O_PATH|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return 0;
 struct proxy_stat st;long r=arch_stat_fd(fd,&st);RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(r || !S_ISREG(st.st_mode) || st.st_uid!=links_euid())return 0;
 if(device)*device=st.st_dev;
 return 1;
}
/* Android denies link(2) in app data. OSTree needs it while Flatpak deploys
 * an installation, so permit the same emulation in each app's private
 * <data-dir>/install tree. Keep this boundary tied to Android app-data paths;
 * a random directory named "install" does not opt in. */
static int app_install_path(const char *dir){
 const char *p=dir,*package=0;
 const char *prefixes[]={"/data/user/","/data/user_de/","/data/data/"};
 int matched=0;
 for(size_t k=0;k<sizeof(prefixes)/sizeof(prefixes[0]);k++){
  const char *prefix=prefixes[k];size_t n=p_len(prefix),i=0;
  while(prefix[i] && dir[i]==prefix[i])i++;
  if(i!=n)continue;
  p=dir+n;
  if(k<2){
   const char *user=p;while(*p>='0' && *p<='9')p++;
   if(p==user || *p!='/')continue;
   p++;
  }
  matched=1;break;
 }
 if(!matched)return 0;
 package=p;
 while(*p && *p!='/')p++;
 if(p==package || !*p || !p_equal(p,"/install"))return 0;
 return 1;
}
static int app_install_device(const char *dir,uint64_t *device){
 if(!app_install_path(dir))return 0;
 struct proxy_stat st;
 if(arch_stat_at(AT_FDCWD,dir,&st,AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode)
     || st.st_uid!=links_euid() || (st.st_mode&0300)!=0300 || (st.st_mode&0022))return 0;
 if(device)*device=st.st_dev;
 return 1;
}
int links_marker(const char *path){const char *name=path;for(const char *p=path;*p;p++)if(*p=='/')name=p+1;return p_equal(name,".v3-store-root");}
/* Marker roots change only through path-mutating syscalls handled by path_call.
 * Every directory on a walk shares the walk's answer (its nearest marker root,
 * or none), so the walk caches each directory it visited: sibling and child
 * lookups then cost no marker probes. Roots live in a small side table so a
 * cache slot only holds its own key. A failed try-lock falls back to the
 * uncached walk, so an interrupting signal handler cannot deadlock on a lock
 * held by its caller. Keys too long for a slot are simply not cached. */
#define TREE_CACHE_SLOTS 256
#define TREE_CACHE_KEY 480
#define TREE_ROOTS 8
#define TREE_WALK_DEPTH 64
struct tree_cache_entry {uint64_t generation;int root;char key[TREE_CACHE_KEY];};
struct tree_root {uint64_t generation,device;char path[PATH_MAX];};
static struct tree_cache_entry tree_cache[TREE_CACHE_SLOTS];
static struct tree_root tree_roots[TREE_ROOTS];
static volatile unsigned tree_cache_lock;
static volatile uint64_t tree_cache_generation=1;
void links_cache_invalidate(void){__atomic_add_fetch(&tree_cache_generation,1,__ATOMIC_RELEASE);}
static int tree_cache_trylock(void){unsigned unlocked=0;return __atomic_compare_exchange_n(&tree_cache_lock,&unlocked,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);}
static void tree_cache_unlock(void){__atomic_store_n(&tree_cache_lock,0,__ATOMIC_RELEASE);}
static uint64_t tree_cache_hash(const char *p){uint64_t h=UINT64_C(1469598103934665603);for(;*p;p++){h^=(unsigned char)*p;h*=UINT64_C(1099511628211);}return h;}
/* Caller holds the lock. Returns -2 on miss, -1 for "no marker", else a root index. */
static int tree_cache_get(const char *dir,uint64_t generation){
 if(p_len(dir)>=TREE_CACHE_KEY)return -2;
 struct tree_cache_entry *e=&tree_cache[tree_cache_hash(dir)%TREE_CACHE_SLOTS];
 if(e->generation!=generation || !p_equal(e->key,dir))return -2;
 if(e->root>=0 && tree_roots[e->root].generation!=generation)return -2;
 return e->root;
}
static void tree_cache_put(const char *dir,uint64_t generation,int root){
 size_t n=p_len(dir);if(n>=TREE_CACHE_KEY)return;
 struct tree_cache_entry *e=&tree_cache[tree_cache_hash(dir)%TREE_CACHE_SLOTS];
 e->generation=generation;e->root=root;p_copy(e->key,dir,n+1);
}
static int tree_root_put(const char *path,uint64_t device,uint64_t generation){
 for(int i=0;i<TREE_ROOTS;i++)if(tree_roots[i].generation==generation && p_equal(tree_roots[i].path,path)){tree_roots[i].device=device;return i;}
 for(int i=0;i<TREE_ROOTS;i++)if(tree_roots[i].generation!=generation){
  tree_roots[i].generation=generation;tree_roots[i].device=device;p_copy(tree_roots[i].path,path,p_len(path)+1);return i;
 }
 return -2;
}
static int tree(char *root,const char *path){
 char up[PATH_MAX],key[PATH_MAX];if(!parent(key,path))return 0;
 uint64_t generation=__atomic_load_n(&tree_cache_generation,__ATOMIC_ACQUIRE),device=0;
 /* Offsets into key of each directory walked past without an answer. */
 size_t visited[TREE_WALK_DEPTH];int depth=0,marked=0,cached=-2;
 p_copy(root,key,p_len(key)+1);
 for(;;){
  if(tree_cache_trylock()){
   cached=tree_cache_get(root,generation);
   if(cached>=0){device=tree_roots[cached].device;p_copy(root,tree_roots[cached].path,p_len(tree_roots[cached].path)+1);}
   tree_cache_unlock();
  }
  if(cached==-1){marked=0;break;}
  if(cached>=0){
   marked=private_dir(root,device);
   if(!marked){links_cache_invalidate();return tree(root,path);}
   break;
  }
  if(marked_device(root,&device)){marked=private_dir(root,device);break;}
  if(app_install_device(root,&device)){marked=1;break;}
  if(depth<TREE_WALK_DEPTH)visited[depth++]=p_len(root);
  if(!parent(up,root) || p_equal(up,root)){marked=0;break;}
  p_copy(root,up,p_len(up)+1);
 }
 if(tree_cache_trylock()){
  if(generation==__atomic_load_n(&tree_cache_generation,__ATOMIC_ACQUIRE)){
   int index=marked?tree_root_put(root,device,generation):-1;
   if(index!=-2){
    /* The walked directories are prefixes of key; truncate in place. */
    char dir[PATH_MAX];
    for(int i=0;i<depth;i++){p_copy(dir,key,visited[i]);dir[visited[i]]=0;tree_cache_put(dir,generation,index);}
    if(marked)tree_cache_put(root,generation,index);
   }
  }
  tree_cache_unlock();
 }
 return marked;
}
/* One constant-time index entry per physical placeholder inode. Values are
 * relative object IDs, so directory relocation needs no target rewriting. */
static int index_path(char *out,const char *store,const struct proxy_stat *st){
 char key[36]="p-";const char hex[]="0123456789abcdef";
 for(unsigned i=0;i<16;i++){key[2+i]=hex[(st->st_dev>>(60-4*i))&15];key[19+i]=hex[(st->st_ino>>(60-4*i))&15];}
 key[18]='-';key[35]=0;return catpath(out,store,key);
}
int links_target(const char *name,char *out){
 struct proxy_stat st;char root[PATH_MAX],store[PATH_MAX],entry[PATH_MAX],id[36];
 if(!tree(root,name) || arch_stat_at(AT_FDCWD,name,&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || !(st.st_mode&S_ISVTX) || !catpath(store,root,".v3-inodes") || !index_path(entry,store,&st))return 0;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,entry,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return 0;
 long n=RAW(PROXY_SC_read,fd,id,sizeof(id),0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(n!=35 || id[0]!='i' || id[1]!='-' || id[34])return 0;
 for(unsigned i=2;i<34;i++)if(!((id[i]>='0' && id[i]<='9') || (id[i]>='a' && id[i]<='f')))return 0;
 return catpath(out,store,id) && !arch_stat_at(AT_FDCWD,out,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode);
}
static long store_lock(const char *root,char *store){if(!catpath(store,root,".v3-inodes"))return -ENAMETOOLONG;long r=RAW(PROXY_SC_mkdirat,AT_FDCWD,store,0700,0,0,0);if(r && r!=-EEXIST)return r;struct proxy_stat st;if(arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode) || st.st_uid!=links_euid() || (st.st_mode&0077))return -EPERM;char file[PATH_MAX];if(!catpath(file,store,"lock"))return -ENAMETOOLONG;long fd=RAW(PROXY_SC_openat,AT_FDCWD,file,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(fd<0)return fd;do{r=RAW(PROXY_SC_flock,fd,LOCK_EX,0,0,0,0);}while(r==-EINTR);if(r){RAW(PROXY_SC_close,fd,0,0,0,0,0);return r;}struct proxy_stat own,named;if(arch_stat_fd(fd,&own) || arch_stat_at(AT_FDCWD,file,&named,AT_SYMLINK_NOFOLLOW) || own.st_ino!=named.st_ino || own.st_dev!=named.st_dev){RAW(PROXY_SC_close,fd,0,0,0,0,0);return store_lock(root,store);}return fd;}
static void unlock(long fd){RAW(PROXY_SC_flock,fd,LOCK_UN,0,0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);}
static long count_names(const char *dir,const char *object,dev_t dev,uint64_t *count){
#ifdef COMPAT_LINKS_TEST
 proxy_log("links tree walk",0,0);
#endif
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 char buf[4096];long r=0,n;
 while((n=RAW(PROXY_SC_getdents64,fd,buf,sizeof(buf),0,0,0))>0){for(long off=0;off<n;){unsigned char *d=(void *)(buf+off);unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n){r=-EIO;goto end;}const char *name=(void *)(d+19);off+=len;if(p_equal(name,".") || p_equal(name,"..") || p_equal(name,".v3-inodes"))continue;char path[PATH_MAX],target[PATH_MAX];if(!catpath(path,dir,name)){r=-ENAMETOOLONG;goto end;}struct proxy_stat st;r=arch_stat_at(AT_FDCWD,path,&st,AT_SYMLINK_NOFOLLOW);if(r==-ENOENT){r=0;continue;}if(r)goto end;if(S_ISREG(st.st_mode) && (st.st_mode&S_ISVTX) && links_target(path,target) && p_equal(target,object)){if(*count==UINT64_MAX){r=-EOVERFLOW;goto end;}(*count)++;}else if(S_ISDIR(st.st_mode) && st.st_dev==dev){r=count_names(path,object,dev,count);if(r)goto end;}}}
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
/* A single staging name is covered by the store lock. The index is complete
 * before the placeholder becomes visible. Recovery discards unpublished work. */
static void discard_new(const char *store){
 char path[PATH_MAX],entry[PATH_MAX];struct proxy_stat st;
 if(catpath(path,store,"new") && !arch_stat_at(AT_FDCWD,path,&st,AT_SYMLINK_NOFOLLOW)){
  if(index_path(entry,store,&st))RAW(PROXY_SC_unlinkat,AT_FDCWD,entry,0,0,0,0);
  RAW(PROXY_SC_unlinkat,AT_FDCWD,path,0,0,0,0);
 }
}
static long placeholder(const char *store,const char *name,const char *object){
 char temp[PATH_MAX],entry[PATH_MAX];if(!catpath(temp,store,"new"))return -ENAMETOOLONG;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600|S_ISVTX,0,0);if(fd<0)return fd;
 struct proxy_stat st;long r=arch_stat_fd(fd,&st);RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(!r && !index_path(entry,store,&st))r=-ENAMETOOLONG;
 const char *id=object;for(const char *p=object;*p;p++)if(*p=='/')id=p+1;
 if(!r)r=journal_write(store,entry+p_len(store)+1,id,p_len(id)+1);
 if(!r)r=RAW(PROXY_SC_renameat2,AT_FDCWD,temp,AT_FDCWD,name,RENAME_NOREPLACE,0);
 if(r)discard_new(store);
 return r;
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
struct intent { uint64_t magic, count[2], before[2]; char object[2][PATH_MAX], name[2][PATH_MAX]; struct proxy_stat placeholder[2]; };
static int terminated(const char *p){for(size_t i=0;i<PATH_MAX;i++)if(!p[i])return 1;return 0;}
static unsigned present(const char *name,const char *object){char target[PATH_MAX];return name[0] && links_target(name,target) && p_equal(target,object);}
static long finish_intent(const char *store){

 char path[PATH_MAX];if(!catpath(path,store,"intent"))return -ENAMETOOLONG;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd==-ENOENT)return 0;if(fd<0)return fd;
 struct intent *j=vfs_alloc(sizeof(*j));if(!j){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -ENOMEM;}
 long r=RAW(PROXY_SC_read,fd,j,sizeof(*j),0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(r!=sizeof(*j) || j->magic!=0x76336c696e6b0002ULL){vfs_free(j);return -EIO;}

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
 /* Remove retired placeholder identities, including overwritten destinations.
  * Rename/exchange keep an identity if either affected name still owns it. */
 for(int i=0;i<2 && !r;i++)if(j->placeholder[i].st_ino){
  int live=0;for(int k=0;k<2;k++){struct proxy_stat st;if(j->name[k][0] && !arch_stat_at(AT_FDCWD,j->name[k],&st,AT_SYMLINK_NOFOLLOW) && st.st_dev==j->placeholder[i].st_dev && st.st_ino==j->placeholder[i].st_ino)live=1;}
  char entry[PATH_MAX];if(!live && index_path(entry,store,&j->placeholder[i])){long e=RAW(PROXY_SC_unlinkat,AT_FDCWD,entry,0,0,0,0);if(e && e!=-ENOENT)r=e;}
 }
 vfs_free(j);if(!r)r=RAW(PROXY_SC_unlinkat,AT_FDCWD,path,0,0,0,0);return r;
}
static long begin_intent(const char *root,const char *store,const char *p,const char *q,const char *extra){
 struct intent *j=vfs_alloc(sizeof(*j));if(!j)return -ENOMEM;*j=(struct intent){.magic=0x76336c696e6b0002ULL};
 p_copy(j->name[0],p,p_len(p)+1);if(q)p_copy(j->name[1],q,p_len(q)+1);
 if(!links_target(p,j->object[0]))j->object[0][0]=0;
 if(q && !links_target(q,j->object[1]))j->object[1][0]=0;
 if(j->object[0][0])arch_stat_at(AT_FDCWD,p,&j->placeholder[0],AT_SYMLINK_NOFOLLOW);
 if(j->object[1][0])arch_stat_at(AT_FDCWD,q,&j->placeholder[1],AT_SYMLINK_NOFOLLOW);
 if(extra)p_copy(j->object[0],extra,p_len(extra)+1);
 if(p_equal(j->object[0],j->object[1]))j->object[1][0]=0;
 long r=0;for(int i=0;i<2;i++)if(j->object[i][0]){r=ensure_count(root,store,j->object[i]);if(r)break;j->count[i]=read_count(j->object[i]);j->before[i]=present(p,j->object[i])+(q && !p_equal(p,q)?present(q,j->object[i]):0);if(extra && j->count[i]==UINT64_MAX){r=-EOVERFLOW;break;}}
 if(!r && (j->object[0][0] || j->object[1][0]))r=journal_write(store,"intent",j,sizeof(*j));
 vfs_free(j);return r;
}
struct recount_record {struct recount_record *next;dev_t dev;ino_t ino;};
static struct recount_record *recounted;
static long recover(const char *root,const char *store){
 discard_new(store);
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
 struct recount_record *record=vfs_palloc(sizeof(*record));if(!record)return -ENOMEM;
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
 r=arch_stat_at(AT_FDCWD,source,&src,AT_SYMLINK_NOFOLLOW);if(r)goto end;if(S_ISLNK(src.st_mode)){r=-EPERM;goto end;}
 struct proxy_stat exists;if(!arch_stat_at(AT_FDCWD,target,&exists,AT_SYMLINK_NOFOLLOW)){r=-EEXIST;goto end;}
 if(!links_target(source,object)){
  r=arch_stat_at(AT_FDCWD,source,&src,AT_SYMLINK_NOFOLLOW);if(r)goto end;if(!S_ISREG(src.st_mode) || src.st_uid!=links_euid()){r=-EPERM;goto end;}
  char id[35]="i-";unsigned char nonce[16];long random=RAW(PROXY_SC_getrandom,nonce,sizeof(nonce),0,0,0,0);if(random!=sizeof(nonce)){r=random<0?random:-EIO;goto end;}const char hex[]="0123456789abcdef";for(size_t k=0;k<sizeof(nonce);k++){id[2+2*k]=hex[nonce[k]>>4];id[3+2*k]=hex[nonce[k]&15];}id[34]=0;if(!catpath(object,store,id)){r=-ENAMETOOLONG;goto end;}
  char journal[PATH_MAX];if(!catpath(journal,store,"conversion")){r=-ENAMETOOLONG;goto end;}char data[PATH_MAX*2];size_t sn=p_len(source)+1,on=p_len(object)+1;p_copy(data,source,sn);p_copy(data+sn,object,on);r=journal_write(store,"conversion",data,sn+on);if(r)goto end;
  r=RAW(PROXY_SC_renameat2,AT_FDCWD,source,AT_FDCWD,object,RENAME_NOREPLACE,0);if(r)goto end;
  if(src.st_mode&S_ISVTX){r=RAW(PROXY_SC_fchmodat,AT_FDCWD,object,src.st_mode&07777&~S_ISVTX,0,0,0);if(r)goto end;}
  r=placeholder(store,source,object);if(r){RAW(PROXY_SC_renameat2,AT_FDCWD,object,AT_FDCWD,source,RENAME_NOREPLACE,0);goto end;}r=write_count(object,1);if(r)goto end;RAW(PROXY_SC_unlinkat,AT_FDCWD,journal,0,0,0,0);
 }
 r=begin_intent(root,store,source,target,object);if(r)goto end;
 r=placeholder(store,target,object);{long clean=finish_intent(store);if(!r)r=clean;}
end:unlock(lock);return r;
}
long links_name_call(long nr,proxy_arg_t *a,const char *p,const char *q){
 char root[PATH_MAX],other[PATH_MAX],store[PATH_MAX];
 if(q && (nr==PROXY_SC_renameat || (nr==PROXY_SC_renameat2 && !(a[4]&RENAME_NOREPLACE)))){char left[PATH_MAX],right[PATH_MAX];if(links_target(p,left) && links_target(q,right) && p_equal(left,right))return 0;}
 if(nr==PROXY_SC_unlinkat && (a[2]&AT_REMOVEDIR))remove_empty_store(p);
 int left=tree(root,p),right=q && tree(other,q);
 if(!left && !right)return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
 if(q && (!left || !right || !p_equal(root,other))){
  char object[PATH_MAX];
  /* A placeholder cannot leave its index. Ordinary incoming files can
   * replace one; journal the destination's removed reference. */
  if(links_target(p,object))return -EXDEV;
  if(!links_target(q,object))return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
  p_copy(root,other,p_len(other)+1);
 }
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
 if(stored){r=ensure_count(root,store,object);if(r)goto end;
  /* Only the final stored name is substituted. Keep flags intact: the
   * backing inode is already regular, so the kernel enforces NOFOLLOW,
   * O_EXCL, O_DIRECTORY and unknown flag semantics itself. */
  int path0=nr==PROXY_SC_getxattr || nr==PROXY_SC_lgetxattr || nr==PROXY_SC_listxattr || nr==PROXY_SC_llistxattr || nr==PROXY_SC_setxattr || nr==PROXY_SC_lsetxattr || nr==PROXY_SC_removexattr || nr==PROXY_SC_lremovexattr || nr==PROXY_SC_truncate || nr==PROXY_SC_statfs;
  a[path0?0:1]=(long)object;
 }
 if(nr==PROXY_SC_fchmodat || nr==PROXY_SC_fchmodat2){
  struct proxy_stat mode;if(!arch_stat_at(AT_FDCWD,stored?object:p,&mode,AT_SYMLINK_NOFOLLOW) && S_ISREG(mode.st_mode))a[2]=(a[2]&~S_ISVTX)|(mode.st_mode&S_ISVTX);
 }
 r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
 if(!r && stored && nr==PROXY_SC_newfstatat)arch_stat_nlink((void *)(uintptr_t)a[2],read_count(object));
 if(!r && stored && nr==PROXY_SC_statx)arch_statx_nlink((void *)(uintptr_t)a[4],read_count(object));
end:if(lock>=0)unlock(lock);return r;
}
static void refresh(const char *object){char root[PATH_MAX],store[PATH_MAX];size_t i=0;for(;object[i];i++){const char marker[]="/.v3-inodes/";size_t j=0;while(marker[j] && object[i+j]==marker[j])j++;if(!marker[j])break;}if(!object[i] || i>=PATH_MAX)return;p_copy(root,object,i);root[i]=0;long lock=store_lock(root,store);if(lock>=0){if(!recover(root,store)){struct proxy_stat st;if(!arch_stat_at(AT_FDCWD,object,&st,0))ensure_count(root,store,object);}unlock(lock);}}
void links_recover_path(const char *path){char root[PATH_MAX],store[PATH_MAX];struct proxy_stat st;if(!tree(root,path) || !catpath(store,root,".v3-inodes") || arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW))return;long lock=store_lock(root,store);if(lock>=0){recover(root,store);unlock(lock);}}
/* Only regular files can be store objects; skip the /proc lookup for the rest. */
static char *fd_path(int fd,uint64_t dev,uint64_t ino){
 char *tracked=vfs_fd_name_identity(fd,dev,ino);if(tracked)return tracked;
 char proc[80]="/proc/self/fd/",path[PATH_MAX];size_t k=14;k+=number(proc+k,(unsigned)fd);proc[k]=0;
 long n=RAW(PROXY_SC_readlinkat,AT_FDCWD,proc,path,sizeof(path)-1,0,0);
 if(n<=0)return NULL;
 path[n]=0;size_t length=(size_t)n+1;char *copy=vfs_alloc(length);
 if(copy)p_copy(copy,path,length);
 return copy;
}
void links_fd_stat(int fd,struct proxy_stat *st){
 if(!S_ISREG(st->st_mode))return;
 char *path=fd_path(fd,(uint64_t)st->st_dev,(uint64_t)st->st_ino);
 if(path){links_stat(path,st);vfs_free(path);}
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
  long used=0;
  for(long off=0;off<n;){
   unsigned char *d=(unsigned char *)buf+off;unsigned short len=*(unsigned short *)(d+16);
   if(len<20 || off+len>n)return -EIO;
   const char *name=(const char *)(d+19);unsigned value=0;int numeric=*name!=0,private=0;
   for(const char *p=name;*p;p++){
    if(*p<'0'||*p>'9'||value>(2147483647U-(unsigned)(*p-'0'))/10){numeric=0;break;}
    value=value*10+(unsigned)(*p-'0');
   }
   if(numeric && vfs_private_fd((int)value))private=1;
   if(!private){for(unsigned short i=0;i<len;i++)((unsigned char *)buf)[used+i]=d[i];used+=len;}
   off+=len;
  }
  if(used)return used;
 }while(n>0);
 return n;
}
long links_dents(long fd,void *buf,long size){
 if(proc_self_fd_directory(fd))return proc_self_fd_dents(fd,buf,size);
 char path[PATH_MAX],root[PATH_MAX],store[PATH_MAX];long lock=-1;struct proxy_stat st,fdst;
 char *tracked=vfs_fd_name_cached((int)fd);
 if(!tracked && !arch_stat_fd((int)fd,&fdst))
  tracked=fd_path((int)fd,(uint64_t)fdst.st_dev,(uint64_t)fdst.st_ino);
 if(tracked){size_t n=p_len(tracked);if(n+3<=sizeof(path)){p_copy(path,tracked,n);path[n++]='/';path[n++]='.';path[n]=0;}else n=0;vfs_free(tracked);
  if(n && tree(root,path) && catpath(store,root,".v3-inodes") && !arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW)){lock=store_lock(root,store);if(lock<0)return lock;long r=recover(root,store);if(r){unlock(lock);return r;}}
 }else{
  char proc[80]="/proc/self/fd/";size_t k=14;k+=number(proc+k,(unsigned)fd);proc[k]=0;long n=RAW(PROXY_SC_readlinkat,AT_FDCWD,proc,path,sizeof(path)-3,0,0);
  if(n>0){path[n++]='/';path[n++]='.';path[n]=0;if(tree(root,path) && catpath(store,root,".v3-inodes") && !arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW)){lock=store_lock(root,store);if(lock<0)return lock;long r=recover(root,store);if(r){unlock(lock);return r;}}}
 }
 long r=dents_locked(fd,buf,size);if(lock>=0)unlock(lock);return r;
}
void links_payload_fd_stat(int fd,void *wire){struct proxy_stat s;arch_stat_decode(&s,wire);links_fd_stat(fd,&s);arch_stat_nlink(wire,s.st_nlink);}
void links_payload_path_stat(const char *path,void *wire,int extended){struct proxy_stat s;if(extended){struct statx *x=wire;s=(struct proxy_stat){.st_mode=x->stx_mode,.st_nlink=x->stx_nlink};links_stat(path,&s);arch_statx_nlink(wire,s.st_nlink);}else{arch_stat_decode(&s,wire);links_stat(path,&s);arch_stat_nlink(wire,s.st_nlink);}}

long links_publish(const char *source,const char *target,long flags){
 char root[PATH_MAX],other[PATH_MAX];int left=tree(root,source),right=tree(other,target);
 if(!left || !right){
  char object[PATH_MAX];if(left && links_target(source,object))return -EXDEV;
  return RAW(PROXY_SC_linkat,AT_FDCWD,source,AT_FDCWD,target,flags,0);
 }
 if(flags&~AT_SYMLINK_FOLLOW)return -EINVAL;
 return store_link(source,target);
}
