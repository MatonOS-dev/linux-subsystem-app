#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <signal.h>
int main(void) {
    struct sigaction action;sigaction(SIGPIPE,NULL,&action);if(action.sa_handler!=SIG_DFL)return 6;
    if(!isatty(0)||!isatty(42))return 1;
    if(getsid(0)!=getpid()||tcgetpgrp(0)!=getpgrp())return 2;
    if(getuid()!=(uid_t)strtoul(getenv("EXPECTED_UID"),NULL,10))return 3;
    int fd=open("/dev/tty",O_RDWR);if(fd<0)return 4;close(fd);
    puts("PTY_READY");fflush(stdout);
    char input[32];if(!fgets(input,sizeof(input),stdin))return 5;
    printf("PTY_INPUT:%s",input);return 0;
}
