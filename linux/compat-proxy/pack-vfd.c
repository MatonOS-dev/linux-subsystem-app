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
static const char *pack_files_suffix(const char *path){for(size_t i=0;path[i];i++)if(path[i]=='/'&&path[i+1]=='f'&&path[i+2]=='i'&&path[i+3]=='l'&&path[i+4]=='e'&&path[i+5]=='s'&&(path[i+6]=='/'||!path[i+6]))return path[i+6]?path+i+7:path+i+6;return NULL;}
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
typedef uint64_t pack_lock_word __attribute__((aligned(8)));
struct pack_file {struct pack_file *next;char *name;int fd;};
struct pack_mount {struct pack_mount *next;int io_fd;pack_lock_word *io_owner;char *target;char *backing;mtp_index *index;void *blob;size_t size;struct pack_file *files;int directory;uint64_t root_dev,root_ino;int root_identity;};
struct pack_fd {struct pack_fd *next;int fd;struct pack_mount *mount;mtp_object object;uint64_t root_dev,root_ino;int root_identity;};
static struct pack_mount *pack_mounts;
static struct pack_fd *pack_fds;
static void *pack_alloc(void *c,size_t n){(void)c;return vfs_palloc(n);}
static void pack_free(void *c,void *p){(void)c;vfs_free(p);}
static char *pack_pdup(const char *s){size_t n=p_len(s);char *p=vfs_palloc(n+1);if(p)p_copy(p,s,n+1);return p;}
static struct pack_fd *pack_find(int fd){for(struct pack_fd *f=pack_fds;f;f=f->next)if(f->fd==fd)return f;return NULL;}
int pack_tracked(int fd){return pack_find(fd)!=NULL;}
uint64_t pack_slice_start(int fd){struct pack_fd *f=pack_find(fd);return f?f->object.offset:0;}
int pack_is_directory(int fd){struct pack_fd *f=pack_find(fd);return f?p_equal(f->object.kind,"dir"):-1;}
int pack_private(int fd){for(struct pack_mount *m=pack_mounts;m;m=m->next){if(m->directory==fd||m->io_fd==fd)return 1;for(struct pack_file *p=m->files;p;p=p->next)if(p->fd==fd)return 1;}return 0;}
/* Path operations arrive here after vfs_resolve_mount has translated their
 * virtual name to the backing path. Pack indexes describe the virtual tree,
 * so reverse that translation before looking up an object. This matters for
 * runtimes whose files/ tree is empty on disk and exists only in an OSTree
 * pack: a bind of files/ at /usr must still expose indexed /usr children. */
static char *pack_virtual_path(const char *path,int *staging){char *virtual=vfs_reverse_pack(path);if(staging)*staging=0;if(virtual&&prefix(virtual,"/newroot")){if(staging)*staging=1;const char *suffix=virtual+8;char *rootless=dupstr(*suffix?suffix:"/");vfs_free(virtual);return rootless;}return virtual;}
int pack_mount_root(const char *path){char *virtual=pack_virtual_path(path,NULL);if(!virtual)return 0;int found=0;for(struct pack_mount *m=pack_mounts;m;m=m->next)if(p_equal(virtual,m->target)){found=1;break;}vfs_free(virtual);return found;}
/* A canonical /usr/... source retained by a packed bind is still indexed,
 * even when its reverse name is a more-specific staging mount. Physical
 * tmpfs sources have no such canonical packed name and must shadow it. */
/* A deployment suffix belongs only to the pack backing that files/ root.
 * Index contents and virtual target length cannot identify a deployment. */
