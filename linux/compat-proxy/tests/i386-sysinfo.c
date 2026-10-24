/* Header-free ELF32 glibc probe: this host has the i386 runtime but not its
 * development headers. Keep ABI declarations local and link against libc. */
typedef unsigned long uintptr_t;
typedef unsigned long size_t;
typedef int pid_t;
typedef int sig_atomic_t;
typedef long ssize_t;
typedef struct { long sec,usec; } timeval;
typedef struct { timeval interval,value; } itimerval;
extern long i386_vsyscall6(long,long,long,long,long,long,long);
extern int i386_check_sysinfo_preserve(int);
extern unsigned long getauxval(unsigned long);
extern char *getenv(const char *);
extern int strcmp(const char *,const char *);
extern int printf(const char *,...);
extern int fprintf(void *,const char *,...);
extern void *stderr;
extern void perror(const char *);
extern int signal(int,void (*)(int));
extern int setitimer(int,const itimerval *,itimerval *);
extern void *dlopen(const char *,int);
extern int dlclose(void *);
extern int pthread_create(unsigned long *,const void *,void *(*)(void *),void *);
extern int pthread_join(unsigned long,void **);
extern pid_t fork(void);
extern pid_t waitpid(pid_t,int *,int);
extern void _exit(int);
extern int execl(const char *,const char *,...);
extern int execle(const char *,const char *,...);
extern ssize_t read(int,void *,size_t);
#define AT_FDCWD -100
#define O_RDONLY 0
#define F_GETFL 3
#define SEEK_SET 0
#define ITIMER_REAL 0
#define SIGALRM 14
#define WIFEXITED(s) (((s)&0x7f)==0)
#define WEXITSTATUS(s) (((s)>>8)&0xff)
static const char *path="/i386-fastpath-file";
static volatile sig_atomic_t signals_seen;
static void on_alarm(int sig) { (void)sig; signals_seen++; }
static int check(int ok,const char *what) { if(!ok){perror(what);return 1;} return 0; }
static int file_calls(void) {
    int failures=0;
    long fd=i386_vsyscall6(295,AT_FDCWD,(long)path,O_RDONLY,0,0,0);
    failures+=check(fd>=0,"openat"); if(fd<0)return failures;
    unsigned char st[128],sx[256];
    failures+=check(i386_vsyscall6(195,(long)path,(long)st,0,0,0,0)==0,"stat64");
    failures+=check(i386_vsyscall6(197,fd,(long)st,0,0,0,0)==0,"fstat64");
    failures+=check(i386_vsyscall6(300,AT_FDCWD,(long)path,(long)st,0,0,0)==0,"fstatat64");
    failures+=check(i386_vsyscall6(383,AT_FDCWD,(long)path,0,0x7ff,(long)sx,0)==0,"statx");
    failures+=check(i386_vsyscall6(221,fd,F_GETFL,0,0,0,0)>=0,"fcntl64 getfl");
    int seek_result=0;
    failures+=check(i386_vsyscall6(140,fd,0,1,(long)&seek_result,SEEK_SET,0)==0 && seek_result==1,"_llseek");
    char byte=0; failures+=check(read((int)fd,&byte,1)==1,"read");
    const char *enabled=getenv("MATON_PROXY_REWRITE");
    if(!enabled || strcmp(enabled,"0"))failures+=check(i386_check_sysinfo_preserve((int)fd)==0,"AT_SYSINFO register/flags preservation and close");
    else failures+=check(i386_vsyscall6(6,fd,0,0,0,0,0)==0,"close");
    char link[256];
    failures+=check(i386_vsyscall6(85,(long)"/i386-fastpath-link",(long)link,sizeof(link),0,0,0)>0,"readlink");
    failures+=check(i386_vsyscall6(305,AT_FDCWD,(long)"/i386-fastpath-link",(long)link,sizeof(link),0,0)>0,"readlinkat");
    long dir=i386_vsyscall6(295,AT_FDCWD,(long)"/",O_RDONLY|0x10000,0,0,0);
    char dents[4096];
    failures+=check(dir>=0 && i386_vsyscall6(220,dir,(long)dents,sizeof(dents),0,0,0)>0,"getdents64");
    if(dir>=0)failures+=check(i386_vsyscall6(6,dir,0,0,0,0,0)==0,"close directory");
    return failures;
}
static void *thread_main(void *arg) { (void)arg; for(int i=0;i<100;i++)if(file_calls())return (void *)1; return 0; }
int main(int argc,char **argv) {
    uintptr_t sysinfo=getauxval(32);
    const char *enabled=getenv("MATON_PROXY_REWRITE");
    if((!enabled || strcmp(enabled,"0")) && (sysinfo<0x70300000u || sysinfo>=0x70400000u)) {
        fprintf(stderr,"AT_SYSINFO did not select the proxy entry: %08x\n",(unsigned)sysinfo);return 1;
    }
    if(enabled && !strcmp(enabled,"0") && sysinfo>=0x70300000u && sysinfo<0x70400000u) {
        fprintf(stderr,"MATON_PROXY_REWRITE=0 left proxy AT_SYSINFO enabled\n");return 1;
    }
    if(argc>1 && !strcmp(argv[1],"--exec-child"))return file_calls()?1:0;
    if(!sysinfo){fprintf(stderr,"AT_SYSINFO missing\n");return 1;}
    int failures=file_calls();
    signal(SIGALRM,on_alarm);
    itimerval timer={{0,1000},{0,1000}};setitimer(ITIMER_REAL,&timer,0);
    for(int i=0;i<4000;i++)failures+=file_calls();
    timer.interval.sec=timer.interval.usec=timer.value.sec=timer.value.usec=0;setitimer(ITIMER_REAL,&timer,0);
    if(!signals_seen){fprintf(stderr,"signal did not arrive during fast-path workload\n");failures++;}
    const char *library=getenv("MATON_I386_DLOPEN");if(!library)library="libm.so.6";
    void *handle=dlopen(library,2);if(!handle)failures++;else if(dlclose(handle))failures++;
    unsigned long thread;void *thread_result=0;
    if(!pthread_create(&thread,0,thread_main,0)){pthread_join(thread,&thread_result);if(thread_result)failures++;}else failures++;
    pid_t child=fork();if(child==0)_exit(file_calls()?1:0);int status=0;
    if(child<0||waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status))failures++;
    pid_t exec_child=fork();if(exec_child==0){char *envp[]={"MATON_PROXY_REWRITE=0",(char *)0};execle(argv[0],argv[0],"--exec-child",(char *)0,envp);_exit(127);}status=0;
    if(exec_child<0||waitpid(exec_child,&status,0)!=exec_child||!WIFEXITED(status)||WEXITSTATUS(status))failures++;
    if(failures)return 1;
    printf("PASS i386 AT_SYSINFO=%08x file syscalls, signals, pthread TLS, fork and exec\n",(unsigned)sysinfo);
    return 0;
}
