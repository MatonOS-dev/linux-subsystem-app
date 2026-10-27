/* Embed the real proxy to exercise resident ownership through Linux exec. */
#include "../proxy.c"
#define CHECK(x) do { if(!(x))return __LINE__; } while(0)
int main(int argc,char **argv,char **env) {
    CHECK(argc==2);
    long ordinary=RAW(PROXY_SC_openat,AT_FDCWD,"/dev/null",O_RDONLY|O_CLOEXEC,0,0,0);
    CHECK(ordinary>=0);
    CHECK(proxy_resident_fds_snapshot(0)==0);
    long owned=RAW(PROXY_SC_openat,AT_FDCWD,"/dev/zero",O_RDONLY|O_CLOEXEC,0,0,0);
    CHECK(owned>=0);
    /* Device EGL keeps many driver descriptors resident across Linux exec. */
    for(unsigned i=0;i<124;i++)CHECK(RAW(PROXY_SC_openat,AT_FDCWD,"/dev/zero",O_RDONLY|O_CLOEXEC,0,0,0)>=0);
    CHECK(proxy_resident_fds_snapshot(1)==0);
    CHECK(!resident_fd_owned(ordinary) && resident_fd_owned(owned));
    if(owned==4) {
        long moved=RAW(PROXY_SC_fcntl,owned,F_DUPFD_CLOEXEC,20,0,0,0);CHECK(moved>=0);
        CHECK(resident_fd_set((int)owned,0)==0 && resident_fd_set((int)moved,2)==0);
        owned=moved;
    }
    /* Adopted Linux children may use dup2 on any descriptor number, including
     * one the Android runtime owned before the fork. Retire that ownership
     * only in the child after the kernel replaces its descriptor. */
    int pipefd[2];CHECK(RAW(PROXY_SC_pipe2,pipefd,O_CLOEXEC,0,0,0,0)==0);
    long source=RAW(PROXY_SC_fcntl,pipefd[0],F_DUPFD_CLOEXEC,5,0,0,0);CHECK(source>=0);
    CHECK(RAW(PROXY_SC_write,pipefd[1],"d",1,0,0,0)==1);
    CHECK(RAW(PROXY_SC_dup2,pipefd[0],4,0,0,0,0)==4);
    CHECK(resident_fd_set(4,2)==0);
    long child=RAW(PROXY_SC_clone,SIGCHLD,0,0,0,0,0);CHECK(child>=0);
    if(!child) {
        proxy_arg_t duplicate[6]={source,4,0,0,0,0};
        if(resident_fd_native(PROXY_SC_dup2,duplicate)!=4 || resident_fd_owned(4))
            RAW(PROXY_SC_exit,31,0,0,0,0,0);
        char byte=0;
        if(RAW(PROXY_SC_read,4,&byte,1,0,0,0)!=1 || byte!='d')RAW(PROXY_SC_exit,32,0,0,0,0,0);
        RAW(PROXY_SC_exit,0,0,0,0,0,0);
    }
    int child_status=0;CHECK(RAW(PROXY_SC_wait4,child,&child_status,0,0,0,0)==child);
    if(!WIFEXITED(child_status))return 60+WTERMSIG(child_status);
    if(WEXITSTATUS(child_status))return 80+WEXITSTATUS(child_status);
    CHECK(resident_fd_owned(4));
    resident_fd_set(4,0);RAW(PROXY_SC_close,4,0,0,0,0,0);RAW(PROXY_SC_close,source,0,0,0,0,0);RAW(PROXY_SC_close,pipefd[0],0,0,0,0,0);RAW(PROXY_SC_close,pipefd[1],0,0,0,0,0);
    proxy_arg_t args[6]={AT_FDCWD,(uintptr_t)"/dev/null",O_RDONLY|O_CLOEXEC,0,0,0};
    long fd=resident_fd_native(PROXY_SC_openat,args);CHECK(fd>=0 && resident_fd_owned(fd));
    args[0]=fd;args[1]=0;
    long copy=resident_fd_native(PROXY_SC_dup,args);CHECK(copy>=0 && resident_fd_owned(copy));
    CHECK(resident_fd_native(PROXY_SC_close,args)==0 && !resident_fd_owned(fd));
    /* Reusing a closed Android fd for Linux must not preserve the new file. */
    long reused=RAW(PROXY_SC_openat,AT_FDCWD,"/dev/null",O_RDONLY|O_CLOEXEC,0,0,0);
    CHECK(reused==fd && !resident_fd_owned(reused));
    args[0]=copy;CHECK(resident_fd_native(PROXY_SC_close,args)==0 && !resident_fd_owned(copy));
    char owned_env[64]="RESIDENT_TEST_FD=",ordinary_env[64]="PAYLOAD_TEST_FD=";
    char *dest[2]={owned_env+17,ordinary_env+16};long nums[2]={owned,ordinary};
    for(unsigned i=0;i<2;i++) {char tmp[16];unsigned n=0;do{tmp[n++]='0'+nums[i]%10;nums[i]/=10;}while(nums[i]);while(n)*dest[i]++=tmp[--n];*dest[i]=0;}
    char *payload_env[]={owned_env,ordinary_env,NULL};
    (void)env;
    struct image image;CHECK(proxy_load(argv[1],argv+1,payload_env,&image)==0);
    CHECK(proxy_install(1)==0);
    proxy_enter(image.entry,image.stack);
}
