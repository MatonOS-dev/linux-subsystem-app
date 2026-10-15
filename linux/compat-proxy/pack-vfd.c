/* SPDX-License-Identifier: Apache-2.0
 * Included by vfs.c: immutable APK slices backed by real kernel descriptors.
 * Mount setup is trusted launcher input, never a payload mount syscall. */
#include <sys/uio.h>
#include <limits.h>
#include <unistd.h>
#include <linux/stat.h>
static int pack_cmp(const void *a,const void *b,size_t n){const unsigned char *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return x[i]-y[i];return 0;}
static int pack_strcmp(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
static char *pack_chr(const char *s,int c){do{if(*s==c)return (char *)s;}while(*s++);return NULL;}
#define MTP_FREESTANDING
#define strlen p_len
#define strcmp pack_strcmp
#define memcmp pack_cmp
#define strchr pack_chr
#include "../packs/reader.c"
#undef strlen
#undef strcmp
#undef memcmp
#undef strchr
struct pack_file {struct pack_file *next;char *name;int fd;};
struct pack_mount {struct pack_mount *next;char *target;mtp_index *index;void *blob;size_t size;struct pack_file *files;int directory;};
struct pack_fd {struct pack_fd *next;int fd;struct pack_mount *mount;mtp_object object;};
static struct pack_mount *pack_mounts;
static struct pack_fd *pack_fds;
static void *pack_alloc(void *c,size_t n){(void)c;return vfs_alloc(n);}
static void pack_free(void *c,void *p){(void)c;vfs_free(p);}
static struct pack_fd *pack_find(int fd){for(struct pack_fd *f=pack_fds;f;f=f->next)if(f->fd==fd)return f;return NULL;}
int pack_private(int fd){for(struct pack_mount *m=pack_mounts;m;m=m->next){if(m->directory==fd)return 1;for(struct pack_file *p=m->files;p;p=p->next)if(p->fd==fd)return 1;}return 0;}
static struct pack_mount *pack_lookup(const char *path){struct pack_mount *best=NULL;for(struct pack_mount *m=pack_mounts;m;m=m->next)if(prefix(path,m->target)&&(!best||p_len(m->target)>p_len(best->target)))best=m;return best;}
static int pack_object(struct pack_mount *m,const char *path,mtp_object *o){const char *key=path+p_len(m->target);return mtp_lookup(m->index,*key?key:"/",o)?-ENOENT:0;}
static uint64_t pack_ino(const mtp_object *o){uint64_t v=1469598103934665603ULL;for(const char *p=o->checksum;*p;p++)v=(v^(unsigned char)*p)*1099511628211ULL;return v;}
static void pack_metadata(const mtp_object *o,void *wire,int extended){
 if(extended){struct statx *s=wire;*s=(struct statx){0};s->stx_mask=STATX_BASIC_STATS;s->stx_mode=o->mode;s->stx_uid=o->uid;s->stx_gid=o->gid;s->stx_size=p_equal(o->kind,"symlink")?p_len(o->target):o->length;s->stx_ino=pack_ino(o);s->stx_nlink=1;s->stx_blksize=4096;}
 else {struct stat *s=wire;*s=(struct stat){0};s->st_mode=o->mode;s->st_uid=o->uid;s->st_gid=o->gid;s->st_size=p_equal(o->kind,"symlink")?p_len(o->target):o->length;s->st_ino=pack_ino(o);s->st_nlink=1;s->st_blksize=4096;}
}
int pack_stat(const char *path,struct proxy_stat *st){struct pack_mount *m=pack_lookup(path);if(!m)return -ENOSYS;mtp_object o;int r=pack_object(m,path,&o);if(!r)*st=(struct proxy_stat){.st_mode=o.mode,.st_uid=o.uid,.st_size=o.length,.st_ino=pack_ino(&o),.st_nlink=1};return r;}
char *pack_link(const char *path,long *error,int *handled){struct pack_mount *m=pack_lookup(path);*handled=m!=NULL;if(!m)return NULL;mtp_object o;*error=pack_object(m,path,&o);if(*error)return NULL;if(!p_equal(o.kind,"symlink")){*error=-EINVAL;return NULL;}return dupstr(o.target);}
long pack_mount(const char *target,const char *index,const char *directory){
 if(!target||target[0]!='/')return -EINVAL;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,index,O_RDONLY|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 struct proxy_stat st;long e=arch_stat_fd(fd,&st);if(e||st.st_size<12||(uint64_t)st.st_size>SIZE_MAX){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -EINVAL;}
 struct pack_mount *m=vfs_alloc(sizeof(*m));if(!m){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -ENOMEM;}*m=(struct pack_mount){.directory=-1};m->size=st.st_size;m->blob=vfs_alloc(m->size);m->target=global_target(target,&e);
 if(!m->blob||!m->target)e=-ENOMEM;
 if(!e && RAW(PROXY_SC_pread64,fd,m->blob,m->size,0,0,0)!=(long)m->size)e=-EIO;
 RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(!e && mtp_index_open_buffer(m->blob,m->size,&m->index,pack_alloc,pack_free,NULL))e=-EINVAL;
 if(!e){m->directory=RAW(PROXY_SC_openat,AT_FDCWD,"/",O_RDONLY|O_DIRECTORY,0,0,0);if(m->directory<0)e=m->directory;}
 for(size_t i=0;!e&&i<mtp_index_count(m->index);i++){
  mtp_object *o=&m->index->items[i];if(!p_equal(o->kind,"file"))continue;
  if(o->offset>INT64_MAX||o->length>INT64_MAX-o->offset){e=-EOVERFLOW;break;}
  struct pack_file *p=m->files;while(p&&!p_equal(p->name,o->pack))p=p->next;
  if(!p){char *path=join(directory,o->pack);if(!path){e=-ENOMEM;break;}fd=RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY,0,0,0);vfs_free(path);if(fd<0){e=fd;break;}p=vfs_alloc(sizeof(*p));if(!p){RAW(PROXY_SC_close,fd,0,0,0,0,0);e=-ENOMEM;break;}*p=(struct pack_file){m->files,dupstr(o->pack),fd};m->files=p;}
  if(arch_stat_fd(p->fd,&st)||(uint64_t)st.st_size<o->offset+o->length)e=-EINVAL;
 }
 if(e){while(m->files){struct pack_file *p=m->files;m->files=p->next;RAW(PROXY_SC_close,p->fd,0,0,0,0,0);vfs_free(p->name);vfs_free(p);}if(m->directory>=0)RAW(PROXY_SC_close,m->directory,0,0,0,0,0);mtp_index_close(m->index);vfs_free(m->blob);vfs_free(m->target);vfs_free(m);return e;}
 m->next=pack_mounts;pack_mounts=m;return 0;
}
void pack_close(int fd){struct pack_fd **p=&pack_fds;while(*p&&(*p)->fd!=fd)p=&(*p)->next;if(*p){struct pack_fd *old=*p;*p=old->next;vfs_free(old);}}
void pack_dup(int from,int to){if(from==to)return;struct pack_fd *f=pack_find(from);struct pack_fd copy;if(f)copy=*f;pack_close(to);if(f){f=vfs_alloc(sizeof(*f));if(!f)return;*f=copy;f->fd=to;f->next=pack_fds;pack_fds=f;}}
long pack_open(const char *path,int flags){
 struct pack_mount *m=pack_lookup(path);if(!m)return -ENOSYS;
 if(flags&(O_WRONLY|O_RDWR|O_CREAT|O_TRUNC) || (flags&O_TMPFILE)==O_TMPFILE)return -EROFS;
 mtp_object o;long e=pack_object(m,path,&o);if(e)return e;
 if(p_equal(o.kind,"symlink"))return -ELOOP;
 int dir=p_equal(o.kind,"dir");if((flags&O_DIRECTORY)&&!dir)return -ENOTDIR;
 int source=m->directory;if(!dir){struct pack_file *p=m->files;while(p&&!p_equal(p->name,o.pack))p=p->next;if(!p)return -EIO;source=p->fd;}
 long fd=RAW(PROXY_SC_fcntl,source,(flags&O_CLOEXEC)?F_DUPFD_CLOEXEC:F_DUPFD,0,0,0,0);if(fd<0)return fd;
 e=RAW(PROXY_SC_lseek,fd,dir?0:o.offset,SEEK_SET,0,0,0);
 struct pack_fd *f=e<0?NULL:vfs_alloc(sizeof(*f));if(!f){RAW(PROXY_SC_close,fd,0,0,0,0,0);return e<0?e:-ENOMEM;}
 *f=(struct pack_fd){pack_fds,fd,m,o};pack_fds=f;return fd;
}
long pack_pread(int fd,void *buf,size_t size,int64_t off){struct pack_fd *f=pack_find(fd);if(!f)return RAW(PROXY_SC_pread64,fd,buf,size,off,0,0);if(off<0)return -EINVAL;if(!p_equal(f->object.kind,"file"))return -EISDIR;uint64_t left=(uint64_t)off>=f->object.length?0:f->object.length-off;if(size>left)size=left;return RAW(PROXY_SC_pread64,fd,buf,size,f->object.offset+off,0,0);}
long pack_path(long nr,proxy_arg_t *a,const char *path,int *handled){
 struct pack_mount *m=pack_lookup(path);*handled=m!=NULL;if(!m)return 0;
 switch(nr){case PROXY_SC_openat:return pack_open(path,a[2]);
 case PROXY_SC_newfstatat:case PROXY_SC_statx:{mtp_object o;long e=pack_object(m,path,&o);if(!e)pack_metadata(&o,(void *)(uintptr_t)a[nr==PROXY_SC_statx?4:2],nr==PROXY_SC_statx);return e;}
 case PROXY_SC_readlinkat:{mtp_object o;long e=pack_object(m,path,&o);if(e)return e;if(!p_equal(o.kind,"symlink"))return -EINVAL;size_t n=p_len(o.target);if(n>(size_t)a[3])n=a[3];p_copy((void *)(uintptr_t)a[2],o.target,n);return n;}
 case PROXY_SC_faccessat:case PROXY_SC_faccessat2:{mtp_object o;long e=pack_object(m,path,&o);if(e)return e;if(a[2]&W_OK)return -EROFS;if((a[2]&X_OK)&&!(o.mode&0111))return -EACCES;return 0;}
 default:return -EROFS;
 }
}
long pack_call(long nr,proxy_arg_t *a,int *handled){
 int fd=nr==PROXY_SC_mmap?a[4]:a[0];struct pack_fd *f=pack_find(fd);*handled=0;if(!f)return 0;
 uint64_t start=f->object.offset,len=f->object.length;int dir=p_equal(f->object.kind,"dir");long r;*handled=1;
 switch(nr){
 case PROXY_SC_fstat:pack_metadata(&f->object,(void *)(uintptr_t)a[1],0);return 0;
 case PROXY_SC_newfstatat:case PROXY_SC_statx:if(a[1]&&!*(char *)(uintptr_t)a[1]&&(a[nr==PROXY_SC_statx?2:3]&AT_EMPTY_PATH)){pack_metadata(&f->object,(void *)(uintptr_t)a[nr==PROXY_SC_statx?4:2],nr==PROXY_SC_statx);return 0;}break;
 case PROXY_SC_lseek:{int64_t off=a[1],base=0;if(a[2]==SEEK_CUR){r=RAW(PROXY_SC_lseek,fd,0,SEEK_CUR,0,0,0);if(r<0)return r;base=r-start;}else if(a[2]==SEEK_END)base=len;else if(a[2]!=SEEK_SET)return -EINVAL;if((off>0&&base>INT64_MAX-off)||(off<0&&base<INT64_MIN-off))return -EOVERFLOW;off+=base;if(off<0)return -EINVAL;if((uint64_t)off>INT64_MAX-start)return -EOVERFLOW;r=RAW(PROXY_SC_lseek,fd,start+off,SEEK_SET,0,0,0);return r<0?r:r-(int64_t)start;}
 case PROXY_SC_pread64:return pack_pread(fd,(void *)(uintptr_t)a[1],a[2],a[3]);
 case PROXY_SC_read:{if(dir)return -EISDIR;r=RAW(PROXY_SC_lseek,fd,0,SEEK_CUR,0,0,0);if(r<0)return r;uint64_t left=(uint64_t)r<start || (uint64_t)r-start>=len?0:len-((uint64_t)r-start);size_t n=a[2];if(n>left)n=left;return RAW(nr,fd,a[1],n,0,0,0);}
 case PROXY_SC_readv:case PROXY_SC_preadv:case PROXY_SC_preadv2:{if(dir)return -EISDIR;if(a[2]<0||(uint64_t)a[2]>SIZE_MAX/sizeof(struct iovec))return -EINVAL;struct iovec *v=(void *)(uintptr_t)a[1];int positional=nr!=PROXY_SC_readv;int64_t off=positional?(int64_t)((uint64_t)(uint32_t)a[3]|(uint64_t)(uint32_t)a[4]<<32):0;if(positional&&off<0)return -EINVAL;long total=0;for(proxy_arg_t i=0;i<a[2];i++){if(v[i].iov_len>(size_t)(LONG_MAX-total))return total?total:-EINVAL;proxy_arg_t b[6]={fd,(uintptr_t)v[i].iov_base,v[i].iov_len};long n=positional?pack_pread(fd,v[i].iov_base,v[i].iov_len,off):pack_call(PROXY_SC_read,b,handled);if(n<0)return total?total:n;total+=n;off+=n;if((size_t)n<v[i].iov_len)break;}return total;}
 case PROXY_SC_mmap:if(dir)return -ENODEV;if((a[2]&PROT_WRITE)&&(a[3]&MAP_SHARED))return -EACCES;if(a[5]<0||((uint64_t)a[5]&4095)||(start&4095))return -EINVAL;if((uint64_t)a[5]>INT64_MAX-start)return -EOVERFLOW;return RAW(nr,a[0],a[1],a[2],a[3],fd,start+a[5]);
 case PROXY_SC_write:case PROXY_SC_writev:case PROXY_SC_pwrite64:case PROXY_SC_pwritev:case PROXY_SC_pwritev2:case PROXY_SC_ftruncate:case PROXY_SC_fallocate:return -EBADF;
 default:break;
 }
 *handled=0;return 0;
}
