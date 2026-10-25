/* SPDX-License-Identifier: Apache-2.0
 * Included by vfs.c: MTPKIDX2 metadata over daemon-mounted EROFS layers.
 * Layer paths and index files are trusted launcher input. */
#include <sys/uio.h>
#include <limits.h>
#include <unistd.h>
#include <linux/stat.h>
static int erofs_cmp(const void *a,const void *b,size_t n){const unsigned char *x=a,*y=b;for(size_t i=0;i<n;i++)if(x[i]!=y[i])return x[i]-y[i];return 0;}
static int erofs_strcmp(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
static char *erofs_chr(const char *s,int c){do{if(*s==c)return (char *)s;}while(*s++);return NULL;}
static const char *erofs_files_suffix(const char *path){for(size_t i=0;path[i];i++)if(path[i]=='/'&&path[i+1]=='f'&&path[i+2]=='i'&&path[i+3]=='l'&&path[i+4]=='e'&&path[i+5]=='s'&&(path[i+6]=='/'||!path[i+6]))return path[i+6]?path+i+7:path+i+6;return NULL;}
#define MTP_FREESTANDING
#define MTPK_V2_ONLY
#define strlen p_len
#define strcmp erofs_strcmp
#define memcmp erofs_cmp
#define strchr erofs_chr
#include "../packs/reader.c"
#undef strlen
#undef strcmp
#undef memcmp
#undef strchr
typedef uint64_t erofs_lock_word __attribute__((aligned(8)));
struct erofs_mount {struct erofs_mount *next;int io_fd;erofs_lock_word *io_owner;char *target;char *backing;char *layer_directory;mtp_index *index;void *blob;size_t size;uint64_t root_dev,root_ino;int root_identity;};
struct erofs_dirfd {struct erofs_dirfd *next;int fd;struct erofs_mount *mount;mtp_object object;uint64_t root_dev,root_ino;int root_identity;};
static struct erofs_mount *erofs_mounts;
static struct erofs_dirfd *erofs_dirs;
static void *erofs_alloc(void *c,size_t n){(void)c;return vfs_palloc(n);}
static void erofs_free(void *c,void *p){(void)c;vfs_free(p);}
static char *erofs_pdup(const char *s){size_t n=p_len(s);char *p=vfs_palloc(n+1);if(p)p_copy(p,s,n+1);return p;}
static struct erofs_dirfd *erofs_dir_find(int fd){for(struct erofs_dirfd *f=erofs_dirs;f;f=f->next)if(f->fd==fd)return f;return NULL;}
int erofs_tracked(int fd){return erofs_dir_find(fd)!=NULL;}
int erofs_is_directory(int fd){return erofs_dir_find(fd)?1:-1;}
int erofs_private(int fd){for(struct erofs_mount *m=erofs_mounts;m;m=m->next)if(m->io_fd==fd)return 1;return 0;}
/* Path operations arrive here after vfs_resolve_mount has translated their
 * virtual name to the backing path. Pack indexes describe the virtual tree,
 * so reverse that translation before looking up an object. This matters for
 * runtimes whose files/ tree is empty on disk and exists only in an OSTree
 * EROFS: a bind of files/ at /usr must still expose indexed /usr children. */
static char *erofs_virtual_path(const char *path,int *staging){char *virtual=vfs_reverse_erofs(path);if(staging)*staging=0;if(virtual&&prefix(virtual,"/newroot")){if(staging)*staging=1;const char *suffix=virtual+8;char *rootless=dupstr(*suffix?suffix:"/");vfs_free(virtual);return rootless;}return virtual;}
int erofs_mount_root(const char *path){char *virtual=erofs_virtual_path(path,NULL);if(!virtual)return 0;int found=0;for(struct erofs_mount *m=erofs_mounts;m;m=m->next)if(p_equal(virtual,m->target)){found=1;break;}vfs_free(virtual);return found;}
/* A canonical /usr/... source retained by an indexed bind is still indexed,
 * even when its reverse name is a more-specific staging mount. Physical
 * tmpfs sources have no such canonical packed name and must shadow it. */
/* A deployment suffix belongs only to the EROFS backing that files/ root.
 * Index contents and virtual target length cannot identify a deployment. */
static const char *erofs_backing_suffix(const struct erofs_mount *m,const char *path){
 const char *suffix=erofs_files_suffix(path);
 if(!suffix||!m->backing)return NULL;
 size_t length=(size_t)(suffix-path);if(length&&path[length-1]=='/')length--;
 return p_len(m->backing)==length&&!erofs_cmp(path,m->backing,length)?suffix:NULL;
}
static struct erofs_mount *erofs_lookup(const char *path){int staging=0;char *virtual=erofs_virtual_path(path,&staging);if(!virtual)return NULL;struct erofs_mount *best=NULL,*fallback=NULL;size_t best_length=0,fallback_length=0;int mount_root=0;for(struct erofs_mount *m=erofs_mounts;m;m=m->next){size_t length=p_len(m->target);char *mapped=NULL;const char *candidate=virtual;if(!prefix(virtual,m->target)){const char *suffix=erofs_backing_suffix(m,path);if(!suffix)continue;mapped=join(m->target,*suffix?suffix:"/");if(!mapped)continue;candidate=mapped;}if(!prefix(candidate,m->target)){vfs_free(mapped);continue;}if(!fallback||length>fallback_length){fallback=m;fallback_length=length;mount_root=p_equal(candidate,m->target);}if(best&&length<best_length){vfs_free(mapped);continue;}const char *key=candidate+length;mtp_object object;if(mtp_lookup(m->index,*key?key:"/",&object)){vfs_free(mapped);continue;}if(!best||length>best_length){best=m;best_length=length;}vfs_free(mapped);}if(best&&!prefix(path,best->target)&&vfs_has_nested_mount(virtual,best->target,staging))best=NULL;if(!best&&fallback&&!vfs_has_nested_mount(virtual,fallback->target,staging)&&!staging&&!mount_root)best=fallback;vfs_free(virtual);return best;}
static int erofs_object(struct erofs_mount *m,const char *path,mtp_object *o){char *virtual=erofs_virtual_path(path,NULL);if(!virtual)return -ENOMEM;if(!prefix(virtual,m->target)){const char *suffix=erofs_backing_suffix(m,path);if(!suffix){vfs_free(virtual);return -ENOENT;}char *mapped=join(m->target,*suffix?suffix:"/");vfs_free(virtual);virtual=mapped;if(!virtual)return -ENOMEM;}const char *key=virtual+p_len(m->target);int r=mtp_lookup(m->index,*key?key:"/",o)?-ENOENT:0;vfs_free(virtual);return r;}
static uint64_t erofs_ino(const mtp_object *o){uint64_t v=1469598103934665603ULL;for(const char *p=o->checksum;*p;p++)v=(v^(unsigned char)*p)*1099511628211ULL;return v;}
static uint64_t erofs_object_ino(const struct erofs_mount *m,const mtp_object *o){return m->root_identity&&p_equal(o->kind,"dir")&&p_equal(o->path,"/")?m->root_ino:erofs_ino(o);}
static uint64_t erofs_object_dev(const struct erofs_mount *m,const mtp_object *o){return m->root_identity&&p_equal(o->kind,"dir")&&p_equal(o->path,"/")?m->root_dev:0;}
static void erofs_root_identity(const struct erofs_mount *m,const mtp_object *o,const char *path,uint64_t *dev,uint64_t *ino,int *valid){
 int root=p_equal(o->kind,"dir")&&p_equal(o->path,"/");
 *dev=erofs_object_dev(m,o);*ino=erofs_object_ino(m,o);*valid=root&&m->root_identity;
 if(root&&path){struct proxy_stat actual;if(!arch_stat_at(AT_FDCWD,path,&actual,0)&&S_ISDIR(actual.st_mode)){*dev=actual.st_dev;*ino=actual.st_ino;*valid=1;}}
}
static void erofs_metadata(const struct erofs_mount *m,const mtp_object *o,void *wire,int extended,uint64_t root_dev,uint64_t root_ino,int root_identity){
 uint64_t size=p_equal(o->kind,"symlink")?p_len(o->target):o->size;
 uint64_t dev=root_identity?root_dev:erofs_object_dev(m,o),ino=root_identity?root_ino:erofs_object_ino(m,o);
 if(extended){arch_statx_synthesize(wire,o->mode,o->uid,o->gid,size,ino);((struct statx *)wire)->stx_mnt_id=(uintptr_t)m;((struct statx *)wire)->stx_mtime.tv_sec=(int64_t)o->mtime;arch_statx_device(wire,dev);}
 else {arch_stat_synthesize(wire,o->mode,o->uid,o->gid,size,ino);((struct stat *)wire)->st_dev=dev;((struct stat *)wire)->st_mtim.tv_sec=(int64_t)o->mtime;}
}
int erofs_stat(const char *path,struct proxy_stat *st){struct erofs_mount *m=erofs_lookup(path);if(!m)return -ENOSYS;mtp_object o;int r=erofs_object(m,path,&o);if(!r){uint64_t dev,ino;int valid;erofs_root_identity(m,&o,path,&dev,&ino,&valid);*st=(struct proxy_stat){.st_mode=o.mode,.st_uid=o.uid,.st_size=o.size,.st_dev=valid?dev:erofs_object_dev(m,&o),.st_ino=valid?ino:erofs_object_ino(m,&o),.st_nlink=1};}return r;}
char *erofs_link(const char *path,long *error,int *handled){struct erofs_mount *m=erofs_lookup(path);*handled=m!=NULL;if(!m)return NULL;mtp_object o;*error=erofs_object(m,path,&o);if(*error)return NULL;if(!p_equal(o.kind,"symlink")){*error=-EINVAL;return NULL;}return dupstr(o.target);}
/* Trusted launcher association, independent of virtual bind/pivot aliases. */
long erofs_backing(const char *target,const char *directory){
 if(!target||target[0]!='/'||!directory||directory[0]!='/')return -EINVAL;
 long e=0;char *name=global_target(target,&e);if(!name)return e;
 struct erofs_mount *m=erofs_mounts;while(m&&!p_equal(m->target,name))m=m->next;
 vfs_free(name);if(!m)return -ENOENT;
 char *normalized=global_target(directory,&e);if(!normalized)return e;
 char *saved=erofs_pdup(normalized);vfs_free(normalized);if(!saved)return -ENOMEM;
 vfs_free(m->backing);m->backing=saved;return 0;
}
long erofs_mount(const char *target,const char *index,const char *directory){
 if(!target||target[0]!='/')return -EINVAL;
 long fd=RAW(PROXY_SC_openat,AT_FDCWD,index,O_RDONLY|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 struct proxy_stat st;long e=arch_stat_fd(fd,&st);if(e||st.st_size<12||(uint64_t)st.st_size>SIZE_MAX){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -EINVAL;}
 struct erofs_mount *m=vfs_palloc(sizeof(*m));if(!m){RAW(PROXY_SC_close,fd,0,0,0,0,0);return -ENOMEM;}*m=(struct erofs_mount){.io_fd=-1};m->size=st.st_size;m->blob=vfs_palloc(m->size);char *temp_target=global_target(target,&e);m->target=temp_target?erofs_pdup(temp_target):NULL;vfs_free(temp_target);m->layer_directory=erofs_pdup(directory);
 if(!m->blob||!m->target)e=-ENOMEM;
 if(!e && RAW(PROXY_SC_pread64,fd,m->blob,m->size,0,0,0)!=(long)m->size)e=-EIO;
 RAW(PROXY_SC_close,fd,0,0,0,0,0);
 if(!e && (m->size<8||erofs_cmp(m->blob,"MTPKIDX2",8)))e=-EINVAL;
 if(!e && mtp_index_open_buffer(m->blob,m->size,&m->index,erofs_alloc,erofs_free,NULL))e=-EINVAL;
 if(!e && (!m->layer_directory || !mtp_layer_count(m->index)))e=-EINVAL;
 if(!e){
  m->io_fd=proxy_private_fd(RAW(PROXY_SC_memfd_create,"erofs-dir-lock",0,0,0,0,0));
  if(m->io_fd<0)e=m->io_fd;
  if(!e)e=RAW(PROXY_SC_ftruncate,m->io_fd,4096,0,0,0,0);
  if(!e){long area=RAW(PROXY_SC_mmap,0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,m->io_fd,0);if((unsigned long)area>=(unsigned long)-4095)e=area;else m->io_owner=(void *)area;}
 }
 if(!e){
  /* The indexed root overlays the real files/ directory. Keep its kernel
   * identity so a directory fd passed to bwrap --bind-fd matches the path
   * after the emulated bind, just as it does for a real mount. */
  char *backing=physical(m->target);m->backing=backing?erofs_pdup(backing):NULL;if(!m->backing)e=-ENOMEM;struct proxy_stat root_stat;
 if(backing&&!arch_stat_at(AT_FDCWD,backing,&root_stat,0)&&S_ISDIR(root_stat.st_mode)){m->root_dev=root_stat.st_dev;m->root_ino=root_stat.st_ino;m->root_identity=1;}
  vfs_free(backing);
  if(!e)e=proxy_mount(m->target,m->target);
  if(!e)table->flags=MS_RDONLY;
 }
 if(e){if(m->io_owner)RAW(PROXY_SC_munmap,m->io_owner,4096,0,0,0,0);if(m->io_fd>=0)RAW(PROXY_SC_close,m->io_fd,0,0,0,0,0);mtp_index_close(m->index);vfs_free(m->blob);vfs_free(m->target);vfs_free(m->backing);vfs_free(m->layer_directory);vfs_free(m);return e;}
 m->next=erofs_mounts;erofs_mounts=m;return 0;
}
void erofs_close(int fd){struct erofs_dirfd **p=&erofs_dirs;while(*p&&(*p)->fd!=fd)p=&(*p)->next;if(*p){struct erofs_dirfd *old=*p;*p=old->next;vfs_free(old);}}
long erofs_dup(int from,int to){if(from==to)return 0;struct erofs_dirfd *f=erofs_dir_find(from);struct erofs_dirfd copy;if(f)copy=*f;erofs_close(to);if(f){f=vfs_palloc(sizeof(*f));if(!f)return -ENOMEM;*f=copy;f->fd=to;f->next=erofs_dirs;erofs_dirs=f;}return 0;}
long erofs_open(const char *path,int flags){
 struct erofs_mount *m=erofs_lookup(path);if(!m)return -ENOSYS;
 if(flags&(O_WRONLY|O_RDWR|O_CREAT|O_TRUNC) || (flags&O_TMPFILE)==O_TMPFILE)return -EROFS;
 mtp_object o;long e=erofs_object(m,path,&o);if(e)return e;
 int path_only=(flags&O_PATH)!=0;
 if(p_equal(o.kind,"symlink")&&!(path_only&&(flags&O_NOFOLLOW)))return -ELOOP;
 int dir=p_equal(o.kind,"dir");if((flags&O_DIRECTORY)&&!dir)return -ENOTDIR;
 {
  char *root=NULL,*native=NULL;
  if(dir){
   mtp_layer layer;if(mtp_get_layer(m->index,0,&layer))return -EIO;
   root=join(m->layer_directory,layer.split);native=root;
   flags=(flags&~(O_CREAT|O_TRUNC|O_EXCL))|O_DIRECTORY;
  }else{
   if(o.layer==UINT32_MAX)return -EIO;
   mtp_layer layer;if(mtp_get_layer(m->index,o.layer,&layer))return -EIO;
   root=join(m->layer_directory,layer.split);
   if(root){const char *relative=o.path;while(*relative=='/')relative++;native=join(root,relative);}
  }
  if(!native){vfs_free(root);return -ENOMEM;}
  long opened=RAW(PROXY_SC_openat,AT_FDCWD,native,flags,0,0,0);
  if(native!=root)vfs_free(native);
  vfs_free(root);
  if(opened<0||!dir)return opened;
  struct erofs_dirfd *f=vfs_palloc(sizeof(*f));if(!f){RAW(PROXY_SC_close,opened,0,0,0,0,0);return -ENOMEM;}
  uint64_t root_dev,root_ino;int root_identity;erofs_root_identity(m,&o,path,&root_dev,&root_ino,&root_identity);
  *f=(struct erofs_dirfd){.next=erofs_dirs,.fd=(int)opened,.mount=m,.object=o,.root_dev=root_dev,.root_ino=root_ino,.root_identity=root_identity};erofs_dirs=f;
  return opened;
 }
 return -EINVAL;
}
long proxy_pread_fd(int fd,void *buf,size_t size,int64_t off){return RAW(PROXY_SC_pread64,fd,buf,size,off,0,0);}
static long erofs_access(const mtp_object *o,int mode,int effective){
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
long erofs_path(long nr,proxy_arg_t *a,const char *path,int *handled){
 struct erofs_mount *m=erofs_lookup(path);*handled=m!=NULL;if(!m)return 0;
 switch(nr){case PROXY_SC_openat:return erofs_open(path,a[2]);
 case PROXY_SC_newfstatat:case PROXY_SC_statx:{mtp_object o;long e=erofs_object(m,path,&o);if(!e){uint64_t dev,ino;int valid;erofs_root_identity(m,&o,path,&dev,&ino,&valid);erofs_metadata(m,&o,(void *)(uintptr_t)a[nr==PROXY_SC_statx?4:2],nr==PROXY_SC_statx,dev,ino,valid);}return e;}
 case PROXY_SC_readlinkat:{mtp_object o;long e=erofs_object(m,path,&o);if(e)return e;if(!p_equal(o.kind,"symlink"))return -EINVAL;size_t n=p_len(o.target);if(n>(size_t)a[3])n=a[3];p_copy((void *)(uintptr_t)a[2],o.target,n);return n;}
 case PROXY_SC_faccessat:case PROXY_SC_faccessat2:{if(a[2]&~7)return -EINVAL;mtp_object o;long e=erofs_object(m,path,&o);if(e)return e;return erofs_access(&o,a[2],nr==PROXY_SC_faccessat2&&(a[3]&AT_EACCESS));}
 default:return -EROFS;
 }
}
static long erofs_call_unlocked(long nr,proxy_arg_t *a,int *handled){
 int fd=nr==PROXY_SC_mmap?a[4]:a[0];struct erofs_dirfd *f=erofs_dir_find(fd);*handled=0;if(!f)return 0;
 *handled=1;
 switch(nr){
 case PROXY_SC_getdents64:{
  if(!p_equal(f->object.kind,"dir"))return -ENOTDIR;
  long cursor=RAW(PROXY_SC_lseek,fd,0,SEEK_CUR,0,0,0);if(cursor<0)return cursor;
  size_t used=0;
  for(;;){mtp_object o=f->object;const char *name;size_t next;
   if(cursor<2){name=cursor?"..":".";next=cursor+1;}
   else {size_t c=(size_t)cursor-2;if(!mtp_readdir(f->mount->index,f->object.path,&c,&name,&o))break;next=c+2;}
   size_t n=p_len(name);if(n>UINT16_MAX-27)return used?(long)used:-EOVERFLOW;size_t size=(19+n+1+7)&~(size_t)7;
   if(size>(uint64_t)a[2]-used){if(!used)return -EINVAL;break;}
   unsigned char *b=(void *)((uintptr_t)a[1]+used);uint64_t ino=cursor<2&&f->root_identity?f->root_ino:erofs_object_ino(f->mount,&o),cookie=next;uint16_t reclen=size;
   p_copy(b,&ino,8);p_copy(b+8,&cookie,8);p_copy(b+16,&reclen,2);b[18]=(o.mode>>12)&15;p_copy(b+19,name,n+1);for(size_t k=20+n;k<size;k++)b[k]=0;
   used+=size;cursor=(long)next;
  }
  long r=RAW(PROXY_SC_lseek,fd,cursor,SEEK_SET,0,0,0);return r<0?r:(long)used;
 }
 case PROXY_SC_fstat:erofs_metadata(f->mount,&f->object,(void *)(uintptr_t)a[1],0,f->root_dev,f->root_ino,f->root_identity);return 0;
 case PROXY_SC_newfstatat:case PROXY_SC_statx:
  if(a[1]&&!*(char *)(uintptr_t)a[1]&&(a[nr==PROXY_SC_statx?2:3]&AT_EMPTY_PATH)){
   erofs_metadata(f->mount,&f->object,(void *)(uintptr_t)a[nr==PROXY_SC_statx?4:2],nr==PROXY_SC_statx,f->root_dev,f->root_ino,f->root_identity);return 0;
  }
  break;
 case PROXY_SC_lseek:{
  if(a[2]==SEEK_CUR)return RAW(PROXY_SC_lseek,fd,a[1],SEEK_CUR,0,0,0);
  if(a[2]==SEEK_SET&&a[1]>=0)return RAW(PROXY_SC_lseek,fd,a[1],SEEK_SET,0,0,0);
  return -EINVAL;
 }
 default:break;
 }
 *handled=0;return 0;
}
/* Serialize the position snapshot plus clamped kernel read. The small shared
 * record is per mount (not a substitute open-file description); the kernel
 * still owns every fd position. Fork and peer exec retain the same record.
 * Block app signals while held, and recover a dead process/thread owner. */
long erofs_call(long nr,proxy_arg_t *a,int *handled){
 struct erofs_dirfd *f=erofs_dir_find(a[0]);
 int position=nr==PROXY_SC_read||nr==PROXY_SC_readv||nr==PROXY_SC_preadv2||nr==PROXY_SC_lseek||nr==PROXY_SC_getdents64;
 if(!f||!position)return erofs_call_unlocked(nr,a,handled);
 proxy_sigmask_t all=~UINT64_C(0),mask=0;RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&all,&mask,8,0,0);
 uint64_t owner=((uint64_t)(uint32_t)RAW(PROXY_SC_getpid,0,0,0,0,0,0)<<32)|(uint32_t)RAW(PROXY_SC_gettid,0,0,0,0,0,0);
 erofs_lock_word *word=f->mount->io_owner;
 for(;;){
  uint64_t expected=0;if(__atomic_compare_exchange_n(word,&expected,owner,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))break;
  long live=RAW(PROXY_SC_tgkill,expected>>32,(uint32_t)expected,0,0,0,0);
  if(live==-ESRCH)__atomic_compare_exchange_n(word,&expected,0,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
  else RAW(PROXY_SC_sched_yield,0,0,0,0,0,0);
 }
 long r=erofs_call_unlocked(nr,a,handled);
 __atomic_store_n(word,0,__ATOMIC_RELEASE);RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&mask,0,8,0,0);return r;
}
/* Real directory descriptors provide fork/dup position sharing. Transfer the
 * immutable index and directory metadata needed by peer-loader exec. */
static void erofs_state(struct state *s){
 uint64_t n=0;for(struct erofs_mount *m=erofs_mounts;m;m=m->next)n++;
 n=state_word(s,n);struct erofs_mount **tail=&erofs_mounts;
 for(uint64_t i=0;i<n&&!s->error;i++){
  struct erofs_mount *m=s->reading?vfs_palloc(sizeof(*m)):*tail;
  if(!m){s->error=-ENOMEM;return;}if(s->reading){*m=(struct erofs_mount){0};*tail=m;}
  state_string(s,&m->target);state_string(s,&m->backing);state_string(s,&m->layer_directory);m->size=state_word(s,m->size);
  if(s->reading){if(m->size>s->size-s->pos){s->error=-EINVAL;return;}m->blob=vfs_palloc(m->size);if(!m->blob){s->error=-ENOMEM;return;}}
  state_bytes(s,m->blob,m->size);
  if(s->reading&&mtp_index_open_buffer(m->blob,m->size,&m->index,erofs_alloc,erofs_free,NULL)){s->error=-EINVAL;return;}
  m->io_fd=state_word(s,m->io_fd);if(!state_live(m->io_fd)){s->error=-EBADF;return;}
  if(s->reading){long area=RAW(PROXY_SC_mmap,0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,m->io_fd,0);if((unsigned long)area>=(unsigned long)-4095){s->error=area;return;}m->io_owner=(void *)area;}
  tail=&m->next;
 }
 n=0;for(struct erofs_dirfd *f=erofs_dirs;f;f=f->next)if(state_live(f->fd))n++;n=state_word(s,n);
 if(s->reading){for(uint64_t i=0;i<n&&!s->error;i++){struct erofs_dirfd *f=vfs_palloc(sizeof(*f));if(!f){s->error=-ENOMEM;return;}*f=(struct erofs_dirfd){0};f->fd=state_word(s,0);uint64_t id=state_word(s,0);struct erofs_mount *m=erofs_mounts;while(id&&m){m=m->next;id--;}f->root_dev=state_word(s,0);f->root_ino=state_word(s,0);f->root_identity=(int)state_word(s,0);char *path=NULL;state_string(s,&path);if(!m||!path||mtp_lookup(m->index,path,&f->object)||!state_live(f->fd)){s->error=-EINVAL;vfs_free(path);vfs_free(f);return;}vfs_free(path);f->mount=m;f->next=erofs_dirs;erofs_dirs=f;}}
 else for(struct erofs_dirfd *f=erofs_dirs;f;f=f->next)if(state_live(f->fd)){state_word(s,f->fd);uint64_t id=0;for(struct erofs_mount *m=erofs_mounts;m&&m!=f->mount;m=m->next)id++;state_word(s,id);state_word(s,f->root_dev);state_word(s,f->root_ino);state_word(s,f->root_identity);char *path=(char *)f->object.path;state_string(s,&path);}
}
