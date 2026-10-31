/* Exercise host-command forks with the production resident adoption backend. */
#include "../arch/x86_64/thread-adopt.c"
#include "../proxy.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
extern char **environ;
extern int proxy_host_test(int);
int main(int argc,char **argv) {
    int i=1;
    while(i<argc && argv[i][0]=='-') {
        if(!strcmp(argv[i],"--android-filter")){i++;continue;}
        if(!strcmp(argv[i],"--trace")){proxy_trace=1;i++;continue;}
        if(!strcmp(argv[i],"--host-command-fd") && i+1<argc){
            if(proxy_host_test(atoi(argv[i+1])))return 2;
            i+=2;continue;
        }
        return 3;
    }
    /* Stand in for a driver descriptor cached by the resident EGL warmup.
     * Host-command must preserve it in both its server and payload child. */
    if(proxy_resident_fds_snapshot(0))return 6;
    int driver=open("/dev/zero",O_RDONLY|O_CLOEXEC);
    if(driver<0 || fcntl(driver,F_DUPFD_CLOEXEC,200)!=200)return 7;
    close(driver);
    if(proxy_resident_fds_snapshot(1))return 8;
    if(i==argc || proxy_install(1) || adopt_install())return 4;
    struct image image;
    if(proxy_load(argv[i],argv+i,environ,&image))return 5;
    adopt_launch(image.entry,image.stack);
}