static const char *pack_backing_suffix(const struct pack_mount *m,const char *path){
 const char *suffix=pack_files_suffix(path);
 if(!suffix||!m->backing)return NULL;
 size_t length=(size_t)(suffix-path);if(length&&path[length-1]=='/')length--;
 return p_len(m->backing)==length&&!pack_cmp(path,m->backing,length)?suffix:NULL;
}
static struct pack_mount *pack_lookup(const char *path){int staging=0;char *virtual=pack_virtual_path(path,&staging);if(!virtual)return NULL;struct pack_mount *best=NULL,*fallback=NULL;size_t best_length=0,fallback_length=0;int mount_root=0;for(struct pack_mount *m=pack_mounts;m;m=m->next){size_t length=p_len(m->target);char *mapped=NULL;const char *candidate=virtual;if(!prefix(virtual,m->target)){const char *suffix=pack_backing_suffix(m,path);if(!suffix)continue;mapped=join(m->target,*suffix?suffix:"/");if(!mapped)continue;candidate=mapped;}if(!prefix(candidate,m->target)){vfs_free(mapped);continue;}if(!fallback||length>fallback_length){fallback=m;fallback_length=length;mount_root=p_equal(candidate,m->target);}if(best&&length<best_length){vfs_free(mapped);continue;}const char *key=candidate+length;mtp_object object;if(mtp_lookup(m->index,*key?key:"/",&object)){vfs_free(mapped);continue;}if(!best||length>best_length){best=m;best_length=length;}vfs_free(mapped);}if(best&&!prefix(path,best->target)&&vfs_has_nested_mount(virtual,best->target,staging))best=NULL;if(!best&&fallback&&!vfs_has_nested_mount(virtual,fallback->target,staging)&&!staging&&!mount_root)best=fallback;vfs_free(virtual);return best;}
static int pack_object(struct pack_mount *m,const char *path,mtp_object *o){char *virtual=pack_virtual_path(path,NULL);if(!virtual)return -ENOMEM;if(!prefix(virtual,m->target)){const char *suffix=pack_backing_suffix(m,path);if(!suffix){vfs_free(virtual);return -ENOENT;}char *mapped=join(m->target,*suffix?suffix:"/");vfs_free(virtual);virtual=mapped;if(!virtual)return -ENOMEM;}const char *key=virtual+p_len(m->target);int r=mtp_lookup(m->index,*key?key:"/",o)?-ENOENT:0;vfs_free(virtual);return r;}
static uint64_t pack_ino(const mtp_object *o){uint64_t v=1469598103934665603ULL;for(const char *p=o->checksum;*p;p++)v=(v^(unsigned char)*p)*1099511628211ULL;return v;}
static uint64_t pack_object_ino(const struct pack_mount *m,const mtp_object *o){return m->root_identity&&p_equal(o->kind,"dir")&&p_equal(o->path,"/")?m->root_ino:pack_ino(o);}
static uint64_t pack_object_dev(const struct pack_mount *m,const mtp_object *o){return m->root_identity&&p_equal(o->kind,"dir")&&p_equal(o->path,"/")?m->root_dev:0;}
static void pack_root_identity(const struct pack_mount *m,const mtp_object *o,const char *path,uint64_t *dev,uint64_t *ino,int *valid){
 int root=p_equal(o->kind,"dir")&&p_equal(o->path,"/");
 *dev=pack_object_dev(m,o);*ino=pack_object_ino(m,o);*valid=root&&m->root_identity;
 if(root&&path){struct proxy_stat actual;if(!arch_stat_at(AT_FDCWD,path,&actual,0)&&S_ISDIR(actual.st_mode)){*dev=actual.st_dev;*ino=actual.st_ino;*valid=1;}}
}
static void pack_metadata(const struct pack_mount *m,const mtp_object *o,void *wire,int extended,uint64_t root_dev,uint64_t root_ino,int root_identity){
 uint64_t size=p_equal(o->kind,"symlink")?p_len(o->target):o->length;
 uint64_t dev=root_identity?root_dev:pack_object_dev(m,o),ino=root_identity?root_ino:pack_object_ino(m,o);
 if(extended){arch_statx_synthesize(wire,o->mode,o->uid,o->gid,size,ino);((struct statx *)wire)->stx_mnt_id=(uintptr_t)m;arch_statx_device(wire,dev);}
 else {arch_stat_synthesize(wire,o->mode,o->uid,o->gid,size,ino);((struct stat *)wire)->st_dev=dev;}
}
int pack_stat(const char *path,struct proxy_stat *st){struct pack_mount *m=pack_lookup(path);if(!m)return -ENOSYS;mtp_object o;int r=pack_object(m,path,&o);if(!r){uint64_t dev,ino;int valid;pack_root_identity(m,&o,path,&dev,&ino,&valid);*st=(struct proxy_stat){.st_mode=o.mode,.st_uid=o.uid,.st_size=o.length,.st_dev=valid?dev:pack_object_dev(m,&o),.st_ino=valid?ino:pack_object_ino(m,&o),.st_nlink=1};}return r;}
char *pack_link(const char *path,long *error,int *handled){struct pack_mount *m=pack_lookup(path);*handled=m!=NULL;if(!m)return NULL;mtp_object o;*error=pack_object(m,path,&o);if(*error)return NULL;if(!p_equal(o.kind,"symlink")){*error=-EINVAL;return NULL;}return dupstr(o.target);}
/* Trusted launcher association, independent of virtual bind/pivot aliases. */
long pack_backing(const char *target,const char *directory){
 if(!target||target[0]!='/'||!directory||directory[0]!='/')return -EINVAL;
 long e=0;char *name=global_target(target,&e);if(!name)return e;
 struct pack_mount *m=pack_mounts;while(m&&!p_equal(m->target,name))m=m->next;
 vfs_free(name);if(!m)return -ENOENT;
 char *normalized=global_target(directory,&e);if(!normalized)return e;
 char *saved=pack_pdup(normalized);vfs_free(normalized);if(!saved)return -ENOMEM;
 vfs_free(m->backing);m->backing=saved;return 0;
}
long pack_mount(const char *target,const char *index,const char *directory){
 if(!target||target[0]!='/')return -EINVAL;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,index,O_RDONLY|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 struct proxy_stat st;long e=arch_stat_fd(fd,&st);if(e||st.st_size<12||(uint64_t)st.st_size>SIZE_MAX){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -EINVAL;}
 struct pack_mount *m=vfs_palloc(sizeof(*m));if(!m){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -ENOMEM;}*m=(struct pack_mount){.directory=-1,.io_fd=-1};m->size=st.st_size;m->blob=vfs_palloc(m->size);char *temp_target=global_target(target,&e);m->target=temp_target?pack_pdup(temp_target):NULL;vfs_free(temp_target);
 if(!m->blob||!m->target)e=-ENOMEM;
 if(!e && RAW(PROXY_SC_pread64,fd,m->blob,m->size,0,0,0)!=(long)m->size)e=-EIO;
 RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(!e && mtp_index_open_buffer(m->blob,m->size,&m->index,pack_alloc,pack_free,NULL))e=-EINVAL;
 if(!e){
  char *base=vfs_temp_create(&e);char *empty=base?join(base,"pack-directory"):NULL;vfs_free(base);
  if(!e&&!empty)e=-ENOMEM;
  if(!e)e=RAW(PROXY_SC_mkdirat,AT_FDCWD,empty,0700,0,0,0);
  if(!e){m->directory=RAW(PROXY_SC_openat,AT_FDCWD,empty,O_RDONLY|O_DIRECTORY,0,0,0);if(m->directory<0)e=m->directory;RAW(PROXY_SC_unlinkat,AT_FDCWD,empty,AT_REMOVEDIR,0,0,0);}
  vfs_free(empty);
 }
 if(!e){
  m->io_fd=proxy_private_fd(RAW(PROXY_SC_memfd_create,"pack-position-lock",0,0,0,0,0));
  if(m->io_fd<0)e=m->io_fd;
  if(!e)e=RAW(PROXY_SC_ftruncate,m->io_fd,4096,0,0,0,0);
  if(!e){long area=RAW(PROXY_SC_mmap,0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,m->io_fd,0);if((unsigned long)area>=(unsigned long)-4095)e=area;else m->io_owner=(void *)area;}
 }
 for(size_t i=0;!e&&i<mtp_index_count(m->index);i++){
  mtp_object *o=&m->index->items[i];if(!p_equal(o->kind,"file"))continue;
  if(o->offset>INT64_MAX||o->length>INT64_MAX-o->offset){e=-EOVERFLOW;break;}
  struct pack_file *p=m->files;while(p&&!p_equal(p->name,o->pack))p=p->next;
  if(!p){char *path=join(directory,o->pack);if(!path){e=-ENOMEM;break;}fd=RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY,0,0,0);vfs_free(path);if(fd<0){e=fd;break;}p=vfs_palloc(sizeof(*p));if(!p){RAW(PROXY_SC_close,fd,0,0,0,0,0);e=-ENOMEM;break;}*p=(struct pack_file){m->files,pack_pdup(o->pack),fd};m->files=p;if(!p->name){e=-ENOMEM;break;}}
  if(arch_stat_fd(p->fd,&st)||(uint64_t)st.st_size<o->offset+o->length)e=-EINVAL;
 }
 if(!e){
  /* The indexed root overlays the real files/ directory. Keep its kernel
   * identity so a directory fd passed to bwrap --bind-fd matches the path
   * after the emulated bind, just as it does for a real mount. */
  char *backing=physical(m->target);m->backing=backing?pack_pdup(backing):NULL;if(!m->backing)e=-ENOMEM;struct proxy_stat root_stat;
  if(backing&&!arch_stat_at(AT_FDCWD,backing,&root_stat,0)&&S_ISDIR(root_stat.st_mode)){m->root_dev=root_stat.st_dev;m->root_ino=root_stat.st_ino;m->root_identity=1;}
  vfs_free(backing);
  if(!e)e=proxy_mount(m->target,m->target);
  if(!e)table->flags=MS_RDONLY;
 }
 if(e){if(m->io_owner)RAW(PROXY_SC_munmap,m->io_owner,4096,0,0,0,0);if(m->io_fd>=0)RAW(PROXY_SC_close,m->io_fd,0,0,0,0,0);while(m->files){struct pack_file *p=m->files;m->files=p->next;RAW(PROXY_SC_close,p->fd,0,0,0,0,0);vfs_free(p->name);vfs_free(p);}if(m->directory>=0)RAW(PROXY_SC_close,m->directory,0,0,0,0,0);mtp_index_close(m->index);vfs_free(m->blob);vfs_free(m->target);vfs_free(m->backing);vfs_free(m);return e;}
 m->next=pack_mounts;pack_mounts=m;return 0;
}
void pack_close(int fd){struct pack_fd **p=&pack_fds;while(*p&&(*p)->fd!=fd)p=&(*p)->next;if(*p){struct pack_fd *old=*p;*p=old->next;vfs_free(old);}}
long pack_dup(int from,int to){if(from==to)return 0;struct pack_fd *f=pack_find(from);struct pack_fd copy;if(f)copy=*f;pack_close(to);if(f){f=vfs_palloc(sizeof(*f));if(!f)return -ENOMEM;*f=copy;f->fd=to;f->next=pack_fds;pack_fds=f;}return 0;}
long pack_open(const char *path,int flags){
 struct pack_mount *m=pack_lookup(path);if(!m)return -ENOSYS;
 if(flags&(O_WRONLY|O_RDWR|O_CREAT|O_TRUNC) || (flags&O_TMPFILE)==O_TMPFILE)return -EROFS;
 mtp_object o;long e=pack_object(m,path,&o);if(e)return e;
 int path_only=(flags&O_PATH)!=0;
 if(p_equal(o.kind,"symlink")&&!(path_only&&(flags&O_NOFOLLOW)))return -ELOOP;
 int dir=p_equal(o.kind,"dir");if((flags&O_DIRECTORY)&&!dir)return -ENOTDIR;
 int source=m->directory;if(!dir&&!path_only){struct pack_file *p=m->files;while(p&&!p_equal(p->name,o.pack))p=p->next;if(!p)return -EIO;source=p->fd;}
 /* dup shares an open file description. Reopen the pinned carrier first so
  * independent OPENs do not move each other's positions; aliases still dup. */
 long carrier;
 if(source==m->directory){carrier=RAW(PROXY_SC_openat,source,".",(path_only?O_PATH:O_RDONLY)|O_DIRECTORY|O_CLOEXEC,0,0,0);}
 else {char proc[64]="/proc/self/fd/";gpu_number(proc+14,source);carrier=RAW(PROXY_SC_openat,AT_FDCWD,proc,O_RDONLY|O_CLOEXEC,0,0,0);}
 if(carrier<0)return carrier;
 long fd=RAW(PROXY_SC_fcntl,carrier,(flags&O_CLOEXEC)?F_DUPFD_CLOEXEC:F_DUPFD,0,0,0,0);RAW(PROXY_SC_close,carrier,0,0,0,0,0);if(fd<0)return fd;
 e=path_only?0:RAW(PROXY_SC_lseek,fd,dir?0:o.offset,SEEK_SET,0,0,0);
 struct pack_fd *f=e<0?NULL:vfs_palloc(sizeof(*f));if(!f){RAW(PROXY_SC_close,fd,0,0,0,0,0);return e<0?e:-ENOMEM;}
 uint64_t root_dev,root_ino;int root_identity;pack_root_identity(m,&o,path,&root_dev,&root_ino,&root_identity);
 *f=(struct pack_fd){.next=pack_fds,.fd=fd,.mount=m,.object=o,.root_dev=root_dev,.root_ino=root_ino,.root_identity=root_identity};pack_fds=f;return fd;
}
long pack_pread(int fd,void *buf,size_t size,int64_t off){struct pack_fd *f=pack_find(fd);if(!f)return RAW(PROXY_SC_pread64,fd,buf,size,off,0,0);if(off<0)return -EINVAL;if(!p_equal(f->object.kind,"file"))return -EISDIR;if((uint64_t)off>=f->object.length)return 0;uint64_t left=f->object.length-off;if(size>left)size=left;return RAW(PROXY_SC_pread64,fd,buf,size,f->object.offset+off,0,0);}
static long pack_access(const mtp_object *o,int mode,int effective){
 if(mode&~7)return -EINVAL;
 if(mode&W_OK)return -EROFS;
 if(!mode)return 0;
 long uid=RAW(effective?PROXY_SC_geteuid:PROXY_SC_getuid,0,0,0,0,0,0);
 if(!uid)return (mode&X_OK)&&!(o->mode&0111)?-EACCES:0;
 long gid=RAW(effective?PROXY_SC_getegid:PROXY_SC_getgid,0,0,0,0,0,0);int group=(uint32_t)gid==o->gid;
 if(!group && (uint32_t)uid!=o->uid){
  long count=RAW(PROXY_SC_getgroups,0,0,0,0,0,0);if(count<0)return count;
  if((uint64_t)count>SIZE_MAX/sizeof(uint32_t))return -EOVERFLOW;
  uint32_t *groups=vfs_alloc(count*sizeof(*groups));if(!groups)return -ENOMEM;
  long n=RAW(PROXY_SC_getgroups,count,groups,0,0,0,0);if(n<0){vfs_free(groups);return n;}
  for(long i=0;i<n;i++)if(groups[i]==o->gid)group=1;
  vfs_free(groups);
 }
 unsigned bits=(uint32_t)uid==o->uid?(o->mode>>6):group?(o->mode>>3):o->mode;
 return (bits&(unsigned)mode)==(unsigned)mode?0:-EACCES;
}
long pack_path(long nr,proxy_arg_t *a,const char *path,int *handled){
 struct pack_mount *m=pack_lookup(path);*handled=m!=NULL;if(!m)return 0;
 switch(nr){case PROXY_SC_openat:return pack_open(path,a[2]);
 case PROXY_SC_newfstatat:case PROXY_SC_statx:{mtp_object o;long e=pack_object(m,path,&o);if(!e){uint64_t dev,ino;int valid;pack_root_identity(m,&o,path,&dev,&ino,&valid);pack_metadata(m,&o,(void *)(uintptr_t)a[nr==PROXY_SC_statx?4:2],nr==PROXY_SC_statx,dev,ino,valid);}return e;}
 case PROXY_SC_readlinkat:{mtp_object o;long e=pack_object(m,path,&o);if(e)return e;if(!p_equal(o.kind,"symlink"))return -EINVAL;size_t n=p_len(o.target);if(n>(size_t)a[3])n=a[3];p_copy((void *)(uintptr_t)a[2],o.target,n);return n;}
 case PROXY_SC_faccessat:case PROXY_SC_faccessat2:{if(a[2]&~7)return -EINVAL;mtp_object o;long e=pack_object(m,path,&o);if(e)return e;return pack_access(&o,a[2],nr==PROXY_SC_faccessat2&&(a[3]&AT_EACCESS));}
 default:return -EROFS;
 }
}
static long pack_transfer(long nr,proxy_arg_t *a,int *handled){
 int in=nr==PROXY_SC_sendfile?a[1]:a[0],out=nr==PROXY_SC_sendfile?a[0]:a[2];
 struct pack_fd *f=pack_find(in);*handled=0;
 if(pack_find(out)){*handled=1;return -EBADF;}if(!f)return 0;*handled=1;
 if(!p_equal(f->object.kind,"file"))return -EISDIR;
 void *ip=(void *)(uintptr_t)a[nr==PROXY_SC_sendfile?2:1];void *op=nr==PROXY_SC_sendfile?NULL:(void *)(uintptr_t)a[3];
 if(nr==PROXY_SC_copy_file_range && a[5])return -EINVAL;
 int64_t off=ip?(nr==PROXY_SC_sendfile?arch_sendfile_offset(ip):*(int64_t *)ip):RAW(PROXY_SC_lseek,in,0,SEEK_CUR,0,0,0)-(int64_t)f->object.offset;
 if(off<0)return -EINVAL;
 uint64_t count=a[nr==PROXY_SC_sendfile?3:4];if(count>LONG_MAX)count=LONG_MAX;
 uint64_t left=(uint64_t)off>=f->object.length?0:f->object.length-off;if(count>left)count=left;
 char buffer[8192];long total=0;
 while(count){size_t n=count>sizeof(buffer)?sizeof(buffer):count;long r=pack_pread(in,buffer,n,off);if(r<=0)return total?total:r;
  long w=op?RAW(PROXY_SC_pwrite64,out,buffer,r,*(int64_t *)op,0,0):RAW(PROXY_SC_write,out,buffer,r,0,0,0);
  if(w<0)return total?total:w;
  if(!w)break;
  total+=w;off+=w;count-=w;
  if(ip){if(nr==PROXY_SC_sendfile)arch_sendfile_result(ip,off);else *(int64_t *)ip=off;}
  else {proxy_arg_t seek[6]={in,off,SEEK_SET};int h;pack_call(PROXY_SC_lseek,seek,&h);}
  if(op)*(int64_t *)op+=w;
  if(w<r)break;
 }
 return total;
}
static long pack_call_unlocked(long nr,proxy_arg_t *a,int *handled){
 if(nr==PROXY_SC_sendfile||nr==PROXY_SC_copy_file_range||nr==PROXY_SC_splice)return pack_transfer(nr,a,handled);
 int fd=nr==PROXY_SC_mmap?a[4]:a[0];struct pack_fd *f=pack_find(fd);*handled=0;if(!f)return 0;
 uint64_t start=f->object.offset,len=f->object.length;int dir=p_equal(f->object.kind,"dir");long r;*handled=1;
 switch(nr){
 case PROXY_SC_readlinkat:if(a[1]&&!*(char *)(uintptr_t)a[1]&&(f->object.mode&S_IFMT)==S_IFLNK){size_t n=p_len(f->object.target);if(n>(size_t)a[3])n=a[3];p_copy((void *)(uintptr_t)a[2],f->object.target,n);return n;}break;
 case PROXY_SC_getdents64:{
  if(!dir)return -ENOTDIR;
  long cursor=RAW(PROXY_SC_lseek,fd,0,SEEK_CUR,0,0,0);if(cursor<0)return cursor;
  size_t used=0;
  for(;;){mtp_object o=f->object;const char *name;size_t next;
   if(cursor<2){name=cursor?"..":".";next=cursor+1;}
   else {size_t c=cursor-2;if(!mtp_readdir(f->mount->index,f->object.path,&c,&name,&o))break;next=c+2;}
   size_t n=p_len(name);if(n>UINT16_MAX-27)return used?(long)used:-EOVERFLOW;size_t size=(19+n+1+7)&~(size_t)7;
   if(size>(uint64_t)a[2]-used){if(!used)return -EINVAL;break;}
   unsigned char *b=(void *)((uintptr_t)a[1]+used);uint64_t ino=cursor<2&&f->root_identity?f->root_ino:pack_object_ino(f->mount,&o),cookie=next;uint16_t reclen=size;
   p_copy(b,&ino,8);p_copy(b+8,&cookie,8);p_copy(b+16,&reclen,2);b[18]=(o.mode>>12)&15;p_copy(b+19,name,n+1);for(size_t k=20+n;k<size;k++)b[k]=0;
   used+=size;cursor=next;
  }
  r=RAW(PROXY_SC_lseek,fd,cursor,SEEK_SET,0,0,0);return r<0?r:(long)used;
 }
 case PROXY_SC_fstat:pack_metadata(f->mount,&f->object,(void *)(uintptr_t)a[1],0,f->root_dev,f->root_ino,f->root_identity);return 0;
 case PROXY_SC_newfstatat:case PROXY_SC_statx:if(a[1]&&!*(char *)(uintptr_t)a[1]&&(a[nr==PROXY_SC_statx?2:3]&AT_EMPTY_PATH)){pack_metadata(f->mount,&f->object,(void *)(uintptr_t)a[nr==PROXY_SC_statx?4:2],nr==PROXY_SC_statx,f->root_dev,f->root_ino,f->root_identity);return 0;}break;
 case PROXY_SC_lseek:{int64_t off=a[1],base=0;if(a[2]==SEEK_CUR){r=RAW(PROXY_SC_lseek,fd,0,SEEK_CUR,0,0,0);if(r<0)return r;base=r-start;}else if(a[2]==SEEK_END)base=len;else if(a[2]!=SEEK_SET)return -EINVAL;if((off>0&&base>INT64_MAX-off)||(off<0&&base<INT64_MIN-off))return -EOVERFLOW;off+=base;if(off<0)return -EINVAL;if((uint64_t)off>INT64_MAX-start)return -EOVERFLOW;r=RAW(PROXY_SC_lseek,fd,start+off,SEEK_SET,0,0,0);return r<0?r:r-(int64_t)start;}
 case PROXY_SC_pread64:return pack_pread(fd,(void *)(uintptr_t)a[1],a[2],a[3]);
 case PROXY_SC_read:{if(dir)return -EISDIR;r=RAW(PROXY_SC_lseek,fd,0,SEEK_CUR,0,0,0);if(r<0)return r;uint64_t left=(uint64_t)r<start || (uint64_t)r-start>=len?0:len-((uint64_t)r-start);size_t n=a[2];if(n>left)n=left;return RAW(nr,fd,a[1],n,0,0,0);}
 case PROXY_SC_readv:case PROXY_SC_preadv:case PROXY_SC_preadv2:{
  if(dir)return -EISDIR;
  if(a[2]<0 || (uint64_t)a[2]>SIZE_MAX/arch_iovec_size(1))return -EINVAL;
  int positional=nr!=PROXY_SC_readv;
  int64_t off=positional?arch_vector_offset(a[3],a[4]):0;
  if(nr==PROXY_SC_preadv2 && off==-1)positional=0;
  if(!positional){r=RAW(PROXY_SC_lseek,fd,0,SEEK_CUR,0,0,0);if(r<0)return r;off=r-(int64_t)start;}
  if(off<0)return -EINVAL;
  uint64_t left=(uint64_t)off>=len?0:len-off;
  if(positional && (uint64_t)off>INT64_MAX-start)return -EOVERFLOW;
  void *vectors=vfs_alloc(arch_iovec_size(a[2]));if(!vectors)return -ENOMEM;
  uint64_t total=0;
  for(proxy_arg_t i=0;i<a[2];i++){
   struct proxy_iovec v=arch_iovec_read((void *)(uintptr_t)a[1],i);
   if(v.len>LONG_MAX-total){vfs_free(vectors);return -EINVAL;}total+=v.len;
   if(v.len>left)v.len=left;
   left-=v.len;arch_iovec_write(vectors,i,v);
  }
  uint64_t shifted=positional?start+off:UINT64_MAX;
  r=RAW(nr,fd,vectors,a[2],arch_vector_low(shifted),arch_vector_high(shifted),a[5]);vfs_free(vectors);return r;
 }
 case PROXY_SC_mmap:if(a[3]&MAP_ANONYMOUS){*handled=0;return 0;}if(dir)return -ENODEV;if((a[2]&PROT_WRITE)&&(a[3]&MAP_SHARED))return -EACCES;if(a[5]<0||((uint64_t)a[5]&4095)||(start&4095))return -EINVAL;if((uint64_t)a[5]>INT64_MAX-start)return -EOVERFLOW;return RAW(nr,a[0],a[1],a[2],a[3],fd,start+a[5]);
 case PROXY_SC_write:case PROXY_SC_writev:case PROXY_SC_pwrite64:case PROXY_SC_pwritev:case PROXY_SC_pwritev2:case PROXY_SC_ftruncate:case PROXY_SC_fallocate:return -EBADF;
 case PROXY_SC_fchmod:case PROXY_SC_fchown:case PROXY_SC_fsetxattr:case PROXY_SC_fremovexattr:case PROXY_SC_utimensat:return -EROFS;
 default:break;
 }
 *handled=0;return 0;
}
/* Serialize the position snapshot plus clamped kernel read. The small shared
 * record is per mount (not a substitute open-file description); the kernel
 * still owns every fd position. Fork and peer exec retain the same record.
 * Block app signals while held, and recover a dead process/thread owner. */
