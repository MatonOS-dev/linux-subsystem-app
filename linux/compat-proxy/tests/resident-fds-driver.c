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
    CHECK(proxy_resident_fds_snapshot(1)==0);
    CHECK(!resident_fd_owned(ordinary) && resident_fd_owned(owned));
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
