/* HostCommand stays in the completed calling app sandbox. No setns/host exec.
 * Included by proxy.c so child entry uses exactly its resident exec reset. */
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <limits.h>
#include <sys/wait.h>
#define HC_MAX (1024*1024)
#define HC_FDS 16
static const char *host_command_socket;
static int host_command_started;
static volatile int hc_stopping;
#ifdef HOST_COMMAND_TEST
int proxy_host_test(int fd){hc_test_fd=fd;return 0;}
#endif
static void hc_stop(int signal) { (void)signal; hc_stopping=1; }
static int hc_io(int fd,void *buf,size_t size,int writing) {
    char *p=buf;
    while(size) {long n=writing?RAW(PROXY_SC_write,fd,p,size,0,0,0):RAW(PROXY_SC_read,fd,p,size,0,0,0);
        if(n==-EINTR)continue;
        if(n<=0)return -EIO;
        p+=n;size-=(size_t)n;
    }return 0;
}
static void hc_close_except(const int *keep,int count) {
    long directory=RAW(PROXY_SC_openat,AT_FDCWD,"/proc/self/fd",O_RDONLY|O_DIRECTORY,0,0,0);
    if(directory<0)return;
    char buf[8192];
    for(;;){long n=RAW(PROXY_SC_getdents64,directory,buf,sizeof(buf),0,0,0);if(n<=0)break;
        for(long off=0;off<n;){unsigned short len=*(unsigned short *)(buf+off+16);if(len<20||off+len>n)break;
            char *s=buf+off+19;int fd=0,valid=*s!=0;
            for(;*s;s++){if(*s<'0'||*s>'9'){valid=0;break;}fd=fd*10+*s-'0';}
            int retain=fd==directory||vfs_private_fd(fd);
            for(int i=0;i<count;i++)if(keep[i]==fd)retain=1;
            if(valid&&!retain)RAW(PROXY_SC_close,fd,0,0,0,0,0);
            off+=len;
        }
    }RAW(PROXY_SC_close,directory,0,0,0,0,0);
}
static void hc_parent(long parent) {
    struct kernel_action action={(uintptr_t)hc_stop,SA_RESTORER,(uintptr_t)proxy_restore,0};
    RAW(PROXY_SC_rt_sigaction,SIGTERM,&action,0,8,0,0);
    struct kernel_action pipe_ignore={1,0,0,0};RAW(PROXY_SC_rt_sigaction,SIGPIPE,&pipe_ignore,0,8,0,0);
    struct kernel_action child_default={0};RAW(PROXY_SC_rt_sigaction,SIGCHLD,&child_default,0,8,0,0);
    unsigned long mask=0;RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&mask,0,8,0,0);
    RAW(PROXY_SC_prctl,PR_SET_PDEATHSIG,SIGTERM,0,0,0,0);
    if(RAW(PROXY_SC_getppid,0,0,0,0,0,0)!=parent)hc_stopping=1;
}
/* Avoid compiler/libc dependence: raw NUL strings with bounded counts. */
static char *hc_next(char **p,char *end) {
    char *s=*p;while(*p<end){if(*(*p)++==0)return s;}return NULL;
}
static int hc_key(const char *a,const char *b) {
    while(*a&&*a!='='&&*a==*b){a++;b++;}return *a=='='&&*b=='=';
}
static long hc_exec(char **argv,char **env,int ready) {
    struct image image;int r;
    if(!p_len(argv[0]))return -EINVAL;
    int slash=0;for(char *p=argv[0];*p;p++)if(*p=='/')slash=1;
    if(slash)r=proxy_load(argv[0],argv,env,&image);
    else {
        const char *path="/usr/bin:/bin";
        for(int i=0;env[i];i++)if(p_len(env[i])>=5&&env[i][0]=='P'&&env[i][1]=='A'&&env[i][2]=='T'&&env[i][3]=='H'&&env[i][4]=='=')path=env[i]+5;
        r=-ENOENT;
        for(;;){const char *start=path;while(*path&&*path!=':')path++;size_t len=(size_t)(path-start),name=p_len(argv[0]);
            char *candidate=vfs_alloc(len+name+2);if(!candidate)return -ENOMEM;
            p_copy(candidate,start,len);size_t prefix=len;if(len)candidate[prefix++]='/';p_copy(candidate+prefix,argv[0],name+1);
            r=proxy_load(candidate,argv,env,&image);vfs_free(candidate);if(!r)break;if(!*path)break;path++;
        }
    }
    if(r)return r;
    RAW(PROXY_SC_close,ready,0,0,0,0,0);
    /* session-helper child setup resets ignored actions too (notably PIPE). */
    struct kernel_action defaults={0};
    for(int sig=1;sig<=64;sig++)if(sig!=SIGKILL&&sig!=SIGSTOP&&sig!=SIGSYS)RAW(PROXY_SC_rt_sigaction,sig,&defaults,0,8,0,0);
    exec_signals();chained.handler=0;
    if(dispatch_selector){RAW(PROXY_SC_prctl,PR_SET_SYSCALL_USER_DISPATCH,PR_SYS_DISPATCH_OFF,0,0,0,0);dispatch_selector=NULL;dispatch_owner=0;}
    unsigned long mask=0;RAW(PROXY_SC_rt_sigprocmask,SIG_SETMASK,&mask,0,8,0,0);
    RAW(PROXY_SC_arch_prctl,0x1002,0,0,0,0,0);
    proxy_enter(image.entry,image.stack);
}
/* Subreaper children cannot reuse their PIDs until this worker reaps them.
 * Kill adopted children, then repeat as their descendants are reparented here.
 * This covers background jobs that create their own sessions/process groups. */
