#define _GNU_SOURCE
/* Virtual pathname compatibility only: UID, seccomp and SELinux still constrain
 * every backing operation. No real mount or namespace is created. */
#include "proxy.h"
#include "vfs.h"
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
static char *dupstr(const char *s){size_t n=p_len(s);char *p=vfs_alloc(n+1);if(p)p_copy(p,s,n+1);return p;}
struct mount {struct mount *next;char *source,*target;unsigned long flags;};
static struct mount *table;
static char *root,*executable,*cwd;
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
char *vfs_reverse(const char *path){struct mount *best=NULL;size_t n=0;for(struct mount *m=table;m;m=m->next){size_t k=p_len(m->source);if(prefix(path,m->source)&&k>n){best=m;n=k;}}char *p=best?join(best->target,path+(n==1?0:n)):dupstr(path);if(p && root && prefix(p,root)){char *q=dupstr(p+p_len(root));vfs_free(p);p=q;if(p && !*p){vfs_free(p);p=dupstr("/");}}return p;}
static char *readlink_dynamic(const char *path,long *error){size_t n=256;for(;;){char *s=vfs_alloc(n+1);if(!s){*error=-ENOMEM;return NULL;}long r=RAW(SYS_readlinkat,AT_FDCWD,path,s,n,0,0);if(r<0){vfs_free(s);*error=r;return NULL;}if((size_t)r<n){s[r]=0;return s;}vfs_free(s);if(n>SIZE_MAX/2-1){*error=-EOVERFLOW;return NULL;}n*=2;}}
static char *basepath(int fd,long *error){if(fd==AT_FDCWD){if(cwd)return dupstr(cwd);size_t n=256;for(;;){char *s=vfs_alloc(n);if(!s){*error=-ENOMEM;return NULL;}long r=RAW(SYS_getcwd,s,n,0,0,0,0);if(r>=0){char *q=vfs_reverse(s);vfs_free(s);return q;}vfs_free(s);if(r!=-ERANGE){*error=r;return NULL;}if(n>SIZE_MAX/2){*error=-EOVERFLOW;return NULL;}n*=2;}}
    struct stat st;long status=RAW(SYS_fstat,fd,&st,0,0,0,0);if(status<0){*error=status;return NULL;}if(!S_ISDIR(st.st_mode)){*error=-ENOTDIR;return NULL;}
    char p[64]="/proc/self/fd/",digits[24];size_t k=14;unsigned d=0;unsigned long v=(unsigned)fd;do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);while(d)p[k++]=digits[--d];p[k]=0;
    char *s=readlink_dynamic(p,error);if(!s)return NULL;char *q=vfs_reverse(s);vfs_free(s);return q;
}
char *vfs_resolve_mount(int fd,const char *path,int follow,long *error,void **identity){
    *error=0;if(!path){*error=-EFAULT;return NULL;}if(!*path){*error=-ENOENT;return NULL;}
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
        size_t next=i;while(input[next]=='/')next++;
        if(!follow && !input[next] && input[i]!='/' )continue;
        char *global=root?join(root,out):dupstr(out);char *host=global?physical(global):NULL;vfs_free(global);if(!host){*error=-ENOMEM;goto fail;}
        long e=0;char *target=readlink_dynamic(host,&e);
        if(!target && input[i]=='/'){struct stat st;long status=RAW(SYS_newfstatat,AT_FDCWD,host,&st,0,0,0);if(status<0){vfs_free(host);*error=status;goto fail;}if(!S_ISDIR(st.st_mode)){vfs_free(host);*error=-ENOTDIR;goto fail;}}
        vfs_free(host);
        if(target){if(++links>40){vfs_free(target);*error=-ELOOP;goto fail;}char *head;
            if(target[0]=='/')head=dupstr(target);else {used-=len;if(used>1)used--;out[used]=0;head=join(out,target);}vfs_free(target);
            char *replacement=head?join(head,input+i):NULL;vfs_free(head);if(!replacement){*error=-ENOMEM;goto fail;}vfs_free(input);vfs_free(out);input=replacement;goto restart;
        }else if(e!=-EINVAL && e!=-ENOENT && e!=-ENOTDIR){*error=e;goto fail;}
    }
    {char *global=root?join(root,out):dupstr(out);if(identity)*identity=global?lookup(global):NULL;char *host=global?physical(global):NULL;vfs_free(global);if(host && input[p_len(input)-1]=='/' && !p_equal(host,"/")){char *q=join(host,"/");vfs_free(host);host=q;}vfs_free(input);vfs_free(out);if(!host)*error=-ENOMEM;return host;}
