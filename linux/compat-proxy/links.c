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
/* Descriptor lists are process-private (and retained by resident exec).
 * Anonymous shared records carry reference counts and publication across fork.
 * One shared mapping per inode per process; dup aliases do not own new maps. */
struct temporary {
    unsigned refs;
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

static struct descriptor *find(int fd) {
    for(struct descriptor *p=descriptors;p;p=p->next)
        if(p->fd==fd)return p;
    return NULL;
}
char *links_fd_name(int fd) {
    struct descriptor *d=find(fd);
    if(!d || __atomic_load_n(&d->tmp->published,__ATOMIC_ACQUIRE)!=1)return NULL;
    return vfs_reverse(d->tmp->path);
}
void links_close(int fd) {
    struct descriptor **p=&descriptors;
    while(*p){
        struct descriptor *d=*p;
        if(d->fd!=fd){p=&d->next;continue;}
        *p=d->next;
        if(__atomic_sub_fetch(&d->tmp->refs,1,__ATOMIC_ACQ_REL)==0 &&
           !__atomic_load_n(&d->tmp->published,__ATOMIC_ACQUIRE))
            RAW(SYS_unlinkat,AT_FDCWD,d->tmp->path,0,0,0,0);
        int local=0;
        for(struct descriptor *alias=descriptors;alias;alias=alias->next)
            if(alias->tmp==d->tmp)local=1;
        if(!local)RAW(SYS_munmap,d->tmp,sizeof(*d->tmp),0,0,0,0);
        vfs_free(d);
        return;
    }
}
long links_dup(int from,int to) {
    if(from==to)return 0;
    struct descriptor *d=find(from),*copy=NULL;
    if(d){
        copy=vfs_alloc(sizeof(*copy));
        if(!copy)return -ENOMEM;
        *copy=(struct descriptor){NULL,to,d->tmp};
        __atomic_add_fetch(&d->tmp->refs,1,__ATOMIC_RELAXED);
    }
    links_close(to);
    if(copy){copy->next=descriptors;descriptors=copy;}
    return 0;
}
/* Reserve child references BEFORE cloning, so a parent closing immediately
 * cannot delete the child's inode. A failed clone rolls the reservation back. */
void links_fork(int delta) {
    for(struct descriptor *d=descriptors;d;d=d->next){
        if(delta>0)__atomic_add_fetch(&d->tmp->refs,1,__ATOMIC_RELAXED);
        else __atomic_sub_fetch(&d->tmp->refs,1,__ATOMIC_RELAXED);
    }
}
void links_exit(void) {
    while(descriptors)links_close(descriptors->fd);
}
static size_t number(char *out,unsigned long value){char b[24];size_t n=0,k=0;do{b[n++]=(char)('0'+value%10);value/=10;}while(value);while(n)out[k++]=b[--n];return k;}
long links_open(const char *dir,long flags,long mode){
 if(!(flags&O_RDWR) && !(flags&O_WRONLY))return -EINVAL;
 struct stat st;long r=RAW(SYS_newfstatat,AT_FDCWD,dir,&st,0,0,0);if(r<0)return r;if(!S_ISDIR(st.st_mode))return -ENOTDIR;
 size_t n=p_len(dir);if(n+70>=PATH_MAX)return -ENAMETOOLONG;
 struct descriptor *d=vfs_alloc(sizeof(*d));if(!d)return -ENOMEM;
 long m=RAW(SYS_mmap,0,sizeof(struct temporary),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
 if((unsigned long)m>=(unsigned long)-4095){vfs_free(d);return m;}struct temporary *t=(void *)m;t->refs=1;t->exclusive=(flags&O_EXCL)!=0;
 p_copy(t->path,dir,n);if(n && t->path[n-1]!='/')t->path[n++]='/';p_copy(t->path+n,".matonos-tmp-",13);n+=13;n+=number(t->path+n,RAW(SYS_getpid,0,0,0,0,0,0));t->path[n++]='-';
 for(unsigned attempt=0;attempt<128;attempt++){size_t end=n+number(t->path+n,__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED));t->path[end]=0;
 r=RAW(SYS_openat,AT_FDCWD,t->path,(flags&~(O_TMPFILE|O_EXCL))|O_CREAT|O_EXCL,mode,0,0);if(r!=-EEXIST)break;}
 if(r<0){RAW(SYS_munmap,t,sizeof(*t),0,0,0,0);vfs_free(d);return r;}
 *d=(struct descriptor){descriptors,(int)r,t};descriptors=d;return r;
}
/* Proc magic links may name any pid: match the underlying inode against our
 * inherited fd table, rather than trusting the numeric descriptor alone. */
static struct temporary *source_tmp(const char *source){struct stat st;if(RAW(SYS_newfstatat,AT_FDCWD,source,&st,0,0,0)<0)return NULL;for(struct descriptor *d=descriptors;d;d=d->next){struct stat own;if(!RAW(SYS_fstat,d->fd,&own,0,0,0,0) && own.st_dev==st.st_dev && own.st_ino==st.st_ino)return d->tmp;}return NULL;}
/* Conservative generic app-tree predicate: a shared parent owned by the
 * real caller, owner-writable/searchable, with no group/other write bits.
 * This is compatibility policy, not a replacement for UID/SELinux checks. */
static int writable_tree(const char *source,const char *target){
 if(source[0]!='/' || target[0]!='/')return 0;
 size_t common=0;for(size_t i=0;source[i] && target[i] && source[i]==target[i];i++)if(source[i]=='/')common=i;
 if(!common || common>=PATH_MAX)return 0;
 char parent[PATH_MAX];p_copy(parent,source,common);parent[common]=0;
 struct stat st;long uid=RAW(SYS_geteuid,0,0,0,0,0,0);
 return !RAW(SYS_newfstatat,AT_FDCWD,parent,&st,AT_SYMLINK_NOFOLLOW,0,0) && S_ISDIR(st.st_mode) && st.st_uid==uid && (st.st_mode&0300)==0300 && !(st.st_mode&0022) && !RAW(SYS_faccessat,AT_FDCWD,parent,W_OK|X_OK,0,0,0);
}

/* Stores are private, filesystem-local trees. Counts are persistent; recovery
 * recomputes them after a dirty transaction, without any name-count ceiling. */
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
static int private_dir(const char *p,dev_t dev){struct stat st;return !RAW(SYS_newfstatat,AT_FDCWD,p,&st,AT_SYMLINK_NOFOLLOW,0,0) && S_ISDIR(st.st_mode) && st.st_dev==dev && st.st_uid==RAW(SYS_geteuid,0,0,0,0,0,0) && (st.st_mode&0300)==0300 && !(st.st_mode&0022);}
static int tree(char *root,const char *path){struct stat st;char up[PATH_MAX];if(!parent(root,path) || RAW(SYS_newfstatat,AT_FDCWD,root,&st,0,0,0)<0 || !private_dir(root,st.st_dev))return 0;for(;;){char store[PATH_MAX];struct stat own;if(catpath(store,root,".v3-inodes") && !RAW(SYS_newfstatat,AT_FDCWD,store,&own,AT_SYMLINK_NOFOLLOW,0,0) && S_ISDIR(own.st_mode))break;if(!parent(up,root) || p_equal(up,root) || !private_dir(up,st.st_dev))break;p_copy(root,up,p_len(up)+1);}return 1;}
int links_target(const char *name,char *out){long n=RAW(SYS_readlinkat,AT_FDCWD,name,out,PATH_MAX-1,0,0);if(n<=0)return 0;out[n]=0;if(out[0]!='/' || !component(out))return 0;struct stat st;return !RAW(SYS_newfstatat,AT_FDCWD,out,&st,AT_SYMLINK_NOFOLLOW,0,0) && S_ISREG(st.st_mode);}
static long store_lock(const char *root,char *store){if(!catpath(store,root,".v3-inodes"))return -ENAMETOOLONG;long r=RAW(SYS_mkdirat,AT_FDCWD,store,0700,0,0,0);if(r && r!=-EEXIST)return r;struct stat st;if(RAW(SYS_newfstatat,AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW,0,0) || !S_ISDIR(st.st_mode) || st.st_uid!=RAW(SYS_geteuid,0,0,0,0,0,0) || (st.st_mode&0077))return -EPERM;char file[PATH_MAX];if(!catpath(file,store,"lock"))return -ENAMETOOLONG;long fd=RAW(SYS_openat,AT_FDCWD,file,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(fd<0)return fd;do{r=RAW(SYS_flock,fd,LOCK_EX,0,0,0,0);}while(r==-EINTR);if(r){RAW(SYS_close,fd,0,0,0,0,0);return r;}struct stat own,named;if(RAW(SYS_fstat,fd,&own,0,0,0,0) || RAW(SYS_newfstatat,AT_FDCWD,file,&named,AT_SYMLINK_NOFOLLOW,0,0) || own.st_ino!=named.st_ino || own.st_dev!=named.st_dev){RAW(SYS_close,fd,0,0,0,0,0);return store_lock(root,store);}return fd;}
static void unlock(long fd){RAW(SYS_flock,fd,LOCK_UN,0,0,0,0);RAW(SYS_close,fd,0,0,0,0,0);}
static long count_names(const char *dir,const char *object,dev_t dev,unsigned long *count){
 long fd=RAW(SYS_openat,AT_FDCWD,dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 char buf[4096];long r=0,n;
 while((n=RAW(SYS_getdents64,fd,buf,sizeof(buf),0,0,0))>0){for(long off=0;off<n;){unsigned char *d=(void *)(buf+off);unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n){r=-EIO;goto end;}const char *name=(void *)(d+19);off+=len;if(p_equal(name,".") || p_equal(name,"..") || p_equal(name,".v3-inodes"))continue;char path[PATH_MAX],target[PATH_MAX];if(!catpath(path,dir,name)){r=-ENAMETOOLONG;goto end;}struct stat st;r=RAW(SYS_newfstatat,AT_FDCWD,path,&st,AT_SYMLINK_NOFOLLOW,0,0);if(r==-ENOENT){r=0;continue;}if(r)goto end;if(S_ISLNK(st.st_mode)){long k=RAW(SYS_readlinkat,AT_FDCWD,path,target,sizeof(target)-1,0,0);if(k>0){target[k]=0;if(p_equal(target,object))(*count)++;}}else if(S_ISDIR(st.st_mode) && st.st_dev==dev){r=count_names(path,object,dev,count);if(r)goto end;}}}
 if(n<0)r=n;
end:RAW(SYS_close,fd,0,0,0,0,0);return r;
}
static long write_count(const char *object,unsigned long n){char meta[PATH_MAX];size_t k=p_len(object);if(k+7>=PATH_MAX)return -ENAMETOOLONG;p_copy(meta,object,k);p_copy(meta+k,".count",7);long fd=RAW(SYS_openat,AT_FDCWD,meta,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(fd<0)return fd;long r=RAW(SYS_write,fd,&n,sizeof(n),0,0,0);RAW(SYS_close,fd,0,0,0,0,0);return r==sizeof(n)?0:r<0?r:-EIO;}
static unsigned long read_count(const char *object){char meta[PATH_MAX];size_t k=p_len(object);unsigned long n=0;if(k+7>=PATH_MAX)return n;p_copy(meta,object,k);p_copy(meta+k,".count",7);long fd=RAW(SYS_openat,AT_FDCWD,meta,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd>=0){RAW(SYS_read,fd,&n,sizeof(n),0,0,0);RAW(SYS_close,fd,0,0,0,0,0);}return n;}
static long dirty(const char *store){char path[PATH_MAX];if(!catpath(path,store,"dirty"))return -ENAMETOOLONG;long fd=RAW(SYS_openat,AT_FDCWD,path,O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(fd<0)return fd;RAW(SYS_close,fd,0,0,0,0,0);return 0;}
/* A store-root rename carries a journal inside the renamed directory.
 * Rewriting absolute targets is idempotent, including after process death. */
static long rewrite_names(const char *dir,const char *oldroot,const char *newroot,const char *store,dev_t dev){
 long fd=RAW(SYS_openat,AT_FDCWD,dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 char *buf=vfs_alloc(4096),*path=vfs_alloc(PATH_MAX),*target=vfs_alloc(PATH_MAX),*replacement=vfs_alloc(PATH_MAX);long r=0,n;
 if(!buf || !path || !target || !replacement){r=-ENOMEM;goto end;}
 while((n=RAW(SYS_getdents64,fd,buf,4096,0,0,0))>0){for(long off=0;off<n;){unsigned char *d=(void *)(buf+off);unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n){r=-EIO;goto end;}char *name=(void *)(d+19);off+=len;if(p_equal(name,".") || p_equal(name,"..") || p_equal(name,".v3-inodes"))continue;if(!catpath(path,dir,name)){r=-ENAMETOOLONG;goto end;}struct stat st;r=RAW(SYS_newfstatat,AT_FDCWD,path,&st,AT_SYMLINK_NOFOLLOW,0,0);if(r)goto end;if(S_ISDIR(st.st_mode) && st.st_dev==dev){r=rewrite_names(path,oldroot,newroot,store,dev);if(r)goto end;}else if(S_ISLNK(st.st_mode)){long k=RAW(SYS_readlinkat,AT_FDCWD,path,target,PATH_MAX-1,0,0);if(k<0){r=k;goto end;}target[k]=0;size_t a=p_len(oldroot),b=p_len(newroot),j=0;while(j<a && target[j]==oldroot[j])j++;if(j==a && target[j]=='/' && links_hidden(target+j+1)){size_t tail=p_len(target+a);if(b+tail>=PATH_MAX){r=-ENAMETOOLONG;goto end;}p_copy(replacement,newroot,b);p_copy(replacement+b,target+a,tail+1);char temp[PATH_MAX];if(!catpath(temp,store,"fix")){r=-ENAMETOOLONG;goto end;}RAW(SYS_unlinkat,AT_FDCWD,temp,0,0,0,0);r=RAW(SYS_symlinkat,replacement,AT_FDCWD,temp,0,0,0);if(!r)r=RAW(SYS_renameat2,AT_FDCWD,temp,AT_FDCWD,path,0,0);if(r)goto end;}}}}
 if(n<0)r=n;
end:RAW(SYS_close,fd,0,0,0,0,0);vfs_free(buf);vfs_free(path);vfs_free(target);vfs_free(replacement);return r;
}
static long move_recovery(const char *root,const char *store){
 char journal[PATH_MAX];if(!catpath(journal,store,"move"))return -ENAMETOOLONG;long fd=RAW(SYS_openat,AT_FDCWD,journal,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd==-ENOENT)return 0;if(fd<0)return fd;
 char *data=vfs_alloc(PATH_MAX*2);if(!data){RAW(SYS_close,fd,0,0,0,0,0);return -ENOMEM;}long n=RAW(SYS_read,fd,data,PATH_MAX*2-1,0,0,0);RAW(SYS_close,fd,0,0,0,0,0);long r=0;if(n>0){data[n]=0;size_t k=p_len(data);if(k+1<(size_t)n && !p_equal(root,data)){struct stat st;r=RAW(SYS_newfstatat,AT_FDCWD,root,&st,0,0,0);if(!r)r=rewrite_names(root,data,root,store,st.st_dev);}}else if(n<0)r=n;
 vfs_free(data);if(!r)r=RAW(SYS_unlinkat,AT_FDCWD,journal,0,0,0,0);return r;
}
static long recover(const char *root,const char *store){
 long moving=move_recovery(root,store);if(moving)return moving;
 char marker[PATH_MAX];struct stat markerstat;if(!catpath(marker,store,"dirty"))return -ENAMETOOLONG;int changed=!RAW(SYS_newfstatat,AT_FDCWD,marker,&markerstat,AT_SYMLINK_NOFOLLOW,0,0);
 /* An interrupted first conversion restores its original name before GC. */
 char journal[PATH_MAX];if(!catpath(journal,store,"conversion"))return -ENAMETOOLONG;
 long j=RAW(SYS_openat,AT_FDCWD,journal,O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);
 if(j>=0){changed=1;char data[PATH_MAX*2];long n=RAW(SYS_read,j,data,sizeof(data)-1,0,0,0);RAW(SYS_close,j,0,0,0,0,0);if(n>0){data[n]=0;size_t k=p_len(data);if(k+1<(size_t)n){struct stat st;if(RAW(SYS_newfstatat,AT_FDCWD,data,&st,AT_SYMLINK_NOFOLLOW,0,0)==-ENOENT)RAW(SYS_renameat2,AT_FDCWD,data+k+1,AT_FDCWD,data,RENAME_NOREPLACE,0);}}RAW(SYS_unlinkat,AT_FDCWD,journal,0,0,0,0);}
 struct stat st;long r=RAW(SYS_newfstatat,AT_FDCWD,root,&st,0,0,0);if(r)return r;
 long fd=RAW(SYS_openat,AT_FDCWD,store,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(fd<0)return fd;char buf[4096];long n;
 while((n=RAW(SYS_getdents64,fd,buf,sizeof(buf),0,0,0))>0){for(long off=0;off<n;){unsigned char *d=(void *)(buf+off);unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n){r=-EIO;goto end;}char *name=(void *)(d+19);off+=len;if(name[0]!='i' || name[1]!='-')continue;size_t k=p_len(name);if(k>6 && p_equal(name+k-6,".count")){char meta[PATH_MAX],backing[PATH_MAX];struct stat bst;if(!catpath(meta,store,name)){r=-ENAMETOOLONG;goto end;}name[k-6]=0;int fits=catpath(backing,store,name);name[k-6]='.';if(!fits){r=-ENAMETOOLONG;goto end;}if(RAW(SYS_newfstatat,AT_FDCWD,backing,&bst,AT_SYMLINK_NOFOLLOW,0,0)==-ENOENT)RAW(SYS_unlinkat,AT_FDCWD,meta,0,0,0,0);continue;}char object[PATH_MAX];if(!catpath(object,store,name)){r=-ENAMETOOLONG;goto end;}char meta[PATH_MAX];size_t olen=p_len(object);if(olen+7>=PATH_MAX){r=-ENAMETOOLONG;goto end;}p_copy(meta,object,olen);p_copy(meta+olen,".count",7);struct stat mst;if(!changed && !RAW(SYS_newfstatat,AT_FDCWD,meta,&mst,AT_SYMLINK_NOFOLLOW,0,0))continue;
 unsigned long count=0;r=count_names(root,object,st.st_dev,&count);if(r)goto end;if(count)r=write_count(object,count);else{r=RAW(SYS_unlinkat,AT_FDCWD,object,0,0,0,0);if(r==-ENOENT)r=0;char meta[PATH_MAX];if(p_len(object)+7<PATH_MAX){p_copy(meta,object,p_len(object));p_copy(meta+p_len(object),".count",7);RAW(SYS_unlinkat,AT_FDCWD,meta,0,0,0,0);}}if(r)goto end;}}
 if(n<0)r=n;
end:RAW(SYS_close,fd,0,0,0,0,0);if(!r)RAW(SYS_unlinkat,AT_FDCWD,marker,0,0,0,0);return r;
}
static void remove_empty_store(const char *dir){
 char store[PATH_MAX],file[PATH_MAX];struct stat st;
 if(!catpath(store,dir,".v3-inodes") || RAW(SYS_newfstatat,AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW,0,0))return;
 long lock=store_lock(dir,store);if(lock<0)return;
 long fd=RAW(SYS_openat,AT_FDCWD,store,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);int empty=fd>=0;char buf[4096];long n;
 if(fd>=0){while((n=RAW(SYS_getdents64,fd,buf,sizeof(buf),0,0,0))>0){for(long off=0;off<n;){unsigned char *d=(void *)(buf+off);unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n){empty=0;break;}char *name=(void *)(d+19);if(!p_equal(name,".") && !p_equal(name,"..") && !p_equal(name,"lock"))empty=0;off+=len;}}if(n<0)empty=0;RAW(SYS_close,fd,0,0,0,0,0);}
 if(empty && catpath(file,store,"lock")){RAW(SYS_unlinkat,AT_FDCWD,file,0,0,0,0);RAW(SYS_unlinkat,AT_FDCWD,store,AT_REMOVEDIR,0,0,0);}
 unlock(lock);
}
static long store_link(const char *source,const char *target){
 char root[PATH_MAX],store[PATH_MAX],object[PATH_MAX],destparent[PATH_MAX];struct stat src,dst;
 char sourceparent[PATH_MAX];if(!parent(sourceparent,source) || !parent(destparent,target))return -ENAMETOOLONG;
 long r=RAW(SYS_newfstatat,AT_FDCWD,sourceparent,&src,0,0,0);if(r)return r;
 r=RAW(SYS_newfstatat,AT_FDCWD,destparent,&dst,0,0,0);if(r)return r;if(src.st_dev!=dst.st_dev)return -EXDEV;
 if(!writable_tree(source,target) || !tree(root,source))return -EPERM;
 long lock=store_lock(root,store);if(lock<0)return lock;r=recover(root,store);if(r)goto end;
 r=dirty(store);if(r)goto end;
 r=RAW(SYS_newfstatat,AT_FDCWD,source,&src,AT_SYMLINK_NOFOLLOW,0,0);if(r)goto end;if(S_ISLNK(src.st_mode) && !links_target(source,object)){r=-EPERM;goto end;}
 struct stat exists;if(!RAW(SYS_newfstatat,AT_FDCWD,target,&exists,AT_SYMLINK_NOFOLLOW,0,0)){r=-EEXIST;goto end;}
 if(!links_target(source,object)){
  r=RAW(SYS_newfstatat,AT_FDCWD,source,&src,AT_SYMLINK_NOFOLLOW,0,0);if(r)goto end;if(!S_ISREG(src.st_mode) || src.st_uid!=RAW(SYS_geteuid,0,0,0,0,0,0)){r=-EPERM;goto end;}
  char id[35]="i-";unsigned char nonce[16];long random=RAW(SYS_getrandom,nonce,sizeof(nonce),0,0,0,0);if(random!=sizeof(nonce)){r=random<0?random:-EIO;goto end;}const char hex[]="0123456789abcdef";for(size_t k=0;k<sizeof(nonce);k++){id[2+2*k]=hex[nonce[k]>>4];id[3+2*k]=hex[nonce[k]&15];}id[34]=0;if(!catpath(object,store,id)){r=-ENAMETOOLONG;goto end;}
  char journal[PATH_MAX];if(!catpath(journal,store,"conversion")){r=-ENAMETOOLONG;goto end;}long j=RAW(SYS_openat,AT_FDCWD,journal,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(j<0){r=j;goto end;}char data[PATH_MAX*2];size_t sn=p_len(source)+1,on=p_len(object)+1;p_copy(data,source,sn);p_copy(data+sn,object,on);r=RAW(SYS_write,j,data,sn+on,0,0,0);RAW(SYS_close,j,0,0,0,0,0);if(r!=(long)(sn+on)){r=r<0?r:-EIO;goto end;}
  r=RAW(SYS_renameat2,AT_FDCWD,source,AT_FDCWD,object,RENAME_NOREPLACE,0);if(r)goto end;
  r=RAW(SYS_symlinkat,object,AT_FDCWD,source,0,0,0);if(r){RAW(SYS_renameat2,AT_FDCWD,object,AT_FDCWD,source,RENAME_NOREPLACE,0);goto end;}RAW(SYS_unlinkat,AT_FDCWD,journal,0,0,0,0);
 }
 r=RAW(SYS_symlinkat,object,AT_FDCWD,target,0,0,0);{long clean=recover(root,store);if(!r)r=clean;}
end:unlock(lock);return r;
}
static long move_root(long nr,long *a,const char *p,const char *q){
 char store[PATH_MAX],next[PATH_MAX],journal[PATH_MAX];struct stat st;
 if(!q || !catpath(store,p,".v3-inodes") || RAW(SYS_newfstatat,AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW,0,0))return -ENOSYS;
 if(nr==SYS_renameat2 && (a[4]&RENAME_EXCHANGE))return -ENOTSUP;
 if(!catpath(next,q,".v3-inodes") || !catpath(journal,store,"move"))return -ENAMETOOLONG;
 long lock=store_lock(p,store);if(lock<0)return lock;long r=recover(p,store);if(r)goto end;r=dirty(store);if(r)goto end;
 long fd=RAW(SYS_openat,AT_FDCWD,journal,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600,0,0);if(fd<0){r=fd;goto end;}char *data=vfs_alloc(PATH_MAX*2);if(!data){RAW(SYS_close,fd,0,0,0,0,0);r=-ENOMEM;goto end;}size_t pn=p_len(p)+1,qn=p_len(q)+1;p_copy(data,p,pn);p_copy(data+pn,q,qn);r=RAW(SYS_write,fd,data,pn+qn,0,0,0);vfs_free(data);RAW(SYS_close,fd,0,0,0,0,0);if(r!=(long)(pn+qn)){r=r<0?r:-EIO;goto end;}
 r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);if(!r)r=recover(q,next);else recover(p,store);
end:unlock(lock);return r;
}
long links_name_call(long nr,long *a,const char *p,const char *q){
 char root[PATH_MAX],other[PATH_MAX],store[PATH_MAX];
 if(q){long moved=move_root(nr,a,p,q);if(moved!=-ENOSYS)return moved;}
 if(q && (nr==SYS_renameat || (nr==SYS_renameat2 && !(a[4]&RENAME_NOREPLACE)))){char left[PATH_MAX],right[PATH_MAX];if(links_target(p,left) && links_target(q,right) && p_equal(left,right))return 0;}
 if(nr==SYS_unlinkat && (a[2]&AT_REMOVEDIR))remove_empty_store(p);
 if(!tree(root,p))return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
 if(q && (!tree(other,q) || !p_equal(root,other)))return -EXDEV;
 /* Only existing stores require serialization; ordinary files retain kernel behavior. */
 if(!catpath(store,root,".v3-inodes"))return -ENAMETOOLONG;
 struct stat st;if(RAW(SYS_newfstatat,AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW,0,0))return RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
 long lock=store_lock(root,store);if(lock<0)return lock;long r=recover(root,store);if(!r)r=dirty(store);if(!r){r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);if(!r){long clean=recover(root,store);if(clean)r=clean;}}unlock(lock);return r;
}
long links_path_call(long nr,long *args,const char *p){
 char root[PATH_MAX],store[PATH_MAX],object[PATH_MAX];struct stat st;long lock=-1,r;
 if(tree(root,p) && catpath(store,root,".v3-inodes") && !RAW(SYS_newfstatat,AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW,0,0)){
  lock=store_lock(root,store);if(lock<0)return lock;r=recover(root,store);if(r){unlock(lock);return r;}
 }
 long a[6];p_copy(a,args,sizeof(a));int stored=links_target(p,object);
 if(stored){if(nr==SYS_readlinkat){r=-EINVAL;goto end;}a[1]=(long)object;
  if(nr==SYS_openat)a[2]&=~O_NOFOLLOW;
  if(nr==SYS_newfstatat || nr==SYS_utimensat || nr==SYS_faccessat2)a[3]&=~AT_SYMLINK_NOFOLLOW;
  if(nr==SYS_fchownat)a[4]&=~AT_SYMLINK_NOFOLLOW;
  if(nr==SYS_statx)a[2]&=~AT_SYMLINK_NOFOLLOW;
 }
 r=RAW(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
 if(!r && stored && nr==SYS_newfstatat)((struct stat *)a[2])->st_nlink=read_count(object);
 if(!r && stored && nr==SYS_statx)((struct statx *)a[4])->stx_nlink=read_count(object);
end:if(lock>=0)unlock(lock);return r;
}
static void refresh(const char *object){char root[PATH_MAX],store[PATH_MAX];size_t i=0;for(;object[i];i++){const char marker[]="/.v3-inodes/";size_t j=0;while(marker[j] && object[i+j]==marker[j])j++;if(!marker[j])break;}if(!object[i] || i>=PATH_MAX)return;p_copy(root,object,i);root[i]=0;long lock=store_lock(root,store);if(lock>=0){recover(root,store);unlock(lock);}}
void links_recover_path(const char *path){char root[PATH_MAX],store[PATH_MAX];struct stat st;if(!tree(root,path) || !catpath(store,root,".v3-inodes") || RAW(SYS_newfstatat,AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW,0,0))return;long lock=store_lock(root,store);if(lock>=0){recover(root,store);unlock(lock);}}
void links_fd_stat(int fd,struct stat *st){char proc[80]="/proc/self/fd/",path[PATH_MAX];size_t k=14;k+=number(proc+k,(unsigned)fd);proc[k]=0;long n=RAW(SYS_readlinkat,AT_FDCWD,proc,path,sizeof(path)-1,0,0);if(n>0){path[n]=0;links_stat(path,st);}}
void links_stat(const char *object,struct stat *st){if(component(object) && S_ISREG(st->st_mode)){refresh(object);st->st_nlink=read_count(object);}}
void links_statx(const char *object,struct statx *st){if(component(object) && S_ISREG(st->stx_mode)){refresh(object);st->stx_nlink=read_count(object);}}
static long dents_locked(long fd,void *buf,long size){
 long n;do{n=RAW(SYS_getdents64,fd,buf,size,0,0,0);if(n<=0)return n;long used=0;for(long off=0;off<n;){unsigned char *d=(unsigned char *)buf+off;unsigned short len=*(unsigned short *)(d+16);if(len<20 || off+len>n)return -EIO;char *name=(void *)(d+19);if(!p_equal(name,".v3-inodes")){char target[PATH_MAX];long k=RAW(SYS_readlinkat,fd,name,target,sizeof(target)-1,0,0);if(k>0){target[k]=0;if(component(target))d[18]=8;}for(unsigned short i=0;i<len;i++)((unsigned char *)buf)[used+i]=d[i];used+=len;}off+=len;}if(used)return used;}while(n>0);return n;
}
long links_dents(long fd,void *buf,long size){
 char proc[80]="/proc/self/fd/",path[PATH_MAX],root[PATH_MAX],store[PATH_MAX];size_t k=14;k+=number(proc+k,(unsigned)fd);proc[k]=0;long lock=-1,n=RAW(SYS_readlinkat,AT_FDCWD,proc,path,sizeof(path)-3,0,0);struct stat st;
 if(n>0){path[n++]='/';path[n++]='.';path[n]=0;if(tree(root,path) && catpath(store,root,".v3-inodes") && !RAW(SYS_newfstatat,AT_FDCWD,store,&st,AT_SYMLINK_NOFOLLOW,0,0)){lock=store_lock(root,store);if(lock<0)return lock;long r=recover(root,store);if(r){unlock(lock);return r;}}}
 long r=dents_locked(fd,buf,size);if(lock>=0)unlock(lock);return r;
}
long links_publish(const char *source,const char *target,long flags){
 if(flags&~AT_SYMLINK_FOLLOW)return -EINVAL;
 struct temporary *t=(flags&AT_SYMLINK_FOLLOW)?source_tmp(source):NULL;
 if(t){
  if(t->exclusive)return -EPERM;
  if(p_len(target)>=sizeof(t->path))return -ENAMETOOLONG;
  int expected=0;if(!__atomic_compare_exchange_n(&t->published,&expected,-1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return expected==1?-EPERM:-EBUSY;
  long r=RAW(SYS_renameat2,AT_FDCWD,t->path,AT_FDCWD,target,RENAME_NOREPLACE,0);
  if(!r)p_copy(t->path,target,p_len(target)+1);
  __atomic_store_n(&t->published,r?0:1,__ATOMIC_RELEASE);return r;
 }
 return store_link(source,target);
}