static int hc_children(int killing) {
    char path[96]="/proc/self/task/",digits[24];long tid=RAW(PROXY_SC_gettid,0,0,0,0,0,0);int n=0;
    do{digits[n++]=(char)('0'+tid%10);tid/=10;}while(tid);
    size_t at=p_len(path);while(n)path[at++]=digits[--n];p_copy(path+at,"/children",10);
    long fd=RAW(PROXY_SC_openat,AT_FDCWD,path,O_RDONLY|O_CLOEXEC,0,0,0);if(fd<0)return (int)fd;
    char buffer[4096];long child=0;int count=0;
    for(;;){long bytes=RAW(PROXY_SC_read,fd,buffer,sizeof(buffer),0,0,0);if(bytes<=0)break;
        for(long i=0;i<bytes;i++){if(buffer[i]>='0'&&buffer[i]<='9')child=child*10+buffer[i]-'0';
            else if(child){if(killing)RAW(PROXY_SC_kill,child,SIGKILL,0,0,0,0);count++;child=0;}}
    }
    if(child){if(killing)RAW(PROXY_SC_kill,child,SIGKILL,0,0,0,0);count++;}
    RAW(PROXY_SC_close,fd,0,0,0,0,0);return count;
}
static void hc_worker(int socket,char *const *original,long server) {
    hc_parent(server);
    int received[HC_FDS],nfds=0;uint32_t header[6]={0};
    char control[CMSG_SPACE(sizeof(received))];struct iovec iov={header,sizeof(header)};
    struct msghdr msg={0};msg.msg_iov=&iov;msg.msg_iovlen=1;msg.msg_control=control;msg.msg_controllen=sizeof(control);
    struct timeval timeout={3,0};RAW(PROXY_SC_setsockopt,socket,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout),0);
    long n=RAW(PROXY_SC_recvmsg,socket,&msg,MSG_CMSG_CLOEXEC,0,0,0);int error=-EINVAL;
    for(size_t off=0;off+sizeof(struct cmsghdr)<=msg.msg_controllen;){
        struct cmsghdr *c=(void *)(control+off);if(c->cmsg_len<sizeof(*c)||c->cmsg_len>msg.msg_controllen-off)goto fail;
        off+=CMSG_ALIGN(c->cmsg_len);if(c->cmsg_level!=SOL_SOCKET||c->cmsg_type!=SCM_RIGHTS)continue;
        size_t count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);if(count>HC_FDS)count=HC_FDS;
        if(nfds+(int)count>HC_FDS)goto fail;
        p_copy(received+nfds,CMSG_DATA(c),count*sizeof(int));nfds+=(int)count;
    }
    if(n>0&&n<(long)sizeof(header)){if(hc_io(socket,(char *)header+n,sizeof(header)-(size_t)n,0))goto fail;}
    else if(n!=(long)sizeof(header))goto fail;
    /* Private runtime shutdown, authenticated by the listener UID check. */
    if(header[0]==0x314d4348&&!header[1]&&!header[2]&&!header[3]&&!header[4]&&header[5]==4&&!nfds){RAW(PROXY_SC_kill,server,SIGTERM,0,0,0,0);RAW(PROXY_SC_exit_group,0,0,0,0,0,0);}
    /* HCM1, body size, argc, envc, fd count, flags. */
    if(header[0]!=0x314d4348||header[1]>HC_MAX||!header[2]||header[2]>4096||header[3]>4096||header[4]!=(unsigned)nfds||header[5]&~3u||msg.msg_flags&MSG_CTRUNC)goto fail;
    error=(int)RAW(PROXY_SC_prctl,PR_SET_CHILD_SUBREAPER,1,0,0,0,0);if(error)goto fail;
    error=hc_children(0);if(error<0)goto fail;
    char *body=vfs_alloc(header[1]);if(!body){error=-ENOMEM;goto fail;}
    if(hc_io(socket,body,header[1],0))goto fail;
    char *p=body,*end=body+header[1];
    unsigned targets[HC_FDS];for(int i=0;i<nfds;i++){if(end-p<4)goto fail;p_copy(&targets[i],p,4);p+=4;if(targets[i]>INT_MAX||vfs_private_fd((int)targets[i]))goto fail;for(int j=0;j<i;j++)if(targets[i]==targets[j])goto fail;}
    char *cwd=hc_next(&p,end);if(!cwd)goto fail;
    char **argv=vfs_alloc((header[2]+1)*sizeof(char *));if(!argv){error=-ENOMEM;goto fail;}
    for(unsigned i=0;i<header[2];i++){argv[i]=hc_next(&p,end);if(!argv[i])goto fail;}argv[header[2]]=NULL;
    unsigned old=0;if(!(header[5]&1))while(original[old])old++;
    char **env=vfs_alloc((old+header[3]+1)*sizeof(char *));if(!env){error=-ENOMEM;goto fail;}
    unsigned ec=old;for(unsigned i=0;i<old;i++)env[i]=original[i];
    for(unsigned i=0;i<header[3];i++){char *entry=hc_next(&p,end);if(!entry||!entry[0]||entry[0]=='=')goto fail;int equals=0;for(char *s=entry;*s;s++)if(*s=='=')equals=1;if(!equals)goto fail;
        unsigned j=0;for(;j<ec;j++)if(hc_key(env[j],entry))break;env[j]=entry;if(j==ec)ec++;
    }env[ec]=NULL;if(p!=end)goto fail;
    /* Duplicate above every destination before remapping (cycles and repeats). */
    unsigned high=3;for(int i=0;i<nfds;i++)if(targets[i]>=high)high=targets[i]+1;
    int copies[HC_FDS];for(int i=0;i<nfds;i++){long fd=RAW(PROXY_SC_fcntl,received[i],F_DUPFD_CLOEXEC,high,0,0,0);if(fd<0){error=(int)fd;goto fail;}copies[i]=(int)fd;}
    int pipe[2];error=(int)RAW(PROXY_SC_pipe2,pipe,O_CLOEXEC,0,0,0,0);if(error)goto fail;
    if((unsigned)pipe[1]<high){long safe=RAW(PROXY_SC_fcntl,pipe[1],F_DUPFD_CLOEXEC,high,0,0,0);if(safe<0){error=(int)safe;goto fail;}RAW(PROXY_SC_close,pipe[1],0,0,0,0,0);pipe[1]=(int)safe;}
    long worker=RAW(PROXY_SC_getpid,0,0,0,0,0,0),child=RAW(PROXY_SC_clone,SIGCHLD,0,0,0,0,0);if(child<0){error=(int)child;goto fail;}
    if(!child){
        RAW(PROXY_SC_close,pipe[0],0,0,0,0,0);RAW(PROXY_SC_prctl,PR_SET_PDEATHSIG,SIGKILL,0,0,0,0);
        if(RAW(PROXY_SC_getppid,0,0,0,0,0,0)!=worker)RAW(PROXY_SC_exit_group,127,0,0,0,0,0);
        RAW(PROXY_SC_setsid,0,0,0,0,0,0);
        int keep[HC_FDS+1];for(int i=0;i<nfds;i++)keep[i]=copies[i];keep[nfds]=pipe[1];hc_close_except(keep,nfds+1);
        long null=RAW(PROXY_SC_openat,AT_FDCWD,"/dev/null",O_RDWR,0,0,0);
        if(null<0){error=(int)null;goto child_fail;}
        for(int i=0;i<3;i++)if(null!=i)RAW(PROXY_SC_dup3,null,i,0,0,0,0);
        if(null>2)RAW(PROXY_SC_close,null,0,0,0,0,0);
        for(int i=0;i<nfds;i++){long r=RAW(PROXY_SC_dup3,copies[i],targets[i],0,0,0,0);if(r<0){error=(int)r;goto child_fail;}RAW(PROXY_SC_close,copies[i],0,0,0,0,0);vfs_fd_close((int)targets[i]);}
        for(int i=0;i<nfds;i++)if(targets[i]<3){struct termios tty;if(RAW(PROXY_SC_ioctl,targets[i],TCGETS,&tty,0,0,0)==0){RAW(PROXY_SC_ioctl,targets[i],TIOCSCTTY,0,0,0,0);break;}}
        if(*cwd){error=(int)vfs_chdir(AT_FDCWD,cwd);if(error)goto child_fail;}
        /* A ready pipe replaces native exec's CLOEXEC handshake. Validate/load
         * first; success closes the pipe just before resident payload entry. */
        error=(int)hc_exec(argv,env,pipe[1]);
child_fail:
        RAW(PROXY_SC_write,pipe[1],&error,4,0,0,0);RAW(PROXY_SC_exit_group,127,0,0,0,0,0);
    }
    RAW(PROXY_SC_close,pipe[1],0,0,0,0,0);
    for(int i=0;i<nfds;i++){RAW(PROXY_SC_close,received[i],0,0,0,0,0);RAW(PROXY_SC_close,copies[i],0,0,0,0,0);}nfds=0;
    /* Child closes its handshake on successful load in hc_exec. */
    error=0;n=RAW(PROXY_SC_read,pipe[0],&error,4,0,0,0);RAW(PROXY_SC_close,pipe[0],0,0,0,0,0);
    if(n!=0){if(n<0){error=(int)n;RAW(PROXY_SC_kill,child,SIGKILL,0,0,0,0);}RAW(PROXY_SC_wait4,child,0,0,0,0,0);goto fail;}
    int pid=(int)child;int lost=hc_io(socket,&pid,4,1)!=0;
    timeout=(struct timeval){0,0};RAW(PROXY_SC_setsockopt,socket,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout),0);
    int connected=!lost,status=0,exited=0,killing=lost;uint32_t command[2];size_t command_bytes=0;
    for(;;){
        long waited;
        while((waited=RAW(PROXY_SC_wait4,-1,&status,WNOHANG,0,0,0))>0){
            if(waited==child&&!exited){exited=1;if(connected)hc_io(socket,&status,4,1);}
        }
        if(waited==-ECHILD)break;
        if(hc_stopping||RAW(PROXY_SC_getppid,0,0,0,0,0,0)!=server)killing=1;
        if(killing){if(!exited)RAW(PROXY_SC_kill,-child,SIGKILL,0,0,0,0);hc_children(1);}
        struct pollfd poll={connected?socket:-1,POLLIN,0};RAW(PROXY_SC_poll,&poll,1,100,0,0,0);
        if(connected&&poll.revents){long got=RAW(PROXY_SC_read,socket,(char *)command+command_bytes,sizeof(command)-command_bytes,0,0,0);
            if(got>0){command_bytes+=(size_t)got;if(command_bytes==sizeof(command)){
                if(command[0]<=64&&command[1]<=1&&!exited)RAW(PROXY_SC_kill,command[1]?-child:child,command[0],0,0,0,0);
                command_bytes=0;
            }}else if(got!=-EAGAIN&&got!=-EINTR){connected=0;if(header[5]&2)killing=1;}
        }
    }
    RAW(PROXY_SC_exit_group,0,0,0,0,0,0);
