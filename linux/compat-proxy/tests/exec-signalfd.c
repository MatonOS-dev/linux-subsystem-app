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
static char *executable;
static void *exec_worker(void *unused){
    (void)unused;char *args[]={executable,"monitor",NULL};
    execv(executable,args);_exit(97);
}
static void *worker(void *unused){(void)unused;for(;;)pause();return NULL;}
int main(int argc,char **argv){
    if(argc==2 && !strcmp(argv[1],"nonleader")){
        executable=argv[0];pthread_t thread;
        CHECK(!pthread_create(&thread,NULL,exec_worker,NULL));
        for(;;)pause();
    }
    if(argc==1){
        pthread_t thread;CHECK(!pthread_create(&thread,NULL,worker,NULL));
        char *args[]={argv[0],"monitor",NULL};execv(argv[0],args);CHECK(0);
    }
    DIR *tasks=opendir("/proc/self/task");CHECK(tasks);int count=0;struct dirent *e;
    while((e=readdir(tasks)))if(e->d_name[0]>='0'&&e->d_name[0]<='9'){
        /* Resident pthread_exit leaves a dead thread-group leader visible
         * until group exit. It cannot receive SIGCHLD or run payload code. */
        char path[128],line[256];snprintf(path,sizeof(path),"/proc/self/task/%.32s/status",e->d_name);
        FILE *status=fopen(path,"r");int zombie=0;
        if(!status)continue;
        while(fgets(line,sizeof(line),status))if(!strncmp(line,"State:",6) && strchr(line,'Z'))zombie=1;
        fclose(status);if(!zombie)count++;
    }
    closedir(tasks);
    CHECK(count==1);
    return signalfd_main();
}
