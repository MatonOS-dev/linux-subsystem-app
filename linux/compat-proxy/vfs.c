#define _GNU_SOURCE
/* Virtual pathname compatibility only: UID, seccomp and SELinux still constrain
 * every backing operation. No real mount or namespace is created. */
#include "proxy.h"
#include "vfs.h"
#include "links.h"
#include "sysvipc.h"
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
struct allocation {size_t size;};
void *vfs_alloc(size_t n) {
    if(n>SIZE_MAX-sizeof(struct allocation)-4095) return NULL;
    size_t bytes=(n+sizeof(struct allocation)+4095)&~(size_t)4095;
    long p=RAW(SYS_mmap,0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if((unsigned long)p>=(unsigned long)-4095) return NULL;
    struct allocation *a=(void *)p;a->size=bytes;return a+1;
}
void vfs_free(void *p) {if(p){struct allocation *a=(struct allocation *)p-1;RAW(SYS_munmap,a,a->size,0,0,0,0);}}
static char *join(const char *a,const char *b) {
    size_t x=p_len(a),y=p_len(b);if(x>SIZE_MAX-y-2) return NULL;
    char *s=vfs_alloc(x+y+2);if(!s)return NULL;
    p_copy(s,a,x);if(x && s[x-1]=='/' && b[0]=='/')x--;
    else if(y && x && s[x-1]!='/' && b[0]!='/')s[x++]='/';
    p_copy(s+x,b,y+1);return s;
}
/* Directory flocks survive fork and resident exec. Reserve their descriptors
 * from payload close/dup/fcntl so bwrap's fd sweep cannot release ownership. */
#include <sys/file.h>
struct temp_lock { int fd; struct temp_lock *next; };
static struct temp_lock *temp_locks;
int proxy_diagnostic_fd=2;
int vfs_private_fd(int fd){if(ipc_private_fd(fd))return 1;if(proxy_diagnostic_fd!=2 && fd==proxy_diagnostic_fd)return 1;for(struct temp_lock *l=temp_locks;l;l=l->next)if(l->fd==fd)return 1;return 0;}
static int temp_parent(void){extern const char *proxy_tmpdir;return (int)RAW(SYS_openat,AT_FDCWD,proxy_tmpdir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);}
static int temp_own(const char *path){
    int fd=(int)RAW(SYS_openat,AT_FDCWD,path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW,0,0,0);if(fd<0)return fd;
    struct temp_lock *l=vfs_alloc(sizeof(*l));if(!l){RAW(SYS_close,fd,0,0,0,0,0);return -ENOMEM;}
    long e=RAW(SYS_flock,fd,LOCK_EX|LOCK_NB,0,0,0,0);
    if(e){vfs_free(l);RAW(SYS_close,fd,0,0,0,0,0);return (int)e;}
    /* Distinguish new locked directories from legacy PID-only directories. */
    long marker=RAW(SYS_openat,fd,".v3-owner",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600,0,0);
    if(marker<0){vfs_free(l);RAW(SYS_close,fd,0,0,0,0,0);return (int)marker;}
    RAW(SYS_close,marker,0,0,0,0,0);l->fd=fd;l->next=temp_locks;temp_locks=l;return 0;
}
static int temp_remove_contents(int fd,unsigned depth){
    if(depth>64)return 0;
    char buf[4096];long n;
    while((n=RAW(SYS_getdents64,fd,buf,sizeof(buf),0,0,0))>0){
        for(long off=0;off<n;){unsigned short len=*(unsigned short *)(buf+off+16);if(len<20||off+len>n)return 0;
            char *name=buf+off+19;off+=len;if(p_equal(name,".")||p_equal(name,".."))continue;
            long child=RAW(SYS_openat,fd,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);
            if(child>=0){int ok=temp_remove_contents((int)child,depth+1);RAW(SYS_close,child,0,0,0,0,0);if(!ok)return 0;RAW(SYS_unlinkat,fd,name,AT_REMOVEDIR,0,0,0);}
            else RAW(SYS_unlinkat,fd,name,0,0,0,0);
        }
    }return n==0;
}
void vfs_temp_gc(void){
    int parent=temp_parent();if(parent<0)return;
    if(RAW(SYS_flock,parent,LOCK_EX,0,0,0,0)<0){RAW(SYS_close,parent,0,0,0,0,0);return;}
    char buf[4096];long n;
    while((n=RAW(SYS_getdents64,parent,buf,sizeof(buf),0,0,0))>0){
        for(long off=0;off<n;){unsigned short len=*(unsigned short *)(buf+off+16);if(len<20||off+len>n)break;char *name=buf+off+19;off+=len;
            const char *tag="compat-proxy-";unsigned k=0;while(tag[k]&&name[k]==tag[k])k++;if(tag[k])continue;
            long pid=0;unsigned start=k;for(;name[k]>='0'&&name[k]<='9'&&pid<2147483647;k++)pid=pid*10+name[k]-'0';
            if(k==start||name[k]!='-'||pid<=0||pid>2147483647)continue;
            int dir=(int)RAW(SYS_openat,parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0,0,0);if(dir<0)continue;
            if(RAW(SYS_flock,dir,LOCK_EX|LOCK_NB,0,0,0,0)<0){RAW(SYS_close,dir,0,0,0,0,0);continue;}
            long marker=RAW(SYS_openat,dir,".v3-owner",O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0,0,0);
            int stale=marker>=0;
            if(marker>=0)RAW(SYS_close,marker,0,0,0,0,0);
            else if(marker==-ENOENT){
                /* Legacy names have no birth token. Keep them if this PID
                 * exists (including reuse); only ENOENT proves them stale. */
                char proc[40]="/proc/",digits[24];unsigned count=0;unsigned long value=(unsigned long)pid;
                do{digits[count++]=(char)('0'+value%10);value/=10;}while(value);
                unsigned p=6;while(count)proc[p++]=digits[--count];proc[p]=0;
                long live=RAW(SYS_openat,AT_FDCWD,proc,O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,0,0);
                stale=live==-ENOENT;if(live>=0)RAW(SYS_close,live,0,0,0,0,0);
            }
            if(stale&&temp_remove_contents(dir,0))RAW(SYS_unlinkat,parent,name,AT_REMOVEDIR,0,0,0);
            RAW(SYS_close,dir,0,0,0,0,0);
        }
    }
    RAW(SYS_close,parent,0,0,0,0,0);
}

static char *dupstr(const char *s){size_t n=p_len(s);char *p=vfs_alloc(n+1);if(p)p_copy(p,s,n+1);return p;}
struct mount {struct mount *next;char *source,*target;unsigned long flags;};
static struct mount *table;
static char *gpu_poll_sources[2];
static char *root,*executable,*cwd;
static int stacked_oldroot;
void vfs_executable(const char *path){vfs_free(executable);executable=dupstr(path);}
char *vfs_exe(void){return executable?dupstr(executable):NULL;}
/* Cache only mount lookup, never filesystem existence or symlink contents.
 * Entries own their keys; mutations invalidate them before freeing mounts. */
struct cached {char *path;struct mount *mount;};
static struct cached cache[16];static unsigned cursor;
static void invalidate(void){for(unsigned i=0;i<16;i++){vfs_free(cache[i].path);cache[i].path=NULL;cache[i].mount=NULL;}}

static int prefix(const char *p,const char *s){size_t n=p_len(s);size_t i=0;while(i<n && p[i]==s[i])i++;return i==n && (n==1 || !p[n] || p[n]=='/');}
static struct mount *lookup(const char *path){for(unsigned i=0;i<16;i++)if(cache[i].path && p_equal(cache[i].path,path))return cache[i].mount;
    struct mount *best=NULL;size_t n=0;for(struct mount *m=table;m;m=m->next){size_t k=p_len(m->target);if(prefix(path,m->target)&&k>n){n=k;best=m;}}unsigned slot=cursor++%16;vfs_free(cache[slot].path);cache[slot].path=dupstr(path);cache[slot].mount=best;return best;}
static char *physical(const char *path){struct mount *m=lookup(path);if(!m)return dupstr(path);return join(m->source,path+(p_len(m->target)==1?0:p_len(m->target)));}
char *vfs_reverse(const char *path){
    struct mount *best=NULL;size_t n=0;
    /* Prefer names reachable in this process's current root. Detached old
     * roots may still have the same backing objects via inherited fds. */
    for(unsigned pass=0;pass<2 && !best;pass++)for(struct mount *m=table;m;m=m->next){
        if(!pass && root && !prefix(m->target,root))continue;
        size_t k=p_len(m->source);
        if(prefix(path,m->source)&&(k>n || (k==n && best && p_len(m->target)<p_len(best->target)))){best=m;n=k;}
    }
    char *p=best?join(best->target,path+(n==1?0:n)):dupstr(path);
    if(p && root && prefix(p,root)){char *q=dupstr(p+p_len(root));vfs_free(p);p=q;if(p && !*p){vfs_free(p);p=dupstr("/");}}
    return p;
}
static char *readlink_dynamic(const char *path,long *error){size_t n=256;for(;;){char *s=vfs_alloc(n+1);if(!s){*error=-ENOMEM;return NULL;}long r=RAW(SYS_readlinkat,AT_FDCWD,path,s,n,0,0);if(r<0){vfs_free(s);*error=r;return NULL;}if((size_t)r<n){s[r]=0;return s;}vfs_free(s);if(n>SIZE_MAX/2-1){*error=-EOVERFLOW;return NULL;}n*=2;}}
struct fd_name {struct fd_name *next;int fd;char *path;};
static struct fd_name *fd_names;
void vfs_fd_close(int fd){struct fd_name **p=&fd_names;while(*p && (*p)->fd!=fd)p=&(*p)->next;if(*p){struct fd_name *old=*p;*p=old->next;vfs_free(old->path);vfs_free(old);}}
void vfs_fd_dup(int from,int to){char *name=vfs_fd_name(from);vfs_fd_close(to);if(name){vfs_fd_record(to,AT_FDCWD,name);vfs_free(name);}}
char *vfs_fd_name(int fd){char *published=links_fd_name(fd);if(published)return published;for(struct fd_name *p=fd_names;p;p=p->next)if(p->fd==fd){
    if(!root)return dupstr(p->path);
    if(!prefix(p->path,root))return NULL;
    const char *name=p->path+p_len(root);return dupstr(*name?name:"/");
}
return NULL;}
static int self_proc(const char *path){
    if(prefix(path,"/proc/self"))return 1;
    char name[64]="/proc/",digits[24];size_t k=6;unsigned d=0;
    unsigned long pid=(unsigned long)RAW(SYS_getpid,0,0,0,0,0,0);
    do{digits[d++]=(char)('0'+pid%10);pid/=10;}while(pid);
    while(d)name[k++]=digits[--d];
    name[k]=0;return prefix(path,name);
}
char *vfs_proc_fd_name(const char *path){
    if(!self_proc(path))return NULL;
    for(size_t i=0;path[i];i++)if(path[i]=='/' && path[i+1]=='f' && path[i+2]=='d' && path[i+3]=='/'){
        const char *n=path+i+4;unsigned fd=0;if(!*n)return NULL;
        while(*n>='0' && *n<='9'){if(fd>214748364U)return NULL;fd=fd*10+(*n++-'0');}
        if(*n)return NULL;
        return vfs_fd_name((int)fd);
    }
return NULL;
}
static char *basepath(int fd,long *error){if(fd==AT_FDCWD){if(cwd){char *global=root?join(root,cwd):dupstr(cwd);char *host=global?physical(global):NULL;struct stat named,actual;int valid=host && !RAW(SYS_newfstatat,AT_FDCWD,host,&named,0,0,0) && !RAW(SYS_newfstatat,AT_FDCWD,".",&actual,0,0,0) && named.st_dev==actual.st_dev && named.st_ino==actual.st_ino;vfs_free(global);vfs_free(host);if(valid)return dupstr(cwd);vfs_free(cwd);cwd=NULL;}size_t n=256;for(;;){char *s=vfs_alloc(n);if(!s){*error=-ENOMEM;return NULL;}long r=RAW(SYS_getcwd,s,n,0,0,0,0);if(r>=0){char *q=vfs_reverse(s);vfs_free(s);return q;}vfs_free(s);if(r!=-ERANGE){*error=r;return NULL;}if(n>SIZE_MAX/2){*error=-EOVERFLOW;return NULL;}n*=2;}}
    char *tracked=vfs_fd_name(fd);if(tracked)return tracked;
    struct stat st;long status=RAW(SYS_fstat,fd,&st,0,0,0,0);if(status<0){*error=status;return NULL;}if(!S_ISDIR(st.st_mode)){*error=-ENOTDIR;return NULL;}
    char p[64]="/proc/self/fd/",digits[24];size_t k=14;unsigned d=0;unsigned long v=(unsigned)fd;do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);while(d)p[k++]=digits[--d];p[k]=0;
    char *s=readlink_dynamic(p,error);if(!s)return NULL;char *q=vfs_reverse(s);vfs_free(s);return q;
}
char *vfs_resolve_mount(int fd,const char *path,int follow,long *error,void **identity){
    *error=0;if(!path){*error=-EFAULT;return NULL;}if(!*path){*error=-ENOENT;return NULL;}
    /* Polling roots are immutable generated directories. Re-check the current
     * mount source so an unmount/overmount cannot reuse a stale fast answer. */
    if(p_equal(path,"/sys/bus") || p_equal(path,"/sys/class")){
        unsigned slot=p_equal(path,"/sys/class");
        if(gpu_poll_sources[slot]){char *global=root?join(root,path):dupstr(path);struct mount *m=global?lookup(global):NULL;
            int hit=m && p_equal(m->target,global) && p_equal(m->source,gpu_poll_sources[slot]);vfs_free(global);
            if(hit){if(identity)*identity=m;char *answer=dupstr(m->source);if(!answer)*error=-ENOMEM;return answer;}}
    }
    char *input;
    if(path[0]=='/')input=dupstr(path);else {char *base=basepath(fd,error);if(!base)return NULL;input=join(base,path);vfs_free(base);}
    if(!input){*error=-ENOMEM;return NULL;}
    unsigned links=0;
restart:;
    char *out=vfs_alloc(p_len(input)+2);if(!out){vfs_free(input);*error=-ENOMEM;return NULL;}size_t used=1;out[0]='/';out[1]=0;
    for(size_t i=0;input[i];){while(input[i]=='/')i++;if(!input[i])break;size_t start=i;while(input[i] && input[i]!='/')i++;size_t len=i-start;
        if(len==1 && input[start]=='.')continue;
        if(len==2 && input[start]=='.' && input[start+1]=='.'){while(used>1 && out[used-1]!='/')used--;if(used>1)used--;out[used]=0;continue;}
        if(used>1)out[used++]='/';
        p_copy(out+used,input+start,len);used+=len;out[used]=0;
        if(p_equal(out+used-len,".v3-inodes")){*error=-ENOENT;goto fail;}
        size_t next=i;while(input[next]=='/')next++;
        if(!follow && !input[next] && input[i]!='/' )continue;
        char *global=root?join(root,out):dupstr(out);char *host=global?physical(global):NULL;vfs_free(global);if(!host){*error=-ENOMEM;goto fail;}
        links_recover_path(host);
        long e=0;char *target=readlink_dynamic(host,&e);
        if(!target && input[i]=='/'){struct stat st;long status=RAW(SYS_newfstatat,AT_FDCWD,host,&st,0,0,0);if(status<0){vfs_free(host);*error=status;goto fail;}if(!S_ISDIR(st.st_mode)){vfs_free(host);*error=-ENOTDIR;goto fail;}}
        /* proc fd magic links point to backing objects, outside a pivoted
         * pathname root. They are not ordinary payload absolute symlinks. */
        if(target && prefix(host,"/proc") && !p_equal(out+ (used>=4?used-4:0),"/exe")){
            int magic=0;for(size_t j=0;host[j];j++)if(host[j]=='/' && host[j+1]=='f' && host[j+2]=='d' && host[j+3]=='/')magic=1;
            if(magic && !input[i] && follow==VFS_FOLLOW_LINK_SOURCE){
                /* Keep the proc magic link itself. readlink text for O_TMPFILE
                 * is a nonexistent "#inode (deleted)" name, not the inode. */
                if(identity){char *name=vfs_proc_fd_name(out);if(!name)name=vfs_reverse(target);char *global=name?(root?join(root,name):dupstr(name)):NULL;
                    *identity=global?lookup(global):NULL;vfs_free(global);vfs_free(name);}
                vfs_free(target);vfs_free(input);vfs_free(out);return host;
            }
            if(magic){char *answer=join(target,input+i);vfs_free(target);vfs_free(host);vfs_free(input);vfs_free(out);if(!answer)*error=-ENOMEM;return answer;}
        }
        if(target && links_hidden(target)){
            char internal[4096];int valid=links_target(host,internal);
            vfs_free(target);target=NULL;e=-EINVAL;
            if(!valid){vfs_free(host);*error=-ENOENT;goto fail;}
        }
        vfs_free(host);
        if(target && self_proc(out) && used>=4 && p_equal(out+used-4,"/exe") && executable){vfs_free(target);target=dupstr(executable);}
        if(target){if(++links>40){vfs_free(target);*error=-ELOOP;goto fail;}char *head;
            if(target[0]=='/')head=dupstr(target);else {used-=len;if(used>1)used--;out[used]=0;head=join(out,target);}vfs_free(target);
            char *replacement=head?join(head,input+i):NULL;vfs_free(head);if(!replacement){*error=-ENOMEM;goto fail;}vfs_free(input);vfs_free(out);input=replacement;goto restart;
        }else if(e!=-EINVAL && e!=-ENOENT && e!=-ENOTDIR){*error=e;goto fail;}
    }
    {char *global=root?join(root,out):dupstr(out);if(identity)*identity=global?lookup(global):NULL;char *host=global?physical(global):NULL;vfs_free(global);if(host && input[p_len(input)-1]=='/' && !p_equal(host,"/")){char *q=join(host,"/");vfs_free(host);host=q;}vfs_free(input);vfs_free(out);if(!host)*error=-ENOMEM;return host;}
fail:vfs_free(input);vfs_free(out);return NULL;
}
char *vfs_resolve(int fd,const char *path,int follow,long *error){return vfs_resolve_mount(fd,path,follow,error,NULL);}
/* A bind source that is a regular file stays referenced by its virtual mount
 * (Linux keeps the inode alive after bwrap unlinks its bind-data temp file). */
