#define _GNU_SOURCE
/* Compatibility only. Every backing operation retains the app's credentials. */
#include "proxy.h"
#include "vfs.h"
#include "links.h"
#include <sys/syscall.h>
#include <sys/mman.h>
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
/* Descriptor lists are process-private (and retained by resident exec).
 * Anonymous shared records carry reference counts and publication across fork.
 * One shared mapping per inode per process; dup aliases do not own new maps. */
struct temporary {
    unsigned refs;
    int backing;
    int published, exclusive; /* 0: hidden, -1: publishing, 1: named */
    char path[PATH_MAX];
};
struct descriptor {
    struct descriptor *next;
    int fd;
    struct temporary *tmp;
};
static struct descriptor *descriptors;
static unsigned long serial;
/* Process-wide descriptor bookkeeping, also used from SIGSYS handlers. Mask
 * signals to prevent reentry; a fork child discards a vanished parent's lock.
 * Shared inode records still retain their cross-process publication CAS. */
static unsigned long descriptor_owner;
static proxy_sigmask_t descriptor_lock(void){
 proxy_sigmask_t all=~UINT64_C(0),mask=0;
 RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&all,&mask,8,0,0);
 unsigned long pid=RAW(PROXY_SC_getpid,0,0,0,0,0,0);
 for(;;){unsigned long owner=__atomic_load_n(&descriptor_owner,__ATOMIC_RELAXED);
  if(!owner || owner!=pid){if(__atomic_compare_exchange_n(&descriptor_owner,&owner,pid,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return mask;}
  __asm__ volatile("pause");
 }
}
static void descriptor_unlock(proxy_sigmask_t mask){
 __atomic_store_n(&descriptor_owner,0,__ATOMIC_RELEASE);
 RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&mask,0,8,0,0);
}