fail:
    if(error>=0)error=-EINVAL;
    hc_io(socket,&error,4,1);RAW(PROXY_SC_exit_group,1,0,0,0,0,0);
}
static void hc_server(int listener,char *const *env,long parent) {
    hc_parent(parent);hc_close_except(&listener,1);
    long workers[128]={0};
    while(!hc_stopping){
        for(int i=0;i<128;i++)if(workers[i]&&RAW(PROXY_SC_wait4,workers[i],0,WNOHANG,0,0,0)!=0)workers[i]=0;
        struct pollfd poll={listener,POLLIN,0};RAW(PROXY_SC_poll,&poll,1,100,0,0,0);
        if(!(poll.revents&POLLIN))continue;
        long socket=RAW(PROXY_SC_accept4,listener,0,0,SOCK_CLOEXEC,0,0);if(socket<0)continue;
        struct ucred cred;unsigned size=sizeof(cred);
        if(RAW(PROXY_SC_getsockopt,socket,SOL_SOCKET,SO_PEERCRED,&cred,&size,0)<0||cred.uid!=(unsigned)RAW(PROXY_SC_getuid,0,0,0,0,0,0)){RAW(PROXY_SC_close,socket,0,0,0,0,0);continue;}
        int slot=0;while(slot<128&&workers[slot])slot++;
        if(slot==128){int err=-EAGAIN;hc_io((int)socket,&err,4,1);RAW(PROXY_SC_close,socket,0,0,0,0,0);continue;}
        long server=RAW(PROXY_SC_getpid,0,0,0,0,0,0);
        long pid=RAW(PROXY_SC_clone,SIGCHLD,0,0,0,0,0);
        if(!pid){RAW(PROXY_SC_close,listener,0,0,0,0,0);hc_worker((int)socket,env,server);}
        if(pid>0)workers[slot]=pid;
        RAW(PROXY_SC_close,socket,0,0,0,0,0);
    }
    for(int i=0;i<128;i++)if(workers[i])RAW(PROXY_SC_kill,workers[i],SIGTERM,0,0,0,0);
    for(int i=0;i<128;i++)if(workers[i])RAW(PROXY_SC_wait4,workers[i],0,0,0,0,0);
    RAW(PROXY_SC_unlinkat,AT_FDCWD,host_command_socket,0,0,0,0);RAW(PROXY_SC_exit_group,0,0,0,0,0,0);
}
int proxy_host_command(const char *path) {
    if(!path||path[0]!='/'||p_len(path)>=sizeof(((struct sockaddr_un *)0)->sun_path))return -EINVAL;
    host_command_socket=path;return 0;
}
void proxy_host_snapshot(char *const *env) {
#ifdef HOST_COMMAND_TEST
    if(hc_test_fd<0&&!host_command_socket)return;
#else
    if(!host_command_socket)return;
#endif
    if(host_command_started)return;
    long error;char *info=vfs_resolve(AT_FDCWD,"/.flatpak-info",1,&error);if(!info)return;
    struct proxy_stat st;long found=arch_stat_at(AT_FDCWD,info,&st,0);vfs_free(info);if(found<0)return;
#ifdef HOST_COMMAND_TEST
    if(hc_test_fd>=0){
        host_command_started=1;long parent=RAW(PROXY_SC_getpid,0,0,0,0,0,0);long pid=RAW(PROXY_SC_clone,SIGCHLD,0,0,0,0,0);
        if(!pid){int channel=hc_test_fd;hc_test_fd=-1;hc_close_except(&channel,1);hc_worker(channel,env,parent);}
        RAW(PROXY_SC_close,hc_test_fd,0,0,0,0,0);hc_test_fd=-1;return;
    }
#endif
    host_command_started=1;
    long socket=RAW(PROXY_SC_socket,AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,0,0,0);if(socket<0)return;
    struct sockaddr_un address={.sun_family=AF_UNIX};p_copy(address.sun_path,host_command_socket,p_len(host_command_socket)+1);
    /* Launcher removes stale sockets before bootstrapping this runtime. */
    if(RAW(PROXY_SC_bind,socket,&address,sizeof(address),0,0,0)<0||RAW(PROXY_SC_fchmodat,AT_FDCWD,host_command_socket,0600,0,0,0)<0||RAW(PROXY_SC_listen,socket,16,0,0,0,0)<0){RAW(PROXY_SC_close,socket,0,0,0,0,0);return;}
    long parent=RAW(PROXY_SC_getpid,0,0,0,0,0,0),pid=RAW(PROXY_SC_clone,SIGCHLD,0,0,0,0,0);
    if(!pid)hc_server((int)socket,env,parent);
    RAW(PROXY_SC_close,socket,0,0,0,0,0);
    if(pid<0)RAW(PROXY_SC_unlinkat,AT_FDCWD,host_command_socket,0,0,0,0);
}