int vfs_is_mount_source(const char *path){for(struct mount *m=table;m;m=m->next)if(p_equal(m->source,path))return 1;return 0;}
long vfs_mount_readonly(void *identity){struct mount *m=identity;return m && (m->flags&MS_RDONLY)?-EROFS:0;}
int proxy_mount(const char *source,const char *target){if(!source || !target || target[0]!='/')return -EINVAL;struct mount *m=vfs_alloc(sizeof(*m));if(!m)return -ENOMEM;m->source=dupstr(source);m->target=dupstr(target);m->flags=0;if(!m->source || !m->target){vfs_free(m->source);vfs_free(m->target);vfs_free(m);return -ENOMEM;}size_t n=p_len(m->target);while(n>1 && m->target[n-1]=='/')m->target[--n]=0;invalidate();m->next=table;table=m;
    proxy_log("mount-owner",SYS_mount,0);
    RAW(SYS_write,proxy_diagnostic_fd,"v3 mount ",9,0,0,0);RAW(SYS_write,proxy_diagnostic_fd,source,p_len(source),0,0,0);RAW(SYS_write,proxy_diagnostic_fd," -> ",4,0,0,0);RAW(SYS_write,proxy_diagnostic_fd,target,p_len(target),0,0,0);RAW(SYS_write,proxy_diagnostic_fd,"\n",1,0,0,0);return 0;}