static struct descriptor *find(int fd) {
    for(struct descriptor *p=descriptors;p;p=p->next)
        if(p->fd==fd)return p;
    return NULL;
}
static char *links_fd_name_locked(int fd) {
    struct descriptor *d=find(fd);
    if(!d || __atomic_load_n(&d->tmp->published,__ATOMIC_ACQUIRE)==-1)return NULL;
    return vfs_reverse(d->tmp->path);
}
static void links_close_locked(int fd) {
    struct descriptor **p=&descriptors;
    while(*p){
        struct descriptor *d=*p;
        if(d->fd!=fd){p=&d->next;continue;}
        *p=d->next;
        if(__atomic_sub_fetch(&d->tmp->refs,1,__ATOMIC_ACQ_REL)==0 &&
           !__atomic_load_n(&d->tmp->published,__ATOMIC_ACQUIRE))
            RAW(PROXY_SC_unlinkat,AT_FDCWD,d->tmp->path,0,0,0,0);
        int local=0;
        for(struct descriptor *alias=descriptors;alias;alias=alias->next)
            if(alias->tmp==d->tmp)local=1;
        if(!local){RAW(PROXY_SC_close,d->tmp->backing,0,0,0,0,0);RAW(PROXY_SC_munmap,d->tmp,sizeof(*d->tmp),0,0,0,0);}
        vfs_free(d);
        return;
    }
}
static int reserve_reference(struct temporary *t){
 unsigned refs=__atomic_load_n(&t->refs,__ATOMIC_RELAXED);
 do {if(refs==~0U)return 0;}
 while(!__atomic_compare_exchange_n(&t->refs,&refs,refs+1,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
 return 1;
}
static long links_dup_locked(int from,int to) {
    if(from==to)return 0;
    struct descriptor *d=find(from),*copy=NULL;
    if(d){
        copy=vfs_alloc(sizeof(*copy));
        if(!copy)return -ENOMEM;
        *copy=(struct descriptor){NULL,to,d->tmp};
        if(!reserve_reference(d->tmp)){vfs_free(copy);return -EOVERFLOW;}
    }
    links_close_locked(to);
    if(copy){copy->next=descriptors;descriptors=copy;}
    return 0;
}
/* Reserve child references BEFORE cloning, so a parent closing immediately
 * cannot delete the child's inode. A failed clone rolls the reservation back. */
static long links_fork_locked(int delta) {
    for(struct descriptor *d=descriptors;d;d=d->next){
        if(delta>0){if(!reserve_reference(d->tmp)){
            for(struct descriptor *p=descriptors;p!=d;p=p->next)__atomic_sub_fetch(&p->tmp->refs,1,__ATOMIC_RELAXED);
            return -EOVERFLOW;
        }}else __atomic_sub_fetch(&d->tmp->refs,1,__ATOMIC_RELAXED);
    }
    return 0;
}
static void links_exit_locked(void) {
    while(descriptors)links_close_locked(descriptors->fd);
}
static size_t number(char *out,unsigned long value){char b[24];size_t n=0,k=0;do{b[n++]=(char)('0'+value%10);value/=10;}while(value);while(n)out[k++]=b[--n];return k;}
static long links_open_locked(const char *dir,long flags,long mode){
 if(!(flags&O_RDWR) && !(flags&O_WRONLY))return -EINVAL;
 struct proxy_stat st;long r=arch_stat_at(AT_FDCWD,dir,&st,0);if(r<0)return r;if(!S_ISDIR(st.st_mode))return -ENOTDIR;
 size_t n=p_len(dir);if(n+70>=PATH_MAX)return -ENAMETOOLONG;
 struct descriptor *d=vfs_alloc(sizeof(*d));if(!d)return -ENOMEM;
 long backing=RAW(PROXY_SC_memfd_create,"v3-inode-ownership",0,0,0,0,0);
 if(backing<0){vfs_free(d);return backing;}
 long e=RAW(PROXY_SC_ftruncate,backing,sizeof(struct temporary),0,0,0,0);
 if(e){RAW(PROXY_SC_close,backing,0,0,0,0,0);vfs_free(d);return e;}
 long m=RAW(PROXY_SC_mmap,0,sizeof(struct temporary),PROT_READ|PROT_WRITE,MAP_SHARED,backing,0);
 if((unsigned long)m>=(unsigned long)-4095){RAW(PROXY_SC_close,backing,0,0,0,0,0);vfs_free(d);return m;}struct temporary *t=(void *)m;t->backing=backing;t->refs=1;t->exclusive=(flags&O_EXCL)!=0;
 p_copy(t->path,dir,n);if(n && t->path[n-1]!='/')t->path[n++]='/';p_copy(t->path+n,".matonos-tmp-",13);n+=13;n+=number(t->path+n,RAW(PROXY_SC_getpid,0,0,0,0,0,0));t->path[n++]='-';
 for(unsigned attempt=0;attempt<128;attempt++){size_t end=n+number(t->path+n,__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED));t->path[end]=0;
 r=RAW(PROXY_SC_openat,AT_FDCWD,t->path,(flags&~(O_TMPFILE|O_EXCL))|O_CREAT|O_EXCL,mode,0,0);if(r!=-EEXIST)break;}
 if(r<0){RAW(PROXY_SC_close,backing,0,0,0,0,0);RAW(PROXY_SC_munmap,t,sizeof(*t),0,0,0,0);vfs_free(d);return r;}
 *d=(struct descriptor){descriptors,(int)r,t};descriptors=d;return r;
}
/* Proc magic links may name any pid: match the underlying inode against our
 * inherited fd table, rather than trusting the numeric descriptor alone. */
static struct temporary *source_tmp(const char *source){
 struct proxy_stat st;if(arch_stat_at(AT_FDCWD,source,&st,0)<0)return NULL;
 /* Usually the numeric fd is ours. Verify its inode before trusting it;
  * another process may use the same number for an unrelated file. */
 const char *last=source;for(const char *p=source;*p;p++)if(*p=='/')last=p+1;
 unsigned fd=0;int valid=*last!=0;
 for(const char *p=last;valid && *p;p++){
  unsigned digit=(unsigned)(*p-'0');
  if(digit>9 || fd>(2147483647U-digit)/10)valid=0;else fd=fd*10+digit;
 }
 struct descriptor *candidate=valid?find((int)fd):NULL;
 if(candidate){struct proxy_stat own;if(!arch_stat_fd(candidate->fd,&own) && own.st_dev==st.st_dev && own.st_ino==st.st_ino)return candidate->tmp;}
 for(struct descriptor *d=descriptors;d;d=d->next){struct proxy_stat own;if(d!=candidate && !arch_stat_fd(d->fd,&own) && own.st_dev==st.st_dev && own.st_ino==st.st_ino)return d->tmp;}
 return NULL;
}
/* Conservative generic app-tree predicate: a shared parent owned by the
 * real caller, owner-writable/searchable, with no group/other write bits.
 * This is compatibility policy, not a replacement for UID/SELinux checks. */
static int writable_tree(const char *source,const char *target){
 if(source[0]!='/' || target[0]!='/')return 0;
 size_t common=0;for(size_t i=0;source[i] && target[i] && source[i]==target[i];i++)if(source[i]=='/')common=i;
 if(!common || common>=PATH_MAX)return 0;
 char parent[PATH_MAX];p_copy(parent,source,common);parent[common]=0;
 struct proxy_stat st;long uid=RAW(PROXY_SC_geteuid,0,0,0,0,0,0);
 return !arch_stat_at(AT_FDCWD,parent,&st,AT_SYMLINK_NOFOLLOW) && S_ISDIR(st.st_mode) && st.st_uid==(uid_t)uid && (st.st_mode&0300)==0300 && !(st.st_mode&0022) && !RAW(PROXY_SC_faccessat,AT_FDCWD,parent,W_OK|X_OK,0,0,0);
}

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
 struct proxy_stat st;char up[PATH_MAX],fallback[PATH_MAX];int selected=0;
 if(!parent(root,path) || arch_stat_at(AT_FDCWD,root,&st,0)<0 || !private_dir(root,st.st_dev))return 0;
 for(;;){
  if(marked(root))return 1;
  if(!selected){char store[PATH_MAX];struct proxy_stat own;
   p_copy(fallback,root,p_len(root)+1);
   if(catpath(store,root,".v3-inodes") && !arch_stat_at(AT_FDCWD,store,&own,AT_SYMLINK_NOFOLLOW) && S_ISDIR(own.st_mode))selected=1;
  }
  if(!parent(up,root) || p_equal(up,root) || !private_dir(up,st.st_dev))break;
  p_copy(root,up,p_len(up)+1);
 }
 p_copy(root,fallback,p_len(fallback)+1);return 1;
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
/* A store-root rename carries a journal inside the renamed directory.
 * Rewriting absolute targets is idempotent, including after process death. */
static long rewrite_names(const char *dir,const char *oldroot,const char *newroot,const char *store,dev_t dev){
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 char *buf=vfs_alloc(4096),*path=vfs_alloc(PATH_MAX),*target=vfs_alloc(PATH_MAX),*replacement=vfs_alloc(PATH_MAX);long r=0,n;
 if(!buf || !path || !target || !replacement){r=-ENOMEM;goto end;}
 while((n=RAW(PROXY_SC_getdents64,fd,buf,4096,0,0,0))>0){for(long off=0;off<n;){unsigned char *d=(void *)(buf+off);unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n){r=-EIO;goto end;}char *name=(void *)(d+19);off+=len;if(p_equal(name,".") || p_equal(name,"..") || p_equal(name,".v3-inodes"))continue;if(!catpath(path,dir,name)){r=-ENAMETOOLONG;goto end;}struct proxy_stat st;r=arch_stat_at(AT_FDCWD,path,&st,AT_SYMLINK_NOFOLLOW);if(r)goto end;if(S_ISDIR(st.st_mode) && st.st_dev==dev){r=rewrite_names(path,oldroot,newroot,store,dev);if(r)goto end;}else if(S_ISLNK(st.st_mode)){long k=RAW(PROXY_SC_readlinkat,AT_FDCWD,path,target,PATH_MAX-1,0,0);if(k<0){r=k;goto end;}target[k]=0;size_t a=p_len(oldroot),b=p_len(newroot),j=0;while(j<a && target[j]==oldroot[j])j++;if(j==a && target[j]=='/' && links_hidden(target+j+1)){size_t tail=p_len(target+a);if(b+tail>=PATH_MAX){r=-ENAMETOOLONG;goto end;}p_copy(replacement,newroot,b);p_copy(replacement+b,target+a,tail+1);char temp[PATH_MAX];if(!catpath(temp,store,"fix")){r=-ENAMETOOLONG;goto end;}RAW(PROXY_SC_unlinkat,AT_FDCWD,temp,0,0,0,0);r=RAW(PROXY_SC_symlinkat,replacement,AT_FDCWD,temp,0,0,0);if(!r)r=RAW(PROXY_SC_renameat2,AT_FDCWD,temp,AT_FDCWD,path,0,0);if(r)goto end;}}}}
 if(n<0)r=n;
end:RAW(PROXY_SC_close,fd,0,0,0,0,0);vfs_free(buf);vfs_free(path);vfs_free(target);vfs_free(replacement);return r;
}
static long move_recovery(const char *root,const char *store){
 char journal[PATH_MAX];if(!catpath(journal,store,"move"))return -ENAMETOOLONG;long fd=RAW(PROXY_SC_openat,AT_FDCWD,journal,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd==-ENOENT)return 0;if(fd<0)return fd;
 char *data=vfs_alloc(PATH_MAX*2);if(!data){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -ENOMEM;}long n=RAW(PROXY_SC_read,fd,data,PATH_MAX*2-1,0,0,0);RAW(PROXY_SC_close,fd,0,0,0,0,0);long r=0;if(n>0){data[n]=0;size_t k=p_len(data);if(k+1<(size_t)n && !p_equal(root,data)){struct proxy_stat st;r=arch_stat_at(AT_FDCWD,root,&st,0);if(!r)r=rewrite_names(root,data,root,store,st.st_dev);}}else if(n<0)r=n;
 vfs_free(data);if(!r)r=RAW(PROXY_SC_unlinkat,AT_FDCWD,journal,0,0,0,0);return r;
}
struct recount_record {struct recount_record *next;dev_t dev;ino_t ino;};
static struct recount_record *recounted;
static long recover(const char *root,const char *store){
 long moving=move_recovery(root,store);if(moving)return moving;
 moving=finish_intent(store);if(moving && moving!=-EIO)return moving;
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
 if(!writable_tree(source,target) || !tree(root,source))return -EPERM;
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
static long move_root(long nr,proxy_arg_t *a,const char *p,const char *q){
 char store[PATH_MAX],next[PATH_MAX],journal[PATH_MAX];struct proxy_stat st;
 if(!q || !catpath(store,p,".v3-inodes") || arch_stat_at(AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW))return -ENOSYS;
 if(nr==PROXY_SC_renameat2 && (a[4]&RENAME_EXCHANGE))return -ENOTSUP;
 if(!catpath(next,q,".v3-inodes") || !catpath(journal,store,"move"))return -ENAMETOOLONG;
 long lock=store_lock(p,store);if(lock<0)return lock;long r=recover(p,store);if(r)goto end;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,journal,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(fd<0){r=fd;goto end;}char *data=vfs_alloc(PATH_MAX*2);if(!data){RAW(PROXY_SC_close,fd,0,0,0,0,0);r=-ENOMEM;goto end;}size_t pn=p_len(p)+1,qn=p_len(q)+1;p_copy(data,p,pn);p_copy(data+pn,q,qn);r=RAW(PROXY_SC_write,fd,data,pn+qn,0,0,0);vfs_free(data);RAW(PROXY_SC_close,fd,0,0,0,0,0);if(r!=(long)(pn+qn)){r=r<0?r:-EIO;goto end;}
 r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);if(!r)r=recover(q,next);else recover(p,store);
end:unlock(lock);return r;
}
long links_name_call(long nr,proxy_arg_t *a,const char *p,const char *q){
 char root[PATH_MAX],other[PATH_MAX],store[PATH_MAX];
 if(q){long moved=move_root(nr,a,p,q);if(moved!=-ENOSYS)return moved;}
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
static long links_publish_locked(const char *source,const char *target,long flags,int *handled){
 *handled=1;
 if(flags&~AT_SYMLINK_FOLLOW)return -EINVAL;
 struct temporary *t=(flags&AT_SYMLINK_FOLLOW)?source_tmp(source):NULL;
 if(t){
  if(t->exclusive)return -EPERM;
  if(p_len(target)>=sizeof(t->path))return -ENAMETOOLONG;
  int expected=0;if(!__atomic_compare_exchange_n(&t->published,&expected,-1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return expected==1?-EPERM:-EBUSY;
  long r=RAW(PROXY_SC_renameat2,AT_FDCWD,t->path,AT_FDCWD,target,RENAME_NOREPLACE,0);
  if(!r)p_copy(t->path,target,p_len(target)+1);
  __atomic_store_n(&t->published,r?0:1,__ATOMIC_RELEASE);return r;
 }
 *handled=0;return 0;
}

void links_payload_fd_stat(int fd,void *wire){struct proxy_stat s;arch_stat_decode(&s,wire);links_fd_stat(fd,&s);arch_stat_nlink(wire,s.st_nlink);}

#include "state.h"
static int links_private_fd_locked(int fd){for(struct descriptor *d=descriptors;d;d=d->next)if(d->tmp->backing==fd)return 1;return 0;}
static void links_state_locked(struct state *s){
 serial=state_word(s,serial);
 uint64_t count=0;for(struct descriptor *d=descriptors;d;d=d->next)count++;
 count=state_word(s,count);if(count>SIZE_MAX/sizeof(struct descriptor)){s->error=-EOVERFLOW;return;}
 if(!s->reading){for(struct descriptor *d=descriptors;d;d=d->next){state_word(s,d->fd);state_word(s,d->tmp->backing);}return;}
 for(uint64_t i=0;i<count&&!s->error;i++){
  int fd=state_word(s,0),backing=state_word(s,0);struct temporary *tmp=NULL;
  if(!state_live(backing)){s->error=-EBADF;return;}
  for(struct descriptor *d=descriptors;d;d=d->next)if(d->tmp->backing==backing)tmp=d->tmp;
  if(!tmp){long m=RAW(PROXY_SC_mmap,0,sizeof(*tmp),PROT_READ|PROT_WRITE,MAP_SHARED,backing,0);if((unsigned long)m>=(unsigned long)-4095){s->error=m;return;}tmp=(void *)m;}
  struct descriptor *d=vfs_alloc(sizeof(*d));if(!d){s->error=-ENOMEM;return;}*d=(struct descriptor){descriptors,fd,tmp};descriptors=d;
 }
 struct descriptor *d=descriptors;while(d){struct descriptor *next=d->next;if(!state_live(d->fd))links_close_locked(d->fd);d=next;}
}

char *links_fd_name(int fd){proxy_sigmask_t mask=descriptor_lock();char *result=links_fd_name_locked(fd);descriptor_unlock(mask);return result;}

long links_fork(int delta){proxy_sigmask_t mask=descriptor_lock();long r=links_fork_locked(delta);descriptor_unlock(mask);return r;}

void links_exit(void){proxy_sigmask_t mask=descriptor_lock();links_exit_locked();descriptor_unlock(mask);}

long links_open(const char *dir,long flags,long mode){proxy_sigmask_t mask=descriptor_lock();long result=links_open_locked(dir,flags,mode);descriptor_unlock(mask);return result;}

long links_publish(const char *source,const char *target,long flags){proxy_sigmask_t mask=descriptor_lock();int handled;long result=links_publish_locked(source,target,flags,&handled);descriptor_unlock(mask);return handled?result:store_link(source,target);}

int links_private_fd(int fd){proxy_sigmask_t mask=descriptor_lock();int result=links_private_fd_locked(fd);descriptor_unlock(mask);return result;}

void links_state(struct state *s){proxy_sigmask_t mask=descriptor_lock();links_state_locked(s);descriptor_unlock(mask);}

/* Retire tracking before a concurrent tmpfile open can reuse the kernel fd.
 * Locking only links_close() after close(2) can unlink the NEW temp inode. */
long links_close_fd(int fd){
 proxy_sigmask_t mask=descriptor_lock();
 long r=RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(!r)links_close_locked(fd);
 descriptor_unlock(mask);return r;
}
long links_dup_fd(long nr,proxy_arg_t *a){
 proxy_sigmask_t mask=descriptor_lock();
 long r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
 if(r>=0){long e=links_dup_locked((int)a[0],(int)r);
  if(e){RAW(PROXY_SC_close,r,0,0,0,0,0);links_close_locked((int)r);r=e;}}
 descriptor_unlock(mask);return r;
}