fail:vfs_free(input);vfs_free(out);return NULL;
}
char *vfs_resolve(int fd,const char *path,int follow,long *error){return vfs_resolve_mount(fd,path,follow,error,NULL);}
long vfs_mount_readonly(void *identity){struct mount *m=identity;return m && (m->flags&MS_RDONLY)?-EROFS:0;}
int proxy_mount(const char *source,const char *target){if(!source || !target || target[0]!='/')return -EINVAL;struct mount *m=vfs_alloc(sizeof(*m));if(!m)return -ENOMEM;m->source=dupstr(source);m->target=dupstr(target);m->flags=0;if(!m->source || !m->target){vfs_free(m->source);vfs_free(m->target);vfs_free(m);return -ENOMEM;}size_t n=p_len(m->target);while(n>1 && m->target[n-1]=='/')m->target[--n]=0;invalidate();m->next=table;table=m;
    RAW(SYS_write,2,"v3 mount ",9,0,0,0);RAW(SYS_write,2,source,p_len(source),0,0,0);RAW(SYS_write,2," -> ",4,0,0,0);RAW(SYS_write,2,target,p_len(target),0,0,0);RAW(SYS_write,2,"\n",1,0,0,0);return 0;}
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
long vfs_mount(const char *source,const char *target,const char *type,unsigned long flags){
    if(flags&(MS_PRIVATE|MS_SLAVE|MS_SHARED|MS_UNBINDABLE))return 0;
    long e=0;char *dest=global_target(target,&e);if(!dest)return e;
    if(flags&MS_REMOUNT){struct mount *m=lookup(dest);if(!m)e=-EINVAL;else m->flags=flags;vfs_free(dest);return e;}
    char *src=NULL;
    if(flags&MS_BIND)src=vfs_resolve(AT_FDCWD,source,1,&e);
    else if(type && p_equal(type,"proc"))src=dupstr("/proc");
    else if(type && p_equal(type,"devpts"))src=dupstr("/dev/pts");
    else if(type && p_equal(type,"mqueue"))src=dupstr("/dev/mqueue");
    else if(type && p_equal(type,"tmpfs")){
        /* mkdtemp equivalent using pid + monotonic local counter; cache path is
         * inherited TMPDIR in the loader, falling back to /tmp on host. */
        extern const char *proxy_tmpdir;static unsigned long sequence;char suffix[80]="compat-proxy-";size_t k=13;unsigned long values[2]={(unsigned long)RAW(SYS_getpid,0,0,0,0,0,0),(unsigned long)0};RAW(SYS_getrandom,&values[1],sizeof(values[1]),0,0,0,0);values[1]^=++sequence;for(unsigned j=0;j<2;j++){char d[24];unsigned n=0;do{d[n++]=(char)('0'+values[j]%10);values[j]/=10;}while(values[j]);while(n)suffix[k++]=d[--n];suffix[k++]=j?0:'-';}src=join(proxy_tmpdir,suffix);if(src)e=RAW(SYS_mkdirat,AT_FDCWD,src,0700,0,0,0);
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
long vfs_unmount(const char *target){long e=0;char *dest=global_target(target,&e);if(!dest)return e;struct mount **p=&table;while(*p && !p_equal((*p)->target,dest))p=&(*p)->next;vfs_free(dest);if(!*p)return p_equal(target,".")?0:-EINVAL;invalidate();struct mount *m=*p;*p=m->next;vfs_free(m->source);vfs_free(m->target);vfs_free(m);return 0;}
long vfs_root(const char *target){long e=0;char *next=global_target(target,&e);if(!next)return e;char *host=physical(next);if(!host){vfs_free(next);return -ENOMEM;}long fd=RAW(SYS_openat,AT_FDCWD,host,O_DIRECTORY|O_PATH|O_CLOEXEC,0,0,0);vfs_free(host);if(fd<0){vfs_free(next);return fd;}RAW(SYS_close,fd,0,0,0,0,0);invalidate();vfs_free(root);root=next;vfs_free(cwd);cwd=NULL;return 0;}
long vfs_pivot(const char *target,const char *old){long e=0;char *next=global_target(target,&e);if(!next)return e;
    if(!p_equal(target,old)){char *oldhost=physical(root?root:"/");char *dest=global_target(old,&e);if(!dest || !oldhost){vfs_free(next);vfs_free(dest);vfs_free(oldhost);return -ENOMEM;}e=proxy_mount(oldhost,dest);vfs_free(dest);vfs_free(oldhost);if(e){vfs_free(next);return e;}}
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
    if(!e){struct mount *m=identity;char *logical=m?join(m->target,host+(p_len(m->source)==1?0:p_len(m->source))):vfs_reverse(host);
        if(logical && m && root && prefix(logical,root)){char *q=dupstr(logical+p_len(root));vfs_free(logical);logical=q;if(logical && !*logical){vfs_free(logical);logical=dupstr("/");}}
        if(!logical)e=-ENOMEM;else {vfs_free(cwd);cwd=logical;}}
    vfs_free(host);return e;
}
char *vfs_getcwd(long *error){return basepath(AT_FDCWD,error);}
long vfs_bootstrap_root(const char *directory){
    /* CLI roots are host-side symlink scaffolds, not payload symlinks. Pin
     * their existing top-level endpoints before virtual resolution begins. */
    const char *names[]={"usr","bin","lib","lib64","etc","sbin","proc","dev","data"};
    char *sources[9]={0};long error=0;
    for(unsigned i=0;i<9;i++){
        char *path=join(directory,names[i]);if(!path){error=-ENOMEM;break;}
        long fd=RAW(SYS_openat,AT_FDCWD,path,O_PATH|O_CLOEXEC,0,0,0);vfs_free(path);
        if(fd<0){if(fd!=-ENOENT)error=fd;continue;}
        char proc[64]="/proc/self/fd/",digits[24];unsigned d=0;size_t k=14;unsigned long v=(unsigned long)fd;do{digits[d++]=(char)('0'+v%10);v/=10;}while(v);while(d)proc[k++]=digits[--d];proc[k]=0;
        sources[i]=readlink_dynamic(proc,&error);RAW(SYS_close,fd,0,0,0,0,0);if(!sources[i])break;
    }
    if(!error)error=proxy_mount(directory,"/");
    for(unsigned i=0;i<9;i++){if(!error && sources[i]){char *target=join("/",names[i]);if(!target)error=-ENOMEM;else error=proxy_mount(sources[i],target);vfs_free(target);}vfs_free(sources[i]);}
    return error;
}