long pack_call(long nr,proxy_arg_t *a,int *handled){
 struct pack_fd *f=pack_find(a[0]);
 int position=nr==PROXY_SC_read||nr==PROXY_SC_readv||nr==PROXY_SC_preadv2||nr==PROXY_SC_lseek||nr==PROXY_SC_getdents64;
 if(!f||!position)return pack_call_unlocked(nr,a,handled);
 proxy_sigmask_t all=~UINT64_C(0),mask=0;RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&all,&mask,8,0,0);
 uint64_t owner=((uint64_t)(uint32_t)RAW(PROXY_SC_getpid,0,0,0,0,0,0)<<32)|(uint32_t)RAW(PROXY_SC_gettid,0,0,0,0,0,0);
 pack_lock_word *word=f->mount->io_owner;
 for(;;){
  uint64_t expected=0;if(__atomic_compare_exchange_n(word,&expected,owner,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))break;
  long live=RAW(PROXY_SC_tgkill,expected>>32,(uint32_t)expected,0,0,0,0);
  if(live==-ESRCH)__atomic_compare_exchange_n(word,&expected,0,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
  else RAW(PROXY_SC_sched_yield,0,0,0,0,0,0);
 }
 long r=pack_call_unlocked(nr,a,handled);
 __atomic_store_n(word,0,__ATOMIC_RELEASE);RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&mask,0,8,0,0);return r;
}
/* Real descriptors provide fork/dup position sharing; only the slice identity
 * and trusted mount index need to cross a peer-loader exec. */
