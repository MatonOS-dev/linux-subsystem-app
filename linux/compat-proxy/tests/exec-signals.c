#define _GNU_SOURCE
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void stale(int sig){(void)sig;_exit(77);}
int main(int argc,char **argv){
    if(argc==1){
        struct sigaction sa={.sa_handler=stale};
        sigaction(SIGUSR1,&sa,NULL);sigaction(SIGSEGV,&sa,NULL);
        signal(SIGUSR2,SIG_IGN);
        int sink=open("/dev/null",O_WRONLY);if(sink<0||dup2(sink,2)!=2)return 8;close(sink);
        execl(argv[0],argv[0],"check",NULL);return 2;
    }
    if(!strcmp(argv[1],"check")){
        struct sigaction sa;
        sigaction(SIGUSR1,NULL,&sa);if(sa.sa_handler!=SIG_DFL)return 3;
        sigaction(SIGUSR2,NULL,&sa);if(sa.sa_handler!=SIG_IGN)return 4;
        execl(argv[0],argv[0],"crash",NULL);return 5;
    }
    volatile int *p=(int *)0;*p=1;return 6;
}
