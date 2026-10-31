#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <signal.h>
#include <string.h>
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"portal")) {
        const char text[]="HC_PORTAL_STDERR\n";
        if(fcntl(2,F_GETFD)<0 || write(2,text,sizeof(text)-1)!=(ssize_t)sizeof(text)-1)return 7;
        return 0;
    }
    struct sigaction action;sigaction(SIGPIPE,NULL,&action);if(action.sa_handler!=SIG_DFL)return 6;
    if(argc==2 && !strcmp(argv[1],"resident-dup-exec")) {
        int fd=open("/dev/null",O_RDONLY);if(fd<0 || dup2(fd,200)!=200)return 9;
        close(fd);
        char *next[]={argv[0],"after-resident-dup",NULL};execv(argv[0],next);return 10;
    }
    if(argc==2 && !strcmp(argv[1],"after-resident-dup")) {
        char byte=99;return read(200,&byte,1)==0?0:11;
    }
    if(argc==2 && !strcmp(argv[1],"resident-fd-collision")) {
        char byte=0;
        if(read(200,&byte,1)!=1 || byte!='r')return 8;
        return 0;
    }
    if(!isatty(0)||!isatty(42))return 1;
    if(getsid(0)!=getpid()||tcgetpgrp(0)!=getpgrp())return 2;
    if(getuid()!=(uid_t)strtoul(getenv("EXPECTED_UID"),NULL,10))return 3;
    int fd=open("/dev/tty",O_RDWR);if(fd<0)return 4;close(fd);
    puts("PTY_READY");fflush(stdout);
    char input[32];if(!fgets(input,sizeof(input),stdin))return 5;
    printf("PTY_INPUT:%s",input);return 0;
}