const char *proxy_path(const char *path,char *buffer,size_t size){long e;char *p=vfs_resolve(AT_FDCWD,path,1,&e);if(!p)return NULL;size_t n=p_len(p);if(n>=size){vfs_free(p);return NULL;}p_copy(buffer,p,n+1);vfs_free(p);return buffer;}
static char *global_target(const char *target,long *error){
    char *virtual;
    if(target[0]=='/')virtual=dupstr(target);
    else {char *base=basepath(AT_FDCWD,error);if(!base)return NULL;virtual=join(base,target);vfs_free(base);}
    if(!virtual){*error=-ENOMEM;return NULL;}
    char *p=root?join(root,virtual):dupstr(virtual);vfs_free(virtual);
    if(!p){*error=-ENOMEM;return NULL;}
    /* lexical dot normalization for mount names, with no backing resolution */
    char *out=vfs_alloc(p_len(p)+2);if(!out){vfs_free(p);*error=-ENOMEM;return NULL;}size_t n=1;out[0]='/';out[1]=0;
    for(size_t i=0;p[i];){while(p[i]=='/')i++;size_t start=i;while(p[i]&&p[i]!='/')i++;size_t len=i-start;if(!len)break;if(len==1&&p[start]=='.')continue;if(len==2&&p[start]=='.'&&p[start+1]=='.'){while(n>1&&out[n-1]!='/')n--;if(n>1)n--;out[n]=0;continue;}if(n>1)out[n++]='/';p_copy(out+n,p+start,len);n+=len;out[n]=0;}vfs_free(p);return out;
}
void vfs_fd_record(int fd,int dirfd,const char *path){
    long e=0;char *global=global_target(path,&e);
    if(path[0]!='/'){
        char *base=basepath(dirfd,&e);vfs_free(global);
        char *relative=base?join(base,path):NULL;vfs_free(base);
        global=relative?global_target(relative,&e):NULL;vfs_free(relative);
    }
    if(!global)return;
    char *logical=global;
    struct fd_name *p=fd_names;while(p && p->fd!=fd)p=p->next;
    if(!p){p=vfs_alloc(sizeof(*p));if(!p){vfs_free(logical);return;}p->fd=fd;p->path=NULL;p->next=fd_names;fd_names=p;}
    vfs_free(p->path);p->path=logical;
}
char *vfs_temp_create(long *error){
    extern const char *proxy_tmpdir;
    static unsigned long sequence;
    char suffix[80]="compat-proxy-";size_t k=13;
    unsigned long values[2]={(unsigned long)RAW(SYS_getpid,0,0,0,0,0,0),0};
    RAW(SYS_getrandom,&values[1],sizeof(values[1]),0,0,0,0);values[1]^=++sequence;
    for(unsigned j=0;j<2;j++){char d[24];unsigned n=0;do{d[n++]=(char)('0'+values[j]%10);values[j]/=10;}while(values[j]);while(n)suffix[k++]=d[--n];suffix[k++]=j?0:'-';}
    char *src=join(proxy_tmpdir,suffix);if(!src){*error=-ENOMEM;return NULL;}
    int parent=temp_parent();if(parent<0)*error=parent;
    else{*error=RAW(SYS_flock,parent,LOCK_EX,0,0,0,0);if(!*error)*error=RAW(SYS_mkdirat,AT_FDCWD,src,0700,0,0,0);if(!*error)*error=temp_own(src);RAW(SYS_close,parent,0,0,0,0,0);}
    if(*error){vfs_free(src);return NULL;}return src;
}
long vfs_mount(const char *source,const char *target,const char *type,unsigned long flags){
    if(flags&(MS_PRIVATE|MS_SLAVE|MS_SHARED|MS_UNBINDABLE))return 0;
    long e=0;char *canonical=NULL;
    if(prefix(target,"/proc")){
        char *backing=vfs_resolve(AT_FDCWD,target,1,&e);
        if(!backing)return e;
        canonical=vfs_proc_fd_name(target);if(!canonical)canonical=vfs_reverse(backing);vfs_free(backing);if(!canonical)return -ENOMEM;
    }
    char *dest=global_target(canonical?canonical:target,&e);vfs_free(canonical);if(!dest)return e;
    if(flags&MS_REMOUNT){struct mount *m=lookup(dest);if(!m)e=-EINVAL;else m->flags=flags;vfs_free(dest);return e;}
    char *src=NULL;
    if(flags&MS_BIND)src=vfs_resolve(AT_FDCWD,source,1,&e);
    else if(type && p_equal(type,"proc"))src=dupstr("/proc");
    else if(type && p_equal(type,"devpts"))src=dupstr("/dev/pts");
    else if(type && p_equal(type,"mqueue"))src=dupstr("/dev/mqueue");
    else if(type && p_equal(type,"tmpfs")){
        src=vfs_temp_create(&e);
    }else e=-ENOSYS;
    if(!e && !src)e=-ENOMEM;
    if(!e){
        struct mount *prior=table;char *virtual_source=(flags&MS_BIND)?global_target(source,&e):NULL;
        if(!e)e=proxy_mount(src,dest);
        if(!e)table->flags=flags;
        if(!e && virtual_source && (flags&MS_REC))for(struct mount *m=prior;m;m=m->next){if(prefix(m->target,virtual_source) && !p_equal(m->target,virtual_source)){
            char *child=join(dest,m->target+p_len(virtual_source));if(!child){e=-ENOMEM;break;}e=proxy_mount(m->source,child);vfs_free(child);if(e)break;table->flags=m->flags;
        }}
        vfs_free(virtual_source);
    }vfs_free(src);vfs_free(dest);return e;
}
long vfs_unmount(const char *target){RAW(SYS_write,proxy_diagnostic_fd,"v3 unmount ",11,0,0,0);RAW(SYS_write,proxy_diagnostic_fd,target,p_len(target),0,0,0);RAW(SYS_write,proxy_diagnostic_fd,"\n",1,0,0,0);if(stacked_oldroot && p_equal(target,".")){stacked_oldroot=0;return 0;}long e=0;char *dest=global_target(target,&e);if(!dest)return e;struct mount **p=&table;while(*p && !p_equal((*p)->target,dest))p=&(*p)->next;vfs_free(dest);if(!*p)return p_equal(target,".")?0:-EINVAL;invalidate();struct mount *m=*p;*p=m->next;vfs_free(m->source);vfs_free(m->target);vfs_free(m);return 0;}
long vfs_root(const char *target){long e=0;char *next=global_target(target,&e);if(!next)return e;char *host=physical(next);if(!host){vfs_free(next);return -ENOMEM;}long fd=RAW(SYS_openat,AT_FDCWD,host,O_DIRECTORY|O_PATH|O_CLOEXEC,0,0,0);vfs_free(host);if(fd<0){vfs_free(next);return fd;}RAW(SYS_close,fd,0,0,0,0,0);invalidate();vfs_free(root);root=next;vfs_free(cwd);cwd=NULL;return 0;}
long vfs_pivot(const char *target,const char *old){long e=0;char *next=global_target(target,&e);if(!next)return e;RAW(SYS_write,proxy_diagnostic_fd,"v3 pivot ",9,0,0,0);RAW(SYS_write,proxy_diagnostic_fd,next,p_len(next),0,0,0);RAW(SYS_write,proxy_diagnostic_fd,"\n",1,0,0,0);
    stacked_oldroot=p_equal(target,old);
    if(!p_equal(target,old)){char *oldhost=physical(root?root:"/");char *dest=global_target(old,&e);if(!dest || !oldhost){vfs_free(next);vfs_free(dest);vfs_free(oldhost);return -ENOMEM;}struct mount *prior=table;
        e=proxy_mount(oldhost,dest);
        if(!e)for(struct mount *m=prior;m;m=m->next){
            const char *relative=m->target;
            if(root){if(!prefix(relative,root))continue;relative+=p_len(root);}
            if(!*relative || p_equal(relative,"/"))continue;
            char *alias=join(dest,relative);if(!alias){e=-ENOMEM;break;}
            e=proxy_mount(m->source,alias);vfs_free(alias);if(e)break;table->flags=m->flags;
        }
        vfs_free(dest);vfs_free(oldhost);if(e){vfs_free(next);return e;}}
    invalidate();vfs_free(root);root=next;vfs_free(cwd);cwd=NULL;return 0;
}
long vfs_proc_open(const char *path){size_t n=p_len(path);if(n<10 || !p_equal(path+n-10,"/mountinfo") || !table)return -ENOSYS;
    long fd=RAW(SYS_memfd_create,"v3-mountinfo",1,0,0,0,0);if(fd<0)return fd;
    const char *header="1 0 0:1 / ",*middle=" rw,nosuid - tmpfs compat-proxy rw\n";
    /* Virtual view required by bwrap's post-bind verification. Stable IDs are
     * not yet exposed: this is a setup-time snapshot. */
    for(struct mount *m=table;m;m=m->next){const char *t=m->target;
        if(root && !prefix(t,root))continue;
        if(root)t+=p_len(root);
        if(!*t)t="/";
        RAW(SYS_write,fd,header,p_len(header),0,0,0);RAW(SYS_write,fd,t,p_len(t),0,0,0);RAW(SYS_write,fd,middle,p_len(middle),0,0,0);
    }
    RAW(SYS_lseek,fd,0,0,0,0,0);return fd;
}
long vfs_chdir(int fd,const char *path){long e=0;void *identity=NULL;char *host;
    if(path)host=vfs_resolve_mount(fd,path,1,&e,&identity);
    else {char proc[64]="/proc/self/fd/",digits[24];size_t k=14;unsigned d=0;unsigned long v=(unsigned)fd;do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);while(d)proc[k++]=digits[--d];proc[k]=0;host=readlink_dynamic(proc,&e);}
    if(!host)return e;
    e=path?RAW(SYS_chdir,host,0,0,0,0,0):RAW(SYS_fchdir,fd,0,0,0,0,0);
    if(!e){struct mount *m=identity;char *logical=!path?vfs_fd_name(fd):NULL;if(!logical)logical=m?join(m->target,host+(p_len(m->source)==1?0:p_len(m->source))):vfs_reverse(host);
        if(logical && m && root && prefix(logical,root)){char *q=dupstr(logical+p_len(root));vfs_free(logical);logical=q;if(logical && !*logical){vfs_free(logical);logical=dupstr("/");}}
        if(!logical)e=-ENOMEM;else {vfs_free(cwd);cwd=logical;}}
    vfs_free(host);return e;
}
char *vfs_getcwd(long *error){return basepath(AT_FDCWD,error);}
long vfs_bootstrap_root(const char *directory){
    /* CLI roots are host-side symlink scaffolds, not payload symlinks. Pin
     * their existing top-level endpoints before virtual resolution begins. */
    const char *names[]={"usr","bin","lib","lib64","etc","sbin","proc","dev","data","system","apex"};
    char *sources[11]={0};long error=0;
    for(unsigned i=0;i<11;i++){
        char *path=join(directory,names[i]);if(!path){error=-ENOMEM;break;}
        long fd=RAW(SYS_openat,AT_FDCWD,path,O_PATH|O_CLOEXEC,0,0,0);vfs_free(path);
        if(fd<0){if(fd!=-ENOENT)error=fd;continue;}
        char proc[64]="/proc/self/fd/",digits[24];unsigned d=0;size_t k=14;unsigned long v=(unsigned long)fd;do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);while(d)proc[k++]=digits[--d];proc[k]=0;
        sources[i]=readlink_dynamic(proc,&error);RAW(SYS_close,fd,0,0,0,0,0);if(!sources[i])break;
    }
    if(!error)error=proxy_mount(directory,"/");
    for(unsigned i=0;i<11;i++){if(!error && sources[i]){char *target=join("/",names[i]);if(!target)error=-ENOMEM;else error=proxy_mount(sources[i],target);vfs_free(target);}vfs_free(sources[i]);}
    return error;
}
/* GPU identity comes only from pc-gpu-detect's property snapshot, never PCI
 * sysfs. A private read-only virtual mount uses the existing VFS resolver for
 * all path, stat, readlink and directory operations (including relative fds).
 * Bionic's v0xfc6ed0ab property area ABI: 128-byte header, offset trie,
 * 96-byte short prop_info. Fail closed on unknown formats/dirty snapshots. */
