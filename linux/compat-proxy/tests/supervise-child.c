#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    pid_t child=fork();
    if(child<0)return 1;
    if(child)_exit(0);
    struct timespec delay={.tv_sec=0,.tv_nsec=350000000};
    while(nanosleep(&delay,&delay)<0) {}
    _exit(0);
}
