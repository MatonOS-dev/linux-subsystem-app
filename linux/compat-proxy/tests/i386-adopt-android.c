/* Device-only harness: exercise adoption without requiring an EGL driver.
 * Production uses loader.c's --gpu-bridge path and read-only bootstrap. */
#include "../proxy.h"
#include "../vfs.h"
#include "../state.h"
#include "../arch/i386/adopt.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv,char **env) {
    if(argc<2)return 2;
    char api[32];snprintf(api,sizeof(api),"%lx",(unsigned long)adopt_api_address());
    if(setenv("MATON_THREAD_ADOPT",api,1))return 3;
    extern char **environ;env=environ;
    state_gpu_bridge=1;
    proxy_i386_tls_reservation(1);
    struct image image;
    int r=proxy_load(argv[1],argv+1,env,&image);proxy_log("test-load",r,0);if(r)return 4;
    r=proxy_install(1);proxy_log("test-install",r,0);if(r)return 5;
    r=adopt_install();proxy_log("test-adopt",r,0);if(r)return 6;
    adopt_launch(image.entry,image.stack);
}
