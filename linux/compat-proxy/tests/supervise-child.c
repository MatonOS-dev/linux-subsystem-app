#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"live")) {
        int pipefd[2];if(pipe(pipefd))return 10;
        pid_t middle=fork();if(middle<0)return 11;
        if(!middle) {
            close(pipefd[0]);
            pid_t orphan=fork();if(orphan<0)_exit(12);
            if(!orphan){close(pipefd[1]);usleep(100000);_exit(37);}
            if(write(pipefd[1],&orphan,sizeof(orphan))!=sizeof(orphan))_exit(13);
            _exit(0);
        }
        close(pipefd[1]);pid_t orphan;
        if(read(pipefd[0],&orphan,sizeof(orphan))!=sizeof(orphan))return 14;
        close(pipefd[0]);int status;
        if(waitpid(middle,&status,0)!=middle || status)return 15;
        char path[64];snprintf(path,sizeof(path),"/proc/%d/status",orphan);
        for(int i=0;i<300;i++) {
            if(access(path,F_OK)<0 && errno==ENOENT)return 23;
            usleep(10000);
        }
        return 16; /* orphan is still a zombie while the main child lives */
    }
    pid_t child=fork();
    if(child<0)return 1;
    if(child)_exit(0);
    struct timespec delay={.tv_sec=0,.tv_nsec=350000000};
    while(nanosleep(&delay,&delay)<0) {}
    _exit(0);
}