static uint32_t gpu_u32(const char *p){return __atomic_load_n((const uint32_t *)p,__ATOMIC_ACQUIRE);}
static int gpu_property(const char *area,size_t size,const char *name,char value[92]){
    if(size<148 || gpu_u32(area+8)!=0x504f5250 || gpu_u32(area+12)!=0xfc6ed0ab)return 0;
    const char *data=area+128;size_t bytes=size-128;uint32_t node=0;
    const char *token=name;
    for(size_t budget=bytes/20;budget;){
        if(node>bytes-20 || (node&3))return 0;
        uint32_t offset=gpu_u32(data+node+16);size_t len=0;while(token[len] && token[len]!='.')len++;
        while(offset){
            if(!budget || offset>bytes-21 || (offset&3))return 0;
            budget--;
            uint32_t n=gpu_u32(data+offset);if(n>bytes-offset-21 || data[offset+20+n])return 0;
            int cmp=len<n?-1:len>n?1:0;
            if(!cmp)for(size_t j=0;j<len;j++)if(token[j]!=data[offset+20+j]){cmp=(unsigned char)token[j]-(unsigned char)data[offset+20+j];break;}
            if(!cmp)break;
            offset=gpu_u32(data+offset+(cmp<0?8:12));
        }
        if(!offset)return 0;
        node=offset;token+=len;
        if(*token=='.'){token++;continue;}
        offset=gpu_u32(data+node+4);
        size_t namelen=p_len(name);
        if(!offset || (offset&3) || offset>bytes || bytes-offset<97+namelen)return 0;
        for(size_t j=0;j<=namelen;j++)if(data[offset+96+j]!=name[j])return 0;
        uint32_t serial=gpu_u32(data+offset);unsigned n=serial>>24;
        if((serial&0x10001) || n>=92)return 0;
        p_copy(value,data+offset+4,n);value[n]=0;
        return serial==gpu_u32(data+offset);
    }
    return 0;
}
static size_t gpu_number(char *out,unsigned long number){char rev[24];size_t n=0,k=0;do{rev[n++]=(char)('0'+number%10);number/=10;}while(number);while(n)out[k++]=rev[--n];out[k]=0;return k;}
static long gpu_dir(const char *base,const char *name){char *p=join(base,name);if(!p)return -ENOMEM;long r=RAW(SYS_mkdirat,AT_FDCWD,p,0700,0,0,0);vfs_free(p);return r;}
static long gpu_file(const char *base,const char *name,const char *value){char *p=join(base,name);if(!p)return -ENOMEM;long fd=RAW(SYS_openat,AT_FDCWD,p,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0400,0,0);vfs_free(p);if(fd<0)return fd;size_t n=p_len(value);long r=RAW(SYS_write,fd,value,n,0,0,0);RAW(SYS_close,fd,0,0,0,0,0);return r==(long)n?0:r<0?r:-EIO;}
static long gpu_link(const char *base,const char *name,const char *target){char *p=join(base,name);if(!p)return -ENOMEM;long r=RAW(SYS_symlinkat,target,AT_FDCWD,p,0,0,0);vfs_free(p);return r;}
static int gpu_hex(const char *p){if(p_len(p)!=6 || p[0]!='0' || p[1]!='x')return 0;for(unsigned i=2;i<6;i++)if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')||(p[i]>='A'&&p[i]<='F')))return 0;return 1;}
static void gpu_append(char *dst,const char *src){p_copy(dst+p_len(dst),src,p_len(src)+1);}
static long gpu_node(const char *base,const char *name,unsigned minor){
    char child[128]="devices/pci0000:00/0000:00:00.0/drm/";p_copy(child+p_len(child),name,p_len(name)+1);
    long r=gpu_dir(base,child);if(r)return r;
    char *dir=join(base,child);if(!dir)return -ENOMEM;
    char number[24];gpu_number(number,minor);char event[160]="MAJOR=226\nMINOR=";
    gpu_append(event,number);gpu_append(event,"\nDEVNAME=dri/");gpu_append(event,name);gpu_append(event,"\n");
    r=gpu_file(dir,"uevent",event);if(!r)r=gpu_link(dir,"device","../..");if(!r)r=gpu_link(dir,"subsystem","../../../../../class/drm");vfs_free(dir);if(r)return r;
    char alias[64]="dev/char/226:";p_copy(alias+p_len(alias),number,p_len(number)+1);
    char target[160]="../../devices/pci0000:00/0000:00:00.0/drm/";p_copy(target+p_len(target),name,p_len(name)+1);
    r=gpu_link(base,alias,target);if(r)return r;
    char cls[128]="class/drm/";gpu_append(cls,name);
    return gpu_link(base,cls,target);
}
/* Separate entry point also lets host fixtures exercise precisely the tree
 * publication code without requiring a host GPU or Android property service. */
long vfs_gpu_publish(const char *base,const char *vendor,const char *device,const char *driver,const char *render,unsigned render_minor,const char *card,unsigned card_minor){
    const char *dirs[]={"dev","dev/char","devices","devices/pci0000:00","devices/pci0000:00/0000:00:00.0","devices/pci0000:00/0000:00:00.0/drm","bus","bus/pci","class","class/drm"};
    long r=0;for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++)if((r=gpu_dir(base,dirs[i])))return r;
    char *pci=join(base,"devices/pci0000:00/0000:00:00.0");if(!pci)return -ENOMEM;
    const char *names[]={"vendor","device","subsystem_vendor","subsystem_device","revision","class"};
    const char *values[]={vendor,device,"0x0000","0x0000","0x00","0x030000"};
    for(unsigned i=0;i<6 && !r;i++){char line[16];p_copy(line,values[i],p_len(values[i]));p_copy(line+p_len(values[i]),"\n",2);r=gpu_file(pci,names[i],line);}
    char event[256]="DRIVER=",id[5];gpu_append(event,driver);gpu_append(event,"\nPCI_ID=");
    p_copy(id,vendor+2,4);id[4]=0;gpu_append(event,id);gpu_append(event,":");
    p_copy(id,device+2,4);gpu_append(event,id);gpu_append(event,"\nPCI_SLOT_NAME=0000:00:00.0\n");
    if(!r)r=gpu_file(pci,"uevent",event);
    if(!r)r=gpu_link(pci,"subsystem","../../../bus/pci");
    vfs_free(pci);
    if(!r)r=gpu_node(base,render,render_minor);
    if(!r && card)r=gpu_node(base,card,card_minor);
    return r;
}
void vfs_gpu_init(void){
    long e=0;char *path=vfs_resolve(AT_FDCWD,"/dev/__properties__/u:object_r:vendor_maton_graphics_prop:s0",1,&e);if(!path)return;
    long fd=RAW(SYS_openat,AT_FDCWD,path,O_RDONLY|O_CLOEXEC,0,0,0);vfs_free(path);if(fd<0)return;
    struct stat st;if(RAW(SYS_fstat,fd,&st,0,0,0,0) || st.st_size<148){RAW(SYS_close,fd,0,0,0,0,0);return;}
    char *area=(void *)RAW(SYS_mmap,0,st.st_size,PROT_READ,MAP_SHARED,fd,0);RAW(SYS_close,fd,0,0,0,0,0);if((unsigned long)area>=(unsigned long)-4095)return;
    char vendor[92],device[92],driver[92],render[92],mode[92];
    int ok=gpu_property(area,st.st_size,"vendor.maton.graphics.vendor",vendor)&&gpu_property(area,st.st_size,"vendor.maton.graphics.device",device)&&gpu_property(area,st.st_size,"vendor.maton.graphics.driver",driver)&&gpu_property(area,st.st_size,"vendor.maton.graphics.render_node",render)&&gpu_property(area,st.st_size,"vendor.maton.graphics.mode",mode);
    RAW(SYS_munmap,area,st.st_size,0,0,0,0);
    if(!ok || !p_equal(mode,"hardware") || !gpu_hex(vendor) || !gpu_hex(device) || !driver[0] || p_equal(driver,"unknown") || p_len(render)<17)return;
    for(size_t i=0;driver[i];i++)if(!((driver[i]>='a'&&driver[i]<='z')||(driver[i]>='0'&&driver[i]<='9')||driver[i]=='_'||driver[i]=='-'))return;
    const char *prefix="/dev/dri/renderD";for(unsigned i=0;i<16;i++)if(render[i]!=prefix[i])return;
    for(unsigned i=16;render[i];i++)if(render[i]<'0'||render[i]>'9')return;
    path=vfs_resolve(AT_FDCWD,render,1,&e);if(!path)return;
    fd=RAW(SYS_openat,AT_FDCWD,path,O_RDONLY|O_CLOEXEC,0,0,0);vfs_free(path);if(fd<0)return;
    e=RAW(SYS_fstat,fd,&st,0,0,0,0);RAW(SYS_close,fd,0,0,0,0,0);
    if(e)return;
    unsigned maj=(st.st_rdev>>8)&0xfff,min=(st.st_rdev&255)|((st.st_rdev>>12)&0xfffff00);
    if(!S_ISCHR(st.st_mode) || maj!=226 || min<128)return;
    /* Only publish a card when /dev/dri has exactly one primary and one render node. The
     * detector does not export its card identity; render/card indices are
     * not necessarily paired. Never invent a multi-GPU association. */
    char card[64]={0};unsigned card_minor=0,count=0,render_count=0;
    path=vfs_resolve(AT_FDCWD,"/dev/dri",1,&e);
    long dri=path?RAW(SYS_openat,AT_FDCWD,path,O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,0,0):-ENOENT;vfs_free(path);
    if(dri>=0){
        char buf[4096];long n;
        while((n=RAW(SYS_getdents64,dri,buf,sizeof(buf),0,0,0))>0){
            for(long off=0;off<n;){
                unsigned short len=*(unsigned short *)(buf+off+16);
                if(len<20 || len>n-off)break;
                const char *name=buf+off+19;
                int primary=name[0]=='c'&&name[1]=='a'&&name[2]=='r'&&name[3]=='d';
                int rendering=name[0]=='r'&&name[1]=='e'&&name[2]=='n'&&name[3]=='d';
                if((primary || rendering) && p_len(name)<sizeof(card)){
                    long c=RAW(SYS_openat,dri,name,O_RDONLY|O_CLOEXEC,0,0,0);struct stat cs;
                    if(c>=0){
                        if(!RAW(SYS_fstat,c,&cs,0,0,0,0) && S_ISCHR(cs.st_mode) && ((cs.st_rdev>>8)&0xfff)==226){
                            unsigned cm=(cs.st_rdev&255)|((cs.st_rdev>>12)&0xfffff00);
                            if(primary && cm<64){count++;p_copy(card,name,p_len(name)+1);card_minor=cm;}
                            else if(rendering && cm>=128)render_count++;
                        }
                        RAW(SYS_close,c,0,0,0,0,0);
                    }
                }
                off+=len;
            }
        }
        if(n<0){count=0;render_count=0;}
        RAW(SYS_close,dri,0,0,0,0,0);
    }
    extern const char *proxy_tmpdir;char suffix[80]="v3-gpu-",number[24];gpu_number(number,RAW(SYS_getpid,0,0,0,0,0,0));p_copy(suffix+7,number,p_len(number)+1);unsigned long random=0;if(RAW(SYS_getrandom,&random,sizeof(random),0,0,0,0)!=sizeof(random))return;size_t k=p_len(suffix);suffix[k++]='-';gpu_number(suffix+k,random);
    char *base=join(proxy_tmpdir,suffix);if(!base)return;
    if(!RAW(SYS_mkdirat,AT_FDCWD,base,0700,0,0,0) && !vfs_gpu_publish(base,vendor,device,driver,render+9,min,count==1 && render_count==1?card:NULL,card_minor)){
        struct mount *prior=table;
        const char *trees[]={"dev","devices","bus","class"};
        for(unsigned i=0;i<4;i++){
            char *src=join(base,trees[i]),*dst=join("/sys",trees[i]);
            e=src&&dst?vfs_mount(src,dst,NULL,MS_BIND|MS_RDONLY):-ENOMEM;
            if(!e && i>=2){vfs_free(gpu_poll_sources[i-2]);gpu_poll_sources[i-2]=dupstr(src);}
            vfs_free(src);vfs_free(dst);if(e)break;
        }
        if(e){invalidate();while(table!=prior){struct mount *old=table;table=old->next;vfs_free(old->source);vfs_free(old->target);vfs_free(old);}for(unsigned i=0;i<2;i++){vfs_free(gpu_poll_sources[i]);gpu_poll_sources[i]=NULL;}}

    }
    vfs_free(base);
}
