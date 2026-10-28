/* Leave a pre-exec payload sibling in the resident registry, as a desktop
 * launcher can do before it execs Flatpak/bwrap. No worker retirement. */
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>
static atomic_int ready;
static void *worker(void *unused) {
    (void)unused;atomic_store(&ready,1);
    for(;;)pause();
    return NULL;
}
int main(int argc,char **argv) {
    if(argc<2)return 2;
    pthread_t thread;if(pthread_create(&thread,NULL,worker,NULL))return 3;
    while(!atomic_load(&ready))sched_yield();
    execv(argv[1],argv+1);return 4;
}
