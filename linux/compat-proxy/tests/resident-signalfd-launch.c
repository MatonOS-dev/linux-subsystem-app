/* Model the resident EGL/Binder workers retained across emulated exec. */
#define main payload_main
#include "adopt-fork-launch.c"
#undef main
#include <stdatomic.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
static atomic_int ready;
static void *worker(void *unused) {
    (void)unused;
    atomic_store(&ready,1);
    for(;;)pause();
    return NULL;
}
int main(int argc,char **argv) {
    uint64_t previous;
    if(proxy_resident_signals_begin(&previous))return 89;
    /* Same direct-child boundary as loader --supervise: continuously wait
     * for all supervisor children while the monitor has children of its own. */
    if(prctl(PR_SET_CHILD_SUBREAPER,1))return 92;
    pid_t payload=fork();
    if(payload<0)return 93;
    if(payload){
        pid_t other=fork();
        if(other<0)return 94;
        if(!other){usleep(20000);_exit(37);}
        int result=95,status;pid_t child;
        while((child=waitpid(-1,&status,0))>0)
            if(child==payload)result=WIFEXITED(status)?WEXITSTATUS(status):96;
        return result;
    }
    pthread_t thread;
    if(pthread_create(&thread,NULL,worker,NULL))return 90;
    while(!atomic_load(&ready))sched_yield();
    if(proxy_resident_signals_end(previous))return 91;
    return payload_main(argc,argv);
}
