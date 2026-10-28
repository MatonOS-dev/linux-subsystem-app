/* A threaded Flatpak execs bwrap, whose only wakeup is child SIGCHLD.
 * Native: passes. Proxy before exec sibling retirement: timeout, because the
 * surviving unblocked sibling consumes the process-directed notification.
 * Run with an external timeout; this deliberately exercises infinite poll. */
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#define main signalfd_main
#include "signalfd-child.c"
#undef main
static void *worker(void *unused){(void)unused;for(;;)pause();return NULL;}
int main(int argc,char **argv){
    if(argc==1){
        pthread_t thread;CHECK(!pthread_create(&thread,NULL,worker,NULL));
        char *args[]={argv[0],"monitor",NULL};execv(argv[0],args);CHECK(0);
    }
    DIR *tasks=opendir("/proc/self/task");CHECK(tasks);int count=0;struct dirent *e;
    while((e=readdir(tasks)))if(e->d_name[0]>='0'&&e->d_name[0]<='9')count++;
    closedir(tasks);
    if(count!=1)fprintf(stderr,"MatonOS exec retained %d threads (Linux expects one)\n",count);
    return signalfd_main();
}
