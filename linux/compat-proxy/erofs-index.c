/* SPDX-License-Identifier: Apache-2.0
 * Prefix swaps for init-mounted, single-image package deployments.
 * Included by vfs.c. The kernel EROFS mount owns all filesystem metadata and
 * descriptor behaviour; this table only translates package paths. */
struct erofs_mount { struct erofs_mount *next; char *target, *directory, *backing; };
static struct erofs_mount *erofs_mounts;

static int erofs_prefix(const char *path,const char *prefix){
    size_t n=p_len(prefix);if(!path||!prefix||n==0)return 0;
    for(size_t i=0;i<n;i++)if(path[i]!=prefix[i])return 0;
    return n==1||path[n]==0||path[n]=='/';
}
static char *erofs_dup(const char *s){size_t n=p_len(s);char *r=vfs_palloc(n+1);if(r)p_copy(r,s,n+1);return r;}
static struct erofs_mount *erofs_find(const char *path){
    struct erofs_mount *best=NULL;size_t length=0;
    for(struct erofs_mount *m=erofs_mounts;m;m=m->next){size_t n=p_len(m->target);if(n>length&&erofs_prefix(path,m->target)){best=m;length=n;}}
    return best;
}
static struct erofs_mount *erofs_find_backing(const char *path){
    struct erofs_mount *best=NULL;size_t length=0;
    for(struct erofs_mount *m=erofs_mounts;m;m=m->next){
        size_t n=m->backing?p_len(m->backing):0;
        if(n>length&&erofs_prefix(path,m->backing)){best=m;length=n;}
    }
    return best;
}
static const char *erofs_root_view(const char *path,const char *root){
    if(!path||!root||!*root)return NULL;
    size_t n=p_len(root);if(n>1){for(size_t i=0;i<n;i++)if(path[i]!=root[i])return NULL;
        if(path[n]&&path[n]!='/')return NULL;}
    else if(path[0]!='/')return NULL;
    const char *view=n==1?path:path+n;return *view?view:"/";
}
size_t erofs_prefix_length(const char *path,const char *root){
    struct erofs_mount *m=erofs_find(path);if(m)return p_len(m->target);
    m=erofs_find_backing(path);if(m)return p_len(m->backing);
    const char *view=erofs_root_view(path,root);m=view?erofs_find(view):NULL;
    return m?p_len(m->target):0;
}
char *erofs_physical(const char *path){
    struct erofs_mount *m=erofs_find(path);size_t n=m?p_len(m->target):0;
    if(!m){m=erofs_find_backing(path);if(m)n=p_len(m->backing);}
    if(!m)return NULL;
    const char *suffix=path+n;if(!*suffix)suffix="/";
    return join(m->directory,suffix);
}
/* VFS paths are made global before physical() is called. After pivot_root or
 * chroot that adds the virtual root in front of package paths, while package
 * prefixes remain rooted at the app's / (for example /usr). Match both views
 * so a rooted /usr/lib/... call still reaches the package image. */
char *erofs_physical_rooted(const char *path,const char *root){
    const char *view=erofs_root_view(path,root);return view?erofs_physical(view):NULL;
}
char *erofs_reverse(const char *path){
    struct erofs_mount *best=NULL;size_t length=0;
    for(struct erofs_mount *m=erofs_mounts;m;m=m->next){size_t n=p_len(m->directory);if(n>length&&erofs_prefix(path,m->directory)){best=m;length=n;}}
    if(!best)return NULL;
    const char *suffix=path+length;if(!*suffix)suffix="/";
    return join(best->target,suffix);
}
long erofs_backing(const char *target,const char *backing){
    if(!target||target[0]!='/'||!backing||backing[0]!='/')return -EINVAL;
    long e=0;char *t=global_target(target,&e);if(!t)return e;
    char *b=global_target(backing,&e);if(!b){vfs_free(t);return e;}
    struct erofs_mount *m=erofs_find(t);
    if(!m){vfs_free(t);vfs_free(b);return -ENOENT;}
    vfs_free(m->backing);m->backing=erofs_dup(b);
    vfs_free(t);vfs_free(b);return m->backing?0:-ENOMEM;
}
long erofs_mount(const char *target,const char *directory){
    if(!target||target[0]!='/'||!directory||directory[0]!='/')return -EINVAL;
    long e=0;char *t=global_target(target,&e);if(!t)return e;
    char *d=global_target(directory,&e);if(!d){vfs_free(t);return e;}
    for(struct erofs_mount *m=erofs_mounts;m;m=m->next)if(p_equal(m->target,t)){
        vfs_free(m->directory);m->directory=erofs_dup(d);vfs_free(t);vfs_free(d);return m->directory?0:-ENOMEM;
    }
    struct erofs_mount *m=vfs_palloc(sizeof(*m));if(!m){vfs_free(t);vfs_free(d);return -ENOMEM;}
    m->target=erofs_dup(t);m->directory=erofs_dup(d);m->backing=NULL;m->next=erofs_mounts;erofs_mounts=m;
    vfs_free(t);vfs_free(d);if(!m->target||!m->directory)return -ENOMEM;
    if(proxy_trace){const char *a[]={"v3 erofs prefix-swap ",m->target," -> ",m->directory,"\n"};for(size_t i=0;i<5;i++)RAW(PROXY_SC_write,proxy_diagnostic_fd,a[i],p_len(a[i]),0,0,0);}
    return 0;
}
static void erofs_state(struct state *s){
    uint64_t count=0;for(struct erofs_mount *m=erofs_mounts;m;m=m->next)count++;
    count=state_word(s,count);if(s->error)return;
    struct erofs_mount **tail=&erofs_mounts;
    if(s->reading)erofs_mounts=NULL;
    for(uint64_t i=0;i<count&&!s->error;i++) {
        struct erofs_mount *m=s->reading?vfs_palloc(sizeof(*m)):*tail;
        if(!m){s->error=-ENOMEM;return;}
        if(s->reading){m->next=NULL;m->target=m->directory=m->backing=NULL;*tail=m;tail=&m->next;}
        state_string(s,&m->target);state_string(s,&m->directory);state_string(s,&m->backing);
    }
}