static void pack_state(struct state *s){
 uint64_t n=0;for(struct pack_mount *m=pack_mounts;m;m=m->next)n++;
 n=state_word(s,n);struct pack_mount **tail=&pack_mounts;
 for(uint64_t i=0;i<n&&!s->error;i++){
  struct pack_mount *m=s->reading?vfs_palloc(sizeof(*m)):*tail;
  if(!m){s->error=-ENOMEM;return;}if(s->reading){*m=(struct pack_mount){0};*tail=m;}
  state_string(s,&m->target);state_string(s,&m->backing);m->size=state_word(s,m->size);
  if(s->reading){if(m->size>s->size-s->pos){s->error=-EINVAL;return;}m->blob=vfs_palloc(m->size);if(!m->blob){s->error=-ENOMEM;return;}}
  state_bytes(s,m->blob,m->size);
  if(s->reading&&mtp_index_open_buffer(m->blob,m->size,&m->index,pack_alloc,pack_free,NULL)){s->error=-EINVAL;return;}
  m->io_fd=state_word(s,m->io_fd);
  if(!state_live(m->io_fd)){s->error=-EBADF;return;}
  if(s->reading){long area=RAW(PROXY_SC_mmap,0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,m->io_fd,0);if((unsigned long)area>=(unsigned long)-4095){s->error=area;return;}m->io_owner=(void *)area;}
  m->directory=state_word(s,m->directory);if(!state_live(m->directory)){s->error=-EBADF;return;}
  uint64_t k=0;for(struct pack_file *p=m->files;p;p=p->next)k++;k=state_word(s,k);struct pack_file **pt=&m->files;
  for(uint64_t j=0;j<k&&!s->error;j++){struct pack_file *p=s->reading?vfs_palloc(sizeof(*p)):*pt;if(!p){s->error=-ENOMEM;return;}if(s->reading){*p=(struct pack_file){0};*pt=p;}state_string(s,&p->name);p->fd=state_word(s,p->fd);if(!state_live(p->fd))s->error=-EBADF;pt=&p->next;}
  tail=&m->next;
 }
 n=0;for(struct pack_fd *f=pack_fds;f;f=f->next)if(state_live(f->fd))n++;n=state_word(s,n);
 if(s->reading){for(uint64_t i=0;i<n&&!s->error;i++){struct pack_fd *f=vfs_palloc(sizeof(*f));if(!f){s->error=-ENOMEM;return;}*f=(struct pack_fd){0};f->fd=state_word(s,0);uint64_t id=state_word(s,0);struct pack_mount *m=pack_mounts;while(id&&m){m=m->next;id--;}f->root_dev=state_word(s,0);f->root_ino=state_word(s,0);f->root_identity=(int)state_word(s,0);char *path=NULL;state_string(s,&path);if(!m||!path||mtp_lookup(m->index,path,&f->object)||!state_live(f->fd)){s->error=-EINVAL;vfs_free(path);vfs_free(f);return;}vfs_free(path);f->mount=m;f->next=pack_fds;pack_fds=f;}}
 else for(struct pack_fd *f=pack_fds;f;f=f->next)if(state_live(f->fd)){state_word(s,f->fd);uint64_t id=0;for(struct pack_mount *m=pack_mounts;m&&m!=f->mount;m=m->next)id++;state_word(s,id);state_word(s,f->root_dev);state_word(s,f->root_ino);state_word(s,f->root_identity);char *path=(char *)f->object.path;state_string(s,&path);}
}
